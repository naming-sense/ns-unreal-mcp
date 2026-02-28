#include "Tools/Common/MCPToolDiagnostics.h"

namespace MCPToolDiagnostics
{
	FMCPDiagnostic MakeDiagnostic(
		const FString& Code,
		const FString& Message,
		const FString& Severity,
		const FString& Detail,
		const FString& Suggestion,
		const bool bRetriable)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = Code;
		Diagnostic.Severity = Severity;
		Diagnostic.Message = Message;
		Diagnostic.Detail = Detail;
		Diagnostic.Suggestion = Suggestion;
		Diagnostic.bRetriable = bRetriable;
		return Diagnostic;
	}

	void AddDiagnostic(
		TArray<FMCPDiagnostic>& OutDiagnostics,
		const FString& Code,
		const FString& Message,
		const FString& Severity,
		const FString& Detail,
		const FString& Suggestion,
		const bool bRetriable)
	{
		OutDiagnostics.Add(MakeDiagnostic(Code, Message, Severity, Detail, Suggestion, bRetriable));
	}
}
