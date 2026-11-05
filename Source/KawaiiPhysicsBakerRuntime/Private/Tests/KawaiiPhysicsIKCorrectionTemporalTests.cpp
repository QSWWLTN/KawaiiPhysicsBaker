#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

#include "AnimNode_KawaiiPhysicsIKCorrection.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace KawaiiIKTemporalTests {
	constexpr int32 SwingBone = 2;
	constexpr int32 TipBone = 4;

	struct FRunResult
	{
		double FinalTipX = 0.0;
		double MaxTipStep = 0.0;
		double FinalPenetration = 0.0;
		double FinalLengthError = 0.0;
		FString FinalReason;
		double GapTipX = 0.0;
	};

	FVector TipPosition(const FCompactPose& Pose) {
		const FBoneContainer& Bones = Pose.GetBoneContainer();
		FTransform Result = FTransform::Identity;
		for (FCompactPoseBoneIndex Bone(TipBone); Bone.GetInt() != INDEX_NONE; Bone = Bones.GetParentBoneIndex(Bone)) {
			Result = Result * Pose[Bone];
		}
		return Result.GetTranslation();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiIKTemporalStabilityTest,
	"KawaiiPhysicsBaker.IKCorrection.TemporalStability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiIKTemporalStabilityTest::RunTest(const FString&) {
	using namespace KawaiiIKTemporalTests;
	// A three-bone strand hangs from an anchor at z=100 and crosses a horizontal collar capsule at z=85.
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>());
	{
		FReferenceSkeletonModifier Modifier(Skeleton.Get());
		Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("Anchor")), TEXT("Anchor"), 0), FTransform(FVector(0, 0, 100)));
		Modifier.Add(FMeshBoneInfo(FName(TEXT("Hair0")), TEXT("Hair0"), 1), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(FName(TEXT("Hair1")), TEXT("Hair1"), 2), FTransform(FVector(0, 0, -10)));
		Modifier.Add(FMeshBoneInfo(FName(TEXT("Hair2")), TEXT("Hair2"), 3), FTransform(FVector(0, 0, -10)));
	}
	TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>());
	Mesh->SetSkeleton(Skeleton.Get());
	Mesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	Component->SetSkeletalMesh(Mesh.Get());
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get()));
	TArray<FBoneIndexType> BoneIndices;
	for (int32 Bone = 0; Bone < Mesh->GetRefSkeleton().GetNum(); ++Bone) {
		BoneIndices.Add(static_cast<FBoneIndexType>(Bone));
	}
	const UE::Anim::FCurveFilterSettings CurveFilter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh.Get());
	FAnimInstanceProxy Proxy(Instance.Get());
	Proxy.GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh.Get());

	FKawaiiPhysicsIKCorrectionChain Chain;
	Chain.RootBone.BoneName = TEXT("Hair0");
	Chain.TipBone.BoneName = TEXT("Hair2");
	Chain.DriverBone.BoneName = TEXT("root");
	Chain.DriverWeight = 0.f;
	Chain.CollisionRadius = 1.f;
	TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile> Temporal(NewObject<UKawaiiPhysicsIKCorrectionProfile>());
	Temporal->Chains = {Chain};
	Temporal->Iterations = 32;
	Temporal->SettleTime = 0.08f;
	Temporal->SmoothingTime = 0.03f;
	TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile> Stateless(DuplicateObject<UKawaiiPhysicsIKCorrectionProfile>(Temporal.Get(), GetTransientPackage()));
	Stateless->SettleTime = Stateless->SmoothingTime = 0.f;
	FKawaiiClothingProxyCapsule Collar;
	Collar.Id = TEXT("Collar");
	Collar.Start = FVector(0, -20, 85);
	Collar.End = FVector(0, 20, 85);
	Collar.Radius = 2.f;

	// The input carries the strand across the capsule axis (never exactly onto it), then holds still.
	TArray<double> Sweep{-2.5, -1.5, -0.5, 0.5, 1.5, 2.5};
	for (int32 Hold = 0; Hold < 24; ++Hold) {
		Sweep.Add(2.5);
	}
	const auto Run = [&](UKawaiiPhysicsIKCorrectionProfile* InProfile, float InDeltaTime, float InGapSeconds) {
		FRunResult Result;
		FAnimNode_KawaiiPhysicsIKCorrection Correction;
		Correction.Profile = InProfile;
		Correction.ExternalCapsules = {Collar};
		Correction.PreUpdate(Instance.Get());
		const auto Solve = [&](double Degrees, float DeltaTime) {
			FMemMark Mark(FMemStack::Get());
			FPoseContext Output(&Proxy);
			Output.Pose.ResetToRefPose();
			Output.Pose[FCompactPoseBoneIndex(SwingBone)].SetRotation(FQuat(FVector::YAxisVector, FMath::DegreesToRadians(Degrees)));
			FPoseContext Baked(Output);
			Baked.Pose.CopyBonesFrom(Output.Pose);
			Correction.AccumulateDeltaTime(DeltaTime);
			Correction.CorrectPose(Output, Baked);
			return TipPosition(Output.Pose);
		};
		TOptional<FVector> PreviousTip;
		for (const double Degrees : Sweep) {
			const FVector Tip = Solve(Degrees, InDeltaTime);
			if (PreviousTip.IsSet()) {
				Result.MaxTipStep = FMath::Max(Result.MaxTipStep, FVector::Distance(Tip, PreviousTip.GetValue()));
			}
			PreviousTip = Tip;
			Result.FinalTipX = Tip.X;
			Result.FinalPenetration = Correction.GetMaxPenetration();
			Result.FinalLengthError = Correction.GetMaxLengthError();
			Result.FinalReason = Correction.GetCorrectionDebugReason();
		}
		if (InGapSeconds > 0.f) {
			Result.GapTipX = Solve(Sweep.Last(), InGapSeconds).X;
		}
		return Result;
	};

	const FRunResult Flipping = Run(Stateless.Get(), 1.f / 60.f, 0.f);
	const FRunResult Stable = Run(Temporal.Get(), 1.f / 60.f, 0.5f);
	const FRunResult Untimed = Run(Temporal.Get(), 0.f, 0.f);
	AddInfo(FString::Printf(
		TEXT("Stateless tip x %.3f, max step %.3f; temporal tip x %.3f, max step %.3f, gap tip x %.3f"),
		Flipping.FinalTipX,
		Flipping.MaxTipStep,
		Stable.FinalTipX,
		Stable.MaxTipStep,
		Stable.GapTipX
	));

	TestTrue(TEXT("Stateless correction resolves the contact"), Flipping.FinalReason.StartsWith(TEXT("corrected")) && Flipping.FinalPenetration < 0.1);
	TestTrue(TEXT("Stateless correction flips the strand to the other side of the capsule"), Flipping.FinalTipX < 0.0);
	TestTrue(TEXT("The stateless flip is a visible jump"), Flipping.MaxTipStep > 3.0);
	TestTrue(TEXT("Temporal correction keeps the strand on its original side"), Stable.FinalTipX > 0.0);
	TestTrue(TEXT("Temporal correction has no frame-to-frame jump"), Stable.MaxTipStep < 1.5);
	TestTrue(
		FString::Printf(TEXT("Temporal correction still fully resolves the held contact (%s)"), *Stable.FinalReason),
		Stable.FinalPenetration < 0.1 && Stable.FinalLengthError < 0.05
	);
	TestTrue(TEXT("A long gap restarts from a stateless solve"), Stable.GapTipX < 0.0);
	TestTrue(TEXT("Without reported time the solve stays stateless"), FMath::IsNearlyEqual(Untimed.FinalTipX, Flipping.FinalTipX, 1.e-6));
	return true;
}

#endif
