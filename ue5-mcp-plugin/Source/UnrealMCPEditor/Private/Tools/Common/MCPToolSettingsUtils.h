#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

class UObject;
class UClass;

struct FMCPSettingsSaveOptions
{
	bool bSaveConfig = true;
	bool bFlushIni = true;
	bool bReloadVerify = true;
};

namespace MCPToolSettingsUtils
{
	bool ParseAutoSaveOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave);
	bool ParseSettingsSaveOptions(const TSharedPtr<FJsonObject>& Params, FMCPSettingsSaveOptions& OutOptions);
	FString ParseTransactionLabel(const TSharedPtr<FJsonObject>& Params, const FString& DefaultLabel);
	bool ParseStringArrayField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FString>& OutValues);
	FString GetSettingsStringProperty(UObject* ConfigObject, const FString& PropertyName);
	bool GetGameModeMapOverrideRawArray(UObject* ConfigObject, TArray<TSharedPtr<FJsonValue>>& OutRawArray);
	bool TryGetMapOverrideGameModeClassPath(const TSharedPtr<FJsonObject>& RawEntry, FString& OutClassPath);
	TSharedRef<FJsonValueObject> BuildSoftClassPathJsonValue(const FString& ClassPath);
	TArray<TSharedPtr<FJsonValue>> BuildMapOverrideEntriesFromRaw(const TArray<TSharedPtr<FJsonValue>>& RawArray);
	bool ResolveGameModeClassPath(
		const FString& GameModeClassPath,
		TFunctionRef<UClass*(const FString&)> ResolveClassByPathFn,
		FString& OutResolvedPath,
		FMCPDiagnostic& OutDiagnostic);
}
