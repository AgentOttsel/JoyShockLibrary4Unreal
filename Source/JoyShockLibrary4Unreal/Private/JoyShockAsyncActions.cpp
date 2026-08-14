// Copyright Epic Games, Inc. All Rights Reserved.

#include "JoyShockAsyncActions.h"

#include "JoyShockBlueprintLibrary.h"
#include "Engine/GameInstance.h"
#include "GameFramework/InputDeviceSubsystem.h"

namespace
{
	// Shared activation plumbing: every action binds to the game instance's JoyShock subsystem, which is
	// the single source of truth for these events (it caches last-known infos for disconnects and already
	// receives everything on the game thread). An action outside a game world has nothing to bind to.
	UJoyShockSubsystem* ResolveSubsystem(const TWeakObjectPtr<UGameInstance>& RegisteredGameInstance)
	{
		UGameInstance* GameInstance = RegisteredGameInstance.Get();
		return GameInstance ? GameInstance->GetSubsystem<UJoyShockSubsystem>() : nullptr;
	}
}

// --- Wait For Controller Changes ---------------------------------------------------------------------

UJSL4UWaitForControllerChanges* UJSL4UWaitForControllerChanges::WaitForControllerChanges(UObject* WorldContextObject)
{
	UJSL4UWaitForControllerChanges* Action = NewObject<UJSL4UWaitForControllerChanges>();
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UJSL4UWaitForControllerChanges::Activate()
{
	UJoyShockSubsystem* FoundSubsystem = ResolveSubsystem(RegisteredWithGameInstance);
	if (FoundSubsystem == nullptr)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("Wait For Controller Changes: no JoyShock Subsystem reachable from this context; the node will never fire."));
		SetReadyToDestroy();
		return;
	}
	Subsystem = FoundSubsystem;

	FoundSubsystem->OnControllerDisconnected.AddDynamic(this, &UJSL4UWaitForControllerChanges::HandleDisconnected);

	// Bind first, then replay, mirroring Listen For Controllers: there is no gap in which a connection
	// arriving mid-activation can be missed, and a controller that connected long ago still fires.
	FoundSubsystem->OnControllerConnected.AddDynamic(this, &UJSL4UWaitForControllerChanges::HandleConnected);
	for (const FJSL4UControllerInfo& Info : UJoyShockLibrary::JSL4UGetConnectedControllers())
	{
		OnConnected.Broadcast(Info);
	}
}

void UJSL4UWaitForControllerChanges::SetReadyToDestroy()
{
	if (UJoyShockSubsystem* BoundSubsystem = Subsystem.Get())
	{
		BoundSubsystem->OnControllerConnected.RemoveAll(this);
		BoundSubsystem->OnControllerDisconnected.RemoveAll(this);
	}
	Super::SetReadyToDestroy();
}

void UJSL4UWaitForControllerChanges::HandleConnected(FJSL4UControllerInfo Controller)
{
	OnConnected.Broadcast(Controller);
}

void UJSL4UWaitForControllerChanges::HandleDisconnected(FJSL4UControllerInfo Controller, bool bTimedOut)
{
	OnDisconnected.Broadcast(Controller, bTimedOut);
}

// --- Wait For Any Controller Changes -----------------------------------------------------------------

