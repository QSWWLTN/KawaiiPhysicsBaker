#include "SKawaiiClothingProxyPanel.h"

#include "AdvancedPreviewScene.h"
#include "AssetToolsModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "IContentBrowserSingleton.h"
#include "IDetailsView.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorModule.h"
#include "SEditorViewport.h"
#include "SceneManagement.h"
#include "ScopedTransaction.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "KawaiiClothingProxyAsset.h"
#include "KawaiiClothingProxyFactory.h"

#define LOCTEXT_NAMESPACE "KawaiiClothingProxyPanel"

class FKawaiiClothingProxyPreview : public FGCObject
{
public:
	FKawaiiClothingProxyPreview() : Scene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false).ShouldSimulatePhysics(false)) {
		Scene.SetFloorVisibility(false, true);
		Component = NewObject<USkeletalMeshComponent>();
		Component->SetDisablePostProcessBlueprint(true);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		Component->bForceRefpose = true;
		Scene.AddComponent(Component, FTransform::Identity);
	}

	virtual ~FKawaiiClothingProxyPreview() {
		Scene.RemoveComponent(Component);
		Component->DestroyComponent();
	}

	virtual void AddReferencedObjects(FReferenceCollector& Collector) override { Collector.AddReferencedObject(Component); }
	virtual FString GetReferencerName() const override { return TEXT("KawaiiClothingProxyPreview"); }

	void SetMesh(USkeletalMesh* Mesh) {
		Component->SetSkeletalMesh(Mesh);
		Component->RefreshBoneTransforms();
		Component->UpdateBounds();
		ReferencePose.Reset();
		if (Mesh) {
			const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
			ReferencePose = Ref.GetRefBonePose();
			for (int32 Bone = 0; Bone < ReferencePose.Num(); ++Bone) {
				const int32 Parent = Ref.GetParentIndex(Bone);
				if (Parent != INDEX_NONE) {
					ReferencePose[Bone] = ReferencePose[Bone] * ReferencePose[Parent];
				}
			}
		}
	}

	void Draw(FPrimitiveDrawInterface* PDI) const {
		const UKawaiiClothingProxyAsset* Asset = Definition.Get();
		const USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
		if (!Asset || !Mesh) {
			return;
		}
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		for (int32 Index = 0; Index < Asset->Shapes.Num(); ++Index) {
			const FKawaiiClothingProxyShape& Shape = Asset->Shapes[Index];
			const int32 Start = Ref.FindBoneIndex(Shape.StartBone);
			const int32 End = Ref.FindBoneIndex(Shape.EndBone.IsNone() ? Shape.StartBone : Shape.EndBone);
			if (!ReferencePose.IsValidIndex(Start) || !ReferencePose.IsValidIndex(End) || Shape.StartOffset.ContainsNaN() || Shape.EndOffset.ContainsNaN()
				|| !FMath::IsFinite(Shape.Radius) || Shape.Radius <= 0.f)
			{
				continue;
			}
			const FVector A = ReferencePose[Start].TransformPosition(Shape.StartOffset);
			const FVector B = ReferencePose[End].TransformPosition(Shape.EndOffset);
			const double Radius = Shape.Radius * FMath::Max(ReferencePose[Start].GetMaximumAxisScale(), ReferencePose[End].GetMaximumAxisScale());
			const FLinearColor Color = FLinearColor::MakeFromHSV8(static_cast<uint8>((Index * 43) % 255), 190, 255);
			const FVector Axis = B - A;
			if (Axis.IsNearlyZero()) {
				DrawWireSphere(PDI, A, Color, Radius, 24, SDPG_Foreground, 1.5f);
			} else {
				const FVector Z = Axis.GetSafeNormal();
				FVector X, Y;
				Z.FindBestAxisVectors(X, Y);
				DrawWireCapsule(PDI, (A + B) * .5, X, Y, Z, Color, Radius, Axis.Size() * .5 + Radius, 24, SDPG_Foreground, 1.5f);
			}
		}
	}

	FAdvancedPreviewScene Scene;
	TObjectPtr<USkeletalMeshComponent> Component;
	TWeakObjectPtr<UKawaiiClothingProxyAsset> Definition;
	TArray<FTransform> ReferencePose;
};

