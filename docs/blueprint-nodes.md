# Blueprint nodes

The API is `JSL4U*` and nothing else. The original library's `Jsl*` layer — its raw integer enums, loose float outputs and synchronous device scan — is gone entirely, not merely hidden from Blueprint: half of it had no caller left, and the half that did was folded into the internal `…ForHandle` helpers the `JSL4U*` nodes call. A node takes a connection id; a helper takes a device handle; there is no third convention.

Current nodes are grouped by responsibility:

- **Events** — the latent **Wait For…** nodes.
- **Controllers** — discovery, connection checks and complete controller identity. **JSL4U Get Switch 2 Sensors** also lives here: the battery voltage, temperature, magnetometer and underside mouse sensor that only the Switch 2 controllers carry, along with **JSL4U Consume Switch 2 Mouse Delta**. Motion is *not* there — accelerometer and gyroscope come from the Motion nodes, which every controller answers.
- **Controller Assignment** and **Local Multiplayer** — controller-to-player routing and native Local Player creation.
- **Joy-Con Pairing** — joining and separating Joy-Cons, and resolving a pair from either half.
- **Input State**, **Motion** and **Touchpad** — direct state queries for features not already better handled by Enhanced Input. The touchpad is bindable in Enhanced Input too — see [Touchpad](input.md#touchpad).
- **Gyro Calibration** — calibration mode, manual calibration, status and persistent offsets.
- **Output** — light color, player indicator, HOME light and direct-device rumble.
- **Diagnostics** — hardware resolution, poll interval and time since the last input report.

Node names and tooltips state units, invalid-result behavior and when Unreal's own Enhanced Input or Force Feedback should be preferred. Tooltips also say which node is the one most games need, and which exist only for less common cases. Search for `JSL4U` to list the complete callable API.

The old empty **Project Settings → Plugins → JoyShockLibrary4Unreal** page has been removed. It had no active setting and suggested configuration that the runtime never read; controller behavior is now either standard Unreal behavior or an explicit `JSL4U*` call.

JSL4U functions favour Unreal Engine's types and standards, so instead of returning three loose floats for an acceleration vector, they return an `FVector`. Vectors and quaternions use Unreal's left-handed, Z-up coordinate system rather than the original library's right-handed, Y-up axes.
