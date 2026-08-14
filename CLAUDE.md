# JoyShockLibrary4Unreal

An Unreal Engine plugin that talks to PlayStation and Nintendo controllers over HID, and reports them to
Unreal as ordinary input devices with motion, rumble, lights and per-player assignment.

This file is for **changing** the plugin. Using it is the README's job, and the README is long and current
— read it before writing code here.

| Question | README section |
|---|---|
| Where does anything live? | *Source layout* — three layers, and the folder tells you which |
| I want to change X, where do I go? | *Where to change what* |
| What will bite me? | *Things worth knowing before changing any of it* |
| What does a game actually call? | *Blueprint nodes*, *Reacting to controllers* |

## Building

The editor must be **closed**. Live Coding holds the module open and blocks the whole build without
reporting a compile error — a build that "does nothing" is almost always an editor still running.

```bash
"<UE install>/Engine/Build/BatchFiles/Build.bat" <Project>Editor Win64 Development -Project="<path>/<Project>.uproject" -WaitMutex
```

**A green build does not prove the includes are right.** UBT compiles this module as one unity blob, so a
`.cpp` missing an `#include` is handed the declaration by whichever neighbour happens to be concatenated
before it, and compiles anyway — until someone reorders files, or builds elsewhere. `-DisableUnity` on the
command line does *not* reach this module. To check properly, add `bUseUnity = false;` next to `PCHUsage`
in `JoyShockLibrary4Unreal.Build.cs`, build, then take it out again. Worth doing after any change that
moves functions between files. (The 2026-08 refactor was green in unity with a missing include in it.)

`Binaries/` and `Intermediate/` are generated; deleting them is always safe and never the fix for a real
compile error. Public headers with `UCLASS`/`USTRUCT`/`UFUNCTION` need their own `.generated.h` include —
UHT regenerates those, but only for headers it can see.

## What counts as verified

Compiling is not working. This plugin's whole job is hardware that an agent cannot reach: no controller
answers in a container, and the failures that matter here — a dropped Bluetooth link, a stick reading
inverted, a rumble packet that never lands — are all invisible to the compiler.

So: **say which of the three you did.** Compiled / compiled and ran / tested on the actual controller. Do
not describe untested code as working, and name the hardware whoever tests it will need — the README's
*Asking for a test* says who owns what, so that request can go to someone who can actually answer it.

**Standalone is the honest test, not PIE.** Play-In-Editor masks at least two classes of bug this codebase
has actually shipped: input focus problems that only appear with more than one player, and re-entrancy
that PIE's timing happens to hide.

There are **no automated tests** in this repository. Any tool reporting "test coverage" here is reporting
zero for everything; do not read that as a finding, and do not promise coverage that does not exist.

## Invariants the compiler cannot check

The README lists four (sole writer of output reports; no blocking HID under `_connectedLock`; identity is
MAC-or-path; `UJoyShockLibrary` and node names cannot be renamed). These are the rest — each one was
learned by breaking it.

**Never re-run a controller's init or configuration exchange on a live Bluetooth stream.** An init
handshake, a feature-report read or a repeated subcommand disturbs the link: the controller goes quiet,
the read timeouts expire, and the poll loop retires a controller that was working perfectly. This has been
rediscovered four separate times, each with a plausible reason (waking a quiet controller, re-asserting
the HOME light, re-initialising an already-connected DS4, a periodic "wakeup"), each costing a working
controller. Only write-only single-report output — rumble, player LEDs — is safe to repeat mid-stream. To
make an output reliable, re-send on an explicit request, never on a timer: the HOME light uses a
generation counter bumped by each `JSL4USetHomeLight` call, so one call means exactly one write. When a
controller is quiet, do nothing — `hid_read` returning 0 means present-but-silent and recovers for free,
while -1 is the genuine disconnect that reconnects cleanly.

**`GetInstance()` must keep answering from the module's own stored pointer, never from `FModuleManager`.**
It looks like a function begging to be simplified back into a one-line `LoadModuleChecked` in the header.
Do not: `FModuleManager::UnloadModule` clears the module's ready flag *before* calling `ShutdownModule`,
and off the game thread `GetModule` then answers null, so `LoadModuleChecked` fails its own check — during
exactly the window in which shutdown is waiting for the polling and enumeration threads to stop. They are
alive, the module is alive, and the only thing that changed is a flag meaning "hand this to no new
callers". The crash reports a three-frame call stack naming nothing but `GetInstance`, on a thread whose
stack explains none of it.

Even so, **prefer handing a thread the reference its caller already holds** — `pollIndividualLoop`
resolves it once at the top and passes it down. It is cheaper, and it does not depend on the above staying
true. Splitting a function out of a polling loop is how the dependency gets reintroduced: the new function
looks like it may simply ask for the module again.

