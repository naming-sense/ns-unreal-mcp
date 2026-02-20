#pragma once

#include "CoreMinimal.h"
#if __has_include("Subsystems/EditorSubsystem.h")
#include "Subsystems/EditorSubsystem.h"
#elif __has_include("EditorSubsystem.h")
#include "EditorSubsystem.h"
#else
#error "EditorSubsystem header not found. Check UnrealEd dependency."
#endif
#include "Dom/JsonObject.h"
#include "MCPTypes.h"
#include "MCPObservabilitySubsystem.generated.h"

struct FMCPToolObservabilityMetrics
{
	int64 TotalRequests = 0;
	int64 OkCount = 0;
	int64 ErrorCount = 0;
	int64 PartialCount = 0;
	int64 ReplayCount = 0;
	int64 TotalDurationMs = 0;
	int64 MaxDurationMs = 0;
	int64 LastDurationMs = 0;
};

UCLASS()
class UNREALMCPEDITOR_API UMCPObservabilitySubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	void RecordToolExecution(const FString& ToolName, EMCPResponseStatus Status, int64 DurationMs, bool bIdempotentReplay);
	void RecordPolicyDenied(bool bSafeModeBlocked);
	void RecordLockAttempt(bool bConflict, int64 WaitMs);
	void RecordStaleLocksReclaimed(int32 ReclaimedCount);
	void RecordSchemaValidationError();
	void RecordTimeoutExceeded();
	void RecordCancelRejected();
	void RecordIdempotencyConflict();
	void RecordChangeSetCreated(int64 ApproximateBytes, int32 SnapshotCount);
	void RecordRollbackResult(bool bSucceeded);
	void RecordJobStatus(const FString& Status);

	TSharedRef<FJsonObject> BuildSnapshot() const;

private:
	mutable FCriticalSection MetricsGuard;
	TMap<FString, FMCPToolObservabilityMetrics> ToolMetrics;

	int64 PolicyDeniedCount = 0;
	int64 SafeModeBlockedCount = 0;
	int64 LockConflictCount = 0;
	int64 LockWaitTotalMs = 0;
	int64 LockWaitSampleCount = 0;
	int64 StaleLockReclaimedCount = 0;
	int64 SchemaInvalidParamsCount = 0;
	int64 TimeoutExceededCount = 0;
	int64 CancelRejectedCount = 0;
	int64 IdempotencyConflictCount = 0;
	int64 ChangeSetCreatedCount = 0;
	int64 ChangeSetBytes = 0;
	int64 SnapshotCreatedCount = 0;
	int64 RollbackSucceededCount = 0;
	int64 RollbackFailedCount = 0;
	TMap<FString, int64> JobStatusCounts;
};

