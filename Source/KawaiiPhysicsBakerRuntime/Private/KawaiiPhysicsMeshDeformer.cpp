#include "KawaiiPhysicsMeshDeformer.h"

#include "Async/Async.h"
#include "Components/SkinnedMeshComponent.h"
#include "ComputeSystemInterface.h"
#include "ComputeWorkerInterface.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/RenderCommandPipes.h"
#include "SceneInterface.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h"

#include "KawaiiPhysicsGPUShaders.h"
#include "KawaiiPhysicsGPUCompute.h"

DEFINE_LOG_CATEGORY_STATIC(LogKawaiiGPUDeformer, Log, All);
DECLARE_GPU_STAT_NAMED(KawaiiGPUCorrection, TEXT("Kawaii Physics Correction"));

IMPLEMENT_GLOBAL_SHADER(FKawaiiCorrectionSolveCS, "/Plugin/KawaiiPhysicsBakerRuntime/Private/KawaiiCorrectionSolve.usf", "Main", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FKawaiiCorrectionSkinCS, "/Plugin/KawaiiPhysicsBakerRuntime/Private/KawaiiCorrectionSkin.usf", "Main", SF_Compute);

namespace KawaiiPhysicsGPU {
	FRDGBufferRef AddSolvePass(FRDGBuilder& Graph, const FFrame& Frame, FRenderState& State, bool bReset) {
		const uint32 Count = Frame.Points.Num() / PointStride;
		bReset |= !State.History || State.PointCount != Count || State.Topology != Frame.Topology || State.ResetSerial != Frame.ResetSerial;
		const bool bTemporal = Frame.SettleTime > 0.f || Frame.SmoothingTime > 0.f;
		FRDGBufferRef HistoryBuffer;
		if (!State.History || State.PointCount != Count) {
			HistoryBuffer = Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Count * 2), TEXT("KawaiiGPU.History"));
		} else {
			HistoryBuffer = Graph.RegisterExternalBuffer(State.History);
		}
		// A long gap/reset must also invalidate the stored validity bits for the NEXT frame.
		// A zero-delta re-evaluation is stateless but preserves history, just like the CPU path.
		if (bReset || !bTemporal) {
			AddClearUAVPass(Graph, Graph.CreateUAV(HistoryBuffer), 0u);
		}
		Graph.QueueBufferExtraction(HistoryBuffer, &State.History);
		State.PointCount = Count;
		State.Topology = Frame.Topology;
		State.ResetSerial = Frame.ResetSerial;
		const FVector4f EmptyCapsule(0, 0, 0, 0);
		auto* Parameters = Graph.AllocParameters<FKawaiiCorrectionSolveCS::FParameters>();
		const bool bAdvance = bTemporal && Frame.DeltaTime > 0.f && Frame.DeltaTime <= 0.25f;
		Parameters->ResetHistory = bReset || !bAdvance;
		Parameters->AdvanceHistory = bAdvance;
		Parameters->UseSideHistory = Frame.SettleTime > 0.f;
		Parameters->UseSmoothing = Frame.SmoothingTime > 0.f;
		Parameters->SettleAlpha = Frame.SettleTime > 0.f ? 1.f - FMath::Exp(-Frame.DeltaTime / Frame.SettleTime) : 1.f;
		Parameters->SmoothingAlpha = Frame.SmoothingTime > 0.f ? 1.f - FMath::Exp(-Frame.DeltaTime / Frame.SmoothingTime) : 1.f;
		Parameters->Chains = Graph.CreateSRV(CreateStructuredBuffer(
			Graph,
			TEXT("KawaiiGPU.Chains"),
			sizeof(FChain),
			Frame.Chains.Num(),
			Frame.Chains.GetData(),
			Frame.Chains.Num() * sizeof(FChain)
		));
		Parameters->PointInput = Graph.CreateSRV(CreateStructuredBuffer(
			Graph,
			TEXT("KawaiiGPU.Input"),
			sizeof(FVector4f),
			Frame.Points.Num(),
			Frame.Points.GetData(),
			Frame.Points.Num() * sizeof(FVector4f)
		));
		Parameters->Capsules = Graph.CreateSRV(CreateStructuredBuffer(
			Graph,
			TEXT("KawaiiGPU.Capsules"),
			sizeof(FVector4f),
			FMath::Max(1, Frame.Capsules.Num()),
			Frame.Capsules.IsEmpty() ? &EmptyCapsule : Frame.Capsules.GetData(),
			FMath::Max(1, Frame.Capsules.Num()) * sizeof(FVector4f)
		));
		Parameters->History = Graph.CreateUAV(HistoryBuffer);
		auto* Deltas = Graph.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Count * 3), TEXT("KawaiiGPU.Deltas"));
		Parameters->Deltas = Graph.CreateUAV(Deltas);
		TShaderMapRef<FKawaiiCorrectionSolveCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(
			Graph,
			RDG_EVENT_NAME("KawaiiGPU Solve (%d chains)", Frame.Chains.Num()),
			Shader,
			Parameters,
			FIntVector(Frame.Chains.Num(), 1, 1)
		);
		return Deltas;
	}

	static bool AddDeformPasses(FRDGBuilder& Graph, FSkeletalMeshObject* Object, const FFrame* Frame, FRenderState& State) {
		if (!Object || Object->IsCPUSkinned()) {
			return false;
		}
		// A mesh deformer replaces ordinary skinning even when there is no correction packet.
		// Resetting its vertex factory would display reference-pose vertices instead of the live pose.
		const bool bApplyCorrection = Frame && !Frame->Chains.IsEmpty();
		const int32 LODIndex = Object->GetLOD();
		const auto& LODs = Object->GetSkeletalMeshRenderData().LODRenderData;
		if (!LODs.IsValidIndex(LODIndex)) {
			return false;
		}
		const auto& LOD = LODs[LODIndex];
		const auto* Weights = Object->GetSkinWeightVertexBuffer(LODIndex);
		if (!Weights || !Weights->GetDataVertexBuffer()->GetSRV() || !LOD.StaticVertexBuffers.PositionVertexBuffer.GetSRV()
			|| !LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV())
		{
			return false;
		}
		const bool bUnlimited = Weights->GetVariableBonesPerVertex();
		if (bUnlimited && !Weights->GetLookupVertexBuffer()->GetSRV()) {
			return false;
		}
		TArray<int32> BoneMap;
		TArray<uint32> SectionOffsets;
		for (int32 SectionIndex = 0; SectionIndex < LOD.RenderSections.Num(); ++SectionIndex) {
			const auto& Section = LOD.RenderSections[SectionIndex];
			if (Section.HasClothingData() || !FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(Object, LODIndex, SectionIndex, false)) {
				return false;
			}
			SectionOffsets.Add(BoneMap.Num());
			if (bApplyCorrection) {
				for (uint16 Bone : Section.BoneMap) {
					BoneMap.Add(Frame->BoneToPoint.IsValidIndex(Bone) ? Frame->BoneToPoint[Bone] : INDEX_NONE);
				}
			}
		}
		if (bApplyCorrection && BoneMap.IsEmpty()) {
			return false;
		}
		const bool bReset = State.LOD != LODIndex || State.LastRenderFrame + 1 < GFrameNumberRenderThread || (bApplyCorrection && Frame->DeltaTime > 0.25f);
		if (!bApplyCorrection) {
			State.History.SafeRelease();
			State.CorrectionDeltas.SafeRelease();
			State.Sequence = 0;
		}
		{
			RDG_EVENT_SCOPE(Graph, "KawaiiGPU Correction");
			RDG_GPU_STAT_SCOPE(Graph, KawaiiGPUCorrection);
			FRDGBufferRef DeltaBuffer = nullptr;
			FRDGBufferRef MapBuffer = nullptr;
			if (bApplyCorrection) {
				// Reusing a correction must not skip skinning: bones/morphs can be updated again
				// for the same animation evaluation, e.g. while recreating render resources.
				if (!bReset && State.Sequence == Frame->Sequence && State.CorrectionDeltas) {
					DeltaBuffer = Graph.RegisterExternalBuffer(State.CorrectionDeltas);
				} else {
					DeltaBuffer = AddSolvePass(Graph, *Frame, State, bReset);
					Graph.QueueBufferExtraction(DeltaBuffer, &State.CorrectionDeltas);
				}
				MapBuffer =
					CreateStructuredBuffer(Graph, TEXT("KawaiiGPU.BoneMap"), sizeof(int32), BoneMap.Num(), BoneMap.GetData(), BoneMap.Num() * sizeof(int32));
			}
			auto* Positions = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer(Graph, Object, LODIndex, bReset, TEXT("KawaiiGPU.Positions"));
			auto* Tangents = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryTangentBuffer(Graph, Object, LODIndex, TEXT("KawaiiGPU.Tangents"));
			auto* PositionUAV = Graph.CreateUAV(Positions, PF_R32_FLOAT);
			auto* TangentUAV = Graph.CreateUAV(Tangents, PF_R16G16B16A16_SNORM);
			FKawaiiCorrectionSkinCS::FPermutationDomain Permutation;
			Permutation.Set<FKawaiiCorrectionSkinCS::FUnlimited>(bUnlimited);
			Permutation.Set<FKawaiiCorrectionSkinCS::FIndex16>(Weights->Use16BitBoneIndex());
			Permutation.Set<FKawaiiCorrectionSkinCS::FWeight16>(Weights->Use16BitBoneWeight());
			Permutation.Set<FKawaiiCorrectionSkinCS::FApplyCorrection>(bApplyCorrection);
			TShaderMapRef<FKawaiiCorrectionSkinCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Permutation);
			for (int32 SectionIndex = 0; SectionIndex < LOD.RenderSections.Num(); ++SectionIndex) {
				const auto& Section = LOD.RenderSections[SectionIndex];
				if (!Section.NumVertices) {
					continue;
				}
				auto* Parameters = Graph.AllocParameters<FKawaiiCorrectionSkinCS::FParameters>();
				Parameters->NumVertices = Section.NumVertices;
				Parameters->BaseVertex = Section.BaseVertexIndex;
				Parameters->NumInfluences = Weights->GetMaxBoneInfluences();
				Parameters->WeightStride = Weights->GetConstantInfluencesVertexStride();
				Parameters->WeightIndexSize = Weights->GetBoneIndexByteSize() | (Weights->GetBoneWeightByteSize() << 8);
				Parameters->BoneMapOffset = SectionOffsets[SectionIndex];
				Parameters->BoneMatrices = FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(Object, LODIndex, SectionIndex, false);
				Parameters->WeightStream = Weights->GetDataVertexBuffer()->GetSRV();
				Parameters->WeightLookup = bUnlimited ? Weights->GetLookupVertexBuffer()->GetSRV() : Weights->GetDataVertexBuffer()->GetSRV();
				auto* Morph = FSkeletalMeshDeformerHelpers::GetMorphTargetBufferForReading(Object, LODIndex, SectionIndex, GFrameNumberRenderThread, false);
				Parameters->HasMorph = Morph != nullptr;
				Parameters->MorphBuffer = Morph ? Morph : LOD.StaticVertexBuffers.PositionVertexBuffer.GetSRV();
				Parameters->StaticPositions = LOD.StaticVertexBuffers.PositionVertexBuffer.GetSRV();
				Parameters->StaticTangents = LOD.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();
				Parameters->BoneToPoint = MapBuffer ? Graph.CreateSRV(MapBuffer) : nullptr;
				Parameters->Deltas = DeltaBuffer ? Graph.CreateSRV(DeltaBuffer) : nullptr;
				Parameters->OutputPositions = PositionUAV;
				Parameters->OutputTangents = TangentUAV;
				FComputeShaderUtils::AddPass(
					Graph,
					RDG_EVENT_NAME("KawaiiGPU Skin (section %d)", SectionIndex),
					Shader,
					Parameters,
					FIntVector(FMath::DivideAndRoundUp(Section.NumVertices, 64u), 1, 1)
				);
			}
			FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(Graph.RHICmdList, Object, LODIndex);
		}
		if (bApplyCorrection && State.Sequence == 0) {
			UE_LOG(
				LogKawaiiGPUDeformer,
				Display,
				TEXT("GPU correction active: %d chains, %d points, %d vertices, LOD %d. No CPU readback."),
				Frame->Chains.Num(),
				Frame->Points.Num() / PointStride,
				LOD.GetNumVertices(),
				LODIndex
			);
		}
		State.Sequence = bApplyCorrection ? Frame->Sequence : 0;
		State.LastRenderFrame = GFrameNumberRenderThread;
		State.LOD = LODIndex;
		return true;
	}

	struct FComputeWork
	{
		FSkeletalMeshObject* MeshObject = nullptr;
		TSharedPtr<const FFrame, ESPMode::ThreadSafe> Frame;
		TSharedPtr<FRenderState, ESPMode::ThreadSafe> RenderState;
		TSharedPtr<FState, ESPMode::ThreadSafe> Handoff;
		FSimpleDelegate Fallback;
#if !UE_BUILD_SHIPPING
		TArray<FMatrix44f> ExpectedPose;
		uint64 GameFrame = 0;
#endif
	};

