#include "KawaiiClothingProxySnapshot.h"

#include "BonePose.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include "KawaiiPhysicsBakerProfiling.h"

namespace KawaiiClothingProxy {
	namespace {
		bool ValidTransform(const FTransform& Transform) {
			const FVector Scale = Transform.GetScale3D();
			return !Transform.ContainsNaN() && Transform.GetRotation().IsNormalized() && Scale.X > SMALL_NUMBER && Scale.Y > SMALL_NUMBER
				&& Scale.Z > SMALL_NUMBER;
		}

		bool Compatible(const FSnapshot& A, const FSnapshot& B) {
			return A.DefinitionIdentity == B.DefinitionIdentity && A.DefinitionSignature == B.DefinitionSignature && A.SourceMesh == B.SourceMesh
				&& A.BoneNames == B.BoneNames && A.ParentIndices == B.ParentIndices && A.RelativeTransform.Equals(B.RelativeTransform, 1.e-6);
		}

		const TArray<FName>& Rules(const FWeightedSnapshot& Sample) {
			return Sample.IgnoredTargetRootBones ? *Sample.IgnoredTargetRootBones : Sample.Motion->IgnoredTargetRootBones;
		}

		bool ValidateHeader(const FSnapshot& Motion, FString& Error) {
			if (Motion.DefinitionIdentity.IsNull() || Motion.SourceMesh.IsNull() || Motion.DefinitionSignature.IsEmpty()) {
				Error = TEXT("Proxy snapshot is missing its validated definition or mesh identity.");
				return false;
			}
			if (Motion.SampleRate < 1 || Motion.SampleRate > 240 || !FMath::IsFinite(Motion.SourceDuration) || Motion.SourceDuration <= 0.f
				|| Motion.SourceDuration > 600.f || Motion.NumFrames < 2
				|| Motion.NumFrames != FMath::Max(1, FMath::CeilToInt(static_cast<double>(Motion.SourceDuration) * Motion.SampleRate - 1.e-5)) + 1)
			{
				Error = TEXT("Proxy snapshot has invalid sample timing.");
				return false;
			}
			const int32 BoneCount = Motion.BoneNames.Num();
			const int64 RequiredKeys = static_cast<int64>(BoneCount) * Motion.NumFrames;
			if (BoneCount <= 0 || BoneCount > MaxBones || Motion.ParentIndices.Num() != BoneCount || RequiredKeys > MaxKeys
				|| Motion.LocalKeys.Num() != RequiredKeys || !ValidTransform(Motion.RelativeTransform))
			{
				Error = TEXT("Proxy snapshot has invalid hierarchy, tracks or recorded placement.");
				return false;
			}
			TSet<FName> Names;
			for (int32 Bone = 0; Bone < BoneCount; ++Bone) {
				if (Motion.BoneNames[Bone].IsNone() || Names.Contains(Motion.BoneNames[Bone])
					|| (Bone == 0 ? Motion.ParentIndices[Bone] != INDEX_NONE : Motion.ParentIndices[Bone] < 0 || Motion.ParentIndices[Bone] >= Bone))
				{
					Error = TEXT("Proxy snapshot has duplicate names or invalid parent ordering.");
					return false;
				}
				Names.Add(Motion.BoneNames[Bone]);
			}
			if (!ValidateShapes(Motion.Shapes, Error)) {
				return false;
			}
			for (const FKawaiiClothingProxyShape& Shape : Motion.Shapes) {
				if (!Names.Contains(Shape.StartBone) || (!Shape.bBox && !Shape.EndBone.IsNone() && !Names.Contains(Shape.EndBone))) {
					Error = TEXT("Proxy snapshot is missing a shape endpoint bone.");
					return false;
				}
			}
			return true;
		}

