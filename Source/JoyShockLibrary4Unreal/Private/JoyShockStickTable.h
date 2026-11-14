// The one place a controller's stick signs and sideways grip are written down.
//
// Every family used to flip a stick axis somewhere -- some in their protocol reader, some in the interface,
// one in both -- and two flips that each looked right cancelled out more than once. The readers now leave
// each axis in the sign the hardware sends, and this table turns that into the plugin's convention: up = +1
// and right = +1 on both sticks, XInput's. Unreal input and the JSL4U stick getters both come through
// PresentSticks, so they cannot disagree.
//
// Note that a game does not see Gamepad_RightY with that sign: Unreal's game viewport negates that one key
// for every device, Xbox pads included (FSceneViewport::OnAnalogValueChanged). That flip is the engine's,
// downstream of this table, and must not be compensated for here.

#pragma once

#include "CoreMinimal.h"

struct FJoyShockState;

namespace JSL4UStickTable
{
	// Which stick slots a controller fills. A Joy-Con half has one stick, in its own side's slot.
	enum class EStickLayout : uint8
	{
		Whole,
		LeftHalf,
		RightHalf,
	};

	// The quarter turn a Joy-Con half is held at when sideways, seen from its face. The left half turns
	// counter-clockwise into that grip and the right half clockwise, so its stick is presented rotated the
	// same way. Motion is undone by the same turn (UJoyShockLibrary::GetJoyConGripUndoRotation).
	enum class ESidewaysTurn : uint8
	{
		None,
		CounterClockwise,
		Clockwise,
	};

	struct FRow
	{
		// FJoyShockInterface::GetHardwareDeviceIdentifier's name for the controller.
		const TCHAR* Identifier;
		EStickLayout Layout;
		// Multiplies the reader's value for each axis to reach up = +1, right = +1.
		int8 SignLX;
		int8 SignLY;
		int8 SignRX;
		int8 SignRY;
		ESidewaysTurn SidewaysTurn;
		// Who measured this row and on which controller, or that it is derived and still unmeasured.
		const TCHAR* MeasuredBy;
	};

	// The row for this identifier; an unknown controller gets a neutral row (all signs +1, whole, no turn).
	const FRow& RowFor(FName HardwareDeviceIdentifier);

	// The sticks as this plugin presents them: the row's signs applied to the reader's values, then, for a
	// Joy-Con half held sideways, its stick turned into that grip and presented as the left stick.
	void PresentSticks(const FJoyShockState& State, FName HardwareDeviceIdentifier, bool bHorizontal,
		float& OutLeftX, float& OutLeftY, float& OutRightX, float& OutRightY);

	// The sideways turn of a Joy-Con half, for the motion undo, which only knows which half it holds.
	ESidewaysTurn SidewaysTurnForJoyConHalf(bool bIsLeft);
}