#if !UE_BUILD_SHIPPING
	static TAutoConsoleVariable<int32> CVarValidateFrame(
		TEXT("a.KawaiiPhysicsBaker.GPU.ValidateFrame"),
		0,
		TEXT(
			"Development diagnostic: compare the queued component pose with the pose consumed by GPU skinning. 0 = off, 1 = log each submission. Adds CPU work."
		),
		ECVF_Default
	);
#endif

	class FComputeWorker : public IComputeTaskWorker
	{
	public:
		void Enqueue(FComputeWork&& Work) {
			check(IsInRenderingThread());
			// Multiple updates can precede one scene render. Only the newest pose may be skinned.
			for (FComputeWork& Pending : PendingWork) {
				if (Pending.RenderState == Work.RenderState) {
					Pending = MoveTemp(Work);
					return;
				}
			}
			PendingWork.Add(MoveTemp(Work));
		}

		virtual bool HasWork(FName ExecutionGroup) const override {
			return ExecutionGroup == ComputeTaskExecutionGroup::EndOfFrameUpdate && !PendingWork.IsEmpty();
		}

		static void Submit(FRDGBuilder& Graph, FComputeWork&& InWork) {
			// Resource release invalidates work before the old FSkeletalMeshObject can be destroyed.
			if (!InWork.RenderState || !InWork.RenderState->bActive) {
				return;
			}
			// Keep extraction targets and upload snapshots alive through deferred RDG execution.
			FComputeWork& Work = *Graph.AllocObject<FComputeWork>(MoveTemp(InWork));
			if (!AddDeformPasses(Graph, Work.MeshObject, Work.Frame.Get(), *Work.RenderState)) {
				if (Work.Handoff) {
					Work.Handoff->RequireCPU(TEXT("GPU skinning buffers unavailable after skeletal update"));
					UE_LOG(LogKawaiiGPUDeformer, Warning, TEXT("GPU skinning buffers unavailable after skeletal update; returning to CPU correction."));
				}
				Work.RenderState->History.SafeRelease();
				Work.RenderState->CorrectionDeltas.SafeRelease();
				Work.RenderState->Sequence = 0;
				Work.RenderState->LOD = INDEX_NONE;
				Work.Fallback.ExecuteIfBound();
				return;
			}
#if !UE_BUILD_SHIPPING
			if (!Work.ExpectedPose.IsEmpty()) {
				const TArray<FMatrix44f>& ActualPose = Work.MeshObject->GetReferenceToLocalMatrices();
				float MaxError = 0.f;
				int32 MismatchedBones = 0;
				for (int32 Bone = 0; Bone < Work.ExpectedPose.Num(); ++Bone) {
					float BoneError = 0.f;
					if (ActualPose.IsValidIndex(Bone)) {
						for (int32 Row = 0; Row < 4; ++Row) {
							for (int32 Column = 0; Column < 4; ++Column) {
								BoneError = FMath::Max(BoneError, FMath::Abs(ActualPose[Bone].M[Row][Column] - Work.ExpectedPose[Bone].M[Row][Column]));
							}
						}
					} else {
						BoneError = MAX_flt;
					}
					MaxError = FMath::Max(MaxError, BoneError);
					MismatchedBones += BoneError > 0.001f;
				}
				UE_LOG(
					LogKawaiiGPUDeformer,
					Display,
					TEXT("FrameValidation frame=%llu bones=%d mismatched=%d max_error=%.6f corrected=%d"),
					Work.GameFrame,
					Work.ExpectedPose.Num(),
					MismatchedBones,
					MaxError,
					Work.Frame.IsValid()
				);
			}
#endif
		}

		virtual void SubmitWork(FRDGBuilder& Graph, FName ExecutionGroup, ERHIFeatureLevel::Type FeatureLevel) override {
			check(IsInRenderingThread());
			if (ExecutionGroup != ComputeTaskExecutionGroup::EndOfFrameUpdate) {
				return;
			}
			for (FComputeWork& Work : PendingWork) {
				Submit(Graph, MoveTemp(Work));
			}
			PendingWork.Reset();
		}

	private:
		TArray<FComputeWork> PendingWork;
	};

	class FComputeSystem : public IComputeSystem
	{
	public:
		virtual void CreateWorkers(const FSceneInterface* Scene, TArray<IComputeTaskWorker*>& OutWorkers) override {
			FScopeLock Lock(&Mutex);
			TSharedPtr<FComputeWorker, ESPMode::ThreadSafe> Worker = MakeShared<FComputeWorker, ESPMode::ThreadSafe>();
			Workers.Add(Scene, Worker);
			OutWorkers.Add(Worker.Get());
		}

		virtual void DestroyWorkers(const FSceneInterface* Scene, TArray<IComputeTaskWorker*>& InOutWorkers) override {
			FScopeLock Lock(&Mutex);
			if (TSharedPtr<FComputeWorker, ESPMode::ThreadSafe>* Worker = Workers.Find(Scene)) {
				InOutWorkers.Remove(Worker->Get());
				Workers.Remove(Scene);
			}
		}

		TSharedPtr<FComputeWorker, ESPMode::ThreadSafe> FindWorker(const FSceneInterface* Scene) {
			FScopeLock Lock(&Mutex);
			return Workers.FindRef(Scene);
		}

	private:
		FCriticalSection Mutex;
		TMap<const FSceneInterface*, TSharedPtr<FComputeWorker, ESPMode::ThreadSafe>> Workers;
	};

	static TUniquePtr<FComputeSystem> ComputeSystem;

	void InitializeComputeSystem() {
		ComputeSystem = MakeUnique<FComputeSystem>();
		ComputeSystemInterface::RegisterSystem(ComputeSystem.Get());
	}

	void ShutdownComputeSystem() {
		ComputeSystemInterface::UnregisterSystem(ComputeSystem.Get());
		FlushRenderingCommands();
		ComputeSystem.Reset();
	}
}

