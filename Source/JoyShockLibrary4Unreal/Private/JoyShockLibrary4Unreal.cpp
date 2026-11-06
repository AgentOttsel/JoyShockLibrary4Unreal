// Copyright Epic Games, Inc. All Rights Reserved.

#include "JoyShockLibrary4Unreal.h"
#include "JoyShockBlueprintLibrary.h"

#include "JoyShockInterface.h"
// For ShutdownAllDevices(), called by ShutdownModule below.
#include "JoyShockLibrary4Unreal/JoyShockLibrary/JoyShockLibrary.h"
#include "JoyShockLibrary4Unreal/JoyShockLibrary/Switch2Bluetooth.h"
#include "Interfaces/IPluginManager.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#define LOCTEXT_NAMESPACE "FJoyShockLibrary4UnrealModule"

// The live module: published by StartupModule, withdrawn only once ShutdownModule has finished tearing
// everything down.
//
// GetInstance answers from here rather than from FModuleManager because the manager stops answering too
// early for this plugin's threads. UnloadModule clears the module's ready flag BEFORE calling
// ShutdownModule, and off the game thread GetModule then returns null, so LoadModuleChecked fails its own
// check -- during exactly the window in which shutdown is waiting for the polling and enumeration threads
// to stop. Those threads are alive, the module they are calling into is alive, and the only thing that
// has changed is a flag meaning "do not hand this out to new callers".
//
// The module object outlives ShutdownModule (the manager destroys it after that returns), so a pointer
// held here is valid for every moment a thread of ours can still be running.
static std::atomic<FJoyShockLibrary4UnrealModule*> GLiveJoyShockModule{ nullptr };

// The thread StartupModule hands the Bluetooth scan to. ShutdownModule waits for it: otherwise a close during
// its blocking adapter query let it go on to install a discovery callback and start a scan after the
// Bluetooth teardown had already run, leaving a radio thread calling into a module that was gone.
static TFuture<void> GBluetoothStartup;

// How the editor learns that a game wants its Bluetooth controllers (see Switch2Ble::SetYieldedToGame).
//
// A game process holds a named mutex for as long as it runs, and the editor checks for it twice a second. A
// named object rather than the editor's own "Standalone launched" delegate: it covers any game with this
// plugin -- the Standalone the editor launched, or a packaged build started beside it -- and a runtime module
// needs nothing from the editor's modules to see it. "Local\" is the logon session, which the editor and
// every game it launches share. The object is never waited on; its existence is the whole message, and
// Windows removes it when the last game holding it exits, crash included.
static const TCHAR* const GBluetoothClaimName = TEXT("Local\\JoyShockLibrary4Unreal.GameWantsBluetooth");
#if PLATFORM_WINDOWS
static HANDLE GBluetoothClaim = nullptr;
#endif
static FTSTicker::FDelegateHandle GBluetoothClaimWatcher;
// Game thread only: the watcher's last answer, so it acts on changes rather than on every check.
static bool GBluetoothYieldedToGame = false;

static bool IsBluetoothClaimedByGame()
{
#if PLATFORM_WINDOWS
	if (HANDLE Claim = ::OpenMutexW(SYNCHRONIZE, 0, GBluetoothClaimName))
	{
		::CloseHandle(Claim);
		return true;
	}
#endif
	return false;
}

static void UpdateBluetoothClaim(FJoyShockLibrary4UnrealModule& Module)
{
	const bool bClaimed = IsBluetoothClaimedByGame();
	if (bClaimed == GBluetoothYieldedToGame)
	{
		return;
	}
	GBluetoothYieldedToGame = bClaimed;
	Switch2Ble::SetYieldedToGame(bClaimed);

	if (bClaimed)
	{
		UE_LOG(LogJoyShockLibrary, Log,
			TEXT("A game with this plugin is running on this PC: the editor is releasing its Switch 2 controllers on Bluetooth to it, leaves every controller's lights and rumble to it, and stops looking for more until it closes."));
		// The polling threads see the flag on their next report and retire their controllers; stopping the
		// scan here is what keeps the editor from taking them straight back when they advertise for the game.
		Switch2Ble::StopScan();
	}
	else
	{
		UE_LOG(LogJoyShockLibrary, Log,
			TEXT("The game that had the Bluetooth controllers closed: the editor is looking for them again, and sends its own lights and rumble to every controller. Press a button on a Bluetooth controller to bring it back."));
		// The enumeration pass restarts the scan, on its own thread, where a blocking adapter call costs nothing.
		Module.RequestConnectDevices();
	}
}

