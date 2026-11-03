#include "SKawaiiPhysicsBakePanel.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "ContentBrowserModule.h"
#include "EditorViewportClient.h"
#include "IContentBrowserSingleton.h"
#include "IDetailsView.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "PropertyEditorModule.h"
#include "SEditorViewport.h"
#include "SScrubWidget.h"
#include "ScopedTransaction.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeSession.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakePanel"

class SKawaiiPhysicsBakeViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SKawaiiPhysicsBakeViewport) {}
	SLATE_END_ARGS()
	void Construct(const FArguments&, TSharedRef<FKawaiiPhysicsBakeSession> InSession) {
		Session = InSession;
		SEditorViewport::Construct(SEditorViewport::FArguments());
	}

	virtual ~SKawaiiPhysicsBakeViewport() override {
		if (ViewportClient) {
			ViewportClient->Viewport = nullptr;
		}
	}

	void FrameMesh() {
		if (USkeletalMeshComponent* Mesh = Session->GetMainComponent()) {
			ViewportClient->FocusViewportOnBox(Mesh->Bounds.GetBox(), true);
			PreviousRoot = RootLocation(Mesh);
			bFollowRoot = true;
		}
	}

	void FollowRoot() {
		if (USkeletalMeshComponent* Mesh = Session->GetMainComponent(); Mesh && bFollowRoot) {
			const FVector Root = RootLocation(Mesh);
			const FVector Delta = Root - PreviousRoot;
			ViewportClient->SetViewLocation(ViewportClient->GetViewLocation() + Delta);
			ViewportClient->SetLookAtLocation(ViewportClient->GetLookAtLocation() + Delta);
			PreviousRoot = Root;
		}
	}

protected:
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override {
		ViewportClient = MakeShared<FEditorViewportClient>(nullptr, Session->GetPreviewScene(), SharedThis(this));
		ViewportClient->SetViewMode(VMI_Lit);
		ViewportClient->SetViewportType(LVT_Perspective);
		ViewportClient->SetViewLocation(FVector(250, 250, 140));
		ViewportClient->SetViewRotation(FRotator(-12, -135, 0));
		ViewportClient->SetRealtime(true);
		ViewportClient->EngineShowFlags.SetGrid(false);
		ViewportClient->EngineShowFlags.SetSelectionOutline(false);
		return ViewportClient.ToSharedRef();
	}

	virtual TSharedPtr<SWidget> MakeViewportToolbar() override { return nullptr; }

private:
	static FVector RootLocation(const USkeletalMeshComponent* Mesh) {
		const TArray<FTransform>& Pose = Mesh->GetComponentSpaceTransforms();
		return Pose.IsEmpty() ? Mesh->GetComponentLocation() : Mesh->GetComponentTransform().TransformPosition(Pose[0].GetTranslation());
	}

private:
	FVector PreviousRoot = FVector::ZeroVector;
	bool bFollowRoot = false;
	TSharedPtr<FKawaiiPhysicsBakeSession> Session;
	TSharedPtr<FEditorViewportClient> ViewportClient;
};

