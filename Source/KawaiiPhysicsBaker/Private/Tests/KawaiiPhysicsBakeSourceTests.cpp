#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#include "AnimNode_KawaiiPhysicsBakeSource.h"
#include "KawaiiPhysicsBakePoseUtils.h"

namespace KawaiiBakeSourceTests {
	void AddBone(FReferenceSkeletonModifier& Modifier, const TCHAR* Name, int32 Parent, FVector Translation) {
		Modifier.Add(FMeshBoneInfo(FName(Name), FString(Name), Parent), FTransform(Translation));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiBakeCopyHierarchyTest,
	"KawaiiPhysicsBaker.Source.CopyDifferentHierarchy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiBakeCopyHierarchyTest::RunTest(const FString& Parameters) {
	FReferenceSkeleton SourceSkeleton;
	FReferenceSkeleton TargetSkeleton;
	{
		FReferenceSkeletonModifier Modifier(SourceSkeleton, nullptr);
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("root"), INDEX_NONE, FVector::ZeroVector);
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("pelvis"), 0, FVector(0, 0, 10));
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("thigh"), 1, FVector(0, 0, -5));
	}
	{
		FReferenceSkeletonModifier Modifier(TargetSkeleton, nullptr);
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("root"), INDEX_NONE, FVector::ZeroVector);
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("pelvis"), 0, FVector(0, 0, 10));
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("garment_parent"), 1, FVector(0, 2, 0));
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("thigh"), 2, FVector(0, -2, -5));
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("skirt_tip"), 3, FVector(1, 2, -3));
	}
	const TArray<FTransform> SourceCS{
		FTransform(FRotator(0, 15, 0), FVector(10, 0, 0)),
		FTransform(FRotator(0, 30, 0), FVector(12, 0, 15)),
		FTransform(FRotator(20, 40, 0), FVector(16, 1, 7))
	};
	const FTransform SourceToTarget(FRotator(0, -10, 0), FVector(0, 5, 0));
	TArray<FTransform> Local;
	FString Error;
	TestTrue(
		TEXT("Distinct skeletons with shared bones can copy"),
		KawaiiPhysicsBake::BuildCopiedLocalPose(SourceSkeleton, SourceCS, TargetSkeleton, SourceToTarget, Local, Error)
	);
	if (Local.Num() != TargetSkeleton.GetNum()) {
		return false;
	}
	TArray<FTransform> ResultCS;
	ResultCS.SetNum(Local.Num());
	for (int32 Index = 0; Index < Local.Num(); ++Index) {
		const int32 Parent = TargetSkeleton.GetParentIndex(Index);
		ResultCS[Index] = Parent == INDEX_NONE ? Local[Index] : Local[Index] * ResultCS[Parent];
	}
	TestTrue(TEXT("Root includes component offset"), ResultCS[0].Equals(SourceCS[0] * SourceToTarget));
	TestTrue(TEXT("Mapped thigh preserves source CS despite a different target parent"), ResultCS[3].Equals(SourceCS[2] * SourceToTarget));
	TestTrue(TEXT("Unmatched intermediate bone keeps reference local pose"), Local[2].Equals(TargetSkeleton.GetRefBonePose()[2]));
	TestTrue(TEXT("Unmatched skirt chain keeps reference local pose"), Local[4].Equals(TargetSkeleton.GetRefBonePose()[4]));

	TArray<FTransform> IncompleteCS = SourceCS;
	IncompleteCS.Pop();
	TestFalse(
		TEXT("Incomplete source snapshots are rejected"),
		KawaiiPhysicsBake::BuildCopiedLocalPose(SourceSkeleton, IncompleteCS, TargetSkeleton, SourceToTarget, Local, Error)
	);
	TestFalse(TEXT("Incomplete snapshot has an actionable error"), Error.IsEmpty());
	FReferenceSkeleton UnrelatedSkeleton;
	{
		FReferenceSkeletonModifier Modifier(UnrelatedSkeleton, nullptr);
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("unrelated_root"), INDEX_NONE, FVector::ZeroVector);
	}
	TestFalse(
		TEXT("No shared names is an error, not a successful reference-pose bake"),
		KawaiiPhysicsBake::BuildCopiedLocalPose(SourceSkeleton, SourceCS, UnrelatedSkeleton, SourceToTarget, Local, Error)
	);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiBakeExactSourceTest,
	"KawaiiPhysicsBaker.Source.ExactTimeCurvesAndErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiBakeExactSourceTest::RunTest(const FString& Parameters) {
	USkeleton* Skeleton = NewObject<USkeleton>();
	{
		FReferenceSkeletonModifier Modifier(Skeleton);
		KawaiiBakeSourceTests::AddBone(Modifier, TEXT("root"), INDEX_NONE, FVector::ZeroVector);
	}
	USkeletalMesh* Mesh = NewObject<USkeletalMesh>();
	Mesh->SetSkeleton(Skeleton);
	Mesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
	USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>();
	Component->SetSkeletalMesh(Mesh);
	UKawaiiPhysicsBakeAnimInstance* Instance = NewObject<UKawaiiPhysicsBakeAnimInstance>(Component);
	const TArray<FBoneIndexType> BoneIndices{0};
	const UE::Anim::FCurveFilterSettings CurveFilter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh);
	Instance->GetRequiredBones().SetUseRAWData(true);
	FAnimInstanceProxy Proxy(Instance);
	Proxy.GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh);
	Proxy.GetRequiredBones().SetUseRAWData(true);

	UAnimSequence* Sequence = NewObject<UAnimSequence>();
	Sequence->SetSkeleton(Skeleton);
	Sequence->bEnableRootMotion = true;
	Sequence->bForceRootLock = true;
	IAnimationDataController& Controller = Sequence->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Bake source test data")), false);
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(1, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(1), false);
	Controller.AddBoneCurve(TEXT("root"), false);
	Controller.SetBoneTrackKeys(
		TEXT("root"),
		TArray<FVector3f>{FVector3f::ZeroVector, FVector3f(100, 0, 0)},
		TArray<FQuat4f>{FQuat4f::Identity, FQuat4f::Identity},
		TArray<FVector3f>{FVector3f::OneVector, FVector3f::OneVector},
		false
	);
	const FName CurveName(TEXT("BakeTestCurve"));
	const FAnimationCurveIdentifier CurveId(CurveName, ERawCurveTrackTypes::RCT_Float);
	Controller.AddCurve(CurveId, AACF_DefaultCurve, false);
	Controller.SetCurveKeys(CurveId, TArray<FRichCurveKey>{FRichCurveKey(0.f, 0.f), FRichCurveKey(1.f, 8.f)}, false);
	Controller.NotifyPopulated();
	Controller.CloseBracket(false);

	FMemMark Mark(FMemStack::Get());
	FAnimNode_KawaiiPhysicsBakeSource Node;
	Instance->SetBakeSource(Sequence, 0.25f);
	Node.PreUpdate(Instance);
	TestTrue(TEXT("Valid source has no error"), Instance->GetSourceError().IsEmpty());
	TestEqual(TEXT("Sampling without evaluation cannot claim an output was baked"), Instance->GetSourceEvaluationSerial(), uint64(0));
	FPoseContext Output(&Proxy);
	Node.Evaluate_AnyThread(Output);
	TestEqual(TEXT("Evaluation serial advances after valid output"), Instance->GetSourceEvaluationSerial(), uint64(1));
	TestEqual(TEXT("Exact sample time is reported"), Instance->GetLastEvaluatedSourceTime(), 0.25f);
	TestTrue(
		TEXT("Root motion and forced lock do not strip authored root translation"),
		Output.Pose[FCompactPoseBoneIndex(0)].GetTranslation().Equals(FVector(25, 0, 0), 0.001)
	);
	TestTrue(TEXT("Curves are sampled at the same exact time"), FMath::IsNearlyEqual(Output.Curve.Get(CurveName), 2.f));

	Instance->SetBakeSource(Sequence, 0.75f);
	Node.PreUpdate(Instance);
	Node.Evaluate_AnyThread(Output);
	TestTrue(
		TEXT("Explicit time can advance independently of editor frame rate"),
		Output.Pose[FCompactPoseBoneIndex(0)].GetTranslation().Equals(FVector(75, 0, 0), 0.001)
	);
	TestTrue(TEXT("Curve follows second explicit time"), FMath::IsNearlyEqual(Output.Curve.Get(CurveName), 6.f));

	UAnimSequence* WrongSkeletonSequence = NewObject<UAnimSequence>();
	WrongSkeletonSequence->SetSkeleton(NewObject<USkeleton>());
	Instance->SetBakeSource(WrongSkeletonSequence, 0.f);
	Node.PreUpdate(Instance);
	Node.Evaluate_AnyThread(Output);
	TestFalse(TEXT("Wrong source skeleton is reported"), Instance->GetSourceError().IsEmpty());
	TestEqual(TEXT("Failed source cannot increment successful evaluation serial"), Instance->GetSourceEvaluationSerial(), uint64(2));
	return true;
}

#endif
