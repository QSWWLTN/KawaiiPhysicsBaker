#include "KawaiiPhysicsBakeSession.h"

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

#include "KawaiiPhysicsBakeSampling.h"
#include "KawaiiPhysicsBakePreviewComponent.h"
#include "KawaiiPhysicsBakeBlueprint.h"
#include "KawaiiPhysicsBakeAnimInstance.h"
#include "AnimNode_KawaiiPhysicsBakeSource.h"

#define LOCTEXT_NAMESPACE "KawaiiPhysicsBakeSession"

struct FKawaiiPhysicsBakeSession::FPart
{
	struct FPreviewPose {
		TArray<FTransform3f> Bones;
		TMap<FName, float> MaterialCurves;
		TMap<FName, float> MorphCurves;
	};
	TArray<FPreviewPose> PreviewPoses;
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

namespace {
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
			Error = TEXT("每个部件都需要带有骨架的骨骼网格体，以及对应的烘焙动画蓝图。");
			return false;
		}
		if (Graph->Status != BS_UpToDate && Graph->Status != BS_UpToDateWithWarnings) {
			Error = FString::Printf(TEXT("请先成功编译 %s，再进行预览或烘焙。"), *Graph->GetName());
			return false;
		}
		if (!Graph->GeneratedClass
			|| !Graph->GeneratedClass->IsChildOf(UKawaiiPhysicsBakeAnimInstance::StaticClass())
			|| Graph->TargetSkeleton != Mesh->GetSkeleton())
		{
			Error = FString::Printf(TEXT("%s 必须继承 KawaiiPhysicsBakeAnimInstance，并使用该部件的骨架。"), *Graph->GetName());
			return false;
		}
		const IAnimClassInterface* Interface = IAnimClassInterface::GetFromClass(Graph->GeneratedClass);
		if (!Interface) {
			Error = TEXT("缺少已编译的动画类接口。");
			return false;
		}
		int32 Sources = 0;
		for (const FStructProperty* Property : Interface->GetAnimNodeProperties()) {
			Sources += Property->Struct->IsChildOf(FAnimNode_KawaiiPhysicsBakeSource::StaticStruct()) ? 1 : 0;
			if (Property->Struct->IsChildOf(FAnimNode_AssetPlayerBase::StaticStruct())) {
				Error = FString::Printf(TEXT("%s 包含不受烘焙工具控制的动画播放器，请使用 Bake Source Pose 作为动画源。"), *Graph->GetName());
				return false;
			}
			if (Property->Struct->IsChildOf(FAnimNode_KawaiiPhysics::StaticStruct())) {
				const FAnimNode_KawaiiPhysics* Node = Property->ContainerPtrToValuePtr<FAnimNode_KawaiiPhysics>(Graph->GeneratedClass->GetDefaultObject());
				if (Node->bAllowWorldCollision
					|| Node->bEnableWind
					|| Node->CustomExternalForces.Num())
				{
					Error = FString::Printf(
						TEXT("%s 使用了世界碰撞、场景风力或自定义蓝图外力。请在烘焙图中改用明确的碰撞约束和图内外力。"),
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
						Error = FString::Printf(TEXT("%s 使用了风力或随机外力。请关闭它，或将 Random Force Scale Range 两端设为相同值，确保预览与烘焙可重复。"),
												*Graph->GetName());
						return false;
					}
				}
			}
		}
		if (Sources != 1) {
			Error = FString::Printf(TEXT("%s 必须有且仅有一个接入输出路径的 Bake Source Pose 节点（当前为 %d 个）。"), *Graph->GetName(), Sources);
			return false;
		}
		return true;
	}

	bool ValidateAnimation(UAnimSequence* Animation, USkeletalMesh* Mesh, float Length, FString& Error) {
		if (!Animation) {
			return true;
		}
		if (Animation->GetSkeleton() != Mesh->GetSkeleton() || Animation->IsValidAdditive()) {
			Error = FString::Printf(TEXT("%s 必须为使用 %s 骨架的非叠加动画。不同骨架可使用姿态复制，直接播放动画不进行重定向。"),
									*Animation->GetName(),
									*Mesh->GetName());
			return false;
		}
		if (!FMath::IsNearlyEqual(Animation->GetPlayLength(), Length, 1.e-4f)) {
			Error = FString::Printf(TEXT("%s 与主动画时长不同。所有直接指定的部件动画必须使用相同的时间轴。"), *Animation->GetName());
			return false;
		}
		return true;
	}
}

