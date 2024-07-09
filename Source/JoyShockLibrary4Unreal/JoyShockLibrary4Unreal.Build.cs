// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class JoyShockLibrary4Unreal : ModuleRules
{
	public JoyShockLibrary4Unreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[]
			{
				// "JoyShockLibrary4Unreal/JoyShockLibrary/hidapi"
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
				// "JoyShockLibrary4Unreal/JoyShockLibrary/hidapi"
			}
		);
			
		
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"ApplicationCore",
				"Core",
				"DeveloperSettings",
				"InputDevice",
				// ... add other public dependencies that you statically link with here ...
			}
		);
			
		
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"ApplicationCore",
				"CoreUObject",
				"Engine",
				"HIDUE",
				"InputCore",
				"Slate",
				"SlateCore",
				// ... add private dependencies that you statically link with here ...	
			}
		);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
		);

		/*if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// string BuildString = (Target.Configuration != UnrealTargetConfiguration.Debug) ? "Release" : "Release";
			PublicAdditionalLibraries.Add(Path.Combine(ModuleDirectory, "JoyShockLibrary", "hidapi", "x64", "hidapi.lib"));

			PublicDelayLoadDLLs.Add("hidapi.dll");
			RuntimeDependencies.Add(Path.Combine(ModuleDirectory, "JoyShockLibrary", "hidapi", "x64", "hidapi.dll"));
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			
		}*/
	}
}
