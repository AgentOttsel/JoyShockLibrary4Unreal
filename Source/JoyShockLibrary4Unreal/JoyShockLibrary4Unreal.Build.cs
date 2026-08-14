// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System;
using System.IO;

public class JoyShockLibrary4Unreal : ModuleRules
{
	private string ThirdPartyPath => Path.GetFullPath(Path.Combine(ModuleDirectory, "../../ThirdParty/"));

	// The C++/WinRT projection headers, which the Switch 2 Bluetooth transport is written against. They
	// ship with the Windows SDK but are not on Unreal's include path, and they live under a version folder
	// that has nothing to do with the SDK version Unreal itself compiles against -- so the newest installed
	// one is picked here rather than assumed. Returns null when no SDK on this machine has them, which
	// leaves the plugin building fine with its Bluetooth transport compiled out.
	private static string FindCppWinRtIncludePath()
	{
		string[] Roots =
		{
			Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Windows Kits", "10", "Include"),
			Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Windows Kits", "10", "Include"),
		};

		string Best = null;
		Version BestVersion = null;
		foreach (string Root in Roots)
		{
			if (!Directory.Exists(Root))
			{
				continue;
			}
			foreach (string VersionDirectory in Directory.GetDirectories(Root))
			{
				string Candidate = Path.Combine(VersionDirectory, "cppwinrt");
				if (!Directory.Exists(Candidate))
				{
					continue;
				}

				Version ParsedVersion;
				if (!Version.TryParse(Path.GetFileName(VersionDirectory), out ParsedVersion))
				{
					continue;
				}
				if (BestVersion == null || ParsedVersion > BestVersion)
				{
					BestVersion = ParsedVersion;
					Best = Candidate;
				}
			}
		}
		return Best;
	}

	public JoyShockLibrary4Unreal(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Stated rather than inherited, because what a module inherits is the TARGET's standard -- and a
		// project whose .Target.cs still has DefaultBuildSettings below V4 gets C++17 there, which UE 5.8
		// refuses outright ("CppStandardVersion.Cpp17 is no longer supported"). That is a setting in the
		// game's target, not in this plugin, but it is this plugin's name in the error, and a plugin should
		// not stop building because of how old the project around it is. C++/WinRT needs C++17 at minimum
		// anyway, and the engine needs C++20.
		CppStandard = CppStandardVersion.Cpp20;

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

			// WinUSB + SetupAPI + CfgMgr: used to send the Nintendo Switch 2 Pro Controller its init
			// commands over its WinUSB (bulk) interface (its HID interface is input-only), and to match
			// that WinUSB interface to the HID interface of the same physical unit via the device tree.
			PublicSystemLibraries.AddRange(new string[] { "setupapi.lib", "winusb.lib", "cfgmgr32.lib" });

			// WinRT, for the Switch 2 Bluetooth transport (Switch2Bluetooth.cpp). Those controllers are BLE
			// GATT peripherals rather than Bluetooth HID devices, and WinRT is the only Windows API that can
			// scan for and connect to one that is not paired through Windows itself -- which is exactly the
			// state a Switch 2 controller is in, since it has no HID profile for Windows to pair with.
			string CppWinRtPath = FindCppWinRtIncludePath();
			if (CppWinRtPath != null)
			{
				PrivateIncludePaths.Add(CppWinRtPath);
				PublicSystemLibraries.Add("windowsapp.lib");

				// C++/WinRT reports every failure by throwing, so its blocking calls cannot be used without
				// exceptions enabled. They are confined to Switch2Bluetooth.cpp, which catches at its
				// boundary and returns failure codes to the rest of the plugin.
				bEnableExceptions = true;

				PublicDefinitions.Add("JSL_SWITCH2_BLUETOOTH=1");
			}
			else
			{
				System.Console.WriteLine(
					"JoyShockLibrary4Unreal: no Windows SDK with the C++/WinRT headers was found, so Switch 2 " +
					"controllers will only work over USB. Install a Windows 10/11 SDK to enable Bluetooth.");
				PublicDefinitions.Add("JSL_SWITCH2_BLUETOOTH=0");
			}
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
			PublicDefinitions.Add("JSL_SWITCH2_BLUETOOTH=0");
		}
	}
}
