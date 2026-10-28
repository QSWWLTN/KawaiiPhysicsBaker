#include "AnimNode_KawaiiPhysicsBakeSource.h"

#include "Animation/AnimationPoseData.h"
#include "Animation/AnimCurveUtils.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "KawaiiPhysicsBakePoseUtils.h"

void FAnimNode_KawaiiPhysicsBakeSource::PreUpdate(const UAnimInstance* InAnimInstance) {
	check(IsInGameThread());
	bSnapshotValid = false;
	MeshLocalPose.Reset();
	SourceCurves.Empty();
	SourceState.Reset();

	const UKawaiiPhysicsBakeAnimInstance* BakeInstance = Cast<UKawaiiPhysicsBakeAnimInstance>(InAnimInstance);
	if (!BakeInstance) {
		return;
	}
	SourceState = BakeInstance->SourceState;
	SourceState->Error.Reset();
	SnapshotTime = BakeInstance->SourceTime;

	USkeletalMeshComponent* TargetComponent = InAnimInstance->GetSkelMeshComponent();
	USkeletalMesh* TargetMesh = TargetComponent ? TargetComponent->GetSkeletalMeshAsset() : nullptr;
	if (!TargetMesh) {
		SourceState->Error = TEXT("Bake Source has no target skeletal mesh.");
		return;
	}
	if (!FMath::IsFinite(SnapshotTime)) {
		SourceState->Error = TEXT("Bake Source time is not finite.");
		return;
	}

	const FReferenceSkeleton& TargetSkeleton = TargetMesh->GetRefSkeleton();
	MeshLocalPose = TargetSkeleton.GetRefBonePose();
	if (UAnimSequence* Animation = BakeInstance->SourceAnimation) {
		if (!Animation->GetSkeleton() || Animation->GetSkeleton() != TargetMesh->GetSkeleton()) {
			SourceState->Error = TEXT("The source sequence and this mesh must use the same skeleton. Use Copy Pose From for a different part skeleton.");
			return;
		}
		if (Animation->AdditiveAnimType != AAT_None) {
			SourceState->Error = TEXT("Bake Source requires an absolute animation sequence. Additive sequences need an explicit base pose and are not supported as the bake source.");
			return;
		}
		const FBoneContainer& RequiredBones = InAnimInstance->GetRequiredBones();
		if (!RequiredBones.IsValid()) {
			SourceState->Error = TEXT("Bake Source required bones are not initialized.");
			return;
		}

		// Sampling is deliberately done on the game thread. Evaluate_AnyThread consumes only
		// owned arrays, never a mutable sequence, component, blueprint, or settings object.
		FMemMark Mark(FMemStack::Get());
		FCompactPose Pose;
		Pose.SetBoneContainer(&RequiredBones);
		Pose.ResetToRefPose();
		FBlendedCurve Curve;
		Curve.InitFrom(RequiredBones);
		UE::Anim::FStackAttributeContainer Attributes;
		FAnimationPoseData PoseData(Pose, Curve, Attributes);
		const double Time = FMath::Clamp(static_cast<double>(SnapshotTime), 0.0, static_cast<double>(Animation->GetPlayLength()));
		FAnimExtractContext ExtractContext(Time, false);
#if WITH_EDITOR
		ExtractContext.bIgnoreRootLock = true;
		ExtractContext.bExtractWithRootMotionProvider = false;
#endif
		Animation->GetAnimationPose(PoseData, ExtractContext);
		for (FCompactPoseBoneIndex BoneIndex : Pose.ForEachBoneIndex()) {
			const int32 MeshIndex = RequiredBones.MakeMeshPoseIndex(BoneIndex).GetInt();
			if (MeshLocalPose.IsValidIndex(MeshIndex)) {
				MeshLocalPose[MeshIndex] = Pose[BoneIndex];
			}
		}
		SourceCurves.CopyFrom(Curve);
		SnapshotTime = static_cast<float>(Time);
		bSnapshotValid = true;
		return;
	}

	USkeletalMeshComponent* SourceComponent = BakeInstance->CopyPoseSource;
	if (!SourceComponent
		|| SourceComponent == TargetComponent
		|| !SourceComponent->IsRegistered()
		|| !SourceComponent->GetSkeletalMeshAsset())
	{
		SourceState->Error = TEXT("Bake Source needs an animation sequence or a distinct, registered Copy Pose From component.");
		return;
	}
	// Bake sessions use independent animation components and finalize each source before
	// its consumer. Leader-pose components require a separate index mapping, so reject them.
	if (SourceComponent->LeaderPoseComponent.IsValid()) {
		SourceState->Error = TEXT("Copy Pose From must have its own evaluated pose, not a Leader Pose Component.");
		return;
	}
	const FReferenceSkeleton& SourceSkeleton = SourceComponent->GetSkeletalMeshAsset()->GetRefSkeleton();
	const TArray<FTransform>& SourceCS = SourceComponent->GetComponentSpaceTransforms();
	const FTransform SourceToTarget = SourceComponent->GetComponentTransform().GetRelativeTransform(TargetComponent->GetComponentTransform());
	if (!KawaiiPhysicsBake::BuildCopiedLocalPose(SourceSkeleton, SourceCS, TargetSkeleton, SourceToTarget, MeshLocalPose, SourceState->Error)) {
		return;
	}
	if (UAnimInstance* SourceInstance = SourceComponent->GetAnimInstance()) {
		UE::Anim::FCurveUtils::BuildUnsorted(SourceCurves, SourceInstance->GetAnimationCurveList(EAnimCurveType::AttributeCurve));
	}
	bSnapshotValid = true;
}

void FAnimNode_KawaiiPhysicsBakeSource::Update_AnyThread(const FAnimationUpdateContext& Context) {
	// No asset tick record: sampling cannot trigger notifies, sync-group advancement,
	// animation events, or root-motion extraction.
}

void FAnimNode_KawaiiPhysicsBakeSource::Evaluate_AnyThread(FPoseContext& Output) {
	Output.ResetToRefPose();
	if (!bSnapshotValid) {
		return;
	}
	const FBoneContainer& RequiredBones = Output.Pose.GetBoneContainer();
	for (FCompactPoseBoneIndex BoneIndex : Output.Pose.ForEachBoneIndex()) {
		const int32 MeshIndex = RequiredBones.MakeMeshPoseIndex(BoneIndex).GetInt();
		if (MeshLocalPose.IsValidIndex(MeshIndex)) {
			Output.Pose[BoneIndex] = MeshLocalPose[MeshIndex];
		}
	}
	Output.Curve.CopyFrom(SourceCurves);
	if (SourceState) {
		SourceState->LastEvaluatedTime.Store(SnapshotTime);
		SourceState->EvaluationSerial.Increment();
	}
}

void FAnimNode_KawaiiPhysicsBakeSource::GatherDebugData(FNodeDebugData& DebugData) {
	DebugData.AddDebugItem(FString::Printf(TEXT("%s (Time: %.3f, Valid: %s)"), *DebugData.GetNodeName(this), SnapshotTime, bSnapshotValid ? TEXT("yes") : TEXT("no")), true);
}
