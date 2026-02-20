#include "MCPCommandRouterSubsystem.h"

#include "Editor.h"
#include "MCPChangeSetSubsystem.h"
#include "MCPErrorCodes.h"
#include "MCPEventStreamSubsystem.h"
#include "MCPLockSubsystem.h"
#include "MCPObservabilitySubsystem.h"
#include "MCPPolicySubsystem.h"
#include "MCPJobSubsystem.h"
#include "MCPToolRegistrySubsystem.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

namespace
{
	int64 GetCurrentUnixTimestampMs()
	{
		return FDateTime::UtcNow().ToUnixTimestamp() * 1000LL;
	}

	FString NormalizeLockKeyPath(const FString& CandidatePath)
	{
		if (CandidatePath.IsEmpty())
		{
			return CandidatePath;
		}

		if (FPackageName::IsValidObjectPath(CandidatePath) || CandidatePath.Contains(TEXT(".")))
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(CandidatePath);
			if (!PackageName.IsEmpty())
			{
				return PackageName;
			}
		}

		return CandidatePath;
	}

	FString TryGetFirstStringFromArray(const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
	{
		if (!Params.IsValid())
		{
			return FString();
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || Values == nullptr)
		{
			return FString();
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Item;
			if (Value.IsValid() && Value->TryGetString(Item) && !Item.IsEmpty())
			{
				return Item;
			}
		}

		return FString();
	}

	FString TryResolveLockKey(const FMCPRequestEnvelope& Request)
	{
		if (!Request.Params.IsValid())
		{
			return FString::Printf(TEXT("tool:%s"), *Request.Tool);
		}

		const TSharedPtr<FJsonObject>* TargetObjectPtr = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("target"), TargetObjectPtr) && TargetObjectPtr != nullptr && TargetObjectPtr->IsValid())
		{
			FString TargetPath;
			if ((*TargetObjectPtr)->TryGetStringField(TEXT("path"), TargetPath) && !TargetPath.IsEmpty())
			{
				return NormalizeLockKeyPath(TargetPath);
			}
		}

		static const TCHAR* CandidateFields[] = {
			TEXT("object_path"),
			TEXT("package_path"),
			TEXT("dest_package_path"),
			TEXT("new_package_path"),
			TEXT("source_object_path")
		};

		for (const TCHAR* CandidateField : CandidateFields)
		{
			FString CandidatePath;
			if (Request.Params->TryGetStringField(CandidateField, CandidatePath) && !CandidatePath.IsEmpty())
			{
				return NormalizeLockKeyPath(CandidatePath);
			}
		}

		const FString ArrayPathCandidate = TryGetFirstStringFromArray(Request.Params, TEXT("object_paths"));
		if (!ArrayPathCandidate.IsEmpty())
		{
			return NormalizeLockKeyPath(ArrayPathCandidate);
		}

		return FString::Printf(TEXT("tool:%s"), *Request.Tool);
	}

	EMCPJobStatus ToJobStatus(const EMCPResponseStatus Status)
	{
		return (Status == EMCPResponseStatus::Error) ? EMCPJobStatus::Failed : EMCPJobStatus::Succeeded;
	}

	EMCPResponseStatus ParseResponseStatus(const FString& ResponseJson)
	{
		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseJson);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			return EMCPResponseStatus::Ok;
		}

		FString StatusString;
		if (!RootObject->TryGetStringField(TEXT("status"), StatusString))
		{
			return EMCPResponseStatus::Ok;
		}

		if (StatusString.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			return EMCPResponseStatus::Error;
		}

		if (StatusString.Equals(TEXT("partial"), ESearchCase::IgnoreCase))
		{
			return EMCPResponseStatus::Partial;
		}

		return EMCPResponseStatus::Ok;
	}

	int64 ComputeDirectorySizeBytes(const FString& DirectoryPath)
	{
		TArray<FString> FilePaths;
		IFileManager::Get().FindFilesRecursive(FilePaths, *DirectoryPath, TEXT("*"), true, false, false);

		int64 TotalBytes = 0;
		for (const FString& FilePath : FilePaths)
		{
			const int64 FileSize = IFileManager::Get().FileSize(*FilePath);
			if (FileSize > 0)
			{
				TotalBytes += FileSize;
			}
		}
		return TotalBytes;
	}

	int32 CountSnapshotFiles(const FString& SnapshotDirectoryPath)
	{
		TArray<FString> SnapshotFiles;
		IFileManager::Get().FindFiles(SnapshotFiles, *(FPaths::Combine(SnapshotDirectoryPath, TEXT("*.before"))), true, false);
		return SnapshotFiles.Num();
	}
}

