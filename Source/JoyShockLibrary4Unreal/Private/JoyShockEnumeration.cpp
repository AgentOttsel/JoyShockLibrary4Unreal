// JoyShockEnumeration.cpp - Finding controllers and opening them.
//
// One pass of ConnectDevices walks every HID device the platform reports, decides which are controllers
// this plugin drives, works out whether each is one it has seen before (by MAC where readable, by HID path
// otherwise), opens it, runs its family's init handshake and starts its polling thread.
//
// The awkward cases it exists to get right: a controller paired over Bluetooth and then plugged in appears
// twice and must stay one device, and a device that opens but never delivers a report is a phantom that
// must not be retried forever.
//
// Split out of JoyShockLibrary.cpp; the registry it fills is declared in JoyShockInternal.h.

#include "JoyShockBlueprintLibrary.h"
#include <bitset>
#include "hidapi.h"
#include <chrono>
#include <thread>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include "JoyShockLibrary4Unreal/JoyShockLibrary/GamepadMotion.hpp"
#include "JoyShockLibrary4Unreal/JoyShockLibrary/JoyShock.h"
#include "JoyShockLibrary4Unreal/JoyShockLibrary/Switch2Bluetooth.h"
#include "JoyShockLibrary4Unreal/JoyShockLibrary/InputHelpers.h"
#include "JoyShockLibrary4Unreal.h"
#include "JoyShockInterface.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/InputDeviceSubsystem.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "JoyShockLibrary4Unreal/JoyShockLibrary/JoyShockInternal.h"

// A controller this pass already knows about, and the transport it is currently read over.
struct FTrackedIdentity
{
	int32 DeviceId;
	bool bIsUsb;
};

// A controller already tracked over Bluetooth that has just turned up on a cable. Its own polling thread
// performs the swap; this pass only records that there is one to make.
struct FTransportUpgrade
{
	FString Mac;
	FString Path;
};

void UJoyShockLibrary::JSL4URefreshControllers()
{
	// Hands the scan to the module's background worker rather than enumerating here. Enumeration opens and
	// initialises HID devices, which blocks -- doing it on the calling thread is what makes the legacy
	// ConnectDevices node freeze the game when a Blueprint calls it.
	FJoyShockLibrary4UnrealModule::GetInstance().RequestConnectDevices();
}

// Reads the MAC address that identifies this physical controller, independently of how it is attached.
//
// Bluetooth hands it over as the HID serial number. USB usually leaves that blank, so it has to be asked
// for with a vendor feature report -- which is the case that matters here, because a USB cable plugged
// into an already-paired controller is exactly when a phantom second device appears.
//
// Returns an empty string when the address cannot be determined; callers must treat that as "cannot tell"
// and let the device through, never as a match. Wrongly merging two real controllers into one would be a
// far worse failure than the duplicate this exists to prevent.
static FString ReadControllerMacAddress(hid_device* InHandle, const hid_device_info* dev)
{
	if (dev->serial_number != nullptr && dev->serial_number[0] != L'\0')
	{
		FString Serial(dev->serial_number);
		Serial.ReplaceInline(TEXT(":"), TEXT(""));
		Serial.ReplaceInline(TEXT("-"), TEXT(""));
		return Serial.ToLower();
	}

	// Only the PlayStation controllers have a known report for this. A Nintendo controller without a
	// serial number simply goes unmatched, which is the safe direction.
	if (dev->vendor_id != DS_VENDOR)
	{
		return FString();
	}

	const bool bIsDualSense = dev->product_id == DS_USB || dev->product_id == DS_USB_V2;
	unsigned char featureBuf[64];
	memset(featureBuf, 0, sizeof(featureBuf));
	featureBuf[0] = bIsDualSense ? 0x09 : 0x12;
	const int res = hid_get_feature_report(InHandle, featureBuf, sizeof(featureBuf));
	if (res < 7)
	{
		return FString();
	}

	// Bytes 1-6 carry the controller's own address, least significant byte first.
	return FString::Printf(TEXT("%02x%02x%02x%02x%02x%02x"),
		featureBuf[6], featureBuf[5], featureBuf[4], featureBuf[3], featureBuf[2], featureBuf[1]);
}

