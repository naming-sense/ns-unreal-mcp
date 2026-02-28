#include "Tools/Material/MCPToolsMaterialHandler.h"

#include "MCPErrorCodes.h"
#include "MCPObjectUtils.h"
#include "ScopedTransaction.h"
#include "Tools/Common/MCPToolCommonJson.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"

namespace
{
	bool IsKindAllowed(const TSet<FString>& AllowedKinds, const FString& Kind)
	{
		return AllowedKinds.Num() == 0 || AllowedKinds.Contains(Kind);
	}

	bool JsonToLinearColor(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		if (Value->Type == EJson::String)
		{
			FString ColorString;
			if (Value->TryGetString(ColorString))
			{
				return OutColor.InitFromString(ColorString);
			}
			return false;
		}

		if (Value->Type != EJson::Object)
		{
			return false;
		}

		const TSharedPtr<FJsonObject> ObjectValue = Value->AsObject();
		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 1.0;
		const bool bHasR = ObjectValue->TryGetNumberField(TEXT("r"), R);
		const bool bHasG = ObjectValue->TryGetNumberField(TEXT("g"), G);
		const bool bHasB = ObjectValue->TryGetNumberField(TEXT("b"), B);
		ObjectValue->TryGetNumberField(TEXT("a"), A);
		if (!bHasR || !bHasG || !bHasB)
		{
			return false;
		}

		OutColor = FLinearColor(
			static_cast<float>(R),
			static_cast<float>(G),
			static_cast<float>(B),
			static_cast<float>(A));
		return true;
	}
}

bool FMCPToolsMaterialHandler::HandleInstanceParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	bool bIncludeInherited = true;
	TSet<FString> AllowedKinds;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

		const TArray<TSharedPtr<FJsonValue>>* Kinds = nullptr;
		if (Request.Params->TryGetArrayField(TEXT("kinds"), Kinds) && Kinds != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& KindValue : *Kinds)
			{
				FString Kind;
				if (KindValue.IsValid() && KindValue->TryGetString(Kind))
				{
					AllowedKinds.Add(Kind);
				}
			}
		}
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for mat.instance.params.get.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMaterialInstance* MaterialInstance = LoadObject<UMaterialInstance>(nullptr, *ObjectPath);
	if (MaterialInstance == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Material instance not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	(void)bIncludeInherited;
	TArray<TSharedPtr<FJsonValue>> Params;

	if (IsKindAllowed(AllowedKinds, TEXT("scalar")))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		MaterialInstance->GetAllScalarParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			float Value = 0.0f;
			if (!MaterialInstance->GetScalarParameterValue(Info, Value))
			{
				continue;
			}

			TSharedRef<FJsonObject> ParamObject = MakeShared<FJsonObject>();
			ParamObject->SetStringField(TEXT("name"), Info.Name.ToString());
			ParamObject->SetStringField(TEXT("kind"), TEXT("scalar"));
			ParamObject->SetNumberField(TEXT("value"), Value);
			ParamObject->SetBoolField(TEXT("overridden"), true);
			ParamObject->SetStringField(TEXT("source"), TEXT("local"));
			Params.Add(MakeShared<FJsonValueObject>(ParamObject));
		}
	}

	if (IsKindAllowed(AllowedKinds, TEXT("vector")))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		MaterialInstance->GetAllVectorParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			FLinearColor Value = FLinearColor::Black;
			if (!MaterialInstance->GetVectorParameterValue(Info, Value))
			{
				continue;
			}

			TSharedRef<FJsonObject> ValueObject = MakeShared<FJsonObject>();
			ValueObject->SetNumberField(TEXT("r"), Value.R);
			ValueObject->SetNumberField(TEXT("g"), Value.G);
			ValueObject->SetNumberField(TEXT("b"), Value.B);
			ValueObject->SetNumberField(TEXT("a"), Value.A);

			TSharedRef<FJsonObject> ParamObject = MakeShared<FJsonObject>();
			ParamObject->SetStringField(TEXT("name"), Info.Name.ToString());
			ParamObject->SetStringField(TEXT("kind"), TEXT("vector"));
			ParamObject->SetObjectField(TEXT("value"), ValueObject);
			ParamObject->SetBoolField(TEXT("overridden"), true);
			ParamObject->SetStringField(TEXT("source"), TEXT("local"));
			Params.Add(MakeShared<FJsonValueObject>(ParamObject));
		}
	}

	if (IsKindAllowed(AllowedKinds, TEXT("texture")))
	{
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		MaterialInstance->GetAllTextureParameterInfo(Infos, Ids);
		for (const FMaterialParameterInfo& Info : Infos)
		{
			UTexture* Value = nullptr;
			if (!MaterialInstance->GetTextureParameterValue(Info, Value))
			{
				continue;
			}

			TSharedRef<FJsonObject> ParamObject = MakeShared<FJsonObject>();
			ParamObject->SetStringField(TEXT("name"), Info.Name.ToString());
			ParamObject->SetStringField(TEXT("kind"), TEXT("texture"));
			ParamObject->SetStringField(TEXT("value"), Value ? Value->GetPathName() : TEXT(""));
			ParamObject->SetBoolField(TEXT("overridden"), true);
			ParamObject->SetStringField(TEXT("source"), TEXT("local"));
			Params.Add(MakeShared<FJsonValueObject>(ParamObject));
		}
	}

	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("params"), Params);
	OutResult.Status = EMCPResponseStatus::Ok;
	return true;
}

