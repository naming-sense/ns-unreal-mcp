#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"
#include "Templates/Function.h"

class FJsonObject;
class UWidget;
class UWidgetBlueprint;
class UClass;
class UPanelWidget;

struct FMCPToolsUMGStructureHandler
{
	using FLoadWidgetBlueprintByPathFn = TFunctionRef<UWidgetBlueprint*(const FString&)>;
	using FResolveWidgetFromRefFn = TFunctionRef<UWidget*(UWidgetBlueprint*, const TSharedPtr<FJsonObject>*)>;
	using FEnsureWidgetGuidMapFn = TFunctionRef<void(UWidgetBlueprint*)>;
	using FEnsureWidgetGuidEntryFn = TFunctionRef<void(UWidgetBlueprint*, UWidget*)>;
	using FCollectWidgetSubtreeVariableNamesFn = TFunctionRef<void(UWidget*, TArray<FName>&)>;
	using FRemoveWidgetGuidEntryFn = TFunctionRef<void(UWidgetBlueprint*, const FName)>;
	using FResolveClassByPathFn = TFunctionRef<UClass*(const FString&)>;
	using FResolveWidgetClassByPathFn = TFunctionRef<UClass*(const FString&)>;
	using FBuildUniqueWidgetNameFn = TFunctionRef<FName(UWidgetBlueprint*, UClass*, const FString&)>;
	using FGetWidgetStableIdFn = TFunctionRef<FString(const UWidgetBlueprint*, const UWidget*)>;
	using FGetParentWidgetIdFn = TFunctionRef<FString(const UWidgetBlueprint*, const UWidget*)>;
	using FGetWidgetSlotTypeFn = TFunctionRef<FString(const UWidgetBlueprint*, const UWidget*, FString*)>;
	using FGetPanelSlotTypeNameFn = TFunctionRef<FString(UPanelWidget*)>;
	using FResolveWidgetTreeNamedSlotNameFn =
		TFunctionRef<bool(UWidgetBlueprint*, const FString&, FName&, FMCPDiagnostic&)>;
	using FResolveNamedSlotNameFn = TFunctionRef<bool(UWidget*, const FString&, FName&, FMCPDiagnostic&)>;
	using FAttachWidgetToParentFn = TFunctionRef<bool(
		UWidgetBlueprint*,
		UWidget*,
		UWidget*,
		int32,
		bool,
		const FString&,
		FString&,
		FString&,
		FMCPDiagnostic&)>;
	using FDetachWidgetFromParentFn = TFunctionRef<bool(UWidgetBlueprint*, UWidget*, FMCPDiagnostic&)>;
	using FBuildGeneratedClassPathFn = TFunctionRef<FString(const FString&)>;
	using FSavePackageByNameFn = TFunctionRef<bool(const FString&, FMCPToolExecutionResult&)>;

	static bool HandleBlueprintPatch(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleBlueprintReparent(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FResolveClassByPathFn ResolveClassByPath,
		FBuildGeneratedClassPathFn BuildGeneratedClassPath,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleWidgetAdd(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FResolveWidgetFromRefFn ResolveWidgetFromRef,
		FResolveWidgetClassByPathFn ResolveWidgetClassByPath,
		FBuildUniqueWidgetNameFn BuildUniqueWidgetName,
		FGetWidgetStableIdFn GetWidgetStableId,
		FGetParentWidgetIdFn GetParentWidgetId,
		FGetWidgetSlotTypeFn GetWidgetSlotType,
		FGetPanelSlotTypeNameFn GetPanelSlotTypeName,
		FResolveWidgetTreeNamedSlotNameFn ResolveWidgetTreeNamedSlotName,
		FResolveNamedSlotNameFn ResolveNamedSlotName,
		FAttachWidgetToParentFn AttachWidgetToParent,
		FEnsureWidgetGuidMapFn EnsureWidgetGuidMap,
		FEnsureWidgetGuidEntryFn EnsureWidgetGuidEntry,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleWidgetRemove(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FResolveWidgetFromRefFn ResolveWidgetFromRef,
		FGetWidgetStableIdFn GetWidgetStableId,
		FGetParentWidgetIdFn GetParentWidgetId,
		FEnsureWidgetGuidMapFn EnsureWidgetGuidMap,
		FCollectWidgetSubtreeVariableNamesFn CollectWidgetSubtreeVariableNames,
		FRemoveWidgetGuidEntryFn RemoveWidgetGuidEntry,
		FSavePackageByNameFn SavePackageByName);
	static bool HandleWidgetReparent(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FResolveWidgetFromRefFn ResolveWidgetFromRef,
		FGetWidgetStableIdFn GetWidgetStableId,
		FGetParentWidgetIdFn GetParentWidgetId,
		FGetWidgetSlotTypeFn GetWidgetSlotType,
		FGetPanelSlotTypeNameFn GetPanelSlotTypeName,
		FResolveWidgetTreeNamedSlotNameFn ResolveWidgetTreeNamedSlotName,
		FResolveNamedSlotNameFn ResolveNamedSlotName,
		FAttachWidgetToParentFn AttachWidgetToParent,
		FDetachWidgetFromParentFn DetachWidgetFromParent,
		FEnsureWidgetGuidMapFn EnsureWidgetGuidMap,
		FSavePackageByNameFn SavePackageByName);

	static bool HandleWidgetPatch(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FResolveWidgetFromRefFn ResolveWidgetFromRef,
		FEnsureWidgetGuidMapFn EnsureWidgetGuidMap);
	static bool HandleSlotPatch(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FResolveWidgetFromRefFn ResolveWidgetFromRef,
		FEnsureWidgetGuidMapFn EnsureWidgetGuidMap);
	static bool HandleWidgetPatchV2(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FResolveWidgetFromRefFn ResolveWidgetFromRef,
		FEnsureWidgetGuidMapFn EnsureWidgetGuidMap);
	static bool HandleSlotPatchV2(
		const FMCPRequestEnvelope& Request,
		FMCPToolExecutionResult& OutResult,
		FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
		FResolveWidgetFromRefFn ResolveWidgetFromRef,
		FEnsureWidgetGuidMapFn EnsureWidgetGuidMap);
};
