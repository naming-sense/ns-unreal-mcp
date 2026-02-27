#include "MCPObjectUtils.h"

#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "MCPErrorCodes.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
	FString DecodePointerToken(FString Token)
	{
		Token.ReplaceInline(TEXT("~1"), TEXT("/"));
		Token.ReplaceInline(TEXT("~0"), TEXT("~"));
		return Token;
	}

	UWorld* GetEditorWorldForObjectUtils()
	{
		if (GEditor == nullptr)
		{
			return nullptr;
		}

		return GEditor->GetEditorWorldContext().World();
	}

	bool MatchesOptionalFilter(const FString& Value, const FString& GlobPattern)
	{
		return GlobPattern.IsEmpty() || Value.MatchesWildcard(GlobPattern);
	}

	TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr, int32 Depth);

	TSharedRef<FJsonObject> BuildPropertyDescriptor(
		FProperty* Property,
		const void* ValuePtr,
		int32 Depth)
	{
		TSharedRef<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
		PropertyObject->SetStringField(TEXT("path"), FString::Printf(TEXT("/%s"), *Property->GetName()));
		PropertyObject->SetStringField(TEXT("name"), Property->GetName());
		PropertyObject->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
		PropertyObject->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		PropertyObject->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
		PropertyObject->SetField(TEXT("value"), PropertyValueToJson(Property, ValuePtr, Depth));

		TSharedRef<FJsonObject> Constraints = MakeShared<FJsonObject>();
		if (Property->HasMetaData(TEXT("ClampMin")))
		{
			Constraints->SetStringField(TEXT("clamp_min"), Property->GetMetaData(TEXT("ClampMin")));
		}
		if (Property->HasMetaData(TEXT("ClampMax")))
		{
			Constraints->SetStringField(TEXT("clamp_max"), Property->GetMetaData(TEXT("ClampMax")));
		}
		if (Constraints->Values.Num() > 0)
		{
			PropertyObject->SetObjectField(TEXT("constraints"), Constraints);
		}

		return PropertyObject;
	}

	FString ExportPropertyToString(FProperty* Property, const void* ValuePtr)
	{
		FString ExportedText;
		Property->ExportTextItem_Direct(ExportedText, ValuePtr, nullptr, nullptr, PPF_None);
		return ExportedText;
	}

	TSharedPtr<FJsonValue> StructToJson(FStructProperty* StructProperty, const void* ValuePtr, const int32 Depth)
	{
		if (Depth <= 0)
		{
			return MakeShared<FJsonValueString>(ExportPropertyToString(StructProperty, ValuePtr));
		}

		TSharedRef<FJsonObject> StructObject = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			FProperty* FieldProperty = *It;
			const void* FieldValuePtr = FieldProperty->ContainerPtrToValuePtr<void>(ValuePtr);
			StructObject->SetField(FieldProperty->GetName(), PropertyValueToJson(FieldProperty, FieldValuePtr, Depth - 1));
		}
		return MakeShared<FJsonValueObject>(StructObject);
	}

	TSharedPtr<FJsonValue> ArrayToJson(FArrayProperty* ArrayProperty, const void* ValuePtr, const int32 Depth)
	{
		FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(ArrayHelper.Num());
		for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
		{
			const void* ElementValuePtr = ArrayHelper.GetRawPtr(Index);
			Values.Add(PropertyValueToJson(ArrayProperty->Inner, ElementValuePtr, Depth - 1));
		}
		return MakeShared<FJsonValueArray>(Values);
	}

	TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr, int32 Depth)
	{
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
		}

		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				return MakeShared<FJsonValueNumber>(static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
			}
			return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
		}

		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(ValuePtr));
		}

		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
		}

		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 EnumValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			if (const UEnum* EnumDefinition = EnumProperty->GetEnum())
			{
				return MakeShared<FJsonValueString>(EnumDefinition->GetNameStringByValue(EnumValue));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(EnumValue));
		}

		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			const uint8 ByteValue = ByteProperty->GetPropertyValue(ValuePtr);
			if (const UEnum* EnumDefinition = ByteProperty->Enum)
			{
				return MakeShared<FJsonValueString>(EnumDefinition->GetNameStringByValue(ByteValue));
			}
			return MakeShared<FJsonValueNumber>(ByteValue);
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (UObject* ReferencedObject = ObjectProperty->GetObjectPropertyValue(ValuePtr))
			{
				return MakeShared<FJsonValueString>(ReferencedObject->GetPathName());
			}
			return MakeShared<FJsonValueNull>();
		}

		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPath SoftPath = SoftObjectProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath();
			return MakeShared<FJsonValueString>(SoftPath.ToString());
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return StructToJson(StructProperty, ValuePtr, Depth);
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return ArrayToJson(ArrayProperty, ValuePtr, Depth);
		}

		return MakeShared<FJsonValueString>(ExportPropertyToString(Property, ValuePtr));
	}

	bool JsonValueToProperty(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FMCPDiagnostic& OutDiagnostic);

	bool JsonObjectToStruct(FStructProperty* StructProperty, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FMCPDiagnostic& OutDiagnostic)
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Object)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Struct assignment requires an object value.");
			OutDiagnostic.Detail = StructProperty->GetName();
			return false;
		}

		const TSharedPtr<FJsonObject> StructObject = JsonValue->AsObject();
		for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
		{
			FProperty* FieldProperty = *It;
			const TSharedPtr<FJsonValue>* FieldJsonValue = StructObject->Values.Find(FieldProperty->GetName());
			if (FieldJsonValue == nullptr || !FieldJsonValue->IsValid())
			{
				continue;
			}

			void* FieldValuePtr = FieldProperty->ContainerPtrToValuePtr<void>(ValuePtr);
			if (!JsonValueToProperty(FieldProperty, FieldValuePtr, *FieldJsonValue, OutDiagnostic))
			{
				return false;
			}
		}

		return true;
	}

	bool JsonArrayToProperty(FArrayProperty* ArrayProperty, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FMCPDiagnostic& OutDiagnostic)
	{
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Array)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Array assignment requires an array value.");
			OutDiagnostic.Detail = ArrayProperty->GetName();
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>& JsonValues = JsonValue->AsArray();
		FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
		ArrayHelper.Resize(JsonValues.Num());
		for (int32 Index = 0; Index < JsonValues.Num(); ++Index)
		{
			void* ElementValuePtr = ArrayHelper.GetRawPtr(Index);
			if (!JsonValueToProperty(ArrayProperty->Inner, ElementValuePtr, JsonValues[Index], OutDiagnostic))
			{
				return false;
			}
		}

		return true;
	}

	bool JsonValueToProperty(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FMCPDiagnostic& OutDiagnostic)
	{
		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool BoolValue = false;
			if (JsonValue->TryGetBool(BoolValue))
			{
				BoolProperty->SetPropertyValue(ValuePtr, BoolValue);
				return true;
			}

			double NumericBool = 0;
			if (JsonValue->TryGetNumber(NumericBool))
			{
				BoolProperty->SetPropertyValue(ValuePtr, !FMath::IsNearlyZero(NumericBool));
				return true;
			}
		}

		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			double NumericValue = 0;
			if (JsonValue->TryGetNumber(NumericValue))
			{
				if (NumericProperty->IsInteger())
				{
					NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(NumericValue));
				}
				else
				{
					NumericProperty->SetFloatingPointPropertyValue(ValuePtr, NumericValue);
				}
				return true;
			}

			FString NumericString;
			if (JsonValue->TryGetString(NumericString))
			{
				if (NumericProperty->IsInteger())
				{
					NumericProperty->SetIntPropertyValue(ValuePtr, FCString::Atoi64(*NumericString));
				}
				else
				{
					NumericProperty->SetFloatingPointPropertyValue(ValuePtr, FCString::Atod(*NumericString));
				}
				return true;
			}
		}

		if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
		{
			FString StringValue;
			if (JsonValue->TryGetString(StringValue))
			{
				StringProperty->SetPropertyValue(ValuePtr, StringValue);
				return true;
			}
		}

		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			FString NameValue;
			if (JsonValue->TryGetString(NameValue))
			{
				NameProperty->SetPropertyValue(ValuePtr, FName(*NameValue));
				return true;
			}
		}

		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			FString TextValue;
			if (JsonValue->TryGetString(TextValue))
			{
				TextProperty->SetPropertyValue(ValuePtr, FText::FromString(TextValue));
				return true;
			}
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const UEnum* EnumDefinition = EnumProperty->GetEnum();
			if (EnumDefinition != nullptr)
			{
				FString EnumString;
				if (JsonValue->TryGetString(EnumString))
				{
					const int64 EnumValue = EnumDefinition->GetValueByNameString(EnumString, EGetByNameFlags::CheckAuthoredName);
					if (EnumValue != INDEX_NONE)
					{
						EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
						return true;
					}
				}
			}

			double EnumNumber = 0;
			if (JsonValue->TryGetNumber(EnumNumber))
			{
				EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, static_cast<int64>(EnumNumber));
				return true;
			}
		}

		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (const UEnum* EnumDefinition = ByteProperty->Enum)
			{
				FString EnumString;
				if (JsonValue->TryGetString(EnumString))
				{
					const int64 EnumValue = EnumDefinition->GetValueByNameString(EnumString, EGetByNameFlags::CheckAuthoredName);
					if (EnumValue != INDEX_NONE)
					{
						ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
						return true;
					}
				}
			}

			double ByteNumber = 0;
			if (JsonValue->TryGetNumber(ByteNumber))
			{
				ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(ByteNumber));
				return true;
			}
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (!JsonValue.IsValid() || JsonValue->Type == EJson::Null)
			{
				ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}

			FString ObjectPath;
			if (JsonValue->TryGetString(ObjectPath))
			{
				UObject* ResolvedObject = FindObject<UObject>(nullptr, *ObjectPath);
				if (ResolvedObject == nullptr)
				{
					ResolvedObject = StaticLoadObject(ObjectProperty->PropertyClass, nullptr, *ObjectPath);
				}

				if (ResolvedObject == nullptr && !ObjectPath.IsEmpty())
				{
					OutDiagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
					OutDiagnostic.Message = TEXT("Failed to resolve object reference during patch.");
					OutDiagnostic.Detail = ObjectPath;
					return false;
				}

				ObjectProperty->SetObjectPropertyValue(ValuePtr, ResolvedObject);
				return true;
			}
		}

		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			if (!JsonValue.IsValid() || JsonValue->Type == EJson::Null)
			{
				SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr());
				return true;
			}

			FString SoftObjectPath;
			if (JsonValue->TryGetString(SoftObjectPath))
			{
				SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(SoftObjectPath)));
				return true;
			}
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return JsonObjectToStruct(StructProperty, ValuePtr, JsonValue, OutDiagnostic);
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return JsonArrayToProperty(ArrayProperty, ValuePtr, JsonValue, OutDiagnostic);
		}

		FString TextValue;
		if (JsonValue->TryGetString(TextValue))
		{
			return Property->ImportText_Direct(*TextValue, ValuePtr, nullptr, PPF_None) != nullptr;
		}

		OutDiagnostic.Code = MCPErrorCodes::SERIALIZE_UNSUPPORTED_TYPE;
		OutDiagnostic.Message = TEXT("Unsupported property type for patch.");
		OutDiagnostic.Detail = Property->GetCPPType();
		return false;
	}

	bool TryParsePathIndex(const FString& Token, int32& OutIndex)
	{
		if (Token.IsEmpty())
		{
			return false;
		}

		TCHAR* EndPtr = nullptr;
		const int64 ParsedValue = FCString::Strtoi64(*Token, &EndPtr, 10);
		if (EndPtr == nullptr || *EndPtr != TEXT('\0') || ParsedValue < 0 || ParsedValue > MAX_int32)
		{
			return false;
		}

		OutIndex = static_cast<int32>(ParsedValue);
		return true;
	}

	bool SetMapKeyFromToken(FProperty* KeyProperty, void* KeyPtr, const FString& Token, FMCPDiagnostic& OutDiagnostic)
	{
		if (FStrProperty* StrProperty = CastField<FStrProperty>(KeyProperty))
		{
			StrProperty->SetPropertyValue(KeyPtr, Token);
			return true;
		}

		if (FNameProperty* NameProperty = CastField<FNameProperty>(KeyProperty))
		{
			NameProperty->SetPropertyValue(KeyPtr, FName(*Token));
			return true;
		}

		if (FTextProperty* TextProperty = CastField<FTextProperty>(KeyProperty))
		{
			TextProperty->SetPropertyValue(KeyPtr, FText::FromString(Token));
			return true;
		}

		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(KeyProperty))
		{
			const double NumberValue = FCString::Atod(*Token);
			if (NumericProperty->IsInteger())
			{
				NumericProperty->SetIntPropertyValue(KeyPtr, static_cast<int64>(NumberValue));
			}
			else
			{
				NumericProperty->SetFloatingPointPropertyValue(KeyPtr, NumberValue);
			}
			return true;
		}

		OutDiagnostic.Code = MCPErrorCodes::SERIALIZE_UNSUPPORTED_TYPE;
		OutDiagnostic.Message = TEXT("Map key type is not supported for v2 patch path traversal.");
		OutDiagnostic.Detail = KeyProperty->GetCPPType();
		return false;
	}

	bool MapKeyMatchesToken(FProperty* KeyProperty, void* KeyPtr, const FString& Token)
	{
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(KeyProperty))
		{
			return StrProperty->GetPropertyValue(KeyPtr).Equals(Token, ESearchCase::CaseSensitive);
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(KeyProperty))
		{
			return NameProperty->GetPropertyValue(KeyPtr).ToString().Equals(Token, ESearchCase::IgnoreCase);
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(KeyProperty))
		{
			return TextProperty->GetPropertyValue(KeyPtr).ToString().Equals(Token, ESearchCase::CaseSensitive);
		}
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(KeyProperty))
		{
			const double NumberValue = FCString::Atod(*Token);
			if (NumericProperty->IsInteger())
			{
				return NumericProperty->GetSignedIntPropertyValue(KeyPtr) == static_cast<int64>(NumberValue);
			}
			return FMath::IsNearlyEqual(NumericProperty->GetFloatingPointPropertyValue(KeyPtr), NumberValue);
		}

		FString ExportedKey;
		KeyProperty->ExportTextItem_Direct(ExportedKey, KeyPtr, nullptr, nullptr, PPF_None);
		return ExportedKey.Equals(Token, ESearchCase::CaseSensitive)
			|| ExportedKey.Equals(Token, ESearchCase::IgnoreCase);
	}

	bool ApplyPatchV2Recursive(
		FProperty* CurrentProperty,
		void* CurrentValuePtr,
		const TArray<FString>& PathTokens,
		const int32 TokenIndex,
		const FString& Operation,
		const TSharedPtr<FJsonValue>& ValueJson,
		TArray<FString>& OutChangedProperties,
		const FString& RootPropertyName,
		FMCPDiagnostic& OutDiagnostic)
	{
		if (CurrentProperty == nullptr || CurrentValuePtr == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch path resolution failed.");
			return false;
		}

		const bool bAtLeaf = TokenIndex >= PathTokens.Num();
		const bool bIsRemove = Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase);
		const bool bIsReplace = Operation.Equals(TEXT("replace"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("merge"), ESearchCase::IgnoreCase);
		const bool bIsInc = Operation.Equals(TEXT("inc"), ESearchCase::IgnoreCase);
		const bool bIsTest = Operation.Equals(TEXT("test"), ESearchCase::IgnoreCase);

		if (bAtLeaf)
		{
			if (bIsRemove)
			{
				CurrentProperty->DestroyValue(CurrentValuePtr);
				CurrentProperty->InitializeValue(CurrentValuePtr);
				OutChangedProperties.AddUnique(RootPropertyName);
				return true;
			}

			if (bIsInc)
			{
				FNumericProperty* NumericProperty = CastField<FNumericProperty>(CurrentProperty);
				if (NumericProperty == nullptr)
				{
					OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					OutDiagnostic.Message = TEXT("inc operation requires numeric property.");
					OutDiagnostic.Detail = CurrentProperty->GetName();
					return false;
				}

				double Delta = 0.0;
				if (!ValueJson.IsValid() || !ValueJson->TryGetNumber(Delta))
				{
					OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					OutDiagnostic.Message = TEXT("inc operation requires numeric value.");
					OutDiagnostic.Detail = CurrentProperty->GetName();
					return false;
				}

				if (NumericProperty->IsInteger())
				{
					const int64 Updated = NumericProperty->GetSignedIntPropertyValue(CurrentValuePtr) + static_cast<int64>(Delta);
					NumericProperty->SetIntPropertyValue(CurrentValuePtr, Updated);
				}
				else
				{
					const double Updated = NumericProperty->GetFloatingPointPropertyValue(CurrentValuePtr) + Delta;
					NumericProperty->SetFloatingPointPropertyValue(CurrentValuePtr, Updated);
				}

				OutChangedProperties.AddUnique(RootPropertyName);
				return true;
			}

			if (bIsTest)
			{
				if (!ValueJson.IsValid())
				{
					OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					OutDiagnostic.Message = TEXT("test operation requires value.");
					OutDiagnostic.Detail = CurrentProperty->GetName();
					return false;
				}

				FString CurrentText;
				FString TestText;
				CurrentProperty->ExportTextItem_Direct(CurrentText, CurrentValuePtr, nullptr, nullptr, PPF_None);
				void* ScratchValue = FMemory::Malloc(CurrentProperty->ElementSize, CurrentProperty->GetMinAlignment());
				CurrentProperty->InitializeValue(ScratchValue);
				const bool bConverted = JsonValueToProperty(CurrentProperty, ScratchValue, ValueJson, OutDiagnostic);
				if (bConverted)
				{
					CurrentProperty->ExportTextItem_Direct(TestText, ScratchValue, nullptr, nullptr, PPF_None);
				}
				CurrentProperty->DestroyValue(ScratchValue);
				FMemory::Free(ScratchValue);
				if (!bConverted)
				{
					return false;
				}

				if (!CurrentText.Equals(TestText, ESearchCase::CaseSensitive))
				{
					OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					OutDiagnostic.Message = TEXT("test operation failed.");
					OutDiagnostic.Detail = RootPropertyName;
					return false;
				}
				return true;
			}

			if (!bIsReplace)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Unsupported patch operation.");
				OutDiagnostic.Detail = Operation;
				return false;
			}

			if (!ValueJson.IsValid())
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Patch operation requires value for add/replace/merge.");
				OutDiagnostic.Detail = CurrentProperty->GetName();
				return false;
			}

			if (!JsonValueToProperty(CurrentProperty, CurrentValuePtr, ValueJson, OutDiagnostic))
			{
				if (OutDiagnostic.Code.IsEmpty())
				{
					OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					OutDiagnostic.Message = TEXT("Failed to convert patch value for property.");
					OutDiagnostic.Detail = CurrentProperty->GetName();
				}
				return false;
			}

			OutChangedProperties.AddUnique(RootPropertyName);
			return true;
		}

		const FString& CurrentToken = PathTokens[TokenIndex];

		if (FStructProperty* StructProperty = CastField<FStructProperty>(CurrentProperty))
		{
			FProperty* ChildProperty = StructProperty->Struct != nullptr
				? StructProperty->Struct->FindPropertyByName(FName(*CurrentToken))
				: nullptr;
			if (ChildProperty == nullptr)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Patch path does not match a struct field.");
				OutDiagnostic.Detail = CurrentToken;
				return false;
			}

			void* ChildValuePtr = ChildProperty->ContainerPtrToValuePtr<void>(CurrentValuePtr);
			return ApplyPatchV2Recursive(
				ChildProperty,
				ChildValuePtr,
				PathTokens,
				TokenIndex + 1,
				Operation,
				ValueJson,
				OutChangedProperties,
				RootPropertyName,
				OutDiagnostic);
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(CurrentProperty))
		{
			FScriptArrayHelper ArrayHelper(ArrayProperty, CurrentValuePtr);
			if (CurrentToken.Equals(TEXT("-"), ESearchCase::CaseSensitive)
				&& (Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
				&& TokenIndex == PathTokens.Num() - 1)
			{
				const int32 NewIndex = ArrayHelper.AddValue();
				void* ElementPtr = ArrayHelper.GetRawPtr(NewIndex);
				if (!JsonValueToProperty(ArrayProperty->Inner, ElementPtr, ValueJson, OutDiagnostic))
				{
					return false;
				}
				OutChangedProperties.AddUnique(RootPropertyName);
				return true;
			}

			int32 ElementIndex = INDEX_NONE;
			if (!TryParsePathIndex(CurrentToken, ElementIndex))
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Array path token must be a non-negative integer index.");
				OutDiagnostic.Detail = CurrentToken;
				return false;
			}

			const bool bCanAppend = Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("replace"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("merge"), ESearchCase::IgnoreCase);
			if (ElementIndex >= ArrayHelper.Num())
			{
				if (!bCanAppend || ElementIndex != ArrayHelper.Num())
				{
					OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					OutDiagnostic.Message = TEXT("Array index is out of range.");
					OutDiagnostic.Detail = CurrentToken;
					return false;
				}
				ArrayHelper.AddValue();
			}

			if (TokenIndex == PathTokens.Num() - 1 && bIsRemove)
			{
				ArrayHelper.RemoveValues(ElementIndex, 1);
				OutChangedProperties.AddUnique(RootPropertyName);
				return true;
			}

			void* ElementPtr = ArrayHelper.GetRawPtr(ElementIndex);
			return ApplyPatchV2Recursive(
				ArrayProperty->Inner,
				ElementPtr,
				PathTokens,
				TokenIndex + 1,
				Operation,
				ValueJson,
				OutChangedProperties,
				RootPropertyName,
				OutDiagnostic);
		}

		if (FMapProperty* MapProperty = CastField<FMapProperty>(CurrentProperty))
		{
			FScriptMapHelper MapHelper(MapProperty, CurrentValuePtr);
			int32 PairIndex = INDEX_NONE;
			for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
			{
				if (!MapHelper.IsValidIndex(Index))
				{
					continue;
				}

				void* KeyPtr = MapHelper.GetKeyPtr(Index);
				if (MapKeyMatchesToken(MapProperty->KeyProp, KeyPtr, CurrentToken))
				{
					PairIndex = Index;
					break;
				}
			}

			const bool bAllowCreate = Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("replace"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("merge"), ESearchCase::IgnoreCase);
			if (PairIndex == INDEX_NONE && bAllowCreate)
			{
				PairIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
				void* KeyPtr = MapHelper.GetKeyPtr(PairIndex);
				if (!SetMapKeyFromToken(MapProperty->KeyProp, KeyPtr, CurrentToken, OutDiagnostic))
				{
					MapHelper.RemoveAt(PairIndex);
					return false;
				}
				MapHelper.Rehash();
			}

			if (PairIndex == INDEX_NONE)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Map key is not found.");
				OutDiagnostic.Detail = CurrentToken;
				return false;
			}

			if (TokenIndex == PathTokens.Num() - 1 && bIsRemove)
			{
				MapHelper.RemoveAt(PairIndex);
				OutChangedProperties.AddUnique(RootPropertyName);
				return true;
			}

			void* MapValuePtr = MapHelper.GetValuePtr(PairIndex);
			return ApplyPatchV2Recursive(
				MapProperty->ValueProp,
				MapValuePtr,
				PathTokens,
				TokenIndex + 1,
				Operation,
				ValueJson,
				OutChangedProperties,
				RootPropertyName,
				OutDiagnostic);
		}

		if (FSetProperty* SetProperty = CastField<FSetProperty>(CurrentProperty))
		{
			FScriptSetHelper SetHelper(SetProperty, CurrentValuePtr);
			if (TokenIndex < PathTokens.Num() - 1)
			{
				OutDiagnostic.Code = MCPErrorCodes::SERIALIZE_UNSUPPORTED_TYPE;
				OutDiagnostic.Message = TEXT("Nested set traversal is not supported.");
				OutDiagnostic.Detail = CurrentToken;
				return false;
			}

			if (bIsRemove)
			{
				for (int32 Index = 0; Index < SetHelper.GetMaxIndex(); ++Index)
				{
					if (!SetHelper.IsValidIndex(Index))
					{
						continue;
					}
					FString ExportedValue;
					SetProperty->ElementProp->ExportTextItem_Direct(ExportedValue, SetHelper.GetElementPtr(Index), nullptr, nullptr, PPF_None);
					if (ExportedValue.Equals(CurrentToken, ESearchCase::CaseSensitive) || ExportedValue.Equals(CurrentToken, ESearchCase::IgnoreCase))
					{
						SetHelper.RemoveAt(Index);
						OutChangedProperties.AddUnique(RootPropertyName);
						return true;
					}
				}

				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Set element is not found.");
				OutDiagnostic.Detail = CurrentToken;
				return false;
			}

			if (Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase) || Operation.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
			{
				const int32 NewIndex = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
				void* ElementPtr = SetHelper.GetElementPtr(NewIndex);
				if (!JsonValueToProperty(SetProperty->ElementProp, ElementPtr, ValueJson, OutDiagnostic))
				{
					SetHelper.RemoveAt(NewIndex);
					return false;
				}
				SetHelper.Rehash();
				OutChangedProperties.AddUnique(RootPropertyName);
				return true;
			}

			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Unsupported operation for set property.");
			OutDiagnostic.Detail = Operation;
			return false;
		}

		OutDiagnostic.Code = MCPErrorCodes::SERIALIZE_UNSUPPORTED_TYPE;
		OutDiagnostic.Message = TEXT("Patch path traversal is unsupported for this property type.");
		OutDiagnostic.Detail = CurrentProperty->GetCPPType();
		return false;
	}

	AActor* ResolveActorByPath(UWorld* World, const FString& ActorPath, const FString& ActorGuid)
	{
		if (World == nullptr)
		{
			return nullptr;
		}

		FGuid ParsedGuid;
		const bool bHasGuid = !ActorGuid.IsEmpty() && FGuid::Parse(ActorGuid, ParsedGuid);

		FString RequestedActorLabel = ActorPath;
		int32 DotIndex = INDEX_NONE;
		if (ActorPath.FindLastChar(TEXT('.'), DotIndex) && DotIndex + 1 < ActorPath.Len())
		{
			RequestedActorLabel = ActorPath.Mid(DotIndex + 1);
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr)
			{
				continue;
			}

			if (bHasGuid && Actor->GetActorGuid() == ParsedGuid)
			{
				return Actor;
			}

			if (MCPObjectUtils::BuildActorPath(Actor).Equals(ActorPath, ESearchCase::IgnoreCase))
			{
				return Actor;
			}

			if (Actor->GetActorLabel().Equals(ActorPath, ESearchCase::IgnoreCase) ||
				Actor->GetActorLabel().Equals(RequestedActorLabel, ESearchCase::IgnoreCase) ||
				Actor->GetPathName().Equals(ActorPath, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}

		return nullptr;
	}

	bool ResolveTargetPath(
		const FString& TargetType,
		const FString& TargetPath,
		const TSharedPtr<FJsonObject>& TargetIdObject,
		UObject*& OutObject)
	{
		OutObject = nullptr;

		if (TargetType.Equals(TEXT("actor"), ESearchCase::IgnoreCase))
		{
			FString ActorGuid;
			if (TargetIdObject.IsValid())
			{
				TargetIdObject->TryGetStringField(TEXT("actor_guid"), ActorGuid);
			}
			OutObject = ResolveActorByPath(GetEditorWorldForObjectUtils(), TargetPath, ActorGuid);
			return OutObject != nullptr;
		}

		if (TargetType.Equals(TEXT("component"), ESearchCase::IgnoreCase))
		{
			FString ActorPath = TargetPath;
			FString ComponentName;
			if (!TargetPath.Split(TEXT(":"), &ActorPath, &ComponentName))
			{
				return false;
			}

			FString ActorGuid;
			if (TargetIdObject.IsValid())
			{
				TargetIdObject->TryGetStringField(TEXT("actor_guid"), ActorGuid);
			}

			AActor* OwnerActor = ResolveActorByPath(GetEditorWorldForObjectUtils(), ActorPath, ActorGuid);
			if (OwnerActor == nullptr)
			{
				return false;
			}

			for (UActorComponent* Component : OwnerActor->GetComponents())
			{
				if (Component != nullptr && (Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase) || Component->GetFName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase)))
				{
					OutObject = Component;
					return true;
				}
			}

			return false;
		}

		OutObject = FindObject<UObject>(nullptr, *TargetPath);
		if (OutObject == nullptr)
		{
			OutObject = StaticLoadObject(UObject::StaticClass(), nullptr, *TargetPath);
		}

		return OutObject != nullptr;
	}
}

