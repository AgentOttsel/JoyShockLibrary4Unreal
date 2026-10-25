# Rumble, lights and output

## Rumble

These controllers work with **Unreal's own force feedback**. `Play Force Feedback Effect`, `Client Play Force Feedback` and Enhanced Input's force feedback effects drive a DualShock 4, DualSense, Joy-Con or Pro Controller exactly as they drive an Xbox pad — so authored effects with curves, falloff and looping work, and you get one code path for every gamepad. Nothing to enable. Effects are aimed at a *player*, so both halves of a joined Joy-Con pair rumble together.

The channels map the way Unreal's XInput interface reads them: `LeftLarge` drives the heavy/low-frequency motor and `RightSmall` the light/high-frequency one, so an effect authored against a standard gamepad comes out the same here.

For direct control there is `JSL4U Set Controller Rumble (ConnectionId, SmallRumble, BigRumble)`, 0-1 per motor (call it with `(0, 0)` to stop). Both routes reach the same maximum — force feedback is clamped to 0-1 and 1 arrives as full strength — so an effect that feels weak is a weak curve in the asset rather than a limit here. (`Force Feedback Scale` and `b Force Feedback Enabled` on the PlayerController also apply.)

Use the direct node for the three things force feedback cannot do, all of them because it is aimed at a *player* rather than a controller: rumbling **one specific controller** (a joined Joy-Con pair is one player but two controllers, so only this can buzz just the left one); rumbling a controller **not assigned to any player**, which is what a controller-assignment screen needs for "press here and feel which controller this is" before players exist; and holding a constant intensity without authoring a looping asset.

The two are independent — a force feedback effect plays over a rumble you set directly, and neither cancels the other. Each motor runs at whichever of the two is stronger.

Per controller family:

- **DualShock 4 / DualSense**: small and big motor intensities.
- **Joy-Cons / Pro Controller (Switch 1)**: full HD-rumble amplitude control — `BigRumble` drives the low-frequency component (heavy shake), `SmallRumble` the high-frequency one (fine buzz). The vibration is sustained automatically until you set `(0, 0)`.
- **Switch 2 Pro Controller**: full HD-rumble amplitude control, the same as the Switch 1 — `BigRumble` drives the low-frequency component, `SmallRumble` the high-frequency one, and the vibration is sustained until you set `(0, 0)`. Each packet carries about 15ms of waveform, so the plug-in feeds the controller continuously while a rumble is held. Works over both USB and Bluetooth.
- Note: close Steam when using the Switch 2 Pro Controller with Unreal — Steam holds the controller's USB command interface exclusively, which blocks the plugin's init and rumble (the plugin logs a warning and re-acquires the interface automatically once Steam is closed). Rumble also has an HID fallback that works without that interface, and Bluetooth is unaffected either way.

## Lights and player indicators

![Controllers Events](../Images/NintendoPlayerLed.png)

**Player indicators follow Unreal assignment automatically.** Joy-Cons, Switch Pro and Switch 2 Pro
Controllers use Nintendo's distinct player patterns (including combination patterns for players 5-8),
and joined Joy-Con halves show the same player. DualSense uses its five native player patterns. Calling
**JSL4U Set Player Indicator** manually overrides the displayed one-based number until the next controller
assignment change.

Nintendo's blue HOME-button light is a notification light, not a player number. Controllers can retain
that light from firmware or a previous host, so JSL4U clears it on Joy-Con R and Switch Pro after their
input stream starts, using Nintendo's explicit zero-brightness pattern; assignment uses only the four green
player LEDs. A game that wants that light as a feedback channel can take it with **JSL4U Set Home Light**,
after which the plugin stops clearing it on that controller for the rest of the connection. Before the game
or editor process opens, the plugin is not running and cannot change a controller's retained/default LED
state. Switch 2 Pro initialization likewise preserves its current indicator until Unreal assigns its actual
Local Player, instead of briefly turning every player LED off.

DualShock 4 has no numeric player LEDs, so **Set Player Indicator** intentionally does nothing on it. To
test its RGB light bar, call **JSL4U Set Light Color** with its `ConnectionId` and an obvious color such as
magenta. The light bar and rumble are separate fields in the same output report, so a broken rumble motor
does not by itself prevent the color test.

## Conflicts with other applications

Steam Input, DS4Windows and similar tools can hold a controller's output path while input keeps flowing,
so rumble or player LEDs stop working with nothing to explain why. **Wait For Controller Function Blocked**
fires when an output write is rejected while the controller is still delivering input, naming which
capability is affected: `Rumble`, `Player Indicator`, `Home Light` or `Motion Sensor`. It fires once per
function until that function works again, and only when the game actually uses it.

Only failures the OS reports are detectable this way. An application that swallows output without failing
the write cannot be distinguished from one that is not running.
