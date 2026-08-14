// JoyShock_Nintendo.cpp - The Switch 1 protocol: Joy-Cons and the Pro Controller.
//
// Nintendo's controllers are driven by subcommands: a numbered request written into an output report,
// usually acknowledged by the next input report. Everything here is built on that -- reading the factory
// stick calibration out of SPI, enabling the IMU, the rumble encoding, the player and HOME lights, and
// the separate init paths a Joy-Con needs over USB and over Bluetooth.
//
// Split out of JoyShock.cpp. See that file for what the families share.

#include "JoyShock.h"
#include "JoyShockLibrary.h"
#include "Switch2Bluetooth.h"
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

bool JoyShock::send_command(int command, uint8_t *data, int len) {
	unsigned char buf[0x40];
	memset(buf, 0, 0x40);

	if (is_usb) {
		buf[0x00] = 0x80;
		buf[0x01] = 0x92;
		buf[0x03] = 0x31;
	}

	buf[is_usb ? 0x8 : 0x0] = command;
	if (data != nullptr && len != 0) {
		memcpy(buf + (is_usb ? 0x9 : 0x1), data, len);
	}

	if (!hid_exchange(this->handle, buf, len + (is_usb ? 0x9 : 0x1)))
	{
		return false;
	}

	if (data) {
		memcpy(data, buf, 0x40);
	}
	return true;
}
bool JoyShock::send_subcommand(int command, int subcommand, uint8_t *data, int len) {
	unsigned char buf[0x40];
	memset(buf, 0, 0x40);

	uint8_t rumble_base[9] = { std::uint8_t((++global_count) & 0xF), 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };
	memcpy(buf, rumble_base, 9);

	if (global_count > 0xF) {
		global_count = 0x0;
	}

	// set neutral rumble base only if the command is vibrate (0x01)
	// if set when other commands are set, might cause the command to be misread and not executed
	//if (subcommand == 0x01) {
	//	uint8_t rumble_base[9] = { (++global_count) & 0xF, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };
	//	memcpy(buf + 10, rumble_base, 9);
	//}

	buf[9] = subcommand;
	if (data && len != 0) {
		memcpy(buf + 10, data, len);
	}

	if (!send_command(command, buf, 10 + len))
	{
		return false;
	}

	if (data) {
		memcpy(data, buf, 0x40); //TODO
	}
	return true;
}
bool JoyShock::send_subcommand_with_ack(int subcommand, const uint8_t *data, int len) {
	// Bluetooth drops subcommands, and a configuration subcommand lost during init fails silently and
	// permanently: the controller keeps streaming buttons, so nothing downstream ever notices that (say)
	// the IMU enable never landed, and the polling loop deliberately never re-configures a live stream.
	// The controller acknowledges every subcommand with an 0x21 report that echoes the subcommand id in
	// byte 14 and sets bit 7 of the ack byte at byte 13 (the same layout get_spi_data relies on), so
	// wait for that echo and re-send when it doesn't come.
	for (int attempt = 0; attempt < 3; attempt++) {
		unsigned char buf[0x40];
		memset(buf, 0, sizeof(buf));
		if (data && len > 0) {
			memcpy(buf, data, len);
		}
		if (!send_subcommand(0x1, subcommand, buf, len)) {
			continue; // nothing came back within the timeout; re-send
		}
		// send_subcommand leaves the first report it read back in buf. That is the ack only when no input
		// report beat it, so skip past interleaved input reports (bounded: once the controller streams at
		// 60Hz most reads return ordinary input, and an unbounded wait would hang on a lost ack).
		for (int read = 0; read < 16; read++) {
			if (buf[0] == 0x21 && buf[14] == subcommand) {
				if (buf[13] & 0x80) {
					return true;
				}
				break; // explicit NACK: re-send rather than keep waiting
			}
			const int res = hid_read_timeout(handle, buf, sizeof(buf), 100);
			if (res < 0) {
				return false; // device is gone; retrying can't bring it back
			}
			if (res == 0) {
				break; // link went quiet; re-send
			}
		}
	}
	return false;
}
bool JoyShock::write_subcommand(int subcommand, const uint8_t *data, int len) {
	// Same wire layout send_subcommand produces over Bluetooth (and that get_spi_data proves also works
	// raw over USB after the handshake): [0x01][4-bit counter][8 neutral rumble bytes][subcmd][payload].
	unsigned char buf[0x40];
	memset(buf, 0, sizeof(buf));
	buf[0] = 0x01;
	buf[1] = std::uint8_t((++global_count) & 0xF);
	if (global_count > 0xF) {
		global_count = 0x0;
	}
	static const unsigned char neutralRumble[8] = { 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40 };
	memcpy(buf + 2, neutralRumble, 8);
	buf[10] = static_cast<unsigned char>(subcommand);
	if (data && len > 0) {
		memcpy(buf + 11, data, len);
	}
	return hid_write(handle, buf, 11 + len) >= 0;
}
void JoyShock::rumble(int frequency, int intensity) {

	unsigned char buf[0x400];
	memset(buf, 0, 0x40);

	// intensity: (0, 8)
	// frequency: (0, 255)

	//	 X	AA	BB	 Y	CC	DD
	//[0 1 x40 x40 0 1 x40 x40] is neutral.


	//for (int j = 0; j <= 8; j++) {
	//	buf[1 + intensity] = 0x1;//(i + j) & 0xFF;
	//}

	buf[1 + 0 + intensity] = 0x1;
	buf[1 + 4 + intensity] = 0x1;

	// Set frequency to increase
	if (this->left_right == 1) {
		buf[1 + 0] = frequency;// (0, 255)
	}
	else {
		buf[1 + 4] = frequency;// (0, 255)
	}

	// set non-blocking:
	hid_set_nonblocking(this->handle, 1);

	send_command(0x10, (uint8_t*)buf, 0x9);
}
bool JoyShock::get_switch_controller_info() {
	bool result = false;

	memset(factory_stick_cal, 0, 0x12);
	memset(device_colours, 0, 0xC);
	memset(user_stick_cal, 0, 0x16);
	memset(sensor_model, 0, 0x6);
	memset(stick_model, 0, 0x12);
	memset(factory_sensor_cal, 0, 0x18);
	memset(user_sensor_cal, 0, 0x1A);
	memset(factory_sensor_cal_calm, 0, 0xC);
	memset(user_sensor_cal_calm, 0, 0xC);
	memset(sensor_cal, 0, sizeof(sensor_cal));
	memset(stick_cal_x_l, 0, sizeof(stick_cal_x_l));
	memset(stick_cal_y_l, 0, sizeof(stick_cal_y_l));
	memset(stick_cal_x_r, 0, sizeof(stick_cal_x_r));
	memset(stick_cal_y_r, 0, sizeof(stick_cal_y_r));


	// These reads used to be all-or-nothing: eight SPI transactions in a row, any one of which failed the
	// whole init -- and with it vibration, the IMU, the report mode and the player LED, since those are set
	// up by the caller before this point. At ~95% per read over a busy Bluetooth link that is 0.95^8, about
	// two connects in three succeeding, which matches the observed flakiness.
	//
	// Only the factory stick calibration is actually load-bearing; without it the sticks have no range.
	// Everything else degrades gracefully, so it warns and carries on: the sensor calibration falls back to
	// the uncalibrated coefficients computed below, the colours are cosmetic, and the two user-calibration
	// blocks are optional anyway (the code below only uses them if they carry their own 0xA1B2 magic, and a
	// failed read leaves them zeroed by the memsets above).
	//
	// The old reads of sensor_model (0x6080) and stick_model (0x6086, 0x6098) are gone entirely: nothing
	// ever read those buffers back, so they were three failure opportunities for data we discard.
	if (!get_spi_data(0x6020, 0x18, factory_sensor_cal))
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("Controller %d: couldn't read factory sensor calibration; continuing with an uncalibrated gyro/accelerometer.\n"), intHandle);
	}
	if (!get_spi_data(0x603D, 0x12, factory_stick_cal))
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("Controller %d: couldn't read factory stick calibration; failing init so it gets retried (the sticks would have no range).\n"), intHandle);
		return false;
	}
	if (!get_spi_data(0x6050, 0xC, device_colours))
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("Controller %d: couldn't read colours; continuing.\n"), intHandle);
	}
	if (!get_spi_data(0x8010, 0x16, user_stick_cal))
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("Controller %d: couldn't read user stick calibration; using the factory values.\n"), intHandle);
	}
	if (!get_spi_data(0x8026, 0x1A, user_sensor_cal))
	{
		UE_LOG(LogJoyShockLibrary, Warning, TEXT("Controller %d: couldn't read user sensor calibration; using the factory values.\n"), intHandle);
	}


	// get stick calibration data:

	// factory calibration:

	if (this->left_right == 1 || this->left_right == 3) {
		stick_cal_x_l[1] = (factory_stick_cal[4] << 8) & 0xF00 | factory_stick_cal[3];
		stick_cal_y_l[1] = (factory_stick_cal[5] << 4) | (factory_stick_cal[4] >> 4);
		stick_cal_x_l[0] = stick_cal_x_l[1] - ((factory_stick_cal[7] << 8) & 0xF00 | factory_stick_cal[6]);
		stick_cal_y_l[0] = stick_cal_y_l[1] - ((factory_stick_cal[8] << 4) | (factory_stick_cal[7] >> 4));
		stick_cal_x_l[2] = stick_cal_x_l[1] + ((factory_stick_cal[1] << 8) & 0xF00 | factory_stick_cal[0]);
		stick_cal_y_l[2] = stick_cal_y_l[1] + ((factory_stick_cal[2] << 4) | (factory_stick_cal[2] >> 4));

	}

	if (this->left_right == 2 || this->left_right == 3) {
		stick_cal_x_r[1] = (factory_stick_cal[10] << 8) & 0xF00 | factory_stick_cal[9];
		stick_cal_y_r[1] = (factory_stick_cal[11] << 4) | (factory_stick_cal[10] >> 4);
		stick_cal_x_r[0] = stick_cal_x_r[1] - ((factory_stick_cal[13] << 8) & 0xF00 | factory_stick_cal[12]);
		stick_cal_y_r[0] = stick_cal_y_r[1] - ((factory_stick_cal[14] << 4) | (factory_stick_cal[13] >> 4));
		stick_cal_x_r[2] = stick_cal_x_r[1] + ((factory_stick_cal[16] << 8) & 0xF00 | factory_stick_cal[15]);
		stick_cal_y_r[2] = stick_cal_y_r[1] + ((factory_stick_cal[17] << 4) | (factory_stick_cal[16] >> 4));

	}


	// if there is user calibration data:
	if ((user_stick_cal[0] | user_stick_cal[1] << 8) == 0xA1B2) {
		stick_cal_x_l[1] = (user_stick_cal[6] << 8) & 0xF00 | user_stick_cal[5];
		stick_cal_y_l[1] = (user_stick_cal[7] << 4) | (user_stick_cal[6] >> 4);
		stick_cal_x_l[0] = stick_cal_x_l[1] - ((user_stick_cal[9] << 8) & 0xF00 | user_stick_cal[8]);
		stick_cal_y_l[0] = stick_cal_y_l[1] - ((user_stick_cal[10] << 4) | (user_stick_cal[9] >> 4));
		stick_cal_x_l[2] = stick_cal_x_l[1] + ((user_stick_cal[3] << 8) & 0xF00 | user_stick_cal[2]);
		stick_cal_y_l[2] = stick_cal_y_l[1] + ((user_stick_cal[4] << 4) | (user_stick_cal[3] >> 4));
		//FormJoy::myform1->textBox_lstick_ucal->Text = String::Format(L"L Stick User:\r\nCenter X,Y: ({0:X3}, {1:X3})\r\nX: [{2:X3} - {4:X3}] Y: [{3:X3} - {5:X3}]",
		//stick_cal_x_l[1], stick_cal_y_l[1], stick_cal_x_l[0], stick_cal_y_l[0], stick_cal_x_l[2], stick_cal_y_l[2]);
	}
	else {
		//FormJoy::myform1->textBox_lstick_ucal->Text = L"L Stick User:\r\nNo calibration";
		//UE_LOG(LogJoyShockLibrary, Log, TEXT("no user Calibration data for left stick.\n"));
	}

	if ((user_stick_cal[0xB] | user_stick_cal[0xC] << 8) == 0xA1B2) {
		stick_cal_x_r[1] = (user_stick_cal[14] << 8) & 0xF00 | user_stick_cal[13];
		stick_cal_y_r[1] = (user_stick_cal[15] << 4) | (user_stick_cal[14] >> 4);
		stick_cal_x_r[0] = stick_cal_x_r[1] - ((user_stick_cal[17] << 8) & 0xF00 | user_stick_cal[16]);
		stick_cal_y_r[0] = stick_cal_y_r[1] - ((user_stick_cal[18] << 4) | (user_stick_cal[17] >> 4));
		stick_cal_x_r[2] = stick_cal_x_r[1] + ((user_stick_cal[20] << 8) & 0xF00 | user_stick_cal[19]);
		stick_cal_y_r[2] = stick_cal_y_r[1] + ((user_stick_cal[21] << 4) | (user_stick_cal[20] >> 4));
		//FormJoy::myform1->textBox_rstick_ucal->Text = String::Format(L"R Stick User:\r\nCenter X,Y: ({0:X3}, {1:X3})\r\nX: [{2:X3} - {4:X3}] Y: [{3:X3} - {5:X3}]",
		//stick_cal_x_r[1], stick_cal_y_r[1], stick_cal_x_r[0], stick_cal_y_r[0], stick_cal_x_r[2], stick_cal_y_r[2]);
	}
	else {
		//FormJoy::myform1->textBox_rstick_ucal->Text = L"R Stick User:\r\nNo calibration";
		//UE_LOG(LogJoyShockLibrary, Log, TEXT("no user Calibration data for right stick.\n"));
	}

	// get gyro / accelerometer calibration data:

	// factory calibration:

	// Acc cal origin position
	sensor_cal[0][0] = uint16_to_int16(factory_sensor_cal[0] | factory_sensor_cal[1] << 8);
	sensor_cal[0][1] = uint16_to_int16(factory_sensor_cal[2] | factory_sensor_cal[3] << 8);
	sensor_cal[0][2] = uint16_to_int16(factory_sensor_cal[4] | factory_sensor_cal[5] << 8);

	// Gyro cal origin position
	sensor_cal[1][0] = uint16_to_int16(factory_sensor_cal[0xC] | factory_sensor_cal[0xD] << 8);
	sensor_cal[1][1] = uint16_to_int16(factory_sensor_cal[0xE] | factory_sensor_cal[0xF] << 8);
	sensor_cal[1][2] = uint16_to_int16(factory_sensor_cal[0x10] | factory_sensor_cal[0x11] << 8);


	//hex_dump(user_sensor_cal, 0x14);

	// user calibration:
	if ((user_sensor_cal[0x0] | user_sensor_cal[0x1] << 8) == 0xA1B2) {
		//UE_LOG(LogJoyShockLibrary, Log, TEXT("User calibration available\n"));
		//if (true) {
		//FormJoy::myform1->textBox_6axis_ucal->Text = L"6-Axis User (XYZ):\r\nAcc:  ";
		//for (int i = 0; i < 0xC; i = i + 6) {
		//	FormJoy::myform1->textBox_6axis_ucal->Text += String::Format(L"{0:X4} {1:X4} {2:X4}\r\n      ",
		//		user_sensor_cal[i + 2] | user_sensor_cal[i + 3] << 8,
		//		user_sensor_cal[i + 4] | user_sensor_cal[i + 5] << 8,
		//		user_sensor_cal[i + 6] | user_sensor_cal[i + 7] << 8);
		//}
		// Acc cal origin position
		sensor_cal[0][0] = uint16_to_int16(user_sensor_cal[2] | user_sensor_cal[3] << 8);
		sensor_cal[0][1] = uint16_to_int16(user_sensor_cal[4] | user_sensor_cal[5] << 8);
		sensor_cal[0][2] = uint16_to_int16(user_sensor_cal[6] | user_sensor_cal[7] << 8);
		//FormJoy::myform1->textBox_6axis_ucal->Text += L"\r\nGyro: ";
		//for (int i = 0xC; i < 0x18; i = i + 6) {
		//	FormJoy::myform1->textBox_6axis_ucal->Text += String::Format(L"{0:X4} {1:X4} {2:X4}\r\n      ",
		//		user_sensor_cal[i + 2] | user_sensor_cal[i + 3] << 8,
		//		user_sensor_cal[i + 4] | user_sensor_cal[i + 5] << 8,
		//		user_sensor_cal[i + 6] | user_sensor_cal[i + 7] << 8);
		//}
		// Gyro cal origin position
		sensor_cal[1][0] = uint16_to_int16(user_sensor_cal[0xE] | user_sensor_cal[0xF] << 8);
		sensor_cal[1][1] = uint16_to_int16(user_sensor_cal[0x10] | user_sensor_cal[0x11] << 8);
		sensor_cal[1][2] = uint16_to_int16(user_sensor_cal[0x12] | user_sensor_cal[0x13] << 8);
	}
	else {
		//FormJoy::myform1->textBox_6axis_ucal->Text = L"\r\n\r\nUser:\r\nNo calibration";
	}

	// Internal scaling and unit conversions as per https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering/blob/master/imu_sensor_notes.md
	// Use SPI calibration and convert them to Gs
	acc_cal_coeff[0] = (float)(1.0 / (float)(16384 - uint16_to_int16(sensor_cal[0][0]))) * 4.0f;
	acc_cal_coeff[1] = (float)(1.0 / (float)(16384 - uint16_to_int16(sensor_cal[0][1]))) * 4.0f;
	acc_cal_coeff[2] = (float)(1.0 / (float)(16384 - uint16_to_int16(sensor_cal[0][2]))) * 4.0f;

	// Use SPI calibration and convert them to degrees per second
	gyro_cal_coeff[0] = (float)(936.0 / (float)(13371 - uint16_to_int16(sensor_cal[1][0])));
	gyro_cal_coeff[1] = (float)(936.0 / (float)(13371 - uint16_to_int16(sensor_cal[1][1])));
	gyro_cal_coeff[2] = (float)(936.0 / (float)(13371 - uint16_to_int16(sensor_cal[1][2])));

	// Device colours
	body_colour =
		(((int)device_colours[0]) << 16) +
		(((int)device_colours[1]) << 8) +
		(((int)device_colours[2]));
	button_colour =
		(((int)device_colours[3]) << 16) +
		(((int)device_colours[4]) << 8) +
		(((int)device_colours[5]));
	left_grip_colour =
		(((int)device_colours[6]) << 16) +
		(((int)device_colours[7]) << 8) +
		(((int)device_colours[8]));
	right_grip_colour =
		(((int)device_colours[9]) << 16) +
		(((int)device_colours[10]) << 8) +
		(((int)device_colours[11]));

	UE_LOG(LogJoyShockLibrary, Log, TEXT("Body: %#08x; Buttons: %#08x; Left Grip: %#08x; Right Grip: %#08x;\n"),
		body_colour,
		button_colour,
		left_grip_colour,
		right_grip_colour);

	//hex_dump(reinterpret_cast<unsigned char*>(sensor_cal[0]), 6);
	//hex_dump(reinterpret_cast<unsigned char*>(sensor_cal[1]), 6);

	return true;
}
bool JoyShock::enable_IMU(unsigned char *buf, int bufLength) {
	memset(buf, 0, bufLength);

	// Enable IMU data
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Enabling IMU data on controller %d (%s)...\n"), this->intHandle, *this->name);
	if (controller_type == ControllerType::s_ds4)
	{
		// These two branches used to be the wrong way round. It went unnoticed because is_usb was itself
		// inverted for every Bluetooth DS4 (see IsBluetoothHidPath), so the two mistakes cancelled out; now
		// that the transport is detected properly, re-running the init for the wrong transport here would
		// leave a Bluetooth controller silent.
		if (is_usb)
		{
			init_ds4_usb();
		}
		else
		{
			init_ds4_bt();
			enable_gyro_ds4_bt(buf, bufLength);
		}
		return true;
	}

	if (!is_usb)
	{
		// Two Joy-Cons share one radio, so this is where a dropped subcommand actually happens -- and a
		// lost enable leaves a controller that streams buttons with zeroed IMU for the whole session.
		const uint8_t enabled = 0x01;
		return send_subcommand_with_ack(0x40, &enabled, 1);
	}

	// USB does not drop packets, and the 0x80 0x92-framed reply layout has not been verified against the
	// 0x21 ack match, so the USB path keeps the historical fire-and-forget send.
	buf[0] = 0x01; // Enabled
	send_subcommand(0x1, 0x40, buf, 1);
	return true;
}
bool JoyShock::init_usb() {
	unsigned char buf[0x400];
	memset(buf, 0, 0x400);

	// set blocking:
	// this insures we get the MAC Address
	hid_set_nonblocking(this->handle, 0);

	//Get MAC Left
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Getting MAC...\n"));
	memset(buf, 0x00, 0x40);
	buf[0] = 0x80;
	buf[1] = 0x01;
	hid_exchange(this->handle, buf, 0x2);

	//if (buf[2] == 0x3) {
	//	UE_LOG(LogJoyShockLibrary, Log, TEXT("%s disconnected!\n", this->name.c_str()));
	//}
	//else {
	//	UE_LOG(LogJoyShockLibrary, Log, TEXT("Found %s, MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", this->name.c_str(), buf[9], buf[8], buf[7], buf[6], buf[5], buf[4]));
	//}

	// set non-blocking:
	//hid_set_nonblocking(jc->handle, 1);

	// Do handshaking
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Doing handshake...\n"));
	memset(buf, 0x00, 0x40);
	buf[0] = 0x80;
	buf[1] = 0x02;
	hid_exchange(this->handle, buf, 0x2);

	// Switch baudrate to 3Mbit
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Switching baudrate...\n"));
	memset(buf, 0x00, 0x40);
	buf[0] = 0x80;
	buf[1] = 0x03;
	hid_exchange(this->handle, buf, 0x2);

	//Do handshaking again at new baudrate so the firmware pulls pin 3 low?
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Doing handshake...\n"));
	memset(buf, 0x00, 0x40);
	buf[0] = 0x80;
	buf[1] = 0x02;
	hid_exchange(this->handle, buf, 0x2);

	//Only talk HID from now on
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Only talk HID...\n"));
	memset(buf, 0x00, 0x40);
	buf[0] = 0x80;
	buf[1] = 0x04;
	hid_exchange(this->handle, buf, 0x2);

	// Enable vibration
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Enabling vibration...\n"));
	memset(buf, 0x00, 0x400);
	buf[0] = 0x01; // Enabled
	send_subcommand(0x1, 0x48, buf, 1);

	enable_IMU(buf, 0x400);

	UE_LOG(LogJoyShockLibrary, Log, TEXT("Getting calibration data...\n"));
	bool result = get_switch_controller_info();

	if (result)
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("Successfully initialized %s!\n"), *this->name);
	}
	else
	{
		UE_LOG(LogJoyShockLibrary, Log, TEXT("Could not initialise %s! Will try again later.\n"), *this->name);
	}
	return result;
}
bool JoyShock::set_switch_player_lights(unsigned char playerLightMask) {
	unsigned char buf[1] = { playerLightMask };
	return send_subcommand(0x01, 0x30, buf, 1);
}
bool JoyShock::clear_switch_home_light() {
	// Subcommand 0x38 controls the blue HOME notification light independently of subcommand 0x30's
	// four green player LEDs. This is Nintendo's five-byte "steady brightness 0" pattern; an all-zero
	// payload is not an off command and can make the ring light instead.
	return set_switch_home_light(0);
}
bool JoyShock::set_switch_home_light(unsigned char intensity) {
	// Nintendo's "steady brightness" form of subcommand 0x38: with no mini cycles requested, the ring
	// simply holds the start intensity carried in the high nibble of byte 1. The remaining bytes are
	// mini-cycle timings that go unused here but still have to carry the values Nintendo sends -- an
	// all-zero payload is not an off command and can leave the ring cycling instead.
	//
	// Written without reading a reply: the polling thread is the handle's sole reader, and send_subcommand
	// blocks in hid_read_timeout for up to a second there, consuming input reports that belong to the
	// stream. That was survivable while the light was set exactly once, but it is re-asserted periodically
	// now, so it has to use the write-only path. See write_subcommand.
	const unsigned char clamped = intensity > 0x0F ? 0x0F : intensity;
	const unsigned char buf[5] = { 0x01, static_cast<unsigned char>(clamped << 4), 0x00, 0x11, 0x11 };
	return write_subcommand(0x38, buf, sizeof(buf));
}
void JoyShock::set_switch_rumble(int smallRumble, int bigRumble) {
	// Switch 1 (Joy-Con / Pro Controller) HD rumble: output report 0x10, followed by a 4-bit packet
	// counter and 4 bytes of rumble data per side (left, right). Fixed frequencies (320 Hz high /
	// 160 Hz low -- the neutral encoding 00 01 40 40) with amplitudes encoded per dekuNukem's
	// Nintendo_Switch_Reverse_Engineering rumble tables:
	//   code = log2(amp * 17) * 16   for 0.12 < amp <= 0.23
	//   code = log2(amp * 8.7) * 32  for amp > 0.23
	//   HF amplitude byte = code * 2; LF amplitude u16 = 0x0040 + code/2, +0x8000 when code is odd.
	auto EncodeSide = [](float hfAmp, float lfAmp, unsigned char* out)
	{
		auto AmpToCode = [](float amp) -> int
		{
			if (amp <= 0.007f)
			{
				return 0;
			}
			float code;
			if (amp <= 0.23f)
			{
				code = log2f(amp * 17.0f) * 16.0f;
			}
			else
			{
				code = log2f(amp * 8.7f) * 32.0f;
			}
			return FMath::Clamp(FMath::RoundToInt(code), 0, 100);
		};

		const int hfCode = AmpToCode(hfAmp);
		const int lfCode = AmpToCode(lfAmp);
		const uint16_t lfEncoded = 0x0040 + (lfCode >> 1) + ((lfCode & 1) ? 0x8000 : 0);

		out[0] = 0x00;                                            // HF 320 Hz (low byte)
		out[1] = static_cast<unsigned char>(0x01 + hfCode * 2);   // HF 320 Hz (high byte) + HF amplitude
		out[2] = static_cast<unsigned char>(0x40 + (lfEncoded >> 8)); // LF 160 Hz + LF amplitude (high)
		out[3] = static_cast<unsigned char>(lfEncoded & 0xFF);    // LF amplitude (low)
	};

	const float hfAmp = FMath::Clamp(smallRumble, 0, 255) / 255.0f;
	const float lfAmp = FMath::Clamp(bigRumble, 0, 255) / 255.0f;

	unsigned char buf[10];
	buf[0] = 0x10;
	buf[1] = static_cast<unsigned char>((++global_count) & 0xF);
	if (global_count > 0xF)
	{
		global_count = 0x0;
	}
	EncodeSide(hfAmp, lfAmp, buf + 2); // left actuator
	EncodeSide(hfAmp, lfAmp, buf + 6); // right actuator

	// Plain hid_write: report 0x10 has no reply, and a blocking read here would fight the poll thread.
	const int res = hid_write(this->handle, buf, sizeof(buf));
	note_output_result(OutputFunctionRumble, res >= 0);
}
bool JoyShock::init_bt() {
	bool result = true;
	unsigned char buf[0x40];
	memset(buf, 0, 0x40);
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Initialising Bluetooth connection...\n"));

	// set blocking to ensure command is recieved:
	hid_set_nonblocking(this->handle, 0);

	// first, check if this is a USB connection
	buf[0] = 0x80;
	buf[1] = 0x01;
	hid_write(this->handle, buf, 2);
	// wait for up to 5 messages for a USB acknowledgement
	for (int idx = 0; idx < 5; idx++)
	{
		if (hid_read_timeout(this->handle, buf, 0x40, 200) && buf[0] == 0x81)
		{
			//UE_LOG(LogJoyShockLibrary, Log, TEXT("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			//	buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10]);
			UE_LOG(LogJoyShockLibrary, Log, TEXT("Attempting USB connection\n"));

			// it's usb!
			is_usb = true;

			init_usb();
			return 1;

			break;
		}
		//UE_LOG(LogJoyShockLibrary, Log, TEXT("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n"),
		//	buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10]);
		UE_LOG(LogJoyShockLibrary, Log, TEXT("Not a USB response...\n"));
	}
	memset(buf, 0, 0x40);
	//if (hid_exchange(this->handle, buf, 2))
	//{
	//	UE_LOG(LogJoyShockLibrary, Log, TEXT("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n"),
	//		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10]);
	//	UE_LOG(LogJoyShockLibrary, Log, TEXT("Attempting USB connection\n"));
	//	// it's usb!
	//	is_usb = true;
	//
	//	init_usb();
	//	return 1;
	//}
	buf[1] = 0x00;

	// Enable vibration
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Enabling vibration...\n"));
	buf[0] = 0x01; // Enabled
	send_subcommand(0x1, 0x48, buf, 1);

	//UE_LOG(LogJoyShockLibrary, Log, TEXT("Set vibration\n"));

	// Enable IMU data
	const bool imuEnabled = enable_IMU(buf, 0x40);
	if (!imuEnabled)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("Controller %d (%s) never acknowledged the IMU enable; leaving init unfinished so the next enumeration pass retries it.\n"),
			this->intHandle, *this->name);
	}

	// Set input report mode (to push at 60hz)
	// x00	Active polling mode for IR camera data. Answers with more than 300 bytes ID 31 packet
	// x01	Active polling mode
	// x02	Active polling mode for IR camera data.Special IR mode or before configuring it ?
	// x21	Unknown.An input report with this ID has pairing or mcu data or serial flash data or device info
	// x23	MCU update input report ?
	// 30	NPad standard mode. Pushes current state @60Hz. Default in SDK if arg is not in the list
	// 31	NFC mode. Pushes large packets @60Hz
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Set input report mode to 0x30...\n"));
	const uint8_t reportMode = 0x30;
	const bool modeSet = send_subcommand_with_ack(0x03, &reportMode, 1);
	if (!modeSet)
	{
		UE_LOG(LogJoyShockLibrary, Warning,
			TEXT("Controller %d (%s) never acknowledged report mode 0x30; leaving init unfinished so the next enumeration pass retries it.\n"),
			this->intHandle, *this->name);
	}

	// @CTCaer

	// get calibration data:
	UE_LOG(LogJoyShockLibrary, Log, TEXT("Getting calibration data...\n"));
	result = get_switch_controller_info();

	// Run every step before judging success: even a partially configured controller should stream what it
	// can (buttons keep working exactly as they did when these sends were fire-and-forget). Reporting the
	// failure is what matters -- it keeps `initialised` false so the next enumeration pass re-runs init,
	// instead of a lost subcommand silently costing the IMU for the whole session.
	return result && imuEnabled && modeSet;
}
void JoyShock::deinit_usb() {
	unsigned char buf[0x40];
	memset(buf, 0x00, 0x40);

	//Let the Joy-Con talk BT again    
	buf[0] = 0x80;
	buf[1] = 0x05;

	hid_set_nonblocking(this->handle, 1);
	hid_write(handle, buf, 0x2);

	initialised = false;
}
//// mfosse credits Hypersect (Ryan Juckett), but I've removed deadzones so the consuming application can deal with them
//// http://blog.hypersect.com/interpreting-analog-sticks/
void JoyShock::CalcAnalogStick2
(
	float &pOutX,       // out: resulting stick X value
	float &pOutY,       // out: resulting stick Y value
	uint16_t x,              // in: initial stick X value
	uint16_t y,              // in: initial stick Y value
	uint16_t x_calc[3],      // calc -X, CenterX, +X
	uint16_t y_calc[3]       // calc -Y, CenterY, +Y
)
{

	float x_f, y_f;

	// convert to float based on calibration and valid ranges per +/-axis
	x = FMath::Clamp(x, x_calc[0], x_calc[2]);
	y = FMath::Clamp(y, y_calc[0], y_calc[2]);
	if (x >= x_calc[1]) {
		x_f = (float)(x - x_calc[1]) / (float)(x_calc[2] - x_calc[1]);
	}
	else {
		x_f = -((float)(x - x_calc[1]) / (float)(x_calc[0] - x_calc[1]));
	}
	if (y >= y_calc[1]) {
		y_f = (float)(y - y_calc[1]) / (float)(y_calc[2] - y_calc[1]);
	}
	else {
		y_f = -((float)(y - y_calc[1]) / (float)(y_calc[0] - y_calc[1]));
	}

	pOutX = x_f;
	pOutY = y_f;
}
// SPI (@CTCaer):
bool JoyShock::get_spi_data(uint32_t offset, const uint8_t read_len, uint8_t *test_buf) {
	int res;
	uint8_t buf[0x100];
	// Bounded retries: once the controller is in report mode 0x30 it streams input at 60Hz, so
	// hid_read_timeout almost always returns *something* -- usually an ordinary input report rather than
	// our SPI reply, and most of these attempts are spent skipping past those. If the SPI response itself
	// is lost (flaky Bluetooth), an unbounded loop here spins forever -- while the enumeration thread holds
	// the exclusive connected lock, freezing connects AND (via the shared lock) the game thread.
	int timeouts = 0;
	for (int attempt = 0; attempt < 32; attempt++) {
		memset(buf, 0, sizeof(buf));
		auto hdr = (brcm_hdr *)buf;
		auto pkt = (brcm_cmd_01 *)(hdr + 1);
		hdr->cmd = 1;
		hdr->rumble[0] = timing_byte;

		buf[1] = timing_byte;

		timing_byte++;
		if (timing_byte > 0xF) {
			timing_byte = 0x0;
		}
		pkt->subcmd = 0x10;
		pkt->offset = offset;
		pkt->size = read_len;

		for (int i = 11; i < 22; ++i) {
			buf[i] = buf[i + 3];
		}

		res = hid_write(handle, buf, sizeof(*hdr) + sizeof(*pkt));

		res = hid_read_timeout(handle, buf, sizeof(buf), 1000);
		if (res < 0)
		{
			// The device is gone -- retrying can't bring it back.
			return false;
		}
		if (res == 0)
		{
			// Nothing came back this time. On a busy Bluetooth link that is transient, but it used to be
			// treated as fatal, which threw away the 32 retries below and failed the read on a single
			// hiccup. Retry -- with its own small bound, so a genuinely silent controller can't hold the
			// connected lock for 32 seconds.
			if (++timeouts >= 3)
			{
				UE_LOG(LogJoyShockLibrary, Warning, TEXT("SPI read at 0x%04X timed out %d times; giving up.\n"), offset, timeouts);
				return false;
			}
			continue;
		}

		if ((*(uint16_t*)&buf[0xD] == 0x1090) && (*(uint32_t*)&buf[0xF] == offset)) {
			if (res >= 0x14 + read_len) {
				for (int i = 0; i < read_len; i++) {
					test_buf[i] = buf[0x14 + i];
				}
			}
			return true;
		}
	}
	return false;
}
int32 JoyShock::write_spi_data(uint32_t offset, const uint8_t write_len, uint8_t* test_buf) {
	int res;
	uint8_t buf[0x100];
	int error_writing = 0;
	while (1) {
		memset(buf, 0, sizeof(buf));
		auto hdr = (brcm_hdr *)buf;
		auto pkt = (brcm_cmd_01 *)(hdr + 1);
		hdr->cmd = 1;
		hdr->rumble[0] = timing_byte;
		timing_byte++;
		if (timing_byte > 0xF) {
			timing_byte = 0x0;
		}
		pkt->subcmd = 0x11;
		pkt->offset = offset;
		pkt->size = write_len;
		for (int i = 0; i < write_len; i++) {
			buf[0x10 + i] = test_buf[i];
		}
		res = hid_write(handle, buf, sizeof(*hdr) + sizeof(*pkt) + write_len);

		// Bounded read: a blocking hid_read here can hang forever if the device stops responding.
		res = hid_read_timeout(handle, buf, sizeof(buf), 1000);
		if (res <= 0) {
			return 1;
		}

		if (*(uint16_t*)&buf[0xD] == 0x1180)
			break;

		error_writing++;
		if (error_writing == 125) {
			return 1;
		}
	}

	return 0;

}