// Phase 2: every controller the platform reports as a HID device, opened and identified without the lock.
//
// One physical controller can appear as two HID devices: plugging a USB cable into a controller that is
// already paired over Bluetooth does not end the Bluetooth link, it adds a second path. Both deliver input,
// so without the MAC comparison the game gains a phantom controller -- taking a player slot, and in a game
// that spawns per controller, an extra character -- for what the player experienced as simply putting their
// controller on charge. A cable for an already-tracked controller is instead recorded as a transport
// upgrade and handed to that device's own polling thread.
static void discover_hid_devices(TSet<FString>& TrackedPaths, TMap<FString, FTrackedIdentity>& TrackedByMac,
	TArray<FTransportUpgrade>& TransportUpgrades, TArray<JoyShock*>& NewDevices, TSet<FString>& NewMacsThisPass)
{
	struct hid_device_info *devs, *cur_dev;

	devs = hid_enumerate(0x0, 0x0);
	cur_dev = devs;
	while (cur_dev) {
		bool isSupported = false;
		switch (cur_dev->vendor_id)
		{
		case JOYCON_VENDOR:
			isSupported = cur_dev->product_id == JOYCON_L_BT ||
				cur_dev->product_id == JOYCON_R_BT ||
				cur_dev->product_id == PRO_CONTROLLER ||
				cur_dev->product_id == PRO_CONTROLLER_2 ||
				cur_dev->product_id == JOYCON_CHARGING_GRIP;
			break;
		case DS_VENDOR:
			isSupported = cur_dev->product_id == DS4_USB ||
				cur_dev->product_id == DS4_USB_V2 ||
				cur_dev->product_id == DS4_USB_DONGLE ||
				cur_dev->product_id == DS4_BT ||
				cur_dev->product_id == DS_USB ||
				cur_dev->product_id == DS_USB_V2;
			break;
		case BROOK_DS4_VENDOR:
			isSupported = cur_dev->product_id == BROOK_DS4_USB;
			break;
		default:
			break;
		}
		if (!isSupported)
		{
			cur_dev = cur_dev->next;
			continue;
		}

		const FString path = cur_dev->path;

		// If we're already tracking this device, leave it entirely alone. Its own poll thread is the sole
		// authority on disconnection (hid_read returning -1), so we don't need to reconcile it here.
		// (Snapshot-based: a path that becomes tracked during this pass is caught again in phase 3.)
		if (TrackedPaths.Contains(path))
		{
			cur_dev = cur_dev->next;
			continue;
		}

		UE_LOG(LogJoyShockLibrary, Log, TEXT("path: %s\n"), *FString(StringCast<TCHAR>(cur_dev->path).Get()));

		hid_device* handle = hid_open_path(cur_dev->path);
		if (handle != nullptr)
		{
			const FString DeviceMac = ReadControllerMacAddress(handle, cur_dev);
			const FTrackedIdentity* Existing = DeviceMac.IsEmpty() ? nullptr : TrackedByMac.Find(DeviceMac);
			if (Existing != nullptr)
			{
				// The existing connection wins: nothing about the player's controller should change just
				// because they put it on charge. Only a cable for a Bluetooth-tracked controller upgrades.
				const bool bIsUsbPath = !IsBluetoothHidPath(path);
				if (bIsUsbPath && !Existing->bIsUsb)
				{
					TransportUpgrades.Add({ DeviceMac, path });
					UE_LOG(LogJoyShockLibrary, Log,
						TEXT("\tdevice at %s is device %d (MAC %s) on its cable; handing it over for a transport switch\n"),
						*path, Existing->DeviceId, *DeviceMac);
				}
				else
				{
					UE_LOG(LogJoyShockLibrary, Log,
						TEXT("\tdevice at %s is the same physical controller as device %d (MAC %s); keeping the existing connection\n"),
						*path, Existing->DeviceId, *DeviceMac);
				}
				// Either way this pass does not own the handle: the polling thread opens the path itself.
				hid_close(handle);
			}
			else if (!DeviceMac.IsEmpty() && NewMacsThisPass.Contains(DeviceMac))
			{
				// Two untracked paths for one physical controller in a single pass -- the first scan of a
				// session where a controller is both paired over Bluetooth and sitting on its cable. The
				// MAC check above only catches duplicates of an ALREADY tracked device, and since device ids
				// are keyed by MAC now, letting this build a second JoyShock would hand two live devices the
				// same id and lose one of them inside _joyshocks. The first path wins; the next enumeration
				// sees this one as a duplicate of a tracked device and routes it through the transport
				// upgrade above, which is where the Bluetooth/cable choice actually belongs.
				UE_LOG(LogJoyShockLibrary, Log,
					TEXT("\tdevice at %s is the same physical controller (MAC %s) as another path found in this scan; ignoring it for now\n"),
					*path, *DeviceMac);
				hid_close(handle);
			}
			else
			{
				// The constructor runs its transport probe (blocking feature-report I/O on PlayStation
				// controllers) -- another reason this phase must be lock-free.
				UE_LOG(LogJoyShockLibrary, Log, TEXT("\tcreating new JoyShock\n"));
				// Prefer the MAC as the id key so a controller that is switched off and back on, or
				// re-paired onto a fresh HID path, comes back as the device id it had before.
				const FString Identity = DeviceMac.IsEmpty() ? path : DeviceMac;
				JoyShock* jc = new JoyShock(cur_dev, handle, GetUniqueHandle(Identity), path);
				jc->mac_address = DeviceMac;
				jc->handle_identity = Identity;
				NewDevices.Add(jc);
				if (!DeviceMac.IsEmpty())
				{
					NewMacsThisPass.Add(DeviceMac);
				}
			}
		}

		cur_dev = cur_dev->next;
	}
	hid_free_enumeration(devs);
}

