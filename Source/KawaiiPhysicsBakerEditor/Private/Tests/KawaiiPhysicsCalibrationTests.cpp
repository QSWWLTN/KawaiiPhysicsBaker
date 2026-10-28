#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "KawaiiPhysicsContactMath.h"
#include "KawaiiPhysicsCalibrationProfile.h"
#include "AnimGraphNode_KawaiiPhysics.h"

#include "KawaiiCalibrationGPU.h"
#include "KawaiiPhysicsCalibrationSession.h"
#include "KawaiiPhysicsCalibrationSettings.h"
#include "KawaiiPhysicsIKCorrectionProfile.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiContactTest, "KawaiiPhysicsBaker.Calibration.CPUContact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiContactTest::RunTest(const FString&) {
	FVector A(0, 0, 0), B(10, 0, 0), PA = A, PB = B;
	KawaiiContact::FCapsule C{FVector(5, -1, 0), FVector(5, -1, 0), 2};
	FVector N;
	double T;
	const double Before = KawaiiContact::SegmentContact(A, B, C, 0, FVector::UpVector, N, T);
	for (int32 I = 0; I < 8; ++I) {
		KawaiiContact::ProjectSegment(A, B, PA, PB, true, false, C, 0, FVector::UpVector, 0, 0);
	}
	TestTrue(TEXT("Interior edge contact is reduced, not just endpoint collision"), KawaiiContact::SegmentContact(A, B, C, 0, FVector::UpVector, N, T) < Before);
	TestEqual(TEXT("Anchor remains fixed"), A, FVector::ZeroVector);
	TestTrue(TEXT("Projection does not create artificial velocity"), (B - PB).IsNearlyZero());
	FVector X = FVector::ZeroVector, Y(1, 0, 0), PX = X, PY = Y;
	KawaiiContact::FCapsule Anchor{X, X, 3};
	TestFalse(TEXT("An immovable penetrating root is reported as unsolvable"), KawaiiContact::ProjectSegment(X, Y, PX, PY, true, false, Anchor, 0, FVector::UpVector, 0, 0));
	TestFalse(TEXT("Degenerate contact never produces NaN"), Y.ContainsNaN());
	KawaiiContact::FCapsule AxisCapsule{FVector(0, 0, -10), FVector(0, 0, 10), 2};
	KawaiiContact::SegmentContact(FVector(0, 0, -5), FVector(0, 0, 5), AxisCapsule, 0, FVector::UpVector, N, T);
	TestTrue(TEXT("Coincident capsule axes choose a radial correction, not axial stretching"), FMath::Abs(N.Z) < 1.e-6 && !N.IsNearlyZero());
	const double PointDepth = KawaiiContact::SegmentContact(FVector::ZeroVector, FVector::ZeroVector, AxisCapsule, 0., FVector::UpVector, N, T);
	TestTrue(TEXT("A skin point on the capsule axis receives a finite radial correction"), FMath::IsNearlyEqual(PointDepth, 2.) && !N.ContainsNaN() && FMath::Abs(N.Z) < 1.e-6 && !N.IsNearlyZero());
	return true;
}

namespace {
	class FCheckCalibrationGPU : public IAutomationLatentCommand
	{
		FAutomationTestBase* Test;
		TSharedPtr<FKawaiiCalibrationGPU, ESPMode::ThreadSafe> Task;
		FVector4f Expected;
		bool bCheckFullScore = false;
		double Start = FPlatformTime::Seconds();

	public:
		FCheckCalibrationGPU(FAutomationTestBase* In, TSharedPtr<FKawaiiCalibrationGPU, ESPMode::ThreadSafe> GPU, FVector4f Reference, bool bInCheckFullScore = false) : Test(In), Task(GPU), Expected(Reference), bCheckFullScore(bInCheckFullScore) {}

