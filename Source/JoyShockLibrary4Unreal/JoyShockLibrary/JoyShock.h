#pragma once

#include "JoyShockLibrary.h"
#include "JoyShock.h"
#include "GamepadMotion.hpp"
#include <bitset>
#include "hidapi.h"
#include <chrono>
#include <thread>
#include <unordered_map>
#include <atomic>
#include "tools.h"
#include <cstring>

#ifdef __GNUC__
#define _wcsdup wcsdup
#endif

enum ControllerType { n_switch, s_ds4, s_ds };

// PS5 stuff
#define DS_VENDOR 0x054C
#define DS_USB 0x0CE6
#define DS_USB_V2 0x0DF2 // DualSense Edge

// PS4 stuff
// http://www.psdevwiki.com/ps4/DS4-USB
// http://www.psdevwiki.com/ps4/DS4-BT
// http://eleccelerator.com/wiki/index.php?title=DualShock_4
// and a little bit of https://github.com/chrippa/ds4drv
#define DS4_VENDOR 0x054C
#define DS4_USB 0x05C4
#define DS4_USB_V2 0x09CC
#define DS4_USB_DONGLE 0x0BA0
#define DS4_BT 0x081F
// DS4 compatible controllers
#define BROOK_DS4_VENDOR 0x0C12
#define BROOK_DS4_USB 0x0E20

// Joycon and Pro conroller stuff is mostly from
// https://github.com/mfosse/JoyCon-Driver
// https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/
#define JOYCON_VENDOR 0x057e
#define JOYCON_L_BT 0x2006
#define JOYCON_R_BT 0x2007
#define PRO_CONTROLLER 0x2009
#define JOYCON_CHARGING_GRIP 0x200e
// Nintendo Switch 2 Pro Controller (enumerates as VID 057E / PID 2069 over USB). NOTE: the Switch 2 uses a
// newer input protocol than the Switch 1 controllers above, so this is recognised/created here but full
// input parsing may still need protocol-specific work.
#define PRO_CONTROLLER_2 0x2069
// The Switch 2 Joy-Cons. They never appear over USB -- the console charges them through the rails, and a
// cable to a PC gives nothing -- so these ids only ever come out of a Bluetooth advertisement.
#define SWITCH2_JOYCON_R 0x2066
#define SWITCH2_JOYCON_L 0x2067
#define L_OR_R(lr) (lr == 1 ? 'L' : (lr == 2 ? 'R' : '?'))

// Whether this Windows HID path belongs to a Bluetooth device. Used both to decide a controller's
// transport at connect time and to recognise the cable when the same controller appears twice.
bool IsBluetoothHidPath(const FString& InPath);

class JoyShock {

public:

	hid_device * handle;

	// Set instead of `handle` for a Switch 2 controller reached over Bluetooth. Those are BLE GATT
	// peripherals rather than HID devices, so there is no hid_device to open -- reads and writes go through
	// Switch2Ble instead. Exactly one of the two is ever set; read_input_report picks between them so the
	// polling thread does not have to care which transport it is on.
	class FSwitch2BleConnection* ble_connection = nullptr;
	bool is_ble() const { return ble_connection != nullptr; }

	int32 intHandle = 0;
	FString path;

	wchar_t *serial;

	// The controller's own MAC address, lower-case and without separators, or empty when it could not be
	// determined. This is the only thing that identifies a physical controller across transports: plugging
	// a USB cable into a controller that is already paired over Bluetooth enumerates a SECOND HID device
	// with its own path, which the plugin would otherwise treat as a second controller. See ConnectDevices.
	FString mac_address;

	// The key this device's intHandle was reserved under: its MAC when one could be read, otherwise the HID
	// path it was found on. Stored rather than recomputed because both of those can change while the device
	// lives -- a transport switch replaces `path` -- and the handle has to be released under the same key it
	// was taken with, or it is never reusable again and device ids climb for the rest of the session.
	FString handle_identity;

