#include "InputHelpers.h"
#include "JoyShockLibrary.h"
#include "JoyShock.h"

// #include <cmath>

// Buckets a percentage into the coarse levels the plugin exposes (0 = empty ... 4 = full). Shared by
// every controller family that reports a percentage, so a DualShock 4 and a DualSense at the same charge
// report the same level even though they encode it differently.
static uint8_t BatteryLevelFromPercent(int32 Percent)
{
	return Percent >= 80 ? 4 : Percent >= 50 ? 3 : Percent >= 25 ? 2 : Percent >= 10 ? 1 : 0;
}

// Holds a controller's buttons at rest for the moment it arrives.
//
// A controller that has just connected is usually still holding the button that woke it -- over Bluetooth
// that press IS the reason it is here -- and reporting that frame delivers it as if the player had made it.
// On a Joy-Con that is worse than a stray press: the wake button is typically a shoulder or a rail button,
// which is exactly what the grip chords watch, so a controller could join or split a pair merely by being
// switched on.
//
// So the buttons are forced to rest until a report arrives with nothing held, or until a short deadline
// passes -- the deadline being what keeps a player who genuinely is holding a button at connect from being
// locked out for good. Once the gate lifts, the still-held button reads as a fresh press, which is the
// right answer for one the player is deliberately holding.
//
// Only the buttons: the report is still published, so the controller is announced to the engine on its
// first frame as before rather than a second late, and its sticks and motion are live throughout. The
// previous state is blanked alongside the current one so lifting the gate cannot look like a release.
static void settle_input(JoyShock* jc)
{
	if (jc->input_settled)
	{
		return;
	}

	if (jc->simple_state.buttons == 0 || std::chrono::steady_clock::now() >= jc->input_settle_deadline)
	{
		jc->input_settled = true;
		return;
	}

	jc->simple_state.buttons = 0;
	jc->simple_state.lTrigger = 0.f;
	jc->simple_state.rTrigger = 0.f;
	jc->last_simple_state.buttons = 0;
	jc->last_simple_state.lTrigger = 0.f;
	jc->last_simple_state.rTrigger = 0.f;
}

	// The Nintendo Switch 2 controllers: their own 64-byte report (id 0x05), decoded from raw dumps + a USB
	// capture of Steam. The protocol differs from the Switch 1's, so the family gets a dedicated parser.
	//
	// One report layout serves all three shapes -- Pro Controller 2, Joy-Con 2 (L) and Joy-Con 2 (R). A
	// controller simply leaves at zero every field it has no hardware for, which is why this reads as one
	// decoder with per-half filtering rather than three parsers. The filtering is not belt-and-braces: the
	// halves' button sets must stay disjoint for a joined pair to combine into one player, which is the
	// contract the interface's pairing code is written against.