		virtual bool Update() override {
			Task->Tick();
			if (!Task->IsDone()) {
				if (FPlatformTime::Seconds() - Start > 90) {
					Test->AddError(TEXT("Real GPU calibration timed out."));
					return true;
				}
				return false;
			}
			if (!Task->GetError().IsEmpty()) {
				Test->AddError(Task->GetError());
				return true;
			}
			const auto& Scores = Task->GetScores();
			Test->TestEqual(TEXT("All candidates return scores"), Scores.Num(), 2);
			if (Scores.Num() == 2) {
				if (bCheckFullScore) {
					Test->TestTrue(TEXT("Changing solver radius cannot improve the score of unchanged geometry"), Scores[0].Equals(Expected, .001f) && Scores[1].Equals(Expected, .001f));
				}
				Test->TestTrue(TEXT("GPU time integration matches independent CPU reference"), FMath::Abs(Scores[0].Z - Expected.Z) < .02f && FMath::Abs(Scores[0].W - Expected.W) < .02f);
				Test->TestTrue(TEXT("Duplicate candidates are deterministic"), Scores[0].Equals(Scores[1], .001f));
				Test->AddInfo(FString::Printf(TEXT("GPU score z/w %.6f %.6f; CPU reference %.6f %.6f"), Scores[0].Z, Scores[0].W, Expected.Z, Expected.W));
			}
			return true;
		}
	};

	class FRunCalibrationAssets : public IAutomationLatentCommand
	{
		FAutomationTestBase* Test;
		TSharedPtr<FKawaiiPhysicsCalibrationSession> Session;
		double Start = FPlatformTime::Seconds();

	public:
		FRunCalibrationAssets(FAutomationTestBase* In, TSharedPtr<FKawaiiPhysicsCalibrationSession> S) : Test(In), Session(S) {}

		virtual bool Update() override {
			Session->Tick();
			if (Session->IsRunning()) {
				if (FPlatformTime::Seconds() - Start > 600) {
					Session->Cancel();
					Test->AddError(TEXT("Real mesh calibration timed out."));
					return true;
				}
				return false;
			}
			if (!Session->GetResult()) {
				Test->AddError(Session->GetStatus());
				return true;
			}
			Test->TestTrue(TEXT("A runtime profile is produced"), Session->GetResult()->BodyCapsules.Num() > 0);
			Test->TestTrue(TEXT("Report exists"), FPaths::FileExists(Session->GetReportPath()));
			Test->AddInfo(Session->GetStatus());
			Test->AddInfo(Session->GetReportPath());
			if (FParse::Param(FCommandLine::Get(), TEXT("KawaiiCalibrationSaveExample"))) {
				FString Error;
				Test->TestTrue(TEXT("Save new example profile"), Session->SaveResult(Error));
				if (!Error.IsEmpty()) {
					Test->AddError(Error);
				} else {
					Test->AddInfo(Session->GetResult()->GetPathName());
				}
			}
			return true;
		}
	};

	class FCheckStrafeJogCalibration : public IAutomationLatentCommand
	{
		FAutomationTestBase* Test;
		TSharedPtr<FKawaiiPhysicsCalibrationSession> Session;
		bool bOldCapsules;
		double Start = FPlatformTime::Seconds();

	public:
		FCheckStrafeJogCalibration(FAutomationTestBase* InTest, TSharedPtr<FKawaiiPhysicsCalibrationSession> InSession, bool bInOldCapsules) : Test(InTest), Session(InSession), bOldCapsules(bInOldCapsules) {}

