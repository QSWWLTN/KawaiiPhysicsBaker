#pragma once

#include "KawaiiPhysicsSharedCollisionSubsystem.h"
#include "KawaiiPhysicsBakeSharedCollisionSubsystem.generated.h"

/**
 * Enables the existing shared-collision registry in an isolated editor preview world.
 * The runtime subsystem continues to own Game, PIE and Editor worlds. Consumers can
 * use GetSubsystem<UKawaiiPhysicsSharedCollisionSubsystem>() unchanged: UE resolves
 * an available derived subsystem when the exact base type was not created.
 */
UCLASS()
class UKawaiiPhysicsBakeSharedCollisionSubsystem : public UKawaiiPhysicsSharedCollisionSubsystem
{
	GENERATED_BODY()

public:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
};
