#include "KawaiiClothingProxyPlaybackUpgrade.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiClothingProxyPlayback.h"

namespace KawaiiClothingProxyUpgrade {
	bool ResolveBodyPlayback(const UKawaiiClothingProxyMotion& Motion, const TArray<FBodyPlaybackCandidate>& Candidates, FSoftObjectPath& OutAnimation, FString& Error) {
		OutAnimation.Reset();
		Error.Reset();
		if (!Motion.BakeGroup.IsValid() || Motion.SourceAnimation.IsNull()) {
			Error = TEXT("代理运动缺少有效的生成组或源动画，无法升级身体动作关联。");
			return false;
		}
		TSet<FSoftObjectPath> Matches;
		for (const FBodyPlaybackCandidate& Candidate : Candidates) {
			if (Candidate.Group == Motion.BakeGroup && Candidate.Part == FName(TEXT("Main")) && Candidate.Source == Motion.SourceAnimation.ToSoftObjectPath()
				&& !Candidate.Animation.IsNull())
			{
				Matches.Add(Candidate.Animation);
			}
		}
		if (Matches.Num() != 1) {
			Error = Matches.IsEmpty() ? TEXT("未找到同一生成组、同一源动作且部件为 Main 的烘焙动画。请检查代理运动所在目录及其子目录；动画已移至其他目录时可重烘以生成关联。") : TEXT("发现多个同组 Main 动画，关联存在歧义；请移除重复副本或重新烘焙，工具不会猜测。");
			return false;
		}
		OutAnimation = *Matches.CreateConstIterator();
		return true;
	}

	namespace {
		void GetSelection(TArray<FAssetData>& Out) {
			FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get().GetSelectedAssets(Out);
		}

		void Report(const FString& Message) {
			UE_LOG(LogTemp, Display, TEXT("Kawaii proxy playback upgrade: %s"), *Message);
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
		}
	}

	bool HasSelectedSets() {
		TArray<FAssetData> Selected;
		GetSelection(Selected);
		return Selected.ContainsByPredicate([](const FAssetData& Asset) { return Asset.IsInstanceOf(UKawaiiClothingProxyMotionSet::StaticClass()); });
	}

