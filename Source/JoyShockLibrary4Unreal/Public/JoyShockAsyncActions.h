// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/CancellableAsyncAction.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "JoyShockSubsystem.h"
#include "JoyShockAsyncActions.generated.h"

/**
 * GAS-style async nodes for the plugin's events. Each of these places a single latent node (clock icon)
 * whose event pins carry their payload as regular data pins -- no Create Event / Bind Event / custom-event
 * signature matching, which is the part of the delegate workflow that is genuinely hard to wire correctly.
 *
 * The UJoyShockSubsystem Listen* functions and BlueprintAssignable events remain the underlying mechanism
 * (and stay available); these nodes are Blueprint sugar bound on top of the same subsystem, so both styles
 * see identical ordering and payloads. All pins fire on the game thread with player assignments already
 * applied, so the whole JSL4U* API is safe to call from them.
 *
 * Lifetime: the action lives until its owning Game Instance shuts down, or until Cancel is called on the
 * Async Action output pin. The pins can fire any number of times.
 */
UCLASS()
class JOYSHOCKLIBRARY4UNREAL_API UJSL4UWaitForControllerChanges : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	// Fires once for every controller already connected when the node activates, then for every later
	// connection -- the same atomic bind-and-replay as Listen For Controllers, so no connection can fall
	// into a gap between placing the node and the controller arriving (in a packaged game controllers
	// usually finish enumerating after BeginPlay).
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Connected"))
	FJSL4UControllerInfoConnectedDelegate OnConnected;

	// Fires with the controller's last known identity; Timed Out is true when reports stopped instead of
	// a clean disconnect.
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Disconnected"))
	FJSL4UControllerInfoDisconnectedDelegate OnDisconnected;

	/**
	 * Waits for controller connections and disconnections. Controllers already connected fire On Connected
	 * immediately, so there is no window in which one can be missed.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Events",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
			DisplayName = "Wait For Controller Changes",
			Keywords = "JSL4U listen controller connect disconnect joyshock"))
	static UJSL4UWaitForControllerChanges* WaitForControllerChanges(UObject* WorldContextObject);

	virtual void Activate() override;
	virtual void SetReadyToDestroy() override;

private:
	UFUNCTION()
	void HandleConnected(FJSL4UControllerInfo Controller);
	UFUNCTION()
	void HandleDisconnected(FJSL4UControllerInfo Controller, bool bTimedOut);

	TWeakObjectPtr<UJoyShockSubsystem> Subsystem;
};

UCLASS()
class JOYSHOCKLIBRARY4UNREAL_API UJSL4UWaitForJoyConPairingChanges : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	// Fires after a left and right Joy-Con become one logical vertical controller. Both infos already
	// contain the shared player index and Vertical grip mode.
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Joined"))
	FJSL4UJoyConsPairingDelegate OnJoined;

	// Fires after a pair becomes two standalone horizontal controllers, each info already carrying its own
	// separate player assignment.
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Separated"))
	FJSL4UJoyConsPairingDelegate OnSeparated;

	/**
	 * Waits for Joy-Con join/separate transitions. No replay: these are actions, not persistent state --
	 * query Get Connected Controllers when the current pairing is also needed during setup.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Events",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
			DisplayName = "Wait For Joy-Con Pairing Changes",
			Keywords = "JSL4U joycon join separate pair joyshock"))
	static UJSL4UWaitForJoyConPairingChanges* WaitForJoyConPairingChanges(UObject* WorldContextObject);

	virtual void Activate() override;
	virtual void SetReadyToDestroy() override;

private:
	UFUNCTION()
	void HandleJoined(FJSL4UControllerInfo LeftJoyCon, FJSL4UControllerInfo RightJoyCon);
	UFUNCTION()
	void HandleSeparated(FJSL4UControllerInfo LeftJoyCon, FJSL4UControllerInfo RightJoyCon);

	TWeakObjectPtr<UJoyShockSubsystem> Subsystem;
};

// One controller, connected or gone. Shared by both pins of Wait For Any Controller Changes: a controller
// leaving carries no extra information worth a second signature, and a controller Unreal does not own has
// no notion of the timeout the JSL4U-specific disconnect pin reports.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJSL4UControllerInfoDelegate,
	FJSL4UControllerInfo, Controller);

UCLASS()
class JOYSHOCKLIBRARY4UNREAL_API UJSL4UWaitForAnyControllerChanges : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	// Fires once for every controller already connected when the node activates, then for every later
	// connection. Same bind-then-replay as Wait For Controller Changes, so nothing arriving mid-activation
	// is missed, and a controller only ever reports once however many times Unreal re-announces it.
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Connected"))
	FJSL4UControllerInfoDelegate OnConnected;

	// Fires with the controller's last known description, since by the time a disconnect is reported there
	// is nothing left to ask about it.
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Disconnected"))
	FJSL4UControllerInfoDelegate OnDisconnected;

	/**
	 * Waits for ANY controller Unreal accepts to connect or disconnect -- including Xbox controllers and
	 * everything else that arrives through XInput, which the JSL4U-only Wait For Controller Changes does
	 * not report. Use this to decide when a player joins or leaves; use Wait For Controller Changes when
	 * you specifically need a JSL4U controller's gyro, touchpad, lights or HD rumble.
	 *
	 * This plugin does not implement XInput and does not need to: Unreal already supports those pads
	 * natively and JSL4U shares player slots with them. This node simply reports both sources through one
	 * pin, and Is Joy Shock Controller on the payload says which one each came from -- though you rarely
	 * need to ask, since every node takes the payload's Connection Id whichever kind it is.
	 *
	 * The keyboard and mouse are not reported. They share a single device that is connected from the first
	 * frame of every game, so announcing it as a controller joining would spawn a player for it before
	 * anyone had touched anything.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Events",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
			DisplayName = "Wait For Any Controller Changes",
			Keywords = "JSL4U xinput xbox gamepad any controller connect disconnect player join joyshock"))
	static UJSL4UWaitForAnyControllerChanges* WaitForAnyControllerChanges(UObject* WorldContextObject);

	virtual void Activate() override;
	virtual void SetReadyToDestroy() override;

private:
	void HandleConnectionChange(EInputDeviceConnectionState NewState, FPlatformUserId PlatformUser,
		FInputDeviceId InputDevice);
	bool DrainPending(float DeltaTime);
	static FJSL4UControllerInfo DescribeDevice(FPlatformUserId PlatformUser, FInputDeviceId InputDevice);

	// A change as the device mapper reported it, before anything was asked about the device.
	struct FPendingChange
	{
		FPlatformUserId PlatformUser;
		FInputDeviceId InputDevice;
		bool bConnected = false;
	};

	/**
	 * Changes waiting to be reported, and the ticker that reports them.
	 *
	 * The mapper's delegate fires from deep inside this plugin's own connect handling -- it is
	 * RefreshPlayerAssignments mapping the device to its player that raises it -- which means it arrives on
	 * the game thread but with the interface's container lock held and its assignment only half applied.
	 * Broadcasting straight from there hands that state to a Blueprint that will happily call Create Player
	 * and Spawn Actor inside it, re-entering the plugin mid-update. The rest of the plugin is careful never
	 * to do this: connects, disconnects and pairing changes are all queued and broadcast once the locks are
	 * released. This queue is how this node keeps the same promise.
	 */
	TArray<FPendingChange> PendingChanges;
	FTSTicker::FDelegateHandle TickerHandle;

	// The last description of every controller seen connected, keyed by Unreal's input device id. A
	// disconnect has to be answered from here: by the time it is reported the device is already gone from
	// everything that could describe it, ours included.
	TMap<int32, FJSL4UControllerInfo> KnownByInputDeviceId;

	FDelegateHandle ConnectionChangeHandle;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJSL4ULowBatteryDelegate,
	FJSL4UControllerInfo, Controller, EJSL4UBatteryLevel, Level);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJSL4UChargingChangedDelegate,
	FJSL4UControllerInfo, Controller, bool, bIsCharging);

