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

A Pro Controller 2 cannot keep both: the set-up that moves its input onto the cable ends its Bluetooth
link, and after the cable is pulled it stays off until a button is pressed, which makes a new Bluetooth
connection. The plugin recognises it on the other side by the serial stored in the
controller and carries it on as the same controller -- same connection id, player slot and Pawn, no
connect or disconnect -- after a pause in its input of a few seconds while the new connection comes up.
If it does not come back within 10 seconds (switched off, out of range), the disconnect is reported then.

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

Which controller moves it depends on focus, not on the controller. To the editor's interface every
player is a separate user with a focus of its own, numbered like the players: the player 1 controller is
user 0, and user 0 shares its focus with the mouse and keyboard. So the player 1 controller navigates from
wherever you last clicked -- after a click in the level viewport, no blue frame shows in the menus --
while the other players' controllers each move a focus of their own, wherever it was left. A joined Joy-Con pair is one
player, so its halves move the same focus.

A controller whose focus is navigated into the level viewport stays there: the viewport takes the
stick for its camera, so navigation never leaves it, and a mouse click moves only player 1's focus. That
controller keeps flying the camera, while player 1's follows the next click out of it.

### A game and the editor at the same time

Only the application in front answers the controllers -- see [Only the window in front](input.md#only-the-window-in-front).
So with a Standalone game launched from the editor, the controller drives whichever of the two you last
clicked, never both. The Output Log says each time input stops or resumes.

While any game with this plugin is running on the same PC, the editor also leaves every controller's
player lights, light bar and rumble to that game, so the lights always show the game's players. The
editor sends its own again when the game closes.

### Using the controllers in another program with the editor open

The editor keeps every controller connected for as long as it is open, so another program that reads them
-- Unity, for example -- has to compete with it, and a Switch 2 on Bluetooth never reaches that program at
all, since a connected controller stops advertising. To hand them over, open **Edit → Editor Preferences →
Plugins → JoyShockLibrary4Unreal** and turn off **Connect Controllers in the Editor**.

The controllers are let go at once, and the editor connects no new ones, Play In Editor included, until the
option is back on. A Switch 2 controller on Bluetooth disconnects, since its link was the editor's. A Joy-Con
or other Switch 1 controller on Bluetooth stays connected, because its link belongs to Windows, and shows
the player 1 light, as when Windows first connects it; closing the editor leaves it the same way. Turning
the option back on looks for controllers again; a Bluetooth controller comes back on a button press. The option is saved per user, in the project's `Saved` folder,
and only the editor reads it: a Standalone game or a packaged build always connects controllers.

Another game with this plugin does not need this: the editor already hands it the Bluetooth controllers
by itself (see [Switch 2](switch2.md#worth-knowing)).
