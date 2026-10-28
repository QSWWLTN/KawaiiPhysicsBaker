#include "KawaiiPhysicsBakeSession.h"

#include "KawaiiPhysicsBakeSampling.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeAnimInstance.h"
#include "AnimNode_KawaiiPhysicsBakeSource.h"
#include "AnimNode_KawaiiPhysics.h"
#include "KawaiiPhysicsClothingCollisionComponent.h"
#include "KawaiiPhysicsClothingCollisionProfile.h"
#include "KawaiiPhysicsSharedCollisionSubsystem.h"
#include "ExternalForces/KawaiiPhysicsExternalForce.h"
#include "ExternalForces/KawaiiPhysicsExternalForce_Wind.h"
#include "AdvancedPreviewScene.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimationPoseData.h"
#include "Animation/AnimNode_AssetPlayerBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/AnimDataNotifications.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakeSession"

struct FKawaiiPhysicsBakeSession::FPart
{
	FName Name;
	TObjectPtr<USkeletalMesh> Mesh = nullptr;
	TObjectPtr<UAnimBlueprint> Graph = nullptr;
	TObjectPtr<UClass> CompiledClass = nullptr;
	TObjectPtr<UAnimSequence> Animation = nullptr;
	TObjectPtr<UObject> InputDataModel = nullptr;
	FDelegateHandle InputModelChangeHandle;
	FGuid InputDataGuid;
	float InputLength = 0.f;
	bool bInputDataChanged = false;
	TObjectPtr<USkeletalMeshComponent> Component = nullptr;
	int32 CopySourceIndex = INDEX_NONE;
	FTransform RelativeTransform;
	TArray<FRawAnimSequenceTrack> Tracks;
	TMap<FName, TArray<FRichCurveKey>> Curves;
};

namespace
{
	FGuid ReadCachedAnimationDataGuid(const UAnimSequence* Animation) {
		// UE 5.5 still maintains this cached guid on model edits. Generating a new
		// model hash here would walk every track on every simulation frame.
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return Animation->GetRawDataGuid();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}

	bool ValidateGraph(UAnimBlueprint* Graph, USkeletalMesh* Mesh, FString& Error) {
		if (!Graph
			|| !Mesh
			|| !Mesh->GetSkeleton())
		{
			Error = TEXT("Each component needs a mesh with a Skeleton and a bake Anim Blueprint.");
			return false;
		}
		if (Graph->Status != BS_UpToDate && Graph->Status != BS_UpToDateWithWarnings) {
			Error = FString::Printf(TEXT("Compile %s successfully before baking; stale generated classes are not accepted."), *Graph->GetName());
			return false;
		}
		if (!Graph->GeneratedClass
			|| !Graph->GeneratedClass->IsChildOf(UKawaiiPhysicsBakeAnimInstance::StaticClass())
			|| Graph->TargetSkeleton != Mesh->GetSkeleton())
		{
			Error = FString::Printf(TEXT("%s must inherit KawaiiPhysicsBakeAnimInstance and target this component's Skeleton."), *Graph->GetName());
			return false;
		}
		const IAnimClassInterface* Interface = IAnimClassInterface::GetFromClass(Graph->GeneratedClass);
		if (!Interface) {
			Error = TEXT("Missing compiled animation class interface.");
			return false;
		}
		int32 Sources = 0;
		for (const FStructProperty* Property : Interface->GetAnimNodeProperties()) {
			Sources += Property->Struct->IsChildOf(FAnimNode_KawaiiPhysicsBakeSource::StaticStruct()) ? 1 : 0;
			if (Property->Struct->IsChildOf(FAnimNode_AssetPlayerBase::StaticStruct())) {
				Error = FString::Printf(TEXT("%s contains an uncontrolled animation player. Use Bake Source Pose as its animation source."), *Graph->GetName());
				return false;
			}
			if (Property->Struct->IsChildOf(FAnimNode_KawaiiPhysics::StaticStruct())) {
				const FAnimNode_KawaiiPhysics* Node = Property->ContainerPtrToValuePtr<FAnimNode_KawaiiPhysics>(Graph->GeneratedClass->GetDefaultObject());
				if (Node->bAllowWorldCollision
					|| Node->bEnableWind
					|| Node->CustomExternalForces.Num())
				{
					Error = FString::Printf(
						TEXT("%s uses world collision, scene wind or a custom Blueprint force. The isolated bake scene has no gameplay world; use explicit collision limits and graph-local forces."),
						*Graph->GetName());
					return false;
				}
				for (const FInstancedStruct& ForceData : Node->ExternalForces) {
					const FKawaiiPhysics_ExternalForce* Force = ForceData.GetPtr<FKawaiiPhysics_ExternalForce>();
					if (!Force || !Force->bIsEnabled) {
						continue;
					}
					if (ForceData.GetScriptStruct()->IsChildOf(FKawaiiPhysics_ExternalForce_Wind::StaticStruct())
						|| !FMath::IsFinite(Force->RandomForceScaleRange.Min)
						|| !FMath::IsFinite(Force->RandomForceScaleRange.Max)
						|| Force->RandomForceScaleRange.Min != Force->RandomForceScaleRange.Max)
					{
						Error = FString::Printf(TEXT("%s uses a wind or randomized external force. Disable it or use a constant Random Force Scale Range to keep Preview and Bake repeatable."),
												*Graph->GetName());
						return false;
					}
				}
			}
		}
		if (Sources != 1) {
			Error = FString::Printf(TEXT("%s must have exactly one connected Bake Source Pose node (found %d)."), *Graph->GetName(), Sources);
			return false;
		}
		return true;
	}

