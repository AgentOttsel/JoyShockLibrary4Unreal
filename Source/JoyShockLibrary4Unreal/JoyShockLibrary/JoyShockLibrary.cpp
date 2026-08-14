// JoyShockLibrary.cpp - The device registry and the library's lifetime.
//
// What lives here is the bookkeeping every other part of the plugin reads: the maps of live devices, the
// allocation of the device ids those maps are keyed by, and the shutdown that stops the polling threads.
// The work itself was split out when this file passed 3500 lines and four unrelated jobs:
//
//   JoyShockPolling.cpp              reading input reports from one controller
//   Private/JoyShockEnumeration.cpp  finding controllers and opening them
//   Private/JoyShockBlueprintLibrary.cpp  the nodes a game calls
//
// The surface those files share is declared in JoyShockInternal.h.

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

DEFINE_LOG_CATEGORY(LogJoyShockLibrary)

// Defined here, declared with their documentation in JoyShockInternal.h.
TMap<int32, JoyShock*> _joyshocks;
TMap<FString, JoyShock*> _byPath;
FCriticalSection _pathHandleLock;
TMap<FString, int32> _handleByIdentity;
TMap<FString, int32> _phantomAttemptsByPath;

// Reserves this controller's device id. `identity` must be the controller's MAC where one could be read and
// its HID path otherwise: keying on the MAC is what lets a controller that was switched off and back on --
// or unpaired and paired again, which is when Windows hands the same controller a brand new HID path -- come
// back as the device id it had before, instead of as a stranger. Whatever key is used here must be the one
// passed to ReleaseUniqueHandle; devices carry it in JoyShock::handle_identity for exactly that reason.
int32 GetUniqueHandle(const FString &identity)
{
	_pathHandleLock.Lock();
	int32* iter = _handleByIdentity.Find(identity);

	if (iter != nullptr)
	{
		_pathHandleLock.Unlock();
		return *iter;
	}

	// Assign the lowest handle not currently in use, so a controller that reconnects reuses a freed slot --
	// e.g. players 0 and 1 -- instead of ever-increasing ids (2, 3, ...). Handles are freed when a device is
	// removed (see the poll-thread disconnect cleanup). This handle is also what the input-device mapper
	// uses as the player index, so keeping it dense keeps player numbers stable across reconnects.
	int32 handle = 0;
	for (bool bInUse = true; bInUse; )
	{
		bInUse = false;
		for (const TTuple<FString, int32>& pair : _handleByIdentity)
		{
			if (pair.Value == handle)
			{
				bInUse = true;
				handle++;
				break;
			}
		}
	}

	_handleByIdentity.Emplace(identity, handle);
	_pathHandleLock.Unlock();

	return handle;
}

// Re-asserts a reservation that GetUniqueHandle already granted. Needed because a device only becomes real
// at the end of enumeration, and the previous holder of its identity can finish disappearing -- releasing
// the very entry we were handed -- in between. Without this the id would look free to the next arrival.
void ReserveUniqueHandle(const FString& identity, int32 handle)
{
	if (identity.IsEmpty())
	{
		return;
	}

	_pathHandleLock.Lock();
	_handleByIdentity.Emplace(identity, handle);
	_pathHandleLock.Unlock();
}

// Returns a device id to the pool. Every path that destroys a JoyShock has to come through here, including
// the ones that throw away a device before it was ever tracked -- a reservation that is never released is a
// device id that can never be handed out again, and that is what makes ids creep upward over a session of
// unplugging and replugging the same controller.
void ReleaseUniqueHandle(const FString& identity)
{
	if (identity.IsEmpty())
	{
		return;
	}

	_pathHandleLock.Lock();
	_handleByIdentity.Remove(identity);
	_pathHandleLock.Unlock();
}

// https://stackoverflow.com/questions/25144887/map-unordered-map-prefer-find-and-then-at-or-try-at-catch-out-of-range
// not thread-safe -- because you probably want to do something with the object you get out of it, I've left locking to the caller
JoyShock* GetJoyShockFromHandle(int handle) {
	JoyShock** iter = _joyshocks.Find(handle);

	if (iter != nullptr)
	{
		return *iter;
	}
	return nullptr;
}

