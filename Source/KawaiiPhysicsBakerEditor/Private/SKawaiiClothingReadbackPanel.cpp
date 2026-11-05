#include "SKawaiiClothingReadbackPanel.h"

#include "Animation/AnimSequence.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "IAssetTools.h"
#include "IContentBrowserSingleton.h"
#include "IDetailsView.h"
#include "Misc/PackageName.h"
#include "PropertyEditorModule.h"
#include "SEditorViewport.h"
#include "SScrubWidget.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace {
	UKawaiiClothingProxyMotion* SelectedMotion() {
		TArray<FAssetData> Assets;
		FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Assets);
		for (const auto& Asset : Assets) {
			UObject* Object = Asset.GetAsset();
			if (auto* Motion = Cast<UKawaiiClothingProxyMotion>(Object)) { return Motion; }
			if (auto* Animation = Cast<UAnimSequence>(Object)) {
				const FString Path = Animation->GetPackage()->GetMetaData()->GetValue(Animation, TEXT("KawaiiBake.ProxyMotion"));
				if (!Path.IsEmpty()) { if (auto* Motion = LoadObject<UKawaiiClothingProxyMotion>(nullptr, *Path)) { return Motion; } }
			}
		}
		return nullptr;
	}
	class FReadbackViewportClient : public FEditorViewportClient {
	public:
		FReadbackViewportClient(TSharedRef<FKawaiiClothingReadbackPreview> InPreview, TSharedRef<SEditorViewport> Widget)
			: FEditorViewportClient(nullptr, &InPreview->GetScene(), Widget), Preview(InPreview) {}
		void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override { FEditorViewportClient::Draw(View, PDI); if (bShowProxy) { Preview->Draw(PDI); } }
		bool bShowProxy = true;
	private:
		TSharedRef<FKawaiiClothingReadbackPreview> Preview;
	};
}

class SKawaiiClothingReadbackViewport : public SEditorViewport {
public:
	SLATE_BEGIN_ARGS(SKawaiiClothingReadbackViewport) {} SLATE_END_ARGS()
	void Construct(const FArguments&, TSharedRef<FKawaiiClothingReadbackPreview> InPreview) { Preview = InPreview; SEditorViewport::Construct(SEditorViewport::FArguments()); }
	~SKawaiiClothingReadbackViewport() { if (Client) { Client->Viewport = nullptr; } }
	void Focus() { if (Preview->NumFrames()) { Client->FocusViewportOnBox(Preview->GetBounds(), true); } }
	void ShowProxy(bool bShow) { Client->bShowProxy = bShow; }
protected:
	TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override {
		Client = MakeShared<FReadbackViewportClient>(Preview.ToSharedRef(), SharedThis(this));
		Client->SetViewMode(VMI_Lit); Client->SetViewportType(LVT_Perspective);
		Client->SetViewLocation(FVector(250, 250, 140)); Client->SetViewRotation(FRotator(-12, -135, 0));
		Client->SetRealtime(true); Client->EngineShowFlags.SetGrid(false);
		return Client.ToSharedRef();
	}
	TSharedPtr<SWidget> MakeViewportToolbar() override { return nullptr; }
private:
	TSharedPtr<FKawaiiClothingReadbackPreview> Preview;
	TSharedPtr<FReadbackViewportClient> Client;
};

void KawaiiClothingReadback::OpenPanel(UKawaiiClothingProxyMotion* Motion, USkeletalMesh* BodyMesh, UKawaiiPhysicsIKCorrectionProfile* Profile) {
	FSlateApplication::Get().AddWindow(SNew(SWindow).Title(FText::FromString(TEXT("Kawaii 衣物动画回读"))).ClientSize(FVector2D(1200, 800))
		[SNew(SKawaiiClothingReadbackPanel, Motion, BodyMesh, Profile)]);
}
void KawaiiClothingReadback::OpenSelected() { OpenPanel(SelectedMotion()); }