// Phase 2b: the Switch 2 controllers that are on the radio rather than on a cable. hid_enumerate cannot see
// them -- they have no Bluetooth HID profile, so Windows never makes them a HID device -- so they come from
// a BLE advertisement scan instead. Connecting is slow (it negotiates a link and reads the GATT table),
// which is exactly why this belongs in the lock-free phase.
static void discover_bluetooth_switch2(TSet<FString>& TrackedPaths, TMap<FString, FTrackedIdentity>& TrackedByMac,
	TArray<JoyShock*>& NewDevices, TSet<FString>& NewMacsThisPass)
{
	if (Switch2Ble::IsSupported())
	{
		// Scanning is what makes a controller appear when its SYNC button is held, so it runs from the
		// first enumeration onward rather than being something a game has to ask for.
		Switch2Ble::StartScan();

		TArray<FSwitch2BleAdvertisement> Discovered;
		Switch2Ble::DrainDiscovered(Discovered);

		for (const FSwitch2BleAdvertisement& Advertisement : Discovered)
		{
			// The three Switch 2 shapes, all of which speak the same protocol on the same GATT service.
			// Anything else advertising as Nintendo is not something this parser can read.
			if (Advertisement.ProductId != PRO_CONTROLLER_2
				&& Advertisement.ProductId != SWITCH2_JOYCON_L
				&& Advertisement.ProductId != SWITCH2_JOYCON_R)
			{
				UE_LOG(LogJoyShockLibrary, Log,
					TEXT("\tignoring Bluetooth controller %012llx (product %04x): not a Switch 2 controller this plugin can read\n"),
					Advertisement.Address, Advertisement.ProductId);
				continue;
			}

			const FString BlePath = FString::Printf(TEXT("ble://%012llx"), Advertisement.Address);
			if (TrackedPaths.Contains(BlePath))
			{
				continue;
			}

			const FString DeviceMac = FString::Printf(TEXT("%012llx"), Advertisement.Address);
			if (TrackedByMac.Contains(DeviceMac) || NewMacsThisPass.Contains(DeviceMac))
			{
				// The same physical controller is already here on its cable. The wired connection wins:
				// it is lower latency, and taking the radio link as well would give one controller two
				// device ids and two player slots.
				continue;
			}

			FSwitch2BleConnection* Connection = Switch2Ble::Connect(Advertisement.Address);
			if (Connection == nullptr)
			{
				continue;
			}

			UE_LOG(LogJoyShockLibrary, Log, TEXT("\tcreating new JoyShock for Bluetooth controller %s\n"), *BlePath);
			JoyShock* jc = new JoyShock(Connection, Advertisement.ProductId, Advertisement.Address,
				GetUniqueHandle(DeviceMac), BlePath);
			jc->handle_identity = DeviceMac;

			// Bond an unpaired controller to this PC so the player does not have to hold SYNC again next
			// session -- a controller advertising with no host recorded is one that has just been put into
			// pairing mode, and pairing it is what they asked for by doing that.
			if (Advertisement.ReconnectMac == 0)
			{
				const uint64 HostAddress = Switch2Ble::GetHostAddress();
				if (HostAddress != 0 && Switch2Ble::Bond(Connection, HostAddress))
				{
					UE_LOG(LogJoyShockLibrary, Log,
						TEXT("\tpaired %s to this PC; it will reconnect on a button press from now on\n"), *BlePath);
				}
			}

			NewDevices.Add(jc);
			NewMacsThisPass.Add(DeviceMac);
		}
	}
}

