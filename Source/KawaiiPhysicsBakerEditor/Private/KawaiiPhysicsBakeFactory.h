#pragma once

#include "Factories/Factory.h"
#include "KawaiiPhysicsBakeFactory.generated.h"

class USkeletalMesh;

UCLASS()
class UKawaiiPhysicsBakeFactory : public UFactory
{
	GENERATED_BODY()
public:
	UKawaiiPhysicsBakeFactory();
	virtual bool ConfigureProperties() override;
	virtual FText GetDisplayName() const override;
	virtual uint32 GetMenuCategories() const override;
	virtual FString GetDefaultNewAssetName() const override;
	virtual UObject* FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;

	UPROPERTY()
		TObjectPtr<USkeletalMesh> SelectedMesh;
};
