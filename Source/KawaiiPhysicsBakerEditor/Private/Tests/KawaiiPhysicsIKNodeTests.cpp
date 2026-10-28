#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/StrongObjectPtr.h"

namespace KawaiiIKNodeTests
{
	struct FPoseSource : FAnimNode_Base
	{
		TArray<FTransform> Transforms;
		virtual void Evaluate_AnyThread(FPoseContext& Output) override {
			Output.ResetToRefPose();
			for (FCompactPoseBoneIndex Index : Output.Pose.ForEachBoneIndex()) {
				Output.Pose[Index] = Transforms[Index.GetInt()];
			}
		}
	};

	struct FFixture
	{
		TStrongObjectPtr<USkeleton> Skeleton{NewObject<USkeleton>()};
		TStrongObjectPtr<USkeletalMesh> Mesh{NewObject<USkeletalMesh>()};
		TStrongObjectPtr<USkeletalMeshComponent> Component{NewObject<USkeletalMeshComponent>()};
		TStrongObjectPtr<UAnimInstance> Instance;
		TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile> Profile{NewObject<UKawaiiPhysicsIKCorrectionProfile>()};
		TUniquePtr<FAnimInstanceProxy> Proxy;
		FPoseSource Baked, Driver;
		FAnimNode_KawaiiPhysicsIKCorrection Node;

		FFixture() {
			{
				FReferenceSkeletonModifier Modifier(Skeleton.Get());
				const auto Add = [&Modifier](const TCHAR* Name, int32 Parent, FVector Position) { Modifier.Add(FMeshBoneInfo(FName(Name), FString(Name), Parent), FTransform(Position)); };
				Add(TEXT("root"), INDEX_NONE, FVector::ZeroVector);
				Add(TEXT("Hips"), 0, FVector(0, 0, 100));
				Add(TEXT("UpperLeg_L"), 1, FVector(0, -8, -8));
				Add(TEXT("LowerLeg_L"), 2, FVector(0, 0, -35));
				Add(TEXT("UpperLeg_R"), 1, FVector(0, 8, -8));
				Add(TEXT("skirt_root"), 1, FVector(10, -10, 0));
				Add(TEXT("skirt_mid"), 5, FVector(0, 0, -15));
				Add(TEXT("skirt_tip"), 6, FVector(0, 0, -15));
				Add(TEXT("skirt_side"), 6, FVector(5, 0, -2));
			}
			Mesh->SetSkeleton(Skeleton.Get());
			Mesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
			Component->SetSkeletalMesh(Mesh.Get());
			Instance.Reset(NewObject<UAnimInstance>(Component.Get()));
			Proxy = MakeUnique<FAnimInstanceProxy>(Instance.Get());
			TArray<FBoneIndexType> Indices;
			for (int32 Bone = 0; Bone < Mesh->GetRefSkeleton().GetNum(); ++Bone) {
				Indices.Add(Bone);
			}
			const UE::Anim::FCurveFilterSettings Filter(UE::Anim::ECurveFilterMode::None);
			Proxy->GetRequiredBones().InitializeTo(Indices, Filter, *Mesh);
			Instance->GetRequiredBones().InitializeTo(Indices, Filter, *Mesh);
			Baked.Transforms = Mesh->GetRefSkeleton().GetRefBonePose();
			// A twist in the stored garment must survive an identity correction.
			Baked.Transforms[6].SetRotation(FQuat(FVector::UpVector, FMath::DegreesToRadians(20.f)));
			Driver.Transforms = Mesh->GetRefSkeleton().GetRefBonePose();
			auto& Chain = Profile->Chains.AddDefaulted_GetRef();
			Chain.RootBone.BoneName = TEXT("skirt_root");
			Chain.TipBone.BoneName = TEXT("skirt_tip");
			Chain.DriverBone.BoneName = TEXT("UpperLeg_L");
			Chain.DriverWeight = 0.6f;
			Node.Profile = Profile.Get();
			Node.BakedPose.SetLinkNode(&Baked);
			Node.DriverPose.SetLinkNode(&Driver);
			Node.Initialize_AnyThread(FAnimationInitializeContext(Proxy.Get()));
			Node.CacheBones_AnyThread(FAnimationCacheBonesContext(Proxy.Get()));
		}

