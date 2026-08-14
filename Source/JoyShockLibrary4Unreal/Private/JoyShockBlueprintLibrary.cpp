// JoyShockBlueprintLibrary.cpp - The Blueprint nodes, implemented.
//
// The other half of Public/JoyShockBlueprintLibrary.h: reading state off a controller, motion and gyro
// calibration, Joy-Con pairing, player assignment, rumble and lights. Almost everything here is a thin,
// locked read of a JoyShock the registry already owns -- the hardware work happens on the polling thread.
//
// Two nodes that belong to this class live elsewhere because they are enumeration, not queries:
// ConnectDevices and JSL4URefreshControllers are in JoyShockEnumeration.cpp.

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

int32 UJoyShockLibrary::GetConnectedDeviceHandles(TArray<int32>& OutDeviceHandleArray)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	int i = 0;
	JSL4UModule._connectedLock.lock_shared();

	for (TTuple<signed int, JoyShock*> pair : _joyshocks)
	{
		// Skip devices that have not delivered input yet: they are enumerable, but not confirmed to be
		// working controllers, and the connect event for them has not been (and may never be) sent.
		if (pair.Value == nullptr || !pair.Value->has_delivered_input.load())
		{
			continue;
		}
		OutDeviceHandleArray.Add(pair.Key);
		i++;
	}
	JSL4UModule._connectedLock.unlock_shared();
	return i; // return num actually found
}

// --- Joy-Con pairing (Blueprint) ------------------------------------------------------------------------

static EJSL4UControllerType JSL4UControllerTypeFromLegacy(int32 LegacyType)
{
	switch (LegacyType)
	{
	case JS_TYPE_JOYCON_LEFT:    return EJSL4UControllerType::JoyConLeft;
	case JS_TYPE_JOYCON_RIGHT:   return EJSL4UControllerType::JoyConRight;
	case JS_TYPE_PRO_CONTROLLER: return EJSL4UControllerType::ProController;
	case JS_TYPE_PRO_CONTROLLER_2: return EJSL4UControllerType::ProController2;
	case JS_TYPE_JOYCON2_LEFT:   return EJSL4UControllerType::JoyCon2Left;
	case JS_TYPE_JOYCON2_RIGHT:  return EJSL4UControllerType::JoyCon2Right;
	case JS_TYPE_DS4:            return EJSL4UControllerType::DualShock4;
	case JS_TYPE_DS:             return EJSL4UControllerType::DualSense;
	default:                     return EJSL4UControllerType::Undefined;
	}
}

// Reads everything the JoyShock itself owns. The caller must hold _connectedLock (shared is enough) and
// must have already null-checked jc. Kept separate from the locking so that callers which already hold
// the lock for a whole batch of devices don't have to re-acquire it per device.
static FJSL4URawSettings ReadControllerSettings(JoyShock* jc)
{
	FJSL4URawSettings settings;

	settings.gyroSpace = jc->gyroSpace;
	settings.playerNumber = jc->player_number;
	settings.splitType = jc->left_right;
	settings.isConnected = true;
	settings.isCalibrating = jc->use_continuous_calibration;
	settings.autoCalibrationEnabled = jc->motion.GetCalibrationMode() != GamepadMotionHelpers::CalibrationMode::Manual;

	switch (jc->controller_type)
	{
	case ControllerType::s_ds4:
		settings.controllerType = JS_TYPE_DS4;
		break;
	case ControllerType::s_ds:
		settings.controllerType = JS_TYPE_DS;
		break;
	default:
	case ControllerType::n_switch:
		settings.controllerType = jc->switch_legacy_type();
		settings.colour = jc->body_colour;
		break;
	}

	if (jc->controller_type != ControllerType::n_switch)
	{
		// get led colour
		settings.colour = (int)(jc->led_b) | ((int)(jc->led_g) << 8) | ((int)(jc->led_r) << 16);
	}

	return settings;
}

// Builds the Blueprint-facing struct from the raw JSL settings. Leaves every field the interface owns --
// the whole identity block, from Connection Id down -- at its defaults; see JSL4UFillPlayerFields, which is
// what turns this into a controller a caller can name.
static FJSL4UControllerInfo JSL4UMakeControllerInfo(const FJSL4URawSettings& RawSettings, const JoyShock* jc)
{
	// The colour arrives packed as 0xRRGGBB (both the Switch body colour read over SPI and the DS4/DualSense
	// LED colour are assembled that way), so it has to go into FColor as (R, G, B) -- passing it reversed
	// swapped red and blue, turning a blue Joy-Con red on screen.
	const uint32 RGBColor = RawSettings.colour;
	const uint8 Red = (RGBColor >> 16) & 0xff;
	const uint8 Green = (RGBColor >> 8) & 0xff;
	const uint8 Blue = RGBColor & 0xff;

	FJSL4UControllerInfo Info;
	// Everything built here is, by definition, a controller this plugin drives. The flag exists for the one
	// node that also reports controllers it does not -- see FJSL4UControllerInfo::bIsJoyShockController.
	Info.bIsJoyShockController = true;
	Info.ControllerType = JSL4UControllerTypeFromLegacy(RawSettings.controllerType);
	Info.PlayerLedNumber = RawSettings.playerNumber;

	if (jc != nullptr)
	{
		// The polling thread stores 0-4 (or BatteryLevelUnknown); the Blueprint enum reserves 0 for
		// Unknown, so a reported level shifts up by one. Anything unexpected reads as Unknown rather than
		// as a low battery -- a wrong "flat controller" warning is worse than no warning.
		const uint8 RawLevel = jc->battery_level.load();
		Info.BatteryLevel = RawLevel <= 4
			? static_cast<EJSL4UBatteryLevel>(RawLevel + 1)
			: EJSL4UBatteryLevel::Unknown;
		Info.bIsCharging = jc->battery_charging.load();
		Info.BatteryPercent = jc->battery_percent.load();
	}
	Info.Color = FColor(Red, Green, Blue);
	// Every family this plugin drives has both; Undefined means one it drives but could not identify, and
	// there is nothing to promise about a controller we cannot name. XInputController is excluded
	// explicitly even though nothing built here can be one: this builder is for controllers we drive, and
	// the day something routes a foreign device through it, "not Undefined" would quietly have an Xbox pad
	// claiming a gyroscope it does not have.
	const bool bIsDrivenFamily = Info.ControllerType != EJSL4UControllerType::Undefined
		&& Info.ControllerType != EJSL4UControllerType::XInputController;
	Info.bHasMotionSensors = bIsDrivenFamily;
	Info.bHasRumble = bIsDrivenFamily;
	Info.bHasRgbLight = Info.ControllerType == EJSL4UControllerType::DualShock4
		|| Info.ControllerType == EJSL4UControllerType::DualSense;
	Info.bHasTouchpad = Info.ControllerType == EJSL4UControllerType::DualShock4
		|| Info.ControllerType == EJSL4UControllerType::DualSense;
	Info.bHasPlayerIndicator = Info.ControllerType == EJSL4UControllerType::JoyConLeft
		|| Info.ControllerType == EJSL4UControllerType::JoyConRight
		|| Info.ControllerType == EJSL4UControllerType::ProController
		|| Info.ControllerType == EJSL4UControllerType::ProController2
		|| Info.ControllerType == EJSL4UControllerType::JoyCon2Left
		|| Info.ControllerType == EJSL4UControllerType::JoyCon2Right
		|| Info.ControllerType == EJSL4UControllerType::DualSense;
	// JSL stores the gyro space as a raw int. Anything outside the three JSL4U knows about reads as Local
	// Space rather than as a garbage enumerator, so a value the library grows later cannot make a Blueprint
	// switch fall through a branch that does not exist.
	Info.GyroSpace = RawSettings.gyroSpace >= 0 && RawSettings.gyroSpace <= static_cast<int32>(EJSL4UGyroSpace::PlayerSpace)
		? static_cast<EJSL4UGyroSpace>(RawSettings.gyroSpace)
		: EJSL4UGyroSpace::LocalSpace;
	Info.bIsCalibrating = RawSettings.isCalibrating;
	Info.bAutoCalibrationEnabled = RawSettings.autoCalibrationEnabled;
	Info.bIsConnected = RawSettings.isConnected;
	return Info;
}

// Fills the fields the input-device interface owns. Must NOT be called while holding _connectedLock:
// the game thread takes the interface's lock and then calls the device getters (which take _connectedLock),
// so acquiring them in the opposite order here could deadlock.
//
// False means the interface has no live registration for this controller, so none of the identity it owns
// -- Connection Id, Input Device Id, Platform User Id, player slot -- was written. The caller must then
// drop the controller rather than hand out the struct: see JSL4UGetConnectedControllers.
static bool JSL4UFillPlayerFields(FJSL4UControllerInfo& Info, int32 Handle, FJoyShockInterface* Interface)
{
	return Interface != nullptr && Interface->FillControllerInfo(Info, Handle);
}

