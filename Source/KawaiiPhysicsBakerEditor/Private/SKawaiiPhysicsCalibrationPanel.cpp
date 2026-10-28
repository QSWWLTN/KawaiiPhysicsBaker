#include "SKawaiiPhysicsCalibrationPanel.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/SavePackage.h"
#include "SAssetDropTarget.h"

#include "KawaiiPhysicsCalibrationProfile.h"

#include "KawaiiPhysicsCalibrationSession.h"

#define LOCTEXT_NAMESPACE "KawaiiCalibrationPanel"

void SKawaiiPhysicsCalibrationPanel::Construct(const FArguments&, UAnimBlueprint* Blueprint) {
	Settings.Reset(NewObject<UKawaiiPhysicsCalibrationSettings>());
	Settings->ClothingBlueprint = Blueprint;
	if (Blueprint) {
		Settings->ClothingMesh = Blueprint->GetPreviewMesh();
	}
	Session = MakeShared<FKawaiiPhysicsCalibrationSession>();
	FDetailsViewArgs Args;
	Args.bHideSelectionTip = true;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Details = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor").CreateDetailView(Args);
	Details->SetObject(Settings.Get());
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(10)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(LOCTEXT("Help", "选择人物与衣物网格体及衣物 Kawaii 节点，按播放顺序添加多个测试动作。关闭“播放前重置”可测试动画切换；将独立片段标为“仅用于验证”。GPU 先筛选候选参数，再由原始 Kawaii CPU 模拟复验。输出为可复用的物理配置，不会覆盖现有动画资产。"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(10, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(3)
			[
				SNew(SButton)
				.Text(LOCTEXT("SaveJob", "保存测试集"))
				.IsEnabled_Lambda([this] { return !Session->IsRunning(); })
				.OnClicked(this, &SKawaiiPhysicsCalibrationPanel::SaveJob)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(3)
			[
				SNew(SButton)
				.Text(LOCTEXT("LoadJob", "载入所选测试集"))
				.IsEnabled_Lambda([this] { return !Session->IsRunning(); })
				.OnClicked(this, &SKawaiiPhysicsCalibrationPanel::LoadJob)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(3)
			[
				SNew(SButton)
				.Text(LOCTEXT("Start", "开始 GPU 校准"))
				.IsEnabled_Lambda([this] { return !Session->IsRunning(); })
				.OnClicked_Lambda([this] {
					Message.Reset();
					Session->Start(Settings.Get(), Message);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(3)
			[
				SNew(SButton)
				.Text(LOCTEXT("Cancel", "取消"))
				.IsEnabled_Lambda([this] { return Session->IsRunning(); })
				.OnClicked_Lambda([this] {
					Session->Cancel();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(3)
			[
				SNew(SButton)
				.Text(LOCTEXT("Save", "保存配置"))
				.IsEnabled_Lambda([this] { return !Session->IsRunning() && Session->GetResult(); })
				.OnClicked_Lambda([this] {
					Message.Reset();
					if (Session->SaveResult(Message)) {
						TArray<UObject*> Assets{Session->GetResult()};
						FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(3)
			[
				SNew(SButton)
				.Text(LOCTEXT("Apply", "应用到蓝图"))
				.IsEnabled_Lambda([this] { return !Session->IsRunning() && Session->GetResult(); })
				.OnClicked_Lambda([this] {
					Message.Reset();
					Session->ApplyToBlueprint(Message);
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(10, 0)
		[
			SNew(SHorizontalBox)
			.IsEnabled_Lambda([this] { return !Session->IsRunning(); })
			+ SHorizontalBox::Slot().AutoWidth().Padding(3)
			[
				SNew(SButton)
				.Text(LOCTEXT("ReadConstraints", "读取碰撞与骨链"))
				.ToolTipText(LOCTEXT("ReadConstraintsHelp", "用所选节点原始设置重新填充编辑列表，会替换当前列表中的手动调整。不会修改源蓝图。"))
				.OnClicked_Lambda([this] {
					Settings->ReadCollisionAndChains(Message);
					Details->ForceRefresh();
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(3)
			[
				SNew(SButton)
				.Text(LOCTEXT("SaveManual", "保存手动配置（未验收）"))
				.ToolTipText(LOCTEXT("SaveManualHelp", "无需测试动作。保存当前碰撞与范围设置为新配置，再点击应用到蓝图进行游戏测试；不生成蒙皮接触点，也不表示通过穿模验收。"))
				.OnClicked_Lambda([this] {
					Message.Reset();
					if (Session->CreateManualProfile(Settings.Get(), Message) && Session->SaveResult(Message)) {
						TArray<UObject*> Assets{Session->GetResult()};
						FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
						Message = TEXT("已保存手动配置，未经穿模验收。点击“应用到蓝图”后可进行游戏测试。更改参数后需要再次保存手动配置。");
					}
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(10)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([this] {
				return FText::FromString(Message.IsEmpty() ? Session->GetStatus() : Message);
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(10, 0)
		[
			SNew(SProgressBar)
			.Percent_Lambda([this]() -> TOptional<float> { return Session->GetProgress(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(10)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([this] { return FText::FromString(Session->GetReportPath()); })
		]
		+ SVerticalBox::Slot().FillHeight(1)
		[
			SNew(SBox)
			.IsEnabled_Lambda([this] { return !Session->IsRunning(); })
			[
				SNew(SAssetDropTarget)
				.bSupportsMultiDrop(true)
				.OnAreAssetsAcceptableForDrop(this, &SKawaiiPhysicsCalibrationPanel::CanDropAnimations)
				.OnAssetsDropped(this, &SKawaiiPhysicsCalibrationPanel::OnAnimationsDropped)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(10)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text(LOCTEXT("DropAnimations", "测试动作：将内容浏览器中的动画序列拖入此处或下方列表区域，支持多选，自动追加到列表末尾。"))
					]
					+ SVerticalBox::Slot().FillHeight(1)
					[
						Details.ToSharedRef()
					]
				]
			]
		]
	];

	Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float) {
		Session->Tick();
		return true;
	}));
}

SKawaiiPhysicsCalibrationPanel::~SKawaiiPhysicsCalibrationPanel() {
	FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
	Session->Cancel();
}

FReply SKawaiiPhysicsCalibrationPanel::SaveJob() {
	FString Directory = Settings->OutputDirectory.Path;
	Directory.RemoveFromEnd(TEXT("/"));
	if (!Directory.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Directory + TEXT("/Probe"))) {
		Message = TEXT("请选择有效的 /Game 输出子目录。");
		return FReply::Handled();
	}
	const FString Name = Directory / TEXT("DA_CalibrationTestSet_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	UPackage* Package = CreatePackage(*Name);
	auto* Asset = DuplicateObject<UKawaiiPhysicsCalibrationSettings>(Settings.Get(), Package, *FPackageName::GetLongPackageAssetName(Name));
	Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	const FString Filename = FPackageName::LongPackageNameToFilename(Name, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	Args.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Asset, *Filename, Args)) {
		Message = TEXT("无法保存测试集。");
		return FReply::Handled();
	}
	FAssetRegistryModule::AssetCreated(Asset);
	TArray<UObject*> Assets{Asset};
	FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().SyncBrowserToAssets(Assets);
	Message = TEXT("已保存仅供编辑器使用的测试集：") + Name;
	return FReply::Handled();
}

FReply SKawaiiPhysicsCalibrationPanel::LoadJob() {
	TArray<FAssetData> Assets;
	FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().GetSelectedAssets(Assets);
	if (Assets.Num() != 1 || !Assets[0].IsInstanceOf(UKawaiiPhysicsCalibrationSettings::StaticClass())) {
		Message = TEXT("请在内容浏览器中选择一个校准测试集资产。");
		return FReply::Handled();
	}
	Settings.Reset(DuplicateObject<UKawaiiPhysicsCalibrationSettings>(CastChecked<UKawaiiPhysicsCalibrationSettings>(Assets[0].GetAsset()), GetTransientPackage()));
	Details->SetObject(Settings.Get());
	Message = TEXT("已载入测试集的工作副本。保存时会创建新资产。");
	return FReply::Handled();
}

bool SKawaiiPhysicsCalibrationPanel::CanDropAnimations(TArrayView<FAssetData> Assets) const {
	if (Session->IsRunning()) {
		return false;
	}
	for (const FAssetData& Asset : Assets) {
		if (Asset.IsInstanceOf(UAnimSequence::StaticClass())) {
			return true;
		}
	}
	return false;
}

void SKawaiiPhysicsCalibrationPanel::OnAnimationsDropped(const FDragDropEvent&, TArrayView<FAssetData> Assets) {
	if (!CanDropAnimations(Assets)) {
		return;
	}
	int32 AddedCount = 0;
	for (const FAssetData& Asset : Assets) {
		if (!Asset.IsInstanceOf(UAnimSequence::StaticClass())) {
			continue;
		}
		auto* Animation = Cast<UAnimSequence>(Asset.GetAsset());
		if (!Animation || Animation->IsValidAdditive()
			|| (Settings->BodyMesh && Animation->GetSkeleton() != Settings->BodyMesh->GetSkeleton()))
		{
			continue;
		}
		FKawaiiCalibrationMotion& Motion = Settings->Motions.AddDefaulted_GetRef();
		Motion.Animation = Animation;
		++AddedCount;
	}
	if (AddedCount > 0) {
		Details->ForceRefresh();
	}
	Message = FString::Printf(TEXT("已追加 %d 个测试动作。每项可独立设置过渡、重置和验证选项。"), AddedCount);
	if (AddedCount < Assets.Num()) {
		Message += FString::Printf(TEXT(" 已跳过 %d 项：仅接受可加载、匹配人物骨架的非叠加动画序列。"), Assets.Num() - AddedCount);
	}
}

#undef LOCTEXT_NAMESPACE