UJSL4UWaitForAnyControllerChanges* UJSL4UWaitForAnyControllerChanges::WaitForAnyControllerChanges(UObject* WorldContextObject)
{
	UJSL4UWaitForAnyControllerChanges* Action = NewObject<UJSL4UWaitForAnyControllerChanges>();
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

FJSL4UControllerInfo UJSL4UWaitForAnyControllerChanges::DescribeDevice(FPlatformUserId PlatformUser,
	FInputDeviceId InputDevice)
{
	const int32 UnrealDeviceId = InputDevice.GetId();

	// "Is this one of ours" is answered by Unreal's own device id, which the plugin allocated for this
	// controller when it connected and reports back in FJSL4UControllerInfo. Matching on that rather than
	// on the hardware name means a controller family the plugin gains later needs no change here. When it
	// is ours, the plugin's own description is already the complete answer and is returned as-is.
	for (const FJSL4UControllerInfo& JoyShockInfo : UJoyShockLibrary::JSL4UGetConnectedControllers())
	{
		if (JoyShockInfo.InputDeviceId == UnrealDeviceId)
		{
			return JoyShockInfo;
		}
	}

	// Not ours. The description lives in the library so that this node and the polling query
	// (Get All Connected Controllers) cannot drift into describing the same pad two different ways.
	return UJoyShockLibrary::JSL4UDescribeUndrivenDevice(PlatformUser, InputDevice);
}

void UJSL4UWaitForAnyControllerChanges::Activate()
{
	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();

	// Bind before replaying, for the same reason Wait For Controller Changes does: a controller arriving
	// mid-activation must not fall into the gap between the two. Re-announcements are filtered on the way
	// out, so overlapping with the replay costs nothing.
	ConnectionChangeHandle = DeviceMapper.GetOnInputDeviceConnectionChange().AddUObject(
		this, &UJSL4UWaitForAnyControllerChanges::HandleConnectionChange);

	TArray<FInputDeviceId> ConnectedDevices;
	DeviceMapper.GetAllConnectedInputDevices(ConnectedDevices);
	for (const FInputDeviceId& InputDevice : ConnectedDevices)
	{
		HandleConnectionChange(EInputDeviceConnectionState::Connected,
			DeviceMapper.GetUserForInputDevice(InputDevice), InputDevice);
	}

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UJSL4UWaitForAnyControllerChanges::DrainPending));
}

void UJSL4UWaitForAnyControllerChanges::SetReadyToDestroy()
{
	// The delegate is a static that outlives every game instance, so leaving this bound would keep calling
	// into a cancelled action for the rest of the session.
	if (ConnectionChangeHandle.IsValid())
	{
		IPlatformInputDeviceMapper::Get().GetOnInputDeviceConnectionChange().Remove(ConnectionChangeHandle);
		ConnectionChangeHandle.Reset();
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	PendingChanges.Reset();
	KnownByInputDeviceId.Reset();
	Super::SetReadyToDestroy();
}

void UJSL4UWaitForAnyControllerChanges::HandleConnectionChange(EInputDeviceConnectionState NewState,
	FPlatformUserId PlatformUser, FInputDeviceId InputDevice)
{
	// The keyboard and mouse share one device that is connected from the first frame of every game.
	// Reporting it as a controller joining would spawn a player for it before anyone had touched anything,
	// and a game that wants a keyboard player knows it has one without being told.
	if (InputDevice == IPlatformInputDeviceMapper::Get().GetDefaultInputDevice())
	{
		return;
	}

	if (NewState != EInputDeviceConnectionState::Connected
		&& NewState != EInputDeviceConnectionState::Disconnected)
	{
		return;
	}

	// Recorded and nothing more. Describing the device here would call back into the plugin that is, right
	// now, part-way through the assignment that raised this delegate -- see PendingChanges.
	PendingChanges.Add({ PlatformUser, InputDevice, NewState == EInputDeviceConnectionState::Connected });
}

bool UJSL4UWaitForAnyControllerChanges::DrainPending(float DeltaTime)
{
	if (PendingChanges.IsEmpty())
	{
		return true;
	}

	// Swapped out before reporting: a listener is free to connect or remove players, which produces more
	// changes, and those belong to the next drain rather than to a queue being walked.
	TArray<FPendingChange> Changes;
	Swap(Changes, PendingChanges);

	for (const FPendingChange& Change : Changes)
	{
		const int32 DeviceKey = Change.InputDevice.GetId();

		if (Change.bConnected)
		{
			const FJSL4UControllerInfo Info = DescribeDevice(Change.PlatformUser, Change.InputDevice);

			// Unreal re-announces a device whenever its user mapping is re-asserted, which this plugin does
			// on every player-slot refresh -- so a second controller connecting would otherwise re-report
			// the first. Only the first announcement fires; later ones just refresh what is remembered,
			// which is what On Disconnected will report and is how a slot change is picked up.
			const bool bAlreadyKnown = KnownByInputDeviceId.Contains(DeviceKey);
			KnownByInputDeviceId.Add(DeviceKey, Info);
			if (!bAlreadyKnown)
			{
				OnConnected.Broadcast(Info);
			}
		}
		else
		{
			// Describing it now would find nothing: a JSL4U controller is gone from the plugin's own list
			// before this fires. Fall back to describing it only for a device never seen connected.
			const FJSL4UControllerInfo* Known = KnownByInputDeviceId.Find(DeviceKey);
			FJSL4UControllerInfo Info = Known != nullptr
				? *Known
				: DescribeDevice(Change.PlatformUser, Change.InputDevice);
			// It is gone, whatever it was when last seen.
			Info.bIsConnected = false;
			KnownByInputDeviceId.Remove(DeviceKey);
			OnDisconnected.Broadcast(Info);
		}
	}

	return true;
}

// --- Wait For Joy-Con Pairing Changes ----------------------------------------------------------------

UJSL4UWaitForJoyConPairingChanges* UJSL4UWaitForJoyConPairingChanges::WaitForJoyConPairingChanges(UObject* WorldContextObject)
{
	UJSL4UWaitForJoyConPairingChanges* Action = NewObject<UJSL4UWaitForJoyConPairingChanges>();
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UJSL4UWaitForJoyConPairingChanges::Activate()
{
	UJoyShockSubsystem* FoundSubsystem = ResolveSubsystem(RegisteredWithGameInstance);
	if (FoundSubsystem == nullptr)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("Wait For Joy-Con Pairing Changes: no JoyShock Subsystem reachable from this context; the node will never fire."));
		SetReadyToDestroy();
		return;
	}
	Subsystem = FoundSubsystem;

	FoundSubsystem->OnJoyConsJoined.AddDynamic(this, &UJSL4UWaitForJoyConPairingChanges::HandleJoined);
	FoundSubsystem->OnJoyConsSeparated.AddDynamic(this, &UJSL4UWaitForJoyConPairingChanges::HandleSeparated);
}

