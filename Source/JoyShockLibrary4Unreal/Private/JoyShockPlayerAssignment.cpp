// JoyShockPlayerAssignment.cpp - Which controller belongs to which player.
//
// The other half of FJoyShockInterface. Where JoyShockInterface.cpp turns hardware state into input
// events, this decides whose events they are: allocating player slots as controllers connect and leave,
// joining two Joy-Cons into one player and splitting them again, following a Joy-Con in and out of its
// grip, and answering the identity questions the Blueprint layer asks (which handle is this connection,
// what is this controller).
//
// It is one class across two files because the state it works on -- ControllerStateByDeviceHandle and the
// join groups -- is the same state the input path reads. What differs is the question being asked.

#include "JoyShockInterface.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreDelegates.h"
// #include "Windows/WindowsApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "GenericPlatform/InputDeviceRegistry.h"
#include "JoyShockBlueprintLibrary.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/IConsoleManager.h"
#include <functional>
#include <mutex>
#include <shared_mutex>

#include "JoyShockLibrary4Unreal.h"
#include "JoyShockInterfaceInternal.h"

// Which half of a Joy-Con pair a hardware identifier names, across both generations. Everything about
// joining is the same for a Joy-Con 2 -- a pair separates on SL+SR and joins on an outer shoulder, each
// half fills only its own buttons and its own stick -- so the generation is not something any of the code
// below should have to know. Kept as identifier tests rather than as a flag on the state because the
// identifier is what the engine already carries per device.
bool IsJoyConLeftIdentifier(const FName& Identifier)
{
	return Identifier == TEXT("JoyConLeft") || Identifier == TEXT("JoyCon2Left");
}
bool IsJoyConRightIdentifier(const FName& Identifier)
{
	return Identifier == TEXT("JoyConRight") || Identifier == TEXT("JoyCon2Right");
}
// The two Sony pads, which share a report layout and therefore a stick convention. Kept as one test
// because every place that cares is a place where they behave identically -- naming only the DualShock 4
// is what left the DualSense's right stick inverted against every other controller for as long as the
// convention has existed. Their parser branches invert the raw Y byte the same way; if a third Sony pad
// ever arrives parsed by that same code, it belongs here too.
bool IsSonyIdentifier(const FName& Identifier)
{
	return Identifier == TEXT("DualShock4") || Identifier == TEXT("DualSense");
}
FJoyShockInterface::FJoyConPairingChange FJoyShockInterface::MakeJoyConPairingChange(
	int32 HandleA, int32 HandleB, bool bJoined) const
{
	const FControllerState* StateA = ControllerStateByDeviceHandle.Find(HandleA);
	const bool bAIsLeft = StateA != nullptr && IsJoyConLeftIdentifier(StateA->HardwareDeviceIdentifier);
	return { bAIsLeft ? HandleA : HandleB, bAIsLeft ? HandleB : HandleA, bJoined };
}
void FJoyShockInterface::BroadcastJoyConPairingChanges(const TArray<FJoyConPairingChange>& PairingChanges)
{
	if (PairingChanges.IsEmpty() || !FJoyShockLibrary4UnrealModule::IsAvailable())
	{
		return;
	}

	FJSL4UJoyConPairingChangedEvent& Event =
		FJoyShockLibrary4UnrealModule::GetInstance().GetOnJoyConPairingChanged();
	for (const FJoyConPairingChange& Change : PairingChanges)
	{
		UE_LOG(LogJoyShockLibrary, Verbose,
			TEXT("Broadcasting Joy-Con %s: left device %d, right device %d."),
			Change.bJoined ? TEXT("join") : TEXT("separation"),
			Change.LeftDeviceId, Change.RightDeviceId);
		Event.Broadcast(Change.LeftDeviceId, Change.RightDeviceId, Change.bJoined);
	}
}
void FJoyShockInterface::UpdateJoyConGripTransitions(TArray<FJoyConPairingChange>& OutPairingChanges)
{
	// Caller holds ControllerContainerLock. Poll callbacks take that lock before SimpleStateLock too, so
	// this preserves the existing lock order while taking one coherent snapshot of every Joy-Con chord.
	FScopeLock StateLock(&SimpleStateLock);
	bool bAssignmentChanged = false;

	for (TTuple<int32, FControllerState>& Pair : ControllerStateByDeviceHandle)
	{
		FControllerState& State = Pair.Value;
		if (State.SuppressedGripButtons != 0
			&& ((State.SimpleState.buttons | State.PreviousSimpleState.buttons) & State.SuppressedGripButtons) == 0)
		{
			State.SuppressedGripButtons = 0;
		}
	}

	// SL+SR on either half means "this is a solo horizontal controller". If it belonged to a joined pair,
	// both halves become standalone/horizontal again; only the initiating half's chord is suppressed.
	TArray<int32> SplitRequests;
	for (int32 Handle : DeviceHandles)
	{
		FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
		if (State == nullptr || !State->bIsConnected || !JoinPartner.Contains(Handle))
		{
			continue;
		}
		const bool bIsJoyCon = IsJoyConLeftIdentifier(State->HardwareDeviceIdentifier)
			|| IsJoyConRightIdentifier(State->HardwareDeviceIdentifier);
		if (bIsJoyCon && (State->SimpleState.buttons & (JSMASK_SL | JSMASK_SR)) == (JSMASK_SL | JSMASK_SR))
		{
			SplitRequests.AddUnique(Handle);
		}
	}

	for (int32 Handle : SplitRequests)
	{
		if (FControllerState* RequestState = ControllerStateByDeviceHandle.Find(Handle))
		{
			RequestState->bJoyConHorizontal = true;
			RequestState->SuppressedGripButtons |= JSMASK_SL | JSMASK_SR;
		}
		const int32* PartnerPtr = JoinPartner.Find(Handle);
		if (PartnerPtr == nullptr)
		{
			continue;
		}
		const int32 Partner = *PartnerPtr;
		JoinPartner.Remove(Handle);
		JoinPartner.Remove(Partner);
		if (FControllerState* PartnerState = ControllerStateByDeviceHandle.Find(Partner))
		{
			PartnerState->bJoyConHorizontal = true;
		}
		OutPairingChanges.Add(MakeJoyConPairingChange(Handle, Partner, false));
		UE_LOG(LogJoyShockLibrary, Log,
			TEXT("Joy-Con grip: SL+SR separated devices %d and %d into horizontal controllers."), Handle, Partner);
		bAssignmentChanged = true;
	}

	// An outer shoulder/trigger on a separated left half plus an outer shoulder/trigger on a separated
	// right half form one vertical pair. In other words, L or ZL may be combined with R or ZR. Sorting
	// makes simultaneous multi-pair registration deterministic without depending on map order.
	constexpr int32 LeftJoinButtons = JSMASK_L | JSMASK_ZL;
	constexpr int32 RightJoinButtons = JSMASK_R | JSMASK_ZR;
	TArray<int32> LeftCandidates;
	TArray<int32> RightCandidates;
	for (int32 Handle : DeviceHandles)
	{
		FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
		if (State == nullptr || !State->bIsConnected || JoinPartner.Contains(Handle))
		{
			continue;
		}
		if (IsJoyConLeftIdentifier(State->HardwareDeviceIdentifier)
			&& (State->SimpleState.buttons & LeftJoinButtons) != 0)
		{
			LeftCandidates.Add(Handle);
		}
		else if (IsJoyConRightIdentifier(State->HardwareDeviceIdentifier)
			&& (State->SimpleState.buttons & RightJoinButtons) != 0)
		{
			RightCandidates.Add(Handle);
		}
	}
	LeftCandidates.Sort();
	RightCandidates.Sort();

	const int32 PairCount = FMath::Min(LeftCandidates.Num(), RightCandidates.Num());
	for (int32 PairIndex = 0; PairIndex < PairCount; ++PairIndex)
	{
		const int32 LeftHandle = LeftCandidates[PairIndex];
		const int32 RightHandle = RightCandidates[PairIndex];
		JoinPartner.Add(LeftHandle, RightHandle);
		JoinPartner.Add(RightHandle, LeftHandle);

		FControllerState& LeftState = ControllerStateByDeviceHandle.FindChecked(LeftHandle);
		FControllerState& RightState = ControllerStateByDeviceHandle.FindChecked(RightHandle);
		LeftState.bJoyConHorizontal = false;
		RightState.bJoyConHorizontal = false;
		LeftState.SuppressedGripButtons |= LeftJoinButtons;
		RightState.SuppressedGripButtons |= RightJoinButtons;
		OutPairingChanges.Add({ LeftHandle, RightHandle, true });
		UE_LOG(LogJoyShockLibrary, Log,
			TEXT("Joy-Con grip: L/ZL + R/ZR joined devices %d and %d as one vertical controller."),
			LeftHandle, RightHandle);
		bAssignmentChanged = true;
	}

	if (bAssignmentChanged)
	{
		RefreshPlayerAssignments();
	}
}
bool FJoyShockInterface::IsOwnInputDevice(FInputDeviceId InInputDevice) const
{
	for (const TTuple<int32, FControllerState>& Pair : ControllerStateByDeviceHandle)
	{
		if (Pair.Value.InputDevice == InInputDevice)
		{
			return true;
		}
	}
	return false;
}
bool FJoyShockInterface::IsPlayerSlotClaimedByAnotherDevice(int32 Slot) const
{
	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();

	const FPlatformUserId SlotUser = DeviceMapper.GetPlatformUserForUserIndex(Slot);
	if (!SlotUser.IsValid())
	{
		return false;
	}

	TArray<FInputDeviceId> DevicesOnSlot;
	DeviceMapper.GetAllConnectedInputDevicesForUser(SlotUser, DevicesOnSlot);

	const FInputDeviceId DefaultDevice = DeviceMapper.GetDefaultInputDevice();

	for (const FInputDeviceId& Device : DevicesOnSlot)
	{
		// The keyboard and mouse live on the first player and never stop a controller from going there --
		// otherwise no controller could ever be player 1.
		if (Device == DefaultDevice)
		{
			continue;
		}
		// Our own controllers are already accounted for by PlayerSlotByPrimary, and counting them here too
		// would push every controller after the first one slot further out than it should be.
		if (IsOwnInputDevice(Device))
		{
			continue;
		}

		// Says which foreign device is holding the slot we are stepping over. Player numbers that skip are
		// almost always this and nothing else, and without the device id there is no way to tell "an XInput
		// pad is legitimately on player 1" from "something claimed player 1 and never let go" -- which is
		// what a wireless pad that flaps once while pairing can leave behind.
		UE_LOG(LogJoyShockLibrary, Verbose,
			TEXT("Player slot %d is already held by input device %d, which this plugin does not own; skipping it."),
			Slot, Device.GetId());
		return true;
	}

	return false;
}
int32 FJoyShockInterface::GetGroupPrimary(int32 Handle) const
{
	// The primary of a logical controller is the lower of the two joined handles (when both are connected),
	// otherwise the handle itself.
	if (const int32* Partner = JoinPartner.Find(Handle))
	{
		const FControllerState* PartnerState = ControllerStateByDeviceHandle.Find(*Partner);
		if (PartnerState != nullptr && PartnerState->bIsConnected)
		{
			return FMath::Min(Handle, *Partner);
		}
	}
	return Handle;
}
void FJoyShockInterface::RefreshPlayerAssignments()
{
	// Runs on the game thread with ControllerContainerLock held (via the connect/disconnect/join callers).
	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();

	// 1. Gather the primary handle of every connected logical controller (a standalone device or a pair).
	TArray<int32> GroupPrimaries;
	for (int32 Handle : DeviceHandles)
	{
		const FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
		if (State == nullptr || !State->bIsConnected)
		{
			continue;
		}
		GroupPrimaries.AddUnique(GetGroupPrimary(Handle));
	}

	// 2. Stable player slots: a controller keeps the same slot for its whole lifetime, so nobody's slot
	//    changes just because another controller connected or dropped. Release the slots of controllers
	//    that are gone, then give each newly-connected controller the lowest slot no one currently holds.
	//    This is what split-screen / party games (Overcooked-style) need: if player 0 drops mid-match,
	//    players 1 and 2 stay on their own characters instead of shuffling down onto each other's. The
	//    freed slot is left as a hole and reused by the next controller to connect. (The previous policy
	//    kept slots dense 0..N-1 but reassigned everyone on every disconnect, which is only ever right for
	//    a single grab-any-controller game and silently swaps characters for anything with more players.)

	// Release slots held by primaries that are no longer connected.
	for (auto It = PlayerSlotByPrimary.CreateIterator(); It; ++It)
	{
		if (!GroupPrimaries.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}

	// Assign the lowest free slot to each connected primary that doesn't have one yet, in handle order so
	// simultaneous connections are deterministic.
	TArray<int32> NewPrimaries;
	for (int32 Primary : GroupPrimaries)
	{
		if (!PlayerSlotByPrimary.Contains(Primary))
		{
			NewPrimaries.Add(Primary);
		}
	}
	NewPrimaries.Sort();

	for (int32 Primary : NewPrimaries)
	{
		int32 Slot = 0;
		for (bool bSlotTaken = true; bSlotTaken && Slot < MaxPlayerSlotSearch; )
		{
			bSlotTaken = false;
			for (const TTuple<int32, int32>& Pair : PlayerSlotByPrimary)
			{
				if (Pair.Value == Slot)
				{
					bSlotTaken = true;
					++Slot;
					break;
				}
			}

			// A slot is equally taken when someone else's controller is already on it -- an XInput pad, most
			// often. Without this the plugin only ever counted its own controllers, so the first JoyShock
			// controller claimed player 0 even with an Xbox pad already sitting there, and both drove the
			// same character. Skipping occupied slots is what makes a JoyShock controller land where a
			// second Xbox pad would have, so a game does not need one scheme for XInput and another for us.
			if (!bSlotTaken && IsPlayerSlotClaimedByAnotherDevice(Slot))
			{
				bSlotTaken = true;
				++Slot;
			}
		}
		PlayerSlotByPrimary.Add(Primary, Slot);
	}

	// 3. Map every connected device to its logical controller's player slot. Both halves of a joined pair
	//    map to the same platform user, so the engine sees one player for the pair.
	for (int32 Handle : DeviceHandles)
	{
		FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
		if (State == nullptr || !State->bIsConnected)
		{
			continue;
		}

		const int32 Slot = PlayerSlotByPrimary[GetGroupPrimary(Handle)];

		const FPlatformUserId SlotUser = DeviceMapper.GetPlatformUserForUserIndex(Slot);

		// Ask the mapper what it currently believes rather than trusting our own cache. The engine reassigns
		// devices behind our back: creating a local player runs the id through
		// RemapControllerIdToPlatformUserAndDevice, which synthesises an FInputDeviceId straight from the
		// controller id and claims it for the new player -- so "Create Player 1" quietly takes whichever of
		// our controllers happens to hold input device id 1. Routing follows the mapper, not the platform
		// user we pass with each event, so the stolen controller starts driving the wrong player while this
		// cache still says otherwise. Comparing against the cache meant we never noticed and never put it
		// back; comparing against the mapper repairs it on the next refresh.
		const FPlatformUserId MappedUser = DeviceMapper.GetUserForInputDevice(State->InputDevice);
		if (MappedUser != SlotUser)
		{
			UE_LOG(LogJoyShockLibrary, Verbose,
				TEXT("Device %d (input device %d) was mapped to platform user %d, restoring it to %d."),
				Handle, State->InputDevice.GetId(), MappedUser.GetInternalId(), SlotUser.GetInternalId());

			DeviceMapper.Internal_MapInputDeviceToUser(State->InputDevice, SlotUser, EInputDeviceConnectionState::Connected);
		}

		State->PlatformUser = SlotUser;

		// Keep the physical indicator tied to the same stable slot the engine routes this device to.
		// This covers hot-plugging, explicit reassignment and joined Joy-Con halves (which share Slot).
		// The setter only stores the semantic one-based number; each controller's polling thread performs
		// the family-specific output write, so no HID/WinUSB I/O blocks this game-thread refresh.
		UJoyShockLibrary::SetPlayerIndicatorForHandle(Handle, Slot + 1);
	}
}
bool FJoyShockInterface::JoinControllers(int32 HandleA, int32 HandleB)
{
	if (HandleA == HandleB)
	{
		return false;
	}

	TArray<FJoyConPairingChange> PairingChanges;
	{
		FScopeLock ContainerLock(&ControllerContainerLock);

		const FControllerState* StateA = ControllerStateByDeviceHandle.Find(HandleA);
		const FControllerState* StateB = ControllerStateByDeviceHandle.Find(HandleB);
		if (StateA == nullptr || !StateA->bIsConnected || StateB == nullptr || !StateB->bIsConnected)
		{
			return false;
		}

		if (JoinPartner.FindRef(HandleA) == HandleB && JoinPartner.FindRef(HandleB) == HandleA)
		{
			ControllerStateByDeviceHandle.FindChecked(HandleA).bJoyConHorizontal = false;
			ControllerStateByDeviceHandle.FindChecked(HandleB).bJoyConHorizontal = false;
			return true;
		}

		// Dissolve any old pair either requested half belonged to. These are real separation transitions
		// too, so expose them before the new join.
		auto DissolveJoinFor = [this, &PairingChanges](int32 Handle)
		{
			const int32* PartnerPtr = JoinPartner.Find(Handle);
			if (PartnerPtr == nullptr)
			{
				return;
			}

			const int32 Partner = *PartnerPtr;
			PairingChanges.Add(MakeJoyConPairingChange(Handle, Partner, false));
			JoinPartner.Remove(Handle);
			JoinPartner.Remove(Partner);
			if (FControllerState* State = ControllerStateByDeviceHandle.Find(Handle))
			{
				State->bJoyConHorizontal = true;
			}
			if (FControllerState* PartnerState = ControllerStateByDeviceHandle.Find(Partner))
			{
				PartnerState->bJoyConHorizontal = true;
			}
		};
		DissolveJoinFor(HandleA);
		DissolveJoinFor(HandleB);

		JoinPartner.Add(HandleA, HandleB);
		JoinPartner.Add(HandleB, HandleA);
		ControllerStateByDeviceHandle.FindChecked(HandleA).bJoyConHorizontal = false;
		ControllerStateByDeviceHandle.FindChecked(HandleB).bJoyConHorizontal = false;
		PairingChanges.Add(MakeJoyConPairingChange(HandleA, HandleB, true));

		RefreshPlayerAssignments();
	}
	BroadcastJoyConPairingChanges(PairingChanges);
	return true;
}
void FJoyShockInterface::UnjoinController(int32 Handle)
{
	TArray<FJoyConPairingChange> PairingChanges;
	{
		FScopeLock ContainerLock(&ControllerContainerLock);

		const int32* PartnerPtr = JoinPartner.Find(Handle);
		if (PartnerPtr == nullptr)
		{
			if (FControllerState* State = ControllerStateByDeviceHandle.Find(Handle))
			{
				if (IsJoyConLeftIdentifier(State->HardwareDeviceIdentifier)
					|| IsJoyConRightIdentifier(State->HardwareDeviceIdentifier))
				{
					State->bJoyConHorizontal = true;
				}
			}
			return;
		}

		const int32 Partner = *PartnerPtr;
		PairingChanges.Add(MakeJoyConPairingChange(Handle, Partner, false));
		JoinPartner.Remove(Partner);
		JoinPartner.Remove(Handle);
		ControllerStateByDeviceHandle.FindChecked(Handle).bJoyConHorizontal = true;
		ControllerStateByDeviceHandle.FindChecked(Partner).bJoyConHorizontal = true;

		RefreshPlayerAssignments();
	}
	BroadcastJoyConPairingChanges(PairingChanges);
}
void FJoyShockInterface::UnjoinAllControllers()
{
	TArray<FJoyConPairingChange> PairingChanges;
	{
		FScopeLock ContainerLock(&ControllerContainerLock);
		for (const TTuple<int32, int32>& Pair : JoinPartner)
		{
			if (Pair.Key < Pair.Value)
			{
				PairingChanges.Add(MakeJoyConPairingChange(Pair.Key, Pair.Value, false));
			}
		}
		JoinPartner.Empty();
		for (TTuple<int32, FControllerState>& Pair : ControllerStateByDeviceHandle)
		{
			if (IsJoyConLeftIdentifier(Pair.Value.HardwareDeviceIdentifier)
				|| IsJoyConRightIdentifier(Pair.Value.HardwareDeviceIdentifier))
			{
				Pair.Value.bJoyConHorizontal = true;
			}
		}
		RefreshPlayerAssignments();
	}
	BroadcastJoyConPairingChanges(PairingChanges);
}
int32 FJoyShockInterface::GetJoinPartner(int32 Handle) const
{
	FScopeLock ContainerLock(&ControllerContainerLock);
	const int32* Partner = JoinPartner.Find(Handle);
	return Partner != nullptr ? *Partner : INDEX_NONE;
}
bool FJoyShockInterface::IsJoinPrimary(int32 Handle) const
{
	FScopeLock ContainerLock(&ControllerContainerLock);
	return GetGroupPrimary(Handle) == Handle;
}
bool FJoyShockInterface::SetPlayerIndexForDevice(int32 Handle, int32 PlayerIndex)
{
	FScopeLock ContainerLock(&ControllerContainerLock);

	const FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
	if (State == nullptr || !State->bIsConnected)
	{
		return false;
	}

	// Keyed on the group primary so assigning either half of a joined Joy-Con pair moves the pair.
	const int32 Primary = GetGroupPrimary(Handle);

	if (PlayerIndex < 0)
	{
		// Back to automatic: dropping the entry makes RefreshPlayerAssignments treat this as a controller
		// without a slot, so it takes the lowest one nobody holds.
		PlayerSlotByPrimary.Remove(Primary);
	}
	else
	{
		// Deliberately no check for the slot already being taken. Assignment is the game's call, and two
		// controllers sharing a player is a legitimate setup (it is exactly what a joined Joy-Con pair is),
		// so silently swapping or refusing here would be second-guessing the caller.
		PlayerSlotByPrimary.Add(Primary, PlayerIndex);
	}

	RefreshPlayerAssignments();
	return true;
}
bool FJoyShockInterface::SetJoyConHorizontal(int32 Handle, bool bHorizontal)
{
	TArray<FJoyConPairingChange> PairingChanges;
	{
		FScopeLock ContainerLock(&ControllerContainerLock);
		FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
		if (State == nullptr || !State->bIsConnected
			|| (!IsJoyConLeftIdentifier(State->HardwareDeviceIdentifier)
				&& !IsJoyConRightIdentifier(State->HardwareDeviceIdentifier)))
		{
			return false;
		}

		if (bHorizontal)
		{
			const int32* PartnerPtr = JoinPartner.Find(Handle);
			if (PartnerPtr != nullptr)
			{
				const int32 Partner = *PartnerPtr;
				PairingChanges.Add(MakeJoyConPairingChange(Handle, Partner, false));
				JoinPartner.Remove(Partner);
				JoinPartner.Remove(Handle);
				if (FControllerState* PartnerState = ControllerStateByDeviceHandle.Find(Partner))
				{
					PartnerState->bJoyConHorizontal = true;
				}
				RefreshPlayerAssignments();
			}
		}
		State->bJoyConHorizontal = bHorizontal;
	}
	BroadcastJoyConPairingChanges(PairingChanges);
	return true;
}
bool FJoyShockInterface::IsJoyConHorizontal(int32 Handle) const
{
	FScopeLock ContainerLock(&ControllerContainerLock);
	const FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
	return State != nullptr && State->bIsConnected && State->bJoyConHorizontal;
}
bool FJoyShockInterface::GetJoyConGrip(int32 Handle, bool& bOutHorizontal, bool& bOutIsLeft) const
{
	bOutHorizontal = false;
	bOutIsLeft = false;

	FScopeLock ContainerLock(&ControllerContainerLock);
	const FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
	if (State == nullptr || !State->bIsConnected)
	{
		return false;
	}

	const bool bIsLeft = IsJoyConLeftIdentifier(State->HardwareDeviceIdentifier);
	if (!bIsLeft && !IsJoyConRightIdentifier(State->HardwareDeviceIdentifier))
	{
		return false;
	}

	bOutHorizontal = State->bJoyConHorizontal;
	bOutIsLeft = bIsLeft;
	return true;
}
int32 FJoyShockInterface::GetHandleForConnection(int64 ConnectionId) const
{
	// Zero is the "no identity" value of the field and negative ids belong to pads Unreal drives, so neither
	// can name a controller here. Rejecting them before taking the lock also means the per-frame nodes of a
	// game holding a foreign pad never contend for it.
	if (ConnectionId <= 0)
	{
		return INDEX_NONE;
	}

	FScopeLock ContainerLock(&ControllerContainerLock);
	for (const TTuple<int32, FControllerState>& Pair : ControllerStateByDeviceHandle)
	{
		if (Pair.Value.ConnectionId == ConnectionId && Pair.Value.bIsConnected)
		{
			return Pair.Key;
		}
	}
	// A connection that has ended. The caller treats this exactly as it treats a foreign pad: there is no
	// controller of ours behind this id, which is the answer that keeps a stale id from driving the
	// controller that inherited its handle.
	return INDEX_NONE;
}
int64 FJoyShockInterface::GetConnectionForHandle(int32 Handle) const
{
	FScopeLock ContainerLock(&ControllerContainerLock);
	const FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
	return State != nullptr && State->bIsConnected ? State->ConnectionId : 0;
}
bool FJoyShockInterface::FillControllerInfo(FJSL4UControllerInfo& Info, int32 Handle) const
{
	FScopeLock ContainerLock(&ControllerContainerLock);
	const FControllerState* State = ControllerStateByDeviceHandle.Find(Handle);
	if (State == nullptr || !State->bIsConnected)
	{
		return false;
	}

	const int32* Slot = PlayerSlotByPrimary.Find(GetGroupPrimary(Handle));
	Info.PlayerIndex = Slot != nullptr ? *Slot : INDEX_NONE;

	// The partner is named the same way everything else is: by its connection id, read here while the
	// container lock is already held rather than left to a second lookup that could see a different pairing.
	Info.JoinedToConnectionId = 0;
	if (const int32* Partner = JoinPartner.Find(Handle))
	{
		if (const FControllerState* PartnerState = ControllerStateByDeviceHandle.Find(*Partner))
		{
			Info.JoinedToConnectionId = PartnerState->ConnectionId;
		}
	}
	Info.ConnectionId = State->ConnectionId;
	Info.InputDeviceId = State->InputDevice.GetId();
	Info.PlatformUserId = State->PlatformUser.GetInternalId();
	Info.HardwareDeviceIdentifier = State->HardwareDeviceIdentifier;
	if (IsJoyConLeftIdentifier(State->HardwareDeviceIdentifier)
		|| IsJoyConRightIdentifier(State->HardwareDeviceIdentifier))
	{
		Info.JoyConGripMode = State->bJoyConHorizontal
			? EJSL4UJoyConGripMode::Horizontal
			: EJSL4UJoyConGripMode::Vertical;
	}
	else
	{
		Info.JoyConGripMode = EJSL4UJoyConGripMode::NotApplicable;
	}
	return true;
}