FQuat UJoyShockLibrary::GetJoyConGripUndoRotation(bool bHorizontal, bool bIsLeft)
{
	// A Joy-Con held sideways is the same hardware turned a quarter turn about the axis that runs out of its
	// FACE -- Unreal's +Z once the readings are in Unreal axes, the same axis a yaw turns about. Its IMU has
	// no idea that happened, so every reading arrives a quarter turn out of true: what the player does as
	// "point the far end up" comes out as roll, and the pitch a game reads never moves. That is the whole
	// bug -- the buttons and the stick have been rotated for the grip since the solo-horizontal support
	// landed, and the motion never was.
	//
	// Undone here rather than left to each game, for the same reason the stick is: a Joy-Con held sideways is
	// a controller in its own right, and "read its pitch" should mean the same thing there as on a DualSense.
	//
	// The angle comes from the stick, whose rotation is hardware-verified: ProcessAnalogInputs presents the
	// left half's stick as (X, Y) -> (-Y, X). The stick lies in the plane of the face, so its "right" is
	// Unreal +Y and its "forward" is Unreal +X -- which makes that transform X' = Y, Y' = -X, a -90 degree
	// turn about +Z. The right half is turned the opposite way into its grip, so its undo is the positive.
	//
	// Getting the axis wrong is not subtle, and the check is worth writing down: a turn about the face
	// normal cannot change which way is DOWN for a controller lying flat on a table. Put a Joy-Con and any
	// other controller flat and facing the same way, and Get Motion State must report the same gravity for
	// both, near (0, 0, -1). It reported (0, -1, 0) for the Joy-Con while this rotated about +X instead.
	if (!bHorizontal)
	{
		return FQuat::Identity;
	}
	return FQuat(FVector::UpVector, bIsLeft ? -HALF_PI : HALF_PI);
}

// The rotation that undoes the grip of the controller this handle names, or identity for anything that is
// not a Joy-Con held sideways.
//
// MUST be called before taking _connectedLock: this asks the interface, and the game thread takes the
// interface's lock and then calls the device getters (which take _connectedLock), so acquiring the two in the
// opposite order here could deadlock. Every caller below resolves it as its first statement.
static FQuat GripUndoRotationForHandle(int32 Handle)
{
	FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface();
	bool bHorizontal = false;
	bool bIsLeft = false;
	if (Interface == nullptr || !Interface->GetJoyConGrip(Handle, bHorizontal, bIsLeft))
	{
		return FQuat::Identity;
	}
	return UJoyShockLibrary::GetJoyConGripUndoRotation(bHorizontal, bIsLeft);
}

// Takes a whole motion state out of a sideways Joy-Con's frame. Identity does nothing, so callers need no
// guard of their own.
static void ApplyGripUndo(FJSL4UMotionState& MotionState, const FQuat& GripUndo)
{
	if (GripUndo.IsIdentity())
	{
		return;
	}

	// Conjugated -- both sides -- and the reason is the reset that starts a manual calibration.
	//
	// The orientation is a rotation FROM a reference pose TO the controller's current one, and
	// motion.Reset() (which Start Manual Gyro Calibration cues) makes the pose the player is holding
	// become that reference. So the quarter turn sits in both frames at once, not just in the device's.
	// Taking it out of one side only counted it twice: at the very moment of calibrating, with the
	// controller perfectly still, the reading came back as a 90 degree roll -- a Joy-Con calibrated lying
	// sideways reported itself as standing up, and stayed a quarter turn out from then on.
	//
	// Conjugating satisfies both things at once, which one-sided multiplication cannot:
	//   - calibrate in the grip you play in, and the reading is neutral, like every other controller;
	//   - turn the far end up, and it arrives as pitch rather than as roll (the bug this whole rotation
	//     exists for -- a rotation axis is carried into the corrected frame by the same conjugation).
	//
	// The three vectors below are readings in the controller's own frame, with no second frame to
	// reconcile, so they stay a plain rotation.
	MotionState.Orientation = GripUndo * MotionState.Orientation * GripUndo.Inverse();
	MotionState.Acceleration = GripUndo.RotateVector(MotionState.Acceleration);
	MotionState.Gravity = GripUndo.RotateVector(MotionState.Gravity);
}

// Whether this controller's gyro readings are still in its own frame, which is the only case the grip
// rotation applies to. In World or Player space the library has already resolved the gyro against gravity,
// so the reading is relative to the room rather than to the controller and is grip-independent already --
// rotating it again would take a correctly-reported turn and tilt it a quarter turn out.
static bool IsGyroInLocalSpace(int32 Handle)
{
	return UJoyShockLibrary::GetControllerSettingsForHandle(Handle).gyroSpace == 0;
}

// The library handle that addresses this connection, or INDEX_NONE when nothing of ours answers to it: a
// pad Unreal drives (negative id), a connection that has ended, or an uninitialised 0.
//
// Every public node goes through here, and INDEX_NONE is deliberately passed on down rather than turned
// into an early return. The layer below is a TMap keyed by handle: a miss returns nullptr, and every getter
// answers nullptr with the zeroed reading its capability flag already promised. That is what lets a game
// call Get Motion State on whatever controller the player happens to be holding, Xbox pad included, without
// asking whose controller it is first.
// Remembers which (connection, call site) pairs have already been warned about. These warnings come from
// nodes that are usually called from a Tick -- an options screen driving whatever controller is selected --
// so warning on every call would bury the log in a single frame. Keyed by the literal's address rather than
// its text so the hot path costs a hash lookup and no allocation.
static FCriticalSection GUnservableWarningLock;
static TSet<TPair<int64, UPTRINT>> GUnservableWarnings;

static int32 HandleForConnection(int64 ConnectionId)
{
	FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface();
	const int32 Handle = Interface != nullptr ? Interface->GetHandleForConnection(ConnectionId) : INDEX_NONE;

	// A reading that quietly comes back as zeroes is the hardest thing to debug in this whole API: it looks
	// exactly like a controller lying still. A NEGATIVE id is not that -- it is a pad we do not drive, and
	// zeroes are the honest answer for hardware without the sensor. Anything else reaching here is an id
	// that names no live controller of ours: an uninitialised 0, or one whose controller is gone. Said once
	// per id, so a getter in a Tick cannot bury the log.
	if (Handle == INDEX_NONE && ConnectionId >= 0)
	{
		static const TCHAR* ReadingSite = TEXT("JSL4U reading");
		bool bAlreadyWarned = false;
		{
			FScopeLock Lock(&GUnservableWarningLock);
			GUnservableWarnings.Add(TPair<int64, UPTRINT>(ConnectionId, reinterpret_cast<UPTRINT>(ReadingSite)),
				&bAlreadyWarned);
		}
		if (!bAlreadyWarned)
		{
			FString Live = TEXT("none");
			if (Interface != nullptr)
			{
				TArray<int64> LiveIds;
				TArray<int32> Handles;
				UJoyShockLibrary::GetConnectedDeviceHandles(Handles);
				for (int32 LiveHandle : Handles)
				{
					const int64 LiveId = Interface->GetConnectionForHandle(LiveHandle);
					if (LiveId != 0)
					{
						LiveIds.Add(LiveId);
					}
				}
				if (LiveIds.Num() > 0)
				{
					Live = FString::JoinBy(LiveIds, TEXT(", "), [](int64 Id) { return LexToString(Id); });
				}
			}
			UE_LOG(LogJoyShockLibrary, Warning,
				TEXT("JSL4U: asked to read connection %lld, which is not a connected controller, so every ")
				TEXT("reading for it is zero. Connected right now: %s. A 0 here means the Blueprint passed an ")
				TEXT("unset variable -- Connection Id comes from the controller info a Wait For ... Changes ")
				TEXT("pin or Get All Connected Controllers handed you."),
				ConnectionId, *Live);
		}
	}
	return Handle;
}

// Resolves for a node that DRIVES a controller, warning once when nothing of ours answers to the id.
//
// Silence would be wrong here in a way it is not for a getter: a getter that returns zeroes has told the
// truth (the pad has no gyro to report), while "set the light colour" quietly doing nothing looks like a
// bug in the game. The call still does nothing -- that is the honest answer for hardware that lacks the
// feature -- but it says so once.
static int32 HandleForOutput(int64 ConnectionId, const TCHAR* NodeName)
{
	const int32 Handle = HandleForConnection(ConnectionId);
	if (Handle != INDEX_NONE)
	{
		return Handle;
	}

	{
		FScopeLock Lock(&GUnservableWarningLock);
		bool bAlreadyWarned = false;
		GUnservableWarnings.Add(TPair<int64, UPTRINT>(ConnectionId, reinterpret_cast<UPTRINT>(NodeName)),
			&bAlreadyWarned);
		if (bAlreadyWarned)
		{
			return INDEX_NONE;
		}
	}

	// The two cases read differently to whoever is debugging, so they are named apart: a negative id is a
	// pad this plugin does not drive (the game asked for something only its own controllers can do), while
	// anything else is an id that no longer refers to a live controller.
	if (ConnectionId < 0)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("%s: connection %lld is a controller this plugin does not drive, so this did nothing. ")
			TEXT("Its rumble is reachable (Set Controller Rumble routes it through Unreal's force feedback); ")
			TEXT("everything else here needs hardware it does not have. Check the capability flag on the ")
			TEXT("controller info before offering the option."), NodeName, ConnectionId);
	}
	else
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("%s: no connected controller has connection id %lld, so this did nothing. Connection ids ")
			TEXT("are not reused, so this one belongs to a controller that has been disconnected."),
			NodeName, ConnectionId);
	}
	return INDEX_NONE;
}

