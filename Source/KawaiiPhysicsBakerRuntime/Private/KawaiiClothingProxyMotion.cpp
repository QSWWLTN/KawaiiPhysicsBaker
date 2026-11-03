#include "KawaiiClothingProxyMotion.h"

#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "KawaiiPhysicsBakerProfiling.h"

namespace {
	// Game-thread gauges describe the last sampling call, not totals across characters.
	TRACE_DECLARE_INT_COUNTER(KawaiiBaker_ProxyInputSamples, TEXT("KawaiiBaker/Proxy/InputSamples"));
	TRACE_DECLARE_INT_COUNTER(KawaiiBaker_ProxyActiveSamples, TEXT("KawaiiBaker/Proxy/ActiveSamples"));
	TRACE_DECLARE_INT_COUNTER(KawaiiBaker_ProxyBones, TEXT("KawaiiBaker/Proxy/Bones"));
	TRACE_DECLARE_INT_COUNTER(KawaiiBaker_ProxyCapsules, TEXT("KawaiiBaker/Proxy/Capsules"));

	bool ValidTransform(const FTransform& Transform) {
		const FVector Scale = Transform.GetScale3D();
		return !Transform.ContainsNaN() && Transform.GetRotation().IsNormalized() && Scale.X > SMALL_NUMBER && Scale.Y > SMALL_NUMBER && Scale.Z > SMALL_NUMBER;
	}

	bool Compatible(const UKawaiiClothingProxyMotion& A, const UKawaiiClothingProxyMotion& B) {
		// Geometry snapshots are independently checked against DefinitionSignature by SampleLocalPose.
		return A.Definition == B.Definition && A.DefinitionSignature == B.DefinitionSignature
			   && A.SourceMesh.ToSoftObjectPath() == B.SourceMesh.ToSoftObjectPath() && A.BoneNames == B.BoneNames && A.ParentIndices == B.ParentIndices
			   && A.RelativeTransform.Equals(B.RelativeTransform, 1.e-6);
	}
}

bool UKawaiiClothingProxyMotion::ValidateHeader(FString& Error) const {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_ValidateHeader);
	Error.Reset();
	if (!IsValid(Definition) || !Definition->Validate(Error)) {
		if (Error.IsEmpty()) {
			Error = TEXT("代理运动缺少有效的衣物代理定义。");
		}
		return false;
	}
	if (SourceMesh.IsNull() || SourceMesh.ToSoftObjectPath() != Definition->PreviewMesh.ToSoftObjectPath()) {
		Error = TEXT("代理运动的衣物网格体与代理定义不一致，需要重新烘焙。");
		return false;
	}
	if (DefinitionSignature.IsEmpty() || DefinitionSignature != Definition->ComputeSignature()
		|| DefinitionSignature != KawaiiClothingProxy::ComputeDefinitionSignature(SourceMesh.ToSoftObjectPath(), Shapes))
	{
		Error = TEXT("衣物代理定义已改变，或运动资产的形状快照不一致，请重新烘焙。");
		return false;
	}
	if (!KawaiiClothingProxy::ValidateShapes(Shapes, Error)) {
		return false;
	}
	if (SampleRate < 1 || SampleRate > 240 || !FMath::IsFinite(SourceDuration) || SourceDuration <= 0.f || SourceDuration > 600.f || NumFrames < 2
		|| NumFrames != FMath::Max(1, FMath::CeilToInt(static_cast<double>(SourceDuration) * SampleRate - 1.e-5)) + 1)
	{
		Error = TEXT("代理运动的帧率、源时长或采样帧数无效。");
		return false;
	}
	const int32 BoneCount = BoneNames.Num();
	const int64 RequiredKeys = static_cast<int64>(BoneCount) * NumFrames;
	if (BoneCount <= 0 || BoneCount > KawaiiClothingProxy::MaxBones || ParentIndices.Num() != BoneCount || RequiredKeys > KawaiiClothingProxy::MaxKeys
		|| LocalKeys.Num() != RequiredKeys)
	{
		Error = TEXT("代理运动的骨骼层级或局部变换轨道数量无效。");
		return false;
	}
	if (!ValidTransform(RelativeTransform)) {
		Error = TEXT("代理运动的记录相对变换无效。");
		return false;
	}
	TSet<FName> Names;
	for (int32 Bone = 0; Bone < BoneCount; ++Bone) {
		if (BoneNames[Bone].IsNone() || Names.Contains(BoneNames[Bone])
			|| (Bone == 0 ? ParentIndices[Bone] != INDEX_NONE : ParentIndices[Bone] < 0 || ParentIndices[Bone] >= Bone))
		{
			Error = TEXT("代理运动的骨骼名称重复，或父骨骼未排在子骨骼之前。");
			return false;
		}
		Names.Add(BoneNames[Bone]);
	}
	for (const FKawaiiClothingProxyShape& Shape : Shapes) {
		if (!Names.Contains(Shape.StartBone) || (!Shape.EndBone.IsNone() && !Names.Contains(Shape.EndBone))) {
			Error = FString::Printf(TEXT("代理 %s 的端点骨骼不在运动层级中。"), *Shape.Id.ToString());
			return false;
		}
	}
	return true;
}

