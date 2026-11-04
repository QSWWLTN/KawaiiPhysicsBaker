// TEMPORARY prototype (DSH session). Re-implements the correction solve offline to compare stabilisation
// strategies on the real climb assets. Delete after diagnosis.
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "BoneContainer.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "KawaiiClothingProxyMotion.h"
#include "KawaiiClothingProxySnapshot.h"
#include "KawaiiPhysicsIKCorrectionMath.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/MemStack.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace TempKawaiiProto
{
	using KawaiiPhysicsIKCorrection::FCapsule;
	constexpr double Tol = 1.e-8;

	FVector Closest(const FVector& Point, const FVector& Start, const FVector& End)
	{
		const FVector Edge = End - Start;
		const double LengthSquared = Edge.SizeSquared();
		const double T = LengthSquared > Tol ? FMath::Clamp(FVector::DotProduct(Point - Start, Edge) / LengthSquared, 0.0, 1.0) : 0.0;
		return Start + T * Edge;
	}

	FVector Perp(const FVector& Axis)
	{
		const FVector N = Axis.GetSafeNormal(Tol);
		if (N.IsNearlyZero()) {
			return FVector::YAxisVector;
		}
		const FVector Abs = N.GetAbs();
		const FVector Seed = Abs.X <= Abs.Y && Abs.X <= Abs.Z ? FVector::XAxisVector : (Abs.Y <= Abs.Z ? FVector::YAxisVector : FVector::ZAxisVector);
		return FVector::CrossProduct(N, Seed).GetSafeNormal(Tol);
	}

	FVector Normal(const FVector& Point, const FVector& OnCapsule, const FVector& Reference, const FCapsule& Capsule, const FVector& ClothSegment, const FVector* Side)
	{
		const FVector Delta = Point - OnCapsule;
		if (Side) {
			const FVector SideDelta = *Side - Closest(*Side, Capsule.Start, Capsule.End);
			if (SideDelta.SizeSquared() > 1.e-6 && (Delta.SizeSquared() <= Tol || FVector::DotProduct(Delta, SideDelta) < 0.0)) {
				return SideDelta.GetSafeNormal();
			}
		}
		if (Delta.SizeSquared() > Tol) {
			return Delta.GetSafeNormal(Tol);
		}
		const FVector Axis = Capsule.End - Capsule.Start;
		const FVector ReferenceDelta = Reference - Closest(Reference, Capsule.Start, Capsule.End);
		FVector N = FVector::CrossProduct(ClothSegment, Axis).GetSafeNormal(Tol);
		if (!N.IsNearlyZero()) {
			return FVector::DotProduct(N, ReferenceDelta) < 0.0 ? -N : N;
		}
		if (ReferenceDelta.SizeSquared() > Tol) {
			N = ReferenceDelta.GetSafeNormal(Tol);
			if (!ClothSegment.IsNearlyZero()) {
				const FVector D = ClothSegment.GetSafeNormal(Tol);
				N -= D * FVector::DotProduct(N, D);
			}
			N.Normalize(Tol);
			if (!N.IsNearlyZero()) {
				return N;
			}
		}
		return Perp(Axis.SizeSquared() > Tol ? Axis : ClothSegment);
	}

	void ProjectLengths(TArray<FVector>& Points, const TArray<double>& Lengths, const TArray<FVector>& Reference, bool bReverse)
	{
		for (int32 Step = 0; Step < Lengths.Num(); ++Step) {
			const int32 A = bReverse ? Lengths.Num() - Step - 1 : Step;
			const int32 B = A + 1;
			const double WeightA = A == 0 ? 0.0 : 1.0;
			const FVector Delta = Points[B] - Points[A];
			const double Length = Delta.Size();
			FVector Direction = Length > Tol ? Delta / Length : (Reference[B] - Reference[A]).GetSafeNormal(Tol);
			if (Direction.IsNearlyZero()) {
				Direction = FVector::XAxisVector;
			}
			const FVector Correction = Direction * ((Length - Lengths[A]) / (WeightA + 1.0));
			Points[A] += WeightA * Correction;
			Points[B] -= Correction;
		}
	}

	void ProjectPoints(TArray<FVector>& Points, const TArray<FVector>& Reference, const TArray<FCapsule>& Capsules, double ParticleRadius, const TArray<FVector>* Side)
	{
		for (int32 Point = 1; Point < Points.Num(); ++Point) {
			for (const FCapsule& Capsule : Capsules) {
				const double Radius = Capsule.Radius + ParticleRadius;
				const FVector OnCapsule = Closest(Points[Point], Capsule.Start, Capsule.End);
				const double Distance = FVector::Distance(Points[Point], OnCapsule);
				if (Distance < Radius) {
					Points[Point] += Normal(Points[Point], OnCapsule, Reference[Point], Capsule, FVector::ZeroVector, Side ? &(*Side)[Point] : nullptr) * (Radius - Distance);
				}
			}
		}
	}

	void ProjectSegments(TArray<FVector>& Points, const TArray<FVector>& Reference, const TArray<FCapsule>& Capsules, double ParticleRadius, const TArray<FVector>* Side)
	{
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
				const double T = EdgeSquared > Tol ? FMath::Clamp(FVector::DotProduct(OnCloth - Points[A], Edge) / EdgeSquared, 0.0, 1.0) : 0.0;
				const double InverseMassA = A == 0 ? 0.0 : 1.0;
				const double Denominator = InverseMassA * (1.0 - T) * (1.0 - T) + T * T;
				if (Denominator <= Tol) {
					continue;
				}
				FVector SidePoint;
				if (Side) {
					SidePoint = FMath::Lerp((*Side)[A], (*Side)[B], T);
				}
				const FVector N = Normal(OnCloth, OnCapsule, FMath::Lerp(Reference[A], Reference[B], T), Capsule, Edge, Side ? &SidePoint : nullptr);
				const FVector Correction = N * ((Radius - Distance) / Denominator);
				const double MaxProjection = 2.0 * Radius;
				Points[A] += (InverseMassA * (1.0 - T) * Correction).GetClampedToMaxSize(MaxProjection);
				Points[B] += (T * Correction).GetClampedToMaxSize(MaxProjection);
			}
		}
	}

	void Solve(TArray<FVector>& Points, const TArray<double>& Lengths, const TArray<FVector>& Reference, const TArray<FCapsule>& Capsules,
			   int32 Iterations, double ParticleRadius, const TArray<FVector>* Side)
	{
		const FVector Root = Points[0];
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration) {
			ProjectLengths(Points, Lengths, Reference, false);
			ProjectLengths(Points, Lengths, Reference, true);
			ProjectPoints(Points, Reference, Capsules, ParticleRadius, Side);
			ProjectSegments(Points, Reference, Capsules, ParticleRadius, Side);
			ProjectPoints(Points, Reference, Capsules, ParticleRadius, Side);
			Points[0] = Root;
		}
	}

	void ComponentSpace(const FCompactPose& Pose, TArray<FTransform>& Out)
	{
		const FBoneContainer& Bones = Pose.GetBoneContainer();
		Out.SetNum(Pose.GetNumBones());
		for (int32 Index = 0; Index < Out.Num(); ++Index) {
			const FCompactPoseBoneIndex Bone(Index);
			const FCompactPoseBoneIndex Parent = Bones.GetParentBoneIndex(Bone);
			Out[Index] = Parent.GetInt() == INDEX_NONE ? Pose[Bone] : Pose[Bone] * Out[Parent.GetInt()];
		}
	}

	FTransform ProxyRootLocal(const KawaiiClothingProxy::FSnapshot& Snap, float Time)
	{
		const double Frame = FMath::Clamp(double(Time) * Snap.SampleRate, 0.0, double(Snap.NumFrames - 1));
		const int32 A = FMath::FloorToInt(Frame);
		const int32 B = FMath::Min(A + 1, Snap.NumFrames - 1);
		FTransform Result;
		Result.Blend(Snap.LocalKeys[A * Snap.BoneNames.Num()], Snap.LocalKeys[B * Snap.BoneNames.Num()], float(Frame - A));
		Result.NormalizeRotation();
		return Result;
	}

	struct FProtoChain
	{
		TArray<int32> Bones;
		FName RootName;
		int32 Anchor = INDEX_NONE;
		double Radius = 0.0;
	};

	struct FProtoMode
	{
		const TCHAR* Name;
		bool bBody;
		bool bSide;
		float Smoothing; // 0 disables; otherwise per-frame blend factor toward a larger offset
		float Release;   // per-frame blend factor toward a smaller offset; 0 uses Smoothing
		float WarmPull = 0.f; // >0: start each solve from last frame's solution pulled toward the baked pose by this factor
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTempKawaiiJitterPrototype, "KawaiiPhysicsBaker.TempDiagnostics.JitterPrototype",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTempKawaiiJitterPrototype::RunTest(const FString&)
{
	using namespace TempKawaiiProto;
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Anime/Lime/Lime.Lime"));
	UAnimSequence* Body = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/KawaiiBakes/Baked_Jump_Climb_2h_Main_D163B64F.Baked_Jump_Climb_2h_Main_D163B64F"));
	UKawaiiClothingProxyMotion* Motion = LoadObject<UKawaiiClothingProxyMotion>(nullptr,
		TEXT("/Game/KawaiiBakes/Baked_Jump_Climb_2h_Base_D163B64F_ProxyMotion.Baked_Jump_Climb_2h_Base_D163B64F_ProxyMotion"));
	UKawaiiPhysicsIKCorrectionProfile* Profile = LoadObject<UKawaiiPhysicsIKCorrectionProfile>(nullptr,
		TEXT("/Game/BP/AnimeBP/Lime/DA_AnimBP_Lime_IKCorrection.DA_AnimBP_Lime_IKCorrection"));
	if (!TestTrue(TEXT("Assets load"), Mesh && Body && Motion && Profile)) {
		return false;
	}
	KawaiiClothingProxy::FSnapshot Snap;
	FString Error;
	if (!KawaiiClothingProxy::BuildSnapshot(*Motion, Snap, Error)) {
		AddError(Error);
		return false;
	}
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	Component->SetSkeletalMesh(Mesh);
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get()));
	TArray<FBoneIndexType> BoneIndices;
	for (int32 Bone = 0; Bone < Mesh->GetRefSkeleton().GetNum(); ++Bone) {
		BoneIndices.Add(FBoneIndexType(Bone));
	}
	const UE::Anim::FCurveFilterSettings CurveFilter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh);
	FAnimInstanceProxy Proxy(Instance.Get());
	Proxy.GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh);
	const FBoneContainer& Required = Proxy.GetRequiredBones();

	TArray<FProtoChain> Chains;
	for (const FKawaiiPhysicsIKCorrectionChain& Definition : Profile->Chains) {
		FProtoChain& Chain = Chains.AddDefaulted_GetRef();
		const int32 Root = Required.GetPoseBoneIndexForBoneName(Definition.RootBone.BoneName);
		for (int32 Bone = Required.GetPoseBoneIndexForBoneName(Definition.TipBone.BoneName); Bone != INDEX_NONE; Bone = Required.GetParentBoneIndex(Bone)) {
			Chain.Bones.Insert(Bone, 0);
			if (Bone == Root) {
				break;
			}
		}
		Chain.RootName = Definition.RootBone.BoneName;
		Chain.Anchor = Required.GetParentBoneIndex(Root);
		Chain.Radius = Definition.CollisionRadius;
	}

	const int32 NumFrames = FMath::FloorToInt(2.54f * 60.f) + 1;
	// Pre-sample inputs once: baseline component pose and re-anchored proxies.
	TArray<TArray<FTransform>> BaseFrames;
	TArray<TArray<FKawaiiClothingProxyCapsule>> ProxyFrames;
	for (int32 Frame = 0; Frame < NumFrames; ++Frame) {
		FMemMark Mark(FMemStack::Get());
		const float Time = Frame / 60.f;
		FPoseContext Output(&Proxy);
		FAnimationPoseData PoseData(Output);
		Body->GetAnimationPose(PoseData, FAnimExtractContext(double(Time), true));
		ComponentSpace(Output.Pose, BaseFrames.AddDefaulted_GetRef());
		KawaiiClothingProxy::FWeightedSnapshot Sample;
		Sample.Motion = &Snap;
		Sample.Time = Time;
		TArray<FKawaiiClothingProxyCapsule>& Capsules = ProxyFrames.AddDefaulted_GetRef();
		if (!KawaiiClothingProxy::SampleSnapshots({Sample}, Snap.RelativeTransform, Capsules, Error)) {
			AddError(Error);
			return false;
		}
		const FTransform Recorded = ProxyRootLocal(Snap, Time) * Snap.RelativeTransform;
		const FTransform RecordedRigid(Recorded.GetRotation(), Recorded.GetTranslation());
		const FTransform Current(BaseFrames.Last()[0].GetRotation(), BaseFrames.Last()[0].GetTranslation());
		for (FKawaiiClothingProxyCapsule& Capsule : Capsules) {
			Capsule.Start = Current.TransformPosition(RecordedRigid.InverseTransformPosition(Capsule.Start));
			Capsule.End = Current.TransformPosition(RecordedRigid.InverseTransformPosition(Capsule.End));
		}
	}

	// Baseline depth statistics against proxies only.
	int64 PointSamples = 0, InContact = 0, Deep = 0;
	for (int32 Frame = 0; Frame < NumFrames; ++Frame) {
		for (const FProtoChain& Chain : Chains) {
			TArray<FCapsule> Capsules;
			KawaiiPhysicsIKCorrection::AppendProxyCapsules(ProxyFrames[Frame], Capsules, Chain.RootName);
			for (int32 Index = 1; Index < Chain.Bones.Num(); ++Index) {
				const FVector Point = BaseFrames[Frame][Chain.Bones[Index]].GetTranslation();
				double Ratio = -1.0;
				for (const FCapsule& Capsule : Capsules) {
					const double R = Capsule.Radius + Chain.Radius;
					Ratio = FMath::Max(Ratio, (R - FVector::Distance(Point, Closest(Point, Capsule.Start, Capsule.End))) / R);
				}
				++PointSamples;
				InContact += Ratio > 0.0 ? 1 : 0;
				Deep += Ratio > 0.5 ? 1 : 0;
			}
		}
	}

	const TArray<FProtoMode> Modes{
		{TEXT("current_with_body"), true, false, 0.f, 0.f},
		{TEXT("side_coherent_with_body"), true, true, 0.f, 0.f},
		{TEXT("side_tau30ms"), true, true, 0.426f, 0.f},
		{TEXT("side_tau50ms"), true, true, 0.283f, 0.f},
		{TEXT("side_tau80ms"), true, true, 0.188f, 0.f},
		{TEXT("side_attack60_release20"), true, true, 0.6f, 0.2f},
		{TEXT("side_attack43_release19"), true, true, 0.426f, 0.188f},
		{TEXT("current_tau50ms"), true, false, 0.283f, 0.f},
		{TEXT("warm_side_tau30ms"), true, true, 0.f, 0.f, 0.426f},
		{TEXT("warm_side_tau50ms"), true, true, 0.f, 0.f, 0.283f},
		{TEXT("warm_side_tau80ms"), true, true, 0.f, 0.f, 0.188f},
		{TEXT("warm_noside_tau50ms"), true, false, 0.f, 0.f, 0.283f},
		{TEXT("warm50_post30"), true, true, 0.426f, 0.f, 0.283f},
		{TEXT("warm80_post30"), true, true, 0.426f, 0.f, 0.188f},
		{TEXT("warm80_post50"), true, true, 0.283f, 0.f, 0.188f},
	};
	int32 BakedReversals = 0;
	for (int32 Frame = 2; Frame < NumFrames; ++Frame) {
		for (const FProtoChain& Chain : Chains) {
			for (int32 Index = 1; Index < Chain.Bones.Num(); ++Index) {
				const int32 Bone = Chain.Bones[Index];
				const FVector V = BaseFrames[Frame][Bone].GetTranslation() - BaseFrames[Frame - 1][Bone].GetTranslation();
				const FVector U = BaseFrames[Frame - 1][Bone].GetTranslation() - BaseFrames[Frame - 2][Bone].GetTranslation();
				BakedReversals += (V.Size() > 0.5 && U.Size() > 0.5 && FVector::DotProduct(V, U) < 0.0) ? 1 : 0;
			}
		}
	}
	FString Json = FString::Printf(TEXT("{\n\"baseline_points\":%lld,\"in_proxy_contact\":%lld,\"deeper_than_half_radius\":%lld,\"baked_hair_reversals\":%d,\n\"modes\":{\n"),
								   PointSamples, InContact, Deep, BakedReversals);
	for (int32 ModeIndex = 0; ModeIndex < Modes.Num(); ++ModeIndex) {
		const FProtoMode& Mode = Modes[ModeIndex];
		// Per chain: previous solved points and smoothed offsets, both stored in anchor-local space.
		TArray<TArray<FVector>> PreviousLocal, SmoothedLocal;
		PreviousLocal.SetNum(Chains.Num());
		SmoothedLocal.SetNum(Chains.Num());
		TArray<TArray<FVector>> Offsets; // [frame][flattened chain point]
		TArray<double> Residuals;        // final overlap of every output point, including body capsules when enabled
		TArray<TArray<FVector>> FinalPositions;
		for (int32 Frame = 0; Frame < NumFrames; ++Frame) {
			const TArray<FTransform>& Base = BaseFrames[Frame];
			TArray<FVector>& FrameOffsets = Offsets.AddDefaulted_GetRef();
			FinalPositions.AddDefaulted();
			for (int32 ChainIndex = 0; ChainIndex < Chains.Num(); ++ChainIndex) {
				const FProtoChain& Chain = Chains[ChainIndex];
				TArray<FCapsule> Capsules;
				if (Mode.bBody) {
					for (const FKawaiiPhysicsIKCorrectionCapsule& Definition : Profile->Capsules) {
						const int32 Start = Required.GetPoseBoneIndexForBoneName(Definition.StartBone.BoneName);
						const int32 End = Definition.EndBone.BoneName.IsNone() ? Start : Required.GetPoseBoneIndexForBoneName(Definition.EndBone.BoneName);
						Capsules.Add({Base[Start].TransformPosition(Definition.StartOffset), Base[End].TransformPosition(Definition.EndOffset),
									  Definition.Radius * FMath::Max(Base[Start].GetScale3D().GetAbsMax(), Base[End].GetScale3D().GetAbsMax())});
					}
				}
				KawaiiPhysicsIKCorrection::AppendProxyCapsules(ProxyFrames[Frame], Capsules, Chain.RootName);
				TArray<FVector> Points, Reference;
				TArray<double> Lengths;
				for (int32 Index = 0; Index < Chain.Bones.Num(); ++Index) {
					Points.Add(Base[Chain.Bones[Index]].GetTranslation());
					if (Index > 0) {
						Lengths.Add(FVector::Distance(Points[Index], Points[Index - 1]));
					}
				}
				Reference = Points;
				const FTransform& Anchor = Base[Chain.Anchor];
				TArray<FVector> Side;
				const bool bHasHistory = PreviousLocal[ChainIndex].Num() == Points.Num();
				const bool bHasSide = Mode.bSide && bHasHistory;
				if (bHasSide) {
					for (const FVector& Local : PreviousLocal[ChainIndex]) {
						Side.Add(Anchor.TransformPosition(Local));
					}
				}
				if (Mode.WarmPull > 0.f && bHasHistory) {
					for (int32 Index = 1; Index < Points.Num(); ++Index) {
						Points[Index] = FMath::Lerp(Anchor.TransformPosition(PreviousLocal[ChainIndex][Index]), Reference[Index], double(Mode.WarmPull));
					}
				}
				Solve(Points, Lengths, Reference, Capsules, Profile->Iterations, Chain.Radius, bHasSide ? &Side : nullptr);
				PreviousLocal[ChainIndex].Reset();
				for (const FVector& Point : Points) {
					PreviousLocal[ChainIndex].Add(Anchor.InverseTransformPosition(Point));
				}
				TArray<FVector>& Smoothed = SmoothedLocal[ChainIndex];
				if (Mode.Smoothing > 0.f && Smoothed.Num() != Points.Num()) {
					Smoothed.Init(FVector::ZeroVector, Points.Num());
				}
				for (int32 Index = 1; Index < Points.Num(); ++Index) {
					FVector Offset = Points[Index] - Reference[Index];
					if (Mode.Smoothing > 0.f) {
						const FVector Local = Anchor.InverseTransformVector(Offset);
						const bool bRelease = Mode.Release > 0.f && Local.SizeSquared() < Smoothed[Index].SizeSquared();
						Smoothed[Index] = FMath::Lerp(Smoothed[Index], Local, double(bRelease ? Mode.Release : Mode.Smoothing));
						Offset = Anchor.TransformVector(Smoothed[Index]);
					}
					FrameOffsets.Add(Offset);
					const FVector Final = Reference[Index] + Offset;
					FinalPositions.Last().Add(Final);
					double Residual = 0.0;
					for (const FCapsule& Capsule : Capsules) {
						Residual = FMath::Max(Residual, Capsule.Radius + Chain.Radius - FVector::Distance(Final, Closest(Final, Capsule.Start, Capsule.End)));
					}
					Residuals.Add(Residual);
				}
			}
		}
		TArray<double> Jitter;
		int32 Flips = 0, Jumps = 0, Reversals = 0;
		double MaxOffset = 0.0, SumOffset = 0.0;
		const int32 PointCount = Offsets[0].Num();
		for (int32 Frame = 0; Frame < NumFrames; ++Frame) {
			for (int32 Point = 0; Point < PointCount; ++Point) {
				const FVector& O = Offsets[Frame][Point];
				MaxOffset = FMath::Max(MaxOffset, O.Size());
				SumOffset += O.Size();
				if (Frame > 0) {
					const FVector& P = Offsets[Frame - 1][Point];
					Flips += (O.Size() > 1.0 && P.Size() > 1.0 && FVector::DotProduct(O, P) < 0.0) ? 1 : 0;
					Jumps += (O - P).Size() > 5.0 ? 1 : 0;
				}
				if (Frame > 1) {
					// Visible shaking: the displayed point reverses its motion between consecutive frames at a noticeable speed.
					const FVector V = FinalPositions[Frame][Point] - FinalPositions[Frame - 1][Point];
					const FVector U = FinalPositions[Frame - 1][Point] - FinalPositions[Frame - 2][Point];
					Reversals += (V.Size() > 0.5 && U.Size() > 0.5 && FVector::DotProduct(V, U) < 0.0) ? 1 : 0;
				}
				if (Frame > 0 && Frame + 1 < NumFrames) {
					Jitter.Add((Offsets[Frame + 1][Point] - 2.0 * O + Offsets[Frame - 1][Point]).Size());
				}
			}
		}
		Jitter.Sort();
		double Sum = 0.0;
		for (const double J : Jitter) {
			Sum += J;
		}
		const auto Pct = [&Jitter](double P) { return Jitter[FMath::Clamp(int32(P * (Jitter.Num() - 1)), 0, Jitter.Num() - 1)]; };
		Residuals.Sort();
		int32 Penetrating = 0;
		double ResidualSum = 0.0;
		for (const double R : Residuals) {
			Penetrating += R > 0.5 ? 1 : 0;
			ResidualSum += FMath::Max(0.0, R);
		}
		Json += FString::Printf(TEXT("%s\"%s\":{\"jitter_mean\":%.4f,\"jitter_p95\":%.4f,\"jitter_p99\":%.4f,\"jitter_max\":%.3f,\"direction_flips\":%d,\"jumps_over_5cm\":%d,\"reversals\":%d,\"offset_mean\":%.3f,\"offset_max\":%.2f,")
								TEXT("\"points_penetrating_over_0_5cm\":%d,\"residual_mean\":%.4f,\"residual_p99\":%.3f,\"residual_max\":%.2f}\n"),
								ModeIndex ? TEXT(",") : TEXT(""), Mode.Name, Sum / Jitter.Num(), Pct(0.95), Pct(0.99), Jitter.Last(), Flips, Jumps, Reversals,
								SumOffset / FMath::Max(1, NumFrames * PointCount), MaxOffset, Penetrating, ResidualSum / FMath::Max(1, Residuals.Num()),
								Residuals[FMath::Clamp(int32(0.99 * (Residuals.Num() - 1)), 0, Residuals.Num() - 1)], Residuals.Last());
	}
	Json += TEXT("}\n}\n");
	const FString Path = FPaths::ProjectSavedDir() / TEXT("DSHInspect/jitter_prototype.json");
	TestTrue(TEXT("Report written"), FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	return true;
}

#endif
