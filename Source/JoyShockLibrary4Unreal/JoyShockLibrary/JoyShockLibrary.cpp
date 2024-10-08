// JoyShockLibrary.cpp : Defines the exported functions for the DLL application.
//

#include "JoyShockLibrary.h"
#include <bitset>
#include "hidapi.h"
#include <chrono>
#include <thread>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>
#include "GamepadMotion.hpp"
#include "JoyShock.h"
#include "InputHelpers.h"
#include "JoyShockLibrary4Unreal.h"

// TEMP DEBUG BEGIN
#include "Kismet/KismetSystemLibrary.h"
// TEMP DEBUG END

DEFINE_LOG_CATEGORY(LogJoyShockLibrary)

TMap<int32, JoyShock*> _joyshocks;

TMap<FString, JoyShock*> _byPath;

FCriticalSection _pathHandleLock;
TMap<FString, int32> _pathHandle;
// https://stackoverflow.com/questions/41206861/atomic-increment-and-return-counter
static std::atomic<int> _joyshockHandleCounter;

static int32 GetUniqueHandle(const FString &path)
{
	_pathHandleLock.Lock();
	int32* iter = _pathHandle.Find(path);

	if (iter != nullptr)
	{
		_pathHandleLock.Unlock();
		return *iter;
	}
	const int handle = _joyshockHandleCounter++;
	_pathHandle.Emplace(path, handle);
	_pathHandleLock.Unlock();

	return handle;
}

// https://stackoverflow.com/questions/25144887/map-unordered-map-prefer-find-and-then-at-or-try-at-catch-out-of-range
// not thread-safe -- because you probably want to do something with the object you get out of it, I've left locking to the caller
static JoyShock* GetJoyShockFromHandle(int handle) {
	JoyShock** iter = _joyshocks.Find(handle);

	if (iter != nullptr)
	{
		return *iter;
	}
	return nullptr;
}