void SKawaiiClothingReadbackPanel::Construct(const FArguments&, UKawaiiClothingProxyMotion* Motion, USkeletalMesh* BodyMesh, UKawaiiPhysicsIKCorrectionProfile* Profile) {
	Settings.Reset(NewObject<UKawaiiClothingReadbackSettings>());
	Settings->Motion = Motion; Settings->BodyMesh = BodyMesh; Settings->CorrectionProfile = Profile;
	FillFromMotion();
	Preview = MakeShared<FKawaiiClothingReadbackPreview>();
	FDetailsViewArgs Args; Args.bHideSelectionTip = true; Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Details = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor")).CreateDetailView(Args);
	Details->SetObject(Settings.Get());
	Details->OnFinishedChangingProperties().AddSP(this, &SKawaiiClothingReadbackPanel::SettingsChanged);
	ModifiedHandle = FCoreUObjectDelegates::OnObjectModified.AddSP(this, &SKawaiiClothingReadbackPanel::ObjectModified);
	Status = FText::FromString(TEXT("先手动修整并保存衣物动画，再读取预览；确认效果后更新代理轨迹。"));
	ChildSlot [ SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8) [ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("初次烘焙 → 创建衣物动画修整副本 → 在动画编辑器中修改骨骼关键帧并应用、保存 → 读取并预览 → 更新代理轨迹。绿色线框显示本次回读的代理。"))) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0) [ SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6, 4))
			+ SWrapBox::Slot() [ SNew(SButton).Text(FText::FromString(TEXT("载入所选代理"))).OnClicked(this, &SKawaiiClothingReadbackPanel::UseSelected) ]
			+ SWrapBox::Slot() [ SNew(SButton).Text(FText::FromString(TEXT("创建修整副本并打开"))).OnClicked(this, &SKawaiiClothingReadbackPanel::DuplicateForEditing) ]
			+ SWrapBox::Slot() [ SNew(SButton).Text(FText::FromString(TEXT("打开衣物动画"))).OnClicked(this, &SKawaiiClothingReadbackPanel::Edit) ]
			+ SWrapBox::Slot() [ SNew(SButton).Text(FText::FromString(TEXT("读取并预览"))).OnClicked(this, &SKawaiiClothingReadbackPanel::Read) ]
			+ SWrapBox::Slot() [ SNew(SButton).Text(FText::FromString(TEXT("更新代理轨迹"))).IsEnabled_Lambda([this] { return bReady; }).OnClicked(this, &SKawaiiClothingReadbackPanel::Save) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8) [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return Status; }) ]
		+ SVerticalBox::Slot().FillHeight(1) [ SNew(SSplitter)
			+ SSplitter::Slot().Value(.7f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().FillHeight(1)
				[ SAssignNew(Viewport, SKawaiiClothingReadbackViewport, Preview.ToSharedRef()) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(8)
				[ MakePreviewControls() ]
			]
			+ SSplitter::Slot().Value(.3f) [ Details.ToSharedRef() ]
		]
	];
}

