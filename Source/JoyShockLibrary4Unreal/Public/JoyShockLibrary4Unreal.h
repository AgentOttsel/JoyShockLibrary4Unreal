// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IInputDeviceModule.h"
#include "JoyShockTypes.h"
#include "Modules/ModuleManager.h"
#include <atomic>
#include <shared_mutex>

#if PLATFORM_WINDOWS
#include "Windows/WindowsApplication.h"
#endif

DECLARE_DELEGATE_OneParam(FJoyShockConnectedDelegate, int32);
DECLARE_DELEGATE_TwoParams(FJoyShockDisconnectedDelegate, int32, bool);
DECLARE_DELEGATE_SixParams(FJoyShockPollDelegate, int32, FJoyShockState, FJoyShockState, FIMUState, FIMUState, float);
DECLARE_DELEGATE_FourParams(FJoyShockPollTouchDelegate, int32, FTouchState, FTouchState, float);
DECLARE_DELEGATE_TwoParams(FJoyShockFunctionBlockedDelegate, int32, EJSL4UControllerFunction);

// Fired on the game thread once a device has been fully added to / removed from the input device
// interface, so listeners can safely call the JSL4U* API from them (e.g. to rebuild a controller list).
// These are multicast, unlike the single-cast OnConnected/OnDisconnected above: those are the transport
// from the background threads and the interface itself consumes them, so nothing else can bind to them.
DECLARE_MULTICAST_DELEGATE_OneParam(FJSL4UDeviceConnectedEvent, int32 /*DeviceId*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FJSL4UDeviceDisconnectedEvent, int32 /*DeviceId*/, bool /*bTimedOut*/);
DECLARE_MULTICAST_DELEGATE_ThreeParams(FJSL4UJoyConPairingChangedEvent,
	int32 /*LeftDeviceId*/, int32 /*RightDeviceId*/, bool /*bJoined*/);
// Fired on the game thread when a controller function's output failed while its input kept flowing --
// the observable signature of another application (Steam Input, typically) holding the controller.
DECLARE_MULTICAST_DELEGATE_TwoParams(FJSL4UDeviceFunctionBlockedEvent,
	int32 /*DeviceId*/, EJSL4UControllerFunction /*Function*/);


#define JoyShockLockedBindLambda(Module, Delegate, Lambda) { \
	(Module)._callbackLock.lock(); \
	(Module).Delegate.BindLambda(Lambda); \
	(Module)._callbackLock.unlock(); }

class FJoyShockInterface;

#if PLATFORM_WINDOWS
class FWindowsDeviceChangeMessageHandler : public IWindowsMessageHandler
{
public:
	virtual bool ProcessMessage(HWND hwnd, uint32 msg, WPARAM wParam, LPARAM lParam, int32& OutResult) override;
};
#endif

class JOYSHOCKLIBRARY4UNREAL_API FJoyShockLibrary4UnrealModule : public IInputDeviceModule
{
public:
	// The module itself, for everything in the plugin that needs to reach it. Answered from a pointer the
	// module publishes at startup rather than from FModuleManager -- see the definition for why that
	// difference matters to the threads. Safe from any thread for as long as the module is alive.
	static FJoyShockLibrary4UnrealModule& GetInstance();
	
	static FORCEINLINE bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("JoyShockLibrary4Unreal"));
	}

	// IModuleInterface implementation
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// IInputDeviceModule implementation
	virtual TSharedPtr<IInputDevice> CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override;

	/**
	 * Kicks off device (re)enumeration on a background thread. Enumerating and initialising HID devices
	 * performs blocking I/O and thread joins, so it must never run on the game thread (doing so freezes
	 * the editor when controllers connect/disconnect). Repeated requests while one is running are
	 * coalesced into a single follow-up pass.
	 */
	void RequestConnectDevices();

	FORCEINLINE FJoyShockConnectedDelegate& GetOnConnected() { return OnConnected; }
	FORCEINLINE FJoyShockDisconnectedDelegate& GetOnDisconnected() { return OnDisconnected; }
	FORCEINLINE FJoyShockPollDelegate& GetOnPoll() { return OnPoll; }
	FORCEINLINE FJoyShockPollTouchDelegate& GetOnPollTouch() { return OnPollTouch; }
	FORCEINLINE FJoyShockFunctionBlockedDelegate& GetOnFunctionBlocked() { return OnFunctionBlocked; }

	// Game-thread device add/remove events. UJoyShockSubsystem re-exposes these to Blueprint; bind here
	// from C++ if you need them before/without a GameInstance.
	FORCEINLINE FJSL4UDeviceConnectedEvent& GetOnDeviceConnected() { return OnDeviceConnected; }
	FORCEINLINE FJSL4UDeviceDisconnectedEvent& GetOnDeviceDisconnected() { return OnDeviceDisconnected; }
	FORCEINLINE FJSL4UJoyConPairingChangedEvent& GetOnJoyConPairingChanged() { return OnJoyConPairingChanged; }
	FORCEINLINE FJSL4UDeviceFunctionBlockedEvent& GetOnDeviceFunctionBlocked() { return OnDeviceFunctionBlocked; }

	// The live input-device interface owns the controller state, player-slot assignment and Joy-Con joining.
	// It registers itself here on creation so the Blueprint pairing API can reach it. May be null before the
	// input device is created or after it's destroyed.
	void SetActiveInterface(FJoyShockInterface* InInterface) { ActiveInterface = InInterface; }
	FJoyShockInterface* GetActiveInterface() const { return ActiveInterface; }

	std::shared_timed_mutex _callbackLock;
	std::shared_timed_mutex _connectedLock;

protected:
	FJoyShockConnectedDelegate OnConnected;
	FJoyShockDisconnectedDelegate OnDisconnected;
	FJoyShockPollDelegate OnPoll;
	FJoyShockPollTouchDelegate OnPollTouch;
	FJoyShockFunctionBlockedDelegate OnFunctionBlocked;

	FJSL4UDeviceConnectedEvent OnDeviceConnected;
	FJSL4UDeviceDisconnectedEvent OnDeviceDisconnected;
	FJSL4UJoyConPairingChangedEvent OnJoyConPairingChanged;
	FJSL4UDeviceFunctionBlockedEvent OnDeviceFunctionBlocked;

#if PLATFORM_WINDOWS
	FWindowsDeviceChangeMessageHandler WindowsDeviceChangeMessageHandler;
#endif

private:
	// State for the coalesced background enumeration driven by RequestConnectDevices().
	std::atomic<bool> bConnectRunning{ false };
	std::atomic<bool> bConnectQueued{ false };
	std::atomic<bool> bShuttingDown{ false };

	FJoyShockInterface* ActiveInterface = nullptr;
};
