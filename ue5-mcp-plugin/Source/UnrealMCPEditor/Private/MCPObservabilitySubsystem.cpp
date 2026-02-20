#include "MCPObservabilitySubsystem.h"

void UMCPObservabilitySubsystem::RecordToolExecution(
	const FString& ToolName,
	const EMCPResponseStatus Status,
	const int64 DurationMs,
	const bool bIdempotentReplay)
{
	FScopeLock ScopeLock(&MetricsGuard);
	FMCPToolObservabilityMetrics& ToolMetric = ToolMetrics.FindOrAdd(ToolName);
	++ToolMetric.TotalRequests;
	ToolMetric.TotalDurationMs += FMath::Max<int64>(0, DurationMs);
	ToolMetric.LastDurationMs = FMath::Max<int64>(0, DurationMs);
	ToolMetric.MaxDurationMs = FMath::Max(ToolMetric.MaxDurationMs, ToolMetric.LastDurationMs);

	if (bIdempotentReplay)
	{
		++ToolMetric.ReplayCount;
	}

	switch (Status)
	{
	case EMCPResponseStatus::Ok:
		++ToolMetric.OkCount;
		break;
	case EMCPResponseStatus::Partial:
		++ToolMetric.PartialCount;
		break;
	default:
		++ToolMetric.ErrorCount;
		break;
	}
}

void UMCPObservabilitySubsystem::RecordPolicyDenied(const bool bSafeModeBlocked)
{
	FScopeLock ScopeLock(&MetricsGuard);
	++PolicyDeniedCount;
	if (bSafeModeBlocked)
	{
		++SafeModeBlockedCount;
	}
}

void UMCPObservabilitySubsystem::RecordLockAttempt(const bool bConflict, const int64 WaitMs)
{
	FScopeLock ScopeLock(&MetricsGuard);
	LockWaitTotalMs += FMath::Max<int64>(0, WaitMs);
	++LockWaitSampleCount;
	if (bConflict)
	{
		++LockConflictCount;
	}
}

void UMCPObservabilitySubsystem::RecordStaleLocksReclaimed(const int32 ReclaimedCount)
{
	FScopeLock ScopeLock(&MetricsGuard);
	StaleLockReclaimedCount += FMath::Max<int32>(0, ReclaimedCount);
}

void UMCPObservabilitySubsystem::RecordSchemaValidationError()
{
	FScopeLock ScopeLock(&MetricsGuard);
	++SchemaInvalidParamsCount;
}

void UMCPObservabilitySubsystem::RecordTimeoutExceeded()
{
	FScopeLock ScopeLock(&MetricsGuard);
	++TimeoutExceededCount;
}

void UMCPObservabilitySubsystem::RecordCancelRejected()
{
	FScopeLock ScopeLock(&MetricsGuard);
	++CancelRejectedCount;
}

void UMCPObservabilitySubsystem::RecordIdempotencyConflict()
{
	FScopeLock ScopeLock(&MetricsGuard);
	++IdempotencyConflictCount;
}

void UMCPObservabilitySubsystem::RecordChangeSetCreated(const int64 ApproximateBytes, const int32 SnapshotCount)
{
	FScopeLock ScopeLock(&MetricsGuard);
	++ChangeSetCreatedCount;
	ChangeSetBytes += FMath::Max<int64>(0, ApproximateBytes);
	SnapshotCreatedCount += FMath::Max<int32>(0, SnapshotCount);
}

void UMCPObservabilitySubsystem::RecordRollbackResult(const bool bSucceeded)
{
	FScopeLock ScopeLock(&MetricsGuard);
	if (bSucceeded)
	{
		++RollbackSucceededCount;
	}
	else
	{
		++RollbackFailedCount;
	}
}

void UMCPObservabilitySubsystem::RecordJobStatus(const FString& Status)
{
	FScopeLock ScopeLock(&MetricsGuard);
	JobStatusCounts.FindOrAdd(Status) += 1;
}

