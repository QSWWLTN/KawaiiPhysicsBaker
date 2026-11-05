#include "KawaiiPhysicsBakeReplacement.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#include "KawaiiClothingProxyMotion.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakeReplacement"

namespace {
	bool SaveAsset(UObject& Asset) {
		const FString Filename = FPackageName::LongPackageNameToFilename(Asset.GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.bSlowTask = false;
		return UPackage::SavePackage(Asset.GetPackage(), &Asset, *Filename, Args);
	}

	FGuid ReadDataGuid(const UAnimSequence& Sequence) {
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return Sequence.GetRawDataGuid();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}
}

namespace KawaiiPhysicsBakeReplacement {
	bool ValidateSource(const UAnimSequence& Source, FString& OutError) {
		const FString PackageName = Source.GetPackage()->GetName();
		FString Filename;
		if (!Source.IsAsset() || Source.HasAnyFlags(RF_Transient) || PackageName.StartsWith(TEXT("/Engine/"))
			|| !FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension()))
		{
			OutError = TEXT("替换源动画需要可保存的项目或项目插件动画资产，不支持临时资产和引擎内容。");
			return false;
		}
		if (IFileManager::Get().IsReadOnly(*Filename)) {
			OutError = TEXT("源动画文件为只读，请先签出或解除只读后再烘焙：") + Filename;
			return false;
		}
		if (!Source.GetDataModel() || Source.IsValidAdditive() || !Source.GetDataModel()->GetAttributes().IsEmpty()) {
			OutError = TEXT("替换源动画需要有效的非叠加动画；暂不支持含类型化骨骼属性的源动画，可关闭替换选项单独导出。");
			return false;
		}
		return true;
	}

	bool Apply(UAnimSequence& Target, UAnimSequence& Baked, FString& OutError) {
		if (&Target == &Baked || !Target.GetDataModel() || !Baked.GetDataModel() || Target.GetSkeleton() != Baked.GetSkeleton() || Target.IsValidAdditive()
			|| Baked.IsValidAdditive() || !Target.GetDataModel()->GetAttributes().IsEmpty())
		{
			OutError = TEXT("烘焙替换要求不同的有效非叠加动画、相同骨架，且源动画不能含类型化骨骼属性。");
			return false;
		}

		const TArray<FAnimNotifyEvent> Notifies = Target.Notifies;
		const TArray<FAnimNotifyTrack> NotifyTracks = Target.AnimNotifyTracks;
		const TArray<FAnimSyncMarker> Markers = Target.AuthoredSyncMarkers;
		// CreateAnimation resets this editor property; UE exposes no public setter for its platform overrides.
		const FStructProperty* FrameRateProperty = FindFProperty<FStructProperty>(UAnimSequence::StaticClass(), TEXT("PlatformTargetFrameRate"));
		if (!FrameRateProperty) {
			OutError = TEXT("无法保留源动画的平台采样率设置，源动画未替换。");
			return false;
		}
		FPerPlatformFrameRate& TargetFrameRate = *FrameRateProperty->ContainerPtrToValuePtr<FPerPlatformFrameRate>(&Target);
		const FPerPlatformFrameRate PlatformFrameRate = TargetFrameRate;
		const TArray<FFloatCurve> OriginalCurves = Target.GetDataModel()->GetFloatCurves();
		Target.Modify();
		// UE replaces the model in-place, so hard/soft references and notify UObject ownership stay intact.
		const bool bCreated = Target.CreateAnimation(&Baked);
		Target.Notifies = Notifies;
		Target.AnimNotifyTracks = NotifyTracks;
		Target.AuthoredSyncMarkers = Markers;
		TargetFrameRate = PlatformFrameRate;
		Target.RefreshCacheData();
		if (!bCreated) {
			OutError = TEXT("无法将烘焙动画数据写回源动画。");
			return false;
		}
		Target.RetargetSource = Baked.RetargetSource;
		Target.SetRetargetSourceAsset(Baked.GetRetargetSourceAsset().LoadSynchronous());
		Target.RetargetSourceAssetReferencePose = Baked.RetargetSourceAssetReferencePose;
		IAnimationDataController& Controller = Target.GetController();
		Controller.OpenBracket(LOCTEXT("KeepCurveMetadata", "保留源动画曲线设置"));
		for (const FFloatCurve& Curve : OriginalCurves) {
			const FAnimationCurveIdentifier ID(Curve.GetName(), ERawCurveTrackTypes::RCT_Float);
			if (const FFloatCurve* BakedCurve = Target.GetDataModel()->FindFloatCurve(ID)) {
				// Redundant flag writes clear UE's compressed curve buffer without changing its DDC key.
				if (BakedCurve->GetCurveTypeFlags() != Curve.GetCurveTypeFlags()) {
					Controller.SetCurveFlags(ID, Curve.GetCurveTypeFlags());
				}
				if (BakedCurve->GetColor() != Curve.GetColor()) {
					Controller.SetCurveColor(ID, Curve.GetColor());
				}
			}
		}
		Controller.CloseBracket();
		Target.MarkPackageDirty();
		return true;
	}

