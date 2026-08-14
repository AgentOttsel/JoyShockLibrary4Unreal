// Copyright Epic Games, Inc. All Rights Reserved.

#include "JoyShockInterface.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
// #include "Windows/WindowsApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "GenericPlatform/InputDeviceRegistry.h"
#include "JoyShockBlueprintLibrary.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/IConsoleManager.h"
#include <functional>
#include <mutex>
#include <shared_mutex>

#include "JoyShockLibrary4Unreal.h"
#include "JoyShockInterfaceInternal.h"
#define LOCTEXT_NAMESPACE "JoyShockLibrary"

static TAutoConsoleVariable<int32> CVarJoyShockDebugInputStalls(
	TEXT("JoyShock.Debug.InputStalls"), 0,
	TEXT("Warns when a connected controller stops delivering HID reports for over one second. 0=off, 1=on."));

static TAutoConsoleVariable<int32> CVarJoyShockDebugTouchpad(
	TEXT("JoyShock.Debug.Touchpad"), 0,
	TEXT("Logs the touchpad state of both fingers whenever it changes, exactly as the axis dispatch sees\n")
	TEXT("it. The one way to tell a touch the hardware never reported from one that was reported and lost\n")
	TEXT("on the way to Enhanced Input. 0=off, 1=on."));

static TAutoConsoleVariable<int32> CVarJoyShockEmulateScreenTouch(
	TEXT("JoyShock.Touchpad.EmulateScreenTouch"), 0,
	TEXT("Reports a DualShock 4 / DualSense touchpad to Slate as a screen touch. Off by default: a Slate\n")
	TEXT("pointer press moves that player's focus to whatever widget it lands on, which takes the\n")
	TEXT("controller's focus off the game viewport. Read the pad with JSL4UGetTouchState instead.\n")
	TEXT("0=off, 1=on."));

