// JoyShockTypes.h - The plugin's Blueprint-facing data: the controller states the polling thread fills
// in, the enums a Blueprint branches on, and the button masks those states are read with.
//
// Split out of JoyShockLibrary.h so that "what can this plugin tell me about a controller" has one place
// to be answered, apart from "what can I ask it to do" (JoyShockBlueprintLibrary.h) and from the hardware
// layer that produces the answers (the JoyShockLibrary/ folder). Nothing here has behaviour: it is the
// vocabulary the other two share, which is why it is also the header that everything else includes.
#pragma once

#include "CoreMinimal.h"

#include "JoyShockTypes.generated.h"

// The plugin's log category. Declared here rather than beside the library it is named after because this
// is the one header every part of the plugin already includes.
DECLARE_LOG_CATEGORY_EXTERN(LogJoyShockLibrary, Verbose, All);

// A physical output capability the plugin drives on a controller. Used by the blocked-function event to
// name which capability another application appears to be interfering with. Values are also bit indices
// into JoyShock::failed_output_functions, so the order is load-bearing.
UENUM(BlueprintType, meta = (DisplayName = "JSL4U Controller Function"))
enum class EJSL4UControllerFunction : uint8
{
	Rumble UMETA(ToolTip = "Vibration output (direct and sustained rumble)."),
	PlayerIndicator UMETA(ToolTip = "Player LEDs / light-bar player identification."),
	HomeLight UMETA(ToolTip = "The Switch HOME notification light."),
	MotionSensor UMETA(ToolTip = "IMU configuration commands (enabling or repairing gyro and accelerometer).")
};

/*#if _MSC_VER // this is defined when compiling with Visual Studio
#define JOY_SHOCK_API __declspec(dllexport) // Visual Studio needs annotating exported functions with this
#else
#define JOY_SHOCK_API // XCode does not need annotating exported functions, so define is empty
#endif*/

#define JS_TYPE_JOYCON_LEFT 1
#define JS_TYPE_JOYCON_RIGHT 2
#define JS_TYPE_PRO_CONTROLLER 3
#define JS_TYPE_DS4 4
#define JS_TYPE_DS 5
#define JS_TYPE_PRO_CONTROLLER_2 6
#define JS_TYPE_JOYCON2_LEFT 7
#define JS_TYPE_JOYCON2_RIGHT 8

#define JS_SPLIT_TYPE_LEFT 1
#define JS_SPLIT_TYPE_RIGHT 2
#define JS_SPLIT_TYPE_FULL 3

#define JSMASK_UP 0x000001
#define JSMASK_DOWN 0x000002
#define JSMASK_LEFT 0x000004
#define JSMASK_RIGHT 0x000008
#define JSMASK_PLUS 0x000010
#define JSMASK_OPTIONS 0x000010
#define JSMASK_MINUS 0x000020
#define JSMASK_SHARE 0x000020
#define JSMASK_LCLICK 0x000040
#define JSMASK_RCLICK 0x000080
#define JSMASK_L 0x000100
#define JSMASK_R 0x000200
#define JSMASK_ZL 0x000400
#define JSMASK_ZR 0x000800
#define JSMASK_S 0x001000
#define JSMASK_E 0x002000
#define JSMASK_W 0x004000
#define JSMASK_N 0x008000
#define JSMASK_HOME 0x010000
#define JSMASK_PS 0x010000
#define JSMASK_CAPTURE 0x020000
#define JSMASK_TOUCHPAD_CLICK 0x020000
#define JSMASK_MIC 0x040000
#define JSMASK_SL 0x080000
#define JSMASK_SR 0x100000
#define JSMASK_FNL 0x200000
#define JSMASK_FNR 0x400000
// Nintendo Switch 2 Pro Controller extras
#define JSMASK_C 0x800000    // "C" (GameChat) button
#define JSMASK_GL 0x1000000  // rear grip button GL
#define JSMASK_GR 0x2000000  // rear grip button GR

