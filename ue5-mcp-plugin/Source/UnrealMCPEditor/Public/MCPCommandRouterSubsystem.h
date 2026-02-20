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
#include "MCPCommandRouterSubsystem.generated.h"

UCLASS()
class UNREALMCPEDITOR_API UMCPCommandRouterSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "UnrealMCP")
	FString ExecuteRequestJson(const FString& RequestJson, bool& bOutSuccess);

private:
	bool ValidateProtocol(const FString& Protocol, FMCPDiagnostic& OutDiagnostic) const;
	bool CheckIdempotencyReplay(
		const FMCPRequestEnvelope& Request,
		FString& OutCachedResponse,
		bool& bOutConflict,
		FMCPDiagnostic& OutConflictDiagnostic);
	void CacheIdempotencyResponse(const FMCPRequestEnvelope& Request, const FString& ResponseJson);
	FString BuildIdempotencyBaseKey(const FMCPRequestEnvelope& Request) const;
	FString BuildIdempotencyFullKey(const FMCPRequestEnvelope& Request) const;

private:
	TMap<FString, FString> CachedResponsesByIdempotencyKey;
	TMap<FString, FString> ParamsHashByIdempotencyBaseKey;
	FCriticalSection IdempotencyGuard;
};