bool FMCPToolsMaterialHandler::HandleInstanceParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult)
{
	FString ObjectPath;
	FString Recompile = TEXT("auto");
	const TArray<TSharedPtr<FJsonValue>>* SetValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ClearValues = nullptr;

	if (Request.Params.IsValid())
	{
		Request.Params->TryGetStringField(TEXT("object_path"), ObjectPath);
		Request.Params->TryGetStringField(TEXT("recompile"), Recompile);
		Request.Params->TryGetArrayField(TEXT("set"), SetValues);
		Request.Params->TryGetArrayField(TEXT("clear"), ClearValues);
	}

	if (ObjectPath.IsEmpty())
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::SCHEMA_INVALID_PARAMS;
		Diagnostic.Message = TEXT("object_path is required for mat.instance.params.set.");
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	UMaterialInstanceConstant* MaterialInstance = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
	if (MaterialInstance == nullptr)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::OBJECT_NOT_FOUND;
		Diagnostic.Message = TEXT("Material instance constant not found.");
		Diagnostic.Detail = ObjectPath;
		OutResult.Diagnostics.Add(Diagnostic);
		OutResult.Status = EMCPResponseStatus::Error;
		return false;
	}

	TArray<FString> UpdatedNames;
	FScopedTransaction Transaction(FText::FromString(TEXT("MCP Material Params Set")));
	MaterialInstance->Modify();

	auto EmitParamNotFound = [&OutResult](const FString& Name, const FString& Kind)
	{
		FMCPDiagnostic Diagnostic;
		Diagnostic.Code = MCPErrorCodes::MATERIAL_PARAM_NOT_FOUND;
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Message = TEXT("Unsupported or invalid material parameter update request.");
		Diagnostic.Detail = FString::Printf(TEXT("name=%s kind=%s"), *Name, *Kind);
		Diagnostic.bRetriable = false;
		OutResult.Diagnostics.Add(Diagnostic);
	};

	if (SetValues != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& SetValue : *SetValues)
		{
			if (!SetValue.IsValid() || SetValue->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> SetObject = SetValue->AsObject();
			FString Name;
			FString Kind;
			SetObject->TryGetStringField(TEXT("name"), Name);
			SetObject->TryGetStringField(TEXT("kind"), Kind);
			const TSharedPtr<FJsonValue>* ValueField = SetObject->Values.Find(TEXT("value"));

			if (Name.IsEmpty() || Kind.IsEmpty() || ValueField == nullptr || !ValueField->IsValid())
			{
				continue;
			}

			const FMaterialParameterInfo ParamInfo{ FName(*Name) };
			if (Kind.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				double ScalarValue = 0;
				if ((*ValueField)->TryGetNumber(ScalarValue))
				{
					MaterialInstance->SetScalarParameterValueEditorOnly(ParamInfo, static_cast<float>(ScalarValue));
					UpdatedNames.AddUnique(Name);
				}
				else
				{
					EmitParamNotFound(Name, Kind);
				}
				continue;
			}

			if (Kind.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				FLinearColor ColorValue = FLinearColor::Black;
				if (JsonToLinearColor(*ValueField, ColorValue))
				{
					MaterialInstance->SetVectorParameterValueEditorOnly(ParamInfo, ColorValue);
					UpdatedNames.AddUnique(Name);
				}
				else
				{
					EmitParamNotFound(Name, Kind);
				}
				continue;
			}

			if (Kind.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				FString TexturePath;
				if ((*ValueField)->TryGetString(TexturePath))
				{
					UTexture* Texture = TexturePath.IsEmpty() ? nullptr : LoadObject<UTexture>(nullptr, *TexturePath);
					MaterialInstance->SetTextureParameterValueEditorOnly(ParamInfo, Texture);
					UpdatedNames.AddUnique(Name);
				}
				else
				{
					EmitParamNotFound(Name, Kind);
				}
				continue;
			}

			EmitParamNotFound(Name, Kind);
		}
	}

	if (ClearValues != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& ClearValue : *ClearValues)
		{
			if (!ClearValue.IsValid() || ClearValue->Type != EJson::Object)
			{
				continue;
			}

			const TSharedPtr<FJsonObject> ClearObject = ClearValue->AsObject();
			FString Name;
			FString Kind;
			ClearObject->TryGetStringField(TEXT("name"), Name);
			ClearObject->TryGetStringField(TEXT("kind"), Kind);
			if (Name.IsEmpty() || Kind.IsEmpty())
			{
				continue;
			}

			const FMaterialParameterInfo ParamInfo{ FName(*Name) };
			if (Kind.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
			{
				MaterialInstance->SetScalarParameterValueEditorOnly(ParamInfo, 0.0f);
				UpdatedNames.AddUnique(Name);
			}
			else if (Kind.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				MaterialInstance->SetVectorParameterValueEditorOnly(ParamInfo, FLinearColor::Black);
				UpdatedNames.AddUnique(Name);
			}
			else if (Kind.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
			{
				MaterialInstance->SetTextureParameterValueEditorOnly(ParamInfo, nullptr);
				UpdatedNames.AddUnique(Name);
			}
			else
			{
				EmitParamNotFound(Name, Kind);
			}
		}
	}

	if (!Request.Context.bDryRun)
	{
		MaterialInstance->PostEditChange();
		MaterialInstance->MarkPackageDirty();
	}
	else
	{
		Transaction.Cancel();
	}

	MCPObjectUtils::AppendTouchedPackage(MaterialInstance, OutResult.TouchedPackages);
	OutResult.ResultObject = MakeShared<FJsonObject>();
	OutResult.ResultObject->SetArrayField(TEXT("updated"), MCPToolCommonJson::ToJsonStringArray(UpdatedNames));
	OutResult.ResultObject->SetStringField(TEXT("compile_status"), Recompile.Equals(TEXT("never"), ESearchCase::IgnoreCase) ? TEXT("skipped") : TEXT("unknown"));
	OutResult.Status = OutResult.Diagnostics.Num() > 0 ? EMCPResponseStatus::Partial : EMCPResponseStatus::Ok;
	return true;
}
