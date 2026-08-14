// JoyShockLibrary.h - The library's own entry points, and the header that ties the plugin together.
//
// What used to be one 1400-line header is now three:
//
//   JoyShockTypes.h            the data - controller states, enums, button masks
//   JoyShockBlueprintLibrary.h the Blueprint nodes a game calls
//   this file                  the library-level functions that are neither of those
//
// Both are included here so that every existing `#include "...JoyShockLibrary.h"` keeps compiling and
// keeps seeing what it saw before. New code is better off including whichever of the two it actually
// needs -- usually JoyShockTypes.h.
#pragma once

// Kept for the sake of the files that include this header and rely on it for these: the polling and
// enumeration code uses shared_mutex, and the module header uses TStaticArray.
#include <shared_mutex>
#include "Containers/StaticArray.h"

#include "JoyShockTypes.h"
#include "JoyShockBlueprintLibrary.h"

// Stops every controller's polling thread and destroys the devices. Called by the module on its way out,
// which is the only place it belongs: a game has no reason to tear the library down while it is running.
// Returns false if a thread was still running when the wait ran out, in which case its device is left alive
// on purpose and the hidapi library must not be unloaded.
bool ShutdownAllDevices();