	// Transport switching. A controller reachable two ways at once (paired over Bluetooth, then plugged
	// in) should be read over the cable -- lower latency, no packet loss -- and fall back to the radio the
	// moment the cable is pulled, with the game seeing neither transition. This mirrors what a PS4/PS5
	// does, except that there the console and firmware negotiate it; here both links stay live and the
	// only decision is which handle to read.
	//
	// Both swaps are performed BY the polling thread on itself. It owns the handle, so nothing has to stop
	// it first -- which is what keeps enumeration from ever having to join a thread while holding the
	// connected lock, a deadlock the polling thread's own disconnect path would walk straight into.
	//
	// Guarded by modifying_lock, since enumeration writes pending_transport_path from another thread.
	FString pending_transport_path;
	FString fallback_path;

	FString name;

	int32 deviceNumber = 0;// left(0) or right(1) vjoy

	int32 left_right = 0;// 1: left joycon, 2: right joycon, 3: pro controller

	std::chrono::steady_clock::time_point last_polled;
	float delta_time = 1.0;

	FJoyShockState simple_state = {};
	FJoyShockState last_simple_state = {};

	FIMUState imu_state = {};
	FIMUState last_imu_state = {};

	FTouchState touch_state = {};
	FTouchState last_touch_state = {};

	GamepadMotion motion;

	float cumulative_gyro_x = 0.f;
	float cumulative_gyro_y = 0.f;
	float cumulative_gyro_z = 0.f;
	int32 num_cumulative_gyro_samples = 0;

	int32 gyroSpace = 0;

	FCriticalSection modifying_lock;

	int8_t dstick;
	uint8_t battery;

	// Charge, normalised across controller families so the Blueprint layer does not have to know each
	// vendor's encoding: 0 = empty, 1 = critical, 2 = low, 3 = medium, 4 = full, and 0xFF for "this
	// controller does not report it". Written by the polling thread as reports arrive, read by the game
	// thread, hence atomic.
	// Set once the first Sony battery byte has been logged for this connection. The DS4/DualSense offsets
	// were written from public documentation rather than measured here, so the first reading is logged to
	// be compared against what the OS reports -- once, not per report, since it is a verification aid and
	// not a running diagnostic.
	bool logged_battery_byte = false;

	static constexpr uint8_t BatteryLevelUnknown = 0xFF;
	std::atomic<uint8_t> battery_level{ BatteryLevelUnknown };
	std::atomic<bool> battery_charging{ false };

	// Charge as a percentage, or -1 where the hardware does not report one finely enough to be worth
	// quoting. The Sony controllers and the Switch 2 fill this; a Switch 1 controller reports five states,
	// and turning those into "75%" would be inventing digits the hardware never measured.
	std::atomic<int32> battery_percent{ -1 };

	// Sensors only the Switch 2 controllers carry, kept apart from imu_state because they are not motion
	// and nothing outside this family reports them. All are written by the polling thread and read by the
	// game thread, hence atomic.
	std::atomic<float> sw2_battery_volts{ 0.f };
	std::atomic<float> sw2_temperature_celsius{ 0.f };
	std::atomic<int32> sw2_magnetometer_x{ 0 };
	std::atomic<int32> sw2_magnetometer_y{ 0 };
	std::atomic<int32> sw2_magnetometer_z{ 0 };
	// The optical sensor in the controller's underside, used for mouse mode. Coordinates are the sensor's
	// own accumulating position, not a per-frame delta; roughness and distance are its read of the surface
	// it is on, and are what tell you whether it is being used as a mouse at all.
	std::atomic<int32> sw2_mouse_x{ 0 };
	std::atomic<int32> sw2_mouse_y{ 0 };
	std::atomic<int32> sw2_mouse_roughness{ 0 };
	std::atomic<int32> sw2_mouse_distance{ 0 };

	// The same movement, added up instead of wrapped.
	//
	// The sensor's own position runs 0..65535 and rolls over, so subtracting one reading from an earlier one
	// reports a jump of a full 65536 whenever it does -- roughly every 80cm of desk, and always as a
	// violent flick in whatever a game aimed with it. Every consumer would have to know that and undo it.
	// Undone once here instead, on the thread that sees every report and therefore never misses a step: the
	// wrap is resolved against the previous reading and the difference accumulated. What comes out only
	// grows, and differences taken from it are just differences.
	std::atomic<int64> sw2_mouse_travel_x{ 0 };
	std::atomic<int64> sw2_mouse_travel_y{ 0 };

