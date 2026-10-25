# Nintendo Switch 2 controllers

## Nintendo Switch 2 Pro Controller

The Switch 2 Pro Controller is supported over **USB and Bluetooth** on Windows. Over USB, just plug it in — the plug-in initializes it through its WinUSB interface (the Switch 2 uses a new protocol that is not compatible with Switch 1 controllers), reads its factory stick calibration and colors, and parses all of its inputs:

- All buttons, including the new **C (GameChat)**, **GL** and **GR** buttons
- Both analog sticks, using the per-unit factory calibration
- Gyro and accelerometer, fully integrated with the motion API (`JSL4U Get Motion State`, `JSL4U Get And Clear Accumulated Gyro`, calibration, etc.)
- HD rumble with real amplitude on both actuators
- Player-indicator LEDs, kept in sync with the controller's assigned Unreal Local Player
- Multiple Switch 2 Pro Controllers at the same time

### Bluetooth

**Do not pair the controller in Windows' Bluetooth settings.** It would not help: the Switch 2 has no
Bluetooth HID profile, so Windows has nothing to pair it *as* and never makes it a gamepad. It is a plain
Bluetooth LE peripheral with a vendor GATT service, and the plug-in speaks to that service directly —
scanning for the controller itself, connecting, and reading its input from a GATT notification.

To connect a controller the first time, **hold its SYNC button** (the small button next to the USB port)
until the lights start moving. The plug-in finds it, connects, and pairs it to this PC. From then on
**pressing any button** is enough — the controller reconnects on its own.

Everything above works the same over Bluetooth as over USB: the same buttons, the same calibrated sticks,
motion, HD rumble and player LEDs. A controller that is connected by cable is left on the cable; the radio
link is only taken for a controller that is not already here on USB.

On connecting, the plug-in asks Windows for a 7.5 ms connection interval. That request is what decides the
polling rate — a Bluetooth LE peripheral may only speak once per interval, and the default Windows picks
for a link it was told nothing about is tens of milliseconds, which turns a controller with input ready
every frame into one heard from a handful of times a second. If the radio refuses the request the
controller still works, just with more latency, and a line is logged saying so.

## Nintendo Switch 2 Joy-Con

The Joy-Con 2 connect the same way, and only that way — they never appear over USB, because the console
charges them through the rails and a cable to a PC gives nothing. Hold SYNC on each half the first time;
after that a button press brings it back.

A detached half behaves like a Switch 1 Joy-Con: it is its own controller, reports as `Joy-Con 2 (L)` or
`Joy-Con 2 (R)`, starts out sideways, and joins into a single two-stick player with the same chords —
**L or ZL on the left half together with R or ZR on the right** to join, **SL+SR on either half** to
separate. The Blueprint pairing nodes treat both generations identically.

Each half also carries the sensors the Switch 2 controllers have and nothing else does — magnetometer,
temperature, and the optical mouse sensor in its underside — all on **JSL4U Get Switch 2 Sensors**.

The mouse sensor reports an **absolute** position that runs 0..65535 on both axes and rolls over, which is
about 80 cm of desk on a scale of roughly 12000 units per length of a Pro Controller. Subtracting one
reading from an earlier one therefore reports a jump of a full 65536 every time it wraps. Two ways not to
deal with that yourself:

- **JoyShock Mouse L / Mouse R 2D-Axis (Switch 2)** are Enhanced Input keys, bound exactly like a
  thumbstick. There are two because a joined pair really is two mice for one player — the console uses them
  that way, and one axis would let one hand cancel the other. A detached half feeds only its own side; bind
  both to the same Input Action if you want "whichever half is in use". The value is a delta in the
  sensor's own counts, like Unreal's Mouse X / Mouse Y, so scale it in the mapping context.
- **JSL4U Consume Switch 2 Mouse Delta** is the same movement as a Blueprint node, for a game not using
  Enhanced Input for it. It consumes what it reports, so call it from one place. It keeps its own tally, so
  using it does not take anything away from the axis keys.
- **Mouse Travel**, on the sensors struct, is the movement accumulated instead of wrapped. It only grows,
  so differences between reads are just differences, and every caller sees the same value.

`Mouse Distance` is what tells you the controller is being used as a mouse at all: about 3000 in the air,
falling from roughly 5 mm out, and 140–150 resting on a surface. `Mouse Roughness` reads the surface —
about 4600 in the air, 4380 on a mousepad, 2500 on a bare desk, 2000 on cloth.

**Tested on hardware once**, by a contributor with a pair, which found and fixed three faults. What is
still unverified is the Bluetooth init's reliability: a controller whose calibration read fails falls back
to a fixed stick range and reaches about two thirds of full deflection, which feels like a character that
walks slower than everyone else's. If you see that, the log says which read failed and at what address.

The transport is Windows-only and needs a machine with Bluetooth LE. It is built against the Windows SDK's
C++/WinRT headers; if the build machine has no Windows 10/11 SDK carrying those, the plug-in still builds
and logs that Switch 2 controllers will be USB-only.

The Switch 2 command endpoint is WinUSB-exclusive even though its HID input endpoint is visible to more
than one process. This matters for Unreal's multi-process Standalone mode: the parent editor and the
Standalone game can discover the same controller. JSL4U releases an idle WinUSB command lease after one
second; a process that initially lost the race retries calibrated initialization after the lease becomes
available. Thus the playing process still reads the controller's factory stick centres and can use rumble
and LEDs without requiring Steam — or the parent editor — to be closed.
