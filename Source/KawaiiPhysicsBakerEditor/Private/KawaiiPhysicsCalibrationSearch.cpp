#include "KawaiiPhysicsCalibrationSearch.h"

#include "AnimNode_KawaiiPhysics.h"
#include "KawaiiPhysicsBoneConstraintsDataAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Math/RandomStream.h"
#include "UObject/UnrealType.h"

#include "KawaiiPhysicsCalibrationSettings.h"

namespace KawaiiCalibrationSearch {
	namespace {
		FModifyBoneConstraint AuthoredEdge(const FModifyBoneConstraint& Source);
	}

	void Capture(const FAnimNode_KawaiiPhysics& Node, FKawaiiCalibrationNodeSettings& OutSettings) {
		OutSettings.PhysicsSettings = Node.PhysicsSettings;
		OutSettings.LimitAngleCurveData = Node.LimitAngleCurveData;
		OutSettings.BoneSubdivisionCount = Node.BoneSubdivisionCount;
		OutSettings.bBoneSubdivisionCollisionOnly = Node.bBoneSubdivisionCollisionOnly;
		OutSettings.bBoneSubdivisionDensifyByRadius = Node.bBoneSubdivisionDensifyByRadius;
		OutSettings.BoneConstraintSubdivisionCount = Node.BoneConstraintSubdivisionCount;
		OutSettings.BoneConstraintSubdivisionFeedbackScale = Node.BoneConstraintSubdivisionFeedbackScale;
		OutSettings.BoneConstraintGlobalComplianceType = Node.BoneConstraintGlobalComplianceType;
		OutSettings.BoneConstraintIterationCountBeforeCollision = Node.BoneConstraintIterationCountBeforeCollision;
		OutSettings.BoneConstraintIterationCountAfterCollision = Node.BoneConstraintIterationCountAfterCollision;
		OutSettings.bAutoAddChildDummyBoneConstraint = Node.bAutoAddChildDummyBoneConstraint;
		OutSettings.BoneConstraints.Reset(Node.BoneConstraints.Num());
		for (const auto& Edge : Node.BoneConstraints) {
			OutSettings.BoneConstraints.Add(AuthoredEdge(Edge));
		}
		OutSettings.BoneConstraintsDataAsset = Node.BoneConstraintsDataAsset;
	}

	void Apply(const FKawaiiCalibrationNodeSettings& Settings, FAnimNode_KawaiiPhysics& OutNode) {
		OutNode.PhysicsSettings = Settings.PhysicsSettings;
		OutNode.LimitAngleCurveData = Settings.LimitAngleCurveData;
		OutNode.BoneSubdivisionCount = Settings.BoneSubdivisionCount;
		OutNode.bBoneSubdivisionCollisionOnly = Settings.bBoneSubdivisionCollisionOnly;
		OutNode.bBoneSubdivisionDensifyByRadius = Settings.bBoneSubdivisionDensifyByRadius;
		OutNode.BoneConstraintSubdivisionCount = Settings.BoneConstraintSubdivisionCount;
		OutNode.BoneConstraintSubdivisionFeedbackScale = Settings.BoneConstraintSubdivisionFeedbackScale;
		OutNode.BoneConstraintGlobalComplianceType = Settings.BoneConstraintGlobalComplianceType;
		OutNode.BoneConstraintIterationCountBeforeCollision = Settings.BoneConstraintIterationCountBeforeCollision;
		OutNode.BoneConstraintIterationCountAfterCollision = Settings.BoneConstraintIterationCountAfterCollision;
		OutNode.bAutoAddChildDummyBoneConstraint = Settings.bAutoAddChildDummyBoneConstraint;
		OutNode.BoneConstraints.Reset(Settings.BoneConstraints.Num());
		for (const auto& Edge : Settings.BoneConstraints) {
			OutNode.BoneConstraints.Add(AuthoredEdge(Edge));
		}
		OutNode.BoneConstraintsDataAsset = Settings.BoneConstraintsDataAsset;
		OutNode.RequestModifyBonesReinit();
	}

	namespace {
		bool ValidRange(const FVector2D& Range, double Minimum, double Maximum, const TCHAR* Name, FString& Error) {
			if (!FMath::IsFinite(Range.X) || !FMath::IsFinite(Range.Y) || Range.X < Minimum || Range.Y > Maximum || Range.X > Range.Y) {
				Error = FString::Printf(TEXT("%s范围必须满足 %.3g ≤ 下限 ≤ 上限 ≤ %.3g。"), Name, Minimum, Maximum);
				return false;
			}
			return true;
		}

