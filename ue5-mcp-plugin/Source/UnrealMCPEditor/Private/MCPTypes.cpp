#include "MCPTypes.h"

#include "MCPErrorCodes.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/SecureHash.h"

TSharedRef<FJsonObject> FMCPDiagnostic::ToJson() const
{
	TSharedRef<FJsonObject> JsonObject = MakeShared<FJsonObject>();
	JsonObject->SetStringField(TEXT("code"), Code);
	JsonObject->SetStringField(TEXT("severity"), Severity);
	JsonObject->SetStringField(TEXT("message"), Message);
	JsonObject->SetStringField(TEXT("detail"), Detail);
	JsonObject->SetStringField(TEXT("suggestion"), Suggestion);
	JsonObject->SetBoolField(TEXT("retriable"), bRetriable);
	return JsonObject;
}

namespace
{
	bool ParseContext(const TSharedPtr<FJsonObject>& ContextObject, FMCPRequestContext& OutContext)
	{
		if (!ContextObject.IsValid())
		{
			return true;
		}

		ContextObject->TryGetStringField(TEXT("project_id"), OutContext.ProjectId);
		ContextObject->TryGetStringField(TEXT("workspace_id"), OutContext.WorkspaceId);
		ContextObject->TryGetStringField(TEXT("engine_version"), OutContext.EngineVersion);
		ContextObject->TryGetBoolField(TEXT("deterministic"), OutContext.bDeterministic);
		ContextObject->TryGetBoolField(TEXT("dry_run"), OutContext.bDryRun);
		ContextObject->TryGetStringField(TEXT("idempotency_key"), OutContext.IdempotencyKey);
		if (ContextObject->TryGetStringField(TEXT("cancel_token"), OutContext.CancelToken))
		{
			OutContext.bHasCancelToken = true;
		}
		double TimeoutMs = static_cast<double>(OutContext.TimeoutMs);
		if (ContextObject->TryGetNumberField(TEXT("timeout_ms"), TimeoutMs))
		{
			OutContext.TimeoutMs = static_cast<int32>(TimeoutMs);
			OutContext.bHasTimeoutOverride = true;
		}
		return true;
	}

	void AddDiagnosticsBySeverity(
		const TArray<FMCPDiagnostic>& Diagnostics,
		TArray<TSharedPtr<FJsonValue>>& OutErrors,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings,
		TArray<TSharedPtr<FJsonValue>>& OutInfos)
	{
		for (const FMCPDiagnostic& Diagnostic : Diagnostics)
		{
			const TSharedRef<FJsonObject> DiagnosticObject = Diagnostic.ToJson();
			const TSharedPtr<FJsonValueObject> DiagnosticValue = MakeShared<FJsonValueObject>(DiagnosticObject);

			if (Diagnostic.Severity.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
			{
				OutWarnings.Add(DiagnosticValue);
			}
			else if (Diagnostic.Severity.Equals(TEXT("info"), ESearchCase::IgnoreCase))
			{
				OutInfos.Add(DiagnosticValue);
			}
			else
			{
				OutErrors.Add(DiagnosticValue);
			}
		}
	}
}

bool MCPJson::ParseRequestEnvelope(const FString& RequestJson, FMCPRequestEnvelope& OutRequest, FMCPDiagnostic& OutError)
{
	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		OutError.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutError.Message = TEXT("Request JSON parsing failed.");
		OutError.Suggestion = TEXT("Check request JSON format and required fields.");
		return false;
	}

	if (!RootObject->TryGetStringField(TEXT("protocol"), OutRequest.Protocol))
	{
		OutRequest.Protocol = TEXT("unreal-mcp/1.0");
	}

	if (!RootObject->TryGetStringField(TEXT("request_id"), OutRequest.RequestId))
	{
		OutError.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutError.Message = TEXT("Missing required field: request_id");
		OutError.Suggestion = TEXT("Provide request_id in the request envelope.");
		return false;
	}

	if (!RootObject->TryGetStringField(TEXT("session_id"), OutRequest.SessionId))
	{
		OutRequest.SessionId = TEXT("default-session");
	}

