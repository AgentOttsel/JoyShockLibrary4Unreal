# Demo levels

The plugin ships two demo levels in its Content folder, `LV_JoyShockLocalMultiplayer` and `LV_JoyShockGyroAim`. To see them in the Content Browser,
enable **Show Plugin Content** in the Content Browser's settings:

![Accessing Test Level](../Images/ShowPluginContent.png)

## Local multiplayer level

Connect one or more controllers and press Play. Every controller gets its own Local Player,
PlayerController and Character, and moves it with its left stick — a small local-multiplayer example. It
runs in Standalone and in the editor's Play viewport alike.

![Demo Level Working](../Images/DemoLevel1.png)

![Demo Level Working](../Images/DemoLevel2.png)

The view stays on a fixed overview camera however many controllers are connected: the level's
`CameraActor` has **Auto Activate For Player** set to **Player 0**, so possessing a Character never moves
the view. That is Unreal's normal camera management, not a graph in the PlayerController.

Next to the players, each controller also gets a **mirror**: a 3D model that follows the controller's
measured orientation, so motion and axis conventions can be checked by eye. A HUD shows the motion and
controller readings and has calibration controls.

## How it is built

Each Blueprint does one standard engine job, so the parts are easy to lift into a project:

| Blueprint | Its job |
|---|---|
| `BP_JoyShockInitializer` | The only one that reacts to controllers. On `BeginPlay` it places one **Wait For Controller Changes** and one **Wait For Joy-Con Pairing Changes**, both feeding the same function: **Ensure Local Player For Controller**, add the Mapping Context, and spawn and possess a Character only if that player has none. Reconnections and Joy-Con separations all go through that one path, so they never produce duplicate Pawns |
| `BP_JoyShockGameMode` | Only `DefaultPawnClass` and `PlayerControllerClass`. The level has separate `PlayerStart` actors |
| `BP_JoyShockPlayerController` | Empty on purpose: routing, possession and Enhanced Input need nothing in it |
| `BP_JoyShockCharacter` | Gameplay input, plus **Set Focus to Game Viewport** one tick after possession, which the Play viewport needs for a second player (see [Testing in the editor](troubleshooting.md#testing-in-the-editor)) |
| `BP_JoyShockControllerMirror` | The 3D mirror of one controller |
| `WBP_JoyShockHUD` and its widgets | The motion readings and calibration controls |

## The mirror and the HUD

They read the direct `JSL4U*` nodes, because motion readings have no Enhanced Input equivalent. They are
diagnostics, not an input path for buttons, sticks or gameplay, and they find no controllers and create no
players — the Initializer does that. A project can delete both without changing how players are created.

- **One mirror per physical connection.** The Initializer spawns it with the controller's full
  `FJSL4UControllerInfo` in its Expose-on-Spawn `Controller` property, and keeps a
  `ConnectionId → mirror` map; a disconnect destroys only that connection's mirror. It is keyed by
  `ConnectionId`, not player index, because the two halves of a joined pair share a player but are still
  two motion sensors — two mirrors. No mirror is placed in the level by hand.
- **The HUD is rebuilt only when the set of controllers changes**, and then receives the current
  `ConnectionId → mirror` map. Its readings keep updating between those changes. It uses the capability
  flags on the controller info (`Has RGB Light`, `Has Player Indicator`, `Has Motion Sensors`,
  `Has Touchpad`, `Has Rumble`) to decide which controls to enable, instead of switching on controller
  type.

## Gyro aim level

`LV_JoyShockGyroAim` is a second, single-player level: a first-person camera turned by the controller's
gyro, facing a wall with a grid and five targets, with a crosshair in the middle and the controller's
reports per second in the corner. Open it and press Play, then click the controller to aim with among the
buttons on the right, one per connected controller; the first one connected is picked to begin with. It is the place to judge whether gyro
aiming is smooth — turn slowly across the grid, then flick between the targets and press A on each to see where the crosshair really was.

The gyro path uses no plugin node at all, so it carries over to any project as is:

| Blueprint | Its job |
|---|---|
| `BP_JoyShockGyroAimGameMode` | Only `DefaultPawnClass` and `PlayerControllerClass` |
| `BP_JoyShockGyroAimPawn` | Adds `JoyShockInputMappingContext` on possession. The `JoyShock-GyroscopeRotation` action, mapped to **Rotation Rate**, gives degrees per second for this frame; times the frame's seconds and **Sensitivity**, that is this frame's turn, added to the Control Rotation (yaw from Z, pitch from Y, pitch clamped to ±89°). **Invert Pitch** flips the vertical axis. Both sticks turn the camera too, at **Stick Speed** degrees per second, to bring the view back after the gyro has wandered — both, because a Joy-Con held sideways on its own presents its stick as the left one. The shared mapping's **To World Space** swaps a stick's axes, so its Y is yaw and its X, negated, is pitch. The right face button (A on a Nintendo controller, Circle on a PlayStation one) traces from the camera and leaves a mark where it hits. It also shows the mouse cursor, for the overlay's buttons |
| `BP_JoyShockGyroAimMark` | A small red sphere (`M_JoyShockGyroAimMark`, red in base and emissive colour so it shows in shadow too) with no collision and a three-second **Initial Life Span**, so a mark removes itself |
| `WBP_JoyShockGyroAimOverlay` | The crosshair, a column of buttons on the right, and **JSL4U Get Reports Per Second** for the controller picked. **Wait For Controller Changes** adds a button when a controller connects and removes it when it leaves; clicking one puts that controller on player 0 with **JSL4U Assign Controller To Player Index** and every other controller on player 1, which has no pawn, so only the picked one turns the camera |
| `WBP_JoyShockGyroAimControllerButton` | One controller's button, in the controller's own colour (the controller info's **Color**): full while it is the one aiming, dimmed otherwise |

The turn is applied straight to the Control Rotation rather than through **Add Controller Yaw Input**,
whose result depends on the project's legacy input scale settings.
