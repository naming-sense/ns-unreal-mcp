#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

namespace MCPToolDiagnostics
{
	FMCPDiagnostic MakeDiagnostic(
		const FString& Code,
		const FString& Message,
		const FString& Severity = TEXT("error"),
		const FString& Detail = TEXT(""),
		const FString& Suggestion = TEXT(""),
		bool bRetriable = false);

	void AddDiagnostic(
		TArray<FMCPDiagnostic>& OutDiagnostics,
		const FString& Code,
		const FString& Message,
		const FString& Severity = TEXT("error"),
		const FString& Detail = TEXT(""),
		const FString& Suggestion = TEXT(""),
		bool bRetriable = false);
}