#define JSOFFSET_UP 0
#define JSOFFSET_DOWN 1
#define JSOFFSET_LEFT 2
#define JSOFFSET_RIGHT 3
#define JSOFFSET_PLUS 4
#define JSOFFSET_OPTIONS 4
#define JSOFFSET_MINUS 5
#define JSOFFSET_SHARE 5
#define JSOFFSET_LCLICK 6
#define JSOFFSET_RCLICK 7
#define JSOFFSET_L 8
#define JSOFFSET_R 9
#define JSOFFSET_ZL 10
#define JSOFFSET_ZR 11
#define JSOFFSET_S 12
#define JSOFFSET_E 13
#define JSOFFSET_W 14
#define JSOFFSET_N 15
#define JSOFFSET_HOME 16
#define JSOFFSET_PS 16
#define JSOFFSET_CAPTURE 17
#define JSOFFSET_TOUCHPAD_CLICK 17
#define JSOFFSET_MIC 18
#define JSOFFSET_SL 19
#define JSOFFSET_SR 20
#define JSOFFSET_FNL 21
#define JSOFFSET_FNR 22
#define JSOFFSET_C 23
#define JSOFFSET_GL 24
#define JSOFFSET_GR 25

// PS5 Player maps for the DS Player Lightbar
#define DS5_PLAYER_1 4
#define DS5_PLAYER_2 10
#define DS5_PLAYER_3 21
#define DS5_PLAYER_4 27
#define DS5_PLAYER_5 31

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJoyShockState // typedef struct JOY_SHOCK_STATE
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	int32 buttons = 0;
	
	UPROPERTY(BlueprintReadOnly)
	float lTrigger = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float rTrigger = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float stickLX = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float stickLY = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float stickRX = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float stickRY = 0.f;

	void Update(const FJoyShockState& UpdateStateData, const FJoyShockState& PreviousTickState)
	{
		buttons = PreviousTickState.buttons ^ ((PreviousTickState.buttons ^ buttons) | (PreviousTickState.buttons ^ UpdateStateData.buttons));
		lTrigger = UpdateStateData.lTrigger;
		rTrigger = UpdateStateData.rTrigger;
		stickLX = UpdateStateData.stickLX;
		stickLY = UpdateStateData.stickLY;
		stickRX = UpdateStateData.stickRX;
		stickRY = UpdateStateData.stickRY;
	}
}; // JOY_SHOCK_STATE;

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4UJoyShockState // typedef struct JOY_SHOCK_STATE
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	int32 Buttons = 0;
	
	UPROPERTY(BlueprintReadOnly)
	float LeftTrigger = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float RightTrigger = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	FVector2D LeftStick = FVector2D::ZeroVector;
	
	UPROPERTY(BlueprintReadOnly)
	FVector2D RightStick = FVector2D::ZeroVector;
};

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FIMUState // typedef struct IMU_STATE
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	float accelX = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float accelY = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float accelZ = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float gyroX = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float gyroY = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float gyroZ = 0.f;
}; // IMU_STATE;

// New Struct
USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4UIMUState
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	FVector Acceleration = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly)
	FVector Gyro = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FMotionState // typedef struct MOTION_STATE
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	float quatW = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float quatX = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float quatY = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float quatZ = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float rawQuatW = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float rawQuatX = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float rawQuatY = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float rawQuatZ = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float accelX = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float accelY = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float accelZ = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float gravX = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float gravY = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	float gravZ = 0.f;
}; // MOTION_STATE;

// New Struct
USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4UMotionState
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	FQuat Orientation = FQuat::Identity;

	UPROPERTY(BlueprintReadOnly)
	FVector Acceleration = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly)
	FVector Gravity = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FTouchState // typedef struct TOUCH_STATE {
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	int32 t0Id = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 t1Id = 0;

	UPROPERTY(BlueprintReadOnly)
	bool t0Down = false;

	UPROPERTY(BlueprintReadOnly)
	bool t1Down = false;

	UPROPERTY(BlueprintReadOnly)
	float t0X = 0.f;

	UPROPERTY(BlueprintReadOnly)
	float t0Y = 0.f;

	UPROPERTY(BlueprintReadOnly)
	float t1X = 0.f;

	UPROPERTY(BlueprintReadOnly)
	float t1Y = 0.f;
}; // TOUCH_STATE;

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4USingleTouchState
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	int32 TouchID = 0;

	UPROPERTY(BlueprintReadOnly)
	bool bIsDown = false;
	
	UPROPERTY(BlueprintReadOnly)
	FVector2D Location = FVector2D::ZeroVector;
};

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4UTouchState
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	FJSL4USingleTouchState PrimaryTouch = {};
	
	UPROPERTY(BlueprintReadOnly)
	FJSL4USingleTouchState SecondaryTouch = {};
};

/**
 * Which model a controller is. Ordered by family rather than by history, so a Switch node reads the way a
 * controller-select screen does; the numbers behind the names are not meaningful and nothing outside this
 * enum depends on them.
 */
