#include "MCPChangeSetSubsystem.h"

#include "MCPErrorCodes.h"
#include "MCPLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString ToIso8601Now()
	{
		return FDateTime::UtcNow().ToIso8601();
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonStringArrayForChangeSet(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> OutValues;
		OutValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			OutValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return OutValues;
	}
}

bool UMCPChangeSetSubsystem::CreateChangeSetRecord(
	const FMCPRequestEnvelope& Request,
	const FMCPToolExecutionResult& Result,
	const FString& PolicyVersion,
	const FString& SchemaHash,
	FString& OutChangeSetId,
	FMCPDiagnostic& OutDiagnostic) const
{
	OutChangeSetId = FString::Printf(TEXT("cs-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ChangeSetDir = BuildChangeSetDirectory(OutChangeSetId);
	const FString DomainDiffDir = FPaths::Combine(ChangeSetDir, TEXT("domain_diffs"));
	const FString SnapshotDir = FPaths::Combine(ChangeSetDir, TEXT("snapshots"));

	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.MakeDirectory(*SnapshotDir, true) || !FileManager.MakeDirectory(*DomainDiffDir, true))
	{
		OutDiagnostic.Code = MCPErrorCodes::SAVE_FAILED;
		OutDiagnostic.Message = TEXT("Failed to create changeset directories.");
		OutDiagnostic.Detail = ChangeSetDir;
		OutDiagnostic.Suggestion = TEXT("Check write permission for Saved/UnrealMCP.");
		return false;
	}

	TSharedRef<FJsonObject> MetaObject = MakeShared<FJsonObject>();
	MetaObject->SetStringField(TEXT("changeset_id"), OutChangeSetId);
	MetaObject->SetStringField(TEXT("request_id"), Request.RequestId);
	MetaObject->SetStringField(TEXT("session_id"), Request.SessionId);
	MetaObject->SetStringField(TEXT("tool"), Request.Tool);
	MetaObject->SetStringField(TEXT("created_at"), ToIso8601Now());
	MetaObject->SetStringField(TEXT("status"), MCPJson::StatusToString(Result.Status));
	MetaObject->SetStringField(TEXT("policy_version"), PolicyVersion);
	MetaObject->SetStringField(TEXT("schema_hash"), SchemaHash);
	MetaObject->SetStringField(TEXT("engine_version"), Request.Context.EngineVersion);
	MetaObject->SetArrayField(TEXT("touched_packages"), ToJsonStringArrayForChangeSet(Result.TouchedPackages));

	TArray<TSharedPtr<FJsonValue>> Targets;
	const TSharedPtr<FJsonObject>* TargetObject = nullptr;
	if (Request.Params.IsValid() && Request.Params->TryGetObjectField(TEXT("target"), TargetObject) && TargetObject != nullptr && TargetObject->IsValid())
	{
		Targets.Add(MakeShared<FJsonValueObject>((*TargetObject).ToSharedRef()));
	}
	MetaObject->SetArrayField(TEXT("targets"), Targets);

	const FString MetaFilePath = FPaths::Combine(ChangeSetDir, TEXT("meta.json"));
	if (!WriteJsonFile(MetaFilePath, MetaObject))
	{
		OutDiagnostic.Code = MCPErrorCodes::SAVE_FAILED;
		OutDiagnostic.Message = TEXT("Failed to write changeset meta.json");
		OutDiagnostic.Detail = MetaFilePath;
		OutDiagnostic.Suggestion = TEXT("Check disk status and retry.");
		return false;
	}

	const FString LogFilePath = FPaths::Combine(ChangeSetDir, TEXT("logs.jsonl"));
	FFileHelper::SaveStringToFile(TEXT(""), *LogFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	UE_LOG(LogUnrealMCP, Log, TEXT("Created changeset %s"), *OutChangeSetId);
	return true;
}

bool UMCPChangeSetSubsystem::ListChangeSets(
	const int32 Limit,
	const int32 Cursor,
	const TArray<FString>& StatusFilter,
	const FString& ToolGlob,
	const FString& SessionId,
	TArray<TSharedPtr<FJsonObject>>& OutItems,
	int32& OutNextCursor,
	FMCPDiagnostic& OutDiagnostic) const
{
	OutItems.Reset();
	OutNextCursor = -1;

	const FString RootDir = GetChangeSetRootDir();
	if (!IFileManager::Get().DirectoryExists(*RootDir))
	{
		return true;
	}

	TArray<FString> Directories;
	IFileManager::Get().FindFiles(Directories, *(FPaths::Combine(RootDir, TEXT("*"))), false, true);

	TArray<TSharedPtr<FJsonObject>> MetaItems;
	for (const FString& DirectoryName : Directories)
	{
		const FString MetaPath = FPaths::Combine(RootDir, DirectoryName, TEXT("meta.json"));
		TSharedPtr<FJsonObject> MetaObject;
		if (!ReadJsonFile(MetaPath, MetaObject))
		{
			continue;
		}

		FString Status;
		MetaObject->TryGetStringField(TEXT("status"), Status);
		if (StatusFilter.Num() > 0 && !StatusFilter.Contains(Status))
		{
			continue;
		}

		FString Tool;
		MetaObject->TryGetStringField(TEXT("tool"), Tool);
		if (!ToolGlob.IsEmpty() && !Tool.MatchesWildcard(ToolGlob))
		{
			continue;
		}

		FString ExistingSessionId;
		MetaObject->TryGetStringField(TEXT("session_id"), ExistingSessionId);
		if (!SessionId.IsEmpty() && ExistingSessionId != SessionId)
		{
			continue;
		}

		MetaItems.Add(MetaObject);
	}

	MetaItems.Sort([](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
	{
		FString LeftCreatedAt;
		FString RightCreatedAt;
		Left->TryGetStringField(TEXT("created_at"), LeftCreatedAt);
		Right->TryGetStringField(TEXT("created_at"), RightCreatedAt);
		return LeftCreatedAt > RightCreatedAt;
	});

	const int32 SafeCursor = FMath::Max(0, Cursor);
	const int32 SafeLimit = FMath::Clamp(Limit, 1, 200);
	if (SafeCursor >= MetaItems.Num())
	{
		return true;
	}

	const int32 EndIndex = FMath::Min(SafeCursor + SafeLimit, MetaItems.Num());
	for (int32 Index = SafeCursor; Index < EndIndex; ++Index)
	{
		OutItems.Add(MetaItems[Index]);
	}

	if (EndIndex < MetaItems.Num())
	{
		OutNextCursor = EndIndex;
	}

	return true;
}

bool UMCPChangeSetSubsystem::GetChangeSet(
	const FString& ChangeSetId,
	const bool bIncludeLogs,
	const bool bIncludeSnapshots,
	TSharedPtr<FJsonObject>& OutResult,
	FMCPDiagnostic& OutDiagnostic) const
{
	const FString ChangeSetDir = BuildChangeSetDirectory(ChangeSetId);
	const FString MetaPath = FPaths::Combine(ChangeSetDir, TEXT("meta.json"));

	TSharedPtr<FJsonObject> MetaObject;
	if (!ReadJsonFile(MetaPath, MetaObject))
	{
		OutDiagnostic.Code = MCPErrorCodes::CHANGESET_NOT_FOUND;
		OutDiagnostic.Message = TEXT("Requested changeset does not exist.");
		OutDiagnostic.Detail = ChangeSetId;
		OutDiagnostic.Suggestion = TEXT("Run changeset.list and retry with a valid changeset_id.");
		return false;
	}

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetStringField(TEXT("changeset_id"), ChangeSetId);
	OutResult->SetObjectField(TEXT("meta"), MetaObject.ToSharedRef());

	const TArray<TSharedPtr<FJsonValue>>* TouchedPackages = nullptr;
	if (!MetaObject->TryGetArrayField(TEXT("touched_packages"), TouchedPackages) || TouchedPackages == nullptr)
	{
		TArray<TSharedPtr<FJsonValue>> EmptyTouchedPackages;
		OutResult->SetArrayField(TEXT("touched_packages"), EmptyTouchedPackages);
	}
	else
	{
		OutResult->SetArrayField(TEXT("touched_packages"), *TouchedPackages);
	}

	TArray<TSharedPtr<FJsonValue>> Logs;
	if (bIncludeLogs)
	{
		const FString LogFilePath = FPaths::Combine(ChangeSetDir, TEXT("logs.jsonl"));
		FString RawLogs;
		if (FFileHelper::LoadFileToString(RawLogs, *LogFilePath))
		{
			TArray<FString> Lines;
			RawLogs.ParseIntoArrayLines(Lines, true);
			for (const FString& Line : Lines)
			{
				TSharedPtr<FJsonObject> LogObject;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
				if (FJsonSerializer::Deserialize(Reader, LogObject) && LogObject.IsValid())
				{
					Logs.Add(MakeShared<FJsonValueObject>(LogObject));
				}
			}
		}
	}
	OutResult->SetArrayField(TEXT("logs"), Logs);

	TArray<TSharedPtr<FJsonValue>> Snapshots;
	if (bIncludeSnapshots)
	{
		const FString SnapshotDir = FPaths::Combine(ChangeSetDir, TEXT("snapshots"));
		TArray<FString> SnapshotFiles;
		IFileManager::Get().FindFiles(SnapshotFiles, *(FPaths::Combine(SnapshotDir, TEXT("*.before"))), true, false);
		for (const FString& SnapshotFile : SnapshotFiles)
		{
			Snapshots.Add(MakeShared<FJsonValueString>(FPaths::Combine(SnapshotDir, SnapshotFile)));
		}
	}
	OutResult->SetArrayField(TEXT("snapshots"), Snapshots);
	return true;
}

bool UMCPChangeSetSubsystem::PreviewRollback(
	const FString& ChangeSetId,
	const FString& Mode,
	TSharedPtr<FJsonObject>& OutResult,
	FMCPDiagnostic& OutDiagnostic) const
{
	TSharedPtr<FJsonObject> ChangeSetInfo;
	if (!GetChangeSet(ChangeSetId, false, true, ChangeSetInfo, OutDiagnostic))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PackageValues = nullptr;
	ChangeSetInfo->TryGetArrayField(TEXT("touched_packages"), PackageValues);
	TArray<TSharedPtr<FJsonValue>> PackageValuesCopy = PackageValues != nullptr ? *PackageValues : TArray<TSharedPtr<FJsonValue>>();

	TArray<TSharedPtr<FJsonValue>> MissingSnapshots;
	if (Mode.Equals(TEXT("local_snapshot"), ESearchCase::IgnoreCase))
	{
		const TArray<TSharedPtr<FJsonValue>>* ExistingSnapshots = nullptr;
		ChangeSetInfo->TryGetArrayField(TEXT("snapshots"), ExistingSnapshots);
		const int32 SnapshotCount = ExistingSnapshots != nullptr ? ExistingSnapshots->Num() : 0;
		if (SnapshotCount == 0 && PackageValuesCopy.Num() > 0)
		{
			MissingSnapshots = PackageValuesCopy;
		}
	}

	OutResult = MakeShared<FJsonObject>();
	OutResult->SetStringField(TEXT("changeset_id"), ChangeSetId);
	OutResult->SetStringField(TEXT("mode"), Mode);

	TSharedRef<FJsonObject> ImpactObject = MakeShared<FJsonObject>();
	ImpactObject->SetArrayField(TEXT("packages"), PackageValuesCopy);
	ImpactObject->SetArrayField(TEXT("missing_snapshots"), MissingSnapshots);
	TArray<TSharedPtr<FJsonValue>> Conflicts;
	ImpactObject->SetArrayField(TEXT("conflicts"), Conflicts);
	OutResult->SetObjectField(TEXT("impact"), ImpactObject);
	return true;
}

bool UMCPChangeSetSubsystem::ApplyRollback(
	const FString& ChangeSetId,
	const FString& Mode,
	const bool bForce,
	TArray<FString>& OutTouchedPackages,
	bool& bOutApplied,
	FMCPDiagnostic& OutDiagnostic) const
{
	OutTouchedPackages.Reset();
	bOutApplied = false;

	TSharedPtr<FJsonObject> PreviewObject;
	if (!PreviewRollback(ChangeSetId, Mode, PreviewObject, OutDiagnostic))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* ImpactObject = nullptr;
	if (!PreviewObject->TryGetObjectField(TEXT("impact"), ImpactObject) || ImpactObject == nullptr || !ImpactObject->IsValid())
	{
		OutDiagnostic.Code = MCPErrorCodes::CHANGESET_ROLLBACK_FAILED;
		OutDiagnostic.Message = TEXT("Rollback preview did not provide an impact payload.");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PackageValues = nullptr;
	(*ImpactObject)->TryGetArrayField(TEXT("packages"), PackageValues);
	if (PackageValues != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& PackageValue : *PackageValues)
		{
			FString PackagePath;
			if (PackageValue.IsValid() && PackageValue->TryGetString(PackagePath))
			{
				OutTouchedPackages.Add(PackagePath);
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* MissingSnapshots = nullptr;
	(*ImpactObject)->TryGetArrayField(TEXT("missing_snapshots"), MissingSnapshots);
	if (MissingSnapshots != nullptr && MissingSnapshots->Num() > 0 && !bForce)
	{
		OutDiagnostic.Code = MCPErrorCodes::CHANGESET_ROLLBACK_FAILED;
		OutDiagnostic.Message = TEXT("Rollback cannot proceed because snapshots are missing.");
		OutDiagnostic.Detail = FString::Printf(TEXT("changeset_id=%s mode=%s"), *ChangeSetId, *Mode);
		OutDiagnostic.Suggestion = TEXT("Use changeset.rollback.preview first and retry with force=true if acceptable.");
		return false;
	}

	if (!Mode.Equals(TEXT("local_snapshot"), ESearchCase::IgnoreCase))
	{
		OutDiagnostic.Code = MCPErrorCodes::CHANGESET_ROLLBACK_FAILED;
		OutDiagnostic.Message = TEXT("Only local_snapshot mode is currently supported.");
		OutDiagnostic.Detail = FString::Printf(TEXT("requested_mode=%s"), *Mode);
		return false;
	}

	if (OutTouchedPackages.Num() == 0)
	{
		bOutApplied = true;
		return true;
	}

	OutDiagnostic.Code = MCPErrorCodes::CHANGESET_ROLLBACK_FAILED;
	OutDiagnostic.Message = TEXT("Rollback apply is not fully implemented for non-empty changesets yet.");
	OutDiagnostic.Detail = FString::Printf(TEXT("changeset_id=%s package_count=%d"), *ChangeSetId, OutTouchedPackages.Num());
	OutDiagnostic.Suggestion = TEXT("Use VCS-based revert or implement package snapshot restore.");
	return false;
}

FString UMCPChangeSetSubsystem::GetChangeSetRootDir() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMCP/ChangeSets"));
}

FString UMCPChangeSetSubsystem::BuildChangeSetDirectory(const FString& ChangeSetId) const
{
	return FPaths::Combine(GetChangeSetRootDir(), ChangeSetId);
}

bool UMCPChangeSetSubsystem::ReadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson) const
{
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *FilePath))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
	return FJsonSerializer::Deserialize(Reader, OutJson) && OutJson.IsValid();
}

bool UMCPChangeSetSubsystem::WriteJsonFile(const FString& FilePath, const TSharedRef<FJsonObject>& JsonObject) const
{
	FString Content;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Content);
	FJsonSerializer::Serialize(JsonObject, Writer);
	return FFileHelper::SaveStringToFile(Content, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
