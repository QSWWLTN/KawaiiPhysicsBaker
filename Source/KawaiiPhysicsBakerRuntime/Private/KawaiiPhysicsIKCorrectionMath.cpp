#include "KawaiiPhysicsIKCorrectionMath.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "KawaiiClothingProxyMotion.h"
#include "KawaiiPhysicsBakerProfiling.h"

namespace KawaiiPhysicsIKCorrection {
	namespace { FVector Perpendicular(const FVector& Axis); }
	void ProjectBridges(TArray<FVector>& Points, const TArray<FVector>& Reference, const TArray<FContactBridge>& Bridges) {
		// Match KawaiiPhysics bridge dummies: sample one pose, accumulate ALL contacting
		// dummies per real endpoint, then apply one weighted feedback. Updating each link
		// immediately makes shared endpoints depend on link order and over-corrects a grid.
		TArray<FVector> Pushes; Pushes.Init(FVector::ZeroVector, Points.Num());
		TArray<double> Weights; Weights.Init(0., Points.Num());
		for (const auto& Bridge : Bridges) {
			if (!Points.IsValidIndex(Bridge.A) || !Points.IsValidIndex(Bridge.B) || Reference.Num() != Points.Num() || Bridge.Samples < 1 || Bridge.Samples > 10
				|| !FMath::IsFinite(Bridge.Feedback) || Bridge.Feedback <= 0.) { continue; }
			// Overlapping endpoint spheres already cover this reference span.
			if (FVector::DistSquared(Reference[Bridge.A], Reference[Bridge.B]) <= FMath::Square(Bridge.RadiusA + Bridge.RadiusB)) { continue; }
			for (int32 Sample = 1; Sample <= Bridge.Samples; ++Sample) {
				const double T = double(Sample) / (Bridge.Samples + 1);
				const FVector Original = FMath::Lerp(Points[Bridge.A], Points[Bridge.B], T);
				FVector Point = Original;
				const double Radius = FMath::Lerp(Bridge.RadiusA, Bridge.RadiusB, T);
				for (const auto& Shape : Bridge.Shapes) {
					if (Shape.IsBox()) { Point += BoxPush(Point, Shape, Radius); continue; }
					const FVector OnShape = FMath::ClosestPointOnSegment(Point, Shape.Start, Shape.End);
					FVector Direction = Point - OnShape;
					const double Distance = Direction.Size(), Depth = Shape.Radius + Radius - Distance;
					if (Depth <= 0.) { continue; }
					if (Distance > 1.e-8) { Direction /= Distance; }
					else {
						const FVector Ref = FMath::Lerp(Reference[Bridge.A], Reference[Bridge.B], T);
						Direction = (Ref - FMath::ClosestPointOnSegment(Ref, Shape.Start, Shape.End)).GetSafeNormal();
						if (Direction.IsNearlyZero()) { Direction = Perpendicular(Shape.End - Shape.Start); }
					}
					Point += Direction * Depth;
				}
				const FVector Push = Point - Original;
				// Non-contacting dummies must not dilute an actual contact's feedback.
				if (Push.SizeSquared() <= 1.e-12) { continue; }
				if (!Bridge.FixedA) { Pushes[Bridge.A] += Push * ((1. - T) * Bridge.Feedback); Weights[Bridge.A] += 1. - T; }
				if (!Bridge.FixedB) { Pushes[Bridge.B] += Push * (T * Bridge.Feedback); Weights[Bridge.B] += T; }
			}
		}
		for (int32 I = 0; I < Points.Num(); ++I) { Points[I] += Pushes[I] / FMath::Max(1., Weights[I]); }
	}
	double SurfaceDistance(const FVector& Point, const FCapsule& Shape) {
		if (!Shape.IsBox()) { return FVector::Distance(Point, FMath::ClosestPointOnSegment(Point, Shape.Start, Shape.End)) - Shape.Radius; }
		const FVector D = Shape.BoxRotation.UnrotateVector(Point - Shape.Start).GetAbs() - Shape.BoxExtent;
		return D.ComponentMax(FVector::ZeroVector).Size() + FMath::Min(0., D.GetMax());
	}
	FVector BoxPush(const FVector& Point, const FCapsule& Box, double Radius) {
		const FVector Local = Box.BoxRotation.UnrotateVector(Point - Box.Start);
		const FVector Closest = Local.ComponentMin(Box.BoxExtent).ComponentMax(-Box.BoxExtent);
		const FVector Delta = Local - Closest;
		const double Length = Delta.Size();
		if (Length > 1.e-8) { return Box.BoxRotation.RotateVector(Delta / Length * FMath::Max(0., Radius - Length)); }
		const FVector Depth = Box.BoxExtent - Local.GetAbs();
		const int32 Axis = Depth.X <= Depth.Y + 1.e-4 && Depth.X <= Depth.Z + 1.e-4 ? 0 : Depth.Y <= Depth.Z + 1.e-4 ? 1 : 2;
		FVector Push = FVector::ZeroVector;
		Push[Axis] = (Local[Axis] >= 0. ? 1. : -1.) * (Depth[Axis] + Radius);
		return Box.BoxRotation.RotateVector(Push);
	}
	static double BoxSegmentTime(const FVector& A, const FVector& B, const FCapsule& Box, double RA, double RB) {
		const FVector LA = Box.BoxRotation.UnrotateVector(A - Box.Start), LB = Box.BoxRotation.UnrotateVector(B - Box.Start);
		const FVector Bounds = Box.BoxExtent + FVector(FMath::Max(RA, RB));
		const FVector Min = LA.ComponentMin(LB), Max = LA.ComponentMax(LB);
		if (Min.X > Bounds.X || Min.Y > Bounds.Y || Min.Z > Bounds.Z || Max.X < -Bounds.X || Max.Y < -Bounds.Y || Max.Z < -Bounds.Z) { return 0.; }
		const auto Distance = [&](double T) {
			const FVector D = FMath::Lerp(LA, LB, T).GetAbs() - Box.BoxExtent;
			return D.ComponentMax(FVector::ZeroVector).Size() + FMath::Min(0., D.GetMax()) - FMath::Lerp(RA, RB, T);
		};
		double Low = 0., High = 1.;
		for (int32 I = 0; I < 28; ++I) {
			const double L = (Low * 2. + High) / 3., R = (Low + High * 2.) / 3.;
			if (Distance(L) < Distance(R)) { High = R; } else { Low = L; }
		}
		return (Low + High) * .5;
	}
	static FVector ProjectBoxPoint(FVector P, const FCapsule& Box, double Radius, const FVector* Previous) {
		if (Previous && SurfaceDistance(*Previous, Box) >= Radius - 1.e-4) {
			const double T = BoxSegmentTime(*Previous, P, Box, Radius, Radius);
			if (SurfaceDistance(FMath::Lerp(*Previous, P, T), Box) < Radius - 1.e-5) {
				double Low = 0., High = T;
				for (int32 I = 0; I < 24; ++I) {
					const double M = (Low + High) * .5;
					if (SurfaceDistance(FMath::Lerp(*Previous, P, M), Box) < Radius) { High = M; } else { Low = M; }
				}
				const FVector Hit = FMath::Lerp(*Previous, P, High);
				const FVector N = BoxPush(Hit, Box, Radius + .001).GetSafeNormal();
				P += N * FMath::Max(0., FVector::DotProduct(Hit - P, N));
			}
		}
		return P + BoxPush(P, Box, Radius);
	}
	bool AppendProxyCapsules(const TArray<FKawaiiClothingProxyCapsule>& Proxies, TArray<FCapsule>& OutCapsules, FName TargetRootBone) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_AppendProxyCapsules);
		if (Proxies.Num() > 64 || OutCapsules.Num() + Proxies.Num() > 128) {
			return false;
		}
		// Validate ignored proxies too, so filtering cannot hide malformed input or append a partial batch.
		for (const FKawaiiClothingProxyCapsule& Proxy : Proxies) {
			if (Proxy.Start.ContainsNaN() || Proxy.End.ContainsNaN() || !FMath::IsFinite(Proxy.Radius) || Proxy.Radius <= 0.f
				|| Proxy.BoxExtent.ContainsNaN() || (!Proxy.BoxExtent.IsZero() && (Proxy.BoxExtent.GetMin() <= 0. || !Proxy.BoxRotation.IsNormalized()))
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
			OutCapsules.Add({Proxy.Start, Proxy.End, Proxy.Radius, Proxy.BoxExtent, Proxy.BoxRotation});
		}
		return true;
	}

	namespace {
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

		// Intersections with the union of the finite cylinder and its two endpoint spheres.
		// D need not be normalized: sweeps use t in [0,1], directed depenetration uses unit D.
		bool CapsuleRayRange(const FVector& O, const FVector& D, const FCapsule& C, double Radius, double& Enter, double& Exit) {
			Enter = TNumericLimits<double>::Max();
			Exit = -TNumericLimits<double>::Max();
			const double DD = D.SizeSquared();
			if (DD <= DirectionTolerance) { return false; }
			const auto AddRoot = [&](double T) { Enter = FMath::Min(Enter, T); Exit = FMath::Max(Exit, T); };
			for (const FVector Center : {C.Start, C.End}) {
				const FVector M = O - Center;
				const double B = FVector::DotProduct(M, D);
				const double Disc = B * B - DD * (M.SizeSquared() - Radius * Radius);
				if (Disc >= 0.) {
					const double Root = FMath::Sqrt(Disc);
					AddRoot((-B - Root) / DD);
					AddRoot((-B + Root) / DD);
				}
			}
			const FVector Edge = C.End - C.Start;
			const double Length = Edge.Size();
			if (Length > DirectionTolerance) {
				const FVector Axis = Edge / Length, M = O - C.Start;
				const double MD = FVector::DotProduct(M, Axis), ND = FVector::DotProduct(D, Axis);
				const FVector Q = M - Axis * MD, V = D - Axis * ND;
				const double A = V.SizeSquared(), B = FVector::DotProduct(Q, V);
				const double Disc = B * B - A * (Q.SizeSquared() - Radius * Radius);
				if (A > DirectionTolerance && Disc >= 0.) {
					const double Root = FMath::Sqrt(Disc);
					for (const double T : {(-B - Root) / A, (-B + Root) / A}) {
						const double Along = MD + ND * T;
						if (Along >= 0. && Along <= Length) { AddRoot(T); }
					}
				}
			}
			return Enter <= Exit;
		}

		FVector TransportSidePoint(const FVector& P, const FCapsule& Before, const FCapsule& Now) {
			if (Now.IsBox()) { return Now.Start + Now.BoxRotation.RotateVector(Before.BoxRotation.UnrotateVector(P - Before.Start)); }
			const FVector OldAxis = Before.End - Before.Start, NewAxis = Now.End - Now.Start;
			const double OldSquared = OldAxis.SizeSquared();
			const double T = OldSquared > DirectionTolerance ? FMath::Clamp(FVector::DotProduct(P - Before.Start, OldAxis) / OldSquared, 0., 1.) : 0.;
			const FQuat Rotation = OldSquared > DirectionTolerance && NewAxis.SizeSquared() > DirectionTolerance
				? FQuat::FindBetweenNormals(OldAxis.GetSafeNormal(), NewAxis.GetSafeNormal()) : FQuat::Identity;
			return Now.Start + T * NewAxis + Rotation.RotateVector(P - Before.Start - T * OldAxis);
		}

		double DirectedContactDistance(const FVector& Point, const FVector& Normal, const FCapsule& Capsule, double Radius) {
			const FVector Delta = Point - ClosestOnSegment(Point, Capsule.Start, Capsule.End);
			const double Distance = Delta.Size();
			if (Distance > DirectionTolerance && FVector::DotProduct(Delta, Normal) >= Distance * (1. - 1.e-6)) {
				return FMath::Max(0., Radius - Distance);
			}
			double Enter, Exit;
			// Radius - unsigned distance is only correct for the nearest outward normal. History may
			// choose the OTHER side, requiring travel through the interior before exiting the capsule.
			return CapsuleRayRange(Point, Normal, Capsule, Radius, Enter, Exit) ? FMath::Max(0., Exit) : 0.;
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

		FVector ContactNormal(const FVector& Point, const FVector& OnCapsule, const FVector& ReferencePoint, const FCapsule& Capsule, const FVector& ClothSegment, const FVector* SidePoint) {
			const FVector Delta = Point - OnCapsule;
			// Keep the side of the capsule this point occupied in the previous solution. Without it a strand
			// that the input animation carries across the capsule axis is pushed out of the opposite surface,
			// and the result flips between frames.
			if (SidePoint) {
				const FVector SideDelta = *SidePoint - ClosestOnSegment(*SidePoint, Capsule.Start, Capsule.End);
				if (SideDelta.SizeSquared() > SideTolerance && (Delta.SizeSquared() <= DirectionTolerance || FVector::DotProduct(Delta, SideDelta) < 0.0)) {
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

		void ProjectPoints(TArray<FVector>& Points, const TArray<FVector>& Reference, const TArray<FCapsule>& Capsules, double ParticleRadius, const TArray<FVector>* SidePoints, const TArray<FCapsule>* PreviousCapsules, const TArray<double>* PointRadii) {
			for (int32 Point = 1; Point < Points.Num(); ++Point) {
				for (int32 CapsuleIndex = 0; CapsuleIndex < Capsules.Num(); ++CapsuleIndex) {
					const FCapsule& Capsule = Capsules[CapsuleIndex];
					const double RPoint = PointRadii ? (*PointRadii)[Point] : ParticleRadius;
					const double Radius = Capsule.Radius + RPoint;
					FVector Side;
					if (Capsule.IsBox()) {
						if (SidePoints) { Side = PreviousCapsules ? TransportSidePoint((*SidePoints)[Point], (*PreviousCapsules)[CapsuleIndex], Capsule) : (*SidePoints)[Point]; }
						Points[Point] = ProjectBoxPoint(Points[Point], Capsule, RPoint, SidePoints ? &Side : nullptr);
						continue;
					}
					if (SidePoints) {
						Side = PreviousCapsules ? TransportSidePoint((*SidePoints)[Point], (*PreviousCapsules)[CapsuleIndex], Capsule) : (*SidePoints)[Point];
						// The endpoints can both be outside after a fast crossing. Stop at the entry
						// tangent plane, retaining tangential motion instead of snapping back in time.
						const FBox SweepBounds(Side.ComponentMin(Points[Point]), Side.ComponentMax(Points[Point]));
						const FBox CapsuleBounds(Capsule.Start.ComponentMin(Capsule.End) - FVector(Radius), Capsule.Start.ComponentMax(Capsule.End) + FVector(Radius));
						const double Slop = FMath::Max(1.e-4, Radius * 1.e-4);
						if (SweepBounds.Intersect(CapsuleBounds)
							&& FVector::Distance(Side, ClosestOnSegment(Side, Capsule.Start, Capsule.End)) >= Radius - Slop) {
							const FVector Motion = Points[Point] - Side;
							double Enter, Exit;
							if (CapsuleRayRange(Side, Motion, Capsule, Radius, Enter, Exit) && Enter >= -Slop / FMath::Max(1., Motion.Size()) && Enter < 1.) {
								const FVector Hit = Side + Motion * FMath::Max(0., Enter);
								const FVector N = (Hit - ClosestOnSegment(Hit, Capsule.Start, Capsule.End)).GetSafeNormal(DirectionTolerance);
								Points[Point] += N * FMath::Max(0., FVector::DotProduct(Hit - Points[Point], N));
							}
						}
					}
					const FVector OnCapsule = ClosestOnSegment(Points[Point], Capsule.Start, Capsule.End);
					const double Distance = FVector::Distance(Points[Point], OnCapsule);
					if (Distance < Radius) {
						const FVector Normal = ContactNormal(Points[Point], OnCapsule, Reference[Point], Capsule, FVector::ZeroVector, SidePoints ? &Side : nullptr);
						Points[Point] += Normal * DirectedContactDistance(Points[Point], Normal, Capsule, Radius);
					}
				}
			}
		}

		void ProjectSegments(TArray<FVector>& Points, const TArray<FVector>& Reference, const TArray<FCapsule>& Capsules, double ParticleRadius, const TArray<FVector>* SidePoints, const TArray<FCapsule>* PreviousCapsules, const TArray<double>* PointRadii) {
			for (int32 A = 0; A + 1 < Points.Num(); ++A) {
				const int32 B = A + 1;
				for (int32 CapsuleIndex = 0; CapsuleIndex < Capsules.Num(); ++CapsuleIndex) {
					const FCapsule& Capsule = Capsules[CapsuleIndex];
					const double RA = PointRadii ? (*PointRadii)[A] : ParticleRadius, RB = PointRadii ? (*PointRadii)[B] : ParticleRadius;
					const double Radius = Capsule.Radius + FMath::Max(RA, RB);
					if (Capsule.IsBox()) {
						if (A == 0 && SurfaceDistance(Points[A], Capsule) < RA) { continue; }
						const double T = BoxSegmentTime(Points[A], Points[B], Capsule, RA, RB);
						const double WA = A == 0 ? 0. : 1. - T, WB = T, Denom = WA * WA + WB * WB;
						if (Denom > 1.e-8) {
							const FVector Push = BoxPush(FMath::Lerp(Points[A], Points[B], T), Capsule, FMath::Lerp(RA, RB, T)) / Denom;
							Points[A] += (WA * Push).GetClampedToMaxSize(2. * Capsule.BoxExtent.GetMax());
							Points[B] += (WB * Push).GetClampedToMaxSize(2. * Capsule.BoxExtent.GetMax());
						}
						continue;
					}
					// A pinned attachment can lie inside the body/garment proxy. Its
					// first edge must exit that volume; pushing the entire edge outside
					// is impossible and makes the free end slide/flip around the anchor.
					// Keep point contacts and all later edges, and still report the residual.
					if (A == 0 && FVector::DistSquared(Points[0], ClosestOnSegment(Points[0], Capsule.Start, Capsule.End)) < Radius * Radius) {
						continue;
					}
					FVector OnCloth, OnCapsule;
					FMath::SegmentDistToSegmentSafe(Points[A], Points[B], Capsule.Start, Capsule.End, OnCloth, OnCapsule);
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
						if (PreviousCapsules) { SidePoint = TransportSidePoint(SidePoint, (*PreviousCapsules)[CapsuleIndex], Capsule); }
					}
					const FVector Normal = ContactNormal(OnCloth, OnCapsule, FMath::Lerp(Reference[A], Reference[B], T), Capsule, Edge, SidePoints ? &SidePoint : nullptr);
					const FVector Correction = Normal * (DirectedContactDistance(OnCloth, Normal, Capsule, Radius) / Denominator);
					// A contact arbitrarily close to a pinned root has a tiny
					// barycentric mass. Bound each projection instead of producing
					// a very large tip displacement for an effectively fixed contact.
					const double MaxProjection = 2.0 * Radius;
					Points[A] += (InverseMassA * WeightA * Correction).GetClampedToMaxSize(MaxProjection);
					Points[B] += (WeightB * Correction).GetClampedToMaxSize(MaxProjection);
				}
			}
		}

		FDiagnostics Measure(const TArray<FVector>& Points, const TArray<double>& Lengths, const TArray<FCapsule>& Capsules, double ParticleRadius, const TArray<double>* PointRadii) {
			TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_MeasureResiduals);
			FDiagnostics Result;
			for (int32 PointIndex = 0; PointIndex < Points.Num(); ++PointIndex) {
				const FVector& Point = Points[PointIndex];
				if (!IsFinite(Point)) {
					Result.bFinite = false;
					return Result;
				}
				for (const FCapsule& Capsule : Capsules) {
					Result.MaxPenetration = FMath::Max(Result.MaxPenetration, (PointRadii ? (*PointRadii)[PointIndex] : ParticleRadius) - SurfaceDistance(Point, Capsule));
				}
			}
			for (int32 A = 0; A < Lengths.Num(); ++A) {
				Result.MaxLengthError = FMath::Max(Result.MaxLengthError, FMath::Abs(FVector::Distance(Points[A], Points[A + 1]) - Lengths[A]));
				for (const FCapsule& Capsule : Capsules) {
					const double RA = PointRadii ? (*PointRadii)[A] : ParticleRadius, RB = PointRadii ? (*PointRadii)[A + 1] : ParticleRadius;
					if (Capsule.IsBox()) {
						const double T = BoxSegmentTime(Points[A], Points[A + 1], Capsule, RA, RB);
						Result.MaxPenetration = FMath::Max(Result.MaxPenetration, FMath::Lerp(RA, RB, T) - SurfaceDistance(FMath::Lerp(Points[A], Points[A + 1], T), Capsule));
						continue;
					}
					FVector OnCloth, OnCapsule;
					FMath::SegmentDistToSegmentSafe(Points[A], Points[A + 1], Capsule.Start, Capsule.End, OnCloth, OnCapsule);
					Result.MaxPenetration = FMath::Max(Result.MaxPenetration, Capsule.Radius + FMath::Max(RA, RB) - FVector::Distance(OnCloth, OnCapsule));
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

	namespace {
		double BendAngle(const TArray<FVector>& P, int32 I) {
			const FVector A = (P[I] - P[I-1]).GetSafeNormal(1.e-8), B = (P[I+1] - P[I]).GetSafeNormal(1.e-8);
			return FMath::Acos(FMath::Clamp(FVector::DotProduct(A, B), -1., 1.));
		}
		void ProjectAngular(TArray<FVector>& P, const FAngularConstraints& C) {
			// Three-point PBD bending: distribute correction to the joint and both neighboring
			// endpoints, retaining the animation's original curvature. The attachment has zero mass.
			if (C.MaxBendChangeRadians > 0.) for (int32 I = 1; I + 1 < P.Num(); ++I) {
				if (FVector::DistSquared(C.Pose[I],C.Pose[I-1]) <= 1.e-8 || FVector::DistSquared(C.Pose[I+1],C.Pose[I]) <= 1.e-8) { continue; }
				const FVector E0 = P[I]-P[I-1], E1 = P[I+1]-P[I];
				const double A = E0.Size(), B = E1.Size();
				if (A <= 1.e-4 || B <= 1.e-4) { continue; }
				const FVector U = E0/A, V = E1/B;
				const double Angle = BendAngle(C.Pose,I), Dot = FMath::Clamp(FVector::DotProduct(U,V),-1.,1.);
				const double Min = FMath::Cos(FMath::Min(UE_PI,Angle+C.MaxBendChangeRadians)), Max = FMath::Cos(FMath::Max(0.,Angle-C.MaxBendChangeRadians));
				const double Error = Dot-FMath::Clamp(Dot,Min,Max); if (FMath::Abs(Error) <= 1.e-7) { continue; }
				const FVector G0 = -(V-U*Dot)/A, G2 = (U-V*Dot)/B, G1 = -G0-G2;
				const double MassA = I == 1 ? 0. : 1., Denom = MassA*G0.SizeSquared()+G1.SizeSquared()+G2.SizeSquared();
				if (Denom <= 1.e-8) {
					FVector Tangent = C.Pose[I+1]-C.Pose[I]; Tangent = (Tangent-U*FVector::DotProduct(Tangent,U)).GetSafeNormal(1.e-8);
					if (Tangent.IsNearlyZero()) { Tangent = Perpendicular(U); }
					P[I+1] += Tangent*(.01*FMath::Min(A,B)); continue;
				}
				const double Lambda = -Error/Denom, MaxStep = .5*FMath::Min(A,B);
				P[I-1] += (G0*(MassA*Lambda)).GetClampedToMaxSize(MaxStep);
				P[I] += (G1*Lambda).GetClampedToMaxSize(MaxStep);
				P[I+1] += (G2*Lambda).GetClampedToMaxSize(MaxStep);
			}
			for (int32 I = 1; I < P.Num(); ++I) {
				const double Limit = C.LimitRadians[I]; if (Limit <= 0.) { continue; }
				const FVector Axis = (C.Pose[I]-C.Pose[I-1]).GetSafeNormal(1.e-8), Edge = P[I]-P[I-1];
				const double Length = Edge.Size(); if (Length <= 1.e-4 || Axis.IsNearlyZero()) { continue; }
				const FVector Direction = Edge / Length; const double Dot = FVector::DotProduct(Axis, Direction);
				if (Dot >= FMath::Cos(Limit)) { continue; }
				FVector Tangent = (Direction-Axis*Dot).GetSafeNormal(1.e-8);
				if (Tangent.IsNearlyZero()) { Tangent = Perpendicular(Axis); }
				const FVector Clamped = Axis*FMath::Cos(Limit)+Tangent*FMath::Sin(Limit);
				const double MassA = I == 1 ? 0. : 1.;
				const FVector Push = (Clamped*Length-Edge)/(MassA+1.);
				P[I-1] -= Push*MassA; P[I] += Push;
			}
		}
	}
	FDiagnostics SolveChain(TArray<FVector>& Points, const TArray<double>& RestLengths, const TArray<FVector>& ReferencePoints, const TArray<FCapsule>& Capsules, int32 Iterations, double ParticleRadius, const TArray<FVector>* SidePoints, const TArray<FCapsule>* PreviousCapsules, const TArray<double>* PointRadii, const FAngularConstraints* Angular) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Correction_SolveChain);
		SCOPE_CYCLE_COUNTER(STAT_KawaiiBakerSolveChain);
		FDiagnostics Invalid;
		Invalid.bFinite = false;
		if ((PointRadii && PointRadii->Num() != Points.Num()) || Points.IsEmpty() || RestLengths.Num() != Points.Num() - 1 || ReferencePoints.Num() != Points.Num() || !FMath::IsFinite(ParticleRadius)
			|| ParticleRadius < 0.0)
		{
			return Invalid;
		}
		for (int32 Index = 0; Index < Points.Num(); ++Index) {
			if (!IsFinite(Points[Index]) || !IsFinite(ReferencePoints[Index]) || (PointRadii && (!FMath::IsFinite((*PointRadii)[Index]) || (*PointRadii)[Index] < 0.))) {
				return Invalid;
			}
		}
		for (const double Length : RestLengths) {
			if (!FMath::IsFinite(Length) || Length < 0.0) {
				return Invalid;
			}
		}
		if (Angular) {
			if (Angular->Pose.Num() != Points.Num() || Angular->LimitRadians.Num() != Points.Num()
				|| !FMath::IsFinite(Angular->MaxBendChangeRadians) || Angular->MaxBendChangeRadians < 0. || Angular->MaxBendChangeRadians > UE_PI) { return Invalid; }
			for (int32 I = 0; I < Points.Num(); ++I) {
				if (!IsFinite(Angular->Pose[I]) || !FMath::IsFinite(Angular->LimitRadians[I]) || Angular->LimitRadians[I] < 0. || Angular->LimitRadians[I] > UE_PI) { return Invalid; }
			}
		}
		for (const FCapsule& Capsule : Capsules) {
			if (!IsFinite(Capsule.Start) || !IsFinite(Capsule.End) || !FMath::IsFinite(Capsule.Radius) || Capsule.Radius < 0.0 || !IsFinite(Capsule.BoxExtent) || (Capsule.IsBox() && (Capsule.BoxExtent.GetMin() <= 0. || !Capsule.BoxRotation.IsNormalized()))) {
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
		if (PreviousCapsules) {
			bool bUsable = PreviousCapsules->Num() == Capsules.Num();
			for (const auto& Capsule : *PreviousCapsules) {
				bUsable &= IsFinite(Capsule.Start) && IsFinite(Capsule.End) && FMath::IsFinite(Capsule.Radius) && Capsule.Radius >= 0. && IsFinite(Capsule.BoxExtent)
					&& (!Capsule.IsBox() || (Capsule.BoxExtent.GetMin() > 0. && Capsule.BoxRotation.IsNormalized()));
			}
			if (!bUsable) { SidePoints = nullptr; PreviousCapsules = nullptr; }
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
				if (Angular) { ProjectAngular(Points, *Angular); }
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(KawaiiBaker_Correction_PointContacts, bDetailedTrace);
				ProjectPoints(Points, ReferencePoints, Capsules, ParticleRadius, SidePoints, PreviousCapsules, PointRadii);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(KawaiiBaker_Correction_SegmentContacts, bDetailedTrace);
				ProjectSegments(Points, ReferencePoints, Capsules, ParticleRadius, SidePoints, PreviousCapsules, PointRadii);
			}
			{
				TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(KawaiiBaker_Correction_FinalPointContacts, bDetailedTrace);
				ProjectPoints(Points, ReferencePoints, Capsules, ParticleRadius, SidePoints, PreviousCapsules, PointRadii);
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
		FDiagnostics Result = Measure(Points, RestLengths, Capsules, ParticleRadius, PointRadii);
		if (Angular) {
			for (int32 I = 1; I < Points.Num(); ++I) {
				if (Angular->LimitRadians[I] > 0. && FVector::DistSquared(Points[I],Points[I-1]) > 1.e-8 && FVector::DistSquared(Angular->Pose[I],Angular->Pose[I-1]) > 1.e-8) {
					const double Angle = FMath::Acos(FMath::Clamp(FVector::DotProduct((Points[I]-Points[I-1]).GetSafeNormal(1.e-8), (Angular->Pose[I]-Angular->Pose[I-1]).GetSafeNormal(1.e-8)), -1., 1.));
					Result.MaxAngleViolationDegrees = FMath::Max(Result.MaxAngleViolationDegrees, FMath::RadiansToDegrees(Angle-Angular->LimitRadians[I]));
				}
				if (I+1 < Points.Num() && Angular->MaxBendChangeRadians > 0.
					&& FVector::DistSquared(Points[I],Points[I-1]) > 1.e-8 && FVector::DistSquared(Points[I+1],Points[I]) > 1.e-8
					&& FVector::DistSquared(Angular->Pose[I],Angular->Pose[I-1]) > 1.e-8 && FVector::DistSquared(Angular->Pose[I+1],Angular->Pose[I]) > 1.e-8) {
					Result.MaxBendViolationDegrees = FMath::Max(Result.MaxBendViolationDegrees, FMath::RadiansToDegrees(FMath::Abs(BendAngle(Points,I)-BendAngle(Angular->Pose,I))-Angular->MaxBendChangeRadians));
				}
			}
		}
		if (!Result.bFinite) {
			Points = OriginalPoints;
		}
		return Result;
	}
}