//
// Returns false for a report that is not a valid input frame; handle_input passes that straight out.
static bool parse_switch2_report(JoyShock* jc, uint8_t* packet, int32 len, FIMUState& imu_state, bool& hasIMU)
{
	if (packet[0] != 0x05 || len < 17)
	{
		return false;
	}

	// The button field is one little-endian 32-bit word at bytes 5-8. Its top six bits are not buttons:
	// a Joy-Con reports status there, and reading them as input is what makes one stream a permanently
	// held ZL/ZR (the Left half's bit 23 in particular).
	const uint32_t rawButtons = (uint32_t)packet[5] | ((uint32_t)packet[6] << 8)
		| ((uint32_t)packet[7] << 16) | ((uint32_t)packet[8] << 24);
	const uint32_t sw2 = rawButtons & 0x03FFFFFF;

	// Named for the controller's own layout rather than JSL's, so the mapping below reads as a
	// translation instead of as two sets of magic numbers. SL and SR appear twice because they are
	// physically different buttons -- the rail buttons on each half -- that JSL folds onto one pair.
	enum : uint32_t
	{
		SW2_Y        = 0x00000001, SW2_X       = 0x00000002, SW2_B        = 0x00000004, SW2_A     = 0x00000008,
		SW2_SR_RIGHT = 0x00000010, SW2_SL_RIGHT= 0x00000020, SW2_R        = 0x00000040, SW2_ZR    = 0x00000080,
		SW2_MINUS    = 0x00000100, SW2_PLUS    = 0x00000200, SW2_RCLICK   = 0x00000400, SW2_LCLICK= 0x00000800,
		SW2_HOME     = 0x00001000, SW2_CAPTURE = 0x00002000, SW2_C        = 0x00004000,
		SW2_DOWN     = 0x00010000, SW2_UP      = 0x00020000, SW2_RIGHT    = 0x00040000, SW2_LEFT  = 0x00080000,
		SW2_SR_LEFT  = 0x00100000, SW2_SL_LEFT = 0x00200000, SW2_L        = 0x00400000, SW2_ZL    = 0x00800000,
		SW2_GR       = 0x01000000, SW2_GL      = 0x02000000,
	};

	const bool bLeftHalf = jc->left_right == 1;
	const bool bRightHalf = jc->left_right == 2;
	const bool bWhole = !bLeftHalf && !bRightHalf;

	int32 buttons = 0;
	// Everything on the right-hand side of a controller: the face buttons, the right shoulders, and the
	// system buttons that live on that half.
	if (bWhole || bRightHalf)
	{
		if (sw2 & SW2_A) buttons |= JSMASK_E;      // A sits right
		if (sw2 & SW2_B) buttons |= JSMASK_S;      // B sits bottom
		if (sw2 & SW2_X) buttons |= JSMASK_N;      // X sits top
		if (sw2 & SW2_Y) buttons |= JSMASK_W;      // Y sits left
		if (sw2 & SW2_R) buttons |= JSMASK_R;
		if (sw2 & SW2_ZR) buttons |= JSMASK_ZR;
		if (sw2 & SW2_PLUS) buttons |= JSMASK_PLUS;
		if (sw2 & SW2_RCLICK) buttons |= JSMASK_RCLICK;
		if (sw2 & SW2_HOME) buttons |= JSMASK_HOME;
		if (sw2 & SW2_C) buttons |= JSMASK_C;      // "C" (GameChat)
	}
	// And everything on the left: d-pad, left shoulders, and its system buttons.
	if (bWhole || bLeftHalf)
	{
		if (sw2 & SW2_UP) buttons |= JSMASK_UP;
		if (sw2 & SW2_DOWN) buttons |= JSMASK_DOWN;
		if (sw2 & SW2_LEFT) buttons |= JSMASK_LEFT;
		if (sw2 & SW2_RIGHT) buttons |= JSMASK_RIGHT;
		if (sw2 & SW2_L) buttons |= JSMASK_L;
		if (sw2 & SW2_ZL) buttons |= JSMASK_ZL;
		if (sw2 & SW2_MINUS) buttons |= JSMASK_MINUS;
		if (sw2 & SW2_LCLICK) buttons |= JSMASK_LCLICK;
		if (sw2 & SW2_CAPTURE) buttons |= JSMASK_CAPTURE;
	}
	// The rear grip buttons exist only on the Pro Controller 2.
	if (bWhole)
	{
		if (sw2 & SW2_GL) buttons |= JSMASK_GL;
		if (sw2 & SW2_GR) buttons |= JSMASK_GR;
	}
	// A detached Joy-Con's rail buttons. Each half reports its own pair in its own bits, and both fold
	// onto JSL's single SL/SR -- the same convention the Switch 1 Joy-Cons already use, which is what
	// lets the SL+SR separation chord work unchanged for both generations.
	if (bLeftHalf)
	{
		if (sw2 & SW2_SL_LEFT) buttons |= JSMASK_SL;
		if (sw2 & SW2_SR_LEFT) buttons |= JSMASK_SR;
	}
	else if (bRightHalf)
	{
		if (sw2 & SW2_SL_RIGHT) buttons |= JSMASK_SL;
		if (sw2 & SW2_SR_RIGHT) buttons |= JSMASK_SR;
	}

	jc->simple_state.buttons = buttons;

	// Sticks: two 12-bit axes packed into 3 bytes each (same layout as the Switch 1), centre ~0x800.
	// Both slots are always present; a Joy-Con fills only the one for its side and leaves the other at
	// rest, so each half is read from the slot it actually writes.
	const uint16_t lx = packet[11] | ((packet[12] & 0x0F) << 8);
	const uint16_t ly = (packet[12] >> 4) | (packet[13] << 4);
	const uint16_t rx = packet[14] | ((packet[15] & 0x0F) << 8);
	const uint16_t ry = (packet[15] >> 4) | (packet[16] << 4);

	// Use the calibration read during init when available; fall back to fixed centre/range otherwise.
	//
	// The range is per shape, from calibration actually read off hardware: a Joy-Con 2's stick travels
	// about 1200 counts from centre and a Pro Controller 2's about 1600. One figure for both is what
	// made an uncalibrated Joy-Con reach two thirds of full deflection -- felt, correctly, as a
	// character that walks slower than everyone else's. This is damage control, not calibration: it is
	// only reached when the controller would not say what its own centres are.
	const float fallbackRange = jc->is_switch2_joycon() ? 1200.0f : 1600.0f;
	auto normStickFallback = [fallbackRange](uint16_t raw) -> float
	{
		const float value = ((int32)raw - 2048) / fallbackRange;
		return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
	};
	if (!bRightHalf)
	{
		if (jc->stick_cal_x_l[2] != 0)
		{
			jc->CalcAnalogStick2(jc->simple_state.stickLX, jc->simple_state.stickLY, lx, ly,
				jc->stick_cal_x_l, jc->stick_cal_y_l);
		}
		else
		{
			jc->simple_state.stickLX = normStickFallback(lx);
			jc->simple_state.stickLY = normStickFallback(ly);
		}
	}
	if (!bLeftHalf)
	{
		if (jc->stick_cal_x_r[2] != 0)
		{
			jc->CalcAnalogStick2(jc->simple_state.stickRX, jc->simple_state.stickRY, rx, ry,
				jc->stick_cal_x_r, jc->stick_cal_y_r);
		}
		else
		{
			jc->simple_state.stickRX = normStickFallback(rx);
			jc->simple_state.stickRY = normStickFallback(ry);
		}
		// The right slot's Y axis is reported inverted relative to the left one's (confirmed on a Pro
		// Controller 2), so negate it to make up positive on both sticks.
		//
		// Only on a whole controller. A right Joy-Con's stick is normalised once, in FJoyShockInterface,
		// exactly as the Switch 1 right half's is -- the note on the Switch 1 Pro's branch further down
		// spells out why the two places must not both do it. Doing it here as well cancelled out, which
		// is what sent a Joy-Con 2 (R) up as down in the vertical grip and left as right in the
		// horizontal one (reported on hardware; the left half was unaffected because it never reaches
		// this branch).
		if (bWhole)
		{
			jc->simple_state.stickRY = -jc->simple_state.stickRY;
		}
	}

	// Switch controllers only report ZL/ZR digitally; surface them as full triggers when pressed.
	jc->simple_state.lTrigger = (buttons & JSMASK_ZL) ? 1.0f : 0.0f;
	jc->simple_state.rTrigger = (buttons & JSMASK_ZR) ? 1.0f : 0.0f;

	// Battery, as the cell's terminal voltage in millivolts. There is no charge percentage in the
	// report, so one is interpolated across the usable span of a single lithium cell -- 3.0V empty to
	// 4.2V full. That is a coarse stand-in for a real discharge curve (voltage sags under load and
	// recovers at rest), so it is worth reading as a bar rather than as a number.
	if (len >= 36)
	{
		const int32 millivolts = packet[32] | (packet[33] << 8);
		if (millivolts >= 2500 && millivolts <= 5000)
		{
			jc->sw2_battery_volts.store(millivolts / 1000.0f);
			const int32 percent = FMath::Clamp((millivolts - 3000) * 100 / 1200, 0, 100);
			jc->battery_percent.store(percent);
			jc->battery_level.store(BatteryLevelFromPercent(percent));
		}
	}

	// The remaining sensors. None of these is motion, and nothing outside this family reports them, so
	// they are published on their own rather than folded into the IMU state.
	if (len >= 32)
	{
		const int32 mouseX = packet[17] | (packet[18] << 8);
		const int32 mouseY = packet[19] | (packet[20] << 8);
		jc->sw2_mouse_x.store(mouseX);
		jc->sw2_mouse_y.store(mouseY);
		jc->accumulate_mouse_travel(mouseX, mouseY);
		jc->sw2_mouse_roughness.store(packet[21] | (packet[22] << 8));
		jc->sw2_mouse_distance.store(packet[23] | (packet[24] << 8));
		jc->sw2_magnetometer_x.store(uint16_to_int16(packet[26] | (packet[27] << 8) & 0xFF00));
		jc->sw2_magnetometer_y.store(uint16_to_int16(packet[28] | (packet[29] << 8) & 0xFF00));
		jc->sw2_magnetometer_z.store(uint16_to_int16(packet[30] | (packet[31] << 8) & 0xFF00));
	}
	if (len >= 49)
	{
		jc->sw2_temperature_celsius.store(25.0f + (packet[47] | (packet[48] << 8)) / 127.0f);
	}

	// IMU: int16 LE triplets at bytes 49-54 (accel) and 55-60 (gyro), one sample per report. Bytes
	// 61-62 are always zero (padding) -- reading the gyro from 57 leaves one axis permanently dead.
	// The gyro's word order differs from the accelerometer's: physical pitch was measured (on
	// hardware) in the third gyro word, so the assignment below is (yaw, roll, pitch).
	// Accel is ~1g = 4096 LSB on every shape; the gyro's range is not shared, hence the per-shape scale.
	if (len >= 61)
	{
		const float accScale = 1.0f / 4096.0f;
		const float gyroScale = jc->switch2_gyro_scale();

		const float aw0 = (float)uint16_to_int16(packet[49] | (packet[50] << 8) & 0xFF00);
		const float aw1 = (float)uint16_to_int16(packet[51] | (packet[52] << 8) & 0xFF00);
		const float aw2 = (float)uint16_to_int16(packet[53] | (packet[54] << 8) & 0xFF00);
		const float gw0 = (float)uint16_to_int16(packet[55] | (packet[56] << 8) & 0xFF00);
		const float gw1 = (float)uint16_to_int16(packet[57] | (packet[58] << 8) & 0xFF00);
		const float gw2 = (float)uint16_to_int16(packet[59] | (packet[60] << 8) & 0xFF00);

		hasIMU = !(aw0 == 0.f && aw1 == 0.f && aw2 == 0.f && gw0 == 0.f && gw1 == 0.f && gw2 == 0.f);

		// Remap into JSL's right-handed Y-up IMU space. Both sensors verified per-axis on hardware
		// against a Switch 1 controller held in identical poses: accel words map (X,Y,Z) = (w0, w2, -w1);
		// gyro words map pitch = w0, yaw = w2, roll = -w1.
		imu_state.accelX = aw0 * accScale;
		imu_state.accelY = aw2 * accScale;
		imu_state.accelZ = -aw1 * accScale;
		imu_state.gyroX = gw0 * gyroScale;
		imu_state.gyroY = gw2 * gyroScale;
		imu_state.gyroZ = -gw1 * gyroScale;

		// A detached Joy-Con 2 is held rotated relative to the whole controller: the right half sits turned
		// half a turn about the roll axis, the left half in the same orientation as the base mapping above.
		//
		// Both sensors must take the SAME correction, because both describe the same physical rotation of
		// the same shell. A half turn about Z sends accelerometer (x,y,z) to (-x,-y,z), and a gyro reading
		// is a rotation about those same axes, so it goes exactly the same way -- the Z component is what
		// the shell was turned about, and turning about an axis cannot flip that axis' own sign.
		//
		// The Switch 1 halves negate gyroZ here as well, and that is where this block came from. It does not
		// carry over: the Switch 1 base maps roll to +w0, this one to -w1, so the extra negation lands on a
		// sign that is already accounted for and the roll comes out backwards -- pitch and yaw correct,
		// roll reversed, which is precisely what a Joy-Con 2 was reported doing.
		if (bRightHalf)
		{
			imu_state.gyroX = -imu_state.gyroX;
			imu_state.gyroY = -imu_state.gyroY;
			imu_state.accelX = -imu_state.accelX;
			imu_state.accelY = -imu_state.accelY;
		}

		jc->modifying_lock.Lock();
		jc->push_sensor_samples(imu_state.gyroX, imu_state.gyroY, imu_state.gyroZ,
			imu_state.accelX, imu_state.accelY, imu_state.accelZ, jc->delta_time);
		jc->get_calibrated_gyro(imu_state.gyroX, imu_state.gyroY, imu_state.gyroZ);
		jc->modifying_lock.Unlock();

		jc->imu_state = imu_state;
	}
	else
	{
		hasIMU = false;
	}

	settle_input(jc);
	return true;
}

