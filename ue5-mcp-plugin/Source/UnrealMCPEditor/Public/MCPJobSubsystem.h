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
#include "MCPJobSubsystem.generated.h"

UENUM()
enum class EMCPJobStatus : uint8
{
	Queued,
	Running,
	Succeeded,
	Failed,
	Canceled
};

struct FMCPJobRecord
{
	FString JobId;
	EMCPJobStatus Status = EMCPJobStatus::Queued;
	double Progress = 0.0;
	FDateTime StartedAtUtc;
	FDateTime UpdatedAtUtc;
	TSharedPtr<FJsonObject> Result;
	TArray<FMCPDiagnostic> Diagnostics;
};

UCLASS()
class UNREALMCPEDITOR_API UMCPJobSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	FString CreateJob();
	bool UpdateJobStatus(const FString& JobId, EMCPJobStatus Status, double Progress);
	bool FinalizeJob(
		const FString& JobId,
		EMCPJobStatus Status,
		const TSharedPtr<FJsonObject>& Result,
		const TArray<FMCPDiagnostic>& Diagnostics);
	bool GetJob(const FString& JobId, FMCPJobRecord& OutRecord) const;
	bool CancelJob(const FString& JobId, FMCPJobRecord& OutRecord, FMCPDiagnostic& OutDiagnostic);

	static FString StatusToString(EMCPJobStatus Status);

private:
	TMap<FString, FMCPJobRecord> Jobs;
	mutable FCriticalSection JobGuard;
};