UENUM(BlueprintType)
enum class EJSL4UControllerType : uint8
{
	Undefined = 0 UMETA(DisplayName = "Undefined"),
	// Any controller Unreal accepts that this plugin does not drive. On Windows that means XInput, which is
	// Xbox pads and the very many third-party controllers presenting as one. Named after the API rather
	// than the brand because the API is the part that is actually true: a controller arriving this way is
	// certainly XInput, and only probably an Xbox. (Strictly the value means "not ours", so a device from
	// some other input plugin would land here too -- put a friendlier label on it in your UI if you want
	// one, rather than making the enum claim more than it knows.) Only Wait For Any Controller Changes ever
	// reports it; the JSL4U-specific nodes never do. Distinct from Undefined, which means a controller this
	// plugin IS driving but could not identify.
	XInputController = 1 UMETA(DisplayName = "XInput Controller"),
	DualShock4 = 2 UMETA(DisplayName = "DualShock 4"),
	DualSense = 3 UMETA(DisplayName = "DualSense"),
	ProController = 4 UMETA(DisplayName = "Pro Controller"),
	ProController2 = 5 UMETA(DisplayName = "Pro Controller 2"),
	JoyConLeft = 6 UMETA(DisplayName = "Joy-Con (L)"),
	JoyConRight = 7 UMETA(DisplayName = "Joy-Con (R)"),
	// Bluetooth only. A Joy-Con 2 never appears over USB -- the console charges it through the rails, and a
	// cable to a PC gives nothing -- so it arrives from the radio scan, and only once it has been paired
	// (hold SYNC) or woken with a button press. Joins into a pair exactly as a Switch 1 Joy-Con does.
	JoyCon2Left = 8 UMETA(DisplayName = "Joy-Con 2 (L)"),
	// Bluetooth only -- see Joy-Con 2 (L) above.
	JoyCon2Right = 9 UMETA(DisplayName = "Joy-Con 2 (R)")
};

/**
 * A controller's charge, in the coarse steps the hardware actually reports.
 *
 * Deliberately not a percentage: a Switch controller reports five states in its input report, so a
 * percentage would be invented precision. Sony controllers report finer steps, which are bucketed into
 * the same five so a game can treat every controller the same way.
 *
 * Unknown is not "flat" -- it means this controller does not report charge (the Switch 2 Pro Controller,
 * currently), or none has arrived yet. Compare against it explicitly before treating a value as low.
 */
UENUM(BlueprintType)
enum class EJSL4UBatteryLevel : uint8
{
	Unknown = 0 UMETA(ToolTip = "This controller does not report its charge, or no report has arrived yet."),
	Empty = 1 UMETA(ToolTip = "Effectively flat; shutdown is imminent."),
	Critical = 2 UMETA(ToolTip = "Very low."),
	Low = 3 UMETA(ToolTip = "Low -- the level Wait For Battery Changes warns at by default."),
	Medium = 4 UMETA(ToolTip = "Comfortably charged."),
	Full = 5 UMETA(ToolTip = "Full or nearly full.")
};

UENUM(BlueprintType)
enum class EJSL4UJoyConGripMode : uint8
{
	NotApplicable = 0 UMETA(DisplayName = "Not a Joy-Con"),
	Vertical = 1 UMETA(DisplayName = "Vertical"),
	Horizontal = 2 UMETA(DisplayName = "Horizontal")
};

UENUM(BlueprintType)
enum class EJSL4UGyroSpace : uint8
{
	LocalSpace = 0 UMETA(DisplayName = "Local Space"), // no transformation is done on gyro input
	WorldSpace = 1 UMETA(DisplayName = "World Space"), // gyro input is transformed based on the calculated gravity direction to account for the player's preferred controller orientation
	PlayerSpace = 2 UMETA(DisplayName = "Player Space"), // a simple combination of local and world space that is as adaptive as world space but is as robust as local space
};

/**
 * How a controller's gyro drift offset is arrived at.
 *
 * A gyroscope reports a small non-zero rotation even when perfectly still, and that offset drifts with
 * temperature and age. Calibrating means measuring it while the controller is still and subtracting it.
 */
UENUM(BlueprintType)
enum class EJSL4UGyroCalibrationMode : uint8
{
	// The controller works out for itself when it is being held still and calibrates continuously, with no
	// action from the player. This is what most games want -- set it once and never think about it again.
	Automatic UMETA(DisplayName = "Automatic"),
	// Calibration only happens between JSL4UStartManualGyroCalibration and JSL4UStopManualGyroCalibration. Use this to
	// drive an explicit "put the controller down and hold still" step in an options screen.
	Manual UMETA(DisplayName = "Manual"),
};