class FKawaiiClothingProxyViewportClient : public FEditorViewportClient
{
public:
	FKawaiiClothingProxyViewportClient(TSharedRef<FKawaiiClothingProxyPreview> InPreview, TSharedRef<SEditorViewport> InViewport)
		: FEditorViewportClient(nullptr, &InPreview->Scene, InViewport), Preview(InPreview) {}

	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override {
		FEditorViewportClient::Draw(View, PDI);
		Preview->Draw(PDI);
	}

private:
	TSharedRef<FKawaiiClothingProxyPreview> Preview;
};

class SKawaiiClothingProxyViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SKawaiiClothingProxyViewport)
	{}
	SLATE_END_ARGS()
	void Construct(const FArguments&, TSharedRef<FKawaiiClothingProxyPreview> InPreview) {
		Preview = InPreview;
		SEditorViewport::Construct(SEditorViewport::FArguments());
	}
	virtual ~SKawaiiClothingProxyViewport() {
		if (Client) {
			Client->Viewport = nullptr;
		}
	}
	void FrameMesh() {
		if (Preview->Component->GetSkeletalMeshAsset()) {
			Client->FocusViewportOnBox(Preview->Component->Bounds.GetBox(), true);
		}
	}

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override {
		Client = MakeShared<FKawaiiClothingProxyViewportClient>(Preview.ToSharedRef(), SharedThis(this));
		Client->SetViewMode(VMI_Lit);
		Client->SetViewportType(LVT_Perspective);
		Client->SetViewLocation(FVector(250, 250, 140));
		Client->SetViewRotation(FRotator(-12, -135, 0));
		Client->SetRealtime(true);
		Client->EngineShowFlags.SetGrid(false);
		Client->EngineShowFlags.SetSelectionOutline(false);
		return Client.ToSharedRef();
	}
	virtual TSharedPtr<SWidget> MakeViewportToolbar() override { return nullptr; }

private:
	TSharedPtr<FKawaiiClothingProxyPreview> Preview;
	TSharedPtr<FKawaiiClothingProxyViewportClient> Client;
};