	bool ReplaceAndSave(UAnimSequence& Source, UAnimSequence& Baked, const FString& BackupPackageName, const TArray<UKawaiiClothingProxyMotion*>& Motions, UAnimSequence*& OutBackup, FString& OutError) {
		OutBackup = nullptr;
		if (!ValidateSource(Source, OutError)) {
			return false;
		}
		if (FindPackage(nullptr, *BackupPackageName) || FPackageName::DoesPackageExist(BackupPackageName)) {
			OutError = TEXT("源动画备份名称已存在，源动画未替换。");
			return false;
		}

		TStrongObjectPtr<UAnimSequence> Candidate(DuplicateObject<UAnimSequence>(&Source, GetTransientPackage()));
		if (!Apply(*Candidate, Baked, OutError)) {
			return false;
		}
		const TMap<FName, FString>* SourceValues = UMetaData::GetMapForObject(&Source);
		const TMap<FName, FString> OriginalMetadata = SourceValues ? *SourceValues : TMap<FName, FString>();
		UPackage* BackupPackage = CreatePackage(*BackupPackageName);
		TStrongObjectPtr<UAnimSequence> Backup(DuplicateObject<UAnimSequence>(&Source, BackupPackage, *FPackageName::GetLongPackageAssetName(BackupPackageName))
		);
		Backup->ClearFlags(RF_Transient);
		Backup->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		BackupPackage->GetMetaData()->SetObjectValues(Backup.Get(), OriginalMetadata);
		BackupPackage->GetMetaData()->SetValue(Backup.Get(), TEXT("KawaiiBake.BackupOf"), *Source.GetPathName());
		FAssetRegistryModule::AssetCreated(Backup.Get());
		Backup->MarkPackageDirty();
		if (!SaveAsset(*Backup)) {
			OutError = TEXT("无法保存源动画备份，源动画未替换：") + BackupPackageName;
			return false;
		}
		OutBackup = Backup.Get();

		FScopedTransaction Transaction(LOCTEXT("ReplaceSource", "用物理烘焙结果替换源动画"));
		if (!Apply(Source, *Candidate, OutError)) {
			Transaction.Cancel();
			return false;
		}
		UMetaData* Metadata = Source.GetPackage()->GetMetaData();
		Metadata->Modify();
		Metadata->SetValue(&Source, TEXT("KawaiiBake.SourceBackup"), *Backup->GetPathName());
		if (Baked.HasAnyFlags(RF_Transient)) {
			// The source is the only persistent Main result. Transfer its bake group
			// and proxy pairing without retaining a path to the temporary take.
			if (const TMap<FName, FString>* BakedValues = UMetaData::GetMapForObject(&Baked)) {
				for (const TPair<FName, FString>& Entry : *BakedValues) {
					if (Entry.Key.ToString().StartsWith(TEXT("KawaiiBake."))) {
						Metadata->SetValue(&Source, Entry.Key, *Entry.Value);
					}
				}
			}
			Metadata->SetValue(
				&Source,
				TEXT("KawaiiBake.Content"),
				TEXT("Final real-bone transforms and float curves; preserved source notifies and sync markers")
			);
			Metadata->RemoveValue(&Source, TEXT("KawaiiBake.ReplacedWith"));
		} else {
			Metadata->SetValue(&Source, TEXT("KawaiiBake.ReplacedWith"), *Baked.GetPathName());
		}
		const FGuid NewGuid = ReadDataGuid(Source);

		struct FMotionBeforeReplacement
		{
			UKawaiiClothingProxyMotion* Motion;
			FGuid SourceDataGuid;
			TSoftObjectPtr<UAnimSequence> BakedAnimation;
			TSoftObjectPtr<UAnimSequence> BodyBakedAnimation;
		};

		TArray<FMotionBeforeReplacement> UpdatedMotions;
		bool bSaved = true;
		for (UKawaiiClothingProxyMotion* Motion : Motions) {
			if (Motion && Motion->SourceAnimation.ToSoftObjectPath() == FSoftObjectPath(&Source)) {
				UpdatedMotions.Add({Motion, Motion->SourceDataGuid, Motion->BakedAnimation, Motion->BodyBakedAnimation});
				Motion->Modify();
				// Replacement has one persistent body animation: the source. Enforce
				// this here as well as in export, including callers with a saved take.
				Motion->BodyBakedAnimation = &Source;
				if (Motion->BakedAnimation.ToSoftObjectPath() == FSoftObjectPath(&Baked)) {
					Motion->BakedAnimation = &Source;
				}
				Motion->SourceDataGuid = NewGuid;
				Motion->MarkPackageDirty();
				bSaved &= SaveAsset(*Motion);
			}
		}
		if (bSaved && SaveAsset(Source)) {
			return true;
		}

		// Restore both sides of the source/proxy version pairing if any package failed to save.
		FString RestoreError;
		bool bRestored = Apply(Source, *Backup, RestoreError);
		Metadata->SetObjectValues(&Source, OriginalMetadata);
		bRestored &= SaveAsset(Source);
		for (const FMotionBeforeReplacement& Previous : UpdatedMotions) {
			UKawaiiClothingProxyMotion* Motion = Previous.Motion;
			Motion->SourceDataGuid = Previous.SourceDataGuid;
			Motion->BakedAnimation = Previous.BakedAnimation;
			Motion->BodyBakedAnimation = Previous.BodyBakedAnimation;
			Motion->MarkPackageDirty();
			bRestored &= SaveAsset(*Motion);
		}
		Transaction.Cancel();
		OutError = bRestored ? TEXT("替换保存失败，已恢复源动画与代理版本。新烘焙结果和源备份仍保留。") : TEXT("替换保存失败且未能完整写回恢复数据，请从以下备份恢复源动画：") + Backup->GetPathName();
		return false;
	}
}

#undef LOCTEXT_NAMESPACE