void UJSL4UWaitForJoyConPairingChanges::SetReadyToDestroy()
{
	if (UJoyShockSubsystem* BoundSubsystem = Subsystem.Get())
	{
		BoundSubsystem->OnJoyConsJoined.RemoveAll(this);
		BoundSubsystem->OnJoyConsSeparated.RemoveAll(this);
	}
	Super::SetReadyToDestroy();
}

void UJSL4UWaitForJoyConPairingChanges::HandleJoined(FJSL4UControllerInfo LeftJoyCon, FJSL4UControllerInfo RightJoyCon)
{
	OnJoined.Broadcast(LeftJoyCon, RightJoyCon);
}

void UJSL4UWaitForJoyConPairingChanges::HandleSeparated(FJSL4UControllerInfo LeftJoyCon, FJSL4UControllerInfo RightJoyCon)
{
	OnSeparated.Broadcast(LeftJoyCon, RightJoyCon);
}

// --- Wait For Battery Changes ------------------------------------------------------------------------

UJSL4UWaitForBatteryChanges* UJSL4UWaitForBatteryChanges::WaitForBatteryChanges(UObject* WorldContextObject,
	EJSL4UBatteryLevel WarnAtLevel)
{
	UJSL4UWaitForBatteryChanges* Action = NewObject<UJSL4UWaitForBatteryChanges>();
	Action->WarnLevel = WarnAtLevel;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UJSL4UWaitForBatteryChanges::Activate()
{
	// Unlike the other actions here this one owns no subsystem binding: charge is not delivered as an
	// event, it is a value that changes over minutes, so a slow poll is both sufficient and far less
	// machinery than a per-report path from the polling threads.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UJSL4UWaitForBatteryChanges::Poll), 1.0f);

	// Report the current state immediately: a controller that was already low when this node activated is
	// exactly the case a warning exists for, and waiting a second to say so would be arbitrary.
	Poll(0.f);
}

void UJSL4UWaitForBatteryChanges::SetReadyToDestroy()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	Super::SetReadyToDestroy();
}

