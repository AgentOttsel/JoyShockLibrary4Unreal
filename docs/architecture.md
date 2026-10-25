# Source layout

Nothing here is needed to *use* the plugin — this is for reading or changing it.

The source is in three layers, and the folder a file sits in tells you which one it belongs to. The rule
they follow is that dependencies point one way only: the hardware layer never knows about Unreal's input
system, and the Unreal layer never talks to a controller directly.

**`ThirdParty/hidapi/`** — the HID library, unmodified. Not ours.

**`Source/JoyShockLibrary4Unreal/JoyShockLibrary/`** — the hardware layer. Opens controllers, speaks each
family's protocol, and runs one polling thread per device. Descends from JibbSmart's original library,
though little of it is unchanged now.

| File | |
|---|---|
| `JoyShock.h` / `JoyShock.cpp` | One connected controller: construction, reading a report off whichever transport it is on, and the motion pipeline. Nothing family-specific |
| `JoyShock_Nintendo.cpp` | Switch 1 protocol — subcommands, SPI stick calibration, rumble, player and HOME lights |
| `JoyShock_Switch2.cpp` | Switch 2 protocol — the WinUSB command interface, the BLE GATT transport, its own rumble frames |
| `JoyShock_Sony.cpp` | DualShock 4 and DualSense — output reports, light bar, the Bluetooth CRC-32 |
| `Switch2Bluetooth.*` | The BLE scan and connection the Switch 2 needs, since Windows will not pair it as a HID device |
| `JoyShockPolling.cpp` | One controller's polling thread, and the output reports it is the sole writer of |
| `JoyShockLibrary.cpp` | The device registry: the maps of live devices, device-id allocation, and shutdown |
| `JoyShockInternal.h` | What those files share — the registry globals and the handle helpers |
| `InputHelpers.h` / `InputHelpers.cpp` | Decoding an input report into controller state, with one parser per family |
| `GamepadMotion.hpp` | JibbSmart's motion and calibration maths, used as-is |
| `tools.*` | Byte-fiddling helpers inherited from the original library |
| `JoyShockLibrary.h` | Now just the library's own entry points; kept so old includes still resolve |

**`Source/JoyShockLibrary4Unreal/Public/`** — what a game can see.

| File | |
|---|---|
| `JoyShockTypes.h` | The data: controller states, the enums a Blueprint branches on, button masks. The header everything else includes |
| `JoyShockBlueprintLibrary.h` | The `JSL4U*` nodes — the plugin's public face |
| `JoyShockInterface.h` | `FJoyShockInterface`, the `IInputDevice` Unreal polls each frame |
| `JoyShockSubsystem.h` | The game-instance subsystem carrying the connect/disconnect/pairing delegates |
| `JoyShockAsyncActions.h` | The latent **Wait For…** nodes |
| `JoyShockLibrary4Unreal.h` | The module itself |

**`Source/JoyShockLibrary4Unreal/Private/`** — how it is done.

| File | |
|---|---|
| `JoyShockBlueprintLibrary.cpp` | The nodes, implemented — mostly locked reads of state the polling thread produced |
| `JoyShockEnumeration.cpp` | Finding controllers and opening them, in three phases that exist to keep blocking HID I/O away from the lock the game thread takes |
| `JoyShockInterface.cpp` | Hardware state into Unreal input events |
| `JoyShockPlayerAssignment.cpp` | Which controller belongs to which player: slot allocation, joining and splitting Joy-Cons, grip transitions |
| `JoyShockInterfaceInternal.h` | The few predicates those two files share |
| `JoyShockSubsystem.cpp`, `JoyShockAsyncActions.cpp`, `JoyShockLibrary4Unreal.cpp` | The subsystem, the latent nodes and module startup |

## Where to change what

| If you want to… | Go to |
|---|---|
| Support a new controller model | `JoyShockEnumeration.cpp` to recognise it, a `JoyShock_*.cpp` for its protocol, `InputHelpers.cpp` for its report layout |
| Fix a misread button, stick or trigger | `InputHelpers.cpp` — the per-family parser |
| Change what a Blueprint node does | `Public/JoyShockBlueprintLibrary.h` for the signature, `Private/JoyShockBlueprintLibrary.cpp` for the body |
| Add a field to a controller's state | `Public/JoyShockTypes.h`, then whichever parser fills it |
| Change rumble or light behaviour | `JoyShockPolling.cpp` — the polling thread is the only writer of output reports |
| Change how players get their controllers | `JoyShockPlayerAssignment.cpp` |
| Fix a controller that connects twice, or not at all | `JoyShockEnumeration.cpp` |

## Things worth knowing before changing any of it

- **The polling thread is the sole writer of output reports.** The `JSL4USet*` nodes only store what the
  game asked for; the thread sends it. Writing from the game thread means a blocking HID write while
  holding the lock that same thread needs to parse every input packet.
- **`_connectedLock` is never held during blocking HID I/O.** hidapi's Windows write has no timeout, so a
  controller that never answers would hang the editor until the cable is pulled. This is why enumeration
  is split into phases.
- **A controller's identity is its MAC where one can be read, and its HID path otherwise.** That is what
  lets a controller keep its player slot across a reconnect, and what stops one controller reachable two
  ways from becoming two.
- **`UJoyShockLibrary` and the node names cannot be renamed.** Saved Blueprint graphs refer to them.