UCLASS()
class JOYSHOCKLIBRARY4UNREAL_API UJSL4UWaitForBatteryChanges : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	// Fires when a controller's charge falls to Warn At Level or below while it is not charging. Fires
	// once per episode: it re-arms when the controller charges past that level again, or is plugged in.
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Low Battery"))
	FJSL4ULowBatteryDelegate OnLowBattery;

	// Fires whenever a controller starts or stops drawing power. Plugging in a warned controller also
	// clears its warning, so a game can dismiss the low-battery UI from here without tracking state.
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Charging Changed"))
	FJSL4UChargingChangedDelegate OnChargingChanged;

	/**
	 * Watches every connected controller's charge.
	 *
	 * Battery moves over minutes, so this polls once a second rather than adding a per-report event path;
	 * a controller that is already low when the node activates is reported on the first poll.
	 *
	 * Controllers reporting Unknown (the Switch 2 Pro Controller does) never raise On Low Battery -- an
	 * absent reading is not a flat battery.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Events",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
			DisplayName = "Wait For Battery Changes", AdvancedDisplay = "WarnAtLevel",
			Keywords = "JSL4U battery low charge charging power joyshock"))
	static UJSL4UWaitForBatteryChanges* WaitForBatteryChanges(UObject* WorldContextObject,
		EJSL4UBatteryLevel WarnAtLevel = EJSL4UBatteryLevel::Low);

	virtual void Activate() override;
	virtual void SetReadyToDestroy() override;

private:
	bool Poll(float DeltaTime);

	struct FBatteryWatch
	{
		EJSL4UBatteryLevel Level = EJSL4UBatteryLevel::Unknown;
		bool bIsCharging = false;
		bool bLowReported = false;
		bool bSeen = false;
	};

	EJSL4UBatteryLevel WarnLevel = EJSL4UBatteryLevel::Low;
	TMap<int64, FBatteryWatch> WatchByConnectionId;
	FTSTicker::FDelegateHandle TickerHandle;
};

UCLASS()
class JOYSHOCKLIBRARY4UNREAL_API UJSL4UWaitForControllerFunctionBlocked : public UCancellableAsyncAction
{
	GENERATED_BODY()

public:
	// Fires when a controller function's output is rejected while its input keeps flowing -- another
	// application (Steam Input, typically) holding the controller. Once per function per failure episode.
	UPROPERTY(BlueprintAssignable, meta = (DisplayName = "On Blocked"))
	FJSL4UControllerFunctionBlockedDelegate OnBlocked;

	/**
	 * Waits for a controller function (rumble, player LEDs, HOME light, motion configuration) to be
	 * blocked by another application. Fires only when the game actually uses the function.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Events",
		meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject",
			DisplayName = "Wait For Controller Function Blocked",
			Keywords = "JSL4U steam conflict blocked rumble joyshock"))
	static UJSL4UWaitForControllerFunctionBlocked* WaitForControllerFunctionBlocked(UObject* WorldContextObject);

	virtual void Activate() override;
	virtual void SetReadyToDestroy() override;

private:
	UFUNCTION()
	void HandleBlocked(FJSL4UControllerInfo Controller, EJSL4UControllerFunction Function);

	TWeakObjectPtr<UJoyShockSubsystem> Subsystem;
};
