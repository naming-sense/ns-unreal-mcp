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
#include "MCPPolicySubsystem.generated.h"

UCLASS()
class UNREALMCPEDITOR_API UMCPPolicySubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	bool PreflightAuthorize(const FMCPRequestEnvelope& Request, FMCPDiagnostic& OutDiagnostic) const;
	void PostflightApply(const FMCPRequestEnvelope& Request, const FMCPToolExecutionResult& Result) const;
	FString GetPolicyVersion() const;
	bool IsSafeMode() const;
};
