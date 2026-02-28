#include "Tools/Asset/MCPToolsAssetLifecycleHandler.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Factories/Factory.h"
#include "Factories/FbxImportUI.h"
#include "IAssetTools.h"
#include "MCPErrorCodes.h"
#include "ObjectTools.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "UObject/GarbageCollection.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
	void ParseStringArrayField(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Params.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString ParsedValue;
			if (Value.IsValid() && Value->TryGetString(ParsedValue))
			{
				OutValues.Add(ParsedValue);
			}
		}
	}

	void ParseAutoSaveOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
	{
		bOutAutoSave = false;
		if (!Params.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* SaveObject = nullptr;
		if (Params->TryGetObjectField(TEXT("save"), SaveObject) && SaveObject != nullptr && SaveObject->IsValid())
		{
			(*SaveObject)->TryGetBoolField(TEXT("auto_save"), bOutAutoSave);
		}
	}
}

bool FMCPToolsAssetLifecycleHandler::HandleImport(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FIsValidAssetDestinationFn IsValidAssetDestination,
	FBuildPackageNameFn BuildPackageName,
	FBuildObjectPathFn BuildObjectPath,
	FNormalizePackageNameFromInputFn NormalizePackageNameFromInput,
	FResolveObjectByPathFn ResolveObjectByPath,
	FSavePackageByNameFn SavePackageByName)
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
	OutResult.ResultObject->SetArrayField(TEXT("failed"), MCPToolCommonJson::ToJsonStringArray(FailedFiles));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
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

bool FMCPToolsAssetLifecycleHandler::HandleDuplicate(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FIsValidAssetDestinationFn IsValidAssetDestination,
	FBuildPackageNameFn BuildPackageName,
	FBuildObjectPathFn BuildObjectPath,
	FNormalizePackageNameFromInputFn NormalizePackageNameFromInput,
	FResolveObjectByPathFn ResolveObjectByPath,
	FSavePackageByNameFn SavePackageByName)
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
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsAssetLifecycleHandler::HandleCreate(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FIsValidAssetDestinationFn IsValidAssetDestination,
	FBuildPackageNameFn BuildPackageName,
	FBuildObjectPathFn BuildObjectPath,
	FResolveObjectByPathFn ResolveObjectByPath,
	FResolveClassByPathFn ResolveClassByPath,
	FSavePackageByNameFn SavePackageByName)
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

	AssetName = ObjectTools::SanitizeObjectName(AssetName);
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
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsAssetLifecycleHandler::HandleRename(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FIsValidAssetDestinationFn IsValidAssetDestination,
	FBuildPackageNameFn BuildPackageName,
	FBuildObjectPathFn BuildObjectPath,
	FNormalizePackageNameFromInputFn NormalizePackageNameFromInput,
	FResolveObjectByPathFn ResolveObjectByPath,
	FSavePackageByNameFn SavePackageByName)
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
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsAssetLifecycleHandler::HandleDelete(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FNormalizePackageNameFromInputFn NormalizePackageNameFromInput,
	FResolveObjectByPathFn ResolveObjectByPath,
	FBuildDeleteConfirmationTokenFn BuildDeleteConfirmationToken,
	FConsumeDeleteConfirmationTokenFn ConsumeDeleteConfirmationToken)
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
			BlockedEntry->SetArrayField(TEXT("referencers"), MCPToolCommonJson::ToJsonStringArray(ReferencerPackages));
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

		auto WasDeletedFromRegistryAndDisk = [&AssetRegistry, &NormalizePackageNameFromInput](const FString& CandidatePath, FString& OutFailureDetail) -> bool
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
	OutResult.ResultObject->SetArrayField(TEXT("deleted"), MCPToolCommonJson::ToJsonStringArray(Deleted));
	OutResult.ResultObject->SetArrayField(TEXT("failed"), MCPToolCommonJson::ToJsonStringArray(Failed));
	OutResult.ResultObject->SetArrayField(TEXT("blocked"), Blocked);
	OutResult.ResultObject->SetStringField(TEXT("confirm_token"), PreviewToken);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));

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
