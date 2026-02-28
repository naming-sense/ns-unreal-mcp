#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

namespace MCPToolAssetUtils
{
	FString NormalizePackageNameFromInput(const FString& InputPath);
	UPackage* ResolvePackageByName(const FString& InputPath);
	FString BuildPackageName(const FString& PackagePath, const FString& AssetName);
	FString BuildObjectPath(const FString& PackagePath, const FString& AssetName);
	bool IsValidAssetDestination(const FString& PackagePath, const FString& AssetName);
	UObject* ResolveObjectByPath(const FString& ObjectPath);
	UClass* ResolveClassByPath(const FString& ClassPath);
	bool SavePackageByName(const FString& PackageName, FMCPToolExecutionResult& OutResult);
	FString BuildGeneratedClassPathFromObjectPath(const FString& ObjectPath);
}
