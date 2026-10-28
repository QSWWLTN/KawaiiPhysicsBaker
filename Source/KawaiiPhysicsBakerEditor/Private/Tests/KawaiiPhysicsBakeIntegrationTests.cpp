#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "KawaiiPhysicsBakeFactory.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeSession.h"
#include "AnimGraphNode_KawaiiPhysicsBakeSource.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_LocalToComponentSpace.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "AnimNode_KawaiiPhysics.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "EdGraph/EdGraph.h"
#include "Engine/SkeletalMesh.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	UEdGraphPin* PosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction) {
		for (UEdGraphPin* Pin : Node->Pins) {
			if (Pin->Direction == Direction
				&& (Pin->PinType.PinSubCategoryObject == FPoseLink::StaticStruct() || Pin->PinType.PinSubCategoryObject == FComponentSpacePoseLink::StaticStruct()))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool AddPhysicsToGraph(UAnimBlueprint* Blueprint, USkeletalMesh* Mesh) {
		UClass* KawaiiClass = LoadClass<UEdGraphNode>(nullptr, TEXT("/Script/KawaiiPhysicsEd.AnimGraphNode_KawaiiPhysics"));
		if (!KawaiiClass) {
			return false;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs) {
			UEdGraphNode* Source = nullptr;
			UEdGraphNode* Root = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes) {
				if (Node->IsA<UAnimGraphNode_KawaiiPhysicsBakeSource>()) {
					Source = Node;
				}
				if (Node->IsA<UAnimGraphNode_Root>()) {
					Root = Node;
				}
			}
			if (!Source || !Root) {
				continue;
			}
			FGraphNodeCreator<UAnimGraphNode_LocalToComponentSpace> ToCSCreator(*Graph);
			UEdGraphNode* ToCS = ToCSCreator.CreateNode();
			ToCSCreator.Finalize();
			FGraphNodeCreator<UAnimGraphNode_ComponentToLocalSpace> ToLocalCreator(*Graph);
			UEdGraphNode* ToLocal = ToLocalCreator.CreateNode();
			ToLocalCreator.Finalize();
			FGraphNodeCreator<UEdGraphNode> KawaiiCreator(*Graph);
			UEdGraphNode* Kawaii = KawaiiCreator.CreateNode(false, KawaiiClass);
			KawaiiCreator.Finalize();
			FStructProperty* Property = FindFProperty<FStructProperty>(KawaiiClass, TEXT("Node"));
			if (!Property) {
				return false;
			}
			FAnimNode_KawaiiPhysics& Runtime = *Property->ContainerPtrToValuePtr<FAnimNode_KawaiiPhysics>(Kawaii);
			const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
			const int32 Parent = Ref.GetParentIndex(Ref.GetNum() - 1);
			if (Parent < 0) {
				return false;
			}
			Runtime.RootBone.BoneName = Ref.GetBoneName(Parent);
			Runtime.DummyBoneLength = 5.f;
			Runtime.bEnableWind = false;
			Runtime.bAllowWorldCollision = false;
			Runtime.Gravity = FVector(0, 0, -980);
			PosePin(Root, EGPD_Input)->BreakAllPinLinks();
			const UEdGraphSchema* Schema = Graph->GetSchema();
			if (!Schema->TryCreateConnection(PosePin(Source, EGPD_Output), PosePin(ToCS, EGPD_Input))
				|| !Schema->TryCreateConnection(PosePin(ToCS, EGPD_Output), PosePin(Kawaii, EGPD_Input))
				|| !Schema->TryCreateConnection(PosePin(Kawaii, EGPD_Output), PosePin(ToLocal, EGPD_Input))
				|| !Schema->TryCreateConnection(PosePin(ToLocal, EGPD_Output), PosePin(Root, EGPD_Input)))
			{
				return false;
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings;
		}
		return false;
	}

	class FRunModularBake : public IAutomationLatentCommand
	{
	public:
		FRunModularBake(FAutomationTestBase* InTest, UAnimBlueprint* InBody, UAnimBlueprint* InClothing)
			: Test(InTest), Body(InBody), Clothing(InClothing), Session(MakeShared<FKawaiiPhysicsBakeSession>()) {
			Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("KawaiiPhysicsBaker/Automation"));
			IFileManager::Get().MakeDirectory(*Directory, true);
			FPackageName::RegisterMountPoint(TEXT("/KawaiiBakeTests/"), Directory + TEXT("/"));
			KawaiiPhysicsBake::GetSettings(Body.Get())->OutputDirectory.Path = TEXT("/KawaiiBakeTests/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			OriginalSource.Reset(KawaiiPhysicsBake::GetSettings(Body.Get())->SourceAnimation);
			StartSeconds = FPlatformTime::Seconds();
		}
		virtual ~FRunModularBake() override {
			// Also restore after an early failure or timeout; never leave the optional editor on a test copy.
			KawaiiPhysicsBake::GetSettings(Body.Get())->SourceAnimation = OriginalSource.Get();
			Session.Reset();
			FPackageName::UnRegisterMountPoint(TEXT("/KawaiiBakeTests/"), Directory + TEXT("/"));
		}
		virtual bool Update() override {
			if (FPlatformTime::Seconds() - StartSeconds > 120) {
				Test->AddError(TEXT("Modular bake timed out."));
				Session->Cancel();
				return true;
			}
			FString Error;
			if (Stage == 3) {
				// The recorder intentionally permits only one step per real engine frame.
				if (GFrameCounter <= MutationFrame) {
					return false;
				}
				Session->Tick();
				Test->TestFalse(TEXT("Editing the active source stops the take on its next step"), Session->IsRunning());
				Test->TestEqual(TEXT("An edited source produces no output assets"), Session->GetOutputAssets().Num(), 0);
				Test->TestTrue(TEXT("The failure identifies the source animation edit"), Session->GetStatus().ToString().Contains(TEXT("source animation was edited")));
				Test->TestTrue(TEXT("The original project animation data was not modified"), OriginalSource->GetDataModel()->GenerateGuid() == OriginalSourceGuid);
				if (Session->IsRunning()) {
					Session->Cancel();
				}
				KawaiiPhysicsBake::GetSettings(Body.Get())->SourceAnimation = OriginalSource.Get();
				Test->AddInfo(TEXT("Two independent meshes, repeated synchronized baking, cancellation, and rejection of an edited transient source completed."));
				if (FParse::Param(FCommandLine::Get(), TEXT("KawaiiBakeOpenEditor"))) {
					GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Body.Get());
				}
				return true;
			}
			if (Stage == 0) {
				if (!Session->Initialize(Body.Get(), Error) || !Session->StartBake(Error)) {
					Test->AddError(Error);
					return true;
				}
				Stage = 1;
			}
			Session->Tick();
			if (Session->IsRunning()) {
				return false;
			}
			const auto& Outputs = Session->GetOutputAssets();
			if (!Test->TestEqual(TEXT("One asset per independently evaluated mesh"), Outputs.Num(), 2)) {
				Test->AddError(Session->GetStatus().ToString());
				return true;
			}
			UAnimSequence* A = Outputs[0].Get();
			UAnimSequence* B = Outputs[1].Get();
			if (!A || !B) {
				Test->AddError(TEXT("Baked assets did not survive GC."));
				return true;
			}
			Test->TestTrue(TEXT("Independent body skeleton"), A->GetSkeleton() == KawaiiPhysicsBake::GetSettings(Body.Get())->MainMesh->GetSkeleton());
			Test->TestTrue(TEXT("Independent clothing skeleton"), B->GetSkeleton() == KawaiiPhysicsBake::GetSettings(Clothing.Get())->MainMesh->GetSkeleton());
			Test->TestEqual(TEXT("Synchronized key count"), A->GetDataModel()->GetNumberOfKeys(), B->GetDataModel()->GetNumberOfKeys());
			Test->TestEqual(TEXT("Synchronized length"), A->GetPlayLength(), B->GetPlayLength());
			Test->TestTrue(TEXT("Animation file persisted"), FPackageName::DoesPackageExist(A->GetOutermost()->GetName()));
			if (Stage == 1) {
				FirstBody.Reset(A);
				FirstClothing.Reset(B);
				if (!Session->StartBake(Error)) {
					Test->AddError(Error);
					return true;
				}
				Stage = 2;
				return false;
			}
			const auto Compare = [this](UAnimSequence* First, UAnimSequence* Second) {
				TArray<FName> FirstTracks, SecondTracks;
				First->GetDataModel()->GetBoneTrackNames(FirstTracks);
				Second->GetDataModel()->GetBoneTrackNames(SecondTracks);
				if (!Test->TestEqual(TEXT("Repeat track count"), FirstTracks.Num(), SecondTracks.Num())) {
					return;
				}
				for (const FName Track : FirstTracks) {
					TArray<FTransform> X, Y;
					First->GetDataModel()->GetBoneTrackTransforms(Track, X);
					Second->GetDataModel()->GetBoneTrackTransforms(Track, Y);
					if (!Test->TestEqual(TEXT("Repeat key count"), X.Num(), Y.Num())) {
						return;
					}
					for (int32 Key = 0; Key < X.Num(); ++Key) {
						if (!X[Key].GetTranslation().Equals(Y[Key].GetTranslation(), 0.001f)
							|| !X[Key].GetRotation().Equals(Y[Key].GetRotation(), 0.0001f)
							|| !X[Key].GetScale3D().Equals(Y[Key].GetScale3D(), 0.0001f))
						{
							Test->AddError(FString::Printf(TEXT("Non-repeatable track %s key %d"), *Track.ToString(), Key));
							return;
						}
					}
				}
			};
			Compare(FirstBody.Get(), A);
			Compare(FirstClothing.Get(), B);
			Test->TestNotEqual(TEXT("New bake does not overwrite previous assets"), FirstBody->GetPathName(), A->GetPathName());
			if (!Session->StartBake(Error)) {
				Test->AddError(Error);
				return true;
			}
			Session->Cancel();
			Test->TestFalse(TEXT("Cancel stops sampling"), Session->IsRunning());

			// Mutate only a transient duplicate, using the real controller notification path.
			OriginalSourceGuid = OriginalSource->GetDataModel()->GenerateGuid();
			EditedSource.Reset(
				DuplicateObject<UAnimSequence>(OriginalSource.Get(), GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UAnimSequence::StaticClass(), TEXT("BakeEditedSourceTest"))));
			if (!Test->TestNotNull(TEXT("Transient animation duplicate"), EditedSource.Get())) {
				return true;
			}
			EditedSource->ClearFlags(RF_Public | RF_Standalone);
			EditedSource->SetFlags(RF_Transient);
			if (!Test->TestTrue(TEXT("The test copy owns a separate animation data model"), EditedSource->GetDataModelInterface().GetObject() != OriginalSource->GetDataModelInterface().GetObject())) {
				return true;
			}
			KawaiiPhysicsBake::GetSettings(Body.Get())->SourceAnimation = EditedSource.Get();
			if (!Session->StartBake(Error)) {
				Test->AddError(Error);
				return true;
			}
			IAnimationDataController& Controller = EditedSource->GetController();
			const FAnimationCurveIdentifier Curve(FName(*FString::Printf(TEXT("BakeMutation_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))), ERawCurveTrackTypes::RCT_Float);
			if (!Test->TestTrue(TEXT("Add a real float curve after sampling starts"), Controller.AddCurve(Curve, AACF_DefaultCurve, false))) {
				Session->Cancel();
				return true;
			}
			MutationFrame = GFrameCounter;
			Stage = 3;
			return false;
		}

	private:
		FAutomationTestBase* Test;
		TStrongObjectPtr<UAnimBlueprint> Body, Clothing;
		TStrongObjectPtr<UAnimSequence> FirstBody, FirstClothing;
		TStrongObjectPtr<UAnimSequence> OriginalSource, EditedSource;
		FGuid OriginalSourceGuid;
		uint64 MutationFrame = MAX_uint64;
		TSharedPtr<FKawaiiPhysicsBakeSession> Session;
		FString Directory;
		double StartSeconds;
		int32 Stage = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKawaiiBakeModularIntegration, "KawaiiPhysicsBaker.Integration.ModularCharacter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FKawaiiBakeModularIntegration::RunTest(const FString&) {
	FString MeshPath, ClothingPath, AnimationPath;
	if (!FParse::Value(FCommandLine::Get(), TEXT("KawaiiBakeTestMesh="), MeshPath)
		|| !FParse::Value(FCommandLine::Get(), TEXT("KawaiiBakeTestClothing="), ClothingPath)
		|| !FParse::Value(FCommandLine::Get(), TEXT("KawaiiBakeTestAnimation="), AnimationPath))
	{
		AddInfo(TEXT("Integration requires -KawaiiBakeTestMesh, -KawaiiBakeTestClothing and -KawaiiBakeTestAnimation; no content dependency is built into this plugin."));
		return true;
	}
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	USkeletalMesh* ClothingMesh = LoadObject<USkeletalMesh>(nullptr, *ClothingPath);
	UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, *AnimationPath);
	if (!TestNotNull(TEXT("Body mesh"), Mesh)
		|| !TestNotNull(TEXT("Clothing mesh"), ClothingMesh)
		|| !TestNotNull(TEXT("Source animation"), Animation))
	{
		return false;
	}
	TStrongObjectPtr<UKawaiiPhysicsBakeFactory> Factory(NewObject<UKawaiiPhysicsBakeFactory>());
	Factory->SelectedMesh = Mesh;
	UAnimBlueprint* Body = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
		UAnimBlueprint::StaticClass(), GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UAnimBlueprint::StaticClass(), TEXT("BakeBodyTest")), RF_Transient, nullptr, GWarn));
	Factory->SelectedMesh = ClothingMesh;
	UAnimBlueprint* Clothing = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(
		UAnimBlueprint::StaticClass(), GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UAnimBlueprint::StaticClass(), TEXT("BakeClothingTest")), RF_Transient, nullptr, GWarn));
	if (!TestNotNull(TEXT("Native body graph"), Body) || !TestNotNull(TEXT("Native clothing graph"), Clothing)) {
		return false;
	}
	if (!TestTrue(TEXT("Native graph compiles with KawaiiPhysics"), AddPhysicsToGraph(Clothing, ClothingMesh))) {
		return false;
	}
	UKawaiiPhysicsBakeSettings* Settings = KawaiiPhysicsBake::GetSettings(Body);
	Settings->SourceAnimation = Animation;
	Settings->SampleRate = 15;
	Settings->WarmUpFrames = 3;
	FKawaiiPhysicsBakePart& Part = Settings->Parts.AddDefaulted_GetRef();
	Part.Name = TEXT("Clothing");
	Part.Mesh = ClothingMesh;
	Part.Graph = Clothing;
	ADD_LATENT_AUTOMATION_COMMAND(FRunModularBake(this, Body, Clothing));
	return true;
}

#endif