		bool ValidIntegers(const TArray<int32>& Values, int32 Maximum, const TCHAR* Name, FString& Error) {
			if (Values.IsEmpty()) {
				Error = FString::Printf(TEXT("%s候选列表不能为空。"), Name);
				return false;
			}
			for (int32 Value : Values) {
				if (Value < 0 || Value > Maximum) {
					Error = FString::Printf(TEXT("%s候选必须在 0 至 %d 之间。"), Name, Maximum);
					return false;
				}
			}
			return true;
		}

		FString EdgeKey(FName A, FName B) {
			FString Left = A.ToString(), Right = B.ToString();
			if (Left.Compare(Right, ESearchCase::CaseSensitive) > 0) {
				Swap(Left, Right);
			}
			return Left + TEXT("|") + Right;
		}

		FModifyBoneConstraint AuthoredEdge(const FModifyBoneConstraint& Source) {
			FModifyBoneConstraint Result;
			Result.Bone1.BoneName = Source.Bone1.BoneName;
			Result.Bone2.BoneName = Source.Bone2.BoneName;
			Result.bOverrideCompliance = Source.bOverrideCompliance;
			Result.ComplianceType = Source.ComplianceType;
			Result.bExcludeFromSubdivision = Source.bExcludeFromSubdivision;
			return Result;
		}

		void ConstantAngleCurve(FKawaiiCalibrationNodeSettings& Settings) {
			// Detach from any shared curve before editing the candidate's owned curve data.
			Settings.LimitAngleCurveData.ExternalCurve = nullptr;
			FRichCurve* Curve = Settings.LimitAngleCurveData.GetRichCurve();
			Curve->Reset();
			Curve->SetDefaultValue(1.f);
			Curve->AddKey(0.f, 1.f);
			Curve->AddKey(1.f, 1.f);
		}

