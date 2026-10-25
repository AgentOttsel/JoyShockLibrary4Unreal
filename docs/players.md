# Players and local multiplayer

Player slots are **stable**: a controller keeps its slot for as long as it stays connected, and a disconnect leaves that slot as a hole rather than shifting the others down — so if the player 1 controller drops mid-match, players 2 and 3 stay on their own characters instead of shuffling. The hole is reused by the next controller to connect. 4 solo-horizontal Joy-Cons = 4 players; two joined vertical pairs = 2 players. Joins dissolve automatically if one of the Joy-Cons disconnects.

Slots already taken by an **XInput pad** are skipped, so these controllers land where a second Xbox pad would have. Plug in an Xbox controller and a DualShock 4 and you get player 1 and player 2, the same as plugging in two Xbox controllers — a game does not need one assignment scheme for XInput and another for this plugin. (The flip side of matching XInput is that you inherit its behaviour: in a single-player game the second controller drives a player that does not exist, exactly as a second Xbox pad would.)

Slots otherwise follow the order controllers were switched on, and a controller that connected second stays on slot 1 even once it is the only one left — which in a single-player game means its input goes to a player that does not exist. Use **JSL4U Assign Controller To Player Index** to decide this yourself rather than inheriting connection order; it is the only thing that overrides it.

**Slots may be shared, and that is how any set of controllers drives one player.** Assign two, three or four controllers to the same slot and they all drive that player — a joined Joy-Con pair is simply the case the plugin performs for you, complete with the vertical stick/button split. Nothing is limited to Joy-Cons: a DualSense and a Pro Controller on one slot both move the same Pawn.

## More than four players

Unreal caps the number of local players at **4** by default, and a fifth controller silently gets no
player: it connects, it is reported here, it is given player slot 4, and `Create Player` refuses it. The
number lives on the game viewport under the name `MaxSplitscreenPlayers`, which is a misnomer worth
knowing about — it caps `Create Player` whether or not the screen is ever split, and it is still enforced
with **Set Force Disable Splitscreen** on. Splitscreen stops at four because four is as many views as fit
on a television; that is a rendering limit and says nothing about how many people are playing.

Call **JSL4U Set Max Local Players** from `BeginPlay`, before creating any player, to raise it. Doing it
there keeps a decision that belongs to your game in your game, rather than in a `DefaultEngine.ini` entry
under the wrong name where nobody reading your player-spawning code will find it. **JSL4U Get Max Local
Players** reads it back.

Assignment nodes are under **JoyShock Library | Controller Assignment**:

- **JSL4U Set Max Local Players / JSL4U Get Max Local Players** — the ceiling on `Create Player`, above.
- Reading the slot back has no node of its own: every controller info already carries **Player Index**, so breaking the struct any node hands you — or **JSL4U Get Controller Info** — answers it, for XInput pads as readily as for ours.
- **JSL4U Assign Controller To Player Index (Controller, Player Index)** — puts a controller on a chosen player slot. Pass -1 to hand it back to automatic assignment. Works for XInput pads too.
- **JSL4U Assign Controller To Player (Controller, Player Controller)** — the same thing addressed by **PlayerController** instead of slot number.
- **JSL4U Get Controllers Assigned To Player** — every controller feeding a player, XInput pads included, taking the generic `Controller` type so a Pawn's `Possessed` event plugs straight in. Two entries for a joined pair.
- **Ensure Local Player For Controller** (on the JoyShock Subsystem) — the high-level local-multiplayer path: reuse or create the correct Local Player from the controller's native Platform User, return its PlayerController, and reconcile the engine device mapper.

## Combining Joy-Cons into one player

A left+right Joy-Con pair can act as a single vertical controller for one player. Grip changes follow the
Switch convention globally:

- A Joy-Con connected by itself starts as a solo-horizontal controller.
- Press **SL + SR** on either half of a joined pair to separate both halves; each becomes horizontal.
- On two separated halves, hold **L or ZL** on the left and **R or ZR** on the right at the same time to
  join them as one vertical controller. For example, **ZL + ZR** is sufficient. The outer buttons are
  registration input and are suppressed until released, so the join does not also fire gameplay actions.
- When several left/right halves perform the chord in the same frame, device-id order pairs them
  deterministically.

Pairing nodes are under **JoyShock Library | Joy-Con Pairing**:

![Controllers Events](../Images/Joy-ConPairing.png)

- **JSL4U Get Joy-Con Pair** — given either half, returns whether it is a pair plus both halves' identities. `Primary` names the same half no matter which one was asked, which is what lets an actor act  on a pair   without first working out which half it is attached to.
- **JSL4U Get Joy-Con Partner** — whether this half is joined right now, and to which Connection Id.
- **JSL4U Is Controller Type Joinable** — whether a controller type can be joined into a pair (currently the left and right Joy-Cons). `JSL4U Join Joy-Cons` validates with this same function.
- **JSL4U Is Joy-Con Primary** — whether this device is the one representing its logical controller. True
  for any standalone controller of any type, and for exactly one half of a joined pair.
- **JSL4U Join Joy-Cons (A, B)** — joins a left and a right Joy-Con so they feed a single player and sets both vertical (left half = left stick and its buttons, right half = right stick and its buttons).
- **JSL4U Set Joy-Con Grip Mode** — explicit per-device override. Standalone defaults to Horizontal; set
  Vertical for exceptional games that intentionally use one upright half, such as Just Dance. Setting
  Horizontal on a joined half separates the pair.
 - **JSL4U Unjoin Joy-Con / JSL4U Unjoin All Joy-Cons** — dissolve joins and return each half to horizontal presentation.



`Primary` identifies a *device*, not a *side*: which half leads depends on connection order, so a pair's
primary may be the right Joy-Con. Read `Controller Type` on the returned infos when something has to land
on a specific physical half.