void pollIndividualLoop(JoyShock *jc) {
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	if (!jc->handle) { return; }

	hid_set_nonblocking(jc->handle, 0);
	//hid_set_nonblocking(jc->handle, 1); // temporary, to see if it helps. this means we'll have a crazy spin

	int numTimeOuts = 0;
	int numNoIMU = 0;
	bool hasIMU = false;
	bool lockedThread = false;
	int noIMULimit;
	switch (jc->controller_type)
	{
	case ControllerType::s_ds4:
		noIMULimit = 250;
		break;
	case ControllerType::s_ds:
		noIMULimit = 250;
		break;
	case ControllerType::n_switch:
	default:
		noIMULimit = 67;
	}
	float wakeupTimer = 0.0f;

	while (!jc->cancel_thread) {
		// get input:
		unsigned char buf[64];
		memset(buf, 0, 64);

		// 10 seconds of no signal means forget this controller
		int reuseCounter = jc->reuse_counter;
		int res = hid_read_timeout(jc->handle, buf, 64, 1000);

		if (res == -1)
		{
			// disconnected!
			UE_LOG(LogJoyShockLibrary, Log, TEXT("Controller %d disconnected\n"), jc->intHandle);

			JSL4UModule._connectedLock.lock();
			// UJoyShockLibrary::_connectedLock.lock();
			// UJoyShockLibrary::ConnectedLock.Lock();
			lockedThread = true;
			const bool gettingReused = jc->reuse_counter != reuseCounter;
			jc->delete_on_finish = true;
			if (gettingReused)
			{
				jc->remove_on_finish = false;
				jc->delete_on_finish = false;
				lockedThread = false;
				JSL4UModule._connectedLock.unlock();
				// UJoyShockLibrary::_connectedLock.unlock();
				// UJoyShockLibrary::ConnectedLock.Unlock();
			}
			break;
		}

		if (res == 0)
		{
			numTimeOuts++;
			if (numTimeOuts >= 10)
			{
				UE_LOG(LogJoyShockLibrary, Log, TEXT("Controller %d timed out\n"), jc->intHandle);

				// just make sure we get this thing deleted before someone else tries to start a new connection
				JSL4UModule._connectedLock.lock();
				// UJoyShockLibrary::ConnectedLock.Lock();
				lockedThread = true;
				const bool gettingReused = jc->reuse_counter != reuseCounter;
				jc->delete_on_finish = true;
				if (gettingReused)
				{
					jc->remove_on_finish = false;
					jc->delete_on_finish = false;
					lockedThread = false;
					JSL4UModule._connectedLock.unlock();
					// UJoyShockLibrary::ConnectedLock.Unlock();
				}
				break;
			}
			else
			{
				// try wake up the controller with the appropriate message
				if (jc->controller_type != ControllerType::n_switch)
				{
					// TODO
				}
				else
				{
					if (jc->is_usb)
					{
						UE_LOG(LogJoyShockLibrary, Log, TEXT("Attempting to re-initialise controller %d\n"), jc->intHandle);
						if (jc->init_usb())
						{
							numTimeOuts = 0;
						}
					}
					else
					{
						UE_LOG(LogJoyShockLibrary, Log, TEXT("Attempting to re-initialise controller %d\n"), jc->intHandle);
						if (jc->init_bt())
						{
							numTimeOuts = 0;
						}
					}
				}
			}
		}
		else
		{
			numTimeOuts = 0;
			// we want to be able to do these check-and-calls without fear of interruption by another thread. there could be many threads (as many as connected controllers),
			// and the callback could be time-consuming (up to the user), so we use a readers-writer-lock.
			if (handle_input(jc, buf, 64, hasIMU)) { // but the user won't necessarily have a callback at all, so we'll skip the lock altogether in that case
				// accumulate gyro
				FIMUState imuState = jc->get_transformed_imu_state(jc->imu_state);
				jc->push_cumulative_gyro(imuState.gyroX, imuState.gyroY, imuState.gyroZ);
				if (JSL4UModule.GetOnPoll().IsBound() || JSL4UModule.GetOnPollTouch().IsBound())
				{
					std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._callbackLock);
					// FScopeLock Lock(&UJoyShockLibrary::CallbackLock);

					JSL4UModule.GetOnPoll().ExecuteIfBound(jc->intHandle, jc->simple_state, jc->last_simple_state, imuState, jc->get_transformed_imu_state(jc->last_imu_state), jc->delta_time);

					// touchpad will have its own callback so that it doesn't change the existing api
					if (jc->controller_type != ControllerType::n_switch) {
						JSL4UModule.GetOnPollTouch().ExecuteIfBound(jc->intHandle, jc->touch_state, jc->last_touch_state, jc->delta_time);
					}
				}
				// count how many have no IMU result. We want to periodically attempt to enable IMU if it's not present
				if (!hasIMU)
				{
					numNoIMU++;
					if (numNoIMU == noIMULimit)
					{
						memset(buf, 0, 64);
						jc->enable_IMU(buf, 64);
						numNoIMU = 0;
					}
				}
				else
				{
					numNoIMU = 0;
				}
				
				// dualshock 4 bluetooth might need waking up
				if (jc->controller_type == ControllerType::s_ds4 && !jc->is_usb)
				{
					wakeupTimer += jc->delta_time;
					if (wakeupTimer > 30.0f)
					{
						jc->init_ds4_bt();
						wakeupTimer = 0.0f;
					}
				}
			}
		}
	}

	if (jc->cancel_thread)
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\tending cancelled thread\n"));
	}
	else
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\ttiming out thread\n"));
	}

	// remove
	if (jc->remove_on_finish)
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\t\tremoving jc\n"));
		if (!lockedThread)
		{
			JSL4UModule._connectedLock.lock();
			// UJoyShockLibrary::ConnectedLock.Lock();
		}
		_joyshocks.Remove(jc->intHandle);
		_byPath.Remove(jc->path);
		if (!lockedThread)
		{
			JSL4UModule._connectedLock.unlock();
			// UJoyShockLibrary::ConnectedLock.Unlock();
		}
	}

	const int32 intHandle = jc->intHandle;
	// disconnect this device
	if (jc->delete_on_finish)
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("\t\tdeleting jc\n"));
		delete jc;
	}

	if (lockedThread)
	{
		JSL4UModule._connectedLock.unlock();
		// UJoyShockLibrary::ConnectedLock.Unlock();
	}

	// notify that we disconnected this device, and say whether or not it was a timeout (if not a timeout, then an explicit disconnect)
	{
		std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._callbackLock);
		// FScopeLock Lock(&UJoyShockLibrary::CallbackLock);

		// UE_LOG(LogJoyShockLibrary, Log, TEXT(">>>>>Remove on Finish: %d - Delete on Finish: %d"), jc->remove_on_finish, jc->delete_on_finish);
		if (jc->remove_on_finish || jc->delete_on_finish) // Don't notify disconnection if device is being reused
		{
			JSL4UModule.GetOnDisconnected().ExecuteIfBound(intHandle, numTimeOuts >= 10);
		}
	}
}

