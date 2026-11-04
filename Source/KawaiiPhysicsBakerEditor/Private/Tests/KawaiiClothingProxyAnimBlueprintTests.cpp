#include "AnimGraphNode_KawaiiAutoClothingProxy.h"
#include "KawaiiClothingProxyAnimSubsystem.h"

#include "AnimGraphNode_SequenceEvaluator.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
struct FProxyCompiledGraphTestProxy : FAnimInstanceProxy
{
	explicit FProxyCompiledGraphTestProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}
	using FAnimInstanceProxy::Initialize;
	using FAnimInstanceProxy::InitializeRootNode;
	using FAnimInstanceProxy::UpdateAnimation;
	using FAnimInstanceProxy::EvaluateAnimation;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiProxyAnimBlueprintSubsystemTest,
	"KawaiiPhysicsBaker.ClothingProxy.AutoNode.CompiledMontageSubsystem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiProxyAnimBlueprintSubsystemTest::RunTest(const FString&)
{
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/KawaiiAutoProxyCompile_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetFlags(RF_Transient);
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>(Package));
	TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>(Package));
	{
		FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), nullptr);
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		Modifier.Add(FMeshBoneInfo(TEXT("child"), TEXT("child"), 0), FTransform(FVector(0, 0, 10)));
	}
	Mesh->SetSkeleton(Skeleton.Get());
	// ReferenceSkeletonModifier alone does not populate USkeleton::BoneTree. Merge the
	// mesh through the normal skeleton API before accessing retargeting modes.
	if (!TestTrue(TEXT("The test mesh populates the skeleton bone tree"), Skeleton->MergeAllBonesToBoneTree(Mesh.Get(), false))) { return false; }
	Skeleton->SetBoneTranslationRetargetingMode(0, EBoneTranslationRetargetingMode::Animation);
	Skeleton->SetBoneTranslationRetargetingMode(1, EBoneTranslationRetargetingMode::Animation);
	Mesh->CalculateInvRefMatrices();
	TStrongObjectPtr<UAnimSequence> Animation(NewObject<UAnimSequence>(Package));
	Animation->SetSkeleton(Skeleton.Get());
	IAnimationDataController& Controller = Animation->GetController();
	Controller.InitializeModel();
	Controller.SetFrameRate(FFrameRate(30, 1), false);
	Controller.SetNumberOfFrames(30, false);
	Controller.AddBoneCurve(TEXT("root"), false);
	Controller.AddBoneCurve(TEXT("child"), false);
	TArray<FQuat4f> Rotations;
	TArray<FVector3f> Scales, RootPositions, ChildPositions;
	Rotations.Init(FQuat4f::Identity, 31);
	Scales.Init(FVector3f::OneVector, 31);
	RootPositions.Init(FVector3f(11, 4, 3), 31);
	ChildPositions.Init(FVector3f(2, 7, 9), 31);
	Controller.SetBoneTrackKeys(TEXT("root"), RootPositions, Rotations, Scales, false);
	Controller.SetBoneTrackKeys(TEXT("child"), ChildPositions, Rotations, Scales, false);
	const FName CurveName(TEXT("ProxyPassthroughTestCurve"));
	const FAnimationCurveIdentifier CurveId(CurveName, ERawCurveTrackTypes::RCT_Float);
	Controller.AddCurve(CurveId, AACF_DefaultCurve, false);
	Controller.SetCurveKeys(CurveId, {FRichCurveKey(0.f, 0.75f), FRichCurveKey(1.f, 0.75f)}, false);
	Controller.NotifyPopulated();
	TStrongObjectPtr<UAnimBlueprint> Blueprint(Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), Package, TEXT("ABP_AutomaticProxyTest"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("KawaiiProxyAutomation"))));
	if (!TestNotNull(TEXT("A transient animation blueprint can be created"), Blueprint.Get()))
	{
		return false;
	}
	Blueprint->TargetSkeleton = Skeleton.Get();
	Blueprint->SetPreviewMesh(Mesh.Get());
	for (UClass* Class : {Blueprint->GeneratedClass.Get(), Blueprint->SkeletonGeneratedClass.Get()})
	{
		if (UAnimBlueprintGeneratedClass* AnimClass = Cast<UAnimBlueprintGeneratedClass>(Class))
		{
			AnimClass->TargetSkeleton = Skeleton.Get();
		}
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	UAnimGraphNode_Root* Root = nullptr;
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : Graphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UAnimGraphNode_Root* Candidate = Cast<UAnimGraphNode_Root>(Node))
			{
				Root = Candidate;
				AnimGraph = Graph;
				break;
			}
		}
		if (Root) { break; }
	}
	if (!TestNotNull(TEXT("The animation graph has an output root"), Root)) { return false; }
	FGraphNodeCreator<UAnimGraphNode_SequenceEvaluator> RefCreator(*AnimGraph);
	UAnimGraphNode_SequenceEvaluator* RefPose = RefCreator.CreateNode();
	RefPose->Node.SetSequence(Animation.Get());
	RefPose->Node.SetExplicitTime(0.5f);
	RefPose->Node.SetTeleportToExplicitTime(true);
	RefCreator.Finalize();
	FGraphNodeCreator<UAnimGraphNode_KawaiiAutoClothingProxy> AutoCreator(*AnimGraph);
	UAnimGraphNode_KawaiiAutoClothingProxy* AutoNode = AutoCreator.CreateNode();
	AutoCreator.Finalize();
	if (UEdGraphPin* AlphaPin = AutoNode->FindPin(TEXT("Alpha"), EGPD_Input)) { AlphaPin->DefaultValue = TEXT("1.0"); }
	const auto PosePin = [](UEdGraphNode* Node, EEdGraphPinDirection Direction) -> UEdGraphPin*
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && Pin->PinType.PinSubCategoryObject == FPoseLink::StaticStruct()) { return Pin; }
		}
		return nullptr;
	};
	UEdGraphPin* RefOutput = PosePin(RefPose, EGPD_Output);
	UEdGraphPin* AutoInput = PosePin(AutoNode, EGPD_Input);
	UEdGraphPin* AutoOutput = PosePin(AutoNode, EGPD_Output);
	UEdGraphPin* RootInput = PosePin(Root, EGPD_Input);
	if (!TestTrue(TEXT("All local-pose pins exist"), RefOutput && AutoInput && AutoOutput && RootInput)) { return false; }
	TestTrue(TEXT("Animated source connects to the automatic node"), AnimGraph->GetSchema()->TryCreateConnection(RefOutput, AutoInput));
	TestTrue(TEXT("The automatic node participates in the output"), AnimGraph->GetSchema()->TryCreateConnection(AutoOutput, RootInput));
	// Simulate a saved node from the version whose input and output were both named Pose.
	// Reconstruction must migrate only the input and preserve both existing connections.
	AutoInput->PinName = TEXT("Pose");
	AutoNode->ReconstructNode();
	AutoInput = AutoNode->FindPin(TEXT("SourcePose"), EGPD_Input);
	AutoOutput = AutoNode->FindPin(TEXT("Pose"), EGPD_Output);
	if (!TestTrue(TEXT("Reconstruction migrates the legacy input while retaining the output name"), AutoInput && AutoOutput)) { return false; }
	TestTrue(TEXT("The legacy input connection survives reconstruction"), AutoInput->LinkedTo.Contains(RefOutput) && RefOutput->LinkedTo.Contains(AutoInput));
	TestTrue(TEXT("The downstream output connection survives reconstruction"), AutoOutput->LinkedTo.Contains(RootInput) && RootInput->LinkedTo.Contains(AutoOutput));
	TestNull(TEXT("No legacy Pose input remains to collide with the output"), AutoNode->FindPin(TEXT("Pose"), EGPD_Input));
	// Do not manually request the extension: the node must install it through normal compilation.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get());
	FKismetEditorUtilities::CompileBlueprint(Blueprint.Get());
	if (!TestTrue(TEXT("The blueprint compiles"), Blueprint->Status != BS_Error)) { return false; }
	IAnimClassInterface* Interface = IAnimClassInterface::GetFromClass(Blueprint->GeneratedClass);
	if (!TestNotNull(TEXT("The generated class implements the animation interface"), Interface)) { return false; }
	TestNotNull(TEXT("Compilation installs the montage class subsystem"), Interface->FindSubsystem<FAnimSubsystem_KawaiiClothingProxy>());
	FStructProperty* AutoProperty = nullptr;
	FStructProperty* SequenceProperty = nullptr;
	int32 SequenceIndex = INDEX_NONE;
	const auto& NodeProperties = Interface->GetAnimNodeProperties();
	for (int32 Index = 0; Index < NodeProperties.Num(); ++Index)
	{
		FStructProperty* Property = NodeProperties[Index];
		if (Property->Struct == FAnimNode_KawaiiAutoClothingProxy::StaticStruct()) { AutoProperty = Property; }
		if (Property->Struct == FAnimNode_SequenceEvaluator::StaticStruct()) { SequenceProperty = Property; SequenceIndex = Index; }
	}
	if (!TestTrue(TEXT("Both nodes survive normal compilation"), AutoProperty && SequenceProperty)) { return false; }
	const auto* DefaultNode = AutoProperty->ContainerPtrToValuePtr<FAnimNode_KawaiiAutoClothingProxy>(Blueprint->GeneratedClass->GetDefaultObject());
	// The direct C++ SetLinkNode tests cannot detect a missing compiler-generated link.
	TestEqual(TEXT("The generated CDO input points to the compiled animation evaluator"), DefaultNode->SourcePose.LinkID, SequenceIndex);
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	Component->SetSkeletalMesh(Mesh.Get());
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get(), Blueprint->GeneratedClass));
	auto* Data = Instance->FindSubsystem<FAnimSubsystemInstance_KawaiiClothingProxy>();
	if (!TestNotNull(TEXT("Every generated instance receives its own montage snapshot storage"), Data)) { return false; }
	TestFalse(TEXT("Snapshot storage starts uncaptured"), Data->MontageSnapshot.bCaptured);
	int32 MatchingSubsystems = 0;
	Interface->ForEachSubsystem(Instance.Get(), [&](const FAnimSubsystemInstanceContext& Context)
	{
		if (Context.SubsystemStruct == FAnimSubsystem_KawaiiClothingProxy::StaticStruct())
		{
			++MatchingSubsystems;
			TestTrue(TEXT("Class and instance subsystem types are paired"), Context.SubsystemInstanceStruct == FAnimSubsystemInstance_KawaiiClothingProxy::StaticStruct());
			FAnimSubsystemUpdateContext Update(Context, Instance.Get(), 1.f / 60.f);
			Context.Subsystem.OnPreUpdate_GameThread(Update);
		}
		return EAnimSubsystemEnumeration::Continue;
	});
	TestEqual(TEXT("Exactly one automatic montage subsystem is installed"), MatchingSubsystems, 1);
	TestTrue(TEXT("The game-thread hook publishes a valid empty montage snapshot"), Data->MontageSnapshot.bCaptured && Data->MontageSnapshot.Error.IsEmpty());
	FProxyCompiledGraphTestProxy Proxy(Instance.Get());
	TestTrue(TEXT("The runtime collector retrieves this instance's snapshot"), KawaiiClothingProxy::GetMontageSnapshot(Proxy) == &Data->MontageSnapshot);
	Proxy.Initialize(Instance.Get());
	const TArray<FBoneIndexType> BoneIndices{0, 1};
	const UE::Anim::FCurveFilterSettings CurveFilter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh.Get());
	Proxy.GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh.Get());
	if (!TestTrue(TEXT("The compiled graph has valid required bones"), Proxy.GetRequiredBones().IsValid())) { return false; }
	FMemMark Mark(FMemStack::Get());
	Proxy.InitializeRootNode();
	if (!TestNotNull(TEXT("Initialization resolves a compiled graph root"), Proxy.GetRootNode())) { return false; }
	Proxy.GetRootNode()->CacheBones_AnyThread(FAnimationCacheBonesContext(&Proxy));
	auto* RuntimeNode = AutoProperty->ContainerPtrToValuePtr<FAnimNode_KawaiiAutoClothingProxy>(Instance.Get());
	auto* RuntimeSequence = SequenceProperty->ContainerPtrToValuePtr<FAnimNode_SequenceEvaluator>(Instance.Get());
	TestEqual(TEXT("The generated instance retains the compiled input link"), RuntimeNode->SourcePose.LinkID, SequenceIndex);
	TestTrue(TEXT("Initialization resolves the input to the real animation node"), RuntimeNode->SourcePose.GetLinkNode() == RuntimeSequence);
	for (int32 Frame = 0; Frame < 2; ++Frame)
	{
		RuntimeNode->PreUpdate(Instance.Get());
		Proxy.UpdateAnimation();
		TestEqual(FString::Printf(TEXT("Frame %d exposed inputs preserve the compiled LinkID"), Frame), RuntimeNode->SourcePose.LinkID, SequenceIndex);
		TestTrue(FString::Printf(TEXT("Frame %d still points to the animation evaluator"), Frame), RuntimeNode->SourcePose.GetLinkNode() == RuntimeSequence);
		TestEqual(TEXT("The empty-list fallback is exercised with full correction alpha"), RuntimeNode->Alpha, 1.f);
		FPoseContext Output(&Proxy);
		Proxy.EvaluateAnimation(Output);
		if (!TestEqual(TEXT("Compiled evaluation emits both fixture bones"), Output.Pose.GetNumBones(), 2)) { return false; }
		TestTrue(FString::Printf(TEXT("Frame %d empty-list fallback preserves animated root translation"), Frame),
			Output.Pose[FCompactPoseBoneIndex(0)].GetTranslation().Equals(FVector(11, 4, 3), 1.e-3));
		TestTrue(FString::Printf(TEXT("Frame %d empty-list fallback preserves animated child translation"), Frame),
			Output.Pose[FCompactPoseBoneIndex(1)].GetTranslation().Equals(FVector(2, 7, 9), 1.e-3));
		TestTrue(FString::Printf(TEXT("Frame %d animation curves pass through the automatic node"), Frame), FMath::IsNearlyEqual(Output.Curve.Get(CurveName), 0.75f));
		TestEqual(TEXT("The complete source pose is retained on the explicit empty-list fallback"),
			RuntimeNode->GetProxyDebugReason(), FString(TEXT("no_clothing_proxy_assets")));
	}
	return true;
}

#endif