	if (!RootObject->TryGetStringField(TEXT("tool"), OutRequest.Tool) || OutRequest.Tool.IsEmpty())
	{
		OutError.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		OutError.Message = TEXT("Missing required field: tool");
		OutError.Suggestion = TEXT("Provide the tool name to execute.");
		return false;
	}

	const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject != nullptr && ParamsObject->IsValid())
	{
		OutRequest.Params = *ParamsObject;
	}
	else
	{
		OutRequest.Params = MakeShared<FJsonObject>();
	}

	const TSharedPtr<FJsonObject>* ContextObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("context"), ContextObject) && ContextObject != nullptr && ContextObject->IsValid())
	{
		ParseContext(*ContextObject, OutRequest.Context);
	}

	return true;
}

FString MCPJson::BuildResponseEnvelope(
	const FMCPRequestEnvelope& Request,
	const FMCPToolExecutionResult& Result,
	const FString& ChangeSetId,
	int64 DurationMs)
{
	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetStringField(TEXT("request_id"), Request.RequestId);
	RootObject->SetStringField(TEXT("status"), StatusToString(Result.Status));
	RootObject->SetObjectField(TEXT("result"), Result.ResultObject.IsValid() ? Result.ResultObject.ToSharedRef() : MakeShared<FJsonObject>());

	if (ChangeSetId.IsEmpty())
	{
		RootObject->SetField(TEXT("changeset_id"), MakeShared<FJsonValueNull>());
	}
	else
	{
		RootObject->SetStringField(TEXT("changeset_id"), ChangeSetId);
	}

	TArray<TSharedPtr<FJsonValue>> TouchedPackageValues;
	for (const FString& TouchedPackage : Result.TouchedPackages)
	{
		TouchedPackageValues.Add(MakeShared<FJsonValueString>(TouchedPackage));
	}
	RootObject->SetArrayField(TEXT("touched_packages"), TouchedPackageValues);

	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	TArray<TSharedPtr<FJsonValue>> InfoValues;
	AddDiagnosticsBySeverity(Result.Diagnostics, ErrorValues, WarningValues, InfoValues);

	TSharedRef<FJsonObject> DiagnosticsObject = MakeShared<FJsonObject>();
	DiagnosticsObject->SetArrayField(TEXT("errors"), ErrorValues);
	DiagnosticsObject->SetArrayField(TEXT("warnings"), WarningValues);
	DiagnosticsObject->SetArrayField(TEXT("infos"), InfoValues);
	RootObject->SetObjectField(TEXT("diagnostics"), DiagnosticsObject);

	TArray<TSharedPtr<FJsonValue>> ArtifactValues;
	for (const TSharedPtr<FJsonObject>& ArtifactObject : Result.Artifacts)
	{
		if (ArtifactObject.IsValid())
		{
			ArtifactValues.Add(MakeShared<FJsonValueObject>(ArtifactObject));
		}
	}
	RootObject->SetArrayField(TEXT("artifacts"), ArtifactValues);

	TSharedRef<FJsonObject> MetricsObject = MakeShared<FJsonObject>();
	MetricsObject->SetNumberField(TEXT("duration_ms"), static_cast<double>(DurationMs));
	RootObject->SetObjectField(TEXT("metrics"), MetricsObject);
	RootObject->SetBoolField(TEXT("idempotent_replay"), Result.bIdempotentReplay);

	return SerializeJsonObject(RootObject);
}

FString MCPJson::HashJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
{
	const FString SerializedJson = SerializeJsonObject(JsonObject);
	FTCHARToUTF8 Utf8Data(*SerializedJson);
	uint8 Digest[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8Data.Get(), Utf8Data.Length(), Digest);
	return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
}

FString MCPJson::SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
{
	if (!JsonObject.IsValid())
	{
		return TEXT("{}");
	}

	FString SerializedJson;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&SerializedJson);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return SerializedJson;
}

FString MCPJson::StatusToString(const EMCPResponseStatus Status)
{
	switch (Status)
	{
	case EMCPResponseStatus::Ok:
		return TEXT("ok");
	case EMCPResponseStatus::Partial:
		return TEXT("partial");
	default:
		return TEXT("error");
	}
}
