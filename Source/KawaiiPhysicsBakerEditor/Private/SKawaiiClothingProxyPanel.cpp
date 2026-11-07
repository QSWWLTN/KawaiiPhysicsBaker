#include "SKawaiiClothingProxyPanel.h"

#include "AdvancedPreviewScene.h"
#include "AssetEditorModeManager.h"
#include "AssetToolsModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HitProxies.h"
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
#include "Styling/AppStyle.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "KawaiiClothingProxyAsset.h"
#include "KawaiiClothingProxyEditorGeometry.h"
#include "KawaiiClothingProxyFactory.h"

#define LOCTEXT_NAMESPACE "KawaiiClothingProxyPanel"

using KawaiiClothingProxyEditor::EEditTarget;

struct HKawaiiClothingProxyShape : public HHitProxy
{
	DECLARE_HIT_PROXY();
	HKawaiiClothingProxyShape(int32 InIndex, EEditTarget InTarget = EEditTarget::Shape)
		: HHitProxy(HPP_UI), Index(InIndex), Target(InTarget) {}
	virtual EMouseCursor::Type GetMouseCursor() override { return EMouseCursor::Crosshairs; }
	int32 Index;
	EEditTarget Target;
};

IMPLEMENT_HIT_PROXY(HKawaiiClothingProxyShape, HHitProxy);

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

	int32 GetSelectedIndex() const {
		const UKawaiiClothingProxyAsset* Asset = Definition.Get();
		return Asset && bHasSelection ? Asset->Shapes.IndexOfByPredicate([this](const FKawaiiClothingProxyShape& Shape) { return Shape.Id == SelectedId; }) : INDEX_NONE;
	}

	void SelectShape(int32 Index, EEditTarget Target = EEditTarget::Shape) {
		const UKawaiiClothingProxyAsset* Asset = Definition.Get();
		bHasSelection = Asset && Asset->Shapes.IsValidIndex(Index);
		SelectedId = bHasSelection ? Asset->Shapes[Index].Id : NAME_None;
		EditTarget = Target;
	}

	bool GetGeometry(int32 Index, KawaiiClothingProxyEditor::FGeometry& Out) const {
		const UKawaiiClothingProxyAsset* Asset = Definition.Get();
		const USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
		return Asset && Mesh && Asset->Shapes.IsValidIndex(Index)
			&& KawaiiClothingProxyEditor::ResolveGeometry(Asset->Shapes[Index], Mesh->GetRefSkeleton(), ReferencePose, Component->GetComponentTransform(), Out);
	}

	void Draw(FPrimitiveDrawInterface* PDI) const {
		const UKawaiiClothingProxyAsset* Asset = Definition.Get();
		const USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset();
		if (!Asset || !Mesh) {
			return;
		}
		for (int32 Index = 0; Index < Asset->Shapes.Num(); ++Index) {
			KawaiiClothingProxyEditor::FGeometry Geometry;
			if (!GetGeometry(Index, Geometry)) {
				continue;
			}
			const bool bSelected = Index == GetSelectedIndex();
			const FLinearColor Color = bSelected ? FLinearColor::Yellow : FLinearColor::MakeFromHSV8(static_cast<uint8>((Index * 43) % 255), 190, 255);
			PDI->SetHitProxy(new HKawaiiClothingProxyShape(Index));
			KawaiiClothingProxyEditor::Draw(PDI, Geometry, Color, bSelected ? 2.5f : 1.5f);
			PDI->DrawLine(Geometry.Start, Geometry.End, Color, SDPG_Foreground, 1.5f, 0.f, true);
			PDI->SetHitProxy(nullptr);
			if (bSelected) {
				PDI->SetHitProxy(new HKawaiiClothingProxyShape(Index, EEditTarget::Start));
				PDI->DrawPoint(Geometry.Start, EditTarget == EEditTarget::Start ? FLinearColor::Yellow : FLinearColor::Green, 12.f, SDPG_Foreground);
				PDI->SetHitProxy(new HKawaiiClothingProxyShape(Index, EEditTarget::End));
				PDI->DrawPoint(Geometry.End, EditTarget == EEditTarget::End ? FLinearColor::Yellow : FLinearColor::Blue, 12.f, SDPG_Foreground);
				PDI->SetHitProxy(nullptr);
			}
		}
	}

	FAdvancedPreviewScene Scene;
	TObjectPtr<USkeletalMeshComponent> Component;
	TWeakObjectPtr<UKawaiiClothingProxyAsset> Definition;
	TArray<FTransform> ReferencePose;
	FName SelectedId;
	bool bHasSelection = false;
	EEditTarget EditTarget = EEditTarget::Shape;
	FSimpleDelegate OnShapeEdited;
};