/** A controller's current gyro calibration state, for showing progress in a calibration screen. */
USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4UGyroCalibrationStatus
{
	GENERATED_BODY()

	// How sure the automatic calibration is of its current offset, 0 to 1. Meaningless in Manual mode.
	// Note that this only ever climbs: it is a high-water mark, not a live reading, so 100% means "this
	// controller was measured well at some point", not "it is being measured well right now".
	UPROPERTY(BlueprintReadOnly, meta = (DisplayName = "Auto Calibration Confidence"))
	float Confidence = 0.f;

	/**
	 * Whether the automatic calibrator is currently accepting this controller as still enough to sample
	 * from. It is the calibrator's own gate, not a general "is the controller moving" reading, and the
	 * difference bites: the stillness test compares against a noise floor that only ever ratchets DOWN and
	 * stops adapting once Confidence reaches 1, with barely any headroom. A controller lying untouched on
	 * a desk can therefore read false indefinitely -- it merely rests slightly noisier than the quietest
	 * moment ever measured -- while Confidence stays pinned at 100%, since Confidence never decays either.
	 * Reset Gyro Calibration clears both and lets it settle again.
	 *
	 * So this is a useful cue inside a calibration flow ("keep holding" versus "sampling"), and the wrong
	 * thing to label "controller is still" on a HUD. For that, compare the length of the gyro vector from
	 * Get IMU State against a small threshold -- that is a genuine reading of the present moment.
	 */
	UPROPERTY(BlueprintReadOnly, meta = (DisplayName = "Auto Calibration Sampling Now"))
	bool bIsSteady = false;

	// The mode this controller is in (see JSL4USetGyroCalibrationMode).
	UPROPERTY(BlueprintReadOnly)
	EJSL4UGyroCalibrationMode Mode = EJSL4UGyroCalibrationMode::Manual;

	// Whether a manual calibration is currently gathering samples, i.e. JSL4UStartManualGyroCalibration was
	// called and JSL4UStopManualGyroCalibration has not been. The automatic counterpart is the flag above; the
	// two never run at once, since automatic sampling is suspended while a manual calibration is active.
	UPROPERTY(BlueprintReadOnly, meta = (DisplayName = "Manual Calibration Running"))
	bool bIsCalibrating = false;
};

