﻿#pragma once


#include "CoreTypes.h"
#include "GenericPlatform/IInputInterface.h"
#include "IInputDevice.h"
#include "JoyShockLibrary4UnrealSettings.h"
#include "GenericPlatform/GenericApplicationMessageHandler.h"
#include "JoyShockLibrary4Unreal/JoyShockLibrary/JoyShockLibrary.h"

// Max number of controllers.
#define MAX_NUM_JOYSHOCK_CONTROLLERS 8

// Max number of controller buttons.  Must be < 256
#define MAX_NUM_CONTROLLER_BUTTONS 27

struct FTouchState;
struct FJoyShockState;
struct FIMUState;
enum class FForceFeedbackChannelType;

// Interface class for JoyShock devices (DualShock 4, DualSense, Switch Pro Controller, JoyCons)
class FJoyShockInterface : public IInputDevice
{
public:
	static TSharedRef<FJoyShockInterface> Create(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler);
	
	/**
	 * Poll for controller state and send events if needed
	 */
	virtual void SendControllerEvents() override;

	virtual void SetMessageHandler(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override;

	void SetNeedsControllerStateUpdate() { bNeedsControllerStateUpdate = true; }

	/**
	* Sets the strength/speed of the given channel for the given controller id.
	* NOTE: If the channel is not supported, the call will silently fail
	*
	* @param ControllerId the id of the controller whose value is to be set
	* @param ChannelType the type of channel whose value should be set
	* @param Value strength or speed of feedback, 0.0f to 1.0f. 0.0f will disable
	*/
	virtual void SetChannelValue(int32 ControllerId, const FForceFeedbackChannelType ChannelType, const float Value) override;

	/**
	* Sets the strength/speed of all the channels for the given controller id.
	* NOTE: Unsupported channels are silently ignored
	*
	* @param ControllerId the id of the controller whose value is to be set
	* @param Values strength or speed of feedback for all channels
	*/
	virtual void SetChannelValues(int32 ControllerId, const FForceFeedbackValues& Values) override;

	virtual bool IsGamepadAttached() const override { return bIsGamepadAttached; }
	virtual void Tick(float DeltaTime) override {};
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override { return false; }

private:
	FJoyShockInterface(const TSharedRef<FGenericApplicationMessageHandler>& MessageHandler);

	void InitializeAdditionalKeys();

	struct FControllerState
	{
		// If the controller is currently connected
		bool bIsConnected = false;

		FPlatformUserId PlatformUser = PLATFORMUSERID_NONE;
		FInputDeviceId InputDevice = INPUTDEVICEID_NONE;
		
		FJoyShockState SimpleState = {};
		FJoyShockState PreviousSimpleState = {};
		FIMUState IMUState = {};
		FIMUState PreviousIMUState = {};
		FTouchState TouchState = {};
		FTouchState PreviousTouchState = {};
		
		/* TODO: Rumble
		// Current force feedback values
		FForceFeedbackValues ForceFeedback;

		float LastLargeValue;
		float LastSmallValue;*/
	};

	// If we've been notified by the system that the controller state may have changed
	bool bNeedsControllerStateUpdate;

	bool bIsGamepadAttached;
	const FName JoyShockControllerName = FName("JoyShock");

	void GetPlatformUserAndDevice(int32 InControllerId, EInputDeviceConnectionState InDeviceState, FPlatformUserId& OutPlatformUserId, FInputDeviceId& OutDeviceId);

	static FString GetDeviceName(int32 InControllerId);

	// Controller states
	TMap<int32, FControllerState> ControllerStateByDeviceHandle = {};
	
	TArray<int32> DeviceHandles = {};

	// Delay before sending a repeat message after a button was first pressed
	float InitialButtonRepeatDelay;

	// Delay before sending a repeat message after a button has been pressed for a while
	float ButtonRepeatDelay;

	FGamepadKeyNames::Type Buttons[MAX_NUM_CONTROLLER_BUTTONS];

	TSharedRef<FGenericApplicationMessageHandler> MessageHandler;

	void OnControllerAnalog(const FPlatformUserId& InPlatformUser, const FInputDeviceId& InInputDevice,
						const FName& GamePadKey, float NewAxisValueNormalized, float OldAxisValueNormalized,
						float DeadZone) const;
	
	void ProcessButtons(int32 CurrentButtons, int32 PreviousButtons, FPlatformUserId PlatformUser, FInputDeviceId InputDevice);
	void ProcessAnalogInputs(const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState, FPlatformUserId PlatformUser, FInputDeviceId InputDevice);
	// Callbacks
	void OnPollCallback(int32 DeviceHandle, const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState, const FIMUState& IMUState, const FIMUState& PreviousIMUState, float DeltaTime);

	void ProcessSingleTouchState(bool bTouchDown, int32 TouchID, const FVector2D& TouchLocation, bool bPreviousTouchDown, int32 PreviousTouchID, const FVector2D& PreviousTouchLocation, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const;
	void ProcessTouchState(const FTouchState& InTouchState, const FTouchState& InPreviousTouchState, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const;
	void OnTouchCallback(int32 DeviceHandle, const FTouchState& TouchState, const FTouchState& PreviousTouchState, float DeltaTime);
	// void ProcessIMUState(const FIMUState& InIMUState, const FIMUState& InPreviousIMUState, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const;

	void OnConnectCallback(int32 InDeviceHandle);

	void OnDisconnectCallback(int32 InDeviceHandle, bool bInHasTimedOut);

	static constexpr int32 XInputGamepadLeftThumbDeadzone = 7849; // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
	static constexpr int32 XInputGamepadRightThumbDeadzone = 8689; // XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
	static constexpr int32 XInputGamepadTriggerThreshold = 30; // XINPUT_GAMEPAD_TRIGGER_THRESHOLD

	static constexpr float XInputLeftStickDeadzone = XInputGamepadLeftThumbDeadzone / 32768.0f;
	static constexpr float XInputRightStickDeadzone = XInputGamepadRightThumbDeadzone / 32768.0f;
	static constexpr float XInputTriggerDeadzone = XInputGamepadTriggerThreshold / 255.0f;

	// Additional input names
	const FGamepadKeyNames::Type HomeButtonKeyName = "HomeButton";
	const FKey HomeButtonKey = "HomeButton"; // "Home" is already taken by the keyboard key

	const FGamepadKeyNames::Type PSButtonKeyName = "PS";
	const FKey PSButtonKey = "PS";
	
	const FGamepadKeyNames::Type CaptureButtonKeyName = "Capture";
	const FKey CaptureButtonKey = "Capture";
	
	const FGamepadKeyNames::Type TouchPadClickKeyName = "TouchPadClick";
	const FKey TouchPadClickKey = "TouchPadClick";
	
	const FGamepadKeyNames::Type MicButtonKeyName = "Mic";
	const FKey MicButtonKey = "Mic";
	
	const FGamepadKeyNames::Type SideLeftButtonKeyName = "SideLeft";
	const FKey SideLeftButtonKey = "SideLeft";
	
	const FGamepadKeyNames::Type SideRightButtonKeyName = "SideRight";
	const FKey SideRightButtonKey = "SideRight";
	
	const FGamepadKeyNames::Type FunctionLeftButtonKeyName = "FunctionLeft";
	const FKey FunctionLeftButtonKey = "FunctionLeft";
	
	const FGamepadKeyNames::Type FunctionRightButtonKeyName = "FunctionRight";
	const FKey FunctionRightButtonKey = "FunctionRight";
	
	
	const TArray<TTuple<int32, FName>> JoyShockMaskMappings = {
		/* [0] = */{JSMASK_UP, FGamepadKeyNames::DPadUp},
		/* [1] = */{JSMASK_DOWN, FGamepadKeyNames::DPadDown},
		/* [2] = */{JSMASK_LEFT, FGamepadKeyNames::DPadLeft},
		/* [3] = */{JSMASK_RIGHT, FGamepadKeyNames::DPadRight},
		/* [4] = */{JSMASK_PLUS, FGamepadKeyNames::SpecialRight},
		/* [5] = */{JSMASK_OPTIONS, FGamepadKeyNames::SpecialRight},
		/* [6] = */{JSMASK_MINUS, FGamepadKeyNames::SpecialLeft},
		/* [7] = */{JSMASK_SHARE, FGamepadKeyNames::SpecialLeft},
		/* [8] = */{JSMASK_LCLICK, FGamepadKeyNames::LeftThumb},
		/* [9] = */{JSMASK_RCLICK, FGamepadKeyNames::RightThumb},
		/* [10] = */{JSMASK_L, FGamepadKeyNames::LeftShoulder},
		/* [11] = */{JSMASK_R, FGamepadKeyNames::RightShoulder},
		/* [12] = */{JSMASK_ZL, FGamepadKeyNames::LeftTriggerThreshold},
		/* [13] = */{JSMASK_ZR, FGamepadKeyNames::RightTriggerThreshold},
		/* [14] = */{JSMASK_S, FGamepadKeyNames::FaceButtonBottom},
		/* [15] = */{JSMASK_E, FGamepadKeyNames::FaceButtonRight},
		/* [16] = */{JSMASK_W, FGamepadKeyNames::FaceButtonLeft},
		/* [17] = */{JSMASK_N, FGamepadKeyNames::FaceButtonTop},
		
		/* [18] = */{JSMASK_HOME, HomeButtonKeyName},
		/* [19] = */{JSMASK_PS, PSButtonKeyName},
		/* [20] = */{JSMASK_CAPTURE, CaptureButtonKeyName},
		/* [21] = */{JSMASK_TOUCHPAD_CLICK, TouchPadClickKeyName},
		/* [22] = */{JSMASK_MIC, MicButtonKeyName},
		/* [23] = */{JSMASK_SL, SideLeftButtonKeyName},
		/* [24] = */{JSMASK_SR, SideRightButtonKeyName},
		/* [25] = */{JSMASK_FNL, FunctionLeftButtonKeyName},
		/* [26] = */{JSMASK_FNR, FunctionRightButtonKeyName}
	};
	
	TArray<FName> ButtonMappingByMaskIndex = {
		/*[0] = */ FGamepadKeyNames::DPadUp,
		/*[1] = */ FGamepadKeyNames::DPadDown,
		/*[2] = */ FGamepadKeyNames::DPadLeft,
		/*[3] = */ FGamepadKeyNames::DPadRight,
		/*[4] = */ FGamepadKeyNames::SpecialRight,
		/*[5] = */ FGamepadKeyNames::SpecialRight,
		/*[6] = */ FGamepadKeyNames::SpecialLeft,
		/*[7] = */ FGamepadKeyNames::SpecialLeft,
		/*[8] = */ FGamepadKeyNames::LeftThumb,
		/*[9] = */ FGamepadKeyNames::RightThumb,
		/*[10] = */ FGamepadKeyNames::LeftShoulder,
		/*[11] = */ FGamepadKeyNames::RightShoulder,
		/*[12] = */ FGamepadKeyNames::LeftTriggerThreshold,
		/*[13] = */ FGamepadKeyNames::RightTriggerThreshold,
		/*[14] = */ FGamepadKeyNames::FaceButtonBottom,
		/*[15] = */ FGamepadKeyNames::FaceButtonRight,
		/*[16] = */ FGamepadKeyNames::FaceButtonLeft,
		/*[17] = */ FGamepadKeyNames::FaceButtonTop,
		
		/*[18] = */ HomeButtonKeyName,
		/*[19] = */ PSButtonKeyName,
		
		/*[20] = */ CaptureButtonKeyName,
		
		/*[21] = */ TouchPadClickKeyName,
		/*[22] = */ MicButtonKeyName,
		/*[23] = */ SideLeftButtonKeyName,
		/*[24] = */ SideRightButtonKeyName,
		/*[25] = */ FunctionLeftButtonKeyName,
		/*[26] = */ FunctionRightButtonKeyName
	};

	TStaticArray<double, MAX_NUM_CONTROLLER_BUTTONS> NextRepeatTimes{InPlace, 0.0};

	UJoyShockLibrary4UnrealSettings* CachedSettings;

	FCriticalSection SimpleStateLock;
	FCriticalSection TouchStateLock;
}; 