	// Where each consumer of that travel left off. Two, because the Blueprint node and the engine's input
	// axes are separate readers of the same movement: sharing one baseline would have each of them see only
	// the part the other had not taken yet, which is the halved sensitivity the node's own tooltip warns
	// about -- except silently, and between two things a game never asked to have connected.
	std::atomic<int64> sw2_mouse_consumed_x{ 0 };
	std::atomic<int64> sw2_mouse_consumed_y{ 0 };
	std::atomic<int64> sw2_mouse_axis_x{ 0 };
	std::atomic<int64> sw2_mouse_axis_y{ 0 };

	// Movement since the given baseline last moved forward, moving it forward to now.
	//
	// Exchanged rather than read-then-written: between a read and a write the polling thread adds more
	// travel, and writing back what was read would drop it. The exchange leaves the baseline at exactly
	// what is being reported, so nothing between two calls is lost or counted twice.
	void consume_mouse_travel(std::atomic<int64>& baselineX, std::atomic<int64>& baselineY,
		int64& outDeltaX, int64& outDeltaY)
	{
		const int64 travelX = sw2_mouse_travel_x.load();
		const int64 travelY = sw2_mouse_travel_y.load();
		outDeltaX = travelX - baselineX.exchange(travelX);
		outDeltaY = travelY - baselineY.exchange(travelY);
	}

	// Adds one report's movement to the running travel. Polling thread only.
	void accumulate_mouse_travel(int32 mouseX, int32 mouseY)
	{
		// A step is only meaningful against a previous reading, and the first report has none: taking it
		// against zero would count the sensor's whole starting offset as movement the player never made.
		if (sw2_mouse_seen)
		{
			// The shortest way round. A real step between reports 7.5ms apart is small, so a difference that
			// looks enormous is the counter having wrapped, not the controller having crossed the desk.
			auto Step = [](int32 current, int32 previous)
			{
				int32 step = current - previous;
				if (step > 32767) { step -= 65536; }
				else if (step < -32768) { step += 65536; }
				return static_cast<int64>(step);
			};

			sw2_mouse_travel_x.fetch_add(Step(mouseX, sw2_mouse_prev_x));
			sw2_mouse_travel_y.fetch_add(Step(mouseY, sw2_mouse_prev_y));
		}

		sw2_mouse_prev_x = mouseX;
		sw2_mouse_prev_y = mouseY;
		sw2_mouse_seen = true;
	}

	// The previous raw mouse reading, and whether there has been one. Polling thread only, hence not atomic.
	int32 sw2_mouse_prev_x = 0;
	int32 sw2_mouse_prev_y = 0;
	bool sw2_mouse_seen = false;

	// Input suppression for the moment a controller arrives. The button that woke a Bluetooth controller is
	// still held when its first reports come in, so without this it reaches the game as a press -- and on a
	// Joy-Con the wake button is usually SL/SR or a shoulder, which is exactly what the grip chords watch.
	// Cleared by the first report with nothing held, or by the deadline, whichever comes first: a player
	// genuinely holding a button at connect must not be locked out for good.
	bool input_settled = false;
	std::chrono::steady_clock::time_point input_settle_deadline;

	int32 global_count = 0;

	// calibration data:
	struct brcm_hdr {
		uint8_t cmd;
		uint8_t rumble[9];
	};

	struct brcm_cmd_01 {
		uint8_t subcmd;
		uint32_t offset;
		uint8_t size;
	};

	int32 timing_byte = 0x0;

	float acc_cal_coeff[3] = {0.0f, 0.0f, 0.0f};
	float gyro_cal_coeff[3] = {0.0f, 0.0f, 0.0f};
	float cal_x[1] = { 0.0f };
	float cal_y[1] = { 0.0f };

	bool initialised = false;

	bool has_user_cal_stick_l = false;
	bool has_user_cal_stick_r = false;
	bool has_user_cal_sensor = false;

	ControllerType controller_type = ControllerType::n_switch;
	bool is_usb = false;

	// Any Switch 2 controller -- the Pro Controller 2 or either Joy-Con 2. They share one protocol, which
	// differs from the Switch 1's: over a cable, commands (init, SPI reads, rumble) go over the controller's
	// WinUSB bulk interface rather than HID; over the radio they are GATT writes. Which of the three this is
	// comes from left_right, exactly as it does for the Switch 1 controllers.
	bool is_switch2 = false;