	bool ValidateAnimation(UAnimSequence* Animation, USkeletalMesh* Mesh, float Length, FString& Error) {
		if (!Animation) {
			return true;
		}
		if (Animation->GetSkeleton() != Mesh->GetSkeleton() || Animation->IsValidAdditive()) {
			Error = FString::Printf(TEXT("%s must be a non-additive sequence on %s's Skeleton. Copy Pose supports separate skeletons; direct sequence playback does not retarget."),
									*Animation->GetName(),
									*Mesh->GetName());
			return false;
		}
		if (!FMath::IsNearlyEqual(Animation->GetPlayLength(), Length, 1.e-4f)) {
			Error = FString::Printf(TEXT("%s has a different length from the main sequence. All explicit part sequences must use the same timeline."), *Animation->GetName());
			return false;
		}
		return true;
	}
}

FKawaiiPhysicsBakeSession::FKawaiiPhysicsBakeSession()
	: PreviewScene(MakeUnique<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(true).ShouldSimulatePhysics(false))),
	  Status(LOCTEXT("Idle", "Choose a source animation, then Preview or Bake.")) {
	PreviewScene->SetFloorVisibility(false, true);
}

FKawaiiPhysicsBakeSession::~FKawaiiPhysicsBakeSession() {
	ClearScene();
}

bool FKawaiiPhysicsBakeSession::Initialize(UAnimBlueprint* InBlueprint, FString& OutError) {
	Blueprint = InBlueprint;
	return ResetPreview(OutError);
}

void FKawaiiPhysicsBakeSession::ClearScene() {
	bRunning = false;
	for (const TUniquePtr<FPart>& Part : Parts) {
		if (IAnimationDataModel* Model = Cast<IAnimationDataModel>(Part->InputDataModel.Get())) {
			Model->GetModifiedEvent().Remove(Part->InputModelChangeHandle);
		}
		if (Part->Component) {
			PreviewScene->RemoveComponent(Part->Component);
			Part->Component->DestroyComponent();
		}
	}
	Parts.Reset();
	if (IsValid(PreviewActor)) {
		PreviewActor->Destroy();
	}
	PreviewActor = nullptr;
	Settings = nullptr;
}