int32 UJoyShockLibrary::JslConnectDevices()
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	// for writing to console:
	//freopen("CONOUT$", "w", stdout);

	// most of the joycon and pro controller stuff here is thanks to mfosse's vjoy feeder
	// Enumerate and print the HID devices on the system
	struct hid_device_info *devs, *cur_dev;

	std::vector<int> createdIds;

	JSL4UModule._connectedLock.lock();

	int res = hid_init();

	devs = hid_enumerate(0x0, 0x0);
	cur_dev = devs;
	while (cur_dev) {
		bool isSupported = false;
		bool isSwitch = false;
		switch (cur_dev->vendor_id)
		{
		case JOYCON_VENDOR:
			isSupported = cur_dev->product_id == JOYCON_L_BT ||
				cur_dev->product_id == JOYCON_R_BT ||
				cur_dev->product_id == PRO_CONTROLLER ||
				cur_dev->product_id == JOYCON_CHARGING_GRIP;
			isSwitch = true;
			break;
		case DS_VENDOR:
			isSupported = cur_dev->product_id == DS4_USB ||
				cur_dev->product_id == DS4_USB_V2 ||
				cur_dev->product_id == DS4_USB_DONGLE ||
				cur_dev->product_id == DS4_BT ||
				cur_dev->product_id == DS_USB ||
				cur_dev->product_id == DS_USB_V2;
			break;
		case BROOK_DS4_VENDOR:
			isSupported = cur_dev->product_id == BROOK_DS4_USB;
			break;
		default:
			break;
		}
		if (!isSupported)
		{
			cur_dev = cur_dev->next;
			continue;
		}

		const FString path = cur_dev->path;
		JoyShock** iter = _byPath.Find(path);
		JoyShock* currentJc = nullptr;
		bool isSameController = false;
		if (iter != nullptr)
		{
			currentJc = *iter;
			isSameController = isSwitch == (currentJc->controller_type == ControllerType::n_switch);
		}
		UE_LOG(LogJoyShockLibrary, Log, TEXT("path: %s\n"), *FString(StringCast<TCHAR>(cur_dev->path).Get()));

		hid_device* handle = hid_open_path(cur_dev->path);
		if (handle != nullptr)
		{
			UE_LOG(LogJoyShockLibrary, Log, TEXT("\topened new handle\n"));
			if (isSameController)
			{
				UE_LOG(LogJoyShockLibrary, Log, TEXT("\tending old thread and joining\n"));
				// we'll get a new thread, so we need to delete the old one, but we need to actually wait for it, I think, because it'll be affected by init and all that...
				// but we can't wait for it! That could deadlock if it happens to be about to disconnect or time out!
				std::thread* thread = currentJc->thread;
				currentJc->delete_on_finish = false;
				currentJc->remove_on_finish = false;
				currentJc->cancel_thread = true;
				currentJc->reuse_counter++;

				// finishing thread may be about to hit a lock
				JSL4UModule._connectedLock.unlock();
				thread->join();
				JSL4UModule._connectedLock.lock();

				delete thread;
				currentJc->thread = nullptr;
				// don't immediately cancel the next thread:
				currentJc->cancel_thread = false;
				currentJc->delete_on_finish = false;
				currentJc->remove_on_finish = true;

				UE_LOG(LogJoyShockLibrary, Log, TEXT("\tinitialising with new handle\n"));
				// keep calibration stuff, but reset other stuff just in case it's actually a new controller
				currentJc->init(cur_dev, handle, GetUniqueHandle(path), path);
				currentJc = nullptr;
			}
			else
			{
				UE_LOG(LogJoyShockLibrary, Log, TEXT("\tcreating new JoyShock\n"));
				JoyShock* jc = new JoyShock(cur_dev, handle, GetUniqueHandle(path), path);
				_joyshocks.Emplace(jc->intHandle, jc);
				_byPath.Emplace(path, jc);
				createdIds.push_back(jc->intHandle);
			}

			if (currentJc != nullptr)
			{
				UE_LOG(LogJoyShockLibrary, Log, TEXT("\tdeinitialising old controller\n"));
				// it's been replaced! get rid of it
				if (currentJc->controller_type == ControllerType::s_ds4) {
					if (currentJc->is_usb) {
						currentJc->deinit_ds4_usb();
					}
					else {
						currentJc->deinit_ds4_bt();
					}
				}
				else if (currentJc->controller_type == ControllerType::s_ds) {

				}
				else if (currentJc->is_usb) {
					currentJc->deinit_usb();
				}
				UE_LOG(LogJoyShockLibrary, Log, TEXT("\tending old thread\n"));
				std::thread* thread = currentJc->thread;
				currentJc->delete_on_finish = true;
				currentJc->remove_on_finish = false;
				currentJc->cancel_thread = true;
				thread->detach();
				delete thread;
				currentJc->thread = nullptr;
			}
		}

		cur_dev = cur_dev->next;
	}
	hid_free_enumeration(devs);

	// init joyshocks:
	for (TTuple<int32, JoyShock*> pair : _joyshocks)
	{
		JoyShock* jc = pair.Value;
		
		if (jc->initialised)
		{
			continue;
		}

		if (jc->controller_type == ControllerType::s_ds4) {
			if (!jc->is_usb) {
				jc->init_ds4_bt();
			}
			else {
				jc->init_ds4_usb();
			}
		} // dualsense
		else if (jc->controller_type == ControllerType::s_ds)
		{
			jc->initialised = true;
		} // charging grip
		else if (jc->is_usb) {
			//UE_LOG(LibraryLogJoyShock, Log, TEXT("USB\n"));
			jc->init_usb();
		}
		else {
			//UE_LOG(LibraryLogJoyShock, Log, TEXT("BT\n"));
			jc->init_bt();
		}
		// all get time now for polling
		jc->last_polled = std::chrono::steady_clock::now();
		jc->delta_time = 0.0;

		jc->deviceNumber = 0; // left
	}

	unsigned char buf[64];

	// set lights:
	//UE_LOG(LibraryLogJoyShock, Log, TEXT("setting LEDs...\n"));
	int switchIndex = 1;
	int dualSenseIndex = 1;
	for (TTuple<int32, JoyShock*> pair : _joyshocks)
	{
		JoyShock *jc = pair.Value;

		// restore colours if we have them set for this controller
		switch (jc->controller_type)
		{
		case ControllerType::s_ds4:
			jc->set_ds4_rumble_light(0, 0, jc->led_r, jc->led_g, jc->led_b);
			break;
		case ControllerType::s_ds:
			jc->set_ds5_rumble_light(0, 0, jc->led_r, jc->led_g, jc->led_b, dualSenseIndex++);
			break;
		case ControllerType::n_switch:
			jc->player_number = switchIndex++;
			memset(buf, 0x00, 0x40);
			buf[0] = static_cast<unsigned char>(jc->player_number);
			jc->send_subcommand(0x01, 0x30, buf, 1);
			break;
		}

		// threads for polling
		if (jc->thread == nullptr)
		{
			UE_LOG(LogJoyShockLibrary, Log, TEXT("\tstarting new thread\n"));
			jc->thread = new std::thread(pollIndividualLoop, jc);
		}
	}

	const int32 totalDevices = _joyshocks.Num();

	JSL4UModule._connectedLock.unlock();

	// notify that we created the new object (now that we're not in a lock that might prevent reading data)
	{
		std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._callbackLock);
		// FScopeLock Lock(&UJoyShockLibrary::CallbackLock);
		if (JSL4UModule.GetOnConnected().IsBound())
		{
			for (int32 newConnectionHandle : createdIds)
			{
				// _connectCallback(newConnectionHandle);
				JSL4UModule.GetOnConnected().Execute(newConnectionHandle);
			}
		}
	}

	return totalDevices;
}

