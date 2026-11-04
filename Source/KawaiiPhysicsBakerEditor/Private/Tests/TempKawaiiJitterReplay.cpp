// TEMPORARY diagnostic (DSH session). Replays the runtime Automatic Clothing Proxy Correction path offline on the
// real climb assets (snapshot sampling, live-root reanchoring, temporal correction at 60 Hz) and measures
// frame-to-frame instability of the corrected hair. Delete after the in-game acceptance of the fix.
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Animation/AttributesRuntime.h"
#include "BoneContainer.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "KawaiiClothingProxyMotion.h"
#include "KawaiiClothingProxySnapshot.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/MemStack.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace TempKawaiiJitter
{
	struct FReplayVariant
	{
		const TCHAR* Name;
		bool bProxies;
		bool bBodyCapsules;
		bool bReanchor;
		bool bTemporal;
	};

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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTempKawaiiJitterReplay, "KawaiiPhysicsBaker.TempDiagnostics.JitterReplay",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTempKawaiiJitterReplay::RunTest(const FString&)
{
	using namespace TempKawaiiJitter;
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
	if (!TestTrue(TEXT("Snapshot builds"), KawaiiClothingProxy::BuildSnapshot(*Motion, Snap, Error))) {
		AddError(Error);
		return false;
	}
	TArray<TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile>> VariantProfiles;
	const auto MakeProfile = [&](bool bBodyCapsules, bool bTemporal) {
		UKawaiiPhysicsIKCorrectionProfile* Copy = DuplicateObject<UKawaiiPhysicsIKCorrectionProfile>(Profile, GetTransientPackage());
		VariantProfiles.Emplace(Copy);
		if (!bBodyCapsules) {
			Copy->Capsules.Reset();
		}
		if (!bTemporal) {
			Copy->SettleTime = Copy->SmoothingTime = 0.f;
		}
		return Copy;
	};

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

	// Every bone on a configured chain path, root excluded (it is pinned by the solver).
	TArray<int32> ChainBones;
	TArray<FString> ChainBoneNames;
	for (const FKawaiiPhysicsIKCorrectionChain& Chain : Profile->Chains) {
		const int32 Root = Required.GetPoseBoneIndexForBoneName(Chain.RootBone.BoneName);
		for (int32 Bone = Required.GetPoseBoneIndexForBoneName(Chain.TipBone.BoneName); Bone != INDEX_NONE && Bone != Root;
			 Bone = Required.GetParentBoneIndex(Bone)) {
			ChainBones.Add(Bone);
			ChainBoneNames.Add(Mesh->GetRefSkeleton().GetBoneName(Bone).ToString());
		}
	}
	const FName ProxyRootName = Snap.BoneNames[0];
	const int32 TargetRoot = Required.GetPoseBoneIndexForBoneName(ProxyRootName);

	const float EndTime = 2.54f;
	const int32 NumFrames = FMath::FloorToInt(EndTime * 60.f) + 1;
	const float DeltaTime = 1.f / 60.f;
	const TArray<FReplayVariant> Variants{
		{TEXT("before_fix"), true, true, false, false},
		{TEXT("reanchor_only"), true, true, true, false},
		{TEXT("temporal_only"), true, true, false, true},
		{TEXT("after_fix"), true, true, true, true},
		{TEXT("after_fix_proxies_only"), true, false, true, true},
		{TEXT("body_capsules_only"), false, true, false, false},
	};

	FString Json = TEXT("{\n");
	Json += FString::Printf(TEXT("\"proxy_root\":\"%s\",\"target_root_index\":%d,\"chain_bones\":%d,\"frames\":%d,\"settle_time\":%.3f,\"smoothing_time\":%.3f,\n"),
							*ProxyRootName.ToString(), TargetRoot, ChainBones.Num(), NumFrames, Profile->SettleTime, Profile->SmoothingTime);
	// Root drift between the locked body root and the proxies' recorded root.
	Json += TEXT("\"root_drift_cm\":[");
	{
		FMemMark Mark(FMemStack::Get());
		for (int32 Frame = 0; Frame < NumFrames; Frame += 15) {
			const float Time = Frame / 60.f;
			FPoseContext Output(&Proxy);
			FAnimationPoseData PoseData(Output);
			Body->GetAnimationPose(PoseData, FAnimExtractContext(double(Time), true));
			const FTransform BodyRoot = Output.Pose[FCompactPoseBoneIndex(0)];
			const FTransform ProxyRoot = ProxyRootLocal(Snap, Time) * Snap.RelativeTransform;
			Json += FString::Printf(TEXT("%s[%.2f,%.1f]"), Frame ? TEXT(",") : TEXT(""), Time, FVector::Distance(BodyRoot.GetTranslation(), ProxyRoot.GetTranslation()));
		}
	}
	Json += TEXT("],\n\"variants\":{\n");

	TArray<TArray<FVector>> Baseline;
	for (int32 VariantIndex = 0; VariantIndex < Variants.Num(); ++VariantIndex) {
		const FReplayVariant& Variant = Variants[VariantIndex];
		FAnimNode_KawaiiPhysicsIKCorrection Correction;
		Correction.Profile = MakeProfile(Variant.bBodyCapsules, Variant.bTemporal);
		Correction.PreUpdate(Instance.Get());
		TArray<TArray<FVector>> Offsets;
		TArray<TArray<FVector>> Positions;
		TMap<FString, int32> Reasons;
		double MaxPenetration = 0.0;
		double SumPenetration = 0.0;
		double MaxLengthError = 0.0;
		for (int32 Frame = 0; Frame < NumFrames; ++Frame) {
			FMemMark Mark(FMemStack::Get());
			const float Time = Frame / 60.f;
			FPoseContext Output(&Proxy);
			FAnimationPoseData PoseData(Output);
			Body->GetAnimationPose(PoseData, FAnimExtractContext(double(Time), true));
			FPoseContext Base(Output);
			Base.Pose.CopyBonesFrom(Output.Pose);
			TArray<FKawaiiClothingProxyCapsule> Capsules;
			if (Variant.bProxies) {
				KawaiiClothingProxy::FWeightedSnapshot Sample;
				Sample.Motion = &Snap;
				Sample.Time = Time;
				Sample.Weight = 1.f;
				FTransform RecordedRoot;
				if (!KawaiiClothingProxy::SampleSnapshots({Sample}, Snap.RelativeTransform, Capsules, Error, &RecordedRoot)) {
					AddError(Error);
					return false;
				}
				// Same mapping as FAnimNode_KawaiiAutoClothingProxy with Follow Live Root enabled.
				if (Variant.bReanchor) {
					KawaiiClothingProxy::ReanchorCapsules(Capsules, RecordedRoot, Output.Pose[FCompactPoseBoneIndex(0)] * Snap.RelativeTransform);
				}
			}
			Correction.ExternalCapsules = Capsules;
			Correction.Alpha = 1.f;
			Correction.AccumulateDeltaTime(DeltaTime);
			Correction.CorrectPose(Output, Base);
			Reasons.FindOrAdd(Correction.GetCorrectionDebugReason())++;
			MaxPenetration = FMath::Max(MaxPenetration, Correction.GetMaxPenetration());
			SumPenetration += Correction.GetMaxPenetration();
			MaxLengthError = FMath::Max(MaxLengthError, Correction.GetMaxLengthError());
			TArray<FTransform> CorrectedCS, BaseCS;
			ComponentSpace(Output.Pose, CorrectedCS);
			ComponentSpace(Base.Pose, BaseCS);
			TArray<FVector>& FrameOffsets = Offsets.AddDefaulted_GetRef();
			TArray<FVector>& FramePositions = Positions.AddDefaulted_GetRef();
			if (VariantIndex == 0) {
				Baseline.AddDefaulted();
			}
			for (const int32 Bone : ChainBones) {
				FramePositions.Add(CorrectedCS[Bone].GetTranslation());
				FrameOffsets.Add(CorrectedCS[Bone].GetTranslation() - BaseCS[Bone].GetTranslation());
				if (VariantIndex == 0) {
					Baseline.Last().Add(BaseCS[Bone].GetTranslation());
				}
			}
		}
		// Second difference of the correction offset: smooth corrections stay near zero.
		TArray<double> Jitter;
		TArray<double> PerBone;
		PerBone.Init(0.0, ChainBones.Num());
		double MaxOffset = 0.0, SumOffset = 0.0;
		int32 Flips = 0, Jumps = 0, Reversals = 0;
		for (int32 Frame = 0; Frame < NumFrames; ++Frame) {
			for (int32 Bone = 0; Bone < ChainBones.Num(); ++Bone) {
				const double Size = Offsets[Frame][Bone].Size();
				MaxOffset = FMath::Max(MaxOffset, Size);
				SumOffset += Size;
				if (Frame > 0) {
					const FVector& Previous = Offsets[Frame - 1][Bone];
					Flips += Size > 1.0 && Previous.Size() > 1.0 && FVector::DotProduct(Offsets[Frame][Bone], Previous) < 0.0 ? 1 : 0;
					Jumps += (Offsets[Frame][Bone] - Previous).Size() > 5.0 ? 1 : 0;
				}
				if (Frame > 1) {
					// Visible shaking: the displayed point reverses its motion between consecutive frames at a noticeable speed.
					const FVector V = Positions[Frame][Bone] - Positions[Frame - 1][Bone];
					const FVector U = Positions[Frame - 1][Bone] - Positions[Frame - 2][Bone];
					Reversals += V.Size() > 0.5 && U.Size() > 0.5 && FVector::DotProduct(V, U) < 0.0 ? 1 : 0;
				}
				if (Frame > 0 && Frame + 1 < NumFrames) {
					const double J = (Offsets[Frame + 1][Bone] - 2.0 * Offsets[Frame][Bone] + Offsets[Frame - 1][Bone]).Size();
					Jitter.Add(J);
					PerBone[Bone] += J;
				}
			}
		}
		Jitter.Sort();
		const auto Percentile = [&Jitter](double P) { return Jitter.IsEmpty() ? 0.0 : Jitter[FMath::Clamp(int32(P * (Jitter.Num() - 1)), 0, Jitter.Num() - 1)]; };
		double JitterSum = 0.0;
		for (const double J : Jitter) {
			JitterSum += J;
		}
		TArray<int32> Order;
		for (int32 Bone = 0; Bone < ChainBones.Num(); ++Bone) {
			Order.Add(Bone);
		}
		Order.Sort([&PerBone](int32 A, int32 B) { return PerBone[A] > PerBone[B]; });
		FString Top;
		for (int32 Index = 0; Index < FMath::Min(6, Order.Num()); ++Index) {
			Top += FString::Printf(TEXT("%s\"%s\":%.3f"), Index ? TEXT(",") : TEXT(""), *ChainBoneNames[Order[Index]], PerBone[Order[Index]] / FMath::Max(1, NumFrames - 2));
		}
		FString ReasonText;
		for (const TPair<FString, int32>& Pair : Reasons) {
			ReasonText += FString::Printf(TEXT("%s\"%s\":%d"), ReasonText.IsEmpty() ? TEXT("") : TEXT(","), *Pair.Key, Pair.Value);
		}
		Json += FString::Printf(TEXT("%s\"%s\":{\"jitter_mean\":%.4f,\"jitter_p95\":%.4f,\"jitter_p99\":%.4f,\"jitter_max\":%.3f,\"direction_flips\":%d,\"jumps_over_5cm\":%d,\"reversals\":%d,")
								TEXT("\"offset_mean\":%.3f,\"offset_max\":%.2f,\"penetration_mean\":%.3f,\"penetration_max\":%.3f,\"length_error_max\":%.4f,\"reasons\":{%s},\"top_bones\":{%s}}\n"),
								VariantIndex ? TEXT(",") : TEXT(""), Variant.Name, Jitter.IsEmpty() ? 0.0 : JitterSum / Jitter.Num(), Percentile(0.95), Percentile(0.99),
								Jitter.IsEmpty() ? 0.0 : Jitter.Last(), Flips, Jumps, Reversals, SumOffset / FMath::Max(1, NumFrames * ChainBones.Num()), MaxOffset,
								SumPenetration / NumFrames, MaxPenetration, MaxLengthError, *ReasonText, *Top);
	}
	// Reference: how much the uncorrected baked hair itself accelerates and reverses.
	{
		TArray<double> Jitter;
		int32 Reversals = 0;
		for (int32 Frame = 1; Frame + 1 < NumFrames; ++Frame) {
			for (int32 Bone = 0; Bone < ChainBones.Num(); ++Bone) {
				Jitter.Add((Baseline[Frame + 1][Bone] - 2.0 * Baseline[Frame][Bone] + Baseline[Frame - 1][Bone]).Size());
				const FVector V = Baseline[Frame + 1][Bone] - Baseline[Frame][Bone];
				const FVector U = Baseline[Frame][Bone] - Baseline[Frame - 1][Bone];
				Reversals += V.Size() > 0.5 && U.Size() > 0.5 && FVector::DotProduct(V, U) < 0.0 ? 1 : 0;
			}
		}
		Jitter.Sort();
		double Sum = 0.0;
		for (const double J : Jitter) {
			Sum += J;
		}
		Json += FString::Printf(TEXT("},\n\"baked_hair_reference\":{\"accel_mean\":%.4f,\"accel_p95\":%.4f,\"accel_max\":%.3f,\"reversals\":%d}\n}\n"),
								Sum / FMath::Max(1, Jitter.Num()), Jitter.IsEmpty() ? 0.0 : Jitter[int32(0.95 * (Jitter.Num() - 1))], Jitter.IsEmpty() ? 0.0 : Jitter.Last(), Reversals);
	}
	const FString Path = FPaths::ProjectSavedDir() / TEXT("DSHInspect/jitter_report.json");
	TestTrue(TEXT("Report written"), FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	return true;
}

#endif
