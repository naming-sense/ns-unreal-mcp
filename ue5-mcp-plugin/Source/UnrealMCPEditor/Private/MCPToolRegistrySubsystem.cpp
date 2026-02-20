#include "MCPToolRegistrySubsystem.h"

#include "Algo/Unique.h"
#include "Editor.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "MCPChangeSetSubsystem.h"
#include "MCPErrorCodes.h"
#include "MCPEventStreamSubsystem.h"
#include "MCPJobSubsystem.h"
#include "MCPLog.h"
#include "MCPObjectUtils.h"
#include "MCPObservabilitySubsystem.h"
#include "MCPPolicySubsystem.h"
#include "MCPWebSocketTransportSubsystem.h"
#include "ScopedTransaction.h"
#include "Engine/Selection.h"
#include "Engine/Texture.h"
#include "EngineUtils.h"
#include "Factories/Factory.h"
#include "Factories/FbxImportUI.h"
#include "FileHelpers.h"
#include "GameMapsSettings.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/SpectatorPawn.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "WidgetBlueprint.h"
#include "Engine/Blueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/Widget.h"
#include "Animation/WidgetAnimation.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace
{
	FString HashToHexSha1(const FString& Input)
	{
		FTCHARToUTF8 Utf8Data(*Input);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8Data.Get(), Utf8Data.Length(), Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	}

	FString ExtractDomain(const FString& ToolName)
	{
		FString LeftPart;
		FString RightPart;
		if (ToolName.Split(TEXT("."), &LeftPart, &RightPart))
		{
			return LeftPart;
		}

		return ToolName;
	}

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

	bool ValidateJsonValueAgainstSchema(
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
					if (!ValidateJsonValueAgainstSchema(Entry.Value, *PropertySchema, ChildPath, OutError))
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
					if (!ValidateJsonValueAgainstSchema(ValueArray[Index], *ItemSchemaObject, ChildPath, OutError))
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

	bool MatchesAnyClassPath(const TSet<FString>& AllowedClassPaths, const FString& ClassPath)
	{
		return AllowedClassPaths.Num() == 0 || AllowedClassPaths.Contains(ClassPath);
	}

	FString NormalizePackageNameFromInput(const FString& InputPath)
	{
		FString PackageName = InputPath;
		if (FPackageName::IsValidObjectPath(InputPath) || InputPath.Contains(TEXT(".")))
		{
			const FString ObjectPathPackage = FPackageName::ObjectPathToPackageName(InputPath);
			if (!ObjectPathPackage.IsEmpty())
			{
				PackageName = ObjectPathPackage;
			}
		}
		return PackageName;
	}

	UPackage* ResolvePackageByName(const FString& InputPath)
	{
		const FString PackageName = NormalizePackageNameFromInput(InputPath);
		if (PackageName.IsEmpty())
		{
			return nullptr;
		}

		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (Package == nullptr)
		{
			Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		}

		return Package;
	}

	FString BuildPackageName(const FString& PackagePath, const FString& AssetName)
	{
		if (PackagePath.IsEmpty() || AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);
	}

	FString BuildObjectPath(const FString& PackagePath, const FString& AssetName)
	{
		if (PackagePath.IsEmpty() || AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
	}

	bool IsValidAssetDestination(const FString& PackagePath, const FString& AssetName)
	{
		if (!FPackageName::IsValidLongPackageName(PackagePath))
		{
			return false;
		}

		const FString ObjectPath = BuildObjectPath(PackagePath, AssetName);
		return !ObjectPath.IsEmpty() && FPackageName::IsValidObjectPath(ObjectPath);
	}

		UObject* ResolveObjectByPath(const FString& ObjectPath)
		{
			if (ObjectPath.IsEmpty())
			{
				return nullptr;
			}

			UObject* Object = FindObject<UObject>(nullptr, *ObjectPath);
			if (Object == nullptr)
			{
				Object = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			return Object;
		}

	UClass* ResolveClassByPath(const FString& ClassPath)
	{
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		UClass* ClassObject = FindObject<UClass>(nullptr, *ClassPath);
		if (ClassObject != nullptr)
		{
			return ClassObject;
		}

		const FString WrappedClassPath = FString::Printf(TEXT("Class'%s'"), *ClassPath);
		ClassObject = LoadObject<UClass>(nullptr, *WrappedClassPath);
		if (ClassObject != nullptr)
		{
			return ClassObject;
		}

		ClassObject = LoadObject<UClass>(nullptr, *ClassPath);
		if (ClassObject != nullptr)
		{
			return ClassObject;
		}

		const int32 DotIndex = ClassPath.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (DotIndex != INDEX_NONE && DotIndex + 1 < ClassPath.Len())
		{
			const FString ClassName = ClassPath.Mid(DotIndex + 1);
			ClassObject = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None, ELogVerbosity::NoLogging);
		}

		return ClassObject;
	}

	bool ParseAutoSaveOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
	{
		bOutAutoSave = false;
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* SaveObject = nullptr;
		if (Params->TryGetObjectField(TEXT("save"), SaveObject) && SaveObject != nullptr && SaveObject->IsValid())
		{
			(*SaveObject)->TryGetBoolField(TEXT("auto_save"), bOutAutoSave);
		}
		return true;
	}

	struct FMCPSettingsSaveOptions
	{
		bool bSaveConfig = true;
		bool bFlushIni = true;
		bool bReloadVerify = true;
	};

	bool ParseSettingsSaveOptions(const TSharedPtr<FJsonObject>& Params, FMCPSettingsSaveOptions& OutOptions)
	{
		OutOptions = FMCPSettingsSaveOptions();
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* SaveObject = nullptr;
		if (Params->TryGetObjectField(TEXT("save"), SaveObject) && SaveObject != nullptr && SaveObject->IsValid())
		{
			(*SaveObject)->TryGetBoolField(TEXT("save_config"), OutOptions.bSaveConfig);
			(*SaveObject)->TryGetBoolField(TEXT("flush_ini"), OutOptions.bFlushIni);
			(*SaveObject)->TryGetBoolField(TEXT("reload_verify"), OutOptions.bReloadVerify);
		}

		return true;
	}

	FString ParseTransactionLabel(const TSharedPtr<FJsonObject>& Params, const FString& DefaultLabel)
	{
		if (!Params.IsValid())
		{
			return DefaultLabel;
		}

		FString TransactionLabel = DefaultLabel;
		const TSharedPtr<FJsonObject>* TransactionObject = nullptr;
		if (Params->TryGetObjectField(TEXT("transaction"), TransactionObject) && TransactionObject != nullptr && TransactionObject->IsValid())
		{
			(*TransactionObject)->TryGetStringField(TEXT("label"), TransactionLabel);
		}
		return TransactionLabel;
	}

	bool ParseTopLevelPatchProperties(
		const TArray<TSharedPtr<FJsonValue>>* PatchOperations,
		UObject* TargetObject,
		TArray<FString>& OutChangedProperties,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutChangedProperties.Reset();
		if (PatchOperations == nullptr || TargetObject == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("patch and target are required.");
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

			const FString PropertyName = Tokens[0];
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

			OutChangedProperties.AddUnique(PropertyName);
		}

		return true;
	}

	FString BuildSettingsPatchSignature(
		const FString& ClassPath,
		const TArray<TSharedPtr<FJsonValue>>* PatchOperations,
		const FMCPSettingsSaveOptions& SaveOptions)
	{
		FString SignatureSource = ClassPath;
		SignatureSource += TEXT("|");
		SignatureSource += SaveOptions.bSaveConfig ? TEXT("1") : TEXT("0");
		SignatureSource += SaveOptions.bFlushIni ? TEXT("1") : TEXT("0");
		SignatureSource += SaveOptions.bReloadVerify ? TEXT("1") : TEXT("0");
		SignatureSource += TEXT("|");

		if (PatchOperations != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& PatchValue : *PatchOperations)
			{
				if (PatchValue.IsValid() && PatchValue->Type == EJson::Object)
				{
					SignatureSource += MCPJson::SerializeJsonObject(PatchValue->AsObject());
				}
				else
				{
					SignatureSource += TEXT("null");
				}
				SignatureSource += TEXT(";");
			}
		}

		return HashToHexSha1(SignatureSource);
	}

	bool ResolveSettingsClass(
		const FString& ClassPath,
		UClass*& OutClass,
		UObject*& OutConfigObject,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutClass = ResolveClassByPath(ClassPath);
		OutConfigObject = nullptr;
		if (OutClass == nullptr || !OutClass->IsChildOf(UObject::StaticClass()))
		{
			OutDiagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
			OutDiagnostic.Message = TEXT("settings class_path could not be resolved.");
			OutDiagnostic.Detail = ClassPath;
			return false;
		}

		if (!OutClass->HasAnyClassFlags(CLASS_Config))
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("settings class_path is not a config-backed class.");
			OutDiagnostic.Detail = ClassPath;
			OutDiagnostic.Suggestion = TEXT("Use a config class such as /Script/EngineSettings.GameMapsSettings.");
			return false;
		}

		OutConfigObject = OutClass->GetDefaultObject();
		if (OutConfigObject == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
			OutDiagnostic.Message = TEXT("Could not resolve class default object for settings class_path.");
			OutDiagnostic.Detail = ClassPath;
			return false;
		}

		return true;
	}

	bool PersistConfigObject(
		UObject* ConfigObject,
		const FMCPSettingsSaveOptions& SaveOptions,
		TArray<FString>& OutSavedConfigFiles,
		bool& bOutVerified,
		FMCPToolExecutionResult& OutResult)
	{
		bOutVerified = false;
		OutSavedConfigFiles.Reset();

		if (ConfigObject == nullptr || !SaveOptions.bSaveConfig)
		{
			return true;
		}

		ConfigObject->SaveConfig();
		const FString ConfigFile = ConfigObject->GetDefaultConfigFilename();
		if (!ConfigFile.IsEmpty())
		{
			OutSavedConfigFiles.AddUnique(ConfigFile);
		}

		if (SaveOptions.bFlushIni && GConfig != nullptr)
		{
			if (!ConfigFile.IsEmpty())
			{
				GConfig->Flush(false, ConfigFile);
			}
			else
			{
				GConfig->Flush(false);
			}
		}

		if (SaveOptions.bReloadVerify)
		{
			ConfigObject->ReloadConfig();
			bOutVerified = true;
		}

		if (!ConfigFile.IsEmpty() && !IFileManager::Get().FileExists(*ConfigFile))
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::SETTINGS_CONFIG_SAVE_FAILED;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Config file was not found after SaveConfig.");
			Diagnostic.Detail = ConfigFile;
			Diagnostic.Suggestion = TEXT("Check config write permissions and project configuration path.");
			OutResult.Diagnostics.Add(Diagnostic);
			return false;
		}

		return true;
	}

	bool TryInspectSettingsPropertyValue(
		UObject* ConfigObject,
		const FString& PropertyName,
		const int32 Depth,
		TSharedPtr<FJsonValue>& OutValue)
	{
		OutValue.Reset();
		if (ConfigObject == nullptr || PropertyName.IsEmpty())
		{
			return false;
		}

		TSharedRef<FJsonObject> FiltersObject = MakeShared<FJsonObject>();
		FiltersObject->SetBoolField(TEXT("only_editable"), false);
		FiltersObject->SetBoolField(TEXT("include_transient"), true);
		FiltersObject->SetStringField(TEXT("property_name_glob"), PropertyName);

		TArray<TSharedPtr<FJsonValue>> Properties;
		MCPObjectUtils::InspectObject(ConfigObject, FiltersObject, Depth, Properties);
		for (const TSharedPtr<FJsonValue>& PropertyValue : Properties)
		{
			if (!PropertyValue.IsValid() || PropertyValue->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> PropertyObject = PropertyValue->AsObject();
			FString CandidateName;
			PropertyObject->TryGetStringField(TEXT("name"), CandidateName);
			if (!CandidateName.Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (const TSharedPtr<FJsonValue>* ValueField = PropertyObject->Values.Find(TEXT("value")); ValueField != nullptr && (*ValueField).IsValid())
			{
				OutValue = *ValueField;
				return true;
			}
		}

		return false;
	}

	FString GetSettingsStringProperty(UObject* ConfigObject, const FString& PropertyName)
	{
		TSharedPtr<FJsonValue> PropertyValue;
		if (TryInspectSettingsPropertyValue(ConfigObject, PropertyName, 3, PropertyValue) && PropertyValue.IsValid())
		{
			FString StringValue;
			if (PropertyValue->TryGetString(StringValue))
			{
				return StringValue;
			}
		}
		return FString();
	}

	bool GetGameModeMapOverrideRawArray(UObject* ConfigObject, TArray<TSharedPtr<FJsonValue>>& OutRawArray)
	{
		OutRawArray.Reset();
		TSharedPtr<FJsonValue> PropertyValue;
		if (!TryInspectSettingsPropertyValue(ConfigObject, TEXT("GameModeMapPrefixes"), 5, PropertyValue) || !PropertyValue.IsValid())
		{
			return true;
		}

		if (PropertyValue->Type != EJson::Array)
		{
			return false;
		}

		OutRawArray = PropertyValue->AsArray();
		return true;
	}

	bool TryGetMapOverrideGameModeClassPath(const TSharedPtr<FJsonObject>& RawEntry, FString& OutClassPath)
	{
		OutClassPath.Reset();
		if (!RawEntry.IsValid())
		{
			return false;
		}

		if (RawEntry->TryGetStringField(TEXT("GameMode"), OutClassPath) && !OutClassPath.IsEmpty())
		{
			return true;
		}
		if (RawEntry->TryGetStringField(TEXT("game_mode"), OutClassPath) && !OutClassPath.IsEmpty())
		{
			return true;
		}

		auto ExtractFromSoftClassObject = [&OutClassPath](const TSharedPtr<FJsonObject>& SoftClassObject) -> bool
		{
			if (!SoftClassObject.IsValid())
			{
				return false;
			}

			if (SoftClassObject->TryGetStringField(TEXT("AssetPath"), OutClassPath) && !OutClassPath.IsEmpty())
			{
				return true;
			}

			const TSharedPtr<FJsonObject>* AssetPathObject = nullptr;
			if (SoftClassObject->TryGetObjectField(TEXT("AssetPath"), AssetPathObject) &&
				AssetPathObject != nullptr &&
				(*AssetPathObject).IsValid())
			{
				FString PackageName;
				FString AssetName;
				(*AssetPathObject)->TryGetStringField(TEXT("PackageName"), PackageName);
				(*AssetPathObject)->TryGetStringField(TEXT("AssetName"), AssetName);
				if (!PackageName.IsEmpty() && !AssetName.IsEmpty())
				{
					OutClassPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
					return true;
				}
			}

			return false;
		};

		const TSharedPtr<FJsonObject>* SoftClassObject = nullptr;
		if (RawEntry->TryGetObjectField(TEXT("GameMode"), SoftClassObject) &&
			SoftClassObject != nullptr &&
			ExtractFromSoftClassObject(*SoftClassObject))
		{
			return true;
		}

		if (RawEntry->TryGetObjectField(TEXT("game_mode"), SoftClassObject) &&
			SoftClassObject != nullptr &&
			ExtractFromSoftClassObject(*SoftClassObject))
		{
			return true;
		}

		return false;
	}

	TSharedRef<FJsonValueObject> BuildSoftClassPathJsonValue(const FString& ClassPath)
	{
		TSharedRef<FJsonObject> SoftClassObject = MakeShared<FJsonObject>();

		FString NormalizedClassPath = ClassPath;
		NormalizedClassPath.TrimStartAndEndInline();
		if (NormalizedClassPath.StartsWith(TEXT("Class'")) && NormalizedClassPath.EndsWith(TEXT("'")))
		{
			NormalizedClassPath = NormalizedClassPath.Mid(6, NormalizedClassPath.Len() - 7);
		}

		FString PackageName = TEXT("None");
		FString AssetName = TEXT("None");
		if (!NormalizedClassPath.IsEmpty())
		{
			FString LeftPart;
			FString RightPart;
			if (NormalizedClassPath.Split(TEXT("."), &LeftPart, &RightPart, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				if (!LeftPart.IsEmpty())
				{
					PackageName = LeftPart;
				}
				if (!RightPart.IsEmpty())
				{
					AssetName = RightPart;
				}
			}
			else
			{
				PackageName = NormalizedClassPath;
				AssetName = FPackageName::GetShortName(NormalizedClassPath);
			}
		}

		TSharedRef<FJsonObject> AssetPathObject = MakeShared<FJsonObject>();
		AssetPathObject->SetStringField(TEXT("PackageName"), PackageName);
		AssetPathObject->SetStringField(TEXT("AssetName"), AssetName);

		SoftClassObject->SetObjectField(TEXT("AssetPath"), AssetPathObject);
		SoftClassObject->SetStringField(TEXT("SubPathString"), TEXT(""));
		return MakeShared<FJsonValueObject>(SoftClassObject);
	}

	TArray<TSharedPtr<FJsonValue>> BuildMapOverrideEntriesFromRaw(const TArray<TSharedPtr<FJsonValue>>& RawArray)
	{
		TArray<TSharedPtr<FJsonValue>> Entries;
		for (const TSharedPtr<FJsonValue>& RawEntryValue : RawArray)
		{
			if (!RawEntryValue.IsValid() || RawEntryValue->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> RawEntry = RawEntryValue->AsObject();
			FString MapKey;
			FString GameModeClassPath;
			RawEntry->TryGetStringField(TEXT("Name"), MapKey);
			if (MapKey.IsEmpty())
			{
				RawEntry->TryGetStringField(TEXT("name"), MapKey);
			}
			TryGetMapOverrideGameModeClassPath(RawEntry, GameModeClassPath);

			if (MapKey.IsEmpty())
			{
				continue;
			}

			TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
			EntryObject->SetStringField(TEXT("map_key"), MapKey);
			EntryObject->SetStringField(TEXT("game_mode_class_path"), GameModeClassPath);
			Entries.Add(MakeShared<FJsonValueObject>(EntryObject));
		}
		return Entries;
	}

	bool ResolveGameModeClassPath(const FString& GameModeClassPath, FString& OutResolvedPath, FMCPDiagnostic& OutDiagnostic)
	{
		OutResolvedPath = GameModeClassPath;
		if (GameModeClassPath.IsEmpty())
		{
			return true;
		}

		UClass* ResolvedClass = ResolveClassByPath(GameModeClassPath);
		if (ResolvedClass == nullptr || !ResolvedClass->IsChildOf(AGameModeBase::StaticClass()))
		{
			OutDiagnostic.Code = MCPErrorCodes::GAMEMODE_CLASS_INVALID;
			OutDiagnostic.Message = TEXT("game_mode_class_path is invalid.");
			OutDiagnostic.Detail = GameModeClassPath;
			OutDiagnostic.Suggestion = TEXT("Provide a class path that resolves to AGameModeBase subclass.");
			return false;
		}

		OutResolvedPath = ResolvedClass->GetPathName();
		return true;
	}

	bool ParseStringArrayField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Params.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text) && !Text.IsEmpty())
			{
				OutValues.Add(Text);
			}
		}

		return true;
	}

	bool SavePackageByName(const FString& PackageName, FMCPToolExecutionResult& OutResult)
	{
		UPackage* PackageToSave = ResolvePackageByName(PackageName);
		if (PackageToSave == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Package was not found while saving asset lifecycle result.");
			Diagnostic.Detail = PackageName;
			OutResult.Diagnostics.Add(Diagnostic);
			return false;
		}

		const TArray<UPackage*> PackagesToSave{ PackageToSave };
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
		if (!bSaved)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::SAVE_FAILED;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Package save failed.");
			Diagnostic.Detail = PackageName;
			Diagnostic.bRetriable = true;
			OutResult.Diagnostics.Add(Diagnostic);
			return false;
		}

		return true;
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

	FString BuildGeneratedClassPathFromObjectPath(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}

		int32 DotIndex = INDEX_NONE;
		if (!ObjectPath.FindLastChar(TEXT('.'), DotIndex) || DotIndex + 1 >= ObjectPath.Len())
		{
			return FString::Printf(TEXT("%s_C"), *ObjectPath);
		}

		const FString Left = ObjectPath.Left(DotIndex + 1);
		const FString Right = ObjectPath.Mid(DotIndex + 1);
		return FString::Printf(TEXT("%s%s_C"), *Left, *Right);
	}

	struct FMCPBlueprintCreateOptions
	{
		FString PackagePath;
		FString AssetName;
		UClass* ParentClass = nullptr;
		bool bOverwrite = false;
		bool bDryRun = false;
		bool bCompileOnSuccess = true;
		bool bAutoSave = false;
		FString TransactionLabel = TEXT("MCP Blueprint Class Create");
	};

	struct FMCPBlueprintCreateResult
	{
		bool bCreatedNew = false;
		UBlueprint* BlueprintAsset = nullptr;
		FString ObjectPath;
		FString PackageName;
		FString ParentClassPath;
		FString GeneratedClassPath;
		bool bSaved = true;
	};

	bool EnsureBlueprintClassAsset(
		const FMCPBlueprintCreateOptions& Options,
		FMCPToolExecutionResult& OutResult,
		FMCPBlueprintCreateResult& OutCreateResult,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutCreateResult = FMCPBlueprintCreateResult();

		if (Options.ParentClass == nullptr || !Options.ParentClass->IsChildOf(UObject::StaticClass()))
		{
			OutDiagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			OutDiagnostic.Message = TEXT("parent_class_path could not be resolved.");
			return false;
		}

		if (!IsValidAssetDestination(Options.PackagePath, Options.AssetName))
		{
			OutDiagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
			OutDiagnostic.Message = TEXT("Invalid package_path or asset_name for blueprint class creation.");
			OutDiagnostic.Detail = FString::Printf(TEXT("package_path=%s asset_name=%s"), *Options.PackagePath, *Options.AssetName);
			return false;
		}

		const FString PackageName = BuildPackageName(Options.PackagePath, Options.AssetName);
		const FString ObjectPath = BuildObjectPath(Options.PackagePath, Options.AssetName);
		UObject* ExistingObject = ResolveObjectByPath(ObjectPath);

		if (ExistingObject != nullptr && !ExistingObject->IsA<UBlueprint>())
		{
			OutDiagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			OutDiagnostic.Message = TEXT("Existing asset is not a Blueprint and cannot be reused.");
			OutDiagnostic.Detail = ObjectPath;
			return false;
		}

		UBlueprint* BlueprintAsset = Cast<UBlueprint>(ExistingObject);
		bool bCreatedNew = false;
		if (BlueprintAsset != nullptr && !Options.bOverwrite)
		{
			const UClass* ExistingParentClass = BlueprintAsset->ParentClass;
			if (ExistingParentClass != nullptr && ExistingParentClass != Options.ParentClass)
			{
				OutDiagnostic.Code = MCPErrorCodes::ASSET_ALREADY_EXISTS;
				OutDiagnostic.Message = TEXT("Blueprint already exists with a different parent class.");
				OutDiagnostic.Detail = FString::Printf(
					TEXT("object_path=%s existing_parent=%s requested_parent=%s"),
					*ObjectPath,
					*ExistingParentClass->GetPathName(),
					*Options.ParentClass->GetPathName());
				OutDiagnostic.Suggestion = TEXT("Set overwrite=true or pick another destination.");
				return false;
			}
		}

		if (Options.bDryRun && (BlueprintAsset == nullptr || Options.bOverwrite))
		{
			bCreatedNew = true;
		}

		if (!Options.bDryRun)
		{
			if (BlueprintAsset != nullptr && Options.bOverwrite)
			{
				if (!ObjectTools::DeleteSingleObject(BlueprintAsset, false))
				{
					OutDiagnostic.Code = MCPErrorCodes::ASSET_DELETE_FAILED;
					OutDiagnostic.Message = TEXT("Failed to delete existing Blueprint for overwrite.");
					OutDiagnostic.Detail = ObjectPath;
					return false;
				}
				BlueprintAsset = nullptr;
			}

			if (BlueprintAsset == nullptr)
			{
				UPackage* Package = CreatePackage(*PackageName);
				if (Package == nullptr)
				{
					OutDiagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
					OutDiagnostic.Message = TEXT("Failed to create package for Blueprint.");
					OutDiagnostic.Detail = PackageName;
					return false;
				}

				FScopedTransaction Transaction(FText::FromString(Options.TransactionLabel));
				BlueprintAsset = FKismetEditorUtilities::CreateBlueprint(
					Options.ParentClass,
					Package,
					FName(*Options.AssetName),
					EBlueprintType::BPTYPE_Normal,
					UBlueprint::StaticClass(),
					UBlueprintGeneratedClass::StaticClass(),
					FName(TEXT("UnrealMCP")));

				if (BlueprintAsset == nullptr)
				{
					Transaction.Cancel();
					OutDiagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
					OutDiagnostic.Message = TEXT("Failed to create Blueprint class asset.");
					OutDiagnostic.Detail = ObjectPath;
					return false;
				}

				bCreatedNew = true;
			}

			if (BlueprintAsset != nullptr && Options.bCompileOnSuccess)
			{
				FKismetEditorUtilities::CompileBlueprint(BlueprintAsset);
			}
		}

		OutResult.TouchedPackages.AddUnique(PackageName);
		bool bSaved = true;
		if (!Options.bDryRun && Options.bAutoSave)
		{
			bSaved = SavePackageByName(PackageName, OutResult);
		}

		FString GeneratedClassPath = BuildGeneratedClassPathFromObjectPath(ObjectPath);
		if (BlueprintAsset != nullptr && BlueprintAsset->GeneratedClass != nullptr)
		{
			GeneratedClassPath = BlueprintAsset->GeneratedClass->GetPathName();
		}

		OutCreateResult.bCreatedNew = bCreatedNew;
		OutCreateResult.BlueprintAsset = BlueprintAsset;
		OutCreateResult.ObjectPath = ObjectPath;
		OutCreateResult.PackageName = PackageName;
		OutCreateResult.ParentClassPath = Options.ParentClass->GetPathName();
		OutCreateResult.GeneratedClassPath = GeneratedClassPath;
		OutCreateResult.bSaved = bSaved;
		return true;
	}

	bool ResolveClassPathWithBase(
		const FString& RequestedClassPath,
		UClass* RequiredBaseClass,
		FString& OutResolvedClassPath,
		UClass*& OutResolvedClass,
		FMCPDiagnostic& OutDiagnostic)
	{
		OutResolvedClassPath.Reset();
		OutResolvedClass = nullptr;
		if (RequestedClassPath.IsEmpty())
		{
			return true;
		}

		UClass* ResolvedClass = ResolveClassByPath(RequestedClassPath);
		if (ResolvedClass == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::ASSET_NOT_FOUND;
			OutDiagnostic.Message = TEXT("Requested class path could not be resolved.");
			OutDiagnostic.Detail = RequestedClassPath;
			return false;
		}

		if (RequiredBaseClass != nullptr && !ResolvedClass->IsChildOf(RequiredBaseClass))
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Resolved class is not compatible with required base class.");
			OutDiagnostic.Detail = FString::Printf(TEXT("class=%s required_base=%s"), *ResolvedClass->GetPathName(), *RequiredBaseClass->GetPathName());
			return false;
		}

		OutResolvedClass = ResolvedClass;
		OutResolvedClassPath = ResolvedClass->GetPathName();
		return true;
	}

	bool SetClassPropertyOnObject(
		UObject* TargetObject,
		const FString& PropertyName,
		UClass* ValueClass,
		FMCPDiagnostic& OutDiagnostic)
	{
		if (TargetObject == nullptr || ValueClass == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Target object and class value are required.");
			return false;
		}

		FClassProperty* ClassProperty = FindFProperty<FClassProperty>(TargetObject->GetClass(), FName(*PropertyName));
		if (ClassProperty == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Class property was not found on target object.");
			OutDiagnostic.Detail = PropertyName;
			return false;
		}

		if (!ClassProperty->HasAnyPropertyFlags(CPF_Edit))
		{
			OutDiagnostic.Code = MCPErrorCodes::PROPERTY_NOT_EDITABLE;
			OutDiagnostic.Message = TEXT("Class property is not editable.");
			OutDiagnostic.Detail = PropertyName;
			return false;
		}

		if (ClassProperty->MetaClass != nullptr && !ValueClass->IsChildOf(ClassProperty->MetaClass))
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Class value is not compatible with target property MetaClass.");
			OutDiagnostic.Detail = FString::Printf(TEXT("property=%s class=%s meta=%s"), *PropertyName, *ValueClass->GetPathName(), *ClassProperty->MetaClass->GetPathName());
			return false;
		}

		ClassProperty->SetPropertyValue_InContainer(TargetObject, ValueClass);
		return true;
	}

	UWorld* GetEditorWorld()
	{
		if (GEditor == nullptr)
		{
			return nullptr;
		}
		return GEditor->GetEditorWorldContext().World();
	}

	AActor* FindActorByReference(UWorld* World, const FString& ActorReference)
	{
		if (World == nullptr || ActorReference.IsEmpty())
		{
			return nullptr;
		}

		FString RequestedLabel = ActorReference;
		int32 DotIndex = INDEX_NONE;
		if (ActorReference.FindLastChar(TEXT('.'), DotIndex) && DotIndex + 1 < ActorReference.Len())
		{
			RequestedLabel = ActorReference.Mid(DotIndex + 1);
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr)
			{
				continue;
			}

			if (MCPObjectUtils::BuildActorPath(Actor).Equals(ActorReference, ESearchCase::IgnoreCase) ||
				Actor->GetPathName().Equals(ActorReference, ESearchCase::IgnoreCase) ||
				Actor->GetActorLabel().Equals(ActorReference, ESearchCase::IgnoreCase) ||
				Actor->GetActorLabel().Equals(RequestedLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}

		return nullptr;
	}

	bool IsKindAllowed(const TSet<FString>& AllowedKinds, const FString& Kind)
	{
		return AllowedKinds.Num() == 0 || AllowedKinds.Contains(Kind);
	}

	bool JsonToLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Elements = Value->AsArray();
			if (Elements.Num() >= 3)
			{
				double R = 0;
				double G = 0;
				double B = 0;
				double A = 1.0;
				if (Elements[0]->TryGetNumber(R) && Elements[1]->TryGetNumber(G) && Elements[2]->TryGetNumber(B))
				{
					if (Elements.Num() >= 4)
					{
						Elements[3]->TryGetNumber(A);
					}
					OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
					return true;
				}
			}
		}

		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> ColorObject = Value->AsObject();
			double R = 0;
			double G = 0;
			double B = 0;
			double A = 1.0;
			if (ColorObject->TryGetNumberField(TEXT("r"), R) &&
				ColorObject->TryGetNumberField(TEXT("g"), G) &&
				ColorObject->TryGetNumberField(TEXT("b"), B))
			{
				ColorObject->TryGetNumberField(TEXT("a"), A);
				OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));
				return true;
			}
		}

		return false;
	}

	UWidgetBlueprint* LoadWidgetBlueprintByPath(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}

		return LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	}

	UWidget* ResolveWidgetFromRef(UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRefObject)
	{
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || WidgetRefObject == nullptr || !WidgetRefObject->IsValid())
		{
			return nullptr;
		}

		FString WidgetId;
		FString WidgetName;
		(*WidgetRefObject)->TryGetStringField(TEXT("widget_id"), WidgetId);
		(*WidgetRefObject)->TryGetStringField(TEXT("name"), WidgetName);

		TArray<FString> CandidateNames;
		if (!WidgetName.IsEmpty())
		{
			CandidateNames.AddUnique(WidgetName);
		}

		if (!WidgetId.IsEmpty())
		{
			if (WidgetId.StartsWith(TEXT("name:")))
			{
				CandidateNames.AddUnique(WidgetId.RightChop(5));
			}
			else
			{
				FGuid ParsedGuid;
				if (!FGuid::Parse(WidgetId, ParsedGuid))
				{
					CandidateNames.AddUnique(WidgetId);
				}
			}
		}

		if (CandidateNames.Num() == 0)
		{
			return nullptr;
		}

		TArray<UWidget*> AllWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
		for (UWidget* Widget : AllWidgets)
		{
			if (Widget == nullptr)
			{
				continue;
			}

			for (const FString& CandidateName : CandidateNames)
			{
				if (Widget->GetName().Equals(CandidateName, ESearchCase::IgnoreCase))
				{
					return Widget;
				}
			}
		}

		return nullptr;
	}

	FString GetWidgetStableId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		if (WidgetBlueprint == nullptr || Widget == nullptr)
		{
			return TEXT("");
		}

		return FString::Printf(TEXT("name:%s"), *Widget->GetName());
	}

	void EnsureWidgetGuidMap(UWidgetBlueprint* WidgetBlueprint)
	{
		(void)WidgetBlueprint;
	}

	void EnsureWidgetGuidEntry(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget)
	{
		(void)WidgetBlueprint;
		(void)Widget;
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
			if (Tokens.Num() == 1)
			{
				OutChangedProperties.AddUnique(Tokens[0]);
			}
		}
	}

	UClass* ResolveWidgetClassByPath(const FString& WidgetClassPath)
	{
		UClass* WidgetClass = ResolveClassByPath(WidgetClassPath);
		if (WidgetClass == nullptr)
		{
			return nullptr;
		}

		if (!WidgetClass->IsChildOf(UWidget::StaticClass()))
		{
			return nullptr;
		}

		if (WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NewerVersionExists))
		{
			return nullptr;
		}

		return WidgetClass;
	}

	FString GetPanelSlotTypeName(UPanelWidget* PanelWidget)
	{
		if (PanelWidget == nullptr)
		{
			return FString();
		}

		const int32 ChildCount = PanelWidget->GetChildrenCount();
		for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
		{
			if (UWidget* ChildWidget = PanelWidget->GetChildAt(ChildIndex))
			{
				if (ChildWidget->Slot != nullptr)
				{
					return ChildWidget->Slot->GetClass()->GetName();
				}
			}
		}

		return UPanelSlot::StaticClass()->GetName();
	}

	FName BuildUniqueWidgetName(UWidgetBlueprint* WidgetBlueprint, UClass* WidgetClass, const FString& RequestedName)
	{
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
		{
			return NAME_None;
		}

		FString BaseName = RequestedName;
		if (BaseName.IsEmpty() && WidgetClass != nullptr)
		{
			BaseName = WidgetClass->GetName();
			if (BaseName.StartsWith(TEXT("U")))
			{
				BaseName.RightChopInline(1);
			}
		}

		BaseName.TrimStartAndEndInline();
		if (BaseName.IsEmpty())
		{
			BaseName = TEXT("Widget");
		}

		FString CandidateName = BaseName;
		int32 Suffix = 1;
		while (WidgetBlueprint->WidgetTree->FindWidget(FName(*CandidateName)) != nullptr)
		{
			CandidateName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
		}
		return FName(*CandidateName);
	}

	FString GetParentWidgetId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		if (WidgetBlueprint == nullptr || Widget == nullptr || Widget->Slot == nullptr || Widget->Slot->Parent == nullptr)
		{
			return TEXT("");
		}
		return GetWidgetStableId(WidgetBlueprint, Widget->Slot->Parent);
	}

	bool AttachWidgetToParent(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* ParentWidget,
		UWidget* ChildWidget,
		const int32 InsertIndex,
		const bool bReplaceContent,
		FMCPDiagnostic& OutDiagnostic)
	{
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || ChildWidget == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Widget blueprint context is invalid.");
			return false;
		}

		if (ParentWidget == nullptr)
		{
			if (WidgetBlueprint->WidgetTree->RootWidget != nullptr)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("WidgetTree root already exists. Provide parent_ref to add child widgets.");
				return false;
			}

			WidgetBlueprint->WidgetTree->RootWidget = ChildWidget;
			return true;
		}

		if (UContentWidget* ContentWidget = Cast<UContentWidget>(ParentWidget))
		{
			if (ContentWidget->GetContent() != nullptr && !bReplaceContent)
			{
				OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				OutDiagnostic.Message = TEXT("Target content widget already has a child. Set replace_content=true to replace it.");
				return false;
			}

			if (ContentWidget->GetContent() != nullptr && bReplaceContent)
			{
				if (!WidgetBlueprint->WidgetTree->RemoveWidget(ContentWidget->GetContent()))
				{
					OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
					OutDiagnostic.Message = TEXT("Failed to remove existing child from content widget.");
					return false;
				}
			}

			UPanelSlot* NewSlot = ContentWidget->SetContent(ChildWidget);
			if (NewSlot == nullptr)
			{
				OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
				OutDiagnostic.Message = TEXT("Failed to attach child to content widget.");
				return false;
			}
			return true;
		}

		UPanelWidget* PanelWidget = Cast<UPanelWidget>(ParentWidget);
		if (PanelWidget == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("parent_ref must point to a panel/content widget.");
			return false;
		}

		UPanelSlot* NewSlot = nullptr;
		if (InsertIndex >= 0)
		{
			NewSlot = PanelWidget->InsertChildAt(InsertIndex, ChildWidget);
		}
		else
		{
			NewSlot = PanelWidget->AddChild(ChildWidget);
		}

		if (NewSlot == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			OutDiagnostic.Message = TEXT("Failed to attach child to panel widget.");
			return false;
		}

		return true;
	}

	bool DetachWidgetFromParent(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		FMCPDiagnostic& OutDiagnostic)
	{
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || Widget == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			OutDiagnostic.Message = TEXT("Widget blueprint context is invalid.");
			return false;
		}

		if (WidgetBlueprint->WidgetTree->RootWidget == Widget)
		{
			WidgetBlueprint->WidgetTree->RootWidget = nullptr;
			return true;
		}

		if (Widget->Slot == nullptr || Widget->Slot->Parent == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
			OutDiagnostic.Message = TEXT("Widget has no parent slot and cannot be detached.");
			return false;
		}

		UPanelWidget* ParentPanel = Widget->Slot->Parent;
		if (!ParentPanel->RemoveChild(Widget))
		{
			OutDiagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			OutDiagnostic.Message = TEXT("Failed to detach widget from its parent.");
			return false;
		}

		return true;
	}

	FString BuildNiagaraCompatModuleKey(const FString& ObjectPath)
	{
		return FString::Printf(TEXT("compat|%s|default"), *HashToHexSha1(ObjectPath.ToLower()));
	}

	void BuildWidgetTreeNodesRecursive(
		const UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		const FString& ParentId,
		const FString& NameGlob,
		const TSet<FString>& AllowedClassPaths,
		const int32 MaxDepth,
		const int32 Depth,
		TArray<TSharedPtr<FJsonValue>>& OutNodes)
	{
		if (Widget == nullptr || Depth > MaxDepth)
		{
			return;
		}

		const FString WidgetId = GetWidgetStableId(WidgetBlueprint, Widget);
		const FString ClassPath = Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString();
		const bool bPassNameFilter = NameGlob.IsEmpty() || Widget->GetName().MatchesWildcard(NameGlob);
		const bool bPassClassFilter = AllowedClassPaths.Num() == 0 || AllowedClassPaths.Contains(ClassPath);

		if (bPassNameFilter && bPassClassFilter)
		{
			TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
			NodeObject->SetStringField(TEXT("widget_id"), WidgetId);
			NodeObject->SetStringField(TEXT("name"), Widget->GetName());
			NodeObject->SetStringField(TEXT("class_path"), ClassPath);
			NodeObject->SetStringField(TEXT("parent_id"), ParentId);
			NodeObject->SetStringField(TEXT("slot_type"), Widget->Slot ? Widget->Slot->GetClass()->GetName() : TEXT(""));

			int32 ChildrenCount = 0;
			if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				ChildrenCount = Panel->GetChildrenCount();
			}
			NodeObject->SetNumberField(TEXT("children_count"), ChildrenCount);
			NodeObject->SetObjectField(TEXT("flags"), MakeShared<FJsonObject>());
			OutNodes.Add(MakeShared<FJsonValueObject>(NodeObject));
		}

		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
			{
				BuildWidgetTreeNodesRecursive(WidgetBlueprint, Panel->GetChildAt(Index), WidgetId, NameGlob, AllowedClassPaths, MaxDepth, Depth + 1, OutNodes);
			}
		}
	}
}

void UMCPToolRegistrySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LoadSchemaBundle();
	RegisterBuiltInTools();
	RebuildSchemaHash();

	UE_LOG(LogUnrealMCP, Log, TEXT("MCP tool registry initialized. registered_tools=%d"), RegisteredTools.Num());
}

void UMCPToolRegistrySubsystem::Deinitialize()
{
	RegisteredTools.Reset();
	BundleSchemas.Reset();
	CachedSchemaHash.Empty();
	Super::Deinitialize();
}

bool UMCPToolRegistrySubsystem::ValidateRequest(const FMCPRequestEnvelope& Request, FMCPDiagnostic& OutDiagnostic) const
{
	const FMCPToolDefinition* ToolDefinition = RegisteredTools.Find(Request.Tool);
	if (ToolDefinition == nullptr || !ToolDefinition->bEnabled || !ToolDefinition->Executor)
	{
		OutDiagnostic.Code = MCPErrorCodes::TOOL_NOT_FOUND;
		OutDiagnostic.Message = TEXT("Requested tool is not available.");
		OutDiagnostic.Detail = FString::Printf(TEXT("tool=%s"), *Request.Tool);
		OutDiagnostic.Suggestion = TEXT("Call tools.list and use an enabled tool.");
		return false;
	}

	if (!ToolDefinition->ParamsSchema.IsValid())
	{
		return true;
	}

	const TSharedPtr<FJsonObject> ParamsObject = Request.Params.IsValid() ? Request.Params : MakeShared<FJsonObject>();
	const TSharedPtr<FJsonValueObject> ParamsValue = MakeShared<FJsonValueObject>(ParamsObject);

	FString SchemaError;
	if (!ValidateJsonValueAgainstSchema(ParamsValue, ToolDefinition->ParamsSchema, TEXT("params"), SchemaError))
	{
		OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutDiagnostic.Message = TEXT("Request params failed schema validation.");
		OutDiagnostic.Detail = SchemaError;
		OutDiagnostic.Suggestion = TEXT("Call tools.list with include_schemas=true and retry with schema-compliant params.");
		return false;
	}

	return true;
}

bool UMCPToolRegistrySubsystem::ExecuteTool(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	const FMCPToolDefinition* ToolDefinition = RegisteredTools.Find(Request.Tool);
	if (ToolDefinition == nullptr || !ToolDefinition->bEnabled || !ToolDefinition->Executor)
	{
		OutResult.Status = EMCPResponseStatus::Error;
		OutResult.ResultObject = MakeShared<FJsonObject>();

		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::TOOL_NOT_FOUND;
		Diagnostic.Message = TEXT("Requested tool is not available.");
		Diagnostic.Detail = FString::Printf(TEXT("tool=%s"), *Request.Tool);
		Diagnostic.Suggestion = TEXT("Call tools.list and use an enabled tool.");
		OutResult.Diagnostics.Add(Diagnostic);
		return false;
	}

	const bool bSuccess = ToolDefinition->Executor(Request, OutResult);
	if (!bSuccess && OutResult.Diagnostics.Num() == 0)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Tool execution failed without diagnostics.");
		Diagnostic.Detail = Request.Tool;
		OutResult.Diagnostics.Add(Diagnostic);
	}

	if (!OutResult.ResultObject.IsValid())
	{
		OutResult.ResultObject = MakeShared<FJsonObject>();
	}

	return bSuccess;
}

bool UMCPToolRegistrySubsystem::IsWriteTool(const FString& ToolName) const
{
	if (const FMCPToolDefinition* ToolDefinition = RegisteredTools.Find(ToolName))
	{
		return ToolDefinition->bWriteTool;
	}

	return false;
}