FKawaiiPhysicsBakeSession::FKawaiiPhysicsBakeSession()
	: PreviewScene(MakeUnique<FAdvancedPreviewScene>(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(true).ShouldSimulatePhysics(false))),
	  Status(LOCTEXT("Idle", "请选择源动画，然后预览或烘焙。")) {
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
	bPreviewAvailable = bPreviewPaused = false;
	PreviewAccumulator = 0.0;
	DisplayedSample = SeekTarget = INDEX_NONE;
	WarmUpIndex = SampleIndex = IntervalCount = 0;
	SourceLength = 0.f;
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
		OutError = TEXT("请在烘焙设置中选择主骨骼网格体和源动画。");
		return false;
	}
	Settings = DuplicateObject<UKawaiiPhysicsBakeSettings>(SourceSettings, GetTransientPackage());
	// This world belongs only to the tool. Reset its clocks without touching engine/global time.
	GetWorld()->TimeSeconds = 0.0;
	GetWorld()->UnpausedTimeSeconds = 0.0;
	GetWorld()->RealTimeSeconds = 0.0;
	GetWorld()->AudioTimeSeconds = 0.0;
	if (!GetWorld()->GetSubsystem<UKawaiiPhysicsSharedCollisionSubsystem>()) {
		OutError = TEXT("预览场景的 Kawaii 共享碰撞服务不可用。请启用烘焙插件并重启编辑器。");
		return false;
	}
	if (Settings->SampleRate < 1
		|| Settings->SampleRate > 240
		|| Settings->WarmUpFrames < 0
		|| Settings->WarmUpFrames > 3600)
	{
		OutError = TEXT("采样帧率应为 1–240，预热帧数应为 0–3600。");
		return false;
	}
	SourceLength = Settings->SourceAnimation->GetPlayLength();
	if (!FMath::IsFinite(SourceLength)
		|| SourceLength <= 0.f
		|| SourceLength > 600.f)
	{
		OutError = TEXT("源动画时长必须大于 0 且不超过 600 秒。");
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
			OutError = TEXT("部件名称不能为空或重复；Main 为主部件保留名称。");
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
				OutError = FString::Printf(TEXT("%s：姿态来源必须为 Main 或列表中更早的部件，不支持向后引用或循环依赖。"), *Definition.Name.ToString());
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
			OutError = Part->Name.ToString() + TEXT("：相对变换必须有效，旋转需归一化，缩放必须为正且非零。");
			return false;
		}
		const int32 NumBones = Part->Mesh->GetRefSkeleton().GetRawBoneNum();
		if (!NumBones) {
			OutError = Part->Name.ToString() + TEXT("：网格体没有真实骨骼。");
			return false;
		}
		Part->CompiledClass = Part->Graph->GeneratedClass;
		if (Part->Animation) {
			IAnimationDataModel* Model = Part->Animation->GetDataModel();
			if (!Model) {
				OutError = Part->Name.ToString() + TEXT("：源动画没有可编辑的数据模型。");
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
		OutError = TEXT("本次采样超过 400 万个骨骼样本，请减少动画时长、采样帧率或部件数量。");
		return false;
	}
	if (Settings->bEnableClothingCollision
		&& (!Settings->ClothingCollisionProfile || !Indices.Contains(Settings->CollisionSourcePart) || Settings->CollisionSourcePart == TEXT("Main") || Settings->WarmUpFrames < 2))
	{
		OutError = TEXT("衣物碰撞需要配置资产、已存在的来源部件（不能为 Main），以及至少 2 个预热帧。");
		return false;
	}
	FActorSpawnParameters Spawn;
	Spawn.ObjectFlags = RF_Transient;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	PreviewActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!PreviewActor) {
		OutError = TEXT("无法创建独立的预览角色。");
		return false;
	}
	PreviewActor->SetActorTickEnabled(false);
	for (const TUniquePtr<FPart>& Part : Parts) {
		USkeletalMeshComponent* Component = NewObject<UKawaiiPhysicsBakePreviewComponent>(PreviewActor, Part->Name, RF_Transient);
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
			OutError = Part->Name.ToString() + TEXT("：无法初始化烘焙动画实例。");
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
	Status = LOCTEXT("Ready", "准备就绪。预览和烘焙均从相同的预热状态开始。");
	return true;
}