// The DualShock 4. Returns false for a report the dongle sends with no controller attached.
static bool parse_ds4_report(JoyShock* jc, uint8_t* packet, int32 len, FIMUState& imu_state, bool& hasIMU)
{
	int indexOffset = 0;
	bool isValid = true;
	if (!jc->is_usb) {
		isValid = packet[0] == 0x11;
		indexOffset = 2;
	}
	else {
		isValid = packet[0] == 0x01;
		if (isValid && (packet[31] & 0x04) == 0x04)
			return false; // ignore packets from Dongle with no connected controller
	}
	if (isValid) {
		// Battery: byte 30 of the DS4 report holds the charge in its low nibble and the cable flag in
		// bit 4. The charge counts further while charging (up to 11) than on battery (up to 8), so the
		// buckets below are thresholds rather than a fixed divisor.
		//
		// NOT YET VERIFIED ON HARDWARE. Offset and encoding come from public reverse-engineering of the
		// DS4 report, not from a controller on this desk -- the risk is a plausible-looking but wrong
		// level rather than an obvious failure. Check against the battery indicator before trusting it.
		if (indexOffset + 30 < len) {
			const uint8_t batteryByte = packet[indexOffset + 30];
			const uint8_t rawCharge = batteryByte & 0x0F;
			const bool bCharging = (batteryByte & 0x10) != 0;
			// Verified on hardware against DS4Windows: the raw count runs to 8 on battery and to 11
			// while charging, so it is a fraction of a moving maximum rather than a fixed 0-15 scale.
			// A controller reading 7 on battery is 87%, which is what the OS reports for it -- reading
			// the same 7 as sevenths-of-fifteen is what made a nearly full controller look half empty.
			const int32 maxCharge = bCharging ? 11 : 8;
			const int32 percent = FMath::Min<int32>(rawCharge, maxCharge) * 100 / maxCharge;
			jc->battery_charging.store(bCharging);
			jc->battery_percent.store(percent);
			jc->battery_level.store(BatteryLevelFromPercent(percent));
			if (!jc->logged_battery_byte) {
				jc->logged_battery_byte = true;
				UE_LOG(LogJoyShockLibrary, Verbose,
					TEXT("DS4 battery: device %d byte[%d] = 0x%02X -> %d%%, charging %d"),
					jc->intHandle, indexOffset + 30, batteryByte, percent, bCharging ? 1 : 0);
			}
		}

		// Gyroscope:
		// Gyroscope data is relative (degrees/s)
		int16_t gyroSampleX = uint16_to_int16(packet[indexOffset+13] | (packet[indexOffset+14] << 8) & 0xFF00);
		int16_t gyroSampleY = uint16_to_int16(packet[indexOffset+15] | (packet[indexOffset+16] << 8) & 0xFF00);
		int16_t gyroSampleZ = uint16_to_int16(packet[indexOffset+17] | (packet[indexOffset+18] << 8) & 0xFF00);
		int16_t accelSampleX = uint16_to_int16(packet[indexOffset+19] | (packet[indexOffset+20] << 8) & 0xFF00);
		int16_t accelSampleY = uint16_to_int16(packet[indexOffset+21] | (packet[indexOffset+22] << 8) & 0xFF00);
		int16_t accelSampleZ = uint16_to_int16(packet[indexOffset+23] | (packet[indexOffset+24] << 8) & 0xFF00);

		if ((gyroSampleX | gyroSampleY | gyroSampleZ | accelSampleX | accelSampleY | accelSampleZ) == 0)
		{
			// all zero?
			hasIMU = false;
		}

		// convert to real units
		imu_state.gyroX = (float)(gyroSampleX) * (2000.0f / 32767.0f);
		imu_state.gyroY = (float)(gyroSampleY) * (2000.0f / 32767.0f);
		imu_state.gyroZ = (float)(gyroSampleZ) * (2000.0f / 32767.0f);

		imu_state.accelX = (float)(accelSampleX) / 8192.0f;
		imu_state.accelY = (float)(accelSampleY) / 8192.0f;
		imu_state.accelZ = (float)(accelSampleZ) / 8192.0f;

		//printf("DS4 accel: %.4f, %.4f, %.4f\n", imu_state.accelX, imu_state.accelY, imu_state.accelZ);

		//printf("%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
		//	jc->gyro.yaw, jc->gyro.pitch, jc->gyro.roll, jc->accel.x, jc->accel.y, jc->accel.z, universal_counter++);

		// Touchpad:
		jc->last_touch_state = jc->touch_state;

		jc->touch_state.t0Id = (int)(packet[indexOffset+35] & 0x7F);
		jc->touch_state.t1Id = (int)(packet[indexOffset+39] & 0x7F);
		jc->touch_state.t0Down = (packet[indexOffset+35] & 0x80) == 0;
		jc->touch_state.t1Down = (packet[indexOffset+39] & 0x80) == 0;

		jc->touch_state.t0X = (packet[indexOffset+36] | (packet[indexOffset+37] & 0x0F) << 8) / 1920.0f;
		jc->touch_state.t0Y = ((packet[indexOffset+37] & 0xF0) >> 4 | packet[indexOffset+38] << 4) / 943.0f;
		jc->touch_state.t1X = (packet[indexOffset+40] | (packet[indexOffset+41] & 0x0F) << 8) / 1920.0f;
		jc->touch_state.t1Y = ((packet[indexOffset+41] & 0xF0) >> 4 | packet[indexOffset+42] << 4) / 943.0f;

		//printf("DS4 touch: %d, %d, %d, %d, %.4f, %.4f, %.4f, %.4f\n",
		//	jc->touch_state.t0Id, jc->touch_state.t1Id, jc->touch_state.t0Down, jc->touch_state.t1Down,
		//	jc->touch_state.t0X, jc->touch_state.t0Y, jc->touch_state.t1X, jc->touch_state.t1Y);

		// DS4 dpad is a hat...  0x08 is released, 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
		// http://eleccelerator.com/wiki/index.php?title=DualShock_4
		uint8_t hat = packet[indexOffset+5] & 0x0f;

		if ((hat > 2) & (hat < 6)) jc->simple_state.buttons |= JSMASK_DOWN; // down = SE | S | SW
		if ((hat == 7) | (hat < 2)) jc->simple_state.buttons |= JSMASK_UP; // up = N | NE | NW
		if ((hat > 0) & (hat < 4)) jc->simple_state.buttons |= JSMASK_RIGHT; // right = NE | E | SE
		if ((hat > 4) & (hat < 8)) jc->simple_state.buttons |= JSMASK_LEFT; // left = SW | W | NW

		jc->simple_state.buttons |= ((int)(packet[indexOffset+5] >> 4) << JSOFFSET_W) & JSMASK_W;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+5] >> 7) << JSOFFSET_N) & JSMASK_N;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+5] >> 5) << JSOFFSET_S) & JSMASK_S;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+5] >> 6) << JSOFFSET_E) & JSMASK_E;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+6] >> 6) << JSOFFSET_LCLICK) & JSMASK_LCLICK;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+6] >> 7) << JSOFFSET_RCLICK) & JSMASK_RCLICK;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+6] >> 5) << JSOFFSET_OPTIONS) & JSMASK_OPTIONS;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+6] >> 4) << JSOFFSET_SHARE) & JSMASK_SHARE;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+6] >> 1) << JSOFFSET_R) & JSMASK_R;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+6]) << JSOFFSET_L) & JSMASK_L;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+7]) << JSOFFSET_PS) & JSMASK_PS;
		jc->simple_state.buttons |= ((int)(packet[indexOffset+7] >> 1) << JSOFFSET_TOUCHPAD_CLICK) & JSMASK_TOUCHPAD_CLICK;
		//jc->btns.zr = (packet[indexOffset+6] >> 3) & 1;
		//jc->btns.zl = (packet[indexOffset+6] >> 2) & 1;
		jc->simple_state.rTrigger = packet[indexOffset+9] / 255.0f;
		jc->simple_state.lTrigger = packet[indexOffset+8] / 255.0f;

		if (jc->simple_state.rTrigger > 0.0) jc->simple_state.buttons |= JSMASK_ZR;
		if (jc->simple_state.lTrigger > 0.0) jc->simple_state.buttons |= JSMASK_ZL;

		uint16_t stick_x = packet[indexOffset+1];
		uint16_t stick_y = packet[indexOffset+2];
		stick_y = 255 - stick_y;

		uint16_t stick2_x = packet[indexOffset+3];
		uint16_t stick2_y = packet[indexOffset+4];
		stick2_y = 255 - stick2_y;

		jc->simple_state.stickLX = (std::fmin)(1.0f, (stick_x - 127.0f) / 127.0f);
		jc->simple_state.stickLY = (std::fmin)(1.0f, (stick_y - 127.0f) / 127.0f);
		jc->simple_state.stickRX = (std::fmin)(1.0f, (stick2_x - 127.0f) / 127.0f);
		jc->simple_state.stickRY = (std::fmin)(1.0f, (stick2_y - 127.0f) / 127.0f);

		jc->modifying_lock.Lock();
		jc->push_sensor_samples(imu_state.gyroX, imu_state.gyroY, imu_state.gyroZ,
				imu_state.accelX, imu_state.accelY, imu_state.accelZ, jc->delta_time);

		jc->get_calibrated_gyro(imu_state.gyroX, imu_state.gyroY, imu_state.gyroZ);
		jc->modifying_lock.Unlock();

		jc->imu_state = imu_state;
	}

	//printf("Buttons: %d LX: %.5f LY: %.5f RX: %.5f RY: %.5f GX: %.4f GY: %.4f GZ: %.4f\n", \
			//	jc->simple_state.buttons, (jc->simple_state.stickLX + 1), (jc->simple_state.stickLY + 1), (jc->simple_state.stickRX + 1), (jc->simple_state.stickRY + 1), jc->imu_state.gyroX, jc->imu_state.gyroY, jc->imu_state.gyroZ);


	return true;
}