void SKawaiiClothingProxyPanel::Construct(const FArguments&) {
	PreviewScene = MakeShared<FKawaiiClothingProxyPreview>();
	FDetailsViewArgs Args;
	Args.bHideSelectionTip = true;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Details = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor").CreateDetailView(Args);
	Details->OnFinishedChangingProperties().AddSP(this, &SKawaiiClothingProxyPanel::PropertiesChanged);
	ChildSlot
		[SNew(SVerticalBox)
		 + SVerticalBox::Slot().AutoHeight().Padding(
			 8)[SNew(STextBlock)
					.AutoWrapText(true)
					.Text(LOCTEXT(
						"Help",
						"选择衣物网格体，创建或载入代理定义。右侧编辑球体／胶囊的骨骼端点、局部偏移和半径，左侧实时显示参考姿态。两端重合时为球体。保存后把定义指定到烘焙部件的衣物代理字段。"))]
		 + SVerticalBox::Slot().AutoHeight().Padding(
			 8,
			 0)[SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)[SNew(STextBlock).Text(LOCTEXT("Definition", "代理定义"))]
				+ SHorizontalBox::Slot().FillWidth(1.f)[SNew(SObjectPropertyEntryBox)
															.AllowedClass(UKawaiiClothingProxyAsset::StaticClass())
															.ObjectPath_Lambda([this] {
																return Definition.IsValid() ? Definition->GetPathName() : FString();
															})
															.OnObjectChanged(this, &SKawaiiClothingProxyPanel::SelectDefinition)]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 8, 0)[SNew(STextBlock).Text(LOCTEXT("Mesh", "衣物网格体"))]
				+ SHorizontalBox::Slot().FillWidth(1.f)[SNew(SObjectPropertyEntryBox)
															.AllowedClass(USkeletalMesh::StaticClass())
															.ObjectPath_Lambda([this] {
																return SelectedMesh.IsValid() ? SelectedMesh->GetPathName() : FString();
															})
															.OnObjectChanged(this, &SKawaiiClothingProxyPanel::SelectMesh)]]
		 + SVerticalBox::Slot().AutoHeight().Padding(
			 8)[SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.f, 4.f))
				+ SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Create", "创建代理定义")).OnClicked(this, &SKawaiiClothingProxyPanel::CreateDefinition)]
				+ SWrapBox::Slot()[SNew(SButton)
									   .Text(LOCTEXT("Save", "校验并保存"))
									   .IsEnabled_Lambda([this] {
										   return Definition.IsValid();
									   })
									   .OnClicked(this, &SKawaiiClothingProxyPanel::SaveDefinition)]
				+ SWrapBox::Slot()[SNew(SButton)
									   .Text(LOCTEXT("Browse", "定位资产"))
									   .IsEnabled_Lambda([this] {
										   return Definition.IsValid();
									   })
									   .OnClicked(this, &SKawaiiClothingProxyPanel::BrowseDefinition)]
				+ SWrapBox::Slot()[SNew(SButton).Text(LOCTEXT("Focus", "聚焦衣物")).OnClicked_Lambda([this] {
					  Viewport->FrameMesh();
					  return FReply::Handled();
				  })]
				+ SWrapBox::Slot()[SNew(SButton)
									   .Text(LOCTEXT("Import", "追加 PhysicsAsset 代理"))
									   .ToolTipText(LOCTEXT("ImportHelp", "追加衣物 PhysicsAsset 中有效的球体和胶囊，不导入盒体或凸包，不修改 PhysicsAsset。"))
									   .IsEnabled_Lambda([this] {
										   return Definition.IsValid() && SelectedMesh.IsValid();
									   })
									   .OnClicked(this, &SKawaiiClothingProxyPanel::ImportPhysicsAsset)]]
		 + SVerticalBox::Slot().AutoHeight().Padding(
			 8,
			 0)[SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)[SNew(STextBlock).Text(LOCTEXT("Bone", "新增代理的起点骨骼"))]
				+ SHorizontalBox::Slot().FillWidth(1.f)[SAssignNew(BonePicker, SComboBox<TSharedPtr<FName>>) .OptionsSource(&BoneOptions) .OnGenerateWidget_Lambda([](TSharedPtr<FName> Item) {
																return SNew(STextBlock).Text(Item.IsValid() ? FText::FromName(*Item) : FText::GetEmpty());
															})
															.OnSelectionChanged_Lambda([this](TSharedPtr<FName> Item, ESelectInfo::Type) {
																SelectedBone = Item;
															})[SNew(STextBlock).Text_Lambda([this] {
																return SelectedBone.IsValid() ? FText::FromName(*SelectedBone) : LOCTEXT("SelectBone", "请选择骨骼");
															})]]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0)[SNew(SButton)
																	   .Text(LOCTEXT("Add", "添加代理"))
																	   .IsEnabled_Lambda([this] {
																		   return Definition.IsValid() && SelectedBone.IsValid()
																				  && Definition->Shapes.Num() < KawaiiClothingProxy::MaxShapes;
																	   })
																	   .OnClicked(this, &SKawaiiClothingProxyPanel::AddShape)]]
		 + SVerticalBox::Slot().AutoHeight().Padding(8)[SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] {
			   return FText::FromString(Message);
		   })]
		 + SVerticalBox::Slot().FillHeight(
			 1.f)[SNew(SSplitter) + SSplitter::Slot().Value(.58f)[SAssignNew(Viewport, SKawaiiClothingProxyViewport, PreviewScene.ToSharedRef())]
				  + SSplitter::Slot().Value(.42f)[Details.ToSharedRef()]]];
	GEditor->RegisterForUndo(this);
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().GetSelectedAssets(Assets);
	for (const FAssetData& Asset : Assets) {
		if (Asset.IsInstanceOf(UKawaiiClothingProxyAsset::StaticClass())) {
			SetDefinition(Cast<UKawaiiClothingProxyAsset>(Asset.GetAsset()));
			break;
		}
		if (!SelectedMesh.IsValid() && Asset.IsInstanceOf(USkeletalMesh::StaticClass())) {
			SelectedMesh.Reset(Cast<USkeletalMesh>(Asset.GetAsset()));
		}
	}
	RefreshMesh();
}

