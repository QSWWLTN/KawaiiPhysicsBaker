#include "KawaiiPhysicsGPUCorrection.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalRenderPublic.h"
#include "RHI.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include "KawaiiPhysicsMeshDeformer.h"

DEFINE_LOG_CATEGORY_STATIC(LogKawaiiGPUCorrection, Log, All);

namespace {
	// GetMeshDeformerInstance() is not exported by UE 5.5; the per-LOD accessor is.
	UMeshDeformerInstance* GetAnyInstance(const USkeletalMeshComponent* Mesh) {
		if (const auto* Asset = Mesh->GetSkeletalMeshAsset()) {
			for (int32 LOD = 0; LOD < Asset->GetLODNum(); ++LOD) {
				if (auto* Instance = Mesh->GetMeshDeformerInstanceForLOD(LOD)) {
					return Instance->GetInstanceForSourceDeformer();
				}
			}
		}
		return nullptr;
	}

	UMeshDeformer* GetConfiguredDeformer(const USkeletalMeshComponent* Mesh) {
		// UE 5.5 exposes the component override only as a protected reflected property.
		static const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(USkinnedMeshComponent::StaticClass(), TEXT("MeshDeformer"));
		return Property ? Cast<UMeshDeformer>(Property->GetObjectPropertyValue_InContainer(Mesh)) : nullptr;
	}

	TAutoConsoleVariable<int32> CVarKawaiiGPU(
		TEXT("a.KawaiiPhysicsBaker.GPU"),
		1,
		TEXT(
			"Visual-only clothing/hair correction: 0 = original CPU solver, 1 = GPU solver + mesh deformer (default). " "Switching resets history. Unsupported meshes, downstream bone edits and other mesh deformers retain the CPU path."
		),
		ECVF_Default
	);

	FAutoConsoleCommandWithWorld GPUStatus(
		TEXT("a.KawaiiPhysicsBaker.GPU.Status"),
		TEXT("Log GPU correction readiness for this world."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World) {
			UE_LOG(LogKawaiiGPUCorrection, Display, TEXT("GPU mode = %d"), CVarKawaiiGPU.GetValueOnGameThread());
			int32 ActiveMeshCount = 0;
			for (TObjectIterator<USkeletalMeshComponent> It; It; ++It) {
				if (It->GetWorld() != World) {
					continue;
				}
				if (auto* Instance = Cast<UKawaiiPhysicsMeshDeformerInstance>(GetAnyInstance(*It))) {
					++ActiveMeshCount;
					FScopeLock Lock(&Instance->State->Mutex);
					const auto& State = *Instance->State;
					UE_LOG(
						LogKawaiiGPUCorrection,
						Display,
						TEXT("%s: ready=%d conflicting_nodes=%d packet=%d submitted=%llu chains=%d reason=%s"),
						*It->GetPathName(),
						State.bResourcesReady,
						State.bConflict,
						State.Frame.IsValid(),
						State.Sequence,
						State.Frame ? State.Frame->Chains.Num() : 0,
						*State.LastReason
					);
				}
			}
			UE_LOG(
				LogKawaiiGPUCorrection,
				Display,
				TEXT("Active GPU deformers = %d. Meshes without active correction use native animation/skinning."),
				ActiveMeshCount
			);
		})
	);
}

namespace KawaiiPhysicsGPU {
	void FState::Register(const void* Node, uint64 FrameNumber) {
		FScopeLock Lock(&Mutex);
		if (RegistrationFrame != FrameNumber) {
			RegistrationFrame = FrameNumber;
			Owner = Node;
			bConflict = false;
			bCorrectionRequested = false;
			Frame.Reset(); // No evaluation this frame must never render a stale correction.
		} else if (Owner != Node) {
			bConflict = true; // Multiple correction nodes require graph-order composition; use CPU for now.
			Frame.Reset();
			++ResetSerial;
		}
	}

	bool FState::CanSubmit(const void* Node) {
		FScopeLock Lock(&Mutex);
		return bResourcesReady && !bRequiresCPU && !bConflict && Owner == Node;
	}

	void FState::RequestCorrection(const void* Node) {
		FScopeLock Lock(&Mutex);
		if (Owner == Node && !bRequiresCPU) {
			bCorrectionRequested = true;
		}
	}

	void FState::Invalidate(const void* Node) {
		FScopeLock Lock(&Mutex);
		if (Owner == Node) {
			Frame.Reset();
			bCorrectionRequested = false;
			++ResetSerial;
		}
	}

	void FState::RequireCPU(const FString& Reason) {
		FScopeLock Lock(&Mutex);
		bRequiresCPU = true;
		bResourcesReady = false;
		bCorrectionRequested = false;
		LastReason = Reason;
		Frame.Reset();
		++ResetSerial;
	}

	bool FState::Submit(const void* Node, TSharedPtr<FFrame, ESPMode::ThreadSafe> Packet) {
		FScopeLock Lock(&Mutex);
		if (!bResourcesReady || bRequiresCPU || bConflict || Owner != Node) {
			return false;
		}
		Packet->Sequence = ++Sequence;
		Packet->GameFrame = RegistrationFrame;
		Packet->ResetSerial = ResetSerial;
		Frame = MoveTemp(Packet);
		return true;
	}