	void UpgradeSelectedSets() {
		check(IsInGameThread());
		TArray<FAssetData> Selected;
		GetSelection(Selected);
		TArray<TStrongObjectPtr<UKawaiiClothingProxyMotionSet>> Sets;
		TArray<TStrongObjectPtr<UKawaiiClothingProxyMotion>> Motions;
		TSet<UKawaiiClothingProxyMotion*> Seen;
		TSet<FName> SearchDirectories;
		int32 AlreadyLinked = 0;
		for (const FAssetData& Asset : Selected) {
			if (!Asset.IsInstanceOf(UKawaiiClothingProxyMotionSet::StaticClass())) {
				continue;
			}
			UKawaiiClothingProxyMotionSet* Set = Cast<UKawaiiClothingProxyMotionSet>(Asset.GetAsset());
			if (!Set) {
				Report(TEXT("选中的代理动作集合加载失败，未修改任何资产。"));
				return;
			}
			Sets.Emplace(Set);
			for (UKawaiiClothingProxyMotion* Motion : Set->Motions) {
				if (!IsValid(Motion)) {
					Report(FString::Printf(TEXT("%s 含有空代理运动，未修改任何资产。"), *Set->GetPathName()));
					return;
				}
				if (Seen.Contains(Motion)) {
					continue;
				}
				Seen.Add(Motion);
				if (!Motion->BodyBakedAnimation.IsNull()) {
					++AlreadyLinked;
					continue;
				}
				Motions.Emplace(Motion);
				const FString PackageName = Motion->GetOutermost()->GetName();
				if (!PackageName.StartsWith(TEXT("/Game/"))) {
					Report(FString::Printf(TEXT("%s 不在 /Game 中，无法自动升级，未修改任何资产。"), *PackageName));
					return;
				}
				SearchDirectories.Add(FName(*FPackageName::GetLongPackagePath(PackageName)));
			}
		}
		if (Sets.IsEmpty()) {
			Report(TEXT("请先在内容浏览器选择 Kawaii 衣物代理动作集合。"));
			return;
		}
		if (Motions.IsEmpty()) {
			Report(FString::Printf(TEXT("没有需要升级的代理运动；已有 %d 个身体动作关联。"), AlreadyLinked));
			return;
		}
		FARFilter Filter;
		Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		for (const FName Directory : SearchDirectories) {
			Filter.PackagePaths.Add(Directory);
		}
		TArray<FAssetData> Animations;
		FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		Registry.Get().GetAssets(Filter, Animations);
		FScopedSlowTask Progress(Animations.Num(), FText::FromString(TEXT("查找已烘焙 Main 动画的生成元数据…")));
		Progress.MakeDialog(true);
		TArray<FBodyPlaybackCandidate> Candidates;
		for (const FAssetData& Asset : Animations) {
			if (Progress.ShouldCancel()) {
				Report(TEXT("已取消动作关联升级，未修改任何资产。"));
				return;
			}
			Progress.EnterProgressFrame(1.f);
			UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset());
			if (!Sequence) {
				Report(FString::Printf(TEXT("候选动画 %s 加载失败，未修改任何资产。"), *Asset.GetSoftObjectPath().ToString()));
				return;
			}
			UMetaData* Meta = Sequence->GetOutermost()->GetMetaData();
			FBodyPlaybackCandidate Candidate;
			Candidate.Animation = FSoftObjectPath(Sequence);
			Candidate.Part = FName(*Meta->GetValue(Sequence, TEXT("KawaiiBake.Part")));
			Candidate.Source = FSoftObjectPath(Meta->GetValue(Sequence, TEXT("KawaiiBake.Source")));
			if (Candidate.Part == FName(TEXT("Main")) && FGuid::Parse(Meta->GetValue(Sequence, TEXT("KawaiiBake.Group")), Candidate.Group)) {
				Candidates.Add(MoveTemp(Candidate));
			}
		}
		// Resolve the entire selection before changing anything. Missing and ambiguous
		// mappings must not leave a half-upgraded list or silently select a different take.
		TArray<FSoftObjectPath> Resolved;
		for (const TStrongObjectPtr<UKawaiiClothingProxyMotion>& Motion : Motions) {
			FSoftObjectPath Playback;
			FString Error;
			if (!ResolveBodyPlayback(*Motion.Get(), Candidates, Playback, Error)) {
				Report(Motion->GetPathName() + TEXT("：") + Error + TEXT("\n未修改任何资产。"));
				return;
			}
			Resolved.Add(Playback);
		}
		const FScopedTransaction Transaction(FText::FromString(TEXT("升级衣物代理身体动作关联")));
		for (int32 Index = 0; Index < Motions.Num(); ++Index) {
			UKawaiiClothingProxyMotion* Motion = Motions[Index].Get();
			Motion->SetFlags(RF_Transactional);
			Motion->Modify();
			Motion->BodyBakedAnimation = TSoftObjectPtr<UAnimSequence>(Resolved[Index]);
			Motion->MarkPackageDirty();
		}
		int32 Saved = 0;
		TArray<FString> Failures;
		for (const TStrongObjectPtr<UKawaiiClothingProxyMotion>& Motion : Motions) {
			UPackage* Package = Motion->GetOutermost();
			const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			if (UPackage::SavePackage(Package, Motion.Get(), *Filename, Args)) {
				++Saved;
			} else {
				Failures.Add(Motion->GetPathName());
			}
		}
		FString Message = FString::Printf(
			TEXT("已补全 %d 个代理运动的身体动作关联，成功保存 %d 个；另有 %d 个已有关联保持不变。无需重新烘焙轨道。"),
			Motions.Num(),
			Saved,
			AlreadyLinked
		);
		if (!Failures.IsEmpty()) {
			Message += TEXT("\n以下资产保存失败，修改仍保留在编辑器内，请检查文件权限并手动保存：\n") + FString::Join(Failures, TEXT("\n"));
		}
		Report(Message);
	}
}