FJoyShockLibrary4UnrealModule& FJoyShockLibrary4UnrealModule::GetInstance()
{
	if (FJoyShockLibrary4UnrealModule* Live = GLiveJoyShockModule.load(std::memory_order_acquire))
	{
		return *Live;
	}

	// Before startup, or after shutdown has finished: ask the manager, which loads the module if this is
	// the first call for it. This is what this function did unconditionally before, and it is still the
	// right answer on the game thread, which is the only place a first call can come from.
	return FModuleManager::LoadModuleChecked<FJoyShockLibrary4UnrealModule>(TEXT("JoyShockLibrary4Unreal"));
}

#if PLATFORM_WINDOWS
void *hidapiDllHandle = nullptr;

bool FWindowsDeviceChangeMessageHandler::ProcessMessage(HWND hwnd, uint32 msg, WPARAM wParam, LPARAM lParam,
	int32& OutResult)
{
	if (msg == WM_DEVICECHANGE)
	{
		// UE_LOG(LogTemp, Log, TEXT(">>>>>CONNECTING DEVICES TO JoyShockLibrary"));
		// Never enumerate on the game thread (this runs inside the game-thread message pump) -- HID
		// enumeration/initialisation blocks and would freeze the editor. Hand it to a background thread.
		FJoyShockLibrary4UnrealModule::GetInstance().RequestConnectDevices();
	}

	return false;
}
#endif

void FJoyShockLibrary4UnrealModule::RequestConnectDevices()
{
	if (bShuttingDown.load())
	{
		return;
	}

	// Mark that an enumeration pass is wanted. If a worker is already running it will pick this up.
	bConnectQueued.store(true);

	bool bExpected = false;
	if (!bConnectRunning.compare_exchange_strong(bExpected, true))
	{
		return; // A worker is already running; it will observe bConnectQueued.
	}

	Async(EAsyncExecution::Thread, [this]()
	{
		for (;;)
		{
			// Coalesce every request that arrived since we started into this single pass.
			while (bConnectQueued.exchange(false))
			{
				if (bShuttingDown.load())
				{
					break;
				}
				UJoyShockLibrary::ConnectDevices();
			}

			bConnectRunning.store(false);

			// A request may have arrived between our last exchange(false) and clearing bConnectRunning;
			// if so, try to re-acquire ownership, otherwise a later RequestConnectDevices() will.
			if (!bConnectQueued.load() || bShuttingDown.load())
			{
				break;
			}

			bool bReExpected = false;
			if (!bConnectRunning.compare_exchange_strong(bReExpected, true))
			{
				break; // Someone else took ownership.
			}
		}
	});
}

void FJoyShockLibrary4UnrealModule::StartupModule()
{
	IInputDeviceModule::StartupModule();

	// Before anything below can start a thread -- the Bluetooth scan at the end of this function is the
	// first one -- so no thread of ours ever runs without a module to reach.
	GLiveJoyShockModule.store(this, std::memory_order_release);

	// Before the Bluetooth scan below starts, so neither side ever connects a controller the other is about to
	// claim. A commandlet is neither a game nor an editor anyone plays in, so it takes no part.
	if (!IsRunningCommandlet())
	{
		if (GIsEditor)
		{
			GBluetoothYieldedToGame = IsBluetoothClaimedByGame();
			Switch2Ble::SetYieldedToGame(GBluetoothYieldedToGame);
			GBluetoothClaimWatcher = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([this](float)
				{
					UpdateBluetoothClaim(*this);
					return true;
				}),
				0.5f);
		}
		else
		{
#if PLATFORM_WINDOWS
			GBluetoothClaim = ::CreateMutexW(nullptr, 0, GBluetoothClaimName);
#endif
		}
	}

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

#if PLATFORM_WINDOWS
	FString AbsPath = IPluginManager::Get().FindPlugin("JoyShockLibrary4Unreal")->GetBaseDir() / TEXT("ThirdParty/hidapi/x64");
	hidapiDllHandle = FPlatformProcess::GetDllHandle(*(AbsPath / TEXT("hidapi.dll")));
