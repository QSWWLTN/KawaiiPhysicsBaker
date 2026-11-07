#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "ReferenceSkeleton.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#include "KawaiiPhysicsBakeReplacement.h"
#include "KawaiiClothingMotionReadback.h"
#include "KawaiiClothingProxyMotion.h"
#include "KawaiiClothingProxyPlayback.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeSession.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace KawaiiClothingReadbackTests {
	void VerifyExport(FAutomationTestBase& Test, const UKawaiiPhysicsBakeSettings& Settings, const TArray<TWeakObjectPtr<UObject>>& Assets, const TArray<TArray<FTransform>>& Preview);
}

namespace {
	const FAnimationCurveIdentifier TestCurve(TEXT("ReplacementCurve"), ERawCurveTrackTypes::RCT_Float);

	void Populate(UAnimSequence& Sequence, USkeleton& Skeleton, float EndPosition, int32 Rate) {
		Sequence.SetSkeleton(&Skeleton);
		IAnimationDataController& Controller = Sequence.GetController();
		Controller.OpenBracket(FText::FromString(TEXT("Replacement test fixture")), false);
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(Rate, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(Rate), false);
		TArray<FVector3f> Positions;
		TArray<FQuat4f> Rotations;
		TArray<FVector3f> Scales;
		for (int32 Frame = 0; Frame <= Rate; ++Frame) {
			Positions.Add(FVector3f(EndPosition * Frame / Rate, 0.f, 0.f));
			Rotations.Add(FQuat4f::Identity);
			Scales.Add(FVector3f::OneVector);
		}
		Controller.AddBoneCurve(TEXT("root"), false);
		Controller.SetBoneTrackKeys(TEXT("root"), Positions, Rotations, Scales, false);
		Controller.AddCurve(TestCurve, AACF_DefaultCurve, false);
		Controller.SetCurveKeys(TestCurve, {FRichCurveKey(0.f, 0.f), FRichCurveKey(1.f, EndPosition)}, false);
		Controller.NotifyPopulated();
		Controller.CloseBracket(false);
	}

	void AddRoot(USkeleton& Skeleton) {
		FReferenceSkeletonModifier Modifier(&Skeleton);
		Modifier.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
	}

	FGuid DataGuid(const UAnimSequence& Sequence) {
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return Sequence.GetRawDataGuid();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}

