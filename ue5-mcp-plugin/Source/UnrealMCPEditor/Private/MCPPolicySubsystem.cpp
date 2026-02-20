#include "MCPPolicySubsystem.h"

#include "Editor.h"
#include "MCPErrorCodes.h"
#include "MCPLog.h"

bool UMCPPolicySubsystem::PreflightAuthorize(const FMCPRequestEnvelope& Request, FMCPDiagnostic& OutDiagnostic) const
{
	if (IsSafeMode())
	{
		OutDiagnostic.Code = MCPErrorCodes::EDITOR_UNSAFE_STATE;
		OutDiagnostic.Message = TEXT("Write tools are blocked while PIE is running.");
		OutDiagnostic.Detail = FString::Printf(TEXT("tool=%s"), *Request.Tool);
		OutDiagnostic.Suggestion = TEXT("Stop PIE and retry the request.");
		OutDiagnostic.bRetriable = true;
		return false;
	}

	return true;
}

void UMCPPolicySubsystem::PostflightApply(const FMCPRequestEnvelope& Request, const FMCPToolExecutionResult& Result) const
{
	if (Result.Status == EMCPResponseStatus::Error)
	{
		return;
	}

	UE_LOG(LogUnrealMCP, Verbose, TEXT("Policy postflight completed. tool=%s touched=%d"), *Request.Tool, Result.TouchedPackages.Num());
}

FString UMCPPolicySubsystem::GetPolicyVersion() const
{
	return TEXT("policy-1");
}

bool UMCPPolicySubsystem::IsSafeMode() const
{
	return GEditor != nullptr && GEditor->PlayWorld != nullptr;
}
