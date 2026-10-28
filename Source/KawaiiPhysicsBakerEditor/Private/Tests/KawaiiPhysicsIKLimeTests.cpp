#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace KawaiiIKLimeTests
{
	struct FPoseSource : FAnimNode_Base
	{
		TArray<FTransform> Transforms;
		virtual void Evaluate_AnyThread(FPoseContext& Output) override {
			Output.ResetToRefPose();
			const FBoneContainer& Required = Output.Pose.GetBoneContainer();
			for (FCompactPoseBoneIndex Index : Output.Pose.ForEachBoneIndex()) {
				Output.Pose[Index] = Transforms[Required.MakeMeshPoseIndex(Index).GetInt()];
			}
		}
	};

	FString ObjectPath(FString Path) {
		if (!Path.Contains(TEXT("."))) {
			Path += TEXT(".") + FPaths::GetCleanFilename(Path);
		}
		return Path;
	}

	bool InSubtree(int32 Bone, int32 Root, const FReferenceSkeleton& Skeleton) {
		for (; Bone != INDEX_NONE; Bone = Skeleton.GetParentIndex(Bone)) {
			if (Bone == Root) {
				return true;
			}
		}
		return false;
	}

	TArray<FTransform> ToCS(const TArray<FTransform>& Local, const FReferenceSkeleton& Skeleton) {
		TArray<FTransform> Result = Local;
		for (int32 Bone = 0; Bone < Result.Num(); ++Bone) {
			const int32 Parent = Skeleton.GetParentIndex(Bone);
			if (Parent != INDEX_NONE) {
				Result[Bone] = Local[Bone] * Result[Parent];
			}
			Result[Bone].NormalizeRotation();
		}
		return Result;
	}

	struct FFixture
	{
		TStrongObjectPtr<USkeletalMesh> Mesh;
		TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile> Profile;
		TStrongObjectPtr<USkeletalMeshComponent> Component{NewObject<USkeletalMeshComponent>()};
		TStrongObjectPtr<UAnimInstance> Instance;
		TUniquePtr<FAnimInstanceProxy> Proxy;
		FPoseSource Baked, Driver;
		FAnimNode_KawaiiPhysicsIKCorrection Node;

		FFixture(USkeletalMesh* InMesh, UKawaiiPhysicsIKCorrectionProfile* InProfile) : Mesh(InMesh), Profile(DuplicateObject<UKawaiiPhysicsIKCorrectionProfile>(InProfile, GetTransientPackage())) {
			// This test supplies synthetic post-IK poses on the real clothing mesh.
			// It intentionally does not simulate an attach parent or a gameplay world.
			Profile->bUseAttachedParentForCollision = false;
			Profile->SetFlags(RF_Transient);
			Component->SetSkeletalMesh(InMesh);
			Instance.Reset(NewObject<UAnimInstance>(Component.Get()));
			Proxy = MakeUnique<FAnimInstanceProxy>(Instance.Get());
			TArray<FBoneIndexType> Indices;
			for (int32 Bone = 0; Bone < InMesh->GetRefSkeleton().GetNum(); ++Bone) {
				Indices.Add(Bone);
			}
			const UE::Anim::FCurveFilterSettings Filter(UE::Anim::ECurveFilterMode::None);
			Proxy->GetRequiredBones().InitializeTo(Indices, Filter, *InMesh);
			Instance->GetRequiredBones().InitializeTo(Indices, Filter, *InMesh);
			Baked.Transforms = InMesh->GetRefSkeleton().GetRefBonePose();
			Driver.Transforms = Baked.Transforms;
			Node.Profile = Profile.Get();
			Node.BakedPose.SetLinkNode(&Baked);
			Node.DriverPose.SetLinkNode(&Driver);
			Node.Initialize_AnyThread(FAnimationInitializeContext(Proxy.Get()));
			Node.CacheBones_AnyThread(FAnimationCacheBonesContext(Proxy.Get()));
		}

		TArray<FTransform> Evaluate(float Alpha) {
			Node.Alpha = Alpha;
			Node.PreUpdate(Instance.Get());
			FMemMark Mark(FMemStack::Get());
			FPoseContext Output(Proxy.Get());
			Node.Evaluate_AnyThread(Output);
			TArray<FTransform> Result = Mesh->GetRefSkeleton().GetRefBonePose();
			const FBoneContainer& Required = Output.Pose.GetBoneContainer();
			for (FCompactPoseBoneIndex Index : Output.Pose.ForEachBoneIndex()) {
				Result[Required.MakeMeshPoseIndex(Index).GetInt()] = Output.Pose[Index];
			}
			return Result;
		}
	};

	struct FSweepCase
	{
		FString Name;
		FVector Axis;
		double LeftDegrees;
		double RightDegrees;
		double PelvisZ;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKLimePoseSweep, "KawaiiPhysicsBaker.IKCorrection.LimeBasePoseSweep", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiIKLimePoseSweep::RunTest(const FString&) {
	using namespace KawaiiIKLimeTests;
	FString ProfilePath, MeshPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("KawaiiIKTestProfile="), ProfilePath) || !FParse::Value(FCommandLine::Get(), TEXT("KawaiiIKTestMesh="), MeshPath)) {
		AddInfo(TEXT("Real-mesh sweep requires -KawaiiIKTestProfile and -KawaiiIKTestMesh; no content dependency is built into the plugin."));
		return true;
	}
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *ObjectPath(MeshPath));
	UKawaiiPhysicsIKCorrectionProfile* Profile = LoadObject<UKawaiiPhysicsIKCorrectionProfile>(nullptr, *ObjectPath(ProfilePath));
	if (!TestNotNull(TEXT("Actual clothing mesh"), Mesh) || !TestNotNull(TEXT("Actual correction profile"), Profile)) {
		return false;
	}
	if (!TestTrue(TEXT("The real profile defines clothing chains and body capsules"), !Profile->Chains.IsEmpty() && !Profile->Capsules.IsEmpty())) {
		return false;
	}

	const FReferenceSkeleton& Skeleton = Mesh->GetRefSkeleton();
	const int32 Hips = Skeleton.FindBoneIndex(TEXT("Hips"));
	const int32 Left = Skeleton.FindBoneIndex(TEXT("UpperLeg_L"));
	const int32 Right = Skeleton.FindBoneIndex(TEXT("UpperLeg_R"));
	if (!TestTrue(TEXT("The mesh has the actual Lime body drivers"), Hips != INDEX_NONE && Left != INDEX_NONE && Right != INDEX_NONE)) {
		return false;
	}
	TArray<int32> Roots;
	TArray<uint8> Affected;
	Affected.Init(0, Skeleton.GetNum());
	for (const FKawaiiPhysicsIKCorrectionChain& Chain : Profile->Chains) {
		const int32 Root = Skeleton.FindBoneIndex(Chain.RootBone.BoneName);
		if (!TestTrue(TEXT("Each configured root exists and has an attachment parent"), Root != INDEX_NONE && Skeleton.GetParentIndex(Root) != INDEX_NONE)) {
			return false;
		}
		Roots.Add(Root);
		for (int32 Bone = 0; Bone < Affected.Num(); ++Bone) {
			if (InSubtree(Bone, Root, Skeleton)) {
				Affected[Bone] = 1;
			}
		}
	}

	FFixture Fixture(Mesh, Profile);
	const TArray<FTransform> Reference = Skeleton.GetRefBonePose();
	TArray<FSweepCase> Cases;
	Cases.Add({TEXT("reference"), FVector::XAxisVector, 0.0, 0.0, 0.0});
	Cases.Add({TEXT("pelvis_lowered"), FVector::XAxisVector, 0.0, 0.0, -8.0});
	for (const FVector Axis : {FVector::XAxisVector, FVector::ZAxisVector}) {
		const TCHAR* AxisName = Axis.X > 0.0 ? TEXT("X") : TEXT("Z");
		for (const double Angle : {-40.0, -20.0, 20.0, 40.0}) {
			Cases.Add({FString::Printf(TEXT("left_%s_%+.0f"), AxisName, Angle), Axis, Angle, 0.0, -8.0});
			Cases.Add({FString::Printf(TEXT("right_%s_%+.0f"), AxisName, Angle), Axis, 0.0, Angle, -8.0});
			Cases.Add({FString::Printf(TEXT("split_%s_%+.0f"), AxisName, Angle), Axis, Angle, -Angle, -8.0});
		}
	}

	FString CSV = TEXT("case,axis_x,axis_y,axis_z,left_degrees,right_degrees,pelvis_z_cm,baseline_penetration_cm,corrected_penetration_cm,improvement_cm,corrected_length_error_cm,debug_reason\n");
	bool bAnyImproved = false;
	double LargestImprovement = 0.0;
	FString BestCase;
	for (const FSweepCase& Case : Cases) {
		Fixture.Driver.Transforms = Reference;
		Fixture.Driver.Transforms[Hips].AddToTranslation(FVector(0, 0, Case.PelvisZ));
		// The offsets are authored in the real thigh's local axes. They exercise
		// synthetic IK variations; they are not ground traces or a Control Rig solve.
		Fixture.Driver.Transforms[Left].SetRotation(Reference[Left].GetRotation() * FQuat(Case.Axis, FMath::DegreesToRadians(Case.LeftDegrees)));
		Fixture.Driver.Transforms[Right].SetRotation(Reference[Right].GetRotation() * FQuat(Case.Axis, FMath::DegreesToRadians(Case.RightDegrees)));
		Fixture.Driver.Transforms[Left].NormalizeRotation();
		Fixture.Driver.Transforms[Right].NormalizeRotation();

		Fixture.Evaluate(0.f);
		const double Before = Fixture.Node.GetMaxPenetration();
		if (!TestTrue(Case.Name + TEXT(": baseline is a composed pose, not fallback"), Fixture.Node.GetCorrectionDebugReason().StartsWith(TEXT("corrected")))) {
			AddError(Fixture.Node.GetCorrectionDebugReason());
			return false;
		}
		const TArray<FTransform> Corrected = Fixture.Evaluate(1.f);
		const double After = Fixture.Node.GetMaxPenetration();
		const double LengthError = Fixture.Node.GetMaxLengthError();
		const FString Reason = Fixture.Node.GetCorrectionDebugReason();
		TestTrue(Case.Name + TEXT(": configured correction evaluates without fallback"), Reason.StartsWith(TEXT("corrected")));
		const TArray<FTransform> Repeated = Fixture.Evaluate(1.f);
		const TArray<FTransform> CorrectedCS = ToCS(Corrected, Skeleton);
		const TArray<FTransform> DriverCS = ToCS(Fixture.Driver.Transforms, Skeleton);
		for (int32 Bone = 0; Bone < Corrected.Num(); ++Bone) {
			if (!Corrected[Bone].IsValid()) {
				AddError(FString::Printf(TEXT("%s: bone %s is not finite/normalized"), *Case.Name, *Skeleton.GetBoneName(Bone).ToString()));
				return false;
			}
			if (!Affected[Bone] && !Corrected[Bone].Equals(Fixture.Driver.Transforms[Bone], 1.e-4)) {
				AddError(FString::Printf(TEXT("%s: body bone %s was overwritten"), *Case.Name, *Skeleton.GetBoneName(Bone).ToString()));
				return false;
			}
			if (!Corrected[Bone].Equals(Repeated[Bone], 1.e-5)) {
				AddError(FString::Printf(TEXT("%s: non-repeatable bone %s"), *Case.Name, *Skeleton.GetBoneName(Bone).ToString()));
				return false;
			}
		}
		for (const int32 Root : Roots) {
			const FTransform Attachment = Fixture.Baked.Transforms[Root] * DriverCS[Skeleton.GetParentIndex(Root)];
			TestTrue(Case.Name + TEXT(": root stays at its live attachment ") + Skeleton.GetBoneName(Root).ToString(), CorrectedCS[Root].GetTranslation().Equals(Attachment.GetTranslation(), 0.001));
		}
		const double Improvement = Before - After;
		if (Before > 0.05 && Improvement > 0.01) {
			bAnyImproved = true;
		}
		if (Improvement > LargestImprovement) {
			LargestImprovement = Improvement;
			BestCase = Case.Name;
		}
		CSV += FString::Printf(TEXT("%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.6f,%.6f,%.6f,%.6f,%s\n"),
							   *Case.Name,
							   Case.Axis.X,
							   Case.Axis.Y,
							   Case.Axis.Z,
							   Case.LeftDegrees,
							   Case.RightDegrees,
							   Case.PelvisZ,
							   Before,
							   After,
							   Improvement,
							   LengthError,
							   *Reason);
	}

	FString CSVPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("KawaiiIKTestCsv="), CSVPath)) {
		CSVPath = FPaths::ProjectSavedDir() / TEXT("KawaiiPhysicsIKCorrectionAudit/pose_sweep.csv");
	}
	CSVPath = FPaths::ConvertRelativePathToFull(CSVPath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(CSVPath), true);
	TestTrue(TEXT("The measured pose sweep is written as CSV"), FFileHelper::SaveStringToFile(CSV, *CSVPath));
	TestTrue(TEXT("At least one colliding real-mesh pose reduces maximum bone/capsule penetration"), bAnyImproved);
	AddInfo(FString::Printf(TEXT("Synthetic IK sweep on actual clothing mesh: %d cases, best maximum-penetration reduction %.3f cm (%s). Results: %s. This measures bone-chain/capsule contact, not "
								 "PIE or skinned-triangle clearance."),
							Cases.Num(),
							LargestImprovement,
							*BestCase,
							*CSVPath));
	return true;
}

#endif
