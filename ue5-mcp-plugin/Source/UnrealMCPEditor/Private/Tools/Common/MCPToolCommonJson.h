#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

namespace MCPToolCommonJson
{
	int32 ParseCursor(const TSharedPtr<FJsonObject>& Params);
	TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values);
	void CollectChangedPropertiesFromPatchOperations(
		const TArray<TSharedPtr<FJsonValue>>* PatchOperations,
		TArray<FString>& OutChangedProperties);
}