int32 UJoyShockLibrary::JslGetConnectedDeviceHandles(TArray<int32>& OutDeviceHandleArray)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	int i = 0;
	JSL4UModule._connectedLock.lock_shared();

	for (TTuple<signed int, JoyShock*> pair : _joyshocks)
	{
		OutDeviceHandleArray.Add(pair.Key);
		i++;
	}
	JSL4UModule._connectedLock.unlock_shared();
	return i; // return num actually found
}

void UJoyShockLibrary::JslDisconnectAndDisposeAll()
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	// no more callback
	JSL4UModule.GetOnConnected().Unbind();
	JSL4UModule.GetOnDisconnected().Unbind();
	JSL4UModule.GetOnPoll().Unbind();
	JSL4UModule.GetOnPollTouch().Unbind();

	JSL4UModule._connectedLock.lock();

	for (TTuple<signed int, JoyShock*> pair : _joyshocks)
	{
		JoyShock* jc = pair.Value;
		if (jc->controller_type == ControllerType::s_ds4) {
			if (jc->is_usb) {
				jc->deinit_ds4_usb();
			}
			else {
				jc->deinit_ds4_bt();
			}
		}
		else if (jc->controller_type == ControllerType::s_ds) {

		} // TODO: Charging grip? bluetooth?
		else if (jc->is_usb) {
			jc->deinit_usb();
		}
		// cleanup
		std::thread* thread = jc->thread;
		jc->delete_on_finish = true;
		jc->remove_on_finish = false;
		jc->cancel_thread = true;
		thread->detach();
		delete thread;
	}
	_joyshocks.Empty();
	_byPath.Empty();

	JSL4UModule._connectedLock.unlock();

	// Finalize the hidapi library
	int res = hid_exit();
}

bool UJoyShockLibrary::JslStillConnected(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	return GetJoyShockFromHandle(deviceId) != nullptr;
}

// get buttons as bits in the following order, using North South East West to name face buttons to avoid ambiguity between Xbox and Nintendo layouts:
// 0x00001: up
// 0x00002: down
// 0x00004: left
// 0x00008: right
// 0x00010: plus
// 0x00020: minus
// 0x00040: left stick click
// 0x00080: right stick click
// 0x00100: L
// 0x00200: R
// ZL and ZR are reported as analogue inputs (GetLeftTrigger, GetRightTrigger), because DS4 and XBox controllers use analogue triggers, but we also have them as raw buttons
// 0x00400: ZL
// 0x00800: ZR
// 0x01000: S
// 0x02000: E
// 0x04000: W
// 0x08000: N
// 0x10000: home / PS
// 0x20000: capture / touchpad-click
// 0x40000: SL
// 0x80000: SR
// if you want the whole state, this is the best way to do it
FJoyShockState UJoyShockLibrary::JslGetSimpleState(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state;
	}
	return {};
}

FJSL4UJoyShockState UJoyShockLibrary::JSL4UGetSimpleState(int32 DeviceId)
{
	const FJoyShockState& LegacySimpleState = JslGetSimpleState(DeviceId);
	return {
		.Buttons = LegacySimpleState.buttons,
		.LeftTrigger = LegacySimpleState.lTrigger,
		.RightTrigger = LegacySimpleState.rTrigger,
		.LeftStick = FVector2D(LegacySimpleState.stickLX, LegacySimpleState.stickLY),
		.RightStick = FVector2D(LegacySimpleState.stickRX, LegacySimpleState.stickRY)
	};
}

FIMUState UJoyShockLibrary::JslGetIMUState(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->get_transformed_imu_state(jc->imu_state);
	}
	return {};
}

