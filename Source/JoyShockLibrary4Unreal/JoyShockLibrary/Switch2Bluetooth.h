#pragma once

#include "CoreMinimal.h"

// Bluetooth transport for the Nintendo Switch 2 controllers.
//
// These controllers do not speak Bluetooth HID. Over the air they are a plain BLE peripheral with a
// vendor GATT service, so Windows never enumerates them as a gamepad and hidapi cannot see them at all --
// which is why this lives beside the HID code rather than inside it. Everything here talks to that GATT
// service directly: scan for the advertisement, connect, subscribe to the input characteristic, and write
// commands and vibration data to their own characteristics.
//
// The implementation is Windows-only (WinRT) and is deliberately kept behind this plain interface: no
// WinRT type appears in this header, so the rest of the plugin includes it without inheriting either the
// WinRT headers or their exception requirements.

// Nintendo's company identifier, as it appears in the manufacturer data of a controller's advertisement.
#define NINTENDO_BLE_MANUFACTURER_ID 0x0553

// One Switch 2 controller seen advertising.
struct FSwitch2BleAdvertisement
{
	// The controller's own BLE address, and the only stable name it has before a connection exists.
	uint64 Address = 0;

	uint16 VendorId = 0;
	uint16 ProductId = 0;

	// The host this controller is already bonded to, or 0 when it is advertising for a new one (the state
	// it enters when the SYNC button is held). A controller bonded elsewhere still advertises when a button
	// is pressed, but connecting to it would take it away from that host.
	uint64 ReconnectMac = 0;
};

// A live connection to one controller. Opaque: its contents are WinRT.
class FSwitch2BleConnection;

namespace Switch2Ble
{
	// False when this platform has no BLE support compiled in, or the machine has no Bluetooth adapter.
	bool IsSupported();

	// The local adapter's address, which is what a controller is told to bond to. 0 when unavailable.
	uint64 GetHostAddress();

	// Scanning runs continuously in the background once started; discovered controllers accumulate until
	// they are drained. Starting an already-running scan is a no-op.
	bool StartScan();
	void StopScan();

	// Called once for each newly discovered controller, from the radio's own thread. A controller
	// advertising is the Bluetooth equivalent of a USB device being plugged in, and nothing else announces
	// it -- there is no WM_DEVICECHANGE for the radio -- so without this a controller that appears while
	// the game is running is queued and never looked at. Set it before starting the scan.
	void SetDiscoveryCallback(TFunction<void()> OnDiscovered);

	// Hands over the controllers seen since the last call and clears the pending list.
	void DrainDiscovered(TArray<FSwitch2BleAdvertisement>& OutDiscovered);

	// Connects, discovers the service's characteristics and subscribes to input. Blocking, and slow enough
	// (a second or more) that it must never be called from the game thread. Returns null on failure.
	FSwitch2BleConnection* Connect(uint64 Address);

	// Ends the connection and frees it. The pointer is invalid afterwards.
	void Disconnect(FSwitch2BleConnection* Connection);

	// False once the link has dropped, which is how the polling thread learns the controller is gone.
	bool IsConnected(const FSwitch2BleConnection* Connection);

	// Sends one command and waits for the controller's acknowledgement, which is how a command reports
	// whether it was accepted. Commands are serialised: the response characteristic carries no request id,
	// so two commands in flight could not be told apart.
	//
	// MinResponseBytes is for a caller that wants a payload rather than just a yes. A memory read is
	// answered with more than one notification, and the acknowledgement -- eight bytes, correct command id,
	// status accepted -- passes every test a payload does except being the payload. Saying how much is
	// expected is what lets the ack be recognised as not the answer and the wait continue.
	bool SendCommand(FSwitch2BleConnection* Connection, uint8 CommandId, uint8 SubcommandId,
		const uint8* Data, int32 DataLength, TArray<uint8>* OutResponse = nullptr,
		int32 MinResponseBytes = 0, int32 TimeoutMs = 1000);

	// Writes bytes to the command characteristic exactly as given, with no header and no acknowledgement.
	// Almost everything the controller is told goes through SendCommand instead; this exists for the one
	// packet that does not fit that shape -- the Joy-Con 2's input-mode selection, whose second byte is not
	// the 0x91 every other command carries.
	bool SendRawCommand(FSwitch2BleConnection* Connection, const uint8* Data, int32 Length);

	// Writes HD rumble frames to the vibration characteristic. Unacknowledged, like every other transport's
	// rumble: the next packet is due in 15ms, so there is nothing useful to do about a lost one.
	bool SendVibration(FSwitch2BleConnection* Connection, const uint8* Payload, int32 Length);

	// Pops one input report, waiting up to TimeoutMs for one to arrive. Returns its length, 0 on timeout,
	// and -1 once the connection is gone -- the same contract as hid_read_timeout, so the polling thread
	// treats both transports identically.
	int32 ReadInputReport(FSwitch2BleConnection* Connection, uint8* OutBuffer, int32 MaxLength, int32 TimeoutMs);

	// Bonds the controller to this host so it reconnects on its own when a button is pressed. Without it a
	// controller has to be put back into SYNC mode every session.
	bool Bond(FSwitch2BleConnection* Connection, uint64 HostAddress);

	// Shuts down scanning and releases every remaining connection. Called when the module unloads.
	void Shutdown();
}