bool UJSL4UWaitForBatteryChanges::Poll(float /*DeltaTime*/)
{
	const TArray<FJSL4UControllerInfo> Controllers = UJoyShockLibrary::JSL4UGetConnectedControllers();

	for (const FJSL4UControllerInfo& Info : Controllers)
	{
		FBatteryWatch& Watch = WatchByConnectionId.FindOrAdd(Info.ConnectionId);
		const bool bFirstSight = !Watch.bSeen;
		const bool bChargingChanged = Watch.bIsCharging != Info.bIsCharging;

		Watch.bSeen = true;
		Watch.Level = Info.BatteryLevel;
		Watch.bIsCharging = Info.bIsCharging;

		// Charging is a state, so only transitions are worth reporting -- not the steady truth every
		// second. A controller already plugged in when the node activates is not an event.
		if (bChargingChanged && !bFirstSight)
		{
			OnChargingChanged.Broadcast(Info, Info.bIsCharging);
		}

		// Unknown is not a low battery: the Switch 2 Pro Controller reports it always, and every
		// controller reports it until its first input report arrives.
		const bool bIsLow = Info.BatteryLevel != EJSL4UBatteryLevel::Unknown
			&& Info.BatteryLevel <= WarnLevel
			&& !Info.bIsCharging;

		if (bIsLow)
		{
			if (!Watch.bLowReported)
			{
				Watch.bLowReported = true;
				OnLowBattery.Broadcast(Info, Info.BatteryLevel);
			}
		}
		else
		{
			// Re-arm once it climbs back above the threshold or goes on charge, so a controller that is
			// drained, charged and drained again warns each time rather than only once per session.
			Watch.bLowReported = false;
		}
	}

	// Forget controllers that have gone away, so a reused device id cannot inherit another controller's
	// "already warned" state.
	if (WatchByConnectionId.Num() > Controllers.Num())
	{
		TSet<int64> Live;
		Live.Reserve(Controllers.Num());
		for (const FJSL4UControllerInfo& Info : Controllers)
		{
			Live.Add(Info.ConnectionId);
		}
		for (auto It = WatchByConnectionId.CreateIterator(); It; ++It)
		{
			if (!Live.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
	}

	return true; // keep ticking
}

// --- Wait For Controller Function Blocked ------------------------------------------------------------

UJSL4UWaitForControllerFunctionBlocked* UJSL4UWaitForControllerFunctionBlocked::WaitForControllerFunctionBlocked(UObject* WorldContextObject)
{
	UJSL4UWaitForControllerFunctionBlocked* Action = NewObject<UJSL4UWaitForControllerFunctionBlocked>();
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UJSL4UWaitForControllerFunctionBlocked::Activate()
{
	UJoyShockSubsystem* FoundSubsystem = ResolveSubsystem(RegisteredWithGameInstance);
	if (FoundSubsystem == nullptr)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("Wait For Controller Function Blocked: no JoyShock Subsystem reachable from this context; the node will never fire."));
		SetReadyToDestroy();
		return;
	}
	Subsystem = FoundSubsystem;

	FoundSubsystem->OnControllerFunctionBlocked.AddDynamic(this, &UJSL4UWaitForControllerFunctionBlocked::HandleBlocked);
}

void UJSL4UWaitForControllerFunctionBlocked::SetReadyToDestroy()
{
	if (UJoyShockSubsystem* BoundSubsystem = Subsystem.Get())
	{
		BoundSubsystem->OnControllerFunctionBlocked.RemoveAll(this);
	}
	Super::SetReadyToDestroy();
}

void UJSL4UWaitForControllerFunctionBlocked::HandleBlocked(FJSL4UControllerInfo Controller, EJSL4UControllerFunction Function)
{
	OnBlocked.Broadcast(Controller, Function);
}
