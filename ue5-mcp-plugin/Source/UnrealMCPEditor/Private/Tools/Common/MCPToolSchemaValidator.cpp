#include "Tools/Common/MCPToolSchemaValidator.h"

namespace
{
	FString GetJsonTypeName(const EJson JsonType)
	{
		switch (JsonType)
		{
		case EJson::String:
			return TEXT("string");
		case EJson::Number:
			return TEXT("number");
		case EJson::Boolean:
			return TEXT("boolean");
		case EJson::Array:
			return TEXT("array");
		case EJson::Object:
			return TEXT("object");
		case EJson::Null:
			return TEXT("null");
		default:
			return TEXT("none");
		}
	}

	bool JsonValuesEquivalent(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
	{
		if (!Left.IsValid() || !Right.IsValid() || Left->Type != Right->Type)
		{
			return false;
		}

		switch (Left->Type)
		{
		case EJson::String:
		{
			FString LeftString;
			FString RightString;
			return Left->TryGetString(LeftString) && Right->TryGetString(RightString) && LeftString == RightString;
		}
		case EJson::Number:
		{
			double LeftNumber = 0.0;
			double RightNumber = 0.0;
			return Left->TryGetNumber(LeftNumber) && Right->TryGetNumber(RightNumber) && FMath::IsNearlyEqual(LeftNumber, RightNumber);
		}
		case EJson::Boolean:
		{
			bool bLeftBool = false;
			bool bRightBool = false;
			return Left->TryGetBool(bLeftBool) && Right->TryGetBool(bRightBool) && bLeftBool == bRightBool;
		}
		case EJson::Null:
			return true;
		default:
			return false;
		}
	}
}

bool MCPToolSchemaValidator::ValidateJsonValueAgainstSchema(
	const TSharedPtr<FJsonValue>& JsonValue,
	const TSharedPtr<FJsonObject>& SchemaObject,
	const FString& Path,
	FString& OutError)
{
	if (!SchemaObject.IsValid())
	{
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
	if (SchemaObject->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues != nullptr && EnumValues->Num() > 0)
	{
		bool bMatched = false;
		for (const TSharedPtr<FJsonValue>& EnumValue : *EnumValues)
		{
			if (JsonValuesEquivalent(JsonValue, EnumValue))
			{
				bMatched = true;
				break;
			}
		}

		if (!bMatched)
		{
			OutError = FString::Printf(TEXT("%s does not match enum constraints."), *Path);
			return false;
		}
	}

	FString ExpectedType;
	SchemaObject->TryGetStringField(TEXT("type"), ExpectedType);
	if (ExpectedType.IsEmpty())
	{
		return true;
	}

	if (ExpectedType.Equals(TEXT("object"), ESearchCase::IgnoreCase))
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("%s expected object but got %s."), *Path, *GetJsonTypeName(JsonValue.IsValid() ? JsonValue->Type : EJson::None));
			return false;
		}

		const TSharedPtr<FJsonObject> ValueObject = JsonValue->AsObject();
		const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
		const bool bHasProperties = SchemaObject->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject != nullptr && PropertiesObject->IsValid();
		bool bAllowAdditionalProperties = true;
		if (SchemaObject->HasField(TEXT("additionalProperties")))
		{
			SchemaObject->TryGetBoolField(TEXT("additionalProperties"), bAllowAdditionalProperties);
		}

		const TArray<TSharedPtr<FJsonValue>>* RequiredFields = nullptr;
		if (SchemaObject->TryGetArrayField(TEXT("required"), RequiredFields) && RequiredFields != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& RequiredFieldValue : *RequiredFields)
			{
				FString RequiredField;
				if (RequiredFieldValue.IsValid() && RequiredFieldValue->TryGetString(RequiredField) && !ValueObject->HasField(RequiredField))
				{
					OutError = FString::Printf(TEXT("%s/%s is required."), *Path, *RequiredField);
					return false;
				}
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : ValueObject->Values)
		{
			const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
			const bool bHasPropertySchema = bHasProperties && (*PropertiesObject)->TryGetObjectField(Entry.Key, PropertySchema) && PropertySchema != nullptr && PropertySchema->IsValid();
			if (bHasPropertySchema)
			{
				const FString ChildPath = FString::Printf(TEXT("%s/%s"), *Path, *Entry.Key);
				if (!MCPToolSchemaValidator::ValidateJsonValueAgainstSchema(Entry.Value, *PropertySchema, ChildPath, OutError))
				{
					return false;
				}
			}
			else if (!bAllowAdditionalProperties)
			{
				OutError = FString::Printf(TEXT("%s/%s is not allowed by schema."), *Path, *Entry.Key);
				return false;
			}
		}

