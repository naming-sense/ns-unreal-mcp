#pragma once

#include "CoreMinimal.h"
#include "MCPTypes.h"

class UClass;
class UPanelWidget;
class UWidget;
class UWidgetBlueprint;
class FJsonObject;

namespace MCPToolUMGUtils
{
	UWidgetBlueprint* LoadWidgetBlueprintByPath(const FString& ObjectPath);
	UWidget* ResolveWidgetFromRef(UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRefObject);
	FString GetWidgetStableId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget);
	void CollectWidgetSubtreeVariableNames(UWidget* Widget, TArray<FName>& OutVariableNames);
	void RemoveWidgetGuidEntry(UWidgetBlueprint* WidgetBlueprint, const FName WidgetName);
	void EnsureWidgetGuidMap(UWidgetBlueprint* WidgetBlueprint);
	void EnsureWidgetGuidEntry(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget);
	UClass* ResolveWidgetClassByPath(const FString& WidgetClassPath);
	FString GetPanelSlotTypeName(UPanelWidget* PanelWidget);
	FName BuildUniqueWidgetName(UWidgetBlueprint* WidgetBlueprint, UClass* WidgetClass, const FString& RequestedName);
	FString GetParentWidgetId(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget);
	FString GetWidgetSlotType(const UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget, FString* OutNamedSlotName = nullptr);
	bool ResolveNamedSlotName(
		UWidget* ParentWidget,
		const FString& RequestedNamedSlotName,
		FName& OutSlotName,
		FMCPDiagnostic& OutDiagnostic);
	bool ResolveWidgetTreeNamedSlotName(
		UWidgetBlueprint* WidgetBlueprint,
		const FString& RequestedNamedSlotName,
		FName& OutSlotName,
		FMCPDiagnostic& OutDiagnostic);
	bool AttachWidgetToParent(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* ParentWidget,
		UWidget* ChildWidget,
		int32 InsertIndex,
		bool bReplaceContent,
		const FString& NamedSlotName,
		FString& OutResolvedNamedSlotName,
		FString& OutResolvedSlotType,
		FMCPDiagnostic& OutDiagnostic);
	bool DetachWidgetFromParent(
		UWidgetBlueprint* WidgetBlueprint,
		UWidget* Widget,
		FMCPDiagnostic& OutDiagnostic);
}
