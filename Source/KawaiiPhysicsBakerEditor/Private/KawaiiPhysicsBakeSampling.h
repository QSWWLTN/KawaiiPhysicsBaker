#pragma once

#include "CoreMinimal.h"

namespace KawaiiPhysicsBakeSampling {
	/** Full duration is covered at a fixed integer rate; the last key samples the exact source end.
	 * Non-integral source durations therefore have less than one sample of end-pose padding. */
	inline int32 NumIntervals(double Length, int32 Rate) {
		return FMath::Max(1, FMath::CeilToInt(Length * Rate - 1.e-5));
	}

	inline float SourceTime(int32 Index, int32 Intervals, double Length, int32 Rate) {
		return Index >= Intervals ? static_cast<float>(Length) : static_cast<float>(FMath::Min(Length, double(Index) / Rate));
	}

	inline bool IsValidTransform(const FTransform& Transform) {
		const FVector Scale = Transform.GetScale3D();
		return !Transform.ContainsNaN() && Transform.GetRotation().IsNormalized() && Scale.X > SMALL_NUMBER && Scale.Y > SMALL_NUMBER && Scale.Z > SMALL_NUMBER;
	}
}
