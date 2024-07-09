// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IInputDeviceModule.h"
#include "JoyShockLibrary4Unreal/JoyShockLibrary/JoyShockLibrary.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsApplication.h"
#endif

DECLARE_DELEGATE_OneParam(FJoyShockConnectedDelegate, int32);
DECLARE_DELEGATE_TwoParams(FJoyShockDisconnectedDelegate, int32, bool);
DECLARE_DELEGATE_SixParams(FJoyShockPollDelegate, int32, FJoyShockState, FJoyShockState, FIMUState, FIMUState, float);
DECLARE_DELEGATE_FourParams(FJoyShockPollTouchDelegate, int32, FTouchState, FTouchState, float);


#define JoyShockLockedBindLambda(Module, Delegate, Lambda) { \
	(Module)._callbackLock.lock(); \
	(Module).Delegate.BindLambda(Lambda); \
	(Module)._callbackLock.unlock(); }

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
	static FORCEINLINE FJoyShockLibrary4UnrealModule& GetInstance()
	{
		return FModuleManager::LoadModuleChecked<FJoyShockLibrary4UnrealModule>(TEXT("JoyShockLibrary4Unreal"));
	}
	
	static FORCEINLINE bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("JoyShockLibrary4Unreal"));
	}

	// IModuleInterface implementation
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// IInputDeviceModule implementation
	virtual TSharedPtr<IInputDevice> CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override;

	FORCEINLINE FJoyShockConnectedDelegate& GetOnConnected() { return OnConnected; }
	FORCEINLINE FJoyShockDisconnectedDelegate& GetOnDisconnected() { return OnDisconnected; }
	FORCEINLINE FJoyShockPollDelegate& GetOnPoll() { return OnPoll; }
	FORCEINLINE FJoyShockPollTouchDelegate& GetOnPollTouch() { return OnPollTouch; }
	
	std::shared_timed_mutex _callbackLock;
	std::shared_timed_mutex _connectedLock;
	
protected:
	FJoyShockConnectedDelegate OnConnected;
	FJoyShockDisconnectedDelegate OnDisconnected;
	FJoyShockPollDelegate OnPoll;
	FJoyShockPollTouchDelegate OnPollTouch;

#if PLATFORM_WINDOWS
	FWindowsDeviceChangeMessageHandler WindowsDeviceChangeMessageHandler;
#endif
	
};
