#include "AnimGraphNode_KawaiiPhysics.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiPhysicsCalibrationSession.h"
#include "KawaiiPhysicsCalibrationSettings.h"
#include "KawaiiPhysicsCalibrationSearch.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace {
	FString CalibrationNodeText(const FAnimNode_KawaiiPhysics& Node) {
		FString Text;
		FAnimNode_KawaiiPhysics::StaticStruct()->ExportText(Text, &Node, nullptr, nullptr, PPF_None, nullptr);
		return Text;
	}

	class FCalibrationProjectReplay : public IAutomationLatentCommand
	{
	public:
		FCalibrationProjectReplay(FAutomationTestBase* InTest, UKawaiiPhysicsCalibrationSettings* InSettings, FString InOutput)
			: Test(InTest), Settings(InSettings), Output(MoveTemp(InOutput)) {
			Original = CalibrationNodeText(Settings->FindPhysicsNode()->Node);
		}

		virtual bool Update() override {
			if (!Session) {
				Session = MakeUnique<FKawaiiPhysicsCalibrationSession>();
				FString Error;
				if (!Session->Start(Settings.Get(), Error)) {
					Test->AddError(Error);
					return true;
				}
				Started = FPlatformTime::Seconds();
			}
			Session->Tick();
			if (Session->IsRunning()) {
				if (FPlatformTime::Seconds() - Started > 300.) {
					Test->AddError(TEXT("Native calibration replay exceeded five minutes."));
					Session->Cancel();
					return true;
				}
				return false;
			}
			Test->TestEqual(TEXT("A complete search does not modify the source node"), CalibrationNodeText(Settings->FindPhysicsNode()->Node), Original);
			FString Json;
			TSharedPtr<FJsonObject> Report;
			if (!FFileHelper::LoadFileToString(Json, *Session->GetReportPath()) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report)
				|| !Report)
			{
				Test->AddError(TEXT("Native replay did not produce a complete report: ") + Session->GetStatus());
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!Report->TryGetArrayField(TEXT("cpu_finalists"), Rows) || Rows->Num() < 2) {
				Test->AddError(TEXT("Expected original baseline and native search candidates."));
				return true;
			}
			Test->TestEqual(TEXT("Original baseline is evaluated first"), (*Rows)[0]->AsObject()->GetIntegerField(TEXT("candidate")), 0);
			if (Run == 0) {
				First = Report;
			} else {
				const auto& FirstRows = First->GetArrayField(TEXT("cpu_finalists"));
				Test->TestEqual(TEXT("Repeated native searches evaluate the same candidates"), Rows->Num(), FirstRows.Num());
				Test->TestEqual(
					TEXT("Repeated native searches select the same result"),
					Report->GetIntegerField(TEXT("selected_candidate")),
					First->GetIntegerField(TEXT("selected_candidate"))
				);
				for (int32 I = 0; I < FMath::Min(Rows->Num(), FirstRows.Num()); ++I) {
					const auto A = FirstRows[I]->AsObject(), B = (*Rows)[I]->AsObject();
					Test->TestEqual(TEXT("Candidate order remains stable"), A->GetIntegerField(TEXT("candidate")), B->GetIntegerField(TEXT("candidate")));
					for (const TCHAR* Split : {TEXT("training"), TEXT("validation")}) {
						for (const TCHAR* Metric : {TEXT("acceptance_penetration_cm"), TEXT("mean_squared_penetration"), TEXT("max_length_error_cm")}) {
							const double X = A->GetObjectField(Split)->GetNumberField(Metric), Y = B->GetObjectField(Split)->GetNumberField(Metric);
							Test->TestTrue(
								FString::Printf(TEXT("%s %s is finite and repeatable"), Split, Metric),
								FMath::IsFinite(X) && FMath::IsFinite(Y) && FMath::Abs(X - Y) < 1.e-5
							);
						}
					}
				}
			}
			Reports.Add(MakeShared<FJsonValueString>(Session->GetReportPath()));
			Test->AddInfo(TEXT("Native replay report: ") + Session->GetReportPath());
			if (++Run < 2) {
				Session.Reset();
				return false;
			}
			if (!Output.IsEmpty()) {
				auto Manifest = MakeShared<FJsonObject>();
				Manifest->SetArrayField(TEXT("reports"), Reports);
				FString Text;
				FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&Text));
				Test->TestTrue(TEXT("Replay report manifest is saved"), FFileHelper::SaveStringToFile(Text, *Output));
			}
			return true;
		}

	private:
		FAutomationTestBase* Test;
		TStrongObjectPtr<UKawaiiPhysicsCalibrationSettings> Settings;
		TUniquePtr<FKawaiiPhysicsCalibrationSession> Session;
		TSharedPtr<FJsonObject> First;
		TArray<TSharedPtr<FJsonValue>> Reports;
		FString Output, Original;
		int32 Run = 0;
		double Started = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiCalibrationProjectReplayTest,
	"KawaiiPhysicsBaker.Calibration.Integration.NativeProjectReplay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiCalibrationProjectReplayTest::RunTest(const FString&) {
	FString ConfigPath, Json;
	if (!FParse::Value(FCommandLine::Get(), TEXT("KawaiiCalibrationReplayConfig="), ConfigPath)) {
		AddInfo(TEXT(
			"Project replay is optional: pass -KawaiiCalibrationReplayConfig=<JSON> with body_mesh, clothing_mesh, blueprint, root and motions. No assets are saved or modified."
		));
		return true;
	}
	TSharedPtr<FJsonObject> Config;
	if (!FFileHelper::LoadFileToString(Json, *ConfigPath) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Config) || !Config) {
		AddError(TEXT("Could not read calibration replay JSON."));
		return false;
	}
	auto* Settings = NewObject<UKawaiiPhysicsCalibrationSettings>();
	Settings->BodyMesh = LoadObject<USkeletalMesh>(nullptr, *Config->GetStringField(TEXT("body_mesh")));
	Settings->ClothingMesh = LoadObject<USkeletalMesh>(nullptr, *Config->GetStringField(TEXT("clothing_mesh")));
	Settings->ClothingBlueprint = LoadObject<UAnimBlueprint>(nullptr, *Config->GetStringField(TEXT("blueprint")));
	Settings->PhysicsRoot = FName(*Config->GetStringField(TEXT("root")));
	Settings->bSearchNodeSettings = true;
	Settings->NativeCandidateCount = Config->HasField(TEXT("candidate_count")) ? Config->GetIntegerField(TEXT("candidate_count")) : 64;
	Settings->WarmUpFrames = 30;
	if (Config->HasField(TEXT("clothing_transform"))) {
		Settings->ClothingTransform.InitFromString(Config->GetStringField(TEXT("clothing_transform")));
	}
	for (const auto& Entry : Config->GetArrayField(TEXT("motions"))) {
		const auto Object = Entry->AsObject();
		auto& Motion = Settings->Motions.AddDefaulted_GetRef();
		Motion.Animation = LoadObject<UAnimSequence>(nullptr, *Object->GetStringField(TEXT("animation")));
		Motion.bResetBefore = true;
		Motion.bValidationOnly = Object->HasField(TEXT("validation_only")) && Object->GetBoolField(TEXT("validation_only"));
	}
	if (!Settings->FindPhysicsNode()) {
		AddError(TEXT("Replay blueprint/root does not resolve to one source node."));
		return false;
	}
	FString Output;
	Config->TryGetStringField(TEXT("report_manifest"), Output);
	ADD_LATENT_AUTOMATION_COMMAND(FCalibrationProjectReplay(this, Settings, Output));
	return true;
}
#endif
