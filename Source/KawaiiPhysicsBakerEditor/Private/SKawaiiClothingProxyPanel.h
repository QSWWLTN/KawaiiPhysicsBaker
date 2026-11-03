#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"

#include "KawaiiClothingProxyAsset.h"

class IDetailsView;
class FKawaiiClothingProxyPreview;
class SKawaiiClothingProxyViewport;
template <typename OptionType> class SComboBox;
struct FAssetData;
struct FPropertyChangedEvent;

/** Edits the real definition asset, with transactions and a reference-pose capsule preview. */
class SKawaiiClothingProxyPanel : public SCompoundWidget, public FEditorUndoClient
{
public:
	SLATE_BEGIN_ARGS(SKawaiiClothingProxyPanel)
	{}
	SLATE_END_ARGS()
	void Construct(const FArguments&);
	virtual ~SKawaiiClothingProxyPanel();
	virtual void Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime) override;
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override { PostUndo(bSuccess); }

private:
	void SelectDefinition(const FAssetData& Asset);
	void SelectMesh(const FAssetData& Asset);
	void SetDefinition(UKawaiiClothingProxyAsset* Asset);
	void RefreshMesh();
	void PropertiesChanged(const FPropertyChangedEvent& Event);
	bool ValidateDefinition(FString& Error) const;
	FName MakeShapeId(const FString& Prefix) const;
	FReply CreateDefinition();
	FReply SaveDefinition();
	FReply AddShape();
	FReply ImportPhysicsAsset();
	FReply BrowseDefinition();

	TStrongObjectPtr<UKawaiiClothingProxyAsset> Definition;
	TStrongObjectPtr<USkeletalMesh> SelectedMesh;
	FSoftObjectPath LastPreviewMeshPath;
	TSharedPtr<IDetailsView> Details;
	TSharedPtr<FKawaiiClothingProxyPreview> PreviewScene;
	TSharedPtr<SKawaiiClothingProxyViewport> Viewport;
	TSharedPtr<SComboBox<TSharedPtr<FName>>> BonePicker;
	TArray<TSharedPtr<FName>> BoneOptions;
	TSharedPtr<FName> SelectedBone;
	FString Message;
};