FJSL4UIMUState UJoyShockLibrary::JSL4UGetIMUState(int32 DeviceID)
{
	const FIMUState& LegacyIMUState = JslGetIMUState(DeviceID);
	return {
		.Acceleration = FVector(LegacyIMUState.accelZ, LegacyIMUState.accelX, -LegacyIMUState.accelY),
		.Gyro = FVector(-LegacyIMUState.gyroZ, LegacyIMUState.gyroX, -LegacyIMUState.gyroY) // Negate rotation around vertical axis to make up for different handedness 
	};
}

FJSL4UIMUState UJoyShockLibrary::JSL4UGetRawIMUState(int32 DeviceID)
{
	const FIMUState& LegacyIMUState = JslGetIMUState(DeviceID);
	return {
		.Acceleration = FVector(LegacyIMUState.accelX, LegacyIMUState.accelY, LegacyIMUState.accelZ),
		.Gyro = FVector(LegacyIMUState.gyroX, LegacyIMUState.gyroY, LegacyIMUState.gyroZ)
	};

	// Right-handed Y-UP (X-RIGHT/Z-BACK)
	// Raw X = JSL4U Y
	// Raw Y = JSL4U Z
	// Raw Z = JSL4U -X
}

FMotionState UJoyShockLibrary::JslGetMotionState(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->get_motion_state();
	}
	return {};
}

FJSL4UMotionState UJoyShockLibrary::JSL4UGetMotionState(int32 DeviceID)
{
	/* TEMP DEBUG
	FVector Origin = FVector(280.0, 0.0f, 0.0f);
	FWorldContext* WorldContext = GEngine->GetWorldContextFromGameViewport(GEngine->GameViewport);
	UWorld* World = WorldContext->World();
	FVector TempFlattened = GamepadMotionHelpers::Motion::Flattened;
	// FVector TempFlattened = FVector(GamepadMotionHelpers::Motion::FlattenedX, GamepadMotionHelpers::Motion::FlattenedY, GamepadMotionHelpers::Motion::FlattenedZ);
	UKismetSystemLibrary::DrawDebugArrow(World, Origin, Origin + TempFlattened * 200.0f, 2.0f, FColor::Red);
	TEMP DEBUG */
	
	FMotionState NativeMotionState = JslGetMotionState(DeviceID);
	FJSL4UMotionState UnrealMotionState;


	// Works with no gravity correction
	UnrealMotionState.Orientation = FQuat(NativeMotionState.rawQuatZ, -NativeMotionState.rawQuatX, -NativeMotionState.rawQuatY, NativeMotionState.rawQuatW);
	
	// Restore one of the two below once the gravity correction bug has been fixed in GamepadMotion.hpp
	// UnrealMotionState.Orientation = FQuat(-NativeMotionState.quatZ, NativeMotionState.quatX, -NativeMotionState.quatY, NativeMotionState.quatW);
	// UnrealMotionState.Orientation = FQuat(NativeMotionState.quatZ, -NativeMotionState.quatX, NativeMotionState.quatY, -NativeMotionState.quatW);

	UnrealMotionState.Acceleration = FVector(NativeMotionState.accelZ, NativeMotionState.accelX, -NativeMotionState.accelY);
	UnrealMotionState.Gravity = FVector(-NativeMotionState.gravZ, NativeMotionState.gravX, NativeMotionState.gravY);
	return UnrealMotionState;
}

FJSL4UMotionState UJoyShockLibrary::JSL4UGetRawMotionState(int32 DeviceID)
{
	FMotionState NativeMotionState = JslGetMotionState(DeviceID);
	FJSL4UMotionState UnrealMotionState;
	UnrealMotionState.Orientation = FQuat(NativeMotionState.quatX, NativeMotionState.quatY, -NativeMotionState.quatZ, NativeMotionState.quatW);

	UnrealMotionState.Acceleration = FVector(NativeMotionState.accelX, NativeMotionState.accelY, NativeMotionState.accelZ);
	UnrealMotionState.Gravity = FVector(NativeMotionState.gravX, NativeMotionState.gravY, NativeMotionState.gravZ);
	return UnrealMotionState;
}

FTouchState UJoyShockLibrary::JslGetTouchState(int32 deviceId, bool previous)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return previous ? jc->last_touch_state : jc->touch_state;
	}
	return {};
}

FJSL4UTouchState UJoyShockLibrary::JSL4UGetTouchState(int32 DeviceId, bool bPrevious)
{
	const FTouchState& LegacyTouchState = JslGetTouchState(DeviceId, bPrevious);

	return {
		.PrimaryTouch = {
			.TouchID = LegacyTouchState.t0Id,
			.bIsDown = LegacyTouchState.t0Down,
			.Location = FVector2D(LegacyTouchState.t0X, LegacyTouchState.t0Y)
		},
		.SecondaryTouch = {
			.TouchID = LegacyTouchState.t1Id,
			.bIsDown = LegacyTouchState.t1Down,
			.Location = FVector2D(LegacyTouchState.t1X, LegacyTouchState.t1Y)
		}
	};
}