// The DualSense.
static bool parse_dualsense_report(JoyShock* jc, uint8_t* packet, int32 len, FIMUState& imu_state, bool& hasIMU)
{
    //printf("%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
    //	packet[0], packet[1], packet[2], packet[3], packet[4], packet[5], packet[6], packet[7], packet[8], packet[9],
    //	packet[10], packet[11], packet[12], packet[13], packet[14], packet[15], packet[16], packet[17], packet[18], packet[19], packet[20],
    //	packet[21], packet[22], packet[23], packet[24], packet[25], packet[26], packet[27], packet[28], packet[29], packet[30],
    //	packet[31], packet[32], packet[33], packet[34], packet[35], packet[36], packet[37], packet[38], packet[39], packet[40],
    //	packet[41], packet[42], packet[43], packet[44], packet[45], packet[46], packet[47], packet[48], packet[49], packet[50]);
    int indexOffset = 1;
    if(!jc->is_usb) {
        indexOffset = 2;
    }

    // Battery: byte 53 of the DualSense report packs the charge (low nibble, 0-10 in tenths) and a
    // power-state code (high nibble: 0 discharging, 1 charging, 2 charged).
    //
    // NOT YET VERIFIED ON HARDWARE, exactly as for the DS4 above -- offset and encoding come from
    // public reverse-engineering rather than measurement here.
    if (indexOffset + 52 < len) {
        const uint8_t batteryByte = packet[indexOffset + 52];
        const uint8_t rawCharge = batteryByte & 0x0F;
        const uint8_t powerState = (batteryByte >> 4) & 0x0F;
        // The DualSense counts in tenths (0-10) with the power state in the high nibble.
        //
        // STILL UNVERIFIED ON HARDWARE, unlike the DS4 above. The one-shot log below is what confirms
        // it: compare the reported percentage against what the OS shows for the same controller.
        const int32 percent = FMath::Min<int32>(rawCharge * 10, 100);
        jc->battery_charging.store(powerState == 0x01 || powerState == 0x02);
        jc->battery_percent.store(percent);
        jc->battery_level.store(BatteryLevelFromPercent(percent));
        if (!jc->logged_battery_byte) {
            jc->logged_battery_byte = true;
            UE_LOG(LogJoyShockLibrary, Log,
                TEXT("DualSense battery (UNVERIFIED offset): device %d report 0x%02X len %d, byte[%d] = 0x%02X -> %d%%, power state %d"),
                jc->intHandle, packet[0], len, indexOffset + 52, batteryByte, percent, powerState);
        }
    }

    // Gyroscope:
    // Gyroscope data is relative (degrees/s)
    int16_t gyroSampleX = uint16_to_int16(packet[indexOffset + 15] | (packet[indexOffset + 16] << 8) & 0xFF00);
    int16_t gyroSampleY = uint16_to_int16(packet[indexOffset + 17] | (packet[indexOffset + 18] << 8) & 0xFF00);
    int16_t gyroSampleZ = uint16_to_int16(packet[indexOffset + 19] | (packet[indexOffset + 20] << 8) & 0xFF00);
    int16_t accelSampleX = uint16_to_int16(packet[indexOffset + 21] | (packet[indexOffset + 22] << 8) & 0xFF00);
    int16_t accelSampleY = uint16_to_int16(packet[indexOffset + 23] | (packet[indexOffset + 24] << 8) & 0xFF00);
    int16_t accelSampleZ = uint16_to_int16(packet[indexOffset + 25] | (packet[indexOffset + 26] << 8) & 0xFF00);

    if ((gyroSampleX | gyroSampleY | gyroSampleZ | accelSampleX | accelSampleY | accelSampleZ) == 0) {
        // all zero?
        hasIMU = false;
    }

    // convert to real units
    imu_state.gyroX = (float) (gyroSampleX) * (2000.0f / 32767.0f);
    imu_state.gyroY = (float) (gyroSampleY) * (2000.0f / 32767.0f);
    imu_state.gyroZ = (float) (gyroSampleZ) * (2000.0f / 32767.0f);

    imu_state.accelX = (float) (accelSampleX) / 8192.0f;
    imu_state.accelY = (float) (accelSampleY) / 8192.0f;
    imu_state.accelZ = (float) (accelSampleZ) / 8192.0f;

    //printf("DS accel: %.4f, %.4f, %.4f\n", imu_state.accelX, imu_state.accelY, imu_state.accelZ);

    //printf("%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d\n",
    //	jc->gyro.yaw, jc->gyro.pitch, jc->gyro.roll, jc->accel.x, jc->accel.y, jc->accel.z, universal_counter++);

    // Touchpad:
    jc->last_touch_state = jc->touch_state;

    jc->touch_state.t0Id = (int) (packet[indexOffset + 32] & 0x7F);
    jc->touch_state.t1Id = (int) (packet[indexOffset + 36] & 0x7F);
    jc->touch_state.t0Down = (packet[indexOffset + 32] & 0x80) == 0;
    jc->touch_state.t1Down = (packet[indexOffset + 36] & 0x80) == 0;

    jc->touch_state.t0X = (packet[indexOffset + 33] | (packet[indexOffset + 34] & 0x0F) << 8) / 1920.0f;
    jc->touch_state.t0Y = ((packet[indexOffset + 34] & 0xF0) >> 4 | packet[indexOffset + 35] << 4) / 943.0f;
    jc->touch_state.t1X = (packet[indexOffset + 37] | (packet[indexOffset + 38] & 0x0F) << 8) / 1920.0f;
    jc->touch_state.t1Y = ((packet[indexOffset + 38] & 0xF0) >> 4 | packet[indexOffset + 39] << 4) / 943.0f;

    //printf("DS touch: %d, %d, %d, %d, %.4f, %.4f, %.4f, %.4f\n",
    //	jc->touch_state.t0Id, jc->touch_state.t1Id, jc->touch_state.t0Down, jc->touch_state.t1Down,
    //	jc->touch_state.t0X, jc->touch_state.t0Y, jc->touch_state.t1X, jc->touch_state.t1Y);

    // DS dpad is a hat...  0x08 is released, 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
    // http://eleccelerator.com/wiki/index.php?title=DualShock_4
    uint8_t hat = packet[indexOffset + 7] & 0x0f;

    if ((hat > 2) & (hat < 6)) jc->simple_state.buttons |= JSMASK_DOWN; // down = SE | S | SW
    if ((hat == 7) | (hat < 2)) jc->simple_state.buttons |= JSMASK_UP; // up = N | NE | NW
    if ((hat > 0) & (hat < 4)) jc->simple_state.buttons |= JSMASK_RIGHT; // right = NE | E | SE
    if ((hat > 4) & (hat < 8)) jc->simple_state.buttons |= JSMASK_LEFT; // left = SW | W | NW

    jc->simple_state.buttons |= ((int) (packet[indexOffset + 7] >> 4) << JSOFFSET_W) & JSMASK_W;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 7] >> 5) << JSOFFSET_S) & JSMASK_S;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 7] >> 6) << JSOFFSET_E) & JSMASK_E;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 7] >> 7) << JSOFFSET_N) & JSMASK_N;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 8] >> 6) << JSOFFSET_LCLICK) & JSMASK_LCLICK;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 8] >> 7) << JSOFFSET_RCLICK) & JSMASK_RCLICK;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 8] >> 5) << JSOFFSET_OPTIONS) & JSMASK_OPTIONS;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 8] >> 4) << JSOFFSET_SHARE) & JSMASK_SHARE;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 8] >> 1) << JSOFFSET_R) & JSMASK_R;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 8]) << JSOFFSET_L) & JSMASK_L;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 9]) << JSOFFSET_PS) & JSMASK_PS;
    // The DS5 has a mute button that is normally ignored on PC. We can use this.
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 9] >> 1) << JSOFFSET_TOUCHPAD_CLICK) & JSMASK_TOUCHPAD_CLICK;
    jc->simple_state.buttons |= ((int) (packet[indexOffset + 9] >> 2) << JSOFFSET_MIC) & JSMASK_MIC;
	jc->simple_state.buttons |= ((int)(packet[indexOffset + 9] >> 4) << JSOFFSET_FNL) & JSMASK_FNL;
	jc->simple_state.buttons |= ((int)(packet[indexOffset + 9] >> 5) << JSOFFSET_FNR) & JSMASK_FNR;
	jc->simple_state.buttons |= ((int)(packet[indexOffset + 9] >> 6) << JSOFFSET_SL) & JSMASK_SL;
	jc->simple_state.buttons |= ((int) (packet[indexOffset + 9] >> 7) << JSOFFSET_SR) & JSMASK_SR;
    //jc->btns.zr = (packet[indexOffset+6] >> 3) & 1;
    //jc->btns.zl = (packet[indexOffset+6] >> 2) & 1;
    jc->simple_state.rTrigger = packet[indexOffset + 5] / 255.0f;
    jc->simple_state.lTrigger = packet[indexOffset + 4] / 255.0f;

    if (jc->simple_state.rTrigger > 0.0) jc->simple_state.buttons |= JSMASK_ZR;
    if (jc->simple_state.lTrigger > 0.0) jc->simple_state.buttons |= JSMASK_ZL;

        uint16_t stick_x = packet[indexOffset + 0];
        uint16_t stick_y = packet[indexOffset + 1];

	stick_y = 255 - stick_y;

	uint16_t stick2_x = packet[indexOffset + 2];
	uint16_t stick2_y = packet[indexOffset + 3];
	stick2_y = 255 - stick2_y;

	jc->simple_state.stickLX = (std::fmin)(1.0f, (stick_x - 127.0f) / 127.0f);
	jc->simple_state.stickLY = (std::fmin)(1.0f, (stick_y - 127.0f) / 127.0f);
	jc->simple_state.stickRX = (std::fmin)(1.0f, (stick2_x - 127.0f) / 127.0f);
	jc->simple_state.stickRY = (std::fmin)(1.0f, (stick2_y - 127.0f) / 127.0f);

	jc->modifying_lock.Lock();
	jc->push_sensor_samples(imu_state.gyroX, imu_state.gyroY, imu_state.gyroZ,
			imu_state.accelX, imu_state.accelY, imu_state.accelZ, jc->delta_time);

	jc->get_calibrated_gyro(imu_state.gyroX, imu_state.gyroY, imu_state.gyroZ);
	jc->modifying_lock.Unlock();

	jc->imu_state = imu_state;

	return true;
}

	// most of this JoyCon and Pro Controller stuff is adapted from MFosse's Joycon driver.
