#include "AnimNode_KawaiiPhysics.h"
#include "KawaiiPhysicsCalibrationProfile.h"
#include "Engine/SkeletalMesh.h"
#include "AnimGraphNode_KawaiiPhysics.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiPhysicsCalibrationSearch.h"
#include "KawaiiPhysicsCalibrationSettings.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace {
	struct FCalibrationSearchFixture
	{
		TStrongObjectPtr<USkeletalMesh> Mesh{NewObject<USkeletalMesh>()};
		TStrongObjectPtr<UKawaiiPhysicsCalibrationSettings> Settings{NewObject<UKawaiiPhysicsCalibrationSettings>()};
		FAnimNode_KawaiiPhysics Node;

		FCalibrationSearchFixture() {
			FReferenceSkeletonModifier Bones(Mesh->GetRefSkeleton(), nullptr);
			Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
			Bones.Add(FMeshBoneInfo(TEXT("anchor_a"), TEXT("anchor_a"), 0), FTransform(FVector(0, 0, 10)));
			Bones.Add(FMeshBoneInfo(TEXT("a1"), TEXT("a1"), 1), FTransform(FVector(0, 0, -5)));
			Bones.Add(FMeshBoneInfo(TEXT("a2"), TEXT("a2"), 2), FTransform(FVector(0, 0, -5)));
			Bones.Add(FMeshBoneInfo(TEXT("anchor_b"), TEXT("anchor_b"), 0), FTransform(FVector(10, 0, 10)));
			Bones.Add(FMeshBoneInfo(TEXT("b1"), TEXT("b1"), 4), FTransform(FVector(0, 0, -5)));
			Bones.Add(FMeshBoneInfo(TEXT("b2"), TEXT("b2"), 5), FTransform(FVector(0, 0, -5)));
			Node.RootBone.BoneName = TEXT("anchor_a");
			Node.AdditionalRootBones.AddDefaulted_GetRef().RootBone.BoneName = TEXT("anchor_b");
			Node.PhysicsSettings.Damping = .28f;
			Node.PhysicsSettings.Stiffness = .12f;
			Node.PhysicsSettings.Radius = 1.5f;
			Node.PhysicsSettings.LimitAngle = 35.f;
			Node.LimitAngleCurveData.GetRichCurve()->AddKey(0.f, .35f);
			Node.LimitAngleCurveData.GetRichCurve()->AddKey(1.f, 1.f);
			Node.BoneSubdivisionCount = 1;
			Node.bBoneSubdivisionDensifyByRadius = true;
			Node.BoneConstraintIterationCountBeforeCollision = 3;
			Node.BoneConstraintIterationCountAfterCollision = 0;
			Settings->ClothingMesh = Mesh.Get();
			Settings->NativeCandidateCount = 64;
		}
	};

	FString SnapshotText(const FKawaiiCalibrationNodeSettings& Settings) {
		FString Text;
		FKawaiiCalibrationNodeSettings::StaticStruct()->ExportText(Text, &Settings, nullptr, nullptr, PPF_None, nullptr);
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiCalibrationSearchDeterminism,
	"KawaiiPhysicsBaker.Calibration.Search.DeterministicCandidates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiCalibrationSearchDeterminism::RunTest(const FString&) {
	FCalibrationSearchFixture F;
	TArray<KawaiiCalibrationSearch::FCandidate> A, B;
	FString Error;
	if (!TestTrue(TEXT("Candidate search accepts a valid fixture"), KawaiiCalibrationSearch::Generate(*F.Settings, F.Node, A, Error))) {
		AddError(Error);
		return false;
	}
	if (!TestTrue(TEXT("Repeated generation succeeds"), KawaiiCalibrationSearch::Generate(*F.Settings, F.Node, B, Error))) {
		return false;
	}
	TestTrue(TEXT("Budget bounds the native candidate count"), A.Num() <= F.Settings->NativeCandidateCount && A.Num() >= 2);
	TestEqual(TEXT("Repeated generation keeps the candidate count"), A.Num(), B.Num());
	TestTrue(TEXT("Candidate zero preserves the original runtime baseline"), A[0].bOriginalBaseline);
	bool bAngleChanged = false, bSubdivisionChanged = false, bConstraintsChanged = false, bRadiusChanged = false;
	for (int32 I = 0; I < A.Num(); ++I) {
		TestEqual(TEXT("Same seed produces the exact node recipe"), SnapshotText(A[I].NodeSettings), SnapshotText(B[I].NodeSettings));
		TestTrue(TEXT("Same seed produces the same contact parameters"), A[I].ProfileParameters == B[I].ProfileParameters);
		FAnimNode_KawaiiPhysics Applied = F.Node;
		KawaiiCalibrationSearch::Apply(A[I].NodeSettings, Applied);
		TestEqual(TEXT("Search keeps the root anchor"), Applied.RootBone.BoneName, F.Node.RootBone.BoneName);
		TestEqual(TEXT("Unidentifiable world damping is preserved"), Applied.PhysicsSettings.WorldDampingLocation, F.Node.PhysicsSettings.WorldDampingLocation);
		bAngleChanged |= Applied.PhysicsSettings.LimitAngle != F.Node.PhysicsSettings.LimitAngle;
		bRadiusChanged |= Applied.PhysicsSettings.Radius != F.Node.PhysicsSettings.Radius;
		bSubdivisionChanged |= Applied.BoneSubdivisionCount != F.Node.BoneSubdivisionCount;
		bConstraintsChanged |= Applied.BoneConstraintIterationCountBeforeCollision != F.Node.BoneConstraintIterationCountBeforeCollision;
		if (!A[I].bOriginalBaseline) {
			TestTrue(
				TEXT("Absolute node parameters are not multiplied twice by the profile"),
				A[I].ProfileParameters.X == 1.f && A[I].ProfileParameters.Y == 1.f && A[I].ProfileParameters.Z == 1.f
			);
		}
	}
	TestTrue(
		TEXT("Angle, radius, topology and constraint iterations receive CPU candidates"),
		bAngleChanged && bRadiusChanged && bSubdivisionChanged && bConstraintsChanged
	);
	TestEqual(TEXT("Generating candidates does not flatten the authored angle curve"), F.Node.LimitAngleCurveData.GetRichCurveConst()->Eval(0.f), .35f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiCalibrationSearchLocks,
	"KawaiiPhysicsBaker.Calibration.Search.RespectsSearchSwitches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiCalibrationSearchLocks::RunTest(const FString&) {
	FCalibrationSearchFixture F;
	F.Settings->bSearchPhysicsSettings = false;
	F.Settings->bSearchBoneConstraints = false;
	FKawaiiCalibrationNodeSettings Original;
	KawaiiCalibrationSearch::Capture(F.Node, Original);
	TArray<KawaiiCalibrationSearch::FCandidate> Candidates;
	FString Error;
	if (!TestTrue(TEXT("Contact-only candidates can be generated"), KawaiiCalibrationSearch::Generate(*F.Settings, F.Node, Candidates, Error))) {
		AddError(Error);
		return false;
	}
	for (const auto& Candidate : Candidates) {
		TestEqual(TEXT("Disabled node dimensions preserve all authored settings"), SnapshotText(Candidate.NodeSettings), SnapshotText(Original));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiCalibrationSearchInvalidRange,
	"KawaiiPhysicsBaker.Calibration.Search.RejectsInvalidRanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiCalibrationSearchInvalidRange::RunTest(const FString&) {
	FCalibrationSearchFixture F;
	F.Settings->RadiusRange = FVector2D(3, .5);
	TArray<KawaiiCalibrationSearch::FCandidate> Candidates;
	FString Error;
	TestFalse(TEXT("An inverted search range cannot start a search"), KawaiiCalibrationSearch::Generate(*F.Settings, F.Node, Candidates, Error));
	TestFalse(TEXT("Invalid input supplies an actionable error"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiCalibrationSearchRecipeRoundTrip,
	"KawaiiPhysicsBaker.Calibration.Search.SerializedNodeRecipe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiCalibrationSearchRecipeRoundTrip::RunTest(const FString&) {
	FCalibrationSearchFixture F;
	auto& Edge = F.Node.BoneConstraints.AddDefaulted_GetRef();
	Edge.Bone1.BoneName = TEXT("a1");
	Edge.Bone2.BoneName = TEXT("b1");
	Edge.bOverrideCompliance = true;
	Edge.ComplianceType = EXPBDComplianceType::Rubber;
	Edge.bExcludeFromSubdivision = true;
	TStrongObjectPtr<UKawaiiPhysicsCalibrationProfile> Profile(NewObject<UKawaiiPhysicsCalibrationProfile>());
	Profile->bHasOptimizedNodeSettings = true;
	KawaiiCalibrationSearch::Capture(F.Node, Profile->OptimizedNodeSettings);
	Profile->OptimizedNodeSettings.PhysicsSettings.LimitAngle = 70.f;
	Profile->ContactIterations = 4;
	Profile->bUseAttachedParent = false;
	Profile->bUseSceneContacts = false;
	TArray<uint8> Data;
	FObjectWriter Writer(Profile.Get(), Data);
	TStrongObjectPtr<UKawaiiPhysicsCalibrationProfile> Reloaded(NewObject<UKawaiiPhysicsCalibrationProfile>());
	FObjectReader Reader(Reloaded.Get(), Data);
	TestTrue(TEXT("The optimized-node recipe marker survives serialization"), Reloaded->bHasOptimizedNodeSettings);
	TestEqual(TEXT("The exact recipe survives serialization"), SnapshotText(Profile->OptimizedNodeSettings), SnapshotText(Reloaded->OptimizedNodeSettings));
	TestEqual(TEXT("Contact iteration count survives serialization"), Reloaded->ContactIterations, 4);
	TestFalse(TEXT("The saved profile does not change to attached-parent contacts"), Reloaded->bUseAttachedParent);
	FAnimNode_KawaiiPhysics Applied;
	Applied.RootBone.BoneName = TEXT("untouched_anchor");
	Applied.Alpha = .37f;
	KawaiiCalibrationSearch::Apply(Reloaded->OptimizedNodeSettings, Applied);
	TestEqual(TEXT("Application updates the node angle, not a post-contact chain override"), Applied.PhysicsSettings.LimitAngle, 70.f);
	TestTrue(TEXT("No post-contact range override is introduced"), Reloaded->ChainLimits.IsEmpty());
	TestEqual(TEXT("Unsearched node root remains intact"), Applied.RootBone.BoneName, FName(TEXT("untouched_anchor")));
	TestEqual(TEXT("Graph alpha is not overwritten by the recipe"), Applied.Alpha, .37f);
	TestTrue(
		TEXT("Explicit edge compliance and subdivision opt-out survive"),
		Applied.BoneConstraints.Num() == 1 && Applied.BoneConstraints[0].bOverrideCompliance
			&& Applied.BoneConstraints[0].ComplianceType == EXPBDComplianceType::Rubber && Applied.BoneConstraints[0].bExcludeFromSubdivision
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiCalibrationSearchConstraintPresets,
	"KawaiiPhysicsBaker.Calibration.Search.ConstraintPresetValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiCalibrationSearchConstraintPresets::RunTest(const FString&) {
	FCalibrationSearchFixture F;
	auto& Preset = F.Settings->ConstraintPresets.AddDefaulted_GetRef();
	Preset.Name = TEXT("Explicit cross-chain pairing");
	Preset.bReplaceExisting = true;
	auto& Edge = Preset.Constraints.AddDefaulted_GetRef();
	Edge.Bone1.BoneName = TEXT("anchor_a");
	Edge.Bone2.BoneName = TEXT("b1");
	TArray<KawaiiCalibrationSearch::FCandidate> Candidates;
	FString Error;
	TestFalse(TEXT("An added XPBD edge cannot move a fixed root anchor"), KawaiiCalibrationSearch::Generate(*F.Settings, F.Node, Candidates, Error));
	Edge.Bone1.BoneName = TEXT("a1");
	if (!TestTrue(
			TEXT("An explicitly supplied movable cross-chain edge is accepted"),
			KawaiiCalibrationSearch::Generate(*F.Settings, F.Node, Candidates, Error)
		))
	{
		AddError(Error);
		return false;
	}
	bool bCovered = false;
	for (const auto& Candidate : Candidates) {
		bCovered |= Candidate.NodeSettings.BoneConstraints.Num() == 1 && Candidate.NodeSettings.BoneConstraints[0].Bone1.BoneName == TEXT("a1")
			&& Candidate.NodeSettings.BoneConstraints[0].Bone2.BoneName == TEXT("b1");
	}
	TestTrue(TEXT("The supplied topology receives a native candidate"), bCovered);
	Edge.Bone2.BoneName = TEXT("a1");
	TestFalse(TEXT("Self constraints are rejected"), KawaiiCalibrationSearch::Generate(*F.Settings, F.Node, Candidates, Error));
	Edge.Bone2.BoneName = TEXT("missing_bone");
	TestFalse(TEXT("Missing bones are rejected before simulation"), KawaiiCalibrationSearch::Generate(*F.Settings, F.Node, Candidates, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiCalibrationContactPoseSource,
	"KawaiiPhysicsBaker.Calibration.Search.ContactPoseSourceValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiCalibrationContactPoseSource::RunTest(const FString&) {
	FCalibrationSearchFixture F;
	TStrongObjectPtr<USkeletalMesh> Body(NewObject<USkeletalMesh>());
	{
		FReferenceSkeletonModifier Bones(Body->GetRefSkeleton(), nullptr);
		Bones.Add(FMeshBoneInfo(TEXT("body_only"), TEXT("body_only"), INDEX_NONE), FTransform::Identity);
	}
	TStrongObjectPtr<UAnimBlueprint> Blueprint(NewObject<UAnimBlueprint>());
	UEdGraph* Graph = NewObject<UEdGraph>(Blueprint.Get());
	Blueprint->FunctionGraphs.Add(Graph);
	auto* GraphNode = NewObject<UAnimGraphNode_KawaiiPhysics>(Graph);
	Graph->Nodes.Add(GraphNode);
	GraphNode->Node = F.Node;
	F.Settings->BodyMesh = Body.Get();
	F.Settings->ClothingBlueprint = Blueprint.Get();
	auto& Capsule = F.Settings->BodyCapsules.AddDefaulted_GetRef();
	Capsule.StartBone = Capsule.EndBone = TEXT("body_only");
	FString Source, Error;
	if (!TestNotNull(TEXT("The fixture resolves its physics node"), F.Settings->FindPhysicsNode())) {
		return false;
	}
	TestFalse(TEXT("Input-pose contacts reject missing clothing bones instead of silently dropping them"), F.Settings->ResolveConstraints(Source, Error));
	TestTrue(FString(TEXT("The missing bone is identified: ")) + Error, Error.Contains(TEXT("body_only")));
	F.Settings->bUseAttachedParentForContacts = true;
	const bool bParentSupported = F.Settings->ResolveConstraints(Source, Error);
	TestTrue(FString(TEXT("A separate body skeleton remains supported through explicit parent contacts: ")) + Error, bParentSupported);
	TStrongObjectPtr<UKawaiiPhysicsCalibrationProfile> Profile(NewObject<UKawaiiPhysicsCalibrationProfile>());
	F.Settings->CopyConstraintsToProfile(*Profile);
	TestTrue(TEXT("Saved contact source matches the tested source"), Profile->bUseAttachedParent);
	return true;
}
#endif
