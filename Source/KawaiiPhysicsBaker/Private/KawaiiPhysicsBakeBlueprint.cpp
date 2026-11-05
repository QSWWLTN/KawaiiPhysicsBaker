#include "KawaiiPhysicsBakeBlueprint.h"

#include "KawaiiPhysicsBakeAnimInstance.h"

UKawaiiPhysicsBakeSettings::UKawaiiPhysicsBakeSettings() {
	OutputDirectory.Path = TEXT("/Game/KawaiiBakes");
}

UKawaiiPhysicsBakeSettings* KawaiiPhysicsBake::GetSettings(const UAnimBlueprint* Blueprint) {
	if (Blueprint) {
		for (const TObjectPtr<UBlueprintExtension>& Extension : Blueprint->GetExtensions()) {
			if (UKawaiiPhysicsBakeSettings* Settings = Cast<UKawaiiPhysicsBakeSettings>(Extension)) {
				return Settings;
			}
		}
	}
	return nullptr;
}

UKawaiiPhysicsBakeSettings* KawaiiPhysicsBake::FindOrAddSettings(UAnimBlueprint* Blueprint) {
	check(IsInGameThread());
	if (!Blueprint) {
		return nullptr;
	}
	if (UKawaiiPhysicsBakeSettings* Settings = GetSettings(Blueprint)) {
		return Settings;
	}
	Blueprint->Modify();
	UKawaiiPhysicsBakeSettings* Settings = NewObject<UKawaiiPhysicsBakeSettings>(Blueprint, NAME_None, RF_Transactional);
	Blueprint->AddExtension(Settings);
	return Settings;
}

bool KawaiiPhysicsBake::IsBakeBlueprint(const UAnimBlueprint* Blueprint) {
	return Blueprint && Blueprint->ParentClass && Blueprint->ParentClass->IsChildOf(UKawaiiPhysicsBakeAnimInstance::StaticClass())
		&& GetSettings(Blueprint) != nullptr;
}