TArray<FJSL4UControllerInfo> UJoyShockLibrary::JSL4UGetConnectedControllers()
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	FJoyShockInterface* Interface = JSL4UModule.GetActiveInterface();

	// Each entry keeps the handle it was built from only for as long as this function runs: it is what the
	// interface is asked about below, and it is deliberately not carried out of here.
	TArray<TPair<int32, FJSL4UControllerInfo>> Found;
	{
		// One shared lock for every device, rather than one per device per getter.
		std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

		Found.Reserve(_joyshocks.Num());
		for (const TTuple<int32, JoyShock*>& Pair : _joyshocks)
		{
			// Same filter as GetConnectedDeviceHandles: only controllers that have actually delivered
			// input, so a device still lingering in enumeration after a disconnect never shows up here.
			if (Pair.Value != nullptr && Pair.Value->has_delivered_input.load())
			{
				Found.Add({ Pair.Key, JSL4UMakeControllerInfo(ReadControllerSettings(Pair.Value), Pair.Value) });
			}
		}
	}

	// A controller the library has opened is not yet a controller the engine knows about: the connect is
	// raised on a polling thread and only becomes an engine identity when the interface drains it on the
	// game thread (see PendingConnects). In between -- a whole engine-init's worth of frames for a
	// controller that was already plugged in at launch, since nothing drains until the loop starts ticking
	// -- the device is here, working and delivering input, with no Connection Id, no Input Device Id and no
	// player slot yet allocated.
	//
	// Listing it in that window handed out an identity that was simply the struct's defaults: Connection Id
	// 0, Input Device Id -1. Zero is not a Connection Id this plugin ever issues (they count from 1), so
	// anything keyed by it -- the demo's HUD, any per-controller map -- filed the controller under a key
	// that the connect event contradicted a frame later, and Get All Connected Controllers listed the same
	// pad twice, once as ours and once as an undriven device, because the -1 defeated its de-duplication.
	//
	// So the listing waits for the identity instead of inventing one. What a caller loses is a controller
	// for the frames before it is announced; what it gains is that every controller in this list has an
	// identity that agrees with the one Wait For Controller Changes and Wait For Any Controller Changes
	// report, which is the whole point of the list.
	TArray<FJSL4UControllerInfo> Result;
	Result.Reserve(Found.Num());
	for (TPair<int32, FJSL4UControllerInfo>& Entry : Found)
	{
		if (JSL4UFillPlayerFields(Entry.Value, Entry.Key, Interface))
		{
			Result.Add(MoveTemp(Entry.Value));
		}
	}

	// Connection order, which is what the ids count. It used to be handle order, and for the plugin's own
	// controllers the two agree often enough that no caller can tell them apart -- but handle order is the
	// library's business and no longer visible from a Blueprint, so ordering by the one identity the caller
	// can see is the only order it can reason about.
	Result.Sort([](const FJSL4UControllerInfo& A, const FJSL4UControllerInfo& B)
		{ return A.ConnectionId < B.ConnectionId; });
	return Result;
}

FJSL4UControllerInfo UJoyShockLibrary::JSL4UDescribeUndrivenDevice(FPlatformUserId PlatformUser,
	FInputDeviceId InputDevice)
{
	// Fill in what Unreal knows about any input device and leave the rest at its defaults, which is what
	// bIsJoyShockController staying false tells a Blueprint to expect.
	FJSL4UControllerInfo Info;
	Info.bIsJoyShockController = false;
	Info.InputDeviceId = InputDevice.GetId();

	// A pad this plugin does not drive still needs a name, and this is it: the same address every other
	// controller has, so one map, one node signature and one Blueprint path cover both kinds. Left at its
	// default of 0, every XInput pad shared one key -- a second one collided with the first in any map keyed
	// by it, and a disconnect removed whichever entry sat on 0.
	//
	// Negative, derived from Unreal's device id: the interface counts Connection Ids up from 1 (see
	// NextConnectionId in JoyShockInterface), so the two sources share a map without ever colliding.
	// Deriving beats counting because every caller then describes a pad identically -- a private counter
	// would make a disconnect match a value some other caller invented. The cost is that Unreal reuses a
	// device id once a pad leaves, so this is unique among connected controllers rather than unique
	// forever: good for keying a live map, not for storing on disk.
	Info.ConnectionId = -(static_cast<int64>(Info.InputDeviceId) + 1);

	Info.PlatformUserId = PlatformUser.GetInternalId();
	Info.PlayerIndex = IPlatformInputDeviceMapper::Get().GetUserIndexForPlatformUser(PlatformUser);
	Info.ControllerType = EJSL4UControllerType::XInputController;
	Info.bIsConnected = true;

	// The capability flags describe the HARDWARE, not what this plugin can drive -- "can I drive it" is
	// what Is Joy Shock Controller says. An XInput pad has rumble motors and a game should offer the setting
	// for it; that rumble is reached through Unreal's own force feedback, which drives our controllers and
	// this one from the same authored effect. Reporting false here made a game hide its vibration options
	// from the one pad every player owns. Motion, touchpad, lights and the player indicator stay false
	// because the hardware genuinely lacks them.
	Info.bHasRumble = true;

	if (const UInputDeviceSubsystem* InputDevices = UInputDeviceSubsystem::Get())
	{
		Info.HardwareDeviceIdentifier =
			InputDevices->GetInputDeviceHardwareIdentifier(InputDevice).HardwareDeviceIdentifier;
	}

	return Info;
}

TArray<FJSL4UControllerInfo> UJoyShockLibrary::JSL4UGetAllConnectedControllers()
{
	// Ours first and already sorted by device id, so the JSL4U block of this list is byte-for-byte the
	// answer Get Connected Controllers gives. Anything a game did with that list keeps working when it
	// switches to this one.
	TArray<FJSL4UControllerInfo> Result = JSL4UGetConnectedControllers();

	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
	const FInputDeviceId DefaultDevice = DeviceMapper.GetDefaultInputDevice();

	TArray<FInputDeviceId> ConnectedDevices;
	DeviceMapper.GetAllConnectedInputDevices(ConnectedDevices);
	for (const FInputDeviceId& InputDevice : ConnectedDevices)
	{
		// The keyboard and mouse share one device that is connected from the first frame of every game.
		// Wait For Any Controller Changes leaves it out for that reason and this has to agree with it --
		// a roster that disagrees with the event that built it is worse than either alone.
		if (InputDevice == DefaultDevice)
		{
			continue;
		}

		// Ours are already in, described by the plugin itself. Matching on Unreal's device id rather than
		// on the hardware name means a controller family the plugin gains later needs no change here.
		const bool bAlreadyListed = Result.ContainsByPredicate(
			[&InputDevice](const FJSL4UControllerInfo& Listed)
			{
				return Listed.InputDeviceId == InputDevice.GetId();
			});
		if (bAlreadyListed)
		{
			continue;
		}

		Result.Add(JSL4UDescribeUndrivenDevice(DeviceMapper.GetUserForInputDevice(InputDevice), InputDevice));
	}

	return Result;
}

bool UJoyShockLibrary::JSL4UIsControllerTypeJoinable(EJSL4UControllerType ControllerType)
{
	return ControllerType == EJSL4UControllerType::JoyConLeft
		|| ControllerType == EJSL4UControllerType::JoyConRight
		|| ControllerType == EJSL4UControllerType::JoyCon2Left
		|| ControllerType == EJSL4UControllerType::JoyCon2Right;
}

bool UJoyShockLibrary::JSL4UJoinJoyCons(int64 ConnectionIdA, int64 ConnectionIdB)
{
	if (ConnectionIdA == ConnectionIdB)
	{
		return false;
	}

	const int32 DeviceIdA = HandleForOutput(ConnectionIdA, TEXT("JSL4UJoinJoyCons"));
	const int32 DeviceIdB = HandleForOutput(ConnectionIdB, TEXT("JSL4UJoinJoyCons"));

	const EJSL4UControllerType TypeA = JSL4UControllerTypeFromLegacy(GetControllerTypeForHandle(DeviceIdA));
	const EJSL4UControllerType TypeB = JSL4UControllerTypeFromLegacy(GetControllerTypeForHandle(DeviceIdB));

	if (!JSL4UIsControllerTypeJoinable(TypeA) || !JSL4UIsControllerTypeJoinable(TypeB))
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("JSL4UJoinJoyCons: both controllers must be Joy-Cons (connections %lld and %lld)."), ConnectionIdA, ConnectionIdB);
		return false;
	}

	if (TypeA == TypeB)
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("JSL4UJoinJoyCons: expected one left and one right Joy-Con; both are the same side."));
		return false;
	}

	FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface();
	return Interface != nullptr && Interface->JoinControllers(DeviceIdA, DeviceIdB);
}

void UJoyShockLibrary::JSL4UUnjoinJoyCon(int64 ConnectionId)
{
	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4UUnjoinJoyCon"));
	if (FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface())
	{
		Interface->UnjoinController(DeviceId);
	}
}

void UJoyShockLibrary::JSL4UUnjoinAllJoyCons()
{
	if (FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface())
	{
		Interface->UnjoinAllControllers();
	}
}

