# Motion and gyro

Every controller this plugin drives has a gyroscope and an accelerometer. An Xbox pad has neither and
reads zero; check **Has Motion Sensors** on the controller info before offering a motion feature.

## Binding motion

Motion arrives through **Unreal's own motion input**, the same path a phone's sensors use, so Enhanced
Input binds it like any other axis, with no plugin-specific Blueprint code:

| Unreal key | What it measures | Typical use |
|---|---|---|
| **Rotation Rate** | How fast the controller is turning | Gyro aiming and camera |
| **Tilt** | The controller's orientation, in radians | Steering, balancing, tilting a level |
| **Gravity** | Which way is down, as the controller sees it | Telling how the controller is held |
| **Acceleration** | The accelerometer's reading, gravity included | Shakes, swings, flicks |

The direct nodes — **JSL4U Get IMU State**, **JSL4U Get Motion State** — use the same axes, so the two
can be mixed freely. They also read a controller before it belongs to any player, which suits a
calibration screen.

Pitch and roll from **Get Motion State** are absolute: they stay anchored to real-world down. Yaw drifts
slowly, as it must without a magnetic or visual reference, which is why a game that maps absolute
orientation to anything should offer a "recentre" action.

## Gyro space

**JSL4U Set Gyro Space** chooses the frame gyro input is reported in:

| Space | What it does | When to use it |
|---|---|---|
| **Player Space** | A blend: as adaptive as World Space and as robust as Local Space | Gyro aiming — the usual choice |
| **World Space** | Corrected by the measured gravity, so yaw is always around the real vertical | When yaw must turn around the real vertical, however the controller is held |
| **Local Space** | Raw, relative to the controller itself | When you want the untouched reading |

In a game that aims with gyro, the space is worth exposing as a player preference.

## Sideways Joy-Cons

A Joy-Con held sideways is the same hardware turned a quarter turn, and its sensor does not know that:
uncorrected, tilting the far end up would read as *roll*. The plugin takes that quarter turn out of
orientation, gravity, acceleration and local-space gyro, through Enhanced Input and the direct nodes
alike, so `Pitch` means pitch in either grip. World and Player Space need no correction, since they are
already resolved against gravity. A joined pair is upright, so nothing is turned.

What this shows: a 3D model driven straight from **Get Motion State** draws a sideways Joy-Con upright.
That is deliberate — the reading describes how the controller is being *played*, not how it is lying —
so nothing in a project needs a Joy-Con special case.

## Gyro calibration

A gyroscope reports a small rotation even when perfectly still, so an uncalibrated controller slowly
drifts. Calibrating measures that offset while the controller is still and subtracts it.

| If the game wants… | Do this |
|---|---|
| Nothing special | Nothing. Every controller starts in **Automatic**, which notices when the controller is held still and keeps the offset current |
| A "Recalibrate gyro" button | **JSL4U Reset Gyro Calibration**. An offset measured while the controller was moving is worse than none, and this is the way out — worth offering even in Automatic |
| Its own calibration screen | **JSL4U Set Gyro Calibration Mode** to *Manual*, then **JSL4U Start Manual Gyro Calibration** and **Stop** while the controller sits still, with **JSL4U Get Gyro Calibration Status** driving the prompts |
| To spare a returning player a fresh calibration | **JSL4U Get Gyro Calibration Offset** to save it per controller, **JSL4U Set Gyro Calibration Offset** to restore it next session |

The status fields are easy to misread:

| Field | What it means |
|---|---|
| **Auto Calibration Confidence** | Only ever climbs: "this controller was measured well at some point", not a live reading |
| **Auto Calibration Sampling Now** | True only while the automatic calibrator is taking samples. Not "the controller is still": one lying on a desk often reads false once calibration has settled |
| **Manual Calibration Running** | True between Start and Stop |

For a real "is it moving right now", compare the length of the gyro vector from **Get IMU State** against
a small threshold.