bool FKawaiiPhysicsBakeSession::Prepare(FString& OutError) {
	ClearScene();
	OutError.Reset();
	UKawaiiPhysicsBakeSettings* SourceSettings = Blueprint ? KawaiiPhysicsBake::GetSettings(Blueprint) : nullptr;
	if (!SourceSettings
		|| !SourceSettings->MainMesh
		|| !SourceSettings->SourceAnimation)
	{
		OutError = TEXT("Select a Main Mesh and Source Animation in Bake Settings.");
		return false;
	}
	Settings = DuplicateObject<UKawaiiPhysicsBakeSettings>(SourceSettings, GetTransientPackage());
	// This world belongs only to the tool. Reset its clocks without touching engine/global time.
	GetWorld()->TimeSeconds = 0.0;
	GetWorld()->UnpausedTimeSeconds = 0.0;
	GetWorld()->RealTimeSeconds = 0.0;
	GetWorld()->AudioTimeSeconds = 0.0;
	if (!GetWorld()->GetSubsystem<UKawaiiPhysicsSharedCollisionSubsystem>()) {
		OutError = TEXT("The preview world's Kawaii shared collision registry is unavailable. Restart the editor after enabling the baker plugin.");
		return false;
	}
	if (Settings->SampleRate < 1
		|| Settings->SampleRate > 240
		|| Settings->WarmUpFrames < 0
		|| Settings->WarmUpFrames > 3600)
	{
		OutError = TEXT("Sample Rate must be 1-240, and Warm Up Frames must be 0-3600.");
		return false;
	}
	SourceLength = Settings->SourceAnimation->GetPlayLength();
	if (!FMath::IsFinite(SourceLength)
		|| SourceLength <= 0.f
		|| SourceLength > 600.f)
	{
		OutError = TEXT("The source sequence must have a finite duration between zero and 600 seconds.");
		return false;
	}
	FixedDelta = 1.f / Settings->SampleRate;
	IntervalCount = KawaiiPhysicsBakeSampling::NumIntervals(SourceLength, Settings->SampleRate);
	WarmUpIndex = SampleIndex = 0;
	// Do not reset this to MAX_uint64: Reset/Start in one editor frame must not advance physics twice.
	LastEngineFrame = GFrameCounter;
	TUniquePtr<FPart> Main = MakeUnique<FPart>();
	Main->Name = TEXT("Main");
	Main->Mesh = Settings->MainMesh;
	Main->Graph = Blueprint;
	Main->Animation = Settings->SourceAnimation;
	Main->RelativeTransform = FTransform::Identity;
	Parts.Add(MoveTemp(Main));
	TMap<FName, int32> Indices;
	Indices.Add(TEXT("Main"), 0);
	for (const FKawaiiPhysicsBakePart& Definition : Settings->Parts) {
		if (Definition.Name.IsNone() || Indices.Contains(Definition.Name)) {
			OutError = TEXT("Parts require unique non-empty names; Main is reserved.");
			return false;
		}
		TUniquePtr<FPart> Part = MakeUnique<FPart>();
		Part->Name = Definition.Name;
		Part->Mesh = Definition.Mesh;
		Part->Graph = Definition.Graph;
		Part->Animation = Definition.Animation;
		Part->RelativeTransform = Definition.RelativeTransform;
		if (!Definition.Animation) {
			const int32* SourceIndex = Indices.Find(Definition.CopyPoseFrom);
			if (!SourceIndex) {
				OutError = FString::Printf(TEXT("%s: Copy Pose From must reference Main or an earlier part. Forward and cyclic dependencies are not supported."), *Definition.Name.ToString());
				return false;
			}
			Part->CopySourceIndex = *SourceIndex;
		}
		Indices.Add(Definition.Name, Parts.Num());
		Parts.Add(MoveTemp(Part));
	}
	int64 TotalBones = 0;
	for (const TUniquePtr<FPart>& Part : Parts) {
		if (!ValidateGraph(Part->Graph, Part->Mesh, OutError) || !ValidateAnimation(Part->Animation, Part->Mesh, SourceLength, OutError)) {
			OutError = Part->Name.ToString() + TEXT(": ") + OutError;
			return false;
		}
		if (!KawaiiPhysicsBakeSampling::IsValidTransform(Part->RelativeTransform)) {
			OutError = Part->Name.ToString() + TEXT(": Relative Transform must be finite, normalized and have positive nonzero scale.");
			return false;
		}
		const int32 NumBones = Part->Mesh->GetRefSkeleton().GetRawBoneNum();
		if (!NumBones) {
			OutError = Part->Name.ToString() + TEXT(": Mesh has no real bones.");
			return false;
		}
		Part->CompiledClass = Part->Graph->GeneratedClass;
		if (Part->Animation) {
			IAnimationDataModel* Model = Part->Animation->GetDataModel();
			if (!Model) {
				OutError = Part->Name.ToString() + TEXT(": Source animation has no editable data model.");
				return false;
			}
			Part->InputDataModel = Part->Animation->GetDataModelInterface().GetObject();
			Part->InputDataGuid = ReadCachedAnimationDataGuid(Part->Animation);
			Part->InputLength = Part->Animation->GetPlayLength();
			FPart* TrackedPart = Part.Get();
			// The cached guid updates at bracket close. A model edit notification also
			// catches an interactive change while the editor's transaction is still open.
			Part->InputModelChangeHandle = Model->GetModifiedEvent().AddLambda([TrackedPart](const EAnimDataModelNotifyType& Type, IAnimationDataModel*, const FAnimDataModelNotifPayload&) {
				if (Type != EAnimDataModelNotifyType::BracketOpened
					&& Type != EAnimDataModelNotifyType::BracketClosed
					&& Type != EAnimDataModelNotifyType::CurveColorChanged
					&& Type != EAnimDataModelNotifyType::CurveCommentChanged)
				{
					TrackedPart->bInputDataChanged = true;
				}
			});
		}
		Part->Tracks.SetNum(NumBones);
		TotalBones += NumBones;
	}
	if (TotalBones * int64(IntervalCount + 1) > 4000000) {
		OutError = TEXT("This take exceeds four million bone samples. Reduce the duration, rate or number of parts.");
		return false;
	}
	if (Settings->bEnableClothingCollision
		&& (!Settings->ClothingCollisionProfile || !Indices.Contains(Settings->CollisionSourcePart) || Settings->CollisionSourcePart == TEXT("Main") || Settings->WarmUpFrames < 2))
	{
		OutError = TEXT("Clothing Collision requires a profile, an existing source part (not Main), and at least two warm-up frames.");
		return false;
	}
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	PreviewActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!PreviewActor) {
		OutError = TEXT("Could not create the isolated preview actor.");
		return false;
	}
	PreviewActor->SetActorTickEnabled(false);
	for (const TUniquePtr<FPart>& Part : Parts) {
		USkeletalMeshComponent* Component = NewObject<USkeletalMeshComponent>(PreviewActor, Part->Name, RF_Transient);
		Part->Component = Component;
		PreviewActor->AddInstanceComponent(Component);
		Component->SetComponentTickEnabled(false);
		Component->bEnableUpdateRateOptimizations = false;
		Component->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		Component->SetDisablePostProcessBlueprint(true);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetSkeletalMesh(Part->Mesh);
		Component->SetForcedLOD(1);
		Component->SetVisibility(true);
		Component->SetHiddenInGame(false);
		PreviewScene->AddComponent(Component, Part->RelativeTransform);
		Component->SetAnimInstanceClass(Part->CompiledClass);
		Component->SetComponentTickEnabled(false);
		if (UKawaiiPhysicsBakeAnimInstance* Instance = Cast<UKawaiiPhysicsBakeAnimInstance>(Component->GetAnimInstance())) {
			Instance->bUseMultiThreadedAnimationUpdate = false;
			Instance->GetRequiredBones().SetUseRAWData(true);
			Instance->SetRootMotionMode(ERootMotionMode::NoRootMotionExtraction);
			Instance->SetBakeSource(Part->Animation, 0.f, Part->CopySourceIndex == INDEX_NONE ? nullptr : Parts[Part->CopySourceIndex]->Component.Get());
		} else {
			OutError = Part->Name.ToString() + TEXT(": Could not initialize the bake Anim Instance.");
			return false;
		}
	}
	if (Settings->bEnableClothingCollision) {
		UKawaiiPhysicsClothingCollisionComponent* Bridge = NewObject<UKawaiiPhysicsClothingCollisionComponent>(PreviewActor, NAME_None, RF_Transient);
		PreviewActor->AddInstanceComponent(Bridge);
		Bridge->RegisterComponentWithWorld(GetWorld());
		UKawaiiPhysicsClothingCollisionProfile* Profile = DuplicateObject<UKawaiiPhysicsClothingCollisionProfile>(Settings->ClothingCollisionProfile, Bridge);
		// The bridge ages by wall time. Offline frames may take seconds, but simulation dt stays fixed.
		Profile->MaxSnapshotAgeSeconds = MAX_flt;
		Profile->MaxDistance = 0.f;
		Bridge->Configure(Parts[Indices.FindChecked(Settings->CollisionSourcePart)]->Component, Parts[0]->Component, Profile);
	}
	return true;
}