	// A single Joy-Con 2 rather than a whole controller. What actually differs is what it has: one stick,
	// half the buttons, a pair of rail buttons, and a gyro on a different range.
	bool is_switch2_joycon() const { return is_switch2 && (left_right == 1 || left_right == 2); }

	// The JS_TYPE_* value this device reports. Only meaningful for the Switch family; the Sony controllers
	// have a type of their own and never reach this.
	int32 switch_legacy_type() const
	{
		if (!is_switch2)
		{
			return left_right;
		}
		switch (left_right)
		{
		case 1:  return JS_TYPE_JOYCON2_LEFT;
		case 2:  return JS_TYPE_JOYCON2_RIGHT;
		default: return JS_TYPE_PRO_CONTROLLER_2;
		}
	}

	// Degrees per second per raw gyro LSB. The Switch 2's two shapes do not share a sensor range: the Pro
	// Controller 2 reads 14.2857 LSB/dps (the Switch 1's 936/13371) while a Joy-Con 2 reads 16.384, so using
	// one figure for both makes the other's motion off by 15%.
	float switch2_gyro_scale() const { return is_switch2_joycon() ? (1.0f / 16.384f) : (936.0f / 13371.0f); }

	// WinUSB handles for the Switch 2 command interface (void* keeps Windows types out of this header).
	// The interface is released after a short idle period so a multi-process Standalone session can take
	// ownership from its parent editor without either process permanently blocking the other.
	void* sw2_winusb_file = nullptr;   // HANDLE
	void* sw2_winusb_handle = nullptr; // WINUSB_INTERFACE_HANDLE
	unsigned char sw2_out_pipe = 0x02;
	unsigned char sw2_in_pipe = 0x82;
	bool sw2_init_succeeded = false;

	// Switch 2 HD rumble: the controller consumes a stream of amplitude/frequency frames, each packet
	// carrying a rolling 4-bit id it uses to drop duplicates. Two routes can carry them (see
	// set_sw2_rumble); once one is proven to work the other is not tried again.
	unsigned char sw2_rumble_packet_id = 0;
	bool sw2_rumble_route_logged = false;
	bool sw2_hid_rumble_ok = false;
	std::chrono::steady_clock::time_point sw2_last_open_attempt = {};
	std::chrono::steady_clock::time_point sw2_last_command_time = {};
	bool sw2_access_warning_logged = false;
	int sw2_access_denied_count = 0;
	bool sw2_init_failure_logged = false;

	// Opens (or re-opens) this controller's WinUSB command interface and stores the handles/pipes above.
	// Fails with a bounded warning when another process holds the interface exclusively.
	bool sw2_open_winusb();
	void release_sw2_command_interface_if_idle();

	// Rumble has two independent sources and they must not overwrite each other. These two are what a game
	// asks for directly through JSL4USetControllerRumble, and hold until it asks for something else.
	unsigned char small_rumble = 0;
	unsigned char big_rumble = 0;

	// ...and these are what Unreal's own force feedback asks for. The engine pushes its values every single
	// frame -- zeroes when no effect is playing -- so routing both sources through the same two fields meant
	// force feedback silently wiped any directly-set rumble on the very next frame.
	// The polling thread sends the stronger of the two per motor, so an effect can play over a held rumble
	// and neither API can cancel the other.
	unsigned char ff_small_rumble = 0;
	unsigned char ff_big_rumble = 0;

	unsigned char get_wanted_small_rumble() const { return small_rumble > ff_small_rumble ? small_rumble : ff_small_rumble; }
	unsigned char get_wanted_big_rumble() const { return big_rumble > ff_big_rumble ? big_rumble : ff_big_rumble; }
	unsigned char led_r = 0;
	unsigned char led_g = 0;
	unsigned char led_b = 0;

	uint32 body_colour = 0xFFFFFF;
	uint32 button_colour = 0xFFFFFF;
	uint32 left_grip_colour = 0xFFFFFF;
	uint32 right_grip_colour = 0xFFFFFF;

	int32 player_number = 0;
	int32 reuse_counter = 0;

	bool cancel_thread = false;
	bool delete_on_finish = false;
	bool remove_on_finish = true;
	std::thread* thread = nullptr;