//
// The fall-through family: anything that is not a Switch 2 or a Sony pad. Unlike the others this
// cannot reject a report -- handle_input has already screened the Switch report ids it accepts.
static void parse_switch_report(JoyShock* jc, uint8_t* packet, int32 len, FIMUState& imu_state, bool& hasIMU)
{

	int buttons_pressed = 0;

	// Input update packet: 0x21 has buttons/sticks but no IMU; 0x30 includes IMU; 0x31 also
	// includes NFC data. Button-only reports must preserve the last valid motion sample.
	{

		// offset for usb or bluetooth data:
		/*int offset = settings.usingBluetooth ? 0 : 10;*/
		int offset = 0;
		//int offset = !jc->is_usb ? 0 : 10;

		uint8_t *btn_data = packet + offset + 3;

		// get button states:
		{
			uint16_t states = 0;
			uint16_t states2 = 0;

			// Left JoyCon:
			if (jc->left_right == 1) {
				states = (btn_data[1] << 8) | (btn_data[2] & 0xFF);
				// Right JoyCon:
			}
			else if (jc->left_right == 2) {
				states = (btn_data[1] << 8) | (btn_data[0] & 0xFF);
				// Pro Controller:
			}
			else if (jc->left_right == 3) {
				states = (btn_data[1] << 8) | (btn_data[2] & 0xFF);
				states2 = (btn_data[1] << 8) | (btn_data[0] & 0xFF);
			}

			buttons_pressed = states;
			// Pro Controller:
			if (jc->left_right == 3) {
				buttons_pressed |= states2 << 16;

				// fix some non-sense the Pro Controller does
				// clear nth bit
				//num &= ~(1UL << n);
				buttons_pressed &= ~(1L << 9);
				buttons_pressed &= ~(1L << 12);
				buttons_pressed &= ~(1L << 14);

				buttons_pressed &= ~(1UL << (8 + 16));
				buttons_pressed &= ~(1UL << (11 + 16));
				buttons_pressed &= ~(1UL << (13 + 16));
			}
			else if (jc->left_right == 2) {
				buttons_pressed = buttons_pressed << 16;
			}
		}

		// get stick data:
		uint8_t *stick_data = packet + offset;
		if (jc->left_right == 1) {
			stick_data += 6;
		}
		else if (jc->left_right == 2) {
			stick_data += 9;
		}

		uint16_t stick_x = stick_data[0] | ((stick_data[1] & 0xF) << 8);
		uint16_t stick_y = (stick_data[1] >> 4) | (stick_data[2] << 4);

		// use calibration data:
		if (jc->left_right == 1) {
			jc->CalcAnalogStick2(jc->simple_state.stickLX, jc->simple_state.stickLY,
				stick_x,
				stick_y,
				jc->stick_cal_x_l,
				jc->stick_cal_y_l);
		}
		else if (jc->left_right == 2) {
			jc->CalcAnalogStick2(jc->simple_state.stickRX, jc->simple_state.stickRY,
				stick_x,
				stick_y,
				jc->stick_cal_x_r,
				jc->stick_cal_y_r);
		}
		else if (jc->left_right == 3) {
			// pro controller
			stick_data += 6;
			//printf("%d, %d\n",
			//	jc->stick_cal_x_l,
			//	jc->stick_cal_y_l);
			uint16_t StickX = stick_data[0] | ((stick_data[1] & 0xF) << 8);
			uint16_t StickY = (stick_data[1] >> 4) | (stick_data[2] << 4);
			jc->CalcAnalogStick2(jc->simple_state.stickLX, jc->simple_state.stickLY,
				StickX,
				StickY,
				jc->stick_cal_x_l,
				jc->stick_cal_y_l);
			stick_data += 3;
			uint16_t stick_x2 = stick_data[0] | ((stick_data[1] & 0xF) << 8);
			uint16_t stick_y2 = (stick_data[1] >> 4) | (stick_data[2] << 4);
			jc->CalcAnalogStick2(jc->simple_state.stickRX, jc->simple_state.stickRY,
				stick_x2,
				stick_y2,
				jc->stick_cal_x_r,
				jc->stick_cal_y_r);
			// Nintendo reports the right stick's Y axis with the opposite sign from the left stick's --
			// the same quirk the Switch 2 Pro Controller has, and the same one the right Joy-Con is
			// corrected for in FJoyShockInterface. This is the only place it was missing, which made the
			// Switch 1 Pro Controller the one family whose right stick pushed "up" as a negative value
			// while every other controller pushed it positive. Only this branch (left_right == 3, a whole
			// controller) needs it: a right Joy-Con fills stickR from the branch above and is normalised
			// once, in the interface -- doing it here as well would cancel out.
			jc->simple_state.stickRY = -jc->simple_state.stickRY;
		}

		// Byte 2 of the standard report carries the charge in bits 5-7 and the charging flag in bit 4.
		//
		// This used to read stick_data[1], which is not that byte at all: stick_data has been advanced to
		// the stick block by here (byte 6 for a left Joy-Con, 9 for a right one), so it was reading the
		// low bits of the analog stick's Y axis. The "battery" therefore tracked stick noise, and the
		// charging flag -- a single bit of a jittering ADC reading -- flickered on a controller sitting
		// still. It went unnoticed for as long as nothing exposed the value.
		if (len > 2) {
			jc->battery = (packet[2] & 0xF0) >> 4;
			jc->battery_charging.store((packet[2] & 0x10) != 0);
			jc->battery_level.store((packet[2] & 0xE0) >> 5);
		}
		//printf("JoyCon battery: %d\n", jc->battery);

		// Accelerometer: only the full 0x30/0x31 reports contain these bytes.
		// Accelerometer data is absolute.
		hasIMU = packet[0] == 0x30 || packet[0] == 0x31;
		if (hasIMU)
		{
			// get accelerometer X:
			float accelSampleZ = (float)uint16_to_int16(packet[13] | (packet[14] << 8) & 0xFF00) * jc->acc_cal_coeff[0];
			float accelSampleX = (float)uint16_to_int16(packet[15] | (packet[16] << 8) & 0xFF00) * jc->acc_cal_coeff[1];
			float accelSampleY = (float)uint16_to_int16(packet[17] | (packet[18] << 8) & 0xFF00) * jc->acc_cal_coeff[2];
			float gyroSampleX = (float)uint16_to_int16(packet[19] | (packet[20] << 8) & 0xFF00) * jc->gyro_cal_coeff[0];
			float gyroSampleY = (float)uint16_to_int16(packet[21] | (packet[22] << 8) & 0xFF00) * jc->gyro_cal_coeff[1];
			float gyroSampleZ = (float)uint16_to_int16(packet[23] | (packet[24] << 8) & 0xFF00) * jc->gyro_cal_coeff[2];

			if (gyroSampleX == 0.f && gyroSampleY == 0.f && gyroSampleZ == 0.f && accelSampleX == 0.f && accelSampleY == 0.f && accelSampleZ == 0.f)
			{
				// all zero?
				hasIMU = false;
			}

			//jc->push_sensor_samples(accelSampleX, accelSampleY, accelSampleZ, gyroSampleX, gyroSampleY, gyroSampleZ);
			float accelX = accelSampleX;
			float accelY = accelSampleY;
			float accelZ = accelSampleZ;
			float totalGyroX = gyroSampleX - jc->sensor_cal[1][0];
			float totalGyroY = gyroSampleY - jc->sensor_cal[1][1];
			float totalGyroZ = gyroSampleZ - jc->sensor_cal[1][2];
			// each packet actually has 3 samples worth of data, so collect sample 2
			accelSampleZ = (float)uint16_to_int16(packet[25] | (packet[26] << 8) & 0xFF00) * jc->acc_cal_coeff[0];
			accelSampleX = (float)uint16_to_int16(packet[27] | (packet[28] << 8) & 0xFF00) * jc->acc_cal_coeff[1];
			accelSampleY = (float)uint16_to_int16(packet[29] | (packet[30] << 8) & 0xFF00) * jc->acc_cal_coeff[2];
			gyroSampleX = (float)uint16_to_int16(packet[31] | (packet[32] << 8) & 0xFF00) * jc->gyro_cal_coeff[0];
			gyroSampleY = (float)uint16_to_int16(packet[33] | (packet[34] << 8) & 0xFF00) * jc->gyro_cal_coeff[1];
			gyroSampleZ = (float)uint16_to_int16(packet[35] | (packet[36] << 8) & 0xFF00) * jc->gyro_cal_coeff[2];
			//jc->push_sensor_samples(accelSampleX, accelSampleY, accelSampleZ, gyroSampleX, gyroSampleY, gyroSampleZ);
			accelX += accelSampleX;
			accelY += accelSampleY;
			accelZ += accelSampleZ;
			totalGyroX += gyroSampleX - jc->sensor_cal[1][0];
			totalGyroY += gyroSampleY - jc->sensor_cal[1][1];
			totalGyroZ += gyroSampleZ - jc->sensor_cal[1][2];
			// ... and sample 3
			accelSampleZ = (float)uint16_to_int16(packet[37] | (packet[38] << 8) & 0xFF00) * jc->acc_cal_coeff[0];
			accelSampleX = (float)uint16_to_int16(packet[39] | (packet[40] << 8) & 0xFF00) * jc->acc_cal_coeff[1];
			accelSampleY = (float)uint16_to_int16(packet[41] | (packet[42] << 8) & 0xFF00) * jc->acc_cal_coeff[2];
			gyroSampleX = (float)uint16_to_int16(packet[43] | (packet[44] << 8) & 0xFF00) * jc->gyro_cal_coeff[0];
			gyroSampleY = (float)uint16_to_int16(packet[45] | (packet[46] << 8) & 0xFF00) * jc->gyro_cal_coeff[1];
			gyroSampleZ = (float)uint16_to_int16(packet[47] | (packet[48] << 8) & 0xFF00) * jc->gyro_cal_coeff[2];
			//jc->push_sensor_samples(accelSampleX, accelSampleY, accelSampleZ, gyroSampleX, gyroSampleY, gyroSampleZ);
			accelX += accelSampleX;
			accelY += accelSampleY;
			accelZ += accelSampleZ;
			totalGyroX += gyroSampleX - jc->sensor_cal[1][0];
			totalGyroY += gyroSampleY - jc->sensor_cal[1][1];
			totalGyroZ += gyroSampleZ - jc->sensor_cal[1][2];
			// average the 3 samples
			accelX /= 3;
			accelY /= 3;
			accelZ /= 3;
			totalGyroX /= 3;
			totalGyroY /= 3;
			totalGyroZ /= 3;
			imu_state.accelX = -accelX;
			imu_state.accelY = accelY;
			imu_state.accelZ = -accelZ;
			imu_state.gyroX = -totalGyroY;
			imu_state.gyroY = totalGyroZ;
			imu_state.gyroZ = totalGyroX;

			//printf("Switch accel: %.4f, %.4f, %.4f\n", imu_state.accelX, imu_state.accelY, imu_state.accelZ);
		}

	}

	// handle buttons
	{
		// left:
		if (jc->left_right == 1) {
			jc->simple_state.buttons |= ((buttons_pressed >> 1) << JSOFFSET_UP) & JSMASK_UP;
			jc->simple_state.buttons |= ((buttons_pressed) << JSOFFSET_DOWN) & JSMASK_DOWN;
			jc->simple_state.buttons |= ((buttons_pressed >> 3) << JSOFFSET_LEFT) & JSMASK_LEFT;
			jc->simple_state.buttons |= ((buttons_pressed >> 2) << JSOFFSET_RIGHT) & JSMASK_RIGHT;
			jc->simple_state.buttons |= ((buttons_pressed >> 11) << JSOFFSET_LCLICK) & JSMASK_LCLICK;
			jc->simple_state.buttons |= ((buttons_pressed >> 8) << JSOFFSET_MINUS) & JSMASK_MINUS;
			jc->simple_state.buttons |= ((buttons_pressed >> 6) << JSOFFSET_L) & JSMASK_L;
			jc->simple_state.buttons |= ((buttons_pressed >> 13) << JSOFFSET_CAPTURE) & JSMASK_CAPTURE;
			jc->simple_state.lTrigger = (float)((buttons_pressed >> 7) & 1);
			jc->simple_state.buttons |= ((int)(jc->simple_state.lTrigger) << JSOFFSET_ZL) & JSMASK_ZL;
			jc->simple_state.buttons |= ((buttons_pressed >> 5) << JSOFFSET_SL) & JSMASK_SL;
			jc->simple_state.buttons |= ((buttons_pressed >> 4) << JSOFFSET_SR) & JSMASK_SR;

			// just need to negate gyroZ
			if (hasIMU)
			{
				imu_state.gyroZ = -imu_state.gyroZ;
			}
		}

		// right:
		if (jc->left_right == 2) {
			jc->simple_state.buttons |= ((buttons_pressed >> 16) << JSOFFSET_W) & JSMASK_W;
			jc->simple_state.buttons |= ((buttons_pressed >> 17) << JSOFFSET_N) & JSMASK_N;
			jc->simple_state.buttons |= ((buttons_pressed >> 18) << JSOFFSET_S) & JSMASK_S;
			jc->simple_state.buttons |= ((buttons_pressed >> 19) << JSOFFSET_E) & JSMASK_E;
			jc->simple_state.buttons |= ((buttons_pressed >> 26) << JSOFFSET_RCLICK) & JSMASK_RCLICK;
			jc->simple_state.buttons |= ((buttons_pressed >> 25) << JSOFFSET_PLUS) & JSMASK_PLUS;
			jc->simple_state.buttons |= ((buttons_pressed >> 22) << JSOFFSET_R) & JSMASK_R;
			jc->simple_state.buttons |= ((buttons_pressed >> 28) << JSOFFSET_HOME) & JSMASK_HOME;
			jc->simple_state.rTrigger = (float)((buttons_pressed >> 23) & 1);
			jc->simple_state.buttons |= ((int)(jc->simple_state.rTrigger) << JSOFFSET_ZR) & JSMASK_ZR;
			jc->simple_state.buttons |= ((buttons_pressed >> 21) << JSOFFSET_SL) & JSMASK_SL;
			jc->simple_state.buttons |= ((buttons_pressed >> 20) << JSOFFSET_SR) & JSMASK_SR;

			// for some reason we need to negate x and y, and z on the right joycon
			if (hasIMU)
			{
				imu_state.gyroX = -imu_state.gyroX;
				imu_state.gyroY = -imu_state.gyroY;
				imu_state.gyroZ = -imu_state.gyroZ;

				imu_state.accelX = -imu_state.accelX;
				imu_state.accelY = -imu_state.accelY;
			}

		}

		// pro controller:
		if (jc->left_right == 3) {
			jc->simple_state.buttons |= ((buttons_pressed >> 1) << JSOFFSET_UP) & JSMASK_UP;
			jc->simple_state.buttons |= ((buttons_pressed) << JSOFFSET_DOWN) & JSMASK_DOWN;
			jc->simple_state.buttons |= ((buttons_pressed >> 3) << JSOFFSET_LEFT) & JSMASK_LEFT;
			jc->simple_state.buttons |= ((buttons_pressed >> 2) << JSOFFSET_RIGHT) & JSMASK_RIGHT;
			jc->simple_state.buttons |= ((buttons_pressed >> 16) << JSOFFSET_W) & JSMASK_W;
			jc->simple_state.buttons |= ((buttons_pressed >> 17) << JSOFFSET_N) & JSMASK_N;
			jc->simple_state.buttons |= ((buttons_pressed >> 18) << JSOFFSET_S) & JSMASK_S;
			jc->simple_state.buttons |= ((buttons_pressed >> 19) << JSOFFSET_E) & JSMASK_E;
			jc->simple_state.buttons |= ((buttons_pressed >> 11) << JSOFFSET_LCLICK) & JSMASK_LCLICK;
			jc->simple_state.buttons |= ((buttons_pressed >> 26) << JSOFFSET_RCLICK) & JSMASK_RCLICK;
			jc->simple_state.buttons |= ((buttons_pressed >> 25) << JSOFFSET_PLUS) & JSMASK_PLUS;
			jc->simple_state.buttons |= ((buttons_pressed >> 8) << JSOFFSET_MINUS) & JSMASK_MINUS;
			jc->simple_state.buttons |= ((buttons_pressed >> 22) << JSOFFSET_R) & JSMASK_R;
			jc->simple_state.buttons |= ((buttons_pressed >> 6) << JSOFFSET_L) & JSMASK_L;
			jc->simple_state.buttons |= ((buttons_pressed >> 28) << JSOFFSET_HOME) & JSMASK_HOME;
			jc->simple_state.buttons |= ((buttons_pressed >> 13) << JSOFFSET_CAPTURE) & JSMASK_CAPTURE;
			jc->simple_state.rTrigger = (float)((buttons_pressed >> 23) & 1);
			jc->simple_state.lTrigger = (float)((buttons_pressed >> 7) & 1);
			jc->simple_state.buttons |= ((int)(jc->simple_state.lTrigger) << JSOFFSET_ZL) & JSMASK_ZL;
			jc->simple_state.buttons |= ((int)(jc->simple_state.rTrigger) << JSOFFSET_ZR) & JSMASK_ZR;

			// just need to negate gyroZ
			if (hasIMU)
			{
				imu_state.gyroZ = -imu_state.gyroZ;
			}
		}
	}
}

