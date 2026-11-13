#include "JoyShockStickTable.h"

#include "JoyShockTypes.h"

namespace JSL4UStickTable
{
	// One row per controller, because it is the controller that decides how its sticks behave even where two
	// share a protocol reader. Changing a row changes that controller and nothing else.
	//
	// Measured rows were read in LV_JoyShockGyroAim's stick check (R-001), which shows what Unreal receives
	// before any Enhanced Input modifier; the right stick's vertical reads -1 there for every controller,
	// because of the engine's own negation described in the header, so +1 below means the controller sends
	// up as +1. Derived rows reproduce what the code before this table did, which no one has reported wrong,
	// and are waiting for a measurement.
	static const FRow Rows[] =
	{
		// Sony's report counts each Y axis from the top, so up is the low byte.
		{ TEXT("DualShock4"), EStickLayout::Whole, +1, -1, +1, -1, ESidewaysTurn::None,
			TEXT("Antonio, 2026-11-06: DualShock 4 over USB") },
		{ TEXT("DualSense"), EStickLayout::Whole, +1, -1, +1, -1, ESidewaysTurn::None,
			TEXT("Derived: same byte layout as the DualShock 4. Also the DualSense Edge, which reports as a ")
			TEXT("DualSense. Unmeasured") },

		// Nintendo's sticks count up as positive on both slots of every controller.
		{ TEXT("Switch2ProController"), EStickLayout::Whole, +1, +1, +1, +1, ESidewaysTurn::None,
			TEXT("Antonio, 2026-11-06: Pro Controller 2 over USB and Bluetooth") },
		{ TEXT("SwitchProController"), EStickLayout::Whole, +1, +1, +1, +1, ESidewaysTurn::None,
			TEXT("Derived. Unmeasured") },
		{ TEXT("JoyConLeft"), EStickLayout::LeftHalf, +1, +1, +1, +1, ESidewaysTurn::CounterClockwise,
			TEXT("Antonio, 2026-11-06: Joy-Con (L) sideways and in a joined pair") },
		{ TEXT("JoyConRight"), EStickLayout::RightHalf, +1, +1, +1, +1, ESidewaysTurn::Clockwise,
			TEXT("Antonio, 2026-11-06: Joy-Con (R) sideways and in a joined pair") },
		{ TEXT("JoyCon2Left"), EStickLayout::LeftHalf, +1, +1, +1, +1, ESidewaysTurn::CounterClockwise,
			TEXT("Derived. Unmeasured") },
		{ TEXT("JoyCon2Right"), EStickLayout::RightHalf, +1, +1, +1, +1, ESidewaysTurn::Clockwise,
			TEXT("Derived. Unmeasured") },
	};

	static const FRow NeutralRow =
		{ TEXT("JoyShockGamepad"), EStickLayout::Whole, +1, +1, +1, +1, ESidewaysTurn::None, TEXT("Default") };

	const FRow& RowFor(FName HardwareDeviceIdentifier)
	{
		// Names built once: this runs for every controller on every frame.
		static const TArray<FName> RowNames = []()
		{
			TArray<FName> Names;
			for (const FRow& Row : Rows)
			{
				Names.Add(FName(Row.Identifier));
			}
			return Names;
		}();

		const int32 Index = RowNames.IndexOfByKey(HardwareDeviceIdentifier);
		return Index == INDEX_NONE ? NeutralRow : Rows[Index];
	}

	void PresentSticks(const FJoyShockState& State, FName HardwareDeviceIdentifier, bool bHorizontal,
		float& OutLeftX, float& OutLeftY, float& OutRightX, float& OutRightY)
	{
		const FRow& Row = RowFor(HardwareDeviceIdentifier);

		// First the signs, so everything after works in the plugin's convention, whatever the controller.
		const float LeftX = State.stickLX * Row.SignLX;
		const float LeftY = State.stickLY * Row.SignLY;
		const float RightX = State.stickRX * Row.SignRX;
		const float RightY = State.stickRY * Row.SignRY;

		OutLeftX = 0.0f;
		OutLeftY = 0.0f;
		OutRightX = 0.0f;
		OutRightY = 0.0f;

		if (Row.Layout == EStickLayout::Whole)
		{
			OutLeftX = LeftX;
			OutLeftY = LeftY;
			OutRightX = RightX;
			OutRightY = RightY;
			return;
		}

		const bool bLeftHalf = Row.Layout == EStickLayout::LeftHalf;
		const float StickX = bLeftHalf ? LeftX : RightX;
		const float StickY = bLeftHalf ? LeftY : RightY;

		if (bHorizontal && Row.SidewaysTurn != ESidewaysTurn::None)
		{
			// Held sideways, a half is a controller of its own, and its one stick is that controller's left
			// stick, whichever half it is. The turn is applied to the already-signed vector, so a row's signs
			// and its turn can be checked one at a time.
			if (Row.SidewaysTurn == ESidewaysTurn::CounterClockwise)
			{
				OutLeftX = -StickY;
				OutLeftY = StickX;
			}
			else
			{
				OutLeftX = StickY;
				OutLeftY = -StickX;
			}
			return;
		}

		if (bLeftHalf)
		{
			OutLeftX = StickX;
			OutLeftY = StickY;
		}
		else
		{
			OutRightX = StickX;
			OutRightY = StickY;
		}
	}

	ESidewaysTurn SidewaysTurnForJoyConHalf(bool bIsLeft)
	{
		return RowFor(bIsLeft ? FName(TEXT("JoyConLeft")) : FName(TEXT("JoyConRight"))).SidewaysTurn;
	}
}
