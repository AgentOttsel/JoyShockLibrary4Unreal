// Copyright Epic Games, Inc. All Rights Reserved.

#include "JoyShockInterface.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
// #include "Windows/WindowsApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "JoyShockLibrary4Unreal/JoyShockLibrary/JoyShockLibrary.h"
#include "Misc/ConfigCacheIni.h"
#include <functional>

#include "JoyShockLibrary4Unreal.h"
#include "JoyShockLibrary4UnrealSettings.h"

#define LOCTEXT_NAMESPACE "JoyShockLibrary"

static int32 JoyShockEnableXInputDeadzones = 0;
FAutoConsoleVariableRef CVarJoyShockLeftStickMessageDeadzone
(
	TEXT("JoyShock.JoyShockEnableXInputDeadzones"),
	JoyShockEnableXInputDeadzones,
	TEXT("Enable the same deadzone values for triggers and analog sticks used by Unreal's XInput interface. If disabled, no deadzones will be used.\n")
	TEXT("0: Not Enabled, 1: Enabled"),
	ECVF_Default
);

TSharedRef<FJoyShockInterface> FJoyShockInterface::Create(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
{
	return MakeShareable(new FJoyShockInterface(InMessageHandler));
}


FJoyShockInterface::FJoyShockInterface(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
	: MessageHandler(InMessageHandler)
{
	CachedSettings = GetMutableDefault<UJoyShockLibrary4UnrealSettings>();

#if WITH_EDITOR
	CachedSettings->GetOnSettingsChanged().AddLambda([this]
	{
		CachedSettings = GetMutableDefault<UJoyShockLibrary4UnrealSettings>();
	});
#endif

	InitializeAdditionalKeys();

	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	// TODO: Bind these without using lambda if possible
	JoyShockLockedBindLambda(JSL4UModule, GetOnConnected(), [this](int32 DeviceHandle)
	{
		if (this)
			this->OnConnectCallback(DeviceHandle);
	});

	JoyShockLockedBindLambda(JSL4UModule, GetOnDisconnected(), [this](int32 DeviceHandle, bool bHasTimedOut)
	{
		if (this)
			this->OnDisconnectCallback(DeviceHandle, bHasTimedOut);
	});

	JoyShockLockedBindLambda(JSL4UModule, GetOnPoll(), [this](int32 DeviceHandle, const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState, const FIMUState& IMUState, const FIMUState& PreviousIMUState, float DeltaTime)
	{
		if (this)
			this->OnPollCallback(DeviceHandle, SimpleState, PreviousSimpleState, IMUState, PreviousIMUState, DeltaTime);
	});

	JoyShockLockedBindLambda(JSL4UModule, GetOnPollTouch(), [this](int32 DeviceHandle, const FTouchState& TouchState, const FTouchState& PreviousTouchState, float DeltaTime)
	{
		if (this)
			this->OnTouchCallback(DeviceHandle, TouchState, PreviousTouchState, DeltaTime);
	});
	
	bIsGamepadAttached = false;
	bNeedsControllerStateUpdate = true;
	InitialButtonRepeatDelay = 0.2f;
	ButtonRepeatDelay = 0.1f;

	GConfig->GetFloat(TEXT("/Script/Engine.InputSettings"), TEXT("InitialButtonRepeatDelay"), InitialButtonRepeatDelay, GInputIni);
	GConfig->GetFloat(TEXT("/Script/Engine.InputSettings"), TEXT("ButtonRepeatDelay"), ButtonRepeatDelay, GInputIni);
}

void FJoyShockInterface::InitializeAdditionalKeys()
{
	EKeys::AddMenuCategoryDisplayInfo(JoyShockControllerName, LOCTEXT("JoyShockSubCategory", "JoyShock"), TEXT("GraphEditor.PadEvent_16x"));
	
	EKeys::AddKey(FKeyDetails(HomeButtonKey, LOCTEXT("JoyShock_Home_Button", "JoyShock Home Button"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// DualShock/DualSense
	EKeys::AddKey(FKeyDetails(PSButtonKey, LOCTEXT("JoyShock_PS_Button", "JoyShock PS Button"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// Switch
	EKeys::AddKey(FKeyDetails(CaptureButtonKey, LOCTEXT("JoyShock_Capture", "JoyShock Capture"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// DualShock/DualSense
	EKeys::AddKey(FKeyDetails(TouchPadClickKey, LOCTEXT("JoyShock_TouchPad_Click", "JoyShock TouchPad Click"), FKeyDetails::GamepadKey, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(MicButtonKey, LOCTEXT("JoyShock_Mic_Button", "JoyShock Mic Button"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// Single Joy-con
	EKeys::AddKey(FKeyDetails(SideLeftButtonKey, LOCTEXT("JoyShock_Side_Left", "JoyShock Side Left"), FKeyDetails::GamepadKey, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(SideRightButtonKey, LOCTEXT("JoyShock_Side_Right", "JoyShock Side Right"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// DualSense Edge
	EKeys::AddKey(FKeyDetails(FunctionLeftButtonKey, LOCTEXT("JoyShock_Function_Left", "JoyShock Function Left"), FKeyDetails::GamepadKey, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(FunctionRightButtonKey, LOCTEXT("JoyShock_Function_Right", "JoyShock Function Right"), FKeyDetails::GamepadKey, JoyShockControllerName));
}

FString FJoyShockInterface::GetDeviceName(int32 InControllerId)
{
	// TODO: Add Player Number to Device Name
	int32 ControllerType = UJoyShockLibrary::JslGetControllerType(InControllerId);

	switch (ControllerType)
	{
	case JS_TYPE_JOYCON_LEFT:
		return TEXT("Left Joy-Con");
	case JS_TYPE_JOYCON_RIGHT:
		return TEXT("Right Joy-Con");
	case JS_TYPE_PRO_CONTROLLER:
		return TEXT("Pro Controller");
	case JS_TYPE_DS4:
		return TEXT("DualShock 4");
	case JS_TYPE_DS:
		return TEXT("DualSense");
	default:
		return TEXT("Unknown Controller");
	}
}

void FJoyShockInterface::GetPlatformUserAndDevice(int32 InControllerId, EInputDeviceConnectionState InDeviceState,
	FPlatformUserId& OutPlatformUserId, FInputDeviceId& OutDeviceId)
{
	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
	DeviceMapper.RemapControllerIdToPlatformUserAndDevice(InControllerId, OUT OutPlatformUserId, OUT OutDeviceId);

	// If the controller is connected now but was not before, refresh the information
	if (InDeviceState == EInputDeviceConnectionState::Connected || InDeviceState == EInputDeviceConnectionState::Disconnected)
	{
		DeviceMapper.Internal_MapInputDeviceToUser(OutDeviceId, OutPlatformUserId, InDeviceState);
	}
}

void FJoyShockInterface::SendControllerEvents()
{
	bIsGamepadAttached = false;

	/*TArray<int32> DeviceHandles = {};
	UJoyShockLibrary::JslGetConnectedDeviceHandles(DeviceHandles);*/

	bIsGamepadAttached = !DeviceHandles.IsEmpty();

	for (int32 Index = DeviceHandles.Num() - 1; Index >= 0; Index--)
	{
		int32 DeviceHandle = DeviceHandles[Index];

		static FName SystemName(TEXT("JoyShock4Unreal"));
		static FString ControllerName(GetDeviceName(DeviceHandle)); 
		FInputDeviceScope InputScope(this, SystemName, DeviceHandle, ControllerName);

		FControllerState& ControllerState = ControllerStateByDeviceHandle[DeviceHandle];
		if (ControllerState.bIsConnected)
		{
			// if (CachedSettings->bControllerEventsWaitForEngineTick) // TODO: Implement this setting
			{
				const FPlatformUserId& PlatformUser = ControllerState.PlatformUser;
				const FInputDeviceId& InputDevice = ControllerState.InputDevice;

				{
					FScopeLock Lock(&SimpleStateLock);
					int32 CurrentButtons = ControllerState.SimpleState.buttons;
					int32 PreviousButtons = ControllerState.PreviousSimpleState.buttons;
					ProcessButtons(CurrentButtons, PreviousButtons, PlatformUser, InputDevice);
					ProcessAnalogInputs(ControllerState.SimpleState, ControllerState.PreviousSimpleState, PlatformUser, InputDevice);
					// ProcessIMUState(ControllerState.IMUState, ControllerState.PreviousIMUState, PlatformUser, InputDevice);
				}
				
				{
					FScopeLock Lock(&TouchStateLock);
					ProcessTouchState(ControllerState.TouchState, ControllerState.PreviousTouchState, PlatformUser, InputDevice);
				}

				ControllerState.PreviousSimpleState = ControllerState.SimpleState;
				ControllerState.PreviousTouchState = ControllerState.TouchState;
				ControllerState.PreviousIMUState = ControllerState.IMUState;
			}
		}
		else
		{
			DeviceHandles.RemoveAt(Index);
		}
	}
}


void FJoyShockInterface::SetMessageHandler(const TSharedRef< FGenericApplicationMessageHandler >& InMessageHandler)
{
	MessageHandler = InMessageHandler;
}

void FJoyShockInterface::SetChannelValue(int32 ControllerId, const FForceFeedbackChannelType ChannelType, const float Value)
{
	if (ControllerId >= 0 && ControllerId < MAX_NUM_JOYSHOCK_CONTROLLERS)
	{
		// TODO: Implement rumble
		/*FControllerState& ControllerState = ControllerStates[ ControllerId ];

		if( ControllerState.bIsConnected )
		{
			switch( ChannelType )
			{
				case FForceFeedbackChannelType::LEFT_LARGE:
					ControllerState.ForceFeedback.LeftLarge = Value;
					break;

				case FForceFeedbackChannelType::LEFT_SMALL:
					ControllerState.ForceFeedback.LeftSmall = Value;
					break;

				case FForceFeedbackChannelType::RIGHT_LARGE:
					ControllerState.ForceFeedback.RightLarge = Value;
					break;

				case FForceFeedbackChannelType::RIGHT_SMALL:
					ControllerState.ForceFeedback.RightSmall = Value;
					break;
			}
		}*/
	}
}

void FJoyShockInterface::SetChannelValues( int32 ControllerId, const FForceFeedbackValues &Values )
{
	if (ControllerId >= 0 && ControllerId < MAX_NUM_JOYSHOCK_CONTROLLERS)
	{
		// TODO: Implement rumble
		/*FControllerState& ControllerState = ControllerStates[ ControllerId ];

		if( ControllerState.bIsConnected )
		{
			ControllerState.ForceFeedback = Values;
		}*/
	}
}

void FJoyShockInterface::OnControllerAnalog(const FPlatformUserId& InPlatformUser, const FInputDeviceId& InInputDevice, const FName& GamePadKey, const float NewAxisValueNormalized, const float OldAxisValueNormalized, float DeadZone) const
{
	if (JoyShockEnableXInputDeadzones == 0)
		DeadZone = 0.0f;

	// Send new analog data if it's different or outside the platform deadzone.
	if (OldAxisValueNormalized != NewAxisValueNormalized || FMath::Abs(NewAxisValueNormalized) > DeadZone)
		MessageHandler->OnControllerAnalog(GamePadKey, InPlatformUser, InInputDevice, NewAxisValueNormalized);
	
}

void FJoyShockInterface::ProcessButtons(int32 CurrentButtons, int32 PreviousButtons, FPlatformUserId PlatformUser, FInputDeviceId InputDevice)
{
	const double CurrentTime = FPlatformTime::Seconds();

	int32 PressedButtons = ~PreviousButtons & CurrentButtons;
	int32 ReleasedButtons = PreviousButtons & ~CurrentButtons;
	int32 HeldButtons = CurrentButtons & PreviousButtons;
	
	for (int32 Index = 0; Index < JoyShockMaskMappings.Num(); Index++)
	{
		TTuple<int32, FName> MaskMappingTuple = JoyShockMaskMappings[Index];
		const int32& Mask = MaskMappingTuple.Key;
		const FName& Mapping = MaskMappingTuple.Value;

		if (PressedButtons & Mask)
		{
			// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>>>BUTTON PRESSED: %s"), *Mapping.ToString());
			MessageHandler->OnControllerButtonPressed(Mapping, PlatformUser, InputDevice, false);
					
			// this button was pressed - set the button's NextRepeatTime to the InitialButtonRepeatDelay
			NextRepeatTimes[Index] = CurrentTime + InitialButtonRepeatDelay;
		}
		else if (ReleasedButtons & Mask)
		{
			// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>>>BUTTON RELEASED: %s"), *Mapping.ToString());
			MessageHandler->OnControllerButtonReleased(Mapping, PlatformUser, InputDevice, false);
		}
		else if ((HeldButtons & Mask) && NextRepeatTimes[Index] <= CurrentTime)
		{
			// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>>>BUTTON HELD: %s"), *Mapping.ToString());
			MessageHandler->OnControllerButtonPressed(Mapping, PlatformUser, InputDevice, true);

			// set the button's NextRepeatTime to the ButtonRepeatDelay
			NextRepeatTimes[Index] = CurrentTime + ButtonRepeatDelay;
		}
	}
}

void FJoyShockInterface::ProcessAnalogInputs(const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState, FPlatformUserId PlatformUser, FInputDeviceId InputDevice)
{
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::LeftAnalogX, SimpleState.stickLX, PreviousSimpleState.stickLX, XInputLeftStickDeadzone);
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::LeftAnalogY, SimpleState.stickLY, PreviousSimpleState.stickLY, XInputLeftStickDeadzone);

	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::RightAnalogX, SimpleState.stickRX, PreviousSimpleState.stickRX, XInputRightStickDeadzone);
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::RightAnalogY, SimpleState.stickRY, PreviousSimpleState.stickRY, XInputRightStickDeadzone);

	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::LeftTriggerAnalog, SimpleState.lTrigger, PreviousSimpleState.lTrigger, XInputTriggerDeadzone);
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::RightTriggerAnalog, SimpleState.rTrigger, PreviousSimpleState.rTrigger, XInputTriggerDeadzone);
}

void FJoyShockInterface::OnPollCallback(int32 DeviceHandle, const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState, const FIMUState& IMUState, const FIMUState& PreviousIMUState, float DeltaTime)
{
	if (!DeviceHandles.Contains(DeviceHandle))
		return;
	
	FControllerState& State = ControllerStateByDeviceHandle.FindOrAdd(DeviceHandle);

	// if (CachedSettings->bControllerEventsWaitForEngineTick) // TODO: Implement this setting
	{
		FScopeLock Lock(&SimpleStateLock);
		State.SimpleState.Update(SimpleState, State.PreviousSimpleState);
		State.IMUState = IMUState;
	}
	/*else
	{
		// Use cached data from OnConnectCallback
		const FPlatformUserId& PlatformUser = State.PlatformUser;
		const FInputDeviceId& InputDevice = State.InputDevice;

		ProcessAnalogInputs(SimpleState, PreviousSimpleState, PlatformUser, InputDevice);

		ProcessButtons(SimpleState.buttons, PreviousSimpleState.buttons, PlatformUser, InputDevice);

		// ProcessIMUState(IMUState, PreviousIMUState, PlatformUser, InputDevice);
	}*/
}

void FJoyShockInterface::ProcessSingleTouchState(bool bTouchDown, int32 TouchID, const FVector2D& TouchLocation, bool bPreviousTouchDown, int32 PreviousTouchID, const FVector2D& PreviousTouchLocation, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const
{
	if (bTouchDown && !bPreviousTouchDown)
	{
		// Just touched
		// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>Touch Started at %s with ID %d."), *TouchLocation.ToString(), TouchID);
		MessageHandler->OnTouchStarted(nullptr, TouchLocation, 1.0f, TouchID, PlatformUser, InputDevice);
	}
	else if (!bTouchDown && bPreviousTouchDown)
	{
		// Just released
		// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>Touch Released at %s with ID %d."), *TouchLocation.ToString(), TouchID);
		MessageHandler->OnTouchEnded(PreviousTouchLocation, TouchID, PlatformUser, InputDevice);
	}
	else if (bTouchDown && bPreviousTouchDown && TouchLocation != PreviousTouchLocation)
	{
		// Moved
		// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>Touch Moved at %s with ID %d."), *TouchLocation.ToString(), TouchID);
		MessageHandler->OnTouchMoved(TouchLocation, 1.0f, TouchID, PlatformUser, InputDevice);
	}
}

void FJoyShockInterface::ProcessTouchState(const FTouchState& InTouchState, const FTouchState& InPreviousTouchState, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const
{
	FVector2D CurrentTouch0Location(InTouchState.t0X, InTouchState.t0Y);
	FVector2D PreviousTouch0Location(InPreviousTouchState.t0X, InPreviousTouchState.t0Y);
	ProcessSingleTouchState(InTouchState.t0Down, /*InTouchState.t0Id*/ 0, CurrentTouch0Location, InPreviousTouchState.t0Down, InPreviousTouchState.t0Id, PreviousTouch0Location, PlatformUser, InputDevice);
    
	FVector2D CurrentTouch1Location(InTouchState.t1X, InTouchState.t1Y);
	FVector2D PreviousTouch1Location(InPreviousTouchState.t1X, InPreviousTouchState.t1Y);
	ProcessSingleTouchState(InTouchState.t1Down, /*InTouchState.t1Id*/ 1, CurrentTouch1Location, InPreviousTouchState.t1Down, InPreviousTouchState.t1Id, PreviousTouch1Location, PlatformUser, InputDevice);
}

void FJoyShockInterface::OnTouchCallback(int32 DeviceHandle, const FTouchState& TouchState, const FTouchState& PreviousTouchState, float DeltaTime)
{
	if (!DeviceHandles.Contains(DeviceHandle))
		return;
	
	FControllerState& ControllerState = ControllerStateByDeviceHandle.FindOrAdd(DeviceHandle);

	// if (CachedSettings->bControllerEventsWaitForEngineTick) // TODO: Implement this setting
	{
		FScopeLock Lock(&TouchStateLock);
		ControllerState.TouchState = TouchState;
	}
	/*else
	{
		const FPlatformUserId& PlatformUser = ControllerState.PlatformUser;
    	const FInputDeviceId& InputDevice = ControllerState.InputDevice;
    	
		ProcessTouchState(TouchState, PreviousTouchState, PlatformUser, InputDevice);
	}*/
}

void FJoyShockInterface::OnConnectCallback(int32 InDeviceHandle)
{
	// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>OnConnectCallback %d"), InDeviceHandle);
	DeviceHandles.AddUnique(InDeviceHandle);

	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
	FPlatformUserId PlatformUser = PLATFORMUSERID_NONE; // FPlatformMisc::GetPlatformUserForUserIndex(i);
	FInputDeviceId InputDevice = INPUTDEVICEID_NONE;
	GetPlatformUserAndDevice(InDeviceHandle, EInputDeviceConnectionState::Connected, PlatformUser, InputDevice);
	
	FControllerState& State = ControllerStateByDeviceHandle.FindOrAdd(InDeviceHandle);

	State.bIsConnected = true;
	State.PlatformUser = PlatformUser;
	State.InputDevice = InputDevice;

	for (int32 Index = 0; Index < DeviceHandles.Num(); Index++)
	{
		int32 Handle = DeviceHandles[Index];

		UJoyShockLibrary::JslGetControllerInfoAndSettings(Handle);
		if (Handle == InDeviceHandle)
		{
			DeviceMapper.RemapControllerIdToPlatformUserAndDevice(Index, OUT PlatformUser, OUT InputDevice);			
		}
	}
}

void FJoyShockInterface::OnDisconnectCallback(int32 InDeviceHandle, bool bInHasTimedOut)
{
	// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>OnDisconnectCallback %d"), InDeviceHandle);
	if (!DeviceHandles.Contains(InDeviceHandle))
	{
		return; // Should never happen!
	}

	FControllerState& ControllerState = ControllerStateByDeviceHandle.FindChecked(InDeviceHandle);
	FPlatformUserId PlatformUser = ControllerState.PlatformUser; // PLATFORMUSERID_NONE;
	FInputDeviceId InputDevice = ControllerState.InputDevice; // INPUTDEVICEID_NONE;
	ControllerState.bIsConnected = false;

	GetPlatformUserAndDevice(InDeviceHandle, EInputDeviceConnectionState::Disconnected, PlatformUser, InputDevice);
}