class FKawaiiClothingProxyViewportClient : public FEditorViewportClient
{
public:
	FKawaiiClothingProxyViewportClient(TSharedRef<FKawaiiClothingProxyPreview> InPreview, TSharedRef<SEditorViewport> InViewport)
		: FEditorViewportClient(nullptr, &InPreview->Scene, InViewport), Preview(InPreview) {
		// A standalone asset viewport starts without active modes; UE suppresses its native widget until a mode enables it.
		StaticCastSharedPtr<FAssetEditorModeManager>(ModeTools)->SetPreviewScene(&InPreview->Scene);
		ModeTools->ActivateDefaultMode();
		EngineShowFlags.SetModeWidgets(true);
	}

	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override {
		Preview->Draw(PDI);
		FEditorViewportClient::Draw(View, PDI);
	}

	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override {
		if (Key == EKeys::LeftMouseButton && HitProxy && HitProxy->IsA(HKawaiiClothingProxyShape::StaticGetType())) {
			const auto* ShapeProxy = static_cast<HKawaiiClothingProxyShape*>(HitProxy);
			Preview->SelectShape(ShapeProxy->Index, ShapeProxy->Target);
			if (ShapeProxy->Target != EEditTarget::Shape) {
				SetWidgetMode(UE::Widget::WM_Translate);
			}
			Invalidate();
			return;
		}
		if (Key == EKeys::LeftMouseButton && !HitProxy) {
			Preview->SelectShape(INDEX_NONE);
			Invalidate();
		}
	}

	virtual UE::Widget::EWidgetMode GetWidgetMode() const override {
		KawaiiClothingProxyEditor::FGeometry Geometry;
		return Preview->GetGeometry(Preview->GetSelectedIndex(), Geometry) ? WidgetMode : UE::Widget::WM_None;
	}

	virtual bool CanSetWidgetMode(UE::Widget::EWidgetMode Mode) const override {
		return !bEditing && (Mode == UE::Widget::WM_Translate || Mode == UE::Widget::WM_Scale);
	}

	virtual void SetWidgetMode(UE::Widget::EWidgetMode Mode) override {
		if (CanSetWidgetMode(Mode)) {
			WidgetMode = Mode;
			if (Mode == UE::Widget::WM_Scale) {
				Preview->EditTarget = EEditTarget::Shape;
			}
			Invalidate();
		}
	}

	virtual FVector GetWidgetLocation() const override {
		KawaiiClothingProxyEditor::FGeometry Geometry;
		if (!Preview->GetGeometry(Preview->GetSelectedIndex(), Geometry)) {
			return FVector::ZeroVector;
		}
		return Preview->EditTarget == EEditTarget::Start ? Geometry.Start
			: Preview->EditTarget == EEditTarget::End ? Geometry.End : (Geometry.Start + Geometry.End) * 0.5;
	}

	virtual FMatrix GetWidgetCoordSystem() const override {
		KawaiiClothingProxyEditor::FGeometry Geometry;
		if (WidgetMode == UE::Widget::WM_Scale && Preview->GetGeometry(Preview->GetSelectedIndex(), Geometry) && (!Geometry.BoxExtent.IsZero() || !Geometry.Start.Equals(Geometry.End))) {
			return !Geometry.BoxExtent.IsZero() ? FQuatRotationMatrix(Geometry.BoxRotation) : FRotationMatrix::MakeFromZ(Geometry.End - Geometry.Start);
		}
		return FMatrix::Identity;
	}