SKawaiiClothingProxyPanel::~SKawaiiClothingProxyPanel() {
	if (GEditor) {
		GEditor->UnregisterForUndo(this);
	}
}

void SKawaiiClothingProxyPanel::Tick(const FGeometry& Geometry, double CurrentTime, float DeltaTime) {
	SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
	// Handles edits in another details window as well as undo of the soft mesh reference.
	if (Definition.IsValid() && Definition->PreviewMesh.ToSoftObjectPath() != LastPreviewMeshPath) {
		RefreshMesh();
	}
}

void SKawaiiClothingProxyPanel::PostUndo(bool bSuccess) {
	if (bSuccess) {
		RefreshMesh();
		Details->ForceRefresh();
	}
}

void SKawaiiClothingProxyPanel::SelectDefinition(const FAssetData& Asset) {
	SetDefinition(Cast<UKawaiiClothingProxyAsset>(Asset.GetAsset()));
}

void SKawaiiClothingProxyPanel::SetDefinition(UKawaiiClothingProxyAsset* Asset) {
	Definition.Reset(Asset);
	if (Asset) {
		Asset->SetFlags(RF_Transactional);
	}
	PreviewScene->Definition = Asset;
	Details->SetObject(Asset);
	RefreshMesh();
	Message = Asset ? TEXT("已载入代理定义。编辑会修改此资产，可使用撤销；完成后点击“校验并保存”。") : FString();
}

void SKawaiiClothingProxyPanel::SelectMesh(const FAssetData& Asset) {
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
	if (Definition.IsValid()) {
		const FScopedTransaction Transaction(LOCTEXT("ChangeMesh", "修改衣物代理网格体"));
		Definition->Modify();
		Definition->PreviewMesh = Mesh;
		Definition->PostEditChange();
		Definition->MarkPackageDirty();
		Details->ForceRefresh();
	}
	SelectedMesh.Reset(Mesh);
	RefreshMesh();
}

void SKawaiiClothingProxyPanel::RefreshMesh() {
	if (Definition.IsValid()) {
		LastPreviewMeshPath = Definition->PreviewMesh.ToSoftObjectPath();
		SelectedMesh.Reset(Definition->PreviewMesh.LoadSynchronous());
	}
	PreviewScene->SetMesh(SelectedMesh.Get());
	BoneOptions.Reset();
	const FName PreviousBone = SelectedBone.IsValid() ? *SelectedBone : NAME_None;
	SelectedBone.Reset();
	if (SelectedMesh.IsValid()) {
		const FReferenceSkeleton& Ref = SelectedMesh->GetRefSkeleton();
		for (int32 BoneIndex = 0; BoneIndex < Ref.GetRawBoneNum(); ++BoneIndex) {
			const FName BoneName = Ref.GetBoneName(BoneIndex);
			const TSharedPtr<FName> Item = MakeShared<FName>(BoneName);
			BoneOptions.Add(Item);
			if (BoneName == PreviousBone) {
				SelectedBone = Item;
			}
		}
	}
	if (!SelectedBone.IsValid() && !BoneOptions.IsEmpty()) {
		SelectedBone = BoneOptions[0];
	}
	if (BonePicker) {
		BonePicker->RefreshOptions();
		BonePicker->SetSelectedItem(SelectedBone);
	}
	if (Viewport) {
		Viewport->FrameMesh();
	}
}