	template <typename T> T* CreateAsset(const FString& PackageName) {
		return NewObject<T>(CreatePackage(*PackageName), *FPackageName::GetLongPackageAssetName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
	}

	bool SaveFixture(UObject& Asset) {
		const FString Filename = FPackageName::LongPackageNameToFilename(Asset.GetPackage()->GetName(), TEXT(".uasset"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Asset.GetPackage(), &Asset, *Filename, Args);
	}

	bool MatchesSequence(UAnimSequence& Sequence, USkeletalMesh& Mesh, float Time, const TArray<FTransform>& Displayed, FString& Error) {
		KawaiiClothingReadback::FFrame Decoded;
		if (!KawaiiClothingReadback::Decode(Sequence, Mesh, Time, Decoded, Error)) { return false; }
		for (int32 Bone = 0; Bone < Mesh.GetRefSkeleton().GetRawBoneNum(); ++Bone) {
			if (!Displayed.IsValidIndex(Bone) || !Displayed[Bone].Equals(FTransform(Decoded.Pose[Bone]), .002)) {
				Error = FString::Printf(TEXT("%s: playback differs at %.4f s, bone %d"), *Sequence.GetName(), Time, Bone);
				return false;
			}
		}
		return true;
	}

	class FSourceReplacementBakeCommand : public IAutomationLatentCommand
	{
	public:
		FSourceReplacementBakeCommand(FAutomationTestBase* InTest, UAnimBlueprint* InBlueprint, UAnimSequence* InOriginal, bool bInReadback = false)
			: Test(InTest), Blueprint(InBlueprint), Original(InOriginal), OriginalGuid(DataGuid(*InOriginal)), Session(MakeShared<FKawaiiPhysicsBakeSession>()),
			  StartedAt(FPlatformTime::Seconds()), bReadback(bInReadback) {}

		virtual bool Update() override {
			if (FPlatformTime::Seconds() - StartedAt > 60.0) {
				Test->AddError(TEXT("Source replacement bake timed out."));
				Session->Cancel();
				return true;
			}
			FString Error;
			if (!bStarted) {
				bStarted = true;
				const auto* Settings = KawaiiPhysicsBake::GetSettings(Blueprint.Get());
				if (Settings->ExistingBakedBodyAnimation) { BodyGuid = DataGuid(*Settings->ExistingBakedBodyAnimation); }
				bRecordingPreview = bReadback || Settings->ExistingBakedBodyAnimation;
				if (!Session->Initialize(Blueprint.Get(), Error) || !(bRecordingPreview ? Session->StartPreview(Error) : Session->StartBake(Error))) {
					Test->AddError(Error);
					return true;
				}
			}
			Session->Tick(1.f / 60.f);
			if (bRecordingPreview) {
				if (!Session->IsRunning()) { Test->AddError(Session->GetStatus().ToString()); return true; }
				if (Session->GetCachedFrameCount() <= PreviewClothing.Num()) { return false; }
				auto* Settings = KawaiiPhysicsBake::GetSettings(Blueprint.Get());
				if (Settings->ExistingBakedBodyAnimation && !MatchesSequence(*Settings->ExistingBakedBodyAnimation, *Settings->MainMesh,
					Session->GetPreviewTime(), Session->GetMainComponent()->GetComponentSpaceTransforms(), Error)) {
					Test->AddError(Error);
					return true;
				}
				TInlineComponentArray<USkeletalMeshComponent*> Components(Session->GetMainComponent()->GetOwner());
				auto** Found = Components.FindByPredicate([&](auto* Component) { return Component->GetFName() == Settings->Parts[0].Name; });
				if (!Found) { Test->AddError(TEXT("Missing preview garment")); return true; }
				PreviewClothing.Add((*Found)->GetComponentSpaceTransforms());
				if (PreviewClothing.Num() <= Session->GetLastFrame()) { return false; }
				bRecordingPreview = false; Session->Cancel();
				// The preview used a different ordinary source. Export must also work with that slot empty.
				if (Settings->ExistingBakedBodyAnimation) { Settings->SourceAnimation = nullptr; }
				if (!Session->StartBake(Error)) { Test->AddError(Error); return true; }
				return false;
			}
			if (Session->IsRunning()) {
				return false;
			}
			const UKawaiiPhysicsBakeSettings* Settings = KawaiiPhysicsBake::GetSettings(Blueprint.Get());
			const bool bReuse = Settings->ExistingBakedBodyAnimation != nullptr;
			UAnimSequence* Source = bReuse ? Settings->ExistingBakedBodyAnimation.Get() : Settings->SourceAnimation.Get();
			const bool bReplace = !bReuse && Settings->bReplaceSourceAnimation;
			Test->TestTrue(*Session->GetStatus().ToString(), Session->GetStatus().ToString().Contains(bReplace ? TEXT("已替换源动画") : TEXT("已保存")));
			Test->TestEqual(TEXT("Source is a result only in replacement mode"), Session->GetOutputAssets().Contains(Source), bReplace);
			int32 NumBackups = 0;
			int32 NumMotions = 0;
			int32 NumSequences = 0;
			UAnimSequence* MainResult = bReplace || bReuse ? Source : nullptr;
			for (const TWeakObjectPtr<UObject>& Asset : Session->GetOutputAssets()) {
				if (!Test->TestNotNull(TEXT("Output asset is valid"), Asset.Get())) {
					continue;
				}
				Test->TestFalse(TEXT("Output assets are persistent"), Asset->HasAnyFlags(RF_Transient));
				Test->TestTrue(TEXT("Every output is saved to disk"), FPackageName::DoesPackageExist(Asset->GetPackage()->GetName()));
				if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.Get())) {
					++NumSequences;
					if (Sequence->GetName().EndsWith(TEXT("_SourceBackup"))) {
						++NumBackups;
						Test->TestEqual(TEXT("Session backup contains the original data"), DataGuid(*Sequence), OriginalGuid);
					} else if (Sequence->GetPackage()->GetMetaData()->GetValue(Sequence, TEXT("KawaiiBake.Part")) == TEXT("Main")) {
						Test->TestNull(TEXT("There is only one Main animation result"), bReplace && Sequence == Source ? nullptr : MainResult);
						MainResult = Sequence;
					}
				}
			}
			Test->TestNotNull(TEXT("Main animation result is available"), MainResult);
			const int32 ExpectedBodySequences = bReuse ? 0 : (bReplace ? 2 : 1);
			Test->TestEqual(TEXT("Only the expected animation sequences are exported"), NumSequences, Settings->Parts.Num() + ExpectedBodySequences);
			if (bReplace) {
				Test->TestTrue(TEXT("Main result is the original source object"), MainResult == Source);
				Test->TestEqual(
					TEXT("Main bake metadata is transferred to the source"),
					Source->GetPackage()->GetMetaData()->GetValue(Source, TEXT("KawaiiBake.Part")),
					FString(TEXT("Main"))
				);
				Test->TestTrue(
					TEXT("Source metadata never points to a transient take"),
					Source->GetPackage()->GetMetaData()->GetValue(Source, TEXT("KawaiiBake.ReplacedWith")).IsEmpty()
				);
			} else if (bReuse) {
				Test->TestEqual(TEXT("Reused body data is untouched"), DataGuid(*Source), BodyGuid);
			} else {
				Test->TestEqual(TEXT("Separate export leaves source data unchanged"), DataGuid(*Source), OriginalGuid);
			}
			for (const TWeakObjectPtr<UObject>& Asset : Session->GetOutputAssets()) {
				if (UKawaiiClothingProxyMotion* Motion = Cast<UKawaiiClothingProxyMotion>(Asset.Get())) {
					++NumMotions;
					Test->TestTrue(TEXT("Complete proxy payload remains valid after replacement"), Motion->Validate(Error));
					if (!Error.IsEmpty()) {
						Test->AddError(Error);
					}
					Test->TestEqual(TEXT("New proxy matches overwritten source revision"), Motion->SourceDataGuid, DataGuid(*Source));
					Test->TestEqual(
						TEXT("Proxy references the persistent Main result"),
						Motion->BodyBakedAnimation.ToSoftObjectPath(),
						FSoftObjectPath(MainResult)
					);
					if (Motion->PartName == TEXT("Main")) {
						Test->TestEqual(
							TEXT("Main proxy playback references the persistent Main result"),
							Motion->BakedAnimation.ToSoftObjectPath(),
							FSoftObjectPath(MainResult)
						);
						Test->TestEqual(
							TEXT("Main result retains its proxy association"),
							MainResult->GetPackage()->GetMetaData()->GetValue(MainResult, TEXT("KawaiiBake.ProxyMotion")),
							Motion->GetPathName()
						);
					} else {
						Test->TestTrue(
							TEXT("Clothing proxy retains its separate animation"),
							Motion->BakedAnimation.Get() != MainResult && Session->GetOutputAssets().Contains(Motion->BakedAnimation.Get())
						);
						if (bReuse && Motion->BakedAnimation.Get()) {
							Test->TestEqual(TEXT("Proxy source is the selected playback sequence"), Motion->SourceAnimation.Get(), Source);
							const auto* Set = Settings->Parts[0].ProxyMotionSet.Get();
							Test->TestTrue(TEXT("Rebaking replaces the same body mapping"), Set && Set->Motions.Num() == 1 && Set->Motions[0] == Motion);
							for (int32 Frame = 0; Frame < PreviewClothing.Num(); ++Frame) {
								if (!MatchesSequence(*Motion->BakedAnimation.Get(), *Settings->Parts[0].Mesh, float(Frame) / Settings->SampleRate,
									PreviewClothing[Frame], Error)) {
									Test->AddError(Error);
									break;
								}
							}
						}
					}
				}
			}
			Test->TestEqual(TEXT("Source backup is created only for replacement"), NumBackups, bReplace ? 1 : 0);
			int32 ExpectedMotions = Settings->ProxyDefinition && !bReuse ? 1 : 0;
			for (const FKawaiiPhysicsBakePart& Part : Settings->Parts) {
				ExpectedMotions += Part.ProxyDefinition ? 1 : 0;
			}
			Test->TestEqual(TEXT("Session exports every selected proxy"), NumMotions, ExpectedMotions);
			TArray<FString> SavedFiles;
			const FString OutputDirectory = FPackageName::LongPackageNameToFilename(Settings->OutputDirectory.Path);
			IFileManager::Get().FindFilesRecursive(SavedFiles, *OutputDirectory, TEXT("*.uasset"), true, false);
			Test->TestEqual( TEXT("No unreported extra asset is saved in the output directory"), SavedFiles.Num(), Session->GetOutputAssets().Num() - (bReplace ? 1 : 0) );
			Test->TestEqual(TEXT("User's animation was never modified"), DataGuid(*Original), OriginalGuid);
			Test->TestFalse(TEXT("Overwritten test source is saved"), Source->GetPackage()->IsDirty());
			if (bReadback) { KawaiiClothingReadbackTests::VerifyExport(*Test, *Settings, Session->GetOutputAssets(), PreviewClothing); }
			return true;
		}

	private:
		FAutomationTestBase* Test;
		TStrongObjectPtr<UAnimBlueprint> Blueprint;
		TStrongObjectPtr<UAnimSequence> Original;
		FGuid OriginalGuid;
		FGuid BodyGuid;
		TSharedRef<FKawaiiPhysicsBakeSession> Session;
		double StartedAt;
		bool bStarted = false;
		bool bReadback = false, bRecordingPreview = false;
		TArray<TArray<FTransform>> PreviewClothing;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiBakeReplaceIdentityTest,
	"KawaiiPhysicsBaker.SourceReplacement.IdentityAndAuthoredEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiBakeReplaceIdentityTest::RunTest(const FString&) {
	USkeleton* Skeleton = NewObject<USkeleton>();
	AddRoot(*Skeleton);
	UAnimSequence* Source = NewObject<UAnimSequence>();
	UAnimSequence* Baked = NewObject<UAnimSequence>();
	Populate(*Source, *Skeleton, 10.f, 24);
	Populate(*Baked, *Skeleton, 90.f, 60);
	Source->bEnableRootMotion = true;
	Source->bForceRootLock = true;
	Source->RootMotionRootLock = ERootMotionRootLock::AnimFirstFrame;
	Source->AnimNotifyTracks.Emplace(TEXT("Gameplay"), FLinearColor::Green);
	FAnimNotifyEvent& Notify = Source->Notifies.AddDefaulted_GetRef();
	Notify.Notify = NewObject<UAnimNotify_PlaySound>(Source);
	Notify.NotifyName = TEXT("PreservedEvent");
	Notify.TrackIndex = 0;
	Notify.Link(Source, 0.3f);
	UAnimNotify* OriginalNotify = Notify.Notify;
	FAnimSyncMarker& Marker = Source->AuthoredSyncMarkers.AddDefaulted_GetRef();
	Marker.MarkerName = TEXT("Footstep");
	Marker.Time = 0.5f;
	Marker.TrackIndex = 0;
	const int32 CurveFlags = AACF_DefaultCurve | AACF_Disabled;
	Source->GetController().SetCurveFlags(TestCurve, CurveFlags, false);
	Source->GetController().SetCurveColor(TestCurve, FLinearColor::Red, false);
	const FStructProperty* RateProperty = FindFProperty<FStructProperty>(UAnimSequence::StaticClass(), TEXT("PlatformTargetFrameRate"));
	if (!TestNotNull(TEXT("Platform frame rate property"), RateProperty)) {
		return false;
	}
	FPerPlatformFrameRate& FrameRate = *RateProperty->ContainerPtrToValuePtr<FPerPlatformFrameRate>(Source);
	FrameRate.Default = FFrameRate(24, 1);
	FrameRate.PerPlatform.Add(TEXT("Windows"), FFrameRate(12, 1));
	FAnimSegment Segment;
	Segment.SetAnimReference(Source);
	const FSoftObjectPath OriginalPath(Source);
	FString Error;
	if (!TestTrue(TEXT("Apply baked data in-place"), KawaiiPhysicsBakeReplacement::Apply(*Source, *Baked, Error))) {
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Existing animation player reference retains the same UObject"), Segment.GetAnimReference() == Source);
	TestEqual(TEXT("Original asset name and path are unchanged"), FSoftObjectPath(Source), OriginalPath);
	TestTrue(
		TEXT("Baked bone keys replace the old motion"),
		Source->GetDataModel()->GetBoneTrackTransform(TEXT("root"), FFrameNumber(60)).GetTranslation().Equals(FVector(90, 0, 0))
	);
	TestEqual(TEXT("Baked source frame rate is copied"), Source->GetDataModel()->GetFrameRate(), FFrameRate(60, 1));
	TestEqual(TEXT("Target sampling frame rate is retained"), FrameRate.Default, FFrameRate(24, 1));
	TestEqual(TEXT("Platform frame rate override is retained"), FrameRate.PerPlatform.FindRef(TEXT("Windows")), FFrameRate(12, 1));
	TestEqual(TEXT("Authored notify count"), Source->Notifies.Num(), 1);
	if (Source->Notifies.Num() == 1) {
		TestTrue(TEXT("Notify UObject and owner survive replacement"), Source->Notifies[0].Notify == OriginalNotify && OriginalNotify->GetOuter() == Source);
		TestEqual(TEXT("Notify time is retained"), Source->Notifies[0].GetTime(), 0.3f);
	}
	TestEqual(TEXT("Authored sync marker count"), Source->AuthoredSyncMarkers.Num(), 1);
	if (Source->AuthoredSyncMarkers.Num() == 1) {
		TestEqual(TEXT("Sync marker time is retained"), Source->AuthoredSyncMarkers[0].Time, 0.5f);
	}
	TestTrue(
		TEXT("Original root motion and root lock settings survive"),
		Source->bEnableRootMotion && Source->bForceRootLock && Source->RootMotionRootLock == ERootMotionRootLock::AnimFirstFrame
	);
	const FFloatCurve* Curve = Source->GetDataModel()->FindFloatCurve(TestCurve);
	if (TestNotNull(TEXT("Baked float curve"), Curve)) {
		TestEqual(TEXT("Float curve uses baked values"), Curve->FloatCurve.Eval(1.f), 90.f);
		TestEqual(TEXT("Original curve flags survive"), Curve->GetCurveTypeFlags(), CurveFlags);
		TestEqual(TEXT("Original curve color survives"), Curve->GetColor(), FLinearColor::Red);
	}
	const FGuid UpdatedGuid = DataGuid(*Source);
	UAnimSequence* Incompatible = NewObject<UAnimSequence>();
	USkeleton* OtherSkeleton = NewObject<USkeleton>();
	AddRoot(*OtherSkeleton);
	Populate(*Incompatible, *OtherSkeleton, 1.f, 60);
	TestFalse(TEXT("Incompatible skeleton cannot overwrite the source"), KawaiiPhysicsBakeReplacement::Apply(*Source, *Incompatible, Error));
	TestEqual(TEXT("Rejected replacement leaves the data unchanged"), DataGuid(*Source), UpdatedGuid);
	TestFalse(TEXT("Existing bake settings default to creating separate outputs"), GetDefault<UKawaiiPhysicsBakeSettings>()->bReplaceSourceAnimation);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiBakeReplaceSaveTest,
	"KawaiiPhysicsBaker.SourceReplacement.BackupAndProxyRevision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiBakeReplaceSaveTest::RunTest(const FString&) {
	// Persistent test assets live only in Saved, never in the user's Content directory.
	const FString Mount = TEXT("/KawaiiReplacementTests/");
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/SourceReplacement/Content/"));
	FPackageName::RegisterMountPoint(Mount, Directory);
	const FString Prefix = Mount + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/");
	USkeleton* Skeleton = CreateAsset<USkeleton>(Prefix + TEXT("Skeleton"));
	AddRoot(*Skeleton);
	UAnimSequence* Source = CreateAsset<UAnimSequence>(Prefix + TEXT("Source"));
	UAnimSequence* Baked = CreateAsset<UAnimSequence>(Prefix + TEXT("Baked"));
	UAnimSequence* Garment = CreateAsset<UAnimSequence>(Prefix + TEXT("Garment"));
	Populate(*Source, *Skeleton, 10.f, 24);
	Populate(*Baked, *Skeleton, 90.f, 60);
	Populate(*Garment, *Skeleton, 45.f, 60);
	Source->GetPackage()->GetMetaData()->SetValue(Source, TEXT("AuthoredMetadata"), TEXT("Retained"));
	UKawaiiClothingProxyMotion* Motion = CreateAsset<UKawaiiClothingProxyMotion>(Prefix + TEXT("Proxy"));
	Motion->SourceAnimation = Source;
	Motion->BakedAnimation = Garment;
	Motion->BodyBakedAnimation = Baked;
	Motion->SourceDataGuid = DataGuid(*Source);
	UKawaiiClothingProxyMotion* MainMotion = CreateAsset<UKawaiiClothingProxyMotion>(Prefix + TEXT("MainProxy"));
	MainMotion->SourceAnimation = Source;
	MainMotion->BakedAnimation = Baked;
	MainMotion->BodyBakedAnimation = Baked;
	MainMotion->SourceDataGuid = DataGuid(*Source);
	const FGuid OriginalGuid = Motion->SourceDataGuid;
	const FString OriginalPath = Source->GetPathName();
	if (!TestTrue(
			TEXT("Initial test assets are saved"),
			SaveFixture(*Skeleton) && SaveFixture(*Source) && SaveFixture(*Baked) && SaveFixture(*Garment) && SaveFixture(*Motion) && SaveFixture(*MainMotion)
		))
	{
		return false;
	}
	FString Error;
	UAnimSequence* Backup = nullptr;
	const bool bReplaced = KawaiiPhysicsBakeReplacement::ReplaceAndSave(*Source, *Baked, Prefix + TEXT("SourceBackup"), {Motion, MainMotion}, Backup, Error);
	TestTrue(*FString::Printf(TEXT("Replacement saved: %s"), *Error), bReplaced);
	if (bReplaced && TestNotNull(TEXT("Original source backup"), Backup)) {
		TestEqual(TEXT("Original asset path is retained"), Source->GetPathName(), OriginalPath);
		TestEqual(TEXT("Backup retains original animation data"), DataGuid(*Backup), OriginalGuid);
		TestTrue(TEXT("Source data is replaced"), DataGuid(*Source) != OriginalGuid);
		TestEqual(TEXT("Proxy revision follows overwritten source"), Motion->SourceDataGuid, DataGuid(*Source));
		TestEqual(TEXT("Proxy source identity is unchanged"), Motion->SourceAnimation.ToSoftObjectPath(), FSoftObjectPath(Source));
		TestEqual(TEXT("Proxy body playback follows the overwritten source"), Motion->BodyBakedAnimation.ToSoftObjectPath(), FSoftObjectPath(Source));
		TestEqual(TEXT("Clothing playback retains its separate animation"), Motion->BakedAnimation.ToSoftObjectPath(), FSoftObjectPath(Garment));
		TestEqual(TEXT("Main proxy playback follows the overwritten source"), MainMotion->BakedAnimation.ToSoftObjectPath(), FSoftObjectPath(Source));
		TestEqual(TEXT("Main proxy body playback follows the overwritten source"), MainMotion->BodyBakedAnimation.ToSoftObjectPath(), FSoftObjectPath(Source));
		TestEqual(
			TEXT("Original asset metadata survives"),
			Source->GetPackage()->GetMetaData()->GetValue(Source, TEXT("AuthoredMetadata")),
			FString(TEXT("Retained"))
		);
		TestTrue(TEXT("Backup is written to disk before overwrite"), FPackageName::DoesPackageExist(Backup->GetPackage()->GetName()));
		TestFalse(TEXT("Source is fully saved"), Source->GetPackage()->IsDirty());
		TestFalse(TEXT("Proxy is fully saved"), Motion->GetPackage()->IsDirty());
		const FGuid UpdatedGuid = DataGuid(*Source);
		UAnimSequence* RejectedBackup = nullptr;
		TestFalse(
			TEXT("Existing backup is never overwritten"),
			KawaiiPhysicsBakeReplacement::ReplaceAndSave(*Source, *Baked, Backup->GetPackage()->GetName(), {Motion}, RejectedBackup, Error)
		);
		TestEqual(TEXT("Rejected backup collision leaves source intact"), DataGuid(*Source), UpdatedGuid);
		TestNull(TEXT("Rejected replacement reports no new backup"), RejectedBackup);
		const FString Filename = FPackageName::LongPackageNameToFilename(Source->GetPackage()->GetName(), TEXT(".uasset"));
		if (TestTrue(TEXT("Fixture can be marked read-only"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Filename, true))) {
			TestFalse(
				TEXT("Read-only source is rejected before writing a backup"),
				KawaiiPhysicsBakeReplacement::ReplaceAndSave(*Source, *Baked, Prefix + TEXT("RejectedBackup"), {Motion}, RejectedBackup, Error)
			);
			TestNull(TEXT("Read-only source creates no backup"), RejectedBackup);
			TestEqual(TEXT("Read-only source data remains intact"), DataGuid(*Source), UpdatedGuid);
			TestTrue(TEXT("Fixture read-only flag is restored"), FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Filename, false));
		}
		// Reload from disk so this also verifies the saved references, not only memory.
		const FString MotionPath = Motion->GetPathName();
		const FString MainMotionPath = MainMotion->GetPathName();
		const FSoftObjectPath SourcePath(Source), GarmentPath(Garment);
		const TArray<UPackage*> PackagesToReload = {Motion->GetPackage(), MainMotion->GetPackage()};
		Motion = MainMotion = nullptr;
		FText ReloadError;
		if (TestTrue(
				TEXT("Saved proxy packages reload"),
				UPackageTools::ReloadPackages(PackagesToReload, ReloadError, EReloadPackagesInteractionMode::AssumePositive)
			))
		{
			Motion = LoadObject<UKawaiiClothingProxyMotion>(nullptr, *MotionPath);
			MainMotion = LoadObject<UKawaiiClothingProxyMotion>(nullptr, *MainMotionPath);
			if (TestNotNull(TEXT("Reloaded clothing proxy"), Motion) && TestNotNull(TEXT("Reloaded Main proxy"), MainMotion)) {
				TestEqual(TEXT("Saved clothing proxy uses the source for body playback"), Motion->BodyBakedAnimation.ToSoftObjectPath(), SourcePath);
				TestEqual(TEXT("Saved clothing playback retains its animation"), Motion->BakedAnimation.ToSoftObjectPath(), GarmentPath);
				TestEqual(TEXT("Saved Main proxy uses the source for playback"), MainMotion->BakedAnimation.ToSoftObjectPath(), SourcePath);
				TestEqual(TEXT("Saved Main proxy uses the source for body playback"), MainMotion->BodyBakedAnimation.ToSoftObjectPath(), SourcePath);
				TestEqual(TEXT("Saved proxy retains the new source revision"), Motion->SourceDataGuid, UpdatedGuid);
			}
		} else {
			AddError(ReloadError.ToString());
		}
	}
	// Keep the test mount alive: editor save validation runs on the next tick.
	return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(
	FKawaiiBakeReplaceSessionTest,
	"KawaiiPhysicsBaker.SourceReplacement.ProjectBakeSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

void FKawaiiBakeReplaceSessionTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const {
	for (const TCHAR* Mode : {TEXT("ReplaceWithParts"), TEXT("SeparateWithParts"), TEXT("ReplaceMainOnly"), TEXT("ReplaceMainProxy"), TEXT("ClothingReadback"), TEXT("ReuseBakedBody")}) {
		OutBeautifiedNames.Add(Mode);
		OutTestCommands.Add(Mode);
	}
}

bool FKawaiiBakeReplaceSessionTest::RunTest(const FString& Mode) {
	if (!FPackageName::DoesPackageExist(TEXT("/Game/KawaiiBakes/ABP_KawaiiBake"))) {
		AddWarning(TEXT("Optional project bake fixture is unavailable; session check skipped."));
		return true;
	}
	UAnimBlueprint* Graph = LoadObject<UAnimBlueprint>(nullptr, TEXT("/Game/KawaiiBakes/ABP_KawaiiBake.ABP_KawaiiBake"));
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* GraphPackage = CreatePackage(*(TEXT("/Temp/KawaiiReplacementGraph_") + Suffix));
	GraphPackage->SetFlags(RF_Transient);
	UAnimBlueprint* Copy = DuplicateObject<UAnimBlueprint>(Graph, GraphPackage, TEXT("ABP_ReplacementTest"));
	UKawaiiPhysicsBakeSettings* Settings = KawaiiPhysicsBake::GetSettings(Copy);
	if (!Settings || !Settings->SourceAnimation || !Settings->MainMesh || Settings->Parts.IsEmpty() || !Settings->Parts[0].ProxyDefinition) {
		AddWarning(TEXT("Optional project clothing bake fixture is incomplete; session check skipped."));
		return true;
	}
	const FString Mount = TEXT("/KawaiiReplacementSessionTests/");
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/SourceReplacementSession/Content/"));
	FPackageName::RegisterMountPoint(Mount, Directory);
	const FString Prefix = Mount + Suffix;
	UAnimSequence* Original = Settings->SourceAnimation;
	UAnimSequence* Source = DuplicateObject<UAnimSequence>(Original, CreatePackage(*(Prefix + TEXT("/Source"))), TEXT("Source"));
	Source->ClearFlags(RF_Transient);
	Source->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	if (!TestTrue(TEXT("Independent source fixture is saved"), SaveFixture(*Source))) {
		return false;
	}
	Settings->SourceAnimation = Source;
	Settings->ExistingBakedBodyAnimation = nullptr;
	Settings->bReplaceSourceAnimation = Mode != TEXT("SeparateWithParts") && Mode != TEXT("ClothingReadback");
	Settings->bPreviewClothingProxyCollision = false;
	Settings->bEnableClothingCollision = false;
	Settings->WarmUpFrames = 0;
	Settings->OutputDirectory.Path = Prefix + TEXT("/Outputs");
	Settings->OutputPrefix = TEXT("ReplacementTest");
	Settings->ProxyMotionSet = nullptr;
	Settings->ProxyDefinition = nullptr;
	if (Mode == TEXT("ClothingReadback") || Mode == TEXT("ReuseBakedBody")) {
		auto* Definition = CreateAsset<UKawaiiClothingProxyAsset>(Prefix + TEXT("/BodyProxy"));
		Definition->PreviewMesh = Settings->MainMesh;
		for (const TCHAR* Side : {TEXT("L"), TEXT("R")}) {
			auto& Shape = Definition->Shapes.AddDefaulted_GetRef();
			Shape.Id = FName(*FString::Printf(TEXT("Thigh_%s"), Side));
			Shape.StartBone = FName(*FString::Printf(TEXT("UpperLeg_%s"), Side));
			Shape.EndBone = FName(*FString::Printf(TEXT("LowerLeg_%s"), Side));
			Shape.Radius = 6.f;
		}
		if (!TestTrue(TEXT("Independent body proxy saved"), SaveFixture(*Definition))) { return false; }
		Settings->ProxyDefinition = Definition;
		Settings->Parts[0].bCollideWithMainProxy = true;
	}
	if (Mode == TEXT("ReplaceMainOnly") || Mode == TEXT("ReplaceMainProxy")) {
		Settings->Parts.Reset();
	}
	if (Mode == TEXT("ReplaceMainProxy")) {
		UKawaiiClothingProxyAsset* Definition = CreateAsset<UKawaiiClothingProxyAsset>(Prefix + TEXT("/MainProxy"));
		Definition->PreviewMesh = Settings->MainMesh.Get();
		FKawaiiClothingProxyShape& Shape = Definition->Shapes.AddDefaulted_GetRef();
		Shape.Id = TEXT("RootProxy");
		Shape.StartBone = Settings->MainMesh->GetRefSkeleton().GetBoneName(0);
		if (!TestTrue(TEXT("Main proxy fixture is saved"), SaveFixture(*Definition))) {
			return false;
		}
		Settings->ProxyDefinition = Definition;
	}
	for (FKawaiiPhysicsBakePart& Part : Settings->Parts) {
		Part.ProxyMotionSet = nullptr;
	}
	if (Mode == TEXT("ReuseBakedBody")) {
		Settings->Parts.SetNum(1);
		auto& Garment = Settings->Parts[0];
		Garment.Animation = nullptr;
		Garment.CopyPoseFrom = TEXT("Main");
		auto* Body = CreateAsset<UAnimSequence>(Prefix + TEXT("/PreviouslyBakedBody"));
		Populate(*Body, *Settings->MainMesh->GetSkeleton(), 87.f, 24);
		Body->bEnableRootMotion = true;
		Body->bForceRootLock = true;
		if (!TestTrue(TEXT("Previously baked body saved independently"), SaveFixture(*Body))) { return false; }
		Settings->ExistingBakedBodyAnimation = Body;
		auto* Set = CreateAsset<UKawaiiClothingProxyMotionSet>(Settings->OutputDirectory.Path + TEXT("/ExistingSet"));
		auto* OldMotion = CreateAsset<UKawaiiClothingProxyMotion>(Prefix + TEXT("/EarlierMotion"));
		OldMotion->Definition = Garment.ProxyDefinition;
		OldMotion->SourceMesh = Garment.Mesh.Get();
		OldMotion->RelativeTransform = Garment.RelativeTransform;
		OldMotion->SourceAnimation = Source;
		OldMotion->BodyBakedAnimation = Body;
		Set->Motions.Add(OldMotion);
		Garment.ProxyMotionSet = Set;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FSourceReplacementBakeCommand(this, Copy, Original, Mode == TEXT("ClothingReadback")));
	return true;
}

#endif
