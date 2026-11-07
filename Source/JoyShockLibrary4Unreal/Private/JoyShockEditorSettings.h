// The plugin's page in Editor Preferences (Plugins > JoyShockLibrary4Unreal).

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "JoyShockEditorSettings.generated.h"

// Stored per user (Saved/Config, never committed): whether one person's controllers are free for another
// program is not a property of the project. Read only by the editor -- a Standalone game or a packaged build
// is a different process with GIsEditor false, and always looks for controllers.
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "JoyShockLibrary4Unreal"))
class UJSL4UEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/**
	 * Find and connect PlayStation and Nintendo controllers while the editor is open, Play In Editor included.
	 * Turn it off to use your controllers in another program, such as Unity, with this editor open: the
	 * controllers already connected are let go at once, Bluetooth links included, and nothing new is
	 * connected until it is back on. Standalone games and packaged builds always connect controllers.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Controllers", meta = (DisplayName = "Connect Controllers in the Editor"))
	bool bConnectControllersInEditor = true;

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