		bool SampleLocalPose(const FSnapshot& Motion, float Time, TArray<FTransform>& OutPose, FString& Error) {
			TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_SnapshotSampleLocalPose);
			const double Frame = FMath::Clamp(static_cast<double>(Time) * Motion.SampleRate, 0.0, static_cast<double>(Motion.NumFrames - 1));
			const int32 FrameA = FMath::FloorToInt(Frame);
			const int32 FrameB = FMath::Min(FrameA + 1, Motion.NumFrames - 1);
			const float Alpha = static_cast<float>(Frame - FrameA);
			const int32 BoneCount = Motion.BoneNames.Num();
			OutPose.SetNumUninitialized(BoneCount);
			for (int32 Bone = 0; Bone < BoneCount; ++Bone) {
				const FTransform& A = Motion.LocalKeys[FrameA * BoneCount + Bone];
				const FTransform& B = Motion.LocalKeys[FrameB * BoneCount + Bone];
				if (!ValidTransform(A) || !ValidTransform(B)) {
					Error = TEXT("Proxy snapshot contains an invalid transform in the sampled keys.");
					return false;
				}
				OutPose[Bone].Blend(A, B, Alpha);
				OutPose[Bone].NormalizeRotation();
				if (!ValidTransform(OutPose[Bone])) {
					Error = TEXT("Proxy snapshot interpolation produced an invalid transform.");
					return false;
				}
			}
			return true;
		}
	}

	bool BuildLiveBodyPose(const FCompactPose& Pose, const TArray<FName>& ExcludedRoots, FLiveBodyPose& Out, FString& Error) {
		Out = FLiveBodyPose();
		Error.Reset();
		if (Pose.GetNumBones() == 0 || !Pose.GetBoneContainer().IsValid()) {
			Error = TEXT("Live clothing proxy alignment requires a valid target pose.");
			return false;
		}
		const FBoneContainer& Container = Pose.GetBoneContainer();
		TArray<FTransform> ComponentPose;
		TArray<bool> Excluded;
		for (int32 Bone = 0; Bone < Pose.GetNumBones(); ++Bone) {
			const FCompactPoseBoneIndex Index(Bone);
			const int32 Parent = Container.GetParentBoneIndex(Index).GetInt();
			const FName Name = Container.GetReferenceSkeleton().GetBoneName(Container.MakeMeshPoseIndex(Index).GetInt());
			FTransform Transform = Parent == INDEX_NONE ? Pose[Index] : Pose[Index] * ComponentPose[Parent];
			Transform.NormalizeRotation();
			if (!ValidTransform(Transform)) {
				Out = FLiveBodyPose();
				Error = TEXT("Live clothing proxy alignment received an invalid bone transform.");
				return false;
			}
			ComponentPose.Add(Transform);
			Excluded.Add(ExcludedRoots.Contains(Name) || (Parent != INDEX_NONE && Excluded[Parent]));
			if (!Excluded.Last()) {
				Out.Bones.Add(Name, Transform);
			}
		}
		Out.Root = ComponentPose[0];
		return true;
	}

	bool FSnapshot::MatchesAnimation(const FSoftObjectPath& Animation) const {
		return !Animation.IsNull() && (Animation == SourceAnimation || Animation == BakedAnimation || Animation == BodyBakedAnimation);
	}

	bool BuildSnapshot(const UKawaiiClothingProxyMotion& Motion, FSnapshot& Out, FString& Error) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_BuildSnapshot);
		Out = FSnapshot();
		Error.Reset();
		if (!IsInGameThread()) {
			Error = TEXT("Proxy snapshots must be built on the game thread.");
			return false;
		}
		if (!IsValid(&Motion) || !Motion.Validate(Error)) {
			if (Error.IsEmpty()) {
				Error = TEXT("Cannot snapshot an invalid proxy motion.");
			}
			return false;
		}
		Out.SourceAnimation = Motion.SourceAnimation.ToSoftObjectPath();
		Out.BakedAnimation = Motion.BakedAnimation.ToSoftObjectPath();
		Out.BodyBakedAnimation = Motion.BodyBakedAnimation.ToSoftObjectPath();
		Out.MotionRevision = Motion.MotionRevision;
		Out.DefinitionIdentity = FSoftObjectPath(Motion.Definition.Get());
		Out.SourceMesh = Motion.SourceMesh.ToSoftObjectPath();
		Out.DefinitionSignature = Motion.DefinitionSignature;
		Out.RelativeTransform = Motion.RelativeTransform;
		Out.SampleRate = Motion.SampleRate;
		Out.NumFrames = Motion.NumFrames;
		Out.SourceDuration = Motion.SourceDuration;
		Out.BoneNames = Motion.BoneNames;
		Out.ParentIndices = Motion.ParentIndices;
		Out.LocalKeys = Motion.LocalKeys;
		Out.Shapes = Motion.Shapes;
		Out.IgnoredTargetRootBones = Motion.Definition->IgnoredTargetRootBones;
		return true;
	}

	bool SampleSnapshots(const TArray<FWeightedSnapshot>& Samples, const FTransform& ProxyToTarget, TArray<FKawaiiClothingProxyCapsule>& OutCapsules, FString& Error, FTransform* OutRecordedRoot, const FLiveBodyPose* LiveBodyPose) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_SampleSnapshots);
		SCOPE_CYCLE_COUNTER(STAT_KawaiiBakerProxySample);
		OutCapsules.Reset();
		Error.Reset();
		if (OutRecordedRoot) {
			*OutRecordedRoot = FTransform::Identity;
		}
		if (Samples.IsEmpty() || Samples.Num() > 64 || !ValidTransform(ProxyToTarget) || (LiveBodyPose && !ValidTransform(LiveBodyPose->Root))) {
			Error = TEXT("Proxy snapshot sampling needs 1-64 samples and a valid target transform with positive scale.");
			return false;
		}
		const FWeightedSnapshot* First = nullptr;
		double TotalWeight = 0.0;
		for (const FWeightedSnapshot& Sample : Samples) {
			if (!FMath::IsFinite(Sample.Weight) || Sample.Weight < 0.f) {
				Error = TEXT("Proxy snapshot weights must be finite and nonnegative.");
				return false;
			}
			if (Sample.Weight == 0.f) {
				continue;
			}
			if (!Sample.Motion || !FMath::IsFinite(Sample.Time)) {
				Error = TEXT("An active proxy snapshot or its sample time is invalid.");
				return false;
			}
			if (!ValidateHeader(*Sample.Motion, Error) || !ValidateIgnoredTargetRoots(Rules(Sample), Error)) {
				return false;
			}
			if (!First) {
				First = &Sample;
			} else if (!Compatible(*First->Motion, *Sample.Motion) || Rules(*First) != Rules(Sample)) {
				Error = TEXT("Proxy snapshots must have the same definition, mesh, hierarchy, placement and current collision rules to blend.");
				return false;
			}
			TotalWeight += Sample.Weight;
		}
		if (!First || !FMath::IsFinite(TotalWeight) || TotalWeight <= 0.0) {
			Error = TEXT("Proxy snapshot sampling has no positive-weight action.");
			return false;
		}

		TArray<FQuat> ReferenceRotations;
		TArray<FVector> Positions, Scales;
		TArray<FQuat> Rotations;
		for (const FWeightedSnapshot& Sample : Samples) {
			if (Sample.Weight == 0.f) {
				continue;
			}
			TArray<FTransform> Pose;
			if (!SampleLocalPose(*Sample.Motion, Sample.Time, Pose, Error)) {
				return false;
			}
			if (ReferenceRotations.IsEmpty()) {
				for (const FTransform& Bone : Pose) {
					ReferenceRotations.Add(Bone.GetRotation());
					Positions.Add(FVector::ZeroVector);
					Scales.Add(FVector::ZeroVector);
					Rotations.Add(FQuat(0, 0, 0, 0));
				}
			}
			const double Weight = static_cast<double>(Sample.Weight) / TotalWeight;
			for (int32 Bone = 0; Bone < Pose.Num(); ++Bone) {
				Positions[Bone] += Pose[Bone].GetTranslation() * Weight;
				Scales[Bone] += Pose[Bone].GetScale3D() * Weight;
				FQuat Rotation = Pose[Bone].GetRotation();
				if ((ReferenceRotations[Bone] | Rotation) < 0.0) {
					Rotation = Rotation * -1.0;
				}
				Rotations[Bone] += Rotation * Weight;
			}
		}
		TArray<FTransform> ComponentPose;
		ComponentPose.SetNumUninitialized(Positions.Num());
		for (int32 Bone = 0; Bone < Positions.Num(); ++Bone) {
			if (Rotations[Bone].SizeSquared() <= SMALL_NUMBER) {
				Error = TEXT("Proxy snapshot rotation blend is degenerate.");
				return false;
			}
			Rotations[Bone].Normalize();
			const FTransform Local(Rotations[Bone], Positions[Bone], Scales[Bone]);
			const int32 Parent = First->Motion->ParentIndices[Bone];
			ComponentPose[Bone] = Parent == INDEX_NONE ? Local : Local * ComponentPose[Parent];
			ComponentPose[Bone].NormalizeRotation();
			if (!ValidTransform(ComponentPose[Bone])) {
				Error = TEXT("Proxy snapshot hierarchy produced an invalid transform.");
				return false;
			}
		}

		const auto FollowBody = [&](int32 Bone, FVector& Point, double& RadiusScale, FQuat* Orientation = nullptr) {
			RadiusScale = 1.;
			if (!LiveBodyPose) {
				return true;
			}
			const FTransform* Live = nullptr;
			int32 Anchor = Bone;
			for (; Anchor != INDEX_NONE; Anchor = First->Motion->ParentIndices[Anchor]) {
				Live = LiveBodyPose->Bones.Find(First->Motion->BoneNames[Anchor]);
				if (Live) {
					break;
				}
			}
			// Older/different skeletons may have no shared names. Keep their existing root-follow behavior.
			const FTransform Recorded = ComponentPose[Anchor == INDEX_NONE ? 0 : Anchor] * ProxyToTarget;
			if (!Live) {
				Live = &LiveBodyPose->Root;
			}
			if (!ValidTransform(*Live) || !ValidTransform(Recorded)) {
				return false;
			}
			Point = Live->TransformPosition(Recorded.InverseTransformPosition(Point));
			if (Orientation) { *Orientation = (Live->GetRotation() * Recorded.GetRotation().Inverse() * *Orientation).GetNormalized(); }
			RadiusScale = (Live->GetScale3D() / Recorded.GetScale3D()).GetAbsMax();
			return true;
		};
		TArray<FKawaiiClothingProxyCapsule> Capsules;
		Capsules.Reserve(First->Motion->Shapes.Num());
		for (const FKawaiiClothingProxyShape& Shape : First->Motion->Shapes) {
			const int32 Start = First->Motion->BoneNames.IndexOfByKey(Shape.StartBone);
			const int32 End = Shape.bBox || Shape.EndBone.IsNone() ? Start : First->Motion->BoneNames.IndexOfByKey(Shape.EndBone);
			if (!ComponentPose.IsValidIndex(Start) || !ComponentPose.IsValidIndex(End)) {
				Error = TEXT("Proxy snapshot shape endpoint is unavailable.");
				return false;
			}
			FKawaiiClothingProxyCapsule& Capsule = Capsules.AddDefaulted_GetRef();
			Capsule.Id = Shape.Id;
			Capsule.IgnoredTargetRootBones = Rules(*First);
			Capsule.Start = ProxyToTarget.TransformPosition(ComponentPose[Start].TransformPosition(Shape.StartOffset));
			Capsule.End = ProxyToTarget.TransformPosition(ComponentPose[End].TransformPosition(Shape.EndOffset));
			Capsule.Radius = Shape.Radius * FMath::Max(ComponentPose[Start].GetMaximumAxisScale(), ComponentPose[End].GetMaximumAxisScale())
				* ProxyToTarget.GetMaximumAxisScale();
			if (Shape.bBox) {
				Capsule.BoxRotation = (ProxyToTarget.GetRotation() * ComponentPose[Start].GetRotation() * Shape.BoxRotation.Quaternion()).GetNormalized();
				Capsule.BoxExtent = Shape.BoxExtent * ComponentPose[Start].GetMaximumAxisScale() * ProxyToTarget.GetMaximumAxisScale();
			}
			double StartScale, EndScale;
			if (!FollowBody(Start, Capsule.Start, StartScale, Shape.bBox ? &Capsule.BoxRotation : nullptr) || !FollowBody(End, Capsule.End, EndScale)) {
				Error = TEXT("Proxy snapshot could not align an endpoint to the live body pose.");
				return false;
			}
			Capsule.Radius *= FMath::Max(StartScale, EndScale);
			if (Shape.bBox) { Capsule.BoxExtent *= StartScale; Capsule.End = Capsule.Start; }
			if (Capsule.Start.ContainsNaN() || Capsule.End.ContainsNaN() || !FMath::IsFinite(Capsule.Radius) || Capsule.Radius <= 0.f) {
				Error = TEXT("Proxy snapshot target conversion produced invalid geometry.");
				return false;
			}
		}
		OutCapsules = MoveTemp(Capsules);
		if (OutRecordedRoot) {
			// Bone 0 is always the garment mesh root: the minimal hierarchy keeps every ancestor.
			*OutRecordedRoot = ComponentPose[0] * ProxyToTarget;
			OutRecordedRoot->NormalizeRotation();
		}
		return true;
	}

	void ReanchorCapsules(TArray<FKawaiiClothingProxyCapsule>& InOutCapsules, const FTransform& RecordedRoot, const FTransform& LiveRoot) {
		TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_Proxy_ReanchorCapsules);
		const FTransform Recorded(RecordedRoot.GetRotation().GetNormalized(), RecordedRoot.GetTranslation());
		const FTransform Live(LiveRoot.GetRotation().GetNormalized(), LiveRoot.GetTranslation());
		for (FKawaiiClothingProxyCapsule& Capsule : InOutCapsules) {
			Capsule.Start = Live.TransformPosition(Recorded.InverseTransformPosition(Capsule.Start));
			Capsule.End = Live.TransformPosition(Recorded.InverseTransformPosition(Capsule.End));
			if (!Capsule.BoxExtent.IsZero()) { Capsule.BoxRotation = (Live.GetRotation() * Recorded.GetRotation().Inverse() * Capsule.BoxRotation).GetNormalized(); }
		}
	}
}