bool FKawaiiPhysicsBakeSession::StartPreview(FString& OutError) {
	if (bRunning) {
		OutError = TEXT("请先停止或取消当前操作。");
		return false;
	}
	if (!ResetPreview(OutError)) {
		return false;
	}
	bRunning = true;
	bPreviewAvailable = true;
	Status = LOCTEXT("Previewing", "预览：正在预热……");
	return true;
}

bool FKawaiiPhysicsBakeSession::StartBake(FString& OutError) {
	if (bRunning) {
		OutError = TEXT("请先停止或取消当前操作。");
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
		OutError = TEXT("输出目录必须为 /Game/KawaiiBakes 等已注册的内容目录，不能使用引擎目录；输出前缀必须为合法且非空的资产名称。");
		Fail(OutError);
		return false;
	}
	Settings->OutputDirectory.Path = Directory;
	OutputAssets.Reset();
	bRunning = bBaking = true;
	Status = LOCTEXT("Baking", "烘焙：正在预热，尚未创建输出资产。");
	return true;
}

void FKawaiiPhysicsBakeSession::StopPreview() {
	if (bBaking) {
		Cancel();
		return;
	}
	SetPreviewPaused(true);
	Status = LOCTEXT("Stopped", "预览已暂停。可拖动时间轴，或点击播放继续。");
}

void FKawaiiPhysicsBakeSession::Cancel() {
	bRunning = bBaking = false;
	bPreviewAvailable = bPreviewPaused = false;
	SeekTarget = INDEX_NONE;
	for (const TUniquePtr<FPart>& Part : Parts) {
		Part->Tracks.Reset();
		Part->Curves.Reset();
		Part->PreviewPoses.Reset();
	}
	Status = LOCTEXT("Cancelled", "已取消。采样缓存已丢弃，本次结果未保存。");
}

void FKawaiiPhysicsBakeSession::Fail(const FString& Error) {
	bRunning = bBaking = false;
	bPreviewAvailable = false;
	SeekTarget = INDEX_NONE;
	Status = FText::FromString(Error);
}

bool FKawaiiPhysicsBakeSession::ValidateInputs(FString& OutError) const {
	for (const TUniquePtr<FPart>& Part : Parts) {
		if (Part->Animation
			&& (Part->bInputDataChanged || Part->Animation->GetDataModelInterface().GetObject() != Part->InputDataModel || ReadCachedAnimationDataGuid(Part->Animation) != Part->InputDataGuid || !FMath::IsNearlyEqual(Part->Animation->GetPlayLength(), Part->InputLength, 1.e-6f)))
		{
			OutError
				= Part->Name.ToString() + TEXT("：采样期间源动画已被修改，请重新开始预览或烘焙。本次结果未保存。");
			return false;
		}
		if (Part->Graph->GeneratedClass != Part->CompiledClass
			|| (Part->Graph->Status != BS_UpToDate && Part->Graph->Status != BS_UpToDateWithWarnings))
		{
			OutError = TEXT("采样期间烘焙动画图已发生变化，请重新编译并开始预览或烘焙。");
			return false;
		}
	}
	return true;
}