bool UJoyShockLibrary::JSL4USetJoyConGripMode(int64 ConnectionId, EJSL4UJoyConGripMode GripMode)
{
	// Before resolving, so asking for a grip mode that means nothing is not also reported as a controller
	// that could not be served.
	if (GripMode == EJSL4UJoyConGripMode::NotApplicable)
	{
		return false;
	}

	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4USetJoyConGripMode"));
	FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface();
	return Interface != nullptr
		&& Interface->SetJoyConHorizontal(DeviceId, GripMode == EJSL4UJoyConGripMode::Horizontal);
}

bool UJoyShockLibrary::JSL4UGetJoyConPartner(int64 ConnectionId, int64& PartnerConnectionId)
{
	PartnerConnectionId = 0;

	FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface();
	if (Interface == nullptr)
	{
		return false;
	}

	const int32 PartnerHandle = Interface->GetJoinPartner(Interface->GetHandleForConnection(ConnectionId));
	if (PartnerHandle == INDEX_NONE)
	{
		return false;
	}

	// Named as the caller names everything else. A partner that has just left resolves to 0, which reads the
	// same as "no partner" -- and is the truth by then.
	PartnerConnectionId = Interface->GetConnectionForHandle(PartnerHandle);
	return PartnerConnectionId != 0;
}

bool UJoyShockLibrary::JSL4UIsJoyConPrimary(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface();
	// Without an interface there is no pairing at all, so every device stands alone -- and a standalone
	// device is its own primary. Returning true keeps a mirror visible rather than hiding everything.
	return Interface == nullptr || Interface->IsJoinPrimary(DeviceId);
}

bool UJoyShockLibrary::JSL4UGetJoyConPair(int64 ConnectionId, FJSL4UControllerInfo& PrimaryController,
	FJSL4UControllerInfo& PartnerController)
{
	PrimaryController = JSL4UGetControllerInfo(ConnectionId);
	PartnerController = FJSL4UControllerInfo();

	int64 PartnerConnectionId = 0;
	if (!JSL4UGetJoyConPartner(ConnectionId, PartnerConnectionId))
	{
		// Standalone: the caller's controller leads a group of one. Partner stays unset rather than being
		// filled with a copy, so "is there a second half" is answerable from the struct alone.
		return false;
	}

	const FJSL4UControllerInfo OtherInfo = JSL4UGetControllerInfo(PartnerConnectionId);

	// Order the two by the plugin's own grouping rule instead of by which half was asked about. Both halves
	// therefore get identical answers, which is what lets a caller act on a pair without first working out
	// which of the two it is holding.
	if (JSL4UIsJoyConPrimary(ConnectionId))
	{
		PartnerController = OtherInfo;
	}
	else
	{
		PartnerController = PrimaryController;
		PrimaryController = OtherInfo;
	}
	return true;
}

// The game viewport owning the world this Blueprint is running in, or null outside a game world.
static UGameViewportClient* JSL4UGetGameViewport(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine != nullptr
		? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull)
		: nullptr;
	return World != nullptr ? World->GetGameViewport() : nullptr;
}

bool UJoyShockLibrary::JSL4USetMaxLocalPlayers(const UObject* WorldContextObject, int32 MaxLocalPlayers)
{
	if (MaxLocalPlayers < 1)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("JSL4USetMaxLocalPlayers: %d is not a usable number of players; ignoring."), MaxLocalPlayers);
		return false;
	}

	UGameViewportClient* Viewport = JSL4UGetGameViewport(WorldContextObject);
	if (Viewport == nullptr)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("JSL4USetMaxLocalPlayers: no game viewport in this context, so the limit was not changed."));
		return false;
	}

	// Unreal's own name for this is MaxSplitscreenPlayers, but it is the ceiling UGameInstance::CreateLocalPlayer
	// checks whatever the screen is doing -- see the note on the declaration.
	Viewport->MaxSplitscreenPlayers = MaxLocalPlayers;
	return true;
}

int32 UJoyShockLibrary::JSL4UGetMaxLocalPlayers(const UObject* WorldContextObject)
{
	const UGameViewportClient* Viewport = JSL4UGetGameViewport(WorldContextObject);
	return Viewport != nullptr ? Viewport->MaxSplitscreenPlayers : INDEX_NONE;
}

bool UJoyShockLibrary::JSL4UAssignControllerToPlayerIndex(const FJSL4UControllerInfo& Controller, int32 PlayerIndex)
{
	// A controller this plugin does not drive has no slot in the plugin's table, and does not need one: the
	// engine decides which player an input device feeds by which platform user it is mapped to, so moving
	// it means remapping it there. Doing that here rather than making the caller find out which kind it is
	// holding is the whole point of taking the description instead of a bare id.
	if (!Controller.bIsJoyShockController)
	{
		if (Controller.InputDeviceId < 0)
		{
			UE_LOG(LogJoyShockLibrary, Warning,
				TEXT("JSL4UAssignControllerToPlayerIndex: this controller has no Unreal input device id, so ")
				TEXT("there is nothing to assign. Pass a controller from Get All Connected Controllers or ")
				TEXT("from one of the Wait For ... Changes nodes."));
			return false;
		}

		IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
		const FInputDeviceId InputDevice = FInputDeviceId::CreateFromInternalId(Controller.InputDeviceId);

		// -1 means "back to automatic" for our devices. Unreal spells the same idea as the unpaired user,
		// so the two halves of this function agree on what -1 does rather than one of them refusing it.
		const FPlatformUserId TargetUser = PlayerIndex < 0
			? DeviceMapper.GetUserForUnpairedInputDevices()
			: DeviceMapper.GetPlatformUserForUserIndex(PlayerIndex);

		if (!TargetUser.IsValid())
		{
			UE_LOG(LogJoyShockLibrary, Warning,
				TEXT("JSL4UAssignControllerToPlayerIndex: player %d has no platform user, so input device %d ")
				TEXT("was not assigned."), PlayerIndex, Controller.InputDeviceId);
			return false;
		}

		DeviceMapper.Internal_MapInputDeviceToUser(InputDevice, TargetUser, EInputDeviceConnectionState::Connected);
		UE_LOG(LogJoyShockLibrary, Verbose,
			TEXT("Input device %d is not one of ours; mapped it to platform user %d for player %d."),
			Controller.InputDeviceId, TargetUser.GetInternalId(), PlayerIndex);
		return true;
	}

	FJoyShockInterface* Interface = FJoyShockLibrary4UnrealModule::GetInstance().GetActiveInterface();
	if (Interface == nullptr)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("JSL4UAssignControllerToPlayerIndex: no input interface yet, so connection %lld was not assigned."),
			Controller.ConnectionId);
		return false;
	}

	// Resolved from the id rather than trusted from the struct: a caller can be holding a description from
	// an earlier event, and this is the point where "that controller is gone" has to become a failure
	// instead of an assignment landing on whichever controller took its place.
	if (!Interface->SetPlayerIndexForDevice(Interface->GetHandleForConnection(Controller.ConnectionId), PlayerIndex))
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("JSL4UAssignControllerToPlayerIndex: connection %lld is not a connected controller, so it was ")
			TEXT("not assigned to player %d. Connection ids are not reused, so a stored one whose controller ")
			TEXT("has been unplugged fails here rather than moving somebody else's controller."),
			Controller.ConnectionId, PlayerIndex);
		return false;
	}

	return true;
}

bool UJoyShockLibrary::JSL4UAssignControllerToPlayer(const FJSL4UControllerInfo& Controller, APlayerController* PlayerController)
{
	if (PlayerController == nullptr)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("JSL4UAssignControllerToPlayer: no player controller given, so connection %lld was not assigned. ")
			TEXT("Create Local Player returns null when it cannot make another player -- check the viewport's "
			TEXT("MaxSplitscreenPlayers if that is where this came from.")),
			Controller.ConnectionId);
		return false;
	}

	// Convert through the same IPlatformInputDeviceMapper the slot assignment uses -- see the note on
	// JSL4UGetControllersAssignedToPlayer about why the legacy controller id is the wrong number here.
	const FPlatformUserId User = PlayerController->GetPlatformUserId();
	const int32 UserIndex = IPlatformInputDeviceMapper::Get().GetUserIndexForPlatformUser(User);

	// A PlayerController only has a platform user once its ULocalPlayer has been attached, which does not
	// happen until after the controller has been spawned -- so this is reachable, and it used to be far
	// worse than a plain failure: passing the resulting -1 through to the index call means "hand the
	// controller back to automatic assignment", so the call quietly did the opposite of what was asked and
	// still reported success.
	if (!User.IsValid() || UserIndex < 0)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("JSL4UAssignControllerToPlayer: %s has no platform user yet, so connection %lld was not assigned. ")
			TEXT("Assign after the player controller has been created and possesses its pawn -- from the pawn's "
			TEXT("Possessed By, for instance, rather than from a Begin Play.")),
			*PlayerController->GetName(), Controller.ConnectionId);
		return false;
	}

	return JSL4UAssignControllerToPlayerIndex(Controller, UserIndex);
}