// Size in pixels to stretch a normalised touchpad coordinate over when the emulation above is enabled.
// Rebuilding the display metrics walks the monitor list, so it is refreshed at most once a second rather
// than once per touch report; changing displays costs at most a second of slightly wrong coordinates.
static FVector2D GetEmulatedTouchScreenSize()
{
	static FVector2D CachedSize(1920.0, 1080.0);
	static double LastRefreshTime = 0.0;

	const double Now = FPlatformTime::Seconds();
	if (Now - LastRefreshTime > 1.0)
	{
		LastRefreshTime = Now;

		FDisplayMetrics Metrics;
		FDisplayMetrics::RebuildDisplayMetrics(Metrics);
		if (Metrics.PrimaryDisplayWidth > 0 && Metrics.PrimaryDisplayHeight > 0)
		{
			CachedSize = FVector2D(Metrics.PrimaryDisplayWidth, Metrics.PrimaryDisplayHeight);
		}
	}

	return CachedSize;
}
TSharedRef<FJoyShockInterface> FJoyShockInterface::Create(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
{
	return MakeShareable(new FJoyShockInterface(InMessageHandler));
}
FJoyShockInterface::FJoyShockInterface(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
	: MessageHandler(InMessageHandler)
{
	InitializeAdditionalKeys();

	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	// Expose this interface to the Blueprint pairing API (UJoyShockLibrary reaches it via the module).
	JSL4UModule.SetActiveInterface(this);

	// TODO: Bind these without using lambda if possible
	JoyShockLockedBindLambda(JSL4UModule, GetOnConnected(), [this](int32 DeviceHandle)
	{
		// Enumeration runs on a background thread, so queue the connect and process it on the game thread
		// in SendControllerEvents (where the platform input-device mapper is safe to touch).
		PendingConnects.Enqueue(DeviceHandle);
	});

	JoyShockLockedBindLambda(JSL4UModule, GetOnDisconnected(), [this](int32 DeviceHandle, bool bHasTimedOut)
	{
		// This fires on a background polling thread. The platform input-device mapper and our
		// controller containers must only be touched on the game thread, so queue the disconnect
		// and let SendControllerEvents process it on the next tick.
		PendingDisconnects.Enqueue(TPair<int32, bool>(DeviceHandle, bHasTimedOut));
	});

	JoyShockLockedBindLambda(JSL4UModule, GetOnFunctionBlocked(), [this](int32 DeviceHandle, EJSL4UControllerFunction Function)
	{
		// Also fires on a polling thread; queue it so the module's game-thread event (and the Blueprint
		// event the subsystem builds on it) fires where the rest of the JSL4U API is safe to call.
		PendingBlockedFunctions.Enqueue(TPair<int32, EJSL4UControllerFunction>(DeviceHandle, Function));
	});

	// These two run on the polling threads. What keeps `this` alive under them is the destructor unbinding
	// them under _callbackLock -- not a null check here, which could never fire: `this` is captured by value,
	// so it is non-null even after the object it points at is gone.
	JoyShockLockedBindLambda(JSL4UModule, GetOnPoll(), [this](int32 DeviceHandle, const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState, const FIMUState& IMUState, const FIMUState& PreviousIMUState, float DeltaTime)
	{
		OnPollCallback(DeviceHandle, SimpleState, PreviousSimpleState, IMUState, PreviousIMUState, DeltaTime);
	});

	JoyShockLockedBindLambda(JSL4UModule, GetOnPollTouch(), [this](int32 DeviceHandle, const FTouchState& TouchState, const FTouchState& PreviousTouchState, float DeltaTime)
	{
		OnTouchCallback(DeviceHandle, TouchState, PreviousTouchState, DeltaTime);
	});
	
	bIsGamepadAttached = false;
	InitialButtonRepeatDelay = 0.2f;
	ButtonRepeatDelay = 0.1f;

	GConfig->GetFloat(TEXT("/Script/Engine.InputSettings"), TEXT("InitialButtonRepeatDelay"), InitialButtonRepeatDelay, GInputIni);
	GConfig->GetFloat(TEXT("/Script/Engine.InputSettings"), TEXT("ButtonRepeatDelay"), ButtonRepeatDelay, GInputIni);

	// Pick up any devices that were already enumerated before this interface (and its OnConnected binding)
	// existed -- e.g. a WM_DEVICECHANGE that fired between module startup and interface creation.
	TArray<int32> ExistingHandles;
	UJoyShockLibrary::GetConnectedDeviceHandles(ExistingHandles);
	for (int32 ExistingHandle : ExistingHandles)
	{
		PendingConnects.Enqueue(ExistingHandle);
	}

	// Enumerate controllers that were already connected before the engine started. WM_DEVICECHANGE only
	// fires on later plug/unplug events, so without this initial pass pre-connected controllers are never
	// discovered. The callbacks are bound above, so OnConnected will be queued for each device found.
	// This runs on a background thread (blocking HID I/O must not stall the game thread).
	JSL4UModule.RequestConnectDevices();
}
FJoyShockInterface::~FJoyShockInterface()
{
	if (FJoyShockLibrary4UnrealModule::IsAvailable())
	{
		FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

		// The four callbacks below capture `this` and are executed by the polling threads, which outlive this
		// object: the threads are stopped when the library shuts down, not when the input device goes away. So
		// leaving them bound leaves a polling thread calling into freed memory the moment the interface is
		// destroyed -- an access violation on shutdown, from a stack that points here rather than at whoever
		// destroyed us.
		//
		// The exclusive lock is what makes this safe rather than merely likely: the polling threads take
		// _callbackLock shared around ExecuteIfBound, so acquiring it exclusively waits for any callback
		// already running to return, and any that arrives afterwards finds the delegates unbound.
		{
			std::unique_lock<std::shared_timed_mutex> CallbackLock(JSL4UModule._callbackLock);
			JSL4UModule.GetOnConnected().Unbind();
			JSL4UModule.GetOnDisconnected().Unbind();
			JSL4UModule.GetOnPoll().Unbind();
			JSL4UModule.GetOnPollTouch().Unbind();
			JSL4UModule.GetOnFunctionBlocked().Unbind();
		}

		if (JSL4UModule.GetActiveInterface() == this)
		{
			JSL4UModule.SetActiveInterface(nullptr);
		}
	}
}
void FJoyShockInterface::InitializeAdditionalKeys()
{
	EKeys::AddMenuCategoryDisplayInfo(JoyShockControllerName, LOCTEXT("JoyShockSubCategory", "JoyShock"), TEXT("GraphEditor.PadEvent_16x"));
	
	// One key per shared bit, named for both families it covers: JSL gives Home (Switch) and PS
	// (PlayStation) the same mask, as it does for Capture and TouchPad Click, so a key per brand is not
	// something the input can distinguish. Branch on FJSL4UControllerInfo::ControllerType if you need a
	// brand-specific button prompt.
	EKeys::AddKey(FKeyDetails(PSButtonKey, LOCTEXT("JoyShock_PS_Button", "JoyShock Home / PS Button"), FKeyDetails::GamepadKey, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(TouchPadClickKey, LOCTEXT("JoyShock_TouchPad_Click", "JoyShock Capture / TouchPad Click"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// DualShock/DualSense
	EKeys::AddKey(FKeyDetails(MicButtonKey, LOCTEXT("JoyShock_Mic_Button", "JoyShock Mic Button"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// Single Joy-con
	EKeys::AddKey(FKeyDetails(SideLeftButtonKey, LOCTEXT("JoyShock_Side_Left", "JoyShock Side Left"), FKeyDetails::GamepadKey, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(SideRightButtonKey, LOCTEXT("JoyShock_Side_Right", "JoyShock Side Right"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// DualSense Edge
	EKeys::AddKey(FKeyDetails(FunctionLeftButtonKey, LOCTEXT("JoyShock_Function_Left", "JoyShock Function Left"), FKeyDetails::GamepadKey, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(FunctionRightButtonKey, LOCTEXT("JoyShock_Function_Right", "JoyShock Function Right"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// Switch 2 Pro Controller
	EKeys::AddKey(FKeyDetails(CButtonKey, LOCTEXT("JoyShock_Switch_C", "JoyShock C Button (Switch 2)"), FKeyDetails::GamepadKey, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(GripLeftButtonKey, LOCTEXT("JoyShock_Grip_Left", "JoyShock Grip Left GL (Switch 2)"), FKeyDetails::GamepadKey, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(GripRightButtonKey, LOCTEXT("JoyShock_Grip_Right", "JoyShock Grip Right GR (Switch 2)"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// DualShock 4 / DualSense touchpad, registered as thumbstick-shaped gamepad axes rather than as screen
	// touches -- see the note on TouchPad1XKeyName for why Unreal's Touch1..Touch10 are the wrong keys for
	// a controller touchpad. The 2D pairing is what makes each pad bindable to a single Vector2D Input
	// Action, exactly like Gamepad Left/Right Thumbstick 2D-Axis.
	EKeys::AddKey(FKeyDetails(TouchPad1XKey, LOCTEXT("JoyShock_TouchPad1_X", "JoyShock TouchPad 1 X-Axis"), FKeyDetails::GamepadKey | FKeyDetails::Axis1D, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(TouchPad1YKey, LOCTEXT("JoyShock_TouchPad1_Y", "JoyShock TouchPad 1 Y-Axis"), FKeyDetails::GamepadKey | FKeyDetails::Axis1D, JoyShockControllerName));
	EKeys::AddPairedKey(FKeyDetails(TouchPad1Key, LOCTEXT("JoyShock_TouchPad1", "JoyShock TouchPad 1 2D-Axis"), FKeyDetails::GamepadKey | FKeyDetails::Axis2D, JoyShockControllerName), TouchPad1XKey, TouchPad1YKey);
	EKeys::AddKey(FKeyDetails(TouchPad1TouchedKey, LOCTEXT("JoyShock_TouchPad1_Touched", "JoyShock TouchPad 1 Touched"), FKeyDetails::GamepadKey, JoyShockControllerName));

	EKeys::AddKey(FKeyDetails(TouchPad2XKey, LOCTEXT("JoyShock_TouchPad2_X", "JoyShock TouchPad 2 X-Axis"), FKeyDetails::GamepadKey | FKeyDetails::Axis1D, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(TouchPad2YKey, LOCTEXT("JoyShock_TouchPad2_Y", "JoyShock TouchPad 2 Y-Axis"), FKeyDetails::GamepadKey | FKeyDetails::Axis1D, JoyShockControllerName));
	EKeys::AddPairedKey(FKeyDetails(TouchPad2Key, LOCTEXT("JoyShock_TouchPad2", "JoyShock TouchPad 2 2D-Axis"), FKeyDetails::GamepadKey | FKeyDetails::Axis2D, JoyShockControllerName), TouchPad2XKey, TouchPad2YKey);
	EKeys::AddKey(FKeyDetails(TouchPad2TouchedKey, LOCTEXT("JoyShock_TouchPad2_Touched", "JoyShock TouchPad 2 Touched"), FKeyDetails::GamepadKey, JoyShockControllerName));

	// Joy-Con 2 mouse sensors -- one pair of axes per half, because a joined pair is two mice for one
	// player. See the note on MouseLeftXKeyName.
	EKeys::AddKey(FKeyDetails(MouseLeftXKey, LOCTEXT("JoyShock_Mouse_Left_X", "JoyShock Mouse L X-Axis (Switch 2)"), FKeyDetails::GamepadKey | FKeyDetails::Axis1D, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(MouseLeftYKey, LOCTEXT("JoyShock_Mouse_Left_Y", "JoyShock Mouse L Y-Axis (Switch 2)"), FKeyDetails::GamepadKey | FKeyDetails::Axis1D, JoyShockControllerName));
	EKeys::AddPairedKey(FKeyDetails(MouseLeftKey, LOCTEXT("JoyShock_Mouse_Left", "JoyShock Mouse L 2D-Axis (Switch 2)"), FKeyDetails::GamepadKey | FKeyDetails::Axis2D, JoyShockControllerName), MouseLeftXKey, MouseLeftYKey);

	EKeys::AddKey(FKeyDetails(MouseRightXKey, LOCTEXT("JoyShock_Mouse_Right_X", "JoyShock Mouse R X-Axis (Switch 2)"), FKeyDetails::GamepadKey | FKeyDetails::Axis1D, JoyShockControllerName));
	EKeys::AddKey(FKeyDetails(MouseRightYKey, LOCTEXT("JoyShock_Mouse_Right_Y", "JoyShock Mouse R Y-Axis (Switch 2)"), FKeyDetails::GamepadKey | FKeyDetails::Axis1D, JoyShockControllerName));
	EKeys::AddPairedKey(FKeyDetails(MouseRightKey, LOCTEXT("JoyShock_Mouse_Right", "JoyShock Mouse R 2D-Axis (Switch 2)"), FKeyDetails::GamepadKey | FKeyDetails::Axis2D, JoyShockControllerName), MouseRightXKey, MouseRightYKey);
}
FString FJoyShockInterface::GetDeviceName(int32 InControllerId)
{
	// TODO: Add Player Number to Device Name
	int32 ControllerType = UJoyShockLibrary::GetControllerTypeForHandle(InControllerId);

	switch (ControllerType)
	{
	case JS_TYPE_JOYCON_LEFT:
		return TEXT("Left Joy-Con");
	case JS_TYPE_JOYCON_RIGHT:
		return TEXT("Right Joy-Con");
	case JS_TYPE_PRO_CONTROLLER:
		return TEXT("Pro Controller");
	case JS_TYPE_PRO_CONTROLLER_2:
		return TEXT("Pro Controller 2");
	case JS_TYPE_JOYCON2_LEFT:
		return TEXT("Left Joy-Con 2");
	case JS_TYPE_JOYCON2_RIGHT:
		return TEXT("Right Joy-Con 2");
	case JS_TYPE_DS4:
		return TEXT("DualShock 4");
	case JS_TYPE_DS:
		return TEXT("DualSense");
	default:
		return TEXT("Unknown Controller");
	}
}
FName FJoyShockInterface::GetHardwareDeviceIdentifier(int32 InControllerId)
{
	switch (UJoyShockLibrary::GetControllerTypeForHandle(InControllerId))
	{
	case JS_TYPE_JOYCON_LEFT:
		return TEXT("JoyConLeft");
	case JS_TYPE_JOYCON_RIGHT:
		return TEXT("JoyConRight");
	case JS_TYPE_PRO_CONTROLLER:
		return TEXT("SwitchProController");
	case JS_TYPE_PRO_CONTROLLER_2:
		return TEXT("Switch2ProController");
	case JS_TYPE_JOYCON2_LEFT:
		return TEXT("JoyCon2Left");
	case JS_TYPE_JOYCON2_RIGHT:
		return TEXT("JoyCon2Right");
	case JS_TYPE_DS4:
		return TEXT("DualShock4");
	case JS_TYPE_DS:
		return TEXT("DualSense");
	default:
		return TEXT("JoyShockGamepad");
	}
}
void FJoyShockInterface::SendControllerEvents()
{
	// Drain connects/disconnects queued from the background enumeration and polling threads. Handling
	// them here means the platform input-device mapper and our containers are only ever touched on the
	// game thread. Disconnects are processed before connects so that if a handle was freed and reused in
	// the same frame (reconnect), the device ends up connected rather than disconnected.
	{
		// Collected while draining and broadcast afterwards: a listener is free to call back into the
		// JSL4U* API, so we must not still be inside the callbacks (which take ControllerContainerLock)
		// when we fire, and the containers must already be consistent.
		TArray<TPair<int32, bool>> DisconnectedThisTick;
		TArray<int32> ConnectedThisTick;
		// Separations caused by one half of a joined pair going away (see OnDisconnectCallback).
		TArray<FJoyConPairingChange> DisconnectPairingChanges;

		TPair<int32, bool> PendingDisconnect;
		while (PendingDisconnects.Dequeue(PendingDisconnect))
		{
			if (OnDisconnectCallback(PendingDisconnect.Key, PendingDisconnect.Value, DisconnectPairingChanges))
			{
				DisconnectedThisTick.Add(PendingDisconnect);
			}
		}

		int32 PendingConnect;
		while (PendingConnects.Dequeue(PendingConnect))
		{
			if (OnConnectCallback(PendingConnect))
			{
				ConnectedThisTick.Add(PendingConnect);
			}
		}

		if (DisconnectedThisTick.Num() > 0 || ConnectedThisTick.Num() > 0)
		{
			FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

			for (const TPair<int32, bool>& Disconnected : DisconnectedThisTick)
			{
				UE_LOG(LogJoyShockLibrary, Verbose, TEXT("Broadcasting disconnect of device %d (timed out: %d), %d listener(s)."),
					Disconnected.Key, Disconnected.Value ? 1 : 0, JSL4UModule.GetOnDeviceDisconnected().IsBound() ? 1 : 0);
				JSL4UModule.GetOnDeviceDisconnected().Broadcast(Disconnected.Key, Disconnected.Value);
			}

			// After the disconnects, so a listener that removes the departed controller's visuals has
			// already done so by the time it is told the pair it belonged to has separated -- and before the
			// connects, so a controller that comes straight back is never announced into a pair that is
			// still, as far as listeners know, intact.
			BroadcastJoyConPairingChanges(DisconnectPairingChanges);

			for (int32 ConnectedDeviceId : ConnectedThisTick)
			{
				UE_LOG(LogJoyShockLibrary, Verbose, TEXT("Broadcasting connect of device %d, %d listener(s)."),
					ConnectedDeviceId, JSL4UModule.GetOnDeviceConnected().IsBound() ? 1 : 0);
				JSL4UModule.GetOnDeviceConnected().Broadcast(ConnectedDeviceId);
			}
		}

		// Blocked-function reports touch no containers, so they broadcast straight from the drain.
		TPair<int32, EJSL4UControllerFunction> PendingBlocked;
		while (PendingBlockedFunctions.Dequeue(PendingBlocked))
		{
			if (FJoyShockLibrary4UnrealModule::IsAvailable())
			{
				FJoyShockLibrary4UnrealModule::GetInstance().GetOnDeviceFunctionBlocked()
					.Broadcast(PendingBlocked.Key, PendingBlocked.Value);
			}
		}
	}

	TArray<FJoyConPairingChange> PairingChanges;
	{
	FScopeLock ContainerLock(&ControllerContainerLock);

	bIsGamepadAttached = !DeviceHandles.IsEmpty();
	UpdateJoyConGripTransitions(PairingChanges);

	for (int32 Index = DeviceHandles.Num() - 1; Index >= 0; Index--)
	{
		int32 DeviceHandle = DeviceHandles[Index];

		FControllerState& ControllerState = ControllerStateByDeviceHandle[DeviceHandle];
		if (ControllerState.bIsConnected)
		{
			// Diagnostic: a controller that is still "connected" but has stopped delivering reports is
			// invisible from the game -- input just goes quiet, exactly as if the engine had stopped routing
			// it. Say which of the two it is. (A healthy controller reports at 60Hz or faster, so a whole
			// second of silence is already far outside normal.)
			const bool bStallDiagnosticsEnabled = CVarJoyShockDebugInputStalls.GetValueOnGameThread() != 0;
			const double SecondsSinceLastReport = FPlatformTime::Seconds() - ControllerState.LastReportTime;
			if (bStallDiagnosticsEnabled && SecondsSinceLastReport > 1.0)
			{
				if (!ControllerState.bReportedInputStall)
				{
					ControllerState.bReportedInputStall = true;
					UE_LOG(LogJoyShockLibrary, Warning,
						TEXT("Device %d (%s) is still connected but has not delivered an input report for %.1fs -- input has stalled below the engine, not in it."),
						DeviceHandle, *ControllerState.DeviceName, SecondsSinceLastReport);
				}
			}
			else if (bStallDiagnosticsEnabled && ControllerState.bReportedInputStall)
			{
				ControllerState.bReportedInputStall = false;
				UE_LOG(LogJoyShockLibrary, Warning, TEXT("Device %d (%s) is delivering input reports again."),
					DeviceHandle, *ControllerState.DeviceName);
			}

			{
				// PlatformUser is the player slot this device is assigned to (see RefreshPlayerAssignments).
				// Both halves of a joined Joy-Con pair share the same PlatformUser, so their (disjoint)
				// buttons and separate stick axes combine into a single player.
				// (Device attribution for these events is registered once at connect time via
				// FInputDeviceRegistry, replacing the deprecated per-dispatch FInputDeviceScope.)
				const FPlatformUserId& PlatformUser = ControllerState.PlatformUser;
				const FInputDeviceId& InputDevice = ControllerState.InputDevice;

				FIMUState CurrentIMUState;
				{
					FScopeLock Lock(&SimpleStateLock);
					const bool bJoyConLeft = IsJoyConLeftIdentifier(ControllerState.HardwareDeviceIdentifier);
					const bool bJoyConRight = IsJoyConRightIdentifier(ControllerState.HardwareDeviceIdentifier);
					const bool bSonyPad = IsSonyIdentifier(ControllerState.HardwareDeviceIdentifier);
					const int32 SuppressedButtons = ControllerState.SuppressedGripButtons;
					int32 CurrentButtons = ControllerState.SimpleState.buttons & ~SuppressedButtons;
					int32 PreviousButtons = ControllerState.PreviousSimpleState.buttons & ~SuppressedButtons;
					CurrentButtons = TransformJoyConButtons(
						CurrentButtons, bJoyConLeft, bJoyConRight, ControllerState.bJoyConHorizontal);
					PreviousButtons = TransformJoyConButtons(
						PreviousButtons, bJoyConLeft, bJoyConRight, ControllerState.bButtonsWereJoyConHorizontal);
					ProcessButtons(CurrentButtons, PreviousButtons, PlatformUser, InputDevice);

					ProcessAnalogInputs(ControllerState.SimpleState, ControllerState.PreviousSimpleState,
						bJoyConLeft, bJoyConRight, bSonyPad, ControllerState.bJoyConHorizontal,
						ControllerState.bAnalogWasJoyConHorizontal,
						PlatformUser, InputDevice);
					ControllerState.bAnalogWasJoyConHorizontal = ControllerState.bJoyConHorizontal;
					ControllerState.bButtonsWereJoyConHorizontal = ControllerState.bJoyConHorizontal;
					// Copied out rather than dispatched here: ProcessIMUState reads the motion state through
					// the device getters, and doing that under SimpleStateLock would nest this lock inside the
					// library's for no reason.
					CurrentIMUState = ControllerState.IMUState;
				}

				ProcessIMUState(DeviceHandle, CurrentIMUState, PlatformUser, InputDevice);

				// The mouse sensor, into this half's own pair of axes. Sent from here, per device handle,
				// rather than from the analog path: both halves of a joined pair run this loop separately
				// while sharing a player, which is exactly what makes two mice for one player fall out
				// without the pairing code having to know the sensor exists.
				//
				// Only a Joy-Con 2 has one. Nothing else reaches this at all, and a half with the sensor
				// idle sends zero, which is the correct value for an axis nobody is moving.
				{
					const bool bMouseLeft = IsJoyConLeftIdentifier(ControllerState.HardwareDeviceIdentifier);
					const bool bMouseRight = IsJoyConRightIdentifier(ControllerState.HardwareDeviceIdentifier);
					if (bMouseLeft || bMouseRight)
					{
						float MouseDeltaX = 0.f;
						float MouseDeltaY = 0.f;
						UJoyShockLibrary::ConsumeMouseAxisDeltaForHandle(DeviceHandle, MouseDeltaX, MouseDeltaY);

						// Y up, like every other axis this plugin publishes: the sensor counts downwards
						// from the top-left, as a screen does.
						MouseDeltaY = -MouseDeltaY;

						// Through the same dispatcher as every other axis, which sends while the value is
						// off centre and once more when it comes back. Going straight to the message handler
						// worked, but sent a zero every frame to every Joy-Con 2 for the rest of the session
						// -- and put this one axis on a different path from all the others for no reason.
						OnControllerAnalog(PlatformUser, InputDevice,
							bMouseLeft ? MouseLeftXKeyName : MouseRightXKeyName,
							MouseDeltaX, ControllerState.PreviousMouseDeltaX);
						OnControllerAnalog(PlatformUser, InputDevice,
							bMouseLeft ? MouseLeftYKeyName : MouseRightYKeyName,
							MouseDeltaY, ControllerState.PreviousMouseDeltaY);

						ControllerState.PreviousMouseDeltaX = MouseDeltaX;
						ControllerState.PreviousMouseDeltaY = MouseDeltaY;
					}
				}


				{
					FScopeLock Lock(&TouchStateLock);
					ProcessTouchpadInputs(ControllerState.TouchState, ControllerState.PreviousTouchState, PlatformUser, InputDevice);
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

	// Pairing listeners commonly query controller info or open UI. Broadcast only after both state locks
	// have been released so those callbacks can safely call the complete JSL4U API.
	BroadcastJoyConPairingChanges(PairingChanges);
}
void FJoyShockInterface::SetMessageHandler(const TSharedRef< FGenericApplicationMessageHandler >& InMessageHandler)
{
	MessageHandler = InMessageHandler;
}
TArray<int32> FJoyShockInterface::GetDeviceHandlesForControllerId(int32 InControllerId) const
{
	// The id Unreal hands to force feedback is a player index, not one of our device handles, so it has to be
	// resolved back through the same mapper that RefreshPlayerAssignments assigns slots from. (The engine has
	// already undone the bOffsetPlayerGamepadIds offset before calling us, so this is the raw index.)
	TArray<int32> Result;
	if (InControllerId < 0)
	{
		return Result;
	}

	const FPlatformUserId User = IPlatformInputDeviceMapper::Get().GetPlatformUserForUserIndex(InControllerId);
	if (!User.IsValid())
	{
		return Result;
	}

	for (int32 Handle : DeviceHandles)
	{
		const FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
		if (State != nullptr && State->bIsConnected && State->PlatformUser == User)
		{
			Result.Add(Handle);
		}
	}
	return Result;
}
void FJoyShockInterface::SendForceFeedback(int32 DeviceHandle, const FForceFeedbackValues& Values) const
{
	// Match how Unreal's XInput interface reads these channels -- the large/low-frequency motor from
	// LeftLarge and the small/high-frequency one from RightSmall -- so that one Force Feedback Effect asset
	// feels the same on a JoyShock controller as it does on an Xbox pad. Picking a different pairing here
	// would make effects authored against a standard gamepad come out wrong on these controllers only.
	//
	// This only stores the values: the controller's own polling thread is the sole writer of rumble packets,
	// which is what keeps a blocking HID write off the game thread even while an effect is running every
	// frame.
	//
	// It writes to force feedback's own channel rather than through JSL4USetControllerRumble, because Unreal
	// calls in here every frame -- with zeroes whenever no effect is playing. Sharing one pair of values meant
	// that per-frame zero wiped anything a game had set with JSL4USetControllerRumble before the controller
	// could act on it, so direct rumble simply never happened. The polling thread takes the stronger of the
	// two sources per motor instead.
	UJoyShockLibrary::SetForceFeedbackRumble(DeviceHandle,
		FMath::RoundToInt(FMath::Clamp(Values.RightSmall, 0.0f, 1.0f) * 255.0f),
		FMath::RoundToInt(FMath::Clamp(Values.LeftLarge, 0.0f, 1.0f) * 255.0f));
}
void FJoyShockInterface::SetChannelValue(int32 ControllerId, const FForceFeedbackChannelType ChannelType, const float Value)
{
	FScopeLock ContainerLock(&ControllerContainerLock);

	for (int32 Handle : GetDeviceHandlesForControllerId(ControllerId))
	{
		FControllerState& ControllerState = ControllerStateByDeviceHandle[Handle];

		// One channel at a time, so the stored values carry the other three.
		switch (ChannelType)
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

		SendForceFeedback(Handle, ControllerState.ForceFeedback);
	}
}
void FJoyShockInterface::SetChannelValues(int32 ControllerId, const FForceFeedbackValues& Values)
{
	FScopeLock ContainerLock(&ControllerContainerLock);

	for (int32 Handle : GetDeviceHandlesForControllerId(ControllerId))
	{
		FControllerState& ControllerState = ControllerStateByDeviceHandle[Handle];
		ControllerState.ForceFeedback = Values;
		SendForceFeedback(Handle, Values);
	}
}
void FJoyShockInterface::OnControllerAnalog(const FPlatformUserId& InPlatformUser, const FInputDeviceId& InInputDevice, const FName& GamePadKey, const float NewAxisValueNormalized, const float OldAxisValueNormalized) const
{
	// Axis values are reported raw. Deadzones belong to the game, not to the device: Enhanced Input has a
	// Dead Zone modifier per Input Action, which is per-player and tunable, and a device that filtered first
	// would destroy information no consumer could get back (gyro and motion work in particular want the raw
	// value). This used to carry a deadzone parameter fed from XInput's constants, gated behind a console
	// variable that never actually did anything -- the "or it changed" half of the test below matches on
	// every jittering sample, so nothing was ever filtered out.
	//
	// Keep sending while the axis is off centre, not only when it changes, so a stick held at a constant
	// deflection keeps feeding the engine; the change test is what delivers the final sample when it
	// returns to centre.
	if (NewAxisValueNormalized != 0.0f || OldAxisValueNormalized != NewAxisValueNormalized)
	{
		MessageHandler->OnControllerAnalog(GamePadKey, InPlatformUser, InInputDevice, NewAxisValueNormalized);
	}
}
int32 FJoyShockInterface::TransformJoyConButtons(int32 Buttons, bool bJoyConLeft, bool bJoyConRight, bool bHorizontal)
{
	if (!bHorizontal || (!bJoyConLeft && !bJoyConRight))
	{
		return Buttons;
	}

	const int32 Original = Buttons;
	Buttons &= ~(JSMASK_UP | JSMASK_DOWN | JSMASK_LEFT | JSMASK_RIGHT
		| JSMASK_N | JSMASK_S | JSMASK_E | JSMASK_W
		| JSMASK_L | JSMASK_ZL | JSMASK_R | JSMASK_ZR
		| JSMASK_SL | JSMASK_SR | JSMASK_LCLICK | JSMASK_RCLICK);

	// The rail buttons become the two standard shoulders. The outer L/ZL or R/ZR pair has no gameplay
	// function in Nintendo's solo-horizontal presentation and remains available only to the join chord.
	if (Original & JSMASK_SL) Buttons |= JSMASK_L;
	if (Original & JSMASK_SR) Buttons |= JSMASK_R;

	if (bJoyConLeft)
	{
		// Rotate the four directional buttons counter-clockwise into positional face-button keys.
		if (Original & JSMASK_UP) Buttons |= JSMASK_W;
		if (Original & JSMASK_DOWN) Buttons |= JSMASK_E;
		if (Original & JSMASK_LEFT) Buttons |= JSMASK_S;
		if (Original & JSMASK_RIGHT) Buttons |= JSMASK_N;
		if (Original & JSMASK_LCLICK) Buttons |= JSMASK_LCLICK;
	}
	else
	{
		// Rotate the Joy-Con R's physical ABXY positions clockwise.
		if (Original & JSMASK_E) Buttons |= JSMASK_S;
		if (Original & JSMASK_S) Buttons |= JSMASK_W;
		if (Original & JSMASK_W) Buttons |= JSMASK_N;
		if (Original & JSMASK_N) Buttons |= JSMASK_E;
		if (Original & JSMASK_RCLICK) Buttons |= JSMASK_LCLICK;
	}

	return Buttons;
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
void FJoyShockInterface::ProcessAnalogInputs(const FJoyShockState& SimpleState,
	const FJoyShockState& PreviousSimpleState,
	bool bJoyConLeft,
	bool bJoyConRight,
	bool bSonyPad,
	bool bHorizontal,
	bool bWasHorizontal,
	FPlatformUserId PlatformUser,
	FInputDeviceId InputDevice)
{
	auto PresentSticks = [bJoyConLeft, bJoyConRight, bSonyPad](const FJoyShockState& State, bool bStateHorizontal,
		float& OutLeftX, float& OutLeftY, float& OutRightX, float& OutRightY)
	{
		if (bJoyConLeft)
		{
			if (bStateHorizontal)
			{
				// Joy-Con L is rotated counter-clockwise into the solo horizontal grip.
				OutLeftX = -State.stickLY;
				OutLeftY = State.stickLX;
			}
			else
			{
				OutLeftX = State.stickLX;
				OutLeftY = State.stickLY;
			}
			OutRightX = 0.0f;
			OutRightY = 0.0f;
			return;
		}

		if (bJoyConRight)
		{
			// The Switch 1 right-half hardware reports its vertical stick axis with the opposite sign from
			// Unreal's standard gamepad convention. Normalise the device-local vector first, then rotate that
			// canonical vector only when the half is held horizontally. Keeping this device-specific here
			// means a shared Enhanced Input mapping (including To World Space) behaves identically for a
			// joined Joy-Con pair, DualShock/DualSense and Pro Controllers.
			const float NormalizedStickX = State.stickRX;
			const float NormalizedStickY = -State.stickRY;
			if (bStateHorizontal)
			{
				// In the solo-horizontal grip, physical left/right comes from the opposite of the
				// normalised vertical axis while physical up/down comes from the opposite of X. Keep this
				// presentation transform separate from the joined-mode Y normalisation above: applying the
				// same sign to both modes makes horizontal left/right run backwards.
				OutLeftX = -NormalizedStickY;
				OutLeftY = -NormalizedStickX;
				OutRightX = 0.0f;
				OutRightY = 0.0f;
			}
			else
			{
				OutLeftX = 0.0f;
				OutLeftY = 0.0f;
				OutRightX = NormalizedStickX;
				// Negated against NormalizedStickY rather than folded into it, because that vector is also
				// what the horizontal grip above rotates into OutLeftX -- changing it there would send solo
				// horizontal left/right backwards, which is the trap the note in that branch describes. See
				// the convention note at the bottom of this lambda for why the sign moved.
				OutRightY = -NormalizedStickY;
			}
			return;
		}

		OutLeftX = State.stickLX;
		OutLeftY = State.stickLY;
		OutRightX = State.stickRX;
		// The low-level JSL state keeps Sony's normalized convention for direct getters. End-to-end
		// measurement at the Enhanced Input action shows a Sony pad's right Y arriving with the opposite
		// sign from the same physical motion on a Pro Controller, so the two are still reconciled here.
		//
		// Sony pad, not DualShock 4. This exception named only the pad it was measured on, and the DualSense
		// -- whose parser branch inverts the raw Y byte identically, a dozen lines from the DualShock 4's --
		// fell through to the other side of the test and came out inverted against every other controller.
		// Reported from hardware, long after the convention it breaks was settled.
		// Correct only the engine-facing presentation so Gamepad_Right2D and To World Space have one
		// convention across devices without changing JSL getters, motion data, or either left stick.
		//
		// Which convention that is was, until now, whatever the Pro Controller happened to report: every
		// other family was lined up against it. Measured against the engine instead, that was backwards.
		// Unreal's XInput plugin publishes RightAnalogY straight from XINPUT_GAMEPAD::sThumbRY, which is
		// positive upwards, so a standard gamepad reads +1 with the right stick pushed up -- and these
		// controllers were reading -1 there, which is why an Xbox pad's right stick came out inverted
		// against every controller this plugin drives. Both signs below are flipped from what they were;
		// the relationship between the families is untouched, only the shared convention moved onto the
		// engine's. A game that had inverted this axis to compensate should now drop that inversion, and it
		// will hold for every pad instead of all but one.
		OutRightY = bSonyPad ? State.stickRY : -State.stickRY;
	};

	float LeftX, LeftY, RightX, RightY;
	float PreviousLeftX, PreviousLeftY, PreviousRightX, PreviousRightY;
	PresentSticks(SimpleState, bHorizontal, LeftX, LeftY, RightX, RightY);
	PresentSticks(PreviousSimpleState, bWasHorizontal,
		PreviousLeftX, PreviousLeftY, PreviousRightX, PreviousRightY);

	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::LeftAnalogX, LeftX, PreviousLeftX);
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::LeftAnalogY, LeftY, PreviousLeftY);
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::RightAnalogX, RightX, PreviousRightX);
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::RightAnalogY, RightY, PreviousRightY);

	const float LeftTrigger = (bHorizontal && (bJoyConLeft || bJoyConRight)) ? 0.0f : SimpleState.lTrigger;
	const float RightTrigger = (bHorizontal && (bJoyConLeft || bJoyConRight)) ? 0.0f : SimpleState.rTrigger;
	const float PreviousLeftTrigger = (bWasHorizontal && (bJoyConLeft || bJoyConRight)) ? 0.0f : PreviousSimpleState.lTrigger;
	const float PreviousRightTrigger = (bWasHorizontal && (bJoyConLeft || bJoyConRight)) ? 0.0f : PreviousSimpleState.rTrigger;
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::LeftTriggerAnalog, LeftTrigger, PreviousLeftTrigger);
	OnControllerAnalog(PlatformUser, InputDevice, FGamepadKeyNames::RightTriggerAnalog, RightTrigger, PreviousRightTrigger);
}
void FJoyShockInterface::ProcessIMUState(int32 DeviceHandle, const FIMUState& InIMUState, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const
{
	// Reports the controller's motion through Unreal's own motion input -- the same path a phone's gyro and
	// accelerometer use, feeding the Tilt / RotationRate / Gravity / Acceleration keys. That means gyro can
	// be bound in Enhanced Input like any other axis, instead of every project that wants motion having to
	// call this plugin's getters from Blueprint.
	//
	// The axes match JSL4UGetIMUState and JSL4UGetMotionState exactly, so a project can mix Enhanced Input
	// bindings and the direct getters without the two disagreeing about which way is up.
	FVector RotationRate(-InIMUState.gyroZ, InIMUState.gyroX, -InIMUState.gyroY);
	FVector Acceleration(InIMUState.accelZ, InIMUState.accelX, -InIMUState.accelY);

	// A Joy-Con held sideways has its buttons and its stick rotated into that grip a few lines above; its
	// motion is rotated here, by the same function the direct getters use, so Enhanced Input and
	// JSL4UGetIMUState cannot end up disagreeing about which way a sideways Joy-Con is pointing. This
	// sample is always in the controller's own frame -- the gyro-space conversion belongs to the getters --
	// so the rotation applies unconditionally, unlike there.
	{
		bool bHorizontal = false;
		bool bIsLeft = false;
		GetJoyConGrip(DeviceHandle, bHorizontal, bIsLeft);
		const FQuat GripUndo = UJoyShockLibrary::GetJoyConGripUndoRotation(bHorizontal, bIsLeft);
		if (!GripUndo.IsIdentity())
		{
			RotationRate = GripUndo.RotateVector(RotationRate);
			Acceleration = GripUndo.RotateVector(Acceleration);
		}
	}

	// Gravity and orientation are derived state the library maintains, not part of the raw IMU sample, so
	// they come from the motion state rather than from InIMUState. get_motion_state only reads already
	// computed values, so this is cheap enough to do per controller per frame.
	const FJSL4UMotionState MotionState = UJoyShockLibrary::GetMotionStateForHandle(DeviceHandle);

	// Unreal's motion input expects Tilt as the device's attitude in radians. Euler() gives (Roll, Pitch,
	// Yaw) in degrees, which is the component order Unreal uses elsewhere for a rotation carried in a vector.
	const FVector EulerDegrees = MotionState.Orientation.Rotator().Euler();
	const FVector Tilt(
		FMath::DegreesToRadians(EulerDegrees.X),
		FMath::DegreesToRadians(EulerDegrees.Y),
		FMath::DegreesToRadians(EulerDegrees.Z));

	MessageHandler->OnMotionDetected(Tilt, RotationRate, MotionState.Gravity, Acceleration, PlatformUser, InputDevice);
}
void FJoyShockInterface::OnPollCallback(int32 DeviceHandle, const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState, const FIMUState& IMUState, const FIMUState& PreviousIMUState, float DeltaTime)
{
	// Runs on a background polling thread. Only read existing entries here (never add) so the map is not
	// structurally modified off the game thread; the entry is created by OnConnectCallback.
	FScopeLock ContainerLock(&ControllerContainerLock);

	if (!DeviceHandles.Contains(DeviceHandle))
		return;

	FControllerState* State = ControllerStateByDeviceHandle.Find(DeviceHandle);
	if (State == nullptr)
		return;

	State->LastReportTime = FPlatformTime::Seconds();

	{
		FScopeLock Lock(&SimpleStateLock);
		State->SimpleState.Update(SimpleState, State->PreviousSimpleState);
		State->IMUState = IMUState;
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
void FJoyShockInterface::ProcessSingleTouchpadInput(bool bTouchDown, float TouchX, float TouchY,
	bool bPreviousTouchDown, float PreviousTouchX, float PreviousTouchY,
	const FName& XKey, const FName& YKey, const FName& TouchedKey,
	FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const
{
	// JSL reports the finger 0..1 from the top-left of the pad. Centre it to -1..1 and flip Y so up is
	// positive, which is the convention every other axis this interface reports already uses. A finger that
	// is not down reports dead centre rather than its last position, so releasing looks like letting go of a
	// stick -- and so the two axes cannot be read as a position without Touched saying there is one.
	auto ToCentred = [](bool bDown, float Value, float Sign)
	{
		return bDown ? Sign * (Value * 2.0f - 1.0f) : 0.0f;
	};

	OnControllerAnalog(PlatformUser, InputDevice, XKey,
		ToCentred(bTouchDown, TouchX, 1.0f), ToCentred(bPreviousTouchDown, PreviousTouchX, 1.0f));
	OnControllerAnalog(PlatformUser, InputDevice, YKey,
		ToCentred(bTouchDown, TouchY, -1.0f), ToCentred(bPreviousTouchDown, PreviousTouchY, -1.0f));

	// Touched is a plain press/release edge. No auto-repeat: a finger resting on the pad is a held state,
	// not a key someone is leaning on, and repeats would make "on touch" fire continuously.
	if (bTouchDown && !bPreviousTouchDown)
	{
		MessageHandler->OnControllerButtonPressed(TouchedKey, PlatformUser, InputDevice, false);
	}
	else if (!bTouchDown && bPreviousTouchDown)
	{
		MessageHandler->OnControllerButtonReleased(TouchedKey, PlatformUser, InputDevice, false);
	}
}
void FJoyShockInterface::ProcessTouchpadInputs(const FTouchState& InTouchState, const FTouchState& InPreviousTouchState,
	FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const
{
	// Logged here rather than in the parser because this is the last point at which the touch is still the
	// controller's own reading: whatever these numbers say is exactly what the axes below are given. That
	// makes it the line that separates "the hardware never reported this finger" from "it reported it and
	// something between here and Enhanced Input dropped it" -- two problems with no symptom in common but
	// which look identical from a Blueprint.
	if (CVarJoyShockDebugTouchpad.GetValueOnGameThread() != 0
		&& (InTouchState.t0Down != InPreviousTouchState.t0Down
			|| InTouchState.t1Down != InPreviousTouchState.t1Down
			|| InTouchState.t0X != InPreviousTouchState.t0X
			|| InTouchState.t0Y != InPreviousTouchState.t0Y
			|| InTouchState.t1X != InPreviousTouchState.t1X
			|| InTouchState.t1Y != InPreviousTouchState.t1Y))
	{
		UE_LOG(LogJoyShockLibrary, Log,
			TEXT("Touchpad: finger 1 down=%d id=%d (%.4f, %.4f) | finger 2 down=%d id=%d (%.4f, %.4f)"),
			InTouchState.t0Down ? 1 : 0, InTouchState.t0Id, InTouchState.t0X, InTouchState.t0Y,
			InTouchState.t1Down ? 1 : 0, InTouchState.t1Id, InTouchState.t1X, InTouchState.t1Y);
	}

	ProcessSingleTouchpadInput(
		InTouchState.t0Down, InTouchState.t0X, InTouchState.t0Y,
		InPreviousTouchState.t0Down, InPreviousTouchState.t0X, InPreviousTouchState.t0Y,
		TouchPad1XKeyName, TouchPad1YKeyName, TouchPad1TouchedKeyName, PlatformUser, InputDevice);

	ProcessSingleTouchpadInput(
		InTouchState.t1Down, InTouchState.t1X, InTouchState.t1Y,
		InPreviousTouchState.t1Down, InPreviousTouchState.t1X, InPreviousTouchState.t1Y,
		TouchPad2XKeyName, TouchPad2YKeyName, TouchPad2TouchedKeyName, PlatformUser, InputDevice);
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
	// A controller touchpad is not a touchscreen, and Unreal has no input concept for one. The only thing
	// the message handler offers is OnTouchStarted/Moved/Ended, which is a *screen* touch, and feeding a
	// touchpad into it was wrong twice over. JSL reports the finger normalised 0..1 while Slate reads those
	// numbers as absolute desktop pixels, so every touch landed within a pixel of the desktop's top-left
	// corner -- in the editor, whatever panel happens to be there, which is why tapping the pad highlighted
	// random editor UI. Worse, a Slate pointer press moves that Slate user's focus onto whatever it lands
	// on, so the touch quietly took the controller's focus off the game viewport and every button pressed
	// afterwards went to an editor widget instead of to the game. That is the "tapping the touchpad freezes
	// the DualShock's buttons" report, and nothing in the game could recover from it: from the game's side
	// that controller simply stopped existing.
	//
	// So it is off by default. The touchpad itself is not lost -- finger positions and per-touch down/up
	// are in JSL4UGetTouchState, and the click is the Capture / TouchPad Click key -- which is what a game
	// binds anyway. JoyShock.Touchpad.EmulateScreenTouch 1 restores the old behaviour for a game that
	// really does want the pad driving Slate; coordinates are then stretched over the primary display so
	// they at least land on screen, but the focus cost is inherent to synthesising a pointer press and
	// stays. Touches already in flight are still ended when the emulation is switched off, so Slate is
	// never left holding a capture that nothing will ever release.
	const bool bEmulateScreenTouch = CVarJoyShockEmulateScreenTouch.GetValueOnGameThread() != 0;
	if (!bEmulateScreenTouch && !InPreviousTouchState.t0Down && !InPreviousTouchState.t1Down)
	{
		return;
	}

	const FVector2D ScreenSize = GetEmulatedTouchScreenSize();
	static const FTouchState NeutralTouchState;
	const FTouchState& CurrentState = bEmulateScreenTouch ? InTouchState : NeutralTouchState;

	FVector2D CurrentTouch0Location = FVector2D(CurrentState.t0X, CurrentState.t0Y) * ScreenSize;
	FVector2D PreviousTouch0Location = FVector2D(InPreviousTouchState.t0X, InPreviousTouchState.t0Y) * ScreenSize;
	ProcessSingleTouchState(CurrentState.t0Down, /*InTouchState.t0Id*/ 0, CurrentTouch0Location, InPreviousTouchState.t0Down, InPreviousTouchState.t0Id, PreviousTouch0Location, PlatformUser, InputDevice);

	FVector2D CurrentTouch1Location = FVector2D(CurrentState.t1X, CurrentState.t1Y) * ScreenSize;
	FVector2D PreviousTouch1Location = FVector2D(InPreviousTouchState.t1X, InPreviousTouchState.t1Y) * ScreenSize;
	ProcessSingleTouchState(CurrentState.t1Down, /*InTouchState.t1Id*/ 1, CurrentTouch1Location, InPreviousTouchState.t1Down, InPreviousTouchState.t1Id, PreviousTouch1Location, PlatformUser, InputDevice);
}
void FJoyShockInterface::OnTouchCallback(int32 DeviceHandle, const FTouchState& TouchState, const FTouchState& PreviousTouchState, float DeltaTime)
{
	// Runs on a background polling thread. Only read existing entries here (never add) so the map is not
	// structurally modified off the game thread; the entry is created by OnConnectCallback.
	FScopeLock ContainerLock(&ControllerContainerLock);

	if (!DeviceHandles.Contains(DeviceHandle))
		return;

	FControllerState* ControllerState = ControllerStateByDeviceHandle.Find(DeviceHandle);
	if (ControllerState == nullptr)
		return;

	{
		FScopeLock Lock(&TouchStateLock);
		ControllerState->TouchState = TouchState;
	}
	/*else
	{
		const FPlatformUserId& PlatformUser = ControllerState.PlatformUser;
    	const FInputDeviceId& InputDevice = ControllerState.InputDevice;
    	
		ProcessTouchState(TouchState, PreviousTouchState, PlatformUser, InputDevice);
	}*/
}
bool FJoyShockInterface::OnConnectCallback(int32 InDeviceHandle)
{
	// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>OnConnectCallback %d"), InDeviceHandle);
	FScopeLock ContainerLock(&ControllerContainerLock);

	// A device can be announced twice: it announces itself from its poll thread on first input, and this
	// interface also sweeps up already-connected devices when it is created (a device that announced before
	// the delegate was bound would otherwise be invisible). Handling the second one would allocate a second
	// input device id for the same controller and leave the first mapped as connected forever, so ignore it.
	if (const FControllerState* Existing = ControllerStateByDeviceHandle.Find(InDeviceHandle))
	{
		if (Existing->bIsConnected)
		{
			return false;
		}
	}

	DeviceHandles.AddUnique(InDeviceHandle);

	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();

	FControllerState& State = ControllerStateByDeviceHandle.FindOrAdd(InDeviceHandle);

	State.bIsConnected = true;
	// Seed the stall diagnostic from now, so the gap before the first report doesn't read as a stall.
	State.LastReportTime = FPlatformTime::Seconds();
	State.bReportedInputStall = false;

	// JSL reuses a freed device handle for the next controller to connect, so this entry may still hold the
	// previous occupant's buttons and axes. Clear them, or the first tick compares the new controller's
	// state against the old one's and fires phantom presses/releases.
	{
		FScopeLock StateLock(&SimpleStateLock);
		State.SimpleState = {};
		State.PreviousSimpleState = {};
		State.IMUState = {};
		State.PreviousIMUState = {};
	}
	{
		FScopeLock StateLock(&TouchStateLock);
		State.TouchState = {};
		State.PreviousTouchState = {};
	}

	// Same reasoning for the force-feedback channels: SetChannelValue writes one channel and carries the
	// other three, so a handle inherited from a previous occupant would fold that controller's leftover
	// intensities into this one's first effect.
	State.ForceFeedback = FForceFeedbackValues();

	// Allocate a globally-unique input device id for this physical controller. (Using the legacy
	// RemapControllerIdToPlatformUserAndDevice "best guess" device id here can collide with the keyboard's
	// device 0 / other controllers, which corrupts the input-device mapper and hangs Enhanced Input's
	// user-settings init when entering play with several controllers.)
	State.InputDevice = DeviceMapper.AllocateNewInputDeviceId();
	State.PlatformUser = PLATFORMUSERID_NONE; // assigned (and mapped connected) by RefreshPlayerAssignments
	State.ConnectionId = NextConnectionId++;
	State.DeviceName = GetDeviceName(InDeviceHandle);
	State.HardwareDeviceIdentifier = GetHardwareDeviceIdentifier(InDeviceHandle);
	State.bJoyConHorizontal = IsJoyConLeftIdentifier(State.HardwareDeviceIdentifier)
		|| IsJoyConRightIdentifier(State.HardwareDeviceIdentifier);
	State.bAnalogWasJoyConHorizontal = false;
	State.bButtonsWereJoyConHorizontal = false;
	State.SuppressedGripButtons = 0;

	// Register the device's hardware descriptor once, for the lifetime of its device id (replaces the
	// deprecated per-dispatch FInputDeviceScope).
	FInputDeviceDescriptor Descriptor;
	Descriptor.HardwareDeviceHandle = State.InputDevice;
	Descriptor.InputDeviceName = TEXT("JoyShock4Unreal");
	Descriptor.HardwareDeviceIdentifier = State.HardwareDeviceIdentifier;
	FInputDeviceRegistry::RegisterDevice(Descriptor);

	RefreshPlayerAssignments();
	return true;
}
void FJoyShockInterface::ReleaseAllInput(FControllerState& State)
{
	// A disconnect is not a neutral event for input that was already in flight. The engine holds the last
	// value it was given for every key and axis, so a controller switched off (or carried out of Bluetooth
	// range) mid-input leaves that input latched on with nothing left to ever clear it: a held stick keeps
	// the character walking, a held button stays down, a gyro rotation rate keeps turning the camera. Nothing
	// downstream can recover from this on its own, because "no more reports" is indistinguishable from "the
	// player is holding perfectly still" -- only the device knows it is going away, so only the device can
	// say so.
	//
	// So the last thing this device does is dispatch the neutral state, by feeding the existing dispatchers a
	// zeroed "current" against the real "previous". That reuses the exact same presentation and edge
	// detection as a normal frame, which is what makes it complete: every button that is down produces a
	// release, every axis that is off centre produces a zero, every touch that is down produces an end, and
	// nothing else is sent.
	const FPlatformUserId& PlatformUser = State.PlatformUser;
	const FInputDeviceId& InputDevice = State.InputDevice;

	{
		FScopeLock StateLock(&SimpleStateLock);

		const bool bJoyConLeft = IsJoyConLeftIdentifier(State.HardwareDeviceIdentifier);
		const bool bJoyConRight = IsJoyConRightIdentifier(State.HardwareDeviceIdentifier);
		const bool bSonyPad = IsSonyIdentifier(State.HardwareDeviceIdentifier);

		// Released against the buttons the engine was actually last told about, which means after the same
		// suppression and grip transform SendControllerEvents applies -- releasing a raw physical bit would
		// miss the rotated key that is really down on a horizontal Joy-Con.
		const int32 HeldButtons = TransformJoyConButtons(
			State.SimpleState.buttons & ~State.SuppressedGripButtons,
			bJoyConLeft, bJoyConRight, State.bJoyConHorizontal);
		ProcessButtons(/*CurrentButtons*/ 0, /*PreviousButtons*/ HeldButtons, PlatformUser, InputDevice);

		const FJoyShockState NeutralState = {};
		ProcessAnalogInputs(NeutralState, State.SimpleState,
			bJoyConLeft, bJoyConRight, bSonyPad,
			State.bJoyConHorizontal, State.bJoyConHorizontal,
			PlatformUser, InputDevice);

		State.SimpleState = NeutralState;
		State.PreviousSimpleState = NeutralState;
		State.IMUState = {};
		State.PreviousIMUState = {};
	}

	{
		FScopeLock StateLock(&TouchStateLock);
		const FTouchState NeutralTouchState = {};
		ProcessTouchpadInputs(NeutralTouchState, State.TouchState, PlatformUser, InputDevice);
		ProcessTouchState(NeutralTouchState, State.TouchState, PlatformUser, InputDevice);
		State.TouchState = NeutralTouchState;
		State.PreviousTouchState = NeutralTouchState;
	}

	// Motion has no edge detection to drive -- Unreal keeps whatever was last reported -- so it is zeroed
	// directly. Rotation rate is the one that matters: a controller that vanishes mid-turn would otherwise
	// leave a gyro-aiming game spinning forever. Sent unconditionally rather than only for controllers with
	// an IMU, because every controller this plugin supports has one.
	MessageHandler->OnMotionDetected(FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector,
		FVector::ZeroVector, PlatformUser, InputDevice);
}
bool FJoyShockInterface::OnDisconnectCallback(int32 InDeviceHandle, bool bInHasTimedOut,
	TArray<FJoyConPairingChange>& OutPairingChanges)
{
	// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>OnDisconnectCallback %d"), InDeviceHandle);
	FScopeLock ContainerLock(&ControllerContainerLock);

	if (!DeviceHandles.Contains(InDeviceHandle))
	{
		return false;
	}

	FControllerState& ControllerState = ControllerStateByDeviceHandle.FindChecked(InDeviceHandle);
	if (!ControllerState.bIsConnected)
	{
		return false;
	}

	// Before anything else, and specifically before the mapper is told this device is gone: the release
	// events are routed by the same platform user / input device pairing as normal input, so they have to go
	// out while that pairing is still live. Runs on the game thread (this is called from the drain at the top
	// of SendControllerEvents), which is where MessageHandler may be used.
	ReleaseAllInput(ControllerState);

	ControllerState.bIsConnected = false;

	// Tell the mapper this physical device is gone.
	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
	DeviceMapper.Internal_MapInputDeviceToUser(ControllerState.InputDevice, ControllerState.PlatformUser, EInputDeviceConnectionState::Disconnected);

	// This device id is permanently retired (a reconnect allocates a fresh id), so drop its descriptor.
	FInputDeviceRegistry::RemoveDevice(ControllerState.InputDevice);

	// Dissolve any join this device was part of, then re-assign remaining players.
	//
	// A pair losing a half is a separation like any other, and has to be finished like any other: the half
	// that is left goes back to being a standalone horizontal Joy-Con, and listeners are told. Doing only
	// the JoinPartner erase (as this used to) left the survivor stuck in the vertical mapping it had as
	// part of a pair -- its stick un-rotated, its SL/SR still reading as a joined pair's -- and left every
	// Blueprint that draws pairs from Wait For Joy-Con Pairing Changes still drawing the pair, so a
	// Joy-Con switched off with its sync button stayed on screen as a ghost half of a couple that no
	// longer existed. Both other separation paths (the SL+SR chord and JSL4UUnjoinJoyCon) already do this;
	// only the disconnect path did not.
	if (const int32* PartnerPtr = JoinPartner.Find(InDeviceHandle))
	{
		const int32 Partner = *PartnerPtr;
		OutPairingChanges.Add(MakeJoyConPairingChange(InDeviceHandle, Partner, false));
		if (FControllerState* PartnerState = ControllerStateByDeviceHandle.Find(Partner))
		{
			PartnerState->bJoyConHorizontal = true;
		}
		JoinPartner.Remove(Partner);
	}
	JoinPartner.Remove(InDeviceHandle);
	for (auto It = JoinPartner.CreateIterator(); It; ++It)
	{
		if (It.Value() == InDeviceHandle)
		{
			It.RemoveCurrent();
		}
	}
	RefreshPlayerAssignments();
	return true;
}

#undef LOCTEXT_NAMESPACE