#endif

	// Start listening for Switch 2 controllers on the radio. A controller advertising is the Bluetooth
	// counterpart of a USB device being plugged in, and the message handler above never hears about it --
	// WM_DEVICECHANGE is for devices Windows enumerates, and these are not among them. So the scan drives
	// the same coalesced enumeration pass directly.
	//
	// On a background thread because querying the adapter blocks, and this runs during engine startup.
	GBluetoothStartup = Async(EAsyncExecution::Thread, [this]()
	{
		if (bShuttingDown.load() || !Switch2Ble::IsSupported() || bShuttingDown.load())
		{
			return;
		}

		Switch2Ble::SetDiscoveryCallback([this]()
		{
			// This module, captured, rather than looked up: this runs on a radio thread, and both lookups
			// (IsAvailable and GetInstance's fallback) go through FModuleManager, which off the game thread
			// during shutdown fails its own check -- see GetInstance. The capture is safe because
			// Switch2Ble::Shutdown clears this callback and waits out any call already running before
			// ShutdownModule returns. RequestConnectDevices does nothing once shutdown has begun.
			RequestConnectDevices();
		});
		Switch2Ble::StartScan();
	});
}

void FJoyShockLibrary4UnrealModule::ShutdownModule()
{
	// Stop accepting new enumeration requests and wait for any in-flight background pass to finish
	// before we free the hidapi DLL out from under it.
	bShuttingDown.store(true);

	if (GBluetoothClaimWatcher.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GBluetoothClaimWatcher);
		GBluetoothClaimWatcher.Reset();
	}

	while (bConnectRunning.load())
	{
		FPlatformProcess::Sleep(0.001f);
	}
	// Before Switch2Ble::Shutdown below, so a scan this thread starts late is still one that gets stopped.
	if (GBluetoothStartup.IsValid())
	{
		GBluetoothStartup.Wait();
	}

	// Then stop the controllers themselves, before anything they use goes away. Their polling threads own a
	// hidapi handle or a Bluetooth connection each, and call back into this module; leaving them running
	// while the rest of this function frees those was the crash on closing the editor with a controller on
	// the radio, because the Bluetooth teardown below freed the very object each thread was asleep inside.
	const bool bAllDevicesStopped = ShutdownAllDevices();

	// Now the radio: stop scanning, drop the discovery callback into this module, and release any connection
	// no controller claimed. By this point every controller has handed its own connection back, so this is
	// normally just the scan. Unlike the HID devices these hold WinRT objects whose callbacks fire on their
	// own threads, so they have to be torn down before the module goes.
	Switch2Ble::Shutdown();

#if PLATFORM_WINDOWS
	// Only now that this game has closed every Bluetooth link it held: the editor starts looking again the
	// moment this is gone, and a controller still connected here would not be advertising for it.
	if (GBluetoothClaim != nullptr)
	{
		::CloseHandle(GBluetoothClaim);
		GBluetoothClaim = nullptr;
	}
#endif

#if PLATFORM_WINDOWS
	// Only once nothing can still be inside it. A polling thread that outran the wait above is still holding
	// a hidapi handle, and unloading the library under it would turn a leak into an access violation on the
	// way out -- so the leak stands, and the process reclaims it a moment later.
	if (hidapiDllHandle && bAllDevicesStopped)
	{
		FPlatformProcess::FreeDllHandle(hidapiDllHandle);
		hidapiDllHandle = nullptr;
	}
#endif

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

	// Withdrawn last, because the manager destroys this object as soon as this function returns and a
	// pointer to freed memory is worse than no pointer at all.
	//
	// A polling thread that outran the wait above is still running and can still call GetInstance, and
	// from here on it gets what it got before any of this existed: the manager's refusal, and the check
	// that comes with it. That case is not solvable from inside the plugin -- the module is genuinely
	// going away -- and it is why the wait exists at all.
	GLiveJoyShockModule.store(nullptr, std::memory_order_release);

	IInputDeviceModule::ShutdownModule();
}

TSharedPtr<IInputDevice> FJoyShockLibrary4UnrealModule::CreateInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
{
	return FJoyShockInterface::Create(InMessageHandler);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FJoyShockLibrary4UnrealModule, JoyShockLibrary4Unreal)