TArray<FJSL4UControllerInfo> UJoyShockLibrary::JSL4UGetControllersAssignedToPlayerIndex(int32 PlayerIndex)
{
	// Filtering a listing keeps this on an existing single-pass path rather than adding a second way to read
	// the same state. It filters the WIDE listing on purpose: a player can be assigned an XInput pad (see
	// JSL4UAssignControllerToPlayerIndex, which handles them through the device mapper), so answering from
	// the narrow one made assignment writable but not readable -- the pad went to the player and this node
	// then reported that player had no controller at all.
	TArray<FJSL4UControllerInfo> Result = JSL4UGetAllConnectedControllers();
	Result.RemoveAll([PlayerIndex](const FJSL4UControllerInfo& Info)
	{
		return Info.PlayerIndex != PlayerIndex;
	});
	return Result;
}

TArray<FJSL4UControllerInfo> UJoyShockLibrary::JSL4UGetControllersAssignedToPlayer(AController* Controller)
{
	// Accepting the base class lets the reference from a Pawn's Possessed event plug in without a
	// Blueprint-side cast; AI controllers simply own no physical input devices.
	const APlayerController* PlayerController = Cast<APlayerController>(Controller);
	if (PlayerController == nullptr)
	{
		return {};
	}

	// RefreshPlayerAssignments maps a device to GetPlatformUserForUserIndex(Slot), so convert back through
	// the same mapper. The player's legacy controller id is a different number and would silently pick the
	// wrong slot for anyone but player 0.
	const int32 UserIndex = IPlatformInputDeviceMapper::Get().GetUserIndexForPlatformUser(PlayerController->GetPlatformUserId());

	// A PlayerController with no Local Player yet (remote net client, or too early in startup) resolves to
	// index -1. Return empty rather than passing -1 through: unassigned devices also carry -1, so the
	// pass-through would hand back controllers that belong to nobody.
	if (UserIndex < 0)
	{
		return {};
	}

	return JSL4UGetControllersAssignedToPlayerIndex(UserIndex);
}

// A DisconnectAndDisposeAll used to live here. It was exposed to Blueprint, called by nothing, and
// unrecoverable: it unbound the module's connect/disconnect/poll delegates, which only the input
// interface's constructor ever binds, so calling it left every controller dead for the rest of the
// session with no way back short of restarting the editor. Removed rather than given a JSL4U name --
// there is no situation in which a game should be tearing the device layer down underneath itself.

bool UJoyShockLibrary::JSL4UIsControllerConnected(int64 ConnectionId)
{
	// "Is this controller still there" is the one question a game asks about EVERY controller it has an id
	// for, so answering only for ours would make a caller keep a second test for the pads it does not drive
	// -- and every Blueprint holding a stored id would need to know which kind it stored.
	if (ConnectionId < 0)
	{
		const FInputDeviceId InputDevice = FInputDeviceId::CreateFromInternalId(
			static_cast<int32>(-ConnectionId - 1));
		TArray<FInputDeviceId> ConnectedDevices;
		IPlatformInputDeviceMapper::Get().GetAllConnectedInputDevices(ConnectedDevices);
		return ConnectedDevices.Contains(InputDevice);
	}

	// For ours, the id resolving at all already means a live connection -- the interface only answers for
	// handles it has connected -- and the library-side test below then agrees with what the listings show.
	const int32 DeviceId = HandleForConnection(ConnectionId);

	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	// Same test the JSL4U listings use, so this can never disagree with JSL4UGetConnectedControllers about
	// whether a controller is there: present in the device map AND having delivered at least one report.
	const JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	return jc != nullptr && jc->has_delivered_input.load();
}

// get buttons as bits in the following order, using North South East West to name face buttons to avoid ambiguity between Xbox and Nintendo layouts:
// 0x00001: up
// 0x00002: down
// 0x00004: left
// 0x00008: right
// 0x00010: plus
// 0x00020: minus
// 0x00040: left stick click
// 0x00080: right stick click
// 0x00100: L
// 0x00200: R
// ZL and ZR are reported as analogue inputs (GetLeftTrigger, GetRightTrigger), because DS4 and XBox controllers use analogue triggers, but we also have them as raw buttons
// 0x00400: ZL
// 0x00800: ZR
// 0x01000: S
// 0x02000: E
// 0x04000: W
// 0x08000: N
// 0x10000: home / PS
// 0x20000: capture / touchpad-click
// 0x40000: SL
// 0x80000: SR
// if you want the whole state, this is the best way to do it
FJoyShockState UJoyShockLibrary::GetSimpleStateForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state;
	}
	return {};
}

FJSL4UJoyShockState UJoyShockLibrary::JSL4UGetControllerState(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	const FJoyShockState& LegacySimpleState = GetSimpleStateForHandle(DeviceId);
	return {
		.Buttons = LegacySimpleState.buttons,
		.LeftTrigger = LegacySimpleState.lTrigger,
		.RightTrigger = LegacySimpleState.rTrigger,
		.LeftStick = FVector2D(LegacySimpleState.stickLX, LegacySimpleState.stickLY),
		.RightStick = FVector2D(LegacySimpleState.stickRX, LegacySimpleState.stickRY)
	};
}

FIMUState UJoyShockLibrary::GetRawIMUStateForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->get_transformed_imu_state(jc->imu_state);
	}
	return {};
}

FJSL4UIMUState UJoyShockLibrary::JSL4UGetIMUState(int64 ConnectionId)
{
	const int32 DeviceID = HandleForConnection(ConnectionId);
	const FQuat GripUndo = GripUndoRotationForHandle(DeviceID);
	const bool bRotateGyro = !GripUndo.IsIdentity() && IsGyroInLocalSpace(DeviceID);

	const FIMUState& LegacyIMUState = GetRawIMUStateForHandle(DeviceID);
	const FVector Acceleration(LegacyIMUState.accelZ, LegacyIMUState.accelX, -LegacyIMUState.accelY);
	// Negate rotation around vertical axis to make up for different handedness
	const FVector Gyro(-LegacyIMUState.gyroZ, LegacyIMUState.gyroX, -LegacyIMUState.gyroY);
	return {
		.Acceleration = GripUndo.RotateVector(Acceleration),
		.Gyro = bRotateGyro ? GripUndo.RotateVector(Gyro) : Gyro
	};
}

FJSL4UIMUState UJoyShockLibrary::JSL4UGetRawIMUState(int64 ConnectionId)
{
	const int32 DeviceID = HandleForConnection(ConnectionId);
	const FIMUState& LegacyIMUState = GetRawIMUStateForHandle(DeviceID);
	return {
		.Acceleration = FVector(LegacyIMUState.accelX, LegacyIMUState.accelY, LegacyIMUState.accelZ),
		.Gyro = FVector(LegacyIMUState.gyroX, LegacyIMUState.gyroY, LegacyIMUState.gyroZ)
	};

	// Right-handed Y-UP (X-RIGHT/Z-BACK)
	// Raw X = JSL4U Y
	// Raw Y = JSL4U Z
	// Raw Z = JSL4U -X
}

FMotionState UJoyShockLibrary::GetRawMotionStateForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->get_motion_state();
	}
	return {};
}

FJSL4UMotionState UJoyShockLibrary::JSL4UGetMotionState(int64 ConnectionId)
{
	return GetMotionStateForHandle(HandleForConnection(ConnectionId));
}

FJSL4UMotionState UJoyShockLibrary::GetMotionStateForHandle(int32 DeviceHandle)
{
	/* TEMP DEBUG
	FVector Origin = FVector(280.0, 0.0f, 0.0f);
	FWorldContext* WorldContext = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport);
	UWorld* World = WorldContext->World();
	FVector TempFlattened = GamepadMotionHelpers::Motion::Flattened;
	// FVector TempFlattened = FVector(GamepadMotionHelpers::Motion::FlattenedX, GamepadMotionHelpers::Motion::FlattenedY, GamepadMotionHelpers::Motion::FlattenedZ);
	UKismetSystemLibrary::DrawDebugArrow(World, Origin, Origin + TempFlattened * 200.0f, 2.0f, FColor::Red);
	TEMP DEBUG */

	FMotionState NativeMotionState = GetRawMotionStateForHandle(DeviceHandle);
	FJSL4UMotionState UnrealMotionState;


	// Gravity-corrected orientation. This used to return rawQuat with a note blaming a bug in
	// GamepadMotion.hpp, alongside two commented-out attempts at the corrected quaternion -- each using a
	// DIFFERENT sign pattern from the one on the line below, and neither behaving correctly.
	//
	// That was the whole bug. Quaternion and RawQuaternion are the same quantity in the same coordinate
	// space: both start at identity and accumulate the same per-frame `*= rotation`, and the only
	// difference is the gravity correction applied to one of them (see Motion::Update). So whatever
	// conversion is right for one is right for the other, and swapping in a different sign pattern along
	// with the corrected quaternion made a working conversion look like broken correction maths. The
	// correction itself is upstream JibbSmart code, unmodified here apart from std:: -> FMath:: swaps.
	UnrealMotionState.Orientation = FQuat(NativeMotionState.quatZ, -NativeMotionState.quatX, -NativeMotionState.quatY, NativeMotionState.quatW);

	UnrealMotionState.Acceleration = FVector(NativeMotionState.accelZ, NativeMotionState.accelX, -NativeMotionState.accelY);
	UnrealMotionState.Gravity = FVector(-NativeMotionState.gravZ, NativeMotionState.gravX, NativeMotionState.gravY);

	// A Joy-Con held sideways reports everything a quarter turn out of true, orientation included -- see
	// GetJoyConGripUndoRotation for what the turn is, and ApplyGripUndo for why the orientation is
	// conjugated while the vectors are simply rotated.
	//
	// This is the getter a game reads to aim, so it is the one that has to be right: pointing the far end of
	// a sideways Joy-Con up used to arrive as roll, leaving the pitch a game reads dead still.
	ApplyGripUndo(UnrealMotionState, GripUndoRotationForHandle(DeviceHandle));
	return UnrealMotionState;
}

