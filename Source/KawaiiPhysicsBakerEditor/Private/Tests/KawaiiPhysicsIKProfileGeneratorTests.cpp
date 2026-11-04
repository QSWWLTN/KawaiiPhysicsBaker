#include "KawaiiPhysicsIKProfileGenerator.h"

#include "AnimGraphNode_KawaiiAutoClothingProxy.h"
#include "AnimGraphNode_KawaiiPhysics.h"
#include "AnimGraphNode_Root.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "EdGraph/EdGraph.h"
#include "Engine/SkeletalMesh.h"
#include "KawaiiClothingProxyAsset.h"
#include "KawaiiClothingProxyMotion.h"
#include "KawaiiClothingProxyPlayback.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/MemStack.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace KawaiiIKProfileGeneratorTests
{
	bool IsInSubtree(int32 Bone, int32 Root, const TArray<int32>& Parents)
	{
		for (int32 Index = Bone; Index != INDEX_NONE; Index = Parents[Index]) {
			if (Index == Root) {
				return true;
			}
		}
		return false;
	}

	/** The same structural rules FAnimNode_KawaiiPhysicsIKCorrection::CorrectPose enforces before solving. */
	bool SatisfiesRuntimeRules(const TArray<FName>& Names, const TArray<int32>& Parents, const TArray<FKawaiiPhysicsIKCorrectionChain>& Chains)
	{
		TArray<int32> RootIndices;
		for (const FKawaiiPhysicsIKCorrectionChain& Chain : Chains) {
			const int32 Root = Names.IndexOfByKey(Chain.RootBone.BoneName);
			const int32 Tip = Names.IndexOfByKey(Chain.TipBone.BoneName);
			if (Root == INDEX_NONE || Tip == INDEX_NONE || Root == Tip || Parents[Root] == INDEX_NONE || !IsInSubtree(Tip, Root, Parents)) {
				return false;
			}
			for (const int32 Existing : RootIndices) {
				if (IsInSubtree(Root, Existing, Parents) || IsInSubtree(Existing, Root, Parents)) {
					return false;
				}
			}
			RootIndices.Add(Root);
		}
		for (const FKawaiiPhysicsIKCorrectionChain& Chain : Chains) {
			const int32 Driver = Names.IndexOfByKey(Chain.DriverBone.BoneName);
			if (Driver == INDEX_NONE) {
				return false;
			}
			for (const int32 Root : RootIndices) {
				if (IsInSubtree(Driver, Root, Parents)) {
					return false;
				}
			}
		}
		return true;
	}

	bool HasChain(const TArray<FKawaiiPhysicsIKCorrectionChain>& Chains, const TCHAR* Root, const TCHAR* Tip, const TCHAR* Driver, float Radius)
	{
		return Chains.ContainsByPredicate([&](const FKawaiiPhysicsIKCorrectionChain& Chain) {
			return Chain.RootBone.BoneName == FName(Root) && Chain.TipBone.BoneName == FName(Tip) && Chain.DriverBone.BoneName == FName(Driver)
				&& Chain.DriverWeight == 0.f && FMath::IsNearlyEqual(Chain.CollisionRadius, Radius);
		});
	}

	bool NotesContain(const TArray<FString>& Notes, const TCHAR* Text)
	{
		return Notes.ContainsByPredicate([Text](const FString& Note) { return Note.Contains(Text); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKProfileChainDecompositionTest, "KawaiiPhysicsBaker.IKProfileGenerator.ChainDecomposition",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiIKProfileChainDecompositionTest::RunTest(const FString&)
{
	using namespace KawaiiPhysicsIKProfileGenerator;
	using namespace KawaiiIKProfileGeneratorTests;
	const TArray<FName> Names{
		TEXT("root"), TEXT("Hips"), TEXT("Spine"), TEXT("Head"), TEXT("Hair_Root"),
		TEXT("Hair_A1"), TEXT("Hair_A2"), TEXT("Hair_Clip"), TEXT("Hair_B1"), TEXT("Hair_B2"),
		TEXT("Hair_front"), TEXT("Hair_front_2"), TEXT("Hair_front_3"),
		TEXT("Tail_Root"), TEXT("Tail_001"), TEXT("Tail_002"), TEXT("Ear_001")};
	const TArray<int32> Parents{INDEX_NONE, 0, 1, 2, 3, 4, 5, 5, 4, 8, 4, 10, 11, 1, 13, 14, 3};

	TArray<FSimulatedRoot> Roots;
	Roots.Add({TEXT("Hair_Root"), {TEXT("Hair_front"), TEXT("Hair_Clip")}, 2.f});
	Roots.Add({TEXT("Hair_front_2"), {}, 1.5f});
	Roots.Add({TEXT("Tail_001"), {}, 2.5f});
	Roots.Add({TEXT("Ear_001"), {}, 1.f});
	Roots.Add({TEXT("Missing_Root"), {}, 1.f});

	TArray<FKawaiiPhysicsIKCorrectionChain> Chains;
	TArray<FString> Notes;
	FString Error;
	if (!TestTrue(TEXT("A valid hierarchy decomposes"), BuildChains(Names, Parents, Roots, {TEXT("Tail_Root"), TEXT("Head"), NAME_None}, Chains, Notes, Error))) {
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Each branch below the shared hair root and the tail become separate chains"), Chains.Num(), 4);
	TestTrue(TEXT("A branch keeps its unsimulated side bone rigid and still forms a chain"), HasChain(Chains, TEXT("Hair_A1"), TEXT("Hair_A2"), TEXT("Hair_Root"), 2.f));
	TestTrue(TEXT("The sibling branch is corrected independently"), HasChain(Chains, TEXT("Hair_B1"), TEXT("Hair_B2"), TEXT("Hair_Root"), 2.f));
	TestTrue(TEXT("A nested KawaiiPhysics root below an excluded bone uses that bone as its anchor"),
			 HasChain(Chains, TEXT("Hair_front_2"), TEXT("Hair_front_3"), TEXT("Hair_front"), 1.5f));
	TestTrue(TEXT("The tail chain is lifted to the garment's ignored root so runtime filtering matches"),
			 HasChain(Chains, TEXT("Tail_Root"), TEXT("Tail_002"), TEXT("Hips"), 2.5f));
	TestFalse(TEXT("The branching hair root itself is not a chain root"),
			  Chains.ContainsByPredicate([](const FKawaiiPhysicsIKCorrectionChain& Chain) { return Chain.RootBone.BoneName == TEXT("Hair_Root"); }));
	TestTrue(TEXT("Generated chains satisfy every runtime structural rule"), SatisfiesRuntimeRules(Names, Parents, Chains));
	TestTrue(TEXT("Single-bone strands are reported"), NotesContain(Notes, TEXT("Ear_001")));
	TestTrue(TEXT("Unknown KawaiiPhysics roots are reported"), NotesContain(Notes, TEXT("Missing_Root")));
	TestTrue(TEXT("An ignore rule covering several chains is reported as ineffective"), NotesContain(Notes, TEXT("忽略规则 Head")));
	TestTrue(TEXT("Lifting is reported"), NotesContain(Notes, TEXT("Tail_Root")));

	TestTrue(TEXT("Without ignore rules the tail keeps its KawaiiPhysics root"),
			 BuildChains(Names, Parents, Roots, {}, Chains, Notes, Error) && HasChain(Chains, TEXT("Tail_001"), TEXT("Tail_002"), TEXT("Tail_Root"), 2.5f));
	TestTrue(TEXT("No roots produce no chains without failing"), BuildChains(Names, Parents, {}, {}, Chains, Notes, Error) && Chains.IsEmpty());
	TestFalse(TEXT("Children must follow their parents"), BuildChains({TEXT("a"), TEXT("b")}, {1, INDEX_NONE}, Roots, {}, Chains, Notes, Error));
	TestFalse(TEXT("Mismatched parent arrays are rejected"), BuildChains(Names, {INDEX_NONE}, Roots, {}, Chains, Notes, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKProfileCapsuleConversionTest, "KawaiiPhysicsBaker.IKProfileGenerator.CapsuleConversion",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiIKProfileCapsuleConversionTest::RunTest(const FString&)
{
	using namespace KawaiiPhysicsIKProfileGenerator;
	const FKawaiiPhysicsIKCorrectionCapsule Upright = MakeBoneCapsule(TEXT("Head"), FVector(1, 2, 3), FRotator::ZeroRotator, 4.f, 10.f);
	TestTrue(TEXT("Both endpoints stay in the driving bone's space"), Upright.StartBone.BoneName == TEXT("Head") && Upright.EndBone.BoneName == TEXT("Head"));
	TestTrue(TEXT("The KawaiiPhysics capsule axis is local Z, centred on the offset"),
			 Upright.StartOffset.Equals(FVector(1, 2, -2)) && Upright.EndOffset.Equals(FVector(1, 2, 8)) && Upright.Radius == 4.f);
	const FRotator Rotation(30.f, 45.f, 60.f);
	const FKawaiiPhysicsIKCorrectionCapsule Rotated = MakeBoneCapsule(TEXT("Chest"), FVector(5, 0, 0), Rotation, 3.f, 12.f);
	TestTrue(TEXT("Rotated capsules keep their centre and length"),
			 ((Rotated.StartOffset + Rotated.EndOffset) * .5).Equals(FVector(5, 0, 0), 1.e-4)
				 && FMath::IsNearlyEqual(FVector::Distance(Rotated.StartOffset, Rotated.EndOffset), 12.0, 1.e-4));
	TestTrue(TEXT("Rotated capsules follow the offset rotation's Z axis"),
			 (Rotated.EndOffset - Rotated.StartOffset).GetSafeNormal().Equals(Rotation.Quaternion().GetAxisZ(), 1.e-4));
	const FKawaiiPhysicsIKCorrectionCapsule Sphere = MakeBoneCapsule(TEXT("Head"), FVector(0, 0, 7), Rotation, 6.f, 0.f);
	TestTrue(TEXT("A sphere limit becomes a capsule with coincident endpoints"), Sphere.StartOffset.Equals(Sphere.EndOffset) && Sphere.StartOffset.Equals(FVector(0, 0, 7)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiIKProfileAnimBlueprintTest, "KawaiiPhysicsBaker.IKProfileGenerator.AnimBlueprintRoundTrip",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiIKProfileAnimBlueprintTest::RunTest(const FString&)
{
	using namespace KawaiiPhysicsIKProfileGenerator;
	const FString Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/KawaiiIKProfileGenSource_%s"), *Unique));
	Package->SetFlags(RF_Transient);
	TStrongObjectPtr<USkeleton> Skeleton(NewObject<USkeleton>(Package));
	{
		FReferenceSkeletonModifier Modifier(Skeleton.Get());
		const auto Add = [&](const TCHAR* Name, int32 Parent, const FVector& Location) {
			Modifier.Add(FMeshBoneInfo(FName(Name), Name, Parent), FTransform(Location));
		};
		Add(TEXT("root"), INDEX_NONE, FVector::ZeroVector);
		Add(TEXT("Spine"), 0, FVector(0, 0, 100));
		Add(TEXT("Head"), 1, FVector(0, 0, 50));
		Add(TEXT("Hair_Root"), 2, FVector(0, 0, 10));
		Add(TEXT("Hair_A1"), 3, FVector(0, 5, 0));
		Add(TEXT("Hair_A2"), 4, FVector(0, 0, -10));
		Add(TEXT("Hair_B1"), 3, FVector(0, -5, 0));
		Add(TEXT("Hair_B2"), 6, FVector(0, 0, -10));
		Add(TEXT("Tail_Root"), 0, FVector(0, -10, 90));
		Add(TEXT("Tail_001"), 8, FVector(0, -5, 0));
		Add(TEXT("Tail_002"), 9, FVector(0, -10, 0));
	}
	TStrongObjectPtr<UAnimBlueprint> Blueprint(Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(), Package, TEXT("ABP_IKProfileGenTest"), BPTYPE_Normal,
		UAnimBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("KawaiiIKProfileAutomation"))));
	if (!TestNotNull(TEXT("A transient animation blueprint can be created"), Blueprint.Get())) {
		return false;
	}
	Blueprint->TargetSkeleton = Skeleton.Get();
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : Graphs) {
		if (Graph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Node && Node->IsA<UAnimGraphNode_Root>(); })) {
			AnimGraph = Graph;
			break;
		}
	}
	if (!TestNotNull(TEXT("The animation graph exists"), AnimGraph)) {
		return false;
	}

	const auto AddPhysicsNode = [&](const TCHAR* Root, float Radius) {
		FGraphNodeCreator<UAnimGraphNode_KawaiiPhysics> Creator(*AnimGraph);
		UAnimGraphNode_KawaiiPhysics* Node = Creator.CreateNode();
		Creator.Finalize();
		Node->Node.RootBone.BoneName = Root;
		Node->Node.PhysicsSettings.Radius = Radius;
		return Node;
	};
	UAnimGraphNode_KawaiiPhysics* Hair = AddPhysicsNode(TEXT("Hair_Root"), 2.f);
	FSphericalLimit HeadSphere;
	HeadSphere.DrivingBone.BoneName = TEXT("Head");
	HeadSphere.OffsetLocation = FVector(0, 0, 5);
	HeadSphere.Radius = 8.f;
	FSphericalLimit InnerSphere = HeadSphere;
	InnerSphere.LimitType = ESphericalLimitType::Inner;
	FCapsuleLimit HairBound;
	HairBound.DrivingBone.BoneName = TEXT("Hair_A1");
	HairBound.Radius = 1.f;
	Hair->Node.SphericalLimits = {HeadSphere, HeadSphere, InnerSphere};
	Hair->Node.CapsuleLimits = {HairBound};
	AddPhysicsNode(TEXT("Tail_001"), 3.f);
	UAnimGraphNode_KawaiiPhysics* Disabled = AddPhysicsNode(TEXT("Spine"), 9.f);
	Disabled->SetEnabledState(ENodeEnabledState::Disabled, false);

	TStrongObjectPtr<UKawaiiClothingProxyAsset> Definition(NewObject<UKawaiiClothingProxyAsset>(Package));
	Definition->IgnoredTargetRootBones = {TEXT("Tail_Root")};
	TStrongObjectPtr<UKawaiiClothingProxyMotion> Motion(NewObject<UKawaiiClothingProxyMotion>(Package));
	Motion->Definition = Definition.Get();
	TStrongObjectPtr<UKawaiiClothingProxyMotionSet> Set(NewObject<UKawaiiClothingProxyMotionSet>(Package));
	Set->Motions = {Motion.Get()};
	FGraphNodeCreator<UAnimGraphNode_KawaiiAutoClothingProxy> ProxyCreator(*AnimGraph);
	UAnimGraphNode_KawaiiAutoClothingProxy* ProxyNode = ProxyCreator.CreateNode();
	ProxyCreator.Finalize();
	UEdGraphPin* SetPin = ProxyNode->FindPin(GET_MEMBER_NAME_CHECKED(FAnimNode_KawaiiAutoClothingProxy, ProxyMotionSet), EGPD_Input);
	if (!TestNotNull(TEXT("The motion set is a visible pin"), SetPin)) {
		return false;
	}
	SetPin->DefaultObject = Set.Get();

	FOptions Options;
	Options.bSave = false;
	Options.PackagePath = FString::Printf(TEXT("/Temp/KawaiiIKProfileGenOutput_%s"), *Unique);
	const FString ProfilePackage = Options.PackagePath / TEXT("DA_ABP_IKProfileGenTest_IKCorrection");
	FString Report;
	Options.bDryRun = true;
	TestNull(TEXT("A dry run returns no asset"), Generate(Blueprint.Get(), Options, Report));
	TestNull(TEXT("A dry run creates no package"), FindPackage(nullptr, *ProfilePackage));
	TestTrue(TEXT("A dry run still reports the inferred chains"), Report.Contains(TEXT("Hair_A1 → Hair_A2")));
	TestNull(TEXT("A dry run leaves the node unassigned"), ProxyNode->Node.Profile.Get());

	Options.bDryRun = false;
	UKawaiiPhysicsIKCorrectionProfile* Profile = Generate(Blueprint.Get(), Options, Report);
	if (!TestNotNull(TEXT("The profile is generated"), Profile)) {
		AddError(Report);
		return false;
	}
	TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile> ProfileGuard(Profile);
	TestEqual(TEXT("The profile is created beside the requested path"), Profile->GetOutermost()->GetName(), ProfilePackage);
	TestEqual(TEXT("Both hair branches and the tail become chains; disabled nodes are ignored"), Profile->Chains.Num(), 3);
	TestTrue(TEXT("The tail follows the garment ignore rule"),
			 KawaiiIKProfileGeneratorTests::HasChain(Profile->Chains, TEXT("Tail_Root"), TEXT("Tail_002"), TEXT("root"), 3.f));
	TestTrue(TEXT("Hair keeps the node radius"),
			 KawaiiIKProfileGeneratorTests::HasChain(Profile->Chains, TEXT("Hair_B1"), TEXT("Hair_B2"), TEXT("Hair_Root"), 2.f));
	if (TestEqual(TEXT("Only the unique outer sphere outside corrected chains is imported"), Profile->Capsules.Num(), 1)) {
		TestTrue(TEXT("The sphere keeps its bone, offset and radius"),
				 Profile->Capsules[0].StartBone.BoneName == TEXT("Head") && Profile->Capsules[0].StartOffset.Equals(FVector(0, 0, 5))
					 && Profile->Capsules[0].EndOffset.Equals(FVector(0, 0, 5)) && Profile->Capsules[0].Radius == 8.f);
	}
	TestTrue(TEXT("The empty proxy profile is assigned"), ProxyNode->Node.Profile.Get() == Profile);
	TestTrue(TEXT("The blueprint still compiles"), Blueprint->Status != BS_Error);

	Profile->Iterations = 24;
	TestTrue(TEXT("Regeneration updates the same asset in place"), Generate(Blueprint.Get(), Options, Report) == Profile);
	TestEqual(TEXT("Regeneration keeps solver tuning"), Profile->Iterations, 24);
	TestTrue(TEXT("Regeneration reports the existing assignment"), Report.Contains(TEXT("已在使用该配置")));

	UKawaiiPhysicsIKCorrectionProfile* Other = NewObject<UKawaiiPhysicsIKCorrectionProfile>(Package);
	TStrongObjectPtr<UKawaiiPhysicsIKCorrectionProfile> OtherGuard(Other);
	ProxyNode->Node.Profile = Other;
	Generate(Blueprint.Get(), Options, Report);
	TestTrue(TEXT("A manually chosen profile is never replaced"), ProxyNode->Node.Profile.Get() == Other);
	ProxyNode->Node.Profile = Profile;

	// The generated asset must pass every validation step of the runtime solver.
	TStrongObjectPtr<USkeletalMesh> Mesh(NewObject<USkeletalMesh>(Package));
	Mesh->SetSkeleton(Skeleton.Get());
	Mesh->GetRefSkeleton() = Skeleton->GetReferenceSkeleton();
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	Component->SetSkeletalMesh(Mesh.Get());
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get()));
	TArray<FBoneIndexType> BoneIndices;
	for (int32 Bone = 0; Bone < Mesh->GetRefSkeleton().GetNum(); ++Bone) {
		BoneIndices.Add(static_cast<FBoneIndexType>(Bone));
	}
	const UE::Anim::FCurveFilterSettings CurveFilter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh.Get());
	FAnimInstanceProxy Proxy(Instance.Get());
	Proxy.GetRequiredBones().InitializeTo(BoneIndices, CurveFilter, *Mesh.Get());
	FMemMark Mark(FMemStack::Get());
	FAnimNode_KawaiiPhysicsIKCorrection Correction;
	Correction.Profile = Profile;
	Correction.PreUpdate(Instance.Get());
	FPoseContext Output(&Proxy);
	Output.Pose.ResetToRefPose();
	FPoseContext Baseline(Output);
	Baseline.Pose.CopyBonesFrom(Output.Pose);
	Correction.CorrectPose(Output, Baseline);
	TestTrue(FString::Printf(TEXT("The runtime solver accepts the generated profile (%s)"), *Correction.GetCorrectionDebugReason()),
			 Correction.GetCorrectionDebugReason().StartsWith(TEXT("corrected")));
	return true;
}

#endif
