#include "KawaiiPhysicsIKCorrectionMath.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "KawaiiClothingProxyMotion.h"
#include "KawaiiPhysicsBakerProfiling.h"

namespace KawaiiPhysicsIKCorrection
{
	bool AppendProxyCapsules(const TArray<FKawaiiClothingProxyCapsule>& Proxies, TArray<FCapsule>& OutCapsules, FName TargetRootBone) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_AppendProxyCapsules);
		if (Proxies.Num() > 64 || OutCapsules.Num() + Proxies.Num() > 128) {
			return false;
		}
		// Validate ignored proxies too, so filtering cannot hide malformed input or append a partial batch.
		for (const FKawaiiClothingProxyCapsule& Proxy : Proxies) {
			if (Proxy.Start.ContainsNaN() || Proxy.End.ContainsNaN() || !FMath::IsFinite(Proxy.Radius) || Proxy.Radius <= 0.f
				|| Proxy.IgnoredTargetRootBones.Num() > KawaiiClothingProxy::MaxIgnoredTargetRoots || Proxy.IgnoredTargetRootBones.Contains(NAME_None))
			{
				return false;
			}
		}
		OutCapsules.Reserve(OutCapsules.Num() + Proxies.Num());
		for (const FKawaiiClothingProxyCapsule& Proxy : Proxies) {
			if (!TargetRootBone.IsNone() && Proxy.IgnoredTargetRootBones.Contains(TargetRootBone)) {
				continue;
			}
			OutCapsules.Add({Proxy.Start, Proxy.End, Proxy.Radius});
		}
		return true;
	}

	namespace
	{
		constexpr double DirectionTolerance = 1.e-8;
		/** A previous point closer than 0.001 cm to the capsule axis carries no usable side. */
		constexpr double SideTolerance = 1.e-6;

		bool IsFinite(const FVector& Value) {
			return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
		}

		FVector ClosestOnSegment(const FVector& Point, const FVector& Start, const FVector& End) {
			const FVector Edge = End - Start;
			const double LengthSquared = Edge.SizeSquared();
			const double T = LengthSquared > DirectionTolerance ? FMath::Clamp(FVector::DotProduct(Point - Start, Edge) / LengthSquared, 0.0, 1.0) : 0.0;
			return Start + T * Edge;
		}

		FVector Perpendicular(const FVector& Axis) {
			const FVector N = Axis.GetSafeNormal(DirectionTolerance);
			if (N.IsNearlyZero()) {
				return FVector::YAxisVector;
			}
			const FVector Abs = N.GetAbs();
			const FVector Seed = Abs.X <= Abs.Y && Abs.X <= Abs.Z ? FVector::XAxisVector : (Abs.Y <= Abs.Z ? FVector::YAxisVector : FVector::ZAxisVector);
			return FVector::CrossProduct(N, Seed).GetSafeNormal(DirectionTolerance);
		}

		FVector ContactNormal(const FVector& Point, const FVector& OnCapsule, const FVector& ReferencePoint, const FCapsule& Capsule, const FVector& ClothSegment,
							  const FVector* SidePoint) {
			const FVector Delta = Point - OnCapsule;
			// Keep the side of the capsule this point occupied in the previous solution. Without it a strand
			// that the input animation carries across the capsule axis is pushed out of the opposite surface,
			// and the result flips between frames.
			if (SidePoint) {
				const FVector SideDelta = *SidePoint - ClosestOnSegment(*SidePoint, Capsule.Start, Capsule.End);
				if (SideDelta.SizeSquared() > SideTolerance
					&& (Delta.SizeSquared() <= DirectionTolerance || FVector::DotProduct(Delta, SideDelta) < 0.0))
				{
					return SideDelta.GetSafeNormal(DirectionTolerance);
				}
			}
			if (Delta.SizeSquared() > DirectionTolerance) {
				return Delta.GetSafeNormal(DirectionTolerance);
			}

			const FVector Axis = Capsule.End - Capsule.Start;
			const FVector ReferenceDelta = ReferencePoint - ClosestOnSegment(ReferencePoint, Capsule.Start, Capsule.End);
			// At an exact crossing, move perpendicular to both segments so that the
			// closest point does not merely slide along either segment's centerline.
			FVector Normal = FVector::CrossProduct(ClothSegment, Axis).GetSafeNormal(DirectionTolerance);
			if (!Normal.IsNearlyZero()) {
				if (FVector::DotProduct(Normal, ReferenceDelta) < 0.0) {
					Normal *= -1.0;
				}
				return Normal;
			}
			if (ReferenceDelta.SizeSquared() > DirectionTolerance) {
				Normal = ReferenceDelta.GetSafeNormal(DirectionTolerance);
				if (!ClothSegment.IsNearlyZero()) {
					const FVector ClothDirection = ClothSegment.GetSafeNormal(DirectionTolerance);
					Normal -= ClothDirection * FVector::DotProduct(Normal, ClothDirection);
				}
				Normal.Normalize(DirectionTolerance);
				if (!Normal.IsNearlyZero()) {
					return Normal;
				}
			}
			return Perpendicular(Axis.SizeSquared() > DirectionTolerance ? Axis : ClothSegment);
		}

		void ProjectLengths(TArray<FVector>& Points, const TArray<double>& Lengths, const TArray<FVector>& Reference, bool bReverse) {
			for (int32 Step = 0; Step < Lengths.Num(); ++Step) {
				const int32 A = bReverse ? Lengths.Num() - Step - 1 : Step;
				const int32 B = A + 1;
				const double WeightA = A == 0 ? 0.0 : 1.0;
				const FVector Delta = Points[B] - Points[A];
				const double Length = Delta.Size();
				FVector Direction = Length > DirectionTolerance ? Delta / Length : (Reference[B] - Reference[A]).GetSafeNormal(DirectionTolerance);
				if (Direction.IsNearlyZero()) {
					Direction = FVector::XAxisVector;
				}
				const FVector Correction = Direction * ((Length - Lengths[A]) / (WeightA + 1.0));
				Points[A] += WeightA * Correction;
				Points[B] -= Correction;
			}
		}

		void ProjectPoints(TArray<FVector>& Points, const TArray<FVector>& Reference, const TArray<FCapsule>& Capsules, double ParticleRadius,
						   const TArray<FVector>* SidePoints) {
			for (int32 Point = 1; Point < Points.Num(); ++Point) {
				for (const FCapsule& Capsule : Capsules) {
					const double Radius = Capsule.Radius + ParticleRadius;
					const FVector OnCapsule = ClosestOnSegment(Points[Point], Capsule.Start, Capsule.End);
					const double Distance = FVector::Distance(Points[Point], OnCapsule);
					if (Distance < Radius) {
						Points[Point] += ContactNormal(Points[Point], OnCapsule, Reference[Point], Capsule, FVector::ZeroVector,
													   SidePoints ? &(*SidePoints)[Point] : nullptr) * (Radius - Distance);
					}
				}
			}
		}

		void ProjectSegments(TArray<FVector>& Points, const TArray<FVector>& Reference, const TArray<FCapsule>& Capsules, double ParticleRadius,
							 const TArray<FVector>* SidePoints) {
			for (int32 A = 0; A + 1 < Points.Num(); ++A) {
				const int32 B = A + 1;
				for (const FCapsule& Capsule : Capsules) {
					FVector OnCloth, OnCapsule;
					FMath::SegmentDistToSegmentSafe(Points[A], Points[B], Capsule.Start, Capsule.End, OnCloth, OnCapsule);
					const double Radius = Capsule.Radius + ParticleRadius;
					const double Distance = FVector::Distance(OnCloth, OnCapsule);
					if (Distance >= Radius) {
						continue;
					}

					const FVector Edge = Points[B] - Points[A];
					const double EdgeSquared = Edge.SizeSquared();
					const double T = EdgeSquared > DirectionTolerance ? FMath::Clamp(FVector::DotProduct(OnCloth - Points[A], Edge) / EdgeSquared, 0.0, 1.0) : 0.0;
					const double InverseMassA = A == 0 ? 0.0 : 1.0;
					const double WeightA = 1.0 - T;
					const double WeightB = T;
					const double Denominator = InverseMassA * WeightA * WeightA + WeightB * WeightB;
					// Contact exactly at a pinned root cannot be resolved by moving
					// another joint. Leave it to the diagnostics instead of exploding.
					if (Denominator <= DirectionTolerance) {
						continue;
					}
					FVector SidePoint;
					if (SidePoints) {
						SidePoint = FMath::Lerp((*SidePoints)[A], (*SidePoints)[B], T);
					}
					const FVector Normal = ContactNormal(OnCloth, OnCapsule, FMath::Lerp(Reference[A], Reference[B], T), Capsule, Edge,
														 SidePoints ? &SidePoint : nullptr);
					const FVector Correction = Normal * ((Radius - Distance) / Denominator);
					// A contact arbitrarily close to a pinned root has a tiny
					// barycentric mass. Bound each projection instead of producing
					// a very large tip displacement for an effectively fixed contact.
					const double MaxProjection = 2.0 * Radius;
					Points[A] += (InverseMassA * WeightA * Correction).GetClampedToMaxSize(MaxProjection);
					Points[B] += (WeightB * Correction).GetClampedToMaxSize(MaxProjection);
				}
			}
		}

		FDiagnostics Measure(const TArray<FVector>& Points, const TArray<double>& Lengths, const TArray<FCapsule>& Capsules, double ParticleRadius) {
			TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_MeasureResiduals);
			FDiagnostics Result;
			for (const FVector& Point : Points) {
				if (!IsFinite(Point)) {
					Result.bFinite = false;
					return Result;
				}
				for (const FCapsule& Capsule : Capsules) {
					const double Distance = FVector::Distance(Point, ClosestOnSegment(Point, Capsule.Start, Capsule.End));
					Result.MaxPenetration = FMath::Max(Result.MaxPenetration, Capsule.Radius + ParticleRadius - Distance);
				}
			}
			for (int32 A = 0; A < Lengths.Num(); ++A) {
				Result.MaxLengthError = FMath::Max(Result.MaxLengthError, FMath::Abs(FVector::Distance(Points[A], Points[A + 1]) - Lengths[A]));
				for (const FCapsule& Capsule : Capsules) {
					FVector OnCloth, OnCapsule;
					FMath::SegmentDistToSegmentSafe(Points[A], Points[A + 1], Capsule.Start, Capsule.End, OnCloth, OnCapsule);
					Result.MaxPenetration = FMath::Max(Result.MaxPenetration, Capsule.Radius + ParticleRadius - FVector::Distance(OnCloth, OnCapsule));
				}
			}
			Result.bFinite = FMath::IsFinite(Result.MaxLengthError) && FMath::IsFinite(Result.MaxPenetration);
			return Result;
		}
	}

	FTransform WarpTransform(const FTransform& Bone, const FTransform& BaseAnchor, const FTransform& CurrentAnchor, const FTransform& BaseDriver, const FTransform& CurrentDriver, float Weight) {
		const FTransform Anchored = Bone.GetRelativeTransform(BaseAnchor) * CurrentAnchor;
		const FTransform Driven = Bone.GetRelativeTransform(BaseDriver) * CurrentDriver;
		FTransform Result;
		Result.Blend(Anchored, Driven, FMath::IsFinite(Weight) ? FMath::Clamp(Weight, 0.f, 1.f) : 0.f);
		Result.NormalizeRotation();
		return Result;
	}

	FDiagnostics
	SolveChain(TArray<FVector>& Points, const TArray<double>& RestLengths, const TArray<FVector>& ReferencePoints, const TArray<FCapsule>& Capsules, int32 Iterations, double ParticleRadius,
			   const TArray<FVector>* SidePoints) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_SolveChain);
		SCOPE_CYCLE_COUNTER(STAT_KawaiiBakerSolveChain);
		FDiagnostics Invalid;
		Invalid.bFinite = false;
		if (Points.IsEmpty()
			|| RestLengths.Num() != Points.Num() - 1 || ReferencePoints.Num() != Points.Num() || !FMath::IsFinite(ParticleRadius) || ParticleRadius < 0.0)
		{
			return Invalid;
		}
		for (int32 Index = 0; Index < Points.Num(); ++Index) {
			if (!IsFinite(Points[Index]) || !IsFinite(ReferencePoints[Index])) {
				return Invalid;
			}
		}
		for (const double Length : RestLengths) {
			if (!FMath::IsFinite(Length) || Length < 0.0) {
				return Invalid;
			}
		}
		for (const FCapsule& Capsule : Capsules) {
			if (!IsFinite(Capsule.Start)
				|| !IsFinite(Capsule.End) || !FMath::IsFinite(Capsule.Radius) || Capsule.Radius < 0.0)
			{
				return Invalid;
			}
		}

		// Side history is optional guidance; a mismatched or non-finite history is ignored rather than rejected.
		if (SidePoints) {
			bool bUsable = SidePoints->Num() == Points.Num();
			for (int32 Index = 0; bUsable && Index < SidePoints->Num(); ++Index) {
				bUsable = IsFinite((*SidePoints)[Index]);
			}
			SidePoints = bUsable ? SidePoints : nullptr;
		}

		const TArray<FVector> OriginalPoints = Points;
		const FVector Root = Points[0];
		const int32 NumIterations = FMath::Clamp(Iterations, 1, 64);
		const bool bDetailedTrace = KawaiiPhysicsBakerProfiling::IsDetailedSolverTracingEnabled();
		INC_DWORD_STAT(STAT_KawaiiBakerSolverCalls);
		INC_DWORD_STAT_BY(STAT_KawaiiBakerSolverPoints, Points.Num());
		for (int32 Iteration = 0; Iteration < NumIterations; ++Iteration) {
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(KawaiiBaker_Correction_LengthConstraints, bDetailedTrace);
				ProjectLengths(Points, RestLengths, ReferencePoints, false);
				ProjectLengths(Points, RestLengths, ReferencePoints, true);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(KawaiiBaker_Correction_PointContacts, bDetailedTrace);
				ProjectPoints(Points, ReferencePoints, Capsules, ParticleRadius, SidePoints);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(KawaiiBaker_Correction_SegmentContacts, bDetailedTrace);
				ProjectSegments(Points, ReferencePoints, Capsules, ParticleRadius, SidePoints);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(KawaiiBaker_Correction_FinalPointContacts, bDetailedTrace);
				ProjectPoints(Points, ReferencePoints, Capsules, ParticleRadius, SidePoints);
			}
			Points[0] = Root;
			bool bFinite = true;
			for (const FVector& Point : Points) {
				bFinite &= IsFinite(Point);
			}
			if (!bFinite) {
				INC_DWORD_STAT_BY(STAT_KawaiiBakerSolverIterations, Iteration + 1);
				Points = OriginalPoints;
				return Invalid;
			}
		}
		INC_DWORD_STAT_BY(STAT_KawaiiBakerSolverIterations, NumIterations);
		FDiagnostics Result = Measure(Points, RestLengths, Capsules, ParticleRadius);
		if (!Result.bFinite) {
			Points = OriginalPoints;
		}
		return Result;
	}
}