bool UKawaiiClothingProxyMotion::Validate(FString& Error) const {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_ValidateAsset);
	SCOPE_CYCLE_COUNTER(STAT_KawaiiBakerProxyValidate);
	if (!ValidateHeader(Error)) {
		return false;
	}
	if (const USkeletalMesh* Mesh = SourceMesh.Get()) {
		if (!SourceSkeletonSignature.IsEmpty() && SourceSkeletonSignature != KawaiiClothingProxy::ComputeSkeletonSignature(Mesh->GetRefSkeleton())) {
			Error = TEXT("衣物参考骨架已改变，请重新烘焙代理运动。");
			return false;
		}
	}
#if WITH_EDITOR
	if (const UAnimSequence* Source = SourceAnimation.Get()) {
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		const FGuid CurrentGuid = Source->GetRawDataGuid();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		if (SourceDataGuid.IsValid() && SourceDataGuid != CurrentGuid) {
			Error = TEXT("源动画数据已改变，请重新烘焙衣物动画与代理运动。");
			return false;
		}
	}
#endif
	for (int32 Key = 0; Key < LocalKeys.Num(); ++Key) {
		if (!ValidTransform(LocalKeys[Key])) {
			Error = FString::Printf(TEXT("代理运动的第 %d 个局部变换无效。"), Key);
			return false;
		}
	}
	return true;
}

bool UKawaiiClothingProxyMotion::SampleLocalPose(float Time, TArray<FTransform>& OutPose, FString& Error) const {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_SampleLocalPose);
	OutPose.Reset();
	if (!FMath::IsFinite(Time)) {
		Error = TEXT("代理采样时间必须是有限数值。");
		return false;
	}
	if (!ValidateHeader(Error)) {
		return false;
	}
	const double Frame = FMath::Clamp(static_cast<double>(Time) * SampleRate, 0.0, static_cast<double>(NumFrames - 1));
	const int32 FrameA = FMath::FloorToInt(Frame);
	const int32 FrameB = FMath::Min(FrameA + 1, NumFrames - 1);
	const float Alpha = static_cast<float>(Frame - FrameA);
	const int32 BoneCount = BoneNames.Num();
	TArray<FTransform> Pose;
	Pose.SetNumUninitialized(BoneCount);
	for (int32 Bone = 0; Bone < BoneCount; ++Bone) {
		const FTransform& A = LocalKeys[FrameA * BoneCount + Bone];
		const FTransform& B = LocalKeys[FrameB * BoneCount + Bone];
		if (!ValidTransform(A) || !ValidTransform(B)) {
			Error = TEXT("代理运动在当前采样帧包含无效的局部变换。");
			return false;
		}
		Pose[Bone].Blend(A, B, Alpha);
		Pose[Bone].NormalizeRotation();
		if (!ValidTransform(Pose[Bone])) {
			Error = TEXT("代理局部变换插值产生了无效结果。");
			return false;
		}
	}
	OutPose = MoveTemp(Pose);
	return true;
}

