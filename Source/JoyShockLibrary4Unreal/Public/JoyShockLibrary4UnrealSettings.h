#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "JoyShockLibrary4UnrealSettings.generated.h"

DECLARE_MULTICAST_DELEGATE(FJSL4USettingsChangedDelegate);

/**
 * 
 */
UCLASS(Config=JSL4U, DefaultConfig)
class JOYSHOCKLIBRARY4UNREAL_API UJoyShockLibrary4UnrealSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	// UDeveloperSettings overrides
	virtual FName GetContainerName() const override { return FName("Project"); }
	virtual FName GetCategoryName() const override { return FName("Plugins"); }
	virtual FName GetSectionName() const override { return FName("JoyShockLibrary4Unreal"); }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	FORCEINLINE FJSL4USettingsChangedDelegate& GetOnSettingsChanged() { return OnSettingsChanged; }
#endif
	
	/* TODO: Add support for these settings in the future:
	// The original JoyShockLibrary returns motion-related values in a Right-handed Y-Up format.
	// In order to match Unreal's coordinate system, JSL4U converts these values to Left-handed Z-Up.
	// You should only enable this if you'd rather use the original coordinate system and do any conversions yourself.
	UPROPERTY(Config, EditAnywhere, Category="JoyShockLibrary4Unreal|Settings")
	bool bKeepRightHandedYUpCoordinates = false;

	// JoyShockLibrary uses a separate thread to check for input updates. This can result in a higher poll rate than the
	// engine's Tick function, which is good for more precise motion accumulation. However, it can cause issues if your
	// game's code expects to only ever receive one input update per tick.
	// Tick this option if you'd like JSL4U to wait for an Engine Tick before issuing any input events, like XInput does.
	// Motion values will still be accumulated on a separate thread, so you don't have to worry about losing precision.
	UPROPERTY(Config, EditAnywhere, Category="JoyShockLibrary4Unreal|Settings")
	bool bControllerEventsWaitForEngineTick = true;

	UPROPERTY(Config, EditAnywhere, Category="JoyShockLibrary4Unreal|Settings")
	TArray<FColor> ConnectedControllerColors = {}; */

protected:
#if WITH_EDITOR
	FJSL4USettingsChangedDelegate OnSettingsChanged;
#endif
};
