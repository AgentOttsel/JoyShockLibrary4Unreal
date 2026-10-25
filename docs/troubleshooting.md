# Diagnostics and troubleshooting

The temporary per-button dispatch and Slate-routing measurements used while developing the local
multiplayer example are not part of normal plugin output. For low-level HID troubleshooting,
`JoyShock.Debug.InputStalls 1` warns if a connected controller stops delivering reports for more than one
second and reports when delivery resumes. Set it back to `0` to disable it.

**Player numbers that skip.** A slot already held by a controller this plugin does not own is stepped
over, by design — that is what makes a JoyShock controller land where a second Xbox pad would. Verbose
logging (`Log LogJoyShockLibrary Verbose`) names the input device holding each slot that gets skipped,
which is what distinguishes "an XInput pad is legitimately on player 1" from "something claimed a slot and
never released it". **Wait For Any Controller Changes** answers the same question in-game: it reports every
controller Unreal knows about with the player index it landed on.

A single `Enabling IMU data...` line is expected while a controller is first initialized.

Bluetooth drops subcommands, and a lost one fails silently and permanently: the controller keeps streaming
buttons while its motion values stay at zero for the whole session. The Joy-Con handshake therefore waits
for the controller's acknowledgement of the IMU enable and the report-mode change, re-sending when it does
not arrive. Joy-Con firmware can also acknowledge the enable and still never start the sensor, so a stream
that stays motionless is repaired in place by toggling the IMU off and on — as a bare write, never a
subcommand exchange, because reading a reply from the polling thread would steal input reports. If both
fail, the plugin logs one warning and preserves the working button/stick stream rather than mutating it
further.

Configuration subcommands are otherwise never sent into an established Bluetooth stream. Re-asserting the
HOME light on a timer, for instance, caused right Joy-Cons to drop off Bluetooth after a few minutes, so
that light is cleared exactly once per connection.

Plugging a USB cable into a controller that is already paired over Bluetooth does not end the Bluetooth
link — it adds a second HID device. The plugin recognises both paths as one controller by its MAC address,
moves input onto the cable and back to the radio when it is pulled, and keeps the same connection id, player
slot and Pawn throughout, so neither event reaches the game as a connect or disconnect.