	// Set by the polling thread as the last thing it does, when it is leaving without freeing this device --
	// i.e. when something else owns it and is waiting to. std::thread cannot be joined with a deadline, and
	// a thread wedged in a blocking HID write must not be able to hold the editor open forever, so shutdown
	// waits on this instead of on a join it could never take back. Atomic because it is the handoff between
	// the polling thread and whoever is waiting for it.
	std::atomic<bool> thread_exited{ false };

	// Set by the poll thread the first time this device delivers a real input report, and never cleared.
	// Until then the device is only "enumerable", not proven: a controller that has been powered off can
	// still linger in HID enumeration long enough to be opened and to answer the init handshake, yet it
	// never sends input. Connect/disconnect are only reported to the engine for devices that got this far,
	// so such a phantom is never announced as a controller. Atomic because the poll thread writes it while
	// the game thread reads it.
	std::atomic<bool> has_delivered_input{ false };

	// for calibration:
	bool use_continuous_calibration = false;
	bool cue_motion_reset = false;

	unsigned char factory_stick_cal[0x12];
	unsigned char device_colours[0xC];
	unsigned char user_stick_cal[0x16];
	unsigned char sensor_model[0x6];
	unsigned char stick_model[0x24];
	unsigned char factory_sensor_cal[0x18];
	unsigned char user_sensor_cal[0x1A];
	uint16_t factory_sensor_cal_calm[0xC];
	uint16_t user_sensor_cal_calm[0xC];
	int16_t sensor_cal[0x2][0x3];
	uint16_t stick_cal_x_l[0x3];
	uint16_t stick_cal_y_l[0x3];
	uint16_t stick_cal_x_r[0x3];
	uint16_t stick_cal_y_r[0x3];

	uint32_t crc_table[256] = {
		0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA,
		0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
		0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
		0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
		0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE,
		0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
		0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
		0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
		0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
		0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
		0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940,
		0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
		0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116,
		0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
		0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
		0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
		0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A,
		0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
		0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818,
		0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
		0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
		0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
		0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
		0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
		0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2,
		0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
		0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
		0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
		0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086,
		0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
		0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4,
		0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
		0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
		0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
		0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8,
		0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
		0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE,
		0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
		0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
		0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
		0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252,
		0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
		0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60,
		0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
		0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
		0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
		0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04,
		0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
		0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A,
		0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
		0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
		0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
		0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E,
		0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
		0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
		0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
		0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
	    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
		0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0,
		0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
		0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6,
		0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
		0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
		0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
	};


	//// https://docs.microsoft.com/en-us/openspecs/office_protocols/ms-abs/06966aa2-70da-4bf9-8448-3355f277cd77
	uint32_t crc_32(unsigned char* buf, int32 length);

	void enable_gyro_ds4_bt(unsigned char *buf, int32 bufLength);

public:
	void init(struct hid_device_info *dev, hid_device* inHandle, int32 uniqueHandle, const FString &inPath);

	JoyShock(struct hid_device_info* dev, hid_device* inHandle, int32 uniqueHandle, const FString& inPath);

	// Builds a Switch 2 controller reached over Bluetooth. There is no hid_device_info for one of these --
	// everything the HID path reads out of the descriptor comes from the advertisement instead.
	JoyShock(class FSwitch2BleConnection* inConnection, uint16 productId, uint64 address, int32 uniqueHandle,
		const FString& inPath);

	// Reads one input report from whichever transport this controller is on, with hid_read_timeout's
	// contract: the report's length, 0 on timeout, negative once the device is gone.
	int32 read_input_report(unsigned char* buf, int32 bufLength, int32 timeoutMs);

	~JoyShock();

	void push_cumulative_gyro(float gyroX, float gyroY, float gyroZ);
	
	void get_and_flush_cumulative_gyro(float& gyroX, float& gyroY, float& gyroZ);

	void reset_continuous_calibration();

	void push_sensor_samples(float gyroX, float gyroY, float gyroZ, float accelX, float accelY, float accelZ, float deltaTime);

	void get_calibrated_gyro(float& gyroX, float& gyroY, float& gyroZ);

	FMotionState get_motion_state();

	FIMUState get_transformed_imu_state(FIMUState& InIMUState);

	bool hid_exchange(hid_device *InHandle, unsigned char *buf, int32 len);

	bool send_command(int32 command, uint8_t *data, int32 len);

