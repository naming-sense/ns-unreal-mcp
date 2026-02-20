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
#include "MCPChangeSetSubsystem.generated.h"

UCLASS()
class UNREALMCPEDITOR_API UMCPChangeSetSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	bool CreateChangeSetRecord(
		const FMCPRequestEnvelope& Request,
		const FMCPToolExecutionResult& Result,
		const FString& PolicyVersion,
		const FString& SchemaHash,
		FString& OutChangeSetId,
		FMCPDiagnostic& OutDiagnostic) const;

	bool ListChangeSets(
		int32 Limit,
		int32 Cursor,
		const TArray<FString>& StatusFilter,
		const FString& ToolGlob,
		const FString& SessionId,
		TArray<TSharedPtr<FJsonObject>>& OutItems,
		int32& OutNextCursor,
		FMCPDiagnostic& OutDiagnostic) const;

	bool GetChangeSet(
		const FString& ChangeSetId,
		bool bIncludeLogs,
		bool bIncludeSnapshots,
		TSharedPtr<FJsonObject>& OutResult,
		FMCPDiagnostic& OutDiagnostic) const;

	bool PreviewRollback(
		const FString& ChangeSetId,
		const FString& Mode,
		TSharedPtr<FJsonObject>& OutResult,
		FMCPDiagnostic& OutDiagnostic) const;

	bool ApplyRollback(
		const FString& ChangeSetId,
		const FString& Mode,
		bool bForce,
		TArray<FString>& OutTouchedPackages,
		bool& bOutApplied,
		FMCPDiagnostic& OutDiagnostic) const;

	FString GetChangeSetRootDir() const;

private:
	FString BuildChangeSetDirectory(const FString& ChangeSetId) const;
	bool ReadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutJson) const;
	bool WriteJsonFile(const FString& FilePath, const TSharedRef<FJsonObject>& JsonObject) const;
};