// Runs each new device's family init handshake and starts its polling thread.
//
// Blocking HID I/O, and unbounded rather than merely slow: hidapi's Windows hid_write waits on an overlapped
// result with no timeout, so a controller that never completes the write (which is what a freshly replugged
// one does) never returns. This is why the caller drops _connectedLock before calling: under the lock it is
// an editor hang lasting until the cable is pulled.
//
// Working from a snapshot is what makes dropping the lock safe -- every device here has no polling thread
// yet, and only a polling thread frees a device, so none of these pointers can go away while we use them.
static void bring_devices_online(const TArray<JoyShock*>& DevicesToBringOnline)
{
	for (JoyShock* jc : DevicesToBringOnline)
	{
		if (jc->initialised)
		{
			continue;
		}

		if (jc->is_switch2) {
			// Send the Switch 2 init sequence that makes it start streaming: over the cable the one Steam
			// uses, over the radio the same commands on the GATT command channel.
			if (jc->is_ble()) {
				jc->init_switch2_bluetooth();
			}
			else {
				jc->init_switch2();
			}
		}
		else if (jc->controller_type == ControllerType::s_ds4) {
			if (!jc->is_usb) {
				jc->init_ds4_bt();
			}
			else {
				jc->init_ds4_usb();
			}
			// Mark it done, for the reason spelled out for the Switch controllers below: an already-
			// connected controller must never be re-initialised from here. The DS4 was the one family left
			// out of that, so every device change re-ran a blocking HID exchange on a controller whose
			// polling thread was using the very same handle -- while holding _connectedLock.
			//
			// That was survivable only while nothing else touched the handle. Once a controller could move
			// between transports, the polling thread would close that handle mid-exchange, the enumeration
			// thread's read would never return, and the lock it holds would freeze every device getter on the
			// game thread -- an editor hang lasting until the controller was physically unplugged.
			jc->initialised = true;
		} // dualsense
		else if (jc->controller_type == ControllerType::s_ds)
		{
			jc->initialised = true;
		} // charging grip
		// init_usb/init_bt don't set this themselves, and it has to be set on success: without it a Switch
		// controller is never considered initialised, so every ConnectDevices call re-runs init (blocking
		// HID I/O while holding _connectedLock) on every already-connected one -- which freezes the game
		// thread's device getters during play.
		//
		// It equally has to NOT be set on failure. Marking a failed init as initialised is silent and
		// permanent: init is where vibration and the IMU get switched on, so the controller streams buttons
		// and looks connected while its rumble does nothing and its IMU reports zeroes forever. The `continue`
		// above is the retry -- leaving the flag false is what arms it for the next enumeration, and costs
		// nothing for a controller that initialised fine.
		else if (jc->is_usb) {
			//UE_LOG(LibraryLogJoyShock, Log, TEXT("USB\n"));
			jc->initialised = jc->init_usb();
		}
		else {
			//UE_LOG(LibraryLogJoyShock, Log, TEXT("BT\n"));
			jc->initialised = jc->init_bt();
		}
		// all get time now for polling
		jc->last_polled = std::chrono::steady_clock::now();
		jc->delta_time = 0.0;

		jc->deviceNumber = 0; // left
	}

	// Same snapshot, still lock-free: every device here is one this pass is bringing online, so it has no
	// polling thread and nothing else can be writing to it.
	for (JoyShock* jc : DevicesToBringOnline)
	{
		// DS4 has no separate player indicator, but its RGB light bar and rumble start in the same report.
		// This is safe before its polling thread exists. All later output, including every numeric player
		// indicator, is written by that controller's polling thread.
		if (jc->controller_type == ControllerType::s_ds4)
		{
			jc->set_ds4_rumble_light(0, 0, jc->led_r, jc->led_g, jc->led_b);
		}

		// threads for polling. From here the device is owned by its polling thread, so this is the last
		// point at which this function may touch it.
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\tstarting new thread\n"));
		jc->thread = new std::thread(pollIndividualLoop, jc);
	}
}