UMeshDeformerInstanceSettings* UKawaiiPhysicsMeshDeformer::CreateSettingsInstance(UMeshComponent*) {
	return nullptr;
}

UMeshDeformerInstance* UKawaiiPhysicsMeshDeformer::CreateInstance(UMeshComponent* Component, UMeshDeformerInstanceSettings*) {
	auto* Instance = NewObject<UKawaiiPhysicsMeshDeformerInstance>(Component);
	Instance->Mesh = Cast<USkinnedMeshComponent>(Component);
	Instance->State = MakeShared<KawaiiPhysicsGPU::FState, ESPMode::ThreadSafe>();
	return Instance;
}

void UKawaiiPhysicsMeshDeformerInstance::AllocateResources() {
	RenderState = MakeShared<KawaiiPhysicsGPU::FRenderState, ESPMode::ThreadSafe>();
	FScopeLock Lock(&State->Mutex);
	State->bResourcesReady = true;
	++State->ResetSerial;
}

void UKawaiiPhysicsMeshDeformerInstance::ReleaseResources() {
	if (State) {
		FScopeLock Lock(&State->Mutex);
		State->bResourcesReady = false;
		State->bCorrectionRequested = false;
		State->Frame.Reset();
		++State->ResetSerial;
	}
	auto Retired = MoveTemp(RenderState);
	ENQUEUE_RENDER_COMMAND(KawaiiGPURetire)([Retired = MoveTemp(Retired)](FRHICommandListImmediate&) mutable {
		if (Retired) {
			Retired->bActive = false;
		}
		Retired.Reset();
	});
}