bool FKawaiiPhysicsBakeSession::ResetPreview(FString& OutError) {
	bBaking = false;
	if (!Prepare(OutError)) {
		Fail(OutError);
		return false;
	}
	Status = LOCTEXT("Ready", "Ready. Preview and Bake restart from the same warm-up state.");
	return true;
}

bool FKawaiiPhysicsBakeSession::StartPreview(FString& OutError) {
	if (bRunning) {
		OutError = TEXT("Stop or cancel the current operation first.");
		return false;
	}
	if (!ResetPreview(OutError)) {
		return false;
	}
	bRunning = true;
	Status = LOCTEXT("Previewing", "Preview: warming up...");
	return true;
}

bool FKawaiiPhysicsBakeSession::StartBake(FString& OutError) {
	if (bRunning) {
		OutError = TEXT("Stop or cancel the current operation first.");
		return false;
	}
	if (!ResetPreview(OutError)) {
		return false;
	}
	FString Directory = Settings->OutputDirectory.Path;
	Directory.RemoveFromEnd(TEXT("/"));
	FString Filename;
	if (!FPackageName::IsValidLongPackageName(Directory + TEXT("/Probe"))
		|| !FPackageName::TryConvertLongPackageNameToFilename(Directory + TEXT("/Probe"), Filename)
		|| Directory.StartsWith(TEXT("/Engine/"))
		|| Settings->OutputPrefix.IsEmpty()
		|| ObjectTools::SanitizeObjectName(Settings->OutputPrefix) != Settings->OutputPrefix)
	{
		OutError = TEXT("Output Directory must be a registered package folder such as /Game/KawaiiBakes (not Engine content), and Output Prefix must be a valid non-empty asset name.");
		Fail(OutError);
		return false;
	}
	Settings->OutputDirectory.Path = Directory;
	OutputAssets.Reset();
	bRunning = bBaking = true;
	Status = LOCTEXT("Baking", "Bake: warming up; no output assets have been created.");
	return true;
}

void FKawaiiPhysicsBakeSession::StopPreview() {
	if (bBaking) {
		Cancel();
		return;
	}
	bRunning = false;
	Status = LOCTEXT("Stopped", "Preview stopped. Play restarts with a fresh simulation.");
}