bool FKawaiiPhysicsBakeSession::Evaluate(float Time, FString& OutError) {
	if (!ValidateInputs(OutError)) {
		return false;
	}
	// Restore the simulation frontier after a cached rewind, before ticking any part.
	if (bPreviewAvailable && SampleIndex > 0 && DisplayedSample != SampleIndex - 1) {
		DisplayPreviewPose(SampleIndex - 1);
	}
	// Only session code advances this world. Mesh ticks are disabled; evaluation is explicitly ordered.
	GetWorld()->Tick(LEVELTICK_All, FixedDelta);
	for (const TUniquePtr<FPart>& Part : Parts) {
		UKawaiiPhysicsBakeAnimInstance* Instance = Cast<UKawaiiPhysicsBakeAnimInstance>(Part->Component->GetAnimInstance());
		if (!Instance) {
			OutError = TEXT("采样期间烘焙动画实例已被替换。");
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
			OutError = Part->Name.ToString() + TEXT("：Bake Source Pose 未在指定时间求值，请检查动画图连线和可能绕过源节点的混合分支。");
			return false;
		}
	}
	GetWorld()->SendAllEndOfFrameUpdates();
	return true;
}

void FKawaiiPhysicsBakeSession::CachePreviewPose() {
	for (const TUniquePtr<FPart>& Part : Parts) {
		FPart::FPreviewPose& Pose = Part->PreviewPoses.AddDefaulted_GetRef();
		for (const FTransform& Bone : Part->Component->GetComponentSpaceTransforms()) {
			Pose.Bones.Add(FTransform3f(Bone));
		}
		Pose.MaterialCurves = Part->Component->GetAnimInstance()->GetAnimationCurveList(EAnimCurveType::MaterialCurve);
		Pose.MorphCurves = Part->Component->GetAnimInstance()->GetAnimationCurveList(EAnimCurveType::MorphTargetCurve);
	}
}

void FKawaiiPhysicsBakeSession::DisplayPreviewPose(int32 Frame) {
	for (const TUniquePtr<FPart>& Part : Parts) {
		const FPart::FPreviewPose& Pose = Part->PreviewPoses[Frame];
		CastChecked<UKawaiiPhysicsBakePreviewComponent>(Part->Component)->DisplayPose(Pose.Bones, Pose.MaterialCurves, Pose.MorphCurves);
	}

	DisplayedSample = Frame;
	GetWorld()->SendAllEndOfFrameUpdates();
}

void FKawaiiPhysicsBakeSession::SetPreviewPaused(bool bPause) {
	if (!CanScrubPreview()) {
		return;
	}

	bPreviewPaused = bPause;
	SeekTarget = INDEX_NONE;
	PreviewAccumulator = 0.0;
	if (!bPause && DisplayedSample >= IntervalCount && SampleIndex > 0) {
		DisplayPreviewPose(0);
	}
}

void FKawaiiPhysicsBakeSession::SeekPreview(int32 Frame) {
	if (!CanScrubPreview()) {
		return;
	}

	FString Error;
	if (!ValidateInputs(Error)) {
		Fail(Error);
		return;
	}
	bPreviewPaused = true;
	PreviewAccumulator = 0.0;
	SeekTarget = FMath::Clamp(Frame, 0, IntervalCount);
	if (SeekTarget < SampleIndex) {
		DisplayPreviewPose(SeekTarget);
		SeekTarget = INDEX_NONE;
	}
}

void FKawaiiPhysicsBakeSession::SetPreviewLooping(bool bLoop) {
	bLoopPreview = bLoop;
}

bool FKawaiiPhysicsBakeSession::CanScrubPreview() const {
	return bPreviewAvailable && !bBaking;
}

bool FKawaiiPhysicsBakeSession::IsSeeking() const {
	return SeekTarget != INDEX_NONE;
}

int32 FKawaiiPhysicsBakeSession::GetPreviewFrame() const {
	return FMath::Max(0, DisplayedSample);
}

int32 FKawaiiPhysicsBakeSession::GetRequestedFrame() const {
	return IsSeeking() ? SeekTarget : GetPreviewFrame();
}

bool FKawaiiPhysicsBakeSession::IsBaking() const {
	return bRunning && bBaking;
}

float FKawaiiPhysicsBakeSession::GetPreviewTime() const {
	return Settings ? KawaiiPhysicsBakeSampling::SourceTime(GetPreviewFrame(), IntervalCount, SourceLength, Settings->SampleRate) : 0.f;
}

