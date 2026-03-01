#include "MCPToolRegistrySubsystem.h"

#include "Algo/Unique.h"
#include "Editor.h"
#include "MCPChangeSetSubsystem.h"
#include "MCPErrorCodes.h"
#include "MCPJobSubsystem.h"
#include "MCPLog.h"
#include "MCPObjectUtils.h"
#include "MCPObservabilitySubsystem.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Tools/Common/MCPToolAssetUtils.h"
#include "Tools/Common/MCPToolDiagnostics.h"
#include "Tools/Common/MCPToolSchemaValidator.h"
#include "Tools/Common/MCPToolSettingsUtils.h"
#include "Tools/Common/MCPToolUMGUtils.h"
#include "Tools/Asset/MCPToolsAssetLifecycleHandler.h"
#include "Tools/Asset/MCPToolsAssetQueryHandler.h"
#include "Tools/Core/MCPToolsCoreHandler.h"
#include "Tools/Material/MCPToolsMaterialHandler.h"
#include "Tools/Niagara/MCPToolsNiagaraHandler.h"
#include "Tools/Object/MCPToolsObjectHandler.h"
#include "Tools/Ops/MCPToolsOpsHandler.h"
#include "Tools/Sequencer/MCPToolsSequencerKeyHandler.h"
#include "Tools/Sequencer/MCPToolsSequencerReadHandler.h"
#include "Tools/Sequencer/MCPToolsSequencerStructureHandler.h"
#include "Tools/Sequencer/MCPToolsSequencerValidationHandler.h"
#include "Tools/UMG/MCPToolsUMGAnimationHandler.h"
#include "Tools/UMG/MCPToolsUMGBindingHandler.h"
#include "Tools/UMG/MCPToolsUMGReadHandler.h"
#include "Tools/UMG/MCPToolsUMGStructureHandler.h"
#include "Tools/World/MCPToolsWorldHandler.h"
#include "Tools/Settings/MCPToolsSettingsGameModeHandler.h"
#include "Tools/Settings/MCPToolsSettingsComposeHandler.h"
#include "Tools/Settings/MCPToolsSettingsProjectHandler.h"
#include "ScopedTransaction.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "Factories/FbxImportUI.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "ObjectTools.h"
#include "WidgetBlueprint.h"
#include "Engine/Blueprint.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/NamedSlotInterface.h"
#include "Components/Widget.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "KeyParams.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MovieScene.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "EdGraph/EdGraph.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
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
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include <type_traits>
#include <utility>

namespace
{
	using MCPToolCommonJson::ParseCursor;
	using MCPToolCommonJson::ToJsonStringArray;

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

	FString NormalizePackageNameFromInput(const FString& InputPath)
	{
		return MCPToolAssetUtils::NormalizePackageNameFromInput(InputPath);
	}

	UPackage* ResolvePackageByName(const FString& InputPath)
	{
		return MCPToolAssetUtils::ResolvePackageByName(InputPath);
	}

	FString BuildPackageName(const FString& PackagePath, const FString& AssetName)
	{
		return MCPToolAssetUtils::BuildPackageName(PackagePath, AssetName);
	}

	FString BuildObjectPath(const FString& PackagePath, const FString& AssetName)
	{
		return MCPToolAssetUtils::BuildObjectPath(PackagePath, AssetName);
	}

	bool IsValidAssetDestination(const FString& PackagePath, const FString& AssetName)
	{
		return MCPToolAssetUtils::IsValidAssetDestination(PackagePath, AssetName);
	}

	UObject* ResolveObjectByPath(const FString& ObjectPath)
	{
		return MCPToolAssetUtils::ResolveObjectByPath(ObjectPath);
	}

	UClass* ResolveClassByPath(const FString& ClassPath)
	{
		return MCPToolAssetUtils::ResolveClassByPath(ClassPath);
	}

	bool ParseAutoSaveOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
	{
		return MCPToolSettingsUtils::ParseAutoSaveOption(Params, bOutAutoSave);
	}

	bool ParseSettingsSaveOptions(const TSharedPtr<FJsonObject>& Params, FMCPSettingsSaveOptions& OutOptions)
	{
		return MCPToolSettingsUtils::ParseSettingsSaveOptions(Params, OutOptions);
	}

	FString ParseTransactionLabel(const TSharedPtr<FJsonObject>& Params, const FString& DefaultLabel)
	{
		return MCPToolSettingsUtils::ParseTransactionLabel(Params, DefaultLabel);
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

	FString GetSettingsStringProperty(UObject* ConfigObject, const FString& PropertyName)
	{
		return MCPToolSettingsUtils::GetSettingsStringProperty(ConfigObject, PropertyName);
	}

	bool GetGameModeMapOverrideRawArray(UObject* ConfigObject, TArray<TSharedPtr<FJsonValue>>& OutRawArray)
	{
		return MCPToolSettingsUtils::GetGameModeMapOverrideRawArray(ConfigObject, OutRawArray);
	}

	bool TryGetMapOverrideGameModeClassPath(const TSharedPtr<FJsonObject>& RawEntry, FString& OutClassPath)
	{
		return MCPToolSettingsUtils::TryGetMapOverrideGameModeClassPath(RawEntry, OutClassPath);
	}

	TSharedRef<FJsonValueObject> BuildSoftClassPathJsonValue(const FString& ClassPath)
	{
		return MCPToolSettingsUtils::BuildSoftClassPathJsonValue(ClassPath);
	}

	TArray<TSharedPtr<FJsonValue>> BuildMapOverrideEntriesFromRaw(const TArray<TSharedPtr<FJsonValue>>& RawArray)
	{
		return MCPToolSettingsUtils::BuildMapOverrideEntriesFromRaw(RawArray);
	}

	bool ResolveGameModeClassPath(const FString& GameModeClassPath, FString& OutResolvedPath, FMCPDiagnostic& OutDiagnostic)
	{
		return MCPToolSettingsUtils::ResolveGameModeClassPath(
			GameModeClassPath,
			[](const FString& RequestedPath)
			{
				return ResolveClassByPath(RequestedPath);
			},
			OutResolvedPath,
			OutDiagnostic);
	}

	bool ParseStringArrayField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FString>& OutValues)
	{
		return MCPToolSettingsUtils::ParseStringArrayField(Params, FieldName, OutValues);
	}

	bool SavePackageByName(const FString& PackageName, FMCPToolExecutionResult& OutResult)
	{
		return MCPToolAssetUtils::SavePackageByName(PackageName, OutResult);
	}

	FString BuildGeneratedClassPathFromObjectPath(const FString& ObjectPath)
	{
		return MCPToolAssetUtils::BuildGeneratedClassPathFromObjectPath(ObjectPath);
	}

