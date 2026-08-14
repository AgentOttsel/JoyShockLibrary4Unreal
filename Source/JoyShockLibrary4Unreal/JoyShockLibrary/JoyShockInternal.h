// JoyShockInternal.h - The device registry, shared between the files that were once one .cpp.
//
// Splitting JoyShockLibrary.cpp into four files (registry, polling, enumeration, Blueprint API) turned a
// handful of file-static helpers and globals into a surface those files share, which is what this header
// declares. It is internal: nothing outside this module's implementation should include it, and nothing
// here is a Blueprint type. The public face of the plugin is JoyShockBlueprintLibrary.h.
//
// Who defines what:
//   JoyShockLibrary.cpp   the globals below, the handle helpers, the LED masks
//   JoyShockPolling.cpp   pollIndividualLoop
#pragma once

#include "CoreMinimal.h"

class JoyShock;

// Every live device by its device id, and by the HID path it was found on. Guarded by the module's
// _connectedLock, not by _pathHandleLock below.
extern TMap<int32, JoyShock*> _joyshocks;
extern TMap<FString, JoyShock*> _byPath;

// Guards the three maps that follow.
extern FCriticalSection _pathHandleLock;

// Which device id each known controller identity currently holds. The identity is the controller's MAC
// where one can be read and its HID path otherwise -- see GetUniqueHandle.
extern TMap<FString, int32> _handleByIdentity;

// How many times in a row a device on this path has been opened, tracked, and then dropped without ever
// delivering an input report.
//
// This exists to bound the retry described at PhantomRescanBudget's use site, and is cleared the moment a
// device on the path delivers input, so a controller that works costs nothing here.
extern TMap<FString, int32> _phantomAttemptsByPath;

// A path that has produced this many consecutive phantoms is left alone until the platform reports a real
// device change. Small on purpose: the point is to cover the handful of passes it takes a controller that is
// mid-reconnect to start streaming, not to poll dead hardware indefinitely.
inline constexpr int32 PhantomRescanBudget = 3;

// Reserves this controller's device id. `identity` must be the controller's MAC where one could be read and
// its HID path otherwise: keying on the MAC is what lets a controller that was switched off and back on --
// or unpaired and paired again, which is when Windows hands the same controller a brand new HID path -- come
// back as the device id it had before, instead of as a stranger. Whatever key is used here must be the one
// passed to ReleaseUniqueHandle; devices carry it in JoyShock::handle_identity for exactly that reason.
int32 GetUniqueHandle(const FString& identity);
void ReserveUniqueHandle(const FString& identity, int32 handle);
void ReleaseUniqueHandle(const FString& identity);

JoyShock* GetJoyShockFromHandle(int handle);

// Player number (1-based) to the bit mask each family's player LEDs are set with.
unsigned char PlayerNumberToSwitchLedMask(int32 PlayerNumber);
unsigned char PlayerNumberToDualSenseLedMask(int32 PlayerNumber);

// One controller's polling thread: reads input reports until the device goes away. Started by
// ConnectDevices, and the only writer of a controller's output reports.
void pollIndividualLoop(JoyShock* jc);