void SKawaiiClothingProxyPanel::PropertiesChanged(const FPropertyChangedEvent&) {
	if (Definition.IsValid()) {
		Definition->MarkPackageDirty();
		if (Definition->PreviewMesh.ToSoftObjectPath() != LastPreviewMeshPath) {
			RefreshMesh();
		}
		FString Error;
		Message = ValidateDefinition(Error) ? TEXT("代理定义有效。保存后重新烘焙使用此定义的代理运动资产。") : Error;
	}
}

bool SKawaiiClothingProxyPanel::ValidateDefinition(FString& Error) const {
	if (!Definition.IsValid() || !SelectedMesh.IsValid() || !SelectedMesh->GetSkeleton()) {
		Error = TEXT("请选择有效的代理定义和带骨架的衣物网格体。");
		return false;
	}
	if (!Definition->Validate(Error)) {
		return false;
	}
	TArray<FName> Names;
	TArray<int32> Parents, MeshIndices;
	return KawaiiClothingProxy::BuildMinimalHierarchy(SelectedMesh->GetRefSkeleton(), Definition->Shapes, Names, Parents, MeshIndices, Error);
}

FName SKawaiiClothingProxyPanel::MakeShapeId(const FString& Prefix) const {
	for (int32 Index = 1;; ++Index) {
		const FName Candidate(*FString::Printf(TEXT("%s_%d"), *Prefix, Index));
		if (!Definition->Shapes.ContainsByPredicate([Candidate](const FKawaiiClothingProxyShape& Shape) {
				return Shape.Id == Candidate;
			}))
		{
			return Candidate;
		}
	}
}

FReply SKawaiiClothingProxyPanel::CreateDefinition() {
	TStrongObjectPtr<UKawaiiClothingProxyFactory> Factory(NewObject<UKawaiiClothingProxyFactory>());
	Factory->SelectedMesh = SelectedMesh.Get();
	Factory->bEditAfterNew = false;
	UObject* Asset =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools")
			.Get()
			.CreateAssetWithDialog(Factory->GetDefaultNewAssetName(), TEXT("/Game/KawaiiProxies"), UKawaiiClothingProxyAsset::StaticClass(), Factory.Get());
	if (Asset) {
		SetDefinition(CastChecked<UKawaiiClothingProxyAsset>(Asset));
		Message = TEXT("已创建代理定义。选择骨骼添加代理，或追加 PhysicsAsset 中的球体／胶囊，然后校验并保存。");
	}
	return FReply::Handled();
}

FReply SKawaiiClothingProxyPanel::SaveDefinition() {
	if (!ValidateDefinition(Message)) {
		return FReply::Handled();
	}
	UPackage* Package = Definition->GetOutermost();
	if (Package == GetTransientPackage() || !FPackageName::IsValidLongPackageName(Package->GetName())) {
		Message = TEXT("代理定义没有有效内容路径，请先创建资产。");
		return FReply::Handled();
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Definition.Get(), *Filename, Args)) {
		Message = TEXT("代理定义校验通过，但保存失败，请检查文件权限或源代码管理状态。");
		return FReply::Handled();
	}
	Message = FString::Printf(TEXT("已保存 %d 个代理：%s。定义改变后，请重新烘焙关联的代理运动资产。"), Definition->Shapes.Num(), *Definition->GetPathName());
	return BrowseDefinition();
}

