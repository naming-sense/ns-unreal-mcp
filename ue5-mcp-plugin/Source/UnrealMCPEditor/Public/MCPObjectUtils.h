#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class AActor;
class FProperty;
class UObject;

namespace MCPObjectUtils
{
	UNREALMCPEDITOR_API bool ResolveTargetObject(
		const TSharedPtr<FJsonObject>& TargetObject,
		UObject*& OutObject,
		FMCPDiagnostic& OutDiagnostic);

	UNREALMCPEDITOR_API void InspectObject(
		UObject* TargetObject,
		const TSharedPtr<FJsonObject>& FiltersObject,
		int32 Depth,
		TArray<TSharedPtr<FJsonValue>>& OutProperties);

	UNREALMCPEDITOR_API bool ApplyPatch(
		UObject* TargetObject,
		const TArray<TSharedPtr<FJsonValue>>* PatchOperations,
		TArray<FString>& OutChangedProperties,
		FMCPDiagnostic& OutDiagnostic);

	UNREALMCPEDITOR_API FString BuildActorPath(const AActor* Actor);
	UNREALMCPEDITOR_API void AppendTouchedPackage(UObject* TargetObject, TArray<FString>& InOutTouchedPackages);
}
