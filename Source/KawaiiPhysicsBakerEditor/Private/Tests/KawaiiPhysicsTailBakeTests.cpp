#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiPhysicsBakeSession.h"
#include "KawaiiPhysicsBakeBlueprint.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace {
	class FKawaiiTailBakeCommand : public IAutomationLatentCommand
	{
	public:
		FKawaiiTailBakeCommand(FAutomationTestBase* InTest, UAnimBlueprint* InBlueprint)
			: Test(InTest), Blueprint(InBlueprint), Session(MakeShared<FKawaiiPhysicsBakeSession>()), Started(FPlatformTime::Seconds()) {}

		bool Update() override {
			if (FPlatformTime::Seconds() - Started > 45.) {
				Test->AddError(TEXT("Tail bake timed out"));
				Session->Cancel();
				return true;
			}
			if (!bStarted) {
				FString Error;
				if (!Session->Initialize(Blueprint.Get(), Error) || !Session->StartPreview(Error)) {
					Test->AddError(Error);
					return true;
				}
				bStarted = true;
			}
			Session->Tick(1.f / 60.f);
			if (!Session->IsRunning()) {
				Test->AddError(Session->GetStatus().ToString());
				return true;
			}
			if (Session->GetCachedFrameCount() > Frames) {
				USkeletalMeshComponent* Main = Session->GetMainComponent();
				const FReferenceSkeleton& Skeleton = Main->GetSkeletalMeshAsset()->GetRefSkeleton();
				const TArray<FTransform>& Local = Main->GetBoneSpaceTransforms();
				for (int32 Bone = 0; Bone < Local.Num(); ++Bone) {
					const FString Name = Skeleton.GetBoneName(Bone).ToString();
					if (!Name.StartsWith(TEXT("Hair_tail_L")) && !Name.StartsWith(TEXT("Hair_tail_R"))) {
						continue;
					}
					const double ReferenceLength = Skeleton.GetRefBonePose()[Bone].GetTranslation().Size();
					const double Ratio = ReferenceLength > SMALL_NUMBER ? Local[Bone].GetTranslation().Size() / ReferenceLength : 1.;
					if (Ratio < MinRatio) {
						MinRatio = Ratio;
						WorstBone = Name;
						WorstFrame = Frames;
					}
					const FVector P = Local[Bone].GetTranslation(), S = Local[Bone].GetScale3D();
					const FQuat Q = Local[Bone].GetRotation();
					Rows += FString::Printf(
						TEXT("%d,%s,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f\n"),
						Frames,
						*Name,
						P.X,
						P.Y,
						P.Z,
						Q.X,
						Q.Y,
						Q.Z,
						Q.W,
						S.X,
						S.Y,
						S.Z
					);
				}
				++Frames;
			}
			if (Frames <= Session->GetLastFrame()) {
				return false;
			}
			Test->AddInfo(
				FString::Printf(TEXT("Tail bake: %d frames, minimum real bone length %.6f of reference at %d %s"), Frames, MinRatio, WorstFrame, *WorstBone)
			);
			Test->TestTrue(TEXT("Collision-only subdivisions preserve real tail bone lengths"), MinRatio > .99);
			const FString Directory = FPaths::ProjectSavedDir() / TEXT("KawaiiMotionWarpAudit");
			IFileManager::Get().MakeDirectory(*Directory, true);
			FFileHelper::SaveStringToFile(Rows, *(Directory / TEXT("tail-rebake.csv")));
			Session->Cancel();
			return true;
		}

	private:
		FAutomationTestBase* Test;
		TStrongObjectPtr<UAnimBlueprint> Blueprint;
		TSharedPtr<FKawaiiPhysicsBakeSession> Session;
		double Started, MinRatio = 1.;
		bool bStarted = false;
		int32 Frames = 0, WorstFrame = 0;
		FString WorstBone;
		FString Rows = TEXT("frame,bone,x,y,z,qx,qy,qz,qw,sx,sy,sz\n");
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiTailBakeTest,
	"KawaiiPhysicsBaker.PreviewProxy.ProjectTailBoneLengths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiTailBakeTest::RunTest(const FString&) {
	const TCHAR* BodyPath = TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/Jump/Jump_Climb_2h");
	if (!FPackageName::DoesPackageExist(BodyPath)) {
		AddWarning(TEXT("Original climb fixture unavailable"));
		return true;
	}
	UAnimSequence* Body = LoadObject<UAnimSequence>(nullptr, BodyPath);
	if (!TestNotNull(TEXT("Current climb sequence"), Body)) {
		return false;
	}
	const FString BackupPath = Body->GetPackage()->GetMetaData()->GetValue(Body, TEXT("KawaiiBake.SourceBackup"));
	if (BackupPath.IsEmpty()) {
		AddWarning(TEXT("Climb has no original source backup; native rebake check skipped"));
		return true;
	}
	UAnimSequence* Backup = LoadObject<UAnimSequence>(nullptr, *BackupPath);
	UAnimBlueprint* Original = LoadObject<UAnimBlueprint>(nullptr, TEXT("/Game/KawaiiBakes/ABP_KawaiiBake"));
	if (!TestTrue(TEXT("Climb bake assets load"), Backup && Original)) {
		return false;
	}
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Temp/KawaiiTailBake_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	Package->SetFlags(RF_Transient);
	UAnimBlueprint* Copy = DuplicateObject<UAnimBlueprint>(Original, Package, TEXT("ABP_TailBakeTest"));
	UKawaiiPhysicsBakeSettings* Settings = KawaiiPhysicsBake::GetSettings(Copy);
	if (!TestNotNull(TEXT("Bake settings"), Settings)) {
		return false;
	}
	Settings->SourceAnimation = Backup;
	Settings->bPreviewClothingProxyCollision = false;
	// This check records the native bake pose, without feeding runtime correction back into it.
	ADD_LATENT_AUTOMATION_COMMAND(FKawaiiTailBakeCommand(this, Copy));
	return true;
}

#endif
