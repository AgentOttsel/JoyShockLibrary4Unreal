// Copyright Epic Games, Inc. All Rights Reserved.

#include "JoyShockSubsystem.h"

#include "JoyShockLibrary4Unreal.h"
#include "JoyShockBlueprintLibrary.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"

void UJoyShockSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (!FJoyShockLibrary4UnrealModule::IsAvailable())
	{
		// Without this the events would just never fire, with nothing to explain why.
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("UJoyShockSubsystem: the JoyShockLibrary4Unreal module is not loaded, so controller events will not fire."));
		return;
	}

	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	// Weak lambdas so a GC'd subsystem can never be broadcast into, even if Deinitialize is skipped.
	// These carry the library handle, because they are raised by the library side. The handle goes no
	// further than this file: it keys the cache below and is turned into a description immediately.
	ConnectedHandle = JSL4UModule.GetOnDeviceConnected().AddWeakLambda(this, [this](int32 DeviceId)
	{
		const FJSL4UControllerInfo Info = UJoyShockLibrary::GetControllerInfoForHandle(DeviceId);
		LastControllerInfoByDeviceId.Add(DeviceId, Info);
		OnControllerConnected.Broadcast(Info);
	});

	DisconnectedHandle = JSL4UModule.GetOnDeviceDisconnected().AddWeakLambda(this, [this](int32 DeviceId, bool bTimedOut)
	{
		// The cache is the only description left: by now the controller is gone from both the library and
		// the interface, so there is nothing to look its identity up in. A controller that was never cached
		// leaves as an empty description rather than one carrying an id that no longer means anything.
		FJSL4UControllerInfo Info;
		if (const FJSL4UControllerInfo* CachedInfo = LastControllerInfoByDeviceId.Find(DeviceId))
		{
			Info = *CachedInfo;
		}
		Info.bIsConnected = false;
		OnControllerDisconnected.Broadcast(Info, bTimedOut);
		LastControllerInfoByDeviceId.Remove(DeviceId);
	});

	PairingChangedHandle = JSL4UModule.GetOnJoyConPairingChanged().AddWeakLambda(this,
		[this](int32 LeftDeviceId, int32 RightDeviceId, bool bJoined)
		{
			const FJSL4UControllerInfo LeftInfo =
				UJoyShockLibrary::GetControllerInfoForHandle(LeftDeviceId);
			const FJSL4UControllerInfo RightInfo =
				UJoyShockLibrary::GetControllerInfoForHandle(RightDeviceId);
			LastControllerInfoByDeviceId.Add(LeftDeviceId, LeftInfo);
			LastControllerInfoByDeviceId.Add(RightDeviceId, RightInfo);

			if (bJoined)
			{
				OnJoyConsJoined.Broadcast(LeftInfo, RightInfo);
			}
			else
			{
				OnJoyConsSeparated.Broadcast(LeftInfo, RightInfo);
			}
		});

	FunctionBlockedHandle = JSL4UModule.GetOnDeviceFunctionBlocked().AddWeakLambda(this,
		[this](int32 DeviceId, EJSL4UControllerFunction Function)
		{
			const FJSL4UControllerInfo Info = UJoyShockLibrary::GetControllerInfoForHandle(DeviceId);
			OnControllerFunctionBlocked.Broadcast(Info, Function);
		});

	// Controllers already connected when this subsystem is created never produced an event for it to hear,
	// so seed the cache on Init and use the events only for changes after that. Seeded from the handles
	// rather than from the public listing because the cache is keyed by handle -- that is what the
	// disconnect event will arrive with, and it is the one place in the plugin where the two must line up.
	TArray<int32> ExistingHandles;
	UJoyShockLibrary::GetConnectedDeviceHandles(ExistingHandles);
	int32 SeededCount = 0;
	for (int32 Handle : ExistingHandles)
	{
		const FJSL4UControllerInfo Info = UJoyShockLibrary::GetControllerInfoForHandle(Handle);
		if (Info.bIsConnected)
		{
			LastControllerInfoByDeviceId.Add(Handle, Info);
			++SeededCount;
		}
	}
	UE_LOG(LogJoyShockLibrary, Verbose, TEXT("UJoyShockSubsystem: listening for device changes (%d controller(s) already connected)."),
		SeededCount);
}

void UJoyShockSubsystem::ListenForControllers(FJSL4UControllerInfoConnectedSignature Event)
{
	if (!Event.IsBound())
	{
		return;
	}

	OnControllerConnected.AddUnique(Event);
	for (const FJSL4UControllerInfo& Info : UJoyShockLibrary::JSL4UGetConnectedControllers())
	{
		// No cache write here: Initialize seeded every controller that was already connected, and the
		// connect event caches every one that arrives later, so this replay has nothing to add.
		Event.Execute(Info);
	}
}