namespace KawaiiClothingProxy {
	bool SampleBlended(const TArray<FKawaiiClothingProxyMotionSample>& Samples, const FTransform& ProxyToTarget, TArray<FKawaiiClothingProxyCapsule>& OutCapsules, FString& Error) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_SampleBlended);
		SCOPE_CYCLE_COUNTER(STAT_KawaiiBakerProxySample);
		OutCapsules.Reset();
		Error.Reset();
		if (!IsInGameThread()) {
			Error = TEXT("代理资产采样必须在游戏线程执行；动画线程只接收采样后的值数组。");
			return false;
		}
		TRACE_COUNTER_SET(KawaiiBaker_ProxyInputSamples, Samples.Num());
		TRACE_COUNTER_SET(KawaiiBaker_ProxyActiveSamples, 0);
		TRACE_COUNTER_SET(KawaiiBaker_ProxyBones, 0);
		TRACE_COUNTER_SET(KawaiiBaker_ProxyCapsules, 0);
		if (Samples.IsEmpty() || Samples.Num() > 64 || !ValidTransform(ProxyToTarget)) {
			Error = TEXT("代理采样需要 1–64 条输入，以及正缩放的有效坐标变换。");
			return false;
		}
		const UKawaiiClothingProxyMotion* First = nullptr;
		double TotalWeight = 0.0;
		[[maybe_unused]] int32 ActiveSamples = 0;
		for (const FKawaiiClothingProxyMotionSample& Sample : Samples) {
			if (!FMath::IsFinite(Sample.Weight) || Sample.Weight < 0.f) {
				Error = TEXT("代理混合权重必须是非负有限数值。");
				return false;
			}
			if (Sample.Weight == 0.f) {
				continue;
			}
			if (!IsValid(Sample.Motion) || !FMath::IsFinite(Sample.Time)) {
				Error = TEXT("参与混合的代理运动资产或采样时间无效。");
				return false;
			}
			if (!First) {
				First = Sample.Motion;
			} else if (!Compatible(*First, *Sample.Motion)) {
				Error = TEXT("代理混合要求相同的衣物定义、网格、骨骼层级和装配变换。");
				return false;
			}
			TotalWeight += Sample.Weight;
			++ActiveSamples;
		}
		if (!First || !FMath::IsFinite(TotalWeight) || TotalWeight <= 0.0) {
			Error = TEXT("代理采样没有正权重的有效动作。");
			return false;
		}
		TRACE_COUNTER_SET(KawaiiBaker_ProxyActiveSamples, ActiveSamples);

		TArray<FTransform> LocalPose;
		TArray<FQuat> ReferenceRotations;
		TArray<FVector> Positions, Scales;
		TArray<FQuat> Rotations;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_BlendLocalPoses);
			for (const FKawaiiClothingProxyMotionSample& Sample : Samples) {
				if (Sample.Weight == 0.f) {
					continue;
				}
				TArray<FTransform> Sampled;
				if (!Sample.Motion->SampleLocalPose(Sample.Time, Sampled, Error)) {
					return false;
				}
				if (ReferenceRotations.IsEmpty()) {
					for (const FTransform& Bone : Sampled) {
						ReferenceRotations.Add(Bone.GetRotation());
						Positions.Add(FVector::ZeroVector);
						Scales.Add(FVector::ZeroVector);
						Rotations.Add(FQuat(0, 0, 0, 0));
					}
				}
				const double Weight = static_cast<double>(Sample.Weight) / TotalWeight;
				for (int32 Bone = 0; Bone < Sampled.Num(); ++Bone) {
					Positions[Bone] += Sampled[Bone].GetTranslation() * Weight;
					Scales[Bone] += Sampled[Bone].GetScale3D() * Weight;
					FQuat Rotation = Sampled[Bone].GetRotation();
					if ((ReferenceRotations[Bone] | Rotation) < 0.0) {
						Rotation = Rotation * -1.0;
					}
					Rotations[Bone] += Rotation * Weight;
				}
			}
		}
		TArray<FTransform> ComponentPose;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_BuildHierarchy);
			LocalPose.SetNumUninitialized(Positions.Num());
			ComponentPose.SetNumUninitialized(Positions.Num());
			for (int32 Bone = 0; Bone < Positions.Num(); ++Bone) {
				if (Rotations[Bone].SizeSquared() <= SMALL_NUMBER) {
					Error = TEXT("代理旋转混合结果退化。");
					return false;
				}
				Rotations[Bone].Normalize();
				LocalPose[Bone] = FTransform(Rotations[Bone], Positions[Bone], Scales[Bone]);
				const int32 Parent = First->ParentIndices[Bone];
				ComponentPose[Bone] = Parent == INDEX_NONE ? LocalPose[Bone] : LocalPose[Bone] * ComponentPose[Parent];
				ComponentPose[Bone].NormalizeRotation();
				if (!ValidTransform(ComponentPose[Bone])) {
					Error = TEXT("代理组件空间姿态无效。");
					return false;
				}
			}
		}
		TRACE_COUNTER_SET(KawaiiBaker_ProxyBones, ComponentPose.Num());
		TArray<FKawaiiClothingProxyCapsule> Capsules;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_BuildCapsules);
			Capsules.Reserve(First->Shapes.Num());
			for (const FKawaiiClothingProxyShape& Shape : First->Shapes) {
				const int32 Start = First->BoneNames.IndexOfByKey(Shape.StartBone);
				const int32 End = Shape.EndBone.IsNone() ? Start : First->BoneNames.IndexOfByKey(Shape.EndBone);
				// ValidateHeader already checked indices. Keep this explicit for all-or-nothing output.
				if (!ComponentPose.IsValidIndex(Start) || !ComponentPose.IsValidIndex(End)) {
					Error = TEXT("代理端点骨骼缺失。");
					return false;
				}
				FKawaiiClothingProxyCapsule& Capsule = Capsules.AddDefaulted_GetRef();
				Capsule.Id = Shape.Id;
				Capsule.Start = ProxyToTarget.TransformPosition(ComponentPose[Start].TransformPosition(Shape.StartOffset));
				Capsule.End = ProxyToTarget.TransformPosition(ComponentPose[End].TransformPosition(Shape.EndOffset));
				// Conservative radius for positive non-uniform scale, including both endpoint bindings.
				Capsule.Radius = Shape.Radius * FMath::Max(ComponentPose[Start].GetMaximumAxisScale(), ComponentPose[End].GetMaximumAxisScale())
								 * ProxyToTarget.GetMaximumAxisScale();
				if (Capsule.Start.ContainsNaN() || Capsule.End.ContainsNaN() || !FMath::IsFinite(Capsule.Radius) || Capsule.Radius <= 0.f) {
					Error = TEXT("代理坐标转换产生了无效几何。");
					return false;
				}
			}
		}
		OutCapsules = MoveTemp(Capsules);
		TRACE_COUNTER_SET(KawaiiBaker_ProxyCapsules, OutCapsules.Num());
		return true;
	}
}

bool UKawaiiClothingProxyLibrary::SampleProxyMotion(UKawaiiClothingProxyMotion* Motion, float Time, const FTransform& ProxyToTarget, TArray<FKawaiiClothingProxyCapsule>& Capsules, FString& Error) {
	FKawaiiClothingProxyMotionSample Sample;
	Sample.Motion = Motion;
	Sample.Time = Time;
	return KawaiiClothingProxy::SampleBlended({Sample}, ProxyToTarget, Capsules, Error);
}

bool UKawaiiClothingProxyLibrary::ValidateProxyMotion(UKawaiiClothingProxyMotion* Motion, FString& Error) {
	Error.Reset();
	if (!IsInGameThread() || !IsValid(Motion)) {
		Error = TEXT("请在游戏线程验证有效的衣物代理运动资产。");
		return false;
	}
	return Motion->Validate(Error);
}

bool UKawaiiClothingProxyLibrary::SampleBlendedProxyMotion(const TArray<FKawaiiClothingProxyMotionSample>& Samples, const FTransform& ProxyToTarget, TArray<FKawaiiClothingProxyCapsule>& Capsules, FString& Error) {
	return KawaiiClothingProxy::SampleBlended(Samples, ProxyToTarget, Capsules, Error);
}