bool MCPObjectUtils::ResolveTargetObject(
	const TSharedPtr<FJsonObject>& TargetObject,
	UObject*& OutObject,
	FMCPDiagnostic& OutDiagnostic)
{
	if (!TargetObject.IsValid())
	{
		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("target object is required.");
		return false;
	}

	FString TargetType;
	FString TargetPath;
	TargetObject->TryGetStringField(TEXT("type"), TargetType);
	TargetObject->TryGetStringField(TEXT("path"), TargetPath);

	if (TargetPath.IsEmpty())
	{
		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("target.path is required.");
		return false;
	}

	const TSharedPtr<FJsonObject>* TargetId = nullptr;
	if (!TargetObject->TryGetObjectField(TEXT("id"), TargetId))
	{
		TargetId = nullptr;
	}

	const TSharedPtr<FJsonObject> TargetIdObject = (TargetId != nullptr) ? *TargetId : nullptr;
	if (!ResolveTargetPath(TargetType, TargetPath, TargetIdObject, OutObject))
	{
		OutDiagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		OutDiagnostic.Message = TEXT("Failed to resolve target object.");
		OutDiagnostic.Detail = FString::Printf(TEXT("type=%s path=%s"), *TargetType, *TargetPath);
		OutDiagnostic.Suggestion = TEXT("Verify object path and target type.");
		return false;
	}

	return true;
}