FString UMCPCommandRouterSubsystem::ExecuteRequestJson(const FString& RequestJson, bool& bOutSuccess)
{
	bOutSuccess = false;
	const int64 StartMs = GetCurrentUnixTimestampMs();

	FMCPRequestEnvelope Request;
	FMCPDiagnostic ParseDiagnostic;
	if (!MCPJson::ParseRequestEnvelope(RequestJson, Request, ParseDiagnostic))
	{
		if (GEditor != nullptr)
		{
			if (UMCPEventStreamSubsystem* EventStreamSubsystem = GEditor->GetEditorSubsystem<UMCPEventStreamSubsystem>())
			{
				EventStreamSubsystem->EmitLog(TEXT("invalid-request"), TEXT("error"), ParseDiagnostic.Message);
			}

			if (UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>())
			{
				ObservabilitySubsystem->RecordSchemaValidationError();
			}
		}

		FMCPRequestEnvelope FallbackRequest;
		FallbackRequest.RequestId = TEXT("invalid-request");
		FMCPToolExecutionResult ErrorResult;
		ErrorResult.Status = EMCPResponseStatus::Error;
		ErrorResult.Diagnostics.Add(ParseDiagnostic);
		return MCPJson::BuildResponseEnvelope(FallbackRequest, ErrorResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
	}

	UMCPEventStreamSubsystem* EventStream = GEditor ? GEditor->GetEditorSubsystem<UMCPEventStreamSubsystem>() : nullptr;
	UMCPObservabilitySubsystem* Observability = GEditor ? GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>() : nullptr;

	const auto EmitProgress = [EventStream, &Request](const double Percent, const TCHAR* Phase)
	{
		if (EventStream != nullptr)
		{
			EventStream->EmitProgress(Request.RequestId, Percent, Phase);
		}
	};

	const auto EmitLog = [EventStream, &Request](const TCHAR* Level, const FString& Message)
	{
		if (EventStream != nullptr)
		{
			EventStream->EmitLog(Request.RequestId, Level, Message);
		}
	};

	const auto EmitDiagnosticLog = [EventStream, &Request](const FMCPDiagnostic& Diagnostic)
	{
		if (EventStream == nullptr)
		{
			return;
		}

		TSharedRef<FJsonObject> DetailObject = MakeShared<FJsonObject>();
		DetailObject->SetStringField(TEXT("code"), Diagnostic.Code);
		DetailObject->SetStringField(TEXT("detail"), Diagnostic.Detail);
		DetailObject->SetStringField(TEXT("suggestion"), Diagnostic.Suggestion);
		EventStream->EmitLog(Request.RequestId, Diagnostic.Severity, Diagnostic.Message, DetailObject);
	};

	const auto RecordToolMetric = [Observability, &Request, StartMs](const EMCPResponseStatus Status, const bool bIdempotentReplay)
	{
		if (Observability != nullptr && !Request.Tool.IsEmpty())
		{
			Observability->RecordToolExecution(Request.Tool, Status, GetCurrentUnixTimestampMs() - StartMs, bIdempotentReplay);
		}
	};

	EmitProgress(5.0, TEXT("request.parsed"));
	EmitLog(TEXT("info"), FString::Printf(TEXT("Received request for tool %s"), *Request.Tool));

	FMCPDiagnostic ProtocolDiagnostic;
	if (!ValidateProtocol(Request.Protocol, ProtocolDiagnostic))
	{
		if (Observability != nullptr)
		{
			Observability->RecordSchemaValidationError();
		}

		FMCPToolExecutionResult ErrorResult;
		ErrorResult.Status = EMCPResponseStatus::Error;
		ErrorResult.Diagnostics.Add(ProtocolDiagnostic);
		EmitDiagnosticLog(ProtocolDiagnostic);
		RecordToolMetric(EMCPResponseStatus::Error, false);
		EmitProgress(100.0, TEXT("request.failed.protocol"));
		return MCPJson::BuildResponseEnvelope(Request, ErrorResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
	}

	EmitProgress(10.0, TEXT("request.protocol_validated"));

	UMCPToolRegistrySubsystem* ToolRegistry = GEditor ? GEditor->GetEditorSubsystem<UMCPToolRegistrySubsystem>() : nullptr;
	UMCPPolicySubsystem* PolicySubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPPolicySubsystem>() : nullptr;
	UMCPLockSubsystem* LockSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPLockSubsystem>() : nullptr;
	UMCPChangeSetSubsystem* ChangeSetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPChangeSetSubsystem>() : nullptr;
	UMCPJobSubsystem* JobSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPJobSubsystem>() : nullptr;

	if (ToolRegistry == nullptr || PolicySubsystem == nullptr || LockSubsystem == nullptr || ChangeSetSubsystem == nullptr || JobSubsystem == nullptr)
	{
		FMCPToolExecutionResult ErrorResult;
		ErrorResult.Status = EMCPResponseStatus::Error;

		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Required MCP subsystems are unavailable.");
		Diagnostic.Suggestion = TEXT("Verify plugin modules are loaded in the Editor.");
		ErrorResult.Diagnostics.Add(Diagnostic);
		EmitDiagnosticLog(Diagnostic);
		RecordToolMetric(EMCPResponseStatus::Error, false);
		EmitProgress(100.0, TEXT("request.failed.subsystems"));
		return MCPJson::BuildResponseEnvelope(Request, ErrorResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
	}

	FMCPDiagnostic SchemaDiagnostic;
	if (!ToolRegistry->ValidateRequest(Request, SchemaDiagnostic))
	{
		if (Observability != nullptr)
		{
			Observability->RecordSchemaValidationError();
		}

		FMCPToolExecutionResult ErrorResult;
		ErrorResult.Status = EMCPResponseStatus::Error;
		ErrorResult.Diagnostics.Add(SchemaDiagnostic);
		EmitDiagnosticLog(SchemaDiagnostic);
		RecordToolMetric(EMCPResponseStatus::Error, false);
		EmitProgress(100.0, TEXT("request.failed.schema"));
		const FString ResponseJson = MCPJson::BuildResponseEnvelope(Request, ErrorResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
		CacheIdempotencyResponse(Request, ResponseJson);
		return ResponseJson;
	}

	EmitProgress(20.0, TEXT("request.schema_validated"));

	if (Request.Context.bHasCancelToken && !Request.Context.CancelToken.IsEmpty())
	{
		FMCPJobRecord CancelTokenRecord;
		if (JobSubsystem->GetJob(Request.Context.CancelToken, CancelTokenRecord) && CancelTokenRecord.Status == EMCPJobStatus::Canceled)
		{
			if (Observability != nullptr)
			{
				Observability->RecordCancelRejected();
			}

			FMCPToolExecutionResult ErrorResult;
			ErrorResult.Status = EMCPResponseStatus::Error;

			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::JOB_CANCELED;
			Diagnostic.Message = TEXT("Request was canceled before execution.");
			Diagnostic.Detail = FString::Printf(TEXT("cancel_token=%s"), *Request.Context.CancelToken);
			Diagnostic.Suggestion = TEXT("Use a new cancel_token or clear cancellation state and retry.");
			ErrorResult.Diagnostics.Add(Diagnostic);
			EmitDiagnosticLog(Diagnostic);
			RecordToolMetric(EMCPResponseStatus::Error, false);
			EmitProgress(100.0, TEXT("request.failed.canceled"));
			const FString ResponseJson = MCPJson::BuildResponseEnvelope(Request, ErrorResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
			CacheIdempotencyResponse(Request, ResponseJson);
			return ResponseJson;
		}
	}

	FString CachedResponse;
	bool bIdempotencyConflict = false;
	FMCPDiagnostic IdempotencyConflictDiagnostic;
	if (CheckIdempotencyReplay(Request, CachedResponse, bIdempotencyConflict, IdempotencyConflictDiagnostic))
	{
		const EMCPResponseStatus ReplayStatus = ParseResponseStatus(CachedResponse);
		RecordToolMetric(ReplayStatus, true);
		EmitLog(TEXT("info"), TEXT("Returned cached idempotent response."));
		EmitProgress(100.0, TEXT("request.idempotent_replay"));
		bOutSuccess = true;
		return CachedResponse;
	}

	if (bIdempotencyConflict)
	{
		if (Observability != nullptr)
		{
			Observability->RecordIdempotencyConflict();
		}

		FMCPToolExecutionResult ErrorResult;
		ErrorResult.Status = EMCPResponseStatus::Error;
		ErrorResult.Diagnostics.Add(IdempotencyConflictDiagnostic);
		EmitDiagnosticLog(IdempotencyConflictDiagnostic);
		RecordToolMetric(EMCPResponseStatus::Error, false);
		EmitProgress(100.0, TEXT("request.failed.idempotency_conflict"));
		const FString ResponseJson = MCPJson::BuildResponseEnvelope(Request, ErrorResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
		CacheIdempotencyResponse(Request, ResponseJson);
		return ResponseJson;
	}

	FMCPToolExecutionResult ExecutionResult;
	const bool bIsWriteTool = ToolRegistry->IsWriteTool(Request.Tool);
	const FString LockOwner = Request.RequestId;
	const FString LockKey = TryResolveLockKey(Request);
	bool bLockAcquired = false;
	const bool bTrackJob = Request.Context.bHasTimeoutOverride || Request.Context.bHasCancelToken;
	FString TrackedJobId;
	const int32 EffectiveTimeoutMs = Request.Context.bHasTimeoutOverride ? Request.Context.TimeoutMs : 0;

	if (Request.Context.bHasTimeoutOverride && EffectiveTimeoutMs <= 0)
	{
		if (Observability != nullptr)
		{
			Observability->RecordSchemaValidationError();
		}

		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("timeout_ms must be greater than zero.");
		Diagnostic.Detail = FString::Printf(TEXT("timeout_ms=%d"), Request.Context.TimeoutMs);
		ExecutionResult.Status = EMCPResponseStatus::Error;
		ExecutionResult.Diagnostics.Add(Diagnostic);
		EmitDiagnosticLog(Diagnostic);
		RecordToolMetric(EMCPResponseStatus::Error, false);
		EmitProgress(100.0, TEXT("request.failed.timeout_validation"));
		const FString ResponseJson = MCPJson::BuildResponseEnvelope(Request, ExecutionResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
		CacheIdempotencyResponse(Request, ResponseJson);
		return ResponseJson;
	}

	ON_SCOPE_EXIT
	{
		if (bLockAcquired)
		{
			LockSubsystem->ReleaseLock(LockKey, LockOwner);
		}
	};

	const int64 ExecutionBeginMs = GetCurrentUnixTimestampMs();

	if (bIsWriteTool)
	{
		EmitProgress(30.0, TEXT("request.write_preflight"));

		FMCPDiagnostic PolicyDiagnostic;
		if (!PolicySubsystem->PreflightAuthorize(Request, PolicyDiagnostic))
		{
			if (Observability != nullptr)
			{
				const bool bSafeModeBlocked = PolicyDiagnostic.Code.Equals(MCPErrorCodes::EDITOR_UNSAFE_STATE, ESearchCase::CaseSensitive);
				Observability->RecordPolicyDenied(bSafeModeBlocked);
			}

			ExecutionResult.Status = EMCPResponseStatus::Error;
			ExecutionResult.Diagnostics.Add(PolicyDiagnostic);
			EmitDiagnosticLog(PolicyDiagnostic);
			RecordToolMetric(EMCPResponseStatus::Error, false);
			EmitProgress(100.0, TEXT("request.failed.policy"));
			const FString ResponseJson = MCPJson::BuildResponseEnvelope(Request, ExecutionResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
			CacheIdempotencyResponse(Request, ResponseJson);
			return ResponseJson;
		}

		FMCPDiagnostic LockDiagnostic;
		if (!LockSubsystem->AcquireLock(LockKey, LockOwner, 30000, LockDiagnostic))
		{
			ExecutionResult.Status = EMCPResponseStatus::Error;
			ExecutionResult.Diagnostics.Add(LockDiagnostic);
			EmitDiagnosticLog(LockDiagnostic);
			RecordToolMetric(EMCPResponseStatus::Error, false);
			EmitProgress(100.0, TEXT("request.failed.lock"));
			const FString ResponseJson = MCPJson::BuildResponseEnvelope(Request, ExecutionResult, TEXT(""), GetCurrentUnixTimestampMs() - StartMs);
			CacheIdempotencyResponse(Request, ResponseJson);
			return ResponseJson;
		}
		bLockAcquired = true;
		EmitProgress(45.0, TEXT("request.lock_acquired"));
	}

	if (bTrackJob)
	{
		TrackedJobId = JobSubsystem->CreateJob();
		JobSubsystem->UpdateJobStatus(TrackedJobId, EMCPJobStatus::Running, 0.0);
	}

	EmitProgress(55.0, TEXT("request.executing_tool"));
	ToolRegistry->ExecuteTool(Request, ExecutionResult);
	const int64 ExecutionDurationMs = GetCurrentUnixTimestampMs() - ExecutionBeginMs;
	const bool bTimeoutExceeded = Request.Context.bHasTimeoutOverride && EffectiveTimeoutMs > 0 && ExecutionDurationMs > EffectiveTimeoutMs;
	EmitProgress(75.0, TEXT("request.tool_executed"));

	FString ChangeSetId;
	if (bIsWriteTool && !Request.Context.bDryRun && ExecutionResult.Status != EMCPResponseStatus::Error)
	{
		FMCPDiagnostic ChangeSetDiagnostic;
		const FString PolicyVersion = PolicySubsystem->GetPolicyVersion();
		if (!ChangeSetSubsystem->CreateChangeSetRecord(
			Request,
			ExecutionResult,
			PolicyVersion,
			ToolRegistry->GetSchemaHash(),
			ChangeSetId,
			ChangeSetDiagnostic))
		{
			ExecutionResult.Status = EMCPResponseStatus::Error;
			ExecutionResult.Diagnostics.Add(ChangeSetDiagnostic);
			EmitDiagnosticLog(ChangeSetDiagnostic);
		}
		else
		{
			const FString ChangeSetPath = FPaths::Combine(ChangeSetSubsystem->GetChangeSetRootDir(), ChangeSetId);
			if (EventStream != nullptr)
			{
				EventStream->EmitChangeSetCreated(Request.RequestId, ChangeSetId, ChangeSetPath);
			}

			if (Observability != nullptr)
			{
				const int64 ChangeSetBytes = ComputeDirectorySizeBytes(ChangeSetPath);
				const int32 SnapshotCount = CountSnapshotFiles(FPaths::Combine(ChangeSetPath, TEXT("snapshots")));
				Observability->RecordChangeSetCreated(ChangeSetBytes, SnapshotCount);
			}
		}
	}

	if (ExecutionResult.Status != EMCPResponseStatus::Error)
	{
		PolicySubsystem->PostflightApply(Request, ExecutionResult);
	}
	EmitProgress(88.0, TEXT("request.postflight"));

	if (bTimeoutExceeded)
	{
		if (Observability != nullptr)
		{
			Observability->RecordTimeoutExceeded();
		}

		FMCPDiagnostic TimeoutDiagnostic;
		TimeoutDiagnostic.Code = MCPErrorCodes::JOB_TIMEOUT;
		TimeoutDiagnostic.Severity = TEXT("warning");
		TimeoutDiagnostic.Message = TEXT("Execution exceeded timeout_ms.");
		TimeoutDiagnostic.Detail = FString::Printf(TEXT("timeout_ms=%d duration_ms=%lld"), EffectiveTimeoutMs, ExecutionDurationMs);
		TimeoutDiagnostic.Suggestion = TEXT("Increase timeout_ms or switch to asynchronous workflow.");
		TimeoutDiagnostic.bRetriable = true;
		ExecutionResult.Diagnostics.Add(TimeoutDiagnostic);
		if (ExecutionResult.Status == EMCPResponseStatus::Ok)
		{
			ExecutionResult.Status = EMCPResponseStatus::Partial;
		}

		EmitDiagnosticLog(TimeoutDiagnostic);
	}

	if (bTrackJob)
	{
		if (!ExecutionResult.ResultObject.IsValid())
		{
			ExecutionResult.ResultObject = MakeShared<FJsonObject>();
		}
		ExecutionResult.ResultObject->SetStringField(TEXT("job_id"), TrackedJobId);
		JobSubsystem->FinalizeJob(TrackedJobId, ToJobStatus(ExecutionResult.Status), ExecutionResult.ResultObject, ExecutionResult.Diagnostics);
	}

	if (EventStream != nullptr)
	{
		for (const FString& TouchedPackage : ExecutionResult.TouchedPackages)
		{
			EventStream->EmitArtifact(Request.RequestId, TouchedPackage, TEXT("touched_package"));
		}

		for (const TSharedPtr<FJsonObject>& ArtifactObject : ExecutionResult.Artifacts)
		{
			if (!ArtifactObject.IsValid())
			{
				continue;
			}

			FString ObjectPath;
			FString Action;
			ArtifactObject->TryGetStringField(TEXT("object_path"), ObjectPath);
			ArtifactObject->TryGetStringField(TEXT("action"), Action);
			if (!ObjectPath.IsEmpty() || !Action.IsEmpty())
			{
				EventStream->EmitArtifact(Request.RequestId, ObjectPath, Action);
			}
		}
	}

	RecordToolMetric(ExecutionResult.Status, false);
	EmitLog(TEXT("info"), FString::Printf(TEXT("Completed request for tool %s with status %s"), *Request.Tool, *MCPJson::StatusToString(ExecutionResult.Status)));
	EmitProgress(100.0, TEXT("request.completed"));

	const FString ResponseJson = MCPJson::BuildResponseEnvelope(Request, ExecutionResult, ChangeSetId, GetCurrentUnixTimestampMs() - StartMs);
	CacheIdempotencyResponse(Request, ResponseJson);
	bOutSuccess = ExecutionResult.Status != EMCPResponseStatus::Error;
	return ResponseJson;
}

bool UMCPCommandRouterSubsystem::ValidateProtocol(const FString& Protocol, FMCPDiagnostic& OutDiagnostic) const
{
	if (Protocol.StartsWith(TEXT("unreal-mcp/1")))
	{
		return true;
	}

	OutDiagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
	OutDiagnostic.Message = TEXT("Unsupported protocol version.");
	OutDiagnostic.Detail = FString::Printf(TEXT("protocol=%s"), *Protocol);
	OutDiagnostic.Suggestion = TEXT("Use protocol unreal-mcp/1.x.");
	return false;
}

bool UMCPCommandRouterSubsystem::CheckIdempotencyReplay(
	const FMCPRequestEnvelope& Request,
	FString& OutCachedResponse,
	bool& bOutConflict,
	FMCPDiagnostic& OutConflictDiagnostic)
{
	bOutConflict = false;
	OutCachedResponse.Empty();

	if (Request.Context.IdempotencyKey.IsEmpty())
	{
		return false;
	}

	const FString BaseKey = BuildIdempotencyBaseKey(Request);
	const FString FullKey = BuildIdempotencyFullKey(Request);
	const FString ParamsHash = MCPJson::HashJsonObject(Request.Params);

	FScopeLock ScopeLock(&IdempotencyGuard);

	if (const FString* ExistingHash = ParamsHashByIdempotencyBaseKey.Find(BaseKey))
	{
		if (*ExistingHash != ParamsHash)
		{
			bOutConflict = true;
			OutConflictDiagnostic.Code = MCPErrorCodes::IDEMPOTENCY_CONFLICT;
			OutConflictDiagnostic.Message = TEXT("Idempotency key was reused with a different payload.");
			OutConflictDiagnostic.Detail = BaseKey;
			OutConflictDiagnostic.Suggestion = TEXT("Use a new idempotency_key for different params.");
			return false;
		}
	}

	if (const FString* CachedResponse = CachedResponsesByIdempotencyKey.Find(FullKey))
	{
		OutCachedResponse = *CachedResponse;

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(OutCachedResponse);
		if (FJsonSerializer::Deserialize(Reader, RootObject) && RootObject.IsValid())
		{
			RootObject->SetBoolField(TEXT("idempotent_replay"), true);
			OutCachedResponse = MCPJson::SerializeJsonObject(RootObject);
		}
		return true;
	}

	return false;
}

void UMCPCommandRouterSubsystem::CacheIdempotencyResponse(const FMCPRequestEnvelope& Request, const FString& ResponseJson)
{
	if (Request.Context.IdempotencyKey.IsEmpty())
	{
		return;
	}

	const FString BaseKey = BuildIdempotencyBaseKey(Request);
	const FString FullKey = BuildIdempotencyFullKey(Request);
	const FString ParamsHash = MCPJson::HashJsonObject(Request.Params);

	FScopeLock ScopeLock(&IdempotencyGuard);
	ParamsHashByIdempotencyBaseKey.FindOrAdd(BaseKey) = ParamsHash;
	CachedResponsesByIdempotencyKey.FindOrAdd(FullKey) = ResponseJson;
}

FString UMCPCommandRouterSubsystem::BuildIdempotencyBaseKey(const FMCPRequestEnvelope& Request) const
{
	return FString::Printf(TEXT("%s|%s|%s"), *Request.SessionId, *Request.Tool, *Request.Context.IdempotencyKey);
}

FString UMCPCommandRouterSubsystem::BuildIdempotencyFullKey(const FMCPRequestEnvelope& Request) const
{
	return BuildIdempotencyBaseKey(Request) + TEXT("|") + MCPJson::HashJsonObject(Request.Params);
}
