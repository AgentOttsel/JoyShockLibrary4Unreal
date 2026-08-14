// JoyShock.cpp - One connected controller, and what every family of them has in common.
//
// Construction and teardown, reading an input report off whichever transport the device is on, and the
// motion pipeline that turns raw gyro and accelerometer counts into a calibrated orientation. Nothing
// here knows a DualSense from a Joy-Con.
//
// Each family's own protocol -- its init handshake, its rumble encoding, its lights -- lives beside this:
//
//   JoyShock_Nintendo.cpp  Switch 1: Joy-Cons and the Pro Controller
//   JoyShock_Switch2.cpp   Switch 2: the Pro Controller 2 and Joy-Con 2, over USB and BLE
//   JoyShock_Sony.cpp      DualShock 4 and DualSense
//
// They are all members of the same JoyShock class, declared in JoyShock.h; the split is by protocol, not
// by type.

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

// Is this device attached over Bluetooth rather than USB?
//
// The product id cannot answer this: a DS4 v2 and a DualSense report the same product id over both
// transports (0x09CC / 0x0CE6), so the old "is_usb = product_id != DS4_BT" guess marked every Bluetooth
// DS4 v2 as USB. That is not a cosmetic mistake -- the two transports have different report ids and
// payload offsets, so a misidentified controller is parsed with the wrong layout and reports no input at
// all (the DS4's BT reports are 0x11, and the USB parser only accepts 0x01, so every packet is discarded).
//
// Windows puts the Bluetooth HID service class GUID in the device path of every Bluetooth HID device, and
// spells the ids as "_VID&0002xxxx_PID&xxxx" instead of USB's "VID_xxxx&PID_xxxx", so the path is a
// reliable and cheap answer. Non-Windows platforms fall through to the report-id probe below, which is
// where this used to be decided.
bool IsBluetoothHidPath(const FString& InPath)
{
	// Bluetooth HID (HIDP) service class UUID -- present in the path of every Windows Bluetooth HID device.
	return InPath.Contains(TEXT("{00001124-0000-1000-8000-00805f9b34fb}"))
		|| InPath.Contains(TEXT("_VID&"));
}
void JoyShock::init(struct hid_device_info *dev, hid_device* inHandle, int uniqueHandle, const FString &inPath) {
	this->path = inPath;

	const bool bIsBluetoothPath = IsBluetoothHidPath(inPath);

	if (dev->product_id == JOYCON_CHARGING_GRIP) {

		if (dev->interface_number == 0 || dev->interface_number == -1) {
			this->name = TEXT("Joy-Con (R)");
			this->left_right = 2;// right joycon
			this->is_usb = true;
		}
		else if (dev->interface_number == 1) {
			this->name = TEXT("Joy-Con (L)");
			this->left_right = 1;// left joycon
			this->is_usb = true;
		}
	}

	if (dev->product_id == JOYCON_L_BT) {
		this->name = TEXT("Joy-Con (L)");
		this->left_right = 1;// left joycon
	}
	else if (dev->product_id == JOYCON_R_BT) {
		this->name = TEXT("Joy-Con (R)");
		this->left_right = 2;// right joycon
	}
	else if (dev->product_id == PRO_CONTROLLER) {
		this->name = TEXT("Pro Controller");
		this->left_right = 3;// left joycon
	}
	else if (dev->product_id == PRO_CONTROLLER_2) {
		// Switch 2 Pro Controller, enumerated here over USB.
		this->name = TEXT("Pro Controller 2");
		this->left_right = 3;// treated like a Pro Controller (both halves)
		this->is_usb = true;
		this->is_switch2 = true;
	}

	if (dev->product_id == DS4_BT ||
		dev->product_id == DS4_USB ||
		dev->product_id == DS4_USB_DONGLE ||
		dev->product_id == DS4_USB_V2) {
		this->name = TEXT("DualShock 4");
		this->left_right = 3; // left and right?
		this->controller_type = ControllerType::s_ds4;
		this->is_usb = (dev->product_id != DS4_BT) && !bIsBluetoothPath;
	}
	
	if (dev->product_id == BROOK_DS4_USB) {
		this->name = TEXT("DualShock 4");
		this->left_right = 3; // left and right?
		this->controller_type = ControllerType::s_ds4;
		this->is_usb = true; // this controller is wired
	}

	if (dev->product_id == DS_USB ||
		dev->product_id == DS_USB_V2) {
		this->name = TEXT("DualSense");
		this->left_right = 3; // left and right?
		this->controller_type = ControllerType::s_ds;
		this->is_usb = !bIsBluetoothPath;
	}

	this->serial = _wcsdup(dev->serial_number);
	this->intHandle = uniqueHandle;

	//UE_LOG(LogJoyShockLibrary, Log, TEXT("Found device %c: %ls %s\n"), L_OR_R(this->left_right), this->serial, dev->path);
	this->handle = inHandle;

	// Ask a PlayStation controller for its calibration, which is also what switches it into the full report
	// mode that carries the IMU and touchpad, then confirm the transport from the report id it answers with.
	//
	// This probe can only ever move a device to Bluetooth, never back to USB: it is a fallback for platforms
	// where the path tells us nothing, and it is inherently racy (the controller does not necessarily emit
	// its first full report within the timeout). Letting it decide "USB" on a timeout is what left Bluetooth
	// controllers parsed with the USB layout. Note also that enable_gyro_ds4_bt overwrites the buffer with
	// the feature report, so the buffer has to be cleared and the read's return value checked -- otherwise a
	// timed-out read leaves the feature report's own id sitting in buf[0].
	if (this->controller_type == ControllerType::s_ds4 || this->controller_type == ControllerType::s_ds) {
		unsigned char buf[64];
		memset(buf, 0, 64);

		// The DS's protocol is literally so similar to the DS4 that we can reuse the same reports to get the
		// same results. Meet the new boss - the same as the old boss.
		enable_gyro_ds4_bt(buf, 64);

		memset(buf, 0, 64);
		const int res = hid_read_timeout(handle, buf, 64, 100);

		const unsigned char bluetoothReportId = (this->controller_type == ControllerType::s_ds4) ? 0x11 : 0x31;
		if (res > 0 && buf[0] == bluetoothReportId) {
			this->is_usb = false;
		}
	}

	UE_LOG(LogJoyShockLibrary, Log, TEXT("\t%s detected on %s"), *this->name, this->is_usb ? TEXT("USB") : TEXT("Bluetooth"));
}
JoyShock::JoyShock(struct hid_device_info* dev, hid_device* inHandle, int uniqueHandle, const FString& inPath) {
	init(dev, inHandle, uniqueHandle, inPath);

	// The window in which a button still held from before this controller existed is ignored. Started here,
	// at creation, rather than at the first report: a controller that arrives already streaming would
	// otherwise settle on that first frame, which is the exact frame the stale press is in.
	input_settle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);

	// initialise continuous calibration windows
	reset_continuous_calibration();

	// Automatic gyro drift calibration, on from the moment the controller exists.
	//
	// GamepadMotion itself starts in Manual, which is right for a generic library: it leaves the policy to
	// whoever integrates it. This plugin is that integration layer, so leaving the default untouched meant
	// every controller arrived with calibration off, and a game that did not know to call
	// JSL4USetGyroCalibrationMode got a gyro that drifts -- the exact problem calibration exists to solve,
	// reintroduced by a default. The two mistakes are not symmetrical: this way, a game that never thinks
	// about calibration behaves correctly, while a game that wants to own it already has the call to switch
	// back to Manual.
	motion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::SensorFusion
		| GamepadMotionHelpers::CalibrationMode::Stillness);
}
JoyShock::JoyShock(FSwitch2BleConnection* inConnection, uint16 productId, uint64 address, int32 uniqueHandle,
	const FString& inPath) {
	this->handle = nullptr;
	this->ble_connection = inConnection;
	this->intHandle = uniqueHandle;
	this->path = inPath;

	// Bluetooth, so not on a cable -- but everything else about the Switch 2 protocol is the same either
	// way, which is what lets the parser, the init sequence and rumble be shared with the USB path.
	this->is_usb = false;
	this->is_switch2 = true;
	this->controller_type = ControllerType::n_switch;

	switch (productId)
	{
	case SWITCH2_JOYCON_L:
		this->name = TEXT("Joy-Con 2 (L)");
		this->left_right = 1;
		break;
	case SWITCH2_JOYCON_R:
		this->name = TEXT("Joy-Con 2 (R)");
		this->left_right = 2;
		break;
	default:
		this->name = TEXT("Pro Controller 2");
		this->left_right = 3;
		break;
	}

	// A BLE peripheral has no HID serial number string, but it does have an address, and that address is
	// what identifies the physical controller here -- the same role the MAC read out of a HID device plays.
	const FString AddressText = FString::Printf(TEXT("%012llx"), address);
	this->serial = _wcsdup(*AddressText);
	this->mac_address = AddressText;

	// See the HID constructor. It matters more here: over Bluetooth the button press IS what brought this
	// controller back, so its first reports always carry one.
	input_settle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);

	reset_continuous_calibration();

	motion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::SensorFusion
		| GamepadMotionHelpers::CalibrationMode::Stillness);
}
int32 JoyShock::read_input_report(unsigned char* buf, int32 bufLength, int32 timeoutMs) {
	if (ble_connection != nullptr)
	{
		// A GATT notification carries the report body alone, while the same report over USB arrives behind
		// a HID report-id byte -- every field in it sits one byte later. Restoring that byte here is what
		// lets one parser read both transports, instead of a second set of offsets that could drift from
		// the first.
		if (bufLength < 2)
		{
			return 0;
		}
		const int32 length = Switch2Ble::ReadInputReport(ble_connection, buf + 1, bufLength - 1, timeoutMs);
		if (length <= 0)
		{
			return length;
		}
		buf[0] = 0x05;
		return length + 1;
	}
	return hid_read_timeout(this->handle, buf, bufLength, timeoutMs);
}
JoyShock::~JoyShock() {
	if (handle != nullptr) {
		hid_close(handle);
	}

	if (ble_connection != nullptr) {
		Switch2Ble::Disconnect(ble_connection);
		ble_connection = nullptr;
	}

#if PLATFORM_WINDOWS
	// Force the same close path used by the polling thread.
	sw2_last_command_time = {};
	release_sw2_command_interface_if_idle();
#endif
}
void JoyShock::push_cumulative_gyro(float gyroX, float gyroY, float gyroZ) {
	modifying_lock.Lock();
	if (num_cumulative_gyro_samples == 0) {
		cumulative_gyro_x = 0.f;
		cumulative_gyro_y = 0.f;
		cumulative_gyro_z = 0.f;
	}
	cumulative_gyro_x += gyroX;
	cumulative_gyro_y += gyroY;
	cumulative_gyro_z += gyroZ;
	num_cumulative_gyro_samples++;
	modifying_lock.Unlock();
}
void JoyShock::get_and_flush_cumulative_gyro(float& gyroX, float& gyroY, float& gyroZ) {
	modifying_lock.Lock();
	if (num_cumulative_gyro_samples == 0) {
		gyroX = cumulative_gyro_x;
		gyroY = cumulative_gyro_y;
		gyroZ = cumulative_gyro_z;
	}
	else {
		gyroX = cumulative_gyro_x / num_cumulative_gyro_samples;
		gyroY = cumulative_gyro_y / num_cumulative_gyro_samples;
		gyroZ = cumulative_gyro_z / num_cumulative_gyro_samples;
		num_cumulative_gyro_samples = 0;
		// so that we don't return zeroes before we receive a new sample, store this for next time:
		cumulative_gyro_x = gyroX;
		cumulative_gyro_y = gyroY;
		cumulative_gyro_z = gyroZ;
	}
	float gravX, gravY, gravZ;
	motion.GetGravity(gravX, gravY, gravZ);
	modifying_lock.Unlock();
	switch (gyroSpace)
	{
	default:
	case 0:
		break;
	case 1:
		GamepadMotion::CalculateWorldSpaceGyro(gyroX, gyroY, gyroX, gyroY, gyroZ, gravX, gravY, gravZ);
		gyroZ = 0.f;
		break;
	case 2:
		GamepadMotion::CalculatePlayerSpaceGyro(gyroX, gyroY, gyroX, gyroY, gyroZ, gravX, gravY, gravZ);
		gyroZ = 0.f;
		break;
	}
}
void JoyShock::reset_continuous_calibration() {
	modifying_lock.Lock();
	motion.ResetContinuousCalibration();
	modifying_lock.Unlock();
}
void JoyShock::push_sensor_samples(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float deltaTime) {
	motion.ProcessMotion(gyroX, gyroY, gyroZ, accelX, accelY, accelZ, deltaTime);
}
void JoyShock::get_calibrated_gyro(float& gyroX, float& gyroY, float& gyroZ)
{
	motion.GetCalibratedGyro(gyroX, gyroY, gyroZ);
}
FMotionState JoyShock::get_motion_state()
{
	FMotionState motionState = FMotionState();
	modifying_lock.Lock();
	motion.GetProcessedAcceleration(motionState.accelX, motionState.accelY, motionState.accelZ);
	motion.GetOrientation(motionState.quatW, motionState.quatX, motionState.quatY, motionState.quatZ);
	motion.GetRawOrientation(motionState.rawQuatW, motionState.rawQuatX, motionState.rawQuatY, motionState.rawQuatZ);
	motion.GetGravity(motionState.gravX, motionState.gravY, motionState.gravZ);
	modifying_lock.Unlock();
	return motionState;
}
FIMUState JoyShock::get_transformed_imu_state(FIMUState& InIMUState)
{
	float gyroX, gyroY, gyroZ, gravX, gravY, gravZ;
	modifying_lock.Lock();
	motion.GetGravity(gravX, gravY, gravZ);
	gyroX = InIMUState.gyroX;
	gyroY = InIMUState.gyroY;
	gyroZ = InIMUState.gyroZ;
	modifying_lock.Unlock();
	switch (gyroSpace)
	{
	default:
	case 0:
		break;
	case 1:
		GamepadMotion::CalculateWorldSpaceGyro(gyroX, gyroY, gyroX, gyroY, gyroZ, gravX, gravY, gravZ);
		gyroZ = 0.f;
		break;
	case 2:
		GamepadMotion::CalculatePlayerSpaceGyro(gyroX, gyroY, gyroX, gyroY, gyroZ, gravX, gravY, gravZ);
		gyroZ = 0.f;
		break;
	}
	FIMUState transformedState = FIMUState();
	transformedState.accelX = InIMUState.accelX;
	transformedState.accelY = InIMUState.accelY;
	transformedState.accelZ = InIMUState.accelZ;
	transformedState.gyroX = gyroX;
	transformedState.gyroY = gyroY;
	transformedState.gyroZ = gyroZ;
	return transformedState;
}
bool JoyShock::hid_exchange(hid_device *InHandle, unsigned char *buf, int len) {
	if (!InHandle) return false;

	int res;

	res = hid_write(InHandle, buf, len);

	res = hid_read_timeout(InHandle, buf, 0x40, 1000);
	if (res == 0)
	{
		return false;
	}
	return true;
}
void JoyShock::note_output_result(uint8_t FunctionBits, bool bSucceeded) {
	if (bSucceeded) {
		failed_output_functions &= static_cast<uint8_t>(~FunctionBits);
	}
	else {
		failed_output_functions |= FunctionBits;
	}
}
