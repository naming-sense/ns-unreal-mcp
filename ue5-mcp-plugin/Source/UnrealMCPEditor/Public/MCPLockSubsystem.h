#pragma once

#include "CoreMinimal.h"
#if __has_include("Subsystems/EditorSubsystem.h")
#include "Subsystems/EditorSubsystem.h"
#elif __has_include("EditorSubsystem.h")
#include "EditorSubsystem.h"
#else
#error "EditorSubsystem header not found. Check UnrealEd dependency."
#endif
#include "MCPTypes.h"
#include "MCPLockSubsystem.generated.h"

struct FMCPLockRecord
{
	FString Owner;
	FDateTime ExpiresAtUtc;
};

UCLASS()
class UNREALMCPEDITOR_API UMCPLockSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	bool AcquireLock(const FString& LockKey, const FString& Owner, int32 LeaseMs, FMCPDiagnostic& OutDiagnostic);
	bool RenewLock(const FString& LockKey, const FString& Owner, int32 LeaseMs);
	void ReleaseLock(const FString& LockKey, const FString& Owner);
	void ReleaseAllByOwner(const FString& Owner);
	void ReclaimStaleLocks();

private:
	TMap<FString, FMCPLockRecord> ActiveLocks;
	mutable FCriticalSection LockGuard;
};