		virtual bool Update() override {
			Session->Tick();
			if (Session->IsRunning()) {
				if (FPlatformTime::Seconds() - Start > 180) {
					Session->Cancel();
					Test->AddError(TEXT("StrafeJog calibration timed out"));
					return true;
				}
				return false;
			}
			Test->AddInfo(Session->GetStatus());
			Test->AddInfo(Session->GetReportPath());
			FString Json;
			TSharedPtr<FJsonObject> Report;
			if (!FFileHelper::LoadFileToString(Json, *Session->GetReportPath()) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report)) {
				Test->AddError(TEXT("Missing StrafeJog diagnostic report"));
				return true;
			}
			Test->TestEqual(TEXT("Report acceptance matches profile availability"), Report->GetBoolField(TEXT("accepted")), Session->GetResult() != nullptr);
			if (bOldCapsules) {
				Test->TestNull(TEXT("Severe penetration cannot produce an applicable profile"), Session->GetResult());
			} else {
				Test->TestEqual(TEXT("Existing clothing limits take precedence over unrelated physics bodies"), Report->GetStringField(TEXT("collision_source")), FString(TEXT("Kawaii 节点及约束资产")));
				Test->TestNotNull(TEXT("StrafeJog meets penetration and length limits"), Session->GetResult());
				if (Session->GetResult() && FParse::Param(FCommandLine::Get(), TEXT("KawaiiCalibrationSaveExample"))) {
					FString Error;
					Test->TestTrue(TEXT("Save verified profile to a new asset"), Session->SaveResult(Error));
					Test->AddInfo(Error.IsEmpty() ? Session->GetResult()->GetPathName() : Error);
				}
			}
			return true;
		}
	};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiGPUFixedEnvelopeTest, "KawaiiPhysicsBaker.Calibration.GPUFixedScoringEnvelope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiGPUFixedEnvelopeTest::RunTest(const FString&) {
	FKawaiiCalibrationGPUInput Input;
	Input.Gravity = FVector3f::ZeroVector;
	Input.SoftMargin = 0.f;
	Input.Bones = {FVector4f(-1, 0, 0, 0), FVector4f(0, 0, 1, 1)};
	Input.Candidates = {FVector4f(1, 1, .5, 2), FVector4f(1, 1, 2, 2)};
	Input.CapsuleCount = 1;
	for (int32 Frame = 0; Frame < 8; ++Frame) {
		Input.Frames.Add(FVector4f(Frame == 0 ? 1 : 0, 1, 0, 0));
		Input.Poses.Append({FVector4f(0, 0, 0, 0), FVector4f(10, 0, 0, 0)});
		Input.Capsules.Append({FVector4f(0, 0, 0, 2), FVector4f(0, 0, 0, 0)});
	}
	FString Error;
	auto GPU = FKawaiiCalibrationGPU::Start(Input, Error);
	if (!GPU) {
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FCheckCalibrationGPU(this, GPU, FVector4f(72, 3, 0, 0), true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiStrafeJogCalibrationTest, "KawaiiPhysicsBaker.Calibration.StrafeJogRegression", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiStrafeJogCalibrationTest::RunTest(const FString&) {
	if (!FParse::Param(FCommandLine::Get(), TEXT("KawaiiCalibrationTestAssets"))) {
		AddWarning(TEXT("StrafeJog asset replay skipped; pass -KawaiiCalibrationTestAssets."));
		return true;
	}
	TStrongObjectPtr<UKawaiiPhysicsCalibrationSettings> Settings(NewObject<UKawaiiPhysicsCalibrationSettings>());
	Settings->BodyMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Anime/Lime/Lime.Lime"));
	Settings->ClothingMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Clothing/Base/Quesgear_lime_ver3.Quesgear_lime_ver3"));
	auto* Source = LoadObject<UAnimBlueprint>(nullptr, TEXT("/Game/BP/AnimeBP/Clothing/Base/AnimBP_Clothing_Base.AnimBP_Clothing_Base"));
	auto* Animation = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/StrafeJog/StrafeJog_F.StrafeJog_F"));
	if (!Settings->BodyMesh || !Settings->ClothingMesh || !Source || !Animation) {
		AddError(TEXT("StrafeJog regression assets are missing"));
		return false;
	}
	Settings->ClothingBlueprint = Source;
	Settings->PhysicsRoot = TEXT("Skirt_Back_003_L");
	Settings->ClothingTransform = FTransform(FQuat::Identity, FVector(0, 0, -1), FVector(1.01));
	const bool bOldCapsules = FParse::Param(FCommandLine::Get(), TEXT("KawaiiCalibrationReplayOldCapsules"));
	if (bOldCapsules) {
		auto* OldProfile = LoadObject<UKawaiiPhysicsCalibrationProfile>(nullptr, TEXT("/Game/KawaiiCalibration/DA_KawaiiCalibration_EB41DFAF408F.DA_KawaiiCalibration_EB41DFAF408F"));
		if (!OldProfile) {
			AddError(TEXT("Original reported profile is missing"));
			return false;
		}
		Settings->BodyCapsules = OldProfile->BodyCapsules;
	}
	FKawaiiCalibrationMotion Motion;
	Motion.Animation = Animation;
	Motion.bResetBefore = true;
	Settings->Motions.Add(Motion);
	FString ReplayReport;
	if (FParse::Value(FCommandLine::Get(), TEXT("KawaiiCalibrationReplayReport="), ReplayReport)) {
		FString Json;
		TSharedPtr<FJsonObject> Report;
		if (!FFileHelper::LoadFileToString(Json, *ReplayReport) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Report)) {
			AddError(TEXT("Could not read the original multi-motion report"));
			return false;
		}
		Settings->Motions.Reset();
		for (const auto& Value : Report->GetArrayField(TEXT("motions"))) {
			const auto Row = Value->AsObject();
			FKawaiiCalibrationMotion Replay;
			Replay.Animation = LoadObject<UAnimSequence>(nullptr, *Row->GetStringField(TEXT("animation")));
			Replay.PlayRate = Row->GetNumberField(TEXT("play_rate"));
			Replay.BlendInSeconds = Row->GetNumberField(TEXT("blend_seconds"));
			Replay.bResetBefore = Row->GetBoolField(TEXT("reset"));
			Replay.bValidationOnly = Row->GetBoolField(TEXT("held_out"));
			Settings->Motions.Add(Replay);
		}
		FKawaiiCalibrationMotion HeldOut;
		HeldOut.Animation = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/WalkStart/Walk_Start_R45.Walk_Start_R45"));
		HeldOut.bResetBefore = true;
		HeldOut.bValidationOnly = true;
		Settings->Motions.Add(HeldOut);
	}
	auto Session = MakeShared<FKawaiiPhysicsCalibrationSession>();
	FString Error;
	if (!Session->Start(Settings.Get(), Error)) {
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FCheckStrafeJogCalibration(this, Session, bOldCapsules));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiGPUCalibrationTest, "KawaiiPhysicsBaker.Calibration.GPUSequentialSimulation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiGPUCalibrationTest::RunTest(const FString&) {
	FKawaiiCalibrationGPUInput Input;
	Input.Gravity = FVector3f(0, 0, -50);
	Input.SoftMargin = 0;
	Input.Bones = {FVector4f(-1, 0, 0, 0), FVector4f(0, .1, .2, 0)};
	Input.Candidates = {FVector4f(1, 1, 1, 1), FVector4f(1, 1, 1, 1)};
	FVector3f Position(10, 0, 0), Previous = Position, OldVelocity = FVector3f::ZeroVector;
	FVector4f Expected(0, 0, 0, 0);
	for (int32 Frame = 0; Frame < 40; ++Frame) {
		Input.Frames.Add(FVector4f(Frame == 0 ? 1 : 0, 1, 0, 0));
		Input.Poses.Add(FVector4f(0, 0, 0, 0));
		Input.Poses.Add(FVector4f(10, 0, 0, 0));
		const FVector3f Old = Position;
		Position = Old + (Old - Previous) * .9f + Input.Gravity * Input.DeltaTime * Input.DeltaTime;
		Position = FMath::Lerp(Position, FVector3f(10, 0, 0), .2f);
		Previous = Old;
		const FVector3f Before = Position;
		Position = Position.GetSafeNormal() * 10.f;
		Previous += Position - Before;
		const FVector3f Velocity = Position - Previous;
		Expected.Z += (Position - FVector3f(10, 0, 0)).SizeSquared();
		Expected.W += (Velocity - OldVelocity).SizeSquared();
		OldVelocity = Velocity;
	}
	FString Error;
	auto GPU = FKawaiiCalibrationGPU::Start(Input, Error);
	if (!GPU) {
		if (FParse::Param(FCommandLine::Get(), TEXT("KawaiiCalibrationRequireGPU"))) {
			AddError(Error);
		} else {
			AddWarning(TEXT("GPU test skipped: ") + Error);
		}
		return !HasAnyErrors();
	}
	ADD_LATENT_AUTOMATION_COMMAND(FCheckCalibrationGPU(this, GPU, Expected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiCalibrationAssetTest, "KawaiiPhysicsBaker.Calibration.LimeBaseGPUAndCPU", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiCalibrationAssetTest::RunTest(const FString&) {
	if (!FParse::Param(FCommandLine::Get(), TEXT("KawaiiCalibrationTestAssets"))) {
		AddWarning(TEXT("Actual mesh calibration skipped; pass -KawaiiCalibrationTestAssets."));
		return true;
	}
	TStrongObjectPtr<UKawaiiPhysicsCalibrationSettings> Settings(NewObject<UKawaiiPhysicsCalibrationSettings>());
	Settings->BodyMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Anime/Lime/Lime.Lime"));
	Settings->ClothingMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Clothing/Base/Quesgear_lime_ver3.Quesgear_lime_ver3"));
	Settings->ClothingBlueprint = LoadObject<UAnimBlueprint>(nullptr, TEXT("/Game/BP/AnimeBP/Clothing/Base/AnimBP_Clothing_Base.AnimBP_Clothing_Base"));
	if (!Settings->BodyMesh || !Settings->ClothingMesh || !Settings->ClothingBlueprint) {
		AddError(TEXT("Lime/Base calibration fixtures are missing."));
		return false;
	}
	TArray<UEdGraph*> Graphs;
	Settings->ClothingBlueprint->GetAllGraphs(Graphs);
	UAnimGraphNode_KawaiiPhysics* Chosen = nullptr;
	for (auto* Graph : Graphs) {
		for (auto Node : Graph->Nodes) {
			if (auto* K = Cast<UAnimGraphNode_KawaiiPhysics>(Node)) {
				AddInfo(FString::Printf(
				    TEXT("Available root %s; additional roots %d; world %d; wind %d; forces %d; shared %d; space %d; legacy gravity %d"),
				    *K->Node.RootBone.BoneName.ToString(), K->Node.AdditionalRootBones.Num(), K->Node.bAllowWorldCollision,
				    K->Node.bEnableWind, K->Node.ExternalForces.Num(), K->Node.bUseSharedCollision, int32(K->Node.SimulationSpace),
				    K->Node.bUseLegacyGravity));
				if (!Chosen || K->Node.RootBone.BoneName.ToString().Contains(TEXT("skirt"), ESearchCase::IgnoreCase)) {
					Chosen = K;
				}
			}
		}
	}
	if (!Chosen) {
		AddError(TEXT("No Kawaii node in clothing fixture."));
		return false;
	}
	Settings->PhysicsRoot = Chosen->Node.RootBone.BoneName;
	Settings->ClothingTransform = FTransform(FQuat::Identity, FVector(0, 0, -1), FVector(1.01));
	Settings->CandidateCount = 16;
	Settings->CPUFinalists = 2;
	Settings->WarmUpFrames = 8;
	Settings->SkinSampleCount = 64;
	if (auto* Profile = LoadObject<UKawaiiPhysicsIKCorrectionProfile>(nullptr, TEXT("/Game/KawaiiBakes/IKValidation/DA_IKCorrection_LimeBase.DA_IKCorrection_LimeBase"))) {
		for (const auto& C : Profile->Capsules) {
			FKawaiiClothingCapsuleDefinition D;
			D.StartBone = C.StartBone.BoneName;
			D.EndBone = C.EndBone.BoneName;
			D.StartOffset = C.StartOffset;
			D.EndOffset = C.EndOffset;
			D.Radius = C.Radius;
			Settings->BodyCapsules.Add(D);
		}
	}
	const TCHAR* Paths[] = {TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/WalkStart/Walk_Start_R0.Walk_Start_R0"), TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/WalkStop/Walk_Stop_Rfoot.Walk_Stop_Rfoot")};
	for (const TCHAR* Path : Paths) {
		FKawaiiCalibrationMotion M;
		M.Animation = LoadObject<UAnimSequence>(nullptr, Path);
		Settings->Motions.Add(M);
	}
	FKawaiiCalibrationMotion Stress = Settings->Motions[0];
	Stress.Animation = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/WalkStart/Walk_Start_R45.Walk_Start_R45"));
	Stress.bValidationOnly = true;
	Stress.bResetBefore = true;
	if (Settings->BodyCapsules.Num()) {
		Stress.OffsetBone = Settings->BodyCapsules[0].StartBone;
		Stress.LocalRotation = FRotator(15, 0, 0);
	}
	Settings->Motions.Add(Stress);
	auto Session = MakeShared<FKawaiiPhysicsCalibrationSession>();
	FString Error;
	if (!Session->Start(Settings.Get(), Error)) {
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FRunCalibrationAssets(this, Session));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiShapeAuthoringTest, "KawaiiPhysicsBaker.Calibration.ShapeAuthoring", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiShapeAuthoringTest::RunTest(const FString&) {
	if (!FParse::Param(FCommandLine::Get(), TEXT("KawaiiCalibrationTestAssets"))) {
		AddWarning(TEXT("Shape authoring asset test skipped; pass -KawaiiCalibrationTestAssets."));
		return true;
	}
	TStrongObjectPtr<UKawaiiPhysicsCalibrationSettings> Settings(NewObject<UKawaiiPhysicsCalibrationSettings>());
	Settings->BodyMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Anime/Lime/Lime.Lime"));
	Settings->ClothingMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Clothing/Base/Quesgear_lime_ver3.Quesgear_lime_ver3"));
	Settings->ClothingBlueprint = LoadObject<UAnimBlueprint>(nullptr, TEXT("/Game/BP/AnimeBP/Clothing/Base/AnimBP_Clothing_Base.AnimBP_Clothing_Base"));
	Settings->PhysicsRoot = TEXT("Skirt_Back_003_L");
	FString Message, Source;
	if (!Settings->ResolveConstraints(Source, Message)) {
		AddError(Message);
		return false;
	}
	const auto Original = Settings->BodyCapsules;
	const auto* Node = Settings->FindPhysicsNode();
	const auto* OriginalProfile = Node->Node.CalibrationProfile.Get();
	const float OriginalAngle = Node->Node.PhysicsSettings.LimitAngle;
	TestTrue(TEXT("Read original node and asset shapes"), Settings->ReadCollisionAndChains(Message));
	TestEqual(TEXT("Read all main and additional chains"), Settings->ChainLimits.Num(), 1 + Node->Node.AdditionalRootBones.Num());
	TestTrue(TEXT("Resolve imported shapes"), Settings->ResolveConstraints(Source, Message));
	TestEqual(TEXT("Import preserves collider count"), Original.Num(), Settings->BodyCapsules.Num());
	for (int32 I = 0; I < Original.Num(); ++I) {
		TestTrue(TEXT("Center/rotation editing preserves imported geometry"), Original[I].StartOffset.Equals(Settings->BodyCapsules[I].StartOffset, .0001) && Original[I].EndOffset.Equals(Settings->BodyCapsules[I].EndOffset, .0001));
	}
	if (Settings->CollisionShapes.Num() < 2 || Settings->ChainLimits.IsEmpty()) {
		AddError(TEXT("Expected multiple body shapes and skirt chains."));
		return false;
	}
	auto& Shape = Settings->CollisionShapes[0];
	Shape.Center = FVector(1, 2, 3);
	Shape.Rotation = FRotator(90, 0, 0);
	Shape.Length = 8.f;
	Shape.Radius = 2.f;
	Settings->CollisionShapes.Last().bEnabled = false;
	Settings->ChainLimits[0].bOverrideAngle = true;
	Settings->ChainLimits[0].LimitAngle = 60.f;
	Settings->ChainLimits[0].bOverridePlane = true;
	Settings->ChainLimits[0].PlanarConstraint = EPlanarConstraint::None;
	FKawaiiPhysicsCalibrationSession Session;
	if (!Session.CreateManualProfile(Settings.Get(), Message)) {
		AddError(Message);
		return false;
	}
	const auto* Profile = Session.GetResult();
	TestEqual(TEXT("Disabled shape is omitted"), Profile->BodyCapsules.Num(), Original.Num() - 1);
	const auto& Capsule = Profile->BodyCapsules[0];
	TestTrue(TEXT("Edited center reaches runtime profile"), ((Capsule.StartOffset + Capsule.EndOffset) * .5).Equals(Shape.Center));
	TestTrue(TEXT("Edited axis and length reach runtime profile"), (Capsule.EndOffset - Capsule.StartOffset).Equals(Shape.Rotation.Quaternion().GetAxisZ() * 8.));
	TestEqual(TEXT("Edited radius reaches runtime profile"), Capsule.Radius, 2.f);
	TestEqual(TEXT("Edited angle reaches runtime profile"), Profile->ChainLimits[0].LimitAngle, 60.f);
	TestTrue(TEXT("Replacement survives disabled shape"), Profile->ReplacedCollisionBones.Contains(Settings->CollisionShapes.Last().Bone));
	TestTrue(TEXT("Manual result is explicitly unverified"), Profile->ValidationSummary.Contains(TEXT("未经")) && Profile->SurfacePoints.IsEmpty());
	TestTrue(TEXT("Authoring does not bind source blueprint"), Node->Node.CalibrationProfile.Get() == OriginalProfile);
	TestEqual(TEXT("Authoring does not modify source angle"), Node->Node.PhysicsSettings.LimitAngle, OriginalAngle);
	for (auto& Entry : Settings->CollisionShapes) {
		Entry.bEnabled = false;
	}
	TestFalse(TEXT("All disabled is rejected without auto-import fallback"), Settings->ResolveConstraints(Source, Message));
	Shape.bEnabled = true;
	Shape.Bone = TEXT("MissingBone");
	TestFalse(TEXT("Wrong skeleton is rejected"), Settings->ResolveConstraints(Source, Message));
	Shape.Bone = Original[0].StartBone;
	const auto DuplicateLimit = Settings->ChainLimits[0];
	Settings->ChainLimits.Add(DuplicateLimit);
	TestFalse(TEXT("Duplicate chain overrides are rejected"), Settings->ResolveConstraints(Source, Message));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiGPURangeTest, "KawaiiPhysicsBaker.Calibration.GPUChainRange", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKawaiiGPURangeTest::RunTest(const FString&) {
	FKawaiiCalibrationGPUInput Input;
	Input.Gravity = FVector3f(0, 20000, -20000);
	Input.SoftMargin = 0.f;
	Input.Bones = {FVector4f(-1, 0, 0, 0), FVector4f(0, 0, 0, 0)};
	Input.Candidates = {FVector4f(1, 1, 1, 1), FVector4f(1, 1, 1, 1)};
	Input.Frames = {FVector4f(1, 1, 0, 0)};
	Input.Poses = {FVector4f(0, 0, 0, 0), FVector4f(10, 0, 0, 0)};
	Input.Ranges = {FVector4f(0, 0, 0, -1), FVector4f(0, 1, 0, 10)};
	const float Deviation = 200.f * (1.f - FMath::Cos(FMath::DegreesToRadians(10.f)));
	const float Jitter = (Input.Gravity * Input.DeltaTime * Input.DeltaTime).SizeSquared();
	FString Error;
	auto GPU = FKawaiiCalibrationGPU::Start(Input, Error);
	if (!GPU) {
		AddError(Error);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FCheckCalibrationGPU(this, GPU, FVector4f(0, 0, Deviation, Jitter)));
	return true;
}
#endif
