#pragma once

#include "CoreMinimal.h"

class UAnimSequence;
class UKawaiiClothingProxyMotion;

namespace KawaiiPhysicsBakeReplacement {
	bool ValidateSource(const UAnimSequence& Source, FString& OutError);
	bool Apply(UAnimSequence& Target, UAnimSequence& Baked, FString& OutError);
	bool ReplaceAndSave(UAnimSequence& Source,
						UAnimSequence& Baked,
						const FString& BackupPackageName,
						const TArray<UKawaiiClothingProxyMotion*>& Motions,
						UAnimSequence*& OutBackup,
						FString& OutError);
}
