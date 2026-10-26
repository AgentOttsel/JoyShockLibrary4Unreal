# Nintendo Switch 2 controllers

| Controller | Connection | State |
|---|---|---|
| Pro Controller 2 | USB and Bluetooth | Tested on USB; see [Hardware support](hardware.md) for what is confirmed |
| Joy-Con 2 | Bluetooth only | Implemented, not yet confirmed working |

The Switch 2 speaks a new protocol, not compatible with the Switch 1 controllers', so it has its own path
through the plugin — and over Bluetooth that path is very different from every other controller's.

## Why Bluetooth works differently here

**Every other controller this plugin supports is a Bluetooth HID device.** The DualShock 4, DualSense,
Joy-Con and Pro Controller announce themselves as gamepads: you pair them in Windows' Bluetooth settings,
Windows turns them into ordinary HID devices, and the plugin reads them exactly as it reads one on a cable.

**The Switch 2 controllers are not, and Windows has no native support for them.** Over Bluetooth they have
no HID profile at all. They are plain Bluetooth LE peripherals carrying a service of Nintendo's own, so
Windows has nothing to pair them *as*: pairing one in Settings never produces a gamepad, and nothing on the
system can read it.

So the plugin does the whole job itself. It scans for the controller, connects to it, pairs it to the PC,
and reads its input straight from that Nintendo service. What follows from that:

- **Do not pair a Switch 2 controller in Windows' Bluetooth settings.** It does not help, and the plugin
  does not need it. Connect it as described below instead.
- **It needs a PC with Bluetooth LE**, and it works on Windows only.
- **The plugin asks Windows for a 7.5 ms connection interval** on connecting. A Bluetooth LE peripheral may
  only speak once per interval, and the default Windows picks for a link it knows nothing about is tens of
  milliseconds — a controller heard from only a handful of times a second. If the radio refuses, the
  controller still works with more latency, and the log says so.
- **The build needs the Windows SDK's C++/WinRT headers.** Without a Windows 10/11 SDK carrying them, the
  plugin still builds, and logs that Switch 2 controllers will be USB-only.

## Connecting

| | First time | After that |
|---|---|---|
| **Pro Controller 2, USB** | Plug it in | Plug it in |
| **Pro Controller 2, Bluetooth** | Hold **SYNC**, the small button next to the USB port, until the lights start moving | Press any button |
| **Joy-Con 2** | Hold **SYNC** on each half | Press any button on that half |

The Joy-Con 2 never appear over USB: the console charges them through the rails, and a cable to a PC gives
nothing. A Pro Controller 2 already connected by cable stays on the cable; Bluetooth is only used for one
that is not.

## Pro Controller 2

Over either connection it delivers the same things:

- every button, including **C** (GameChat), **GL** and **GR** — see [Input](input.md#keys-the-plugin-adds);
- both sticks, with the unit's own factory calibration;
- gyro and accelerometer, through the same motion path as every other controller — see [Motion and gyro](motion.md);
- HD rumble on both actuators — see [Rumble](output.md#rumble);
- player LEDs that follow the controller's Unreal player;
- several Pro Controller 2 at the same time.

## Joy-Con 2

A detached half behaves like a Switch 1 Joy-Con: it is its own controller, reports as `Joy-Con 2 (L)` or
`Joy-Con 2 (R)`, starts out sideways, and joins into one two-stick player with the same chords — **L or ZL
on the left half together with R or ZR on the right** to join, **SL+SR on either half** to separate. The
pairing nodes treat both generations alike.

A first hardware session found and fixed three faults. What has not been confirmed since is the Bluetooth
set-up's reliability: a half whose calibration read fails falls back to a fixed stick range and reaches
about two thirds of full deflection — a character that walks slower than everyone else's. If you see that,
the log says which read failed and at what address.

### The extra sensors

Each Joy-Con 2 half carries sensors no Switch 1 or PlayStation controller has — a magnetometer, a temperature
sensor, and an optical mouse sensor in its underside — all read with **JSL4U Get Switch 2 Sensors**. The
Pro Controller 2 has the magnetometer, read over Bluetooth only, and no mouse sensor.

The mouse sensor reports an **absolute** position from 0 to 65535 on each axis that rolls over: about 80 cm
of desk, on a scale of roughly 12000 units per length of a Pro Controller. Subtracting one reading from an
earlier one therefore jumps by 65536 every time it wraps. Three ways not to deal with that yourself:

| Use | What it gives |
|---|---|
| **JoyShock Mouse L / Mouse R 2D-Axis (Switch 2)** | Enhanced Input keys, bound like a thumbstick. A delta in the sensor's own counts, like Unreal's Mouse X / Mouse Y, so scale it in the Mapping Context. There are two because a joined pair really is two mice for one player; bind both to one Input Action for "whichever half is in use" |
| **JSL4U Consume Switch 2 Mouse Delta** | The same movement as a node. It consumes what it reports, so call it from one place; it does not take anything away from the keys |
| **Mouse Travel**, on the sensors struct | The movement accumulated instead of wrapped. It only grows, so differences between reads are just differences, and every caller sees the same value |

Two more fields say whether the controller is being used as a mouse at all:

| Field | In the air | On a surface |
|---|---|---|
| **Mouse Distance** | About 3000, falling from roughly 5 mm out | 140–150 |
| **Mouse Roughness** | About 4600 | 4380 on a mousepad, 2500 on a bare desk, 2000 on cloth |

## Worth knowing

**Steam and USB.** Steam holds the Pro Controller 2's USB command interface for itself, which blocks the
plugin's set-up and rumble — close Steam while using it over USB. See
[Conflicts with other applications](output.md#conflicts-with-other-applications).

**Standalone from the editor works.** The USB command interface can only be held by one process at a time,
and Unreal's Standalone mode runs the game as a second process next to the editor. The plugin releases an
idle hold after one second, and a process that lost the race retries, so the game still gets the factory
stick calibration, rumble and LEDs without closing the editor.

**Over Bluetooth it does not.** A Bluetooth connection belongs to the process that opened it, and nothing
hands it over: if the editor connected the controller, the Standalone game never sees it. Play in the
editor's viewport, use the cable, or close the editor and launch the game on its own.
