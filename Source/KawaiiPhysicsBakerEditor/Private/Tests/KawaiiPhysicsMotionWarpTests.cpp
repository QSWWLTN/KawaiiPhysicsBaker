#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Animation/AttributesRuntime.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"

#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "KawaiiClothingProxySnapshot.h"
#include "KawaiiClothingProxyPlayback.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiClimbRootExtractionTest,
	"KawaiiPhysicsBaker.MotionWarp.ProjectClimbRootExtraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiClimbRootExtractionTest::RunTest(const FString&) {
	// Optional project fixture. Read the currently equipped proxy, never a hard-coded bake group.
	const TCHAR* SetPath = TEXT("/Game/Assets/Clothing/Base/BakeKawaiiPhysics/Cloting_Base_ProxySet");
	if (!FPackageName::DoesPackageExist(SetPath)) {
		AddWarning(TEXT("Project climb fixture unavailable; root extraction integration check skipped."));
		return true;
	}
	UKawaiiClothingProxyMotionSet* Set = LoadObject<UKawaiiClothingProxyMotionSet>(nullptr, SetPath);
	UAnimSequence* Body = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/Jump/Jump_Climb_2h"));
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Anime/Lime/Lime"));
	UKawaiiPhysicsIKCorrectionProfile* Profile =
		LoadObject<UKawaiiPhysicsIKCorrectionProfile>(nullptr, TEXT("/Game/BP/AnimeBP/Lime/DA_AnimBP_Lime_IKCorrection"));
	if (!TestTrue(TEXT("Current climb assets load"), Set && Body && Mesh && Profile)) {
		return false;
	}
	UKawaiiClothingProxyMotion* Motion = nullptr;
	for (UKawaiiClothingProxyMotion* Candidate : Set->Motions) {
		if (Candidate && Candidate->SourceAnimation.ToSoftObjectPath() == FSoftObjectPath(Body)) {
			Motion = Candidate;
			break;
		}
	}
	if (!TestNotNull(TEXT("Current climb proxy mapping"), Motion)) {
		return false;
	}
	TestTrue(TEXT("Replacement proxy uses the original body animation"), Motion->BodyBakedAnimation.ToSoftObjectPath() == FSoftObjectPath(Body));
	KawaiiClothingProxy::FSnapshot Snapshot;
	FString Error;
	if (!KawaiiClothingProxy::BuildSnapshot(*Motion, Snapshot, Error)) {
		AddError(Error);
		return false;
	}

	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	Component->SetSkeletalMesh(Mesh);
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get()));
	TArray<FBoneIndexType> Indices;
	for (int32 Bone = 0; Bone < Mesh->GetRefSkeleton().GetNum(); ++Bone) {
		Indices.Add(FBoneIndexType(Bone));
	}
	const UE::Anim::FCurveFilterSettings CurveFilter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(Indices, CurveFilter, *Mesh);
	FAnimInstanceProxy Proxy(Instance.Get());
	Proxy.GetRequiredBones().InitializeTo(Indices, CurveFilter, *Mesh);
	FAnimNode_KawaiiPhysicsIKCorrection PreviewCorrection, ExtractedCorrection;
	PreviewCorrection.Profile = ExtractedCorrection.Profile = Profile;
	PreviewCorrection.PreUpdate(Instance.Get());
	ExtractedCorrection.PreUpdate(Instance.Get());
	TArray<FName> ExcludedRoots;
	for (const auto& Chain : Profile->Chains) {
		ExcludedRoots.Add(Chain.RootBone.BoneName);
	}
	const auto ComponentPose = [](const FCompactPose& Pose) {
		TArray<FTransform> Result;
		for (int32 Bone = 0; Bone < Pose.GetNumBones(); ++Bone) {
			const FCompactPoseBoneIndex Index(Bone);
			const int32 Parent = Pose.GetBoneContainer().GetParentBoneIndex(Index).GetInt();
			Result.Add(Parent == INDEX_NONE ? Pose[Index] : Pose[Index] * Result[Parent]);
		}
		return Result;
	};
	double MaxRootError = 0., MaxCapsuleError = 0., MaxBoneError = 0., MaxInputError = 0., OldPlacementError = 0.;
	FString WorstBone;
	int32 WorstFrame = 0;
	double MaxIKProxyError = 0., RootOnlyIKError = 0.;
	double MinSavedTailLengthRatio = 1.;
	int32 CheckedIKEndpoints = 0;
	for (int32 Frame = 0; Frame < Snapshot.NumFrames; ++Frame) {
		FMemMark Mark(FMemStack::Get());
		const float Time = FMath::Min(float(Frame) / Snapshot.SampleRate, Snapshot.SourceDuration);
		FPoseContext Preview(&Proxy), Extracted(&Proxy);
		FAnimationPoseData PreviewData(Preview), ExtractedData(Extracted);
		Body->GetAnimationPose(PreviewData, FAnimExtractContext(double(Time), false));
		Body->GetAnimationPose(ExtractedData, FAnimExtractContext(double(Time), true));
		const FTransform PreviewRoot = Preview.Pose[FCompactPoseBoneIndex(0)];
		const FTransform LiveRoot = Extracted.Pose[FCompactPoseBoneIndex(0)];
		KawaiiClothingProxy::FLiveBodyPose PreviewBody, ExtractedBody;
		if (!KawaiiClothingProxy::BuildLiveBodyPose(Preview.Pose, ExcludedRoots, PreviewBody, Error)
			|| !KawaiiClothingProxy::BuildLiveBodyPose(Extracted.Pose, ExcludedRoots, ExtractedBody, Error))
		{
			AddError(Error);
			return false;
		}
		FTransform RecordedRoot;
		if (!KawaiiClothingProxy::SampleSnapshots(
				{{&Snapshot, Time, 1.f}},
				Snapshot.RelativeTransform,
				PreviewCorrection.ExternalCapsules,
				Error,
				&RecordedRoot,
				&PreviewBody
			))
		{
			AddError(Error);
			return false;
		}
		// Copy Pose already converted the garment root to its own component space during baking.
		// Mapping it back must recover Main's root without another garment placement.
		MaxRootError = FMath::Max(MaxRootError, FVector::Distance(RecordedRoot.GetTranslation(), PreviewRoot.GetTranslation()));
		if (!KawaiiClothingProxy::SampleSnapshots(
				{{&Snapshot, Time, 1.f}},
				Snapshot.RelativeTransform,
				ExtractedCorrection.ExternalCapsules,
				Error,
				nullptr,
				&ExtractedBody
			))
		{
			AddError(Error);
			return false;
		}
		// Traversal hand IK moves Hips without changing root, then solves arms toward the world-space
		// ledge. Reproduce those local changes independently of native Kawaii simulation.
		FPoseContext IKPose(Extracted);
		IKPose.Pose.CopyBonesFrom(Extracted.Pose);
		const FBoneContainer& Bones = IKPose.Pose.GetBoneContainer();
		const FCompactPoseBoneIndex Hips(Bones.GetPoseBoneIndexForBoneName(TEXT("Hips")));
		const FCompactPoseBoneIndex Arm(Bones.GetPoseBoneIndexForBoneName(TEXT("UpperArm_L")));
		if (!TestTrue(TEXT("Climb fixture contains IK bones"), Hips.GetInt() >= 0 && Arm.GetInt() >= 0)) {
			return false;
		}
		const double Weight = FMath::Sin(PI * double(Frame) / (Snapshot.NumFrames - 1));
		IKPose.Pose[Hips].AddToTranslation(FVector(15, 0, 20) * Weight);
		IKPose.Pose[Arm].SetRotation(FQuat(FVector::ZAxisVector, Weight * 0.4) * IKPose.Pose[Arm].GetRotation());
		KawaiiClothingProxy::FLiveBodyPose IKBody;
		TArray<FKawaiiClothingProxyCapsule> IKCapsules, RootOnly;
		if (!KawaiiClothingProxy::BuildLiveBodyPose(IKPose.Pose, ExcludedRoots, IKBody, Error)
			|| !KawaiiClothingProxy::SampleSnapshots({{&Snapshot, Time, 1.f}}, Snapshot.RelativeTransform, IKCapsules, Error, nullptr, &IKBody)
			|| !KawaiiClothingProxy::SampleSnapshots({{&Snapshot, Time, 1.f}}, Snapshot.RelativeTransform, RootOnly, Error, &RecordedRoot))
		{
			AddError(Error);
			return false;
		}
		KawaiiClothingProxy::ReanchorCapsules(RootOnly, RecordedRoot, LiveRoot);
		for (int32 ShapeIndex = 0; ShapeIndex < Snapshot.Shapes.Num(); ++ShapeIndex) {
			const auto& Shape = Snapshot.Shapes[ShapeIndex];
			const auto CheckEndpoint = [&](FName BoneName, const FVector& Offset, const FVector& Actual, const FVector& Old) {
				if (const FTransform* BodyBone = IKBody.Bones.Find(BoneName)) {
					// Shared garment bones are copied from Main; their offsets must follow Main's final IK pose.
					const FVector Expected = BodyBone->TransformPosition(Offset);
					MaxIKProxyError = FMath::Max(MaxIKProxyError, FVector::Distance(Expected, Actual));
					RootOnlyIKError = FMath::Max(RootOnlyIKError, FVector::Distance(Expected, Old));
					++CheckedIKEndpoints;
				}
			};
			CheckEndpoint(Shape.StartBone, Shape.StartOffset, IKCapsules[ShapeIndex].Start, RootOnly[ShapeIndex].Start);
			CheckEndpoint(Shape.EndBone.IsNone() ? Shape.StartBone : Shape.EndBone, Shape.EndOffset, IKCapsules[ShapeIndex].End, RootOnly[ShapeIndex].End);
		}
		for (int32 Index = 0; Index < PreviewCorrection.ExternalCapsules.Num(); ++Index) {
			const auto& A = PreviewCorrection.ExternalCapsules[Index];
			const auto& B = ExtractedCorrection.ExternalCapsules[Index];
			MaxCapsuleError =
				FMath::Max(MaxCapsuleError, FVector::Distance(PreviewRoot.InverseTransformPosition(A.Start), LiveRoot.InverseTransformPosition(B.Start)));
			MaxCapsuleError =
				FMath::Max(MaxCapsuleError, FVector::Distance(PreviewRoot.InverseTransformPosition(A.End), LiveRoot.InverseTransformPosition(B.End)));
		}
		OldPlacementError =
			FMath::Max(OldPlacementError, FVector::Distance((PreviewRoot * Snapshot.RelativeTransform).GetTranslation(), PreviewRoot.GetTranslation()));
		FPoseContext PreviewBase(Preview), ExtractedBase(Extracted);
		PreviewBase.Pose.CopyBonesFrom(Preview.Pose);
		ExtractedBase.Pose.CopyBonesFrom(Extracted.Pose);
		const TArray<FTransform> InputA = ComponentPose(Preview.Pose), InputB = ComponentPose(Extracted.Pose);
		for (int32 Bone = 1; Bone < InputA.Num(); ++Bone) {
			const FString Name = Mesh->GetRefSkeleton().GetBoneName(Bone).ToString();
			if (Name.StartsWith(TEXT("Hair_tail_L")) || Name.StartsWith(TEXT("Hair_tail_R"))) {
				const double ReferenceLength = Mesh->GetRefSkeleton().GetRefBonePose()[Bone].GetTranslation().Size();
				if (ReferenceLength > SMALL_NUMBER) {
					MinSavedTailLengthRatio =
						FMath::Min(MinSavedTailLengthRatio, Extracted.Pose[FCompactPoseBoneIndex(Bone)].GetTranslation().Size() / ReferenceLength);
				}
			}
			MaxInputError = FMath::Max(
				MaxInputError,
				FVector::Distance(
					PreviewRoot.InverseTransformPosition(InputA[Bone].GetTranslation()),
					LiveRoot.InverseTransformPosition(InputB[Bone].GetTranslation())
				)
			);
		}
		PreviewCorrection.AccumulateDeltaTime(1.f / Snapshot.SampleRate);
		PreviewCorrection.CorrectPose(Preview, PreviewBase);
		ExtractedCorrection.AccumulateDeltaTime(1.f / Snapshot.SampleRate);
		ExtractedCorrection.CorrectPose(Extracted, ExtractedBase);
		if (!PreviewCorrection.GetCorrectionDebugReason().StartsWith(TEXT("corrected"))
			|| !ExtractedCorrection.GetCorrectionDebugReason().StartsWith(TEXT("corrected")))
		{
			AddError(PreviewCorrection.GetCorrectionDebugReason() + TEXT(" / ") + ExtractedCorrection.GetCorrectionDebugReason());
			return false;
		}
		const TArray<FTransform> A = ComponentPose(Preview.Pose), B = ComponentPose(Extracted.Pose);
		// A warped actor can translate/rotate arbitrarily: both the mesh and its corrected capsules
		// share this component-to-world transform. Compare in the actor's current root frame.
		const FTransform WarpedWorld(FRotator(0, Time * 95.f, 0), FVector(12000 + Time * 450, -8000, Time * Time * 120));
		const FTransform WorldRoot = LiveRoot * WarpedWorld;
		for (int32 Bone = 1; Bone < A.Num(); ++Bone) {
			const double BoneError = FVector::Distance(
				PreviewRoot.InverseTransformPosition(A[Bone].GetTranslation()),
				WorldRoot.InverseTransformPosition(WarpedWorld.TransformPosition(B[Bone].GetTranslation()))
			);
			if (BoneError > MaxBoneError) {
				MaxBoneError = BoneError;
				WorstFrame = Frame;
				WorstBone = Mesh->GetRefSkeleton().GetBoneName(Bone).ToString();
			}
		}
	}
	AddInfo(FString::Printf(
		TEXT("%d climb frames: mapped root error %.6f cm, capsule error %.6f cm, corrected pose error %.6f cm; old duplicate placement up to %.6f cm."),
		Snapshot.NumFrames,
		MaxRootError,
		MaxCapsuleError,
		MaxBoneError,
		OldPlacementError
	));
	AddInfo(FString::Printf(TEXT("Input error %.9f cm; worst corrected bone %s frame %d."), MaxInputError, *WorstBone, WorstFrame));
	AddInfo(FString::Printf(
		TEXT("Body IK: %d endpoints; live-body error %.6f cm, old root-only error %.6f cm."),
		CheckedIKEndpoints,
		MaxIKProxyError,
		RootOnlyIKError
	));
	TestTrue(TEXT("Body IK fixture exercises existing chest/arm proxies"), CheckedIKEndpoints > 0 && RootOnlyIKError > 20.);
	TestTrue(TEXT("Proxy endpoints follow body IK even when root stays locked"), MaxIKProxyError < 0.001);
	TestTrue(TEXT("Garment root maps back to Main exactly once"), MaxRootError < 0.001);
	TestTrue(TEXT("Extracted proxies retain root-relative contact geometry"), MaxCapsuleError < 0.001);
	TestTrue(TEXT("Root extraction and warped component transforms preserve corrected pose"), MaxBoneError < 0.01);
	AddInfo(FString::Printf(TEXT("Saved tail animation minimum bone length: %.6f of reference."), MinSavedTailLengthRatio));
	TestTrue(TEXT("Saved climb tail tracks contain no collapsed real segments"), MinSavedTailLengthRatio > .99);
	return true;
}

#endif
