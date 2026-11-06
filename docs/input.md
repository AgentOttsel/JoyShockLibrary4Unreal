# Input

Everything a standard gamepad has — face buttons, d-pad, shoulders, triggers, sticks — arrives as Unreal's
own gamepad keys, with no setup (see [Getting started](getting-started.md#level-1-nothing)). This page
covers what is left: the keys the plugin adds, and how a sideways Joy-Con maps onto a gamepad.

## Keys the plugin adds

All of them show up in Enhanced Input's key picker under the **JoyShock** prefix.

| Key | Type | Sent by |
|---|---|---|
| **JoyShock Home / PS Button** | button | PS on DualShock 4 and DualSense; HOME on Joy-Con R, Joy-Con 2 R, Pro Controller and Pro Controller 2 |
| **JoyShock Capture / TouchPad Click** | button | The touchpad click on DualShock 4 and DualSense; Capture on Joy-Con L, Joy-Con 2 L, Pro Controller and Pro Controller 2 |
| **JoyShock Mic Button** | button | The mute button on DualSense |
| **JoyShock Side Left** / **JoyShock Side Right** | button | SL and SR on a Joy-Con held upright or joined; the back buttons on DualSense Edge |
| **JoyShock Function Left** / **JoyShock Function Right** | button | The Fn buttons on DualSense Edge |
| **JoyShock C Button (Switch 2)** | button | C on Pro Controller 2 and Joy-Con 2 R |
| **JoyShock Grip Left GL (Switch 2)** / **JoyShock Grip Right GR (Switch 2)** | button | The rear grip buttons on Pro Controller 2 |
| **JoyShock TouchPad 1 2D-Axis** | Axis2D | The first finger on the DualShock 4 or DualSense touchpad — see [Touchpad](#touchpad) |
| **JoyShock TouchPad 1 X-Axis** / **Y-Axis** | Axis1D | The same, one component at a time |
| **JoyShock TouchPad 1 Touched** | button | Down while the first finger is on the pad |
| **JoyShock TouchPad 2 …** | | The same four for the second finger |
| **JoyShock Mouse L 2D-Axis (Switch 2)** / **Mouse R 2D-Axis (Switch 2)** | Axis2D | The mouse sensor under Joy-Con 2 L or R — see [Nintendo Switch 2 Joy-Con](switch2.md#the-extra-sensors) |
| **JoyShock Mouse L/R X-Axis** / **Y-Axis (Switch 2)** | Axis1D | The same, one component at a time |

Motion is bound through Unreal's own motion keys rather than plugin keys; see [Motion and gyro](motion.md).

## Sideways Joy-Cons

A Joy-Con on its own is played sideways, the way the Switch presents it, and becomes a small gamepad with
one stick:

| On the Joy-Con | Unreal key |
|---|---|
| Stick | Gamepad Left Thumbstick |
| Stick click | Gamepad Left Thumbstick Button |
| The four front buttons | The four face buttons, by their position once the Joy-Con is sideways |
| SL / SR | Gamepad Left Shoulder / Right Shoulder |
| − / + | Gamepad Special Left / Special Right |
| HOME / Capture | JoyShock Home / PS Button, JoyShock Capture / TouchPad Click |
| L / ZL or R / ZR | Nothing — out of reach when held sideways, and kept for the join chord |

Its motion is turned the same way; see [Sideways Joy-Cons](motion.md#sideways-joy-cons).

A **joined pair** is presented upright, as one full controller: the left half supplies the left stick, the
d-pad and its shoulders, the right half the right stick, the face buttons and its shoulders. SL and SR then
arrive as **JoyShock Side Left** / **Side Right**. How to join and separate is in
[Players and local multiplayer](players.md#combining-joy-cons-into-one-player).

## Only the window in front

A controller drives only the application that is in front. While another window is in front -- another
program, or the editor when a Standalone game is behind it, or the other way round -- the plugin keeps
the controllers connected and reading, but hands none of their input to Unreal; held buttons, sticks and
motion are released at that moment, and input resumes when the application is back in front. This is
stricter than Unreal's own default, which delivers gamepad input to a window in the background too. Play
In Editor runs inside the editor, so it answers whenever the editor is in front.

A side effect worth knowing: two game windows on one PC, such as a multi-process network session, take
controller input only while in front.

## Sticks

Every controller reports its sticks with the same convention, XInput's: up = +1 and right = +1 on both
sticks. One Mapping Context serves them all, and modifiers such as **To World Space** need no per-device
variant.

In a game, though, **Gamepad Right Y** reads -1 with the right stick pushed up. That is Unreal, not the
plugin: the game viewport negates that one key for every gamepad, an Xbox pad included, before Enhanced
Input sees it. So a camera that looks up with the left stick and the right stick alike needs a **Negate**
on one of the two, exactly as it would with an Xbox pad.

For movement, bind only **Gamepad Left 2D**. Binding **Gamepad Right 2D** to movement as well would let a
full controller's camera stick, and its drift, move the Pawn.

The direct `JSL4U` stick getters return what the plugin sends Unreal, a sideways Joy-Con's stick
included, so they read up = +1 on both sticks: they are not behind the viewport, so the right stick's
vertical is not negated there.

## Touchpad

The DualShock 4 and DualSense touchpad is exposed as **gamepad axes**, not as screen touches, so Enhanced
Input binds it exactly like a thumbstick, with the keys in the table above.

Values are centred like a stick: `(0, 0)` is the middle of the pad, X runs left to right and Y runs bottom
to top, matching the sticks so a shared mapping (**To World Space** included) behaves the same on both. A
finger that is not down reads `(0, 0)`, which is why **Touched** exists — a corner-relative `0..1` range
could not tell "no finger" from "finger at the top-left corner".

Do not use Unreal's **Touch 1 … Touch 10** keys for it. Those are for touchscreens, and a press on them
moves that player's UI focus off the game viewport, taking their controller's input with it. For a game
that genuinely wants the pad driving screen touch, `JoyShock.Touchpad.EmulateScreenTouch 1` turns that on.