void FKawaiiPhysicsBakeSession::Cancel() {
	bRunning = bBaking = false;
	for (const TUniquePtr<FPart>& Part : Parts) {
		Part->Tracks.Reset();
		Part->Curves.Reset();
	}
	Status = LOCTEXT("Cancelled", "Cancelled. Buffered samples discarded; no take was saved.");
}

void FKawaiiPhysicsBakeSession::Fail(const FString& Error) {
	bRunning = bBaking = false;
	Status = FText::FromString(Error);
}

bool FKawaiiPhysicsBakeSession::Evaluate(float Time, FString& OutError) {
	// Only session code advances this world. Mesh ticks are disabled; evaluation is explicitly ordered.
	GetWorld()->Tick(LEVELTICK_All, FixedDelta);
	for (const TUniquePtr<FPart>& Part : Parts) {
		if (Part->Animation
			&& (Part->bInputDataChanged || Part->Animation->GetDataModelInterface().GetObject() != Part->InputDataModel || ReadCachedAnimationDataGuid(Part->Animation) != Part->InputDataGuid || !FMath::IsNearlyEqual(Part->Animation->GetPlayLength(), Part->InputLength, 1.e-6f)))
		{
			OutError
				= Part->Name.ToString() + TEXT(": The source animation was edited during sampling. Restart Preview or Bake so the entire take uses one version of the animation. No output was saved.");
			return false;
		}
		if (Part->Graph->GeneratedClass != Part->CompiledClass
			|| (Part->Graph->Status != BS_UpToDate && Part->Graph->Status != BS_UpToDateWithWarnings))
		{
			OutError = TEXT("A bake graph changed during sampling. Recompile and restart the take.");
			return false;
		}
		UKawaiiPhysicsBakeAnimInstance* Instance = Cast<UKawaiiPhysicsBakeAnimInstance>(Part->Component->GetAnimInstance());
		if (!Instance) {
			OutError = TEXT("Bake animation instance was replaced during sampling.");
			return false;
		}
		const uint64 PreviousSerial = Instance->GetSourceEvaluationSerial();
		// Keep editor compression completion from changing extraction paths mid-take.
		Instance->GetRequiredBones().SetUseRAWData(true);
		Instance->SetBakeSource(Part->Animation, Time, Part->CopySourceIndex == INDEX_NONE ? nullptr : Parts[Part->CopySourceIndex]->Component.Get());
		Part->Component->TickAnimation(FixedDelta, false);
		// A null tick function forces synchronous evaluation and completes finalized-bone delegates.
		Part->Component->RefreshBoneTransforms(nullptr);
		Part->Component->UpdateBounds();
		Part->Component->MarkRenderTransformDirty();
		Part->Component->MarkRenderDynamicDataDirty();
		if (!Instance->GetSourceError().IsEmpty()) {
			OutError = Part->Name.ToString() + TEXT(": ") + Instance->GetSourceError();
			return false;
		}
		if (Instance->GetSourceEvaluationSerial() <= PreviousSerial || !FMath::IsNearlyEqual(Instance->GetLastEvaluatedSourceTime(), Time, 1.e-4f)) {
			OutError = Part->Name.ToString() + TEXT(": Bake Source Pose was not evaluated at the requested time. Check graph connections and blends that bypass the source.");
			return false;
		}
	}
	return true;
}

bool FKawaiiPhysicsBakeSession::Capture(FString& OutError) {
	for (const TUniquePtr<FPart>& Part : Parts) {
		const FReferenceSkeleton& Ref = Part->Mesh->GetRefSkeleton();
		const TArray<FTransform>& ComponentPose = Part->Component->GetComponentSpaceTransforms();
		if (ComponentPose.Num() < Ref.GetRawBoneNum()) {
			OutError = Part->Name.ToString() + TEXT(": Final component pose is incomplete.");
			return false;
		}
		for (int32 Bone = 0; Bone < Ref.GetRawBoneNum(); ++Bone) {
			const int32 Parent = Ref.GetParentIndex(Bone);
			FTransform Local = Parent == INDEX_NONE ? ComponentPose[Bone] : ComponentPose[Bone].GetRelativeTransform(ComponentPose[Parent]);
			Local.NormalizeRotation();
			if (!KawaiiPhysicsBakeSampling::IsValidTransform(Local)) {
				OutError = FString::Printf(TEXT("%s: Bone %s produced an invalid transform at sample %d."), *Part->Name.ToString(), *Ref.GetBoneName(Bone).ToString(), SampleIndex);
				return false;
			}
			FRawAnimSequenceTrack& Track = Part->Tracks[Bone];
			Track.PosKeys.Add(FVector3f(Local.GetTranslation()));
			Track.RotKeys.Add(FQuat4f(Local.GetRotation()));
			Track.ScaleKeys.Add(FVector3f(Local.GetScale3D()));
		}
		const TMap<FName, float>& Values = Part->Component->GetAnimInstance()->GetAnimationCurveList(EAnimCurveType::AttributeCurve);
		for (const TPair<FName, float>& Value : Values) {
			if (!FMath::IsFinite(Value.Value)) {
				OutError = TEXT("A curve contains a non-finite value.");
				return false;
			}
			if (!Part->Curves.Contains(Value.Key)) {
				TArray<FRichCurveKey>& Keys = Part->Curves.Add(Value.Key);
				for (int32 Previous = 0; Previous < SampleIndex; ++Previous) {
					FRichCurveKey Key(float(Previous) / Settings->SampleRate, 0.f);
					Key.InterpMode = RCIM_Linear;
					Keys.Add(Key);
				}
			}
		}
		for (TPair<FName, TArray<FRichCurveKey>>& Curve : Part->Curves) {
			FRichCurveKey Key(float(SampleIndex) / Settings->SampleRate, Values.FindRef(Curve.Key));
			Key.InterpMode = RCIM_Linear;
			Curve.Value.Add(Key);
		}
	}
	return true;
}