void UJoyShockSubsystem::ListenForControllerChanges(FJSL4UControllerInfoConnectedSignature OnConnected,
	FJSL4UControllerInfoDisconnectedSignature OnDisconnected)
{
	if (OnDisconnected.IsBound())
	{
		OnControllerDisconnected.AddUnique(OnDisconnected);
	}

	// Reuse the atomic bind-and-replay implementation so a connection cannot fall into a gap between
	// enumerating existing devices and subscribing for future ones.
	ListenForControllers(OnConnected);
}

void UJoyShockSubsystem::ListenForJoyConPairingChanges(FJSL4UJoyConsPairingSignature OnJoined,
	FJSL4UJoyConsPairingSignature OnSeparated)
{
	if (OnJoined.IsBound())
	{
		OnJoyConsJoined.AddUnique(OnJoined);
	}
	if (OnSeparated.IsBound())
	{
		OnJoyConsSeparated.AddUnique(OnSeparated);
	}
}

APlayerController* UJoyShockSubsystem::FindLocalPlayerForController(const FJSL4UControllerInfo& Controller) const
{
	// Same freshness rule as EnsureLocalPlayerForController: re-read one of ours, trust the payload for a
	// controller we do not drive, because for that one the payload is the only description there is.
	const FJSL4UControllerInfo Info = Controller.bIsJoyShockController
		? UJoyShockLibrary::JSL4UGetControllerInfo(Controller.ConnectionId)
		: Controller;

	const UGameInstance* GameInstance = GetGameInstance();
	if (!Info.bIsConnected || Info.PlatformUserId < 0 || GameInstance == nullptr)
	{
		return nullptr;
	}

	const FPlatformUserId PlatformUser = FPlatformUserId::CreateFromInternalId(Info.PlatformUserId);
	for (const ULocalPlayer* LocalPlayer : GameInstance->GetLocalPlayers())
	{
		if (LocalPlayer != nullptr && LocalPlayer->GetPlatformUserId() == PlatformUser)
		{
			return LocalPlayer->GetPlayerController(GetWorld());
		}
	}
	return nullptr;
}