		TArray<FTransform> Evaluate() {
			Node.PreUpdate(Instance.Get());
			FMemMark Mark(FMemStack::Get());
			FPoseContext Output(Proxy.Get());
			Node.Evaluate_AnyThread(Output);
			TArray<FTransform> Result;
			for (FCompactPoseBoneIndex Index : Output.Pose.ForEachBoneIndex()) {
				Result.Add(Output.Pose[Index]);
			}
			return Result;
		}
		TArray<FTransform> ToCS(const TArray<FTransform>& Local) const {
			TArray<FTransform> CS = Local;
			for (int32 Bone = 0; Bone < CS.Num(); ++Bone) {
				const int32 Parent = Mesh->GetRefSkeleton().GetParentIndex(Bone);
				if (Parent != INDEX_NONE) {
					CS[Bone] = Local[Bone] * CS[Parent];
				}
			}
			return CS;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKNodeComposition, "KawaiiPhysicsBaker.IKCorrection.NodePoseComposition", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FKawaiiIKNodeComposition::RunTest(const FString&) {
	using namespace KawaiiIKNodeTests;
	FFixture Fixture;
	const TArray<FTransform> Neutral = Fixture.Evaluate();
	for (int32 Bone = 0; Bone < Neutral.Num(); ++Bone) {
		const FTransform& Expected = Bone >= 5 ? Fixture.Baked.Transforms[Bone] : Fixture.Driver.Transforms[Bone];
		TestTrue(FString::Printf(TEXT("No IK preserves bone %d including garment twist"), Bone), Neutral[Bone].Equals(Expected, 1.e-4));
	}
	const FVector Offset(7, -3, -12);
	Fixture.Driver.Transforms[1].AddToTranslation(Offset);
	const TArray<FTransform> Moved = Fixture.Evaluate();
	const TArray<FTransform> NeutralCS = Fixture.ToCS(Neutral), MovedCS = Fixture.ToCS(Moved);
	for (int32 Bone = 1; Bone < Moved.Num(); ++Bone) {
		TestTrue(FString::Printf(TEXT("Pelvis applied exactly once to bone %d"), Bone), MovedCS[Bone].GetTranslation().Equals(NeutralCS[Bone].GetTranslation() + Offset, 0.001));
	}
	Fixture.Driver.Transforms[2].SetRotation(FQuat(FVector::RightVector, FMath::DegreesToRadians(45.f)));
	const TArray<FTransform> Raised = Fixture.Evaluate();
	for (int32 Bone = 0; Bone < 5; ++Bone) {
		TestTrue(TEXT("Correction never overwrites live body IK"), Raised[Bone].Equals(Fixture.Driver.Transforms[Bone], 0.0001));
	}
	TestFalse(TEXT("The garment responds to the raised leg"), Fixture.ToCS(Raised)[7].Equals(MovedCS[7], 0.001));
	TestTrue(TEXT("Side branches preserve authored local animation"), Raised[8].Equals(Fixture.Baked.Transforms[8], 0.0001));
	TestTrue(TEXT("Valid configuration reports a corrected pose"), Fixture.Node.GetCorrectionDebugReason().StartsWith(TEXT("corrected")));
	Fixture.Node.Alpha = 0.f;
	const TArray<FTransform> Disabled = Fixture.Evaluate();
	for (int32 Bone = 5; Bone < Disabled.Num(); ++Bone) {
		TestTrue(TEXT("Alpha zero preserves baked local motion under live anchors"), Disabled[Bone].Equals(Fixture.Baked.Transforms[Bone], 0.0001));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKNodeInvalidProfile, "KawaiiPhysicsBaker.IKCorrection.InvalidProfilePreservesBody", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FKawaiiIKNodeInvalidProfile::RunTest(const FString&) {
	KawaiiIKNodeTests::FFixture Fixture;
	Fixture.Driver.Transforms[1].AddToTranslation(FVector(0, 0, -9));
	Fixture.Profile->Chains[0].TipBone.BoneName = TEXT("not_in_this_lod");
	const TArray<FTransform> Missing = Fixture.Evaluate();
	TestEqual(TEXT("Missing bone is diagnosed"), Fixture.Node.GetCorrectionDebugReason(), FString(TEXT("chain_bone_missing_at_current_lod")));
	for (int32 Bone = 0; Bone < Missing.Num(); ++Bone) {
		TestTrue(TEXT("Invalid profile leaves the entire driver pose intact"), Missing[Bone].Equals(Fixture.Driver.Transforms[Bone]));
	}
	Fixture.Profile->Chains[0].TipBone.BoneName = TEXT("skirt_tip");
	const FKawaiiPhysicsIKCorrectionChain Duplicate = Fixture.Profile->Chains[0];
	Fixture.Profile->Chains.Add(Duplicate);
	Fixture.Evaluate();
	TestEqual(TEXT("Overlapping roots are rejected"), Fixture.Node.GetCorrectionDebugReason(), FString(TEXT("overlapping_corrected_subtrees")));
	return true;
}

#endif