void MCPObjectUtils::InspectObject(
	UObject* TargetObject,
	const TSharedPtr<FJsonObject>& FiltersObject,
	const int32 Depth,
	TArray<TSharedPtr<FJsonValue>>& OutProperties)
{
	OutProperties.Reset();
	if (TargetObject == nullptr)
	{
		return;
	}

	bool bOnlyEditable = true;
	bool bIncludeTransient = false;
	FString CategoryGlob;
	FString PropertyNameGlob;
	if (FiltersObject.IsValid())
	{
		FiltersObject->TryGetBoolField(TEXT("only_editable"), bOnlyEditable);
		FiltersObject->TryGetBoolField(TEXT("include_transient"), bIncludeTransient);
		FiltersObject->TryGetStringField(TEXT("category_glob"), CategoryGlob);
		FiltersObject->TryGetStringField(TEXT("property_name_glob"), PropertyNameGlob);
	}

	for (TFieldIterator<FProperty> It(TargetObject->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (Property == nullptr)
		{
			continue;
		}

		const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit);
		if (bOnlyEditable && !bEditable)
		{
			continue;
		}

		if (!bIncludeTransient && Property->HasAnyPropertyFlags(CPF_Transient))
		{
			continue;
		}

		const FString PropertyName = Property->GetName();
		const FString CategoryName = Property->GetMetaData(TEXT("Category"));
		if (!MatchesOptionalFilter(PropertyName, PropertyNameGlob) || !MatchesOptionalFilter(CategoryName, CategoryGlob))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(TargetObject);
		OutProperties.Add(MakeShared<FJsonValueObject>(BuildPropertyDescriptor(Property, ValuePtr, Depth)));
	}
}

