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
			.Text_Lambda([this] {
				return Settings->bSearchNodeSettings
					? LOCTEXT("NodeSearchHelp", "节点参数搜索：在设置的范围与骨骼配对候选方案内，用原生 Kawaii CPU 模拟逐一评估 Physics Settings 和 Bone Constraints。按顺序添加测试动作，并用独立“仅用于验证”片段验收。本次有限候选的最佳结果不保证全局最优。保存结果后，点击“应用到蓝图”会一并写回选中的节点参数；未达标候选仅供试用，仍可能穿透。")
					: LOCTEXT("LegacySearchHelp", "旧 GPU 倍率搜索：GPU 先筛选阻尼、刚度、骨骼半径倍率与碰撞迭代候选，再由原生 Kawaii CPU 模拟复验。关闭“播放前重置”可测试动画切换；将独立片段标为“仅用于验证”。本次有限候选的最佳结果不保证全局最优。开启“搜索节点参数”可搜索节点本身的 Physics Settings 和 Bone Constraints。");
			})
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
				.Text_Lambda([this] {
					return Settings->bSearchNodeSettings ? LOCTEXT("StartNodeSearch", "开始节点参数搜索") : LOCTEXT("StartLegacySearch", "开始旧 GPU 搜索");
				})
				.ToolTipText_Lambda([this] {
					return Settings->bSearchNodeSettings
						? LOCTEXT("StartNodeSearchHelp", "按 CPU 搜索候选数生成有限参数组合，并对全部候选运行原生 Kawaii 模拟与验收。搜索期间不修改源蓝图；范围和候选方案由下方设置控制。")
						: LOCTEXT("StartLegacySearchHelp", "使用旧版 GPU 倍率筛选与 CPU 复验流程，需要支持 SM5 计算着色器的图形接口。");
				})
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
				.Text_Lambda([this] {
					return Session->HasUnacceptedResult() ? LOCTEXT("SaveUnaccepted", "保存最佳候选（未达标）") : LOCTEXT("Save", "保存配置");
				})
				.ToolTipText_Lambda([this] {
					return Session->HasUnacceptedResult()
						? LOCTEXT("SaveUnacceptedHelp", "保存当前未通过验收的结果供试用。它仍可能穿透或拉伸；保存和应用均不会将其标记为验收通过。手动配置没有经过搜索验收。")
						: LOCTEXT("SaveHelp", "将搜索结果保存为新配置资产。节点参数搜索结果同时保存获选的节点参数方案，随后点击“应用到蓝图”写回。");
				})
				.IsEnabled_Lambda([this] { return !Session->IsRunning() && Session->GetResult(); })
				.OnClicked_Lambda([this] {
					Message.Reset();
					if (Session->SaveResult(Message, Session->HasUnacceptedResult())) {
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
				.ToolTipText_Lambda([this] {
					return Session->HasUnacceptedResult()
						? LOCTEXT("ApplyUnacceptedHelp", "将已保存的未通过验收结果写回所选节点供试用，并绑定配置、编译蓝图。结果可能仍有穿透或骨长误差，必须检查实际动作；应用不会将它标记为验收通过。操作可撤销，检查后需保存蓝图。")
						: LOCTEXT("ApplyHelp", "将已保存结果绑定到所选 Kawaii 节点；含节点参数方案时，一并写回获选的 Physics Settings 和 Bone Constraints 并编译蓝图。直接把配置拖入节点槽不会自动写回这些参数。应用可撤销，检查后需保存蓝图。");
				})
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
					if (Session->CreateManualProfile(Settings.Get(), Message) && Session->SaveResult(Message, true)) {
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