bool UJoyShockLibrary::JslGetTouchpadDimension(int32 deviceId, int32 &sizeX, int32 &sizeY)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	// I am assuming a single touchpad (or all touchpads are the same dimension)?
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr)
	{
		switch (jc->controller_type)
		{
		case ControllerType::s_ds4:
		case ControllerType::s_ds:
			sizeX = 1920;
			sizeY = 943;
			break;
		default:
			sizeX = 0;
			sizeY = 0;
			break;
		}
		return true;
	}
	return false;
}

int32 UJoyShockLibrary::JslGetButtons(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state.buttons;
	}
	return 0;
}

FVector2D UJoyShockLibrary::JSL4UGetLeftStick(int32 DeviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc != nullptr)
	{
		return FVector2D(jc->simple_state.stickLX, jc->simple_state.stickLY);
	}
	return FVector2D::ZeroVector;
}

// get thumbsticks
float UJoyShockLibrary::JslGetLeftX(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state.stickLX;
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetLeftY(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state.stickLY;
	}
	return 0.0f;
}

FVector2D UJoyShockLibrary::JSL4UGetRightStick(int32 DeviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc != nullptr) {
		return FVector2D(jc->simple_state.stickRX, jc->simple_state.stickRY);
	}
	return FVector2D::ZeroVector;
}

float UJoyShockLibrary::JslGetRightX(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state.stickRX;
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetRightY(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state.stickRY;
	}
	return 0.0f;
}

// get triggers. Switch controllers don't have analogue triggers, but will report 0.0 or 1.0 so they can be used in the same way as others
float UJoyShockLibrary::JslGetLeftTrigger(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state.lTrigger;
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetRightTrigger(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->simple_state.rTrigger;
	}
	return 0.0f;
}

// get gyro
float UJoyShockLibrary::JslGetGyroX(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->get_transformed_imu_state(jc->imu_state).gyroX;
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetGyroY(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->get_transformed_imu_state(jc->imu_state).gyroY;
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetGyroZ(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->get_transformed_imu_state(jc->imu_state).gyroZ;
	}
	return 0.0f;
}

void UJoyShockLibrary::JslGetAndFlushAccumulatedGyro(int32 deviceId, float& gyroX, float& gyroY, float& gyroZ)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->get_and_flush_cumulative_gyro(gyroX, gyroY, gyroZ);
		return;
	}
	gyroX = gyroY = gyroZ = 0.f;
}

FVector UJoyShockLibrary::JSL4UGetAndFlushAccumulatedGyro(int32 InDeviceId)
{
	float GyroX, GyroY, GyroZ;
	JslGetAndFlushAccumulatedGyro(InDeviceId, GyroY, GyroZ, GyroX);
	return FVector(-GyroX, GyroY, -GyroZ);
}

void UJoyShockLibrary::JslSetGyroSpace(int32 deviceId, int32 gyroSpace)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	if (gyroSpace < 0) {
		gyroSpace = 0;
	}
	if (gyroSpace > 2) {
		gyroSpace = 2;
	}
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->modifying_lock.Lock();
		jc->gyroSpace = gyroSpace;
		jc->modifying_lock.Unlock();
	}
}

void UJoyShockLibrary::JSL4USetGyroSpace(int32 InDeviceID, EJSL4UGyroSpace InGyroSpace)
{
	JslSetGyroSpace(InDeviceID, static_cast<int32>(InGyroSpace));
}

// get accelerometer
float UJoyShockLibrary::JslGetAccelX(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->imu_state.accelX;
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetAccelY(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->imu_state.accelY;
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetAccelZ(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->imu_state.accelZ;
	}
	return 0.0f;
}

// get touchpad
int32 UJoyShockLibrary::JslGetTouchId(int32 deviceId, bool secondTouch)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		if (!secondTouch) {
			return jc->touch_state.t0Id;
		}
		else {
			return jc->touch_state.t1Id;
		}
	}
	return false;
}
bool UJoyShockLibrary::JslGetTouchDown(int32 deviceId, bool secondTouch)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		if (!secondTouch) {
			return jc->touch_state.t0Down;
		}
		else {
			return jc->touch_state.t1Down;
		}
	}
	return false;
}

FVector2D UJoyShockLibrary::JSL4UGetTouch(int32 DeviceId, bool bSecondTouch)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(DeviceId);
	if (jc != nullptr) {
		if (!bSecondTouch) {
			return FVector2D(jc->touch_state.t0X, jc->touch_state.t0Y);
		}
		else {
			return FVector2D(jc->touch_state.t1X, jc->touch_state.t1Y);
		}
	}
	return FVector2D::ZeroVector;
}

float UJoyShockLibrary::JslGetTouchX(int32 deviceId, bool secondTouch)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		if (!secondTouch) {
			return jc->touch_state.t0X;
		}
		else {
			return jc->touch_state.t1X;
		}
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetTouchY(int32 deviceId, bool secondTouch)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		if (!secondTouch) {
			return jc->touch_state.t0Y;
		}
		else {
			return jc->touch_state.t1Y;
		}
	}
	return 0.0f;
}

