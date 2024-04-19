// Copyright Epic Games, Inc. All Rights Reserved.

#include "JoyShockLibrary4Unreal.h"

#define LOCTEXT_NAMESPACE "FJoyShockLibrary4UnrealModule"

void FJoyShockLibrary4UnrealModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
}

void FJoyShockLibrary4UnrealModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
}

TSharedPtr<IInputDevice> FJoyShockLibrary4UnrealModule::CreateInputDevice(
	const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
{
	return nullptr;
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FJoyShockLibrary4UnrealModule, JoyShockLibrary4Unreal)