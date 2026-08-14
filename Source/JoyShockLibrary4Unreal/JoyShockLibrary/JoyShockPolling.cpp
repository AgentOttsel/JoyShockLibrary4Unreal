// JoyShockPolling.cpp - One controller's polling thread.
//
// Every connected controller gets one of these, started by ConnectDevices. It reads input reports until
// the device goes away, hands each one to handle_input, and is the only writer of a controller's output
// reports -- rumble, lights and IMU configuration all queue here rather than being written by the caller.
//
// Split out of JoyShockLibrary.cpp; the registry it reads is declared in JoyShockInternal.h.

#include "JoyShockLibrary.h"
#include <bitset>
#include "hidapi.h"
#include <chrono>
#include <thread>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include "GamepadMotion.hpp"
#include "JoyShock.h"
#include "Switch2Bluetooth.h"
#include "InputHelpers.h"
#include "JoyShockLibrary4Unreal.h"
#include "JoyShockInterface.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/InputDeviceSubsystem.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "JoyShockInternal.h"

// What this thread has already sent to the controller, so it can tell a change from a repeat.
//
// These were eleven locals of the polling loop before the output-report writing was split out of it. They
// travel together because they answer one question -- is this packet worth sending -- and because the Sony
// pads carry rumble, colour and the player LED in a single report, so a change to any one of them is a
// change to all three.
struct FPolledOutputState
{
	explicit FPolledOutputState(const JoyShock* jc)
		// Seeded from what ConnectDevices already sent when it brought this device online, so we do not
		// re-send it immediately.
		: lastSentLedR(jc->led_r)
		, lastSentLedG(jc->led_g)
		, lastSentLedB(jc->led_b)
		, lastSentPlayerNumber(jc->player_number)
	{
	}

	// Rumble: the polling thread is the SOLE writer of rumble packets (JSL4USetControllerRumble only stores
	// the values). Switch 1 rumble decays shortly after a single 0x10 packet, and a Switch 2 packet only
	// carries ~15ms of waveform, so while active the state is re-sent continuously; a change to (0,0) sends a
	// short run of silent packets and then stops.
	int rumbleRefreshCounter = 0;
	unsigned char lastSentSmallRumble = 0;
	unsigned char lastSentBigRumble = 0;
	int32 sw2SilentPacketsSent = 0;
	std::chrono::steady_clock::time_point lastSw2RumbleTime = std::chrono::steady_clock::now();

	// Which JSL4USetHomeLight call this thread has already acted on. Generation 0 is "no call yet", and the
	// counter only ever increases, so the first game-set value always sends. Tracking the call rather than
	// the value is what makes re-sending an unchanged intensity work -- see home_light_generation.
	uint32 lastSentHomeLightGeneration = 0;

	// DualShock 4 / DualSense: rumble, light colour and the player LED all ride in the SAME output report, so
	// they are tracked together and sent as one packet whenever any of them changes.
	unsigned char lastSentLedR;
	unsigned char lastSentLedG;
	unsigned char lastSentLedB;
	int32 lastSentPlayerNumber;

	bool bHomeLightCleared = false;
};

// Moves this controller onto a different HID path and re-runs whatever init that transport needs.
// Returns false and leaves the controller untouched if the new path cannot be opened, so a failed swap
// is never worse than not attempting one.
//
// Called only from the polling thread: it owns the handle, so the swap needs no synchronisation with a
// reader, and no other thread has to be stopped for it to happen.
static bool SwitchControllerTransport(JoyShock* jc, const FString& NewPath, bool bNewIsUsb)
{
	hid_device* NewHandle = hid_open_path(TCHAR_TO_UTF8(*NewPath));
	if (NewHandle == nullptr)
	{
		return false;
	}

	hid_close(jc->handle);
	jc->handle = NewHandle;
	hid_set_nonblocking(jc->handle, 0);
	jc->is_usb = bNewIsUsb;
	jc->path = NewPath;

	// The two transports speak different init sequences, and the wrong one leaves the controller silent
	// (see the note in enable_IMU about how that failure used to hide).
	if (jc->controller_type == ControllerType::s_ds4)
	{
		// USB needs no init at a swap: a DS4 streams its full report (IMU included) over USB by default,
		// and the light/rumble state init would set is re-sent by the polling loop anyway, because the
		// caller invalidates its last-sent tracking. Skipping it also keeps the swap's blocking-I/O
		// surface as small as possible -- this runs where a wedged exchange costs a controller, and used
		// to cost the whole editor. Bluetooth genuinely needs its init: the feature-report read inside is
		// what switches the controller back into the full 0x11 report mode.
		if (!bNewIsUsb)
		{
			jc->init_ds4_bt();
		}
	}
	else if (jc->controller_type == ControllerType::n_switch && !jc->is_switch2)
	{
		bNewIsUsb ? jc->init_usb() : jc->init_bt();
	}
	// The DualSense needs no explicit init for either transport, matching ConnectDevices.

	return true;
}