// analog parameters have different resolutions depending on device
float UJoyShockLibrary::JslGetStickStep(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		if (jc->controller_type != ControllerType::n_switch) {
			return 1.0f / 128.0;
		}
		else {
			if (jc->left_right == 2) // right joycon has no calibration for left stick
			{
				return 1.0f / (jc->stick_cal_x_r[2] - jc->stick_cal_x_r[1]);
			}
			else {
				return 1.0f / (jc->stick_cal_x_l[2] - jc->stick_cal_x_l[1]);
			}
		}
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetTriggerStep(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->controller_type != ControllerType::n_switch ? 1 / 256.0f : 1.0f;
	}
	return 1.0f;
}
float UJoyShockLibrary::JslGetPollRate(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->controller_type != ControllerType::n_switch ? 250.0f : 66.6667f;
	}
	return 0.0f;
}
float UJoyShockLibrary::JslGetTimeSinceLastUpdate(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		auto time_now = std::chrono::steady_clock::now();
		return (float)(std::chrono::duration_cast<std::chrono::microseconds>(time_now - jc->last_polled).count() / 1000000.0);
	}
	return 0.0f;
}

// calibration
void UJoyShockLibrary::JslResetContinuousCalibration(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->reset_continuous_calibration();
	}
}
void UJoyShockLibrary::JslStartContinuousCalibration(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->use_continuous_calibration = true;
		jc->cue_motion_reset = true;
	}
}
void UJoyShockLibrary::JslPauseContinuousCalibration(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->use_continuous_calibration = false;
	}
}
void UJoyShockLibrary::JslSetAutomaticCalibration(int32 deviceId, bool enabled)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();	
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->modifying_lock.Lock();
		jc->motion.SetCalibrationMode(enabled ? GamepadMotionHelpers::CalibrationMode::SensorFusion | GamepadMotionHelpers::CalibrationMode::Stillness : GamepadMotionHelpers::CalibrationMode::Manual);
		jc->modifying_lock.Unlock();
	}
}
void UJoyShockLibrary::JslGetCalibrationOffset(int32 deviceId, float& xOffset, float& yOffset, float& zOffset)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		// not technically modifying, but also not a simple getter
		jc->modifying_lock.Lock();
		jc->motion.GetCalibrationOffset(xOffset, yOffset, zOffset);
		jc->modifying_lock.Unlock();
	}
}

void UJoyShockLibrary::JslSetCalibrationOffset(int32 deviceId, float xOffset, float yOffset, float zOffset)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		jc->modifying_lock.Lock();
		jc->motion.SetCalibrationOffset(xOffset, yOffset, zOffset, 1);
		jc->modifying_lock.Unlock();
	}
}
FJSLAutoCalibration UJoyShockLibrary::JslGetAutoCalibrationStatus(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		FJSLAutoCalibration calibration;

		calibration.autoCalibrationEnabled = jc->motion.GetCalibrationMode() != GamepadMotionHelpers::CalibrationMode::Manual;
		calibration.confidence = jc->motion.GetAutoCalibrationConfidence();
		calibration.isSteady = jc->motion.GetAutoCalibrationIsSteady();

		return calibration;
	}

	return {};
}

FJSL4USettings UJoyShockLibrary::JSL4UGetControllerInfoAndSettings(int32 DeviceId)
{
	const FJSLSettings& JslSettings = JslGetControllerInfoAndSettings(DeviceId);

	uint32 RGBColor = JslSettings.colour;
	uint8 Red = (RGBColor >> 16) & 0xff;
	uint8 Green = (RGBColor >> 8) & 0xff;;
	uint8 Blue = RGBColor & 0xff;

	return {
		.GyroSpace = JslSettings.gyroSpace,
		.Color = FColor(Blue, Green, Red),
		.PlayerNumber = JslSettings.playerNumber,
		.ControllerType = static_cast<EJSL4UControllerType>(JslSettings.controllerType),
		.SplitType = JslSettings.splitType,
		.bIsCalibrating = JslSettings.isCalibrating,
		.bAutoCalibrationEnabled = JslSettings.autoCalibrationEnabled,
		.bIsConnected = JslSettings.isConnected
	};
}

// super-getter for reading a whole lot of state at once
FJSLSettings UJoyShockLibrary::JslGetControllerInfoAndSettings(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		FJSLSettings settings;

		settings.gyroSpace = jc->gyroSpace;
		settings.playerNumber = jc->player_number;
		settings.splitType = jc->left_right;
		settings.isConnected = true;
		settings.isCalibrating = jc->use_continuous_calibration;
		settings.autoCalibrationEnabled = jc->motion.GetCalibrationMode() != GamepadMotionHelpers::CalibrationMode::Manual;

		switch (jc->controller_type)
		{
		case ControllerType::s_ds4:
			settings.controllerType = JS_TYPE_DS4;
			break;
		case ControllerType::s_ds:
			settings.controllerType = JS_TYPE_DS;
			break;
		default:
		case ControllerType::n_switch:
			settings.controllerType = jc->left_right;
			settings.colour = jc->body_colour;
			break;
		}

		if (jc->controller_type != ControllerType::n_switch)
		{
			// get led colour
			settings.colour = (int)(jc->led_b) | ((int)(jc->led_g) << 8) | ((int)(jc->led_r) << 16);
		}

		return settings;
	}
	return {};
}