	virtual ECoordSystem GetWidgetCoordSystemSpace() const override {
		return WidgetMode == UE::Widget::WM_Scale ? COORD_Local : COORD_World;
	}

	virtual void TrackingStarted(const FInputEventState&, bool bIsDraggingWidget, bool bNudge) override {
		bEditing = bIsDraggingWidget || bNudge;
		bHasScaleBaseline = false;
		AccumulatedScale = FVector::ZeroVector;
	}

	virtual bool InputWidgetDelta(FViewport*, EAxisList::Type CurrentAxis, FVector& Drag, FRotator&, FVector& Scale) override {
		KawaiiClothingProxyEditor::FGeometry Geometry;
		const int32 Index = Preview->GetSelectedIndex();
		if (!bEditing || CurrentAxis == EAxisList::None || !Preview->GetGeometry(Index, Geometry)) {
			return false;
		}
		UKawaiiClothingProxyAsset* Asset = Preview->Definition.Get();
		FKawaiiClothingProxyShape Edited = Asset->Shapes[Index];
		bool bChanged = false;
		if (WidgetMode == UE::Widget::WM_Scale) {
			if (!bHasScaleBaseline) {
				ScaleBaseline = Edited;
				ScaleGeometry = Geometry;
				bHasScaleBaseline = true;
			}
			// UE already converts normal scaling to fractional deltas. Its legacy percentage mode needs conversion.
			AccumulatedScale += GEditor->UsePercentageBasedScaling()
				? Scale * ((GEditor->GetScaleGridSize() / 100.0f) / GEditor->GetGridSize()) : Scale;
			Edited = ScaleBaseline;
			KawaiiClothingProxyEditor::Scale(Edited, ScaleGeometry, AccumulatedScale);
			bChanged = Edited.Radius != Asset->Shapes[Index].Radius || !Edited.StartOffset.Equals(Asset->Shapes[Index].StartOffset)
				|| !Edited.EndOffset.Equals(Asset->Shapes[Index].EndOffset);
		} else {
			bChanged = KawaiiClothingProxyEditor::Translate(Edited, Geometry, Drag, Preview->EditTarget);
		}
		if (bChanged) {
			if (!Transaction) {
				Transaction = MakeUnique<FScopedTransaction>(LOCTEXT("TransformProxy", "调整衣物代理"));
				EditedAsset = Asset;
				Asset->Modify();
				Asset->PreEditChange(FindFProperty<FProperty>(UKawaiiClothingProxyAsset::StaticClass(), GET_MEMBER_NAME_CHECKED(UKawaiiClothingProxyAsset, Shapes)));
			}
			Asset->Shapes[Index] = Edited;
			FPropertyChangedEvent Changed(FindFProperty<FProperty>(UKawaiiClothingProxyAsset::StaticClass(), GET_MEMBER_NAME_CHECKED(UKawaiiClothingProxyAsset, Shapes)), EPropertyChangeType::Interactive);
			Asset->PostEditChangeProperty(Changed);
			Invalidate();
		}
		return true;
	}

	virtual void TrackingStopped() override { FinishEdit(); }
	void FinishEdit() {
		bEditing = false;
		if (Transaction) {
			if (UKawaiiClothingProxyAsset* Asset = EditedAsset.Get()) {
				FPropertyChangedEvent Changed(FindFProperty<FProperty>(UKawaiiClothingProxyAsset::StaticClass(), GET_MEMBER_NAME_CHECKED(UKawaiiClothingProxyAsset, Shapes)), EPropertyChangeType::ValueSet);
				Asset->PostEditChangeProperty(Changed);
				Asset->MarkPackageDirty();
			}
			Transaction.Reset();
			EditedAsset.Reset();
			Preview->OnShapeEdited.ExecuteIfBound();
		}
	}
	bool IsEditing() const { return bEditing; }

private:
	TSharedRef<FKawaiiClothingProxyPreview> Preview;
	UE::Widget::EWidgetMode WidgetMode = UE::Widget::WM_Translate;
	bool bEditing = false;
	bool bHasScaleBaseline = false;
	FKawaiiClothingProxyShape ScaleBaseline;
	KawaiiClothingProxyEditor::FGeometry ScaleGeometry;
	FVector AccumulatedScale = FVector::ZeroVector;
	TUniquePtr<FScopedTransaction> Transaction;
	TWeakObjectPtr<UKawaiiClothingProxyAsset> EditedAsset;
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
			Client->FinishEdit();
			Client->Viewport = nullptr;
		}
	}

	void FrameMesh() {
		if (Preview->Component->GetSkeletalMeshAsset()) {
			Client->FocusViewportOnBox(Preview->Component->Bounds.GetBox(), true);
		}
	}

