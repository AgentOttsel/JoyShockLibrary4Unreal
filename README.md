[Português](README.pt-BR.md) · **English**

# JoyShockLibrary4Unreal

An Unreal Engine plugin that makes PlayStation and Nintendo controllers work natively in your game:
DualShock 4, DualSense (including the Edge), Joy-Con, Pro Controller and Pro Controller 2, with the
features only they have, such as gyro, touchpad, light bar and joining two Joy-Cons into a single player.
The Pro Controller 2 connects over Bluetooth without being paired in Windows first.

**It fills the gaps rather than replacing Unreal's input.** Anything a standard gamepad can do — buttons,
sticks, triggers, rumble, motion — reaches your game through Unreal's own input system, as the same keys
and the same nodes an Xbox pad already uses. The `JSL4U*` nodes exist only for what Unreal and Windows
have no concept of: gyro calibration, the light bar, joining two Joy-Cons, deciding which controller
belongs to which player. Write your game against the engine's APIs and it supports every gamepad; reach
for a `JSL4U*` node when you want something only these controllers can do.

## Contents

- [Supported controllers](#supported-controllers)
- [Installation](#installation)
- [Getting started](#getting-started)
- [Documentation](#documentation)
- [Questions](#questions)
- [Credits](#credits)

## Supported controllers

- **PlayStation:** DualShock 4, DualSense and DualSense Edge, over USB and Bluetooth.
- **Switch 1:** Joy-Con (solo and sideways, or as a pair), over Bluetooth, and Pro Controller, over USB and
  Bluetooth.
- **Switch 2:** Pro Controller 2, over USB and Bluetooth. The Joy-Con 2 is implemented, over Bluetooth
  only, but has not yet been confirmed working.
- **Xbox and other XInput pads** stay with Unreal; the plugin coexists with them and respects the player
  slots they hold.

What has been tested on each, and whom to ask for a test, is in [docs/hardware.md](docs/hardware.md).

## Installation

- Download or clone this repository into your project's `Plugins` folder. The path to the Content folder
  should look like this: `<project>/Plugins/JoyShockLibrary4Unreal/Content`.
- Make sure JoyShockLibrary4Unreal is enabled in your `.uproject` or under *Edit → Plugins*.
- On Windows, no other plugin is needed: hidapi is bundled. On other platforms the plugin uses microdee's
  [HIDUE](https://github.com/microdee/HIDUE) for HID communication; add it to your project as well.

## Getting started

There are three levels, and most projects only ever need the first:

1. **Nothing.** Once the plugin is installed, a game built for gamepads already accepts these controllers:
   Input Mapping Contexts, `Play Force Feedback Effect` and everything else keep working.
2. **Reacting to controllers coming and going**, for a controller-assignment screen or per-controller UI:
   the **Wait For Controller Changes** node.
3. **Local multiplayer:** **Ensure Local Player For Controller** for each controller that connects.

Each level is walked through in [docs/getting-started.md](docs/getting-started.md).

The plugin ships a demo level, `LV_JoyShockDemo`, with local multiplayer and a 3D mirror of each connected
controller. See [docs/demo.md](docs/demo.md).

## Documentation

| Page | What it covers |
|---|---|
| [Hardware support](docs/hardware.md) | What has been tested on each controller, and who can test what |
| [Getting started](docs/getting-started.md) | The three setup levels, step by step |
| [Demo level](docs/demo.md) | How the demo level is put together |
| [Input](docs/input.md) | Every key the plugin adds, sideways Joy-Cons, sticks, touchpad |
| [Motion and gyro](docs/motion.md) | Motion through Enhanced Input, gyro space, calibration |
| [Reacting to controllers](docs/controllers.md) | The **Wait For…** nodes, `ConnectionId`, battery and model identifiers |
| [Players and local multiplayer](docs/players.md) | Player slots, more than four players, joining Joy-Cons |
| [Rumble, lights and output](docs/output.md) | Rumble, lights, player indicator, conflicts with other applications |
| [Nintendo Switch 2 controllers](docs/switch2.md) | Pro Controller 2 and Joy-Con 2, USB and Bluetooth, extra sensors |
| [Blueprint nodes](docs/blueprint-nodes.md) | How the `JSL4U*` API is organised |
| [Diagnostics and troubleshooting](docs/troubleshooting.md) | Diagnostic commands, Bluetooth issues and testing in the editor |
| [Source layout](docs/architecture.md) | For changing the code: the layers, and where to change what |

## Questions

**Which versions of Unreal has this been tested with?** Only 5.8. The plugin uses the
`FInputDeviceRegistry` API. Epic Games has marked `FInputDeviceScope` as deprecated, and it will be
removed in a future version of the engine.

**Why use this instead of Steam Input or DS4Windows?** They translate these controllers into XInput, and
do it well, even giving gyro aiming to games that do not have it. With this plugin the game accepts the
controllers and their features by itself, without the player needing anything running in the background.

**Can the motion processing be used with the official Sony and Nintendo libraries?** No official library
was used in its development, so we cannot say. The functions in `GamepadMotion.hpp` process motion data
wherever it comes from.

## Credits

- JibbSmart, for creating the original [JoyShockLibrary](https://github.com/JibbSmart/JoyShockLibrary) and
  for answering the questions sent to his Twitter DMs. The full credits of the original library are in his
  repository.
- microdee, for the [HIDUE](https://github.com/microdee/HIDUE) plugin, the plugin's HID layer outside
  Windows.
- The DualSense 3D model is by [Saleem Akhtar](https://www.artstation.com/marketplace/p/zBM9R/ps5-duelsense-controller-3d-model-fbx).
- The Joy-Con, Joy-Con grip and DualShock 4 models used by the demo's controller mirror were modelled by
  [uayten](https://github.com/uayten) for this plugin.
- [uayten](https://github.com/uayten), for the 07/2026 overhaul: Unreal Engine 5.8 native input and local
  multiplayer; stable controller discovery, identity and player routing; Joy-Con horizontal play,
  joining, separation and player LEDs; the complete implementation of the Pro Controller 2; battery
  reporting; cable/Bluetooth transport switching; the per-controller mirror, sensor HUD and demo workflow;
  consistent engine-facing stick axes; and the controller connection, freeze and shutdown-crash fixes.