FReply SKawaiiClothingProxyPanel::AddShape() {
	if (!Definition.IsValid() || !SelectedMesh.IsValid() || !SelectedBone.IsValid() || Definition->Shapes.Num() >= KawaiiClothingProxy::MaxShapes) {
		return FReply::Handled();
	}
	const FScopedTransaction Transaction(LOCTEXT("AddShapeTransaction", "添加衣物代理"));
	Definition->Modify();
	FKawaiiClothingProxyShape Shape;
	Shape.Id = MakeShapeId(SelectedBone->ToString());
	Shape.StartBone = *SelectedBone;
	const FReferenceSkeleton& Ref = SelectedMesh->GetRefSkeleton();
	const int32 Parent = Ref.FindBoneIndex(Shape.StartBone);
	for (int32 Bone = Parent + 1; Bone < Ref.GetRawBoneNum(); ++Bone) {
		if (Ref.GetParentIndex(Bone) == Parent) {
			Shape.EndBone = Ref.GetBoneName(Bone);
			break;
		}
	}
	Definition->Shapes.Add(Shape);
	Definition->PostEditChange();
	Definition->MarkPackageDirty();
	Details->ForceRefresh();
	Message = TEXT("已添加代理。默认连接所选骨骼及其第一个子骨骼；没有子骨骼时为球体。请在右侧调整半径和端点，检查与衣物的贴合程度。");
	return FReply::Handled();
}

FReply SKawaiiClothingProxyPanel::ImportPhysicsAsset() {
	if (!Definition.IsValid() || !SelectedMesh.IsValid()) {
		return FReply::Handled();
	}
	const UPhysicsAsset* Physics = SelectedMesh->GetPhysicsAsset();
	if (!Physics) {
		Message = TEXT("当前衣物网格体没有 PhysicsAsset。请选择骨骼手动添加代理。");
		return FReply::Handled();
	}
	FScopedTransaction Transaction(LOCTEXT("ImportTransaction", "追加衣物物理资产代理"));
	Definition->Modify();
	int32 Added = 0;
	int32 Skipped = 0;
	const auto Add = [&](FName Bone, const FVector& Center, const FRotator& Rotation, float Radius, float Length) {
		if (Definition->Shapes.Num() >= KawaiiClothingProxy::MaxShapes || SelectedMesh->GetRefSkeleton().FindBoneIndex(Bone) == INDEX_NONE
			|| !FMath::IsFinite(Radius) || Radius <= 0.f || !FMath::IsFinite(Length) || Length < 0.f || Center.ContainsNaN() || Rotation.ContainsNaN())
		{
			++Skipped;
			return;
		}
		FKawaiiClothingProxyShape Shape;
		Shape.Id = MakeShapeId(Bone.ToString());
		Shape.StartBone = Shape.EndBone = Bone;
		const FVector HalfAxis = Rotation.Quaternion().GetAxisZ() * Length * .5;
		Shape.StartOffset = Center - HalfAxis;
		Shape.EndOffset = Center + HalfAxis;
		Shape.Radius = Radius;
		Definition->Shapes.Add(Shape);
		++Added;
	};
	for (const USkeletalBodySetup* Setup : Physics->SkeletalBodySetups) {
		if (!Setup) {
			continue;
		}
		for (const FKSphylElem& Shape : Setup->AggGeom.SphylElems) {
			Add(Setup->BoneName, Shape.Center, Shape.Rotation, Shape.Radius, Shape.Length);
		}
		for (const FKSphereElem& Shape : Setup->AggGeom.SphereElems) {
			Add(Setup->BoneName, Shape.Center, FRotator::ZeroRotator, Shape.Radius, 0.f);
		}
	}
	if (Added > 0) {
		Definition->PostEditChange();
		Definition->MarkPackageDirty();
		Details->ForceRefresh();
	} else {
		Transaction.Cancel();
	}
	Message =
		FString::Printf(TEXT("已追加 %d 个有效球体／胶囊，保留原有代理。跳过 %d 个无效或超过 64 个上限的形状。盒体和凸包未导入；请在视口中检查贴合程度。"),
						Added,
						Skipped
		);
	return FReply::Handled();
}

FReply SKawaiiClothingProxyPanel::BrowseDefinition() {
	if (Definition.IsValid()) {
		TArray<UObject*> Assets{Definition.Get()};
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
