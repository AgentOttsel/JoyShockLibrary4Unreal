# Blueprint nodes

The API is `JSL4U*` and nothing else. The original library's `Jsl*` layer — its raw integer enums, loose float outputs and synchronous device scan — is gone entirely, not merely hidden from Blueprint: half of it had no caller left, and the half that did was folded into the internal `…ForHandle` helpers the `JSL4U*` nodes call. A node takes a connection id; a helper takes a device handle; there is no third convention.

Node names and tooltips state units, invalid-result behavior and when Unreal's own Enhanced Input or Force Feedback should be preferred. Tooltips also say which node is the one most games need, and which exist only for less common cases.

## Every node

Every node the plugin adds, grouped as Unreal's node menu groups them, under **JoyShock Library**. Unless
it says otherwise, a node takes the controller's `ConnectionId` and answers for any controller Unreal
accepts — see [One address, honest answers](controllers.md#one-address-honest-answers).

The nodes marked *subsystem* are called on the **JoyShock Subsystem**, a Game Instance subsystem: get it
first (searching the node menu for *JoyShock Subsystem* finds the getter), then call the node on it.

### Events

The **Wait For…** nodes are latent: place one, and its pins fire whenever the event happens, carrying the
controller info as data. Each returns an `Async Action` that accepts **Cancel**. Why they are nodes rather
than bindings is in [Reacting to controllers](controllers.md).

| Node | Pins | What it does |
|---|---|---|
| **Wait For Controller Changes** | On Connected, On Disconnected | This plugin's controllers. Controllers already connected fire On Connected at once, so none can be missed |
| **Wait For Any Controller Changes** | On Connected, On Disconnected | Every controller Unreal accepts, Xbox and other XInput pads included. The one to decide when a *player* joins or leaves |
| **Wait For Joy-Con Pairing Changes** | On Joined, On Separated | Joy-Con joins and separations. Not replayed: read **JSL4U Get Connected Controllers** for the current pairing |
| **Wait For Battery Changes** | On Low Battery, On Charging Changed | Charge across every controller. *Warn At Level* (advanced pin, default Low) sets when On Low Battery fires |
| **Wait For Controller Function Blocked** | On Blocked | A function (rumble, player LEDs, HOME light, motion) rejected by another application such as Steam |
| **Listen For Controllers** (*subsystem*) | — | Binds an event, calls it once for every connected controller, then for each new one |
| **Listen For Controller Changes** (*subsystem*) | — | The same as Wait For Controller Changes, as a binding |
| **Listen For Joy-Con Pairing Changes** (*subsystem*) | — | The same as Wait For Joy-Con Pairing Changes, as a binding |

The subsystem also has the assignable events behind them, for C++ or for Blueprints that prefer binding:
**On Controller Connected**, **On Controller Disconnected**, **On Joy-Cons Joined**, **On Joy-Cons
Separated** and **On Controller Function Blocked**.

### Controllers

| Node | What it does |
|---|---|
| **JSL4U Get Connected Controllers** | Every connected controller this plugin drives, with its identities, model, assignment and settings |
| **JSL4U Get All Connected Controllers** | Every controller Unreal accepts, this plugin's and XInput pads alike, in one list |
| **JSL4U Get Controller Info** | One controller's identities, model, assignment and settings. *Is Connected* is false for an id that names no live controller |
| **JSL4U Is Controller Connected** | Whether this `ConnectionId` names a connected controller right now |
| **JSL4U Refresh Controllers** | Asks for a rescan. Rarely needed: device changes are detected on their own |
| **JSL4U Get Switch 2 Sensors** | Battery voltage, temperature, magnetometer and mouse sensor of a Switch 2 controller. *Is Supported* is false on any other |
| **JSL4U Consume Switch 2 Mouse Delta** | The Switch 2 mouse sensor's movement since the last call, wrap already resolved. Consumes what it reports: call it from one place |

### Controller Assignment

| Node | What it does |
|---|---|
| **JSL4U Assign Controller To Player Index** | Puts a controller on a zero-based player slot; -1 hands it back to automatic assignment. Joined Joy-Cons move together |
| **JSL4U Assign Controller To Player** | The same, addressed by Player Controller |
| **JSL4U Get Controllers Assigned To Player Index** | Every controller feeding a player slot. A joined pair returns both halves |
| **JSL4U Get Controllers Assigned To Player** | The same, from a Controller — the one from a Pawn's Possessed event plugs straight in |
| **JSL4U Set Max Local Players** | How many local players the game may create, above the engine's default of 4. Call before creating players |
| **JSL4U Get Max Local Players** | Reads that limit back; -1 when there is no game viewport |

### Local Multiplayer

| Node | What it does |
|---|---|
| **Ensure Local Player For Controller** (*subsystem*) | Finds the controller's Local Player, creates it if asked, and reconciles the assignment. Returns its Player Controller and index. Spawns no Pawn and adds no Mapping Context |
| **Find Local Player For Controller** (*subsystem*) | The Player Controller whose Local Player owns this controller, or None |

### Joy-Con Pairing

| Node | What it does |
|---|---|
| **JSL4U Join Joy-Cons** | Joins a left and a right Joy-Con into one vertical controller for one player |
| **JSL4U Unjoin Joy-Con** | Separates the pair this half belongs to |
| **JSL4U Unjoin All Joy-Cons** | Separates every pair |
| **JSL4U Set Joy-Con Grip Mode** | Horizontal or Vertical for one Joy-Con. Horizontal on a joined half separates the pair |
| **JSL4U Get Joy-Con Pair** | From either half: whether it is joined, and both halves' infos, Primary first |
| **JSL4U Get Joy-Con Partner** | Whether this half is joined right now, and the partner's `ConnectionId` |
| **JSL4U Is Joy-Con Primary** | Whether this device represents its logical controller — always for a standalone one, for exactly one half of a pair |
| **JSL4U Is Controller Type Joinable** | Whether a controller type can be joined (today, left and right Joy-Cons) |

### Input State

For screens and diagnostics that need a controller before it belongs to a player. For gameplay, bind
Enhanced Input instead.

| Node | What it does |
|---|---|
| **JSL4U Get Controller State** | Buttons, sticks and triggers of one controller |
| **JSL4U Get Left Stick** / **JSL4U Get Right Stick** | One stick's position, -1 to 1 |

### Motion

| Node | What it does |
|---|---|
| **JSL4U Get IMU State** | Gyro and acceleration in Unreal axes, in the chosen gyro space |
| **JSL4U Get Raw IMU State** | The same, ignoring the gyro space |
| **JSL4U Get Motion State** | Orientation, acceleration and gravity, gravity-corrected |
| **JSL4U Get Raw Motion State** | The same, untransformed |
| **JSL4U Get And Clear Accumulated Gyro** | Gyro rotation accumulated since the last call, then cleared |
| **JSL4U Set Gyro Space** | Local, World or Player Space; Player Space is the usual choice for aiming |

### Gyro Calibration

Most games need none of these: every controller starts in Automatic. See
[Gyro calibration](motion.md#gyro-calibration).

| Node | What it does |
|---|---|
| **JSL4U Set Gyro Calibration Mode** | Automatic (the default) or Manual |
| **JSL4U Start Manual Gyro Calibration** / **JSL4U Stop Manual Gyro Calibration** | Gather drift samples while the controller sits still, then keep the offset. Manual mode only |
| **JSL4U Reset Gyro Calibration** | Throws the offset away and starts over — the one worth a "Recalibrate gyro" button |
| **JSL4U Get Gyro Calibration Status** | Mode, confidence and sampling state, for a calibration screen |
| **JSL4U Get Gyro Calibration Offset** / **JSL4U Set Gyro Calibration Offset** | Save and restore the offset per controller |

### Touchpad

The touchpad is bindable in Enhanced Input too — see [Touchpad](input.md#touchpad).

| Node | What it does |
|---|---|
| **JSL4U Get Touch State** | Both touch contacts; *Previous* reads the report before the current one |
| **JSL4U Get Touch Position** | One contact's position, 0 to 1 |
| **JSL4U Get Touchpad Size** | The pad's native width and height; zero without a touchpad |

### Output

| Node | What it does |
|---|---|
| **JSL4U Set Controller Rumble** | Rumble on one controller, 0 to 1 per motor; (0, 0) stops it. Works before the controller has a player |
| **Stop All Controller Rumble** (*subsystem*) | Stops rumble on every controller. Also runs when the game shuts down |
| **JSL4U Set Light Color** | The DualShock 4 or DualSense light bar |
| **JSL4U Set Player Indicator** | A one-based player number on Switch, Switch 2 or DualSense player LEDs |
| **JSL4U Set Home Light** | HOME light brightness, 0 to 1, on a right Joy-Con or Pro Controller. The plugin stops clearing it from then on |
| **JSL4U Get Switch Player LED Pattern** | The four Nintendo player LEDs for player numbers 1 to 8 |

### Diagnostics

| Node | What it does |
|---|---|
| **JSL4U Get Stick Resolution Step** | The smallest change one stick axis can report |
| **JSL4U Get Trigger Resolution Step** | The smallest trigger change; 1 for the Switch's digital triggers |
| **JSL4U Get Poll Interval** | Expected milliseconds between input reports |
| **JSL4U Get Seconds Since Last Report** | Seconds since the last input report; a value that keeps growing means the controller stopped reporting |
