#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_Slot.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "EdGraph/EdGraph.h"
#include "IDetailsView.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiMontageSlotPicker.h"
#include "AnimGraphNode_KawaiiAutoClothingProxy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace KawaiiMontageSlotPickerTests {
	USkeleton* MakeSkeleton(UObject* Outer) {
		USkeleton* Skeleton = NewObject<USkeleton>(Outer);
		{
			FReferenceSkeletonModifier Modifier(Skeleton);
			Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
		}
		Skeleton->AddSlotGroupName(TEXT("WholeBodyGroup"));
		for (const TCHAR* Slot : {TEXT("WholeBodyAttack"), TEXT("WholeBodyTraversal")}) {
			Skeleton->RegisterSlotNode(Slot);
			Skeleton->SetSlotGroupName(Slot, TEXT("WholeBodyGroup"));
		}
		return Skeleton;
	}

	const KawaiiMontageSlotPicker::FOption* Find(const TArray<KawaiiMontageSlotPicker::FOption>& Options, const TCHAR* Slot) {
		return Options.FindByPredicate([Slot](const KawaiiMontageSlotPicker::FOption& Option) { return Option.Slot == FName(Slot); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiMontageSlotOptionsTest,
	"KawaiiPhysicsBaker.ClothingProxy.AutoNode.MontageSlotOptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiMontageSlotOptionsTest::RunTest(const FString&) {
	using namespace KawaiiMontageSlotPicker;
	using KawaiiMontageSlotPickerTests::Find;
	TStrongObjectPtr<USkeleton> Skeleton(KawaiiMontageSlotPickerTests::MakeSkeleton(GetTransientPackage()));
	TArray<FOption> Options;
	BuildOptions(Skeleton.Get(), {TEXT("WholeBodyTraversal")}, TEXT("WholeBodyTraversal"), true, Options);
	if (!TestTrue(TEXT("None is always the first choice"), !Options.IsEmpty() && Options[0].Slot.IsNone())) {
		return false;
	}
	const FOption* Traversal = Find(Options, TEXT("WholeBodyTraversal"));
	const FOption* Attack = Find(Options, TEXT("WholeBodyAttack"));
	if (!TestTrue(TEXT("Every skeleton slot is offered"), Traversal && Attack)) {
		return false;
	}
	TestTrue(TEXT("Slots use the engine Group.Slot label"), Attack->Label.ToString() == TEXT("WholeBodyGroup.WholeBodyAttack"));
	TestTrue(
		TEXT("Slots used by Slot nodes are marked"),
		Traversal->Label.ToString().StartsWith(TEXT("WholeBodyGroup.WholeBodyTraversal")) && Traversal->Label.ToString().Contains(TEXT("本蓝图已使用"))
	);
	TestEqual(
		TEXT("A registered current value is not duplicated"),
		Options.FilterByPredicate([](const FOption& Option) { return Option.Slot == TEXT("WholeBodyTraversal"); }).Num(),
		1
	);

	BuildOptions(Skeleton.Get(), {}, TEXT("RemovedSlot"), true, Options);
	TestTrue(
		TEXT("An unregistered saved value stays selectable and is marked"),
		Options.Last().Slot == TEXT("RemovedSlot") && Options.Last().Label.ToString().Contains(TEXT("未注册"))
	);
	TestFalse(TEXT("Building options never registers slots on the skeleton"), Skeleton->ContainsSlotName(TEXT("RemovedSlot")));
	BuildOptions(Skeleton.Get(), {}, TEXT("RemovedSlot"), false, Options);
	TestNull(TEXT("Multiple selected values do not add a stale entry"), Find(Options, TEXT("RemovedSlot")));
	BuildOptions(nullptr, {}, NAME_None, true, Options);
	TestEqual(TEXT("Without a skeleton only None is offered"), Options.Num(), 1);

	TestTrue(
		TEXT("Current values are displayed with their group"),
		DescribeSlot(Skeleton.Get(), TEXT("WholeBodyAttack")).ToString() == TEXT("WholeBodyGroup.WholeBodyAttack")
	);
	TestTrue(TEXT("None is described as disabling montage tracking"), DescribeSlot(Skeleton.Get(), NAME_None).ToString().StartsWith(TEXT("None")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiMontageSlotDetailsTest,
	"KawaiiPhysicsBaker.ClothingProxy.AutoNode.MontageSlotDetails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiMontageSlotDetailsTest::RunTest(const FString&) {
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/KawaiiMontageSlotDetails_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetFlags(RF_Transient);
	TStrongObjectPtr<USkeleton> Skeleton(KawaiiMontageSlotPickerTests::MakeSkeleton(Package));
	TStrongObjectPtr<UAnimBlueprint> Blueprint(Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UAnimInstance::StaticClass(),
		Package,
		TEXT("ABP_MontageSlotDetails"),
		BPTYPE_Normal,
		UAnimBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("KawaiiMontageSlotAutomation")
	)));
	if (!TestNotNull(TEXT("A transient animation blueprint can be created"), Blueprint.Get())) {
		return false;
	}
	Blueprint->TargetSkeleton = Skeleton.Get();
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	UEdGraph* const* AnimGraph = Graphs.FindByPredicate([](const UEdGraph* Graph) {
		return Graph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node) { return Node && Node->IsA<UAnimGraphNode_Root>(); });
	});
	if (!TestNotNull(TEXT("The animation graph exists"), AnimGraph)) {
		return false;
	}
	FGraphNodeCreator<UAnimGraphNode_Slot> SlotCreator(**AnimGraph);
	UAnimGraphNode_Slot* SlotNode = SlotCreator.CreateNode();
	SlotCreator.Finalize();
	SlotNode->Node.SlotName = TEXT("WholeBodyTraversal");
	FGraphNodeCreator<UAnimGraphNode_KawaiiAutoClothingProxy> ProxyCreator(**AnimGraph);
	UAnimGraphNode_KawaiiAutoClothingProxy* ProxyNode = ProxyCreator.CreateNode();
	ProxyCreator.Finalize();
	TestTrue(TEXT("Slot nodes in the blueprint are discovered"), KawaiiMontageSlotPicker::GetUsedSlots(Blueprint.Get()).Contains(TEXT("WholeBodyTraversal")));

	// Runs FAnimGraphNodeDetails and the node customization exactly as the Animation Blueprint editor does.
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs Args;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Args.bHideSelectionTip = true;
	const TSharedRef<IDetailsView> View = PropertyEditor.CreateDetailView(Args);
	View->SetObject(ProxyNode, true);
	TestTrue(TEXT("The customized node details can be built"), View->GetSelectedObjects().Num() == 1);
	ProxyNode->Node.MontageSlot = TEXT("WholeBodyTraversal");
	View->ForceRefresh();
	TestTrue(TEXT("Refreshing with a selected slot keeps the node customization valid"), View->GetSelectedObjects().Num() == 1);
	View->SetObject(nullptr);
	return true;
}

#endif
