[Português](getting-started.pt-BR.md) · **English**

# Getting started

There are three levels, and most projects only ever need the first:

1. **Nothing** — a game built for gamepads already works.
2. **Controllers coming and going** — for a controller-assignment screen or per-controller UI.
3. **Local multiplayer** — one player per controller.

## Level 1: nothing

Buttons, sticks, triggers, the touchpad, rumble and motion all arrive through Unreal's own input system, as
the same keys an Xbox pad produces. Install the plugin and a project already built for gamepads supports
DualShock 4, DualSense, Joy-Cons and Pro Controllers — existing Input Mapping Contexts,
`Play Force Feedback Effect` and everything else keep working untouched.

The only thing you bind by hand is what has no Xbox equivalent: motion, and the extra buttons listed in
[Input](input.md). Even then you are binding *engine* keys, not plugin keys.

## Level 2: controllers coming and going

Place a **Wait For Controller Changes** node and handle its `On Connected` pin. It fires at once for every
controller already connected, then for each new one. Why this is a single node rather than a binding is in
[Reacting to controllers](controllers.md).

## Level 3: local multiplayer

1. **Turn off *Skip Assigning Gamepad to Player 1*,** in *Project Settings → Maps & Modes → Local
   Multiplayer*. It is on by default, and once a second player exists it moves every gamepad's input to
   the next player. Why it is so hard to spot is in [Troubleshooting](troubleshooting.md#every-controller-drives-the-wrong-player).
2. **Set up the GameMode:** its `DefaultPawnClass`, and one `PlayerStart` in the level per player.
3. **Create the players** from `BeginPlay`:

   `BeginPlay → Wait For Controller Changes → (On Connected) → Ensure Local Player For Controller → Restart Player`

   **Ensure Local Player For Controller** reuses player 0 for the first controller, creates only the Local
   Players that are missing, and returns the Player Controller. **Restart Player** picks a `PlayerStart`,
   spawns the default Pawn and possesses it. A game with its own spawn rules can use **Spawn Actor** +
   **Possess** instead.
4. **Add the Mapping Context** in your PlayerController's `BeginPlay`:

   `Self → Get Enhanced Input Local Player Subsystem → Add Mapping Context`

5. **More than four players:** Unreal stops at four by default. Raise the limit with **JSL4U Set Max Local
   Players** from `BeginPlay`, or with Unreal's own `MaxSplitscreenPlayers` in `Config/DefaultEngine.ini`;
   both change the same value. See [More than four players](players.md#more-than-four-players).

### Worth knowing

- **`bWasCreated` is false for player 0**, because Unreal creates that player before `BeginPlay`. It does
  not mean player 0 already has your Pawn.
- **Ensure Local Player For Controller is safe to call again.** Guard what comes after it: check whether
  the returned Player Controller already possesses a Pawn, or keep a `ConnectionId → Object` map for
  per-controller actors and UI.
- **In a Pawn's Event Possessed, use `New Controller`,** passed straight into **JSL4U Get Controllers
  Assigned To Player**. **Get Player Controller 0** always means player 0, whichever player owns the Pawn.

Testing two players in the editor's Play viewport needs one extra node — see
[Testing in the editor](troubleshooting.md#testing-in-the-editor).