		return true;
	}

	if (ExpectedType.Equals(TEXT("array"), ESearchCase::IgnoreCase))
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Array)
		{
			OutError = FString::Printf(TEXT("%s expected array but got %s."), *Path, *GetJsonTypeName(JsonValue.IsValid() ? JsonValue->Type : EJson::None));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>& ValueArray = JsonValue->AsArray();
		double MinItems = 0.0;
		if (SchemaObject->TryGetNumberField(TEXT("minItems"), MinItems) && ValueArray.Num() < static_cast<int32>(MinItems))
		{
			OutError = FString::Printf(TEXT("%s expected at least %d items."), *Path, static_cast<int32>(MinItems));
			return false;
		}

		double MaxItems = 0.0;
		if (SchemaObject->TryGetNumberField(TEXT("maxItems"), MaxItems) && ValueArray.Num() > static_cast<int32>(MaxItems))
		{
			OutError = FString::Printf(TEXT("%s expected at most %d items."), *Path, static_cast<int32>(MaxItems));
			return false;
		}

		const TSharedPtr<FJsonObject>* ItemSchemaObject = nullptr;
		if (SchemaObject->TryGetObjectField(TEXT("items"), ItemSchemaObject) && ItemSchemaObject != nullptr && ItemSchemaObject->IsValid())
		{
			for (int32 Index = 0; Index < ValueArray.Num(); ++Index)
			{
				const FString ChildPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
				if (!MCPToolSchemaValidator::ValidateJsonValueAgainstSchema(ValueArray[Index], *ItemSchemaObject, ChildPath, OutError))
				{
					return false;
				}
			}
		}

		return true;
	}

	if (ExpectedType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
	{
		FString ValueString;
		if (!JsonValue.IsValid() || !JsonValue->TryGetString(ValueString))
		{
			OutError = FString::Printf(TEXT("%s expected string but got %s."), *Path, *GetJsonTypeName(JsonValue.IsValid() ? JsonValue->Type : EJson::None));
			return false;
		}

		double MinLength = 0.0;
		if (SchemaObject->TryGetNumberField(TEXT("minLength"), MinLength) && ValueString.Len() < static_cast<int32>(MinLength))
		{
			OutError = FString::Printf(TEXT("%s expected minimum length %d."), *Path, static_cast<int32>(MinLength));
			return false;
		}

		double MaxLength = 0.0;
		if (SchemaObject->TryGetNumberField(TEXT("maxLength"), MaxLength) && ValueString.Len() > static_cast<int32>(MaxLength))
		{
			OutError = FString::Printf(TEXT("%s expected maximum length %d."), *Path, static_cast<int32>(MaxLength));
			return false;
		}

		return true;
	}

	if (ExpectedType.Equals(TEXT("number"), ESearchCase::IgnoreCase) || ExpectedType.Equals(TEXT("integer"), ESearchCase::IgnoreCase))
	{
		double ValueNumber = 0.0;
		if (!JsonValue.IsValid() || !JsonValue->TryGetNumber(ValueNumber))
		{
			OutError = FString::Printf(TEXT("%s expected %s but got %s."), *Path, *ExpectedType, *GetJsonTypeName(JsonValue.IsValid() ? JsonValue->Type : EJson::None));
			return false;
		}

		if (ExpectedType.Equals(TEXT("integer"), ESearchCase::IgnoreCase) &&
			FMath::Abs(ValueNumber - FMath::RoundToDouble(ValueNumber)) > KINDA_SMALL_NUMBER)
		{
			OutError = FString::Printf(TEXT("%s expected integer value."), *Path);
			return false;
		}

		double Minimum = 0.0;
		if (SchemaObject->TryGetNumberField(TEXT("minimum"), Minimum) && ValueNumber < Minimum)
		{
			OutError = FString::Printf(TEXT("%s expected value >= %g."), *Path, Minimum);
			return false;
		}

		double Maximum = 0.0;
		if (SchemaObject->TryGetNumberField(TEXT("maximum"), Maximum) && ValueNumber > Maximum)
		{
			OutError = FString::Printf(TEXT("%s expected value <= %g."), *Path, Maximum);
			return false;
		}

		return true;
	}

	if (ExpectedType.Equals(TEXT("boolean"), ESearchCase::IgnoreCase))
	{
		bool bValue = false;
		if (!JsonValue.IsValid() || !JsonValue->TryGetBool(bValue))
		{
			OutError = FString::Printf(TEXT("%s expected boolean but got %s."), *Path, *GetJsonTypeName(JsonValue.IsValid() ? JsonValue->Type : EJson::None));
			return false;
		}
		return true;
	}

	if (ExpectedType.Equals(TEXT("null"), ESearchCase::IgnoreCase))
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Null)
		{
			OutError = FString::Printf(TEXT("%s expected null but got %s."), *Path, *GetJsonTypeName(JsonValue.IsValid() ? JsonValue->Type : EJson::None));
			return false;
		}
	}

	return true;
}