		bool BuildPresets(const UKawaiiPhysicsCalibrationSettings& Settings, const FAnimNode_KawaiiPhysics& Node, const FKawaiiCalibrationNodeSettings& Original, TArray<FKawaiiCalibrationNodeSettings>& Results, FString& Error) {
			if (Settings.ConstraintPresets.IsEmpty()) {
				return true;
			}
			if (!Settings.ClothingMesh) {
				Error = TEXT("验证骨骼约束方案需要衣物骨骼网格体。");
				return false;
			}
			const FReferenceSkeleton& Ref = Settings.ClothingMesh->GetRefSkeleton();
			TSet<int32> FixedRoots, Movable;
			const auto AddRoot = [&](FName Name, const TArray<FBoneReference>& Excludes) -> bool {
				const int32 Root = Ref.FindBoneIndex(Name);
				if (Root == INDEX_NONE) {
					Error = TEXT("物理根骨骼不存在：") + Name.ToString();
					return false;
				}
				FixedRoots.Add(Root);
				for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone) {
					bool bExcluded = false;
					for (int32 Parent = Bone; Parent != INDEX_NONE; Parent = Ref.GetParentIndex(Parent)) {
						const FName ParentName = Ref.GetBoneName(Parent);
						bExcluded |= Excludes.ContainsByPredicate([&](const FBoneReference& Exclude) { return Exclude.BoneName == ParentName; });
						if (Parent == Root) {
							if (!bExcluded && Bone != Root) {
								Movable.Add(Bone);
							}
							break;
						}
					}
				}
				return true;
			};
			if (!AddRoot(Node.RootBone.BoneName, Node.ExcludeBones)) {
				return false;
			}
			for (const auto& Root : Node.AdditionalRootBones) {
				if (!AddRoot(Root.RootBone.BoneName, Root.bUseOverrideExcludeBones ? Root.OverrideExcludeBones : Node.ExcludeBones)) {
					return false;
				}
			}
			for (int32 Root : FixedRoots) {
				Movable.Remove(Root);
			}
			TArray<FModifyBoneConstraint> Existing = Original.BoneConstraints;
			if (Original.BoneConstraintsDataAsset) {
				Existing.Append(Original.BoneConstraintsDataAsset->GenerateBoneConstraints());
			}
			for (int32 Index = 0; Index < Settings.ConstraintPresets.Num(); ++Index) {
				const auto& Preset = Settings.ConstraintPresets[Index];
				FKawaiiCalibrationNodeSettings Result = Original;
				TSet<FString> Edges;
				if (Preset.bReplaceExisting) {
					Result.BoneConstraints.Reset();
					Result.BoneConstraintsDataAsset = nullptr;
				} else {
					for (const auto& Edge : Existing) {
						Edges.Add(EdgeKey(Edge.Bone1.BoneName, Edge.Bone2.BoneName));
					}
				}
				for (const auto& Edge : Preset.Constraints) {
					const FName A = Edge.Bone1.BoneName, B = Edge.Bone2.BoneName;
					const int32 IA = Ref.FindBoneIndex(A), IB = Ref.FindBoneIndex(B);
					const FString Key = EdgeKey(A, B);
					if (A == B || IA == INDEX_NONE || IB == INDEX_NONE || !Movable.Contains(IA) || !Movable.Contains(IB) || Edges.Contains(Key)
						|| static_cast<uint8>(Edge.ComplianceType) > static_cast<uint8>(EXPBDComplianceType::Fat))
					{
						Error = FString::Printf(
							TEXT("约束方案 %d（%s）的 %s ↔ %s 无效：骨骼必须存在且位于未排除的可动骨链内，不能自连、重复或连接固定根。"),
							Index + 1,
							*Preset.Name.ToString(),
							*A.ToString(),
							*B.ToString()
						);
						return false;
					}
					Edges.Add(Key);
					Result.BoneConstraints.Add(AuthoredEdge(Edge));
				}
				Results.Add(MoveTemp(Result));
			}
			return true;
		}
	}

	bool Generate(const UKawaiiPhysicsCalibrationSettings& Settings, const FAnimNode_KawaiiPhysics& Node, TArray<FCandidate>& OutCandidates, FString& OutError) {
		OutCandidates.Reset();
		OutError.Reset();
		if (Settings.NativeCandidateCount < 2 || Settings.NativeCandidateCount > 256 || Settings.MaxContactIterations < 1 || Settings.MaxContactIterations > 8) {
			OutError = TEXT("原生候选数量必须为 2 至 256，碰撞迭代上限必须为 1 至 8。");
			return false;
		}
		const bool bPhysics = Settings.bSearchNodeSettings && Settings.bSearchPhysicsSettings;
		const bool bConstraints = Settings.bSearchNodeSettings && Settings.bSearchBoneConstraints;
		if (bPhysics
			&& (!ValidRange(Settings.DampingRange, 0, 1, TEXT("阻尼"), OutError) || !ValidRange(Settings.StiffnessRange, 0, 1, TEXT("刚度"), OutError)
				|| !ValidRange(Settings.RadiusRange, .001, 1000, TEXT("骨骼碰撞半径"), OutError)
				|| !ValidRange(Settings.LimitAngleRange, 0, 180, TEXT("摆动角度"), OutError)))
		{
			return false;
		}
		if (bConstraints) {
			if (!ValidIntegers(Settings.BeforeCollisionIterations, 16, TEXT("碰撞前约束迭代"), OutError)
				|| !ValidIntegers(Settings.AfterCollisionIterations, 16, TEXT("碰撞后约束迭代"), OutError)
				|| !ValidIntegers(Settings.VerticalSubdivisions, 10, TEXT("纵向细分"), OutError)
				|| !ValidIntegers(Settings.HorizontalSubdivisions, 10, TEXT("横向细分"), OutError)
				|| !ValidRange(Settings.FeedbackScaleRange, 0, 2, TEXT("横向碰撞反馈"), OutError))
			{
				return false;
			}
			if (Settings.ComplianceCandidates.IsEmpty()) {
				OutError = TEXT("约束柔顺度候选列表不能为空。");
				return false;
			}
			for (auto Value : Settings.ComplianceCandidates) {
				if (static_cast<uint8>(Value) > static_cast<uint8>(EXPBDComplianceType::Fat)) {
					OutError = TEXT("约束柔顺度候选包含无效枚举值。");
					return false;
				}
			}
		}
		FCandidate Original;
		Capture(Node, Original.NodeSettings);
		Original.bOriginalBaseline = true;
		Original.Label = TEXT("原始节点与原校准配置");
		Original.ProfileParameters = FVector4f(1, 1, 1, 0);
		if (const auto* Profile = Node.CalibrationProfile.Get(); Profile && (Profile->TargetRoot.IsNone() || Profile->TargetRoot == Node.RootBone.BoneName)) {
			Original.ProfileParameters =
				FVector4f(Profile->DampingScale, Profile->StiffnessScale, Profile->RadiusScale, FMath::Clamp(Profile->ContactIterations, 1, 8));
		}
		TArray<FKawaiiCalibrationNodeSettings> Presets;
		if (bConstraints && !BuildPresets(Settings, Node, Original.NodeSettings, Presets, OutError)) {
			return false;
		}
		if (Presets.Num() + 2 > Settings.NativeCandidateCount) {
			OutError = FString::Printf(TEXT("原生候选数量至少需要 %d，才能保留原始基线、新接触配置和每个约束方案。"), Presets.Num() + 2);
			return false;
		}
		OutCandidates.Add(Original);
		FCandidate Base = Original;
		Base.bOriginalBaseline = false;
		Base.ProfileParameters = FVector4f(1, 1, 1, FMath::Min(2, Settings.MaxContactIterations));
		Base.Label = TEXT("原节点参数与新接触配置");
		TSet<FString> Seen;
		const auto Add = [&](FCandidate Candidate) {
			if (OutCandidates.Num() >= Settings.NativeCandidateCount) {
				return;
			}
			FString Key;
			FKawaiiCalibrationNodeSettings::StaticStruct()->ExportText(Key, &Candidate.NodeSettings, nullptr, nullptr, PPF_None, nullptr);
			Key += FString::Printf(TEXT("|contact=%d"), FMath::RoundToInt(Candidate.ProfileParameters.W));
			if (!Seen.Contains(Key)) {
				Seen.Add(Key);
				OutCandidates.Add(MoveTemp(Candidate));
			}
		};
		Add(Base);
		// Every explicit topology gets a candidate before the scalar axes consume the budget.
		for (int32 I = 0; I < Presets.Num(); ++I) {
			FCandidate C = Base;
			C.NodeSettings = Presets[I];
			C.Label = FString::Printf(TEXT("骨骼约束方案 %d：%s"), I + 1, *Settings.ConstraintPresets[I].Name.ToString());
			Add(MoveTemp(C));
		}
		const auto Axis = [&](const TCHAR* Name, const FVector2D& Range, const auto& Set) {
			for (double Value : {Range.X, (Range.X + Range.Y) * .5, Range.Y}) {
				FCandidate C = Base;
				Set(C.NodeSettings, static_cast<float>(Value));
				C.Label = FString::Printf(TEXT("%s = %.4g"), Name, Value);
				Add(MoveTemp(C));
			}
		};
		if (bPhysics) {
			Axis(TEXT("阻尼"), Settings.DampingRange, [](auto& C, float V) { C.PhysicsSettings.Damping = V; });
			Axis(TEXT("刚度"), Settings.StiffnessRange, [](auto& C, float V) { C.PhysicsSettings.Stiffness = V; });
			Axis(TEXT("骨骼碰撞半径"), Settings.RadiusRange, [](auto& C, float V) { C.PhysicsSettings.Radius = V; });
			Axis(TEXT("摆动角度"), Settings.LimitAngleRange, [](auto& C, float V) { C.PhysicsSettings.LimitAngle = V; });
			if (Settings.bTryConstantLimitAngleCurve) {
				FCandidate C = Base;
				ConstantAngleCurve(C.NodeSettings);
				C.Label = TEXT("原摆动角度与恒定角度曲线");
				Add(MoveTemp(C));
				Axis(TEXT("恒定曲线摆动角度"), Settings.LimitAngleRange, [](auto& N, float V) {
					N.PhysicsSettings.LimitAngle = V;
					ConstantAngleCurve(N);
				});
			}
		}
		const auto Choices = [&](const TCHAR* Name, const auto& Values, const auto& Set) {
			for (const auto Value : Values) {
				FCandidate C = Base;
				Set(C.NodeSettings, Value);
				C.Label = FString::Printf(TEXT("%s = %d"), Name, static_cast<int32>(Value));
				Add(MoveTemp(C));
			}
		};
		if (bConstraints) {
			Choices(TEXT("碰撞前约束迭代"), Settings.BeforeCollisionIterations, [](auto& C, int32 V) { C.BoneConstraintIterationCountBeforeCollision = V; });
			Choices(TEXT("碰撞后约束迭代"), Settings.AfterCollisionIterations, [](auto& C, int32 V) { C.BoneConstraintIterationCountAfterCollision = V; });
			Choices(TEXT("约束柔顺度"), Settings.ComplianceCandidates, [](auto& C, auto V) { C.BoneConstraintGlobalComplianceType = V; });
			Choices(TEXT("纵向细分"), Settings.VerticalSubdivisions, [](auto& C, int32 V) { C.BoneSubdivisionCount = V; });
			Choices(TEXT("横向细分"), Settings.HorizontalSubdivisions, [](auto& C, int32 V) { C.BoneConstraintSubdivisionCount = V; });
			Axis(TEXT("横向碰撞反馈"), Settings.FeedbackScaleRange, [](auto& C, float V) { C.BoneConstraintSubdivisionFeedbackScale = V; });
			if (Settings.bTryDisableRadiusDensification) {
				FCandidate C = Base;
				C.NodeSettings.bBoneSubdivisionDensifyByRadius = false;
				C.Label = TEXT("关闭纵向半径加密");
				Add(MoveTemp(C));
			}
		}
		for (int32 Iteration = 1; Iteration <= Settings.MaxContactIterations; ++Iteration) {
			FCandidate C = Base;
			C.ProfileParameters.W = Iteration;
			C.Label = FString::Printf(TEXT("接触迭代 = %d"), Iteration);
			Add(MoveTemp(C));
		}
		FRandomStream Random(Settings.Seed);
		const auto PickRange = [&](const FVector2D& Range) { return Random.FRandRange(static_cast<float>(Range.X), static_cast<float>(Range.Y)); };
		const auto PickIndex = [&](int32 Count) { return Random.RandRange(0, Count - 1); };
		// A finite attempt budget also handles contact-only or completely fixed search spaces.
		for (int32 Attempt = 0; OutCandidates.Num() < Settings.NativeCandidateCount && Attempt < Settings.NativeCandidateCount * 32; ++Attempt) {
			FCandidate C = Base;
			if (bConstraints && !Presets.IsEmpty()) {
				const int32 Preset = Random.RandRange(0, Presets.Num());
				if (Preset < Presets.Num()) {
					C.NodeSettings = Presets[Preset];
				}
			}
			auto& N = C.NodeSettings;
			if (bPhysics) {
				N.PhysicsSettings.Damping = PickRange(Settings.DampingRange);
				N.PhysicsSettings.Stiffness = PickRange(Settings.StiffnessRange);
				N.PhysicsSettings.Radius = PickRange(Settings.RadiusRange);
				N.PhysicsSettings.LimitAngle = PickRange(Settings.LimitAngleRange);
				if (Settings.bTryConstantLimitAngleCurve && Random.RandRange(0, 1)) {
					ConstantAngleCurve(N);
				}
			}
			if (bConstraints) {
				N.BoneConstraintIterationCountBeforeCollision = Settings.BeforeCollisionIterations[PickIndex(Settings.BeforeCollisionIterations.Num())];
				N.BoneConstraintIterationCountAfterCollision = Settings.AfterCollisionIterations[PickIndex(Settings.AfterCollisionIterations.Num())];
				N.BoneConstraintGlobalComplianceType = Settings.ComplianceCandidates[PickIndex(Settings.ComplianceCandidates.Num())];
				N.BoneSubdivisionCount = Settings.VerticalSubdivisions[PickIndex(Settings.VerticalSubdivisions.Num())];
				N.BoneConstraintSubdivisionCount = Settings.HorizontalSubdivisions[PickIndex(Settings.HorizontalSubdivisions.Num())];
				N.BoneConstraintSubdivisionFeedbackScale = PickRange(Settings.FeedbackScaleRange);
				if (Settings.bTryDisableRadiusDensification && Random.RandRange(0, 1)) {
					N.bBoneSubdivisionDensifyByRadius = false;
				}
			}
			C.ProfileParameters.W = Random.RandRange(1, Settings.MaxContactIterations);
			C.Label = FString::Printf(TEXT("组合候选 %d（种子 %d）"), Attempt + 1, Settings.Seed);
			Add(MoveTemp(C));
		}
		return true;
	}

	double Cost(const FCandidate& Candidate) {
		const auto& N = Candidate.NodeSettings;
		int32 Edges = N.BoneConstraints.Num();
		if (N.BoneConstraintsDataAsset) {
			Edges += N.BoneConstraintsDataAsset->BoneConstraintsData.Num();
		}
		const double BeforeAfter = FMath::Max(0, N.BoneConstraintIterationCountBeforeCollision) + FMath::Max(0, N.BoneConstraintIterationCountAfterCollision);
		const double Vertical = FMath::Clamp(N.BoneSubdivisionCount, 0, 10);
		const double Horizontal = FMath::Clamp(N.BoneConstraintSubdivisionCount, 0, 10);
		// Stable workload proxy, not wall-clock time. Original profiles retain their real contact count.
		return FMath::Max(0.f, Candidate.ProfileParameters.W) + .25 * BeforeAfter + .5 * Vertical + .25 * Horizontal
			+ .01 * Edges * (BeforeAfter * (N.bAutoAddChildDummyBoneConstraint ? 1. + Vertical : 1.) + Horizontal)
			+ (N.bBoneSubdivisionDensifyByRadius && Vertical > 0 ? .5 : 0.);
	}
}