void UKawaiiPhysicsMeshDeformerInstance::EnqueueWork(const FEnqueueWorkDesc& Desc) {
	TRACE_CPUPROFILER_EVENT_SCOPE(KawaiiBaker_GPU_Submit);
	if (Desc.WorkLoadType != WorkLoad_Update) {
		return;
	}
	auto Packet = State ? State->Read() : nullptr;
	// Nodes may stop receiving PreUpdate altogether after an AnimGraph branch/profile change.
	// Never apply their last correction to a later CPU pose, even if the deformer is still installed.
	if (Packet && Packet->GameFrame != GFrameCounter) {
		Packet.Reset();
	}
	if (Packet && Mesh) {
		int32 MismatchedBone = INDEX_NONE;
		if (!KawaiiPhysicsGPU::MatchesComponentPose(*Packet, Mesh->GetComponentSpaceTransforms(), MismatchedBone)) {
			const FString Reason = FString::Printf(
				TEXT("final pose differs from correction baseline at bone %s (%d)"),
				*Mesh->GetBoneName(MismatchedBone).ToString(),
				MismatchedBone
			);
			State->RequireCPU(Reason);
			UE_LOG(
				LogKawaiiGPUDeformer,
				Warning,
				TEXT(
					"%s: %s; returning to CPU correction. Move the correction node after bone edits, including Control Rig/IK and post-process edits. GPU 0 then GPU 1 retries."
				),
				*Mesh->GetPathName(),
				*Reason
			);
			// Render the final CPU baseline on this transition frame; the next evaluation solves on CPU.
			Packet.Reset();
		}
	}
	KawaiiPhysicsGPU::FComputeWork Work;
	Work.MeshObject = Mesh ? Mesh->MeshObject : nullptr;
	Work.Frame = Packet;
	Work.RenderState = RenderState;
	Work.Handoff = State;
	Work.Fallback = Desc.FallbackDelegate;
#if !UE_BUILD_SHIPPING
	if (Mesh && KawaiiPhysicsGPU::CVarValidateFrame.GetValueOnAnyThread() != 0) {
		const FSkeletalMeshRenderData* MeshData = Mesh->GetSkeletalMeshRenderData();
		const int32 LOD = MeshData ? FMath::Max(Mesh->GetPredictedLODLevel(), int32(MeshData->PendingFirstLODIdx)) : INDEX_NONE;
		if (MeshData && MeshData->LODRenderData.IsValidIndex(LOD)) {
			Mesh->GetCurrentRefToLocalMatrices(Work.ExpectedPose, LOD);
			Work.GameFrame = GFrameCounter;
		}
	}
#endif
	if (Desc.ExecutionGroup == ExecutionGroup_Immediate) {
		// Match the engine's immediate deformer path: flush skeletal pipe commands, not the GPU.
		check(IsInGameThread());
		UE::RenderCommandPipe::FSyncScope SyncScope({&UE::RenderCommandPipe::SkeletalMesh});
		ENQUEUE_RENDER_COMMAND(KawaiiGPUImmediate)([Work = MoveTemp(Work)](FRHICommandListImmediate& RHICmdList) mutable {
			FRDGBuilder Graph(RHICmdList);
			KawaiiPhysicsGPU::FComputeWorker::Submit(Graph, MoveTemp(Work));
			Graph.Execute();
		});
	} else {
		TSharedPtr<KawaiiPhysicsGPU::FComputeWorker, ESPMode::ThreadSafe> Worker = KawaiiPhysicsGPU::ComputeSystem->FindWorker(Desc.Scene);
		ENQUEUE_RENDER_COMMAND(KawaiiGPUEnqueue)([Worker, Work = MoveTemp(Work)](FRHICommandListImmediate&) mutable {
			if (Worker) {
				Worker->Enqueue(MoveTemp(Work));
			} else {
				if (Work.Handoff) {
					Work.Handoff->RequireCPU(TEXT("scene compute worker unavailable"));
					UE_LOG(LogKawaiiGPUDeformer, Warning, TEXT("Scene compute worker unavailable; returning to CPU correction."));
				}
				Work.Fallback.ExecuteIfBound();
			}
		});
	}
	if (!Packet && Desc.ExecutionGroup != ExecutionGroup_Immediate && State) {
		bool bNeedsRetirement = false;
		{
			FScopeLock Lock(&State->Mutex);
			bNeedsRetirement = !State->bCorrectionRequested || State->RegistrationFrame != GFrameCounter;
		}
		if (bNeedsRetirement) {
			// Inactive AnimGraph branches may never call PreUpdate again. Detach on the game thread;
			// the last submitted frame still needs valid baseline skinning until render-state recreation.
			AsyncTask(ENamedThreads::GameThread, [WeakInstance = TWeakObjectPtr<UKawaiiPhysicsMeshDeformerInstance>(this)]() {
				UKawaiiPhysicsMeshDeformerInstance* Instance = WeakInstance.Get();
				if (!Instance || !Instance->State || !IsValid(Instance->Mesh)) {
					return;
				}
				{
					FScopeLock Lock(&Instance->State->Mutex);
					if (Instance->State->bCorrectionRequested && Instance->State->RegistrationFrame == GFrameCounter) {
						return;
					}
				}
				USkinnedMeshComponent* Component = Instance->Mesh;
				if (Component->GetMeshDeformerInstanceForLOD(Component->GetPredictedLODLevel()) == Instance) {
					Instance->ReleaseResources();
					Component->UnsetMeshDeformer();
					UE_LOG(LogKawaiiGPUDeformer, Display, TEXT("%s: native animation/skinning restored (no current correction)."), *Component->GetPathName());
				}
			});
		}
	}
}

EMeshDeformerOutputBuffer UKawaiiPhysicsMeshDeformerInstance::GetOutputBuffers() const {
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition | EMeshDeformerOutputBuffer::SkinnedMeshTangents;
}