// Decodes one input report into the controller's state. Which parser runs is decided here and
// nowhere else; each returns whether the report was a valid input frame.
bool handle_input(JoyShock* jc, uint8_t* packet, int32 len, bool &hasIMU) {
	hasIMU = true;
	if (packet[0] == 0) return false; // ignore non-responses
	if (jc->controller_type == ControllerType::n_switch && !jc->is_switch2)
	{
		// Reject short/unknown Switch reports before touching last/current state. Otherwise a command
		// response can look like a complete zeroed input frame and synthesize releases on every key.
		const bool bStandardReport = packet[0] == 0x21 || packet[0] == 0x30 || packet[0] == 0x31;
		const int32 minimumLength = packet[0] == 0x21 ? 12 : 49;
		if (!bStandardReport || len < minimumLength)
		{
			return false;
		}
	}
									  // remember last input

	//printf("%d: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	//	jc->left_right,
	//	packet[0], packet[1], packet[2], packet[3], packet[4], packet[5], packet[6], packet[7], packet[8], packet[9],
	//	packet[10], packet[11], packet[12], packet[13], packet[14], packet[15], packet[16], packet[17], packet[18], packet[19], packet[20]);

	jc->last_simple_state = jc->simple_state;
	jc->simple_state.buttons = 0;
	jc->last_imu_state = jc->imu_state;
	FIMUState imu_state;
	// delta time
	auto time_now = std::chrono::steady_clock::now();
	jc->delta_time = (float)(std::chrono::duration_cast<std::chrono::microseconds>(time_now - jc->last_polled).count() / 1000000.0);
	jc->last_polled = time_now;

	if (jc->cue_motion_reset)
	{
		//printf("RESET motion\n");
		jc->cue_motion_reset = false;
		jc->motion.Reset();
	}
	if (jc->motion.GetCalibrationMode() == GamepadMotionHelpers::CalibrationMode::Manual)
	{
		if (jc->use_continuous_calibration)
		{
			jc->motion.StartContinuousCalibration();
		}
		else
		{
			jc->motion.PauseContinuousCalibration();
		}
	}

	if (jc->is_switch2)
	{
		return parse_switch2_report(jc, packet, len, imu_state, hasIMU);
	}

	if (jc->controller_type == ControllerType::s_ds4)
	{
		return parse_ds4_report(jc, packet, len, imu_state, hasIMU);
	}

	if (jc->controller_type == ControllerType::s_ds)
	{
		return parse_dualsense_report(jc, packet, len, imu_state, hasIMU);
	}

	parse_switch_report(jc, packet, len, imu_state, hasIMU);

	if (hasIMU)
	{
		jc->modifying_lock.Lock();
		jc->push_sensor_samples(imu_state.gyroX, imu_state.gyroY, imu_state.gyroZ,
			imu_state.accelX, imu_state.accelY, imu_state.accelZ, jc->delta_time);

		jc->get_calibrated_gyro(imu_state.gyroX, imu_state.gyroY, imu_state.gyroZ);
		jc->modifying_lock.Unlock();

		jc->imu_state = imu_state;
	}

	settle_input(jc);
	return true;
}
