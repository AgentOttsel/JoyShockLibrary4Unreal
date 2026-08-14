// JoyShockInterfaceInternal.h - The handful of things FJoyShockInterface's two implementation files share.
//
// JoyShockInterface.cpp translates hardware state into Unreal input events; JoyShockPlayerAssignment.cpp
// decides which controller belongs to which player. These predicates are needed by both -- input dispatch
// asks "is this a left Joy-Con?" to know how to rotate its stick, and pairing asks the same question to
// know what it can be paired with -- so they cannot stay file-static in either.
//
// Private to the module's implementation. Nothing in Public/ includes this.
#pragma once

#include "CoreMinimal.h"

// Whether a hardware device identifier (as returned by GetHardwareDeviceIdentifier) names a controller of
// this kind. Matching is on the identifier rather than the controller type so that both the input path and
// the pairing path agree on one answer.
bool IsJoyConLeftIdentifier(const FName& Identifier);
bool IsJoyConRightIdentifier(const FName& Identifier);
bool IsSonyIdentifier(const FName& Identifier);