	struct FMCPBlueprintCreateOptions
	{
		FString PackagePath;
		FString AssetName;
		UClass* ParentClass = nullptr;
		UClass* BlueprintAssetClass = UBlueprint::StaticClass();
		UClass* BlueprintGeneratedClass = UBlueprintGeneratedClass::StaticClass();
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

		if (Options.BlueprintAssetClass == nullptr || !Options.BlueprintAssetClass->IsChildOf(UBlueprint::StaticClass()))
		{
			OutDiagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			OutDiagnostic.Message = TEXT("Blueprint asset class is invalid.");
			return false;
		}

		if (Options.BlueprintGeneratedClass == nullptr || !Options.BlueprintGeneratedClass->IsChildOf(UBlueprintGeneratedClass::StaticClass()))
		{
			OutDiagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			OutDiagnostic.Message = TEXT("Blueprint generated class is invalid.");
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
		if (BlueprintAsset != nullptr && !BlueprintAsset->IsA(Options.BlueprintAssetClass))
		{
			OutDiagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
			OutDiagnostic.Message = TEXT("Existing Blueprint type is incompatible with requested Blueprint asset class.");
			OutDiagnostic.Detail = FString::Printf(
				TEXT("object_path=%s existing_type=%s requested_type=%s"),
				*ObjectPath,
				*BlueprintAsset->GetClass()->GetPathName(),
				*Options.BlueprintAssetClass->GetPathName());
			OutDiagnostic.Suggestion = TEXT("Set overwrite=true or choose another destination.");
			return false;
		}

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
					Options.BlueprintAssetClass,
					Options.BlueprintGeneratedClass,
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

	UWidgetBlueprint* LoadWidgetBlueprintByPath(const FString& ObjectPath)
	{
		return MCPToolUMGUtils::LoadWidgetBlueprintByPath(ObjectPath);
	}

	UWidget* ResolveWidgetFromRef(UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRefObject)
	{
		return MCPToolUMGUtils::ResolveWidgetFromRef(WidgetBlueprint, WidgetRefObject);
	}

	FString GetWidgetStableId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		return MCPToolUMGUtils::GetWidgetStableId(WidgetBlueprint, Widget);
	}

	void CollectWidgetSubtreeVariableNames(UWidget* Widget, TArray<FName>& OutVariableNames)
	{
		MCPToolUMGUtils::CollectWidgetSubtreeVariableNames(Widget, OutVariableNames);
	}

	void RemoveWidgetGuidEntry(UWidgetBlueprint* WidgetBlueprint, const FName WidgetName)
	{
		MCPToolUMGUtils::RemoveWidgetGuidEntry(WidgetBlueprint, WidgetName);
	}

	void EnsureWidgetGuidMap(UWidgetBlueprint* WidgetBlueprint)
	{
		MCPToolUMGUtils::EnsureWidgetGuidMap(WidgetBlueprint);
	}

	void EnsureWidgetGuidEntry(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget)
	{
		MCPToolUMGUtils::EnsureWidgetGuidEntry(WidgetBlueprint, Widget);
	}

	UClass* ResolveWidgetClassByPath(const FString& WidgetClassPath)
	{
		return MCPToolUMGUtils::ResolveWidgetClassByPath(WidgetClassPath);
	}

	FString GetPanelSlotTypeName(UPanelWidget* PanelWidget)
	{
		return MCPToolUMGUtils::GetPanelSlotTypeName(PanelWidget);
	}

	FName BuildUniqueWidgetName(UWidgetBlueprint* WidgetBlueprint, UClass* WidgetClass, const FString& RequestedName)
	{
		return MCPToolUMGUtils::BuildUniqueWidgetName(WidgetBlueprint, WidgetClass, RequestedName);
	}

	FString GetParentWidgetId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		return MCPToolUMGUtils::GetParentWidgetId(WidgetBlueprint, Widget);
	}

	FString GetWidgetSlotType(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget, FString* OutNamedSlotName = nullptr)
	{
		return MCPToolUMGUtils::GetWidgetSlotType(WidgetBlueprint, Widget, OutNamedSlotName);
	}

	bool ResolveNamedSlotName(
		UWidget* ParentWidget,
		const FString& RequestedNamedSlotName,
		FName& OutSlotName,
		FMCPDiagnostic& OutDiagnostic)
	{
		return MCPToolUMGUtils::ResolveNamedSlotName(ParentWidget, RequestedNamedSlotName, OutSlotName, OutDiagnostic);
	}

	bool ResolveWidgetTreeNamedSlotName(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& RequestedNamedSlotName,
		FName& OutSlotName,
		FMCPDiagnostic& OutDiagnostic)
	{
		return MCPToolUMGUtils::ResolveWidgetTreeNamedSlotName(WidgetBlueprint, RequestedNamedSlotName, OutSlotName, OutDiagnostic);
	}

	bool AttachWidgetToParent(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* ParentWidget,
		UWidget* ChildWidget,
		const int32 InsertIndex,
		const bool bReplaceContent,
		const FString& NamedSlotName,
		FString& OutResolvedNamedSlotName,
		FString& OutResolvedSlotType,
		FMCPDiagnostic& OutDiagnostic)
	{
		return MCPToolUMGUtils::AttachWidgetToParent(
			WidgetBlueprint,
			ParentWidget,
			ChildWidget,
			InsertIndex,
			bReplaceContent,
			NamedSlotName,
			OutResolvedNamedSlotName,
			OutResolvedSlotType,
			OutDiagnostic);
	}