// Sends whatever output reports this controller is now due: player LEDs, the HOME light, rumble, and the
// Sony pads' combined rumble/colour/LED report.
//
// Called once per input report, from the polling thread, which is the sole writer of all of them -- the
// JSL4USet* nodes only store what the game asked for. That is deliberate: writing from the game thread
// meant a blocking HID write while holding modifying_lock, the same lock this thread takes to parse every
// input packet.
static void send_pending_output_reports(JoyShock* jc, FPolledOutputState& out)
{
	// Player indicators are assigned from Unreal's actual player slot, not from HID enumeration order.
	// The game thread stores the semantic one-based player number; this polling thread remains the sole
	// writer to both Nintendo command pipes, keeping LED changes serialised with rumble.
	if (jc->controller_type == ControllerType::n_switch)
	{
		jc->modifying_lock.Lock();
		const int32 wantedPlayerNumber = jc->player_number;
		jc->modifying_lock.Unlock();

		if (wantedPlayerNumber != out.lastSentPlayerNumber)
		{
			const unsigned char playerLightMask = PlayerNumberToSwitchLedMask(wantedPlayerNumber);
			bool bSent = false;
			if (jc->is_switch2)
			{
				bSent = jc->set_sw2_player_lights(playerLightMask);
			}
			else
			{
				bSent = jc->set_switch_player_lights(playerLightMask);
			}
			jc->note_output_result(JoyShock::OutputFunctionPlayerIndicator, bSent);
			if (bSent)
			{
				UE_LOG(LogJoyShockLibrary, Verbose,
					TEXT("Player indicator device %d -> player %d, mask 0x%02X"),
					jc->intHandle, wantedPlayerNumber, playerLightMask);
				out.lastSentPlayerNumber = wantedPlayerNumber;
			}
		}
	}

	// The blue HOME light on a right Joy-Con / Pro Controller is a notification channel, not one
	// of Nintendo's four player indicators. Controllers can retain a pulse from firmware or a
	// previous host, so clear it once the live input stream is established -- and then keep
	// re-asserting it.
	//
	// Clearing it only once was not enough: the firmware owns this light and turns it back on by
	// itself (a reconnect or a battery notification is enough), after which nothing here ever
	// switched it off again and it stayed lit for the rest of the session. It is cheap to repeat
	// next to a 60Hz input stream, and the write-only path means it cannot disturb that stream.
	if (jc->controller_type == ControllerType::n_switch
		&& !jc->is_switch2 && (jc->left_right == 2 || jc->left_right == 3))
	{
		if (jc->home_light_owned_by_game.load())
		{
			// A game has taken the light over (JSL4USetHomeLight). Its value is authoritative from
			// then on, and the keep-it-off upkeep below stops -- otherwise the two would fight and
			// the game's light would last at most one upkeep interval.
			//
			// Sent once per call, not once per change: the firmware turns this light back on behind
			// the plugin's back, so a cached intensity is not evidence of what the light is actually
			// doing, and "set it to what I already asked for" is a legitimate request. A failed
			// write leaves the generation unrecorded, which retries on the next report.
			const uint32 wantedGeneration = jc->home_light_generation.load();
			if (wantedGeneration != out.lastSentHomeLightGeneration)
			{
				const unsigned char wantedHomeLight = jc->wanted_home_light.load();
				const bool bSent = jc->set_switch_home_light(wantedHomeLight);
				jc->note_output_result(JoyShock::OutputFunctionHomeLight, bSent);
				if (bSent)
				{
					out.lastSentHomeLightGeneration = wantedGeneration;
					UE_LOG(LogJoyShockLibrary, Verbose,
						TEXT("HOME light on device %d set to intensity %d by the game"),
						jc->intHandle, wantedHomeLight);
				}
			}
		}
		else if (!out.bHomeLightCleared)
		{
			// Cleared exactly once per connection, never on a timer.
			//
			// This was briefly re-asserted every few seconds, because the firmware can switch the
			// light back on by itself. That cost far more than it bought: subcommand 0x38 goes only
			// to a right Joy-Con or Pro Controller, and those started dropping off Bluetooth after
			// a while -- the very hazard the IMU note above describes, that configuration
			// subcommands disturb an established Bluetooth stream. A notification light that may
			// come back on is a blemish; a controller that disconnects mid-game is a broken game.
			//
			// The flag is reset after a transport switch, so a swapped controller is cleared again.
			const bool bSent = jc->clear_switch_home_light();
			jc->note_output_result(JoyShock::OutputFunctionHomeLight, bSent);
			if (bSent)
			{
				out.bHomeLightCleared = true;
				UE_LOG(LogJoyShockLibrary, Verbose,
					TEXT("Cleared HOME notification light on device %d"), jc->intHandle);
			}
		}
	}

	// Switch 1 rumble (sole writer, see above): send immediately when the requested values change
	// (including a final stop packet on a change to 0,0), and while active re-send roughly every
	// 4 input reports (~60ms at 66Hz) -- the actuator fades out on its own otherwise, which made a
	// single packet feel like a short, inconsistent blip. The write happens outside modifying_lock
	// so a slow Bluetooth write can never stall the game thread's device getters.
	if (jc->controller_type == ControllerType::n_switch && !jc->is_switch2)
	{
		jc->modifying_lock.Lock();
		const unsigned char wantedSmallRumble = jc->get_wanted_small_rumble();
		const unsigned char wantedBigRumble = jc->get_wanted_big_rumble();
		jc->modifying_lock.Unlock();

		const bool bRumbleActive = wantedSmallRumble != 0 || wantedBigRumble != 0;
		const bool bRumbleChanged = wantedSmallRumble != out.lastSentSmallRumble || wantedBigRumble != out.lastSentBigRumble;
		if (bRumbleChanged || (bRumbleActive && ++out.rumbleRefreshCounter >= 4))
		{
			out.rumbleRefreshCounter = 0;
			jc->set_switch_rumble(wantedSmallRumble, wantedBigRumble);
			out.lastSentSmallRumble = wantedSmallRumble;
			out.lastSentBigRumble = wantedBigRumble;
		}
	}

	// Switch 2 rumble: each packet is three 5-byte frames, ~5ms of waveform each, so the controller
	// runs out of rumble to play 15ms after one arrives. Sending on that cadence is what makes a
	// held rumble continuous. Time-based (not report-count) because the Switch 2 streams reports
	// much faster than the Switch 1's 66Hz.
	//
	// Going silent takes a few zero-amplitude packets -- enough to overwrite whatever the actuator
	// still had queued -- and after those the wire is left alone entirely rather than carrying a
	// steady stream of silence.
	if (jc->is_switch2)
	{
		constexpr int32 Sw2RumbleIntervalMs = 15;
		constexpr int32 Sw2RumbleSilentPackets = 3;

		jc->modifying_lock.Lock();
		const unsigned char wantedSmallRumble = jc->get_wanted_small_rumble();
		const unsigned char wantedBigRumble = jc->get_wanted_big_rumble();
		jc->modifying_lock.Unlock();

		const bool bRumbleActive = wantedSmallRumble != 0 || wantedBigRumble != 0;
		const bool bRumbleChanged = wantedSmallRumble != out.lastSentSmallRumble || wantedBigRumble != out.lastSentBigRumble;
		if (bRumbleChanged)
		{
			out.sw2SilentPacketsSent = 0;
		}

		const bool bKeepSending = bRumbleActive || out.sw2SilentPacketsSent < Sw2RumbleSilentPackets;
		const auto now = std::chrono::steady_clock::now();
		if (bKeepSending
			&& (bRumbleChanged
				|| std::chrono::duration_cast<std::chrono::milliseconds>(now - out.lastSw2RumbleTime).count() >= Sw2RumbleIntervalMs))
		{
			jc->set_sw2_rumble(wantedSmallRumble, wantedBigRumble);
			out.lastSentSmallRumble = wantedSmallRumble;
			out.lastSentBigRumble = wantedBigRumble;
			out.lastSw2RumbleTime = now;
			out.sw2SilentPacketsSent = bRumbleActive ? 0 : out.sw2SilentPacketsSent + 1;
		}

		// HID input is visible to both the editor and its multi-process Standalone child, but
		// WinUSB permits only one command-interface owner. Release an inactive lease so the process
		// that is actually playing can acquire it for calibrated init, LEDs and rumble.
		jc->release_sw2_command_interface_if_idle();
	}

	// DualShock 4 / DualSense output (sole writer, see above): unlike the Switch controllers these
	// actuators hold whatever they were last told until told otherwise, so there is nothing to
	// sustain -- one packet per change is enough. Rumble, colour and player LED share one report, so
	// a change to any of them sends a single coalesced write.
	//
	// This has to happen here rather than in the JSL4USet* calls: those run on the game thread, and
	// writing from there meant a blocking HID write while holding modifying_lock -- the very lock
	// this thread takes to parse every input packet.
	if (jc->controller_type == ControllerType::s_ds4 || jc->controller_type == ControllerType::s_ds)
	{
		jc->modifying_lock.Lock();
		const unsigned char wantedSmallRumble = jc->get_wanted_small_rumble();
		const unsigned char wantedBigRumble = jc->get_wanted_big_rumble();
		const unsigned char wantedLedR = jc->led_r;
		const unsigned char wantedLedG = jc->led_g;
		const unsigned char wantedLedB = jc->led_b;
		const int32 wantedPlayerNumber = jc->player_number;
		jc->modifying_lock.Unlock();

		if (wantedSmallRumble != out.lastSentSmallRumble || wantedBigRumble != out.lastSentBigRumble
			|| wantedLedR != out.lastSentLedR || wantedLedG != out.lastSentLedG || wantedLedB != out.lastSentLedB
			|| wantedPlayerNumber != out.lastSentPlayerNumber)
		{
			// Outside modifying_lock: a slow Bluetooth write must not stall the game thread's setters.
			if (jc->controller_type == ControllerType::s_ds4)
			{
				jc->set_ds4_rumble_light(wantedSmallRumble, wantedBigRumble, wantedLedR, wantedLedG, wantedLedB);
			}
			else
			{
				const unsigned char playerLightMask = PlayerNumberToDualSenseLedMask(wantedPlayerNumber);
				jc->set_ds5_rumble_light(wantedSmallRumble, wantedBigRumble, wantedLedR, wantedLedG, wantedLedB,
					playerLightMask);
				if (wantedPlayerNumber != out.lastSentPlayerNumber)
				{
					UE_LOG(LogJoyShockLibrary, Verbose,
						TEXT("Player indicator device %d -> player %d, DualSense mask 0x%02X"),
						jc->intHandle, wantedPlayerNumber, playerLightMask);
				}
			}

			out.lastSentSmallRumble = wantedSmallRumble;
			out.lastSentBigRumble = wantedBigRumble;
			out.lastSentLedR = wantedLedR;
			out.lastSentLedG = wantedLedG;
			out.lastSentLedB = wantedLedB;
			out.lastSentPlayerNumber = wantedPlayerNumber;
		}
	}

}

