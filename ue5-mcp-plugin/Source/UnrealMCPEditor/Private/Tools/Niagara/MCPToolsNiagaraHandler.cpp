#include "Tools/Niagara/MCPToolsNiagaraHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "ScopedTransaction.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Misc/SecureHash.h"

namespace
{
	FString HashToHexSha1(const FString& Input)
	{
		FTCHARToUTF8 Utf8Data(*Input);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8Data.Get(), Utf8Data.Length(), Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
	}

	FString BuildNiagaraCompatModuleKey(const FString& ObjectPath)
	{
		return FString::Printf(TEXT("compat|%s|default"), *HashToHexSha1(ObjectPath.ToLower()));
	}
}

bool FMCPToolsNiagaraHandler::HandleParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for niagara.params.get.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* NiagaraObject = LoadObject<UObject>(nullptr, *ObjectPath);
	if (NiagaraObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Niagara object not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> InspectedProperties;
	MCPObjectUtils::InspectObject(NiagaraObject, nullptr, 1, InspectedProperties);

	TArray<TSharedPtr<FJsonValue>> Params;
	for (const TSharedPtr<FJsonValue>& PropertyValue : InspectedProperties)
	{
		if (!PropertyValue.IsValid() || PropertyValue->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> PropertyObject = PropertyValue->AsObject();
		FString Name;
		FString CppType;
		PropertyObject->TryGetStringField(TEXT("name"), Name);
		PropertyObject->TryGetStringField(TEXT("cpp_type"), CppType);
		const TSharedPtr<FJsonValue>* ValueField = PropertyObject->Values.Find(TEXT("value"));

		TSharedRef<FJsonObject> ParamObject = MakeShared<FJsonObject>();
		ParamObject->SetStringField(TEXT("name"), Name);
		ParamObject->SetStringField(TEXT("type"), CppType);
		ParamObject->SetField(TEXT("value"), ValueField != nullptr ? *ValueField : MakeShared<FJsonValueNull>());
		ParamObject->SetBoolField(TEXT("exposed"), true);
		Params.Add(MakeShared<FJsonValueObject>(ParamObject));
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("params"), Params);
	OutResult.ResultObject->SetStringField(TEXT("compile_status"), TEXT("unknown"));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsNiagaraHandler::HandleParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	bool bStrictTypes = true;
	const TArray<TSharedPtr<FJsonValue>>* SetValues = nullptr;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetBoolField(TEXT("strict_types"), bStrictTypes);
		Request.Params->TryGetArrayField(TEXT("set"), SetValues);
	}

	(void)bStrictTypes;

	if (ObjectPath.IsEmpty() || SetValues == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path and set are required for niagara.params.set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* NiagaraObject = LoadObject<UObject>(nullptr, *ObjectPath);
	if (NiagaraObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Niagara object not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> PatchOperations;
	TArray<FString> UpdatedNames;
	for (const TSharedPtr<FJsonValue>& SetValue : *SetValues)
	{
		if (!SetValue.IsValid() || SetValue->Type != EJson::Object)
		{
			continue;
		}

		const TSharedPtr<FJsonObject> SetObject = SetValue->AsObject();
		FString Name;
		SetObject->TryGetStringField(TEXT("name"), Name);
		const TSharedPtr<FJsonValue>* ValueField = SetObject->Values.Find(TEXT("value"));
		if (Name.IsEmpty() || ValueField == nullptr || !ValueField->IsValid())
		{
			continue;
		}

		TSharedRef<FJsonObject> PatchObject = MakeShared<FJsonObject>();
		PatchObject->SetStringField(TEXT("op"), TEXT("replace"));
		PatchObject->SetStringField(TEXT("path"), FString::Printf(TEXT("/%s"), *Name));
		PatchObject->SetField(TEXT("value"), *ValueField);
		PatchOperations.Add(MakeShared<FJsonValueObject>(PatchObject));
		UpdatedNames.AddUnique(Name);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP Niagara Params Set")));
	NiagaraObject->Modify();

	FMCPDiagnostic PatchDiagnostic;
	TArray<FString> ChangedProperties;
	if (!MCPObjectUtils::ApplyPatch(NiagaraObject, &PatchOperations, ChangedProperties, PatchDiagnostic))
	{
		Transaction.Cancel();
		OutResult.Diagnostics.Add(PatchDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	if (!Request.Context.bDryRun)
	{
		NiagaraObject->PostEditChange();
		NiagaraObject->MarkPackageDirty();
	}
	else
	{
		Transaction.Cancel();
	}

	MCPObjectUtils::AppendTouchedPackage(NiagaraObject, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("updated"), MCPToolCommonJson::ToJsonStringArray(UpdatedNames));
	OutResult.ResultObject->SetStringField(TEXT("compile_status"), TEXT("unknown"));
	OutResult.ResultObject->SetArrayField(TEXT("errors"), MCPToolCommonJson::ToJsonStringArray({}));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsNiagaraHandler::HandleStackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	FString EmitterName;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("emitter_name"), EmitterName);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for niagara.stack.list.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* NiagaraObject = LoadObject<UObject>(nullptr, *ObjectPath);
	if (NiagaraObject == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Niagara object not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TSharedRef<FJsonObject> ModuleObject = MakeShared<FJsonObject>();
	ModuleObject->SetStringField(TEXT("module_key"), BuildNiagaraCompatModuleKey(ObjectPath));
	ModuleObject->SetStringField(TEXT("display_name"), TEXT("CompatibilityParameters"));
	ModuleObject->SetBoolField(TEXT("enabled"), true);
	ModuleObject->SetStringField(TEXT("category"), EmitterName.IsEmpty() ? TEXT("compat") : EmitterName);

	TArray<TSharedPtr<FJsonValue>> Modules;
	Modules.Add(MakeShared<FJsonValueObject>(ModuleObject));

	FMCPDiagnostic Diagnostic;
	Diagnostic.Code = MCPErrorCodes::NIAGARA_MODULE_KEY_NOT_FOUND;
	Diagnostic.Severity = TEXT("info");
	Diagnostic.Message = TEXT("Niagara stack adapter is running in compatibility mode.");
	Diagnostic.Suggestion = TEXT("Use returned module_key for niagara.stack.module.set_param.");
	OutResult.Diagnostics.Add(Diagnostic);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("modules"), Modules);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsNiagaraHandler::HandleStackModuleSetParam(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	FString ModuleKey;
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("module_key"), ModuleKey);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for niagara.stack.module.set_param.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	const FString ExpectedModuleKey = BuildNiagaraCompatModuleKey(ObjectPath);
	if (!ModuleKey.Equals(ExpectedModuleKey, ESearchCase::IgnoreCase))
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::NIAGARA_MODULE_KEY_NOT_FOUND;
		Diagnostic.Message = TEXT("module_key does not match compatibility key for the target object.");
		Diagnostic.Detail = FString::Printf(TEXT("expected=%s actual=%s"), *ExpectedModuleKey, *ModuleKey);
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	return HandleParamsSet(Request, OutResult);
}
