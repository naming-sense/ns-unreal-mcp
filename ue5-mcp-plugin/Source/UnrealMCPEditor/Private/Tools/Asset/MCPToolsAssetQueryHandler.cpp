#include "Tools/Asset/MCPToolsAssetQueryHandler.h"

#include "MCPErrorCodes.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "FileHelpers.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

namespace
{
	bool MatchesAnyClassPath(const TSet<FString>& AllowedClassPaths, const FString& ClassPath)
	{
		return AllowedClassPaths.Num() == 0 || AllowedClassPaths.Contains(ClassPath);
	}
}

bool FMCPToolsAssetQueryHandler::HandleFind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString PathGlob = TEXT("/Game/**");
	FString NameGlob;
	int32 Limit = 50;
	const int32 Cursor = MCPToolCommonJson::ParseCursor(Request.Params);
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

bool FMCPToolsAssetQueryHandler::HandleLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
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

bool FMCPToolsAssetQueryHandler::HandleSave(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FResolvePackageByNameFn ResolvePackageByName,
	FNormalizePackageNameFn NormalizePackageNameFromInput)
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
	OutResult.ResultObject->SetArrayField(TEXT("saved"), MCPToolCommonJson::ToJsonStringArray(SavedPackages));
	OutResult.ResultObject->SetArrayField(TEXT("failed"), MCPToolCommonJson::ToJsonStringArray(FailedPackages));

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
