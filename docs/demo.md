# Demo levels

The plugin ships two demo levels in its Content folder, `LV_JoyShockLocalMultiplayer` and `LV_JoyShockGyroAim`. To see them in the Content Browser,
enable **Show Plugin Content** in the Content Browser's settings:

![Accessing Test Level](../Images/ShowPluginContent.png)

## Local multiplayer level

Connect one or more controllers and press Play. Every controller gets its own Local Player,
PlayerController and Character, and moves it with its left stick — a small local-multiplayer example. It
runs in Standalone and in the editor's Play viewport alike.

Several controllers can drive one Character too. Each controller's panel has **Share player 1**, which
puts that controller on player 1's slot with **JSL4U Assign Controller To Player Index**, and **Own
player**, which hands it back to automatic assignment (-1) and so, normally, to the slot it left. A slot
shared this way is the plugin's own feature, the one a joined Joy-Con pair uses: every controller on it
feeds the same player through Enhanced Input, and their player lights show the same number. The
controller's old Character stays where it was, idle, until the controller comes back. The names above each Character list the controllers now feeding its player; an assignment change raises no controller event, so the two buttons call **Refresh From Player** on every `WBP_ControllerName` right after moving the controller.

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
| `BP_JoyShockPlayerController` | Only a rumble test, shared by both levels: **L** plays a short, light pulse (intensity 0.2) and **R** a short, strong one (1.0), each for 0.3 s, through Unreal's **Play Dynamic Force Feedback**. Routing, possession and Enhanced Input need nothing else in it |
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
| `BP_JoyShockGyroAimGameMode` | `PlayerControllerClass`, and no `DefaultPawnClass`: on **OnPostLogin** it spawns the level's one pawn for player 0 only, so the players created when a controller is picked get none of their own |
| `BP_JoyShockGyroAimPawn` | Adds `JoyShockInputMappingContext` to whichever player possesses it, points player 0's view at itself, creates the overlay the first time, and after a later possession gives the Play viewport focus one tick on (the second player needs it in PIE). The `JoyShock-GyroscopeRotation` action, mapped to **Rotation Rate**, gives degrees per second for this frame; times the frame's seconds and **Sensitivity**, that is this frame's turn, added to the Control Rotation (yaw from Z, pitch from Y, pitch clamped to ±89°). **Invert Pitch** flips the vertical axis. Both sticks turn the camera too, at **Stick Speed** degrees per second, to bring the view back after the gyro has wandered — both, because a Joy-Con held sideways on its own presents its stick as the left one. The shared mapping's **To World Space** swaps a stick's axes, so its Y is yaw and its X is pitch. Unreal's viewport negates **Gamepad Right Y** for every gamepad, so the two sticks reach the pawn with opposite vertical signs: the left one is negated before both share the same turn, and either stick pushed up looks up. The right face button (A on a Nintendo controller, Circle on a PlayStation one) traces from the camera and leaves a mark where it hits. It also shows the mouse cursor, for the overlay's buttons |
| `BP_JoyShockGyroAimMark` | A small red sphere (`M_JoyShockGyroAimMark`, red in base and emissive colour so it shows in shadow too) with no collision and a three-second **Initial Life Span**, so a mark removes itself |
| `WBP_JoyShockGyroAimOverlay` | The crosshair, a column of buttons on the right, and **JSL4U Get Reports Per Second** for the controller picked. Under it, a stick check: pushed fully up, then fully right, every number should read +1.00 except the right stick's up/down in the first pair, which reads -1.00 because Unreal inverts that key for every gamepad. Its first pair of lines, one per stick, is what Unreal receives (the aiming player's **Get Input Analog Key State** on Gamepad Left/Right X/Y, before any Enhanced Input modifier); the second pair is what **JSL4U Get Left/Right Stick** returns for the controller picked, which should agree with it. The buttons are rebuilt from **JSL4U Get Connected Controllers** whenever **Wait For Controller Changes** or **Wait For Joy-Con Pairing Changes** fires, so a joined pair shows as one button, *Joy-Con pair #3 + #4*, in the colour of the half with the lower connection id; clicking one hands the pawn to that controller's own player: **Ensure Local Player For Controller** returns its Player Controller, creating the player if needed, and that controller **Possess**es the pawn. No controller changes player, so every player light stays as it was, and a joined pair aims together because it is one player. Split screen is forced off, so player 0's view is the only one drawn; the stick check reads the aiming player. Picking a half that has just been joined moves the selection to the pair's button |
| `WBP_JoyShockGyroAimControllerButton` | One controller's button, in the controller's own colour (the controller info's **Color**): full while it is the one aiming, dimmed otherwise |

The turn is applied straight to the Control Rotation rather than through **Add Controller Yaw Input**,
whose result depends on the project's legacy input scale settings.