// Winds the thread down: hands back the transport, decides whether the game should be told the controller
// disconnected, and frees the device if this thread is the one that owns that.
//
// Split out of pollIndividualLoop, whose loop this runs after. Nothing here can be reordered lightly --
// the comments on each step say what breaks.
//
// The module arrives as an argument, resolved once when the thread started, and must NOT be looked up here
// with GetInstance(): this function runs precisely when the thread is being retired, which is what module
// shutdown does to every controller at once. FModuleManager::UnloadModule clears the module's "ready" flag
// BEFORE calling ShutdownModule, and from any thread other than the game thread LoadModuleChecked then
// fails its own check -- so a lookup here kills the editor on close, on a thread whose call stack says
// nothing about why.
static void finish_polling_thread(JoyShock* jc, FJoyShockLibrary4UnrealModule& JSL4UModule,
	bool bReceivedInput, bool lockedThread, int numTimeOuts)
{
	if (jc->cancel_thread)
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\tending cancelled thread\n"));
	}
	else
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\ttiming out thread\n"));
	}

	// remove
	if (jc->remove_on_finish)
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\t\tremoving jc\n"));
		if (!lockedThread)
		{
			JSL4UModule._connectedLock.lock();
			// UJoyShockLibrary::ConnectedLock.Lock();
		}
		_joyshocks.Remove(jc->intHandle);
		_byPath.Remove(jc->path);
		// A controller that was switched to its cable is mapped under both paths, so the idle one has to go
		// too -- otherwise it would keep a dead device "tracked" and block that path from ever reconnecting.
		if (!jc->fallback_path.IsEmpty())
		{
			_byPath.Remove(jc->fallback_path);
		}
		if (!lockedThread)
		{
			JSL4UModule._connectedLock.unlock();
			// UJoyShockLibrary::ConnectedLock.Unlock();
		}

		// Free this device's handle so it can be reused by a future connection (keeps player numbers dense).
		ReleaseUniqueHandle(jc->handle_identity);
	}

	const int32 intHandle = jc->intHandle;
	// Capture the notify decision before deleting jc -- reading jc->* after delete is a use-after-free.
	// Only devices that were announced as connected (i.e. that delivered input) may report a disconnect;
	// otherwise a phantom that was opened but never worked would produce a disconnect for a controller the
	// engine was never told about.
	const bool bShouldNotifyDisconnect = (jc->remove_on_finish || jc->delete_on_finish) // Don't notify if reused
		&& bReceivedInput;

	// Whether this pass should ask for another scan even though it has nothing to announce -- i.e. whether
	// this was a phantom, and the path has budget left.
	//
	// A phantom holds its path in _byPath for the full ten seconds it takes to time out, and enumeration
	// skips tracked paths, so for that whole window the real controller behind that path is invisible. Once
	// the phantom is gone the path is free again -- but the old code notified nothing and scanned nothing for
	// a device that never delivered input, so the plugin simply stopped looking. The controller then stayed
	// missing until something else happened to produce a WM_DEVICECHANGE, which is why it appeared to come
	// back only after unrelated poking around in the editor.
	//
	// Rescanning unconditionally is not the answer either: for hardware that lingers in enumeration while
	// switched off, that is an endless recreate/timeout cycle. Hence the budget -- enough passes to cover a
	// controller that is mid-reconnect, then quiet.
	bool bShouldRescanForPhantom = false;
	if (!bReceivedInput && (jc->remove_on_finish || jc->delete_on_finish))
	{
		_pathHandleLock.Lock();
		int32& Attempts = _phantomAttemptsByPath.FindOrAdd(jc->path);
		bShouldRescanForPhantom = ++Attempts <= PhantomRescanBudget;
		_pathHandleLock.Unlock();
	}

	// disconnect this device
	const bool bDeletedHere = jc->delete_on_finish;
	if (bDeletedHere)
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\t\tdeleting jc\n"));
		delete jc;
	}

	if (lockedThread)
	{
		JSL4UModule._connectedLock.unlock();
		// UJoyShockLibrary::ConnectedLock.Unlock();
	}

	// notify that we disconnected this device, and say whether or not it was a timeout (if not a timeout, then an explicit disconnect)
	if (bShouldNotifyDisconnect)
	{
		{
			std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._callbackLock);
			// FScopeLock Lock(&UJoyShockLibrary::CallbackLock);
			JSL4UModule.GetOnDisconnected().ExecuteIfBound(intHandle, numTimeOuts >= 10);
		}

		// A controller may have reconnected on the same device path just before we finished cleaning up
		// (within the poll timeout window). Re-scan so it is picked up again -- cheap now that enumeration
		// only creates genuinely new devices and never touches existing ones. The device that has just gone
		// away is often still enumerable for a moment, so this pass tends to recreate it; that recreated
		// device is harmless because it never delivers input and so is never announced.
		JSL4UModule.RequestConnectDevices();
	}
	else if (bShouldRescanForPhantom)
	{
		UE_LOG(LogJoyShockLibrary, Verbose,
			TEXT("Device %d never delivered input; re-scanning in case a working controller is behind that path."),
			intHandle);
		JSL4UModule.RequestConnectDevices();
	}

	// The very last thing, and only when this thread did not free the device itself: this is the handoff to
	// a shutdown that kept ownership precisely so it could wait here. Guarded, because in every other path
	// jc is already gone and reading it would be the use-after-free this exists to prevent.
	if (!bDeletedHere)
	{
		jc->thread_exited.store(true);
	}
}

