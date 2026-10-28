#include "KawaiiPhysicsBakeSharedCollisionSubsystem.h"

bool UKawaiiPhysicsBakeSharedCollisionSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const {
	// Do not create a second registry alongside the runtime registry in real worlds.
	return WorldType == EWorldType::EditorPreview;
}