void SKawaiiPhysicsBakePanel::Construct(const FArguments&, UAnimBlueprint* InBlueprint) {
	Blueprint = InBlueprint;
	Session = MakeShared<FKawaiiPhysicsBakeSession>();
	FString Error;
	if (!Session->Initialize(InBlueprint, Error)) {
		ErrorText = FText::FromString(Error);
	} else {
		bRestartPreview = true;
	}
	Session->SetPreviewLooping(bLoopPreview);
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Details = PropertyEditor.CreateDetailView(DetailsArgs);
	Details->SetObject(KawaiiPhysicsBake::GetSettings(InBlueprint));
	Details->OnFinishedChangingProperties().AddSP(this, &SKawaiiPhysicsBakePanel::SettingsChanged);

	FAssetPickerConfig Picker;
	Picker.Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	Picker.InitialAssetViewType = EAssetViewType::List;
	Picker.SelectionMode = ESelectionMode::Single;
	Picker.OnAssetDoubleClicked = FOnAssetDoubleClicked::CreateSP(this, &SKawaiiPhysicsBakePanel::SelectAnimation);
	Picker.OnShouldFilterAsset = FOnShouldFilterAsset::CreateLambda([WeakBP = Blueprint](const FAssetData& Asset) {
		if (!WeakBP.IsValid() || !WeakBP->TargetSkeleton) {
			return false;
		}
		FString SkeletonTag;
		return Asset.GetTagValue(TEXT("Skeleton"), SkeletonTag) && SkeletonTag != FAssetData(WeakBP->TargetSkeleton).GetExportTextName();
	});
	FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT("Help", "选择源动画即可预览完整角色。拖动时间轴或逐帧查看物理效果，确认后再导出。修改动画图后请点击“编译并预览”。"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0)
		[
			SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(6.f, 4.f))
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text(LOCTEXT("Play", "编译并预览"))
				.IsEnabled_Lambda([this] { return !Session->IsBaking(); })
				.OnClicked(this, &SKawaiiPhysicsBakePanel::Play)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text(LOCTEXT("Reset", "重置"))
				.IsEnabled_Lambda([this] { return !Session->IsBaking(); })
				.OnClicked(this, &SKawaiiPhysicsBakePanel::Reset)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text(LOCTEXT("Bake", "烘焙新动画"))
				.IsEnabled_Lambda([this] { return !Session->IsBaking(); })
				.OnClicked(this, &SKawaiiPhysicsBakePanel::Bake)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "停止／取消"))
				.IsEnabled_Lambda([this] { return Session->IsRunning(); })
				.OnClicked(this, &SKawaiiPhysicsBakePanel::Cancel)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text(LOCTEXT("Results", "定位结果"))
				.IsEnabled_Lambda([this] { return !Session->GetOutputAssets().IsEmpty(); })
				.OnClicked(this, &SKawaiiPhysicsBakePanel::BrowseResults)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8)
		[
			SNew(STextBlock).AutoWrapText(true).Text(this, &SKawaiiPhysicsBakePanel::StatusText)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 6)
		[
			SNew(SProgressBar).Percent_Lambda([this]() -> TOptional<float> { return Session->GetProgress(); })
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SSplitter)
			+ SSplitter::Slot().Value(0.62f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(8, 4)
				[
					SNew(STextBlock).Text(LOCTEXT("PreviewTitle", "物理预览"))
				]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					SAssignNew(Preview, SKawaiiPhysicsBakeViewport, Session.ToSharedRef())
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(8)
				[
					MakePreviewControls()
				]
			]
			+ SSplitter::Slot().Value(0.38f)
			[
				SNew(SSplitter).Orientation(Orient_Vertical)
				+ SSplitter::Slot().Value(0.68f)
				[
					SNew(SBox).IsEnabled_Lambda([this] { return !Session->IsBaking(); })
					[
						Details.ToSharedRef()
					]
				]
				+ SSplitter::Slot().Value(0.32f)
				[
					SNew(SBox).IsEnabled_Lambda([this] { return !Session->IsBaking(); })
					[
						ContentBrowser.Get().CreateAssetPicker(Picker)
					]
				]
			]
		]
	];

	// An editor ticker keeps a bake progressing while its graph document is selected.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &SKawaiiPhysicsBakePanel::TickSession));
	Preview->FrameMesh();
}

SKawaiiPhysicsBakePanel::~SKawaiiPhysicsBakePanel() {
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	if (Session) {
		Session->Cancel();
	}
}

bool SKawaiiPhysicsBakePanel::TickSession(float DeltaTime) {
	if (!Blueprint.IsValid()) {
		Session->Cancel();
		return false;
	}
	if (bRestartPreview && !Session->IsBaking()) {
		bRestartPreview = false;
		Play();
	}
	Session->Tick(DeltaTime);
	Preview->FollowRoot();
	Preview->Invalidate();
	return true;
}

bool SKawaiiPhysicsBakePanel::CompileGraphs() {
	Session->Cancel();
	ErrorText = FText::GetEmpty();
	if (!Blueprint.IsValid() || !KawaiiPhysicsBake::GetSettings(Blueprint.Get())) {
		return false;
	}
	TSet<UAnimBlueprint*> Graphs;
	Graphs.Add(Blueprint.Get());
	for (const FKawaiiPhysicsBakePart& Part : KawaiiPhysicsBake::GetSettings(Blueprint.Get())->Parts) {
		if (Part.Graph) {
			Graphs.Add(Part.Graph);
		}
	}
	for (UAnimBlueprint* Graph : Graphs) {
		FKismetEditorUtilities::CompileBlueprint(Graph);
		if (Graph->Status == BS_Error || !Graph->GeneratedClass) {
			ErrorText = FText::Format(LOCTEXT("CompileError", "请先修复 {0} 的编译错误，再进行预览或烘焙。"), FText::FromString(Graph->GetName()));
			return false;
		}
	}
	return true;
}