bool MCPObjectUtils::ApplyPatch(
	UObject* TargetObject,
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations,
	TArray<FString>& OutChangedProperties,
	FMCPDiagnostic& OutDiagnostic)
{
	OutChangedProperties.Reset();

	if (TargetObject == nullptr || PatchOperations == nullptr)
	{
		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("Patch target and patch operations are required.");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& PatchValue : *PatchOperations)
	{
		if (!PatchValue.IsValid() || PatchValue->Type != EJson::Object)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch entry must be a JSON object.");
			return false;
		}

		const TSharedPtr<FJsonObject> PatchObject = PatchValue->AsObject();
		FString Operation;
		FString Path;
		PatchObject->TryGetStringField(TEXT("op"), Operation);
		PatchObject->TryGetStringField(TEXT("path"), Path);

		if (Operation.IsEmpty() || Path.IsEmpty())
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch entry requires op and path.");
			return false;
		}

		TArray<FString> Tokens;
		Path.ParseIntoArray(Tokens, TEXT("/"), true);
		if (Tokens.Num() != 1)
		{
			OutDiagnostic.Code = MCPErrorCodes::SERIALIZE_UNSUPPORTED_TYPE;
			OutDiagnostic.Message = TEXT("Only top-level property patch path is currently supported.");
			OutDiagnostic.Detail = Path;
			OutDiagnostic.Suggestion = TEXT("Use path format like /PropertyName.");
			return false;
		}

		const FString PropertyName = DecodePointerToken(Tokens[0]);
		FProperty* Property = TargetObject->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (Property == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch path does not match a property.");
			OutDiagnostic.Detail = Path;
			return false;
		}

		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			OutDiagnostic.Code = MCPErrorCodes::PROPERTY_NOT_EDITABLE;
			OutDiagnostic.Message = TEXT("Property is not editable.");
			OutDiagnostic.Detail = PropertyName;
			return false;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(TargetObject);
		if (Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
		{
			UObject* ClassDefaultObject = TargetObject->GetClass()->GetDefaultObject();
			void* DefaultValuePtr = Property->ContainerPtrToValuePtr<void>(ClassDefaultObject);
			Property->CopyCompleteValue(ValuePtr, DefaultValuePtr);
			OutChangedProperties.AddUnique(PropertyName);
			continue;
		}

		if (!Operation.Equals(TEXT("replace"), ESearchCase::IgnoreCase) && !Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase))
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Unsupported patch operation.");
			OutDiagnostic.Detail = Operation;
			return false;
		}

		const TSharedPtr<FJsonValue>* ValueJson = PatchObject->Values.Find(TEXT("value"));
		if (ValueJson == nullptr || !ValueJson->IsValid())
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch operation requires value for add/replace.");
			OutDiagnostic.Detail = PropertyName;
			return false;
		}

		if (!JsonValueToProperty(Property, ValuePtr, *ValueJson, OutDiagnostic))
		{
			if (OutDiagnostic.Code.IsEmpty())
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Failed to convert patch value for property.");
				OutDiagnostic.Detail = PropertyName;
			}
			return false;
		}

		OutChangedProperties.AddUnique(PropertyName);
	}

	return true;
}

