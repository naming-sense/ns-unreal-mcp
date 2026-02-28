#include "Tools/UMG/MCPToolsUMGBindingHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
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

	FString EventName;
	Request.Params->TryGetStringField(TEXT("event_name"), EventName);
	if (EventName.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("event_name is required for umg.widget.event.bind.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	FMCPRequestEnvelope DelegatedRequest = Request;
	DelegatedRequest.Tool = TEXT("umg.binding.set");
	DelegatedRequest.Params = MakeShared<FJsonObject>();
	FString ObjectPath;
	Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	DelegatedRequest.Params->SetStringField(TEXT("object_path"), ObjectPath);
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	if (Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef) && WidgetRef != nullptr && WidgetRef->IsValid())
	{
		DelegatedRequest.Params->SetObjectField(TEXT("widget_ref"), *WidgetRef);
	}
	DelegatedRequest.Params->SetStringField(TEXT("property_name"), EventName);
	FString FunctionName;
	Request.Params->TryGetStringField(TEXT("function_name"), FunctionName);
	DelegatedRequest.Params->SetStringField(TEXT("function_name"), FunctionName);
	bool bCompileOnSuccess = true;
	Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	DelegatedRequest.Params->SetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	return HandleBindingSet(DelegatedRequest, OutResult, SavePackageByName);
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

	FMCPRequestEnvelope DelegatedRequest = Request;
	DelegatedRequest.Tool = TEXT("umg.binding.clear");
	DelegatedRequest.Params = MakeShared<FJsonObject>();
	FString ObjectPath;
	Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	DelegatedRequest.Params->SetStringField(TEXT("object_path"), ObjectPath);
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	if (Request.Params->TryGetObjectField(TEXT("widget_ref"), WidgetRef) && WidgetRef != nullptr && WidgetRef->IsValid())
	{
		DelegatedRequest.Params->SetObjectField(TEXT("widget_ref"), *WidgetRef);
	}
	FString EventName;
	Request.Params->TryGetStringField(TEXT("event_name"), EventName);
	if (!EventName.IsEmpty())
	{
		DelegatedRequest.Params->SetStringField(TEXT("property_name"), EventName);
	}
	FString FunctionName;
	Request.Params->TryGetStringField(TEXT("function_name"), FunctionName);
	if (!FunctionName.IsEmpty())
	{
		DelegatedRequest.Params->SetStringField(TEXT("function_name"), FunctionName);
	}
	bool bCompileOnSuccess = true;
	Request.Params->TryGetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	DelegatedRequest.Params->SetBoolField(TEXT("compile_on_success"), bCompileOnSuccess);
	return HandleBindingClear(DelegatedRequest, OutResult, SavePackageByName);
}