FReply SKawaiiPhysicsBakePanel::Play() {
	bRestartPreview = false;
	FString Error;
	if (CompileGraphs()) {
		if (!Session->StartPreview(Error)) {
			ErrorText = FText::FromString(Error);
		} else {
			Preview->FrameMesh();
		}
	}
	return FReply::Handled();
}

FReply SKawaiiPhysicsBakePanel::Reset() {
	bRestartPreview = false;
	FString Error;
	ErrorText = FText::GetEmpty();
	if (!Session->ResetPreview(Error)) {
		ErrorText = FText::FromString(Error);
	} else {
		Preview->FrameMesh();
	}
	return FReply::Handled();
}

FReply SKawaiiPhysicsBakePanel::Bake() {
	bRestartPreview = false;
	FString Error;
	if (CompileGraphs()) {
		if (!Session->StartBake(Error)) {
			ErrorText = FText::FromString(Error);
		} else {
			Preview->FrameMesh();
		}
	}
	return FReply::Handled();
}

FReply SKawaiiPhysicsBakePanel::Cancel() {
	bRestartPreview = false;
	Session->Cancel();
	ErrorText = FText::GetEmpty();
	return FReply::Handled();
}

FReply SKawaiiPhysicsBakePanel::BrowseResults() {
	TArray<UObject*> Assets;
	for (const TWeakObjectPtr<UAnimSequence>& Asset : Session->GetOutputAssets()) {
		if (Asset.IsValid()) {
			Assets.Add(Asset.Get());
		}
	}
	FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
	return FReply::Handled();
}

void SKawaiiPhysicsBakePanel::SelectAnimation(const FAssetData& Asset) {
	if (!Blueprint.IsValid() || Session->IsBaking()) {
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("Select", "选择烘焙动画"));
	KawaiiPhysicsBake::GetSettings(Blueprint.Get())->Modify();
	KawaiiPhysicsBake::GetSettings(Blueprint.Get())->SourceAnimation = Cast<UAnimSequence>(Asset.GetAsset());
	Blueprint->MarkPackageDirty();
	Details->ForceRefresh();
	Play();
}

void SKawaiiPhysicsBakePanel::SettingsChanged(const FPropertyChangedEvent& Event) {
	const FName Property = Event.GetMemberPropertyName();
	if (Property == GET_MEMBER_NAME_CHECKED(UKawaiiPhysicsBakeSettings, OutputDirectory)
		|| Property == GET_MEMBER_NAME_CHECKED(UKawaiiPhysicsBakeSettings, OutputPrefix))
	{
		if (Blueprint.IsValid()) {
			Blueprint->MarkPackageDirty();
		}
		return;
	}
	Session->Cancel();
	bRestartPreview = true;
	ErrorText = FText::GetEmpty();
	if (Blueprint.IsValid()) {
		Blueprint->MarkPackageDirty();
	}
}

FText SKawaiiPhysicsBakePanel::StatusText() const {
	return ErrorText.IsEmpty() ? Session->GetStatus() : ErrorText;
}

FReply SKawaiiPhysicsBakePanel::TogglePlayback() {
	if (!Session->CanScrubPreview()) {
		return Play();
	}
	Session->SetPreviewPaused(!Session->IsPreviewPaused());
	return FReply::Handled();
}

FReply SKawaiiPhysicsBakePanel::StepFrame(int32 Direction) {
	Session->SeekPreview(Session->GetRequestedFrame() + Direction);
	Preview->FollowRoot();
	Preview->Invalidate();
	return FReply::Handled();
}

void SKawaiiPhysicsBakePanel::ScrubTimeline(float Value) {
	Session->SeekPreview(FMath::RoundToInt(Value * Session->GetLastFrame()));
	Preview->FollowRoot();
	Preview->Invalidate();
}

void SKawaiiPhysicsBakePanel::ScrubTime(float Time) {
	ScrubTimeline(Session->GetDuration() > 0.f ? Time / Session->GetDuration() : 0.f);
}

float SKawaiiPhysicsBakePanel::TimelinePosition() const {
	return Session->GetLastFrame() > 0 ? Session->GetDuration() * Session->GetRequestedFrame() / Session->GetLastFrame() : 0.f;
}