// Nintendo's four LEDs use distinct patterns for players 1-8. They are deliberately not the
// one-hot value 1 << (player-1): after player 4 the console identifies players with combinations.
unsigned char PlayerNumberToSwitchLedMask(int32 PlayerNumber)
{
	static constexpr unsigned char PlayerMasks[] =
	{
		0x01, // P1: *...
		0x03, // P2: **..
		0x07, // P3: ***.
		0x0F, // P4: ****
		0x09, // P5: *..*
		0x05, // P6: *.*.
		0x0D, // P7: *.**
		0x06, // P8: .**.
	};
	return PlayerNumber >= 1 && PlayerNumber <= UE_ARRAY_COUNT(PlayerMasks)
		? PlayerMasks[PlayerNumber - 1]
		: 0;
}

unsigned char PlayerNumberToDualSenseLedMask(int32 PlayerNumber)
{
	static constexpr unsigned char PlayerMasks[] =
	{
		DS5_PLAYER_1,
		DS5_PLAYER_2,
		DS5_PLAYER_3,
		DS5_PLAYER_4,
		DS5_PLAYER_5,
	};
	return PlayerNumber >= 1 && PlayerNumber <= UE_ARRAY_COUNT(PlayerMasks)
		? PlayerMasks[PlayerNumber - 1]
		: 0;
}

// Stops every controller's polling thread and destroys the devices, in that order. Returns false if any
// thread was still running when the wait ran out, which means its device (and the hidapi handle inside it)
// is deliberately left alive for the process to reclaim.
//
// This exists because the polling threads outlive everything they touch. Nothing stopped them: the maps
// they read, the module they call back into and the hidapi library they hold handles from were all freed
// during module shutdown while the threads were still running on them. Over HID that mostly went unnoticed,
// because the process usually died before a thread noticed. Bluetooth made it deterministic -- shutdown
// actively freed the connection object each polling thread was asleep inside -- which is the crash on
// closing the editor with a controller on the radio.
bool ShutdownAllDevices()
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	// Empty the maps first, under the lock, so nothing can hand one of these devices out again while it is
	// being stopped -- and so the threads themselves have nothing left to remove on their way out.
	TArray<JoyShock*> Devices;
	JSL4UModule._connectedLock.lock();
	_joyshocks.GenerateValueArray(Devices);
	_joyshocks.Empty();
	_byPath.Empty();
	JSL4UModule._connectedLock.unlock();

	_pathHandleLock.Lock();
	_handleByIdentity.Empty();
	_phantomAttemptsByPath.Empty();
	_pathHandleLock.Unlock();

	Devices.RemoveAll([](const JoyShock* Device) { return Device == nullptr; });

	// Signalled all at once, before waiting on any of them. Each polling thread is blocked in a read of up
	// to a second, so asking them one at a time would cost a second per controller instead of a second for
	// all of them.
	for (JoyShock* jc : Devices)
	{
		// Neither flag may be left set: the thread must not touch the maps (already gone) or announce a
		// disconnect (the callbacks are unbound), and above all it must not free itself, because a thread
		// that never exits would then be running inside memory this function had freed.
		jc->remove_on_finish = false;
		jc->delete_on_finish = false;
		jc->cancel_thread = true;
	}

	// Bounded, not a join. hidapi's Windows write waits on its overlapped result with no timeout, so a
	// controller that stops completing writes -- a half-unplugged one, in practice -- leaves its polling
	// thread wedged for good. Joining that would leave the editor unable to close, which is a worse failure
	// than the one being fixed. Anything still running when the deadline passes is left to process teardown,
	// exactly as every polling thread was before this function existed.
	const double Deadline = FPlatformTime::Seconds() + 3.0;
	for (const JoyShock* jc : Devices)
	{
		while (!jc->thread_exited.load() && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.002f);
		}
	}

	bool bAllStopped = true;
	for (JoyShock* jc : Devices)
	{
		if (!jc->thread_exited.load())
		{
			UE_LOG(LogJoyShockLibrary, Warning,
				TEXT("Controller %d did not stop polling in time; leaving it and its device open for process teardown."),
				jc->intHandle);
			// Deliberately leaked, thread object included: the thread is still inside this device, so both
			// freeing it and destroying an unjoined std::thread (which terminates the process) are worse
			// than the leak. The process is exiting.
			bAllStopped = false;
			continue;
		}

		if (jc->thread != nullptr)
		{
			jc->thread->join();
			delete jc->thread;
			jc->thread = nullptr;
		}
		// Closes the HID handle, or hands the Bluetooth connection back -- the single owner of both, now
		// that the only other user has stopped.
		delete jc;
	}

	return bAllStopped;
}