void FKawaiiPhysicsBakeSession::Tick() {
	if (!bRunning || LastEngineFrame == GFrameCounter) {
		return;
	}
	LastEngineFrame = GFrameCounter;
	FString Error;
	const bool bWarmUp = WarmUpIndex < Settings->WarmUpFrames;
	const float Time = bWarmUp ? 0.f : KawaiiPhysicsBakeSampling::SourceTime(SampleIndex, IntervalCount, SourceLength, Settings->SampleRate);
	if (!Evaluate(Time, Error)) {
		Fail(Error);
		return;
	}
	if (bWarmUp) {
		++WarmUpIndex;
		Status = FText::Format(LOCTEXT("WarmupProgress", "Warm-up {0}/{1}"), WarmUpIndex, Settings->WarmUpFrames);
		return;
	}
	if (bBaking && !Capture(Error)) {
		Fail(Error);
		return;
	}
	++SampleIndex;
	Status = FText::Format(LOCTEXT("SampleProgress", "Sample {0}/{1} at {2} fps"), SampleIndex, IntervalCount + 1, Settings->SampleRate);
	if (SampleIndex > IntervalCount) {
		bRunning = false;
		if (bBaking) {
			bBaking = false;
			if (!WriteAssets(Error)) {
				Fail(Error);
				return;
			}
			Status = FText::Format(LOCTEXT("Complete", "Saved {0} synchronized animations ({1} keys, {2} fps). Root bone motion retained; actor stayed stationary."),
								   OutputAssets.Num(),
								   SampleIndex,
								   Settings->SampleRate);
		} else {
			Status = LOCTEXT("PreviewComplete", "Preview complete. Bake repeats this simulation from a fresh warm-up state.");
		}
	}
}

