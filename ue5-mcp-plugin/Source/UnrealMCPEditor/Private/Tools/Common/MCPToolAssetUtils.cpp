#include "Tools/Common/MCPToolAssetUtils.h"

#if __has_include("EditorLoadingAndSavingUtils.h")
#include "EditorLoadingAndSavingUtils.h"
#elif __has_include("FileHelpers.h")
#include "FileHelpers.h"
#else
#error "Editor loading/saving utilities header not found."
#endif
#include "MCPErrorCodes.h"
#include "Tools/Common/MCPToolDiagnostics.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"

FString MCPToolAssetUtils::NormalizePackageNameFromInput(const FString& InputPath)
{
	FString NormalizedPath = InputPath;
	NormalizedPath.TrimStartAndEndInline();
	if (NormalizedPath.IsEmpty())
	{
		return FString();
	}

	if (NormalizedPath.Contains(TEXT(".")))
	{
		NormalizedPath = FSoftObjectPath(NormalizedPath).GetLongPackageName();
	}

	return NormalizedPath;
}

UPackage* MCPToolAssetUtils::ResolvePackageByName(const FString& InputPath)
{
	const FString PackageName = NormalizePackageNameFromInput(InputPath);
	if (PackageName.IsEmpty())
	{
		return nullptr;
	}

	if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
	{
		return ExistingPackage;
	}

	return LoadPackage(nullptr, *PackageName, LOAD_None);
}

FString MCPToolAssetUtils::BuildPackageName(const FString& PackagePath, const FString& AssetName)
{
	const FString SanitizedAssetName = AssetName.TrimStartAndEnd();
	if (PackagePath.IsEmpty() || SanitizedAssetName.IsEmpty())
	{
		return FString();
	}

	return FString::Printf(TEXT("%s/%s"), *PackagePath, *SanitizedAssetName);
}

FString MCPToolAssetUtils::BuildObjectPath(const FString& PackagePath, const FString& AssetName)
{
	const FString PackageName = BuildPackageName(PackagePath, AssetName);
	if (PackageName.IsEmpty())
	{
		return FString();
	}

	return FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
}

bool MCPToolAssetUtils::IsValidAssetDestination(const FString& PackagePath, const FString& AssetName)
{
	const FString PackageName = BuildPackageName(PackagePath, AssetName);
	if (PackageName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackageName))
	{
		return false;
	}

	const FString ObjectPath = BuildObjectPath(PackagePath, AssetName);
	return !ObjectPath.IsEmpty();
}

UObject* MCPToolAssetUtils::ResolveObjectByPath(const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty())
	{
		return nullptr;
	}

	if (UObject* ExistingObject = FindObject<UObject>(nullptr, *ObjectPath))
	{
		return ExistingObject;
	}

	return LoadObject<UObject>(nullptr, *ObjectPath);
}

UClass* MCPToolAssetUtils::ResolveClassByPath(const FString& ClassPath)
{
	if (ClassPath.IsEmpty())
	{
		return nullptr;
	}

	if (UClass* FoundClass = FindObject<UClass>(nullptr, *ClassPath))
	{
		return FoundClass;
	}

	FString NormalizedClassPath = ClassPath;
	if (!NormalizedClassPath.StartsWith(TEXT("/")))
	{
		NormalizedClassPath = FString::Printf(TEXT("/Script/%s"), *NormalizedClassPath);
	}

	UClass* ClassObject = LoadObject<UClass>(nullptr, *NormalizedClassPath);
	if (ClassObject != nullptr)
	{
		return ClassObject;
	}

	const FString WrappedClassPath = FString::Printf(TEXT("Class'%s'"), *ClassPath);
	ClassObject = LoadObject<UClass>(nullptr, *WrappedClassPath);
	if (ClassObject != nullptr)
	{
		return ClassObject;
	}

	ClassObject = LoadObject<UClass>(nullptr, *ClassPath);
	if (ClassObject != nullptr)
	{
		return ClassObject;
	}

	const int32 DotIndex = ClassPath.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (DotIndex != INDEX_NONE && DotIndex + 1 < ClassPath.Len())
	{
		const FString ClassName = ClassPath.Mid(DotIndex + 1);
		ClassObject = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None, ELogVerbosity::NoLogging);
	}

	return ClassObject;
}

bool MCPToolAssetUtils::SavePackageByName(const FString& PackageName, FMCPToolExecutionResult& OutResult)
{
	if (PackageName.IsEmpty())
	{
		return true;
	}

	UPackage* Package = ResolvePackageByName(PackageName);
	if (Package == nullptr)
	{
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::OBJECT_NOT_FOUND,
			TEXT("Package was not found while saving asset lifecycle result."),
			TEXT("warning"),
			PackageName);
		return false;
	}

	const TArray<UPackage*> PackagesToSave{ Package };
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	if (!bSaved)
	{
		MCPToolDiagnostics::AddDiagnostic(
			OutResult.Diagnostics,
			MCPErrorCodes::SAVE_FAILED,
			TEXT("Package save failed."),
			TEXT("warning"),
			PackageName,
			TEXT(""),
			true);
		return false;
	}

	return true;
}

FString MCPToolAssetUtils::BuildGeneratedClassPathFromObjectPath(const FString& ObjectPath)
{
	if (ObjectPath.IsEmpty())
	{
		return FString();
	}

	int32 DotIndex = INDEX_NONE;
	if (!ObjectPath.FindLastChar(TEXT('.'), DotIndex) || DotIndex + 1 >= ObjectPath.Len())
	{
		return FString::Printf(TEXT("%s_C"), *ObjectPath);
	}

	const FString Left = ObjectPath.Left(DotIndex + 1);
	const FString Right = ObjectPath.Mid(DotIndex + 1);
	return FString::Printf(TEXT("%s%s_C"), *Left, *Right);
}