FJSL4UMotionState UJoyShockLibrary::JSL4UGetRawMotionState(int64 ConnectionId)
{
	const int32 DeviceID = HandleForConnection(ConnectionId);
	FMotionState NativeMotionState = GetRawMotionStateForHandle(DeviceID);
	FJSL4UMotionState UnrealMotionState;

	// The uncorrected orientation -- which is what "raw" means here, the counterpart to the corrected one
	// JSL4UGetMotionState returns. This read quat (the CORRECTED quaternion) with a third sign pattern of
	// its own: the two getters were reading each other's quaternion. Axes are converted exactly as in
	// JSL4UGetMotionState, so both getters describe the same space and differ only in the correction --
	// matching how JSL4UGetRawIMUState relates to JSL4UGetIMUState.
	UnrealMotionState.Orientation = FQuat(NativeMotionState.rawQuatZ, -NativeMotionState.rawQuatX, -NativeMotionState.rawQuatY, NativeMotionState.rawQuatW);

	UnrealMotionState.Acceleration = FVector(NativeMotionState.accelZ, NativeMotionState.accelX, -NativeMotionState.accelY);
	UnrealMotionState.Gravity = FVector(-NativeMotionState.gravZ, NativeMotionState.gravX, NativeMotionState.gravY);

	// "Raw" here means without the gravity correction, not in some other space -- so the grip comes out of
	// this one too, or the two getters would describe different spaces the moment a Joy-Con is held sideways.
	ApplyGripUndo(UnrealMotionState, GripUndoRotationForHandle(DeviceID));
	return UnrealMotionState;
}

FTouchState UJoyShockLibrary::GetTouchStateForHandle(int32 deviceId, bool previous)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return previous ? jc->last_touch_state : jc->touch_state;
	}
	return {};
}

FJSL4UTouchState UJoyShockLibrary::JSL4UGetTouchState(int64 ConnectionId, bool bPrevious)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	const FTouchState& LegacyTouchState = GetTouchStateForHandle(DeviceId, bPrevious);

	return {
		.PrimaryTouch = {
			.TouchID = LegacyTouchState.t0Id,
			.bIsDown = LegacyTouchState.t0Down,
			.Location = FVector2D(LegacyTouchState.t0X, LegacyTouchState.t0Y)
		},
		.SecondaryTouch = {
			.TouchID = LegacyTouchState.t1Id,
			.bIsDown = LegacyTouchState.t1Down,
			.Location = FVector2D(LegacyTouchState.t1X, LegacyTouchState.t1Y)
		}
	};
}

FVector2D UJoyShockLibrary::JSL4UGetTouchpadSize(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	int32 SizeX = 0;
	int32 SizeY = 0;
	// Returns zero for a controller with no touchpad, which is also what the legacy call leaves behind.
	GetTouchpadDimensionForHandle(DeviceId, SizeX, SizeY);
	return FVector2D(SizeX, SizeY);
}

bool UJoyShockLibrary::GetTouchpadDimensionForHandle(int32 deviceId, int32 &sizeX, int32 &sizeY)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	// I am assuming a single touchpad (or all touchpads are the same dimension)?
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr)
	{
		switch (jc->controller_type)
		{
		case ControllerType::s_ds4:
		case ControllerType::s_ds:
			sizeX = 1920;
			sizeY = 943;
			break;
		default:
			sizeX = 0;
			sizeY = 0;
			break;
		}
		return true;
	}
	return false;
}

FVector2D UJoyShockLibrary::JSL4UGetLeftStick(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc != nullptr)
	{
		return FVector2D(jc->simple_state.stickLX, jc->simple_state.stickLY);
	}
	return FVector2D::ZeroVector;
}

FVector2D UJoyShockLibrary::JSL4UGetRightStick(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc != nullptr) {
		return FVector2D(jc->simple_state.stickRX, jc->simple_state.stickRY);
	}
	return FVector2D::ZeroVector;
}

void UJoyShockLibrary::GetAndFlushAccumulatedGyroForHandle(int32 deviceId, float& gyroX, float& gyroY, float& gyroZ)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->get_and_flush_cumulative_gyro(gyroX, gyroY, gyroZ);
		return;
	}
	gyroX = gyroY = gyroZ = 0.f;
}

FVector UJoyShockLibrary::JSL4UGetAndClearAccumulatedGyro(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	const FQuat GripUndo = GripUndoRotationForHandle(DeviceId);
	// Same rule as the live gyro in JSL4UGetIMUState: only a reading still in the controller's own frame
	// needs the grip taken out of it. What accumulates here is whatever the poll thread transformed, so the
	// space question is the same question.
	const bool bRotate = !GripUndo.IsIdentity() && IsGyroInLocalSpace(DeviceId);

	float GyroX, GyroY, GyroZ;
	GetAndFlushAccumulatedGyroForHandle(DeviceId, GyroY, GyroZ, GyroX);
	const FVector Gyro(-GyroX, GyroY, -GyroZ);
	return bRotate ? GripUndo.RotateVector(Gyro) : Gyro;
}

void UJoyShockLibrary::SetGyroSpaceForHandle(int32 deviceId, int32 gyroSpace)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	if (gyroSpace < 0) {
		gyroSpace = 0;
	}
	if (gyroSpace > 2) {
		gyroSpace = 2;
	}
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->modifying_lock.Lock();
		jc->gyroSpace = gyroSpace;
		jc->modifying_lock.Unlock();
	}
}

void UJoyShockLibrary::JSL4USetGyroSpace(int64 ConnectionId, EJSL4UGyroSpace GyroSpace)
{
	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4USetGyroSpace"));
	SetGyroSpaceForHandle(DeviceId, static_cast<int32>(GyroSpace));
}

FVector2D UJoyShockLibrary::JSL4UGetTouchPosition(int64 ConnectionId, bool bSecondTouch)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc != nullptr) {
		if (!bSecondTouch) {
			return FVector2D(jc->touch_state.t0X, jc->touch_state.t0Y);
		}
		else {
			return FVector2D(jc->touch_state.t1X, jc->touch_state.t1Y);
		}
	}
	return FVector2D::ZeroVector;
}

// analog parameters have different resolutions depending on device
float UJoyShockLibrary::JSL4UGetStickResolutionStep(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	return GetStickStepForHandle(DeviceId);
}

float UJoyShockLibrary::JSL4UGetTriggerResolutionStep(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	return GetTriggerStepForHandle(DeviceId);
}

float UJoyShockLibrary::JSL4UGetPollInterval(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	return GetPollRateForHandle(DeviceId);
}

float UJoyShockLibrary::JSL4UGetSecondsSinceLastReport(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	return GetTimeSinceLastUpdateForHandle(DeviceId);
}

float UJoyShockLibrary::GetStickStepForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		if (jc->controller_type != ControllerType::n_switch) {
			return 1.0f / 128.0;
		}
		else {
			if (jc->left_right == 2) // right joycon has no calibration for left stick
			{
				return 1.0f / (jc->stick_cal_x_r[2] - jc->stick_cal_x_r[1]);
			}
			else {
				return 1.0f / (jc->stick_cal_x_l[2] - jc->stick_cal_x_l[1]);
			}
		}
	}
	return 0.0f;
}
float UJoyShockLibrary::GetTriggerStepForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->controller_type != ControllerType::n_switch ? 1 / 256.0f : 1.0f;
	}
	return 1.0f;
}
float UJoyShockLibrary::GetPollRateForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->controller_type != ControllerType::n_switch ? 250.0f : 66.6667f;
	}
	return 0.0f;
}
float UJoyShockLibrary::GetTimeSinceLastUpdateForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		auto time_now = std::chrono::steady_clock::now();
		return (float)(std::chrono::duration_cast<std::chrono::microseconds>(time_now - jc->last_polled).count() / 1000000.0);
	}
	return 0.0f;
}

// calibration
// --- Gyro calibration (JSL4U) ---------------------------------------------------------------------------

// The gyro axis convention JSL4U exposes everywhere else (see JSL4UGetIMUState / JSL4UGetAndClearAccumulatedGyro):
//   Unreal = (-jslZ, jslX, -jslY)
// The calibration offset lives in the same space as the raw gyro readings, so it is converted the same way
// -- otherwise an offset read back would not line up with the gyro values it is subtracted from.
static FVector JSL4UGyroToUnreal(float RawX, float RawY, float RawZ)
{
	return FVector(-RawZ, RawX, -RawY);
}

// Exact inverse of the above, for handing a value back to the library.
static void JSL4UGyroFromUnreal(const FVector& InVector, float& OutRawX, float& OutRawY, float& OutRawZ)
{
	OutRawX = InVector.Y;
	OutRawY = -InVector.Z;
	OutRawZ = -InVector.X;
}