bool FKawaiiPhysicsBakeSession::WriteAssets(FString& OutError) {
	const FString Group = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<FString> PackageNames;
	for (const TUniquePtr<FPart>& Part : Parts) {
		const FString Name = ObjectTools::SanitizeObjectName(Settings->OutputPrefix + TEXT("_") + Settings->SourceAnimation->GetName() + TEXT("_") + Part->Name.ToString() + TEXT("_") + Group.Left(8));
		const FString PackageName = Settings->OutputDirectory.Path + TEXT("/") + Name;
		if (FindPackage(nullptr, *PackageName)
			|| FPackageName::DoesPackageExist(PackageName)
			|| PackageNames.Contains(PackageName))
		{
			OutError = TEXT("An output name already exists. No assets were overwritten; start a fresh take.");
			return false;
		}
		PackageNames.Add(PackageName);
		for (const FRawAnimSequenceTrack& Track : Part->Tracks) {
			if (Track.PosKeys.Num() != IntervalCount + 1
				|| Track.RotKeys.Num() != IntervalCount + 1
				|| Track.ScaleKeys.Num() != IntervalCount + 1)
			{
				OutError = TEXT("The recorded parts do not have matching sample counts. No assets were created.");
				return false;
			}
		}
		// Skeleton retargeting discards authored translations on playback. Ordinary
		// Skeleton-mode bones are fine when their recorded local translation stays at
		// the mesh reference value; only reject actual loss of a simulated displacement.
		const USkeleton* Skeleton = Part->Mesh->GetSkeleton();
		const FReferenceSkeleton& MeshRef = Part->Mesh->GetRefSkeleton();
		for (int32 Bone = 0; Bone < Part->Tracks.Num(); ++Bone) {
			const FName BoneName = MeshRef.GetBoneName(Bone);
			const int32 SkeletonBone = Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName);
			if (SkeletonBone == INDEX_NONE) {
				OutError = FString::Printf(TEXT("%s: Mesh bone %s is absent from its Skeleton; its baked track cannot be exported."), *Part->Name.ToString(), *BoneName.ToString());
				return false;
			}
			if (Skeleton->GetBoneTranslationRetargetingMode(SkeletonBone) != EBoneTranslationRetargetingMode::Skeleton) {
				continue;
			}
			const FVector ReferenceTranslation = MeshRef.GetRefBonePose()[Bone].GetTranslation();
			for (int32 Frame = 0; Frame < Part->Tracks[Bone].PosKeys.Num(); ++Frame) {
				if (!FVector(Part->Tracks[Bone].PosKeys[Frame]).Equals(ReferenceTranslation, 0.01)) {
					OutError = FString::Printf(
						TEXT("%s: Bone %s changes local translation at frame %d, but its Skeleton translation retargeting would discard that motion. Use Animation translation retargeting for this "
							 "bone on the intended bake skeleton, or remove that translation change from the graph. The tool has not modified the Skeleton or created output assets."),
						*Part->Name.ToString(),
						*BoneName.ToString(),
						Frame);
					return false;
				}
			}
		}
	}
	// Asset creation is deliberately deferred until every component has a complete validated take.
	for (int32 Index = 0; Index < Parts.Num(); ++Index) {
		FPart& Part = *Parts[Index];
		UPackage* Package = CreatePackage(*PackageNames[Index]);
		UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, *FPackageName::GetLongPackageAssetName(PackageNames[Index]), RF_Public | RF_Standalone | RF_Transactional);
		CreatedAssets.Add(Sequence);
		OutputAssets.Add(Sequence);
		Sequence->SetSkeleton(Part.Mesh->GetSkeleton());
		Sequence->SetPreviewMesh(Part.Mesh, false);
		// Captured keys are already evaluated on this mesh. Recording its reference
		// pose prevents AnimationScaled/Relative/OrientAndScale from retargeting twice.
		Sequence->SetRetargetSourceAsset(Part.Mesh);
		Sequence->UpdateRetargetSourceAssetData();
		Sequence->bEnableRootMotion = Part.Animation ? Part.Animation->bEnableRootMotion : Settings->SourceAnimation->bEnableRootMotion;
		Sequence->bForceRootLock = false;
		IAnimationDataController& Controller = Sequence->GetController();
		Controller.OpenBracket(LOCTEXT("Populate", "Populate physics bake"), false);
		Controller.InitializeModel();
		Controller.SetFrameRate(FFrameRate(Settings->SampleRate, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(IntervalCount), false);
		bool bSuccess = true;
		for (int32 Bone = 0; Bone < Part.Tracks.Num(); ++Bone) {
			const FName BoneName = Part.Mesh->GetRefSkeleton().GetBoneName(Bone);
			const FRawAnimSequenceTrack& Track = Part.Tracks[Bone];
			bSuccess &= Controller.AddBoneCurve(BoneName, false);
			bSuccess &= Controller.SetBoneTrackKeys(BoneName, Track.PosKeys, Track.RotKeys, Track.ScaleKeys, false);
		}
		for (const TPair<FName, TArray<FRichCurveKey>>& Curve : Part.Curves) {
			const FAnimationCurveIdentifier Id(Curve.Key, ERawCurveTrackTypes::RCT_Float);
			bSuccess &= Controller.AddCurve(Id, AACF_DefaultCurve, false);
			bSuccess &= Controller.SetCurveKeys(Id, Curve.Value, false);
		}
		Controller.NotifyPopulated();
		Controller.CloseBracket(false);
		UMetaData* Meta = Package->GetMetaData();
		Meta->SetValue(Sequence, TEXT("KawaiiBake.Group"), *Group);
		Meta->SetValue(Sequence, TEXT("KawaiiBake.Graph"), *Part.Graph->GetPathName());
		Meta->SetValue(Sequence, TEXT("KawaiiBake.MainGraph"), *Blueprint->GetPathName());
		Meta->SetValue(Sequence, TEXT("KawaiiBake.Source"), *Settings->SourceAnimation->GetPathName());
		Meta->SetValue(Sequence, TEXT("KawaiiBake.Part"), *Part.Name.ToString());
		Meta->SetValue(Sequence, TEXT("KawaiiBake.Rate"), *LexToString(Settings->SampleRate));
		Meta->SetValue(Sequence, TEXT("KawaiiBake.WarmUpFrames"), *LexToString(Settings->WarmUpFrames));
		Meta->SetValue(Sequence, TEXT("KawaiiBake.SourceLength"), *LexToString(SourceLength));
		Meta->SetValue(Sequence, TEXT("KawaiiBake.TimePolicy"), TEXT("CeilToFrame; exact source endpoint; less than one frame end padding"));
		Meta->SetValue(Sequence, TEXT("KawaiiBake.RootPolicy"), TEXT("Local root preserved; stationary actor; no component-motion extraction"));
		Meta->SetValue(Sequence, TEXT("KawaiiBake.Content"), TEXT("Final real-bone transforms and float curves; no notifies, sync markers or typed attributes"));
		Sequence->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Sequence);
		if (!bSuccess || Sequence->GetDataModel()->GetNumberOfFrames() != IntervalCount) {
			OutError = TEXT("Animation data construction failed. New assets remain unsaved for inspection; no existing asset was changed.");
			return false;
		}
		// Read back the real output using UE's normal retargeting path before saving.
		// All frames were checked for Skeleton translation loss above; representative
		// keys additionally catch wrong reference bases, mappings or root handling.
		TArray<FBoneIndexType> RequiredIndices;
		for (int32 Bone = 0; Bone < Part.Tracks.Num(); ++Bone) {
			RequiredIndices.Add(Bone);
		}
		FBoneContainer RequiredBones(RequiredIndices, UE::Anim::FCurveFilterSettings(UE::Anim::ECurveFilterMode::None), *Part.Mesh);
		RequiredBones.SetUseRAWData(true);
		for (const int32 Frame : {0, IntervalCount / 2, IntervalCount}) {
			FMemMark Mark(FMemStack::Get());
			FCompactPose Pose;
			Pose.SetBoneContainer(&RequiredBones);
			Pose.ResetToRefPose();
			FBlendedCurve Curve;
			Curve.InitFrom(RequiredBones);
			UE::Anim::FStackAttributeContainer Attributes;
			FAnimationPoseData PoseData(Pose, Curve, Attributes);
			FAnimExtractContext Extraction(double(Frame) / Settings->SampleRate, false);
			Extraction.bIgnoreRootLock = true;
			Extraction.bExtractWithRootMotionProvider = false;
			Sequence->GetBonePose(PoseData, Extraction, true);
			for (const FCompactPoseBoneIndex BoneIndex : Pose.ForEachBoneIndex()) {
				const int32 Bone = RequiredBones.MakeMeshPoseIndex(BoneIndex).GetInt();
				if (!Part.Tracks.IsValidIndex(Bone)) {
					continue;
				}
				const FRawAnimSequenceTrack& Track = Part.Tracks[Bone];
				const FTransform& Actual = Pose[BoneIndex];
				if (!Actual.GetTranslation().Equals(FVector(Track.PosKeys[Frame]), 0.01)
					|| !Actual.GetRotation().Equals(FQuat(Track.RotKeys[Frame]), 0.0001)
					|| !Actual.GetScale3D().Equals(FVector(Track.ScaleKeys[Frame]), 0.0001))
				{
					OutError = FString::Printf(TEXT("%s: Export playback differs from the captured pose on bone %s at frame %d. Check translation retargeting and the mesh reference pose. New "
													"animations remain unsaved; no existing Skeleton or animation was changed."),
											   *Part.Name.ToString(),
											   *Part.Mesh->GetRefSkeleton().GetBoneName(Bone).ToString(),
											   Frame);
					return false;
				}
			}
		}
	}
	for (int32 Index = 0; Index < OutputAssets.Num(); ++Index) {
		UAnimSequence* Sequence = OutputAssets[Index].Get();
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageNames[Index], FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.bSlowTask = false;
		if (!UPackage::SavePackage(Sequence->GetPackage(), Sequence, *Filename, Args)) {
			Sequence->MarkPackageDirty();
			OutError = FString::Printf(TEXT("Could not save %s. The take is incomplete on disk; all created assets remain available to save manually."), *Filename);
			return false;
		}
	}
	return true;
}

