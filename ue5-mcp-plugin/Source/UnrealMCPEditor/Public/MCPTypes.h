#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

enum class EMCPResponseStatus : uint8
{
	Ok,
	Error,
	Partial
};

struct FMCPDiagnostic
{
	FString Code;
	FString Severity = TEXT("error");
	FString Message;
	FString Detail;
	FString Suggestion;
	bool bRetriable = false;

	TSharedRef<FJsonObject> ToJson() const;
};

struct FMCPRequestContext
{
	FString ProjectId;
	FString WorkspaceId;
	FString EngineVersion;
	bool bDeterministic = true;
	bool bDryRun = false;
	FString IdempotencyKey;
	int32 TimeoutMs = 30000;
	bool bHasTimeoutOverride = false;
	FString CancelToken;
	bool bHasCancelToken = false;
};

struct FMCPRequestEnvelope
{
	FString Protocol = TEXT("unreal-mcp/1.0");
	FString RequestId;
	FString SessionId;
	FString Tool;
	TSharedPtr<FJsonObject> Params;
	FMCPRequestContext Context;
};

struct FMCPToolExecutionResult
{
	EMCPResponseStatus Status = EMCPResponseStatus::Ok;
	TSharedPtr<FJsonObject> ResultObject;
	TArray<FMCPDiagnostic> Diagnostics;
	TArray<FString> TouchedPackages;
	TArray<TSharedPtr<FJsonObject>> Artifacts;
	bool bIdempotentReplay = false;
};

namespace MCPJson
{
	UNREALMCPEDITOR_API bool ParseRequestEnvelope(const FString& RequestJson, FMCPRequestEnvelope& OutRequest, FMCPDiagnostic& OutError);
	UNREALMCPEDITOR_API FString BuildResponseEnvelope(
		const FMCPRequestEnvelope& Request,
		const FMCPToolExecutionResult& Result,
		const FString& ChangeSetId,
		int64 DurationMs);
	UNREALMCPEDITOR_API FString HashJsonObject(const TSharedPtr<FJsonObject>& JsonObject);
	UNREALMCPEDITOR_API FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject);
	UNREALMCPEDITOR_API FString StatusToString(EMCPResponseStatus Status);
}
