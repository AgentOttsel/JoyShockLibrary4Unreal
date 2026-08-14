// JoyShockBlueprintLibrary.h - Everything a game can ask this plugin to do, as Blueprint nodes.
//
// This is the plugin's public face: the ~50 nodes that list controllers, read their sticks and motion,
// pair Joy-Cons, assign players, and drive rumble and lights. It lives in Public/ with the rest of the
// Unreal layer rather than in the JoyShockLibrary/ folder, which holds the hardware code this calls into.
//
// The class is still named UJoyShockLibrary, because that name is what every existing Blueprint node and
// C++ call site refers to; renaming the type would break saved Blueprint graphs for no gain.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "JoyShockTypes.h"

#include "JoyShockBlueprintLibrary.generated.h"

class AController;
class APlayerController;

UCLASS()
class JOYSHOCKLIBRARY4UNREAL_API UJoyShockLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// Returns every currently connected controller, in connection order. Use this to list controllers
	// (e.g. "JoyCon (R), DualSense, JoyCon (L)") and pick which Joy-Cons to join. Use
	// "Enum to String" on ControllerType for a readable name.
	//
	// "Connected" means the controller has an engine identity, not merely that the library has it open: one
	// whose connect has not reached the game thread yet is left out for those few frames rather than listed
	// with the Connection Id and Input Device Id it does not have yet. It arrives on Wait For Controller
	// Changes the moment it does, and the two never disagree about what it is.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Controllers",
		meta = (DisplayName = "JSL4U Get Connected Controllers", ToolTip = "Returns every connected JoyShock controller with its plugin and Unreal device identities, model, assignment and current settings."))
	static TArray<FJSL4UControllerInfo> JSL4UGetConnectedControllers();

	/**
	 * Returns every controller Unreal accepts, ours and everyone else's, as one list.
	 *
	 * The polling counterpart to Wait For Any Controller Changes, and it exists because that node used to
	 * be the only way to see a controller this plugin does not drive. A game that wanted the current roster
	 * at any other moment -- opening a pause menu, rebuilding a player-select screen -- got a list with the
	 * Xbox pads missing and had to keep its own tally from the event to fill the gap. This is that tally,
	 * asked for directly.
	 *
	 * Ours come first, in connection order, so the JSL4U block of the list matches Get Connected
	 * Controllers exactly; the rest follow in the order Unreal reports them. The keyboard and mouse are
	 * left out, for the same reason the event node leaves them out: they are connected from the first frame
	 * and are not a player joining.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Controllers",
		meta = (DisplayName = "JSL4U Get All Connected Controllers", Keywords = "xinput xbox any every gamepad roster list joyshock", ToolTip = "Returns every controller Unreal accepts -- this plugin's and the XInput pads it does not drive -- in one list. Every node here takes a Connection Id and accepts either kind; check the capability flags to know what a given controller can actually answer."))
	static TArray<FJSL4UControllerInfo> JSL4UGetAllConnectedControllers();

	/**
	 * Describes a controller this plugin does not drive, from what Unreal knows about any input device.
	 *
	 * C++ only: FInputDeviceId and FPlatformUserId are not Blueprint types, which is the whole reason the
	 * identities in FJSL4UControllerInfo are integers. Shared so the polling query and the event node
	 * cannot drift into describing the same pad two different ways.
	 */
	static FJSL4UControllerInfo JSL4UDescribeUndrivenDevice(FPlatformUserId PlatformUser,
		FInputDeviceId InputDevice);

	// True for controller types that can be joined into a pair -- currently the left and right Joy-Cons.
	// This is the single source of truth for "can this be joined": JSL4UJoinJoyCons validates with it too.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Joy-Con Pairing",
		meta = (DisplayName = "JSL4U Is Controller Type Joinable", ToolTip = "Returns true when this controller type can be paired with another controller. Currently this means left and right Joy-Cons."))
	static bool JSL4UIsControllerTypeJoinable(EJSL4UControllerType ControllerType);

	// Joins two Joy-Cons so they act as a single controller for one player: their inputs are merged and
	// delivered to the player of whichever half connected first. Both must be Joy-Cons (one left, one
	// right). Returns true on success.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Joy-Con Pairing",
		meta = (DisplayName = "JSL4U Join Joy-Cons", ToolTip = "Pairs one left and one right Joy-Con so both feed the same player. Returns false when either device is unavailable or the types are incompatible."))
	static bool JSL4UJoinJoyCons(int64 ConnectionIdA, int64 ConnectionIdB);

	// Undoes a join involving this controller (both halves go back to being their own players).
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Joy-Con Pairing",
		meta = (DisplayName = "JSL4U Unjoin Joy-Con", ToolTip = "Dissolves the Joy-Con pair containing this device. Both halves return to independent assignment."))
	static void JSL4UUnjoinJoyCon(int64 ConnectionId);

	// Undoes every Joy-Con join.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Joy-Con Pairing",
		meta = (DisplayName = "JSL4U Unjoin All Joy-Cons", ToolTip = "Dissolves every joined Joy-Con pair."))
	static void JSL4UUnjoinAllJoyCons();

	// Overrides one Joy-Con's grip presentation. Horizontal rotates its stick/buttons into standard
	// one-gamepad positions and separates it from a joined pair. Vertical is intended for exceptional
	// single-Joy-Con games such as Just Dance.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Joy-Con Pairing",
		meta = (DisplayName = "JSL4U Set Joy-Con Grip Mode", ToolTip = "Sets one Joy-Con to Horizontal or Vertical presentation. Horizontal separates a joined pair. Standalone Joy-Cons use Horizontal by default."))
	static bool JSL4USetJoyConGripMode(int64 ConnectionId, EJSL4UJoyConGripMode GripMode);

	/**
	 * Whether this Joy-Con is currently joined with another, and to which one -- read live rather than
	 * from a struct.
	 *
	 * Prefer this over a stored Controller Info's JoinedToConnectionId whenever the answer is needed at an
	 * arbitrary moment (drawing a mirror, refreshing UI). A Controller Info is a snapshot: one captured
	 * when a controller connected still says "not joined" forever, because joining happens later, and
	 * nothing rewrites the copy the caller kept. This asks the input interface for the current pairing.
	 */
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Joy-Con Pairing",
		meta = (DisplayName = "JSL4U Get Joy-Con Partner", Keywords = "join joined pair partner",
			ToolTip = "Returns whether this Joy-Con is joined right now, and its partner's Connection Id (0 when standalone). Reads live pairing state, unlike a stored Controller Info."))
	static bool JSL4UGetJoyConPartner(int64 ConnectionId, int64& PartnerConnectionId);

	/**
	 * Whether this device is the one that represents its logical controller: true for any standalone
	 * controller, and for exactly one half of a joined pair.
	 *
	 * This is the plugin's own grouping rule (the same one player slots are assigned from), so it answers
	 * "should this actor draw the pair, or stand down?" without the caller inventing a tie-break that
	 * could drift from the plugin's. Note it identifies a device, not a side: which half wins depends on
	 * connection order, so a pair's primary may be the right Joy-Con. When the answer must always be the
	 * same physical half (drawing a grip that holds both), branch on Controller Type instead.
	 */
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Joy-Con Pairing",
		meta = (DisplayName = "JSL4U Is Joy-Con Primary", Keywords = "join joined pair primary leader",
			ToolTip = "True when this device represents its logical controller: always true for a standalone controller of any type, and true for exactly one half of a joined Joy-Con pair. Use it to decide which of two paired actors is the active one."))
	static bool JSL4UIsJoyConPrimary(int64 ConnectionId);

	/**
	 * Resolves a whole logical controller from either of its halves, in a single call: whether it is
	 * a joined pair, which half leads it, and both halves' full identities.
	 *
	 * Ask it with either half and the answer is the same -- Primary is always the device the plugin uses
	 * to represent the pair (the same one Is Join Primary reports and player slots are assigned from), and
	 * Partner is always the other. That stability is the point: an actor drawing a pair does not have to
	 * work out which of the two it happens to be attached to.
	 *
	 * Standalone controllers are not a special case to handle separately: Primary is that controller,
	 * Partner comes back unset (its Is Connected is false), and the return value is false. So this is also
	 * a safe first call at start-up, where controllers are already connected and no pairing event will
	 * ever arrive to announce them.
	 *
	 * Primary is a device, not a side: which half leads depends on connection order, so a pair's Primary
	 * may be the right Joy-Con. When something has to go on a specific half -- painting the left Joy-Con's
	 * colour on the left half of a mesh -- read Controller Type on each returned info rather than assuming.
	 */
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Joy-Con Pairing",
		meta = (DisplayName = "JSL4U Get Joy-Con Pair", ToolTip = "Given either half, returns whether it is a joined pair plus both controller infos (Primary leads, Partner is the other half). A standalone controller returns itself as Primary, an unset Partner and false. Reads live state, so it works for controllers that were already connected at start-up."))
	static bool JSL4UGetJoyConPair(int64 ConnectionId, FJSL4UControllerInfo& PrimaryController,
		FJSL4UControllerInfo& PartnerController);

	/**
	 * How many local players this game may create -- i.e. the ceiling on Create Player, and so on how many
	 * characters a game can spawn for the controllers this plugin reports.
	 *
	 * Unreal keeps this number on the game viewport under the name MaxSplitscreenPlayers, which is a
	 * misnomer worth knowing about: it caps Create Player whether or not the screen is ever split, and it
	 * is still enforced with Force Disable Split Screen on. It defaults to 4, which is why a fifth
	 * controller can connect, be reported here, be given player slot 4 -- and still get no character.
	 * Splitscreen layouts stop at 4 because four is as many views as fit on a television; that is a
	 * rendering limit and has nothing to do with how many people are playing, which is the only thing this
	 * number really means.
	 *
	 * The alternative is an entry in DefaultEngine.ini under [/Script/Engine.GameViewportClient], which
	 * puts a decision belonging to your game in a config file, under the wrong name, where nobody looking
	 * at your player-spawning code will find it. Prefer setting it here, from wherever you decide how many
	 * players your game supports.
	 *
	 * This does not spawn or remove anything by itself, and lowering it does not evict players who already
	 * exist. Set it before you create players -- Begin Play is the natural place.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Controller Assignment",
		meta = (WorldContext = "WorldContextObject", DisplayName = "JSL4U Set Max Local Players", ToolTip = "Sets how many local players this game may create, overriding the engine's default of 4. Call it before creating players."))
	static bool JSL4USetMaxLocalPlayers(const UObject* WorldContextObject, int32 MaxLocalPlayers);

	// The current ceiling on Create Player -- see JSL4USetMaxLocalPlayers. Returns -1 when there is no game
	// viewport to ask (a Blueprint running outside a game world).
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Controller Assignment",
		meta = (WorldContext = "WorldContextObject", DisplayName = "JSL4U Get Max Local Players", ToolTip = "Returns how many local players this game may create, or -1 when there is no game viewport."))
	static int32 JSL4UGetMaxLocalPlayers(const UObject* WorldContextObject);

	/**
	 * Assigns a controller to a player slot, overriding the slot it was given when it connected.
	 *
	 * Slots are otherwise decided by connection order and are stable: a controller keeps its slot until it
	 * disconnects, and its slot is then left as a hole rather than shifting the others down, so nobody
	 * swaps characters mid-game. The consequence is that the slots you end up with depend on the order
	 * controllers were switched on -- if the player 1 controller disconnects, the remaining ones do NOT
	 * move down into slot 0. This is the node that fixes that, and it is the only thing that decides slots
	 * other than connection order.
	 *
	 * Slots may be shared: assigning two controllers to one slot makes both drive that player, which is
	 * exactly what a joined Joy-Con pair already does. Assigning either half of a joined pair moves the
	 * pair. The assignment lasts as long as the controller stays connected -- on reconnect it is a new
	 * controller and gets a slot automatically again.
	 *
	 * Takes the whole controller description so it can move an XInput pad too: one this plugin drives goes
	 * through the plugin's own slot table, one it does not is remapped through Unreal's input device
	 * mapper. Both end up on the same slot, and a game does not need to know which kind it is holding.
	 *
	 * @param Controller   The controller to assign (from Get All Connected Controllers, or the payload of
	 *                     any of the Wait For ... Changes nodes).
	 * @param PlayerIndex  The player slot to put it on, counting from 0. Pass -1 to hand the controller
	 *                     back to automatic assignment -- for a pad this plugin does not drive that is the
	 *                     unpaired user, which is Unreal's own way of saying the same thing.
	 * @return False if the controller is not connected.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Controller Assignment",
		meta = (DisplayName = "JSL4U Assign Controller To Player Index", ToolTip = "Assigns this controller to a zero-based player slot. Works for XInput pads too. Pass -1 to restore automatic assignment. Joined Joy-Cons move together."))
	static bool JSL4UAssignControllerToPlayerIndex(const FJSL4UControllerInfo& Controller, int32 PlayerIndex);

	// Assigns a controller to the player behind a PlayerController -- the setter counterpart of
	// JSL4UGetControllersAssignedToPlayer, and the one-node answer to "make this controller drive this
	// player". Same caveat as the getter: do NOT build this out of "Get Player Controller ID", which is the
	// legacy controller id rather than the platform user index slots are assigned from.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Controller Assignment",
		meta = (DisplayName = "JSL4U Assign Controller To Player", DefaultToSelf = "PlayerController", ToolTip = "Assigns this controller to the Local Player owned by Player Controller. Works for XInput pads too. Defaults to Self in a PlayerController Blueprint."))
	static bool JSL4UAssignControllerToPlayer(const FJSL4UControllerInfo& Controller, APlayerController* PlayerController);

	// The inverse of the PlayerIndex a controller reports in its FJSL4UControllerInfo: every controller
	// currently feeding a player slot. Two entries for a joined Joy-Con pair (rumble both to rumble "the
	// player"), one for a standalone controller, none if nothing is assigned to that slot. PlayerIndex is a
	// platform user index -- if you have a PlayerController, prefer JSL4UGetControllersAssignedToPlayer,
	// which converts it for you.
	// Answers for every controller Unreal accepts, XInput pads included, so "does this player have a
	// controller" is one question rather than one per controller family. Check the capability
	// flags on a result before calling a controller-specific node with it.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Controller Assignment",
		meta = (DisplayName = "JSL4U Get Controllers Assigned To Player Index", ToolTip = "Returns every controller feeding this zero-based player slot, XInput pads included. A joined Joy-Con pair returns both halves."))
	static TArray<FJSL4UControllerInfo> JSL4UGetControllersAssignedToPlayerIndex(int32 PlayerIndex);

	// The controller(s) of the player behind a Controller -- i.e. of whoever issued the command you
	// are reacting to. Defaults to self inside a Controller Blueprint, so this is the one-node answer
	// to "which controller is this player holding?" (e.g. to rumble it).
	// Takes the base AController so the reference a Pawn's Possessed event (or Get Controller) hands out
	// plugs in directly -- the PlayerController downcast happens here, natively. An AIController (or a
	// PlayerController with no Local Player yet, e.g. a remote net client) owns no physical input devices
	// and returns an empty array.
	// Note: do NOT build this out of "Get Player Controller ID". That is the legacy controller id, which is
	// a different number from the platform user index that player slots are assigned from -- this converts
	// through the same IPlatformInputDeviceMapper the assignment uses.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Controller Assignment",
		meta = (DefaultToSelf = "Controller", DisplayName = "JSL4U Get Controllers Assigned To Player", ToolTip = "Returns every controller feeding the Local Player behind this Controller, XInput pads included. Accepts the Controller from a Pawn's Possessed event directly; AI controllers return an empty array. Defaults to Self in a Controller Blueprint."))
	static TArray<FJSL4UControllerInfo> JSL4UGetControllersAssignedToPlayer(AController* Controller);

	/**
	 * Asks the plugin to re-scan for controllers, on a background thread.
	 *
	 * You rarely need this: controllers are picked up automatically at startup and whenever Windows reports a
	 * device change. It exists for the cases that produce no device-change message -- and it returns
	 * immediately, with the scan happening off the game thread, so it is safe to call from gameplay.
	 * Repeated calls while a scan is running are coalesced into one follow-up pass.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Controllers",
		meta = (DisplayName = "JSL4U Refresh Controllers", ToolTip = "Requests an asynchronous controller rescan. Normal device changes are detected automatically, so this is only needed when the platform sends no notification."))
	static void JSL4URefreshControllers();

	static int32 ConnectDevices();

	/**
	 * Fills OutDeviceHandleArray with the device id of every connected controller and returns how many
	 * there were. Kept as a low-level C++ helper; Blueprint uses JSL4UGetConnectedControllers.
	 * "Connected" means the controller has actually delivered input, not merely that it turned up in HID
	 * enumeration -- a controller that has just been switched off can linger in enumeration for a moment,
	 * and is deliberately not listed (nor reported as connected) during that window.
	 * For new Blueprints, prefer JSL4UGetConnectedControllers: it names each controller by Connection Id
	 * plus each controller's type, player slot and settings, so you rarely need this raw handle list.
	 */
	static int32 GetConnectedDeviceHandles(/* int* */ TArray<int32>& OutDeviceHandleArray); //, int32 InSize);

	/**
	 * Whether this connection id currently refers to a connected, working controller.
	 *
	 * Agrees with JSL4UGetConnectedControllers: a device that turned up in enumeration but has not delivered
	 * input (a controller that has just been switched off can linger there for a moment) reports false here
	 * too, unlike the lower-level HID enumeration check.
	 *
	 * A stored id whose controller has been unplugged reports false and keeps reporting false: connection
	 * ids are not reused, so this can never quietly become true again for a different controller. (The one
	 * asterisk is a pad this plugin does not drive: its negative id is derived from Unreal's device id,
	 * which does come back into circulation.) Prefer reacting to the JoyShock subsystem's connect/disconnect
	 * events over polling this; it is meant for cheaply validating an id you are already holding on to,
	 * without building the whole controller list to look it up.
	 */
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Controllers",
		meta = (DisplayName = "JSL4U Is Controller Connected", ToolTip = "Returns true only when Connection Id currently identifies a connected controller. Answers for every controller Unreal accepts, not only this plugin's."))
	static bool JSL4UIsControllerConnected(int64 ConnectionId);

	// get buttons as bits in the following order, using North South East West to name face buttons to avoid ambiguity between Xbox and Nintendo layouts:
	// 0x00001: up
	// 0x00002: down
	// 0x00004: left
	// 0x00008: right
	// 0x00010: plus
	// 0x00020: minus
	// 0x00040: left stick click
	// 0x00080: right stick click
	// 0x00100: L
	// 0x00200: R
	// ZL and ZR are reported as analogue inputs (GetLeftTrigger, GetRightTrigger), because DS4 and XBox controllers use analogue triggers, but we also have them as raw buttons
	// 0x00400: ZL
	// 0x00800: ZR
	// 0x01000: S
	// 0x02000: E
	// 0x04000: W
	// 0x08000: N
	// 0x10000: home / PS
	// 0x20000: capture / touchpad-click
	// 0x40000: SL
	// 0x80000: SR
	// These are the best way to get all the buttons/triggers/sticks, gyro/accelerometer (IMU), orientation/acceleration/gravity (Motion), or touchpad
	static FJoyShockState GetSimpleStateForHandle(int32 deviceId);

	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Input State",
		meta = (DisplayName = "JSL4U Get Controller State", ToolTip = "Returns one controller's current buttons, sticks and triggers in Unreal-friendly types. Reads that device directly, so it answers before the controller belongs to any player -- which is what a controller-assignment screen needs (\"press a button on the pad you want to be player 2\"). For gameplay, bind Enhanced Input instead: it is per-player, and these same inputs already reach it."))
	static FJSL4UJoyShockState JSL4UGetControllerState(int64 ConnectionId);
	
	static FIMUState GetRawIMUStateForHandle(int32 deviceId);

	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Motion",
		meta = (DisplayName = "JSL4U Get IMU State", ToolTip = "Returns one controller's gyroscope and acceleration in Unreal axes, after applying the selected gyro space. Reads that device directly, so it answers before the controller belongs to any player -- for assignment screens, calibration UI and diagnostics. For gameplay, this plugin already reports motion to Unreal's motion input, so bind Tilt / Rotation Rate / Gravity / Acceleration in Enhanced Input instead. Returns zeroes for a controller without motion sensors -- check Has Motion Sensors on the controller info."))
	static FJSL4UIMUState JSL4UGetIMUState(int64 ConnectionId);

	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Motion",
		meta = (DisplayName = "JSL4U Get Raw IMU State", ToolTip = "Returns one controller's untransformed gyroscope and acceleration in Unreal axes, ignoring the selected gyro space. Use Get IMU State unless you specifically need the untransformed reading."))
	static FJSL4UIMUState JSL4UGetRawIMUState(int64 ConnectionId);
	
	static FMotionState GetRawMotionStateForHandle(int32 deviceId);

	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Motion",
		meta = (DisplayName = "JSL4U Get Motion State", ToolTip = "Returns one controller's processed orientation, acceleration and gravity in Unreal coordinates. Reads that device directly, so it answers before the controller belongs to any player. For gameplay, bind Enhanced Input's motion inputs instead. Returns zeroes for a controller without motion sensors -- check Has Motion Sensors on the controller info."))
	static FJSL4UMotionState JSL4UGetMotionState(int64 ConnectionId);

	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Motion",
		meta = (DisplayName = "JSL4U Get Raw Motion State", ToolTip = "Returns one controller's untransformed orientation, acceleration and gravity from the motion processor. Use Get Motion State unless you specifically need the untransformed reading."))
	static FJSL4UMotionState JSL4UGetRawMotionState(int64 ConnectionId);

	static FTouchState GetTouchStateForHandle(int32 deviceId, bool previous = false);

	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Touchpad",
		meta = (DisplayName = "JSL4U Get Touch State", ToolTip = "Returns one controller's two touch contacts. Enable Previous to read the preceding report instead of the current one. Reads that device directly, so it answers before the controller belongs to any player. For gameplay, this plugin already reports the touchpad as gamepad axes, so bind TouchPad 1 / TouchPad 2 in Enhanced Input instead. Returns nothing touched for a controller without a touchpad -- check Has Touchpad on the controller info."))
	static FJSL4UTouchState JSL4UGetTouchState(int64 ConnectionId, bool bPrevious = false);

	// The touchpad's size in its own units (1920 x 943 on the DualShock 4 and DualSense), or zero for a
	// controller without one. JSL4UGetTouchState reports touches normalised to 0-1, so multiply by this if
	// you need touchpad-native coordinates -- e.g. to keep a drag's aspect ratio right.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Touchpad",
		meta = (DisplayName = "JSL4U Get Touchpad Size", ToolTip = "Returns the touchpad's native width and height, or zero for a controller without a touchpad. Touch State positions are normalized from 0 to 1."))
	static FVector2D JSL4UGetTouchpadSize(int64 ConnectionId);

	static bool GetTouchpadDimensionForHandle(int32 deviceId, int32 &sizeX, int32 &sizeY);

	// get thumbsticks

	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Input State",
		meta = (DisplayName = "JSL4U Get Left Stick", ToolTip = "Returns one controller's current left-stick position, from -1 to 1. Reads that device directly, so it answers before the controller belongs to any player -- for assignment screens and diagnostics. For gameplay, bind Enhanced Input instead."))
	static FVector2D JSL4UGetLeftStick(int64 ConnectionId);
	
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Input State",
		meta = (DisplayName = "JSL4U Get Right Stick", ToolTip = "Returns one controller's current right-stick position, from -1 to 1. Reads that device directly, so it answers before the controller belongs to any player -- for assignment screens and diagnostics. For gameplay, bind Enhanced Input instead."))
	static FVector2D JSL4UGetRightStick(int64 ConnectionId);

	// get accumulated average gyro since this function was last called or last flushed values
	static void GetAndFlushAccumulatedGyroForHandle(int32 deviceId, float& gyroX, float& gyroY, float& gyroZ);

	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Motion",
		meta = (DisplayName = "JSL4U Get And Clear Accumulated Gyro", ToolTip = "Returns the accumulated gyro rotation since the previous call, then clears the accumulator. Values use Unreal axes."))
	static FVector JSL4UGetAndClearAccumulatedGyro(int64 ConnectionId);

	// Sets how gyro input is transformed. GetAndFlushAccumulatedGyroForHandle, GetRawIMUStateForHandle and the
	// IMU states reported through the poll callback all honour it. One of 3 transformations:
	// 0 = local space -> no transformation is done on gyro input
	// 1 = world space -> gyro input is transformed based on the calculated gravity direction to account for the player's preferred controller orientation
	// 2 = player space -> a simple combination of local and world space that is as adaptive as world space but is as robust as local space
	static void SetGyroSpaceForHandle(int32 deviceId, int32 gyroSpace);

	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Motion",
		meta = (DisplayName = "JSL4U Set Gyro Space", ToolTip = "TYPICAL USE: Player Space, the usual choice for gyro aiming -- as adaptive as world space and as robust as local space. Local Space is the untransformed controller frame; World Space corrects by the measured gravity direction so yaw is always around the real vertical. Worth exposing as a player preference in a game that aims with gyro."))
	static void JSL4USetGyroSpace(int64 ConnectionId, EJSL4UGyroSpace GyroSpace);

	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Touchpad",
		meta = (DisplayName = "JSL4U Get Touch Position", ToolTip = "Returns the selected touch contact's normalized position from 0 to 1. Use Get Touch State when you also need contact id or down state."))
	static FVector2D JSL4UGetTouchPosition(int64 ConnectionId, bool bSecondTouch = false);

	// analog parameters have different resolutions depending on device
	// The smallest change this controller can report on a stick axis. Sticks are 8-bit on a DualShock 4 and
	// 12-bit on Switch controllers, so this differs per device -- useful for sizing a deadzone or an
	// on-screen readout to what the hardware can actually resolve.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Diagnostics",
		meta = (DisplayName = "JSL4U Get Stick Resolution Step", ToolTip = "Returns the smallest change this controller can report on one stick axis."))
	static float JSL4UGetStickResolutionStep(int64 ConnectionId);

	// The smallest change this controller can report on a trigger. Switch controllers have no analog
	// triggers and report 1 (fully on or fully off).
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Diagnostics",
		meta = (DisplayName = "JSL4U Get Trigger Resolution Step", ToolTip = "Returns the smallest trigger change this controller can report. Digital Switch triggers return 1."))
	static float JSL4UGetTriggerResolutionStep(int64 ConnectionId);

	// How often this controller sends input reports, in milliseconds per report.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Diagnostics",
		meta = (DisplayName = "JSL4U Get Poll Interval", ToolTip = "Returns this controller's expected milliseconds between input reports. This is an interval, not reports per second."))
	static float JSL4UGetPollInterval(int64 ConnectionId);

	// Seconds since this controller last sent an input report. A value climbing well past the poll rate means
	// the controller has gone quiet, which is the difference between the engine not routing its input and the
	// controller not sending any.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Diagnostics",
		meta = (DisplayName = "JSL4U Get Seconds Since Last Report", ToolTip = "Returns seconds since the controller last delivered an input report. A steadily increasing value indicates the device has stopped reporting."))
	static float JSL4UGetSecondsSinceLastReport(int64 ConnectionId);

	static float GetStickStepForHandle(int32 deviceId);

	static float GetTriggerStepForHandle(int32 deviceId);

	static float GetPollRateForHandle(int32 deviceId);

	static float GetTimeSinceLastUpdateForHandle(int32 deviceId);

	// --- Gyro calibration -------------------------------------------------------------------------------
	//
	// A gyroscope reports a small non-zero rotation even when perfectly still, so a controller left alone
	// will slowly drift. Calibrating measures that offset while the controller is still and subtracts it.
	//
	// Most games only need JSL4USetGyroCalibrationMode(Automatic) once per controller, and nothing else here:
	// the controller then works out on its own when it is being held still and keeps itself calibrated. The
	// Start/Stop/Reset nodes exist for driving an explicit "hold still while we calibrate" step in an options
	// screen, and only do anything in Manual mode.

	/**
	 * Chooses whether this controller calibrates its gyro on its own or only when told to.
	 * @param ConnectionId  The controller (see JSL4UGetConnectedControllers).
	 * @param Mode      Automatic for the set-and-forget behaviour most games want; Manual to drive it yourself.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Gyro Calibration",
		meta = (DisplayName = "JSL4U Set Gyro Calibration Mode", ToolTip = "TYPICAL USE: none -- every controller already starts in Automatic, which keeps the drift offset current by noticing when the controller is being held still. Call this only to opt out, with Manual, which hands calibration entirely to the Start/Stop nodes for a game that wants an explicit 'hold still' step."))
	static void JSL4USetGyroCalibrationMode(int64 ConnectionId, EJSL4UGyroCalibrationMode Mode);

	// Begins gathering samples for the gyro's drift offset. Call with the controller sitting still, and call
	// JSL4UStopManualGyroCalibration when you're done -- the longer it gathers, the better the offset. Only
	// meaningful in Manual mode.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Gyro Calibration",
		meta = (DisplayName = "JSL4U Start Manual Gyro Calibration", ToolTip = "Starts collecting gyro drift samples; the controller must stay still until Stop. Only meaningful in Manual mode -- in Automatic the controller already does this for itself, so most games never call this."))
	static void JSL4UStartManualGyroCalibration(int64 ConnectionId);

	// Stops gathering samples. The offset measured so far stays in effect.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Gyro Calibration",
		meta = (DisplayName = "JSL4U Stop Manual Gyro Calibration", ToolTip = "Stops collecting manual calibration samples and keeps the measured offset. Pairs with Start Manual Gyro Calibration; like it, unnecessary in Automatic mode."))
	static void JSL4UStopManualGyroCalibration(int64 ConnectionId);

	// Throws away the offset gathered so far and starts over. Use this when a calibration was taken while the
	// controller was in fact being moved, which leaves the gyro worse off than no calibration at all.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Gyro Calibration",
		meta = (DisplayName = "JSL4U Reset Gyro Calibration", ToolTip = "Discards the current drift offset and starts over. This is the one manual node worth exposing to players even in Automatic mode: an offset measured while the controller was moving leaves the gyro worse than no calibration, and this is the way out. Good behind a 'Recalibrate gyro' button."))
	static void JSL4UResetGyroCalibration(int64 ConnectionId);

	// This controller's calibration state, for driving a calibration screen (progress, "hold still" prompts).
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Gyro Calibration",
		meta = (DisplayName = "JSL4U Get Gyro Calibration Status", ToolTip = "Returns calibration mode, confidence, whether the controller reads as steady right now, and whether a manual calibration is running. Needed only to drive a calibration screen's prompts and progress; games without one never call it."))
	static FJSL4UGyroCalibrationStatus JSL4UGetGyroCalibrationStatus(int64 ConnectionId);

	/**
	 * The gyro drift offset currently being subtracted, in the same axes as JSL4UGetIMUState's Gyro.
	 * Save this per controller to restore a calibration between sessions, so a returning player doesn't have
	 * to calibrate again. Pairs exactly with JSL4USetGyroCalibrationOffset.
	 */
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Gyro Calibration",
		meta = (DisplayName = "JSL4U Get Gyro Calibration Offset", ToolTip = "Returns the drift offset, in the same Unreal axes as Get IMU State. Only useful if the game saves settings per controller, to spare a returning player a fresh calibration -- otherwise ignore this pair."))
	static FVector JSL4UGetGyroCalibrationOffset(int64 ConnectionId);

	// Restores an offset previously read with JSL4UGetGyroCalibrationOffset.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Gyro Calibration",
		meta = (DisplayName = "JSL4U Set Gyro Calibration Offset", ToolTip = "Restores a gyro drift offset previously returned by Get Gyro Calibration Offset."))
	static void JSL4USetGyroCalibrationOffset(int64 ConnectionId, FVector Offset);

	// calibration
	static void ResetContinuousCalibrationForHandle(int32 deviceId);

	static void StartContinuousCalibrationForHandle(int32 deviceId);

	static void PauseContinuousCalibrationForHandle(int32 deviceId);

	static void SetAutomaticCalibrationForHandle(int32 deviceId, bool enabled);

	static void GetCalibrationOffsetForHandle(int32 deviceId, float& xOffset, float& yOffset, float& zOffset);

	static void SetCalibrationOffsetForHandle(int32 deviceId, float xOffset, float yOffset, float zOffset);

	// Everything the plugin knows about one controller. Returns a struct with bIsConnected == false if
	// no controller has this connection id.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Controllers",
		meta = (DisplayName = "JSL4U Get Controller Info", ToolTip = "Returns this controller's connection id, Unreal input identities, model, assignment and current settings. Is Connected is false for a Connection Id that names no live controller."))
	static FJSL4UControllerInfo JSL4UGetControllerInfo(int64 ConnectionId);

	// super-getter for reading a whole lot of state at once
	static FJSL4URawSettings GetControllerSettingsForHandle(int32 deviceId);

	// what kind of controller is this?
	static int32 GetControllerTypeForHandle(int32 deviceId);

	// The mouse sensor's movement since the engine's input axes were last given it, in sensor counts. Not a
	// Blueprint node: this is the feed behind the JoyShock Mouse axis keys, and it keeps its own baseline
	// so a game reading Consume Switch 2 Mouse Delta and a game binding the axis key see the same motion
	// rather than half of it each. Zero for a controller without the sensor.
	static void ConsumeMouseAxisDeltaForHandle(int32 deviceId, float& outDeltaX, float& outDeltaY);

	/**
	 * Sets the controller's light: the DualShock 4's light bar or the DualSense's. Controllers without a
	 * settable light ignore this (Switch controllers report a fixed body colour instead -- see
	 * JSL4UGetConnectedControllers).
	 * @param ConnectionId  The controller (see JSL4UGetConnectedControllers).
	 * @param Color     The colour to display. Alpha is ignored.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Output",
		meta = (DisplayName = "JSL4U Set Light Color", ToolTip = "Sets a DualShock 4 or DualSense light color. Controllers without a controllable light ignore this call."))
	static void JSL4USetLightColor(int64 ConnectionId, FColor Color);

	/**
	 * Sets the controller's rumble motors directly, 0 (off) to 1 (maximum intensity).
	 *
	 * You often don't need this node. These controllers work with Unreal's own force feedback, so
	 * "Play Force Feedback Effect" and "Client Play Force Feedback" drive them exactly as they drive an
	 * Xbox pad -- which gets you authored effects with curves, falloff and looping for free, and one code
	 * path for every gamepad. Both reach the same maximum: force feedback is clamped to 0-1 and 1 arrives
	 * here as full strength, so an effect that feels weak is a weak curve in the asset, not a limit of the
	 * plugin.
	 *
	 * Three things this node does that force feedback cannot, because force feedback is aimed at a *player*:
	 *  - Rumble one specific controller. Both halves of a joined Joy-Con pair are one player but two device
	 *    ids, so only this can buzz just the left one.
	 *  - Rumble a controller that is not assigned to a player at all. A controller-assignment screen
	 *    usually wants "press here and feel which controller this is" before any players exist, and force
	 *    feedback delivers nothing to a slot with no local player behind it.
	 *  - Hold a constant intensity without authoring a looping effect asset.
	 *
	 * BigRumble drives the heavy/low-frequency motor (strong shake, e.g. explosions, impacts);
	 * SmallRumble drives the light/high-frequency motor (fine buzz, e.g. UI feedback, engines).
	 * The rumble stays at the given intensities until you call this again -- call it with (0, 0) to stop.
	 * This and Unreal's force feedback write the same two values, so whichever ran most recently wins.
	 *
	 * Supported controllers: DualShock 4, DualSense, Joy-Cons and Pro Controller (HD rumble, both values
	 * mapped to the low/high-frequency actuator components).
	 * The Switch 2 Pro Controller does NOT support amplitude: it plays a fixed vibration preset while either
	 * value is above 0 and stops at (0, 0), so it is effectively on/off. Its amplitude-accurate rumble
	 * channel hasn't been mapped over USB yet, which means an effect that fades in or out feels like a flat
	 * buzz on that controller.
	 *
	 * The packet goes out from the controller's own polling thread, so this never blocks the game thread;
	 * it takes effect on that controller's next report (well under a frame for a connected controller).
	 * @param ConnectionId  The controller (see JSL4UGetConnectedControllers).
	 * @param SmallRumble  High-frequency motor intensity, 0-1.
	 * @param BigRumble    Low-frequency motor intensity, 0-1.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Output",
		meta = (DisplayName = "JSL4U Set Controller Rumble", ToolTip = "Sets one controller's high-frequency and low-frequency rumble, from 0 to 1; call with both at zero to stop. Drives that device directly, so it works before the controller belongs to any player -- buzzing a pad on an assignment screen so the player can tell which one they are holding. For authored gameplay effects, use Unreal Force Feedback instead: it is per-player, and it drives this plugin's controllers and XInput pads from the same effect."))
	static void JSL4USetControllerRumble(int64 ConnectionId, float SmallRumble, float BigRumble);

	// The channel Unreal's own force feedback writes to, kept separate from JSL4USetControllerRumble's so the two
	// cannot cancel each other -- the engine pushes force feedback values every frame, zeroes included.
	// Not exposed to Blueprint: games drive this through Play Force Feedback Effect and friends.
	static void SetForceFeedbackRumble(int32 DeviceHandle, int32 SmallRumble, int32 BigRumble);

	// --- Addressed by library handle, for the plugin's own insides only --------------------------------
	//
	// The handle is the JoyShockLibrary's key and it is reused: the controller it names today is not
	// necessarily the one it named a moment ago. That is exactly why nothing outside the plugin is given
	// one -- see FJSL4UControllerInfo::ConnectionId. These exist for the paths that START from the library
	// side and already hold a handle from the callback that raised them (the connect and disconnect
	// callbacks, the polling threads, the player-slot refresh), where translating to a connection id and
	// straight back would only add a lookup and a way to be wrong.
	static FJSL4UControllerInfo GetControllerInfoForHandle(int32 DeviceHandle);
	static FJSL4UMotionState GetMotionStateForHandle(int32 DeviceHandle);
	static void SetPlayerIndicatorForHandle(int32 DeviceHandle, int32 Number);

	// The rotation that takes a reading out of a sideways Joy-Con's frame and into the upright one it would
	// have had in a pair. Identity for everything else. Pure maths, no locks: the library's getters and the
	// interface's Enhanced Input dispatch both apply it, and calling one function is what stops the two from
	// disagreeing about which way a horizontal Joy-Con is pointing.
	static FQuat GetJoyConGripUndoRotation(bool bHorizontal, bool bIsLeft);

	/**
	 * Sets the controller's player number indicator (the DualSense's player LEDs, or a Switch controller's
	 * row of LEDs). The DualShock 4 has no such indicator and ignores this.
	 * @param ConnectionId  The controller (see JSL4UGetConnectedControllers).
	 * @param Number    The player number to show, counting from 1.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Output",
		meta = (DisplayName = "JSL4U Set Player Indicator", ToolTip = "Shows a one-based player number on Switch, Switch 2 or DualSense player LEDs. The DualShock 4 has no numeric indicator and ignores this call; use Set Light Color for its RGB light bar."))
	static void JSL4USetPlayerIndicator(int64 ConnectionId, int32 Number);

	/**
	 * Sets the blue HOME ring light on a right Joy-Con or Pro Controller (0 = off, 1 = full brightness).
	 * Other controllers ignore this.
	 *
	 * This is Nintendo's *notification* light, not a player indicator -- on a real Switch it stays off
	 * during play and the system uses it for pairing, low battery and lost-connection feedback. The four
	 * green player LEDs are the player identity (see Set Player Indicator); driving this one instead is
	 * likely to read as a system message rather than as "you are player 2".
	 *
	 * The plugin switches this light off once, when the controller comes online. Calling this hands
	 * ownership to the game permanently: that automatic clear stops for the controller, and the light then
	 * holds whatever the game last set -- including zero. There is no way back to automatic for the rest of
	 * the connection, which is deliberate: a game that has opinions about this light should not have them
	 * silently overwritten.
	 *
	 * Every call is sent to the controller, even one that asks for the brightness the light is already
	 * believed to have. The firmware turns this light back on by itself (a reconnect or a battery
	 * notification will do it), so re-asserting a value the plugin thinks is already set is a real request
	 * and not a no-op -- which is what makes "set it to 0" reliable for turning the light off.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Output",
		meta = (DisplayName = "JSL4U Set Home Light", ToolTip = "Sets the HOME ring light brightness (0-1) on a right Joy-Con or Pro Controller. This is a notification light, not a player indicator. Calling it stops the plugin's automatic keep-it-off upkeep for that controller."))
	static void JSL4USetHomeLight(int64 ConnectionId, float Brightness);

	/**
	 * The extra sensors a Switch 2 controller carries: battery voltage, temperature, the magnetometer and
	 * the optical mouse sensor in its underside.
	 *
	 * Motion is NOT here -- read the accelerometer and gyroscope through the usual IMU nodes, which every
	 * controller this plugin drives answers. This is only for readings no other family reports.
	 *
	 * Any other controller returns Is Supported false with every field zero, so this is safe to call
	 * without checking the controller's model first.
	 */
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Controllers",
		meta = (DisplayName = "JSL4U Get Switch 2 Sensors", ToolTip = "Battery voltage, temperature, magnetometer and mouse sensor from a Switch 2 controller. Motion is not here -- use the IMU nodes for that. Other controllers return Is Supported false."))
	static FJSL4USwitch2Sensors JSL4UGetSwitch2Sensors(int64 ConnectionId);

	// How far the mouse sensor has moved since this node last answered for the same controller -- the
	// per-frame vector to aim or scroll with, ready to use.
	//
	// It CONSUMES: each call reports the movement since the previous call and starts counting again, so two
	// callers on the same controller each see part of the motion and neither sees all of it. Call it in one
	// place. The name says so because the alternative -- a second consumer quietly halving the sensitivity
	// -- is not something the caller could see going wrong.
	//
	// A game that wants to read the same movement from several places should use Mouse Travel on Get
	// Switch 2 Sensors instead, and take its own differences: that value only accumulates and answers
	// everyone the same.
	UFUNCTION(BlueprintCallable, Category = "JoyShock Library|Controllers",
		meta = (DisplayName = "JSL4U Consume Switch 2 Mouse Delta", ToolTip = "Movement of the Switch 2 mouse sensor since this node last answered for this controller, with the sensor's wrap already resolved. Consumes what it reports: call it from one place only. Zero for a controller without the sensor."))
	static FVector2D JSL4UConsumeSwitch2MouseDelta(int64 ConnectionId);

	// Converts the same semantic one-based number into Nintendo's four visible LED states. This is useful
	// for controller mirrors and UI; the physical controller is updated automatically by assignment.
	UFUNCTION(BlueprintPure, Category = "JoyShock Library|Output",
		meta = (DisplayName = "JSL4U Get Switch Player LED Pattern", ToolTip = "Returns the four Nintendo player LEDs for player numbers 1-8, matching Joy-Cons, Switch Pro and Switch 2 Pro Controllers."))
	static void JSL4UGetSwitchPlayerLedPattern(int32 PlayerNumber,
		bool& bLed1, bool& bLed2, bool& bLed3, bool& bLed4);

	// --- Low-level C++ compatibility helpers. Not exposed to Blueprint. ---

};
