#include "Tools/Common/MCPToolCommonJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MCPToolCommonJson
{
	int32 ParseCursor(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return 0;
		}

		FString CursorString;
		if (Params->TryGetStringField(TEXT("cursor"), CursorString))
		{
			return FCString::Atoi(*CursorString);
		}

		double CursorNumber = 0;
		if (Params->TryGetNumberField(TEXT("cursor"), CursorNumber))
		{
			return static_cast<int32>(CursorNumber);
		}

		return 0;
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> OutValues;
		OutValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			OutValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return OutValues;
	}

	void CollectChangedPropertiesFromPatchOperations(
		const TArray<TSharedPtr<FJsonValue>>* PatchOperations,
		TArray<FString>& OutChangedProperties)
	{
		OutChangedProperties.Reset();
		if (PatchOperations == nullptr)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& PatchValue : *PatchOperations)
		{
			if (!PatchValue.IsValid() || PatchValue->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> PatchObject = PatchValue->AsObject();
			FString PropertyPath;
			PatchObject->TryGetStringField(TEXT("path"), PropertyPath);
			TArray<FString> Tokens;
			PropertyPath.ParseIntoArray(Tokens, TEXT("/"), true);
			if (Tokens.Num() >= 1)
			{
				OutChangedProperties.AddUnique(Tokens[0]);
			}
		}
	}
}
