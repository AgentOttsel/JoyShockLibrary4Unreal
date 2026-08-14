// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "JoyShockTypes.h"
#include "JoyShockSubsystem.generated.h"

class APlayerController;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FJSL4UControllerInfoConnectedDelegate, FJSL4UControllerInfo, Controller);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJSL4UControllerInfoDisconnectedDelegate, FJSL4UControllerInfo, Controller, bool, bTimedOut);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJSL4UJoyConsPairingDelegate,
	FJSL4UControllerInfo, LeftJoyCon, FJSL4UControllerInfo, RightJoyCon);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FJSL4UControllerFunctionBlockedDelegate,
	FJSL4UControllerInfo, Controller, EJSL4UControllerFunction, Function);

DECLARE_DYNAMIC_DELEGATE_OneParam(FJSL4UControllerInfoConnectedSignature, FJSL4UControllerInfo, Controller);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FJSL4UControllerInfoDisconnectedSignature, FJSL4UControllerInfo, Controller, bool, bTimedOut);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FJSL4UJoyConsPairingSignature,
	FJSL4UControllerInfo, LeftJoyCon, FJSL4UControllerInfo, RightJoyCon);

/**
 * Blueprint access to controller connect/disconnect events, e.g. to keep a controller-assignment screen
 * up to date without polling. Both events fire on the game thread once the controller has been fully
 * added to / removed from the input device interface, so the whole JSL4U* API is safe to call from them.
 */