bool UJoyShockSubsystem::EnsureLocalPlayerForController(const FJSL4UControllerInfo& Controller,
	bool bCreateIfMissing, APlayerController*& PlayerController, int32& LocalPlayerIndex, bool& bWasCreated)
{
	PlayerController = nullptr;
	LocalPlayerIndex = INDEX_NONE;
	bWasCreated = false;

	// For one of ours, re-read the live description instead of trusting what was handed in: a caller can
	// be holding an info from an earlier event, and the platform user is exactly the field that moves
	// underneath it. For a controller we do not drive there is nothing to re-read -- the payload from
	// Wait For Any Controller Changes is the only description of it that exists.
	const FJSL4UControllerInfo Info = Controller.bIsJoyShockController
		? UJoyShockLibrary::JSL4UGetControllerInfo(Controller.ConnectionId)
		: Controller;

	// Every way out of this function says why, because the caller cannot tell them apart and the
	// consequence of any of them is the same thing from the player's seat: a controller that is visibly
	// connected, whose motion readouts work, driving no character at all. A game typically calls this once,
	// when the controller arrives, so a single bad moment is permanent -- and silence about which moment it
	// was is what made it unfixable from a bug report.
	UGameInstance* GameInstance = GetGameInstance();
	if (!Info.bIsConnected || Info.PlatformUserId < 0 || GameInstance == nullptr)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("EnsureLocalPlayerForController: nothing to seat for connection %lld ")
			TEXT("(connected %d, platform user %d, game instance %d). A controller that had not finished ")
			TEXT("arriving when this was called reads exactly like this."),
			Info.ConnectionId, Info.bIsConnected ? 1 : 0, Info.PlatformUserId, GameInstance != nullptr ? 1 : 0);
		return false;
	}

	const FPlatformUserId PlatformUser = FPlatformUserId::CreateFromInternalId(Info.PlatformUserId);
	ULocalPlayer* LocalPlayer = nullptr;
	const TArray<ULocalPlayer*>& LocalPlayers = GameInstance->GetLocalPlayers();
	for (int32 Index = 0; Index < LocalPlayers.Num(); ++Index)
	{
		if (LocalPlayers[Index] != nullptr && LocalPlayers[Index]->GetPlatformUserId() == PlatformUser)
		{
			LocalPlayer = LocalPlayers[Index];
			LocalPlayerIndex = Index;
			break;
		}
	}

	if (LocalPlayer == nullptr && bCreateIfMissing)
	{
		FString Error;
		LocalPlayer = GameInstance->CreateLocalPlayer(PlatformUser, Error, true);
		if (LocalPlayer == nullptr)
		{
			UE_LOG(LogJoyShockLibrary, Warning,
				TEXT("EnsureLocalPlayerForController failed for connection %lld / platform user %d: %s"),
				Info.ConnectionId, Info.PlatformUserId, *Error);
			return false;
		}
		bWasCreated = true;
		LocalPlayerIndex = GameInstance->GetLocalPlayers().IndexOfByKey(LocalPlayer);
	}

	if (LocalPlayer == nullptr || LocalPlayerIndex == INDEX_NONE)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("EnsureLocalPlayerForController: connection %lld has no local player for platform user %d, ")
			TEXT("and %s."),
			Info.ConnectionId, Info.PlatformUserId,
			bCreateIfMissing ? TEXT("creating one did not produce an index") : TEXT("was told not to create one"));
		return false;
	}

	// CreateLocalPlayer can remap an existing FInputDeviceId while synthesising its legacy controller id.
	// Assigning through the interface immediately reconciles every JoyShock device with the mapper again.
	//
	// Only ours needs this, and only ours may have it done to it. The local player above was found BY the
	// controller's platform user, so a controller we do not drive is already sitting on exactly the user
	// this would map it to -- and LocalPlayerIndex is an index into the local-player array, which is not
	// the same number as a platform user index the moment those two lists diverge. Re-mapping a foreign
	// device from that number is a no-op at best and moves it off its own player at worst. Skipping here
	// rather than at the call site is what lets one Blueprint path seat every controller.
	if (Info.bIsJoyShockController && !UJoyShockLibrary::JSL4UAssignControllerToPlayerIndex(Info, LocalPlayerIndex))
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("EnsureLocalPlayerForController: connection %lld could not be assigned to local player %d ")
			TEXT("(the line above says why)."),
			Info.ConnectionId, LocalPlayerIndex);
		return false;
	}

	PlayerController = LocalPlayer->GetPlayerController(GetWorld());
	if (PlayerController == nullptr)
	{
		// The local player exists and owns no player controller in this world. Worth its own line because
		// it is the one failure that repeats: the local player is not torn down when the controller drops,
		// so unplugging and replugging finds this same player again and fails here again, which is what a
		// player experiences as a controller that never recovers.
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("EnsureLocalPlayerForController: local player %d (platform user %d, connection %lld) has no ")
			TEXT("player controller in this world. Reconnecting the controller will find this same local ")
			TEXT("player and fail here again."),
			LocalPlayerIndex, Info.PlatformUserId, Info.ConnectionId);
	}
	return PlayerController != nullptr;
}

void UJoyShockSubsystem::StopAllControllerRumble()
{
	// Setting the values is enough: the polling thread is the sole writer and sends one stop packet when it
	// sees them change to zero.
	for (const FJSL4UControllerInfo& Info : UJoyShockLibrary::JSL4UGetConnectedControllers())
	{
		UJoyShockLibrary::JSL4USetControllerRumble(Info.ConnectionId, 0.0f, 0.0f);
	}
}

void UJoyShockSubsystem::Deinitialize()
{
	// The module outlives the game instance, so leaving these bound would keep firing into a dead
	// subsystem for the rest of the session.
	if (FJoyShockLibrary4UnrealModule::IsAvailable())
	{
		FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
		JSL4UModule.GetOnDeviceConnected().Remove(ConnectedHandle);
		JSL4UModule.GetOnDeviceDisconnected().Remove(DisconnectedHandle);
		JSL4UModule.GetOnJoyConPairingChanged().Remove(PairingChangedHandle);
		JSL4UModule.GetOnDeviceFunctionBlocked().Remove(FunctionBlockedHandle);

		// Rumble is sustained by the polling threads, which belong to the module and keep running after the
		// game stops -- so a controller still rumbling when the game ends would rumble until something set
		// it back to zero, and nothing would. (This can't be left to the game: it may well be stopping
		// *because* it crashed.) Stopping in PIE is what makes this urgent; a packaged game exiting tears
		// the module down with it.
		StopAllControllerRumble();
	}

	ConnectedHandle.Reset();
	DisconnectedHandle.Reset();
	PairingChangedHandle.Reset();
	FunctionBlockedHandle.Reset();
	LastControllerInfoByDeviceId.Reset();

	Super::Deinitialize();
}
