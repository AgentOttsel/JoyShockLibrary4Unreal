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

## Every controller drives the wrong player

If one player works perfectly and the assignment scrambles the moment a second player is created — the
first controller driving player 2's Pawn, the last one driving a player that does not exist — *Skip
Assigning Gamepad to Player 1* is on. It lives in *Project Settings → Maps & Modes → Local Multiplayer*,
is **on by default**, and exists for games where the keyboard is player 1 and gamepads start at player 2.

It is almost impossible to diagnose from the symptom. Everything you can inspect looks right — the plugin
reports the correct player index, the platform input device mapper agrees with it, and each player
controller possesses the Pawn you expect. The redirect happens inside
`UGameViewportClient::RemapControllerInput`, after all of that, so nothing upstream shows a discrepancy.

## Testing in the editor

### The second player does nothing in the Play viewport

PIE keeps Slate focus separately for every local user. After creating a second player, that user's focus
can remain on the editor `SWindow` instead of the PIE `SViewport`; Slate then discards that user's input
before it ever reaches the game viewport. Standalone and packaged games do not normally have this
editor-window artifact.

Call Unreal's standard **Set Focus to Game Viewport** node after the local players, possession and
mapping contexts have been set up. That node focuses the game viewport for all Slate users. Call it again
when closing an in-game menu if that menu intentionally moved focus. The plugin does not force focus
automatically, because doing so would break legitimate UI input modes.

### The controller also moves the editor's focus

```
Slate.EnableGamepadEditorNavigation false
```

It is **on** by default, and it lets a gamepad move the editor's own focus around — so a controller you
are testing with is also tabbing through panels behind the viewport, and a stick flick can land somewhere
you did not intend. With two controllers connected it is worse, because both are doing it. The setting is
the editor's, not this plugin's, and it does not affect a packaged game.
