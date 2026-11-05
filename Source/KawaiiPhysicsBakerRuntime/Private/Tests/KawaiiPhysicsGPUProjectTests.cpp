#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Animation/AttributesRuntime.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "UObject/StrongObjectPtr.h"

#include "KawaiiPhysicsGPUShaders.h"
#include "AnimNode_KawaiiPhysicsIKCorrection.h"
#include "KawaiiClothingProxyPlayback.h"
#include "KawaiiClothingProxySnapshot.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKawaiiGPUProjectClimbTest,
	"KawaiiPhysicsBaker.GPU.ProjectClimbParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FKawaiiGPUProjectClimbTest::RunTest(const FString&) {
	using namespace KawaiiPhysicsGPU;
	if (!GDynamicRHI || GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) {
		AddWarning(TEXT("Climb parity requires a real SM5 RHI."));
		return true;
	}
	const TCHAR* SetPath = TEXT("/Game/Assets/Clothing/Base/BakeKawaiiPhysics/Cloting_Base_ProxySet");
	if (!FPackageName::DoesPackageExist(SetPath)) {
		AddWarning(TEXT("Project climb assets unavailable."));
		return true;
	}
	UKawaiiClothingProxyMotionSet* Set = LoadObject<UKawaiiClothingProxyMotionSet>(nullptr, SetPath);
	UAnimSequence* Body = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/Assets/Anime/Lime/FreeWalkJog/Jump/Jump_Climb_2h"));
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Assets/Anime/Lime/Lime"));
	UKawaiiPhysicsIKCorrectionProfile* Profile =
		LoadObject<UKawaiiPhysicsIKCorrectionProfile>(nullptr, TEXT("/Game/BP/AnimeBP/Lime/DA_AnimBP_Lime_IKCorrection"));
	if (!TestTrue(TEXT("Current project assets load"), Set && Body && Mesh && Profile)) {
		return false;
	}
	UKawaiiClothingProxyMotion* Motion = nullptr;
	for (UKawaiiClothingProxyMotion* Candidate : Set->Motions) {
		if (Candidate && Candidate->SourceAnimation.ToSoftObjectPath() == FSoftObjectPath(Body)) {
			Motion = Candidate;
			break;
		}
	}
	if (!TestNotNull(TEXT("Current climb mapping"), Motion)) {
		return false;
	}
	KawaiiClothingProxy::FSnapshot Snapshot;
	FString Error;
	if (!KawaiiClothingProxy::BuildSnapshot(*Motion, Snapshot, Error)) {
		AddError(Error);
		return false;
	}
	TStrongObjectPtr<USkeletalMeshComponent> Component(NewObject<USkeletalMeshComponent>());
	Component->SetSkeletalMesh(Mesh);
	TStrongObjectPtr<UAnimInstance> Instance(NewObject<UAnimInstance>(Component.Get()));
	TArray<FBoneIndexType> Indices;
	for (int32 Bone = 0; Bone < Mesh->GetRefSkeleton().GetNum(); ++Bone) {
		Indices.Add(FBoneIndexType(Bone));
	}
	const UE::Anim::FCurveFilterSettings Filter(UE::Anim::ECurveFilterMode::None);
	Instance->GetRequiredBones().InitializeTo(Indices, Filter, *Mesh);
	FAnimInstanceProxy Proxy(Instance.Get());
	Proxy.GetRequiredBones().InitializeTo(Indices, Filter, *Mesh);
	FAnimNode_KawaiiPhysicsIKCorrection CPU, GPU;
	CPU.Profile = GPU.Profile = Profile;
	CPU.PreUpdate(Instance.Get());
	GPU.PreUpdate(Instance.Get());
	// Exercise the production packet builder without installing a mesh deformer on a test component.
	GPU.GPUState = MakeShared<FState, ESPMode::ThreadSafe>();
	GPU.GPUState->bResourcesReady = true;
	auto Render = MakeShared<FRenderState, ESPMode::ThreadSafe>();
	const auto ComponentPose = [](const FCompactPose& Pose) {
		TArray<FTransform> Out;
		for (int32 Bone = 0; Bone < Pose.GetNumBones(); ++Bone) {
			const FCompactPoseBoneIndex Index(Bone);
			const int32 Parent = Pose.GetBoneContainer().GetParentBoneIndex(Index).GetInt();
			Out.Add(Parent == INDEX_NONE ? Pose[Index] : Pose[Index] * Out[Parent]);
		}
		return Out;
	};
	TArray<FName> Excluded;
	for (const auto& Chain : Profile->Chains) {
		Excluded.Add(Chain.RootBone.BoneName);
	}
	double MaxError = 0., MaxRotationError = 0.;
	int32 WorstFrame = 0, WorstBone = 0;
	FString Rows = TEXT("frame,bone,input_x,input_y,input_z,cpu_x,cpu_y,cpu_z,gpu_x,gpu_y,gpu_z,local_rotation_error_deg\n");
	for (int32 FrameIndex = 0; FrameIndex < Snapshot.NumFrames; ++FrameIndex) {
		FMemMark Mark(FMemStack::Get());
		FPoseContext Base(&Proxy);
		FAnimationPoseData PoseData(Base);
		const float Time = FMath::Min(float(FrameIndex) / Snapshot.SampleRate, Snapshot.SourceDuration);
		Body->GetAnimationPose(PoseData, FAnimExtractContext(double(Time), true));
		const auto Reference = ComponentPose(Base.Pose);
		KawaiiClothingProxy::FLiveBodyPose Live;
		if (!KawaiiClothingProxy::BuildLiveBodyPose(Base.Pose, Excluded, Live, Error)
			|| !KawaiiClothingProxy::SampleSnapshots({{&Snapshot, Time, 1.f}}, Snapshot.RelativeTransform, CPU.ExternalCapsules, Error, nullptr, &Live))
		{
			AddError(Error);
			return false;
		}
		GPU.ExternalCapsules = CPU.ExternalCapsules;
		FPoseContext CPUOutput(Base), GPUOutput(Base);
		CPUOutput.Pose.CopyBonesFrom(Base.Pose);
		GPUOutput.Pose.CopyBonesFrom(Base.Pose);
		const float DeltaTime = 1.f / Snapshot.SampleRate;
		CPU.AccumulateDeltaTime(DeltaTime);
		GPU.AccumulateDeltaTime(DeltaTime);
		CPU.CorrectPose(CPUOutput, Base);
		GPU.GPUState->Register(&GPU, FrameIndex + 1);
		GPU.CorrectPose(GPUOutput, Base);
		auto Packet = GPU.GPUState->Read();
		if (!Packet) {
			AddError(GPU.GetCorrectionDebugReason());
			return false;
		}
		const auto Expected = ComponentPose(CPUOutput.Pose);
		TArray<FVector4f> Result;
		ENQUEUE_RENDER_COMMAND(KawaiiProjectClimb)([Packet, Render, &Result](FRHICommandListImmediate& RHICmdList) {
			FRHIGPUBufferReadback Readback(TEXT("KawaiiGPU.ProjectClimb"));
			FRDGBuilder Graph(RHICmdList);
			auto* Deltas = AddSolvePass(Graph, *Packet, *Render, false);
			AddEnqueueCopyPass(Graph, &Readback, Deltas, 0u);
			Graph.Execute();
			RHICmdList.BlockUntilGPUIdle();
			const uint32 Count = Packet->PointBones.Num() * 3;
			if (const void* Data = Readback.Lock(Count * sizeof(FVector4f))) {
				Result.SetNumUninitialized(Count);
				FMemory::Memcpy(Result.GetData(), Data, Count * sizeof(FVector4f));
				Readback.Unlock();
			}
		});
		FlushRenderingCommands();
		if (!TestEqual(TEXT("GPU readback size"), Result.Num(), Packet->PointBones.Num() * 3)) {
			return false;
		}
		for (int32 Point = 0; Point < Packet->PointBones.Num(); ++Point) {
			const int32 Bone = Packet->PointBones[Point];
			const FVector4f P(FVector3f(Reference[Bone].GetTranslation()), 1.f);
			const FVector Actual(Dot4(Result[Point * 3], P), Dot4(Result[Point * 3 + 1], P), Dot4(Result[Point * 3 + 2], P));
			const double Distance = FVector::Distance(Actual, Expected[Bone].GetTranslation());
			for (const FVector& Axis : {FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector}) {
				const FVector4f BaseAxis(FVector3f(Reference[Bone].TransformVector(Axis)), 0.f);
				const FVector GPUAxis(Dot4(Result[Point * 3], BaseAxis), Dot4(Result[Point * 3 + 1], BaseAxis), Dot4(Result[Point * 3 + 2], BaseAxis));
				const double Cosine = FVector::DotProduct(GPUAxis.GetSafeNormal(), Expected[Bone].TransformVector(Axis).GetSafeNormal());
				MaxRotationError = FMath::Max(MaxRotationError, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Cosine, -1., 1.))));
			}
			if (Actual.ContainsNaN()) {
				AddError(TEXT("Nonfinite GPU result"));
				return false;
			}
			if (Distance > MaxError) {
				MaxError = Distance;
				WorstFrame = FrameIndex;
				WorstBone = Bone;
			}
			const int32 Parent = Mesh->GetRefSkeleton().GetParentIndex(Bone);
			const FQuat Local = Expected[Bone].GetRelativeTransform(Expected[Parent]).GetRotation();
			const FQuat BaseLocal = Reference[Bone].GetRelativeTransform(Reference[Parent]).GetRotation();
			const FVector RefP = Reference[Bone].GetTranslation(), CP = Expected[Bone].GetTranslation();
			Rows += FString::Printf(
				TEXT("%d,%s,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.8f,%.6f\n"),
				FrameIndex,
				*Mesh->GetRefSkeleton().GetBoneName(Bone).ToString(),
				RefP.X,
				RefP.Y,
				RefP.Z,
				CP.X,
				CP.Y,
				CP.Z,
				Actual.X,
				Actual.Y,
				Actual.Z,
				FMath::RadiansToDegrees(Local.AngularDistance(BaseLocal))
			);
		}
	}
	AddInfo(FString::Printf(
		TEXT("Climb CPU/GPU: %d frames, max error %.6f cm at frame %d bone %s."),
		Snapshot.NumFrames,
		MaxError,
		WorstFrame,
		*Mesh->GetRefSkeleton().GetBoneName(WorstBone).ToString()
	));
	FFileHelper::SaveStringToFile(Rows, *(FPaths::ProjectSavedDir() / TEXT("KawaiiMotionWarpAudit/climb-cpu-gpu.csv")));
	TestTrue(TEXT("Actual climb GPU output matches CPU preview within 0.1 cm"), MaxError < 0.1);
	AddInfo(FString::Printf(TEXT("Climb CPU/GPU maximum bone-axis rotation difference: %.6f degrees."), MaxRotationError));
	TestTrue(TEXT("Actual climb GPU bone orientation matches CPU preview within 0.1 degree"), MaxRotationError < .1);
	return true;
}

#endif