UCLASS(DisplayName = "JoyShock Subsystem")
class JOYSHOCKLIBRARY4UNREAL_API UJoyShockSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Carries the complete controller identity after the device is ready for the rest of the API.
	UPROPERTY(BlueprintAssignable, Category = "JoyShock Library|Events",
		meta = (DisplayName = "On Controller Connected", ToolTip = "Fires on the game thread after a controller is fully registered. Controller contains both JoyShock and Unreal device identities."))
	FJSL4UControllerInfoConnectedDelegate OnControllerConnected;

	// The last known Controller Info is supplied even though the physical device has already been removed.
	// bIsConnected is false; ConnectionId still identifies the exact connection that ended.
	UPROPERTY(BlueprintAssignable, Category = "JoyShock Library|Events",
		meta = (DisplayName = "On Controller Disconnected", ToolTip = "Fires on the game thread with the disconnected controller's last known identity. Timed Out is true when reports stopped instead of a clean disconnect."))
	FJSL4UControllerInfoDisconnectedDelegate OnControllerDisconnected;

	// Fires after two halves have become one logical vertical controller. Both infos already contain the
	// shared player index, their reciprocal JoinedToConnectionId and Vertical grip mode.
	UPROPERTY(BlueprintAssignable, Category = "JoyShock Library|Events",
		meta = (DisplayName = "On Joy-Cons Joined", ToolTip = "Fires after a left and right Joy-Con become one logical vertical controller. Both controller infos already contain the new pairing and player assignment."))
	FJSL4UJoyConsPairingDelegate OnJoyConsJoined;

	// Fires after a pair has become two standalone horizontal controllers. Both infos already contain their
	// separate player assignments, no join partner and Horizontal grip mode.
	UPROPERTY(BlueprintAssignable, Category = "JoyShock Library|Events",
		meta = (DisplayName = "On Joy-Cons Separated", ToolTip = "Fires after a joined pair becomes two standalone horizontal controllers. Both controller infos already contain the new grip and player assignments."))
	FJSL4UJoyConsPairingDelegate OnJoyConsSeparated;

	/**
	 * Fires when a controller function's output is being rejected while the controller's input keeps
	 * arriving -- the observable signature of another application (Steam Input, typically) holding the
	 * controller's output path. Fires once per function per failure episode: a later successful write of
	 * that function re-arms it, so a recurring conflict fires again rather than spamming every frame.
	 *
	 * Detection is per attempted write, so this only fires when the game actually uses the function (e.g.
	 * the first rumble request while Steam holds the controller), and only for failures the OS reports --
	 * an application that silently swallows output without failing the write is not detectable here.
	 */
	UPROPERTY(BlueprintAssignable, Category = "JoyShock Library|Events",
		meta = (DisplayName = "On Controller Function Blocked", ToolTip = "Fires when a controller function (rumble, player LEDs, HOME light, motion configuration) is rejected while input keeps flowing -- usually another application such as Steam holding the controller. Fires once per function until that function works again."))
	FJSL4UControllerFunctionBlockedDelegate OnControllerFunctionBlocked;

	/**
	 * Calls Event once for every controller that is already connected, then keeps calling it for every
	 * controller that connects later. Prefer this to binding OnControllerConnected directly.
	 *
	 * Controllers finish enumerating on a background thread, at a time nothing in the game controls: in the
	 * editor they are usually ready before the level loads, in a packaged game they usually are not, and a
	 * Bluetooth controller may arrive minutes in. So "connected already" and "connects later" are not two
	 * cases a caller can tell apart, and handling only one of them produces a game that works on the machine
	 * it was written on. Binding and replaying in a single call removes the distinction: there is no window
	 * in which a controller can be missed, and no ordering for the caller to get right.
	 *
	 * Safe to call more than once with the same event -- the binding is not duplicated, though the event will
	 * be called again for controllers already connected.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Events",
		meta = (DisplayName = "Listen For Controllers", ToolTip = "Atomically binds Event, immediately calls it once for every connected controller, then calls it for future connections. Use this instead of combining an event binding with Get Connected Controllers."))
	void ListenForControllers(FJSL4UControllerInfoConnectedSignature Event);

	/**
	 * Atomically subscribes to the complete controller lifecycle. Existing controllers are replayed to
	 * On Connected before this function returns; later connections and disconnections are delivered on
	 * the game thread. Calling this again with the same events does not duplicate either binding.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Events",
		meta = (DisplayName = "Listen For Controller Changes", ToolTip = "Subscribes to connections and disconnections in one call. Existing controllers are immediately replayed to On Connected, then both events remain active for hot-plug changes."))
	void ListenForControllerChanges(FJSL4UControllerInfoConnectedSignature OnConnected,
		FJSL4UControllerInfoDisconnectedSignature OnDisconnected);

	/**
	 * Subscribes to future Joy-Con join/separate transitions. Unlike controller connections there is no
	 * replay: these are actions, not persistent discoveries. Query Get Connected Controllers when the
	 * current pairing state is also needed during setup.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Events",
		meta = (DisplayName = "Listen For Joy-Con Pairing Changes", ToolTip = "Subscribes to future Joy-Con joins and separations. Each event receives the left and right controller infos after the new pairing and player assignment have been applied."))
	void ListenForJoyConPairingChanges(FJSL4UJoyConsPairingSignature OnJoined,
		FJSL4UJoyConsPairingSignature OnSeparated);

	// Finds the local player whose native platform user owns this controller. Takes the whole description
	// for the same reason its Ensure counterpart does: the answer comes from the Platform User, which every
	// controller Unreal accepts has, so this reads an Xbox pad as readily as a DualSense.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Local Multiplayer",
		meta = (ToolTip = "Returns the Player Controller whose Local Player owns this controller's native Platform User, or None when no matching Local Player exists. Works for every controller Unreal accepts."))
	APlayerController* FindLocalPlayerForController(const FJSL4UControllerInfo& Controller) const;

	/**
	 * Finds that local player, or creates it with Unreal's FPlatformUserId overload when requested. The
	 * controller is then assigned back to the resulting local-player index, repairing mapper changes made
	 * by Create Local Player. This does not spawn or possess a Pawn and does not add mapping contexts.
	 *
	 * Takes the whole controller description rather than a bare id, and that is the point: a player is
	 * found by Platform User, which every controller Unreal accepts has, while only the ones this plugin
	 * drives are ours. Feeding this the payload from Wait For Any Controller Changes therefore
	 * seats an Xbox pad and a DualSense through the same node, with no branch on which kind arrived -- the
	 * assignment step that only means something for ours is skipped from in here, where the caller cannot
	 * forget it. Naming controllers per family was what made a game write one path per family.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Local Multiplayer",
		meta = (ExpandBoolAsExecs = "ReturnValue", CPP_Default_bCreateIfMissing = "true", ToolTip = "Finds the Local Player for this controller's Platform User, optionally creates it, then reconciles controller assignment. Works for every controller Unreal accepts, including XInput pads this plugin does not drive. It does not spawn a Pawn or add an Input Mapping Context."))
	bool EnsureLocalPlayerForController(const FJSL4UControllerInfo& Controller, bool bCreateIfMissing,
		APlayerController*& PlayerController, int32& LocalPlayerIndex, bool& bWasCreated);

	// Stops rumble on every connected controller. Called automatically when the game shuts down -- rumble
	// is sustained by threads that outlive the game, so a controller left rumbling would never stop on its
	// own. Also useful on its own (pausing, a cutscene, losing focus).
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Output",
		meta = (DisplayName = "Stop All Controller Rumble", ToolTip = "Stops direct and sustained rumble on every connected JoyShock controller. This also runs automatically when the Game Instance shuts down."))
	void StopAllControllerRumble();

private:
	FDelegateHandle ConnectedHandle;
	FDelegateHandle DisconnectedHandle;
	FDelegateHandle PairingChangedHandle;
	FDelegateHandle FunctionBlockedHandle;
	TMap<int32, FJSL4UControllerInfo> LastControllerInfoByDeviceId;
};
