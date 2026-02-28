#include "Tools/UMG/MCPToolsUMGStructureHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ContentWidget.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

namespace
{
	void ParseAutoSaveOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
	{
		bOutAutoSave = false;
		if (!Params.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* SaveObject = nullptr;
		if (Params->TryGetObjectField(TEXT("save"), SaveObject) && SaveObject != nullptr && SaveObject->IsValid())
		{
			(*SaveObject)->TryGetBoolField(TEXT("auto_save"), bOutAutoSave);
		}
	}

	FString ParseTransactionLabel(const TSharedPtr<FJsonObject>& Params, const FString& DefaultLabel)
	{
		if (!Params.IsValid())
		{
			return DefaultLabel;
		}

		const TSharedPtr<FJsonObject>* TransactionObject = nullptr;
		if (Params->TryGetObjectField(TEXT("transaction"), TransactionObject) && TransactionObject != nullptr && TransactionObject->IsValid())
		{
			FString Label = DefaultLabel;
			(*TransactionObject)->TryGetStringField(TEXT("label"), Label);
			return Label.IsEmpty() ? DefaultLabel : Label;
		}

		return DefaultLabel;
	}
}

bool FMCPToolsUMGStructureHandler::HandleBlueprintPatch(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;

	const TSharedPtr<FJsonObject>* DesignTimeSizeObject = nullptr;
	const TSharedPtr<FJsonValue>* DesignSizeModeValue = nullptr;
	bool bHasDesignTimeSize = false;
	bool bHasDesignSizeMode = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);