TSharedRef<FJsonObject> UMCPObservabilitySubsystem::BuildSnapshot() const
{
	FScopeLock ScopeLock(&MetricsGuard);

	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();

	TArray<FString> ToolNames;
	ToolMetrics.GetKeys(ToolNames);
	ToolNames.Sort();

	TArray<TSharedPtr<FJsonValue>> ToolMetricValues;
	ToolMetricValues.Reserve(ToolNames.Num());
	for (const FString& ToolName : ToolNames)
	{
		const FMCPToolObservabilityMetrics* ToolMetric = ToolMetrics.Find(ToolName);
		if (ToolMetric == nullptr)
		{
			continue;
		}

		TSharedRef<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("tool"), ToolName);
		ToolObject->SetNumberField(TEXT("total_requests"), static_cast<double>(ToolMetric->TotalRequests));
		ToolObject->SetNumberField(TEXT("ok"), static_cast<double>(ToolMetric->OkCount));
		ToolObject->SetNumberField(TEXT("error"), static_cast<double>(ToolMetric->ErrorCount));
		ToolObject->SetNumberField(TEXT("partial"), static_cast<double>(ToolMetric->PartialCount));
		ToolObject->SetNumberField(TEXT("replay"), static_cast<double>(ToolMetric->ReplayCount));
		ToolObject->SetNumberField(TEXT("avg_duration_ms"), ToolMetric->TotalRequests > 0 ? static_cast<double>(ToolMetric->TotalDurationMs) / static_cast<double>(ToolMetric->TotalRequests) : 0.0);
		ToolObject->SetNumberField(TEXT("max_duration_ms"), static_cast<double>(ToolMetric->MaxDurationMs));
		ToolObject->SetNumberField(TEXT("last_duration_ms"), static_cast<double>(ToolMetric->LastDurationMs));
		ToolMetricValues.Add(MakeShared<FJsonValueObject>(ToolObject));
	}
	Snapshot->SetArrayField(TEXT("tool_metrics"), ToolMetricValues);

	TSharedRef<FJsonObject> PolicyObject = MakeShared<FJsonObject>();
	PolicyObject->SetNumberField(TEXT("deny_count"), static_cast<double>(PolicyDeniedCount));
	PolicyObject->SetNumberField(TEXT("safe_mode_block_count"), static_cast<double>(SafeModeBlockedCount));
	Snapshot->SetObjectField(TEXT("policy"), PolicyObject);

	TSharedRef<FJsonObject> LockObject = MakeShared<FJsonObject>();
	LockObject->SetNumberField(TEXT("conflict_count"), static_cast<double>(LockConflictCount));
	LockObject->SetNumberField(TEXT("wait_sample_count"), static_cast<double>(LockWaitSampleCount));
	LockObject->SetNumberField(TEXT("avg_wait_ms"), LockWaitSampleCount > 0 ? static_cast<double>(LockWaitTotalMs) / static_cast<double>(LockWaitSampleCount) : 0.0);
	LockObject->SetNumberField(TEXT("stale_reclaimed_count"), static_cast<double>(StaleLockReclaimedCount));
	Snapshot->SetObjectField(TEXT("lock"), LockObject);

	TSharedRef<FJsonObject> RequestErrorsObject = MakeShared<FJsonObject>();
	RequestErrorsObject->SetNumberField(TEXT("schema_invalid_params"), static_cast<double>(SchemaInvalidParamsCount));
	RequestErrorsObject->SetNumberField(TEXT("timeout_exceeded"), static_cast<double>(TimeoutExceededCount));
	RequestErrorsObject->SetNumberField(TEXT("cancel_rejected"), static_cast<double>(CancelRejectedCount));
	RequestErrorsObject->SetNumberField(TEXT("idempotency_conflict"), static_cast<double>(IdempotencyConflictCount));
	Snapshot->SetObjectField(TEXT("request_errors"), RequestErrorsObject);

	TSharedRef<FJsonObject> ChangeSetObject = MakeShared<FJsonObject>();
	ChangeSetObject->SetNumberField(TEXT("created_count"), static_cast<double>(ChangeSetCreatedCount));
	ChangeSetObject->SetNumberField(TEXT("bytes"), static_cast<double>(ChangeSetBytes));
	ChangeSetObject->SetNumberField(TEXT("snapshot_count"), static_cast<double>(SnapshotCreatedCount));
	ChangeSetObject->SetNumberField(TEXT("rollback_success_count"), static_cast<double>(RollbackSucceededCount));
	ChangeSetObject->SetNumberField(TEXT("rollback_failed_count"), static_cast<double>(RollbackFailedCount));
	Snapshot->SetObjectField(TEXT("changeset"), ChangeSetObject);

	TArray<FString> JobStatuses;
	JobStatusCounts.GetKeys(JobStatuses);
	JobStatuses.Sort();

	TArray<TSharedPtr<FJsonValue>> JobValues;
	JobValues.Reserve(JobStatuses.Num());
	for (const FString& JobStatus : JobStatuses)
	{
		const int64* StatusCount = JobStatusCounts.Find(JobStatus);
		if (StatusCount == nullptr)
		{
			continue;
		}

		TSharedRef<FJsonObject> JobObject = MakeShared<FJsonObject>();
		JobObject->SetStringField(TEXT("status"), JobStatus);
		JobObject->SetNumberField(TEXT("count"), static_cast<double>(*StatusCount));
		JobValues.Add(MakeShared<FJsonValueObject>(JobObject));
	}
	Snapshot->SetArrayField(TEXT("job_status_counts"), JobValues);

	return Snapshot;
}