// what split type of controller is this?
int32 UJoyShockLibrary::JslGetControllerType(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		switch (jc->controller_type)
		{
		case ControllerType::s_ds4:
			return JS_TYPE_DS4;
		case ControllerType::s_ds:
			return JS_TYPE_DS;
		default:
		case ControllerType::n_switch:
			return jc->left_right;
		}
	}
	return 0;
}

// what split type of controller is this?
int32 UJoyShockLibrary::JslGetControllerSplitType(int32 deviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();

	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr) {
		return jc->left_right;
	}
	return 0;
}

// what colour is the controller (not all controllers support this; those that don't will report white)
FColor UJoyShockLibrary::JslGetControllerColor(int32 InDeviceId)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	// this just reports body colour. Switch controllers also give buttons colour, and in Pro's case, left and right grips
	JoyShock* jc = GetJoyShockFromHandle(InDeviceId);
	if (jc != nullptr) {
		uint32 RGBColor = jc->body_colour;
		uint8 Red = (RGBColor >> 16) & 0xff;
		uint8 Green = (RGBColor >> 8) & 0xff;
		uint8 Blue = RGBColor & 0xff;
		return FColor(Blue, Green, Red);
	}
	return FColor::White;
}

// set controller light colour (not all controllers have a light whose colour can be set, but that just means nothing will be done when this is called -- no harm)
void UJoyShockLibrary::JslSetLightColor(int32 InDeviceId, FColor InColor)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(InDeviceId);
	if (jc != nullptr && jc->controller_type == ControllerType::s_ds4) {
		jc->modifying_lock.Lock();
		jc->led_r = InColor.R; // (colour >> 16) & 0xff;
		jc->led_g = InColor.G; // (colour >> 8) & 0xff;
		jc->led_b = InColor.B; // colour & 0xff;
		jc->set_ds4_rumble_light(
			jc->small_rumble,
			jc->big_rumble,
			jc->led_r,
			jc->led_g,
			jc->led_b);
		jc->modifying_lock.Unlock();
	}
	else if(jc != nullptr && jc->controller_type == ControllerType::s_ds) {
		jc->modifying_lock.Lock();
        jc->led_r = InColor.R; // (colour >> 16) & 0xff;
        jc->led_g = InColor.G; // (colour >> 8) & 0xff;
        jc->led_b = InColor.B; // colour & 0xff;
        jc->set_ds5_rumble_light(
                jc->small_rumble,
                jc->big_rumble,
                jc->led_r,
                jc->led_g,
                jc->led_b,
                jc->player_number);
		jc->modifying_lock.Unlock();
	}
}

// set controller rumble
void UJoyShockLibrary::JslSetRumble(int32 deviceId, int32 smallRumble, int32 bigRumble)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr && jc->controller_type == ControllerType::s_ds4) {
		jc->modifying_lock.Lock();
		jc->small_rumble = smallRumble;
		jc->big_rumble = bigRumble;
		jc->set_ds4_rumble_light(
			jc->small_rumble,
			jc->big_rumble,
			jc->led_r,
			jc->led_g,
			jc->led_b);
		jc->modifying_lock.Unlock();
	}
    else if (jc != nullptr && jc->controller_type == ControllerType::s_ds) {
		jc->modifying_lock.Lock();
        jc->small_rumble = smallRumble;
        jc->big_rumble = bigRumble;
        jc->set_ds5_rumble_light(
                jc->small_rumble,
                jc->big_rumble,
                jc->led_r,
                jc->led_g,
                jc->led_b,
                jc->player_number);
		jc->modifying_lock.Unlock();
    }
}
// set controller player number indicator (not all controllers have a number indicator which can be set, but that just means nothing will be done when this is called -- no harm)
void UJoyShockLibrary::JslSetPlayerNumber(int32 deviceId, int32 number)
{
	FJoyShockLibrary4UnrealModule& JSL4UModule = FJoyShockLibrary4UnrealModule::GetInstance();
	
	std::shared_lock<std::shared_timed_mutex> lock(JSL4UModule._connectedLock);
	
	JoyShock* jc = GetJoyShockFromHandle(deviceId);
	if (jc != nullptr && jc->controller_type == ControllerType::n_switch) {
		jc->modifying_lock.Lock();
		jc->player_number = number;
		unsigned char buf[64];
		memset(buf, 0x00, 0x40);
		buf[0] = (unsigned char)number;
		jc->send_subcommand(0x01, 0x30, buf, 1);
		jc->modifying_lock.Unlock();
	}
	else if(jc != nullptr && jc->controller_type == ControllerType::s_ds) {
		jc->modifying_lock.Lock();
	    jc->player_number = number;
        jc->set_ds5_rumble_light(
                jc->small_rumble,
                jc->big_rumble,
                jc->led_r,
                jc->led_g,
                jc->led_b,
                jc->player_number);
		jc->modifying_lock.Unlock();
	}
}
