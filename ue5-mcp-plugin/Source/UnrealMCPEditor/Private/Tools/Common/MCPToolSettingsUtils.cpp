#include "Tools/Common/MCPToolSettingsUtils.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "GameFramework/GameModeBase.h"
#include "Misc/PackageName.h"

namespace
{
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
}

bool MCPToolSettingsUtils::ParseAutoSaveOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
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

bool MCPToolSettingsUtils::ParseSettingsSaveOptions(const TSharedPtr<FJsonObject>& Params, FMCPSettingsSaveOptions& OutOptions)
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

FString MCPToolSettingsUtils::ParseTransactionLabel(const TSharedPtr<FJsonObject>& Params, const FString& DefaultLabel)
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

bool MCPToolSettingsUtils::ParseStringArrayField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FString>& OutValues)
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

FString MCPToolSettingsUtils::GetSettingsStringProperty(UObject* ConfigObject, const FString& PropertyName)
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

bool MCPToolSettingsUtils::GetGameModeMapOverrideRawArray(UObject* ConfigObject, TArray<TSharedPtr<FJsonValue>>& OutRawArray)
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

bool MCPToolSettingsUtils::TryGetMapOverrideGameModeClassPath(const TSharedPtr<FJsonObject>& RawEntry, FString& OutClassPath)
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

TSharedRef<FJsonValueObject> MCPToolSettingsUtils::BuildSoftClassPathJsonValue(const FString& ClassPath)
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

TArray<TSharedPtr<FJsonValue>> MCPToolSettingsUtils::BuildMapOverrideEntriesFromRaw(const TArray<TSharedPtr<FJsonValue>>& RawArray)
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

bool MCPToolSettingsUtils::ResolveGameModeClassPath(
	const FString& GameModeClassPath,
	TFunctionRef<UClass*(const FString&)> ResolveClassByPathFn,
	FString& OutResolvedPath,
	FMCPDiagnostic& OutDiagnostic)
{
	OutResolvedPath = GameModeClassPath;
	if (GameModeClassPath.IsEmpty())
	{
		return true;
	}

	UClass* ResolvedClass = ResolveClassByPathFn(GameModeClassPath);
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