bool MCPObjectUtils::ApplyPatchV2(
	UObject* TargetObject,
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations,
	TArray<FString>& OutChangedProperties,
	FMCPDiagnostic& OutDiagnostic)
{
	OutChangedProperties.Reset();

	if (TargetObject == nullptr || PatchOperations == nullptr)
	{
		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("Patch target and patch operations are required.");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& PatchValue : *PatchOperations)
	{
		if (!PatchValue.IsValid() || PatchValue->Type != EJson::Object)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch entry must be a JSON object.");
			return false;
		}

		const TSharedPtr<FJsonObject> PatchObject = PatchValue->AsObject();
		FString Operation;
		FString Path;
		PatchObject->TryGetStringField(TEXT("op"), Operation);
		PatchObject->TryGetStringField(TEXT("path"), Path);

		if (Operation.IsEmpty() || Path.IsEmpty())
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch entry requires op and path.");
			return false;
		}

		TArray<FString> Tokens;
		Path.ParseIntoArray(Tokens, TEXT("/"), true);
		if (Tokens.Num() == 0)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch path is invalid.");
			OutDiagnostic.Detail = Path;
			return false;
		}

		for (FString& Token : Tokens)
		{
			Token = DecodePointerToken(Token);
		}

		const FString RootPropertyName = Tokens[0];
		FProperty* RootProperty = TargetObject->GetClass()->FindPropertyByName(FName(*RootPropertyName));
		if (RootProperty == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch path does not match a property.");
			OutDiagnostic.Detail = Path;
			return false;
		}

		if (!RootProperty->HasAnyPropertyFlags(CPF_Edit))
		{
			OutDiagnostic.Code = MCPErrorCodes::PROPERTY_NOT_EDITABLE;
			OutDiagnostic.Message = TEXT("Property is not editable.");
			OutDiagnostic.Detail = RootPropertyName;
			return false;
		}

		void* RootValuePtr = RootProperty->ContainerPtrToValuePtr<void>(TargetObject);
		const TSharedPtr<FJsonValue>* ValueJson = PatchObject->Values.Find(TEXT("value"));
		const bool bNeedsValue =
			Operation.Equals(TEXT("add"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("replace"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("merge"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("inc"), ESearchCase::IgnoreCase)
			|| Operation.Equals(TEXT("test"), ESearchCase::IgnoreCase);

		if (bNeedsValue && (ValueJson == nullptr || !ValueJson->IsValid()))
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Patch operation requires value.");
			OutDiagnostic.Detail = Path;
			return false;
		}

		if (!ApplyPatchV2Recursive(
			RootProperty,
			RootValuePtr,
			Tokens,
			1,
			Operation,
			bNeedsValue ? *ValueJson : nullptr,
			OutChangedProperties,
			RootPropertyName,
			OutDiagnostic))
		{
			return false;
		}
	}

	return true;
}

FString MCPObjectUtils::BuildActorPath(const AActor* Actor)
{
	if (Actor == nullptr)
	{
		return TEXT("");
	}

	const ULevel* Level = Actor->GetLevel();
	const FString LevelName = Level ? Level->GetName() : TEXT("PersistentLevel");
	return FString::Printf(TEXT("%s.%s"), *LevelName, *Actor->GetActorLabel());
}

void MCPObjectUtils::AppendTouchedPackage(UObject* TargetObject, TArray<FString>& InOutTouchedPackages)
{
	if (TargetObject == nullptr)
	{
		return;
	}

	if (UPackage* OuterMostPackage = TargetObject->GetOutermost())
	{
		InOutTouchedPackages.AddUnique(OuterMostPackage->GetName());
	}
}
