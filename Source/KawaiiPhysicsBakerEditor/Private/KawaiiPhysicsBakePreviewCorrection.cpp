#include "KawaiiPhysicsBakePreviewCorrection.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/MemStack.h"

bool FKawaiiPhysicsBakePreviewCorrection::Initialize(const UKawaiiClothingProxyAsset& Definition, const USkeletalMeshComponent& Source, UKawaiiPhysicsIKCorrectionProfile* Profile, int32 SampleRate, FString& OutError) {
	if (!Profile || Profile->Chains.IsEmpty() || Profile->bUseAttachedParentForCollision) {
		OutError =
			TEXT("运行时代理预览需要有效的修正配置，且 Main 的身体碰撞必须从自身姿态读取。请选择与角色 Automatic Clothing Proxy Correction 节点相同的 Profile。"
			);
		return false;
	}
	if (SampleRate < 1 || SampleRate > 240) {
		OutError = TEXT("运行时代理预览的采样帧率必须为 1–240。");
		return false;
	}
	const USkeletalMesh* Mesh = Source.GetSkeletalMeshAsset();
	if (!Mesh || !Definition.Validate(OutError)) {
		return false;
	}
	if (!KawaiiClothingProxy::BuildMinimalHierarchy(
			Mesh->GetRefSkeleton(),
			Definition.Shapes,
			Snapshot.BoneNames,
			Snapshot.ParentIndices,
			SourceBones,
			OutError
		))
	{
		return false;
	}
	Correction.Profile = Profile;
	Correction.ResetTemporalState();
	Snapshot.DefinitionIdentity = FSoftObjectPath(&Definition);
	Snapshot.DefinitionSignature = Definition.ComputeSignature();
	Snapshot.SourceMesh = FSoftObjectPath(Mesh);
	Snapshot.Shapes = Definition.Shapes;
	Snapshot.IgnoredTargetRootBones = Definition.IgnoredTargetRootBones;
	// Two identical endpoints represent the current completed sample before an exported track exists.
	// Sampling at zero then uses exactly the runtime hierarchy, shape conversion and ignore rules.
	Snapshot.SampleRate = SampleRate;
	Snapshot.SourceDuration = 1.f / SampleRate;
	Snapshot.NumFrames = 2;
	Snapshot.LocalKeys.SetNum(SourceBones.Num() * 2);
	return true;
}

bool FKawaiiPhysicsBakePreviewCorrection::Evaluate(USkeletalMeshComponent& Target, const USkeletalMeshComponent& Source, float DeltaTime, float Alpha, TArray<FTransform3f>& OutPose, FString& OutError) {
	OutPose.Reset();
	UAnimInstance* Instance = Target.GetAnimInstance();
	if (!Instance || !Instance->GetRequiredBones().IsValid()) {
		OutError = TEXT("运行时代理预览缺少 Main 动画实例或有效骨架。");
		return false;
	}
	const TArray<FTransform>& SourcePose = Source.GetComponentSpaceTransforms();
	for (int32 Bone = 0; Bone < SourceBones.Num(); ++Bone) {
		const int32 MeshBone = SourceBones[Bone];
		const int32 Parent = Snapshot.ParentIndices[Bone];
		if (!SourcePose.IsValidIndex(MeshBone)) {
			OutError = TEXT("衣物代理预览缺少来源骨骼。");
			return false;
		}
		FTransform Local = Parent == INDEX_NONE ? SourcePose[MeshBone] : SourcePose[MeshBone].GetRelativeTransform(SourcePose[SourceBones[Parent]]);
		Local.NormalizeRotation();
		Snapshot.LocalKeys[Bone] = Snapshot.LocalKeys[SourceBones.Num() + Bone] = Local;
	}
	Snapshot.RelativeTransform = Source.GetComponentTransform().GetRelativeTransform(Target.GetComponentTransform());
	Snapshot.RelativeTransform.NormalizeRotation();
	KawaiiClothingProxy::FWeightedSnapshot Sample;
	Sample.Motion = &Snapshot;
	FMemMark Mark(FMemStack::Get());
	FAnimInstanceProxy Proxy(Instance);
	Proxy.GetRequiredBones() = Instance->GetRequiredBones();
	FPoseContext Output(&Proxy);
	const FBoneContainer& Bones = Output.Pose.GetBoneContainer();
	const TArray<FTransform>& TargetPose = Target.GetComponentSpaceTransforms();
	for (int32 Bone = 0; Bone < Output.Pose.GetNumBones(); ++Bone) {
		const FCompactPoseBoneIndex Compact(Bone);
		const int32 MeshBone = Bones.MakeMeshPoseIndex(Compact).GetInt();
		const FCompactPoseBoneIndex Parent = Bones.GetParentBoneIndex(Compact);
		Output.Pose[Compact] = Parent.GetInt() == INDEX_NONE ? TargetPose[MeshBone] : TargetPose[MeshBone].GetRelativeTransform(TargetPose[Bones.MakeMeshPoseIndex(Parent).GetInt()]);
		Output.Pose[Compact].NormalizeRotation();
	}
	TArray<FName> ExcludedRoots;
	for (const auto& Chain : Correction.Profile->Chains) {
		ExcludedRoots.Add(Chain.RootBone.BoneName);
	}
	KawaiiClothingProxy::FLiveBodyPose LiveBody;
	if (!KawaiiClothingProxy::BuildLiveBodyPose(Output.Pose, ExcludedRoots, LiveBody, OutError)
		|| !KawaiiClothingProxy::SampleSnapshots({Sample}, Snapshot.RelativeTransform, Correction.ExternalCapsules, OutError, nullptr, &LiveBody))
	{
		return false;
	}
	FPoseContext Baseline(Output);
	Baseline.Pose.CopyBonesFrom(Output.Pose);
	// EditorPreview keeps the solver on CPU even when the game's GPU option is enabled.
	Correction.PreUpdate(Instance);
	Correction.Alpha = Alpha;
	Correction.AccumulateDeltaTime(DeltaTime);
	Correction.CorrectPose(Output, Baseline);
	if (!Correction.GetCorrectionDebugReason().StartsWith(TEXT("corrected"))) {
		OutError = TEXT("运行时代理预览失败：") + Correction.GetCorrectionDebugReason();
		return false;
	}
	for (const FTransform& Transform : TargetPose) {
		OutPose.Add(FTransform3f(Transform));
	}
	TArray<FTransform> Corrected;
	Corrected.SetNum(Output.Pose.GetNumBones());
	for (int32 Bone = 0; Bone < Corrected.Num(); ++Bone) {
		const FCompactPoseBoneIndex Compact(Bone);
		const int32 Parent = Bones.GetParentBoneIndex(Compact).GetInt();
		Corrected[Bone] = Parent == INDEX_NONE ? Output.Pose[Compact] : Output.Pose[Compact] * Corrected[Parent];
		Corrected[Bone].NormalizeRotation();
		OutPose[Bones.MakeMeshPoseIndex(Compact).GetInt()] = FTransform3f(Corrected[Bone]);
	}
	return true;
}
