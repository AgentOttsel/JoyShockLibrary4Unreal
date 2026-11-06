#pragma once

#include "CoreMinimal.h"

// Turns gyro samples, which arrive on the controller's schedule, into one angular velocity per game frame.
//
// The two schedules rarely match. A Pro Controller 2 over Bluetooth sends 80-90 reports a second, fewer than a
// game draws frames, while a DualShock 4 on the cable sends 500. Taking the latest sample each frame got both
// wrong: below the frame rate it repeated a stale velocity for as long as the controller stayed quiet, so a
// link that hiccupped mid-turn kept the camera turning by itself; above it, it threw most samples away, and a
// flick between two frames went missing along with the averaging the extra samples would have given.
//
// So each sample is kept as a stretch of time -- the time since the sample before it, the same delta GyroWiki
// integrates with -- and each frame plays back exactly the stretch of time it covers, at the velocities that
// were measured there. When the samples run out mid-frame, the last velocity is held, for at most
// MaxHoldSeconds, and that held time is owed: the next samples to arrive cover those same moments, so they pay
// it off instead of being played on top. Summed over frames, the turn equals the turn the controller measured,
// whatever either rate is, at the cost of up to one sample of latency.
//
// Not thread-safe on its own; the owner holds its lock around both calls.
struct FGyroTimeSlicer
{
	// How long a velocity may be repeated with no newer sample. Long enough to bridge the gap between two
	// Bluetooth reports at the slowest rate seen (about 70 a second); short enough that a controller gone
	// quiet stops turning the camera before anyone sees it drift.
	static constexpr float MaxHoldSeconds = 0.05f;
	// Samples not yet played are latency. Anything queued past this is played in the next frame at once,
	// rather than letting a burst push every later frame further behind.
	static constexpr float MaxQueuedSeconds = 0.05f;
	// A frame longer than this is not reconstructed: the hold would have expired long before its end anyway.
	static constexpr double MaxFrameSeconds = 0.25;
	// A second of samples at the fastest rate seen (500 a second on the cable), with room to spare.
	static constexpr int32 MaxSegments = 1024;

	// SampleSeconds is the time since the previous sample arrived.
	void Push(float SampleSeconds, float X, float Y, float Z)
	{
		if (SampleSeconds > MaxHoldSeconds)
		{
			// A gap longer than any hold: the held output already stopped, and what arrives now is new
			// motion, not the moments the hold was standing in for. Nothing is owed.
			Debt = 0.f;
		}
		// Nothing may be slicing at all (no frame reads this controller), so the queue is bounded here too.
		if (Segments.Num() >= MaxSegments)
		{
			Segments.RemoveAt(0, Segments.Num() - MaxSegments + 1, EAllowShrinking::No);
		}
		Segments.Add({ FMath::Clamp(SampleSeconds, 0.f, MaxHoldSeconds), X, Y, Z });
	}

	// Called once per frame with the current time in seconds. Returns the average angular velocity over the
	// time since the previous call.
	void Slice(double NowSeconds, float& OutX, float& OutY, float& OutZ)
	{
		// The first call, or the first after nobody sliced for a while (a hitch, a breakpoint, a reader that
		// stopped asking): what queued meanwhile is motion nobody saw, and playing it now would be a jump.
		// Start over from the newest velocity.
		if (LastSliceSeconds < 0.0 || NowSeconds - LastSliceSeconds > MaxFrameSeconds)
		{
			if (Segments.Num() > 0)
			{
				const FSegment& Newest = Segments.Last();
				LastX = Newest.X; LastY = Newest.Y; LastZ = Newest.Z;
				HoldX = Newest.X; HoldY = Newest.Y; HoldZ = Newest.Z;
				Segments.Reset();
			}
			Debt = 0.f;
			HeldSeconds = 0.f;
			LastSliceSeconds = NowSeconds;
			OutX = LastX; OutY = LastY; OutZ = LastZ;
			return;
		}
		const float FrameSeconds = static_cast<float>(FMath::Max(NowSeconds - LastSliceSeconds, 0.0));
		LastSliceSeconds = NowSeconds;
		if (FrameSeconds <= 0.f)
		{
			OutX = LastX; OutY = LastY; OutZ = LastZ;
			return;
		}

		float AngleX = 0.f, AngleY = 0.f, AngleZ = 0.f;
		int32 Index = 0;

		// Moments already played with a held velocity: the samples that cover them replace that guess
		// rather than being played a second time.
		while (Index < Segments.Num() && Debt > 0.f)
		{
			FSegment& Segment = Segments[Index];
			const float Take = FMath::Min(Debt, Segment.Remaining);
			Segment.Remaining -= Take;
			Debt -= Take;
			HoldX = Segment.X; HoldY = Segment.Y; HoldZ = Segment.Z;
			if (Segment.Remaining <= 0.f)
			{
				++Index;
			}
		}

		auto Play = [&](float Seconds)
		{
			while (Index < Segments.Num() && Seconds > 0.f)
			{
				FSegment& Segment = Segments[Index];
				const float Take = FMath::Min(Seconds, Segment.Remaining);
				AngleX += Segment.X * Take;
				AngleY += Segment.Y * Take;
				AngleZ += Segment.Z * Take;
				Segment.Remaining -= Take;
				Seconds -= Take;
				HoldX = Segment.X; HoldY = Segment.Y; HoldZ = Segment.Z;
				HeldSeconds = 0.f;
				if (Segment.Remaining <= 0.f)
				{
					++Index;
				}
			}
			return Seconds;
		};

		const float Unfilled = Play(FrameSeconds);

		float Queued = 0.f;
		for (int32 Rest = Index; Rest < Segments.Num(); ++Rest)
		{
			Queued += Segments[Rest].Remaining;
		}
		if (Queued > MaxQueuedSeconds)
		{
			Play(Queued - MaxQueuedSeconds);
		}

		if (Unfilled > 0.f)
		{
			const float Hold = FMath::Min(Unfilled, FMath::Max(0.f, MaxHoldSeconds - HeldSeconds));
			AngleX += HoldX * Hold;
			AngleY += HoldY * Hold;
			AngleZ += HoldZ * Hold;
			HeldSeconds += Hold;
			Debt += Hold;
		}

		Segments.RemoveAt(0, Index, EAllowShrinking::No);

		LastX = AngleX / FrameSeconds;
		LastY = AngleY / FrameSeconds;
		LastZ = AngleZ / FrameSeconds;
		OutX = LastX; OutY = LastY; OutZ = LastZ;
	}

private:
	struct FSegment
	{
		float Remaining;
		float X, Y, Z;
	};

	TArray<FSegment> Segments;
	float HoldX = 0.f, HoldY = 0.f, HoldZ = 0.f;
	// Held time output since a sample was last played.
	float HeldSeconds = 0.f;
	// Held time not yet covered by a sample.
	float Debt = 0.f;
	double LastSliceSeconds = -1.0;
	float LastX = 0.f, LastY = 0.f, LastZ = 0.f;
};