	bool send_subcommand(int32 command, int32 subcommand, uint8_t *data, int32 len);

	// Sends a Switch subcommand and waits for the controller's 0x21 acknowledgement, re-sending on loss.
	// Use for configuration subcommands whose silent loss is permanent (IMU enable, report mode): unlike
	// send_subcommand, a true return means the controller actually applied the command.
	bool send_subcommand_with_ack(int32 subcommand, const uint8_t *data, int32 len);

	// Writes a Switch subcommand without reading any reply. The only subcommand form that is safe from the
	// polling thread: that thread is the handle's sole reader once the controller streams, so a read here
	// would steal input reports, while a bare write rides alongside the stream like the rumble packets the
	// polling thread already sends. The 0x21 ack arrives interleaved in the input stream and is parsed as
	// a harmless buttons-only report.
	bool write_subcommand(int32 subcommand, const uint8_t *data, int32 len);

	// One bit per output capability, mirroring EJSL4UControllerFunction (bit = 1 << (uint8)Function). A set
	// bit means that function's most recent write failed. Whether that means "blocked by another
	// application" is judged by the polling loop, not here: only a device whose input keeps flowing gets
	// its failures reported -- an unplugged controller fails its writes too, and its read says so first.
	static constexpr uint8_t OutputFunctionRumble = 1 << 0;
	static constexpr uint8_t OutputFunctionPlayerIndicator = 1 << 1;
	static constexpr uint8_t OutputFunctionHomeLight = 1 << 2;
	static constexpr uint8_t OutputFunctionMotionSensor = 1 << 3;
	std::atomic<uint8_t> failed_output_functions{ 0 };

	// Records the outcome of an output write for the given function bits: failure marks them, success
	// clears them (which also re-arms the polling loop's one-shot blocked report).
	void note_output_result(uint8_t FunctionBits, bool bSucceeded);

	void rumble(int32 frequency, int32 intensity);

	bool get_switch_controller_info();

	// Returns whether the controller confirmed the IMU is on. Only the Bluetooth Switch path can report
	// failure; the other transports keep their historical fire-and-forget behaviour and return true.
	bool enable_IMU(unsigned char *buf, int32 bufLength);

	bool init_usb();

	bool init_bt();

	// Nintendo Switch 2 init sequence (sends the SW2 command reports that make it start streaming input),
	// reads factory stick calibration/colours over SPI, and keeps the WinUSB command interface open.
	bool init_switch2();

	// The same for a controller reached over Bluetooth: the commands are the protocol's, not the cable's,
	// so the sequence matches -- but they travel on the GATT command characteristic, and factory data is
	// read with a different subcommand than the USB path's SPI read.
	bool init_switch2_bluetooth();
	bool read_sw2_ble_memory(uint32 address, int32 length, TArray<uint8>& outData);

	// Sends an HD-rumble packet to a Switch 2 controller. smallRumble drives the high-frequency motor
	// component, bigRumble the low-frequency one (0-255 each), and both are true amplitudes rather than
	// the on/off preset this used to trigger.
	void set_sw2_rumble(int smallRumble, int bigRumble);

	// Packs one 5-byte Switch 2 HD-rumble frame: a 40-bit little-endian bitfield of
	// lf_freq:9, lf_tone:1, lf_amp:10, hf_freq:9, hf_tone:1, hf_amp:10. Frequencies are the 9-bit
	// encoded values (0x0E1 / 0x1E1 are the neutral pair), amplitudes are 0-1023.
	static void encode_sw2_rumble_frame(uint16_t lfFreq, uint16_t lfAmp, uint16_t hfFreq, uint16_t hfAmp,
		unsigned char* outFrame);

	// Builds one 16-byte Switch 2 motor block: a packet-id byte followed by three identical 5-byte frames
	// (the controller consumes ~5ms of waveform per frame, so one block is ~15ms of rumble).
	void build_sw2_rumble_block(int smallRumble, int bigRumble, unsigned char* outBlock);

	// Sets the Switch 2 player-indicator bit pattern over its WinUSB command interface.
	bool set_sw2_player_lights(unsigned char playerLightMask);

