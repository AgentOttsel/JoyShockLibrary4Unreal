// JoyShock_Switch2.cpp - The Switch 2 protocol: Pro Controller 2 and Joy-Con 2.
//
// A different controller from the Switch 1 despite the name, and awkward on a PC in two ways this file
// exists to handle. Over USB its HID interface is input-only, so commands go out over a second, WinUSB
// (bulk) interface that has to be matched to the right physical unit through the device tree. Over
// Bluetooth it is not a HID device at all but a BLE GATT peripheral, which is why it can be paired
// without Windows knowing what it is.
//
// Also here: its rumble frame encoding and its player lights, both unlike the Switch 1's.
//
// Split out of JoyShock.cpp. See that file for what the families share.

#include "JoyShock.h"
#include "JoyShockLibrary.h"
#include "Switch2Bluetooth.h"
#include "GamepadMotion.hpp"
#include <bitset>
#include "hidapi.h"
#include <chrono>
#include <thread>
#include <unordered_map>
#include <atomic>
#include "tools.h"
#include <cstring>
#ifdef __GNUC__
#define _wcsdup wcsdup
#endif

#if PLATFORM_WINDOWS
// The Switch 2 Pro Controller takes its commands over the WinUSB (bulk) interface MI_01, not over HID.
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <setupapi.h>
#include <winusb.h>
#include <cfgmgr32.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif


#if PLATFORM_WINDOWS
// Walks up the device tree from a device node and returns the instance id of the composite USB parent
// (the node whose id has the controller's VID/PID but no interface suffix), or empty if not found.
static FString Sw2GetCompositeParentId(DEVINST InDevInst)
{
	DEVINST current = InDevInst;
	for (int depth = 0; depth < 4; depth++)
	{
		DEVINST parent = 0;
		if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS)
		{
			break;
		}
		WCHAR idBuffer[MAX_DEVICE_ID_LEN];
		if (CM_Get_Device_IDW(parent, idBuffer, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
		{
			break;
		}
		FString id(idBuffer);
		if (id.Contains(TEXT("VID_057E")) && id.Contains(TEXT("PID_2069")) && !id.Contains(TEXT("&MI_")))
		{
			return id;
		}
		current = parent;
	}
	return FString();
}

// Resolves the composite USB parent id for a device *interface path* (e.g. this JoyShock's HID path),
// so a controller's HID (MI_00) and WinUSB (MI_01) interfaces can be matched to the same physical unit.
static FString Sw2GetCompositeParentIdForInterfacePath(const FString& InterfacePath)
{
	FString result;
	HDEVINFO devInfo = SetupDiCreateDeviceInfoList(nullptr, nullptr);
	if (devInfo == INVALID_HANDLE_VALUE)
	{
		return result;
	}

	SP_DEVICE_INTERFACE_DATA ifData;
	ifData.cbSize = sizeof(ifData);
	if (SetupDiOpenDeviceInterfaceW(devInfo, *InterfacePath, 0, &ifData))
	{
		DWORD requiredSize = 0;
		SP_DEVINFO_DATA devData;
		devData.cbSize = sizeof(devData);
		devData.DevInst = 0;
		// Sizing call; fails with insufficient-buffer but still fills devData with the owning devnode.
		SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &requiredSize, &devData);
		if (devData.DevInst != 0)
		{
			result = Sw2GetCompositeParentId(devData.DevInst);
		}
	}
	SetupDiDestroyDeviceInfoList(devInfo);
	return result;
}
#endif

