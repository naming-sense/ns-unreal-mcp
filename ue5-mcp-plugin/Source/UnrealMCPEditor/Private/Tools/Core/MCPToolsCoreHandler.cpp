#include "Tools/Core/MCPToolsCoreHandler.h"

#include "MCPToolRegistrySubsystem.h"
#include "Editor.h"
#include "MCPEventStreamSubsystem.h"
#include "MCPObservabilitySubsystem.h"
#include "MCPPolicySubsystem.h"
#include "MCPWebSocketTransportSubsystem.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Tools/Common/MCPToolDiagnostics.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS && __has_include("ILiveCodingModule.h")
#include "ILiveCodingModule.h"
#define MCP_CORE_WITH_LIVE_CODING 1
#else
#define MCP_CORE_WITH_LIVE_CODING 0
#endif

namespace
{
#if MCP_CORE_WITH_LIVE_CODING
	FString LiveCodingCompileResultToString(const ELiveCodingCompileResult Result)
	{
		switch (Result)
		{
		case ELiveCodingCompileResult::Success:
			return TEXT("success");
		case ELiveCodingCompileResult::NoChanges:
			return TEXT("no_changes");
		case ELiveCodingCompileResult::InProgress:
			return TEXT("in_progress");
		case ELiveCodingCompileResult::CompileStillActive:
			return TEXT("compile_still_active");
		case ELiveCodingCompileResult::NotStarted:
			return TEXT("not_started");
		case ELiveCodingCompileResult::Failure:
			return TEXT("failure");
		case ELiveCodingCompileResult::Cancelled:
			return TEXT("cancelled");
		default:
			break;
		}

		return TEXT("unknown");
	}
#endif
}

bool FMCPToolsCoreHandler::HandleToolsList(
	const UMCPToolRegistrySubsystem& Registry,
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult)
{
	bool bIncludeSchemas = true;
	FString DomainFilter;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetBoolField(TEXT("include_schemas"), bIncludeSchemas);
		Request.Params->TryGetStringField(TEXT("domain_filter"), DomainFilter);
	}

	TArray<TSharedPtr<FJsonValue>> Tools;
	Registry.BuildToolsList(bIncludeSchemas, DomainFilter, Tools);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("protocol_version"), Registry.GetProtocolVersion());
	OutResult.ResultObject->SetStringField(TEXT("schema_hash"), Registry.GetSchemaHash());
	OutResult.ResultObject->SetArrayField(TEXT("capabilities"), MCPToolCommonJson::ToJsonStringArray(Registry.GetCapabilities()));
	OutResult.ResultObject->SetArrayField(TEXT("tools"), Tools);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsCoreHandler::HandleSystemHealth(
	const UMCPToolRegistrySubsystem& Registry,
	const int32 RegisteredToolCount,
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMCP"));
	const FString PluginVersion = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : TEXT("unknown");
	const FString EngineVersion = FEngineVersion::Current().ToString();

	const UMCPPolicySubsystem* PolicySubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPPolicySubsystem>() : nullptr;
	const UMCPEventStreamSubsystem* EventStreamSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPEventStreamSubsystem>() : nullptr;
	const UMCPObservabilitySubsystem* ObservabilitySubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPObservabilitySubsystem>() : nullptr;
	const UMCPWebSocketTransportSubsystem* WebSocketTransportSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMCPWebSocketTransportSubsystem>() : nullptr;
	const bool bSafeMode = PolicySubsystem ? PolicySubsystem->IsSafeMode() : false;

	TSharedRef<FJsonObject> EditorState = MakeShared<FJsonObject>();
	EditorState->SetBoolField(TEXT("pie"), GEditor != nullptr && GEditor->PlayWorld != nullptr);
	EditorState->SetBoolField(TEXT("dry_run_request"), Request.Context.bDryRun);
	EditorState->SetNumberField(TEXT("registered_tool_count"), static_cast<double>(RegisteredToolCount));

	if (EventStreamSubsystem != nullptr)
	{
		EditorState->SetObjectField(TEXT("event_stream"), EventStreamSubsystem->BuildSnapshot(8));
	}
	else
	{
		TSharedRef<FJsonObject> EventStreamFallback = MakeShared<FJsonObject>();
		EventStreamFallback->SetBoolField(TEXT("supported"), false);
		EditorState->SetObjectField(TEXT("event_stream"), EventStreamFallback);
	}

	if (ObservabilitySubsystem != nullptr)
	{
		EditorState->SetObjectField(TEXT("observability"), ObservabilitySubsystem->BuildSnapshot());
	}
	else
	{
		EditorState->SetObjectField(TEXT("observability"), MakeShared<FJsonObject>());
	}

	TSharedRef<FJsonObject> TransportState = MakeShared<FJsonObject>();
	if (WebSocketTransportSubsystem != nullptr)
	{
		TransportState->SetBoolField(TEXT("enabled"), WebSocketTransportSubsystem->IsEnabled());
		TransportState->SetBoolField(TEXT("listening"), WebSocketTransportSubsystem->IsListening());
		TransportState->SetStringField(TEXT("bind_address"), WebSocketTransportSubsystem->GetBindAddress());
		TransportState->SetNumberField(TEXT("port"), static_cast<double>(WebSocketTransportSubsystem->GetListenPort()));
		TransportState->SetNumberField(TEXT("client_count"), static_cast<double>(WebSocketTransportSubsystem->GetClientCount()));
	}
	else
	{
		TransportState->SetBoolField(TEXT("enabled"), false);
		TransportState->SetBoolField(TEXT("listening"), false);
		TransportState->SetStringField(TEXT("bind_address"), TEXT(""));
		TransportState->SetNumberField(TEXT("port"), 0.0);
		TransportState->SetNumberField(TEXT("client_count"), 0.0);
	}
	EditorState->SetObjectField(TEXT("event_stream_transport"), TransportState);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetStringField(TEXT("engine_version"), EngineVersion);
	OutResult.ResultObject->SetStringField(TEXT("plugin_version"), PluginVersion);
	OutResult.ResultObject->SetStringField(TEXT("protocol_version"), Registry.GetProtocolVersion());
	OutResult.ResultObject->SetBoolField(TEXT("safe_mode"), bSafeMode);
	OutResult.ResultObject->SetObjectField(TEXT("editor_state"), EditorState);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsCoreHandler::HandleEditorLiveCodingCompile(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	bool bEnsureEnabledForSession = true;
	bool bWaitForCompletion = true;
	bool bShowConsole = false;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetBoolField(TEXT("ensure_enabled_for_session"), bEnsureEnabledForSession);
		Request.Params->TryGetBoolField(TEXT("wait_for_completion"), bWaitForCompletion);
		Request.Params->TryGetBoolField(TEXT("show_console"), bShowConsole);
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("supported"), MCP_CORE_WITH_LIVE_CODING != 0);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("ensure_enabled_for_session"), bEnsureEnabledForSession);
	OutResult.ResultObject->SetBoolField(TEXT("wait_for_completion"), bWaitForCompletion);
	OutResult.ResultObject->SetBoolField(TEXT("show_console"), bShowConsole);

