#pragma once


#include "CoreTypes.h"
#include "Containers/Queue.h"
#include "GenericPlatform/IInputInterface.h"
#include "IInputDevice.h"
#include "GenericPlatform/GenericApplicationMessageHandler.h"
#include "JoyShockTypes.h"

// Max number of controller buttons.  Must be < 256
#define MAX_NUM_CONTROLLER_BUTTONS 27

struct FTouchState;
struct FJoyShockState;
struct FIMUState;
enum class FForceFeedbackChannelType;

// Interface class for JoyShock devices (DualShock 4, DualSense, Switch Pro Controller, JoyCons)
class FJoyShockInterface : public IInputDevice
{
public:
	static TSharedRef<FJoyShockInterface> Create(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler);
	
	/**
	 * Poll for controller state and send events if needed
	 */
	virtual void SendControllerEvents() override;

	virtual void SetMessageHandler(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override;

	/**
	* Sets the strength/speed of the given channel for the given controller id.
	* NOTE: If the channel is not supported, the call will silently fail
	*
	* @param ControllerId the id of the controller whose value is to be set
	* @param ChannelType the type of channel whose value should be set
	* @param Value strength or speed of feedback, 0.0f to 1.0f. 0.0f will disable
	*/
	virtual void SetChannelValue(int32 ControllerId, const FForceFeedbackChannelType ChannelType, const float Value) override;

	/**
	* Sets the strength/speed of all the channels for the given controller id.
	* NOTE: Unsupported channels are silently ignored
	*
	* @param ControllerId the id of the controller whose value is to be set
	* @param Values strength or speed of feedback for all channels
	*/
	virtual void SetChannelValues(int32 ControllerId, const FForceFeedbackValues& Values) override;

	virtual bool IsGamepadAttached() const override { return bIsGamepadAttached; }
	virtual void Tick(float DeltaTime) override {};
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override { return false; }

	virtual ~FJoyShockInterface() override;

	// --- Joy-Con pairing (game thread only) --------------------------------------------------------------
	// Joins two connected device handles so they share one player slot (their input feeds a single player).
	// Returns false if the handles are equal or not both connected. Any existing joins involving either
	// handle are dissolved first.
	bool JoinControllers(int32 HandleA, int32 HandleB);
	// Dissolves any join involving this handle (both halves become their own players again).
	void UnjoinController(int32 Handle);
	// Dissolves every join.
	void UnjoinAllControllers();
	// Returns the handle joined with this one, or INDEX_NONE if it isn't joined.
	int32 GetJoinPartner(int32 Handle) const;
	// Whether this handle is the one that represents its logical controller (see GetGroupPrimary). A
	// standalone controller is its own primary; exactly one half of a joined pair is.
	bool IsJoinPrimary(int32 Handle) const;
	// Overrides one Joy-Con's presentation. Horizontal mode is the standalone default; setting it on a
	// joined half dissolves the pair. Vertical mode is available for exceptional games such as Just Dance.
	bool SetJoyConHorizontal(int32 Handle, bool bHorizontal);
	bool IsJoyConHorizontal(int32 Handle) const;
	// Both halves of the question the motion presentation asks -- is this controller held sideways, and is
	// it the left half -- answered from one locked read, so a grip change cannot be seen half-applied (which
	// would rotate a reading the wrong way for exactly one frame). False for anything that is not a Joy-Con.
	bool GetJoyConGrip(int32 Handle, bool& bOutHorizontal, bool& bOutIsLeft) const;
	// Assigns this device's logical controller to a player slot, overriding the slot it was given on
	// connection. Pass INDEX_NONE to hand it back to automatic assignment. Returns false if the handle is
	// not a connected controller. Slots may be shared: assigning two controllers to one slot makes both
	// drive that player, which is what a joined Joy-Con pair already does.
	bool SetPlayerIndexForDevice(int32 Handle, int32 PlayerIndex);
	// Adds all identity and player-assignment fields owned by the input-device interface to a controller
	// info struct in one locked read. Returns false when the handle is not connected.
	bool FillControllerInfo(FJSL4UControllerInfo& Info, int32 Handle) const;

	// The library handle this connection id addresses, or INDEX_NONE for a connection this interface does
	// not own -- an id belonging to a controller that has left, or the negative id of a pad Unreal drives.
	//
	// This is the whole translation layer behind the plugin's public API: a Blueprint names a controller by
	// its connection id, which is unique for the run, and only here does that become the library's reusable
	// handle, immediately before the call that uses it. Nothing outside the plugin ever holds the handle,
	// so nothing outside can hold a stale one and drive whichever controller inherited it.
	int32 GetHandleForConnection(int64 ConnectionId) const;

	// The connection id of a live handle, or 0 when the handle is not a connected controller. For the paths
	// that start from the library side (the connect/disconnect callbacks, the polling threads) and have to
	// name the controller the way the public API does.
	int64 GetConnectionForHandle(int32 Handle) const;

private:
	FJoyShockInterface(const TSharedRef<FGenericApplicationMessageHandler>& MessageHandler);

	void InitializeAdditionalKeys();

	struct FControllerState
	{
		// If the controller is currently connected
		bool bIsConnected = false;

		FPlatformUserId PlatformUser = PLATFORMUSERID_NONE;
		FInputDeviceId InputDevice = INPUTDEVICEID_NONE;
		int64 ConnectionId = 0;
		FName HardwareDeviceIdentifier = NAME_None;

		// Readable device name, cached at connect time so we don't re-query it (under a lock) every frame.
		FString DeviceName;

		// Diagnostic only: when this device last delivered an input report to the interface, and whether a
		// stall has already been reported for it. This distinguishes the two ways input can appear to "stop
		// working" -- the controller's reports no longer reaching us (a plugin/HID problem) versus reports
		// arriving fine but the engine not routing them (focus, input mode, bindings). Without it the two
		// look identical from the game.
		double LastReportTime = 0.0;
		bool bReportedInputStall = false;

		FJoyShockState SimpleState = {};
		FJoyShockState PreviousSimpleState = {};
		FIMUState IMUState = {};
		FIMUState PreviousIMUState = {};
		FTouchState TouchState = {};
		FTouchState PreviousTouchState = {};

		// Standalone Joy-Cons use Nintendo's horizontal grip by default. A joined pair is vertical. The
		// previous value lets the analog dispatcher explicitly clear axes when a grip transition rotates
		// or moves the physical stick between Unreal's left/right stick keys.
		bool bJoyConHorizontal = false;
		bool bAnalogWasJoyConHorizontal = false;
		bool bButtonsWereJoyConHorizontal = false;

		// Last mouse delta sent to the engine, for the same reason every other axis keeps its previous
		// value: the dispatcher sends while an axis is off centre and once more when it returns, and it
		// needs the old value to know which. A mouse that stops moving is an axis returning to centre.
		float PreviousMouseDeltaX = 0.f;
		float PreviousMouseDeltaY = 0.f;
		// Registration chords are control-management input, not gameplay input. After a transition, suppress
		// those physical buttons until all of them are released so joining cannot also fire four actions.
		int32 SuppressedGripButtons = 0;
		
		// Force-feedback values most recently set through Unreal's own system (Play Force Feedback Effect and
		// friends). Kept per device because SetChannelValue updates one channel at a time and the other three
		// have to survive that.
		FForceFeedbackValues ForceFeedback;
	};

	bool bIsGamepadAttached;
	const FName JoyShockControllerName = FName("JoyShock");

	static FString GetDeviceName(int32 InControllerId);
	static FName GetHardwareDeviceIdentifier(int32 InControllerId);

	// Controller states
	TMap<int32, FControllerState> ControllerStateByDeviceHandle = {};

	TArray<int32> DeviceHandles = {};

	// Guards structural access to DeviceHandles / ControllerStateByDeviceHandle. These containers are
	// read/iterated by the game thread (SendControllerEvents, connect/disconnect handling) and read by
	// the background polling threads, so all access must be serialised to avoid reallocation races.
	mutable FCriticalSection ControllerContainerLock;

	// Joy-Con joining: bidirectional map of a device handle to the handle it's joined with. A handle not
	// present here is a standalone controller.
	TMap<int32, int32> JoinPartner;

	// Player-slot assignment: maps each "logical controller" (a standalone device or a joined pair,
	// identified by its lower "primary" handle) to a player slot (0, 1, 2, ...). Slots are STABLE, not
	// compact: a controller keeps its slot for as long as it stays connected, and a disconnect leaves its
	// slot as a hole rather than shifting the others down, so nobody swaps characters mid-game. The hole is
	// reused by the next controller to connect. A game that wants a specific controller on a specific
	// player calls SetPlayerIndexForDevice -- connection order is otherwise the only thing deciding this,
	// and the plugin will not second-guess it. Recomputed by RefreshPlayerAssignments() whenever devices
	// connect/disconnect or joins change.
	//
	// Slots already occupied by a controller from another input interface (an XInput pad) are skipped, so a
	// controller here lands on the same player a second Xbox pad would have. Getting that wrong is what
	// forced a game to treat XInput and JoyShock controllers differently.
	TMap<int32, int32> PlayerSlotByPrimary;

	// Recomputes player slots and maps every connected device to its logical controller's platform user in
	// the platform input-device mapper, so the engine sees one player per logical controller.
	void RefreshPlayerAssignments();

	int64 NextConnectionId = 1;

	// Returns the primary (lower) handle of the logical controller this handle belongs to.
	int32 GetGroupPrimary(int32 Handle) const;

	// Whether this player slot already has a controller on it that isn't one of ours -- an XInput pad, in
	// practice. The keyboard and mouse don't count: they sit on the first player and would otherwise make
	// slot 0 permanently unavailable to controllers.
	bool IsPlayerSlotClaimedByAnotherDevice(int32 Slot) const;

	// Whether this input device id belongs to a controller this interface created.
	bool IsOwnInputDevice(FInputDeviceId InInputDevice) const;

	// Upper bound on the slot search, so a mapper that reported every slot as occupied could not spin here.
	static constexpr int32 MaxPlayerSlotSearch = 64;

	// Every connected device currently driving the player Unreal identifies by InControllerId. Usually one,
	// but two for a joined Joy-Con pair -- both halves share a platform user, so force feedback aimed at
	// "that player" has to reach both. The caller must hold ControllerContainerLock.
	TArray<int32> GetDeviceHandlesForControllerId(int32 InControllerId) const;

	// Turns a device's stored force-feedback channels into a rumble request. The caller must hold
	// ControllerContainerLock.
	void SendForceFeedback(int32 DeviceHandle, const FForceFeedbackValues& Values) const;

	// Connect/disconnect notifications originate on background threads (enumeration and polling threads),
	// but touching the platform input-device mapper and our containers is only safe on the game thread.
	// We queue them here and drain them at the start of SendControllerEvents (which runs on the game thread).
	TQueue<int32, EQueueMode::Mpsc> PendingConnects;
	TQueue<TPair<int32, bool>, EQueueMode::Mpsc> PendingDisconnects;
	// Blocked-function reports also originate on the polling threads; they only need the game-thread
	// module event (no container or mapper access), so the drain broadcasts them directly.
	TQueue<TPair<int32, EJSL4UControllerFunction>, EQueueMode::Mpsc> PendingBlockedFunctions;

	struct FJoyConPairingChange
	{
		int32 LeftDeviceId = INDEX_NONE;
		int32 RightDeviceId = INDEX_NONE;
		bool bJoined = false;
	};

	// Delay before sending a repeat message after a button was first pressed
	float InitialButtonRepeatDelay;

	// Delay before sending a repeat message after a button has been pressed for a while
	float ButtonRepeatDelay;

	TSharedRef<FGenericApplicationMessageHandler> MessageHandler;

	// Reports an axis to the engine as-is. Deadzones are deliberately not applied here -- see the note on
	// the definition.
	void OnControllerAnalog(const FPlatformUserId& InPlatformUser, const FInputDeviceId& InInputDevice,
						const FName& GamePadKey, float NewAxisValueNormalized, float OldAxisValueNormalized) const;
	
	void ProcessButtons(int32 CurrentButtons, int32 PreviousButtons, FPlatformUserId PlatformUser, FInputDeviceId InputDevice);
	void ProcessAnalogInputs(const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState,
		bool bJoyConLeft, bool bJoyConRight, bool bSonyPad, bool bHorizontal, bool bWasHorizontal,
		FPlatformUserId PlatformUser, FInputDeviceId InputDevice);
	// Dispatches this device's neutral state -- releases for every held button, zeroes for every off-centre
	// axis and trigger, ends for every active touch, and zeroed motion -- then clears its cached state.
	// Called when the device disconnects, because the engine latches the last value it was given and a
	// vanished controller would otherwise leave its input held down for the rest of the session. Must run on
	// the game thread, with ControllerContainerLock held and the device still mapped to its platform user.
	void ReleaseAllInput(FControllerState& State);

	void UpdateJoyConGripTransitions(TArray<FJoyConPairingChange>& OutPairingChanges);
	FJoyConPairingChange MakeJoyConPairingChange(int32 HandleA, int32 HandleB, bool bJoined) const;
	static void BroadcastJoyConPairingChanges(const TArray<FJoyConPairingChange>& PairingChanges);
	static int32 TransformJoyConButtons(int32 Buttons, bool bJoyConLeft, bool bJoyConRight, bool bHorizontal);
	// Callbacks
	void OnPollCallback(int32 DeviceHandle, const FJoyShockState& SimpleState, const FJoyShockState& PreviousSimpleState, const FIMUState& IMUState, const FIMUState& PreviousIMUState, float DeltaTime);

	// Reports the touchpad as gamepad axes and a Touched button -- the route Enhanced Input should use for
	// it. Independent of ProcessTouchState, which is the (off by default) screen-touch emulation.
	void ProcessTouchpadInputs(const FTouchState& InTouchState, const FTouchState& InPreviousTouchState,
		FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const;
	void ProcessSingleTouchpadInput(bool bTouchDown, float TouchX, float TouchY,
		bool bPreviousTouchDown, float PreviousTouchX, float PreviousTouchY,
		const FName& XKey, const FName& YKey, const FName& TouchedKey,
		FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const;

	void ProcessSingleTouchState(bool bTouchDown, int32 TouchID, const FVector2D& TouchLocation, bool bPreviousTouchDown, int32 PreviousTouchID, const FVector2D& PreviousTouchLocation, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const;
	void ProcessTouchState(const FTouchState& InTouchState, const FTouchState& InPreviousTouchState, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const;
	void OnTouchCallback(int32 DeviceHandle, const FTouchState& TouchState, const FTouchState& PreviousTouchState, float DeltaTime);
	// Reports this controller's motion to Unreal's motion input (Tilt / RotationRate / Gravity /
	// Acceleration), so gyro is bindable in Enhanced Input without any plugin-specific code.
	void ProcessIMUState(int32 DeviceHandle, const FIMUState& InIMUState, FPlatformUserId PlatformUser, FInputDeviceId InputDevice) const;

	bool OnConnectCallback(int32 InDeviceHandle);

	// Losing one half of a joined pair separates it, exactly as SL+SR or JSL4UUnjoinJoyCon would, so the
	// separation has to be reported like any other. The change is handed back rather than broadcast here
	// because this runs inside the drain, under ControllerContainerLock.
	bool OnDisconnectCallback(int32 InDeviceHandle, bool bInHasTimedOut, TArray<FJoyConPairingChange>& OutPairingChanges);

	// Additional input names.
	// JSL aliases one bit per pair of equivalent buttons across controller families, so each of these is a
	// single key covering both: PS == Home (JSMASK_PS == JSMASK_HOME) and TouchPadClick == Capture
	// (JSMASK_TOUCHPAD_CLICK == JSMASK_CAPTURE). That matches how Plus/Options and Minus/Share are already
	// handled (one abstract SpecialRight/SpecialLeft key), and how Unreal names shared positions.
	const FGamepadKeyNames::Type PSButtonKeyName = "PS";
	const FKey PSButtonKey = "PS";

	const FGamepadKeyNames::Type TouchPadClickKeyName = "TouchPadClick";
	const FKey TouchPadClickKey = "TouchPadClick";
	
	const FGamepadKeyNames::Type MicButtonKeyName = "Mic";
	const FKey MicButtonKey = "Mic";
	
	const FGamepadKeyNames::Type SideLeftButtonKeyName = "SideLeft";
	const FKey SideLeftButtonKey = "SideLeft";
	
	const FGamepadKeyNames::Type SideRightButtonKeyName = "SideRight";
	const FKey SideRightButtonKey = "SideRight";
	
	const FGamepadKeyNames::Type FunctionLeftButtonKeyName = "FunctionLeft";
	const FKey FunctionLeftButtonKey = "FunctionLeft";

	const FGamepadKeyNames::Type FunctionRightButtonKeyName = "FunctionRight";
	const FKey FunctionRightButtonKey = "FunctionRight";

	// Switch 2 Pro Controller
	const FGamepadKeyNames::Type CButtonKeyName = "SwitchC";
	const FKey CButtonKey = "SwitchC";

	const FGamepadKeyNames::Type GripLeftButtonKeyName = "GripLeft";
	const FKey GripLeftButtonKey = "GripLeft";

	const FGamepadKeyNames::Type GripRightButtonKeyName = "GripRight";
	const FKey GripRightButtonKey = "GripRight";

	/**
	 * DualShock 4 / DualSense touchpad, as ordinary gamepad axes.
	 *
	 * Unreal's Touch1..Touch10 keys are the wrong place to look for these, which is what everyone tries
	 * first. Those are TOUCHSCREEN keys: they are fed by Slate's pointer pipeline, so binding a controller
	 * touchpad to them means synthesising a screen touch at a screen position, for a surface that has no
	 * screen position -- and a Slate pointer press moves that player's focus to whatever widget it lands
	 * on, which is how tapping the pad used to kill the controller's buttons (see ProcessTouchState).
	 *
	 * A controller touchpad is not a touchscreen; it is part of one player's gamepad, exactly like that
	 * player's right stick. So it is registered like one. Enhanced Input binds JoyShock TouchPad 1 as a 2D
	 * axis the same way it binds a thumbstick, routed to the player who owns the controller, with no
	 * pointer, no focus and no window coordinates involved.
	 *
	 * Both fingers the hardware reports get a pad. Each is centred like a stick -- (0, 0) is the middle of
	 * the surface, X runs left to right and Y runs bottom to top, matching the sticks so a shared Enhanced
	 * Input mapping (To World Space included) behaves the same on both. Not touching reads as (0, 0), which
	 * a corner-relative 0..1 range could not express: there, "no finger" and "finger at the top-left corner"
	 * would be the same value. Touched says which it is.
	 */
	const FGamepadKeyNames::Type TouchPad1XKeyName = "TouchPad1_X";
	const FKey TouchPad1XKey = "TouchPad1_X";
	const FGamepadKeyNames::Type TouchPad1YKeyName = "TouchPad1_Y";
	const FKey TouchPad1YKey = "TouchPad1_Y";
	const FKey TouchPad1Key = "TouchPad1";
	const FGamepadKeyNames::Type TouchPad1TouchedKeyName = "TouchPad1Touched";
	const FKey TouchPad1TouchedKey = "TouchPad1Touched";

	const FGamepadKeyNames::Type TouchPad2XKeyName = "TouchPad2_X";
	const FKey TouchPad2XKey = "TouchPad2_X";
	const FGamepadKeyNames::Type TouchPad2YKeyName = "TouchPad2_Y";
	const FKey TouchPad2YKey = "TouchPad2_Y";
	const FKey TouchPad2Key = "TouchPad2";
	const FGamepadKeyNames::Type TouchPad2TouchedKeyName = "TouchPad2Touched";
	const FKey TouchPad2TouchedKey = "TouchPad2Touched";

	/**
	 * The optical mouse sensor in a Joy-Con 2's underside, as gamepad axes.
	 *
	 * Registered like the touchpad above and for the same reason: it belongs to one player's controller, so
	 * it is that player's gamepad axis rather than anything to do with the system pointer. Nothing here
	 * moves the OS cursor, and a game binding it does not have to care whether Windows thinks a mouse
	 * exists.
	 *
	 * There are two, one per half, because a joined pair really is two mice for one player -- the console
	 * uses them that way, and merging them into a single axis would make one hand cancel the other. Which
	 * half a value came from is the whole information: a detached Joy-Con 2 (L) feeds only the left pair, a
	 * (R) only the right, and a joined pair feeds both independently while remaining one player. A game that
	 * wants "the mouse, whichever half is in use" binds both to the same Input Action; only one of them will
	 * be moving.
	 *
	 * The value is a DELTA in the sensor's own counts, not a position -- about 12000 counts to the length of
	 * a Pro Controller -- which is the same shape as Unreal's own Mouse X / Mouse Y and wants the same
	 * treatment: scale it in the Input Mapping Context to taste. It is deliberately not normalised to
	 * anything, because normalising would mean inventing a DPI the hardware never states.
	 */
	const FGamepadKeyNames::Type MouseLeftXKeyName = "Switch2MouseLeft_X";
	const FKey MouseLeftXKey = "Switch2MouseLeft_X";
	const FGamepadKeyNames::Type MouseLeftYKeyName = "Switch2MouseLeft_Y";
	const FKey MouseLeftYKey = "Switch2MouseLeft_Y";
	const FKey MouseLeftKey = "Switch2MouseLeft";

	const FGamepadKeyNames::Type MouseRightXKeyName = "Switch2MouseRight_X";
	const FKey MouseRightXKey = "Switch2MouseRight_X";
	const FGamepadKeyNames::Type MouseRightYKeyName = "Switch2MouseRight_Y";
	const FKey MouseRightYKey = "Switch2MouseRight_Y";
	const FKey MouseRightKey = "Switch2MouseRight";


	
	const TArray<TTuple<int32, FName>> JoyShockMaskMappings = {
		{JSMASK_UP, FGamepadKeyNames::DPadUp},
		{JSMASK_DOWN, FGamepadKeyNames::DPadDown},
		{JSMASK_LEFT, FGamepadKeyNames::DPadLeft},
		{JSMASK_RIGHT, FGamepadKeyNames::DPadRight},
		{JSMASK_OPTIONS, FGamepadKeyNames::SpecialRight}, // Also matches JSMASK_PLUS
		{JSMASK_SHARE, FGamepadKeyNames::SpecialLeft}, // == JSMASK_MINUS
		{JSMASK_LCLICK, FGamepadKeyNames::LeftThumb},
		{JSMASK_RCLICK, FGamepadKeyNames::RightThumb},
		{JSMASK_L, FGamepadKeyNames::LeftShoulder},
		{JSMASK_R, FGamepadKeyNames::RightShoulder},
		{JSMASK_ZL, FGamepadKeyNames::LeftTriggerThreshold},
		{JSMASK_ZR, FGamepadKeyNames::RightTriggerThreshold},
		{JSMASK_S, FGamepadKeyNames::FaceButtonBottom},
		{JSMASK_E, FGamepadKeyNames::FaceButtonRight},
		{JSMASK_W, FGamepadKeyNames::FaceButtonLeft},
		{JSMASK_N, FGamepadKeyNames::FaceButtonTop},

		// These two masks are each shared by two names in JSL (PS == HOME, TOUCHPAD_CLICK == CAPTURE), so
		// there is one entry -- and one key -- per bit. Registering a separate Home/Capture key here would
		// be dead: the bit is already claimed, and nothing would ever emit it.
		{JSMASK_PS, PSButtonKeyName}, // == JSMASK_HOME
		{JSMASK_TOUCHPAD_CLICK, TouchPadClickKeyName}, // == JSMASK_CAPTURE
		{JSMASK_MIC, MicButtonKeyName},
		{JSMASK_SL, SideLeftButtonKeyName},
		{JSMASK_SR, SideRightButtonKeyName},
		{JSMASK_FNL, FunctionLeftButtonKeyName},
		{JSMASK_FNR, FunctionRightButtonKeyName},

		// Switch 2 Pro Controller
		{JSMASK_C, CButtonKeyName},
		{JSMASK_GL, GripLeftButtonKeyName},
		{JSMASK_GR, GripRightButtonKeyName}
	};

	TStaticArray<double, MAX_NUM_CONTROLLER_BUTTONS> NextRepeatTimes{InPlace, 0.0};

	FCriticalSection SimpleStateLock;
	FCriticalSection TouchStateLock;
}; 