SKawaiiClothingReadbackPanel::~SKawaiiClothingReadbackPanel() { FCoreUObjectDelegates::OnObjectModified.Remove(ModifiedHandle); }
void SKawaiiClothingReadbackPanel::FillFromMotion() {
	if (Settings->Motion) {
		Settings->EditedAnimation = Settings->Motion->BakedAnimation.LoadSynchronous();
		if (auto* Body = Settings->Motion->BodyBakedAnimation.LoadSynchronous(); Body && (!Settings->BodyMesh || Settings->BodyMesh->GetSkeleton() != Body->GetSkeleton())) { Settings->BodyMesh = Body->GetPreviewMesh(); }
	}
}
void SKawaiiClothingReadbackPanel::InvalidateRead() { bReady = bPlaying = false; PlayAccumulator = 0.; if (Preview) { Preview->Clear(); } }
void SKawaiiClothingReadbackPanel::SettingsChanged(const FPropertyChangedEvent& Event) {
	InvalidateRead();
	if (Event.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UKawaiiClothingReadbackSettings, Motion)) { FillFromMotion(); Details->ForceRefresh(); }
	Status = FText::FromString(TEXT("设置已改变，请重新读取并预览。"));
}
void SKawaiiClothingReadbackPanel::ObjectModified(UObject* Object) {
	if (!bReady || bPublishing || !Object) { return; }
	const auto* Motion = Settings->Motion.Get();
	const UObject* Inputs[] = {Settings->EditedAnimation, Settings->BodyMesh, Settings->CorrectionProfile, Motion, Motion ? Motion->Definition.Get() : nullptr, Motion ? Motion->SourceMesh.Get() : nullptr, Motion ? Motion->BodyBakedAnimation.Get() : nullptr};
	for (const UObject* Input : Inputs) {
		if (Input && (Object == Input || Object->IsIn(Input))) { InvalidateRead(); Status = FText::FromString(TEXT("预览输入正在修改，请保存动画后重新读取。")); return; }
	}
}
void SKawaiiClothingReadbackPanel::Tick(const FGeometry& Geometry, double Time, float Delta) {
	SCompoundWidget::Tick(Geometry, Time, Delta);
	if (bReady && Time - LastValidation > .5) {
		LastValidation = Time; FString Error;
		if (!Preview->IsCurrent(Error)) { InvalidateRead(); Status = FText::FromString(Error); }
	}
	if (bReady && bPlaying) {
		PlayAccumulator += Delta * Preview->GetRate();
		const int32 Steps = FMath::FloorToInt(PlayAccumulator);
		if (Steps > 0) {
			PlayAccumulator -= Steps;
			const int32 NextFrame = Preview->GetFrame() + Steps;
			if (!bLoopPreview && NextFrame >= Preview->NumFrames() - 1) {
				SeekFrame(Preview->NumFrames() - 1);
			} else {
				Preview->ShowFrame(NextFrame % Preview->NumFrames());
			}
		}
	}
	Viewport->Invalidate();
}
FReply SKawaiiClothingReadbackPanel::Read() {
	InvalidateRead(); FString Error;
	if (Preview->Read(*Settings.Get(), Error)) { bReady = true; Viewport->Focus(); Status = FText::FromString(FString::Printf(TEXT("已读取 %d 帧。可播放或拖动时间轴检查身体、修整衣物及绿色代理；确认后点击“更新代理轨迹”。%s"), Preview->NumFrames(), Settings->CorrectionProfile ? TEXT("已启用头发运行时修正预览。") : TEXT("未启用头发修正预览，可在右侧选择相同的运行时修正配置。"))); }
	else { Status = FText::FromString(Error); }
	return FReply::Handled();
}
FReply SKawaiiClothingReadbackPanel::Save() {
	TGuardValue<bool> Guard(bPublishing, true); FString Error, Backup;
	Status = Preview->Publish(Backup, Error) ? FText::FromString(TEXT("代理轨迹已更新，现有动作集合继续使用该资产。修整前代理备份：") + Backup) : FText::FromString(Error);
	return FReply::Handled();
}
FReply SKawaiiClothingReadbackPanel::Edit() {
	if (Settings->EditedAnimation) { GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Settings->EditedAnimation); }
	else { Status = FText::FromString(TEXT("请先选择原代理和衣物动画。")); }
	return FReply::Handled();
}
FReply SKawaiiClothingReadbackPanel::DuplicateForEditing() {
	if (!Settings->EditedAnimation) { return Edit(); }
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	FString Package, Name; Tools.CreateUniqueAssetName(Settings->EditedAnimation->GetPackage()->GetName(), TEXT("_Edit"), Package, Name);
	if (auto* Copy = Cast<UAnimSequence>(Tools.DuplicateAsset(Name, FPackageName::GetLongPackagePath(Package), Settings->EditedAnimation))) {
		InvalidateRead(); Settings->EditedAnimation = Copy; Details->ForceRefresh(); Edit();
		Status = FText::FromString(TEXT("已创建修整副本。修改骨骼关键帧并应用、保存后，回到此窗口读取预览。"));
	}
	return FReply::Handled();
}
FReply SKawaiiClothingReadbackPanel::UseSelected() {
	if (auto* Motion = SelectedMotion()) { InvalidateRead(); Settings->Motion = Motion; FillFromMotion(); Details->ForceRefresh(); }
	else { Status = FText::FromString(TEXT("请在内容浏览器选择衣物 ProxyMotion，或带有代理关联的烘焙衣物动画。")); }
	return FReply::Handled();
}

float SKawaiiClothingReadbackPanel::TimelineDuration() const {
	return float(FMath::Max(0, Preview->NumFrames() - 1)) / Preview->GetRate();
}

float SKawaiiClothingReadbackPanel::TimelinePosition() const {
	return float(Preview->GetFrame()) / Preview->GetRate();
}

FText SKawaiiClothingReadbackPanel::TimelineText() const {
	return FText::FromString(FString::Printf(
		TEXT("%.3f / %.3f 秒   |   第 %d / %d 帧   |   已缓存 %d / %d 帧"),
		TimelinePosition(), TimelineDuration(), Preview->GetFrame(), FMath::Max(0, Preview->NumFrames() - 1),
		Preview->NumFrames(), Preview->NumFrames()));
}

void SKawaiiClothingReadbackPanel::SeekFrame(int32 Frame) {
	bPlaying = false;
	PlayAccumulator = 0.;
	if (bReady) { Preview->ShowFrame(Frame); Viewport->Invalidate(); }
}

