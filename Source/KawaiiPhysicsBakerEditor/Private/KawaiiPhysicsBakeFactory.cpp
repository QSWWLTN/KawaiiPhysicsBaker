#include "KawaiiPhysicsBakeFactory.h"

#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/Skeleton.h"
#include "AssetTypeCategories.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "PropertyCustomizationHelpers.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeAnimInstance.h"
#include "AnimGraphNode_KawaiiPhysicsBakeSource.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakeFactory"

UKawaiiPhysicsBakeFactory::UKawaiiPhysicsBakeFactory() {
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UAnimBlueprint::StaticClass();
}

FText UKawaiiPhysicsBakeFactory::GetDisplayName() const {
	return LOCTEXT("Name", "Kawaii Physics Bake Blueprint");
}

uint32 UKawaiiPhysicsBakeFactory::GetMenuCategories() const {
	return EAssetTypeCategories::Animation;
}

FString UKawaiiPhysicsBakeFactory::GetDefaultNewAssetName() const {
	return TEXT("ABP_KawaiiBake");
}

bool UKawaiiPhysicsBakeFactory::ConfigureProperties() {
	bool bAccepted = false;
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("CreateTitle", "Create Kawaii Physics Bake Blueprint"))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);
	const TWeakPtr<SWindow> WeakWindow = Window;
	Window->SetContent(
		SNew(SBox).WidthOverride(520).Padding(16)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT(
					"MeshHelp",
					"Choose the skeletal mesh for this graph. Create a separate bake blueprint for each clothing skeleton, then add it as a Part in the main bake blueprint."
				))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(USkeletalMesh::StaticClass())
				.ObjectPath_Lambda([this] { return SelectedMesh ? SelectedMesh->GetPathName() : FString(); })
				.OnObjectChanged_Lambda([this](const FAssetData& Data) { SelectedMesh = Cast<USkeletalMesh>(Data.GetAsset()); })
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 16, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Create", "Create"))
					.IsEnabled_Lambda([this] { return SelectedMesh && SelectedMesh->GetSkeleton(); })
					.OnClicked_Lambda([&bAccepted, WeakWindow] {
						bAccepted = true;
						if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) {
							Pinned->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Cancel", "Cancel"))
					.OnClicked_Lambda([WeakWindow] {
						if (const TSharedPtr<SWindow> Pinned = WeakWindow.Pin()) {
							Pinned->RequestDestroyWindow();
						}
						return FReply::Handled();
					})
				]
			]
		]
	);
	GEditor->EditorAddModalWindow(Window);
	return bAccepted;
}

UObject* UKawaiiPhysicsBakeFactory::FactoryCreateNew(UClass*, UObject* Parent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*) {
	if (!SelectedMesh || !SelectedMesh->GetSkeleton()) {
		return nullptr;
	}
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
		UKawaiiPhysicsBakeAnimInstance::StaticClass(),
		Parent,
		Name,
		BPTYPE_Normal,
		UAnimBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("KawaiiPhysicsBaker")
	));
	if (!Blueprint) {
		return nullptr;
	}
	Blueprint->SetFlags(Flags);
	Blueprint->TargetSkeleton = SelectedMesh->GetSkeleton();
	Blueprint->SetPreviewMesh(SelectedMesh);
	KawaiiPhysicsBake::FindOrAddSettings(Blueprint)->MainMesh = SelectedMesh;
	if (UAnimBlueprintGeneratedClass* Generated = Cast<UAnimBlueprintGeneratedClass>(Blueprint->GeneratedClass)) {
		Generated->TargetSkeleton = Blueprint->TargetSkeleton;
	}
	if (UAnimBlueprintGeneratedClass* SkeletonClass = Cast<UAnimBlueprintGeneratedClass>(Blueprint->SkeletonGeneratedClass)) {
		SkeletonClass->TargetSkeleton = Blueprint->TargetSkeleton;
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs) {
		UAnimGraphNode_Root* Root = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes) {
			Root = Cast<UAnimGraphNode_Root>(Node);
			if (Root) {
				break;
			}
		}
		if (!Root) {
			continue;
		}
		FGraphNodeCreator<UAnimGraphNode_KawaiiPhysicsBakeSource> Creator(*Graph);
		UAnimGraphNode_KawaiiPhysicsBakeSource* Source = Creator.CreateNode();
		Source->NodePosX = Root->NodePosX - 360;
		Source->NodePosY = Root->NodePosY;
		Creator.Finalize();
		UEdGraphPin* Output = nullptr;
		UEdGraphPin* Input = nullptr;
		for (UEdGraphPin* Pin : Source->Pins) {
			if (Pin->Direction == EGPD_Output) {
				Output = Pin;
				break;
			}
		}
		for (UEdGraphPin* Pin : Root->Pins) {
			if (Pin->Direction == EGPD_Input) {
				Input = Pin;
				break;
			}
		}
		if (Output && Input) {
			Graph->GetSchema()->TryCreateConnection(Output, Input);
		}
		break;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	return Blueprint;
}

#undef LOCTEXT_NAMESPACE
