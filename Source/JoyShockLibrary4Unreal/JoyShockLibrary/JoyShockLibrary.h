// JoyShockLibrary.h - Contains declarations of functions
#pragma once
#include "JoyShockLibrary.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogJoyShockLibrary, Verbose, All);

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
}; // JOY_SHOCK_STATE;

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
struct JOYSHOCKLIBRARY4UNREAL_API FJSLAutoCalibration // typedef struct JSL_AUTO_CALIBRATION {
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly)
	float confidence = 0.f;
	
	UPROPERTY(BlueprintReadOnly)
	bool autoCalibrationEnabled = false;
	
	UPROPERTY(BlueprintReadOnly)
	bool isSteady = false;
}; // JSL_AUTO_CALIBRATION;

USTRUCT(BlueprintType)
struct JOYSHOCKLIBRARY4UNREAL_API FJSLSettings // typedef struct JSL_SETTINGS {
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int32 gyroSpace = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 colour = 0;

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

UCLASS()
class UJoyShockLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	static JOYSHOCKLIBRARY4UNREAL_API int32 JslConnectDevices();

	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API int32 JslGetConnectedDeviceHandles(/* int* */ TArray<int32>& OutDeviceHandleArray, int32 InSize);

	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslDisconnectAndDisposeAll();

	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API bool JslStillConnected(int32 deviceId);

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
	// These are the best way to get all the buttons/triggers/sticks, gyro/accelerometer (IMU), orientation/acceleration/gravity (Motion), or touchpad
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API FJoyShockState JslGetSimpleState(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API FIMUState JslGetIMUState(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API FMotionState JslGetMotionState(int32 deviceId);

	// NEW FUNCTION
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	static JOYSHOCKLIBRARY4UNREAL_API FJSL4UMotionState JSL4UGetMotionState(int32 DeviceID);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API FTouchState JslGetTouchState(int32 deviceId, bool previous = false);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API bool JslGetTouchpadDimension(int32 deviceId, int32 &sizeX, int32 &sizeY);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API int32 JslGetButtons(int32 deviceId);

	// get thumbsticks
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetLeftX(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetLeftY(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetRightX(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetRightY(int32 deviceId);

	// get triggers. Switch controllers don't have analogue triggers, but will report 0.0 or 1.0 so they can be used in the same way as others
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetLeftTrigger(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetRightTrigger(int32 deviceId);

	// get gyro
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetGyroX(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetGyroY(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetGyroZ(int32 deviceId);

	// get accumulated average gyro since this function was last called or last flushed values
	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslGetAndFlushAccumulatedGyro(int32 deviceId, float& gyroX, float& gyroY, float& gyroZ);

	// set gyro space. JslGetGyro*, JslGetAndFlushAccumulatedGyro, JslGetIMUState, and the IMU_STATEs reported in the callback functions will use one of 3 transformations:
	// 0 = local space -> no transformation is done on gyro input
	// 1 = world space -> gyro input is transformed based on the calculated gravity direction to account for the player's preferred controller orientation
	// 2 = player space -> a simple combination of local and world space that is as adaptive as world space but is as robust as local space
	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetGyroSpace(int32 deviceId, int32 gyroSpace);

	// get accelerometer
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetAccelX(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetAccelY(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetAccelZ(int32 deviceId);

	// get touchpad
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API int32 JslGetTouchId(int32 deviceId, bool secondTouch = false);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API bool JslGetTouchDown(int32 deviceId, bool secondTouch = false);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetTouchX(int32 deviceId, bool secondTouch = false);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetTouchY(int32 deviceId, bool secondTouch = false);

	// analog parameters have different resolutions depending on device
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetStickStep(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetTriggerStep(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetPollRate(int32 deviceId);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API float JslGetTimeSinceLastUpdate(int32 deviceId);

	// calibration
	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslResetContinuousCalibration(int32 deviceId);

	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslStartContinuousCalibration(int32 deviceId);

	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslPauseContinuousCalibration(int32 deviceId);

	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetAutomaticCalibration(int32 deviceId, bool enabled);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslGetCalibrationOffset(int32 deviceId, float& xOffset, float& yOffset, float& zOffset);

	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetCalibrationOffset(int32 deviceId, float xOffset, float yOffset, float zOffset);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API FJSLAutoCalibration JslGetAutoCalibrationStatus(int32 deviceId);

	// this function will get called for each input event from each controller
	// UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetCallback(void(*callback)(int, FJoyShockState, FJoyShockState, FIMUState, FIMUState, float));

	// this function will get called for each input event, even if touch data didn't update
	// UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetTouchCallback(void(*callback)(int, FTouchState, FTouchState, float));

	// this function will get called for each device when it is newly connected
	// UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetConnectCallback(void(*callback)(int));

	// this function will get called for each device when it is disconnected
	// UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetDisconnectCallback(void(*callback)(int, bool));

	// super-getter for reading a whole lot of state at once
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API FJSLSettings JslGetControllerInfoAndSettings(int32 deviceId);

	// what kind of controller is this?
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API int32 JslGetControllerType(int32 deviceId);

	// is this a left, right, or full controller?
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API int32 JslGetControllerSplitType(int32 deviceId);

	// what colour is the controller (not all controllers support this; those that don't will report white)
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API int32 JslGetControllerColour(int32 deviceId);

	// set controller light colour (not all controllers have a light whose colour can be set, but that just means nothing will be done when this is called -- no harm)
	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetLightColour(int32 deviceId, int32 colour);

	// set controller rumble
	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetRumble(int32 deviceId, int32 smallRumble, int32 bigRumble);

	// set controller player number indicator (not all controllers have a number indicator which can be set, but that just means nothing will be done when this is called -- no harm)
	UFUNCTION(BlueprintCallable, Category = JoyShockLibrary)
	/*extern "C"*/ static JOYSHOCKLIBRARY4UNREAL_API void JslSetPlayerNumber(int32 deviceId, int32 number);
};