protected:
	virtual void BindCommands() override {
		SEditorViewport::BindCommands();
		CommandList->MapAction(FGenericCommands::Get().Undo,
			FExecuteAction::CreateLambda([] { GEditor->UndoTransaction(); }),
			FCanExecuteAction::CreateLambda([this] { return !Client->IsEditing(); }));
		CommandList->MapAction(FGenericCommands::Get().Redo,
			FExecuteAction::CreateLambda([] { GEditor->RedoTransaction(); }),
			FCanExecuteAction::CreateLambda([this] { return !Client->IsEditing(); }));
	}

	virtual void OnFocusViewportToSelection() override {
		KawaiiClothingProxyEditor::FGeometry Geometry;
		if (Preview->GetGeometry(Preview->GetSelectedIndex(), Geometry)) {
			FBox Bounds(Geometry.Start, Geometry.Start);
			Bounds += Geometry.End;
			Client->FocusViewportOnBox(Bounds.ExpandBy(Geometry.Radius + 5.0), true);
		} else {
			FrameMesh();
		}
	}

	virtual TSharedPtr<SWidget> BuildViewportToolbar() override {
		return SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.f, 4.f))
			+ SWrapBox::Slot()
			[
				SNew(SComboButton)
				.OnGetMenuContent_Lambda([this] {
					FMenuBuilder Menu(true, nullptr);
					if (const UKawaiiClothingProxyAsset* Asset = Preview->Definition.Get()) {
						for (int32 Index = 0; Index < Asset->Shapes.Num(); ++Index) {
							Menu.AddMenuEntry(FText::FromString(FString::Printf(TEXT("%d: %s"), Index, *Asset->Shapes[Index].Id.ToString())), FText::GetEmpty(), FSlateIcon(),
								FUIAction(FExecuteAction::CreateLambda([this, Index] { Preview->SelectShape(Index); Client->Invalidate(); })));
						}
					}
					return Menu.MakeWidget();
				})
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda([this] {
						return Preview->GetSelectedIndex() != INDEX_NONE ? FText::FromName(Preview->SelectedId) : LOCTEXT("SelectProxy", "选择代理");
					})
				]
			]
			+ SWrapBox::Slot() [ MakeModeButton(LOCTEXT("MoveProxy", "移动 (W)"), UE::Widget::WM_Translate) ]
			+ SWrapBox::Slot() [ MakeModeButton(LOCTEXT("ScaleProxy", "缩放 (R)"), UE::Widget::WM_Scale) ]
			+ SWrapBox::Slot() [ MakeTargetButton(LOCTEXT("WholeProxy", "整体"), EEditTarget::Shape) ]
			+ SWrapBox::Slot() [ MakeTargetButton(LOCTEXT("StartProxy", "起点"), EEditTarget::Start) ]
			+ SWrapBox::Slot() [ MakeTargetButton(LOCTEXT("EndProxy", "终点"), EEditTarget::End) ];
	}

	TSharedRef<SWidget> MakeModeButton(FText Label, UE::Widget::EWidgetMode Mode) {
		return SNew(SCheckBox).Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.IsChecked_Lambda([this, Mode] { return Client->GetWidgetMode() == Mode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Mode](ECheckBoxState) { Client->SetWidgetMode(Mode); })
			[ SNew(STextBlock).Text(Label) ];
	}

	TSharedRef<SWidget> MakeTargetButton(FText Label, EEditTarget Target) {
		return SNew(SCheckBox).Style(FAppStyle::Get(), "ToggleButtonCheckbox")
			.IsChecked_Lambda([this, Target] { return Preview->EditTarget == Target ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Target](ECheckBoxState) {
				Preview->EditTarget = Target;
				if (Target != EEditTarget::Shape) { Client->SetWidgetMode(UE::Widget::WM_Translate); }
				Client->Invalidate();
			})
			[ SNew(STextBlock).Text(Label) ];
	}

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
	PreviewScene->OnShapeEdited = FSimpleDelegate::CreateSP(this, &SKawaiiClothingProxyPanel::GizmoEdited);
	FDetailsViewArgs Args;
	Args.bHideSelectionTip = true;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Details = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor").CreateDetailView(Args);
	Details->OnFinishedChangingProperties().AddSP(this, &SKawaiiClothingProxyPanel::PropertiesChanged);
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT(
				"Help",
				"选择或点击代理线框后，W 移动、R 缩放、F 聚焦。移动可选择整体／起点／终点；缩放时 X/Y 调整半径，Z 调整胶囊长度，中心手柄等比缩放（球体各轴均调整半径，盒体按各轴调整半尺寸）。支持 Ctrl+Z / Ctrl+Y 撤销重做。右侧可精确编辑数值；完成后校验并保存。"
			))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("Definition", "代理定义"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UKawaiiClothingProxyAsset::StaticClass())
				.ObjectPath_Lambda([this] {
					return Definition.IsValid() ? Definition->GetPathName() : FString();
				})
				.OnObjectChanged(this, &SKawaiiClothingProxyPanel::SelectDefinition)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("Mesh", "衣物网格体"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(USkeletalMesh::StaticClass())
				.ObjectPath_Lambda([this] {
					return SelectedMesh.IsValid() ? SelectedMesh->GetPathName() : FString();
				})
				.OnObjectChanged(this, &SKawaiiClothingProxyPanel::SelectMesh)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8)
		[
			SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.f, 4.f))
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(LOCTEXT("Create", "创建代理定义")).OnClicked(this, &SKawaiiClothingProxyPanel::CreateDefinition)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text(LOCTEXT("Save", "校验并保存"))
				.IsEnabled_Lambda([this] { return Definition.IsValid(); })
				.OnClicked(this, &SKawaiiClothingProxyPanel::SaveDefinition)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text(LOCTEXT("Browse", "定位资产"))
				.IsEnabled_Lambda([this] { return Definition.IsValid(); })
				.OnClicked(this, &SKawaiiClothingProxyPanel::BrowseDefinition)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(LOCTEXT("Focus", "聚焦衣物")).OnClicked_Lambda([this] {
					Viewport->FrameMesh();
					return FReply::Handled();
				})
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text(LOCTEXT("Import", "追加 PhysicsAsset 代理"))
				.ToolTipText(LOCTEXT("ImportHelp", "追加衣物 PhysicsAsset 中有效的球体、胶囊和盒体，不导入凸包，不修改 PhysicsAsset。"))
				.IsEnabled_Lambda([this] { return Definition.IsValid() && SelectedMesh.IsValid(); })
				.OnClicked(this, &SKawaiiClothingProxyPanel::ImportPhysicsAsset)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("Bone", "新增代理的起点骨骼"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
				SAssignNew(BonePicker, SComboBox<TSharedPtr<FName>>)
				.OptionsSource(&BoneOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FName> Item) {
					return SNew(STextBlock).Text(Item.IsValid() ? FText::FromName(*Item) : FText::GetEmpty());
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FName> Item, ESelectInfo::Type) {
					SelectedBone = Item;
				})
				[
					SNew(STextBlock).Text_Lambda([this] {
						return SelectedBone.IsValid() ? FText::FromName(*SelectedBone) : LOCTEXT("SelectBone", "请选择骨骼");
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("Add", "添加代理"))
				.IsEnabled_Lambda([this] {
					return Definition.IsValid() && SelectedBone.IsValid() && Definition->Shapes.Num() < KawaiiClothingProxy::MaxShapes;
				})
				.OnClicked(this, &SKawaiiClothingProxyPanel::AddShape)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8)
		[
			SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(Message); })
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SSplitter)
			+ SSplitter::Slot().Value(.58f)
			[
				SAssignNew(Viewport, SKawaiiClothingProxyViewport, PreviewScene.ToSharedRef())
			]
			+ SSplitter::Slot().Value(.42f)
			[
				Details.ToSharedRef()
			]
		]
	];
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
		if (Definition.IsValid() && Definition->PreviewMesh.ToSoftObjectPath() != LastPreviewMeshPath) {
			RefreshMesh();
		}
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
	PreviewScene->SelectShape(Asset && !Asset->Shapes.IsEmpty() ? 0 : INDEX_NONE);
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
		Message = ValidateDefinition(Error) ? TEXT("代理定义有效。修改形状或绑定后需重新烘焙；仅修改忽略目标骨链时，保存并重新预览即可，运行时下次采样生效。") : Error;
	}
}