// Everything the plugin knows about a controller: its identity, its type, the player slot it feeds and
// its live JSL settings. Returned for a single device by JSL4UGetControllerInfo and for every
// connected device by JSL4UGetConnectedControllers.
USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4UControllerInfo // typedef struct JSL_SETTINGS {
{
	GENERATED_BODY()

	/**
	 * True when this plugin is the one driving the controller, and so when everything below the identity
	 * block means anything.
	 *
	 * Every JSL4U node that returns controllers returns only ours, so this is true throughout -- except
	 * from Get All Connected Controllers and Wait For Any Controller Changes, which report every controller
	 * Unreal accepts so that "has a player joined" can be answered once for all of them. A controller that
	 * is not ours fills in only what Unreal knows about any device -- Connection Id, Player Index, Input
	 * Device Id, Hardware Device Identifier -- and reports the capabilities below honestly: false for the
	 * hardware it does not have, true for the rumble every pad has, which reaches it through Unreal's own
	 * force feedback.
	 *
	 * This is not a test you need before calling a node. Every node takes a Connection Id and answers for
	 * any controller, serving what it can and doing nothing (with one log warning) for what it cannot. Ask
	 * the capability flag for the thing you are about to use -- Has Motion Sensors before reading a gyro --
	 * rather than asking whose controller it is.
	 */
	UPROPERTY(BlueprintReadOnly)
	bool bIsJoyShockController = false;

	// The controller's identity, and the only address the plugin's nodes take. Unique for this connection:
	// never reused when a controller disconnects and another takes its place. Use it as the key of
	// persistent assignment maps.
	//
	// Positive (from 1) for a controller this plugin drives. A controller it does not drive gets a negative
	// one instead, so the two sources can key the same map without ever colliding, and so the sign alone
	// says which kind of controller a node was handed.
	//
	// The two halves differ in one way worth knowing, now that this is the only address there is: a
	// positive id is unique for the whole run, while a negative one is derived from Unreal's device id and
	// comes back into circulation once that pad leaves. Both are safe to hold -- a stale id of either kind
	// resolves to nothing while the pad is gone -- but only the positive kind is safe to hold ACROSS a
	// reconnect. Neither survives a session on disk.
	//
	// Zero is never issued: it is what this field reads before a controller has an identity at all, and no
	// node hands out a controller in that state (see JSL4UGetConnectedControllers). A 0 arriving in a
	// Blueprint is an uninitialised struct, not a controller.
	UPROPERTY(BlueprintReadOnly)
	int64 ConnectionId = 0;

	// Unreal's native identities for this connection. These are exposed as integers because
	// FInputDeviceId / FPlatformUserId are not Blueprint structs.
	UPROPERTY(BlueprintReadOnly)
	int32 InputDeviceId = -1;

	UPROPERTY(BlueprintReadOnly)
	int32 PlatformUserId = -1;

	// Stable identifier registered with UInputDeviceSubsystem (for example DualSense or JoyConLeft).
	UPROPERTY(BlueprintReadOnly)
	FName HardwareDeviceIdentifier = NAME_None;

	UPROPERTY(BlueprintReadOnly)
	EJSL4UControllerType ControllerType = EJSL4UControllerType::Undefined;

	// The player slot (0, 1, 2, ...) this controller's input is delivered to, or -1 if it isn't connected.
	// Both halves of a joined Joy-Con pair share the same PlayerIndex.
	UPROPERTY(BlueprintReadOnly)
	int32 PlayerIndex = -1;

	// If this controller is joined with another, the connection id of its partner; otherwise 0.
	UPROPERTY(BlueprintReadOnly)
	int64 JoinedToConnectionId = 0;

	// Standalone Joy-Cons default to Horizontal. Joining a left and right half makes both Vertical.
	// Non-Joy-Con controllers report NotApplicable.
	UPROPERTY(BlueprintReadOnly)
	EJSL4UJoyConGripMode JoyConGripMode = EJSL4UJoyConGripMode::NotApplicable;

	// The number shown on the controller's player indicator (the Switch or DualSense player LEDs), as set
	// by JSL4USetPlayerIndicator. DualShock 4 has an RGB light bar but no numeric player indicator. This is
	// a display value, not an identity -- to identify a controller use ConnectionId, and to know which player it
	// feeds use PlayerIndex.
	UPROPERTY(BlueprintReadOnly)
	int32 PlayerLedNumber = 0;

	// Charge level, in the coarse steps the hardware reports. Unknown when the controller does not report
	// it. Sony values still need verification against real hardware -- see the parsing in InputHelpers.cpp.
	UPROPERTY(BlueprintReadOnly)
	EJSL4UBatteryLevel BatteryLevel = EJSL4UBatteryLevel::Unknown;

	// Whether the controller is drawing power (cable, dock, or a Joy-Con seated in a charging grip).
	UPROPERTY(BlueprintReadOnly)
	bool bIsCharging = false;

	// Charge from 0 to 100, or -1 when the controller does not report one finely enough to quote. Only the
	// PlayStation controllers fill this. Prefer Battery Level for anything that decides something: it is
	// the only reading every family can give, so a game written against it behaves the same on a Joy-Con
	// as on a DualSense. This is for display -- a percentage on a settings screen, a battery bar.
	UPROPERTY(BlueprintReadOnly)
	int32 BatteryPercent = -1;

	UPROPERTY(BlueprintReadOnly)
	FColor Color = FColor::Black;

	// Output/input capabilities used by diagnostic UI. Prefer these to switches on ControllerType so new
	// controller variants can inherit the right UI without every Blueprint being updated.
	UPROPERTY(BlueprintReadOnly)
	bool bHasRgbLight = false;

	UPROPERTY(BlueprintReadOnly)
	bool bHasPlayerIndicator = false;

	UPROPERTY(BlueprintReadOnly)
	bool bHasMotionSensors = false;

	UPROPERTY(BlueprintReadOnly)
	bool bHasTouchpad = false;

	UPROPERTY(BlueprintReadOnly)
	bool bHasRumble = false;

	// The space this controller's gyro is currently reported in, as set by JSL4USetGyroSpace.
	UPROPERTY(BlueprintReadOnly)
	EJSL4UGyroSpace GyroSpace = EJSL4UGyroSpace::LocalSpace;

	// (JSL's raw SplitType used to sit here. It said nothing ControllerType does not already say --
	// left half, right half or whole controller -- as an unlabelled integer, so it was dropped rather
	// than promoted to an enum that would have duplicated EJSL4UControllerType.)

	UPROPERTY(BlueprintReadOnly)
	bool bIsCalibrating = false;

	UPROPERTY(BlueprintReadOnly)
	bool bAutoCalibrationEnabled = false;

	UPROPERTY(BlueprintReadOnly)
	bool bIsConnected = false;
};

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4URawSettings // typedef struct JSL_SETTINGS {
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int32 gyroSpace = 0;

	// UPROPERTY(BlueprintReadOnly)
	uint32 colour = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 playerNumber = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 controllerType = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 splitType = 0;

	UPROPERTY(BlueprintReadOnly)
	bool isCalibrating = false;

	UPROPERTY(BlueprintReadOnly)
	bool autoCalibrationEnabled = false;

	UPROPERTY(BlueprintReadOnly)
	bool isConnected = false;
}; // JSL_SETTINGS;
// The sensors that only the Switch 2 controllers carry. These are not motion -- the accelerometer and
// gyroscope are read through the usual IMU nodes, which every driven controller answers -- and nothing else
// in the plugin's families reports them, which is why they are gathered here instead of being added to the
// IMU state that a DualSense would then have to answer with zeroes.
//
// Any other controller returns this struct with Is Supported false and every field zero, so a Blueprint can
// read it unconditionally and branch on the flag.
USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSL4USwitch2Sensors
{
	GENERATED_BODY()

	// False for every controller that is not a Switch 2, and for a Switch 2 that has not reported yet.
	UPROPERTY(BlueprintReadOnly)
	bool bIsSupported = false;

	// The battery cell's terminal voltage. Roughly 4.2 full and 3.0 empty, but it sags under load and
	// recovers at rest -- Battery Level on the controller info is the interpolated version of this, and the
	// better thing to drive a UI from. This is here for a readout that wants the measurement itself.
	UPROPERTY(BlueprintReadOnly)
	float BatteryVolts = 0.f;

	// Treat as unconfirmed. A pair on the same desk at the same room temperature read 25 and 36, and the 25
	// is exactly the constant this is offset from -- so that half's raw field was zero, and the offset is a
	// guess that has never been checked against a thermometer. Fine as a curiosity, not as a measurement.
	UPROPERTY(BlueprintReadOnly)
	float TemperatureCelsius = 0.f;

	// Raw magnetometer counts. Uncalibrated: a magnetometer reads the local field, which in a room means
	// the building's steel and whatever is on the desk as much as it means north. Useful as an extra axis
	// for a fusion filter that knows how to calibrate it, not as a compass on its own.
	UPROPERTY(BlueprintReadOnly)
	FVector Magnetometer = FVector::ZeroVector;

	// The optical sensor in the controller's underside, the one the console uses for mouse mode.
	//
	// An absolute position, not a per-frame delta: it runs from 0 at the top-left to 65535 and then wraps
	// back to 0, on both axes, and it is always a whole number despite the float. Which means a delta is a
	// subtraction that has to survive that wrap -- plain "now minus last" reports a jump of ±65536 every
	// time an axis rolls over. Measured on hardware at roughly 12000 units per length of a Pro Controller,
	// with no acceleration curve of the sensor's own.
	UPROPERTY(BlueprintReadOnly)
	FVector2D MousePosition = FVector2D::ZeroVector;

	// The same movement with the wrap already taken out: it only grows, so the difference between two
	// reads is the distance travelled between them, with no special case at the roll-over. Prefer this to
	// Mouse Position for anything that measures movement rather than reporting the sensor's own number.
	// Reset when the controller reconnects, like everything else about a connection.
	UPROPERTY(BlueprintReadOnly)
	FVector2D MouseTravel = FVector2D::ZeroVector;

	// The sensor's read of the surface under it. Distance rises as the controller is lifted, so these are
	// what tell you whether it is actually being used as a mouse rather than held in the air.
	//
	// Measured on a Joy-Con 2: roughness is about 4600 in the air, 4380 on a mousepad, 2500 on a bare desk
	// and 2000 on cloth. Distance sits near 3000 in the air, starts falling about 5 mm from a surface, and
	// rests at 140-150 against one -- a threshold on it is a fair way to decide the controller is being
	// used as a mouse at all.
	UPROPERTY(BlueprintReadOnly)
	int32 MouseRoughness = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 MouseDistance = 0;
};
