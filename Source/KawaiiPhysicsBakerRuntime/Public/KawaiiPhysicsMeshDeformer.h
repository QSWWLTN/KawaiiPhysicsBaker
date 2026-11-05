#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformer.h"
#include "Animation/MeshDeformerInstance.h"

#include "KawaiiPhysicsMeshDeformer.generated.h"

class USkinnedMeshComponent;

namespace KawaiiPhysicsGPU {
	struct FState;
	struct FRenderState;
}

/** Transient runtime deformer selected by a.KawaiiPhysicsBaker.GPU. */
UCLASS(Transient)
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiPhysicsMeshDeformer : public UMeshDeformer
{
	GENERATED_BODY()
public:
	virtual UMeshDeformerInstanceSettings* CreateSettingsInstance(UMeshComponent* Component) override;
	virtual UMeshDeformerInstance* CreateInstance(UMeshComponent* Component, UMeshDeformerInstanceSettings* Settings) override;
};

UCLASS(Transient)
class KAWAIIPHYSICSBAKERRUNTIME_API UKawaiiPhysicsMeshDeformerInstance : public UMeshDeformerInstance
{
	GENERATED_BODY()
public:
	virtual void AllocateResources() override;
	virtual void ReleaseResources() override;
	virtual void EnqueueWork(const FEnqueueWorkDesc& Desc) override;
	virtual EMeshDeformerOutputBuffer GetOutputBuffers() const override;

	virtual UMeshDeformerInstance* GetInstanceForSourceDeformer() override { return this; }

public:
	UPROPERTY(Transient)
		TObjectPtr<USkinnedMeshComponent> Mesh;

	TSharedPtr<KawaiiPhysicsGPU::FState, ESPMode::ThreadSafe> State;
	TSharedPtr<KawaiiPhysicsGPU::FRenderState, ESPMode::ThreadSafe> RenderState;
};