void FKawaiiPhysicsBakeSession::TickPreview(float DeltaSeconds) {
	FString Error;
	// Validate even while paused or replaying: cached poses belong to one graph/source version.
	if (!ValidateInputs(Error)) {
		Fail(Error);
		return;
	}
	if (!IsSeeking()) {
		if (bPreviewPaused) {
			Status = LOCTEXT("PreviewPaused", "预览已暂停。可拖动时间轴或逐帧检查姿态。");
			return;
		}
		if (DeltaSeconds >= 0.f) {
			PreviewAccumulator += FMath::Min(DeltaSeconds, 0.25f);
			if (PreviewAccumulator + 1.e-7 < FixedDelta) {
				return;
			}
			// Shared collision uses engine frame IDs: never evaluate multiple physics steps
			// in one engine frame or skip simulation samples to catch up after a stall.
			PreviewAccumulator = FMath::Min(PreviewAccumulator - FixedDelta, static_cast<double>(FixedDelta));
		}
		if (DisplayedSample >= IntervalCount) {
			if (!bLoopPreview) {
				bPreviewPaused = true;
				return;
			}
			DisplayPreviewPose(0);
			return;
		}
		if (DisplayedSample + 1 < SampleIndex) {
			DisplayPreviewPose(DisplayedSample + 1);
			Status = LOCTEXT("CachedPreview", "正在回放已缓存的物理预览，无需导出动画。");
			return;
		}
	}
	const bool bWarmUp = WarmUpIndex < Settings->WarmUpFrames;
	const float Time = bWarmUp ? 0.f : KawaiiPhysicsBakeSampling::SourceTime(SampleIndex, IntervalCount, SourceLength, Settings->SampleRate);
	if (!Evaluate(Time, Error)) {
		Fail(Error);
		return;
	}
	if (bWarmUp) {
		++WarmUpIndex;
		Status = FText::Format(LOCTEXT("PreviewWarmup", "预览预热：{0}/{1} 帧"), WarmUpIndex, Settings->WarmUpFrames);
		return;
	}

	CachePreviewPose();
	DisplayedSample = SampleIndex++;
	Status = IsSeeking()
		? FText::Format(LOCTEXT("Seeking", "正在补算物理预览：第 {0}/{1} 帧。已缓存的帧可立即拖动查看。"), DisplayedSample, SeekTarget)
		: LOCTEXT("LivePreview", "正在实时预览物理效果。拖动时间轴可检查指定帧，点击烘焙将重新模拟并导出。");
	if (IsSeeking() && DisplayedSample >= SeekTarget) {
		SeekTarget = INDEX_NONE;
	}
}

