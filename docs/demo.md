# Demo level

There's a demo level called LV_JoyShockDemo in the Content folder! You can find it in your Content Browser, as long as you enable showing Plugin Content in your Content Browser settings:
![Accessing Test Level](../Images/ShowPluginContent.png)

Connect one or more controllers and press Play. The level is a small local-multiplayer example: every detected controller gets a native Unreal `LocalPlayer`, `PlayerController` and possessed Character. Move each Character with its controller's left stick. Both Standalone and the PIE viewport are supported.

The level's existing `CameraActor` has **Auto Activate For Player** set to **Player 0**. Consequently the
demo keeps its fixed overview camera whether zero or several controllers are connected; possessing a
Character does not make the view jump to that Pawn. This uses Unreal's normal automatic camera management
instead of a `Set View Target with Blend` graph in the PlayerController.

The example deliberately uses standard engine responsibilities:

- `BP_JoyShockInitializer` is a lightweight Actor. On `BeginPlay` it places one **Wait For Controller Changes** node and one **Wait For Joy-Con Pairing Changes** node. Both feed the same idempotent function, which calls **Ensure Local Player For Controller**, adds the demo Mapping Context, and spawns and possesses a Character only when that player does not already have one. Replayed connections, reconnections and Joy-Con separations therefore all converge on the same code path without ever producing duplicate Pawns.
- `BP_JoyShockGameMode` owns only the `DefaultPawnClass` and `PlayerControllerClass`; its old manual player-management graph was removed. The level contains separate `PlayerStart` actors.
- `BP_JoyShockPlayerController` is intentionally empty. It demonstrates that controller routing, possession and Enhanced Input do not require a second input implementation in the controller Blueprint.
- `BP_JoyShockCharacter` contains only gameplay input and a one-tick-delayed **Set Focus to Game Viewport** after possession, which also makes newly created Slate users work in the PIE viewport.

![Demo Level Working](../Images/DemoLevel1.png)

![Demo Level Working](../Images/DemoLevel2.png)

The sensor diagnostics remain available as a companion to the multiplayer example. `BP_JoyShockControllerMirror` is a digital representation of the physical controller: it follows the controller's measured orientation so motion and coordinate conventions can be checked visually. `WBP_JoyShockHUD` and its supporting widgets display numeric motion/controller state and expose calibration controls. They consume the direct-device `JSL4U*` API because those measurements have no Xbox/Enhanced Input equivalent; they are not a replacement input path for buttons, sticks or gameplay.

The Mirror and HUD are deliberately not responsible for controller discovery, Local Player creation, possession or Mapping Context setup. The Initializer owns that lifecycle, so a project can remove the visual diagnostics without changing gameplay routing.

The demo automatically spawns one Mirror per physical connection and passes the complete
`FJSL4UControllerInfo` into its Expose-on-Spawn `Controller` property. The Initializer keeps a
`ConnectionId → BP_JoyShockControllerMirror` map; disconnecting destroys only that connection's actor.
`ConnectionId` rather than `PlayerIndex` is intentional: joined Joy-Con halves share a PlayerIndex but
remain two physical motion sensors and therefore two Mirrors. No Mirror is placed manually in the level.

The HUD is rebuilt only when the connected set changes, then receives the current ConnectionId-to-Mirror map;
its sensor values continue updating normally between those rare changes. The Mirror never enumerates
controllers or creates Local Players. `FJSL4UControllerInfo` also
exposes capability flags (`Has RGB Light`, `Has Player Indicator`, `Has Motion Sensors`, `Has Touchpad`,
`Has Rumble`) so diagnostic widgets can enable their controls without hard-coding controller-type switches.
