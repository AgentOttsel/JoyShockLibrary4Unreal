# Getting started

Three levels, and most projects only ever need the first.

**1. Nothing.** Buttons, sticks, triggers, the touchpad, rumble and motion all arrive through Unreal's own input system, as the same keys an Xbox pad produces. Install the plugin and a project already built for gamepads supports DualShock 4, DualSense, Joy-Cons and Pro Controllers — existing Input Mapping Contexts, `Play Force Feedback Effect` and everything else keep working untouched. The only thing you bind by hand is what has no Xbox equivalent (motion, and the extra buttons listed in [Input](input.md)), and even then you are binding *engine* keys, not plugin keys.

**2. Reacting to controllers coming and going.** Only needed for a controller-assignment screen, per-controller UI, or local multiplayer. Place a **Wait For Controller Changes** node and handle its `On Connected` pin; see [Reacting to controllers](controllers.md) for why this is a single node rather than a binding.

**3. Local multiplayer.** For each `Controller` delivered by `On Connected`, call **Ensure Local Player For Controller** with `bCreateIfMissing` enabled. It reuses player 0 for the first controller, creates only the missing local players through Unreal's native `FPlatformUserId` API, repairs any mapper reassignment caused by player creation, and returns the correctly typed **Player Controller** plus its local-player index.

A typical Blueprint flow is:

`BeginPlay → Wait For Controller Changes → (On Connected) → Ensure Local Player For Controller → Restart Player`

Set the GameMode's `DefaultPawnClass` and place enough `PlayerStart` actors for the maximum number of local players. **Restart Player** then chooses a start, spawns the default Pawn and possesses it through Unreal's normal player lifecycle. Manual **Spawn Actor** + **Possess** is still valid when a game needs custom spawn rules, but it is not required for the common case.

Unreal limits a `UGameViewportClient` to four Local Players by default, even when splitscreen rendering is disabled. A game that needs more must raise the standard engine setting in its project `Config/DefaultEngine.ini` and provide the same number of `PlayerStart` actors:

```ini
[/Script/Engine.GameViewportClient]
MaxSplitscreenPlayers=6
```

The demo project used for the six-controller test is configured this way; the plugin does not change a project's player limit automatically.

Mapping contexts belong to the Local Player. The cleanest reusable place to add one is the custom PlayerController's `BeginPlay`:

`Self → Get Enhanced Input Local Player Subsystem → Add Mapping Context`

If you instead do this from a Pawn's **Event Possessed**, pass `New Controller` straight into
**JSL4U Get Controllers Assigned To Player**, which accepts the generic `Controller` type. Do not use
**Get Player Controller 0** there: that always addresses player 0, including when the Pawn belongs to
another Local Player.

`Ensure Local Player For Controller` defaults `bCreateIfMissing` to true and is safe to call again. Guard any downstream game logic separately: for a standard GameMode, checking whether the returned PlayerController already possesses the expected Pawn is enough; for per-connection actors or UI, keep a `ConnectionId → Object` map. Also note that `bWasCreated` is **false for player 0**, because Unreal created that Local Player before `BeginPlay`; it does not mean that player 0 already has your gameplay Pawn.

> **Turn off "Skip Assigning Gamepad to Player 1" before you do.** It lives in *Project Settings → Maps & Modes → Local Multiplayer* and is **on by default**. While it is on, and only once a second local player exists, the engine shifts every gamepad's input to the *next* player — so the first controller starts driving player 2's pawn and the last controller drives a player that does not exist, silently. It is there for games where the keyboard is player 1 and gamepads start at player 2; it works against you otherwise.
>
> This one is worth knowing about because it is almost impossible to diagnose from the symptom. Everything you can inspect looks right — the plugin reports the correct player index, the platform input device mapper agrees with it, and each player controller possesses the pawn you expect. The redirect happens inside `UGameViewportClient::RemapControllerInput`, after all of that, so nothing upstream shows a discrepancy. The giveaway is that a single player works perfectly and the assignment only scrambles the moment the second player is created.

### Local multiplayer in the PIE viewport

PIE keeps Slate focus separately for every local user. After creating a second player, that user's focus can remain on the editor `SWindow` instead of the PIE `SViewport`; Slate then discards that user's input before it ever reaches the game viewport. Standalone and packaged games do not normally have this editor-window artifact.

For a playable PIE viewport, call Unreal's standard **Set Focus to Game Viewport** node after the local players, possession, and mapping contexts have been set up. That node focuses the game viewport for all Slate users. Call it again when closing an in-game menu if that menu intentionally moved focus. The plugin does not force focus automatically, because doing so would break legitimate UI input modes.

While testing in the editor, you probably also want this in the console:

```
Slate.EnableGamepadEditorNavigation false
```

It is **on** by default, and it lets a gamepad move the editor's own focus around — so a controller you are testing with is also tabbing through panels behind the viewport, and a stick flick can land somewhere you did not intend. With two controllers connected it is worse, because both are doing it. The setting is the editor's, not this plugin's, and it does not affect a packaged game.