	// Sends an HD-rumble packet (output report 0x10) to a Switch 1 controller (Joy-Con / Pro Controller).
	// smallRumble drives the high-frequency component, bigRumble the low-frequency one (0-255 each).
	void set_switch_rumble(int smallRumble, int bigRumble);

	// Sets the four player-indicator LEDs on a Switch 1 Joy-Con / Pro Controller.
	bool set_switch_player_lights(unsigned char playerLightMask);

	// The blue HOME-button light is a notification light, not a player indicator. Keep it off when
	// JSL4U owns the controller so it cannot be confused with the four green player LEDs.
	bool clear_switch_home_light();

	// Sets the HOME ring to a steady brightness (0-15). Zero is off, which is what clear_switch_home_light
	// asks for.
	bool set_switch_home_light(unsigned char intensity);

	// Ownership of the HOME light. The plugin clears the light once when the controller comes online, and
	// that upkeep must stop the moment a game sets the light deliberately, or the two would fight -- so the
	// first JSL4USetHomeLight call hands ownership over for good.
	std::atomic<bool> home_light_owned_by_game{ false };
	std::atomic<unsigned char> wanted_home_light{ 0 };

	// Bumped by every JSL4USetHomeLight call, including one that asks for the value the light is already
	// believed to hold. The polling thread writes whenever this differs from the generation it last sent, so
	// what reaches the controller is one write per call rather than one write per change.
	//
	// The difference matters because this light is not ours alone: the firmware switches it back on by
	// itself (a reconnect or a battery notification is enough). Comparing intensities meant the plugin
	// answered "already 0, nothing to do" to a game trying to turn off a light that was physically lit --
	// so "set 0" appeared to do nothing, while "set 0.5 then 0" worked, because only the second of those
	// changed the cached value. A game asking for a state is entitled to have it sent.
	std::atomic<uint32> home_light_generation{ 0 };

	void init_ds4_bt();

	// placeholder to get things working quickly. overdue for a refactor
	void init_ds_usb();

	// this is mostly copied from init_usb() below, but modified to speak DS4
	void init_ds4_usb();

	void deinit_ds4_bt();

	// TODO: implement this
	void deinit_ds4_usb();

	void deinit_usb();

	void set_ds5_rumble_light(unsigned char smallRumble, unsigned char bigRumble,
                           unsigned char colourR,
                           unsigned char colourG,
                           unsigned char colourB,
                           unsigned char playerlights);

	void set_ds4_rumble_light(unsigned char smallRumble, unsigned char bigRumble,
		unsigned char colourR,
		unsigned char colourG,
		unsigned char colourB);

	void set_ds4_rumble_light_usb(unsigned char smallRumble, unsigned char bigRumble,
		unsigned char colourR,
		unsigned char colourG,
		unsigned char colourB);

	void set_ds4_rumble_light_bt(unsigned char smallRumble, unsigned char bigRumble,
		unsigned char colourR,
		unsigned char colourG,
		unsigned char colourB);

    void set_ds5_rumble_light_usb(unsigned char smallRumble, unsigned char bigRumble,
                                 unsigned char colourR,
                                 unsigned char colourG,
                                 unsigned char colourB,
                                 unsigned char playerlights);

	// Calling the Dualsense anything but the DS5 is confusing, since DS also = DualShock, and the DualSense is the PS5 Controller anyway
    void set_ds5_rumble_light_bt(unsigned char smallRumble, unsigned char bigRumble,
                                 unsigned char colourR,
                                 unsigned char colourG,
                                 unsigned char colourB,
                                 unsigned char playerlights);

	//// mfosse credits Hypersect (Ryan Juckett), but I've removed deadzones so the consuming application can deal with them
	//// http://blog.hypersect.com/interpreting-analog-sticks/
	void CalcAnalogStick2
	(
		float &pOutX,       // out: resulting stick X value
		float &pOutY,       // out: resulting stick Y value
		uint16_t x,              // in: initial stick X value
		uint16_t y,              // in: initial stick Y value
		uint16_t x_calc[3],      // calc -X, CenterX, +X
		uint16_t y_calc[3]       // calc -Y, CenterY, +Y
	);

	// SPI (@CTCaer):
	bool get_spi_data(uint32_t offset, const uint8_t read_len, uint8_t *test_buf);

	int32 write_spi_data(uint32_t offset, const uint8_t write_len, uint8_t* test_buf);
};
