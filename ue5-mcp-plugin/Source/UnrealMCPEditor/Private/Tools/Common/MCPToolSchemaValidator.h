#pragma once

#include "CoreMinimal.h"

namespace MCPToolSchemaValidator
{
	bool ValidateJsonValueAgainstSchema(
		const TSharedPtr<FJsonValue>& JsonValue,
		const TSharedPtr<FJsonObject>& SchemaObject,
		const FString& Path,
		FString& OutError);
}