void SKawaiiClothingProxyPanel::GizmoEdited() {
	Details->ForceRefresh();
	FPropertyChangedEvent Event(nullptr);
	PropertiesChanged(Event);
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
		if (!Definition->Shapes.ContainsByPredicate([Candidate](const FKawaiiClothingProxyShape& Shape) { return Shape.Id == Candidate; })) {
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
		Message = TEXT("已创建代理定义。选择骨骼添加代理，或追加 PhysicsAsset 中的球体／胶囊／盒体，然后校验并保存。");
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
	Message = FString::Printf(
		TEXT("已保存 %d 个代理：%s。形状或绑定改变需要重新烘焙；忽略目标骨链无需重新烘焙。"),
		Definition->Shapes.Num(),
		*Definition->GetPathName()
	);
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
	PreviewScene->SelectShape(Definition->Shapes.Num() - 1);
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
	const auto Add = [&](FName Bone, const FVector& Center, const FRotator& Rotation, float Radius, float Length, const FVector& BoxExtent = FVector(0., 0., 0.)) {
		if (Definition->Shapes.Num() >= KawaiiClothingProxy::MaxShapes || SelectedMesh->GetRefSkeleton().FindBoneIndex(Bone) == INDEX_NONE
			|| !FMath::IsFinite(Radius) || Radius <= 0.f || !FMath::IsFinite(Length) || Length < 0.f || Center.ContainsNaN() || Rotation.ContainsNaN() || BoxExtent.ContainsNaN() || (!BoxExtent.IsZero() && BoxExtent.GetMin() <= 0.))
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
		if (!BoxExtent.IsZero()) { Shape.bBox = true; Shape.BoxExtent = BoxExtent; Shape.BoxRotation = Rotation; }
		Definition->Shapes.Add(Shape);
		++Added;
	};
	for (const USkeletalBodySetup* Setup : Physics->SkeletalBodySetups) {
		if (!Setup) {
			continue;
		}
		for (const FKBoxElem& Shape : Setup->AggGeom.BoxElems) {
			Add(Setup->BoneName, Shape.Center, Shape.Rotation, 1.f, 0.f, FVector(Shape.X, Shape.Y, Shape.Z) * .5);
		}
		for (const FKSphylElem& Shape : Setup->AggGeom.SphylElems) {
			Add(Setup->BoneName, Shape.Center, Shape.Rotation, Shape.Radius, Shape.Length);
		}
		for (const FKSphereElem& Shape : Setup->AggGeom.SphereElems) {
			Add(Setup->BoneName, Shape.Center, FRotator::ZeroRotator, Shape.Radius, 0.f);
		}
	}
	if (Added > 0) {
		if (PreviewScene->GetSelectedIndex() == INDEX_NONE) {
			PreviewScene->SelectShape(Definition->Shapes.Num() - Added);
		}
		Definition->PostEditChange();
		Definition->MarkPackageDirty();
		Details->ForceRefresh();
	} else {
		Transaction.Cancel();
	}
	Message = FString::Printf(
		TEXT("已追加 %d 个有效球体／胶囊／盒体，保留原有代理。跳过 %d 个无效或超过 64 个上限的形状。凸包未导入；请在视口中检查贴合程度。"),
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