void SKawaiiClothingReadbackPanel::ScrubTime(float Time) {
	SeekFrame(FMath::RoundToInt(FMath::Clamp(Time, 0.f, TimelineDuration()) * Preview->GetRate()));
}

FReply SKawaiiClothingReadbackPanel::StepFrame(int32 Direction) {
	SeekFrame(Preview->GetFrame() + Direction);
	return FReply::Handled();
}

FReply SKawaiiClothingReadbackPanel::TogglePlayback() {
	if (bReady) {
		if (!bPlaying && Preview->GetFrame() >= Preview->NumFrames() - 1) { SeekFrame(0); }
		bPlaying = !bPlaying;
		PlayAccumulator = 0.;
	}
	return FReply::Handled();
}

TSharedRef<SWidget> SKawaiiClothingReadbackPanel::MakePreviewControls() {
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[ SNew(STextBlock).AutoWrapText(true).Text(this, &SKawaiiClothingReadbackPanel::TimelineText) ]
		+ SVerticalBox::Slot().AutoHeight()
		[
			// Match the bake preview's Persona timeline, including rendering during mouse capture.
			SNew(SScrubWidget)
			.IsEnabled_Lambda([this] { return bReady; })
			.Value(this, &SKawaiiClothingReadbackPanel::TimelinePosition)
			.NumOfKeys_Lambda([this] { return static_cast<uint32>(Preview->NumFrames()); })
			.SequenceLength(this, &SKawaiiClothingReadbackPanel::TimelineDuration)
			.ViewInputMin(0.f)
			.ViewInputMax_Lambda([this] { return FMath::Max(TimelineDuration(), UE_SMALL_NUMBER); })
			.DisplayDrag(true)
			.bAllowZoom(false)
			.bDisplayAnimScrubBarEditing(false)
			.OnBeginSliderMovement_Lambda([this] { bPlaying = false; PlayAccumulator = 0.; })
			.OnValueChanged(this, &SKawaiiClothingReadbackPanel::ScrubTime)
			.ToolTipText(FText::FromString(TEXT("拖动以暂停并查看指定帧。回读缓存立即显示身体、衣物和代理轨迹。")))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(5.f, 4.f))
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(FText::FromString(TEXT("|<"))).ToolTipText(FText::FromString(TEXT("跳到首帧")))
				.IsEnabled_Lambda([this] { return bReady; })
				.OnClicked_Lambda([this] { SeekFrame(0); return FReply::Handled(); })
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(FText::FromString(TEXT("<"))).ToolTipText(FText::FromString(TEXT("上一帧")))
				.IsEnabled_Lambda([this] { return bReady; })
				.OnClicked(this, &SKawaiiClothingReadbackPanel::StepFrame, -1)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text_Lambda([this] { return FText::FromString(bPlaying ? TEXT("暂停") : TEXT("播放")); })
				.IsEnabled_Lambda([this] { return bReady; })
				.OnClicked(this, &SKawaiiClothingReadbackPanel::TogglePlayback)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(FText::FromString(TEXT(">"))).ToolTipText(FText::FromString(TEXT("下一帧")))
				.IsEnabled_Lambda([this] { return bReady; })
				.OnClicked(this, &SKawaiiClothingReadbackPanel::StepFrame, 1)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(FText::FromString(TEXT(">|"))).ToolTipText(FText::FromString(TEXT("跳到末帧")))
				.IsEnabled_Lambda([this] { return bReady; })
				.OnClicked_Lambda([this] { SeekFrame(Preview->NumFrames() - 1); return FReply::Handled(); })
			]
			+ SWrapBox::Slot()
			[
				SNew(SCheckBox).IsChecked_Lambda([this] { return bLoopPreview ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bLoopPreview = State == ECheckBoxState::Checked; })
				[ SNew(STextBlock).Text(FText::FromString(TEXT("循环"))) ]
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(FText::FromString(TEXT("聚焦角色")))
				.OnClicked_Lambda([this] { Viewport->Focus(); return FReply::Handled(); })
			]
			+ SWrapBox::Slot()
			[
				SNew(SCheckBox).IsChecked(ECheckBoxState::Checked)
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Viewport->ShowProxy(State == ECheckBoxState::Checked); })
				[ SNew(STextBlock).Text(FText::FromString(TEXT("显示代理"))) ]
			]
		];
}