FText SKawaiiPhysicsBakePanel::TimelineText() const {
	const FString Timeline = FString::Printf(
		TEXT("%.3f / %.3f 秒   |   第 %d / %d 帧   |   已缓存 %d / %d 帧"),
		Session->GetPreviewTime(),
		Session->GetDuration(),
		Session->GetPreviewFrame(),
		Session->GetLastFrame(),
		Session->CanScrubPreview() ? Session->GetCachedFrameCount() : 0,
		Session->GetLastFrame() + 1
	);
	return FText::FromString(Timeline);
}

TSharedRef<SWidget> SKawaiiPhysicsBakePanel::MakePreviewControls() {
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock).AutoWrapText(true).Text(this, &SKawaiiPhysicsBakePanel::TimelineText)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			// Persona's scrub widget keeps viewport rendering active during mouse capture.
			SNew(SScrubWidget)
			.IsEnabled_Lambda([this] { return Session->CanScrubPreview(); })
			.Value(this, &SKawaiiPhysicsBakePanel::TimelinePosition)
			.NumOfKeys_Lambda([this] { return static_cast<uint32>(Session->GetLastFrame() + 1); })
			.SequenceLength_Lambda([this] { return Session->GetDuration(); })
			.ViewInputMin(0.f)
			.ViewInputMax_Lambda([this] { return FMath::Max(Session->GetDuration(), UE_SMALL_NUMBER); })
			.DisplayDrag(true)
			.bAllowZoom(false)
			.bDisplayAnimScrubBarEditing(false)
			.OnBeginSliderMovement_Lambda([this] { Session->SetPreviewPaused(true); })
			.OnValueChanged(this, &SKawaiiPhysicsBakePanel::ScrubTime)
			.ToolTipText(LOCTEXT("ScrubHelp", "拖动以暂停并查看指定帧。已缓存的帧立即显示，未缓存的帧会按顺序补算物理。"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(5.f, 4.f))
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(LOCTEXT("FirstFrame", "|<"))
				.ToolTipText(LOCTEXT("FirstFrameHelp", "跳到首帧"))
				.IsEnabled_Lambda([this] { return Session->CanScrubPreview(); })
				.OnClicked_Lambda([this] {
					ScrubTimeline(0.f);
					return FReply::Handled();
				})
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(LOCTEXT("PreviousFrame", "<"))
				.ToolTipText(LOCTEXT("PreviousFrameHelp", "上一帧"))
				.IsEnabled_Lambda([this] { return Session->CanScrubPreview(); })
				.OnClicked(this, &SKawaiiPhysicsBakePanel::StepFrame, -1)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton)
				.Text_Lambda([this] {
					return Session->CanScrubPreview() && !Session->IsPreviewPaused() ? LOCTEXT("PausePreview", "暂停") : LOCTEXT("PlayPreview", "播放");
				})
				.IsEnabled_Lambda([this] { return !Session->IsBaking(); })
				.OnClicked(this, &SKawaiiPhysicsBakePanel::TogglePlayback)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(LOCTEXT("NextFrame", ">"))
				.ToolTipText(LOCTEXT("NextFrameHelp", "下一帧"))
				.IsEnabled_Lambda([this] { return Session->CanScrubPreview(); })
				.OnClicked(this, &SKawaiiPhysicsBakePanel::StepFrame, 1)
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(LOCTEXT("LastFrame", ">|"))
				.ToolTipText(LOCTEXT("LastFrameHelp", "跳到末帧；尚未缓存时先补算剩余物理帧"))
				.IsEnabled_Lambda([this] { return Session->CanScrubPreview(); })
				.OnClicked_Lambda([this] {
					ScrubTimeline(1.f);
					return FReply::Handled();
				})
			]
			+ SWrapBox::Slot()
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return bLoopPreview ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) {
					bLoopPreview = State == ECheckBoxState::Checked;
					Session->SetPreviewLooping(bLoopPreview);
				})
				[
					SNew(STextBlock).Text(LOCTEXT("LoopPreview", "循环"))
				]
			]
			+ SWrapBox::Slot()
			[
				SNew(SButton).Text(LOCTEXT("FocusPreview", "聚焦角色"))
				.OnClicked_Lambda([this] {
					Preview->FrameMesh();
					return FReply::Handled();
				})
			]
		];
}

#undef LOCTEXT_NAMESPACE
