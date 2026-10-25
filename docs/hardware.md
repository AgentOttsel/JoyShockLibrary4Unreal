# Hardware support

The current Unreal Engine 5.8 overhaul has been exercised with the hardware below. "Needs a regression
test" means the implementation is present and was working before the overhaul, not that the controller is
known to be broken. "Untested" means no unit has been available.

| Controller | Current validation |
| --- | --- |
| Nintendo Switch Joy-Con | Gameplay input, solo-horizontal and joined presentation, motion, pairing/separation, player LEDs, battery and multiple simultaneous controllers tested. |
| Nintendo Switch 2 Joy-Con | Implemented over Bluetooth (they never appear over USB). **Tested on hardware once**, by a contributor with a pair, which found and fixed three faults; the Bluetooth init's reliability since then is not yet confirmed (see [Hardware still to be tested](#hardware-still-to-be-tested)). Buttons, the single stick with its calibration, motion, rumble, player LEDs and joining into a pair are all in place; the sensor axis signs for a detached half are carried over from the Switch 1 Joy-Cons rather than measured. |
| Nintendo Switch Pro Controller | USB/Bluetooth input, motion, player LEDs and HD rumble are implemented, but untested since the overhaul. |
| Nintendo Switch 2 Pro Controller | USB gameplay input, calibrated sticks, motion and player LEDs tested. HD rumble with amplitude, the Bluetooth transport, battery reporting and the extra sensors (magnetometer, mouse, temperature) are implemented but not yet confirmed on hardware. |
| DualShock 4 | Bluetooth and USB gameplay input, stick conventions, motion, battery reporting and cable/Bluetooth transport switching tested. Battery percentages were checked against DS4Windows. Rumble could not be validated because the available controller's rumble hardware is broken. |
| DualSense / DualSense Edge | USB/Bluetooth input, motion, touchpad, light, player indicators and rumble are implemented, but need a regression test after the overhaul. Battery report offsets are unverified. |
| Xbox / other XInput pads | Handled entirely by Unreal, not by this plugin. The plugin takes their player slots into account so a JoyShock controller lands where a second Xbox pad would, and **Wait For Any Controller Changes** reports them alongside ours. Mixing them works; player numbering when a *wireless* Xbox pad is in the mix is still under investigation (see [Hardware still to be tested](#hardware-still-to-be-tested)). |
| 8BitDo Pro 2 | **Not validated.** In Switch mode it presents itself as a Nintendo Switch Pro Controller, but a first try suggests it does not work through that path. Nobody has looked into why yet — if you own one, help is very welcome (see [Hardware still to be tested](#hardware-still-to-be-tested)). |

## Asking for a test

Nothing here is verified by compiling — a dropped Bluetooth link, a stick reading inverted, a rumble
packet that never lands, none of them show up until a real controller is in someone's hands. So if you
fork this and write something you cannot try yourself, open an issue saying which controller it needs.
Between the two of us we cover most of the table above:

| Who | Has | Worth knowing |
| --- | --- | --- |
| [@AgentOttsel](https://github.com/AgentOttsel) | One of every controller in the table above: DualShock 4, DualSense ×2, DualSense Edge, a Joy-Con pair, a Joy-Con 2 pair, both Pro Controllers. Plus Xbox Series X ×2, a Steam Controller and an 8BitDo Pro 2 | The whole matrix, including the two families nobody else here can reach. Runs Unreal from Visual Studio with the debugger attached, so asserts and shutdown crashes that a plain editor session swallows do get caught |
| [@uayten](https://github.com/uayten) | Joy-Con (Switch 1) ×4, Nintendo Switch 2 Pro Controller, DualShock 4, an XInput pad | **Four Joy-Con halves**, so joining, separation and four-player assignment are testable here and nowhere else. Two of his pads have **stick drift**, which is the only way anyone here reproduces a worn controller — deadzone and calibration behaviour that new hardware cannot show. Runs the editor plainly, so a failure that only stops a debugger goes unnoticed |

A report from either of them is worth more than any amount of code review, and so is one from you — if
you own a controller marked untested above, that is the most useful thing anyone can contribute to this
plugin.

## Hardware still to be tested
These are implemented but unverified. Between the two maintainers a unit exists for every one of them
(see [Asking for a test](#asking-for-a-test)), so what these wait on is a test session rather than
hardware — and reports from anyone else who owns one are very welcome all the same.

- **Nintendo Switch 2 Joy-Con, Bluetooth init reliability** — a first hardware session found the factory-data reads failing at connect, seemingly at random, on either half. The cause was the response channel taking the first notification to arrive as the answer, which for a memory read is often the acknowledgement rather than the data; that is fixed, and the reads now retry and log which address failed. What has not been re-tested is whether anything still fails after it. A controller whose calibration read fails is not dead — it falls back to a fixed stick range and reaches about two thirds of full deflection
- **Nintendo Switch Pro Controller (Switch 1)** — untested since the Unreal Engine 5.8 overhaul. Its right stick's Y axis was the one family never sign-normalised, which is now fixed in the parser and wants confirming on hardware
- **Xbox and other XInput pads** — mixing them with JoyShock controllers works, but a *wireless* Xbox pad appears to consume more than one player slot, so the next JoyShock controller lands a number further out than expected. A pad connected by cable does not do this. Unconfirmed; Verbose logging now names whatever is holding each skipped slot
- **DualSense battery reporting** — the report offsets come from public reverse-engineering rather than measurement, and are marked as unverified in the code. The DualShock 4 equivalents were checked against DS4Windows
- **DualSense touchpad Y range** — the DualSense normalises its touch coordinates with the DualShock 4's pad height, which the two do not share. If that is wrong, a finger at the bottom edge reads past 1.0 instead of reaching it. Easy to check with the demo's touchpad readout
- **8BitDo Pro 2 in Switch mode** — it presents itself as a Nintendo Switch Pro Controller, so it was expected to work through that path unchanged, but a first try with the unit one maintainer owns suggests it does not. The cause has not been investigated. This one is open to anyone who owns the controller and wants to contribute: a log of it connecting, or a fix, would move it forward
