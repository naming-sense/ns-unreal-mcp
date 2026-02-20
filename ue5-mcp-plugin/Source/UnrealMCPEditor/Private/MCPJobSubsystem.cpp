#include "MCPJobSubsystem.h"

#include "Editor.h"
#include "MCPErrorCodes.h"
#include "MCPEventStreamSubsystem.h"
#include "MCPObservabilitySubsystem.h"
#include "Misc/Guid.h"

namespace
{
	void PublishJobStatusEvent(
		const FString& RequestId,
		const FMCPJobRecord& Record)
	{
		if (GEditor == nullptr)
		{
			return;
		}

		if (UMCPEventStreamSubsystem* EventStreamSubsystem = GEditor->GetEditorSubsystem<UMCPEventStreamSubsystem>())
		{
			EventStreamSubsystem->EmitJobStatus(
				RequestId,
				Record.JobId,
				UMCPJobSubsystem::StatusToString(Record.Status),
				Record.Progress,
				Record.StartedAtUtc.ToIso8601(),
				Record.UpdatedAtUtc.ToIso8601());
		}

		if (UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>())
		{
			ObservabilitySubsystem->RecordJobStatus(UMCPJobSubsystem::StatusToString(Record.Status));
		}
	}
}

FString UMCPJobSubsystem::CreateJob()
{
	FMCPJobRecord Record;
	{
		FScopeLock ScopeLock(&JobGuard);
		Record.JobId = FString::Printf(TEXT("job-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Record.Status = EMCPJobStatus::Queued;
		Record.Progress = 0.0;
		Record.StartedAtUtc = FDateTime::UtcNow();
		Record.UpdatedAtUtc = Record.StartedAtUtc;
		Record.Result = MakeShared<FJsonObject>();
		Jobs.Add(Record.JobId, Record);
	}
	PublishJobStatusEvent(TEXT(""), Record);
	return Record.JobId;
}

bool UMCPJobSubsystem::UpdateJobStatus(const FString& JobId, const EMCPJobStatus Status, const double Progress)
{
	FMCPJobRecord PublishedRecord;
	bool bShouldPublish = false;
	{
		FScopeLock ScopeLock(&JobGuard);
		if (FMCPJobRecord* Record = Jobs.Find(JobId))
		{
			Record->Status = Status;
			Record->Progress = FMath::Clamp(Progress, 0.0, 100.0);
			Record->UpdatedAtUtc = FDateTime::UtcNow();
			PublishedRecord = *Record;
			bShouldPublish = true;
		}
	}

	if (bShouldPublish)
	{
		PublishJobStatusEvent(TEXT(""), PublishedRecord);
		return true;
	}

	return false;
}

bool UMCPJobSubsystem::FinalizeJob(
	const FString& JobId,
	const EMCPJobStatus Status,
	const TSharedPtr<FJsonObject>& Result,
	const TArray<FMCPDiagnostic>& Diagnostics)
{
	FMCPJobRecord PublishedRecord;
	bool bShouldPublish = false;
	{
		FScopeLock ScopeLock(&JobGuard);
		if (FMCPJobRecord* Record = Jobs.Find(JobId))
		{
			Record->Status = Status;
			Record->Progress = (Status == EMCPJobStatus::Succeeded) ? 100.0 : Record->Progress;
			Record->UpdatedAtUtc = FDateTime::UtcNow();
			Record->Result = Result.IsValid() ? Result : MakeShared<FJsonObject>();
			Record->Diagnostics = Diagnostics;
			PublishedRecord = *Record;
			bShouldPublish = true;
		}
	}

	if (bShouldPublish)
	{
		PublishJobStatusEvent(TEXT(""), PublishedRecord);
		return true;
	}

	return false;
}

bool UMCPJobSubsystem::GetJob(const FString& JobId, FMCPJobRecord& OutRecord) const
{
	FScopeLock ScopeLock(&JobGuard);
	if (const FMCPJobRecord* Found = Jobs.Find(JobId))
	{
		OutRecord = *Found;
		return true;
	}

	return false;
}

bool UMCPJobSubsystem::CancelJob(const FString& JobId, FMCPJobRecord& OutRecord, FMCPDiagnostic& OutDiagnostic)
{
	bool bShouldPublish = false;
	{
		FScopeLock ScopeLock(&JobGuard);
		FMCPJobRecord* Record = Jobs.Find(JobId);
		if (Record == nullptr)
		{
			OutDiagnostic.Code = MCPErrorCodes::JOB_NOT_FOUND;
			OutDiagnostic.Message = TEXT("Requested job was not found.");
			OutDiagnostic.Detail = JobId;
			OutDiagnostic.Suggestion = TEXT("Call job.get with a valid job_id.");
			return false;
		}

		if (Record->Status == EMCPJobStatus::Succeeded || Record->Status == EMCPJobStatus::Failed || Record->Status == EMCPJobStatus::Canceled)
		{
			OutRecord = *Record;
			return true;
		}

		Record->Status = EMCPJobStatus::Canceled;
		Record->UpdatedAtUtc = FDateTime::UtcNow();
		OutRecord = *Record;
		bShouldPublish = true;
	}

	if (bShouldPublish)
	{
		PublishJobStatusEvent(TEXT(""), OutRecord);
	}
	return true;
}

FString UMCPJobSubsystem::StatusToString(const EMCPJobStatus Status)
{
	switch (Status)
	{
	case EMCPJobStatus::Queued:
		return TEXT("queued");
	case EMCPJobStatus::Running:
		return TEXT("running");
	case EMCPJobStatus::Succeeded:
		return TEXT("succeeded");
	case EMCPJobStatus::Failed:
		return TEXT("failed");
	default:
		return TEXT("canceled");
	}
}