	TSharedPtr<const FFrame, ESPMode::ThreadSafe> FState::Read() {
		FScopeLock Lock(&Mutex);
		return Frame;
	}

	TSharedPtr<FState, ESPMode::ThreadSafe> Prepare(const UAnimInstance* Instance, const void* Node, bool bNeedsCorrection, const TSharedPtr<FState, ESPMode::ThreadSafe>& PreviousState) {
		check(IsInGameThread());
		USkeletalMeshComponent* Mesh = Instance ? Instance->GetSkelMeshComponent() : nullptr;
		if (!Mesh || !Mesh->IsRegistered()) {
			return nullptr;
		}
		auto* Existing = Cast<UKawaiiPhysicsMeshDeformerInstance>(GetAnyInstance(Mesh));
		UMeshDeformer* Configured = GetConfiguredDeformer(Mesh);
		const bool bGPUEnabled = CVarKawaiiGPU.GetValueOnGameThread() != 0;
		bool bRequiresCPU = false;
		if (bGPUEnabled && PreviousState) {
			FScopeLock Lock(&PreviousState->Mutex);
			bRequiresCPU = PreviousState->bRequiresCPU;
		}
		if (bRequiresCPU) {
			if (Existing && Existing->State == PreviousState) {
				Existing->ReleaseResources();
				Mesh->UnsetMeshDeformer();
			}
			// Keep the rejected state on the node, even after detachment. GPU 0 clears it,
			// so GPU 1 can retry after the graph has been fixed without reinstalling every frame.
			return PreviousState;
		}
		if (!bGPUEnabled || !bNeedsCorrection) {
			bool bOwnsInstance = false;
			if (Existing) {
				FScopeLock Lock(&Existing->State->Mutex);
				bOwnsInstance = Existing->State->Owner == Node;
			}
			if (bOwnsInstance || (!bGPUEnabled && (Existing || Cast<UKawaiiPhysicsMeshDeformer>(Configured)))) {
				if (Existing) {
					Existing->ReleaseResources();
				}
				Mesh->UnsetMeshDeformer();
				UE_LOG(
					LogKawaiiGPUCorrection,
					Display,
					TEXT("%s: native animation/skinning restored (%s)."),
					*Mesh->GetPathName(),
					bGPUEnabled ? TEXT("no active correction") : TEXT("a.KawaiiPhysicsBaker.GPU 0")
				);
			}
			return nullptr;
		}
		// Offline baking needs CPU bone transforms; visual-only correction belongs to Game/PIE worlds.
		if (!Mesh->GetWorld() || !Mesh->GetWorld()->IsGameWorld() || !GDynamicRHI || GUsingNullRHI || GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5
			|| !Mesh->MeshObject || Mesh->MeshObject->IsCPUSkinned() || Mesh->LeaderPoseComponent.IsValid())
		{
			return nullptr;
		}
		if (!Existing) {
			if (Configured || GetAnyInstance(Mesh) || !Mesh->GetSkeletalMeshAsset() || Mesh->GetSkeletalMeshAsset()->GetDefaultMeshDeformer()) {
				return nullptr;
			}
			// The prototype owns linear blend skinning + morphs, not Chaos cloth simulation.
			const FSkeletalMeshRenderData* RenderData = Mesh->GetSkeletalMeshRenderData();
			if (!RenderData) {
				return nullptr;
			}
			for (const auto& LOD : RenderData->LODRenderData) {
				for (const auto& Section : LOD.RenderSections) {
					if (Section.HasClothingData()) {
						return nullptr;
					}
				}
			}
			Mesh->SetMeshDeformer(NewObject<UKawaiiPhysicsMeshDeformer>(Mesh));
			Existing = Cast<UKawaiiPhysicsMeshDeformerInstance>(GetAnyInstance(Mesh));
			if (Existing) {
				UE_LOG(
					LogKawaiiGPUCorrection,
					Display,
					TEXT("%s: GPU visual correction requested; CPU pose remains the baseline. Use a.KawaiiPhysicsBaker.GPU 0 to revert."),
					*Mesh->GetPathName()
				);
			}
		}
		if (!Existing || Mesh->GetMeshDeformerInstanceForLOD(Mesh->GetPredictedLODLevel()) != Existing) {
			return nullptr;
		}
		Existing->State->Register(Node, GFrameCounter);
		return Existing->State;
	}

	bool MatchesComponentPose(const FFrame& Frame, TConstArrayView<FTransform> ComponentPose, int32& OutMismatchedBone) {
		OutMismatchedBone = INDEX_NONE;
		if (Frame.PointBones.IsEmpty() || Frame.Points.Num() != Frame.PointBones.Num() * PointStride) {
			return false;
		}
		for (int32 Point = 0; Point < Frame.PointBones.Num(); ++Point) {
			const int32 Bone = Frame.PointBones[Point];
			if (!ComponentPose.IsValidIndex(Bone)) {
				OutMismatchedBone = Bone;
				return false;
			}
			const FVector4f& Rotation = Frame.Points[Point * PointStride + 2];
			const FTransform Baseline(
				FQuat(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W),
				FVector(Frame.Points[Point * PointStride + 1]),
				FVector(Frame.Points[Point * PointStride + 3])
			);
			if (!ComponentPose[Bone].Equals(Baseline, 0.001f)) {
				OutMismatchedBone = Bone;
				return false;
			}
		}
		return true;
	}
}