		bHasDesignTimeSize = Request.Params->HasField(TEXT("design_time_size"));
		if (bHasDesignTimeSize && !Request.Params->TryGetObjectField(TEXT("design_time_size"), DesignTimeSizeObject))
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			Diagnostic.Message = TEXT("design_time_size must be an object with x/y.");
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		bHasDesignSizeMode = Request.Params->HasField(TEXT("design_size_mode"));
		if (bHasDesignSizeMode)
		{
			DesignSizeModeValue = Request.Params->Values.Find(TEXT("design_size_mode"));
			if (DesignSizeModeValue == nullptr || !DesignSizeModeValue->IsValid())
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("design_size_mode must be string/number/null.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for umg.blueprint.patch.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!bHasDesignTimeSize && !bHasDesignSizeMode)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("At least one of design_time_size or design_size_mode is required.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> PatchOperations;

	if (bHasDesignTimeSize)
	{
		double X = 0.0;
		double Y = 0.0;
		const bool bHasX = DesignTimeSizeObject != nullptr && DesignTimeSizeObject->IsValid()
			&& ((*DesignTimeSizeObject)->TryGetNumberField(TEXT("x"), X) || (*DesignTimeSizeObject)->TryGetNumberField(TEXT("X"), X));
		const bool bHasY = DesignTimeSizeObject != nullptr && DesignTimeSizeObject->IsValid()
			&& ((*DesignTimeSizeObject)->TryGetNumberField(TEXT("y"), Y) || (*DesignTimeSizeObject)->TryGetNumberField(TEXT("Y"), Y));
		if (!bHasX || !bHasY)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			Diagnostic.Message = TEXT("design_time_size requires both x and y numeric fields.");
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		TSharedRef<FJsonObject> ValueObject = MakeShared<FJsonObject>();
		ValueObject->SetNumberField(TEXT("X"), X);
		ValueObject->SetNumberField(TEXT("Y"), Y);

		TSharedRef<FJsonObject> PatchObject = MakeShared<FJsonObject>();
		PatchObject->SetStringField(TEXT("op"), TEXT("replace"));
		PatchObject->SetStringField(TEXT("path"), TEXT("/DesignTimeSize"));
		PatchObject->SetObjectField(TEXT("value"), ValueObject);
		PatchOperations.Add(MakeShared<FJsonValueObject>(PatchObject));
	}

	if (bHasDesignSizeMode)
	{
		TSharedRef<FJsonObject> PatchObject = MakeShared<FJsonObject>();
		if ((*DesignSizeModeValue)->Type == EJson::Null)
		{
			PatchObject->SetStringField(TEXT("op"), TEXT("remove"));
		}
		else
		{
			PatchObject->SetStringField(TEXT("op"), TEXT("replace"));
			PatchObject->SetField(TEXT("value"), *DesignSizeModeValue);
		}
		PatchObject->SetStringField(TEXT("path"), TEXT("/DesignSizeMode"));
		PatchOperations.Add(MakeShared<FJsonValueObject>(PatchObject));
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;

	if (!Request.Context.bDryRun)
	{
		const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP UMG Blueprint Patch"));
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		WidgetBlueprint->Modify();

		if (!MCPObjectUtils::ApplyPatch(WidgetBlueprint, &PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		WidgetBlueprint->PostEditChange();
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(&PatchOperations, ChangedProperties);
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGStructureHandler::HandleBlueprintReparent(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
	FResolveClassByPathFn ResolveClassByPath,
	FBuildGeneratedClassPathFn BuildGeneratedClassPath,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	FString NewParentClassPath;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("new_parent_class_path"), NewParentClassPath);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || NewParentClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path and new_parent_class_path are required for umg.blueprint.reparent.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* NewParentClass = ResolveClassByPath(NewParentClassPath);
	if (NewParentClass == nullptr || !NewParentClass->IsChildOf(UUserWidget::StaticClass()))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("new_parent_class_path must resolve to a UUserWidget-derived class.");
		Diagnostic.Detail = NewParentClassPath;
		Diagnostic.Suggestion = TEXT("Use /Script/UMG.UserWidget or a Blueprintable UUserWidget subclass.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(NewParentClass))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::ASSET_CREATE_FAILED;
		Diagnostic.Message = TEXT("Widget Blueprint cannot be reparented to the requested parent class.");
		Diagnostic.Detail = NewParentClass->GetPathName();
		Diagnostic.Suggestion = TEXT("Use a Blueprintable UUserWidget parent class.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString OldParentClassPath = WidgetBlueprint->ParentClass
		? WidgetBlueprint->ParentClass->GetPathName()
		: FString();
	const FString ResolvedParentClassPath = NewParentClass->GetPathName();
	const bool bNoop = WidgetBlueprint->ParentClass == NewParentClass;

	TArray<FString> ChangedProperties;
	if (!bNoop)
	{
		ChangedProperties.Add(TEXT("ParentClass"));
	}

	if (!Request.Context.bDryRun && !bNoop)
	{
		const FString TransactionLabel = ParseTransactionLabel(Request.Params, TEXT("MCP UMG Blueprint Reparent"));
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		WidgetBlueprint->Modify();

		WidgetBlueprint->ParentClass = NewParentClass;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->PostEditChange();
		WidgetBlueprint->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun && !bNoop)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	if (!Request.Context.bDryRun && !bNoop)
	{
		const bool bParentApplied = WidgetBlueprint->ParentClass == NewParentClass;
		if (!bParentApplied)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			Diagnostic.Message = TEXT("Widget Blueprint parent class was not applied.");
			Diagnostic.Detail = FString::Printf(
				TEXT("expected=%s actual=%s"),
				*ResolvedParentClassPath,
				*(WidgetBlueprint->ParentClass ? WidgetBlueprint->ParentClass->GetPathName() : FString(TEXT("None"))));
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	const FString GeneratedClassPath = (WidgetBlueprint->GeneratedClass != nullptr)
		? WidgetBlueprint->GeneratedClass->GetPathName()
		: BuildGeneratedClassPath(ObjectPath);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("reparented"), !Request.Context.bDryRun && !bNoop);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("no_op"), bNoop);
	OutResult.ResultObject->SetStringField(TEXT("old_parent_class_path"), OldParentClassPath);
	OutResult.ResultObject->SetStringField(TEXT("new_parent_class_path"), ResolvedParentClassPath);
	OutResult.ResultObject->SetStringField(TEXT("generated_class_path"), GeneratedClassPath);
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGStructureHandler::HandleWidgetAdd(
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
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	FString WidgetClassPath;
	FString WidgetName;
	FString NamedSlotName;
	const TSharedPtr<FJsonObject>* ParentRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* WidgetPatchOperations = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* SlotPatchOperations = nullptr;
	bool bCompileOnSuccess = true;
	bool bReplaceContent = false;
	bool bAutoSave = false;
	int32 InsertIndex = -1;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("widget_class_path"), WidgetClassPath);
		Request.Params->TryGetStringField(TEXT("widget_name"), WidgetName);
		Request.Params->TryGetStringField(TEXT("named_slot_name"), NamedSlotName);
		Request.Params->TryGetObjectField(TEXT("parent_ref"), ParentRef);
		Request.Params->TryGetArrayField(TEXT("widget_patch"), WidgetPatchOperations);
		Request.Params->TryGetArrayField(TEXT("slot_patch"), SlotPatchOperations);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		Request.Params->TryGetBoolField(TEXT("replace_content"), bReplaceContent);

		double InsertIndexNumber = static_cast<double>(InsertIndex);
		Request.Params->TryGetNumberField(TEXT("insert_index"), InsertIndexNumber);
		InsertIndex = FMath::Clamp(static_cast<int32>(InsertIndexNumber), -1, 10000);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	if (ObjectPath.IsEmpty() || WidgetClassPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path and widget_class_path are required for umg.widget.add.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UClass* WidgetClass = ResolveWidgetClassByPath(WidgetClassPath);
	if (WidgetClass == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("widget_class_path must resolve to a concrete UWidget class.");
		Diagnostic.Detail = WidgetClassPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidget* ParentWidget = nullptr;
	if (ParentRef != nullptr && ParentRef->IsValid())
	{
		ParentWidget = ResolveWidgetFromRef(WidgetBlueprint, ParentRef);
		if (ParentWidget == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
			Diagnostic.Message = TEXT("parent_ref could not be resolved.");
			Diagnostic.Detail = ObjectPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}
	else if (WidgetBlueprint->WidgetTree->RootWidget != nullptr && NamedSlotName.IsEmpty())
	{
		ParentWidget = WidgetBlueprint->WidgetTree->RootWidget;
	}

	const FName GeneratedWidgetName = BuildUniqueWidgetName(WidgetBlueprint, WidgetClass, WidgetName);
	if (GeneratedWidgetName.IsNone())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
		Diagnostic.Message = TEXT("Failed to allocate widget name.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedWidgetProperties;
	TArray<FString> ChangedSlotProperties;
	FString AddedWidgetId = FString::Printf(TEXT("name:%s"), *GeneratedWidgetName.ToString());
	FString ParentId = ParentWidget ? GetWidgetStableId(WidgetBlueprint, ParentWidget) : FString();
	FString SlotType;
	FString ResolvedNamedSlotName;

	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Add")));
		WidgetBlueprint->Modify();

		UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, GeneratedWidgetName);
		if (NewWidget == nullptr)
		{
			Transaction.Cancel();
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			Diagnostic.Message = TEXT("Failed to construct widget instance.");
			Diagnostic.Detail = WidgetClassPath;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		NewWidget->Modify();
		FMCPDiagnostic AttachDiagnostic;
		if (!AttachWidgetToParent(
			WidgetBlueprint,
			ParentWidget,
			NewWidget,
			InsertIndex,
			bReplaceContent,
			NamedSlotName,
			ResolvedNamedSlotName,
			SlotType,
			AttachDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(AttachDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		EnsureWidgetGuidEntry(WidgetBlueprint, NewWidget);

		if (WidgetPatchOperations != nullptr && WidgetPatchOperations->Num() > 0)
		{
			FMCPDiagnostic PatchDiagnostic;
			if (!MCPObjectUtils::ApplyPatch(NewWidget, WidgetPatchOperations, ChangedWidgetProperties, PatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}

		if (SlotPatchOperations != nullptr && SlotPatchOperations->Num() > 0)
		{
			if (NewWidget->Slot == nullptr)
			{
				Transaction.Cancel();
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("slot_patch requires the widget to have a slot.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			NewWidget->Slot->Modify();
			FMCPDiagnostic SlotPatchDiagnostic;
			if (!MCPObjectUtils::ApplyPatch(NewWidget->Slot, SlotPatchOperations, ChangedSlotProperties, SlotPatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(SlotPatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}

		NewWidget->PostEditChange();
		if (NewWidget->Slot != nullptr)
		{
			NewWidget->Slot->PostEditChange();
		}

		EnsureWidgetGuidMap(WidgetBlueprint);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
		AddedWidgetId = GetWidgetStableId(WidgetBlueprint, NewWidget);
		ParentId = GetParentWidgetId(WidgetBlueprint, NewWidget);
		SlotType = GetWidgetSlotType(WidgetBlueprint, NewWidget, &ResolvedNamedSlotName);
	}
	else
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(WidgetPatchOperations, ChangedWidgetProperties);
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(SlotPatchOperations, ChangedSlotProperties);

		if (ParentWidget == nullptr && !NamedSlotName.IsEmpty())
		{
			FName ResolvedNamedSlot = NAME_None;
			FMCPDiagnostic NamedSlotDiagnostic;
			if (!ResolveWidgetTreeNamedSlotName(WidgetBlueprint, NamedSlotName, ResolvedNamedSlot, NamedSlotDiagnostic))
			{
				OutResult.Diagnostics.Add(NamedSlotDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			ResolvedNamedSlotName = ResolvedNamedSlot.ToString();
			if (WidgetBlueprint->WidgetTree->GetContentForSlot(ResolvedNamedSlot) != nullptr && !bReplaceContent)
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("Target top-level named slot already has content. Set replace_content=true to replace it.");
				Diagnostic.Detail = ResolvedNamedSlotName;
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			SlotType = FString::Printf(TEXT("NamedSlot:%s"), *ResolvedNamedSlotName);
		}
		else if (ParentWidget != nullptr)
		{
			const bool bIsPanelParent = Cast<UPanelWidget>(ParentWidget) != nullptr;
			const bool bIsContentParent = Cast<UContentWidget>(ParentWidget) != nullptr;
			const bool bUseNamedSlot =
				!NamedSlotName.IsEmpty() || (!bIsPanelParent && !bIsContentParent && Cast<INamedSlotInterface>(ParentWidget) != nullptr);
			if (bUseNamedSlot)
			{
				FName ResolvedNamedSlot = NAME_None;
				FMCPDiagnostic NamedSlotDiagnostic;
				if (!ResolveNamedSlotName(ParentWidget, NamedSlotName, ResolvedNamedSlot, NamedSlotDiagnostic))
				{
					OutResult.Diagnostics.Add(NamedSlotDiagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}

				ResolvedNamedSlotName = ResolvedNamedSlot.ToString();
				if (INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(ParentWidget))
				{
					if (NamedSlotHost->GetContentForSlot(ResolvedNamedSlot) != nullptr && !bReplaceContent)
					{
						FMCPDiagnostic Diagnostic;
						Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
						Diagnostic.Message = TEXT("Target named slot already has content. Set replace_content=true to replace it.");
						Diagnostic.Detail = ResolvedNamedSlotName;
						OutResult.Diagnostics.Add(Diagnostic);
						OutResult.Status = EMCPResponseStatus::Error;
						return false;
					}
				}
				SlotType = FString::Printf(TEXT("NamedSlot:%s"), *ResolvedNamedSlotName);
			}
			else if (UContentWidget* ContentParent = Cast<UContentWidget>(ParentWidget))
			{
				if (ContentParent->GetContent() != nullptr && !bReplaceContent)
				{
					FMCPDiagnostic Diagnostic;
					Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					Diagnostic.Message = TEXT("Target content widget already has a child. Set replace_content=true to replace it.");
					OutResult.Diagnostics.Add(Diagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}
			}
			else if (!Cast<UPanelWidget>(ParentWidget))
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("parent_ref must point to a panel/content/named-slot widget.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			if (UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget))
			{
				SlotType = GetPanelSlotTypeName(ParentPanel);
			}
		}
		else if (WidgetBlueprint->WidgetTree->RootWidget != nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
			Diagnostic.Message = TEXT("WidgetTree root already exists. Provide parent_ref or named_slot_name.");
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	TSharedRef<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
	WidgetObject->SetStringField(TEXT("widget_id"), AddedWidgetId);
	WidgetObject->SetStringField(TEXT("name"), GeneratedWidgetName.ToString());
	WidgetObject->SetStringField(TEXT("class_path"), WidgetClass->GetPathName());
	WidgetObject->SetStringField(TEXT("parent_id"), ParentId);
	WidgetObject->SetStringField(TEXT("slot_type"), SlotType);
	if (!ResolvedNamedSlotName.IsEmpty())
	{
		WidgetObject->SetStringField(TEXT("named_slot_name"), ResolvedNamedSlotName);
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("created"), !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetObjectField(TEXT("widget"), WidgetObject);
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedWidgetProperties));
	OutResult.ResultObject->SetArrayField(TEXT("slot_changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedSlotProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGStructureHandler::HandleWidgetRemove(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
	FResolveWidgetFromRefFn ResolveWidgetFromRef,
	FGetWidgetStableIdFn GetWidgetStableId,
	FGetParentWidgetIdFn GetParentWidgetId,
	FEnsureWidgetGuidMapFn EnsureWidgetGuidMap,
	FCollectWidgetSubtreeVariableNamesFn CollectWidgetSubtreeVariableNames,
	FRemoveWidgetGuidEntryFn RemoveWidgetGuidEntry,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
		Diagnostic.Message = TEXT("object_path and widget_ref must resolve to an existing widget.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString RemovedWidgetId = GetWidgetStableId(WidgetBlueprint, Widget);
	const FString RemovedWidgetName = Widget->GetName();
	const FString RemovedClassPath = Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString();
	const int32 RemovedChildrenCount = Cast<UPanelWidget>(Widget) ? Cast<UPanelWidget>(Widget)->GetChildrenCount() : 0;
	const FString RemovedParentId = GetParentWidgetId(WidgetBlueprint, Widget);

	bool bRemoved = true;
	TArray<FName> RemovedVariableNames;
	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
		CollectWidgetSubtreeVariableNames(Widget, RemovedVariableNames);
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Remove")));
		WidgetBlueprint->Modify();

		bRemoved = WidgetBlueprint->WidgetTree->RemoveWidget(Widget);
		if (!bRemoved)
		{
			Transaction.Cancel();
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::INTERNAL_EXCEPTION;
			Diagnostic.Message = TEXT("Failed to remove widget from widget tree.");
			Diagnostic.Detail = RemovedWidgetName;
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		for (const FName& RemovedVariableName : RemovedVariableNames)
		{
			RemoveWidgetGuidEntry(WidgetBlueprint, RemovedVariableName);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
		EnsureWidgetGuidMap(WidgetBlueprint);
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("removed"), !Request.Context.bDryRun && bRemoved);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetStringField(TEXT("removed_widget_id"), RemovedWidgetId);
	OutResult.ResultObject->SetStringField(TEXT("removed_widget_name"), RemovedWidgetName);
	OutResult.ResultObject->SetStringField(TEXT("removed_class_path"), RemovedClassPath);
	OutResult.ResultObject->SetStringField(TEXT("old_parent_id"), RemovedParentId);
	OutResult.ResultObject->SetNumberField(TEXT("removed_children_count"), RemovedChildrenCount);
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGStructureHandler::HandleWidgetReparent(
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
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	FString NamedSlotName;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	const TSharedPtr<FJsonObject>* NewParentRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* SlotPatchOperations = nullptr;
	bool bCompileOnSuccess = true;
	bool bReplaceContent = false;
	bool bSetAsRoot = false;
	bool bAutoSave = false;
	int32 InsertIndex = -1;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("named_slot_name"), NamedSlotName);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetObjectField(TEXT("new_parent_ref"), NewParentRef);
		Request.Params->TryGetArrayField(TEXT("slot_patch"), SlotPatchOperations);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		Request.Params->TryGetBoolField(TEXT("replace_content"), bReplaceContent);
		Request.Params->TryGetBoolField(TEXT("set_as_root"), bSetAsRoot);
		ParseAutoSaveOption(Request.Params, bAutoSave);

		double InsertIndexNumber = static_cast<double>(InsertIndex);
		Request.Params->TryGetNumberField(TEXT("insert_index"), InsertIndexNumber);
		InsertIndex = FMath::Clamp(static_cast<int32>(InsertIndexNumber), -1, 10000);
	}

	const bool bUseTopLevelNamedSlotTarget = !bSetAsRoot
		&& (NewParentRef == nullptr || !NewParentRef->IsValid())
		&& !NamedSlotName.IsEmpty();

	if (!bSetAsRoot && !bUseTopLevelNamedSlotTarget && (NewParentRef == nullptr || !NewParentRef->IsValid()))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("new_parent_ref is required unless set_as_root=true or named_slot_name targets a top-level named slot.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
		Diagnostic.Message = TEXT("object_path and widget_ref must resolve to an existing widget.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidget* NewParentWidget = nullptr;
	if (!bSetAsRoot)
	{
		if (!bUseTopLevelNamedSlotTarget)
		{
			NewParentWidget = ResolveWidgetFromRef(WidgetBlueprint, NewParentRef);
			if (NewParentWidget == nullptr)
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::UMG_WIDGET_NOT_FOUND;
				Diagnostic.Message = TEXT("new_parent_ref could not be resolved.");
				Diagnostic.Detail = ObjectPath;
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			if (NewParentWidget == Widget)
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("Widget cannot be reparented to itself.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			UWidget* CursorWidget = NewParentWidget;
			while (CursorWidget != nullptr)
			{
				if (CursorWidget == Widget)
				{
					FMCPDiagnostic Diagnostic;
					Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
					Diagnostic.Message = TEXT("Widget cannot be reparented into its own subtree.");
					OutResult.Diagnostics.Add(Diagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}
				CursorWidget = (CursorWidget->Slot != nullptr) ? CursorWidget->Slot->Parent : nullptr;
			}
		}
	}
	else if (WidgetBlueprint->WidgetTree->RootWidget != nullptr && WidgetBlueprint->WidgetTree->RootWidget != Widget)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("WidgetTree already has a root widget. set_as_root=true requires replacing root manually.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (bSetAsRoot && !NamedSlotName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("named_slot_name cannot be used when set_as_root=true.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString WidgetId = GetWidgetStableId(WidgetBlueprint, Widget);
	const FString WidgetName = Widget->GetName();
	const FString WidgetClassPath = Widget->GetClass() ? Widget->GetClass()->GetPathName() : FString();
	const FString OldParentId = GetParentWidgetId(WidgetBlueprint, Widget);
	TArray<FString> ChangedSlotProperties;
	FString NewParentId = bSetAsRoot ? FString() : (NewParentWidget ? GetWidgetStableId(WidgetBlueprint, NewParentWidget) : FString());
	FString SlotType;
	FString ResolvedNamedSlotName;

	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Reparent")));
		WidgetBlueprint->Modify();
		Widget->Modify();

		FMCPDiagnostic DetachDiagnostic;
		if (!DetachWidgetFromParent(WidgetBlueprint, Widget, DetachDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(DetachDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		if (bSetAsRoot)
		{
			WidgetBlueprint->WidgetTree->RootWidget = Widget;
		}
		else
		{
			FMCPDiagnostic AttachDiagnostic;
			if (!AttachWidgetToParent(
				WidgetBlueprint,
				NewParentWidget,
				Widget,
				InsertIndex,
				bReplaceContent,
				NamedSlotName,
				ResolvedNamedSlotName,
				SlotType,
				AttachDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(AttachDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
		}

		if (SlotPatchOperations != nullptr && SlotPatchOperations->Num() > 0)
		{
			if (Widget->Slot == nullptr)
			{
				Transaction.Cancel();
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("slot_patch requires the widget to have a slot after reparent.");
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			Widget->Slot->Modify();
			FMCPDiagnostic PatchDiagnostic;
			if (!MCPObjectUtils::ApplyPatch(Widget->Slot, SlotPatchOperations, ChangedSlotProperties, PatchDiagnostic))
			{
				Transaction.Cancel();
				OutResult.Diagnostics.Add(PatchDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}
			Widget->Slot->PostEditChange();
		}

		Widget->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
		EnsureWidgetGuidMap(WidgetBlueprint);

		NewParentId = GetParentWidgetId(WidgetBlueprint, Widget);
		SlotType = GetWidgetSlotType(WidgetBlueprint, Widget, &ResolvedNamedSlotName);
	}
	else
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(SlotPatchOperations, ChangedSlotProperties);

		if (!bSetAsRoot && NewParentWidget == nullptr && !NamedSlotName.IsEmpty())
		{
			FName ResolvedNamedSlot = NAME_None;
			FMCPDiagnostic NamedSlotDiagnostic;
			if (!ResolveWidgetTreeNamedSlotName(WidgetBlueprint, NamedSlotName, ResolvedNamedSlot, NamedSlotDiagnostic))
			{
				OutResult.Diagnostics.Add(NamedSlotDiagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			ResolvedNamedSlotName = ResolvedNamedSlot.ToString();
			UWidget* ExistingContent = WidgetBlueprint->WidgetTree->GetContentForSlot(ResolvedNamedSlot);
			if (ExistingContent != nullptr && ExistingContent != Widget && !bReplaceContent)
			{
				FMCPDiagnostic Diagnostic;
				Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
				Diagnostic.Message = TEXT("Target top-level named slot already has content. Set replace_content=true to replace it.");
				Diagnostic.Detail = ResolvedNamedSlotName;
				OutResult.Diagnostics.Add(Diagnostic);
				OutResult.Status = EMCPResponseStatus::Error;
				return false;
			}

			NewParentId = TEXT("this");
			SlotType = FString::Printf(TEXT("NamedSlot:%s"), *ResolvedNamedSlotName);
		}
		else if (!bSetAsRoot && NewParentWidget != nullptr)
		{
			const bool bIsPanelParent = Cast<UPanelWidget>(NewParentWidget) != nullptr;
			const bool bIsContentParent = Cast<UContentWidget>(NewParentWidget) != nullptr;
			const bool bUseNamedSlot =
				!NamedSlotName.IsEmpty() || (!bIsPanelParent && !bIsContentParent && Cast<INamedSlotInterface>(NewParentWidget) != nullptr);
			if (bUseNamedSlot)
			{
				FName ResolvedNamedSlot = NAME_None;
				FMCPDiagnostic NamedSlotDiagnostic;
				if (!ResolveNamedSlotName(NewParentWidget, NamedSlotName, ResolvedNamedSlot, NamedSlotDiagnostic))
				{
					OutResult.Diagnostics.Add(NamedSlotDiagnostic);
					OutResult.Status = EMCPResponseStatus::Error;
					return false;
				}

				ResolvedNamedSlotName = ResolvedNamedSlot.ToString();
				if (INamedSlotInterface* NamedSlotHost = Cast<INamedSlotInterface>(NewParentWidget))
				{
					UWidget* ExistingContent = NamedSlotHost->GetContentForSlot(ResolvedNamedSlot);
					if (ExistingContent != nullptr && ExistingContent != Widget && !bReplaceContent)
					{
						FMCPDiagnostic Diagnostic;
						Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
						Diagnostic.Message = TEXT("Target named slot already has content. Set replace_content=true to replace it.");
						Diagnostic.Detail = ResolvedNamedSlotName;
						OutResult.Diagnostics.Add(Diagnostic);
						OutResult.Status = EMCPResponseStatus::Error;
						return false;
					}
				}
				SlotType = FString::Printf(TEXT("NamedSlot:%s"), *ResolvedNamedSlotName);
			}
			else if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(NewParentWidget))
			{
				SlotType = GetPanelSlotTypeName(PanelWidget);
			}
		}
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	bool bAllSaved = true;
	if (!Request.Context.bDryRun && bAutoSave)
	{
		for (const FString& PackageName : OutResult.TouchedPackages)
		{
			bAllSaved &= SavePackageByName(PackageName, OutResult);
		}
	}

	TSharedRef<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
	WidgetObject->SetStringField(TEXT("widget_id"), WidgetId);
	WidgetObject->SetStringField(TEXT("name"), WidgetName);
	WidgetObject->SetStringField(TEXT("class_path"), WidgetClassPath);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetBoolField(TEXT("moved"), !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetObjectField(TEXT("widget"), WidgetObject);
	OutResult.ResultObject->SetStringField(TEXT("old_parent_id"), OldParentId);
	OutResult.ResultObject->SetStringField(TEXT("new_parent_id"), NewParentId);
	OutResult.ResultObject->SetStringField(TEXT("slot_type"), SlotType);
	if (!ResolvedNamedSlotName.IsEmpty())
	{
		OutResult.ResultObject->SetStringField(TEXT("named_slot_name"), ResolvedNamedSlotName);
	}
	OutResult.ResultObject->SetArrayField(TEXT("slot_changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedSlotProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGStructureHandler::HandleWidgetPatch(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
	FResolveWidgetFromRefFn ResolveWidgetFromRef,
	FEnsureWidgetGuidMapFn EnsureWidgetGuidMap)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;
	bool bCompileOnSuccess = true;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, widget_ref, and patch are required for umg.widget.patch.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;
	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
	}

	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Patch")));
		WidgetBlueprint->Modify();
		Widget->Modify();
		if (!MCPObjectUtils::ApplyPatch(Widget, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		Widget->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(PatchOperations, ChangedProperties);
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGStructureHandler::HandleSlotPatch(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
	FResolveWidgetFromRefFn ResolveWidgetFromRef,
	FEnsureWidgetGuidMapFn EnsureWidgetGuidMap)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr || PatchOperations == nullptr || Widget->Slot == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, widget_ref, and patch are required and widget must have a slot.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;
	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
	}
	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Slot Patch")));
		WidgetBlueprint->Modify();
		Widget->Slot->Modify();
		if (!MCPObjectUtils::ApplyPatch(Widget->Slot, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		Widget->Slot->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(PatchOperations, ChangedProperties);
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGStructureHandler::HandleWidgetPatchV2(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
	FResolveWidgetFromRefFn ResolveWidgetFromRef,
	FEnsureWidgetGuidMapFn EnsureWidgetGuidMap)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;
	bool bCompileOnSuccess = true;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, widget_ref, and patch are required for umg.widget.patch.v2.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;
	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Patch V2")));
		WidgetBlueprint->Modify();
		Widget->Modify();
		if (!MCPObjectUtils::ApplyPatchV2(Widget, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		Widget->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(PatchOperations, ChangedProperties);
	}

	TSharedRef<FJsonObject> CompileObject = MakeShared<FJsonObject>();
	if (bCompileOnSuccess && !Request.Context.bDryRun)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		CompileObject->SetStringField(TEXT("status"), TEXT("requested"));
	}
	else
	{
		CompileObject->SetStringField(TEXT("status"), TEXT("skipped"));
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsUMGStructureHandler::HandleSlotPatchV2(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FLoadWidgetBlueprintByPathFn LoadWidgetBlueprintByPath,
	FResolveWidgetFromRefFn ResolveWidgetFromRef,
	FEnsureWidgetGuidMapFn EnsureWidgetGuidMap)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr || Widget->Slot == nullptr || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, widget_ref, and patch are required and widget must have a slot for umg.slot.patch.v2.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;
	if (!Request.Context.bDryRun)
	{
		EnsureWidgetGuidMap(WidgetBlueprint);
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Slot Patch V2")));
		WidgetBlueprint->Modify();
		Widget->Slot->Modify();
		if (!MCPObjectUtils::ApplyPatchV2(Widget->Slot, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		Widget->Slot->PostEditChange();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}
	else
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(PatchOperations, ChangedProperties);
	}

	MCPObjectUtils::AppendTouchedPackage(WidgetBlueprint, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}