bool UMCPToolRegistrySubsystem::BuildToolsList(
	const bool bIncludeSchemas,
	const FString& DomainFilter,
	TArray<TSharedPtr<FJsonValue>>& OutTools) const
{
	OutTools.Reset();
	TArray<FString> ToolNames;
	RegisteredTools.GetKeys(ToolNames);
	ToolNames.Sort();

	for (const FString& ToolName : ToolNames)
	{
		const FMCPToolDefinition* ToolDefinition = RegisteredTools.Find(ToolName);
		if (ToolDefinition == nullptr || !ToolDefinition->bEnabled)
		{
			continue;
		}

		if (!DomainFilter.IsEmpty() && !ToolDefinition->Domain.Equals(DomainFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedRef<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("name"), ToolDefinition->Name);
		ToolObject->SetStringField(TEXT("domain"), ToolDefinition->Domain);
		ToolObject->SetStringField(TEXT("version"), ToolDefinition->Version);
		ToolObject->SetBoolField(TEXT("enabled"), ToolDefinition->bEnabled);
		ToolObject->SetBoolField(TEXT("write"), ToolDefinition->bWriteTool);

		if (bIncludeSchemas)
		{
			if (ToolDefinition->ParamsSchema.IsValid())
			{
				ToolObject->SetObjectField(TEXT("params_schema"), ToolDefinition->ParamsSchema.ToSharedRef());
			}
			if (ToolDefinition->ResultSchema.IsValid())
			{
				ToolObject->SetObjectField(TEXT("result_schema"), ToolDefinition->ResultSchema.ToSharedRef());
			}
		}

		OutTools.Add(MakeShared<FJsonValueObject>(ToolObject));
	}

	return true;
}

FString UMCPToolRegistrySubsystem::GetSchemaHash() const
{
	return CachedSchemaHash;
}

FString UMCPToolRegistrySubsystem::GetProtocolVersion() const
{
	return TEXT("unreal-mcp/1.0");
}

TArray<FString> UMCPToolRegistrySubsystem::GetCapabilities() const
{
	return {
		TEXT("core_tools_v1"),
		TEXT("asset_ops_v1"),
		TEXT("changeset_ops_v1"),
		TEXT("job_ops_v1"),
		TEXT("idempotency_v1"),
		TEXT("lock_lease_v1"),
		TEXT("schema_validation_v1"),
		TEXT("timeout_override_v1"),
		TEXT("umg_stable_widget_id_v1"),
		TEXT("niagara_stack_compat_v2"),
		TEXT("event_stream_v1"),
		TEXT("observability_metrics_v1"),
		TEXT("event_stream_ws_push_v1")
	};
}

TArray<FString> UMCPToolRegistrySubsystem::GetRegisteredToolNames() const
{
	TArray<FString> ToolNames;
	RegisteredTools.GetKeys(ToolNames);
	ToolNames.Sort();
	return ToolNames;
}

void UMCPToolRegistrySubsystem::RegisterBuiltInTools()
{
	RegisterTool({
		TEXT("tools.list"),
		TEXT("tools"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleToolsList(Request, OutResult); }
	});

	RegisterTool({
		TEXT("system.health"),
		TEXT("system"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSystemHealth(Request, OutResult); }
	});

	RegisterTool({
		TEXT("asset.find"),
		TEXT("asset"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleAssetFind(Request, OutResult); }
	});

	RegisterTool({
		TEXT("asset.load"),
		TEXT("asset"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleAssetLoad(Request, OutResult); }
	});

	RegisterTool({
		TEXT("asset.save"),
		TEXT("asset"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleAssetSave(Request, OutResult); }
	});

	RegisterTool({
		TEXT("asset.import"),
		TEXT("asset"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleAssetImport(Request, OutResult); }
	});

	RegisterTool({
		TEXT("asset.create"),
		TEXT("asset"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleAssetCreate(Request, OutResult); }
	});

	RegisterTool({
		TEXT("blueprint.class.create"),
		TEXT("blueprint"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleBlueprintClassCreate(Request, OutResult); }
	});

	RegisterTool({
		TEXT("asset.duplicate"),
		TEXT("asset"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleAssetDuplicate(Request, OutResult); }
	});

	RegisterTool({
		TEXT("asset.rename"),
		TEXT("asset"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleAssetRename(Request, OutResult); }
	});

	RegisterTool({
		TEXT("asset.delete"),
		TEXT("asset"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleAssetDelete(Request, OutResult); }
	});

	RegisterTool({
		TEXT("settings.project.get"),
		TEXT("settings"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSettingsProjectGet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("settings.project.patch"),
		TEXT("settings"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSettingsProjectPatch(Request, OutResult); }
	});

	RegisterTool({
		TEXT("settings.project.apply"),
		TEXT("settings"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSettingsProjectApply(Request, OutResult); }
	});

	RegisterTool({
		TEXT("settings.gamemode.get"),
		TEXT("settings"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSettingsGameModeGet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("settings.gamemode.set_default"),
		TEXT("settings"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSettingsGameModeSetDefault(Request, OutResult); }
	});

	RegisterTool({
		TEXT("settings.gamemode.compose"),
		TEXT("settings"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSettingsGameModeCompose(Request, OutResult); }
	});

	RegisterTool({
		TEXT("settings.gamemode.set_map_override"),
		TEXT("settings"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSettingsGameModeSetMapOverride(Request, OutResult); }
	});

	RegisterTool({
		TEXT("settings.gamemode.remove_map_override"),
		TEXT("settings"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleSettingsGameModeRemoveMapOverride(Request, OutResult); }
	});

	RegisterTool({
		TEXT("object.inspect"),
		TEXT("object"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleObjectInspect(Request, OutResult); }
	});

	RegisterTool({
		TEXT("object.patch"),
		TEXT("object"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleObjectPatch(Request, OutResult); }
	});

	RegisterTool({
		TEXT("world.outliner.list"),
		TEXT("world"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleWorldOutlinerList(Request, OutResult); }
	});

	RegisterTool({
		TEXT("world.selection.get"),
		TEXT("world"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleWorldSelectionGet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("world.selection.set"),
		TEXT("world"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleWorldSelectionSet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("mat.instance.params.get"),
		TEXT("mat"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleMatInstanceParamsGet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("mat.instance.params.set"),
		TEXT("mat"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleMatInstanceParamsSet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("niagara.params.get"),
		TEXT("niagara"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleNiagaraParamsGet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("niagara.params.set"),
		TEXT("niagara"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleNiagaraParamsSet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("niagara.stack.list"),
		TEXT("niagara"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleNiagaraStackList(Request, OutResult); }
	});

	RegisterTool({
		TEXT("niagara.stack.module.set_param"),
		TEXT("niagara"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleNiagaraStackModuleSetParam(Request, OutResult); }
	});

	RegisterTool({
		TEXT("umg.widget.class.list"),
		TEXT("umg"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleUMGWidgetClassList(Request, OutResult); }
	});

	RegisterTool({
		TEXT("umg.tree.get"),
		TEXT("umg"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleUMGTreeGet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("umg.widget.inspect"),
		TEXT("umg"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleUMGWidgetInspect(Request, OutResult); }
	});

	RegisterTool({
		TEXT("umg.widget.add"),
		TEXT("umg"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleUMGWidgetAdd(Request, OutResult); }
	});

	RegisterTool({
		TEXT("umg.widget.remove"),
		TEXT("umg"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleUMGWidgetRemove(Request, OutResult); }
	});

	RegisterTool({
		TEXT("umg.widget.reparent"),
		TEXT("umg"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleUMGWidgetReparent(Request, OutResult); }
	});

	RegisterTool({
		TEXT("umg.widget.patch"),
		TEXT("umg"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleUMGWidgetPatch(Request, OutResult); }
	});

	RegisterTool({
		TEXT("umg.slot.patch"),
		TEXT("umg"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleUMGSlotPatch(Request, OutResult); }
	});

	RegisterTool({
		TEXT("changeset.list"),
		TEXT("changeset"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleChangeSetList(Request, OutResult); }
	});

	RegisterTool({
		TEXT("changeset.get"),
		TEXT("changeset"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleChangeSetGet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("changeset.rollback.preview"),
		TEXT("changeset"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleChangeSetRollbackPreview(Request, OutResult); }
	});

	RegisterTool({
		TEXT("changeset.rollback.apply"),
		TEXT("changeset"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleChangeSetRollbackApply(Request, OutResult); }
	});

	RegisterTool({
		TEXT("job.get"),
		TEXT("job"),
		TEXT("1.0.0"),
		true,
		false,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleJobGet(Request, OutResult); }
	});

	RegisterTool({
		TEXT("job.cancel"),
		TEXT("job"),
		TEXT("1.0.0"),
		true,
		true,
		nullptr,
		nullptr,
		[this](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) { return HandleJobCancel(Request, OutResult); }
	});
}

void UMCPToolRegistrySubsystem::RegisterTool(FMCPToolDefinition Definition)
{
	Definition.Domain = ExtractDomain(Definition.Name);
	Definition.ParamsSchema = FindSchemaObject(Definition.Name, TEXT("params_schema"));
	Definition.ResultSchema = FindSchemaObject(Definition.Name, TEXT("result_schema"));
	RegisteredTools.Add(Definition.Name, MoveTemp(Definition));
}

void UMCPToolRegistrySubsystem::LoadSchemaBundle()
{
	BundleSchemas.Reset();

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("Could not find UnrealMCP plugin descriptor while loading schema bundle."));
		return;
	}

	const FString SchemaFilePath30 = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/schemas_30_tools.json"));
	const FString SchemaFilePath26 = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/schemas_26_tools.json"));
	const FString PreferredSchemaFilePath = FPaths::FileExists(SchemaFilePath30) ? SchemaFilePath30 : SchemaFilePath26;

	FString SchemaContent;
	if (!FFileHelper::LoadFileToString(SchemaContent, *PreferredSchemaFilePath))
	{
		if (!PreferredSchemaFilePath.Equals(SchemaFilePath26, ESearchCase::CaseSensitive) ||
			!FFileHelper::LoadFileToString(SchemaContent, *SchemaFilePath26))
		{
			UE_LOG(LogUnrealMCP, Warning, TEXT("Could not load schema bundle file: %s"), *PreferredSchemaFilePath);
			return;
		}
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SchemaContent);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogUnrealMCP, Warning, TEXT("Failed to parse schema bundle JSON."));
		return;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : RootObject->Values)
	{
		TSharedPtr<FJsonObject> ToolSchema = Entry.Value.IsValid() ? Entry.Value->AsObject() : nullptr;
		if (ToolSchema.IsValid())
		{
			BundleSchemas.Add(Entry.Key, ToolSchema);
		}
	}

	UE_LOG(LogUnrealMCP, Log, TEXT("Loaded schema bundle. tools=%d"), BundleSchemas.Num());
}

TSharedPtr<FJsonObject> UMCPToolRegistrySubsystem::FindSchemaObject(const FString& ToolName, const FString& SchemaKey) const
{
	if (const TSharedPtr<FJsonObject>* ToolSchema = BundleSchemas.Find(ToolName))
	{
		const TSharedPtr<FJsonObject>* SchemaObject = nullptr;
		if ((*ToolSchema).IsValid() && (*ToolSchema)->TryGetObjectField(SchemaKey, SchemaObject) && SchemaObject != nullptr && SchemaObject->IsValid())
		{
			return *SchemaObject;
		}
	}

	return nullptr;
}

void UMCPToolRegistrySubsystem::RebuildSchemaHash()
{
	TArray<FString> ToolNames = GetRegisteredToolNames();
	FString HashMaterial;
	for (const FString& ToolName : ToolNames)
	{
		const FMCPToolDefinition* ToolDefinition = RegisteredTools.Find(ToolName);
		if (ToolDefinition == nullptr || !ToolDefinition->bEnabled)
		{
			continue;
		}

		HashMaterial += ToolDefinition->Name;
		HashMaterial += TEXT("|");
		HashMaterial += MCPJson::SerializeJsonObject(ToolDefinition->ParamsSchema);
		HashMaterial += TEXT("|");
		HashMaterial += MCPJson::SerializeJsonObject(ToolDefinition->ResultSchema);
		HashMaterial += TEXT("\n");
	}

	FTCHARToUTF8 Utf8Data(*HashMaterial);
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8Data.Get(), Utf8Data.Length(), Digest);
	CachedSchemaHash = BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

bool UMCPToolRegistrySubsystem::HandleToolsList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	bool bIncludeSchemas = true;
	FString DomainFilter;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetBoolField(TEXT("include_schemas"), bIncludeSchemas);
		Request.Params->TryGetStringField(TEXT("domain_filter"), DomainFilter);
	}

	TArray<TSharedPtr<FJsonValue>> Tools;
	BuildToolsList(bIncludeSchemas, DomainFilter, Tools);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("protocol_version"), GetProtocolVersion());
	OutResult.ResultObject->SetStringField(TEXT("schema_hash"), GetSchemaHash());
	OutResult.ResultObject->SetArrayField(TEXT("capabilities"), ToJsonStringArray(GetCapabilities()));
	OutResult.ResultObject->SetArrayField(TEXT("tools"), Tools);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleSystemHealth(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
	const FString PluginVersion = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("unknown");
	const FString EngineVersion = FEngineVersion::Current().ToString();

	const UMCPPolicySubsystem* PolicySubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPPolicySubsystem>() : nullptr;
	const UMCPEventStreamSubsystem* EventStreamSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPEventStreamSubsystem>() : nullptr;
	const UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>() : nullptr;
	const UMCPWebSocketTransportSubsystem* WebSocketTransportSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPWebSocketTransportSubsystem>() : nullptr;
	const bool bSafeMode = PolicySubsystem ? PolicySubsystem->IsSafeMode() : false;

	TSharedRef<FJsonObject> EditorState = MakeShared<FJsonObject>();
	EditorState->SetBoolField(TEXT("pie"), GEditor != nullptr && GEditor->PlayWorld != nullptr);
	EditorState->SetBoolField(TEXT("dry_run_request"), Request.Context.bDryRun);
	EditorState->SetNumberField(TEXT("registered_tool_count"), static_cast<double>(RegisteredTools.Num()));

	if (EventStreamSubsystem != nullptr)
	{
		EditorState->SetObjectField(TEXT("event_stream"), EventStreamSubsystem->BuildSnapshot(8));
	}
	else
	{
		TSharedRef<FJsonObject> EventStreamFallback = MakeShared<FJsonObject>();
		EventStreamFallback->SetBoolField(TEXT("supported"), false);
		EditorState->SetObjectField(TEXT("event_stream"), EventStreamFallback);
	}

	if (ObservabilitySubsystem != nullptr)
	{
		EditorState->SetObjectField(TEXT("observability"), ObservabilitySubsystem->BuildSnapshot());
	}
	else
	{
		EditorState->SetObjectField(TEXT("observability"), MakeShared<FJsonObject>());
	}

	TSharedRef<FJsonObject> TransportState = MakeShared<FJsonObject>();
	if (WebSocketTransportSubsystem != nullptr)
	{
		TransportState->SetBoolField(TEXT("enabled"), WebSocketTransportSubsystem->IsEnabled());
		TransportState->SetBoolField(TEXT("listening"), WebSocketTransportSubsystem->IsListening());
		TransportState->SetStringField(TEXT("bind_address"), WebSocketTransportSubsystem->GetBindAddress());
		TransportState->SetNumberField(TEXT("port"), static_cast<double>(WebSocketTransportSubsystem->GetListenPort()));
		TransportState->SetNumberField(TEXT("client_count"), static_cast<double>(WebSocketTransportSubsystem->GetClientCount()));
	}
	else
	{
		TransportState->SetBoolField(TEXT("enabled"), false);
		TransportState->SetBoolField(TEXT("listening"), false);
		TransportState->SetStringField(TEXT("bind_address"), TEXT(""));
		TransportState->SetNumberField(TEXT("port"), 0.0);
		TransportState->SetNumberField(TEXT("client_count"), 0.0);
	}
	EditorState->SetObjectField(TEXT("event_stream_transport"), TransportState);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("engine_version"), EngineVersion);
	OutResult.ResultObject->SetStringField(TEXT("plugin_version"), PluginVersion);
	OutResult.ResultObject->SetStringField(TEXT("protocol_version"), GetProtocolVersion());
	OutResult.ResultObject->SetBoolField(TEXT("safe_mode"), bSafeMode);
	OutResult.ResultObject->SetObjectField(TEXT("editor_state"), EditorState);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleAssetFind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString PathGlob = TEXT("/Game/**");
	FString NameGlob;
	int32 Limit = 50;
	const int32 Cursor = ParseCursor(Request.Params);
	TSet<FString> AllowedClassPaths;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("path_glob"), PathGlob);
		Request.Params->TryGetStringField(TEXT("name_glob"), NameGlob);
		double LimitNumber = static_cast<double>(Limit);
		Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, 500);

		const TArray<TSharedPtr<FJsonValue>>* ClassPathValues = nullptr;
		if (Request.Params->TryGetArrayField(TEXT("class_path_in"), ClassPathValues) && ClassPathValues != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& ClassPathValue : *ClassPathValues)
			{
				FString ClassPath;
				if (ClassPathValue.IsValid() && ClassPathValue->TryGetString(ClassPath) && !ClassPath.IsEmpty())
				{
					AllowedClassPaths.Add(ClassPath);
				}
			}
		}
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AssetDataList;
	AssetRegistry.GetAllAssets(AssetDataList, true);

	TArray<TSharedPtr<FJsonObject>> FilteredAssets;
	for (const FAssetData& AssetData : AssetDataList)
	{
		const FString ObjectPath = AssetData.GetObjectPathString();
		const FString PackagePath = AssetData.PackagePath.ToString();
		const FString ClassPath = AssetData.AssetClassPath.ToString();
		const FString AssetName = AssetData.AssetName.ToString();

		if (!PathGlob.IsEmpty() && !ObjectPath.MatchesWildcard(PathGlob) && !PackagePath.MatchesWildcard(PathGlob))
		{
			continue;
		}

		if (!NameGlob.IsEmpty() && !AssetName.MatchesWildcard(NameGlob))
		{
			continue;
		}

		if (!MatchesAnyClassPath(AllowedClassPaths, ClassPath))
		{
			continue;
		}

		TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("object_path"), ObjectPath);
		AssetObject->SetStringField(TEXT("class_path"), ClassPath);
		AssetObject->SetStringField(TEXT("package_path"), PackagePath);
		AssetObject->SetStringField(TEXT("name"), AssetName);
		FilteredAssets.Add(AssetObject);
	}

	FilteredAssets.Sort([](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
	{
		FString LeftPath;
		FString RightPath;
		Left->TryGetStringField(TEXT("object_path"), LeftPath);
		Right->TryGetStringField(TEXT("object_path"), RightPath);
		return LeftPath < RightPath;
	});

	const int32 SafeCursor = FMath::Max(0, Cursor);
	const int32 EndIndex = FMath::Min(SafeCursor + Limit, FilteredAssets.Num());

	TArray<TSharedPtr<FJsonValue>> ResultAssets;
	for (int32 Index = SafeCursor; Index < EndIndex; ++Index)
	{
		ResultAssets.Add(MakeShared<FJsonValueObject>(FilteredAssets[Index].ToSharedRef()));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("assets"), ResultAssets);
	if (EndIndex < FilteredAssets.Num())
	{
		OutResult.ResultObject->SetStringField(TEXT("next_cursor"), FString::FromInt(EndIndex));
	}
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleAssetLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for asset.load.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* LoadedObject = FindObject<UObject>(nullptr, *ObjectPath);
	if (LoadedObject == nullptr)
	{
		LoadedObject = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("loaded"), LoadedObject != nullptr);
	OutResult.ResultObject->SetStringField(TEXT("object_path"), ObjectPath);

	if (LoadedObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Asset could not be loaded.");
		Diagnostic.Detail = ObjectPath;
		Diagnostic.Suggestion = TEXT("Verify object_path and ensure the asset exists.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleAssetSave(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	bool bOnlyDirty = true;
	const TArray<TSharedPtr<FJsonValue>>* PackageValues = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetBoolField(TEXT("only_dirty"), bOnlyDirty);
		Request.Params->TryGetArrayField(TEXT("packages"), PackageValues);
	}

	if (PackageValues == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("packages array is required for asset.save.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> SavedPackages;
	TArray<FString> FailedPackages;
	TArray<UPackage*> ExplicitPackages;
	TArray<FString> ExplicitPackageNames;
	ExplicitPackages.Reserve(PackageValues->Num());
	ExplicitPackageNames.Reserve(PackageValues->Num());

	for (const TSharedPtr<FJsonValue>& PackageValue : *PackageValues)
	{
		FString PackageInput;
		if (!PackageValue.IsValid() || !PackageValue->TryGetString(PackageInput) || PackageInput.IsEmpty())
		{
			continue;
		}

		UPackage* Package = ResolvePackageByName(PackageInput);
		const FString PackageName = NormalizePackageNameFromInput(PackageInput);
		if (Package == nullptr)
		{
			FailedPackages.Add(PackageName.IsEmpty() ? PackageInput : PackageName);
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Package not found while saving assets.");
			Diagnostic.Detail = PackageInput;
			Diagnostic.bRetriable = false;
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		ExplicitPackages.Add(Package);
		ExplicitPackageNames.Add(Package->GetName());
	}

	for (int32 Index = 0; Index < ExplicitPackages.Num(); ++Index)
	{
		UPackage* Package = ExplicitPackages[Index];
		const FString PackageName = ExplicitPackageNames.IsValidIndex(Index) ? ExplicitPackageNames[Index] : Package->GetName();
		bool bSaved = true;
		if (!Request.Context.bDryRun)
		{
			const TArray<UPackage*> SinglePackageArray{ Package };
			bSaved = UEditorLoadingAndSavingUtils::SavePackages(SinglePackageArray, bOnlyDirty);
		}
		if (bSaved)
		{
			SavedPackages.Add(PackageName);
		}
		else
		{
			FailedPackages.Add(PackageName);
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::SAVE_FAILED;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Package save failed.");
			Diagnostic.Detail = PackageName;
			Diagnostic.bRetriable = true;
			OutResult.Diagnostics.Add(Diagnostic);
		}
	}

	OutResult.TouchedPackages.Append(SavedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("saved"), ToJsonStringArray(SavedPackages));
	OutResult.ResultObject->SetArrayField(TEXT("failed"), ToJsonStringArray(FailedPackages));

	if (FailedPackages.Num() == 0)
	{
		OutResult.Status = EMCPResponseStatus::Ok;
		return true;
	}

	if (SavedPackages.Num() > 0)
	{
		OutResult.Status = EMCPResponseStatus::Partial;
		return true;
	}

	OutResult.Status = EMCPResponseStatus::Error;
	return false;
}

bool UMCPToolRegistrySubsystem::HandleAssetImport(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	TArray<FString> SourceFiles;
	FString DestPackagePath;
	FString DestAssetName;
	FString ImportAs = TEXT("auto");
	bool bReplaceExisting = false;
	bool bAutomated = true;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		ParseStringArrayField(Request.Params, TEXT("source_files"), SourceFiles);
		Request.Params->TryGetStringField(TEXT("dest_package_path"), DestPackagePath);
		Request.Params->TryGetStringField(TEXT("dest_asset_name"), DestAssetName);
		Request.Params->TryGetStringField(TEXT("import_as"), ImportAs);
		Request.Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
		Request.Params->TryGetBoolField(TEXT("automated"), bAutomated);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (SourceFiles.Num() == 0 || DestPackagePath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("source_files and dest_package_path are required for asset.import.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!FPackageName::IsValidLongPackageName(DestPackagePath))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid dest_package_path for asset.import.");
		Diagnostic.Detail = DestPackagePath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	ImportAs = ImportAs.ToLower();
	const bool bImportAsAuto = ImportAs.Equals(TEXT("auto"));
	const bool bImportAsTexture = ImportAs.Equals(TEXT("texture"));
	const bool bImportAsStaticMesh = ImportAs.Equals(TEXT("static_mesh"));
	const bool bImportAsSkeletalMesh = ImportAs.Equals(TEXT("skeletal_mesh"));
	if (!bImportAsAuto && !bImportAsTexture && !bImportAsStaticMesh && !bImportAsSkeletalMesh)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("import_as must be one of: auto, texture, static_mesh, skeletal_mesh.");
		Diagnostic.Detail = ImportAs;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	DestAssetName = ObjectTools::SanitizeObjectName(DestAssetName);
	if (!DestAssetName.IsEmpty() && SourceFiles.Num() != 1)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("dest_asset_name can only be used when source_files has exactly one entry.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const TSet<FString> ImageExtensions = { TEXT(".png"), TEXT(".jpg"), TEXT(".jpeg"), TEXT(".tga"), TEXT(".exr") };
	const TSet<FString> MeshExtensions = { TEXT(".fbx") };

	auto NormalizeSourceFilePath = [](const FString& InPath) -> FString
	{
		FString CandidatePath = InPath;
		CandidatePath.TrimStartAndEndInline();
		if (CandidatePath.IsEmpty())
		{
			return CandidatePath;
		}

		FPaths::NormalizeFilename(CandidatePath);

#if PLATFORM_WINDOWS
		if (CandidatePath.StartsWith(TEXT("/mnt/"), ESearchCase::IgnoreCase) &&
			CandidatePath.Len() > 7 &&
			FChar::IsAlpha(CandidatePath[5]) &&
			CandidatePath[6] == TCHAR('/'))
		{
			const TCHAR DriveLetter = FChar::ToUpper(CandidatePath[5]);
			const FString Remainder = CandidatePath.Mid(6);
			CandidatePath = FString::Printf(TEXT("%c:%s"), DriveLetter, *Remainder);
		}
		CandidatePath.ReplaceInline(TEXT("/"), TEXT("\\"));
#endif

		const FString AbsolutePath = FPaths::ConvertRelativePathToFull(CandidatePath);
		if (IFileManager::Get().FileExists(*AbsolutePath))
		{
			return AbsolutePath;
		}

		return CandidatePath;
	};

	auto InferClassPath = [&](const FString& Extension) -> FString
	{
		if (bImportAsTexture || (bImportAsAuto && ImageExtensions.Contains(Extension)))
		{
			return TEXT("/Script/Engine.Texture2D");
		}
		if (bImportAsStaticMesh)
		{
			return TEXT("/Script/Engine.StaticMesh");
		}
		if (bImportAsSkeletalMesh)
		{
			return TEXT("/Script/Engine.SkeletalMesh");
		}
		return FString();
	};

	struct FImportCandidate
	{
		FString RequestedPath;
		FString ResolvedPath;
		FString Extension;
		FString AssetName;
	};

	TArray<FImportCandidate> Candidates;
	TArray<FString> FailedFiles;

	for (const FString& RequestedPathRaw : SourceFiles)
	{
		FString RequestedPath = RequestedPathRaw;
		RequestedPath.TrimStartAndEndInline();
		if (RequestedPath.IsEmpty())
		{
			continue;
		}

		const FString ResolvedPath = NormalizeSourceFilePath(RequestedPath);
		if (!IFileManager::Get().FileExists(*ResolvedPath))
		{
			FailedFiles.AddUnique(RequestedPath);
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_NOT_FOUND;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("source file was not found.");
			Diagnostic.Detail = RequestedPath;
			Diagnostic.Suggestion = TEXT("Provide an existing file path reachable by the Unreal Editor host.");
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		const FString Extension = FPaths::GetExtension(ResolvedPath, true).ToLower();
		const bool bIsImageFile = ImageExtensions.Contains(Extension);
		const bool bIsMeshFile = MeshExtensions.Contains(Extension);
		if (!bIsImageFile && !bIsMeshFile)
		{
			FailedFiles.AddUnique(RequestedPath);
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_IMPORT_UNSUPPORTED;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Unsupported source file extension for asset.import.");
			Diagnostic.Detail = FString::Printf(TEXT("file=%s extension=%s"), *RequestedPath, *Extension);
			Diagnostic.Suggestion = TEXT("Use png/jpg/jpeg/tga/exr for textures or fbx for static/skeletal meshes.");
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		if ((bImportAsTexture && !bIsImageFile) || ((bImportAsStaticMesh || bImportAsSkeletalMesh) && !bIsMeshFile))
		{
			FailedFiles.AddUnique(RequestedPath);
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_IMPORT_UNSUPPORTED;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("source file extension is incompatible with import_as.");
			Diagnostic.Detail = FString::Printf(TEXT("file=%s extension=%s import_as=%s"), *RequestedPath, *Extension, *ImportAs);
			Diagnostic.Suggestion = TEXT("Use import_as=auto or provide a matching source file type.");
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		FString CandidateAssetName = SourceFiles.Num() == 1 && !DestAssetName.IsEmpty()
			? DestAssetName
			: ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(ResolvedPath));

		if (!IsValidAssetDestination(DestPackagePath, CandidateAssetName))
		{
			FailedFiles.AddUnique(RequestedPath);
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Could not build valid destination object path for imported file.");
			Diagnostic.Detail = FString::Printf(TEXT("dest_package_path=%s asset_name=%s"), *DestPackagePath, *CandidateAssetName);
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		Candidates.Add({ RequestedPath, ResolvedPath, Extension, CandidateAssetName });
	}

	TArray<TSharedPtr<FJsonValue>> ImportedEntries;
	ImportedEntries.Reserve(Candidates.Num());

	if (Request.Context.bDryRun)
	{
		for (const FImportCandidate& Candidate : Candidates)
		{
			const FString ObjectPath = BuildObjectPath(DestPackagePath, Candidate.AssetName);
			const FString PackageName = BuildPackageName(DestPackagePath, Candidate.AssetName);
			const FString ClassPath = InferClassPath(Candidate.Extension);

			if (!PackageName.IsEmpty())
			{
				OutResult.TouchedPackages.AddUnique(PackageName);
			}

			TSharedRef<FJsonObject> ImportedEntry = MakeShared<FJsonObject>();
			ImportedEntry->SetStringField(TEXT("source_file"), Candidate.RequestedPath);
			ImportedEntry->SetStringField(TEXT("resolved_source_file"), Candidate.ResolvedPath);
			ImportedEntry->SetStringField(TEXT("object_path"), ObjectPath);
			ImportedEntry->SetStringField(TEXT("package"), PackageName);
			if (!ClassPath.IsEmpty())
			{
				ImportedEntry->SetStringField(TEXT("class_path"), ClassPath);
			}
			ImportedEntries.Add(MakeShared<FJsonValueObject>(ImportedEntry));
		}
	}
	else
	{
		TArray<UAssetImportTask*> ImportTasks;
		ImportTasks.Reserve(Candidates.Num());
		TMap<UAssetImportTask*, FString> RequestedPathByTask;
		RequestedPathByTask.Reserve(Candidates.Num());
		TMap<UAssetImportTask*, FString> ExtensionByTask;
		ExtensionByTask.Reserve(Candidates.Num());

		for (const FImportCandidate& Candidate : Candidates)
		{
			UAssetImportTask* ImportTask = NewObject<UAssetImportTask>(GetTransientPackage());
			if (ImportTask == nullptr)
			{
				FailedFiles.AddUnique(Candidate.RequestedPath);
				continue;
			}

			ImportTask->Filename = Candidate.ResolvedPath;
			ImportTask->DestinationPath = DestPackagePath;
			ImportTask->DestinationName = Candidate.AssetName;
			ImportTask->bAutomated = bAutomated;
			ImportTask->bReplaceExisting = bReplaceExisting;
			ImportTask->bReplaceExistingSettings = bReplaceExisting;
			ImportTask->bSave = false;
			ImportTask->bAsync = false;

			if (Candidate.Extension.Equals(TEXT(".fbx")))
			{
				if (bImportAsStaticMesh || bImportAsSkeletalMesh)
				{
					UFbxImportUI* FbxImportUI = NewObject<UFbxImportUI>(ImportTask);
					if (FbxImportUI != nullptr)
					{
						FbxImportUI->bAutomatedImportShouldDetectType = false;
						FbxImportUI->bImportMesh = true;
						FbxImportUI->bImportAnimations = false;
						FbxImportUI->bImportMaterials = false;
						FbxImportUI->bImportTextures = false;
						FbxImportUI->bImportAsSkeletal = bImportAsSkeletalMesh;
						FbxImportUI->MeshTypeToImport = bImportAsSkeletalMesh ? FBXIT_SkeletalMesh : FBXIT_StaticMesh;
						ImportTask->Options = FbxImportUI;
					}
				}
				else if (bImportAsAuto)
				{
					UFbxImportUI* FbxImportUI = NewObject<UFbxImportUI>(ImportTask);
					if (FbxImportUI != nullptr)
					{
						FbxImportUI->bAutomatedImportShouldDetectType = true;
						FbxImportUI->bImportMaterials = false;
						FbxImportUI->bImportTextures = false;
						ImportTask->Options = FbxImportUI;
					}
				}
			}

			ImportTasks.Add(ImportTask);
			RequestedPathByTask.Add(ImportTask, Candidate.RequestedPath);
			ExtensionByTask.Add(ImportTask, Candidate.Extension);
		}

		if (ImportTasks.Num() > 0)
		{
			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			AssetToolsModule.Get().ImportAssetTasks(ImportTasks);
		}

		for (UAssetImportTask* ImportTask : ImportTasks)
		{
			if (ImportTask == nullptr)
			{
				continue;
			}

			const FString RequestedPath = RequestedPathByTask.FindRef(ImportTask);
			const FString ResolvedPath = ImportTask->Filename;
			const FString ImportedExtension = ExtensionByTask.FindRef(ImportTask);
			const FString FallbackClassPath = InferClassPath(ImportedExtension);
			if (ImportTask->ImportedObjectPaths.Num() == 0)
			{
				FailedFiles.AddUnique(RequestedPath.IsEmpty() ? ResolvedPath : RequestedPath);
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::ASSET_IMPORT_FAILED;
				Diagnostic.Severity = TEXT("warning");
				Diagnostic.Message = TEXT("Asset import finished without imported objects.");
				Diagnostic.Detail = RequestedPath.IsEmpty() ? ResolvedPath : RequestedPath;
				Diagnostic.Suggestion = TEXT("Check source data validity and import settings.");
				Diagnostic.bRetriable = true;
				OutResult.Diagnostics.Add(Diagnostic);
				continue;
			}

			for (const FString& ImportedObjectPath : ImportTask->ImportedObjectPaths)
			{
				const FString PackageName = NormalizePackageNameFromInput(ImportedObjectPath);
				if (!PackageName.IsEmpty())
				{
					OutResult.TouchedPackages.AddUnique(PackageName);
				}

				UObject* ImportedObject = ResolveObjectByPath(ImportedObjectPath);
				FString ClassPath = ImportedObject != nullptr && ImportedObject->GetClass() != nullptr
					? ImportedObject->GetClass()->GetClassPathName().ToString()
					: FallbackClassPath;

				TSharedRef<FJsonObject> ImportedEntry = MakeShared<FJsonObject>();
				ImportedEntry->SetStringField(TEXT("source_file"), RequestedPath.IsEmpty() ? ResolvedPath : RequestedPath);
				ImportedEntry->SetStringField(TEXT("resolved_source_file"), ResolvedPath);
				ImportedEntry->SetStringField(TEXT("object_path"), ImportedObjectPath);
				ImportedEntry->SetStringField(TEXT("package"), PackageName);
				if (!ClassPath.IsEmpty())
				{
					ImportedEntry->SetStringField(TEXT("class_path"), ClassPath);
				}
				ImportedEntries.Add(MakeShared<FJsonValueObject>(ImportedEntry));
			}
		}
	}

	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("destination_path"), DestPackagePath);
	OutResult.ResultObject->SetStringField(TEXT("import_as"), ImportAs);
	OutResult.ResultObject->SetArrayField(TEXT("imported"), ImportedEntries);
	OutResult.ResultObject->SetArrayField(TEXT("failed"), ToJsonStringArray(FailedFiles));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.ResultObject->SetNumberField(TEXT("imported_count"), ImportedEntries.Num());
	OutResult.ResultObject->SetNumberField(TEXT("failed_count"), FailedFiles.Num());

	if (ImportedEntries.Num() == 0)
	{
		if (OutResult.Diagnostics.Num() == 0)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_IMPORT_FAILED;
			Diagnostic.Message = TEXT("No files were imported.");
			Diagnostic.Suggestion = TEXT("Verify source_files, import_as and destination settings.");
			OutResult.Diagnostics.Add(Diagnostic);
		}
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (FailedFiles.Num() > 0 || !bAllSaved)
	{
		OutResult.Status = EMCPResponseStatus::Partial;
		return true;
	}

	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleAssetCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString PackagePath;
	FString AssetName;
	FString AssetClassPath;
	FString FactoryClassPath;
	bool bOverwrite = false;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("package_path"), PackagePath);
		Request.Params->TryGetStringField(TEXT("asset_name"), AssetName);
		Request.Params->TryGetStringField(TEXT("asset_class_path"), AssetClassPath);
		Request.Params->TryGetStringField(TEXT("factory_class_path"), FactoryClassPath);
		Request.Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (PackagePath.IsEmpty() || AssetName.IsEmpty() || AssetClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("package_path, asset_name and asset_class_path are required for asset.create.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!IsValidAssetDestination(PackagePath, AssetName))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid package_path or asset_name for asset.create.");
		Diagnostic.Detail = FString::Printf(TEXT("package_path=%s asset_name=%s"), *PackagePath, *AssetName);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString PackageName = BuildPackageName(PackagePath, AssetName);
	const FString ObjectPath = BuildObjectPath(PackagePath, AssetName);
	UObject* ExistingObject = ResolveObjectByPath(ObjectPath);
	if (ExistingObject != nullptr && !bOverwrite)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_ALREADY_EXISTS;
		Diagnostic.Message = TEXT("Asset already exists.");
		Diagnostic.Detail = ObjectPath;
		Diagnostic.Suggestion = TEXT("Set overwrite=true or use a different destination.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (ExistingObject != nullptr && bOverwrite && !Request.Context.bDryRun)
	{
		if (!ObjectTools::DeleteSingleObject(ExistingObject, false))
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_DELETE_FAILED;
			Diagnostic.Message = TEXT("Failed to delete existing asset for overwrite.");
			Diagnostic.Detail = ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}

	UObject* CreatedAsset = nullptr;
	bool bCreatedSuccessfully = false;
	UClass* ResolvedAssetClass = ResolveClassByPath(AssetClassPath);

	if (ResolvedAssetClass == nullptr || !ResolvedAssetClass->IsChildOf(UObject::StaticClass()) || ResolvedAssetClass->HasAnyClassFlags(CLASS_Abstract))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("asset_class_path could not be resolved to a creatable UObject class.");
		Diagnostic.Detail = AssetClassPath;
		Diagnostic.Suggestion = TEXT("Provide a valid non-abstract class path.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!Request.Context.bDryRun)
	{
		UFactory* Factory = nullptr;
		if (!FactoryClassPath.IsEmpty())
		{
			UClass* FactoryClass = ResolveClassByPath(FactoryClassPath);

			if (FactoryClass == nullptr || !FactoryClass->IsChildOf(UFactory::StaticClass()))
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
				Diagnostic.Message = TEXT("factory_class_path is invalid.");
				Diagnostic.Detail = FactoryClassPath;
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
		}

		if (Factory != nullptr)
		{
			FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
			CreatedAsset = AssetToolsModule.Get().CreateAsset(AssetName, PackagePath, ResolvedAssetClass, Factory, FName(TEXT("UnrealMCP")));
			bCreatedSuccessfully = CreatedAsset != nullptr;
		}
		else
		{
			UPackage* Package = CreatePackage(*PackageName);
			if (Package != nullptr)
			{
				CreatedAsset = NewObject<UObject>(Package, ResolvedAssetClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
				if (CreatedAsset != nullptr)
				{
					FAssetRegistryModule::AssetCreated(CreatedAsset);
					CreatedAsset->MarkPackageDirty();
					Package->MarkPackageDirty();
					CreatedAsset->PostEditChange();
					bCreatedSuccessfully = true;
				}
			}
		}
	}
	else
	{
		bCreatedSuccessfully = true;
	}

	if (!bCreatedSuccessfully && CreatedAsset == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("Failed to create asset.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.TouchedPackages.AddUnique(PackageName);

	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		bAllSaved = SavePackageByName(PackageName, OutResult);
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("created"), true);
	OutResult.ResultObject->SetStringField(TEXT("object_path"), ObjectPath);
	OutResult.ResultObject->SetStringField(TEXT("class_path"), ResolvedAssetClass->GetClassPathName().ToString());
	OutResult.ResultObject->SetStringField(TEXT("package"), PackageName);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleBlueprintClassCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString PackagePath;
	FString AssetName;
	FString ParentClassPath;
	bool bOverwrite = false;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("package_path"), PackagePath);
		Request.Params->TryGetStringField(TEXT("asset_name"), AssetName);
		Request.Params->TryGetStringField(TEXT("parent_class_path"), ParentClassPath);
		Request.Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	AssetName = ObjectTools::SanitizeObjectName(AssetName);
	if (PackagePath.IsEmpty() || AssetName.IsEmpty() || ParentClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("package_path, asset_name and parent_class_path are required for blueprint.class.create.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!IsValidAssetDestination(PackagePath, AssetName))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid package_path or asset_name for blueprint.class.create.");
		Diagnostic.Detail = FString::Printf(TEXT("package_path=%s asset_name=%s"), *PackagePath, *AssetName);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* ParentClass = ResolveClassByPath(ParentClassPath);
	if (ParentClass == nullptr || !ParentClass->IsChildOf(UObject::StaticClass()))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("parent_class_path could not be resolved to a UObject class.");
		Diagnostic.Detail = ParentClassPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("Blueprint cannot be created from the requested parent class.");
		Diagnostic.Detail = ParentClass->GetPathName();
		Diagnostic.Suggestion = TEXT("Use a Blueprintable parent class.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPBlueprintCreateOptions CreateOptions;
	CreateOptions.PackagePath = PackagePath;
	CreateOptions.AssetName = AssetName;
	CreateOptions.ParentClass = ParentClass;
	CreateOptions.bOverwrite = bOverwrite;
	CreateOptions.bDryRun = Request.Context.bDryRun;
	CreateOptions.bCompileOnSuccess = bCompileOnSuccess;
	CreateOptions.bAutoSave = bAutoSave;
	CreateOptions.TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP Blueprint Class Create"));

	FMCPBlueprintCreateResult CreateResult;
	FMCPDiagnostic CreateDiagnostic;
	if (!EnsureBlueprintClassAsset(CreateOptions, OutResult, CreateResult, CreateDiagnostic))
	{
		OutResult.Diagnostics.Add(CreateDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* GeneratedClass = ResolveClassByPath(CreateResult.GeneratedClassPath);
	const FString GeneratedClassPath = GeneratedClass != nullptr
		? GeneratedClass->GetPathName()
		: CreateResult.GeneratedClassPath;

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("created"), CreateResult.bCreatedNew);
	OutResult.ResultObject->SetBoolField(TEXT("reused"), !CreateResult.bCreatedNew);
	OutResult.ResultObject->SetStringField(TEXT("object_path"), CreateResult.ObjectPath);
	OutResult.ResultObject->SetStringField(TEXT("generated_class_path"), GeneratedClassPath);
	OutResult.ResultObject->SetStringField(TEXT("parent_class_path"), CreateResult.ParentClassPath);
	OutResult.ResultObject->SetStringField(TEXT("package"), CreateResult.PackageName);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = CreateResult.bSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeCompose(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString PackagePath;
	FString GameModeAssetName;
	FString GameModeParentClassPath = TEXT("/Script/Engine.GameModeBase");
	FString MapKey;
	bool bCreateMissingClasses = true;
	bool bSetAsDefault = false;
	bool bOverwrite = false;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	FMCPSettingsSaveOptions SettingsSaveOptions;
	const TSharedPtr<FJsonObject>* ClassPathsObject = nullptr;
	const TSharedPtr<FJsonObject>* ClassNamesObject = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("package_path"), PackagePath);
		Request.Params->TryGetStringField(TEXT("game_mode_asset_name"), GameModeAssetName);
		Request.Params->TryGetStringField(TEXT("game_mode_parent_class_path"), GameModeParentClassPath);
		Request.Params->TryGetStringField(TEXT("map_key"), MapKey);
		Request.Params->TryGetBoolField(TEXT("create_missing_classes"), bCreateMissingClasses);
		Request.Params->TryGetBoolField(TEXT("set_as_default"), bSetAsDefault);
		Request.Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		Request.Params->TryGetObjectField(TEXT("class_paths"), ClassPathsObject);
		Request.Params->TryGetObjectField(TEXT("class_names"), ClassNamesObject);
		ParseAutoSaveOption(Request.Params, bAutoSave);
		ParseSettingsSaveOptions(Request.Params, SettingsSaveOptions);
	}

	GameModeAssetName = ObjectTools::SanitizeObjectName(GameModeAssetName);
	MapKey.TrimStartAndEndInline();

	if (PackagePath.IsEmpty() || GameModeAssetName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("package_path and game_mode_asset_name are required for settings.gamemode.compose.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!IsValidAssetDestination(PackagePath, GameModeAssetName))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid package_path or game_mode_asset_name for settings.gamemode.compose.");
		Diagnostic.Detail = FString::Printf(TEXT("package_path=%s game_mode_asset_name=%s"), *PackagePath, *GameModeAssetName);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* GameModeParentClass = ResolveClassByPath(GameModeParentClassPath);
	if (GameModeParentClass == nullptr || !GameModeParentClass->IsChildOf(AGameModeBase::StaticClass()))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::GAMEMODE_CLASS_INVALID;
		Diagnostic.Message = TEXT("game_mode_parent_class_path is invalid.");
		Diagnostic.Detail = GameModeParentClassPath;
		Diagnostic.Suggestion = TEXT("Provide an AGameModeBase subclass path.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(GameModeParentClass))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("Blueprint cannot be created from game_mode_parent_class_path.");
		Diagnostic.Detail = GameModeParentClass->GetPathName();
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP GameMode Compose"));
	FMCPBlueprintCreateOptions GameModeCreateOptions;
	GameModeCreateOptions.PackagePath = PackagePath;
	GameModeCreateOptions.AssetName = GameModeAssetName;
	GameModeCreateOptions.ParentClass = GameModeParentClass;
	GameModeCreateOptions.bOverwrite = bOverwrite;
	GameModeCreateOptions.bDryRun = Request.Context.bDryRun;
	GameModeCreateOptions.bCompileOnSuccess = false;
	GameModeCreateOptions.bAutoSave = false;
	GameModeCreateOptions.TransactionLabel = TransactionLabel;

	FMCPBlueprintCreateResult GameModeCreateResult;
	FMCPDiagnostic GameModeCreateDiagnostic;
	if (!EnsureBlueprintClassAsset(GameModeCreateOptions, OutResult, GameModeCreateResult, GameModeCreateDiagnostic))
	{
		OutResult.Diagnostics.Add(GameModeCreateDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	struct FGameModeSlotSpec
	{
		FString JsonKey;
		FString PropertyName;
		FString DefaultNameSuffix;
		UClass* RequiredBaseClass = nullptr;
	};

	struct FGameModeSlotState
	{
		FString JsonKey;
		FString PropertyName;
		FString ClassPath;
		FString ObjectPath;
		FString PackageName;
		FString Source = TEXT("none");
		UClass* ResolvedClass = nullptr;
		bool bCreated = false;
		bool bApplied = false;
	};

	const TArray<FGameModeSlotSpec> SlotSpecs{
		{ TEXT("default_pawn_class"), TEXT("DefaultPawnClass"), TEXT("Pawn"), APawn::StaticClass() },
		{ TEXT("hud_class"), TEXT("HUDClass"), TEXT("HUD"), AHUD::StaticClass() },
		{ TEXT("player_controller_class"), TEXT("PlayerControllerClass"), TEXT("PlayerController"), APlayerController::StaticClass() },
		{ TEXT("game_state_class"), TEXT("GameStateClass"), TEXT("GameState"), AGameStateBase::StaticClass() },
		{ TEXT("player_state_class"), TEXT("PlayerStateClass"), TEXT("PlayerState"), APlayerState::StaticClass() },
		{ TEXT("spectator_class"), TEXT("SpectatorClass"), TEXT("SpectatorPawn"), ASpectatorPawn::StaticClass() }
	};

	UClass* GameModeGeneratedClass = ResolveClassByPath(GameModeCreateResult.GeneratedClassPath);
	if (!Request.Context.bDryRun && GameModeGeneratedClass == nullptr && GameModeCreateResult.BlueprintAsset != nullptr)
	{
		FKismetEditorUtilities::CompileBlueprint(GameModeCreateResult.BlueprintAsset);
		GameModeGeneratedClass = GameModeCreateResult.BlueprintAsset->GeneratedClass;
	}

	UObject* GameModeCDO = (GameModeGeneratedClass != nullptr) ? GameModeGeneratedClass->GetDefaultObject() : nullptr;
	TArray<FString> ChangedProperties;
	TArray<FGameModeSlotState> SlotStates;
	SlotStates.Reserve(SlotSpecs.Num());

	auto ResolveSlotClassFromCDO = [&](const FString& PropertyName, UClass* RequiredBaseClass, FGameModeSlotState& InOutSlotState) -> void
	{
		if (GameModeCDO == nullptr)
		{
			return;
		}

		FClassProperty* ClassProperty = FindFProperty<FClassProperty>(GameModeCDO->GetClass(), FName(*PropertyName));
		if (ClassProperty == nullptr)
		{
			return;
		}

		UClass* CurrentClass = Cast<UClass>(ClassProperty->GetObjectPropertyValue_InContainer(GameModeCDO));
		if (CurrentClass == nullptr || (RequiredBaseClass != nullptr && !CurrentClass->IsChildOf(RequiredBaseClass)))
		{
			return;
		}

		InOutSlotState.Source = TEXT("existing");
		InOutSlotState.ResolvedClass = CurrentClass;
		InOutSlotState.ClassPath = CurrentClass->GetPathName();
	};

	for (const FGameModeSlotSpec& SlotSpec : SlotSpecs)
	{
		FGameModeSlotState SlotState;
		SlotState.JsonKey = SlotSpec.JsonKey;
		SlotState.PropertyName = SlotSpec.PropertyName;

		FString RequestedClassPath;
		if (ClassPathsObject != nullptr && ClassPathsObject->IsValid())
		{
			(*ClassPathsObject)->TryGetStringField(SlotSpec.JsonKey, RequestedClassPath);
		}
		RequestedClassPath.TrimStartAndEndInline();

		if (!RequestedClassPath.IsEmpty())
		{
			FMCPDiagnostic ResolveDiagnostic;
			if (!ResolveClassPathWithBase(
				RequestedClassPath,
				SlotSpec.RequiredBaseClass,
				SlotState.ClassPath,
				SlotState.ResolvedClass,
				ResolveDiagnostic))
			{
				OutResult.Diagnostics.Add(ResolveDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			SlotState.Source = TEXT("provided");
		}
		else if (bCreateMissingClasses)
		{
			FString SlotAssetName;
			if (ClassNamesObject != nullptr && ClassNamesObject->IsValid())
			{
				(*ClassNamesObject)->TryGetStringField(SlotSpec.JsonKey, SlotAssetName);
			}

			SlotAssetName = ObjectTools::SanitizeObjectName(SlotAssetName);
			if (SlotAssetName.IsEmpty())
			{
				SlotAssetName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("%s_%s"), *GameModeAssetName, *SlotSpec.DefaultNameSuffix));
			}

			if (SlotAssetName.IsEmpty())
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
				Diagnostic.Message = TEXT("Failed to resolve a valid asset name for compose slot.");
				Diagnostic.Detail = SlotSpec.JsonKey;
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			FMCPBlueprintCreateOptions SlotCreateOptions;
			SlotCreateOptions.PackagePath = PackagePath;
			SlotCreateOptions.AssetName = SlotAssetName;
			SlotCreateOptions.ParentClass = SlotSpec.RequiredBaseClass;
			SlotCreateOptions.bOverwrite = bOverwrite;
			SlotCreateOptions.bDryRun = Request.Context.bDryRun;
			SlotCreateOptions.bCompileOnSuccess = bCompileOnSuccess;
			SlotCreateOptions.bAutoSave = false;
			SlotCreateOptions.TransactionLabel = FString::Printf(TEXT("%s - %s"), *TransactionLabel, *SlotSpec.JsonKey);

			FMCPBlueprintCreateResult SlotCreateResult;
			FMCPDiagnostic SlotCreateDiagnostic;
			if (!EnsureBlueprintClassAsset(SlotCreateOptions, OutResult, SlotCreateResult, SlotCreateDiagnostic))
			{
				OutResult.Diagnostics.Add(SlotCreateDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			SlotState.Source = SlotCreateResult.bCreatedNew ? TEXT("created") : TEXT("reused");
			SlotState.bCreated = SlotCreateResult.bCreatedNew;
			SlotState.ObjectPath = SlotCreateResult.ObjectPath;
			SlotState.PackageName = SlotCreateResult.PackageName;
			SlotState.ClassPath = SlotCreateResult.GeneratedClassPath;

			FMCPDiagnostic ResolveDiagnostic;
			FString ResolvedSlotClassPath;
			UClass* ResolvedSlotClass = nullptr;
			if (!ResolveClassPathWithBase(
				SlotState.ClassPath,
				SlotSpec.RequiredBaseClass,
				ResolvedSlotClassPath,
				ResolvedSlotClass,
				ResolveDiagnostic))
			{
				if (!Request.Context.bDryRun)
				{
					OutResult.Diagnostics.Add(ResolveDiagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}

				SlotState.ResolvedClass = nullptr;
			}
			else
			{
				SlotState.ClassPath = ResolvedSlotClassPath;
				SlotState.ResolvedClass = ResolvedSlotClass;
			}
		}
		else
		{
			ResolveSlotClassFromCDO(SlotSpec.PropertyName, SlotSpec.RequiredBaseClass, SlotState);
			if (SlotState.Source.Equals(TEXT("none"), ESearchCase::CaseSensitive))
			{
				SlotState.Source = TEXT("unchanged");
			}
		}

		SlotStates.Add(MoveTemp(SlotState));
	}

	if (!Request.Context.bDryRun)
	{
		if (GameModeCreateResult.BlueprintAsset == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			Diagnostic.Message = TEXT("GameMode blueprint asset could not be resolved.");
			Diagnostic.Detail = GameModeCreateResult.ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		if (GameModeGeneratedClass == nullptr)
		{
			GameModeGeneratedClass = GameModeCreateResult.BlueprintAsset->GeneratedClass;
		}
		if (GameModeGeneratedClass == nullptr)
		{
			FKismetEditorUtilities::CompileBlueprint(GameModeCreateResult.BlueprintAsset);
			GameModeGeneratedClass = GameModeCreateResult.BlueprintAsset->GeneratedClass;
		}
		if (GameModeGeneratedClass == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			Diagnostic.Message = TEXT("Failed to resolve generated class for GameMode blueprint.");
			Diagnostic.Detail = GameModeCreateResult.ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		GameModeCDO = GameModeGeneratedClass->GetDefaultObject();
		if (GameModeCDO == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			Diagnostic.Message = TEXT("Failed to resolve default object for composed GameMode.");
			Diagnostic.Detail = GameModeGeneratedClass->GetPathName();
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		GameModeCreateResult.BlueprintAsset->Modify();
		for (FGameModeSlotState& SlotState : SlotStates)
		{
			if (SlotState.ResolvedClass == nullptr)
			{
				continue;
			}

			FMCPDiagnostic SetPropertyDiagnostic;
			if (!SetClassPropertyOnObject(GameModeCDO, SlotState.PropertyName, SlotState.ResolvedClass, SetPropertyDiagnostic))
			{
				OutResult.Diagnostics.Add(SetPropertyDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			SlotState.bApplied = true;
			ChangedProperties.AddUnique(SlotState.PropertyName);
		}

		GameModeCDO->MarkPackageDirty();
		GameModeCreateResult.BlueprintAsset->MarkPackageDirty();

		if (bCompileOnSuccess)
		{
			FKismetEditorUtilities::CompileBlueprint(GameModeCreateResult.BlueprintAsset);
		}

		if (GameModeCreateResult.BlueprintAsset->GeneratedClass != nullptr)
		{
			GameModeGeneratedClass = GameModeCreateResult.BlueprintAsset->GeneratedClass;
		}
	}

	const FString GameModeGeneratedClassPath = GameModeGeneratedClass != nullptr
		? GameModeGeneratedClass->GetPathName()
		: GameModeCreateResult.GeneratedClassPath;

	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			if (!PackageName.StartsWith(TEXT("/Game"), ESearchCase::CaseSensitive))
			{
				continue;
			}
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	auto CreateSettingsRequest = [&](const FString& ToolName) -> FMCPRequestEnvelope
	{
		FMCPRequestEnvelope SettingsRequest = Request;
		SettingsRequest.Tool = ToolName;
		SettingsRequest.Params = MakeShared<FJsonObject>();

		TSharedRef<FJsonObject> SaveObject = MakeShared<FJsonObject>();
		SaveObject->SetBoolField(TEXT("save_config"), SettingsSaveOptions.bSaveConfig);
		SaveObject->SetBoolField(TEXT("flush_ini"), SettingsSaveOptions.bFlushIni);
		SaveObject->SetBoolField(TEXT("reload_verify"), SettingsSaveOptions.bReloadVerify);
		SettingsRequest.Params->SetObjectField(TEXT("save"), SaveObject);

		TSharedRef<FJsonObject> TransactionObject = MakeShared<FJsonObject>();
		TransactionObject->SetStringField(TEXT("label"), TransactionLabel);
		SettingsRequest.Params->SetObjectField(TEXT("transaction"), TransactionObject);
		return SettingsRequest;
	};

	auto MergeToolResult = [&](const FMCPToolExecutionResult& ToolResult, bool& bOutVerified, TArray<FString>& OutSavedConfigFiles, bool& bOutHadPartialStatus) -> void
	{
		for (const FMCPDiagnostic& Diagnostic : ToolResult.Diagnostics)
		{
			OutResult.Diagnostics.Add(Diagnostic);
		}
		for (const FString& TouchedPackage : ToolResult.TouchedPackages)
		{
			OutResult.TouchedPackages.AddUnique(TouchedPackage);
		}

		if (ToolResult.ResultObject.IsValid())
		{
			TArray<FString> SavedFiles;
			ParseStringArrayField(ToolResult.ResultObject, TEXT("saved_config_files"), SavedFiles);
			for (const FString& SavedFile : SavedFiles)
			{
				OutSavedConfigFiles.AddUnique(SavedFile);
			}

			TArray<FString> ToolChangedProperties;
			ParseStringArrayField(ToolResult.ResultObject, TEXT("changed_properties"), ToolChangedProperties);
			for (const FString& PropertyName : ToolChangedProperties)
			{
				ChangedProperties.AddUnique(PropertyName);
			}

			bool bToolVerified = true;
			if (ToolResult.ResultObject->TryGetBoolField(TEXT("verified"), bToolVerified))
			{
				bOutVerified &= bToolVerified;
			}
		}

		bOutHadPartialStatus |= ToolResult.Status == EMCPResponseStatus::Partial;
	};

	TArray<FString> SavedConfigFiles;
	bool bVerified = true;
	bool bSettingsSetDefaultApplied = false;
	bool bSettingsMapOverrideApplied = false;
	bool bHadPartialStatus = !bAllSaved;

	if (bSetAsDefault)
	{
		FMCPRequestEnvelope SettingsRequest = CreateSettingsRequest(TEXT("settings.gamemode.set_default"));
		SettingsRequest.Params->SetStringField(TEXT("game_mode_class_path"), GameModeGeneratedClassPath);

		FMCPToolExecutionResult SettingsResult;
		if (!HandleSettingsGameModeSetDefault(SettingsRequest, SettingsResult))
		{
			MergeToolResult(SettingsResult, bVerified, SavedConfigFiles, bHadPartialStatus);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		MergeToolResult(SettingsResult, bVerified, SavedConfigFiles, bHadPartialStatus);
		bSettingsSetDefaultApplied = true;
	}

	if (!MapKey.IsEmpty())
	{
		FMCPRequestEnvelope SettingsRequest = CreateSettingsRequest(TEXT("settings.gamemode.set_map_override"));
		SettingsRequest.Params->SetStringField(TEXT("map_key"), MapKey);
		SettingsRequest.Params->SetStringField(TEXT("game_mode_class_path"), GameModeGeneratedClassPath);

		FMCPToolExecutionResult SettingsResult;
		if (!HandleSettingsGameModeSetMapOverride(SettingsRequest, SettingsResult))
		{
			MergeToolResult(SettingsResult, bVerified, SavedConfigFiles, bHadPartialStatus);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		MergeToolResult(SettingsResult, bVerified, SavedConfigFiles, bHadPartialStatus);
		bSettingsMapOverrideApplied = true;
	}

	TArray<TSharedPtr<FJsonValue>> SlotEntries;
	SlotEntries.Reserve(SlotStates.Num());
	for (const FGameModeSlotState& SlotState : SlotStates)
	{
		TSharedRef<FJsonObject> SlotObject = MakeShared<FJsonObject>();
		SlotObject->SetStringField(TEXT("slot"), SlotState.JsonKey);
		SlotObject->SetStringField(TEXT("property"), SlotState.PropertyName);
		SlotObject->SetStringField(TEXT("class_path"), SlotState.ClassPath);
		SlotObject->SetStringField(TEXT("source"), SlotState.Source);
		SlotObject->SetBoolField(TEXT("created"), SlotState.bCreated);
		SlotObject->SetBoolField(TEXT("applied"), SlotState.bApplied);
		if (!SlotState.ObjectPath.IsEmpty())
		{
			SlotObject->SetStringField(TEXT("object_path"), SlotState.ObjectPath);
		}
		if (!SlotState.PackageName.IsEmpty())
		{
			SlotObject->SetStringField(TEXT("package"), SlotState.PackageName);
		}
		SlotEntries.Add(MakeShared<FJsonValueObject>(SlotObject));
	}

	TSharedRef<FJsonObject> SettingsAppliedObject = MakeShared<FJsonObject>();
	SettingsAppliedObject->SetBoolField(TEXT("set_default"), bSettingsSetDefaultApplied);
	SettingsAppliedObject->SetBoolField(TEXT("map_override"), bSettingsMapOverrideApplied);
	SettingsAppliedObject->SetStringField(TEXT("map_key"), MapKey);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("game_mode_object_path"), GameModeCreateResult.ObjectPath);
	OutResult.ResultObject->SetStringField(TEXT("game_mode_class_path"), GameModeGeneratedClassPath);
	OutResult.ResultObject->SetStringField(TEXT("game_mode_parent_class_path"), GameModeParentClass->GetPathName());
	OutResult.ResultObject->SetArrayField(TEXT("slots"), SlotEntries);
	OutResult.ResultObject->SetObjectField(TEXT("settings_applied"), SettingsAppliedObject);
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bHadPartialStatus ? EMCPResponseStatus::Partial : EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleAssetDuplicate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString SourceObjectPath;
	FString DestPackagePath;
	FString DestAssetName;
	bool bOverwrite = false;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("source_object_path"), SourceObjectPath);
		Request.Params->TryGetStringField(TEXT("dest_package_path"), DestPackagePath);
		Request.Params->TryGetStringField(TEXT("dest_asset_name"), DestAssetName);
		Request.Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (SourceObjectPath.IsEmpty() || DestPackagePath.IsEmpty() || DestAssetName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("source_object_path, dest_package_path and dest_asset_name are required for asset.duplicate.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!IsValidAssetDestination(DestPackagePath, DestAssetName))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid destination path for asset.duplicate.");
		Diagnostic.Detail = FString::Printf(TEXT("dest_package_path=%s dest_asset_name=%s"), *DestPackagePath, *DestAssetName);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* SourceObject = ResolveObjectByPath(SourceObjectPath);
	if (SourceObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_NOT_FOUND;
		Diagnostic.Message = TEXT("source_object_path was not found.");
		Diagnostic.Detail = SourceObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString DestPackageName = BuildPackageName(DestPackagePath, DestAssetName);
	const FString DestObjectPath = BuildObjectPath(DestPackagePath, DestAssetName);

	UObject* ExistingObject = ResolveObjectByPath(DestObjectPath);
	if (ExistingObject != nullptr && !bOverwrite)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_ALREADY_EXISTS;
		Diagnostic.Message = TEXT("Destination asset already exists.");
		Diagnostic.Detail = DestObjectPath;
		Diagnostic.Suggestion = TEXT("Set overwrite=true or change destination.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (ExistingObject != nullptr && bOverwrite && !Request.Context.bDryRun)
	{
		if (!ObjectTools::DeleteSingleObject(ExistingObject, false))
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_DELETE_FAILED;
			Diagnostic.Message = TEXT("Failed to delete existing destination asset for overwrite.");
			Diagnostic.Detail = DestObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}

	UObject* DuplicatedAsset = nullptr;
	bool bDuplicatedSuccessfully = false;
	if (!Request.Context.bDryRun)
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		DuplicatedAsset = AssetToolsModule.Get().DuplicateAsset(DestAssetName, DestPackagePath, SourceObject);
		bDuplicatedSuccessfully = DuplicatedAsset != nullptr;
	}
	else
	{
		bDuplicatedSuccessfully = true;
	}

	if (!bDuplicatedSuccessfully)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_DUPLICATE_FAILED;
		Diagnostic.Message = TEXT("Failed to duplicate asset.");
		Diagnostic.Detail = FString::Printf(TEXT("source=%s destination=%s"), *SourceObjectPath, *DestObjectPath);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.TouchedPackages.AddUnique(DestPackageName);

	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		bAllSaved = SavePackageByName(DestPackageName, OutResult);
	}

	const FString ClassPath = SourceObject->GetClass() ? SourceObject->GetClass()->GetClassPathName().ToString() : FString();
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("duplicated"), true);
	OutResult.ResultObject->SetStringField(TEXT("object_path"), DestObjectPath);
	OutResult.ResultObject->SetStringField(TEXT("class_path"), ClassPath);
	OutResult.ResultObject->SetStringField(TEXT("package"), DestPackageName);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleAssetRename(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	FString NewPackagePath;
	FString NewAssetName;
	bool bFixupRedirectors = false;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("new_package_path"), NewPackagePath);
		Request.Params->TryGetStringField(TEXT("new_asset_name"), NewAssetName);
		Request.Params->TryGetBoolField(TEXT("fixup_redirectors"), bFixupRedirectors);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || NewPackagePath.IsEmpty() || NewAssetName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, new_package_path and new_asset_name are required for asset.rename.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!IsValidAssetDestination(NewPackagePath, NewAssetName))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid destination path for asset.rename.");
		Diagnostic.Detail = FString::Printf(TEXT("new_package_path=%s new_asset_name=%s"), *NewPackagePath, *NewAssetName);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* AssetObject = ResolveObjectByPath(ObjectPath);
	if (AssetObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_NOT_FOUND;
		Diagnostic.Message = TEXT("object_path was not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString OldObjectPath = AssetObject->GetPathName();
	const FString OldPackageName = NormalizePackageNameFromInput(OldObjectPath);
	const FString NewObjectPath = BuildObjectPath(NewPackagePath, NewAssetName);
	const FString NewPackageName = BuildPackageName(NewPackagePath, NewAssetName);

	bool bRenameSucceeded = true;
	bool bRedirectorsFixed = false;
	if (!Request.Context.bDryRun)
	{
		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		IAssetTools& AssetTools = AssetToolsModule.Get();

		TArray<FAssetRenameData> RenameData;
		RenameData.Add(FAssetRenameData(AssetObject, NewPackagePath, NewAssetName));
		bRenameSucceeded = AssetTools.RenameAssets(RenameData);
		if (!bRenameSucceeded)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_RENAME_FAILED;
			Diagnostic.Message = TEXT("Failed to rename asset.");
			Diagnostic.Detail = FString::Printf(TEXT("object_path=%s new_object_path=%s"), *ObjectPath, *NewObjectPath);
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		if (bFixupRedirectors && !OldPackageName.IsEmpty())
		{
			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			TArray<FAssetData> OldPackageAssets;
			AssetRegistryModule.Get().GetAssetsByPackageName(*OldPackageName, OldPackageAssets, false);

			TArray<UObjectRedirector*> Redirectors;
			for (const FAssetData& AssetData : OldPackageAssets)
			{
				if (AssetData.AssetClassPath != UObjectRedirector::StaticClass()->GetClassPathName())
				{
					continue;
				}

				if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(AssetData.GetAsset()))
				{
					Redirectors.Add(Redirector);
				}
			}

			if (Redirectors.Num() > 0)
			{
				AssetTools.FixupReferencers(Redirectors, false, ERedirectFixupMode::DeleteFixedUpRedirectors);
				bRedirectorsFixed = true;
			}
		}
	}

	OutResult.TouchedPackages.AddUnique(OldPackageName);
	OutResult.TouchedPackages.AddUnique(NewPackageName);

	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		if (!OldPackageName.IsEmpty())
		{
			UPackage* OldPackage = FindPackage(nullptr, *OldPackageName);
			if (OldPackage != nullptr)
			{
				bAllSaved &= SavePackageByName(OldPackageName, OutResult);
			}
		}
		bAllSaved &= SavePackageByName(NewPackageName, OutResult);
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("renamed"), bRenameSucceeded);
	OutResult.ResultObject->SetStringField(TEXT("old_object_path"), OldObjectPath);
	OutResult.ResultObject->SetStringField(TEXT("new_object_path"), NewObjectPath);
	OutResult.ResultObject->SetBoolField(TEXT("redirectors_fixed"), bRedirectorsFixed);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleAssetDelete(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	TArray<FString> ObjectPaths;
	bool bFailIfReferenced = true;
	FString Mode = TEXT("preview");
	FString ConfirmToken;

	if (Request.Params.IsValid())
	{
		ParseStringArrayField(Request.Params, TEXT("object_paths"), ObjectPaths);
		Request.Params->TryGetBoolField(TEXT("fail_if_referenced"), bFailIfReferenced);
		Request.Params->TryGetStringField(TEXT("mode"), Mode);
		Request.Params->TryGetStringField(TEXT("confirm_token"), ConfirmToken);
	}

	if (ObjectPaths.Num() == 0)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_paths is required for asset.delete.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	Mode = Mode.ToLower();
	if (!Mode.Equals(TEXT("preview")) && !Mode.Equals(TEXT("apply")))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("mode must be preview or apply.");
		Diagnostic.Detail = Mode;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (Mode.Equals(TEXT("apply")) && !ConsumeDeleteConfirmationToken(ConfirmToken, ObjectPaths, bFailIfReferenced))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::CONFIRM_TOKEN_INVALID;
		Diagnostic.Message = TEXT("Invalid or expired confirm_token.");
		Diagnostic.Suggestion = TEXT("Call asset.delete in preview mode and retry with returned confirm_token.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<UObject*> ObjectsToDelete;
	TArray<FString> DeleteCandidatePaths;
	TArray<FString> Failed;
	TArray<FString> Deleted;
	TArray<TSharedPtr<FJsonValue>> Blocked;

	for (const FString& ObjectPath : ObjectPaths)
	{
		UObject* AssetObject = ResolveObjectByPath(ObjectPath);
		if (AssetObject == nullptr)
		{
			Failed.AddUnique(ObjectPath);
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_NOT_FOUND;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Asset was not found for delete request.");
			Diagnostic.Detail = ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		const FString PackageName = NormalizePackageNameFromInput(ObjectPath);
		TArray<FName> Referencers;
		if (bFailIfReferenced && !PackageName.IsEmpty())
		{
			AssetRegistry.GetReferencers(*PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);
		}

		TArray<FString> ReferencerPackages;
		for (const FName Referencer : Referencers)
		{
			const FString ReferencerName = Referencer.ToString();
			if (!ReferencerName.IsEmpty() && !ReferencerName.Equals(PackageName, ESearchCase::CaseSensitive))
			{
				ReferencerPackages.AddUnique(ReferencerName);
			}
		}

		if (bFailIfReferenced && ReferencerPackages.Num() > 0)
		{
			Failed.AddUnique(ObjectPath);

			TSharedRef<FJsonObject> BlockedEntry = MakeShared<FJsonObject>();
			BlockedEntry->SetStringField(TEXT("object_path"), ObjectPath);
			BlockedEntry->SetArrayField(TEXT("referencers"), ToJsonStringArray(ReferencerPackages));
			Blocked.Add(MakeShared<FJsonValueObject>(BlockedEntry));

			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::ASSET_REFERENCED;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Asset has referencers and cannot be deleted with fail_if_referenced=true.");
			Diagnostic.Detail = ObjectPath;
			Diagnostic.Suggestion = TEXT("Inspect referencers or set fail_if_referenced=false.");
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		ObjectsToDelete.Add(AssetObject);
		DeleteCandidatePaths.Add(ObjectPath);
	}

	const bool bPreviewMode = Mode.Equals(TEXT("preview"));
	const bool bCanDelete = Failed.Num() == 0 && Blocked.Num() == 0;
	FString PreviewToken;

	if (bPreviewMode)
	{
		PreviewToken = BuildDeleteConfirmationToken(ObjectPaths, bFailIfReferenced);
	}
	else if (Request.Context.bDryRun)
	{
		Deleted.Append(DeleteCandidatePaths);
	}
	else if (ObjectsToDelete.Num() > 0)
	{
		ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

		auto WasDeletedFromRegistryAndDisk = [&AssetRegistry](const FString& CandidatePath, FString& OutFailureDetail) -> bool
		{
			OutFailureDetail.Reset();
			const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(CandidatePath), true);
			if (AssetData.IsValid())
			{
				OutFailureDetail = FString::Printf(TEXT("asset_registry_has_object=%s"), *CandidatePath);
				return false;
			}

			const FString PackageName = NormalizePackageNameFromInput(CandidatePath);
			if (PackageName.IsEmpty())
			{
				return true;
			}

			FString AssetFilename;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, AssetFilename, FPackageName::GetAssetPackageExtension()) &&
				IFileManager::Get().FileExists(*AssetFilename))
			{
				OutFailureDetail = FString::Printf(TEXT("asset_file_exists=%s"), *AssetFilename);
				return false;
			}

			FString MapFilename;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, MapFilename, FPackageName::GetMapPackageExtension()) &&
				IFileManager::Get().FileExists(*MapFilename))
			{
				OutFailureDetail = FString::Printf(TEXT("map_file_exists=%s"), *MapFilename);
				return false;
			}

			return true;
		};

		for (const FString& CandidatePath : DeleteCandidatePaths)
		{
			FString FailureDetail;
			if (WasDeletedFromRegistryAndDisk(CandidatePath, FailureDetail))
			{
				Deleted.AddUnique(CandidatePath);
			}
			else
			{
				Failed.AddUnique(CandidatePath);

				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::ASSET_DELETE_FAILED;
				Diagnostic.Severity = TEXT("warning");
				Diagnostic.Message = TEXT("Asset delete apply could not verify removal.");
				Diagnostic.Detail = FailureDetail.IsEmpty()
					? CandidatePath
					: FString::Printf(TEXT("%s %s"), *CandidatePath, *FailureDetail);
				Diagnostic.Suggestion = TEXT("Close editors referencing the asset and retry, or delete fewer assets per request.");
				Diagnostic.bRetriable = true;
				OutResult.Diagnostics.Add(Diagnostic);
			}
		}
	}

	for (const FString& DeletedPath : Deleted)
	{
		const FString TouchedPackage = NormalizePackageNameFromInput(DeletedPath);
		if (!TouchedPackage.IsEmpty())
		{
			OutResult.TouchedPackages.AddUnique(TouchedPackage);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("mode"), Mode);
	OutResult.ResultObject->SetBoolField(TEXT("can_delete"), bCanDelete);
	OutResult.ResultObject->SetArrayField(TEXT("deleted"), ToJsonStringArray(Deleted));
	OutResult.ResultObject->SetArrayField(TEXT("failed"), ToJsonStringArray(Failed));
	OutResult.ResultObject->SetArrayField(TEXT("blocked"), Blocked);
	OutResult.ResultObject->SetStringField(TEXT("confirm_token"), PreviewToken);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));

	if (bPreviewMode)
	{
		OutResult.Status = bCanDelete ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
		return true;
	}

	if (Failed.Num() == 0)
	{
		OutResult.Status = EMCPResponseStatus::Ok;
		return true;
	}

	if (Deleted.Num() > 0)
	{
		OutResult.Status = EMCPResponseStatus::Partial;
		return true;
	}

	FMCPDiagnostic Diagnostic;
	Diagnostic.Code = MCPErrorCodes::ASSET_DELETE_FAILED;
	Diagnostic.Message = TEXT("No assets were deleted in apply mode.");
	Diagnostic.Detail = FString::Join(Failed, TEXT(","));
	Diagnostic.Suggestion = TEXT("Inspect failed targets and retry after closing referencing editors.");
	Diagnostic.bRetriable = true;
	OutResult.Diagnostics.Add(Diagnostic);

	OutResult.Status = EMCPResponseStatus::Error;
	return false;
}

bool UMCPToolRegistrySubsystem::HandleSettingsProjectGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ClassPath;
	const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
	int32 Depth = 2;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("class_path"), ClassPath);
		Request.Params->TryGetObjectField(TEXT("filters"), FiltersObject);
		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 5);
	}

	if (ClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("class_path is required for settings.project.get.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* SettingsClass = nullptr;
	UObject* ConfigObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!ResolveSettingsClass(ClassPath, SettingsClass, ConfigObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Properties;
	MCPObjectUtils::InspectObject(ConfigObject, (FiltersObject != nullptr) ? *FiltersObject : nullptr, Depth, Properties);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("class_path"), SettingsClass->GetClassPathName().ToString());
	OutResult.ResultObject->SetStringField(TEXT("config_name"), SettingsClass->ClassConfigName.ToString());
	OutResult.ResultObject->SetStringField(TEXT("config_file"), ConfigObject->GetDefaultConfigFilename());
	OutResult.ResultObject->SetArrayField(TEXT("properties"), Properties);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleSettingsProjectPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ClassPath;
	FString Mode = TEXT("preview");
	FString ConfirmToken;
	FMCPSettingsSaveOptions SaveOptions;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("class_path"), ClassPath);
		Request.Params->TryGetStringField(TEXT("mode"), Mode);
		Request.Params->TryGetStringField(TEXT("confirm_token"), ConfirmToken);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
		ParseSettingsSaveOptions(Request.Params, SaveOptions);
	}

	if (ClassPath.IsEmpty() || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("class_path and patch are required for settings.project.patch.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	Mode = Mode.ToLower();
	if (!Mode.Equals(TEXT("preview")) && !Mode.Equals(TEXT("apply")))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("mode must be preview or apply.");
		Diagnostic.Detail = Mode;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* SettingsClass = nullptr;
	UObject* ConfigObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!ResolveSettingsClass(ClassPath, SettingsClass, ConfigObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> PreviewChangedProperties;
	FMCPDiagnostic ParsePatchDiagnostic;
	if (!ParseTopLevelPatchProperties(PatchOperations, ConfigObject, PreviewChangedProperties, ParsePatchDiagnostic))
	{
		OutResult.Diagnostics.Add(ParsePatchDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString Signature = BuildSettingsPatchSignature(SettingsClass->GetClassPathName().ToString(), PatchOperations, SaveOptions);
	if (Mode.Equals(TEXT("apply")) && !ConsumeSettingsConfirmationToken(ConfirmToken, Signature))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CONFIRM_TOKEN_INVALID;
		Diagnostic.Message = TEXT("Invalid or expired confirm_token.");
		Diagnostic.Suggestion = TEXT("Run settings.project.patch in preview mode and retry with returned confirm_token.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString PreviewToken;
	TArray<FString> ChangedProperties = PreviewChangedProperties;
	TArray<FString> SavedConfigFiles;
	bool bVerified = false;
	bool bPersistSucceeded = true;

	if (Mode.Equals(TEXT("preview")))
	{
		PreviewToken = BuildSettingsConfirmationToken(Signature);
	}
	else if (!Request.Context.bDryRun)
	{
		const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP Project Settings Patch"));
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		ConfigObject->Modify();

		FMCPDiagnostic PatchDiagnostic;
		if (!MCPObjectUtils::ApplyPatch(ConfigObject, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		ConfigObject->PostEditChange();
		ConfigObject->MarkPackageDirty();
		bPersistSucceeded = PersistConfigObject(ConfigObject, SaveOptions, SavedConfigFiles, bVerified, OutResult);
	}

	MCPObjectUtils::AppendTouchedPackage(ConfigObject, OutResult.TouchedPackages);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("mode"), Mode);
	OutResult.ResultObject->SetStringField(TEXT("class_path"), SettingsClass->GetClassPathName().ToString());
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetStringField(TEXT("confirm_token"), PreviewToken);
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));

	if (!Mode.Equals(TEXT("preview")) && SaveOptions.bReloadVerify && SaveOptions.bSaveConfig && !bVerified)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_VERIFY_FAILED;
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Message = TEXT("Settings reload verification did not run.");
		Diagnostic.Detail = SettingsClass->GetClassPathName().ToString();
		OutResult.Diagnostics.Add(Diagnostic);
	}

	if (Mode.Equals(TEXT("preview")))
	{
		OutResult.Status = EMCPResponseStatus::Ok;
		return true;
	}

	OutResult.Status = bPersistSucceeded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleSettingsProjectApply(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	if (!Request.Params.IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("params are required for settings.project.apply.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPRequestEnvelope ApplyRequest = Request;
	ApplyRequest.Params = MakeShared<FJsonObject>(*Request.Params);
	ApplyRequest.Params->SetStringField(TEXT("mode"), TEXT("apply"));
	return HandleSettingsProjectPatch(ApplyRequest, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	(void)Request;

	UGameMapsSettings* GameMapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (GameMapsSettings == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
		Diagnostic.Message = TEXT("UGameMapsSettings default object is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> RawMapOverrides;
	if (!GetGameModeMapOverrideRawArray(GameMapsSettings, RawMapOverrides))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_VERIFY_FAILED;
		Diagnostic.Message = TEXT("Failed to inspect GameModeMapPrefixes from UGameMapsSettings.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("class_path"), UGameMapsSettings::StaticClass()->GetClassPathName().ToString());
	OutResult.ResultObject->SetStringField(TEXT("config_file"), GameMapsSettings->GetDefaultConfigFilename());
	OutResult.ResultObject->SetStringField(TEXT("global_default_game_mode"), UGameMapsSettings::GetGlobalDefaultGameMode());
	OutResult.ResultObject->SetStringField(TEXT("global_default_server_game_mode"), GetSettingsStringProperty(GameMapsSettings, TEXT("GlobalDefaultServerGameMode")));
	OutResult.ResultObject->SetArrayField(TEXT("map_overrides"), BuildMapOverrideEntriesFromRaw(RawMapOverrides));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeSetDefault(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString GameModeClassPath;
	FString ServerGameModeClassPath;
	FMCPSettingsSaveOptions SaveOptions;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("game_mode_class_path"), GameModeClassPath);
		Request.Params->TryGetStringField(TEXT("server_game_mode_class_path"), ServerGameModeClassPath);
		ParseSettingsSaveOptions(Request.Params, SaveOptions);
	}

	if (GameModeClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("game_mode_class_path is required for settings.gamemode.set_default.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPDiagnostic ClassDiagnostic;
	FString ResolvedGameModeClassPath;
	if (!ResolveGameModeClassPath(GameModeClassPath, ResolvedGameModeClassPath, ClassDiagnostic))
	{
		OutResult.Diagnostics.Add(ClassDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString ResolvedServerGameModeClassPath;
	if (!ResolveGameModeClassPath(ServerGameModeClassPath, ResolvedServerGameModeClassPath, ClassDiagnostic))
	{
		OutResult.Diagnostics.Add(ClassDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UGameMapsSettings* GameMapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (GameMapsSettings == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
		Diagnostic.Message = TEXT("UGameMapsSettings default object is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	const FString CurrentDefaultGameModeBefore = UGameMapsSettings::GetGlobalDefaultGameMode();
	const FString CurrentServerGameModeBefore = GetSettingsStringProperty(GameMapsSettings, TEXT("GlobalDefaultServerGameMode"));
	const bool bDefaultChanged = !CurrentDefaultGameModeBefore.Equals(ResolvedGameModeClassPath, ESearchCase::CaseSensitive);
	if (bDefaultChanged)
	{
		ChangedProperties.Add(TEXT("GlobalDefaultGameMode"));
	}
	const bool bServerChanged =
		!ServerGameModeClassPath.IsEmpty() &&
		!CurrentServerGameModeBefore.Equals(ResolvedServerGameModeClassPath, ESearchCase::CaseSensitive);
	if (bServerChanged)
	{
		ChangedProperties.Add(TEXT("GlobalDefaultServerGameMode"));
	}

	TArray<FString> SavedConfigFiles;
	bool bVerified = false;
	bool bPersistSucceeded = true;

	if (!Request.Context.bDryRun && (bDefaultChanged || bServerChanged))
	{
		const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP GameMode Set Default"));
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		GameMapsSettings->Modify();
		if (bDefaultChanged)
		{
			UGameMapsSettings::SetGlobalDefaultGameMode(ResolvedGameModeClassPath);
		}

		if (bServerChanged)
		{
			TSharedRef<FJsonObject> PatchEntry = MakeShared<FJsonObject>();
			PatchEntry->SetStringField(TEXT("op"), TEXT("replace"));
			PatchEntry->SetStringField(TEXT("path"), TEXT("/GlobalDefaultServerGameMode"));
			PatchEntry->SetField(TEXT("value"), MakeShared<FJsonValueString>(ResolvedServerGameModeClassPath));
			const TArray<TSharedPtr<FJsonValue>> PatchOperations{ MakeShared<FJsonValueObject>(PatchEntry) };

			FMCPDiagnostic PatchDiagnostic;
			TArray<FString> PatchedProperties;
			if (!MCPObjectUtils::ApplyPatch(GameMapsSettings, &PatchOperations, PatchedProperties, PatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}

		GameMapsSettings->PostEditChange();
		GameMapsSettings->MarkPackageDirty();
		bPersistSucceeded = PersistConfigObject(GameMapsSettings, SaveOptions, SavedConfigFiles, bVerified, OutResult);
	}

	MCPObjectUtils::AppendTouchedPackage(GameMapsSettings, OutResult.TouchedPackages);
	const FString CurrentDefaultGameModeAfter = UGameMapsSettings::GetGlobalDefaultGameMode();
	const FString CurrentServerGameModeAfter = GetSettingsStringProperty(GameMapsSettings, TEXT("GlobalDefaultServerGameMode"));
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetStringField(TEXT("global_default_game_mode"), CurrentDefaultGameModeAfter);
	OutResult.ResultObject->SetStringField(TEXT("global_default_server_game_mode"), CurrentServerGameModeAfter);
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bPersistSucceeded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeSetMapOverride(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString MapKey;
	FString GameModeClassPath;
	FMCPSettingsSaveOptions SaveOptions;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("map_key"), MapKey);
		Request.Params->TryGetStringField(TEXT("game_mode_class_path"), GameModeClassPath);
		ParseSettingsSaveOptions(Request.Params, SaveOptions);
	}

	MapKey.TrimStartAndEndInline();
	if (MapKey.IsEmpty() || GameModeClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("map_key and game_mode_class_path are required for settings.gamemode.set_map_override.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPDiagnostic ClassDiagnostic;
	FString ResolvedGameModeClassPath;
	if (!ResolveGameModeClassPath(GameModeClassPath, ResolvedGameModeClassPath, ClassDiagnostic))
	{
		OutResult.Diagnostics.Add(ClassDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UGameMapsSettings* GameMapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (GameMapsSettings == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
		Diagnostic.Message = TEXT("UGameMapsSettings default object is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CurrentRawArray;
	if (!GetGameModeMapOverrideRawArray(GameMapsSettings, CurrentRawArray))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_VERIFY_FAILED;
		Diagnostic.Message = TEXT("Failed to inspect GameModeMapPrefixes from UGameMapsSettings.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	auto GetRawEntryName = [](const TSharedPtr<FJsonValue>& EntryValue) -> FString
	{
		if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
		FString Name;
		EntryObject->TryGetStringField(TEXT("Name"), Name);
		if (Name.IsEmpty())
		{
			EntryObject->TryGetStringField(TEXT("name"), Name);
		}
		return Name;
	};

	auto GetRawEntryGameMode = [](const TSharedPtr<FJsonValue>& EntryValue) -> FString
	{
		if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
		FString GameMode;
		TryGetMapOverrideGameModeClassPath(EntryObject, GameMode);
		return GameMode;
	};

	TArray<TSharedPtr<FJsonValue>> UpdatedRawArray = CurrentRawArray;
	int32 ExistingIndex = INDEX_NONE;
	for (int32 Index = 0; Index < UpdatedRawArray.Num(); ++Index)
	{
		if (GetRawEntryName(UpdatedRawArray[Index]).Equals(MapKey, ESearchCase::IgnoreCase))
		{
			ExistingIndex = Index;
			break;
		}
	}

	const bool bUpdated = ExistingIndex != INDEX_NONE;
	bool bChanged = true;
	if (ExistingIndex != INDEX_NONE)
	{
		bChanged = !GetRawEntryGameMode(UpdatedRawArray[ExistingIndex]).Equals(ResolvedGameModeClassPath, ESearchCase::CaseSensitive);
		if (bChanged)
		{
			TSharedRef<FJsonObject> UpdatedObject = MakeShared<FJsonObject>();
			UpdatedObject->SetStringField(TEXT("Name"), MapKey);
			UpdatedObject->SetField(TEXT("GameMode"), BuildSoftClassPathJsonValue(ResolvedGameModeClassPath));
			UpdatedRawArray[ExistingIndex] = MakeShared<FJsonValueObject>(UpdatedObject);
		}
	}
	else
	{
		TSharedRef<FJsonObject> NewEntry = MakeShared<FJsonObject>();
		NewEntry->SetStringField(TEXT("Name"), MapKey);
		NewEntry->SetField(TEXT("GameMode"), BuildSoftClassPathJsonValue(ResolvedGameModeClassPath));
		UpdatedRawArray.Add(MakeShared<FJsonValueObject>(NewEntry));
	}

	TArray<FString> SavedConfigFiles;
	bool bVerified = false;
	bool bPersistSucceeded = true;
	TArray<FString> ChangedProperties;

	if (bChanged)
	{
		TSharedRef<FJsonObject> PatchEntry = MakeShared<FJsonObject>();
		PatchEntry->SetStringField(TEXT("op"), TEXT("replace"));
		PatchEntry->SetStringField(TEXT("path"), TEXT("/GameModeMapPrefixes"));
		PatchEntry->SetField(TEXT("value"), MakeShared<FJsonValueArray>(UpdatedRawArray));
		const TArray<TSharedPtr<FJsonValue>> PatchOperations{ MakeShared<FJsonValueObject>(PatchEntry) };

		FMCPDiagnostic PatchDiagnostic;
		if (Request.Context.bDryRun)
		{
			if (!ParseTopLevelPatchProperties(&PatchOperations, GameMapsSettings, ChangedProperties, PatchDiagnostic))
			{
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}
		else
		{
			const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP GameMode Set Map Override"));
			FScopedTransaction Transaction(FText::FromString(TransactionLabel));
			GameMapsSettings->Modify();

			if (!MCPObjectUtils::ApplyPatch(GameMapsSettings, &PatchOperations, ChangedProperties, PatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			GameMapsSettings->PostEditChange();
			GameMapsSettings->MarkPackageDirty();
			bPersistSucceeded = PersistConfigObject(GameMapsSettings, SaveOptions, SavedConfigFiles, bVerified, OutResult);
		}
	}

	TArray<TSharedPtr<FJsonValue>> ResultRawArray = UpdatedRawArray;
	if (!Request.Context.bDryRun && !GetGameModeMapOverrideRawArray(GameMapsSettings, ResultRawArray))
	{
		ResultRawArray = UpdatedRawArray;
	}

	MCPObjectUtils::AppendTouchedPackage(GameMapsSettings, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("map_key"), MapKey);
	OutResult.ResultObject->SetBoolField(TEXT("updated"), bUpdated);
	OutResult.ResultObject->SetStringField(TEXT("game_mode_class_path"), ResolvedGameModeClassPath);
	OutResult.ResultObject->SetArrayField(TEXT("map_overrides"), BuildMapOverrideEntriesFromRaw(ResultRawArray));
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bPersistSucceeded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeRemoveMapOverride(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString MapKey;
	FMCPSettingsSaveOptions SaveOptions;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("map_key"), MapKey);
		ParseSettingsSaveOptions(Request.Params, SaveOptions);
	}

	MapKey.TrimStartAndEndInline();
	if (MapKey.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("map_key is required for settings.gamemode.remove_map_override.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UGameMapsSettings* GameMapsSettings = GetMutableDefault<UGameMapsSettings>();
	if (GameMapsSettings == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_CLASS_NOT_FOUND;
		Diagnostic.Message = TEXT("UGameMapsSettings default object is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CurrentRawArray;
	if (!GetGameModeMapOverrideRawArray(GameMapsSettings, CurrentRawArray))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SETTINGS_VERIFY_FAILED;
		Diagnostic.Message = TEXT("Failed to inspect GameModeMapPrefixes from UGameMapsSettings.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	auto GetRawEntryName = [](const TSharedPtr<FJsonValue>& EntryValue) -> FString
	{
		if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
		FString Name;
		EntryObject->TryGetStringField(TEXT("Name"), Name);
		if (Name.IsEmpty())
		{
			EntryObject->TryGetStringField(TEXT("name"), Name);
		}
		return Name;
	};

	TArray<TSharedPtr<FJsonValue>> UpdatedRawArray;
	UpdatedRawArray.Reserve(CurrentRawArray.Num());
	int32 RemovedCount = 0;
	for (const TSharedPtr<FJsonValue>& EntryValue : CurrentRawArray)
	{
		if (GetRawEntryName(EntryValue).Equals(MapKey, ESearchCase::IgnoreCase))
		{
			RemovedCount++;
			continue;
		}
		UpdatedRawArray.Add(EntryValue);
	}

	if (RemovedCount == 0)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::GAMEMODE_OVERRIDE_NOT_FOUND;
		Diagnostic.Message = TEXT("No map override entry matched map_key.");
		Diagnostic.Detail = MapKey;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> SavedConfigFiles;
	bool bVerified = false;
	bool bPersistSucceeded = true;
	TArray<FString> ChangedProperties;

	TSharedRef<FJsonObject> PatchEntry = MakeShared<FJsonObject>();
	PatchEntry->SetStringField(TEXT("op"), TEXT("replace"));
	PatchEntry->SetStringField(TEXT("path"), TEXT("/GameModeMapPrefixes"));
	PatchEntry->SetField(TEXT("value"), MakeShared<FJsonValueArray>(UpdatedRawArray));
	const TArray<TSharedPtr<FJsonValue>> PatchOperations{ MakeShared<FJsonValueObject>(PatchEntry) };

	FMCPDiagnostic PatchDiagnostic;
	if (Request.Context.bDryRun)
	{
		if (!ParseTopLevelPatchProperties(&PatchOperations, GameMapsSettings, ChangedProperties, PatchDiagnostic))
		{
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}
	else
	{
		const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP GameMode Remove Map Override"));
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		GameMapsSettings->Modify();

		if (!MCPObjectUtils::ApplyPatch(GameMapsSettings, &PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		GameMapsSettings->PostEditChange();
		GameMapsSettings->MarkPackageDirty();
		bPersistSucceeded = PersistConfigObject(GameMapsSettings, SaveOptions, SavedConfigFiles, bVerified, OutResult);
	}

	TArray<TSharedPtr<FJsonValue>> ResultRawArray = UpdatedRawArray;
	if (!Request.Context.bDryRun && !GetGameModeMapOverrideRawArray(GameMapsSettings, ResultRawArray))
	{
		ResultRawArray = UpdatedRawArray;
	}

	MCPObjectUtils::AppendTouchedPackage(GameMapsSettings, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("map_key"), MapKey);
	OutResult.ResultObject->SetNumberField(TEXT("removed_count"), RemovedCount);
	OutResult.ResultObject->SetArrayField(TEXT("map_overrides"), BuildMapOverrideEntriesFromRaw(ResultRawArray));
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("saved_config_files"), ToJsonStringArray(SavedConfigFiles));
	OutResult.ResultObject->SetBoolField(TEXT("verified"), bVerified);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bPersistSucceeded ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

FString UMCPToolRegistrySubsystem::BuildDeleteConfirmationToken(const TArray<FString>& ObjectPaths, const bool bFailIfReferenced) const
{
	ReclaimExpiredDeleteConfirmationTokens();

	FPendingDeleteConfirmation Confirmation;
	Confirmation.ObjectPaths = ObjectPaths;
	Confirmation.ObjectPaths.Sort();
	Confirmation.ObjectPaths.SetNum(Algo::Unique(Confirmation.ObjectPaths));
	Confirmation.bFailIfReferenced = bFailIfReferenced;
	Confirmation.ExpiresAtUtc = FDateTime::UtcNow() + FTimespan::FromSeconds(60.0);

	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	{
		FScopeLock ScopeLock(&DeleteConfirmationGuard);
		PendingDeleteConfirmations.Add(Token, MoveTemp(Confirmation));
	}

	return Token;
}

bool UMCPToolRegistrySubsystem::ConsumeDeleteConfirmationToken(
	const FString& Token,
	const TArray<FString>& ObjectPaths,
	const bool bFailIfReferenced) const
{
	ReclaimExpiredDeleteConfirmationTokens();

	if (Token.IsEmpty())
	{
		return false;
	}

	TArray<FString> NormalizedPaths = ObjectPaths;
	NormalizedPaths.Sort();
	NormalizedPaths.SetNum(Algo::Unique(NormalizedPaths));

	FScopeLock ScopeLock(&DeleteConfirmationGuard);
	FPendingDeleteConfirmation* Confirmation = PendingDeleteConfirmations.Find(Token);
	if (Confirmation == nullptr)
	{
		return false;
	}

	const bool bMatched = Confirmation->bFailIfReferenced == bFailIfReferenced && Confirmation->ObjectPaths == NormalizedPaths;
	PendingDeleteConfirmations.Remove(Token);
	return bMatched;
}

void UMCPToolRegistrySubsystem::ReclaimExpiredDeleteConfirmationTokens() const
{
	const FDateTime NowUtc = FDateTime::UtcNow();
	FScopeLock ScopeLock(&DeleteConfirmationGuard);
	for (auto It = PendingDeleteConfirmations.CreateIterator(); It; ++It)
	{
		if (It->Value.ExpiresAtUtc <= NowUtc)
		{
			It.RemoveCurrent();
		}
	}
}

FString UMCPToolRegistrySubsystem::BuildSettingsConfirmationToken(const FString& Signature) const
{
	ReclaimExpiredSettingsConfirmationTokens();

	FPendingSettingsConfirmation Confirmation;
	Confirmation.Signature = Signature;
	Confirmation.ExpiresAtUtc = FDateTime::UtcNow() + FTimespan::FromSeconds(60.0);

	const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	{
		FScopeLock ScopeLock(&SettingsConfirmationGuard);
		PendingSettingsConfirmations.Add(Token, MoveTemp(Confirmation));
	}

	return Token;
}

bool UMCPToolRegistrySubsystem::ConsumeSettingsConfirmationToken(const FString& Token, const FString& Signature) const
{
	ReclaimExpiredSettingsConfirmationTokens();

	if (Token.IsEmpty())
	{
		return false;
	}

	FScopeLock ScopeLock(&SettingsConfirmationGuard);
	FPendingSettingsConfirmation* Confirmation = PendingSettingsConfirmations.Find(Token);
	if (Confirmation == nullptr)
	{
		return false;
	}

	const bool bMatched = Confirmation->Signature.Equals(Signature, ESearchCase::CaseSensitive);
	PendingSettingsConfirmations.Remove(Token);
	return bMatched;
}

void UMCPToolRegistrySubsystem::ReclaimExpiredSettingsConfirmationTokens() const
{
	const FDateTime NowUtc = FDateTime::UtcNow();
	FScopeLock ScopeLock(&SettingsConfirmationGuard);
	for (auto It = PendingSettingsConfirmations.CreateIterator(); It; ++It)
	{
		if (It->Value.ExpiresAtUtc <= NowUtc)
		{
			It.RemoveCurrent();
		}
	}
}

bool UMCPToolRegistrySubsystem::HandleObjectInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	const TSharedPtr<FJsonObject>* TargetObject = nullptr;
	const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
	int32 Depth = 2;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetObjectField(TEXT("target"), TargetObject);
		Request.Params->TryGetObjectField(TEXT("filters"), FiltersObject);

		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 5);
	}

	if (TargetObject == nullptr || !TargetObject->IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("target is required for object.inspect.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* ResolvedObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!MCPObjectUtils::ResolveTargetObject(*TargetObject, ResolvedObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Properties;
	MCPObjectUtils::InspectObject(
		ResolvedObject,
		(FiltersObject != nullptr) ? *FiltersObject : nullptr,
		Depth,
		Properties);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("properties"), Properties);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleObjectPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	const TSharedPtr<FJsonObject>* TargetObject = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;
	FString TransactionLabel = TEXT("MCP Patch");

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetObjectField(TEXT("target"), TargetObject);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);

		const TSharedPtr<FJsonObject>* TransactionObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("transaction"), TransactionObject) && TransactionObject != nullptr && TransactionObject->IsValid())
		{
			(*TransactionObject)->TryGetStringField(TEXT("label"), TransactionLabel);
		}
	}

	if (TargetObject == nullptr || !TargetObject->IsValid() || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("target and patch are required for object.patch.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* ResolvedObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!MCPObjectUtils::ResolveTargetObject(*TargetObject, ResolvedObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;

	if (Request.Context.bDryRun)
	{
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
			if (Tokens.Num() == 1)
			{
				ChangedProperties.AddUnique(Tokens[0]);
			}
		}
	}
	else
	{
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		ResolvedObject->Modify();
		if (!MCPObjectUtils::ApplyPatch(ResolvedObject, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		ResolvedObject->PostEditChange();
		ResolvedObject->MarkPackageDirty();
	}

	MCPObjectUtils::AppendTouchedPackage(ResolvedObject, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleWorldOutlinerList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	UWorld* World = GetEditorWorld();
	if (World == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Editor world is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	int32 Limit = 200;
	const int32 Cursor = ParseCursor(Request.Params);
	bool bIncludeClassPath = true;
	bool bIncludeFolderPath = true;
	bool bIncludeTags = false;
	bool bIncludeTransform = false;
	FString NameGlob;
	TSet<FString> AllowedClassPaths;

	if (Request.Params.IsValid())
	{
		double LimitNumber = static_cast<double>(Limit);
		Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, 2000);

		const TSharedPtr<FJsonObject>* IncludeObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("include"), IncludeObject) && IncludeObject != nullptr && IncludeObject->IsValid())
		{
			(*IncludeObject)->TryGetBoolField(TEXT("class"), bIncludeClassPath);
			(*IncludeObject)->TryGetBoolField(TEXT("folder_path"), bIncludeFolderPath);
			(*IncludeObject)->TryGetBoolField(TEXT("tags"), bIncludeTags);
			(*IncludeObject)->TryGetBoolField(TEXT("transform"), bIncludeTransform);
		}

		const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("filters"), FiltersObject) && FiltersObject != nullptr && FiltersObject->IsValid())
		{
			(*FiltersObject)->TryGetStringField(TEXT("name_glob"), NameGlob);
			const TArray<TSharedPtr<FJsonValue>>* ClassPathArray = nullptr;
			if ((*FiltersObject)->TryGetArrayField(TEXT("class_path_in"), ClassPathArray) && ClassPathArray != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& ClassPathValue : *ClassPathArray)
				{
					FString ClassPath;
					if (ClassPathValue.IsValid() && ClassPathValue->TryGetString(ClassPath) && !ClassPath.IsEmpty())
					{
						AllowedClassPaths.Add(ClassPath);
					}
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonObject>> AllNodes;
	TSet<FString> FolderPaths;
	USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor == nullptr)
		{
			continue;
		}

		const FString ActorLabel = Actor->GetActorLabel();
		if (!NameGlob.IsEmpty() && !ActorLabel.MatchesWildcard(NameGlob))
		{
			continue;
		}

		const FString ClassPath = Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString();
		if (AllowedClassPaths.Num() > 0 && !AllowedClassPaths.Contains(ClassPath))
		{
			continue;
		}

		const FString FolderPath = Actor->GetFolderPath().ToString();
		if (bIncludeFolderPath && !FolderPath.IsEmpty())
		{
			FolderPaths.Add(FolderPath);
		}

		TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		NodeObject->SetStringField(TEXT("node_type"), TEXT("actor"));
		NodeObject->SetStringField(TEXT("id"), Actor->GetActorGuid().IsValid() ? Actor->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphens) : MCPObjectUtils::BuildActorPath(Actor));
		NodeObject->SetStringField(TEXT("name"), ActorLabel);
		NodeObject->SetStringField(TEXT("actor_path"), MCPObjectUtils::BuildActorPath(Actor));
		NodeObject->SetStringField(TEXT("folder_path"), FolderPath);
		NodeObject->SetStringField(TEXT("parent_id"), FolderPath.IsEmpty() ? TEXT("") : FString::Printf(TEXT("folder:%s"), *FolderPath));
		NodeObject->SetStringField(TEXT("class_path"), bIncludeClassPath ? ClassPath : TEXT(""));
		NodeObject->SetBoolField(TEXT("is_selected"), Selection != nullptr && Selection->IsSelected(Actor));
		NodeObject->SetBoolField(TEXT("is_hidden_in_editor"), Actor->IsHiddenEd());

		if (bIncludeTags)
		{
			TArray<FString> TagStrings;
			TagStrings.Reserve(Actor->Tags.Num());
			for (const FName& Tag : Actor->Tags)
			{
				TagStrings.Add(Tag.ToString());
			}
			NodeObject->SetArrayField(TEXT("tags"), ToJsonStringArray(TagStrings));
		}

		if (bIncludeTransform)
		{
			const FTransform Transform = Actor->GetActorTransform();
			TSharedRef<FJsonObject> TransformObject = MakeShared<FJsonObject>();
			TransformObject->SetStringField(TEXT("location"), Transform.GetLocation().ToCompactString());
			TransformObject->SetStringField(TEXT("rotation"), Transform.GetRotation().Rotator().ToCompactString());
			TransformObject->SetStringField(TEXT("scale"), Transform.GetScale3D().ToCompactString());
			NodeObject->SetObjectField(TEXT("transform"), TransformObject);
		}

		AllNodes.Add(NodeObject);
	}

	if (bIncludeFolderPath)
	{
		TArray<FString> SortedFolders = FolderPaths.Array();
		SortedFolders.Sort();
		for (const FString& FolderPath : SortedFolders)
		{
			TSharedRef<FJsonObject> FolderNode = MakeShared<FJsonObject>();
			FolderNode->SetStringField(TEXT("node_type"), TEXT("folder"));
			FolderNode->SetStringField(TEXT("id"), FString::Printf(TEXT("folder:%s"), *FolderPath));
			FolderNode->SetStringField(TEXT("folder_path"), FolderPath);

			FString ParentFolderPath;
			FString FolderName = FolderPath;
			int32 SeparatorIndex = INDEX_NONE;
			if (FolderPath.FindLastChar(TEXT('/'), SeparatorIndex))
			{
				FolderName = FolderPath.Mid(SeparatorIndex + 1);
				ParentFolderPath = FolderPath.Left(SeparatorIndex);
			}

			FolderNode->SetStringField(TEXT("name"), FolderName);
			FolderNode->SetStringField(TEXT("parent_id"), ParentFolderPath.IsEmpty() ? TEXT("") : FString::Printf(TEXT("folder:%s"), *ParentFolderPath));
			AllNodes.Add(FolderNode);
		}
	}

	AllNodes.Sort([](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
	{
		FString LeftType;
		FString RightType;
		Left->TryGetStringField(TEXT("node_type"), LeftType);
		Right->TryGetStringField(TEXT("node_type"), RightType);
		if (LeftType != RightType)
		{
			return LeftType < RightType;
		}

		FString LeftName;
		FString RightName;
		Left->TryGetStringField(TEXT("name"), LeftName);
		Right->TryGetStringField(TEXT("name"), RightName);
		return LeftName < RightName;
	});

	const int32 SafeCursor = FMath::Max(0, Cursor);
	const int32 EndIndex = FMath::Min(SafeCursor + Limit, AllNodes.Num());
	TArray<TSharedPtr<FJsonValue>> OutputNodes;
	for (int32 Index = SafeCursor; Index < EndIndex; ++Index)
	{
		OutputNodes.Add(MakeShared<FJsonValueObject>(AllNodes[Index].ToSharedRef()));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("nodes"), OutputNodes);
	if (EndIndex < AllNodes.Num())
	{
		OutResult.ResultObject->SetStringField(TEXT("next_cursor"), FString::FromInt(EndIndex));
	}
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleWorldSelectionGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	(void)Request;

	USelection* Selection = GEditor ? GEditor->GetSelectedActors() : nullptr;
	TArray<FString> SelectedActorPaths;
	if (Selection != nullptr)
	{
		for (FSelectionIterator It(*Selection); It; ++It)
		{
			AActor* Actor = Cast<AActor>(*It);
			if (Actor != nullptr)
			{
				SelectedActorPaths.Add(MCPObjectUtils::BuildActorPath(Actor));
			}
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("actors"), ToJsonStringArray(SelectedActorPaths));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleWorldSelectionSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	UWorld* World = GetEditorWorld();
	if (World == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Editor world is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString Mode = TEXT("replace");
	const TArray<TSharedPtr<FJsonValue>>* ActorValues = nullptr;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("mode"), Mode);
		Request.Params->TryGetArrayField(TEXT("actors"), ActorValues);
	}

	if (ActorValues == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("actors array is required for world.selection.set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (Mode.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
	{
		GEditor->SelectNone(false, true, false);
	}

	int32 MissingActorCount = 0;
	for (const TSharedPtr<FJsonValue>& ActorValue : *ActorValues)
	{
		FString ActorReference;
		if (!ActorValue.IsValid() || !ActorValue->TryGetString(ActorReference) || ActorReference.IsEmpty())
		{
			continue;
		}

		AActor* Actor = FindActorByReference(World, ActorReference);
		if (Actor == nullptr)
		{
			++MissingActorCount;
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Message = TEXT("Actor reference could not be resolved.");
			Diagnostic.Detail = ActorReference;
			Diagnostic.bRetriable = true;
			OutResult.Diagnostics.Add(Diagnostic);
			continue;
		}

		if (Mode.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
		{
			GEditor->SelectActor(Actor, false, false, true);
		}
		else
		{
			GEditor->SelectActor(Actor, true, false, true);
		}
	}

	GEditor->NoteSelectionChange();

	USelection* Selection = GEditor->GetSelectedActors();
	TArray<FString> SelectedActorPaths;
	for (FSelectionIterator It(*Selection); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (Actor != nullptr)
		{
			SelectedActorPaths.Add(MCPObjectUtils::BuildActorPath(Actor));
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("selected"), ToJsonStringArray(SelectedActorPaths));
	OutResult.Status = (MissingActorCount > 0 && SelectedActorPaths.Num() > 0) ? EMCPResponseStatus::Partial : EMCPResponseStatus::Ok;
	if (MissingActorCount > 0 && SelectedActorPaths.Num() == 0)
	{
		OutResult.Status = EMCPResponseStatus::Error;
	}

	return OutResult.Status != EMCPResponseStatus::Error;
}

bool UMCPToolRegistrySubsystem::HandleMatInstanceParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	bool bIncludeInherited = true;
	TSet<FString> AllowedKinds;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

		const TArray<TSharedPtr<FJsonValue>>* Kinds = nullptr;
		if (Request.Params->TryGetArrayField(TEXT("kinds"), Kinds) && Kinds != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& KindValue : *Kinds)
			{
				FString Kind;
				if (KindValue.IsValid() && KindValue->TryGetString(Kind))
				{
					AllowedKinds.Add(Kind);
				}
			}
		}
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for mat.instance.params.get.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMaterialInstance* MaterialInstance = LoadObject<UMaterialInstance>(nullptr, *ObjectPath);
	if (MaterialInstance == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Material instance not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	(void)bIncludeInherited;
	TArray<TSharedPtr<FJsonValue>> Params;

	if (IsKindAllowed(AllowedKinds, TEXT("scalar")))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		MaterialInstance->GetAllScalarParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			float Value = 0.0f;
			if (!MaterialInstance->GetScalarParameterValue(Info, Value))
			{
				continue;
			}

			TSharedRef<FJsonObject> ParamObject = MakeShared<FJsonObject>();
			ParamObject->SetStringField(TEXT("name"), Info.Name.ToString());
			ParamObject->SetStringField(TEXT("kind"), TEXT("scalar"));
			ParamObject->SetNumberField(TEXT("value"), Value);
			ParamObject->SetBoolField(TEXT("overridden"), true);
			ParamObject->SetStringField(TEXT("source"), TEXT("local"));
			Params.Add(MakeShared<FJsonValueObject>(ParamObject));
		}
	}

	if (IsKindAllowed(AllowedKinds, TEXT("vector")))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		MaterialInstance->GetAllVectorParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			FLinearColor Value = FLinearColor::Black;
			if (!MaterialInstance->GetVectorParameterValue(Info, Value))
			{
				continue;
			}

			TSharedRef<FJsonObject> ValueObject = MakeShared<FJsonObject>();
			ValueObject->SetNumberField(TEXT("r"), Value.R);
			ValueObject->SetNumberField(TEXT("g"), Value.G);
			ValueObject->SetNumberField(TEXT("b"), Value.B);
			ValueObject->SetNumberField(TEXT("a"), Value.A);

			TSharedRef<FJsonObject> ParamObject = MakeShared<FJsonObject>();
			ParamObject->SetStringField(TEXT("name"), Info.Name.ToString());
			ParamObject->SetStringField(TEXT("kind"), TEXT("vector"));
			ParamObject->SetObjectField(TEXT("value"), ValueObject);
			ParamObject->SetBoolField(TEXT("overridden"), true);
			ParamObject->SetStringField(TEXT("source"), TEXT("local"));
			Params.Add(MakeShared<FJsonValueObject>(ParamObject));
		}
	}

	if (IsKindAllowed(AllowedKinds, TEXT("texture")))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		MaterialInstance->GetAllTextureParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			UTexture* Value = nullptr;
			if (!MaterialInstance->GetTextureParameterValue(Info, Value))
			{
				continue;
			}

			TSharedRef<FJsonObject> ParamObject = MakeShared<FJsonObject>();
			ParamObject->SetStringField(TEXT("name"), Info.Name.ToString());
			ParamObject->SetStringField(TEXT("kind"), TEXT("texture"));
			ParamObject->SetStringField(TEXT("value"), Value ? Value->GetPathName() : TEXT(""));
			ParamObject->SetBoolField(TEXT("overridden"), true);
			ParamObject->SetStringField(TEXT("source"), TEXT("local"));
			Params.Add(MakeShared<FJsonValueObject>(ParamObject));
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("params"), Params);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleMatInstanceParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	FString Recompile = TEXT("auto");
	const TArray<TSharedPtr<FJsonValue>>* SetValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ClearValues = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("recompile"), Recompile);
		Request.Params->TryGetArrayField(TEXT("set"), SetValues);
		Request.Params->TryGetArrayField(TEXT("clear"), ClearValues);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for mat.instance.params.set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMaterialInstanceConstant* MaterialInstance = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	if (MaterialInstance == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Material instance constant not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> UpdatedNames;
	FScopedTransaction Transaction(FText::FromString(TEXT("MCP Material Params Set")));
	MaterialInstance->Modify();

	auto EmitParamNotFound = [&OutResult](const FString& Name, const FString& Kind)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::MATERIAL_PARAM_NOT_FOUND;
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Message = TEXT("Unsupported or invalid material parameter update request.");
		Diagnostic.Detail = FString::Printf(TEXT("name=%s kind=%s"), *Name, *Kind);
		Diagnostic.bRetriable = false;
		OutResult.Diagnostics.Add(Diagnostic);
	};

	if (SetValues != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& SetValue : *SetValues)
		{
			if (!SetValue.IsValid() || SetValue->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> SetObject = SetValue->AsObject();
			FString Name;
			FString Kind;
			SetObject->TryGetStringField(TEXT("name"), Name);
			SetObject->TryGetStringField(TEXT("kind"), Kind);
			const TSharedPtr<FJsonValue>* ValueField = SetObject->Values.Find(TEXT("value"));

			if (Name.IsEmpty() || Kind.IsEmpty() || ValueField == nullptr || !ValueField->IsValid())
			{
				continue;
			}

				const FMaterialParameterInfo ParamInfo{ FName(*Name) };
			if (Kind.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				double ScalarValue = 0;
				if ((*ValueField)->TryGetNumber(ScalarValue))
				{
						MaterialInstance->SetScalarParameterValueEditorOnly(ParamInfo, static_cast<float>(ScalarValue));
					UpdatedNames.AddUnique(Name);
				}
				else
				{
					EmitParamNotFound(Name, Kind);
				}
				continue;
			}

			if (Kind.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				FLinearColor ColorValue = FLinearColor::Black;
				if (JsonToLinearColor(*ValueField, ColorValue))
				{
						MaterialInstance->SetVectorParameterValueEditorOnly(ParamInfo, ColorValue);
					UpdatedNames.AddUnique(Name);
				}
				else
				{
					EmitParamNotFound(Name, Kind);
				}
				continue;
			}

			if (Kind.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				FString TexturePath;
				if ((*ValueField)->TryGetString(TexturePath))
				{
					UTexture* Texture = TexturePath.IsEmpty() ? nullptr : LoadObject<UTexture>(nullptr, *TexturePath);
						MaterialInstance->SetTextureParameterValueEditorOnly(ParamInfo, Texture);
					UpdatedNames.AddUnique(Name);
				}
				else
				{
					EmitParamNotFound(Name, Kind);
				}
				continue;
			}

			EmitParamNotFound(Name, Kind);
		}
	}

	if (ClearValues != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& ClearValue : *ClearValues)
		{
			if (!ClearValue.IsValid() || ClearValue->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> ClearObject = ClearValue->AsObject();
			FString Name;
			FString Kind;
			ClearObject->TryGetStringField(TEXT("name"), Name);
			ClearObject->TryGetStringField(TEXT("kind"), Kind);
			if (Name.IsEmpty() || Kind.IsEmpty())
			{
				continue;
			}

				const FMaterialParameterInfo ParamInfo{ FName(*Name) };
			if (Kind.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
					MaterialInstance->SetScalarParameterValueEditorOnly(ParamInfo, 0.0f);
				UpdatedNames.AddUnique(Name);
			}
			else if (Kind.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
					MaterialInstance->SetVectorParameterValueEditorOnly(ParamInfo, FLinearColor::Black);
				UpdatedNames.AddUnique(Name);
			}
			else if (Kind.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
					MaterialInstance->SetTextureParameterValueEditorOnly(ParamInfo, nullptr);
				UpdatedNames.AddUnique(Name);
			}
			else
			{
				EmitParamNotFound(Name, Kind);
			}
		}
	}

	if (!Request.Context.bDryRun)
	{
		MaterialInstance->PostEditChange();
		MaterialInstance->MarkPackageDirty();
	}
	else
	{
		Transaction.Cancel();
	}

	MCPObjectUtils::AppendTouchedPackage(MaterialInstance, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("updated"), ToJsonStringArray(UpdatedNames));
	OutResult.ResultObject->SetStringField(TEXT("compile_status"), Recompile.Equals(TEXT("never"), ESearchCase::IgnoreCase) ? TEXT("skipped") : TEXT("unknown"));
	OutResult.Status = OutResult.Diagnostics.Num() > 0 ? EMCPResponseStatus::Partial : EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleNiagaraParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for niagara.params.get.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* NiagaraObject = LoadObject<UObject>(nullptr, *ObjectPath);
	if (NiagaraObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Niagara object not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> InspectedProperties;
	MCPObjectUtils::InspectObject(NiagaraObject, nullptr, 1, InspectedProperties);

	TArray<TSharedPtr<FJsonValue>> Params;
	for (const TSharedPtr<FJsonValue>& PropertyValue : InspectedProperties)
	{
		if (!PropertyValue.IsValid() || PropertyValue->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> PropertyObject = PropertyValue->AsObject();
		FString Name;
		FString CppType;
		PropertyObject->TryGetStringField(TEXT("name"), Name);
		PropertyObject->TryGetStringField(TEXT("cpp_type"), CppType);
		const TSharedPtr<FJsonValue>* ValueField = PropertyObject->Values.Find(TEXT("value"));

		TSharedRef<FJsonObject> ParamObject = MakeShared<FJsonObject>();
		ParamObject->SetStringField(TEXT("name"), Name);
		ParamObject->SetStringField(TEXT("type"), CppType);
		ParamObject->SetField(TEXT("value"), ValueField != nullptr ? *ValueField : MakeShared<FJsonValueNull>());
		ParamObject->SetBoolField(TEXT("exposed"), true);
		Params.Add(MakeShared<FJsonValueObject>(ParamObject));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("params"), Params);
	OutResult.ResultObject->SetStringField(TEXT("compile_status"), TEXT("unknown"));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleNiagaraParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	bool bStrictTypes = true;
	const TArray<TSharedPtr<FJsonValue>>* SetValues = nullptr;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetBoolField(TEXT("strict_types"), bStrictTypes);
		Request.Params->TryGetArrayField(TEXT("set"), SetValues);
	}

	(void)bStrictTypes;

	if (ObjectPath.IsEmpty() || SetValues == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path and set are required for niagara.params.set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* NiagaraObject = LoadObject<UObject>(nullptr, *ObjectPath);
	if (NiagaraObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Niagara object not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> PatchOperations;
	TArray<FString> UpdatedNames;
	for (const TSharedPtr<FJsonValue>& SetValue : *SetValues)
	{
		if (!SetValue.IsValid() || SetValue->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> SetObject = SetValue->AsObject();
		FString Name;
		SetObject->TryGetStringField(TEXT("name"), Name);
		const TSharedPtr<FJsonValue>* ValueField = SetObject->Values.Find(TEXT("value"));
		if (Name.IsEmpty() || ValueField == nullptr || !ValueField->IsValid())
		{
			continue;
		}

		TSharedRef<FJsonObject> PatchObject = MakeShared<FJsonObject>();
		PatchObject->SetStringField(TEXT("op"), TEXT("replace"));
		PatchObject->SetStringField(TEXT("path"), FString::Printf(TEXT("/%s"), *Name));
		PatchObject->SetField(TEXT("value"), *ValueField);
		PatchOperations.Add(MakeShared<FJsonValueObject>(PatchObject));
		UpdatedNames.AddUnique(Name);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP Niagara Params Set")));
	NiagaraObject->Modify();

	FMCPDiagnostic PatchDiagnostic;
	TArray<FString> ChangedProperties;
	if (!MCPObjectUtils::ApplyPatch(NiagaraObject, &PatchOperations, ChangedProperties, PatchDiagnostic))
	{
		Transaction.Cancel();
		OutResult.Diagnostics.Add(PatchDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!Request.Context.bDryRun)
	{
		NiagaraObject->PostEditChange();
		NiagaraObject->MarkPackageDirty();
	}
	else
	{
		Transaction.Cancel();
	}

	MCPObjectUtils::AppendTouchedPackage(NiagaraObject, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("updated"), ToJsonStringArray(UpdatedNames));
	OutResult.ResultObject->SetStringField(TEXT("compile_status"), TEXT("unknown"));
	OutResult.ResultObject->SetArrayField(TEXT("errors"), ToJsonStringArray({}));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleNiagaraStackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	FString EmitterName;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("emitter_name"), EmitterName);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for niagara.stack.list.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* NiagaraObject = LoadObject<UObject>(nullptr, *ObjectPath);
	if (NiagaraObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Niagara object not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TSharedRef<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
	ModuleObject->SetStringField(TEXT("module_key"), BuildNiagaraCompatModuleKey(ObjectPath));
	ModuleObject->SetStringField(TEXT("display_name"), TEXT("CompatibilityParameters"));
	ModuleObject->SetBoolField(TEXT("enabled"), true);
	ModuleObject->SetStringField(TEXT("category"), EmitterName.IsEmpty() ? TEXT("compat") : EmitterName);

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(ModuleObject));

	FMCPDiagnostic Diagnostic;
	Diagnostic.Code = MCPErrorCodes::NIAGARA_MODULE_KEY_NOT_FOUND;
	Diagnostic.Severity = TEXT("info");
	Diagnostic.Message = TEXT("Niagara stack adapter is running in compatibility mode.");
	Diagnostic.Suggestion = TEXT("Use returned module_key for niagara.stack.module.set_param.");
	OutResult.Diagnostics.Add(Diagnostic);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("modules"), Modules);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleNiagaraStackModuleSetParam(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	FString ModuleKey;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("module_key"), ModuleKey);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for niagara.stack.module.set_param.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString ExpectedModuleKey = BuildNiagaraCompatModuleKey(ObjectPath);
	if (!ModuleKey.Equals(ExpectedModuleKey, ESearchCase::IgnoreCase))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::NIAGARA_MODULE_KEY_NOT_FOUND;
		Diagnostic.Message = TEXT("module_key does not match compatibility key for the target object.");
		Diagnostic.Detail = FString::Printf(TEXT("expected=%s actual=%s"), *ExpectedModuleKey, *ModuleKey);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	return HandleNiagaraParamsSet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetClassList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ClassPathGlob = TEXT("/Script/UMG.*");
	FString NameGlob;
	bool bIncludeAbstract = false;
	bool bIncludeDeprecated = false;
	bool bIncludeEditorOnly = false;
	int32 Limit = 200;
	int32 Cursor = 0;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("class_path_glob"), ClassPathGlob);
		Request.Params->TryGetStringField(TEXT("name_glob"), NameGlob);
		Request.Params->TryGetBoolField(TEXT("include_abstract"), bIncludeAbstract);
		Request.Params->TryGetBoolField(TEXT("include_deprecated"), bIncludeDeprecated);
		Request.Params->TryGetBoolField(TEXT("include_editor_only"), bIncludeEditorOnly);

		double LimitNumber = static_cast<double>(Limit);
		Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, 2000);

		double CursorNumber = static_cast<double>(Cursor);
		Request.Params->TryGetNumberField(TEXT("cursor"), CursorNumber);
		Cursor = FMath::Max(0, static_cast<int32>(CursorNumber));
	}

	struct FWidgetClassEntry
	{
		FString ClassPath;
		FString ClassName;
		FString ModulePath;
		bool bIsAbstract = false;
		bool bIsDeprecated = false;
		bool bIsEditorOnly = false;
	};

	TArray<FWidgetClassEntry> Entries;
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* WidgetClass = *ClassIt;
		if (WidgetClass == nullptr || !WidgetClass->IsChildOf(UWidget::StaticClass()))
		{
			continue;
		}

		const FString ClassName = WidgetClass->GetName();
		if (ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_")))
		{
			continue;
		}

		const bool bIsAbstract = WidgetClass->HasAnyClassFlags(CLASS_Abstract);
		const bool bIsDeprecated = WidgetClass->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists);
		const bool bIsEditorOnly = WidgetClass->IsEditorOnly();

		if (!bIncludeAbstract && bIsAbstract)
		{
			continue;
		}
		if (!bIncludeDeprecated && bIsDeprecated)
		{
			continue;
		}
		if (!bIncludeEditorOnly && bIsEditorOnly)
		{
			continue;
		}

		const FString ClassPath = WidgetClass->GetClassPathName().ToString();
		if (!ClassPathGlob.IsEmpty() && !ClassPath.MatchesWildcard(ClassPathGlob))
		{
			continue;
		}
		if (!NameGlob.IsEmpty() && !ClassName.MatchesWildcard(NameGlob))
		{
			continue;
		}

		FWidgetClassEntry Entry;
		Entry.ClassPath = ClassPath;
		Entry.ClassName = ClassName;
		Entry.ModulePath = WidgetClass->GetClassPathName().GetPackageName().ToString();
		Entry.bIsAbstract = bIsAbstract;
		Entry.bIsDeprecated = bIsDeprecated;
		Entry.bIsEditorOnly = bIsEditorOnly;
		Entries.Add(MoveTemp(Entry));
	}

	Entries.Sort([](const FWidgetClassEntry& Left, const FWidgetClassEntry& Right)
	{
		return Left.ClassPath < Right.ClassPath;
	});

	const int32 SafeCursor = FMath::Clamp(Cursor, 0, Entries.Num());
	const int32 EndIndex = FMath::Min(SafeCursor + Limit, Entries.Num());
	TArray<TSharedPtr<FJsonValue>> ClassValues;
	ClassValues.Reserve(EndIndex - SafeCursor);
	for (int32 Index = SafeCursor; Index < EndIndex; ++Index)
	{
		const FWidgetClassEntry& Entry = Entries[Index];
		TSharedRef<FJsonObject> ClassObject = MakeShared<FJsonObject>();
		ClassObject->SetStringField(TEXT("class_path"), Entry.ClassPath);
		ClassObject->SetStringField(TEXT("class_name"), Entry.ClassName);
		ClassObject->SetStringField(TEXT("module_path"), Entry.ModulePath);
		ClassObject->SetBoolField(TEXT("is_abstract"), Entry.bIsAbstract);
		ClassObject->SetBoolField(TEXT("is_deprecated"), Entry.bIsDeprecated);
		ClassObject->SetBoolField(TEXT("is_editor_only"), Entry.bIsEditorOnly);
		ClassValues.Add(MakeShared<FJsonValueObject>(ClassObject));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("classes"), ClassValues);
	OutResult.ResultObject->SetNumberField(TEXT("total_count"), Entries.Num());
	if (EndIndex < Entries.Num())
	{
		OutResult.ResultObject->SetStringField(TEXT("next_cursor"), FString::FromInt(EndIndex));
	}
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	FString WidgetClassPath;
	FString WidgetName;
	const TSharedPtr<FJsonObject>* ParentRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* WidgetPatchOperations = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* SlotPatchOperations = nullptr;
	bool bCompileOnSuccess = true;
	bool bReplaceContent = false;
	bool bAutoSave = false;
	int32 InsertIndex = -1;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("widget_class_path"), WidgetClassPath);
		Request.Params->TryGetStringField(TEXT("widget_name"), WidgetName);
		Request.Params->TryGetObjectField(TEXT("parent_ref"), ParentRef);
		Request.Params->TryGetArrayField(TEXT("widget_patch"), WidgetPatchOperations);
		Request.Params->TryGetArrayField(TEXT("slot_patch"), SlotPatchOperations);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		Request.Params->TryGetBoolField(TEXT("replace_content"), bReplaceContent);

		double InsertIndexNumber = static_cast<double>(InsertIndex);
		Request.Params->TryGetNumberField(TEXT("insert_index"), InsertIndexNumber);
		InsertIndex = FMath::Clamp(static_cast<int32>(InsertIndexNumber), -1, 10000);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || WidgetClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path and widget_class_path are required for umg.widget.add.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* WidgetClass = ResolveWidgetClassByPath(WidgetClassPath);
	if (WidgetClass == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("widget_class_path must resolve to a concrete UWidget class.");
		Diagnostic.Detail = WidgetClassPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidget* ParentWidget = nullptr;
	if (ParentRef != nullptr && ParentRef->IsValid())
	{
		ParentWidget = ResolveWidgetFromRef(WidgetBlueprint, ParentRef);
		if (ParentWidget == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
			Diagnostic.Message = TEXT("parent_ref could not be resolved.");
			Diagnostic.Detail = ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}
	else if (WidgetBlueprint->WidgetTree->RootWidget != nullptr)
	{
		ParentWidget = WidgetBlueprint->WidgetTree->RootWidget;
	}

	const FName GeneratedWidgetName = BuildUniqueWidgetName(WidgetBlueprint, WidgetClass, WidgetName);
	if (GeneratedWidgetName.IsNone())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Failed to allocate widget name.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedWidgetProperties;
	TArray<FString> ChangedSlotProperties;
	FString AddedWidgetId = FString::Printf(TEXT("name:%s"), *GeneratedWidgetName.ToString());
	FString ParentId = ParentWidget ? GetWidgetStableId(WidgetBlueprint, ParentWidget) : FString();
	FString SlotType;

		if (!Request.Context.bDryRun)
		{
			EnsureWidgetGuidMap(WidgetBlueprint);
			FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Add")));
			WidgetBlueprint->Modify();

		UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, GeneratedWidgetName);
		if (NewWidget == nullptr)
		{
			Transaction.Cancel();
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			Diagnostic.Message = TEXT("Failed to construct widget instance.");
			Diagnostic.Detail = WidgetClassPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		NewWidget->Modify();
		FMCPDiagnostic AttachDiagnostic;
		if (!AttachWidgetToParent(WidgetBlueprint, ParentWidget, NewWidget, InsertIndex, bReplaceContent, AttachDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(AttachDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		EnsureWidgetGuidEntry(WidgetBlueprint, NewWidget);

		if (WidgetPatchOperations != nullptr && WidgetPatchOperations->Num() > 0)
		{
			FMCPDiagnostic PatchDiagnostic;
			if (!MCPObjectUtils::ApplyPatch(NewWidget, WidgetPatchOperations, ChangedWidgetProperties, PatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}

		if (SlotPatchOperations != nullptr && SlotPatchOperations->Num() > 0)
		{
			if (NewWidget->Slot == nullptr)
			{
				Transaction.Cancel();
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("slot_patch requires the widget to have a slot.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			NewWidget->Slot->Modify();
			FMCPDiagnostic SlotPatchDiagnostic;
			if (!MCPObjectUtils::ApplyPatch(NewWidget->Slot, SlotPatchOperations, ChangedSlotProperties, SlotPatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(SlotPatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}

		NewWidget->PostEditChange();
		if (NewWidget->Slot != nullptr)
		{
			NewWidget->Slot->PostEditChange();
		}

		EnsureWidgetGuidMap(WidgetBlueprint);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
		AddedWidgetId = GetWidgetStableId(WidgetBlueprint, NewWidget);
		ParentId = GetParentWidgetId(WidgetBlueprint, NewWidget);
		SlotType = NewWidget->Slot ? NewWidget->Slot->GetClass()->GetName() : FString();
	}
	else
	{
		CollectChangedPropertiesFromPatchOperations(WidgetPatchOperations, ChangedWidgetProperties);
		CollectChangedPropertiesFromPatchOperations(SlotPatchOperations, ChangedSlotProperties);
		if (ParentWidget != nullptr)
		{
			if (UContentWidget* ContentParent = Cast<UContentWidget>(ParentWidget))
			{
				if (ContentParent->GetContent() != nullptr && !bReplaceContent)
				{
					FMCPDiagnostic Diagnostic;
					Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					Diagnostic.Message = TEXT("Target content widget already has a child. Set replace_content=true to replace it.");
					OutResult.Diagnostics.Add(Diagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}
			}
			else if (!Cast<UPanelWidget>(ParentWidget))
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("parent_ref must point to a panel/content widget.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

				if (UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget))
				{
					SlotType = GetPanelSlotTypeName(ParentPanel);
				}
			}
		}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	TSharedRef<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
	WidgetObject->SetStringField(TEXT("widget_id"), AddedWidgetId);
	WidgetObject->SetStringField(TEXT("name"), GeneratedWidgetName.ToString());
	WidgetObject->SetStringField(TEXT("class_path"), WidgetClass->GetPathName());
	WidgetObject->SetStringField(TEXT("parent_id"), ParentId);
	WidgetObject->SetStringField(TEXT("slot_type"), SlotType);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("created"), !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetObjectField(TEXT("widget"), WidgetObject);
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedWidgetProperties));
	OutResult.ResultObject->SetArrayField(TEXT("slot_changed_properties"), ToJsonStringArray(ChangedSlotProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
		Diagnostic.Message = TEXT("object_path and widget_ref must resolve to an existing widget.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString RemovedWidgetId = GetWidgetStableId(WidgetBlueprint, Widget);
	const FString RemovedWidgetName = Widget->GetName();
	const FString RemovedClassPath = Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString();
	const int32 RemovedChildrenCount = Cast<UPanelWidget>(Widget) ? Cast<UPanelWidget>(Widget)->GetChildrenCount() : 0;
	const FString RemovedParentId = GetParentWidgetId(WidgetBlueprint, Widget);

	bool bRemoved = true;
	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Remove")));
		WidgetBlueprint->Modify();

		bRemoved = WidgetBlueprint->WidgetTree->RemoveWidget(Widget);
		if (!bRemoved)
		{
			Transaction.Cancel();
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			Diagnostic.Message = TEXT("Failed to remove widget from widget tree.");
			Diagnostic.Detail = RemovedWidgetName;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
		EnsureWidgetGuidMap(WidgetBlueprint);
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("removed"), !Request.Context.bDryRun && bRemoved);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetStringField(TEXT("removed_widget_id"), RemovedWidgetId);
	OutResult.ResultObject->SetStringField(TEXT("removed_widget_name"), RemovedWidgetName);
	OutResult.ResultObject->SetStringField(TEXT("removed_class_path"), RemovedClassPath);
	OutResult.ResultObject->SetStringField(TEXT("old_parent_id"), RemovedParentId);
	OutResult.ResultObject->SetNumberField(TEXT("removed_children_count"), RemovedChildrenCount);
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetReparent(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	const TSharedPtr<FJsonObject>* NewParentRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* SlotPatchOperations = nullptr;
	bool bCompileOnSuccess = true;
	bool bReplaceContent = false;
	bool bSetAsRoot = false;
	bool bAutoSave = false;
	int32 InsertIndex = -1;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetObjectField(TEXT("new_parent_ref"), NewParentRef);
		Request.Params->TryGetArrayField(TEXT("slot_patch"), SlotPatchOperations);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		Request.Params->TryGetBoolField(TEXT("replace_content"), bReplaceContent);
		Request.Params->TryGetBoolField(TEXT("set_as_root"), bSetAsRoot);
		ParseAutoSaveOption(Request.Params, bAutoSave);

		double InsertIndexNumber = static_cast<double>(InsertIndex);
		Request.Params->TryGetNumberField(TEXT("insert_index"), InsertIndexNumber);
		InsertIndex = FMath::Clamp(static_cast<int32>(InsertIndexNumber), -1, 10000);
	}

	if (!bSetAsRoot && (NewParentRef == nullptr || !NewParentRef->IsValid()))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("new_parent_ref is required unless set_as_root=true.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
		Diagnostic.Message = TEXT("object_path and widget_ref must resolve to an existing widget.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidget* NewParentWidget = nullptr;
	if (!bSetAsRoot)
	{
		NewParentWidget = ResolveWidgetFromRef(WidgetBlueprint, NewParentRef);
		if (NewParentWidget == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
			Diagnostic.Message = TEXT("new_parent_ref could not be resolved.");
			Diagnostic.Detail = ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		if (NewParentWidget == Widget)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			Diagnostic.Message = TEXT("Widget cannot be reparented to itself.");
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		UWidget* CursorWidget = NewParentWidget;
		while (CursorWidget != nullptr)
		{
			if (CursorWidget == Widget)
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("Widget cannot be reparented into its own subtree.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
			CursorWidget = (CursorWidget->Slot != nullptr) ? CursorWidget->Slot->Parent : nullptr;
		}
	}
	else if (WidgetBlueprint->WidgetTree->RootWidget != nullptr && WidgetBlueprint->WidgetTree->RootWidget != Widget)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("WidgetTree already has a root widget. set_as_root=true requires replacing root manually.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString WidgetId = GetWidgetStableId(WidgetBlueprint, Widget);
	const FString WidgetName = Widget->GetName();
	const FString WidgetClassPath = Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString();
	const FString OldParentId = GetParentWidgetId(WidgetBlueprint, Widget);
	TArray<FString> ChangedSlotProperties;
	FString NewParentId = bSetAsRoot ? FString() : GetWidgetStableId(WidgetBlueprint, NewParentWidget);
	FString SlotType;

	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Reparent")));
		WidgetBlueprint->Modify();
		Widget->Modify();

		FMCPDiagnostic DetachDiagnostic;
		if (!DetachWidgetFromParent(WidgetBlueprint, Widget, DetachDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(DetachDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		if (bSetAsRoot)
		{
			WidgetBlueprint->WidgetTree->RootWidget = Widget;
		}
		else
		{
			FMCPDiagnostic AttachDiagnostic;
			if (!AttachWidgetToParent(WidgetBlueprint, NewParentWidget, Widget, InsertIndex, bReplaceContent, AttachDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(AttachDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}

		if (SlotPatchOperations != nullptr && SlotPatchOperations->Num() > 0)
		{
			if (Widget->Slot == nullptr)
			{
				Transaction.Cancel();
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("slot_patch requires the widget to have a slot after reparent.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			Widget->Slot->Modify();
			FMCPDiagnostic PatchDiagnostic;
			if (!MCPObjectUtils::ApplyPatch(Widget->Slot, SlotPatchOperations, ChangedSlotProperties, PatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
			Widget->Slot->PostEditChange();
		}

		Widget->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
		EnsureWidgetGuidMap(WidgetBlueprint);

		NewParentId = GetParentWidgetId(WidgetBlueprint, Widget);
		SlotType = Widget->Slot ? Widget->Slot->GetClass()->GetName() : FString();
	}
	else
	{
		CollectChangedPropertiesFromPatchOperations(SlotPatchOperations, ChangedSlotProperties);
			if (!bSetAsRoot && NewParentWidget != nullptr)
			{
				if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(NewParentWidget))
				{
					SlotType = GetPanelSlotTypeName(PanelWidget);
				}
			}
		}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	TSharedRef<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
	WidgetObject->SetStringField(TEXT("widget_id"), WidgetId);
	WidgetObject->SetStringField(TEXT("name"), WidgetName);
	WidgetObject->SetStringField(TEXT("class_path"), WidgetClassPath);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("moved"), !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetObjectField(TEXT("widget"), WidgetObject);
	OutResult.ResultObject->SetStringField(TEXT("old_parent_id"), OldParentId);
	OutResult.ResultObject->SetStringField(TEXT("new_parent_id"), NewParentId);
	OutResult.ResultObject->SetStringField(TEXT("slot_type"), SlotType);
	OutResult.ResultObject->SetArrayField(TEXT("slot_changed_properties"), ToJsonStringArray(ChangedSlotProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleUMGTreeGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	int32 Depth = 10;
	FString NameGlob;
	TSet<FString> AllowedClassPaths;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 20);

		const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("filters"), FiltersObject) && FiltersObject != nullptr && FiltersObject->IsValid())
		{
			(*FiltersObject)->TryGetStringField(TEXT("name_glob"), NameGlob);
			const TArray<TSharedPtr<FJsonValue>>* ClassPaths = nullptr;
			if ((*FiltersObject)->TryGetArrayField(TEXT("class_path_in"), ClassPaths) && ClassPaths != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& ClassPathValue : *ClassPaths)
				{
					FString ClassPath;
					if (ClassPathValue.IsValid() && ClassPathValue->TryGetString(ClassPath))
					{
						AllowedClassPaths.Add(ClassPath);
					}
				}
			}
		}
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	UWidget* RootWidget = WidgetBlueprint->WidgetTree->RootWidget;
	if (RootWidget != nullptr)
	{
		BuildWidgetTreeNodesRecursive(WidgetBlueprint, RootWidget, TEXT(""), NameGlob, AllowedClassPaths, Depth, 0, Nodes);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (RootWidget == nullptr)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("WidgetTree has no root widget.")));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("root_id"), RootWidget ? GetWidgetStableId(WidgetBlueprint, RootWidget) : TEXT(""));
	OutResult.ResultObject->SetArrayField(TEXT("nodes"), Nodes);
	OutResult.ResultObject->SetArrayField(TEXT("warnings"), Warnings);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	int32 Depth = 2;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 5);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
		Diagnostic.Message = TEXT("UMG widget could not be resolved.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Properties;
	MCPObjectUtils::InspectObject(Widget, nullptr, Depth, Properties);

	TSharedRef<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
	WidgetObject->SetStringField(TEXT("widget_id"), GetWidgetStableId(WidgetBlueprint, Widget));
	WidgetObject->SetStringField(TEXT("name"), Widget->GetName());
	WidgetObject->SetStringField(TEXT("class_path"), Widget->GetClass()->GetPathName());

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetObjectField(TEXT("widget"), WidgetObject);
	OutResult.ResultObject->SetArrayField(TEXT("properties"), Properties);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;
	bool bCompileOnSuccess = true;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, widget_ref, and patch are required for umg.widget.patch.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;
	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
	}

	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Patch")));
		WidgetBlueprint->Modify();
		Widget->Modify();
		if (!MCPObjectUtils::ApplyPatch(Widget, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		Widget->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
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
			if (Tokens.Num() == 1)
			{
				ChangedProperties.AddUnique(Tokens[0]);
			}
		}
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleUMGSlotPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr || PatchOperations == nullptr || Widget->Slot == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, widget_ref, and patch are required and widget must have a slot.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;
	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
	}
	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Slot Patch")));
		WidgetBlueprint->Modify();
		Widget->Slot->Modify();
		if (!MCPObjectUtils::ApplyPatch(Widget->Slot, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		Widget->Slot->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
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
			if (Tokens.Num() == 1)
			{
				ChangedProperties.AddUnique(Tokens[0]);
			}
		}
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleChangeSetList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	const UMCPChangeSetSubsystem* ChangeSetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPChangeSetSubsystem>() : nullptr;
	if (ChangeSetSubsystem == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("ChangeSet subsystem is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	int32 Limit = 50;
	FString ToolGlob;
	FString SessionId;
	TArray<FString> StatusFilter;

	if (Request.Params.IsValid())
	{
		double LimitNumber = static_cast<double>(Limit);
		Request.Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		Limit = static_cast<int32>(LimitNumber);
		Request.Params->TryGetStringField(TEXT("tool_glob"), ToolGlob);
		Request.Params->TryGetStringField(TEXT("session_id"), SessionId);

		const TArray<TSharedPtr<FJsonValue>>* StatusValues = nullptr;
		if (Request.Params->TryGetArrayField(TEXT("status_in"), StatusValues) && StatusValues != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& StatusValue : *StatusValues)
			{
				FString Status;
				if (StatusValue.IsValid() && StatusValue->TryGetString(Status))
				{
					StatusFilter.Add(Status);
				}
			}
		}
	}

	const int32 Cursor = ParseCursor(Request.Params);
	TArray<TSharedPtr<FJsonObject>> Items;
	int32 NextCursor = -1;
	FMCPDiagnostic Diagnostic;

	if (!ChangeSetSubsystem->ListChangeSets(
		Limit,
		Cursor,
		StatusFilter,
		ToolGlob,
		SessionId,
		Items,
		NextCursor,
		Diagnostic))
	{
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ItemValues;
	ItemValues.Reserve(Items.Num());
	for (const TSharedPtr<FJsonObject>& Item : Items)
	{
		ItemValues.Add(MakeShared<FJsonValueObject>(Item.ToSharedRef()));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changesets"), ItemValues);
	if (NextCursor >= 0)
	{
		OutResult.ResultObject->SetStringField(TEXT("next_cursor"), FString::FromInt(NextCursor));
	}
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleChangeSetGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	const UMCPChangeSetSubsystem* ChangeSetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPChangeSetSubsystem>() : nullptr;
	if (ChangeSetSubsystem == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("ChangeSet subsystem is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString ChangeSetId;
	bool bIncludeLogs = true;
	bool bIncludeSnapshots = false;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("changeset_id"), ChangeSetId);
		Request.Params->TryGetBoolField(TEXT("include_logs"), bIncludeLogs);
		Request.Params->TryGetBoolField(TEXT("include_snapshots"), bIncludeSnapshots);
	}

	if (ChangeSetId.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("changeset_id is required.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TSharedPtr<FJsonObject> ResultObject;
	FMCPDiagnostic Diagnostic;
	if (!ChangeSetSubsystem->GetChangeSet(ChangeSetId, bIncludeLogs, bIncludeSnapshots, ResultObject, Diagnostic))
	{
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.ResultObject = ResultObject;
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleChangeSetRollbackPreview(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	const UMCPChangeSetSubsystem* ChangeSetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPChangeSetSubsystem>() : nullptr;
	if (ChangeSetSubsystem == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("ChangeSet subsystem is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString ChangeSetId;
	FString Mode = TEXT("local_snapshot");
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("changeset_id"), ChangeSetId);
		Request.Params->TryGetStringField(TEXT("mode"), Mode);
	}

	if (ChangeSetId.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("changeset_id is required.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TSharedPtr<FJsonObject> ResultObject;
	FMCPDiagnostic Diagnostic;
	if (!ChangeSetSubsystem->PreviewRollback(ChangeSetId, Mode, ResultObject, Diagnostic))
	{
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.ResultObject = ResultObject;
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleChangeSetRollbackApply(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	const UMCPChangeSetSubsystem* ChangeSetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPChangeSetSubsystem>() : nullptr;
	UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>() : nullptr;
	if (ChangeSetSubsystem == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("ChangeSet subsystem is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString ChangeSetId;
	FString Mode = TEXT("local_snapshot");
	bool bForce = false;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("changeset_id"), ChangeSetId);
		Request.Params->TryGetStringField(TEXT("mode"), Mode);
		Request.Params->TryGetBoolField(TEXT("force"), bForce);
	}

	if (ChangeSetId.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("changeset_id is required.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	bool bApplied = false;
	TArray<FString> TouchedPackages;
	FMCPDiagnostic Diagnostic;
	if (!ChangeSetSubsystem->ApplyRollback(ChangeSetId, Mode, bForce, TouchedPackages, bApplied, Diagnostic))
	{
		if (ObservabilitySubsystem != nullptr)
		{
			ObservabilitySubsystem->RecordRollbackResult(false);
		}

		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.TouchedPackages = TouchedPackages;
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("applied"), bApplied);
	OutResult.ResultObject->SetField(TEXT("rollback_changeset_id"), MakeShared<FJsonValueNull>());
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArray(TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;

	if (ObservabilitySubsystem != nullptr)
	{
		ObservabilitySubsystem->RecordRollbackResult(bApplied);
	}

	return true;
}

bool UMCPToolRegistrySubsystem::HandleJobGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	UMCPJobSubsystem* JobSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPJobSubsystem>() : nullptr;
	if (JobSubsystem == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Job subsystem is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString JobId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("job_id"), JobId);
	}

	if (JobId.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("job_id is required.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPJobRecord Record;
	if (!JobSubsystem->GetJob(JobId, Record))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::JOB_NOT_FOUND;
		Diagnostic.Message = TEXT("Requested job was not found.");
		Diagnostic.Detail = JobId;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const FMCPDiagnostic& Diagnostic : Record.Diagnostics)
	{
		Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic.ToJson()));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("job_id"), Record.JobId);
	OutResult.ResultObject->SetStringField(TEXT("status"), UMCPJobSubsystem::StatusToString(Record.Status));
	OutResult.ResultObject->SetNumberField(TEXT("progress"), Record.Progress);
	OutResult.ResultObject->SetStringField(TEXT("started_at"), Record.StartedAtUtc.ToIso8601());
	OutResult.ResultObject->SetStringField(TEXT("updated_at"), Record.UpdatedAtUtc.ToIso8601());
	OutResult.ResultObject->SetObjectField(TEXT("result"), Record.Result.IsValid() ? Record.Result.ToSharedRef() : MakeShared<FJsonObject>());
	OutResult.ResultObject->SetArrayField(TEXT("diagnostics"), Diagnostics);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool UMCPToolRegistrySubsystem::HandleJobCancel(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	UMCPJobSubsystem* JobSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPJobSubsystem>() : nullptr;
	if (JobSubsystem == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Job subsystem is unavailable.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString JobId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("job_id"), JobId);
	}

	if (JobId.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("job_id is required.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPJobRecord Record;
	FMCPDiagnostic Diagnostic;
	if (!JobSubsystem->CancelJob(JobId, Record, Diagnostic))
	{
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("job_id"), Record.JobId);
	OutResult.ResultObject->SetBoolField(TEXT("canceled"), Record.Status == EMCPJobStatus::Canceled);
	OutResult.ResultObject->SetStringField(TEXT("status"), UMCPJobSubsystem::StatusToString(Record.Status));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}
