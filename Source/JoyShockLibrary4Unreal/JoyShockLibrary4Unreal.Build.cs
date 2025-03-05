// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class JoyShockLibrary4Unreal : ModuleRules
{
	private string ThirdPartyPath => Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/"));

	public JoyShockLibrary4Unreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[]
			{
				// "JoyShockLibrary4Unreal/JoyShockLibrary/hidapi"
				Path.Combine(ThirdPartyPath, "hidapi")
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
				// "HIDUE",
				"InputCore",
				"Projects",
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

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// string BuildString = (Target.Configuration != UnrealTargetConfiguration.Debug) ? "Release" : "Release";
			string x64Path = Path.Combine(ThirdPartyPath, "hidapi", "x64");
			PublicAdditionalLibraries.Add(Path.Combine(x64Path, "hidapi.lib"));

			PublicDelayLoadDLLs.Add("hidapi.dll");
			RuntimeDependencies.Add(Path.Combine(x64Path, "hidapi.dll"));
		}
		/*else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			// TODO: Add DLL equivalent for Linux
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			// TODO: Add DLL equivalent for Mac
		}*/
		else // Fallback to HIDUE plugin (get source from https://github.com/microdee/HIDUE)
		{
			PrivateDependencyModuleNames.Add("HIDUE");
		}
	}
}
