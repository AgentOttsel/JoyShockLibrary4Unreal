# Diagnostics and troubleshooting

## Diagnostic commands

Typed into Unreal's console.

| Command | What it does |
|---|---|
| `JoyShock.Debug.InputStalls 1` | Warns when a connected controller stops delivering reports for more than a second, and says when it resumes. `0` turns it off |
| `Log LogJoyShockLibrary Verbose` | The plugin's detailed log — among other things, which device holds each player slot that gets skipped |
| `Slate.EnableGamepadEditorNavigation false` | Stops a gamepad from moving the editor's own focus — see [Testing in the editor](#testing-in-the-editor) |

A single `Enabling IMU data...` line while a controller is first set up is expected.

## Player numbers skip

A slot already held by a controller this plugin does not own is stepped over, by design — that is what
makes a JoyShock controller land where a second Xbox pad would. Verbose logging names the device holding
each skipped slot, which tells "an XInput pad is legitimately on player 1" apart from "something claimed a
slot and never released it". **Wait For Any Controller Changes** answers the same question in-game: it
reports every controller Unreal knows about, with the player index it landed on.

## A Joy-Con's buttons work but its motion stays at zero

Bluetooth drops commands, and a lost one fails silently: the controller keeps sending buttons while its
motion stays at zero. The plugin guards against it in two steps:

1. While setting the Joy-Con up, it waits for the controller to confirm that motion is on, and sends the
   command again when no confirmation arrives.
2. A Joy-Con can confirm and still never start its sensor, so a stream that stays motionless is repaired in
   place by switching the sensor off and on.

If both fail, the plugin logs one warning and leaves the working buttons and sticks alone rather than
disturbing the connection further.

## Plugging in a cable while on Bluetooth

Not a problem. The cable does not end the Bluetooth link — it adds a second path to the same controller.
The plugin recognises both as one controller by its MAC address, moves input onto the cable and back to the
radio when the cable is pulled, and keeps the same connection id, player slot and Pawn throughout, so the
game sees neither a connect nor a disconnect.

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