	bool DetachWidgetFromParent(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		FMCPDiagnostic& OutDiagnostic)
	{
		return MCPToolUMGUtils::DetachWidgetFromParent(WidgetBlueprint, Widget, OutDiagnostic);
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
	if (!MCPToolSchemaValidator::ValidateJsonValueAgainstSchema(ParamsValue, ToolDefinition->ParamsSchema, TEXT("params"), SchemaError))
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
		TEXT("event_stream_ws_push_v1"),
		TEXT("live_coding_compile_v1"),
		TEXT("umg_widget_event_k2_v1"),
		TEXT("sequencer_core_v1"),
		TEXT("sequencer_keys_v1")
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
	RegisterBuiltInToolsImpl();
}

void UMCPToolRegistrySubsystem::RegisterBuiltInToolsImpl()
{
	using FToolHandler = bool (UMCPToolRegistrySubsystem::*)(const FMCPRequestEnvelope&, FMCPToolExecutionResult&) const;
	struct FToolSpec
	{
		const TCHAR* Name;
		bool bWriteTool = false;
		FToolHandler Handler = nullptr;
	};

	static const FToolSpec ToolSpecs[] = {
		{ TEXT("tools.list"), false, &UMCPToolRegistrySubsystem::HandleToolsList },
		{ TEXT("system.health"), false, &UMCPToolRegistrySubsystem::HandleSystemHealth },
		{ TEXT("editor.livecoding.compile"), true, &UMCPToolRegistrySubsystem::HandleEditorLiveCodingCompile },
		{ TEXT("asset.find"), false, &UMCPToolRegistrySubsystem::HandleAssetFind },
		{ TEXT("asset.load"), false, &UMCPToolRegistrySubsystem::HandleAssetLoad },
		{ TEXT("asset.save"), true, &UMCPToolRegistrySubsystem::HandleAssetSave },
		{ TEXT("asset.import"), true, &UMCPToolRegistrySubsystem::HandleAssetImport },
		{ TEXT("asset.create"), true, &UMCPToolRegistrySubsystem::HandleAssetCreate },
		{ TEXT("blueprint.class.create"), true, &UMCPToolRegistrySubsystem::HandleBlueprintClassCreate },
		{ TEXT("asset.duplicate"), true, &UMCPToolRegistrySubsystem::HandleAssetDuplicate },
		{ TEXT("asset.rename"), true, &UMCPToolRegistrySubsystem::HandleAssetRename },
		{ TEXT("asset.delete"), true, &UMCPToolRegistrySubsystem::HandleAssetDelete },
		{ TEXT("settings.project.get"), false, &UMCPToolRegistrySubsystem::HandleSettingsProjectGet },
		{ TEXT("settings.project.patch"), true, &UMCPToolRegistrySubsystem::HandleSettingsProjectPatch },
		{ TEXT("settings.project.apply"), true, &UMCPToolRegistrySubsystem::HandleSettingsProjectApply },
		{ TEXT("settings.gamemode.get"), false, &UMCPToolRegistrySubsystem::HandleSettingsGameModeGet },
		{ TEXT("settings.gamemode.set_default"), true, &UMCPToolRegistrySubsystem::HandleSettingsGameModeSetDefault },
		{ TEXT("settings.gamemode.compose"), true, &UMCPToolRegistrySubsystem::HandleSettingsGameModeCompose },
		{ TEXT("settings.gamemode.set_map_override"), true, &UMCPToolRegistrySubsystem::HandleSettingsGameModeSetMapOverride },
		{ TEXT("settings.gamemode.remove_map_override"), true, &UMCPToolRegistrySubsystem::HandleSettingsGameModeRemoveMapOverride },
		{ TEXT("object.inspect"), false, &UMCPToolRegistrySubsystem::HandleObjectInspect },
		{ TEXT("object.patch"), true, &UMCPToolRegistrySubsystem::HandleObjectPatch },
		{ TEXT("object.patch.v2"), true, &UMCPToolRegistrySubsystem::HandleObjectPatchV2 },
		{ TEXT("world.outliner.list"), false, &UMCPToolRegistrySubsystem::HandleWorldOutlinerList },
		{ TEXT("world.selection.get"), false, &UMCPToolRegistrySubsystem::HandleWorldSelectionGet },
		{ TEXT("world.selection.set"), false, &UMCPToolRegistrySubsystem::HandleWorldSelectionSet },
		{ TEXT("mat.instance.params.get"), false, &UMCPToolRegistrySubsystem::HandleMatInstanceParamsGet },
		{ TEXT("mat.instance.params.set"), true, &UMCPToolRegistrySubsystem::HandleMatInstanceParamsSet },
		{ TEXT("niagara.params.get"), false, &UMCPToolRegistrySubsystem::HandleNiagaraParamsGet },
		{ TEXT("niagara.params.set"), true, &UMCPToolRegistrySubsystem::HandleNiagaraParamsSet },
		{ TEXT("niagara.stack.list"), false, &UMCPToolRegistrySubsystem::HandleNiagaraStackList },
		{ TEXT("niagara.stack.module.set_param"), true, &UMCPToolRegistrySubsystem::HandleNiagaraStackModuleSetParam },
		{ TEXT("seq.asset.create"), true, &UMCPToolRegistrySubsystem::HandleSeqAssetCreate },
		{ TEXT("seq.asset.load"), false, &UMCPToolRegistrySubsystem::HandleSeqAssetLoad },
		{ TEXT("seq.inspect"), false, &UMCPToolRegistrySubsystem::HandleSeqInspect },
		{ TEXT("seq.binding.list"), false, &UMCPToolRegistrySubsystem::HandleSeqBindingList },
		{ TEXT("seq.track.list"), false, &UMCPToolRegistrySubsystem::HandleSeqTrackList },
		{ TEXT("seq.section.list"), false, &UMCPToolRegistrySubsystem::HandleSeqSectionList },
		{ TEXT("seq.channel.list"), false, &UMCPToolRegistrySubsystem::HandleSeqChannelList },
		{ TEXT("seq.binding.add"), true, &UMCPToolRegistrySubsystem::HandleSeqBindingAdd },
		{ TEXT("seq.binding.remove"), true, &UMCPToolRegistrySubsystem::HandleSeqBindingRemove },
		{ TEXT("seq.track.add"), true, &UMCPToolRegistrySubsystem::HandleSeqTrackAdd },
		{ TEXT("seq.track.remove"), true, &UMCPToolRegistrySubsystem::HandleSeqTrackRemove },
		{ TEXT("seq.section.add"), true, &UMCPToolRegistrySubsystem::HandleSeqSectionAdd },
		{ TEXT("seq.section.patch"), true, &UMCPToolRegistrySubsystem::HandleSeqSectionPatch },
		{ TEXT("seq.section.remove"), true, &UMCPToolRegistrySubsystem::HandleSeqSectionRemove },
		{ TEXT("seq.key.set"), true, &UMCPToolRegistrySubsystem::HandleSeqKeySet },
		{ TEXT("seq.key.remove"), true, &UMCPToolRegistrySubsystem::HandleSeqKeyRemove },
		{ TEXT("seq.key.bulk_set"), true, &UMCPToolRegistrySubsystem::HandleSeqKeyBulkSet },
		{ TEXT("seq.object.inspect"), false, &UMCPToolRegistrySubsystem::HandleObjectInspect },
		{ TEXT("seq.object.patch.v2"), true, &UMCPToolRegistrySubsystem::HandleObjectPatchV2 },
		{ TEXT("seq.playback.patch"), true, &UMCPToolRegistrySubsystem::HandleSeqPlaybackPatch },
		{ TEXT("seq.save"), true, &UMCPToolRegistrySubsystem::HandleSeqSave },
		{ TEXT("seq.validate"), false, &UMCPToolRegistrySubsystem::HandleSeqValidate },
		{ TEXT("umg.blueprint.create"), true, &UMCPToolRegistrySubsystem::HandleUMGBlueprintCreate },
		{ TEXT("umg.blueprint.patch"), true, &UMCPToolRegistrySubsystem::HandleUMGBlueprintPatch },
		{ TEXT("umg.blueprint.reparent"), true, &UMCPToolRegistrySubsystem::HandleUMGBlueprintReparent },
		{ TEXT("umg.widget.class.list"), false, &UMCPToolRegistrySubsystem::HandleUMGWidgetClassList },
		{ TEXT("umg.tree.get"), false, &UMCPToolRegistrySubsystem::HandleUMGTreeGet },
		{ TEXT("umg.widget.inspect"), false, &UMCPToolRegistrySubsystem::HandleUMGWidgetInspect },
		{ TEXT("umg.slot.inspect"), false, &UMCPToolRegistrySubsystem::HandleUMGSlotInspect },
		{ TEXT("umg.widget.add"), true, &UMCPToolRegistrySubsystem::HandleUMGWidgetAdd },
		{ TEXT("umg.widget.remove"), true, &UMCPToolRegistrySubsystem::HandleUMGWidgetRemove },
		{ TEXT("umg.widget.reparent"), true, &UMCPToolRegistrySubsystem::HandleUMGWidgetReparent },
		{ TEXT("umg.widget.patch"), true, &UMCPToolRegistrySubsystem::HandleUMGWidgetPatch },
		{ TEXT("umg.widget.patch.v2"), true, &UMCPToolRegistrySubsystem::HandleUMGWidgetPatchV2 },
		{ TEXT("umg.slot.patch"), true, &UMCPToolRegistrySubsystem::HandleUMGSlotPatch },
		{ TEXT("umg.slot.patch.v2"), true, &UMCPToolRegistrySubsystem::HandleUMGSlotPatchV2 },
		{ TEXT("umg.animation.list"), false, &UMCPToolRegistrySubsystem::HandleUMGAnimationList },
		{ TEXT("umg.animation.create"), true, &UMCPToolRegistrySubsystem::HandleUMGAnimationCreate },
		{ TEXT("umg.animation.remove"), true, &UMCPToolRegistrySubsystem::HandleUMGAnimationRemove },
		{ TEXT("umg.animation.track.add"), true, &UMCPToolRegistrySubsystem::HandleUMGAnimationTrackAdd },
		{ TEXT("umg.animation.key.set"), true, &UMCPToolRegistrySubsystem::HandleUMGAnimationKeySet },
		{ TEXT("umg.animation.key.remove"), true, &UMCPToolRegistrySubsystem::HandleUMGAnimationKeyRemove },
		{ TEXT("umg.binding.list"), false, &UMCPToolRegistrySubsystem::HandleUMGBindingList },
		{ TEXT("umg.binding.set"), true, &UMCPToolRegistrySubsystem::HandleUMGBindingSet },
		{ TEXT("umg.binding.clear"), true, &UMCPToolRegistrySubsystem::HandleUMGBindingClear },
		{ TEXT("umg.widget.event.bind"), true, &UMCPToolRegistrySubsystem::HandleUMGWidgetEventBind },
		{ TEXT("umg.widget.event.unbind"), true, &UMCPToolRegistrySubsystem::HandleUMGWidgetEventUnbind },
		{ TEXT("umg.graph.summary"), false, &UMCPToolRegistrySubsystem::HandleUMGGraphSummary },
		{ TEXT("changeset.list"), false, &UMCPToolRegistrySubsystem::HandleChangeSetList },
		{ TEXT("changeset.get"), false, &UMCPToolRegistrySubsystem::HandleChangeSetGet },
		{ TEXT("changeset.rollback.preview"), false, &UMCPToolRegistrySubsystem::HandleChangeSetRollbackPreview },
		{ TEXT("changeset.rollback.apply"), true, &UMCPToolRegistrySubsystem::HandleChangeSetRollbackApply },
		{ TEXT("job.get"), false, &UMCPToolRegistrySubsystem::HandleJobGet },
		{ TEXT("job.cancel"), true, &UMCPToolRegistrySubsystem::HandleJobCancel },
	};

	for (const FToolSpec& ToolSpec : ToolSpecs)
	{
		const FToolHandler Handler = ToolSpec.Handler;
		RegisterTool({
			ToolSpec.Name,
			TEXT(""),
			TEXT("1.0.0"),
			true,
			ToolSpec.bWriteTool,
			nullptr,
			nullptr,
			[this, Handler](const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
			{
				return (this->*Handler)(Request, OutResult);
			}
		});
	}
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
	return FMCPToolsCoreHandler::HandleToolsList(*this, Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSystemHealth(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsCoreHandler::HandleSystemHealth(*this, RegisteredTools.Num(), Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleEditorLiveCodingCompile(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsCoreHandler::HandleEditorLiveCodingCompile(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleAssetFind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsAssetQueryHandler::HandleFind(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleAssetLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsAssetQueryHandler::HandleLoad(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleAssetSave(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsAssetQueryHandler::HandleSave(
		Request,
		OutResult,
		[](const FString& InputPath) { return ResolvePackageByName(InputPath); },
		[](const FString& InputPath) { return NormalizePackageNameFromInput(InputPath); });
}

bool UMCPToolRegistrySubsystem::HandleAssetImport(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsAssetLifecycleHandler::HandleImport(
		Request,
		OutResult,
		[](const FString& PackagePath, const FString& AssetName)
		{
			return IsValidAssetDestination(PackagePath, AssetName);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return BuildPackageName(PackagePath, AssetName);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return BuildObjectPath(PackagePath, AssetName);
		},
		[](const FString& InputPath)
		{
			return NormalizePackageNameFromInput(InputPath);
		},
		[](const FString& ObjectPath)
		{
			return ResolveObjectByPath(ObjectPath);
		},
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleAssetCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsAssetLifecycleHandler::HandleCreate(
		Request,
		OutResult,
		[](const FString& PackagePath, const FString& AssetName)
		{
			return IsValidAssetDestination(PackagePath, AssetName);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return BuildPackageName(PackagePath, AssetName);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return BuildObjectPath(PackagePath, AssetName);
		},
		[](const FString& ObjectPath)
		{
			return ResolveObjectByPath(ObjectPath);
		},
		[](const FString& ClassPath)
		{
			return ResolveClassByPath(ClassPath);
		},
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
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

bool UMCPToolRegistrySubsystem::HandleUMGBlueprintCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	FString PackagePath;
	FString AssetName;
	FString ParentClassPath = TEXT("/Script/UMG.UserWidget");
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
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("package_path and asset_name are required for umg.blueprint.create.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!IsValidAssetDestination(PackagePath, AssetName))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_PATH_INVALID;
		Diagnostic.Message = TEXT("Invalid package_path or asset_name for umg.blueprint.create.");
		Diagnostic.Detail = FString::Printf(TEXT("package_path=%s asset_name=%s"), *PackagePath, *AssetName);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* ParentClass = ResolveClassByPath(ParentClassPath);
	if (ParentClass == nullptr || !ParentClass->IsChildOf(UUserWidget::StaticClass()))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("parent_class_path must resolve to a UUserWidget-derived class.");
		Diagnostic.Detail = ParentClassPath;
		Diagnostic.Suggestion = TEXT("Use /Script/UMG.UserWidget or a Blueprintable UUserWidget subclass.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("Widget Blueprint cannot be created from the requested parent class.");
		Diagnostic.Detail = ParentClass->GetPathName();
		Diagnostic.Suggestion = TEXT("Use a Blueprintable UUserWidget parent class.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPBlueprintCreateOptions CreateOptions;
	CreateOptions.PackagePath = PackagePath;
	CreateOptions.AssetName = AssetName;
	CreateOptions.ParentClass = ParentClass;
	CreateOptions.BlueprintAssetClass = UWidgetBlueprint::StaticClass();
	CreateOptions.BlueprintGeneratedClass = UWidgetBlueprintGeneratedClass::StaticClass();
	CreateOptions.bOverwrite = bOverwrite;
	CreateOptions.bDryRun = Request.Context.bDryRun;
	CreateOptions.bCompileOnSuccess = bCompileOnSuccess;
	CreateOptions.bAutoSave = bAutoSave;
	CreateOptions.TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP UMG Blueprint Create"));

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

bool UMCPToolRegistrySubsystem::HandleUMGBlueprintPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleBlueprintPatch(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGBlueprintReparent(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleBlueprintReparent(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](const FString& ClassPath) { return ResolveClassByPath(ClassPath); },
		[](const FString& ObjectPath) { return BuildGeneratedClassPathFromObjectPath(ObjectPath); },
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeCompose(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSettingsComposeHandler::HandleGameModeCompose(
		Request,
		OutResult,
		[](const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
		{
			return ParseAutoSaveOption(Params, bOutAutoSave);
		},
		[](const TSharedPtr<FJsonObject>& Params, FMCPToolSettingsSaveOptions& OutOptions)
		{
			FMCPSettingsSaveOptions SaveOptions;
			const bool bParsed = ParseSettingsSaveOptions(Params, SaveOptions);
			OutOptions.bSaveConfig = SaveOptions.bSaveConfig;
			OutOptions.bFlushIni = SaveOptions.bFlushIni;
			OutOptions.bReloadVerify = SaveOptions.bReloadVerify;
			return bParsed;
		},
		[](const TSharedPtr<FJsonObject>& Params, const FString& DefaultLabel)
		{
			return ParseTransactionLabel(Params, DefaultLabel);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return IsValidAssetDestination(PackagePath, AssetName);
		},
		[](const FString& ClassPath)
		{
			return ResolveClassByPath(ClassPath);
		},
		[](const FMCPComposeBlueprintRequest& InRequest, FMCPToolExecutionResult& InOutResult, FMCPComposeBlueprintResult& OutCreateResult, FMCPDiagnostic& OutDiagnostic)
		{
			FMCPBlueprintCreateOptions CreateOptions;
			CreateOptions.PackagePath = InRequest.PackagePath;
			CreateOptions.AssetName = InRequest.AssetName;
			CreateOptions.ParentClass = InRequest.ParentClass;
			CreateOptions.bOverwrite = InRequest.bOverwrite;
			CreateOptions.bDryRun = InRequest.bDryRun;
			CreateOptions.bCompileOnSuccess = InRequest.bCompileOnSuccess;
			CreateOptions.bAutoSave = InRequest.bAutoSave;
			CreateOptions.TransactionLabel = InRequest.TransactionLabel;

			FMCPBlueprintCreateResult CreateResult;
			const bool bSucceeded = EnsureBlueprintClassAsset(CreateOptions, InOutResult, CreateResult, OutDiagnostic);
			if (!bSucceeded)
			{
				return false;
			}

			OutCreateResult.bCreatedNew = CreateResult.bCreatedNew;
			OutCreateResult.BlueprintAsset = CreateResult.BlueprintAsset;
			OutCreateResult.ObjectPath = CreateResult.ObjectPath;
			OutCreateResult.PackageName = CreateResult.PackageName;
			OutCreateResult.ParentClassPath = CreateResult.ParentClassPath;
			OutCreateResult.GeneratedClassPath = CreateResult.GeneratedClassPath;
			OutCreateResult.bSaved = CreateResult.bSaved;
			return true;
		},
		[](const FString& RequestedClassPath, UClass* RequiredBaseClass, FString& OutResolvedClassPath, UClass*& OutResolvedClass, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveClassPathWithBase(RequestedClassPath, RequiredBaseClass, OutResolvedClassPath, OutResolvedClass, OutDiagnostic);
		},
		[](UObject* TargetObject, const FString& PropertyName, UClass* ValueClass, FMCPDiagnostic& OutDiagnostic)
		{
			return SetClassPropertyOnObject(TargetObject, PropertyName, ValueClass, OutDiagnostic);
		},
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		},
		[this](const FMCPRequestEnvelope& InRequest, FMCPToolExecutionResult& InOutResult)
		{
			return HandleSettingsGameModeSetDefault(InRequest, InOutResult);
		},
		[this](const FMCPRequestEnvelope& InRequest, FMCPToolExecutionResult& InOutResult)
		{
			return HandleSettingsGameModeSetMapOverride(InRequest, InOutResult);
		},
		[](const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FString>& OutValues)
		{
			return ParseStringArrayField(Params, FieldName, OutValues);
		});
}

bool UMCPToolRegistrySubsystem::HandleAssetDuplicate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsAssetLifecycleHandler::HandleDuplicate(
		Request,
		OutResult,
		[](const FString& DestPackagePath, const FString& DestAssetName)
		{
			return IsValidAssetDestination(DestPackagePath, DestAssetName);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return BuildPackageName(PackagePath, AssetName);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return BuildObjectPath(PackagePath, AssetName);
		},
		[](const FString& InputPath)
		{
			return NormalizePackageNameFromInput(InputPath);
		},
		[](const FString& ObjectPath)
		{
			return ResolveObjectByPath(ObjectPath);
		},
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleAssetRename(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsAssetLifecycleHandler::HandleRename(
		Request,
		OutResult,
		[](const FString& PackagePath, const FString& AssetName)
		{
			return IsValidAssetDestination(PackagePath, AssetName);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return BuildPackageName(PackagePath, AssetName);
		},
		[](const FString& PackagePath, const FString& AssetName)
		{
			return BuildObjectPath(PackagePath, AssetName);
		},
		[](const FString& InputPath)
		{
			return NormalizePackageNameFromInput(InputPath);
		},
		[](const FString& ObjectPath)
		{
			return ResolveObjectByPath(ObjectPath);
		},
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleAssetDelete(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsAssetLifecycleHandler::HandleDelete(
		Request,
		OutResult,
		[](const FString& InputPath)
		{
			return NormalizePackageNameFromInput(InputPath);
		},
		[](const FString& ObjectPath)
		{
			return ResolveObjectByPath(ObjectPath);
		},
		[this](const TArray<FString>& ObjectPaths, const bool bFailIfReferenced)
		{
			return BuildDeleteConfirmationToken(ObjectPaths, bFailIfReferenced);
		},
		[this](const FString& Token, const TArray<FString>& ObjectPaths, const bool bFailIfReferenced)
		{
			return ConsumeDeleteConfirmationToken(Token, ObjectPaths, bFailIfReferenced);
		});
}

bool UMCPToolRegistrySubsystem::HandleSettingsProjectGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSettingsProjectHandler::HandleProjectGet(
		Request,
		OutResult,
		[](const FString& ClassPath, UClass*& OutClass, UObject*& OutConfigObject, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveSettingsClass(ClassPath, OutClass, OutConfigObject, OutDiagnostic);
		});
}

bool UMCPToolRegistrySubsystem::HandleSettingsProjectPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSettingsProjectHandler::HandleProjectPatch(
		Request,
		OutResult,
		[](const FString& ClassPath, UClass*& OutClass, UObject*& OutConfigObject, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveSettingsClass(ClassPath, OutClass, OutConfigObject, OutDiagnostic);
		},
		[](const TArray<TSharedPtr<FJsonValue>>* PatchOperations, UObject* TargetObject, TArray<FString>& OutChangedProperties, FMCPDiagnostic& OutDiagnostic)
		{
			return ParseTopLevelPatchProperties(PatchOperations, TargetObject, OutChangedProperties, OutDiagnostic);
		},
		[](const FString& ClassPath, const TArray<TSharedPtr<FJsonValue>>* PatchOperations, const FMCPToolSettingsSaveOptions& SaveOptions)
		{
			FMCPSettingsSaveOptions InternalSaveOptions;
			InternalSaveOptions.bSaveConfig = SaveOptions.bSaveConfig;
			InternalSaveOptions.bFlushIni = SaveOptions.bFlushIni;
			InternalSaveOptions.bReloadVerify = SaveOptions.bReloadVerify;
			return BuildSettingsPatchSignature(ClassPath, PatchOperations, InternalSaveOptions);
		},
		[this](const FString& Signature)
		{
			return BuildSettingsConfirmationToken(Signature);
		},
		[this](const FString& Token, const FString& Signature)
		{
			return ConsumeSettingsConfirmationToken(Token, Signature);
		},
		[](UObject* ConfigObject, const FMCPToolSettingsSaveOptions& SaveOptions, TArray<FString>& OutSavedConfigFiles, bool& bOutVerified, FMCPToolExecutionResult& InOutResult)
		{
			FMCPSettingsSaveOptions InternalSaveOptions;
			InternalSaveOptions.bSaveConfig = SaveOptions.bSaveConfig;
			InternalSaveOptions.bFlushIni = SaveOptions.bFlushIni;
			InternalSaveOptions.bReloadVerify = SaveOptions.bReloadVerify;
			return PersistConfigObject(ConfigObject, InternalSaveOptions, OutSavedConfigFiles, bOutVerified, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleSettingsProjectApply(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSettingsProjectHandler::HandleProjectApply(
		Request,
		OutResult,
		[](const FString& ClassPath, UClass*& OutClass, UObject*& OutConfigObject, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveSettingsClass(ClassPath, OutClass, OutConfigObject, OutDiagnostic);
		},
		[](const TArray<TSharedPtr<FJsonValue>>* PatchOperations, UObject* TargetObject, TArray<FString>& OutChangedProperties, FMCPDiagnostic& OutDiagnostic)
		{
			return ParseTopLevelPatchProperties(PatchOperations, TargetObject, OutChangedProperties, OutDiagnostic);
		},
		[](const FString& ClassPath, const TArray<TSharedPtr<FJsonValue>>* PatchOperations, const FMCPToolSettingsSaveOptions& SaveOptions)
		{
			FMCPSettingsSaveOptions InternalSaveOptions;
			InternalSaveOptions.bSaveConfig = SaveOptions.bSaveConfig;
			InternalSaveOptions.bFlushIni = SaveOptions.bFlushIni;
			InternalSaveOptions.bReloadVerify = SaveOptions.bReloadVerify;
			return BuildSettingsPatchSignature(ClassPath, PatchOperations, InternalSaveOptions);
		},
		[this](const FString& Signature)
		{
			return BuildSettingsConfirmationToken(Signature);
		},
		[this](const FString& Token, const FString& Signature)
		{
			return ConsumeSettingsConfirmationToken(Token, Signature);
		},
		[](UObject* ConfigObject, const FMCPToolSettingsSaveOptions& SaveOptions, TArray<FString>& OutSavedConfigFiles, bool& bOutVerified, FMCPToolExecutionResult& InOutResult)
		{
			FMCPSettingsSaveOptions InternalSaveOptions;
			InternalSaveOptions.bSaveConfig = SaveOptions.bSaveConfig;
			InternalSaveOptions.bFlushIni = SaveOptions.bFlushIni;
			InternalSaveOptions.bReloadVerify = SaveOptions.bReloadVerify;
			return PersistConfigObject(ConfigObject, InternalSaveOptions, OutSavedConfigFiles, bOutVerified, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSettingsGameModeHandler::HandleGameModeGet(
		Request,
		OutResult,
		[](UObject* ConfigObject, const FString& PropertyName)
		{
			return GetSettingsStringProperty(ConfigObject, PropertyName);
		},
		[](UObject* ConfigObject, TArray<TSharedPtr<FJsonValue>>& OutRawArray)
		{
			return GetGameModeMapOverrideRawArray(ConfigObject, OutRawArray);
		},
		[](const TArray<TSharedPtr<FJsonValue>>& RawArray)
		{
			return BuildMapOverrideEntriesFromRaw(RawArray);
		});
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeSetDefault(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSettingsGameModeHandler::HandleGameModeSetDefault(
		Request,
		OutResult,
		[](const FString& InputClassPath, FString& OutResolvedPath, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveGameModeClassPath(InputClassPath, OutResolvedPath, OutDiagnostic);
		},
		[](UObject* ConfigObject, const FString& PropertyName)
		{
			return GetSettingsStringProperty(ConfigObject, PropertyName);
		},
		[](UObject* ConfigObject, const FMCPToolSettingsSaveOptions& SaveOptions, TArray<FString>& OutSavedConfigFiles, bool& bOutVerified, FMCPToolExecutionResult& InOutResult)
		{
			FMCPSettingsSaveOptions InternalSaveOptions;
			InternalSaveOptions.bSaveConfig = SaveOptions.bSaveConfig;
			InternalSaveOptions.bFlushIni = SaveOptions.bFlushIni;
			InternalSaveOptions.bReloadVerify = SaveOptions.bReloadVerify;
			return PersistConfigObject(ConfigObject, InternalSaveOptions, OutSavedConfigFiles, bOutVerified, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeSetMapOverride(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSettingsGameModeHandler::HandleGameModeSetMapOverride(
		Request,
		OutResult,
		[](const FString& InputClassPath, FString& OutResolvedPath, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveGameModeClassPath(InputClassPath, OutResolvedPath, OutDiagnostic);
		},
		[](UObject* ConfigObject, TArray<TSharedPtr<FJsonValue>>& OutRawArray)
		{
			return GetGameModeMapOverrideRawArray(ConfigObject, OutRawArray);
		},
		[](const TSharedPtr<FJsonObject>& RawEntry, FString& OutClassPath)
		{
			return TryGetMapOverrideGameModeClassPath(RawEntry, OutClassPath);
		},
		[](const FString& ClassPath)
		{
			return BuildSoftClassPathJsonValue(ClassPath);
		},
		[](const TArray<TSharedPtr<FJsonValue>>& RawArray)
		{
			return BuildMapOverrideEntriesFromRaw(RawArray);
		},
		[](const TArray<TSharedPtr<FJsonValue>>* PatchOperations, UObject* TargetObject, TArray<FString>& OutChangedProperties, FMCPDiagnostic& OutDiagnostic)
		{
			return ParseTopLevelPatchProperties(PatchOperations, TargetObject, OutChangedProperties, OutDiagnostic);
		},
		[](UObject* ConfigObject, const FMCPToolSettingsSaveOptions& SaveOptions, TArray<FString>& OutSavedConfigFiles, bool& bOutVerified, FMCPToolExecutionResult& InOutResult)
		{
			FMCPSettingsSaveOptions InternalSaveOptions;
			InternalSaveOptions.bSaveConfig = SaveOptions.bSaveConfig;
			InternalSaveOptions.bFlushIni = SaveOptions.bFlushIni;
			InternalSaveOptions.bReloadVerify = SaveOptions.bReloadVerify;
			return PersistConfigObject(ConfigObject, InternalSaveOptions, OutSavedConfigFiles, bOutVerified, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleSettingsGameModeRemoveMapOverride(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSettingsGameModeHandler::HandleGameModeRemoveMapOverride(
		Request,
		OutResult,
		[](UObject* ConfigObject, TArray<TSharedPtr<FJsonValue>>& OutRawArray)
		{
			return GetGameModeMapOverrideRawArray(ConfigObject, OutRawArray);
		},
		[](const TArray<TSharedPtr<FJsonValue>>& RawArray)
		{
			return BuildMapOverrideEntriesFromRaw(RawArray);
		},
		[](const TArray<TSharedPtr<FJsonValue>>* PatchOperations, UObject* TargetObject, TArray<FString>& OutChangedProperties, FMCPDiagnostic& OutDiagnostic)
		{
			return ParseTopLevelPatchProperties(PatchOperations, TargetObject, OutChangedProperties, OutDiagnostic);
		},
		[](UObject* ConfigObject, const FMCPToolSettingsSaveOptions& SaveOptions, TArray<FString>& OutSavedConfigFiles, bool& bOutVerified, FMCPToolExecutionResult& InOutResult)
		{
			FMCPSettingsSaveOptions InternalSaveOptions;
			InternalSaveOptions.bSaveConfig = SaveOptions.bSaveConfig;
			InternalSaveOptions.bFlushIni = SaveOptions.bFlushIni;
			InternalSaveOptions.bReloadVerify = SaveOptions.bReloadVerify;
			return PersistConfigObject(ConfigObject, InternalSaveOptions, OutSavedConfigFiles, bOutVerified, InOutResult);
		});
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
	return FMCPToolsObjectHandler::HandleInspect(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleObjectPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsObjectHandler::HandlePatch(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleWorldOutlinerList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsWorldHandler::HandleOutlinerList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleWorldSelectionGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsWorldHandler::HandleSelectionGet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleWorldSelectionSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsWorldHandler::HandleSelectionSet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleMatInstanceParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsMaterialHandler::HandleInstanceParamsGet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleMatInstanceParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsMaterialHandler::HandleInstanceParamsSet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleNiagaraParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsNiagaraHandler::HandleParamsGet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleNiagaraParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsNiagaraHandler::HandleParamsSet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleNiagaraStackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsNiagaraHandler::HandleStackList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleNiagaraStackModuleSetParam(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsNiagaraHandler::HandleStackModuleSetParam(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqAssetCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerReadHandler::HandleAssetCreate(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqAssetLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerReadHandler::HandleAssetLoad(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerReadHandler::HandleInspect(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqBindingList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerReadHandler::HandleBindingList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqTrackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerReadHandler::HandleTrackList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqSectionList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerReadHandler::HandleSectionList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqChannelList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerReadHandler::HandleChannelList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqBindingAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandleBindingAdd(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqBindingRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandleBindingRemove(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqTrackAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandleTrackAdd(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqTrackRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandleTrackRemove(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqSectionAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandleSectionAdd(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqSectionPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandleSectionPatch(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqSectionRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandleSectionRemove(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqKeySet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerKeyHandler::HandleKeySet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqKeyRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerKeyHandler::HandleKeyRemove(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqKeyBulkSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerKeyHandler::HandleKeyBulkSet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqPlaybackPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandlePlaybackPatch(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqSave(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerStructureHandler::HandleSave(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleSeqValidate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsSequencerValidationHandler::HandleValidate(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetClassList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGReadHandler::HandleWidgetClassList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleWidgetAdd(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRef)
		{
			return ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
		},
		[](const FString& WidgetClassPath)
		{
			return ResolveWidgetClassByPath(WidgetClassPath);
		},
		[](UWidgetBlueprint* WidgetBlueprint, UClass* WidgetClass, const FString& WidgetName)
		{
			return BuildUniqueWidgetName(WidgetBlueprint, WidgetClass, WidgetName);
		},
		[](const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
		{
			return GetWidgetStableId(WidgetBlueprint, Widget);
		},
		[](const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
		{
			return GetParentWidgetId(WidgetBlueprint, Widget);
		},
		[](const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget, FString* OutNamedSlotName)
		{
			return GetWidgetSlotType(WidgetBlueprint, Widget, OutNamedSlotName);
		},
		[](UPanelWidget* PanelWidget)
		{
			return GetPanelSlotTypeName(PanelWidget);
		},
		[](UWidgetBlueprint* WidgetBlueprint, const FString& RequestedSlotName, FName& OutResolvedSlotName, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveWidgetTreeNamedSlotName(WidgetBlueprint, RequestedSlotName, OutResolvedSlotName, OutDiagnostic);
		},
		[](UWidget* ParentWidget, const FString& RequestedSlotName, FName& OutResolvedSlotName, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveNamedSlotName(ParentWidget, RequestedSlotName, OutResolvedSlotName, OutDiagnostic);
		},
		[](UWidgetBlueprint* WidgetBlueprint,
			UWidget* ParentWidget,
			UWidget* ChildWidget,
			int32 InsertIndex,
			bool bReplaceContent,
			const FString& NamedSlotName,
			FString& OutResolvedNamedSlotName,
			FString& OutSlotType,
			FMCPDiagnostic& OutDiagnostic)
		{
			return AttachWidgetToParent(
				WidgetBlueprint,
				ParentWidget,
				ChildWidget,
				InsertIndex,
				bReplaceContent,
				NamedSlotName,
				OutResolvedNamedSlotName,
				OutSlotType,
				OutDiagnostic);
		},
		[](UWidgetBlueprint* WidgetBlueprint)
		{
			EnsureWidgetGuidMap(WidgetBlueprint);
		},
		[](UWidgetBlueprint* WidgetBlueprint, UWidget* Widget)
		{
			EnsureWidgetGuidEntry(WidgetBlueprint, Widget);
		},
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleWidgetRemove(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRef)
		{
			return ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
		},
		[](const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
		{
			return GetWidgetStableId(WidgetBlueprint, Widget);
		},
		[](const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
		{
			return GetParentWidgetId(WidgetBlueprint, Widget);
		},
		[](UWidgetBlueprint* WidgetBlueprint)
		{
			EnsureWidgetGuidMap(WidgetBlueprint);
		},
		[](UWidget* Widget, TArray<FName>& OutVariableNames)
		{
			CollectWidgetSubtreeVariableNames(Widget, OutVariableNames);
		},
		[](UWidgetBlueprint* WidgetBlueprint, const FName VariableName)
		{
			RemoveWidgetGuidEntry(WidgetBlueprint, VariableName);
		},
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetReparent(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleWidgetReparent(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRef)
		{
			return ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
		},
		[](const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
		{
			return GetWidgetStableId(WidgetBlueprint, Widget);
		},
		[](const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
		{
			return GetParentWidgetId(WidgetBlueprint, Widget);
		},
		[](const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget, FString* OutNamedSlotName)
		{
			return GetWidgetSlotType(WidgetBlueprint, Widget, OutNamedSlotName);
		},
		[](UPanelWidget* PanelWidget)
		{
			return GetPanelSlotTypeName(PanelWidget);
		},
		[](UWidgetBlueprint* WidgetBlueprint, const FString& RequestedSlotName, FName& OutResolvedSlotName, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveWidgetTreeNamedSlotName(WidgetBlueprint, RequestedSlotName, OutResolvedSlotName, OutDiagnostic);
		},
		[](UWidget* ParentWidget, const FString& RequestedSlotName, FName& OutResolvedSlotName, FMCPDiagnostic& OutDiagnostic)
		{
			return ResolveNamedSlotName(ParentWidget, RequestedSlotName, OutResolvedSlotName, OutDiagnostic);
		},
		[](UWidgetBlueprint* WidgetBlueprint,
			UWidget* ParentWidget,
			UWidget* ChildWidget,
			int32 InsertIndex,
			bool bReplaceContent,
			const FString& NamedSlotName,
			FString& OutResolvedNamedSlotName,
			FString& OutSlotType,
			FMCPDiagnostic& OutDiagnostic)
		{
			return AttachWidgetToParent(
				WidgetBlueprint,
				ParentWidget,
				ChildWidget,
				InsertIndex,
				bReplaceContent,
				NamedSlotName,
				OutResolvedNamedSlotName,
				OutSlotType,
				OutDiagnostic);
		},
		[](UWidgetBlueprint* WidgetBlueprint, UWidget* Widget, FMCPDiagnostic& OutDiagnostic)
		{
			return DetachWidgetFromParent(WidgetBlueprint, Widget, OutDiagnostic);
		},
		[](UWidgetBlueprint* WidgetBlueprint)
		{
			EnsureWidgetGuidMap(WidgetBlueprint);
		},
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGTreeGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGReadHandler::HandleTreeGet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGReadHandler::HandleWidgetInspect(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleUMGSlotInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGReadHandler::HandleSlotInspect(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleWidgetPatch(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRef)
		{
			return ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
		},
		[](UWidgetBlueprint* WidgetBlueprint) { EnsureWidgetGuidMap(WidgetBlueprint); });
}

bool UMCPToolRegistrySubsystem::HandleUMGSlotPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleSlotPatch(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRef)
		{
			return ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
		},
		[](UWidgetBlueprint* WidgetBlueprint) { EnsureWidgetGuidMap(WidgetBlueprint); });
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetPatchV2(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleWidgetPatchV2(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRef)
		{
			return ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
		},
		[](UWidgetBlueprint* WidgetBlueprint) { EnsureWidgetGuidMap(WidgetBlueprint); });
}

bool UMCPToolRegistrySubsystem::HandleUMGSlotPatchV2(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGStructureHandler::HandleSlotPatchV2(
		Request,
		OutResult,
		[](const FString& ObjectPath) { return LoadWidgetBlueprintByPath(ObjectPath); },
		[](UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRef)
		{
			return ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
		},
		[](UWidgetBlueprint* WidgetBlueprint) { EnsureWidgetGuidMap(WidgetBlueprint); });
}

bool UMCPToolRegistrySubsystem::HandleUMGAnimationList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGAnimationHandler::HandleAnimationList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleUMGAnimationCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGAnimationHandler::HandleAnimationCreate(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGAnimationRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGAnimationHandler::HandleAnimationRemove(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGAnimationTrackAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGAnimationHandler::HandleAnimationTrackAdd(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGAnimationKeySet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGAnimationHandler::HandleAnimationKeySet(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGAnimationKeyRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGAnimationHandler::HandleAnimationKeyRemove(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGBindingList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGReadHandler::HandleBindingList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleUMGBindingSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGBindingHandler::HandleBindingSet(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGBindingClear(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGBindingHandler::HandleBindingClear(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetEventBind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGBindingHandler::HandleWidgetEventBind(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGWidgetEventUnbind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGBindingHandler::HandleWidgetEventUnbind(
		Request,
		OutResult,
		[](const FString& PackageName, FMCPToolExecutionResult& InOutResult)
		{
			return SavePackageByName(PackageName, InOutResult);
		});
}

bool UMCPToolRegistrySubsystem::HandleUMGGraphSummary(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsUMGReadHandler::HandleGraphSummary(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleObjectPatchV2(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsObjectHandler::HandlePatchV2(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleChangeSetList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsOpsHandler::HandleChangeSetList(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleChangeSetGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsOpsHandler::HandleChangeSetGet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleChangeSetRollbackPreview(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsOpsHandler::HandleChangeSetRollbackPreview(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleChangeSetRollbackApply(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsOpsHandler::HandleChangeSetRollbackApply(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleJobGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsOpsHandler::HandleJobGet(Request, OutResult);
}

bool UMCPToolRegistrySubsystem::HandleJobCancel(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const
{
	return FMCPToolsOpsHandler::HandleJobCancel(Request, OutResult);
}