void UJoyShockLibrary::JSL4USetGyroCalibrationMode(int64 ConnectionId, EJSL4UGyroCalibrationMode Mode)
{
	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4USetGyroCalibrationMode"));
	// Automatic is the library's SensorFusion+Stillness pair: it decides for itself when the controller is
	// being held still. Manual leaves it entirely to JSL4UStartManualGyroCalibration / JSL4UStopManualGyroCalibration.
	SetAutomaticCalibrationForHandle(DeviceId, Mode == EJSL4UGyroCalibrationMode::Automatic);
}

void UJoyShockLibrary::JSL4UStartManualGyroCalibration(int64 ConnectionId)
{
	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4UStartManualGyroCalibration"));
	StartContinuousCalibrationForHandle(DeviceId);
}

void UJoyShockLibrary::JSL4UStopManualGyroCalibration(int64 ConnectionId)
{
	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4UStopManualGyroCalibration"));
	PauseContinuousCalibrationForHandle(DeviceId);
}

void UJoyShockLibrary::JSL4UResetGyroCalibration(int64 ConnectionId)
{
	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4UResetGyroCalibration"));
	ResetContinuousCalibrationForHandle(DeviceId);
}

FJSL4UGyroCalibrationStatus UJoyShockLibrary::JSL4UGetGyroCalibrationStatus(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	FJSL4UGyroCalibrationStatus Status;

	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc != nullptr)
	{
		const bool bAutomatic = jc->motion.GetCalibrationMode() != GamepadMotionHelpers::CalibrationMode::Manual;

		Status.Mode = bAutomatic ? EJSL4UGyroCalibrationMode::Automatic : EJSL4UGyroCalibrationMode::Manual;
		Status.Confidence = jc->motion.GetAutoCalibrationConfidence();
		Status.bIsSteady = jc->motion.GetAutoCalibrationIsSteady();
		// use_continuous_calibration is what Start/Stop toggle, so it is the "manual calibration in progress"
		// flag -- which is the one a calibration screen needs, and the one the legacy status struct omitted.
		Status.bIsCalibrating = jc->use_continuous_calibration;
	}

	return Status;
}

FVector UJoyShockLibrary::JSL4UGetGyroCalibrationOffset(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	float RawX = 0.f, RawY = 0.f, RawZ = 0.f;
	GetCalibrationOffsetForHandle(DeviceId, RawX, RawY, RawZ);
	return JSL4UGyroToUnreal(RawX, RawY, RawZ);
}

void UJoyShockLibrary::JSL4USetGyroCalibrationOffset(int64 ConnectionId, FVector Offset)
{
	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4USetGyroCalibrationOffset"));
	float RawX, RawY, RawZ;
	JSL4UGyroFromUnreal(Offset, RawX, RawY, RawZ);
	SetCalibrationOffsetForHandle(DeviceId, RawX, RawY, RawZ);
}

void UJoyShockLibrary::ResetContinuousCalibrationForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->reset_continuous_calibration();
	}
}
void UJoyShockLibrary::StartContinuousCalibrationForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->use_continuous_calibration = true;
		jc->cue_motion_reset = true;
	}
}
void UJoyShockLibrary::PauseContinuousCalibrationForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->use_continuous_calibration = false;
	}
}
void UJoyShockLibrary::SetAutomaticCalibrationForHandle(int32 deviceId, bool enabled)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();	
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->modifying_lock.Lock();
		jc->motion.SetCalibrationMode(enabled ? GamepadMotionHelpers::CalibrationMode::SensorFusion | GamepadMotionHelpers::CalibrationMode::Stillness : GamepadMotionHelpers::CalibrationMode::Manual);
		jc->modifying_lock.Unlock();
	}
}
void UJoyShockLibrary::GetCalibrationOffsetForHandle(int32 deviceId, float& xOffset, float& yOffset, float& zOffset)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		// not technically modifying, but also not a simple getter
		jc->modifying_lock.Lock();
		jc->motion.GetCalibrationOffset(xOffset, yOffset, zOffset);
		jc->modifying_lock.Unlock();
	}
}

void UJoyShockLibrary::SetCalibrationOffsetForHandle(int32 deviceId, float xOffset, float yOffset, float zOffset)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->modifying_lock.Lock();
		jc->motion.SetCalibrationOffset(xOffset, yOffset, zOffset, 1);
		jc->modifying_lock.Unlock();
	}
}
FJSL4UControllerInfo UJoyShockLibrary::GetControllerInfoForHandle(int32 Handle)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	FJoyShockInterface* Interface = JSL4UModule.GetActiveInterface();

	FJSL4UControllerInfo Info;
	{
		std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

		JoyShock* jc = GetJoyShockFromHandle(Handle);
		if (jc == nullptr)
		{
			return {}; // bIsConnected stays false
		}
		Info = JSL4UMakeControllerInfo(ReadControllerSettings(jc), jc);
	}

	// Same rule as the listing: a controller whose connect has not reached the game thread yet has no
	// identity to report, and reporting the defaults as one is what put a Connection Id of 0 into a
	// Blueprint. Is Connected false is the answer a caller already handles, since it is what an id naming
	// no live controller gives.
	if (!JSL4UFillPlayerFields(Info, Handle, Interface))
	{
		return {};
	}
	return Info;
}

FJSL4UControllerInfo UJoyShockLibrary::JSL4UGetControllerInfo(int64 ConnectionId)
{
	// A pad this plugin does not drive is not an error here: it has a description, it is simply Unreal's
	// rather than ours, and a caller holding its id deserves the same struct back that the listing gave.
	if (ConnectionId < 0)
	{
		const FInputDeviceId InputDevice = FInputDeviceId::CreateFromInternalId(
			static_cast<int32>(-ConnectionId - 1));
		IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
		TArray<FInputDeviceId> ConnectedDevices;
		DeviceMapper.GetAllConnectedInputDevices(ConnectedDevices);
		if (!ConnectedDevices.Contains(InputDevice))
		{
			return {};
		}
		return JSL4UDescribeUndrivenDevice(DeviceMapper.GetUserForInputDevice(InputDevice), InputDevice);
	}

	return GetControllerInfoForHandle(HandleForConnection(ConnectionId));
}

// super-getter for reading a whole lot of state at once
FJSL4URawSettings UJoyShockLibrary::GetControllerSettingsForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return ReadControllerSettings(jc);
	}
	return {};
}

// what split type of controller is this?
int32 UJoyShockLibrary::GetControllerTypeForHandle(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		switch (jc->controller_type)
		{
		case ControllerType::s_ds4:
			return JS_TYPE_DS4;
		case ControllerType::s_ds:
			return JS_TYPE_DS;
		default:
		case ControllerType::n_switch:
			return jc->switch_legacy_type();
		}
	}
	return 0;
}

// Takes a device handle rather than a connection id, so the assignment code can drive a controller's
// light before the game has a connection id for it.
static void SetLightColorRaw(int32 DeviceHandle, FColor Color)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	JoyShock* jc = GetJoyShockFromHandle(DeviceHandle);
	if (jc != nullptr && (jc->controller_type == ControllerType::s_ds4 || jc->controller_type == ControllerType::s_ds))
	{
		// Store only -- the polling thread is the sole writer of the output report (see pollIndividualLoop).
		jc->modifying_lock.Lock();
		jc->led_r = Color.R;
		jc->led_g = Color.G;
		jc->led_b = Color.B;
		jc->modifying_lock.Unlock();
	}
}

void UJoyShockLibrary::JSL4USetLightColor(int64 ConnectionId, FColor Color)
{
	SetLightColorRaw(HandleForOutput(ConnectionId, TEXT("JSL4USetLightColor")), Color);
}

// Takes a device handle rather than a connection id. Every controller family works the same way:
// store the requested intensities and let that device's polling thread do the writing. What the poll thread
// then does with them differs (Switch 1 re-sends to fight the actuator's decay, Switch 2 retriggers its
// one-shot preset, DualShock 4 / DualSense send once per change), but no HID write happens on this thread.
static void SetRumbleRaw(int32 DeviceId, int32 SmallRumble, int32 BigRumble)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	// Diagnostic (Verbose so per-frame rumble pulses don't spam the log; enable with
	// `Log LogJoyShockLibrary Verbose` to see why a controller isn't vibrating -- wrong id, disconnected...).
	UE_LOG(LogJoyShockLibrary, Verbose, TEXT("SetRumble(device %d, small %d, big %d) -> %s"),
		DeviceId, SmallRumble, BigRumble, jc != nullptr ? *jc->name : TEXT("NO DEVICE WITH THIS ID"));

	if (jc == nullptr)
	{
		return;
	}

	jc->modifying_lock.Lock();
	jc->small_rumble = static_cast<unsigned char>(FMath::Clamp(SmallRumble, 0, 255));
	jc->big_rumble = static_cast<unsigned char>(FMath::Clamp(BigRumble, 0, 255));
	jc->modifying_lock.Unlock();
}

void UJoyShockLibrary::SetForceFeedbackRumble(int32 DeviceId, int32 SmallRumble, int32 BigRumble)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	// Deliberately unlogged: Unreal pushes force feedback values every frame, so logging here would bury
	// everything else in the log at Verbose.
	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc == nullptr)
	{
		return;
	}

	jc->modifying_lock.Lock();
	jc->ff_small_rumble = static_cast<unsigned char>(FMath::Clamp(SmallRumble, 0, 255));
	jc->ff_big_rumble = static_cast<unsigned char>(FMath::Clamp(BigRumble, 0, 255));
	jc->modifying_lock.Unlock();
}

