#include "MCPLockSubsystem.h"

#include "Editor.h"
#include "MCPErrorCodes.h"
#include "MCPObservabilitySubsystem.h"

bool UMCPLockSubsystem::AcquireLock(const FString& LockKey, const FString& Owner, const int32 LeaseMs, FMCPDiagnostic& OutDiagnostic)
{
	FScopeLock ScopeLock(&LockGuard);
	const int64 WaitMs = 0;

	const FDateTime NowUtc = FDateTime::UtcNow();
	for (auto It = ActiveLocks.CreateIterator(); It; ++It)
	{
		if (It.Value().ExpiresAtUtc <= NowUtc)
		{
			It.RemoveCurrent();
		}
	}

	if (const FMCPLockRecord* ExistingRecord = ActiveLocks.Find(LockKey))
	{
		if (ExistingRecord->Owner != Owner)
		{
			if (GEditor != nullptr)
			{
				if (UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>())
				{
					ObservabilitySubsystem->RecordLockAttempt(true, WaitMs);
				}
			}

			OutDiagnostic.Code = MCPErrorCodes::LOCK_CONFLICT;
			OutDiagnostic.Message = TEXT("Lock conflict detected for requested resource.");
			OutDiagnostic.Detail = FString::Printf(TEXT("lock_key=%s owner=%s"), *LockKey, *ExistingRecord->Owner);
			OutDiagnostic.Suggestion = TEXT("Retry later with exponential backoff.");
			OutDiagnostic.bRetriable = true;
			return false;
		}
	}

	FMCPLockRecord NewRecord;
	NewRecord.Owner = Owner;
	NewRecord.ExpiresAtUtc = FDateTime::UtcNow() + FTimespan::FromMilliseconds(LeaseMs);
	ActiveLocks.Add(LockKey, NewRecord);

	if (GEditor != nullptr)
	{
		if (UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>())
		{
			ObservabilitySubsystem->RecordLockAttempt(false, WaitMs);
		}
	}
	return true;
}

bool UMCPLockSubsystem::RenewLock(const FString& LockKey, const FString& Owner, const int32 LeaseMs)
{
	FScopeLock ScopeLock(&LockGuard);
	if (FMCPLockRecord* ExistingRecord = ActiveLocks.Find(LockKey))
	{
		if (ExistingRecord->Owner == Owner)
		{
			ExistingRecord->ExpiresAtUtc = FDateTime::UtcNow() + FTimespan::FromMilliseconds(LeaseMs);
			return true;
		}
	}

	return false;
}

void UMCPLockSubsystem::ReleaseLock(const FString& LockKey, const FString& Owner)
{
	FScopeLock ScopeLock(&LockGuard);
	if (const FMCPLockRecord* ExistingRecord = ActiveLocks.Find(LockKey))
	{
		if (ExistingRecord->Owner == Owner)
		{
			ActiveLocks.Remove(LockKey);
		}
	}
}

void UMCPLockSubsystem::ReleaseAllByOwner(const FString& Owner)
{
	FScopeLock ScopeLock(&LockGuard);
	for (auto It = ActiveLocks.CreateIterator(); It; ++It)
	{
		if (It.Value().Owner == Owner)
		{
			It.RemoveCurrent();
		}
	}
}

void UMCPLockSubsystem::ReclaimStaleLocks()
{
	FScopeLock ScopeLock(&LockGuard);
	const FDateTime NowUtc = FDateTime::UtcNow();
	int32 ReclaimedCount = 0;
	for (auto It = ActiveLocks.CreateIterator(); It; ++It)
	{
		if (It.Value().ExpiresAtUtc <= NowUtc)
		{
			It.RemoveCurrent();
			++ReclaimedCount;
		}
	}

	if (ReclaimedCount > 0 && GEditor != nullptr)
	{
		if (UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>())
		{
			ObservabilitySubsystem->RecordStaleLocksReclaimed(ReclaimedCount);
		}
	}
}
