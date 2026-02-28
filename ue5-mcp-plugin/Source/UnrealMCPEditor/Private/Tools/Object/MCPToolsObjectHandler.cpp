#include "Tools/Object/MCPToolsObjectHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "ScopedTransaction.h"

bool FMCPToolsObjectHandler::HandleInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	const TSharedPtr<FJsonObject>* TargetObject = nullptr;
	const TSharedPtr<FJsonObject>* FiltersObject = nullptr;
	int32 Depth = 2;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetObjectField(TEXT("target"), TargetObject);
		Request.Params->TryGetObjectField(TEXT("filters"), FiltersObject);

		double DepthNumber = static_cast<double>(Depth);
		Request.Params->TryGetNumberField(TEXT("depth"), DepthNumber);
		Depth = FMath::Clamp(static_cast<int32>(DepthNumber), 0, 5);
	}

	if (TargetObject == nullptr || !TargetObject->IsValid())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("target is required for object.inspect.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* ResolvedObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!MCPObjectUtils::ResolveTargetObject(*TargetObject, ResolvedObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Properties;
	MCPObjectUtils::InspectObject(
		ResolvedObject,
		(FiltersObject != nullptr) ? *FiltersObject : nullptr,
		Depth,
		Properties);

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("properties"), Properties);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsObjectHandler::HandlePatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	const TSharedPtr<FJsonObject>* TargetObject = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;
	FString TransactionLabel = TEXT("MCP Patch");

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetObjectField(TEXT("target"), TargetObject);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);

		const TSharedPtr<FJsonObject>* TransactionObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("transaction"), TransactionObject) && TransactionObject != nullptr && TransactionObject->IsValid())
		{
			(*TransactionObject)->TryGetStringField(TEXT("label"), TransactionLabel);
		}
	}

	if (TargetObject == nullptr || !TargetObject->IsValid() || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("target and patch are required for object.patch.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* ResolvedObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!MCPObjectUtils::ResolveTargetObject(*TargetObject, ResolvedObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;

	if (Request.Context.bDryRun)
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(PatchOperations, ChangedProperties);
	}
	else
	{
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		ResolvedObject->Modify();
		if (!MCPObjectUtils::ApplyPatch(ResolvedObject, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		ResolvedObject->PostEditChange();
		ResolvedObject->MarkPackageDirty();
	}

	MCPObjectUtils::AppendTouchedPackage(ResolvedObject, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsObjectHandler::HandlePatchV2(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	const TSharedPtr<FJsonObject>* TargetObject = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PatchOperations = nullptr;
	FString TransactionLabel = TEXT("MCP Patch V2");
	if (Request.Params.IsValid())
	{
		Request.Params->TryGetObjectField(TEXT("target"), TargetObject);
		Request.Params->TryGetArrayField(TEXT("patch"), PatchOperations);
		const TSharedPtr<FJsonObject>* TransactionObject = nullptr;
		if (Request.Params->TryGetObjectField(TEXT("transaction"), TransactionObject) && TransactionObject != nullptr && TransactionObject->IsValid())
		{
			(*TransactionObject)->TryGetStringField(TEXT("label"), TransactionLabel);
		}
	}

	if (TargetObject == nullptr || !TargetObject->IsValid() || PatchOperations == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("target and patch are required for object.patch.v2.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UObject* ResolvedObject = nullptr;
	FMCPDiagnostic ResolveDiagnostic;
	if (!MCPObjectUtils::ResolveTargetObject(*TargetObject, ResolvedObject, ResolveDiagnostic))
	{
		OutResult.Diagnostics.Add(ResolveDiagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> ChangedProperties;
	FMCPDiagnostic PatchDiagnostic;
	if (!Request.Context.bDryRun)
	{
		FScopedTransaction Transaction(FText::FromString(TransactionLabel));
		ResolvedObject->Modify();
		if (!MCPObjectUtils::ApplyPatchV2(ResolvedObject, PatchOperations, ChangedProperties, PatchDiagnostic))
		{
			Transaction.Cancel();
			OutResult.Diagnostics.Add(PatchDiagnostic);
			OutResult.Status = EMCPResponseStatus::Error;
			return false;
		}

		ResolvedObject->PostEditChange();
		ResolvedObject->MarkPackageDirty();
	}
	else
	{
		MCPToolCommonJson::CollectChangedPropertiesFromPatchOperations(PatchOperations, ChangedProperties);
	}

	MCPObjectUtils::AppendTouchedPackage(ResolvedObject, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("changed_properties"), MCPToolCommonJson::ToJsonStringArray(ChangedProperties));
	OutResult.ResultObject->SetArrayField(TEXT("touched_packages"), MCPToolCommonJson::ToJsonStringArray(OutResult.TouchedPackages));
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}