// Buzzes a controller this plugin does not drive, through the only route there is to one: Unreal's own
// force-feedback channels for the device.
//
// Worth being exact about what this can and cannot do, because it is not the same guarantee our own
// controllers get. We write their motors over HID, so nothing else can overwrite the value. Here the engine
// owns the channels and rewrites them every frame for any device that belongs to a player, zeroes included,
// so a value set on an assigned pad survives at most one frame. On an UNASSIGNED pad -- the assignment
// screen, "buzz this one so you know which it is", which is the job this node exists for -- nothing is
// ticking those channels and the value stands.
//
// For rumble during play, on any pad, the answer remains Unreal's force feedback: it is per-player, it
// drives ours and the foreign ones from a single authored effect, and it does not fight anybody.
static void SetUndrivenPadRumble(int64 ConnectionId, float SmallRumble, float BigRumble)
{
	// The negative id is derived from Unreal's device id (see JSL4UDescribeUndrivenDevice), so it inverts
	// exactly rather than needing a lookup.
	const FInputDeviceId InputDevice = FInputDeviceId::CreateFromInternalId(
		static_cast<int32>(-ConnectionId - 1));

	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
	const FPlatformUserId PlatformUser = DeviceMapper.GetUserForInputDevice(InputDevice);

	// The force-feedback interface is still addressed by the legacy controller id, and the mapper is what
	// knows which one this device is.
	int32 ControllerId = INDEX_NONE;
	if (!DeviceMapper.RemapUserAndDeviceToControllerId(PlatformUser, ControllerId, InputDevice))
	{
		return;
	}

	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	IInputInterface* InputInterface = FSlateApplication::Get().GetInputInterface();
	if (InputInterface == nullptr)
	{
		return;
	}

	// Both sides get the same value: the plugin's two channels are "heavy" and "light" motors, which is what
	// LeftLarge/RightLarge and LeftSmall/RightSmall are on a pad that reports one pair per side.
	FForceFeedbackValues Values;
	Values.LeftLarge = Values.RightLarge = FMath::Clamp(BigRumble, 0.0f, 1.0f);
	Values.LeftSmall = Values.RightSmall = FMath::Clamp(SmallRumble, 0.0f, 1.0f);
	InputInterface->SetForceFeedbackChannelValues(ControllerId, Values);
}

void UJoyShockLibrary::JSL4USetControllerRumble(int64 ConnectionId, float SmallRumble, float BigRumble)
{
	// The one output every controller has, so this is the one node that does not simply give up on a pad we
	// do not drive. Reporting Has Rumble true for an Xbox pad and then refusing to rumble it would be the
	// worse half of both answers.
	if (ConnectionId < 0)
	{
		SetUndrivenPadRumble(ConnectionId, SmallRumble, BigRumble);
		return;
	}

	// Normalised 0..1 to match Unreal's force-feedback convention, rather than the raw 0-255 the HID reports
	// carry.
	SetRumbleRaw(HandleForOutput(ConnectionId, TEXT("JSL4USetControllerRumble")),
		FMath::RoundToInt(FMath::Clamp(SmallRumble, 0.0f, 1.0f) * 255.0f),
		FMath::RoundToInt(FMath::Clamp(BigRumble, 0.0f, 1.0f) * 255.0f));
}

FJSL4USwitch2Sensors UJoyShockLibrary::JSL4UGetSwitch2Sensors(int64 ConnectionId)
{
	FJSL4USwitch2Sensors Sensors;

	const int32 DeviceId = HandleForConnection(ConnectionId);
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	const JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc == nullptr || !jc->is_switch2)
	{
		// Zeroes and Is Supported false, which is the honest answer for a controller without these sensors
		// -- and for a connection id that names nothing, which HandleForConnection has already warned about.
		return Sensors;
	}

	// Each field is written by the polling thread as reports arrive, so this reads a set of independently
	// atomic values rather than one coherent snapshot. That is fine for what they are: nothing here is a
	// pair of numbers that has to agree with itself, and the newest reading of each is what a caller wants.
	Sensors.bIsSupported = true;
	Sensors.BatteryVolts = jc->sw2_battery_volts.load();
	Sensors.TemperatureCelsius = jc->sw2_temperature_celsius.load();
	Sensors.Magnetometer = FVector(
		jc->sw2_magnetometer_x.load(), jc->sw2_magnetometer_y.load(), jc->sw2_magnetometer_z.load());
	Sensors.MousePosition = FVector2D(jc->sw2_mouse_x.load(), jc->sw2_mouse_y.load());
	Sensors.MouseTravel = FVector2D(jc->sw2_mouse_travel_x.load(), jc->sw2_mouse_travel_y.load());
	Sensors.MouseRoughness = jc->sw2_mouse_roughness.load();
	Sensors.MouseDistance = jc->sw2_mouse_distance.load();
	return Sensors;
}

FVector2D UJoyShockLibrary::JSL4UConsumeSwitch2MouseDelta(int64 ConnectionId)
{
	const int32 DeviceId = HandleForConnection(ConnectionId);
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc == nullptr || !jc->is_switch2)
	{
		return FVector2D::ZeroVector;
	}

	int64 DeltaX = 0;
	int64 DeltaY = 0;
	jc->consume_mouse_travel(jc->sw2_mouse_consumed_x, jc->sw2_mouse_consumed_y, DeltaX, DeltaY);
	return FVector2D(DeltaX, DeltaY);
}

void UJoyShockLibrary::ConsumeMouseAxisDeltaForHandle(int32 deviceId, float& outDeltaX, float& outDeltaY)
{
	outDeltaX = 0.f;
	outDeltaY = 0.f;

	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc == nullptr || !jc->is_switch2)
	{
		return;
	}

	// Its own baseline, separate from the Blueprint node's: both read the same travel, neither takes it
	// from the other.
	int64 DeltaX = 0;
	int64 DeltaY = 0;
	jc->consume_mouse_travel(jc->sw2_mouse_axis_x, jc->sw2_mouse_axis_y, DeltaX, DeltaY);
	outDeltaX = static_cast<float>(DeltaX);
	outDeltaY = static_cast<float>(DeltaY);
}

void UJoyShockLibrary::JSL4USetHomeLight(int64 ConnectionId, float Brightness)
{
	const int32 DeviceId = HandleForOutput(ConnectionId, TEXT("JSL4USetHomeLight"));
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	// Only a Switch 1 right Joy-Con (left_right 2) or Pro Controller (3) has this light. The Switch 2 Pro
	// speaks a different command protocol and is excluded until its equivalent is mapped.
	if (jc == nullptr || jc->controller_type != ControllerType::n_switch || jc->is_switch2
		|| (jc->left_right != 2 && jc->left_right != 3))
	{
		return;
	}

	// Store only; the polling thread stays the sole writer to the controller, exactly as with rumble and
	// the player indicator, so no blocking HID write happens on the game thread.
	const unsigned char Intensity =
		static_cast<unsigned char>(FMath::RoundToInt(FMath::Clamp(Brightness, 0.f, 1.f) * 15.f));
	jc->wanted_home_light.store(Intensity);
	jc->home_light_owned_by_game.store(true);
	// Published last, and after the intensity, so the polling thread cannot observe a new generation
	// alongside the previous value. Bumping it unconditionally is the point: this call must reach the
	// controller even when the intensity is the one the plugin already believes it set, because the firmware
	// can have turned the light on since.
	jc->home_light_generation.fetch_add(1);
}

void UJoyShockLibrary::JSL4USetPlayerIndicator(int64 ConnectionId, int32 Number)
{
	SetPlayerIndicatorForHandle(HandleForOutput(ConnectionId, TEXT("JSL4USetPlayerIndicator")), Number);
}

void UJoyShockLibrary::SetPlayerIndicatorForHandle(int32 DeviceHandle, int32 Number)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);

	JoyShock* jc = GetJoyShockFromHandle(DeviceHandle);
	if (jc != nullptr && (jc->controller_type == ControllerType::n_switch
		|| jc->controller_type == ControllerType::s_ds))
	{
		// Store the semantic one-based number, never a hardware-specific mask. The polling thread converts
		// it for Switch 1, Switch 2 or DualSense and remains the sole writer to the controller. Apart from
		// avoiding a blocking HID call on the game thread, this prevents the old Switch-1 subcommand from
		// accidentally being sent to a Switch 2 Pro Controller.
		jc->modifying_lock.Lock();
		jc->player_number = Number;
		jc->modifying_lock.Unlock();
	}
}

void UJoyShockLibrary::JSL4UGetSwitchPlayerLedPattern(int32 PlayerNumber,
	bool& bLed1, bool& bLed2, bool& bLed3, bool& bLed4)
{
	const unsigned char Mask = PlayerNumberToSwitchLedMask(PlayerNumber);
	bLed1 = (Mask & 0x01) != 0;
	bLed2 = (Mask & 0x02) != 0;
	bLed3 = (Mask & 0x04) != 0;
	bLed4 = (Mask & 0x08) != 0;
}

