#include "Tools/Ops/MCPToolsOpsHandler.h"

#include "Editor.h"
#include "MCPChangeSetSubsystem.h"
#include "MCPErrorCodes.h"
#include "MCPJobSubsystem.h"
#include "MCPObservabilitySubsystem.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Tools/Common/MCPToolDiagnostics.h"

namespace
{
	bool ResolveChangeSetSubsystem(const UMCPChangeSetSubsystem*& OutSubsystem, FMCPToolExecutionResult& OutResult)
	{
		OutSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPChangeSetSubsystem>() : nullptr;
		if (OutSubsystem != nullptr)
		{
			return true;
		}

		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::INTERNAL_EXCEPTION,
			TEXT("ChangeSet subsystem is unavailable."),
			TEXT("error"));
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	bool ResolveJobSubsystem(UMCPJobSubsystem*& OutSubsystem, FMCPToolExecutionResult& OutResult)
	{
		OutSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPJobSubsystem>() : nullptr;
		if (OutSubsystem != nullptr)
		{
			return true;
		}

		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::INTERNAL_EXCEPTION,
			TEXT("Job subsystem is unavailable."),
			TEXT("error"));
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}
}

bool FMCPToolsOpsHandler::HandleChangeSetList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	const UMCPChangeSetSubsystem* ChangeSetSubsystem = nullptr;
	if (!ResolveChangeSetSubsystem(ChangeSetSubsystem, OutResult))
	{
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

	const int32 Cursor = MCPToolCommonJson::ParseCursor(Request.Params);
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

bool FMCPToolsOpsHandler::HandleChangeSetGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	const UMCPChangeSetSubsystem* ChangeSetSubsystem = nullptr;
	if (!ResolveChangeSetSubsystem(ChangeSetSubsystem, OutResult))
	{
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
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::SCHEMA_INVALID_PARAMS,
			TEXT("changeset_id is required."),
			TEXT("error"));
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

bool FMCPToolsOpsHandler::HandleChangeSetRollbackPreview(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	const UMCPChangeSetSubsystem* ChangeSetSubsystem = nullptr;
	if (!ResolveChangeSetSubsystem(ChangeSetSubsystem, OutResult))
	{
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
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::SCHEMA_INVALID_PARAMS,
			TEXT("changeset_id is required."),
			TEXT("error"));
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

bool FMCPToolsOpsHandler::HandleChangeSetRollbackApply(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	const UMCPChangeSetSubsystem* ChangeSetSubsystem = nullptr;
	if (!ResolveChangeSetSubsystem(ChangeSetSubsystem, OutResult))
	{
		return false;
	}
	UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>() : nullptr;

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
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::SCHEMA_INVALID_PARAMS,
			TEXT("changeset_id is required."),
			TEXT("error"));
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
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;

	if (ObservabilitySubsystem != nullptr)
	{
		ObservabilitySubsystem->RecordRollbackResult(bApplied);
	}

	return true;
}

bool FMCPToolsOpsHandler::HandleJobGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	UMCPJobSubsystem* JobSubsystem = nullptr;
	if (!ResolveJobSubsystem(JobSubsystem, OutResult))
	{
		return false;
	}

	FString JobId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("job_id"), JobId);
	}

	if (JobId.IsEmpty())
	{
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::SCHEMA_INVALID_PARAMS,
			TEXT("job_id is required."),
			TEXT("error"));
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPJobRecord Record;
	if (!JobSubsystem->GetJob(JobId, Record))
	{
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::JOB_NOT_FOUND,
			TEXT("Requested job was not found."),
			TEXT("error"),
			JobId);
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

bool FMCPToolsOpsHandler::HandleJobCancel(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	UMCPJobSubsystem* JobSubsystem = nullptr;
	if (!ResolveJobSubsystem(JobSubsystem, OutResult))
	{
		return false;
	}

	FString JobId;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("job_id"), JobId);
	}

	if (JobId.IsEmpty())
	{
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::SCHEMA_INVALID_PARAMS,
			TEXT("job_id is required."),
			TEXT("error"));
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
