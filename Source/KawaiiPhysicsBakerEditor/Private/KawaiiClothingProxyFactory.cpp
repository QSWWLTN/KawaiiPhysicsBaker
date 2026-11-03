#include "KawaiiClothingProxyFactory.h"

#include "AssetTypeCategories.h"
#include "Engine/SkeletalMesh.h"

#include "KawaiiClothingProxyAsset.h"

#define LOCTEXT_NAMESPACE "KawaiiClothingProxyFactory"

UKawaiiClothingProxyFactory::UKawaiiClothingProxyFactory() {
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UKawaiiClothingProxyAsset::StaticClass();
}

FText UKawaiiClothingProxyFactory::GetDisplayName() const {
	return LOCTEXT("Name", "Kawaii 衣物代理定义");
}

uint32 UKawaiiClothingProxyFactory::GetMenuCategories() const {
	return EAssetTypeCategories::Animation;
}

FString UKawaiiClothingProxyFactory::GetDefaultNewAssetName() const {
	return TEXT("DA_ClothingProxy");
}

UObject* UKawaiiClothingProxyFactory::FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*) {
	UKawaiiClothingProxyAsset* Asset = NewObject<UKawaiiClothingProxyAsset>(InParent, Class, Name, Flags | RF_Transactional);
	Asset->PreviewMesh = SelectedMesh;
	return Asset;
}

#undef LOCTEXT_NAMESPACE
