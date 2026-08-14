// JoyShock_Sony.cpp - The DualShock 4 and DualSense protocol.
//
// Sony's controllers need no handshake to start reporting; the work is in the output reports, which carry
// rumble, the light bar and the player LEDs together in one packet whose layout differs between USB and
// Bluetooth -- and which, over Bluetooth, must carry a CRC-32 the controller checks.
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

//// https://docs.microsoft.com/en-us/openspecs/office_protocols/ms-abs/06966aa2-70da-4bf9-8448-3355f277cd77
uint32_t JoyShock::crc_32(unsigned char* buf, int length) {
	uint32_t result = 0xFFFFFFFF;
	int index = 0;
	while (index < length) {
		result = crc_table[(result & 0xFF) ^ buf[index]] ^ (result >> 8);
		index++;
	}
	return result ^ 0xFFFFFFFF;
}
void JoyShock::enable_gyro_ds4_bt(unsigned char *buf, int bufLength)
{
	// gyro is enabled by getting feature report 0x05 on BT controllers.
	// in addition, this request is also responsible for getting current calibration info.
	buf[0] = 0x05; // controller calibration request for BT

	hid_get_feature_report(handle, buf, 41);
	//hid_write(handle, buf, 38);
	//hid_read_timeout(handle, buf, bufLength, 100);
}
void JoyShock::init_ds4_bt() {
	UE_LOG(LogJoyShockLibrary, Log, TEXT("initialise, set colour\n"));
	unsigned char buf[78];
	memset(buf, 0, 78);

	//buf[0] = 0x11;
	//buf[1] = 0x80;
	//buf[3] = 0xff;

	// https://github.com/Ryochan7/DS4Windows/blob/jay/DS4Windows/DS4Library/DS4Device.cs
	buf[0] = 0x15;
	buf[1] = 0xC0 | 1;
	buf[2] = 0xA0;
	buf[3] = 0xf7;
	buf[4] = 0x04;

	//// https://github.com/chrippa/ds4drv/blob/master/ds4drv/device.py
	//buf[0] = 0xa2; // 0x80;
	////buf[1] = 0xff;
	//// trying to do colour stuff
	//// http://eleccelerator.com/wiki/index.php?title=DualShock_4
	//// this is only for bt
	//buf[1] = 0x11;
	//buf[2] = 0xc0;
	//buf[3] = 0x20;
	//buf[4] = 0xf3;
	//buf[5] = 0x04;
	//// rumble
	//buf[7] = 0xFF;
	//buf[8] = 0x00;
	//// colour
	//buf[9] = 0x00;
	//buf[10] = 0x00;
	//buf[11] = 0x00;
	//// flash time
	//buf[12] = 0xff;
	//buf[13] = 0x00;
	//// now we need a CRC-32 of previous bytes
	//uint32_t crc = crc_32(buf, 75);
	//buf[75] = (crc >> 24) & 0xFF;
	//buf[76] = (crc >> 16) & 0xFF;
	//buf[77] = (crc >> 8) & 0xFF;
	//buf[78] = crc & 0xFF;

	//// https://github.com/chrippa/ds4drv/blob/master/ds4drv/device.py
	//buf[0] = 0x80;
	//buf[1] = 0xff;
	//// trying to do colour stuff
	//// http://eleccelerator.com/wiki/index.php?title=DualShock_4
	//// this is only for bt
	//buf[2] = 0x11;
	//// rumble
	//buf[6] = 0xFF;
	//buf[7] = 0xFF;	
	//// colour
	//buf[8] = 0xFF; // 0x00;
	//buf[9] = 0x80; // 0x00;
	//buf[10] = 0x00;
	//// flash time
	//buf[11] = 0xff;
	//buf[12] = 0x00;
	//// now we need a CRC-32 of previous bytes
	//uint32_t crc = crc_32(buf, 75);
	//buf[75] = (crc >> 24) & 0xFF;
	//buf[76] = (crc >> 16) & 0xFF;
	//buf[77] = (crc >> 8) & 0xFF;
	//buf[78] = crc & 0xFF;

	// set blocking:
	// this insures we get the MAC Address
	hid_set_nonblocking(this->handle, 0);

	hid_write(handle, buf, 78);

	// initialise stuff
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
	stick_cal_x_l[0] =
		stick_cal_y_l[0] =
		stick_cal_x_r[0] =
		stick_cal_y_r[0] = 0;
	stick_cal_x_l[1] =
		stick_cal_y_l[1] =
		stick_cal_x_r[1] =
		stick_cal_y_r[1] = 127;
	stick_cal_x_l[2] =
		stick_cal_y_l[2] =
		stick_cal_x_r[2] =
		stick_cal_y_r[2] = 255;
	//// Acc cal origin position
	//sensor_cal[0][0] = 0;
	//sensor_cal[0][1] = 0;
	//sensor_cal[0][2] = 0;
	//
	//// Gyro cal origin position
	//sensor_cal[1][0] = 0;
	//sensor_cal[1][1] = 0;
	//sensor_cal[1][2] = 0;

	enable_gyro_ds4_bt(buf, 78);

	initialised = true;
}
// placeholder to get things working quickly. overdue for a refactor
void JoyShock::init_ds_usb() {
	// initialise stuff
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
	stick_cal_x_l[0] =
		stick_cal_y_l[0] =
		stick_cal_x_r[0] =
		stick_cal_y_r[0] = 0;
	stick_cal_x_l[1] =
		stick_cal_y_l[1] =
		stick_cal_x_r[1] =
		stick_cal_y_r[1] = 127;
	stick_cal_x_l[2] =
		stick_cal_y_l[2] =
		stick_cal_x_r[2] =
		stick_cal_y_r[2] = 255;

	initialised = true;
}
// this is mostly copied from init_usb() below, but modified to speak DS4
void JoyShock::init_ds4_usb() {
	unsigned char buf[31];
	memset(buf, 0, 31);

	// report id?
	buf[0] = 0x05;
	// I dunno what this is
	buf[1] = 0xf7;
	buf[2] = 0x04;
	//// http://eleccelerator.com/wiki/index.php?title=DualShock_4
	//// https://github.com/chrippa/ds4drv/blob/master/ds4drv/device.py
	//// rumble
	//buf[4] = 0x00;
	//buf[5] = 0x00;
	//// colour
	//buf[6] = 0x00;
	////buf[7] = 0xff;
	//buf[7] = 0x00;
	//buf[8] = 0x00;
	//// flash time
	//buf[9] = 0xff;
	//buf[10] = 0x00;
	// now we need a CRC-32 of previous bytes
	//uint32_t = crc_32(buf, 75);
	//buf[75] = 

	// set blocking:
	// this insures we get the MAC Address
	hid_set_nonblocking(this->handle, 0);

	hid_write(handle, buf, 31);

	// initialise stuff
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
	stick_cal_x_l[0] =
		stick_cal_y_l[0] =
		stick_cal_x_r[0] =
		stick_cal_y_r[0] = 0;
	stick_cal_x_l[1] =
		stick_cal_y_l[1] =
		stick_cal_x_r[1] =
		stick_cal_y_r[1] = 127;
	stick_cal_x_l[2] =
		stick_cal_y_l[2] =
		stick_cal_x_r[2] =
		stick_cal_y_r[2] = 255;
	//// Acc cal origin position
	//sensor_cal[0][0] = 0;
	//sensor_cal[0][1] = 0;
	//sensor_cal[0][2] = 0;
	//
	//// Gyro cal origin position
	//sensor_cal[1][0] = 0;
	//sensor_cal[1][1] = 0;
	//sensor_cal[1][2] = 0;

	initialised = true;
}
void JoyShock::deinit_ds4_bt() {
	// TODO. For now, init, which stops rumbling and disables light
	init_ds4_bt();

	initialised = false;
}
// TODO: implement this
void JoyShock::deinit_ds4_usb() {
	unsigned char buf[40];
	memset(buf, 0, 40);

	// report id?
	buf[0] = 0x05;
	// don't know what this is
	buf[1] = 0xff;
	// rumble
	buf[4] = 0x00;
	buf[5] = 0x00;
	// colour
	buf[6] = 0x00;
	buf[7] = 0x00;
	buf[8] = 0x00;
	// flash time
	buf[9] = 0x00;
	buf[10] = 0x00;
	// now we need a CRC-32 of previous bytes
	//uint32_t = crc_32(buf, 75);
	//buf[75] = 

	// set non-blocking
	hid_set_nonblocking(this->handle, 1);

	hid_write(handle, buf, 31);

	initialised = false;
}
void JoyShock::set_ds5_rumble_light(unsigned char smallRumble, unsigned char bigRumble,
                       unsigned char colourR,
                       unsigned char colourG,
                       unsigned char colourB,
                       unsigned char playerlights) {
    if(!is_usb) {
        set_ds5_rumble_light_bt(smallRumble, bigRumble, colourR, colourG, colourB, playerlights);
    }
    else {
        set_ds5_rumble_light_usb(smallRumble, bigRumble, colourR, colourG, colourB, playerlights);
    }

}
void JoyShock::set_ds4_rumble_light(unsigned char smallRumble, unsigned char bigRumble,
	unsigned char colourR,
	unsigned char colourG,
	unsigned char colourB) {
	if (!is_usb) {
		set_ds4_rumble_light_bt(smallRumble, bigRumble, colourR, colourG, colourB);
	}
	else {
		set_ds4_rumble_light_usb(smallRumble, bigRumble, colourR, colourG, colourB);
	}
}
void JoyShock::set_ds4_rumble_light_usb(unsigned char smallRumble, unsigned char bigRumble,
	unsigned char colourR,
	unsigned char colourG,
	unsigned char colourB) {
	// todo: based on bluetoothness, switch report id to 0x11, offset everything by 2 -- basically use init stuff as basis
	unsigned char buf[40];
	memset(buf, 0, 40);

	// report id?
	buf[0] = 0x05;
	// don't know what this is
	buf[1] = 0xff;
	// rumble
	buf[4] = smallRumble;
	buf[5] = bigRumble;
	// colour
	buf[6] = colourR;
	buf[7] = colourG;
	buf[8] = colourB;
	// flash time
	buf[9] = 0xff;
	buf[10] = 0x00;
	// now we need a CRC-32 of previous bytes
	//uint32_t = crc_32(buf, 75);
	//buf[75] =

	// Rumble, light bar and player identification share this one report, so a failed write blocks them all.
	note_output_result(OutputFunctionRumble | OutputFunctionPlayerIndicator,
		hid_write(handle, buf, 31) >= 0);
}
void JoyShock::set_ds4_rumble_light_bt(unsigned char smallRumble, unsigned char bigRumble,
	unsigned char colourR,
	unsigned char colourG,
	unsigned char colourB) {
	unsigned char buf[79];
	memset(buf, 0, 79);

	// https://github.com/chrippa/ds4drv/blob/master/ds4drv/device.py
	//buf[0] = 0xa2; // 0x80;
	//buf[1] = 0xff;
	// trying to do colour stuff
	// http://eleccelerator.com/wiki/index.php?title=DualShock_4
	// this is only for bt

	buf[0] = 0xa2; // Output report header, needs to be included in crc32
	buf[1] = 0x11; // Output report 0x11
	buf[2] = 0xc0; // HID + CRC according to hid-sony
	buf[3] = 0x20; // ????
	buf[4] = 0x07; // Set blink + leds + motor
	buf[5] = 0x00;
	buf[6] = 0x00;
	// rumble
	buf[7] = smallRumble;
	buf[8] = bigRumble;
	// colour
	buf[9] = colourR;
	buf[10] = colourG;
	buf[11] = colourB;
	// flash time
	buf[12] = 0xff;
	buf[13] = 0x00;
	// now we need a CRC-32 of previous bytes

	/*
	// test
    buf[0] = 0xa2; // Output report header, needs to be included in crc32
    buf[1] = 0x11; // Output report 0x11
    buf[2] = 0xc0; // HID + CRC according to hid-sony
    buf[3] = 0x00; // ????
    buf[4] = 0x07; // Set blink + leds + motor
    buf[5] = 0x00;
    buf[6] = 0x00;
    buf[7] = 0xff;
    buf[8] = 0xff;
    buf[9] = 0xff;
    buf[10] = 0xff;
    buf[11] = 0xff;
    buf[12] = 0xff;
    buf[22] = 0x43;
    buf[23] = 0x43;
    buf[25] = 0x4d;
    buf[26] = 0x85;
*/

	uint32_t crc = crc_32(buf, 75);
	memcpy(&buf[75], &crc, 4);
	//buf[75] = (crc >> 24) & 0xFF;
	//buf[76] = (crc >> 16) & 0xFF;
	//buf[77] = (crc >> 8) & 0xFF;
	//buf[78] = crc & 0xFF;

	// Rumble, light bar and player identification share this one report, so a failed write blocks them all.
	note_output_result(OutputFunctionRumble | OutputFunctionPlayerIndicator,
		hid_write(handle, &buf[1], 78) >= 0);
}
void JoyShock::set_ds5_rumble_light_usb(unsigned char smallRumble, unsigned char bigRumble,
                             unsigned char colourR,
                             unsigned char colourG,
                             unsigned char colourB,
                             unsigned char playerlights) { // DS5 actually has player lights.
    unsigned char buf[79];
    memset(buf, 0, 79);

    // https://github.com/Ryochan7/DS4Windows/blob/jay/DS4Windows/DS4Library/InputDevices/DualSenseDevice.cs
    // DS4Windows to the rescue.
    // Also thanks to Neilk1 for sharing his doc on the DS5 protocol.

    // Header & Report Information
    buf[0] = 0xa2; // Output report header, needs to be included in crc32
    buf[1] = 0x02; // DualSense output report is 0x02 for USB
    //buf[1] = 0x02; // DATA (0x02)


    buf[2] = 0x03;

    buf[3] = 0x54; // Toggle LED Strips, player lights, motor effect. Ignore Mic LED

    // Rumble emulation bytes.
    buf[4] = smallRumble;
    buf[5] = bigRumble;

    // 7-10 are mostly just audio settings.

    // Mute Button state. 0x00 = off, 0x01 = solid, 0x02 = pulsating.
    buf[10] = 0x00;

    // Skip to about 41, since we are ignoring trigger effect data.
    // Enable LED brightness
    buf[40] = 0x02; // ???
    buf[41] = 0x02;
    buf[44] = 0x02;

    // Controls the player lights, which the DS5 has.
    // Last two bits are unused - unset them to avoid issues.
    buf[45] = playerlights;
    buf[45] &= ~(1 << 7);
    buf[45] &= ~(1 << 8);

    // colour
    buf[46] = colourR;
    buf[47] = colourG;
    buf[48] = colourB;

    // USB does not send CRC32

    //uint32_t crc = crc_32(buf, 74);
    //memcpy(&buf[74], &crc, 4);
    //buf[75] = (crc >> 24) & 0xFF;
    //buf[76] = (crc >> 16) & 0xFF;
    //buf[77] = (crc >> 8) & 0xFF;
    //buf[78] = crc & 0xFF;

    // Rumble, LEDs and player lights share this one report, so a failed write blocks them all.
    note_output_result(OutputFunctionRumble | OutputFunctionPlayerIndicator,
        hid_write(handle, &buf[1], 74) >= 0);
}
// Calling the Dualsense anything but the DS5 is confusing, since DS also = DualShock, and the DualSense is the PS5 Controller anyway
void JoyShock::set_ds5_rumble_light_bt(unsigned char smallRumble, unsigned char bigRumble,
                             unsigned char colourR,
                             unsigned char colourG,
                             unsigned char colourB,
                             unsigned char playerlights) { // DS5 actually has player lights.
    unsigned char buf[79];
    memset(buf, 0, 79);

    // https://github.com/Ryochan7/DS4Windows/blob/jay/DS4Windows/DS4Library/InputDevices/DualSenseDevice.cs
    // DS4Windows to the rescue.
    // Also thanks to Neilk1 for sharing his doc on the DS5 protocol.

    // Header & Report Information
    buf[0] = 0xa2; // Output report header, needs to be included in crc32
    buf[1] = 0x31; // DualSense output report is 0x31
    buf[2] = 0x02; // DATA (0x02)

    // Comment stolen from DS4Windows:
    // 0x01 Set the main motors (also requires flag 0x02)
    // 0x02 Set the main motors (also requires flag 0x01)
    // 0x04 Set the right trigger motor
    // 0x08 Set the left trigger motor
    // 0x10 Enable modification of audio volume
    // 0x20 Enable internal speaker (even while headset is connected)
    // 0x40 Enable modification of microphone volume
    // 0x80 Enable internal mic (even while headset is connected)
    buf[3] = 0x03;

    // Comment stolen from DS4Windows:
    // 0x01 Toggling microphone LED, 0x02 Toggling Audio/Mic Mute
    // 0x04 Toggling LED strips on the sides of the Touchpad, 0x08 Turn off all LED lights
    // 0x10 Toggle player LED lights below Touchpad, 0x20 ???
    // 0x40 Adjust overall motor/effect power, 0x80 ???
    buf[4] = 0x54; // Toggle LED Strips, player lights, motor effect. Ignore Mic LED

    // Rumble emulation bytes.
    buf[5] = smallRumble;
    buf[6] = bigRumble;

    // 7-10 are mostly just audio settings.

    // Mute Button state. 0x00 = off, 0x01 = solid, 0x02 = pulsating.
    buf[11] = 0x00;

    // Skip to about 41, since we are ignoring trigger effect data.
    // Enable LED brightness
    buf[41] = 0x02; // ???
    buf[44] = 0x02;
    buf[45] = 0x02;

    // Last two bits are unused - unset them to avoid issues.
    buf[46] = playerlights;
    buf[46] &= ~(1 << 7);
    buf[46] &= ~(1 << 8);

    // colour
    buf[47] = colourR;
    buf[48] = colourG;
    buf[49] = colourB;

    uint32_t crc = crc_32(buf, 75);
    memcpy(&buf[75], &crc, 4);
    //buf[75] = (crc >> 24) & 0xFF;
    //buf[76] = (crc >> 16) & 0xFF;
    //buf[77] = (crc >> 8) & 0xFF;
    //buf[78] = crc & 0xFF;

    // Rumble, LEDs and player lights share this one report, so a failed write blocks them all.
    note_output_result(OutputFunctionRumble | OutputFunctionPlayerIndicator,
        hid_write(handle, &buf[1], 78) >= 0);
}