bool FKawaiiPhysicsBakeSession::Capture(FString& OutError) {
	for (const TUniquePtr<FPart>& Part : Parts) {
		const FReferenceSkeleton& Ref = Part->Mesh->GetRefSkeleton();
		const TArray<FTransform>& ComponentPose = Part->Component->GetComponentSpaceTransforms();
		if (ComponentPose.Num() < Ref.GetRawBoneNum()) {
			OutError = Part->Name.ToString() + TEXT("：部件最终姿态不完整。");
			return false;
		}
		for (int32 Bone = 0; Bone < Ref.GetRawBoneNum(); ++Bone) {
			const int32 Parent = Ref.GetParentIndex(Bone);
			FTransform Local = Parent == INDEX_NONE ? ComponentPose[Bone] : ComponentPose[Bone].GetRelativeTransform(ComponentPose[Parent]);
			Local.NormalizeRotation();
			if (!KawaiiPhysicsBakeSampling::IsValidTransform(Local)) {
				OutError = FString::Printf(TEXT("%s：骨骼 %s 在第 %d 个采样处产生了无效变换。"), *Part->Name.ToString(), *Ref.GetBoneName(Bone).ToString(), SampleIndex);
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
				OutError = TEXT("动画曲线包含无效数值。");
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

void FKawaiiPhysicsBakeSession::Tick(float DeltaSeconds) {
	if (!bRunning || LastEngineFrame == GFrameCounter) {
		return;
	}
	LastEngineFrame = GFrameCounter;
	if (bPreviewAvailable) {
		TickPreview(DeltaSeconds);
		return;
	}
	FString Error;
	const bool bWarmUp = WarmUpIndex < Settings->WarmUpFrames;
	const float Time = bWarmUp ? 0.f : KawaiiPhysicsBakeSampling::SourceTime(SampleIndex, IntervalCount, SourceLength, Settings->SampleRate);
	if (!Evaluate(Time, Error)) {
		Fail(Error);
		return;
	}
	if (bWarmUp) {
		++WarmUpIndex;
		Status = FText::Format(LOCTEXT("WarmupProgress", "预热：{0}/{1} 帧"), WarmUpIndex, Settings->WarmUpFrames);
		return;
	}
	if (bBaking && !Capture(Error)) {
		Fail(Error);
		return;
	}
	++SampleIndex;
	DisplayedSample = SampleIndex - 1;
	Status = FText::Format(LOCTEXT("SampleProgress", "采样：{0}/{1} 帧，采样帧率 {2}"), SampleIndex, IntervalCount + 1, Settings->SampleRate);
	if (SampleIndex > IntervalCount) {
		bRunning = false;
		if (bBaking) {
			bBaking = false;
			if (!WriteAssets(Error)) {
				Fail(Error);
				return;
			}
			Status = FText::Format(
				LOCTEXT("Complete", "已保存 {0} 条同步动画（{1} 个关键帧，帧率 {2}）。保留根骨骼运动，角色组件保持原位。"),
				OutputAssets.Num(),
				SampleIndex,
				Settings->SampleRate
			);
		} else {
			Status = LOCTEXT("PreviewComplete", "预览完成。烘焙将从全新的预热状态重新模拟。");
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
			OutError = TEXT("输出名称已存在，请重新开始烘焙。已有资产未被覆盖。");
			return false;
		}
		PackageNames.Add(PackageName);
		for (const FRawAnimSequenceTrack& Track : Part->Tracks) {
			if (Track.PosKeys.Num() != IntervalCount + 1
				|| Track.RotKeys.Num() != IntervalCount + 1
				|| Track.ScaleKeys.Num() != IntervalCount + 1)
			{
				OutError = TEXT("各部件的采样数量不一致，未创建输出资产。");
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
				OutError = FString::Printf(TEXT("%s：骨架中缺少网格体骨骼 %s，无法导出该骨骼的烘焙轨道。"), *Part->Name.ToString(), *BoneName.ToString());
				return false;
			}
			if (Skeleton->GetBoneTranslationRetargetingMode(SkeletonBone) != EBoneTranslationRetargetingMode::Skeleton) {
				continue;
			}
			const FVector ReferenceTranslation = MeshRef.GetRefBonePose()[Bone].GetTranslation();
			for (int32 Frame = 0; Frame < Part->Tracks[Bone].PosKeys.Num(); ++Frame) {
				if (!FVector(Part->Tracks[Bone].PosKeys[Frame]).Equals(ReferenceTranslation, 0.01)) {
					OutError = FString::Printf(
						TEXT("%s：骨骼 %s 在第 %d 帧改变了局部平移，但骨架的平移重定向设置会丢弃该运动。请将对应骨骼的平移重定向设为 Animation，或移除图中的平移修改。工具未修改骨架或创建输出资产。"),
						*Part->Name.ToString(),
						*BoneName.ToString(),
						Frame
					);
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
		Controller.OpenBracket(LOCTEXT("Populate", "写入物理烘焙数据"), false);
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
			OutError = TEXT("动画数据构建失败。新资产尚未保存，可检查后处理；已有资产未修改。");
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
					OutError = FString::Printf(
						TEXT("%s：骨骼 %s 在第 %d 帧的导出回放与采样姿态不同，请检查平移重定向和网格体参考姿态。新动画尚未保存，已有骨架和动画未修改。"),
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
			OutError = FString::Printf(TEXT("无法保存 %s，本次结果尚未完整写入磁盘。已创建的资产仍可手动保存。"), *Filename);
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