int32 UJoyShockLibrary::ConnectDevices()
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	// for writing to console:
	//freopen("CONOUT$", "w", stdout);

	// most of the joycon and pro controller stuff here is thanks to mfosse's vjoy feeder
	// Enumerate and print the HID devices on the system

	// Discovery is split into three phases so that NO blocking HID I/O ever happens while holding
	// _connectedLock. The game thread's device getters take that lock shared, so any wedged I/O under it --
	// hid_get_feature_report has no timeout, and a freshly replugged device can leave one hanging until
	// the cable is pulled -- freezes the whole editor, which is exactly what repeated DS4 replugs did.
	//
	// Phase 1 snapshots the tracked paths/identities under the lock; phase 2 does every open, identity
	// read and constructor probe lock-free; phase 3 retakes the lock and merges, re-validating everything
	// against the live maps, because poll threads may have disconnected or transport-switched devices
	// while phase 2 was blocked on hardware.

	hid_init();

	// Phase 1: snapshot.
	TSet<FString> TrackedPaths;
	TMap<FString, FTrackedIdentity> TrackedByMac;

	JSL4UModule._connectedLock.lock();
	for (const TTuple<FString, JoyShock*>& Pair : _byPath)
	{
		TrackedPaths.Add(Pair.Key);
	}
	for (const TTuple<int32, JoyShock*>& Pair : _joyshocks)
	{
		if (Pair.Value != nullptr && !Pair.Value->mac_address.IsEmpty())
		{
			TrackedByMac.Add(Pair.Value->mac_address, { Pair.Key, Pair.Value->is_usb });
		}
	}
	JSL4UModule._connectedLock.unlock();

	// Phase 2: all the blocking work, without the lock.
	//
	TArray<FTransportUpgrade> TransportUpgrades;
	TArray<JoyShock*> NewDevices;
	// MACs already claimed by a device created in THIS pass, so one controller reachable two ways cannot be
	// built twice before either path is tracked. See its use below.
	TSet<FString> NewMacsThisPass;

	discover_hid_devices(TrackedPaths, TrackedByMac, TransportUpgrades, NewDevices, NewMacsThisPass);

	discover_bluetooth_switch2(TrackedPaths, TrackedByMac, NewDevices, NewMacsThisPass);

	// Phase 3: merge under the lock, re-validating every decision against the live maps.
	JSL4UModule._connectedLock.lock();

	for (const FTransportUpgrade& Upgrade : TransportUpgrades)
	{
		JoyShock* ExistingDevice = nullptr;
		for (const TTuple<int32, JoyShock*>& Pair : _joyshocks)
		{
			if (Pair.Value != nullptr && Pair.Value->mac_address == Upgrade.Mac)
			{
				ExistingDevice = Pair.Value;
				break;
			}
		}
		// Stale if the controller disconnected during phase 2, already moved onto a cable, or the path got
		// claimed meanwhile.
		if (ExistingDevice == nullptr || ExistingDevice->is_usb || _byPath.Contains(Upgrade.Path))
		{
			continue;
		}
		ExistingDevice->modifying_lock.Lock();
		ExistingDevice->pending_transport_path = Upgrade.Path;
		ExistingDevice->modifying_lock.Unlock();
		_byPath.Emplace(Upgrade.Path, ExistingDevice);
	}

	for (JoyShock* jc : NewDevices)
	{
		// The path may have been claimed while phase 2 ran (a transport switch, or a concurrent caller of
		// this function). The live device wins over our fresh handle.
		//
		// So may the device id. Ids are keyed by controller identity, so a device that finished
		// disappearing between phase 1 and phase 2 hands its id straight back to the controller behind it,
		// which is the whole point -- but if it has NOT finished, that id is still in use and this device
		// cannot have it. Better to drop this one and pick the controller up on the next scan, which the
		// departing device asks for anyway, than to overwrite a live entry in _joyshocks.
		if (_byPath.Contains(jc->path) || _joyshocks.Contains(jc->intHandle))
		{
			// Give back the device id phase 2 reserved for it. Dropping it here without releasing it is
			// what used to burn an id per lost race, permanently, so a session of reconnects walked the
			// numbers upward with nothing ever occupying the ones it skipped.
			//
			// Only when nothing live holds that id, though. Whoever won the race is very often the same
			// physical controller (a transport switch, or another scan that got there first), and since ids
			// are keyed by identity our reservation is then literally the winner's own -- releasing it would
			// leave a connected controller's id unclaimed and free to be handed to the next one along.
			// (Safe to take the handle lock under _connectedLock: nothing takes them the other way round.)
			const FString Identity = jc->handle_identity;
			const int32 DiscardedHandle = jc->intHandle;
			delete jc;
			if (!_joyshocks.Contains(DiscardedHandle))
			{
				ReleaseUniqueHandle(Identity);
			}
			continue;
		}
		// The previous holder of this identity may have released the reservation on its way out after phase 2
		// read it, so put it back before the device goes live -- otherwise the id reads as free and the next
		// controller to arrive is given the one this device is already using.
		ReserveUniqueHandle(jc->handle_identity, jc->intHandle);
		_joyshocks.Emplace(jc->intHandle, jc);
		_byPath.Emplace(jc->path, jc);
	}

	// Collect the devices this pass has to bring online, then release the lock before touching any of them.
	//
	// Init is blocking HID I/O -- and unbounded, not merely slow: hidapi's Windows hid_write waits on its
	// overlapped result with no timeout, so a controller that never completes the write (which is what a
	// freshly replugged one does) never returns. Under the lock that is an editor hang lasting until the
	// cable is pulled, which is precisely the USB replug freeze. Working from a snapshot is what makes
	// dropping the lock safe: every device in it has no polling thread yet, and only a polling thread ever
	// frees a device, so none of these pointers can go away while we use them.
	TArray<JoyShock*> DevicesToBringOnline;
	for (const TTuple<int32, JoyShock*>& Pair : _joyshocks)
	{
		if (Pair.Value != nullptr && Pair.Value->thread == nullptr)
		{
			DevicesToBringOnline.Add(Pair.Value);
		}
	}
	const int32 totalDevices = _joyshocks.Num();

	JSL4UModule._connectedLock.unlock();

	bring_devices_online(DevicesToBringOnline);

	// Deliberately no connect notification here. Being enumerable is not the same as being a working
	// controller: a device that has just dropped off Bluetooth lingers in HID enumeration for a moment, and
	// can still be opened and answer the init handshake, so announcing at creation time produced a bogus
	// "input device connected" for a controller that had in fact just disconnected (and, because it was
	// gone again by the time the game thread looked it up, one reported as "Unknown Controller"). Each
	// device announces itself from its own poll thread once it delivers a real input report.

	return totalDevices;
}