bool JoyShock::sw2_open_winusb() {
#if PLATFORM_WINDOWS
	// Device interface GUID exposed by the controller's own MS OS descriptor for its WinUSB interface
	// (registry: Enum\USB\VID_057E&PID_2069&MI_01\...\Device Parameters\DeviceInterfaceGUID). Firmware-
	// defined, so it is the same on every machine.
	static const GUID Sw2WinUsbGuid = { 0x6F13725E, 0xEF0E, 0x4FD3, { 0xAE, 0x5F, 0xB2, 0xDE, 0x98, 0x9E, 0xC8, 0x25 } };

	HDEVINFO devInfo = SetupDiGetClassDevsW(&Sw2WinUsbGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devInfo == INVALID_HANDLE_VALUE)
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("SW2: SetupDiGetClassDevs failed (%u)\n"), GetLastError());
		return false;
	}

	// Composite USB parent of this JoyShock's HID interface: used to pick the WinUSB interface belonging
	// to the SAME physical controller when several Pro Controller 2s are connected.
	const FString MyCompositeId = Sw2GetCompositeParentIdForInterfacePath(this->path);

	bool bOpened = false;
	SP_DEVICE_INTERFACE_DATA ifData;
	ifData.cbSize = sizeof(ifData);
	for (DWORD index = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &Sw2WinUsbGuid, index, &ifData); index++)
	{
		DWORD requiredSize = 0;
		SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &requiredSize, nullptr);
		if (requiredSize == 0)
		{
			continue;
		}

		TArray<uint8> detailBuffer;
		detailBuffer.SetNumZeroed(requiredSize);
		PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.GetData());
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		SP_DEVINFO_DATA devData;
		devData.cbSize = sizeof(devData);
		devData.DevInst = 0;
		if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, requiredSize, nullptr, &devData))
		{
			continue;
		}

		FString devicePath(detail->DevicePath);
		if (!devicePath.Contains(TEXT("vid_057e")) || !devicePath.Contains(TEXT("pid_2069")))
		{
			continue;
		}

		// Match this WinUSB interface to the same physical controller as our HID interface (shared
		// composite parent). If the HID side couldn't be resolved, fall back to first-match.
		if (!MyCompositeId.IsEmpty() && devData.DevInst != 0)
		{
			const FString CandidateCompositeId = Sw2GetCompositeParentId(devData.DevInst);
			if (!CandidateCompositeId.Equals(MyCompositeId, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		HANDLE fileHandle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
		if (fileHandle == INVALID_HANDLE_VALUE)
		{
			const DWORD openError = GetLastError();
			if (openError == ERROR_ACCESS_DENIED)
			{
				++sw2_access_denied_count;
				if (sw2_access_denied_count >= 3 && !sw2_access_warning_logged)
				{
					UE_LOG(LogJoyShockLibrary, Warning,
						TEXT("SW2: the controller command interface remains owned by another process after multiple retries. ")
						TEXT("This can be another Unreal instance, Steam, or another controller tool."));
					sw2_access_warning_logged = true;
				}
			}
			else
			{
				UE_LOG(LogJoyShockLibrary, Warning, TEXT("SW2: CreateFile failed (%u) for %s\n"), openError, *devicePath);
			}
			continue;
		}

		WINUSB_INTERFACE_HANDLE usbHandle = nullptr;
		if (!WinUsb_Initialize(fileHandle, &usbHandle))
		{
			UE_LOG(LogJoyShockLibrary, Warning, TEXT("SW2: WinUsb_Initialize failed (%u)\n"), GetLastError());
			CloseHandle(fileHandle);
			continue;
		}

		// Find the bulk pipes (capture shows OUT=0x02, IN=0x82; query them to be safe).
		UCHAR outPipe = 0x02, inPipe = 0x82;
		USB_INTERFACE_DESCRIPTOR ifaceDesc;
		if (WinUsb_QueryInterfaceSettings(usbHandle, 0, &ifaceDesc))
		{
			for (UCHAR p = 0; p < ifaceDesc.bNumEndpoints; p++)
			{
				WINUSB_PIPE_INFORMATION pipeInfo;
				if (WinUsb_QueryPipe(usbHandle, 0, p, &pipeInfo) && pipeInfo.PipeType == UsbdPipeTypeBulk)
				{
					if (pipeInfo.PipeId & 0x80) { inPipe = pipeInfo.PipeId; }
					else { outPipe = pipeInfo.PipeId; }
				}
			}
		}

		// Bound every transfer on both pipes: these handles stay open for the device's lifetime and rumble
		// runs on the game thread, so an unbounded WritePipe/ReadPipe could otherwise hang the editor.
		ULONG timeoutMs = 200;
		WinUsb_SetPipePolicy(usbHandle, inPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
		WinUsb_SetPipePolicy(usbHandle, outPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);

		// Keep the command interface open for the device's lifetime (closed in the destructor).
		sw2_winusb_file = fileHandle;
		sw2_winusb_handle = usbHandle;
		sw2_out_pipe = outPipe;
		sw2_in_pipe = inPipe;
		sw2_last_command_time = std::chrono::steady_clock::now();
		sw2_access_warning_logged = false;
		sw2_access_denied_count = 0;
		bOpened = true;
		break; // this JoyShock's own controller found (matched via composite parent above)
	}

	SetupDiDestroyDeviceInfoList(devInfo);
	return bOpened;
#else
	return false;
#endif
}
void JoyShock::release_sw2_command_interface_if_idle() {
#if PLATFORM_WINDOWS
	if (sw2_winusb_handle == nullptr)
	{
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (sw2_last_command_time.time_since_epoch().count() != 0
		&& std::chrono::duration_cast<std::chrono::milliseconds>(now - sw2_last_command_time).count() < 1000)
	{
		return;
	}

	WinUsb_Free(static_cast<WINUSB_INTERFACE_HANDLE>(sw2_winusb_handle));
	sw2_winusb_handle = nullptr;
	if (sw2_winusb_file != nullptr)
	{
		CloseHandle(static_cast<HANDLE>(sw2_winusb_file));
		sw2_winusb_file = nullptr;
	}
#endif
}
bool JoyShock::read_sw2_ble_memory(uint32 address, int32 length, TArray<uint8>& outData) {
	// Memory read, command 0x02 subcommand 0x04: [length][0x7e][0][0][address, little-endian]. The reply
	// echoes both back before the data.
	if (length <= 0 || length > 0x4F)
	{
		return false;
	}

	unsigned char payload[8];
	memset(payload, 0, sizeof(payload));
	payload[0] = static_cast<unsigned char>(length);
	payload[1] = 0x7e;
	payload[4] = address & 0xFF;
	payload[5] = (address >> 8) & 0xFF;
	payload[6] = (address >> 16) & 0xFF;
	payload[7] = (address >> 24) & 0xFF;

	// Three attempts. A read that comes back wrong is not a controller that cannot answer -- it is one
	// answer lost on a radio that is also carrying an input report every 7.5ms -- and everything downstream
	// of a failed read is a controller running on guessed calibration for as long as it stays connected.
	// Cheap to ask again; expensive to be wrong until the player unplugs it.
	FString Failure;
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		TArray<uint8> Response;

		// 8 + length: the header the controller echoes back, then the bytes asked for. Without saying so,
		// the plain acknowledgement -- same command id, status accepted, eight bytes and no data -- answers
		// this call, and the read fails on a reply that was never meant to be the reply.
		if (!Switch2Ble::SendCommand(ble_connection, 0x02, 0x04, payload, sizeof(payload), &Response,
			8 + length))
		{
			Failure = TEXT("no answer");
			continue;
		}
		if (Response.Num() < 8 + length || Response[0] != static_cast<uint8>(length))
		{
			Failure = FString::Printf(TEXT("answer was %d bytes for a %d-byte read"), Response.Num(), length);
			continue;
		}

		// The reply echoes the address before the data, which is worth checking: a mismatched echo means
		// this is the answer to some earlier read, and parsing it as calibration would centre the sticks
		// somewhere wrong.
		const uint32 EchoedAddress = Response[4] | (Response[5] << 8) | (Response[6] << 16) | (static_cast<uint32>(Response[7]) << 24);
		if (EchoedAddress != address)
		{
			Failure = FString::Printf(TEXT("answer echoed 0x%06X"), EchoedAddress);
			continue;
		}

		outData.Append(Response.GetData() + 8, length);
		return true;
	}

	// Logged here rather than left to the caller: a read that fails silently is invisible in the log except
	// as the absence of a line that was expected, which is exactly how this went unnoticed on hardware.
	UE_LOG(LogJoyShockLibrary, Warning,
		TEXT("Controller %d: read of 0x%06X (%d bytes) failed three times (%s).\n"),
		intHandle, address, length, *Failure);
	return false;
}
bool JoyShock::init_switch2_bluetooth() {
	if (ble_connection == nullptr)
	{
		return false;
	}

	UE_LOG(LogJoyShockLibrary, Log, TEXT("Running Switch 2 (Bluetooth) init sequence on device %d...\n"), intHandle);

	struct Sw2InitCmd { unsigned char cmd; unsigned char subcmd; int dataLen; unsigned char data[20]; };
	const Sw2InitCmd cmds[] = {
		{ 0x03, 0x0d, 8,  { 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } },
		{ 0x07, 0x01, 0,  {} },
		{ 0x16, 0x01, 0,  {} },
		{ 0x15, 0x03, 1,  { 0x00 } },
		// FEATSEL: motion (0x04), mouse (0x10) and magnetometer (0x80). Asking for every feature instead
		// turns on report fields the parser does not expect, and the extra bits read as stuck triggers.
		{ 0x0c, 0x02, 4,  { 0x94, 0x00, 0x00, 0x00 } },
		{ 0x11, 0x03, 0,  {} },
		{ 0x0a, 0x08, 20, { 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
		{ 0x0c, 0x04, 4,  { 0x94, 0x00, 0x00, 0x00 } },
		{ 0x03, 0x0a, 4,  { 0x09, 0x00, 0x00, 0x00 } },
		{ 0x10, 0x01, 0,  {} },
		{ 0x01, 0x0c, 0,  {} },
		{ 0x09, 0x07, 8,  { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	};

	// A single command the controller does not like is normal -- the sequence covers several controller
	// shapes and not all of them implement all of it. A run of them means the link is not really up, and
	// carrying on would leave a device that never streams while looking connected.
	int32 ConsecutiveFailures = 0;
	for (const Sw2InitCmd& c : cmds)
	{
		if (Switch2Ble::SendCommand(ble_connection, c.cmd, c.subcmd, c.data, c.dataLen))
		{
			ConsecutiveFailures = 0;
			continue;
		}

		UE_LOG(LogJoyShockLibrary, Log, TEXT("  SW2 BT cmd %02x:%02x was not acknowledged\n"), c.cmd, c.subcmd);
		if (++ConsecutiveFailures >= 3)
		{
			UE_LOG(LogJoyShockLibrary, Warning,
				TEXT("Switch 2 Bluetooth init abandoned after three commands in a row went unanswered.\n"));
			return false;
		}
	}

	// A Joy-Con 2 needs to be told which report format to stream. Left in its default it puts its status
	// byte where the button word's top bits are, and the Left half's bit 23 lands on ZL -- a controller
	// that arrives holding a trigger nothing can release. Format 3 (0x30) moves it out of the way; the
	// parser's 0x03FFFFFF mask covers what is left.
	//
	// Sent raw because it is not a command in the usual shape: its second byte is 0x00 where every other
	// command carries 0x91, so it cannot go through SendCommand, and nothing acknowledges it.
	if (is_switch2_joycon())
	{
		static const uint8 SetInputModeFormat3[11] = {
			0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x30 };
		if (!Switch2Ble::SendRawCommand(ble_connection, SetInputModeFormat3, sizeof(SetInputModeFormat3)))
		{
			UE_LOG(LogJoyShockLibrary, Warning,
				TEXT("Joy-Con 2 on device %d refused the input-mode selection; its triggers may read as held.\n"),
				intHandle);
		}
	}

	// Factory data, same blocks the USB path reads over SPI. A controller whose calibration cannot be read
	// still works -- the parser falls back to fixed centres -- so none of this fails the init.
	TArray<uint8> Block;
	if (read_sw2_ble_memory(0x013000, 0x40, Block) && Block.Num() >= 0x25)
	{
		body_colour = (Block[0x19] << 16) | (Block[0x1A] << 8) | Block[0x1B];
		button_colour = (Block[0x1C] << 16) | (Block[0x1D] << 8) | Block[0x1E];
		left_grip_colour = (Block[0x1F] << 16) | (Block[0x20] << 8) | Block[0x21];
		right_grip_colour = (Block[0x22] << 16) | (Block[0x23] << 8) | Block[0x24];
		UE_LOG(LogJoyShockLibrary, Log, TEXT("SW2 colours: body %06x, buttons %06x\n"), body_colour, button_colour);
	}

	// Three packed 12-bit pairs: the centre, then the distance to each end of travel. Same layout the USB
	// path unpacks, read from the same addresses -- only the way of asking for them differs.
	auto Unpack12 = [](const uint8* p, uint16_t& v0, uint16_t& v1)
	{
		v0 = static_cast<uint16_t>(p[0] | ((p[1] & 0x0F) << 8));
		v1 = static_cast<uint16_t>((p[1] >> 4) | (p[2] << 4));
	};

	// One address, parsed and judged. A block that is blank or erased parses into numbers perfectly happily
	// -- an erased one into a centre of 4095, which then reads as a stick jammed to one side, and a blank
	// one into a zero range that would divide by zero -- so what comes back is checked as a description of
	// a stick, not merely for whether the read succeeded. Anything rejected leaves the slot untouched,
	// which the parser already understands as "no calibration, use the fixed centre".
	auto ReadStickCalFrom = [this, &Unpack12](uint32 address, uint16_t* calX, uint16_t* calY,
		const TCHAR* label, const TCHAR* source)
	{
		TArray<uint8> Cal;
		if (!read_sw2_ble_memory(address, 9, Cal) || Cal.Num() < 9)
		{
			return false;
		}

		// An erased region reads back as all-ones. That is the ordinary state of a user block on a
		// controller nobody has ever recalibrated, so it is refused quietly -- it is not a fault worth a
		// line in the log on every connect.
		if (Cal[0] == 0xFF && Cal[1] == 0xFF && Cal[2] == 0xFF)
		{
			return false;
		}

		uint16_t cx, cy, rxa, rya, rxb, ryb;
		Unpack12(Cal.GetData(), cx, cy);
		Unpack12(Cal.GetData() + 3, rxa, rya);
		Unpack12(Cal.GetData() + 6, rxb, ryb);

		// Assembled aside and only committed once it passes, so a rejected block cannot leave a half-written
		// calibration behind for the next address to be tried on top of.
		const uint16_t candidateX[3] = { static_cast<uint16_t>(cx - rxb), cx, static_cast<uint16_t>(cx + rxa) };
		const uint16_t candidateY[3] = { static_cast<uint16_t>(cy - ryb), cy, static_cast<uint16_t>(cy + rya) };

		// A 12-bit axis: the centre sits near the middle of the scale, and each direction of travel is some
		// real distance without spanning the whole of it. Written as one test over both axes because a
		// calibration is only usable if all four directions are.
		auto AxisIsPlausible = [](const uint16_t* cal)
		{
			return cal[1] >= 1000 && cal[1] <= 3000
				&& cal[2] > cal[1] + 200 && cal[2] - cal[1] <= 2048
				&& cal[0] + 200 < cal[1] && cal[1] - cal[0] <= 2048;
		};
		if (!AxisIsPlausible(candidateX) || !AxisIsPlausible(candidateY))
		{
			UE_LOG(LogJoyShockLibrary, Log,
				TEXT("SW2 %s stick cal (%s) at 0x%06X describes no stick "
					 "(centre (%d, %d), min (%d, %d), max (%d, %d)); ignored.\n"),
				label, source, address,
				candidateX[1], candidateY[1], candidateX[0], candidateY[0], candidateX[2], candidateY[2]);
			return false;
		}

		calX[0] = candidateX[0]; calX[1] = candidateX[1]; calX[2] = candidateX[2];
		calY[0] = candidateY[0]; calY[1] = candidateY[1]; calY[2] = candidateY[2];

		UE_LOG(LogJoyShockLibrary, Log,
			TEXT("SW2 %s stick cal (%s at 0x%06X): centre (%d, %d), min (%d, %d), max (%d, %d)\n"),
			label, source, address, calX[1], calY[1], calX[0], calY[0], calX[2], calY[2]);
		return true;
	};

	// The player's own calibration is preferred over the factory's, and this is not a nicety: a controller
	// whose sticks drifted far enough for its owner to have run the console's correction screen is exactly
	// the one whose factory centres are now wrong. The user region is separate and starts out erased, so
	// most controllers fall through to the factory copy -- as does one whose user block holds something
	// unusable, which is why this is a fall-through rather than a choice made up front.
	auto ReadStickCal = [&ReadStickCalFrom](uint32 userAddress, uint32 factoryAddress,
		uint16_t* calX, uint16_t* calY, const TCHAR* label)
	{
		return ReadStickCalFrom(userAddress, calX, calY, label, TEXT("user"))
			|| ReadStickCalFrom(factoryAddress, calX, calY, label, TEXT("factory"));
	};

	// A Joy-Con has one stick, and it is in the FIRST calibration block whichever half it is: a Joy-Con 2
	// (R) was seen on hardware reading its calibration out of 0x0130A8, the block a whole controller keeps
	// its left stick in. The blocks are per stick on the controller, not per side of a pair. Which of the
	// plugin's two slots that fills is decided by the half, matching where the parser reads it from.
	//
	// The second side is still tried if the first says nothing, because one controller is not the whole
	// range of them -- but in that order, so the common case costs one exchange rather than three. Over a
	// radio this init has already proven flaky on, a wasted round trip is not free.
	if (is_switch2_joycon())
	{
		const bool bLeft = left_right == 1;
		uint16_t* calX = bLeft ? stick_cal_x_l : stick_cal_x_r;
		uint16_t* calY = bLeft ? stick_cal_y_l : stick_cal_y_r;
		const TCHAR* label = bLeft ? TEXT("left") : TEXT("right");

		if (!ReadStickCal(0x1FC042, 0x0130A8, calX, calY, label))
		{
			ReadStickCal(0x1FC062, 0x0130E8, calX, calY, label);
		}
	}
	else
	{
		ReadStickCal(0x1FC042, 0x0130A8, stick_cal_x_l, stick_cal_y_l, TEXT("left"));
		ReadStickCal(0x1FC062, 0x0130E8, stick_cal_x_r, stick_cal_y_r, TEXT("right"));
	}

	sw2_init_succeeded = true;
	initialised = true;
	return true;
}
bool JoyShock::init_switch2() {
	// Nintendo Switch 2 Pro Controller init, replicating what Steam does (confirmed with a USBPcap capture):
	// commands go over the controller's WinUSB interface (MI_01) bulk OUT endpoint 0x02 -- NOT over HID
	// (the HID interface MI_00 is input-only; hid_write fails with -1 on it). Once initialised, the
	// controller streams its 0x05 input reports on the HID interface, which the poll thread parses.
	// Command format: [command_id] 0x91 0x00 [subcommand_id] 0x00 [data_len] 0x00 0x00 [data...].
	// (Over Bluetooth LE byte 2 is 0x01 instead; sending that over USB makes it search for a BT host.)
#if PLATFORM_WINDOWS
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Running Switch 2 (WinUSB) init sequence...\n"));

	if (!sw2_open_winusb())
	{
		// The controller may still stream input if it was already initialised (e.g. by Steam or a previous
		// session), so don't treat this as fatal; rumble retries the open lazily (see set_sw2_rumble).
		if (!sw2_init_failure_logged)
		{
			UE_LOG(LogJoyShockLibrary, Log,
				TEXT("SW2: command interface unavailable; deferring calibrated init until the current owner releases it."));
			sw2_init_failure_logged = true;
		}
		sw2_init_succeeded = false;
		this->initialised = true;
		return false;
	}

	WINUSB_INTERFACE_HANDLE usbHandle = static_cast<WINUSB_INTERFACE_HANDLE>(sw2_winusb_handle);
	const UCHAR outPipe = sw2_out_pipe;
	const UCHAR inPipe = sw2_in_pipe;
	bool bSuccess = false;

		// --- Factory config over SPI (same reads Steam performs before configuring the controller) ---
		// SPI read command 0x02:0x01, data = [flags u32 = 0][address LE u32]; the response is a 16-byte
		// header (cmd echo, status, length 0x40, address echo) followed by 64 bytes of flash data.
		auto SpiRead = [usbHandle, outPipe, inPipe](uint32 address, unsigned char* outData) -> bool
		{
			unsigned char cmdBuf[16];
			memset(cmdBuf, 0, sizeof(cmdBuf));
			cmdBuf[0] = 0x02;
			cmdBuf[1] = 0x91;
			cmdBuf[3] = 0x01;
			cmdBuf[5] = 0x08;
			cmdBuf[12] = address & 0xFF;
			cmdBuf[13] = (address >> 8) & 0xFF;
			cmdBuf[14] = (address >> 16) & 0xFF;
			cmdBuf[15] = (address >> 24) & 0xFF;

			ULONG written = 0;
			if (!WinUsb_WritePipe(usbHandle, outPipe, cmdBuf, sizeof(cmdBuf), &written, nullptr))
			{
				return false;
			}

			unsigned char resp[96];
			ULONG total = 0;
			while (total < 80) // 16-byte header + 64 bytes of data (may arrive split across bulk packets)
			{
				ULONG got = 0;
				if (!WinUsb_ReadPipe(usbHandle, inPipe, resp + total, sizeof(resp) - total, &got, nullptr) || got == 0)
				{
					break;
				}
				total += got;
			}
			if (total < 80 || resp[0] != 0x02)
			{
				return false;
			}
			memcpy(outData, resp + 16, 64);
			return true;
		};

		// Two 12-bit values packed into 3 bytes (same packing as the stick axes in input reports).
		auto Unpack12 = [](const unsigned char* p, uint16_t& v0, uint16_t& v1)
		{
			v0 = p[0] | ((p[1] & 0x0F) << 8);
			v1 = (p[1] >> 4) | (p[2] << 4);
		};

		// Stick calibration is nine bytes wherever it is stored: packed 12-bit pairs holding
		// [centerX,centerY][rangeX_a,rangeY_a][rangeX_b,rangeY_b]. Takes the nine bytes rather than a block
		// base, because the factory copy and the user copy sit at different offsets within their blocks.
		auto ParseStickCal = [&Unpack12](const unsigned char* nine, uint16_t* calX, uint16_t* calY)
		{
			uint16_t cx, cy, rxa, rya, rxb, ryb;
			Unpack12(nine, cx, cy);
			Unpack12(nine + 3, rxa, rya);
			Unpack12(nine + 6, rxb, ryb);
			calX[1] = cx; calX[2] = cx + rxa; calX[0] = cx - rxb;
			calY[1] = cy; calY[2] = cy + rya; calY[0] = cy - ryb;
		};

		unsigned char spi[64];
		// Block 0x013000: serial, ids, then body/buttons/left-grip/right-grip RGB colours at offset 0x19.
		if (SpiRead(0x013000, spi))
		{
			body_colour = (spi[0x19] << 16) | (spi[0x1A] << 8) | spi[0x1B];
			button_colour = (spi[0x1C] << 16) | (spi[0x1D] << 8) | spi[0x1E];
			left_grip_colour = (spi[0x1F] << 16) | (spi[0x20] << 8) | spi[0x21];
			right_grip_colour = (spi[0x22] << 16) | (spi[0x23] << 8) | spi[0x24];
			UE_LOG(LogJoyShockLibrary, Log, TEXT("SW2 colours: body %06x, buttons %06x\n"), body_colour, button_colour);
		}

		// The player's own calibration, written by the console's stick-correction screen, wins over the
		// factory's: a controller drifted enough for its owner to have corrected it is exactly the one whose
		// factory centres no longer describe it. Both sticks' user copies live in the one 64-byte block at
		// 0x1FC040 (left at +0x02, right at +0x22), so one read covers them. An erased region reads back as
		// all-ones, which is how "never calibrated" is told from a real reading.
		unsigned char userCal[64];
		const bool bReadUserCal = SpiRead(0x1FC040, userCal);
		auto UserCalWritten = [](const unsigned char* nine)
		{
			return !(nine[0] == 0xFF && nine[1] == 0xFF && nine[2] == 0xFF);
		};

		if (bReadUserCal && UserCalWritten(userCal + 0x02))
		{
			ParseStickCal(userCal + 0x02, stick_cal_x_l, stick_cal_y_l);
			UE_LOG(LogJoyShockLibrary, Log, TEXT("SW2 left stick cal (user): centre (%d, %d), min (%d, %d), max (%d, %d)\n"),
				stick_cal_x_l[1], stick_cal_y_l[1], stick_cal_x_l[0], stick_cal_y_l[0], stick_cal_x_l[2], stick_cal_y_l[2]);
		}
		else if (SpiRead(0x013080, spi))
		{
			ParseStickCal(spi + 0x28, stick_cal_x_l, stick_cal_y_l);
			UE_LOG(LogJoyShockLibrary, Log, TEXT("SW2 left stick cal (factory): centre (%d, %d), min (%d, %d), max (%d, %d)\n"),
				stick_cal_x_l[1], stick_cal_y_l[1], stick_cal_x_l[0], stick_cal_y_l[0], stick_cal_x_l[2], stick_cal_y_l[2]);
		}

		if (bReadUserCal && UserCalWritten(userCal + 0x22))
		{
			ParseStickCal(userCal + 0x22, stick_cal_x_r, stick_cal_y_r);
			UE_LOG(LogJoyShockLibrary, Log, TEXT("SW2 right stick cal (user): centre (%d, %d), min (%d, %d), max (%d, %d)\n"),
				stick_cal_x_r[1], stick_cal_y_r[1], stick_cal_x_r[0], stick_cal_y_r[0], stick_cal_x_r[2], stick_cal_y_r[2]);
		}
		else if (SpiRead(0x0130C0, spi))
		{
			ParseStickCal(spi + 0x28, stick_cal_x_r, stick_cal_y_r);
			UE_LOG(LogJoyShockLibrary, Log, TEXT("SW2 right stick cal (factory): centre (%d, %d), min (%d, %d), max (%d, %d)\n"),
				stick_cal_x_r[1], stick_cal_y_r[1], stick_cal_x_r[0], stick_cal_y_r[0], stick_cal_x_r[2], stick_cal_y_r[2]);
		}

		struct Sw2InitCmd { unsigned char cmd; unsigned char subcmd; int dataLen; unsigned char data[20]; };
		const Sw2InitCmd cmds[] = {
			{ 0x07, 0x01, 0,  {} },
			{ 0x0c, 0x02, 4,  { 0x27, 0x00, 0x00, 0x00 } },              // FEATSEL
			{ 0x11, 0x01, 0,  {} },
			{ 0x0a, 0x08, 20, { 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
			{ 0x0c, 0x04, 4,  { 0x27, 0x00, 0x00, 0x00 } },
			{ 0x01, 0x0c, 0,  {} },
			{ 0x01, 0x01, 0,  {} },
			{ 0x08, 0x02, 4,  { 0x01, 0x00, 0x00, 0x00 } },
			{ 0x03, 0x0a, 4,  { 0x05, 0x00, 0x00, 0x00 } },              // input report mode 0x05
			{ 0x03, 0x0d, 8,  { 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } },
		};

		bSuccess = true;
		for (const Sw2InitCmd& c : cmds)
		{
			unsigned char cmdBuf[28];
			memset(cmdBuf, 0, sizeof(cmdBuf));
			cmdBuf[0] = c.cmd;
			cmdBuf[1] = 0x91;
			cmdBuf[2] = 0x00;
			cmdBuf[3] = c.subcmd;
			cmdBuf[4] = 0x00;
			cmdBuf[5] = static_cast<unsigned char>(c.dataLen);
			cmdBuf[6] = 0x00;
			cmdBuf[7] = 0x00;
			if (c.dataLen > 0)
			{
				memcpy(cmdBuf + 8, c.data, c.dataLen);
			}

			ULONG written = 0;
			if (!WinUsb_WritePipe(usbHandle, outPipe, cmdBuf, 8 + c.dataLen, &written, nullptr))
			{
				UE_LOG(LogJoyShockLibrary, Warning, TEXT("  SW2 cmd %02x:%02x -> WritePipe failed (%u)\n"), c.cmd, c.subcmd, GetLastError());
				bSuccess = false;
				continue;
			}

			// Drain the command response (best effort; also confirms the controller acknowledged).
			unsigned char resp[64];
			ULONG got = 0;
			if (WinUsb_ReadPipe(usbHandle, inPipe, resp, sizeof(resp), &got, nullptr) && got >= 2)
			{
				UE_LOG(LogJoyShockLibrary, Log, TEXT("  SW2 cmd %02x:%02x -> ack %02x %02x (%u bytes)\n"), c.cmd, c.subcmd, resp[0], resp[1], got);
			}
			else
			{
				UE_LOG(LogJoyShockLibrary, Log, TEXT("  SW2 cmd %02x:%02x -> sent (%u bytes), no ack\n"), c.cmd, c.subcmd, written);
			}
		}
		sw2_last_command_time = std::chrono::steady_clock::now();

	if (!bSuccess)
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("SW2: some init commands failed; the controller may not stream input.\n"));
	}
	sw2_init_succeeded = bSuccess;
	if (bSuccess)
	{
		sw2_init_failure_logged = false;
	}
	this->initialised = true;
	return bSuccess;
#else
	// Non-Windows: the WinUSB path doesn't apply; hidapi may be able to talk to the command interface
	// directly on other platforms (untested).
	sw2_init_succeeded = false;
	this->initialised = true;
	return false;
#endif
}
void JoyShock::encode_sw2_rumble_frame(uint16_t lfFreq, uint16_t lfAmp, uint16_t hfFreq, uint16_t hfAmp,
	unsigned char* outFrame)
{
	// Bit layout, least significant bit first: lf_freq:9, lf_en_tone:1, lf_amp:10, hf_freq:9,
	// hf_en_tone:1, hf_amp:10 -- 40 bits, written out little-endian. The tone-enable bits stay clear:
	// they switch the actuator to a pure tone, which is not what a rumble request means.
	uint64_t value = 0;
	value |= static_cast<uint64_t>(lfFreq & 0x1FF);
	value |= static_cast<uint64_t>(lfAmp & 0x3FF) << 10;
	value |= static_cast<uint64_t>(hfFreq & 0x1FF) << 20;
	value |= static_cast<uint64_t>(hfAmp & 0x3FF) << 30;

	for (int32 idx = 0; idx < 5; idx++)
	{
		outFrame[idx] = static_cast<unsigned char>((value >> (idx * 8)) & 0xFF);
	}
}
void JoyShock::build_sw2_rumble_block(int smallRumble, int bigRumble, unsigned char* outBlock)
{
	// 0x0E1 / 0x1E1 are the controller's neutral low/high frequency pair -- the values its own firmware
	// uses for a plain rumble with no tone shaping. Only the amplitudes carry the game's request.
	constexpr uint16_t NeutralLowFrequency = 0x0E1;
	constexpr uint16_t NeutralHighFrequency = 0x1E1;

	// The two 10-bit amplitude fields each go to 1023, but the actuator can only be driven so hard: their
	// sum has to stay within 511, or the frame is rejected/clipped. Scale both together when the request
	// exceeds that, so asking for more of everything never changes the balance between the two motors.
	constexpr int32 CombinedAmplitudeLimit = 511;

	int32 highAmp = FMath::Clamp(smallRumble, 0, 255) * 1023 / 255;
	int32 lowAmp = FMath::Clamp(bigRumble, 0, 255) * 1023 / 255;
	const int32 total = highAmp + lowAmp;
	if (total > CombinedAmplitudeLimit)
	{
		highAmp = highAmp * CombinedAmplitudeLimit / total;
		lowAmp = lowAmp * CombinedAmplitudeLimit / total;
	}

	// The id lets the controller drop a packet it has already played; it must advance every send, or a
	// sustained rumble looks like one repeated packet and the actuator falls silent after the first.
	outBlock[0] = static_cast<unsigned char>(0x50 | (sw2_rumble_packet_id & 0x0F));
	sw2_rumble_packet_id++;

	for (int32 frame = 0; frame < 3; frame++)
	{
		encode_sw2_rumble_frame(NeutralLowFrequency, static_cast<uint16_t>(lowAmp),
			NeutralHighFrequency, static_cast<uint16_t>(highAmp), outBlock + 1 + frame * 5);
	}
}
void JoyShock::set_sw2_rumble(int smallRumble, int bigRumble) {
	// One block per actuator, left then right. Both carry the same request: JSL's two rumble values are a
	// high/low frequency pair, not a left/right one, so splitting them across the actuators would put all
	// the low end on one side of the controller.
	unsigned char motorBlock[16];
	build_sw2_rumble_block(smallRumble, bigRumble, motorBlock);

	if (ble_connection != nullptr)
	{
		// Over Bluetooth the frames go to their own characteristic rather than through the command channel,
		// behind one leading zero byte. A Joy-Con drives a single actuator and takes one block; the Pro
		// Controller has two and takes both.
		unsigned char payload[1 + 16 + 16];
		memset(payload, 0, sizeof(payload));
		memcpy(payload + 1, motorBlock, sizeof(motorBlock));
		memcpy(payload + 1 + sizeof(motorBlock), motorBlock, sizeof(motorBlock));

		const int32 length = (left_right == 3) ? sizeof(payload) : (1 + sizeof(motorBlock));
		note_output_result(OutputFunctionRumble, Switch2Ble::SendVibration(ble_connection, payload, length));
		return;
	}

#if PLATFORM_WINDOWS

	// HID output report 0x02: [report id][left block][right block], padded to the report's fixed 0x2A-byte
	// body. This route needs nothing but the HID handle the poll thread already owns, so rumble survives
	// another process holding the WinUSB interface -- but the command interface is the route this plugin
	// has actually proven on hardware, so HID is only tried when there is no command interface to use.
	// Once it does work for a device it is kept, since it costs less and conflicts with nothing.
	if (sw2_hid_rumble_ok || sw2_winusb_handle == nullptr)
	{
		unsigned char hidReport[1 + 0x2A];
		memset(hidReport, 0, sizeof(hidReport));
		hidReport[0] = 0x02;
		memcpy(hidReport + 1, motorBlock, sizeof(motorBlock));
		memcpy(hidReport + 1 + sizeof(motorBlock), motorBlock, sizeof(motorBlock));

		if (hid_write(this->handle, hidReport, sizeof(hidReport)) >= 0)
		{
			if (!sw2_hid_rumble_ok)
			{
				sw2_hid_rumble_ok = true;
				UE_LOG(LogJoyShockLibrary, Log,
					TEXT("SW2: HD rumble is going out over HID output report 0x02 on device %d.\n"), this->intHandle);
			}
			note_output_result(OutputFunctionRumble, true);
			return;
		}
		// An HID interface that used to accept reports and stopped is a stalled pipe, not a wrong route:
		// fall through to the command interface, and let the next call try HID again.
	}

	if (sw2_winusb_handle == nullptr)
	{
		// The command interface couldn't be opened at connect time (typically another application such as
		// Steam holding it exclusively). Retry lazily -- if that application has since been closed, rumble
		// self-heals. Throttled so failed attempts don't run the device enumeration on every rumble call.
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - sw2_last_open_attempt).count() < 2)
		{
			note_output_result(OutputFunctionRumble, false);
			return;
		}
		sw2_last_open_attempt = now;
		if (!sw2_open_winusb())
		{
			// This is the concrete Steam conflict: the WinUSB command interface allows one owner, and
			// another application already holds it.
			note_output_result(OutputFunctionRumble, false);
			return;
		}
		UE_LOG(LogJoyShockLibrary, Log, TEXT("SW2: command interface acquired on retry; rumble available.\n"));
	}

	// Command 0x0A, subcommand 0x08 -- "send vibration data" -- carries the same HD-rumble block over the
	// command interface, wrapped in the usual eight-byte command header. The payload is a leading 0x01
	// followed by one 16-byte motor block and three bytes of padding, for the declared length of 0x14.
	// (A bare motor block written to this pipe is silently ignored: the header is what makes it a command.)
	unsigned char cmdBuf[8 + 0x14];
	memset(cmdBuf, 0, sizeof(cmdBuf));
	cmdBuf[0] = 0x0A; // vibration command
	cmdBuf[1] = 0x91;
	cmdBuf[2] = 0x00; // USB transport flag
	cmdBuf[3] = 0x08; // send vibration data
	cmdBuf[5] = 0x14; // data length
	cmdBuf[8] = 0x01;
	memcpy(cmdBuf + 9, motorBlock, sizeof(motorBlock));

	ULONG written = 0;
	const bool bWritten = WinUsb_WritePipe(static_cast<WINUSB_INTERFACE_HANDLE>(sw2_winusb_handle), sw2_out_pipe, cmdBuf, sizeof(cmdBuf), &written, nullptr) != 0;
	note_output_result(OutputFunctionRumble, bWritten);
	if (bWritten && !sw2_rumble_route_logged)
	{
		sw2_rumble_route_logged = true;
		UE_LOG(LogJoyShockLibrary, Log,
			TEXT("SW2: HD rumble is going out over the WinUSB command interface on device %d.\n"), this->intHandle);
	}

	// Deliberately no read back. Configuration commands are drained for their acknowledgement, but this one
	// is sent on every rumble packet -- roughly every 15ms -- and the read pipe has a 200ms timeout, so a
	// packet the controller chooses not to answer would stall the polling thread for that whole timeout and
	// stall input with it. Vibration data is fire-and-forget on the wire.
	sw2_last_command_time = std::chrono::steady_clock::now();
#endif
}
bool JoyShock::set_sw2_player_lights(unsigned char playerLightMask) {
	if (ble_connection != nullptr)
	{
		// Same command as over the cable, on the GATT command channel: an eight-byte payload whose first
		// byte is the four-bit pattern.
		unsigned char payload[8];
		memset(payload, 0, sizeof(payload));
		payload[0] = playerLightMask;
		return Switch2Ble::SendCommand(ble_connection, 0x09, 0x07, payload, sizeof(payload));
	}

#if PLATFORM_WINDOWS
	if (sw2_winusb_handle == nullptr)
	{
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::seconds>(now - sw2_last_open_attempt).count() < 2)
		{
			return false;
		}
		sw2_last_open_attempt = now;
		if (!sw2_open_winusb())
		{
			return false;
		}
	}

	// Player LEDs use command 09:07. The mask is the same four-bit pattern used by Switch 1;
	// byte 8 is the first byte of this command's eight-byte payload.
	unsigned char cmdBuf[16];
	memset(cmdBuf, 0, sizeof(cmdBuf));
	cmdBuf[0] = 0x09;
	cmdBuf[1] = 0x91;
	cmdBuf[2] = 0x00;
	cmdBuf[3] = 0x07;
	cmdBuf[5] = 0x08;
	cmdBuf[8] = playerLightMask;

	ULONG written = 0;
	const bool bWritten = WinUsb_WritePipe(static_cast<WINUSB_INTERFACE_HANDLE>(sw2_winusb_handle), sw2_out_pipe,
		cmdBuf, sizeof(cmdBuf), &written, nullptr) && written == sizeof(cmdBuf);
	if (!bWritten)
	{
		return false;
	}

	// Drain the acknowledgement so the shared command pipe remains ready for rumble and later LED changes.
	unsigned char resp[64];
	ULONG got = 0;
	WinUsb_ReadPipe(static_cast<WINUSB_INTERFACE_HANDLE>(sw2_winusb_handle), sw2_in_pipe,
		resp, sizeof(resp), &got, nullptr);
	sw2_last_command_time = std::chrono::steady_clock::now();
	return true;
#else
	return false;
#endif
}
