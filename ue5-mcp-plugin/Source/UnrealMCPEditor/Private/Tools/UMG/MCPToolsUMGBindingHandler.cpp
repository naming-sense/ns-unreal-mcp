#include "Tools/UMG/MCPToolsUMGBindingHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace
{
	bool ParseAutoSaveOption(const TSharedPtr<FJsonObject>& Params, bool& bOutAutoSave)
	{
		bOutAutoSave = false;
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* SaveObject = nullptr;
		if (Params->TryGetObjectField(TEXT("save"), SaveObject) && SaveObject != nullptr && SaveObject->IsValid())
		{
			(*SaveObject)->TryGetBoolField(TEXT("auto_save"), bOutAutoSave);
		}
		return true;
	}

	UWidgetBlueprint* LoadWidgetBlueprintByPath(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}

		return LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
	}

	UWidget* ResolveWidgetFromRef(UWidgetBlueprint* WidgetBlueprint, const TSharedPtr<FJsonObject>* WidgetRefObject)
	{
		if (WidgetBlueprint == nullptr || WidgetBlueprint->WidgetTree == nullptr || WidgetRefObject == nullptr || !WidgetRefObject->IsValid())
		{
			return nullptr;
		}

		FString WidgetId;
		FString WidgetName;
		(*WidgetRefObject)->TryGetStringField(TEXT("widget_id"), WidgetId);
		(*WidgetRefObject)->TryGetStringField(TEXT("name"), WidgetName);

		TArray<FString> CandidateNames;
		if (!WidgetName.IsEmpty())
		{
			CandidateNames.AddUnique(WidgetName);
		}

		if (!WidgetId.IsEmpty())
		{
			if (WidgetId.StartsWith(TEXT("name:")))
			{
				CandidateNames.AddUnique(WidgetId.RightChop(5));
			}
			else
			{
				FGuid ParsedGuid;
				if (!FGuid::Parse(WidgetId, ParsedGuid))
				{
					CandidateNames.AddUnique(WidgetId);
				}
			}
		}

		if (CandidateNames.Num() == 0)
		{
			return nullptr;
		}

		TArray<UWidget*> AllWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(AllWidgets);
		for (UWidget* Widget : AllWidgets)
		{
			if (Widget == nullptr)
			{
				continue;
			}

			for (const FString& CandidateName : CandidateNames)
			{
				if (Widget->GetName().Equals(CandidateName, ESearchCase::IgnoreCase))
				{
					return Widget;
				}
			}
		}

		return nullptr;
	}

	bool TryReadStructFieldAsString(
		const UStruct* OwnerStruct,
		const void* OwnerData,
		const TArray<FString>& CandidateNames,
		FString& OutValue)
	{
		OutValue.Reset();
		if (OwnerStruct == nullptr || OwnerData == nullptr)
		{
			return false;
		}

		for (const FString& CandidateName : CandidateNames)
		{
			FProperty* Property = OwnerStruct->FindPropertyByName(FName(*CandidateName));
			if (Property == nullptr)
			{
				continue;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(OwnerData);
			if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
			{
				OutValue = NameProperty->GetPropertyValue(ValuePtr).ToString();
				return true;
			}

			if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
			{
				OutValue = StringProperty->GetPropertyValue(ValuePtr);
				return true;
			}

			if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
			{
				OutValue = TextProperty->GetPropertyValue(ValuePtr).ToString();
				return true;
			}
		}

		return false;
	}

	bool TryWriteStructFieldFromString(
		const UStruct* OwnerStruct,
		void* OwnerData,
		const TArray<FString>& CandidateNames,
		const FString& Value)
	{
		if (OwnerStruct == nullptr || OwnerData == nullptr)
		{
			return false;
		}

		for (const FString& CandidateName : CandidateNames)
		{
			FProperty* Property = OwnerStruct->FindPropertyByName(FName(*CandidateName));
			if (Property == nullptr)
			{
				continue;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(OwnerData);
			if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
			{
				NameProperty->SetPropertyValue(ValuePtr, FName(*Value));
				return true;
			}

			if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
			{
				StringProperty->SetPropertyValue(ValuePtr, Value);
				return true;
			}

			if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
			{
				TextProperty->SetPropertyValue(ValuePtr, FText::FromString(Value));
				return true;
			}
		}

		return false;
	}

	bool TryGetWidgetBlueprintBindingArray(UWidgetBlueprint* WidgetBlueprint, FArrayProperty*& OutArrayProperty, void*& OutArrayPtr)
	{
		OutArrayProperty = nullptr;
		OutArrayPtr = nullptr;
		if (WidgetBlueprint == nullptr)
		{
			return false;
		}

		OutArrayProperty = FindFProperty<FArrayProperty>(WidgetBlueprint->GetClass(), TEXT("Bindings"));
		if (OutArrayProperty == nullptr || !OutArrayProperty->Inner->IsA<FStructProperty>())
		{
			return false;
		}

		OutArrayPtr = OutArrayProperty->ContainerPtrToValuePtr<void>(WidgetBlueprint);
		return OutArrayPtr != nullptr;
	}

	FObjectProperty* FindWidgetVariableProperty(UWidgetBlueprint* WidgetBlueprint, const UWidget* Widget)
	{
		if (WidgetBlueprint == nullptr || Widget == nullptr)
		{
			return nullptr;
		}

		const FName WidgetName = Widget->GetFName();
		if (WidgetName.IsNone())
		{
			return nullptr;
		}

		if (WidgetBlueprint->SkeletonGeneratedClass != nullptr)
		{
			if (FObjectProperty* SkeletonProperty = FindFProperty<FObjectProperty>(WidgetBlueprint->SkeletonGeneratedClass, WidgetName))
			{
				return SkeletonProperty;
			}
		}

		if (WidgetBlueprint->GeneratedClass != nullptr)
		{
			if (FObjectProperty* GeneratedProperty = FindFProperty<FObjectProperty>(WidgetBlueprint->GeneratedClass, WidgetName))
			{
				return GeneratedProperty;
			}
		}

		return nullptr;
	}

	FString ResolveEventFunctionName(const UK2Node_ComponentBoundEvent* EventNode)
	{
		if (EventNode == nullptr)
		{
			return FString();
		}

		if (!EventNode->CustomFunctionName.IsNone())
		{
			return EventNode->CustomFunctionName.ToString();
		}

		return EventNode->GetFunctionName().ToString();
	}

	void GatherWidgetDelegateEventNames(UClass* WidgetClass, TArray<FString>& OutEventNames)
	{
		OutEventNames.Reset();
		if (WidgetClass == nullptr)
		{
			return;
		}

		for (TFieldIterator<FMulticastDelegateProperty> It(WidgetClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FMulticastDelegateProperty* DelegateProperty = *It;
			if (DelegateProperty == nullptr)
			{
				continue;
			}

			OutEventNames.Add(DelegateProperty->GetName());
		}

		OutEventNames.Sort();
	}
}

bool FMCPToolsUMGBindingHandler::HandleBindingSet(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	FString ObjectName;
	FString PropertyName;
	FString FunctionName;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	bool bReplace = true;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetStringField(TEXT("object_name"), ObjectName);
		Request.Params->TryGetStringField(TEXT("property_name"), PropertyName);
		Request.Params->TryGetStringField(TEXT("function_name"), FunctionName);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		Request.Params->TryGetBoolField(TEXT("replace"), bReplace);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (Widget != nullptr)
	{
		ObjectName = Widget->GetName();
	}

	if (WidgetBlueprint == nullptr || ObjectName.IsEmpty() || PropertyName.IsEmpty() || FunctionName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, property_name, function_name and (widget_ref|object_name) are required for umg.binding.set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FArrayProperty* BindingsArrayProperty = nullptr;
	void* BindingsArrayPtr = nullptr;
	if (!TryGetWidgetBlueprintBindingArray(WidgetBlueprint, BindingsArrayProperty, BindingsArrayPtr))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.BINDING.UNSUPPORTED");
		Diagnostic.Message = TEXT("Current engine/widget blueprint does not expose Bindings array via reflection.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FStructProperty* BindingStructProperty = CastField<FStructProperty>(BindingsArrayProperty->Inner);
	FScriptArrayHelper ArrayHelper(BindingsArrayProperty, BindingsArrayPtr);

	int32 ExistingIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		const void* EntryPtr = ArrayHelper.GetRawPtr(Index);
		FString EntryObjectName;
		FString EntryPropertyName;
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("ObjectName"), TEXT("WidgetName") }, EntryObjectName);
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("PropertyName"), TEXT("DelegatePropertyName") }, EntryPropertyName);
		if (EntryObjectName.Equals(ObjectName, ESearchCase::IgnoreCase) && EntryPropertyName.Equals(PropertyName, ESearchCase::IgnoreCase))
		{
			ExistingIndex = Index;
			break;
		}
	}

	int32 AppliedIndex = ExistingIndex;
	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Binding Set")));
		WidgetBlueprint->Modify();

		if (ExistingIndex == INDEX_NONE)
		{
			AppliedIndex = ArrayHelper.AddValue();
		}
		else if (!bReplace)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = MCPErrorCodes::IDEMPOTENCY_CONFLICT;
			Diagnostic.Message = TEXT("Binding already exists and replace=false.");
			Diagnostic.Detail = FString::Printf(TEXT("object=%s property=%s"), *ObjectName, *PropertyName);
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		void* EntryPtr = ArrayHelper.GetRawPtr(AppliedIndex);
		if (ExistingIndex == INDEX_NONE)
		{
			BindingStructProperty->InitializeValue(EntryPtr);
		}
		TryWriteStructFieldFromString(BindingStructProperty->Struct, EntryPtr, { TEXT("ObjectName"), TEXT("WidgetName") }, ObjectName);
		TryWriteStructFieldFromString(BindingStructProperty->Struct, EntryPtr, { TEXT("PropertyName"), TEXT("DelegatePropertyName") }, PropertyName);
		TryWriteStructFieldFromString(BindingStructProperty->Struct, EntryPtr, { TEXT("FunctionName"), TEXT("SourceFunctionName") }, FunctionName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
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
	OutResult.ResultObject->SetBoolField(TEXT("bound"), !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetStringField(TEXT("object_name"), ObjectName);
	OutResult.ResultObject->SetStringField(TEXT("property_name"), PropertyName);
	OutResult.ResultObject->SetStringField(TEXT("function_name"), FunctionName);
	OutResult.ResultObject->SetNumberField(TEXT("binding_index"), AppliedIndex);
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGBindingHandler::HandleBindingClear(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	FString ObjectName;
	FString PropertyName;
	FString FunctionName;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
		Request.Params->TryGetStringField(TEXT("object_name"), ObjectName);
		Request.Params->TryGetStringField(TEXT("property_name"), PropertyName);
		Request.Params->TryGetStringField(TEXT("function_name"), FunctionName);
		Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
		ParseAutoSaveOption(Request.Params, bAutoSave);
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (Widget != nullptr)
	{
		ObjectName = Widget->GetName();
	}

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

	FArrayProperty* BindingsArrayProperty = nullptr;
	void* BindingsArrayPtr = nullptr;
	if (!TryGetWidgetBlueprintBindingArray(WidgetBlueprint, BindingsArrayProperty, BindingsArrayPtr))
	{
		TArray<TSharedPtr<FJsonValue>> EmptyRemoved;
		OutResult.ResultObject = MakeShared<FJsonObject>();
		OutResult.ResultObject->SetNumberField(TEXT("removed_count"), 0);
		OutResult.ResultObject->SetArrayField(TEXT("removed"), EmptyRemoved);
		OutResult.Status = EMCPResponseStatus::Ok;
		return true;
	}

	const FStructProperty* BindingStructProperty = CastField<FStructProperty>(BindingsArrayProperty->Inner);
	FScriptArrayHelper ArrayHelper(BindingsArrayProperty, BindingsArrayPtr);
	TArray<int32> RemoveIndices;
	TArray<FString> RemovedDescriptions;
	for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
	{
		const void* EntryPtr = ArrayHelper.GetRawPtr(Index);
		FString EntryObjectName;
		FString EntryPropertyName;
		FString EntryFunctionName;
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("ObjectName"), TEXT("WidgetName") }, EntryObjectName);
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("PropertyName"), TEXT("DelegatePropertyName") }, EntryPropertyName);
		TryReadStructFieldAsString(BindingStructProperty->Struct, EntryPtr, { TEXT("FunctionName"), TEXT("SourceFunctionName") }, EntryFunctionName);

		const bool bObjectMatch = ObjectName.IsEmpty() || EntryObjectName.Equals(ObjectName, ESearchCase::IgnoreCase);
		const bool bPropertyMatch = PropertyName.IsEmpty() || EntryPropertyName.Equals(PropertyName, ESearchCase::IgnoreCase);
		const bool bFunctionMatch = FunctionName.IsEmpty() || EntryFunctionName.Equals(FunctionName, ESearchCase::IgnoreCase);
		if (bObjectMatch && bPropertyMatch && bFunctionMatch)
		{
			RemoveIndices.Add(Index);
			RemovedDescriptions.Add(FString::Printf(TEXT("%s.%s -> %s"), *EntryObjectName, *EntryPropertyName, *EntryFunctionName));
		}
	}

	if (!Request.Context.bDryRun && RemoveIndices.Num() > 0)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Binding Clear")));
		WidgetBlueprint->Modify();
		RemoveIndices.Sort();
		for (int32 RemoveIdx = RemoveIndices.Num() - 1; RemoveIdx >= 0; --RemoveIdx)
		{
			ArrayHelper.RemoveValues(RemoveIndices[RemoveIdx], 1);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
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
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetNumberField(TEXT("removed_count"), RemoveIndices.Num());
	OutResult.ResultObject->SetArrayField(TEXT("removed"), MCPToolCommonJson::ToJsonStringArray(RemovedDescriptions));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGBindingHandler::HandleWidgetEventBind(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	if (!Request.Params.IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("params are required for umg.widget.event.bind.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	FString EventName;
	FString FunctionName;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
	Request.Params->TryGetStringField(TEXT("event_name"), EventName);
	Request.Params->TryGetStringField(TEXT("function_name"), FunctionName);
	Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	ParseAutoSaveOption(Request.Params, bAutoSave);

	if (EventName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("event_name is required for umg.widget.event.bind.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}
	if (ObjectPath.IsEmpty() || WidgetRef == nullptr || !WidgetRef->IsValid() || FunctionName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path, widget_ref, event_name, and function_name are required for umg.widget.event.bind.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint or widget_ref target not found.");
		Diagnostic.Detail = FString::Printf(TEXT("object_path=%s"), *ObjectPath);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FName EventNameFName(*EventName);
	FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), EventNameFName);
	if (DelegateProperty == nullptr)
	{
		TArray<FString> SupportedEvents;
		GatherWidgetDelegateEventNames(Widget->GetClass(), SupportedEvents);

		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.EVENT.NOT_SUPPORTED");
		Diagnostic.Message = TEXT("event_name is not a multicast delegate on the target widget class.");
		Diagnostic.Detail = FString::Printf(TEXT("widget=%s class=%s event=%s"), *Widget->GetName(), *Widget->GetClass()->GetPathName(), *EventName);
		Diagnostic.Suggestion = SupportedEvents.Num() > 0
			? FString::Printf(TEXT("Use one of supported events: %s"), *FString::Join(SupportedEvents, TEXT(", ")))
			: TEXT("Target widget class does not expose bindable multicast delegate events.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FObjectProperty* ComponentProperty = FindWidgetVariableProperty(WidgetBlueprint, Widget);
	if (ComponentProperty == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = TEXT("MCP.UMG.WIDGET_VARIABLE_REQUIRED");
		Diagnostic.Message = TEXT("Target widget must be exposed as variable to bind blueprint events.");
		Diagnostic.Detail = FString::Printf(TEXT("widget=%s"), *Widget->GetName());
		Diagnostic.Suggestion = TEXT("Enable 'Is Variable' for the widget and retry.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<UK2Node_ComponentBoundEvent*> ExistingNodes;
	FKismetEditorUtilities::FindAllBoundEventsForComponent(WidgetBlueprint, ComponentProperty->GetFName(), ExistingNodes);

	UK2Node_ComponentBoundEvent* MatchingNode = nullptr;
	bool bEventAlreadyBound = false;
	for (UK2Node_ComponentBoundEvent* CandidateNode : ExistingNodes)
	{
		if (CandidateNode == nullptr || CandidateNode->DelegatePropertyName != DelegateProperty->GetFName())
		{
			continue;
		}

		bEventAlreadyBound = true;
		const FString CandidateFunctionName = ResolveEventFunctionName(CandidateNode);
		if (CandidateFunctionName.Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			MatchingNode = CandidateNode;
			break;
		}
	}

	const bool bAlreadyBoundBefore = MatchingNode != nullptr || bEventAlreadyBound;
	if (MatchingNode == nullptr && bEventAlreadyBound)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::IDEMPOTENCY_CONFLICT;
		Diagnostic.Message = TEXT("event_name already bound to another function on this widget.");
		Diagnostic.Detail = FString::Printf(TEXT("widget=%s event=%s function=%s"), *Widget->GetName(), *EventName, *FunctionName);
		Diagnostic.Suggestion = TEXT("Use umg.widget.event.unbind first, then bind desired function.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!Request.Context.bDryRun && MatchingNode == nullptr)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Event Bind")));
		WidgetBlueprint->Modify();

		FKismetEditorUtilities::CreateNewBoundEventForClass(
			Widget->GetClass(),
			DelegateProperty->GetFName(),
			WidgetBlueprint,
			ComponentProperty);

		MatchingNode = const_cast<UK2Node_ComponentBoundEvent*>(
			FKismetEditorUtilities::FindBoundEventForComponent(
				WidgetBlueprint,
				DelegateProperty->GetFName(),
				ComponentProperty->GetFName()));

		if (MatchingNode == nullptr)
		{
			FMCPDiagnostic Diagnostic;
			Diagnostic.Code = TEXT("MCP.UMG.EVENT.BIND_FAILED");
			Diagnostic.Message = TEXT("Failed to create component bound event node.");
			Diagnostic.Detail = FString::Printf(TEXT("widget=%s event=%s"), *Widget->GetName(), *EventName);
			OutResult.Diagnostics.Add(Diagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		MatchingNode->Modify();
		MatchingNode->CustomFunctionName = FName(*FunctionName);
		MatchingNode->bOverrideFunction = false;

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
	}

	const FString ResolvedFunctionName = MatchingNode != nullptr
		? ResolveEventFunctionName(MatchingNode)
		: FunctionName;

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
	OutResult.ResultObject->SetBoolField(TEXT("bound"), !Request.Context.bDryRun);
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetStringField(TEXT("object_name"), Widget->GetName());
	OutResult.ResultObject->SetStringField(TEXT("property_name"), DelegateProperty->GetFName().ToString());
	OutResult.ResultObject->SetStringField(TEXT("function_name"), ResolvedFunctionName);
	OutResult.ResultObject->SetNumberField(TEXT("binding_index"), -1);
	OutResult.ResultObject->SetBoolField(TEXT("already_bound"), bAlreadyBoundBefore);
	OutResult.ResultObject->SetStringField(TEXT("binding_kind"), TEXT("k2_component_bound_event"));
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}

bool FMCPToolsUMGBindingHandler::HandleWidgetEventUnbind(
	const FMCPRequestEnvelope& Request,
	FMCPToolExecutionResult& OutResult,
	FSavePackageByNameFn SavePackageByName)
{
	if (!Request.Params.IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("params are required for umg.widget.event.unbind.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FString ObjectPath;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	FString EventName;
	FString FunctionName;
	bool bCompileOnSuccess = true;
	bool bAutoSave = false;
	Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef);
	Request.Params->TryGetStringField(TEXT("event_name"), EventName);
	Request.Params->TryGetStringField(TEXT("function_name"), FunctionName);
	Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	ParseAutoSaveOption(Request.Params, bAutoSave);

	if (ObjectPath.IsEmpty() || WidgetRef == nullptr || !WidgetRef->IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path and widget_ref are required for umg.widget.event.unbind.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint = LoadWidgetBlueprintByPath(ObjectPath);
	UWidget* Widget = ResolveWidgetFromRef(WidgetBlueprint, WidgetRef);
	if (WidgetBlueprint == nullptr || Widget == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Widget blueprint or widget_ref target not found.");
		Diagnostic.Detail = FString::Printf(TEXT("object_path=%s"), *ObjectPath);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FObjectProperty* ComponentProperty = FindWidgetVariableProperty(WidgetBlueprint, Widget);
	if (ComponentProperty == nullptr)
	{
		OutResult.ResultObject = MakeShared<FJsonObject>();
		OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
		OutResult.ResultObject->SetNumberField(TEXT("removed_count"), 0);
		OutResult.ResultObject->SetArrayField(TEXT("removed"), TArray<TSharedPtr<FJsonValue>>());
		OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), TArray<TSharedPtr<FJsonValue>>());
		OutResult.Status = EMCPResponseStatus::Ok;
		return true;
	}

	const FName EventNameFilter = EventName.IsEmpty() ? NAME_None : FName(*EventName);
	const bool bHasFunctionFilter = !FunctionName.IsEmpty();

	TArray<UK2Node_ComponentBoundEvent*> ExistingNodes;
	FKismetEditorUtilities::FindAllBoundEventsForComponent(WidgetBlueprint, ComponentProperty->GetFName(), ExistingNodes);

	TArray<UK2Node_ComponentBoundEvent*> RemoveNodes;
	TArray<TSharedPtr<FJsonValue>> RemovedEntries;
	for (UK2Node_ComponentBoundEvent* Node : ExistingNodes)
	{
		if (Node == nullptr)
		{
			continue;
		}

		if (!EventNameFilter.IsNone() && Node->DelegatePropertyName != EventNameFilter)
		{
			continue;
		}

		const FString NodeFunctionName = ResolveEventFunctionName(Node);
		if (bHasFunctionFilter && !NodeFunctionName.Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedRef<FJsonObject> RemovedObject = MakeShared<FJsonObject>();
		RemovedObject->SetStringField(TEXT("object_name"), Widget->GetName());
		RemovedObject->SetStringField(TEXT("event_name"), Node->DelegatePropertyName.ToString());
		RemovedObject->SetStringField(TEXT("function_name"), NodeFunctionName);
		RemovedEntries.Add(MakeShared<FJsonValueObject>(RemovedObject));

		RemoveNodes.Add(Node);
	}

	if (!Request.Context.bDryRun && RemoveNodes.Num() > 0)
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("MCP UMG Widget Event Unbind")));
		WidgetBlueprint->Modify();

		for (UK2Node_ComponentBoundEvent* Node : RemoveNodes)
		{
			if (Node == nullptr)
			{
				continue;
			}

			UEdGraph* Graph = Node->GetGraph();
			if (Graph != nullptr)
			{
				Graph->Modify();
			}

			Node->Modify();
			Node->DestroyNode();
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		WidgetBlueprint->MarkPackageDirty();
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
	OutResult.ResultObject->SetBoolField(TEXT("dry_run"), Request.Context.bDryRun);
	OutResult.ResultObject->SetNumberField(TEXT("removed_count"), RemoveNodes.Num());
	OutResult.ResultObject->SetArrayField(TEXT("removed"), RemovedEntries);
	OutResult.ResultObject->SetObjectField(TEXT("compile"), CompileObject);
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = bAllSaved ? EMCPResponseStatus::Ok : EMCPResponseStatus::Partial;
	return true;
}