float FKawaiiPhysicsBakeSession::GetProgress() const {
	return Settings ? FMath::Clamp(float(WarmUpIndex + SampleIndex) / float(Settings->WarmUpFrames + IntervalCount + 1), 0.f, 1.f) : 0.f;
}

UWorld* FKawaiiPhysicsBakeSession::GetWorld() const {
	return PreviewScene->GetWorld();
}
FPreviewScene* FKawaiiPhysicsBakeSession::GetPreviewScene() const {
	return PreviewScene.Get();
}
USkeletalMeshComponent* FKawaiiPhysicsBakeSession::GetMainComponent() const {
	return Parts.IsEmpty() ? nullptr : Parts[0]->Component.Get();
}

void FKawaiiPhysicsBakeSession::AddReferencedObjects(FReferenceCollector& Collector) {
	Collector.AddReferencedObject(Blueprint);
	Collector.AddReferencedObject(Settings);
	Collector.AddReferencedObject(PreviewActor);
	Collector.AddReferencedObjects(CreatedAssets);
	for (const TUniquePtr<FPart>& Part : Parts) {
		Collector.AddReferencedObject(Part->Mesh);
		Collector.AddReferencedObject(Part->Graph);
		Collector.AddReferencedObject(Part->CompiledClass);
		Collector.AddReferencedObject(Part->Animation);
		Collector.AddReferencedObject(Part->InputDataModel);
		Collector.AddReferencedObject(Part->Component);
	}
}

#undef LOCTEXT_NAMESPACE