**Never broadcast a game-visible event straight from `IPlatformInputDeviceMapper`'s connection delegate.**
It looks like a neutral engine signal, but the thing raising it is this plugin's own
`Internal_MapInputDeviceToUser` inside `RefreshPlayerAssignments()` — so it fires re-entrantly, inside our
connect handling, with the container lock held and the player slot only half assigned. Queue the change
and drain it from a ticker on a later frame, the way `SendControllerEvents` and
`UJSL4UWaitForAnyControllerChanges::DrainPending` already do. Getting this wrong cost a full debugging
round: spawning failed in standalone only, and looked like a demo bug.

**A right stick's Y is normalised in exactly one place.** Every family flips it somewhere, and two flips
that each look correct in isolation cancel out. A whole controller (Pro, Pro 2, DS4, DualSense) is flipped
in the parser, `InputHelpers.cpp`; a Joy-Con half is flipped **only** in the interface,
`ProcessAnalogInputs` — so the parser must leave a Joy-Con's `stickRY` alone. This has bitten twice: once
a family normalised nowhere, once a Joy-Con 2 normalised twice. Check both places before touching either
sign.

**Connection Id is the only address a game is given.** The `JSL4U*` nodes take an `int64 ConnectionId`;
the library's own `int32` handle is internal and gets reused, so the controller it names today is not
necessarily the one it named a moment ago. Functions ending in `...ForHandle` are the inside of the
plugin, for paths that already hold a handle from the callback that raised them. Do not widen a handle
into the public API.

**No CoreRedirects, ever — function, class, property or pin.** There are none in this repository and that
is deliberate. Every Blueprint-exposed `UFUNCTION`'s C++ symbol must equal its `DisplayName` with spaces
and hyphens removed (`JSL4UGetJoyConPartner` ↔ `"JSL4U Get Joy-Con Partner"`), because UE's Find-in-
Blueprints and a placed node's stored `MemberName` match the declared name, not the displayed one — a
divergent name is a node nobody can find. Keep the `DisplayName` metadata even when it is only the spaced
form, since UE's auto-derived string mangles the `JSL4U` prefix. Renaming a node breaks every already
placed instance in `Content/`, and those can only be repaired in the editor: find them by grepping the
`.uasset` binaries for the old symbol and tell the user which assets to re-point.

## Conventions

- **Code, comments and commit messages are in English**, regardless of the language of the conversation.
- **Comments say why, not what.** The existing ones explain the constraint that made the code look like
  that — which invariant it protects, what broke when it was written differently. Match that; a comment
  restating the line below it is noise here.
- **`JSL4U` prefixes everything a game sees**, and see the naming rule above before adding a node.
- **A new event is an async `WaitFor*` node**, in the shape of GAS's latent actions — not a delegate the
  game has to bind by hand.
- **The polling thread owns output.** A setter stores what the game asked for and returns; the thread
  decides when it reaches the hardware.

<!-- code-review-graph MCP tools -->
## MCP Tools: code-review-graph

**IMPORTANT: This project has a knowledge graph. ALWAYS use the
code-review-graph MCP tools BEFORE using Grep/Glob/Read to explore
the codebase.** The graph is faster, cheaper (fewer tokens), and gives
you structural context (callers, dependents, test coverage) that file
scanning cannot.

### When to use graph tools FIRST

- **Exploring code**: `semantic_search_nodes_tool` or `query_graph_tool` instead of Grep
- **Understanding impact**: `get_impact_radius_tool` instead of manually tracing imports
- **Code review**: `detect_changes_tool` + `get_review_context_tool` instead of reading entire files
- **Finding relationships**: `query_graph_tool` with callers_of/callees_of/imports_of/tests_for
- **Architecture questions**: `get_architecture_overview_tool` + `list_communities_tool`

Fall back to Grep/Glob/Read **only** when the graph doesn't cover what you need.

### Key Tools

| Tool | Use when |
| ------ | ---------- |
| `detect_changes_tool` | Reviewing code changes — gives risk-scored analysis |
| `get_review_context_tool` | Need source snippets for review — token-efficient |
| `get_impact_radius_tool` | Understanding blast radius of a change |
| `get_affected_flows_tool` | Finding which execution paths are impacted |
| `query_graph_tool` | Tracing callers, callees, imports, tests, dependencies |
| `semantic_search_nodes_tool` | Finding functions/classes by name or keyword |
| `get_architecture_overview_tool` | Understanding high-level codebase structure |
| `refactor_tool` | Planning renames, finding dead code |

### Workflow

1. The graph auto-updates on file changes (via hooks).
2. Use `detect_changes_tool` for code review.
3. Use `get_affected_flows_tool` to understand impact.
4. Use `query_graph_tool` pattern="tests_for" to check coverage — but see *What counts as verified*: there
   are no tests, so it always answers none.
