# Motion and gyro

Motion is reported through **Unreal's own motion input**, the same path a phone's gyro and accelerometer use. That means `Tilt`, `RotationRate`, `Gravity` and `Acceleration` can be bound in Enhanced Input like any other axis, with no plugin-specific Blueprint code — `RotationRate` is the one you want for gyro aiming. The direct getters (`JSL4U Get IMU State`, `JSL4U Get Motion State`) are still there and use the same axes, so you can mix the two freely.

`JSL4U Get Motion State` returns a gravity-corrected orientation, so pitch and roll are absolute — they
stay anchored to real-world down rather than accumulating error. Yaw still drifts, as it must without a
magnetic or visual reference, which is what makes a "recentre" action worth offering in a game that maps
absolute orientation to anything.

**JSL4U Set Gyro Space** chooses the frame of reference gyro input is reported in — *Local Space* (raw,
relative to the controller), *World Space* (corrected by the measured gravity direction, so yaw is always
around the real vertical), or *Player Space* (a blend that is as adaptive as world space and as robust as
local space). Player Space is the usual choice for gyro aiming, and is worth exposing as a player
preference in a game that aims with gyro.

## Sideways Joy-Cons

A solo Joy-Con's stick and buttons are turned for horizontal play (see
[Input](input.md#sideways-joy-cons)), and its motion is rotated with them. A Joy-Con held sideways is the same hardware turned a quarter turn about
the axis out of its face, and its IMU does not know that: without correction, pointing the far end of a
sideways Joy-Con up arrives as *roll*, and the pitch a game reads never moves. The plugin takes that quarter turn out of orientation,
gravity, acceleration and local-space gyro, so `Pitch` means pitch and `Yaw` means yaw in either grip —
through the direct getters and through Enhanced Input's motion keys alike, which apply the same rotation.
World- and player-space gyro are left alone: those are already resolved against gravity and so are
grip-independent by construction. Joining two halves returns both to the vertical presentation and the
rotation disappears with it.

There is one place this shows: a mesh driven straight from `Get Motion State` draws a sideways Joy-Con
upright, because upright is the pose its input is being reported in. That is deliberate — the reading
describes how the controller is being *played*, not how it is lying — and it is the same answer for every
controller, so nothing in a project needs a Joy-Con special case.

## Gyro calibration

A gyroscope reports a small non-zero rotation even when perfectly still, so a controller left alone slowly drifts. Calibrating measures that offset while the controller is still and subtracts it. Nodes live under **JoyShock Library | Gyro Calibration**.

**Most games need none of them.** Every controller starts in *Automatic*, which works out for itself when
the controller is being held still and keeps the offset current. Calibration is handled unless you decide
otherwise, and **JSL4U Set Gyro Calibration Mode** exists to opt out with *Manual*.

The rest exist for games that want an explicit calibration screen:

- **JSL4U Start / Stop Manual Gyro Calibration** — gather samples for the drift offset while the controller sits still, then stop. Only meaningful in Manual mode.
- **JSL4U Reset Gyro Calibration** — throw the current offset away and start over. This is the one manual node worth exposing to players even in Automatic mode, because an offset measured while the controller was moving leaves the gyro worse off than no calibration at all, and this is the way out. It suits a "Recalibrate gyro" button.
- **JSL4U Get Gyro Calibration Status** — what drives a calibration screen's prompts and progress.
- **JSL4U Get / Set Gyro Calibration Offset** — read the offset as a **Vector** to save it per controller and restore it next session, so a returning player doesn't have to calibrate again. These use the same axes as *JSL4U Get IMU State*.

The status struct's three fields are easy to misread, so they are named after the mechanism they belong
to. **Auto Calibration Confidence** only ever climbs: it is a high-water mark meaning "this controller was
measured well at some point", not a live reading. **Auto Calibration Sampling Now** is the automatic
calibrator's own gate, true only while it is taking samples — it is *not* a general "the controller is
still" reading, and a controller lying untouched on a desk commonly reports false once the calibrator has
settled. **Manual Calibration Running** is the manual counterpart, true between Start and Stop. For a
genuine "is it moving right now" readout, compare the length of the gyro vector from *Get IMU State*
against a small threshold.
