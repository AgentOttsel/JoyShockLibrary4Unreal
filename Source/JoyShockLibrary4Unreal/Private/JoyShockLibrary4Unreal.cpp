// Copyright Epic Games, Inc. All Rights Reserved.

#include "JoyShockLibrary4Unreal.h"

#include "JoyShockInterface.h"

#define LOCTEXT_NAMESPACE "FJoyShockLibrary4UnrealModule"

#if PLATFORM_WINDOWS
bool FWindowsDeviceChangeMessageHandler::ProcessMessage(HWND hwnd, uint32 msg, WPARAM wParam, LPARAM lParam,
	int32& OutResult)
{
	if (msg == WM_DEVICECHANGE)
	{
		// UE_LOG(LogTemp, Log, TEXT(">>>>>CONNECTING DEVICES TO JoyShockLibrary"));
		UJoyShockLibrary::JslConnectDevices();
	}

	return false;
}
#endif

void FJoyShockLibrary4UnrealModule::StartupModule()
{
	IInputDeviceModule::StartupModule();

	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<GenericApplication> Application = FSlateApplication::Get().GetPlatformApplication();

		if (Application.IsValid())
		{
#if PLATFORM_WINDOWS
			FWindowsApplication* WindowsApplication = static_cast<FWindowsApplication*>(Application.Get());
			WindowsApplication->AddMessageHandler(WindowsDeviceChangeMessageHandler);
#endif
			// TODO: Add Linux and macOS support
		}
	}
}

void FJoyShockLibrary4UnrealModule::ShutdownModule()
{
	// Application is already invalid at this point, so there's no message handler to remove.
	/*if (FSlateApplication::IsInitialized())
	{
		UE_LOG(LogTemp, Log, TEXT(">>>>>SHUTTING DOWN JoyShockLibrary MODULE"));
		if (TSharedPtr<GenericApplication> Application = FSlateApplication::Get().GetPlatformApplication())
		{
			if (Application.IsValid())
			{
#if PLATFORM_WINDOWS
				FWindowsApplication* WindowsApplication = static_cast<FWindowsApplication*>(Application.Get());
				WindowsApplication->RemoveMessageHandler(WindowsDeviceChangeMessageHandler);
#endif
			}
		}
	}*/

	IInputDeviceModule::ShutdownModule();
}

TSharedPtr<IInputDevice> FJoyShockLibrary4UnrealModule::CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
{
	return FJoyShockInterface::Create(InMessageHandler);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FJoyShockLibrary4UnrealModule, JoyShockLibrary4Unreal)