void pollIndividualLoop(JoyShock *jc) {
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	// A Bluetooth Switch 2 has no HID handle at all -- its reports arrive as GATT notifications, and
	// read_input_report blocks on those instead. Only a device with neither transport has nothing to poll.
	if (!jc->handle && !jc->is_ble()) { jc->thread_exited.store(true); return; }

	if (jc->handle)
	{
		hid_set_nonblocking(jc->handle, 0);
		//hid_set_nonblocking(jc->handle, 1); // temporary, to see if it helps. this means we'll have a crazy spin
	}

	int numTimeOuts = 0;
	int consecutiveReportsWithoutIMU = 0;
	// Joy-Con firmware can acknowledge the IMU enable during init and still never start the sensor
	// (observed on the first Joy-Con to wake the Bluetooth radio: acked 0x40, then streamed zeroed IMU
	// forever). These drive the in-stream repair: an off->on toggle of subcommand 0x40, split across two
	// report reads so the controller gets an input-report interval to process the disable.
	int imuRepairAttempts = 0;
	bool bImuRepairToggleOffSent = false;
	bool bLoggedMissingIMU = false;
	// Functions already reported as blocked, so each one fires a single event per failure episode.
	uint8_t reportedBlockedFunctions = 0;
	bool hasIMU = false;
	bool lockedThread = false;
	// Whether this device ever delivered a real input packet. Used to avoid re-scanning for a "phantom"
	// device: a controller that is powered off but still lingers in HID enumeration can be opened and
	// even respond to the init handshake, yet never produce input. Re-scanning after such a device drops
	// would just recreate it in an endless loop, so we only trigger a rescan for devices that actually worked.
	bool bReceivedInput = false;

	// Diagnostic: log the first report the Switch 2 streams (size + report id), so we can confirm whether
	// it started streaming after init.
	bool bLoggedFirstRead = false;

	FPolledOutputState out(jc);
	auto lastSw2InitRetryTime = std::chrono::steady_clock::now();

	while (!jc->cancel_thread) {
		// get input:
		unsigned char buf[64];
		memset(buf, 0, 64);

		// Enumeration found this same physical controller on a better transport (the cable). Adopt it here
		// rather than there, so the swap happens on the thread that owns the handle.
		{
			jc->modifying_lock.Lock();
			const FString PendingPath = jc->pending_transport_path;
			jc->pending_transport_path.Reset();
			jc->modifying_lock.Unlock();

			if (!PendingPath.IsEmpty())
			{
				const FString PreviousPath = jc->path;
				// Deliberately NOT under _connectedLock. The swap's init performs blocking HID I/O, and on
				// a freshly replugged device that I/O can block until the cable is pulled again -- holding
				// the exclusive lock through it froze every device getter on the game thread, i.e. the whole
				// editor, on the second plug of a session. The lock is not needed for handle safety any
				// more: enumeration does no I/O on a tracked device's handle (every family marks
				// initialised now), and this thread is the handle's only user. Worst case an init that
				// wedges costs this one controller until its read fails, which the fallback below already
				// handles -- degradation instead of a frozen editor.
				const bool bSwitched = SwitchControllerTransport(jc, PendingPath, true);
				if (bSwitched)
				{
					jc->modifying_lock.Lock();
					jc->fallback_path = PreviousPath;
					jc->modifying_lock.Unlock();
					UE_LOG(LogJoyShockLibrary, Log,
						TEXT("Controller %d (%s) switched to its USB connection; the Bluetooth path is kept as a fallback."),
						jc->intHandle, *jc->name);
					// Rumble/LED state is re-sent by the tracking below, since the values it last sent went
					// to a handle that is now closed.
					out.lastSentSmallRumble = 0;
					out.lastSentBigRumble = 0;
					out.sw2SilentPacketsSent = 0;
					out.lastSentPlayerNumber = -1;
					out.bHomeLightCleared = false;
					// Generation 0 means "nothing sent on this handle yet", so a game-owned light is
					// re-asserted once on the new transport and an unclaimed one falls to the clear below.
					out.lastSentHomeLightGeneration = 0;
					continue;
				}
				// Unmap the path we never managed to adopt: enumeration registered it as ours when queueing
				// the swap, and leaving it there would make every replug look "already tracked" and never
				// retry. This brief map-only mutation is what the lock is actually for.
				JSL4UModule._connectedLock.lock();
				_byPath.Remove(PendingPath);
				JSL4UModule._connectedLock.unlock();
				UE_LOG(LogJoyShockLibrary, Warning,
					TEXT("Controller %d (%s): could not open its USB connection; staying on Bluetooth."),
					jc->intHandle, *jc->name);
			}
		}

		// 10 seconds of no signal means forget this controller
		int reuseCounter = jc->reuse_counter;
		int res = jc->read_input_report(buf, 64, 1000);

		// Shutdown asked this thread to stop while it was inside that read. Leave before the branches below
		// can read the result as a disconnect: they take the connected lock, announce the controller as gone
		// and free it -- all of which now belong to whoever is shutting this thread down, and none of which
		// is safe once the maps and callbacks it would touch have been torn down. Everything this device
		// owns is released after the join.
		if (jc->cancel_thread)
		{
			break;
		}

		if (res == -1)
		{
			// The cable was pulled on a controller that is still paired over Bluetooth. That is not a
			// disconnect -- the controller never left -- so return to the radio instead of telling the game
			// its controller is gone, which would cost the player their pawn for the sake of unplugging.
			jc->modifying_lock.Lock();
			const FString FallbackPath = jc->fallback_path;
			jc->modifying_lock.Unlock();

			if (!FallbackPath.IsEmpty())
			{
				const FString AbandonedPath = jc->path;
				// Lock-free for the same reason as the swap onto the cable above: the init inside can
				// block, and only map mutations need the lock.
				const bool bSwitched = SwitchControllerTransport(jc, FallbackPath, false);
				if (bSwitched)
				{
					// Drop the cable's path so replugging is seen as a new transport rather than as
					// something already tracked.
					JSL4UModule._connectedLock.lock();
					_byPath.Remove(AbandonedPath);
					JSL4UModule._connectedLock.unlock();

					jc->modifying_lock.Lock();
					jc->fallback_path.Reset();
					jc->modifying_lock.Unlock();

					UE_LOG(LogJoyShockLibrary, Log,
						TEXT("Controller %d (%s) lost its USB connection and returned to Bluetooth."),
						jc->intHandle, *jc->name);
					out.lastSentSmallRumble = 0;
					out.lastSentBigRumble = 0;
					out.sw2SilentPacketsSent = 0;
					out.lastSentPlayerNumber = -1;
					out.bHomeLightCleared = false;
					out.lastSentHomeLightGeneration = 0;
					numTimeOuts = 0;
					continue;
				}
				// Both transports are gone: this really is a disconnect, so fall through.
				jc->modifying_lock.Lock();
				jc->fallback_path.Reset();
				jc->modifying_lock.Unlock();
			}

			// disconnected!
			UE_LOG(LogJoyShockLibrary, Log, TEXT("Controller %d disconnected\n"), jc->intHandle);

			JSL4UModule._connectedLock.lock();
			// UJoyShockLibrary::_connectedLock.lock();
			// UJoyShockLibrary::ConnectedLock.Lock();
			lockedThread = true;
			const bool gettingReused = jc->reuse_counter != reuseCounter;
			jc->delete_on_finish = true;
			if (gettingReused)
			{
				jc->remove_on_finish = false;
				jc->delete_on_finish = false;
				lockedThread = false;
				JSL4UModule._connectedLock.unlock();
				// UJoyShockLibrary::_connectedLock.unlock();
				// UJoyShockLibrary::ConnectedLock.Unlock();
			}
			break;
		}

		if (res == 0)
		{
			numTimeOuts++;
			if (numTimeOuts >= 10 && !jc->is_switch2)
			{
				UE_LOG(LogJoyShockLibrary, Log, TEXT("Controller %d timed out\n"), jc->intHandle);

				// just make sure we get this thing deleted before someone else tries to start a new connection
				JSL4UModule._connectedLock.lock();
				// UJoyShockLibrary::ConnectedLock.Lock();
				lockedThread = true;
				const bool gettingReused = jc->reuse_counter != reuseCounter;
				jc->delete_on_finish = true;
				if (gettingReused)
				{
					jc->remove_on_finish = false;
					jc->delete_on_finish = false;
					lockedThread = false;
					JSL4UModule._connectedLock.unlock();
					// UJoyShockLibrary::ConnectedLock.Unlock();
				}
				break;
			}
			// Deliberately no re-init here to "wake up" a quiet controller: the full handshake is
			// destructive, not restorative. A silent device is still present (hid_read returns 0, not -1),
			// but re-running init_bt on it drops the Bluetooth link outright -- the very next read returns
			// -1 -- so a one-second hiccup was being turned into a disconnect/reconnect cycle. The Switch 2
			// already had to be excluded here for the same reason (its handshake resets the stream so it
			// never settles), and there is nothing Switch 1 specific about the problem.
			//
			// Doing nothing is strictly better: a hiccuping controller recovers for free on the next read,
			// and one that is genuinely gone still gets dropped by the timeout above and recreated by
			// enumeration -- with a fresh handle and a clean init, which is the path that actually works.
		}
		else
		{
			numTimeOuts = 0;

			if (jc->is_switch2 && !bLoggedFirstRead)
			{
				UE_LOG(LogJoyShockLibrary, Log, TEXT("Pro Controller 2 first report received: %d bytes, report id 0x%02X"), res, buf[0]);
				bLoggedFirstRead = true;
			}

			if (jc->is_switch2 && !jc->sw2_init_succeeded)
			{
				const auto now = std::chrono::steady_clock::now();
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSw2InitRetryTime).count() >= 2000)
				{
					lastSw2InitRetryTime = now;
					if (jc->is_ble() ? jc->init_switch2_bluetooth() : jc->init_switch2())
					{
						UE_LOG(LogJoyShockLibrary, Log,
							TEXT("SW2: acquired the released command interface and completed calibrated init."));
					}
				}
			}

			send_pending_output_reports(jc, out);
			// we want to be able to do these check-and-calls without fear of interruption by another thread. there could be many threads (as many as connected controllers),
			// and the callback could be time-consuming (up to the user), so we use a readers-writer-lock.
			if (handle_input(jc, buf, res, hasIMU)) { // but the user won't necessarily have a callback at all, so we'll skip the lock altogether in that case
				if (!bReceivedInput)
				{
					// First real input report: only now is this a controller rather than a device that
					// merely answered enumeration, so this is where it gets announced to the engine. See
					// JoyShock::has_delivered_input for why the connect can't be sent at creation time.
					bReceivedInput = true;
					jc->has_delivered_input.store(true);

					// This path produces working controllers, so it owes nothing to the phantom budget.
					_pathHandleLock.Lock();
					_phantomAttemptsByPath.Remove(jc->path);
					_pathHandleLock.Unlock();

					std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._callbackLock);
					JSL4UModule.GetOnConnected().ExecuteIfBound(jc->intHandle);
				}
				// accumulate gyro
				FIMUState imuState = jc->get_transformed_imu_state(jc->imu_state);
				jc->push_cumulative_gyro(imuState.gyroX, imuState.gyroY, imuState.gyroZ);
				if (JSL4UModule.GetOnPoll().IsBound() || JSL4UModule.GetOnPollTouch().IsBound())
				{
					std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._callbackLock);
					// FScopeLock Lock(&UJoyShockLibrary::CallbackLock);

					JSL4UModule.GetOnPoll().ExecuteIfBound(jc->intHandle, jc->simple_state, jc->last_simple_state, imuState, jc->get_transformed_imu_state(jc->last_imu_state), jc->delta_time);

					// touchpad will have its own callback so that it doesn't change the existing api
					if (jc->controller_type != ControllerType::n_switch) {
						JSL4UModule.GetOnPollTouch().ExecuteIfBound(jc->intHandle, jc->touch_state, jc->last_touch_state, jc->delta_time);
					}
				}
				// IMU is configured with a verified ack as part of the controller handshake, but Joy-Con
				// firmware can acknowledge the enable and still never start the sensor. When the stream
				// stays IMU-less, toggle subcommand 0x40 off and back on from here -- as bare writes only
				// (see write_subcommand for why that is the one subcommand form this thread may use).
				// Reading a reply here, or re-running the full init handshake, remains forbidden: the
				// former steals input reports, the latter drops the Bluetooth link outright.
				if (!hasIMU)
				{
					consecutiveReportsWithoutIMU++;
					const bool bCanRepairIMU = jc->controller_type == ControllerType::n_switch
						&& !jc->is_switch2;
					if (bCanRepairIMU && imuRepairAttempts < 3)
					{
						if (bImuRepairToggleOffSent)
						{
							// Second half of the toggle, one report later, so the controller had a full
							// input-report interval to process the disable.
							const uint8_t imuOn = 0x01;
							jc->note_output_result(JoyShock::OutputFunctionMotionSensor,
								jc->write_subcommand(0x40, &imuOn, 1));
							bImuRepairToggleOffSent = false;
							imuRepairAttempts++;
						}
						else if (consecutiveReportsWithoutIMU >= 60 + 250 * imuRepairAttempts)
						{
							UE_LOG(LogJoyShockLibrary, Log,
								TEXT("Controller %d (%s): no IMU samples after %d reports; toggling the IMU enable in-stream (attempt %d/3)."),
								jc->intHandle, *jc->name, consecutiveReportsWithoutIMU, imuRepairAttempts + 1);
							const uint8_t imuOff = 0x00;
							jc->note_output_result(JoyShock::OutputFunctionMotionSensor,
								jc->write_subcommand(0x40, &imuOff, 1));
							bImuRepairToggleOffSent = true;
						}
					}
					if (!bLoggedMissingIMU && consecutiveReportsWithoutIMU >= 250)
					{
						UE_LOG(LogJoyShockLibrary, Warning,
							TEXT("Controller %d (%s) is delivering input without IMU samples "
								"(report 0x%02X, %d bytes); preserving the active input stream."),
							jc->intHandle, *jc->name, buf[0], res);
						bLoggedMissingIMU = true;
					}
				}
				else
				{
					if (bLoggedMissingIMU)
					{
						UE_LOG(LogJoyShockLibrary, Log,
							TEXT("Controller %d (%s) resumed IMU samples (report 0x%02X, %d bytes)."),
							jc->intHandle, *jc->name, buf[0], res);
						bLoggedMissingIMU = false;
					}
					consecutiveReportsWithoutIMU = 0;
					imuRepairAttempts = 0;
					bImuRepairToggleOffSent = false;
				}
				
				// A failed output write is ambiguous on its own: an unplugged controller fails its writes
				// too, but its next read returns -1 and is handled above as a disconnect. A write that
				// fails while input keeps arriving means something else owns the controller's output path
				// (Steam Input, most often). Report each function once, on the transition into failure; a
				// later successful write of that function re-arms it.
				{
					const uint8_t failedFunctions = jc->failed_output_functions.load();
					const uint8_t newlyFailed = failedFunctions & ~reportedBlockedFunctions;
					reportedBlockedFunctions = failedFunctions;
					for (uint8_t FunctionIndex = 0; newlyFailed != 0 && FunctionIndex < 4; FunctionIndex++)
					{
						if ((newlyFailed & (1 << FunctionIndex)) == 0)
						{
							continue;
						}
						const EJSL4UControllerFunction Function = static_cast<EJSL4UControllerFunction>(FunctionIndex);
						UE_LOG(LogJoyShockLibrary, Warning,
							TEXT("Controller %d (%s): %s output is being rejected while input still flows -- another application (Steam Input, typically) appears to be holding this controller."),
							jc->intHandle, *jc->name, *UEnum::GetValueAsString(Function));
						std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._callbackLock);
						JSL4UModule.GetOnFunctionBlocked().ExecuteIfBound(jc->intHandle, Function);
					}
				}

				// Deliberately no periodic "wake up" re-init for a Bluetooth DualShock 4.
				//
				// This used to call init_ds4_bt() every 30 seconds. That is the same mistake the timeout
				// branch and the HOME light upkeep above each had to unlearn: a configuration exchange
				// disturbs an established Bluetooth stream, and it does so most readily when the radio is
				// already busy -- which is exactly what several Joy-Cons on the same adapter make it. The
				// controller then goes quiet, ten one-second reads time out, and a working controller is
				// dropped as "disconnected" while it is sitting there streaming.
				//
				// It was also worse than a no-op even when it worked. init_ds4_bt writes a full output
				// report with rumble and colour zeroed, so every thirty seconds the light bar went black
				// and stayed black: the output tracking below still believed its own last-sent colour, so
				// nothing re-sent it until the game happened to change it.
				//
				// Nothing needs the repeat. The report mode init_ds4_bt selects is a property of the
				// connection, so it survives for as long as the connection does, and a connection that has
				// genuinely gone is handled by the read failing -- which is the path that reconnects
				// cleanly. A controller that is quiet is still present (hid_read returns 0, not -1) and
				// recovers for free on its next report.
			}
		}
	}

	finish_polling_thread(jc, JSL4UModule, bReceivedInput, lockedThread, numTimeOuts);
}
