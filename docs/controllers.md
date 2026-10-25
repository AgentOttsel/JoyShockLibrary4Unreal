# Reacting to controllers

![Controllers Events](../Images/ControllerWaitNode.png)

Controllers finish enumerating on a background thread, at a moment nothing in the game controls: in the
editor they are usually ready before the level loads, in a packaged game they usually are not, and a
Bluetooth controller may arrive minutes in. Code that only handles "already connected" works on the
machine it was written on and nowhere else.

The **Wait For…** nodes remove that distinction. Each is a single latent node whose event pins carry their
payload as ordinary data pins — no Create Event, no Bind Event, no matching custom-event signatures:

- **Wait For Controller Changes** — `On Connected` fires immediately for every controller already
  connected and then for each new one, so there is no window in which a controller can be missed;
  `On Disconnected` reports the last known identity and whether reports simply stopped.
- **Wait For Any Controller Changes** — the same two pins, but for **every controller Unreal accepts**,
  including Xbox pads and everything else arriving through XInput. Use this one to decide when a *player*
  joins or leaves, and the JSL4U-only node above when you specifically need gyro, touchpad, lights or HD
  rumble. Both report the same `FJSL4UControllerInfo`; `bIsJoyShockController` says which kind arrived, and
  a controller the plugin does not drive reports the capability flags of its own hardware -- false for the
  gyro it does not have, true for the rumble it does. You rarely need to ask which kind arrived: every
  node takes the payload's `ConnectionId`, whichever kind it is. Keyboard and mouse are not reported: they share
  one device that is connected from the first frame, so announcing it would spawn a player before anyone
  touched anything.
- **Wait For Joy-Con Pairing Changes** — `On Joined` / `On Separated`, with both halves' identities
  already carrying their new grip mode, join partner and player assignment. Pairing actions are not
  replayed, because they are actions rather than persistent state; read **JSL4U Get Connected Controllers**
  when initial setup also needs the current pairing.
- **Wait For Battery Changes** — `On Low Battery` and `On Charging Changed`. See [Battery](#battery) below.
- **Wait For Controller Function Blocked** — see [Conflicts with other applications](output.md#conflicts-with-other-applications).

Because `On Connected` already replays, do **not** add a separate `For Each` over
**JSL4U Get Connected Controllers** alongside it; doing both processes every existing controller twice.

Each node's `Async Action` output accepts **Cancel**, which is how a widget stops listening when it is
removed. `Remove From Parent` does not unbind anything on its own, so a widget that keeps listening after
being removed will keep running its handler against a half-dismantled state.

The same events are also available as assignable delegates on the **JoyShock Subsystem**, for C++ and for
Blueprints that prefer that style. If you use those directly, get the subsystem from a `BeginPlay` — not
from a Game Instance's `Init`, because subsystems are created at the very end of `Init` and the events
would silently never fire.

`FJSL4UControllerInfo` includes two different identities:

- `ConnectionId` names the controller. It is the only address the plugin's nodes take, it is not reused during the run, and it exists for every controller Unreal accepts -- positive for one this plugin drives, negative for one it does not. Use it as the key of maps that track spawned Pawns or handled connections.
- `InputDeviceId` and `PlatformUserId` are Unreal's native identities, exposed as integers for Blueprint. `HardwareDeviceIdentifier` is the stable model name registered with the engine.

## One address, honest answers

Every per-controller node takes a `ConnectionId` and accepts one for **any** controller, including the Xbox pads this plugin does not drive. A game does not branch on whose controller it is holding; it asks the controller for what it wants, and what comes back is the truth about that hardware:

- **Readings** — `Get IMU State`, `Get Motion State`, `Get Touch State` and friends return zeroes for hardware that has no such sensor. Check `Has Motion Sensors` / `Has Touchpad` on the controller info before believing (or offering) a reading of zero.
- **Rumble** — works on every pad. For one of ours the plugin writes the motors over HID; for a foreign pad it routes through Unreal's own force-feedback channels. One caveat there: the engine rewrites those channels every frame for a pad that belongs to a player, so a direct value set on an **assigned** foreign pad survives about a frame. On an unassigned one — the assignment screen, "buzz this pad so you know which it is" — it stands. For rumble during play, on any pad, use Force Feedback.
- **Everything else** (gyro calibration, light colour, player indicator, HOME light) does nothing on hardware that lacks it, and logs one warning naming the node and the connection. One warning per controller per node, not one per call, so a node sitting in a Tick cannot bury the log.

A stored `ConnectionId` whose controller has been unplugged resolves to nothing: the call fails or does nothing rather than landing on whichever controller connected next. That is the whole reason the nodes take this id and not the library's own handle, which *is* reused.

## Battery

`Controller Info` reports **Battery Level** and **Is Charging** for Joy-Cons, Switch Pro, DualShock 4 and
DualSense, plus **Battery Percent** where the hardware measures one. **Wait For Battery Changes** turns
those into events: `On Low Battery` fires once per episode and re-arms when the controller is charged or
plugged in, and `On Charging Changed` reports the cable or dock going in and out.

Battery Level is a coarse enum — `Empty`, `Critical`, `Low`, `Medium`, `Full` — deliberately, not a
percentage: a Switch 1 controller reports five states, so a percentage there would be invented precision,
and a game written against a percentage misbehaves on exactly the controllers that cannot supply one.
The PlayStation controllers and the Switch 2 fill Battery Percent; it is `-1` elsewhere and is meant for
display, not for decisions. `Unknown` means the controller does not report charge at all, and is never a
low battery.

A Switch 2 controller reports its battery as the cell's terminal **voltage** rather than as a charge, so
its percentage is interpolated across a lithium cell's usable span (3.0 V empty to 4.2 V full). That is a
straight line where the real discharge curve is not one, and voltage sags under load — a controller reads
lower mid-rumble than it does at rest. Drive a bar from it, not a number. The measured voltage itself is
available from **JSL4U Get Switch 2 Sensors** if you want the reading rather than the estimate.