#if MCP_CORE_WITH_LIVE_CODING
	ILiveCodingModule* LiveCodingModule = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCodingModule == nullptr && !Request.Context.bDryRun)
	{
		LiveCodingModule = FModuleManager::LoadModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	}

	if (LiveCodingModule == nullptr)
	{
		OutResult.ResultObject->SetBoolField(TEXT("module_loaded"), false);
		OutResult.ResultObject->SetStringField(TEXT("compile_result"), TEXT("unavailable"));

		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			TEXT("MCP.EDITOR.LIVECODING.UNAVAILABLE"),
			TEXT("Live Coding module is not available in this editor build/session."),
			TEXT("error"),
			TEXT(""),
			TEXT("Enable Live Coding in Editor Preferences and ensure the LiveCoding module is included for this target."));
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const bool bStartedBefore = LiveCodingModule->HasStarted();
	const bool bEnabledForSessionBefore = LiveCodingModule->IsEnabledForSession();
	const bool bWasCompiling = LiveCodingModule->IsCompiling();

	bool bCompileRequested = false;
	bool bCompileCallAccepted = false;
	ELiveCodingCompileResult CompileResult = ELiveCodingCompileResult::NotStarted;

	if (!Request.Context.bDryRun)
	{
		if (bShowConsole)
		{
			LiveCodingModule->ShowConsole();
		}

		if (bEnsureEnabledForSession && !LiveCodingModule->IsEnabledForSession())
		{
			LiveCodingModule->EnableForSession(true);
		}

		if (bEnsureEnabledForSession && !LiveCodingModule->IsEnabledForSession())
		{
			OutResult.ResultObject->SetBoolField(TEXT("module_loaded"), true);
			OutResult.ResultObject->SetBoolField(TEXT("started_before"), bStartedBefore);
			OutResult.ResultObject->SetBoolField(TEXT("enabled_for_session_before"), bEnabledForSessionBefore);
			OutResult.ResultObject->SetBoolField(TEXT("started_after"), LiveCodingModule->HasStarted());
			OutResult.ResultObject->SetBoolField(TEXT("enabled_for_session_after"), LiveCodingModule->IsEnabledForSession());
			OutResult.ResultObject->SetBoolField(TEXT("was_compiling"), bWasCompiling);
			OutResult.ResultObject->SetBoolField(TEXT("is_compiling"), LiveCodingModule->IsCompiling());
			OutResult.ResultObject->SetBoolField(TEXT("compile_requested"), false);
			OutResult.ResultObject->SetBoolField(TEXT("compile_call_accepted"), false);
			OutResult.ResultObject->SetStringField(TEXT("compile_result"), TEXT("enable_failed"));

			FString EnableErrorText = LiveCodingModule->GetEnableErrorText().ToString();
			if (EnableErrorText.IsEmpty())
			{
				EnableErrorText = TEXT("Live Coding could not be enabled for this session.");
			}
			OutResult.ResultObject->SetStringField(TEXT("enable_error"), EnableErrorText);

			MCPToolDiagnostics::AddDiagnostic(
				OutResult.Diagnostics,
				TEXT("MCP.EDITOR.LIVECODING.ENABLE_FAILED"),
				TEXT("Failed to enable Live Coding for current session."),
				TEXT("error"),
				EnableErrorText,
				TEXT("Restart editor without hot-reloaded modules and retry."));
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		bCompileRequested = true;
		if (bWaitForCompletion)
		{
			bCompileCallAccepted = LiveCodingModule->Compile(ELiveCodingCompileFlags::WaitForCompletion, &CompileResult);
		}
		else
		{
			LiveCodingModule->Compile();
			bCompileCallAccepted = true;
			CompileResult = ELiveCodingCompileResult::InProgress;
		}
	}
	else
	{
		CompileResult = ELiveCodingCompileResult::InProgress;
	}

	OutResult.ResultObject->SetBoolField(TEXT("module_loaded"), true);
	OutResult.ResultObject->SetBoolField(TEXT("started_before"), bStartedBefore);
	OutResult.ResultObject->SetBoolField(TEXT("enabled_for_session_before"), bEnabledForSessionBefore);
	OutResult.ResultObject->SetBoolField(TEXT("started_after"), LiveCodingModule->HasStarted());
	OutResult.ResultObject->SetBoolField(TEXT("enabled_for_session_after"), LiveCodingModule->IsEnabledForSession());
	OutResult.ResultObject->SetBoolField(TEXT("was_compiling"), bWasCompiling);
	OutResult.ResultObject->SetBoolField(TEXT("is_compiling"), LiveCodingModule->IsCompiling());
	OutResult.ResultObject->SetBoolField(TEXT("compile_requested"), bCompileRequested);
	OutResult.ResultObject->SetBoolField(TEXT("compile_call_accepted"), bCompileCallAccepted || Request.Context.bDryRun);
	OutResult.ResultObject->SetStringField(TEXT("compile_result"), LiveCodingCompileResultToString(CompileResult));

	if (Request.Context.bDryRun)
	{
		OutResult.Status = EMCPResponseStatus::Ok;
		return true;
	}

	if (!bCompileCallAccepted)
	{
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			TEXT("MCP.EDITOR.LIVECODING.COMPILE_CALL_FAILED"),
			TEXT("Live Coding compile call was rejected."),
			TEXT("error"),
			TEXT(""),
			TEXT("Check Live Coding session state and retry."),
			true);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (CompileResult == ELiveCodingCompileResult::CompileStillActive)
	{
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			TEXT("MCP.EDITOR.LIVECODING.COMPILE_ACTIVE"),
			TEXT("A prior Live Coding compile is still active."),
			TEXT("warning"),
			TEXT(""),
			TEXT("Wait for previous compile to complete, then retry."),
			true);
		OutResult.Status = EMCPResponseStatus::Partial;
		return true;
	}

	if (CompileResult == ELiveCodingCompileResult::Failure
		|| CompileResult == ELiveCodingCompileResult::Cancelled
		|| CompileResult == ELiveCodingCompileResult::NotStarted)
	{
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			TEXT("MCP.EDITOR.LIVECODING.COMPILE_FAILED"),
			TEXT("Live Coding compile failed."),
			TEXT("error"),
			LiveCodingCompileResultToString(CompileResult),
			TEXT(""),
			CompileResult != ELiveCodingCompileResult::NotStarted);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
#else
	OutResult.ResultObject->SetBoolField(TEXT("module_loaded"), false);
	OutResult.ResultObject->SetStringField(TEXT("compile_result"), TEXT("unsupported_platform_or_build"));

	MCPToolDiagnostics::AddDiagnostic(
		OutResult.Diagnostics,
		TEXT("MCP.EDITOR.LIVECODING.UNSUPPORTED"),
		TEXT("Live Coding compile is not supported on this platform/build."),
		TEXT("error"),
		TEXT(""),
		TEXT("Use Build.bat or IDE build for this environment."));
	OutResult.Status = EMCPResponseStatus::Error;
	return false;
#endif
}
