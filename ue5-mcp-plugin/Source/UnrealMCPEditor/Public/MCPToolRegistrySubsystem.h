#pragma once

#include "CoreMinimal.h"
#if __has_include("Subsystems/EditorSubsystem.h")
#include "Subsystems/EditorSubsystem.h"
#elif __has_include("EditorSubsystem.h")
#include "EditorSubsystem.h"
#else
#error "EditorSubsystem header not found. Check UnrealEd dependency."
#endif
#include "Templates/Function.h"
#include "HAL/CriticalSection.h"
#include "MCPTypes.h"
#include "MCPToolRegistrySubsystem.generated.h"

struct FMCPToolDefinition
{
	using FExecutor = TFunction<bool(const FMCPRequestEnvelope&, FMCPToolExecutionResult&)>;

	FString Name;
	FString Domain;
	FString Version = TEXT("1.0.0");
	bool bEnabled = true;
	bool bWriteTool = false;
	TSharedPtr<FJsonObject> ParamsSchema;
	TSharedPtr<FJsonObject> ResultSchema;
	FExecutor Executor;
};

UCLASS()
class UNREALMCPEDITOR_API UMCPToolRegistrySubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool ValidateRequest(const FMCPRequestEnvelope& Request, FMCPDiagnostic& OutDiagnostic) const;
	bool ExecuteTool(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool IsWriteTool(const FString& ToolName) const;

	bool BuildToolsList(
		bool bIncludeSchemas,
		const FString& DomainFilter,
		TArray<TSharedPtr<FJsonValue>>& OutTools) const;

	FString GetSchemaHash() const;
	FString GetProtocolVersion() const;
	TArray<FString> GetCapabilities() const;
	TArray<FString> GetRegisteredToolNames() const;

private:
	void RegisterBuiltInTools();
	void RegisterBuiltInToolsImpl();
	void RegisterTool(FMCPToolDefinition Definition);
	void LoadSchemaBundle();
	TSharedPtr<FJsonObject> FindSchemaObject(const FString& ToolName, const FString& SchemaKey) const;
	void RebuildSchemaHash();

	bool HandleToolsList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSystemHealth(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleEditorLiveCodingCompile(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleAssetFind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleAssetLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleAssetSave(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleAssetImport(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleAssetCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleBlueprintClassCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGBlueprintCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGBlueprintPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGBlueprintReparent(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleAssetDuplicate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleAssetRename(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleAssetDelete(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSettingsProjectGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSettingsProjectPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSettingsProjectApply(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSettingsGameModeGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSettingsGameModeSetDefault(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSettingsGameModeCompose(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSettingsGameModeSetMapOverride(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSettingsGameModeRemoveMapOverride(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleChangeSetList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleChangeSetGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleChangeSetRollbackPreview(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleChangeSetRollbackApply(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleJobGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleJobCancel(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleObjectInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleObjectPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleObjectPatchV2(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleWorldOutlinerList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleWorldSelectionGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleWorldSelectionSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleMatInstanceParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleMatInstanceParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleNiagaraParamsGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleNiagaraParamsSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleNiagaraStackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleNiagaraStackModuleSetParam(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetClassList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGTreeGet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGSlotInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetReparent(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGSlotPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetPatchV2(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGSlotPatchV2(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGAnimationList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGAnimationCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGAnimationRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGAnimationTrackAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGAnimationKeySet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGAnimationKeyRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGBindingList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGBindingSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGBindingClear(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetEventBind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGWidgetEventUnbind(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleUMGGraphSummary(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqAssetCreate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqAssetLoad(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqInspect(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqBindingList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqTrackList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqSectionList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqChannelList(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqBindingAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqBindingRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqTrackAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqTrackRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqSectionAdd(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqSectionPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqSectionRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqKeySet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqKeyRemove(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqKeyBulkSet(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqPlaybackPatch(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqSave(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;
	bool HandleSeqValidate(const FMCPRequestEnvelope& Request, FMCPToolExecutionResult& OutResult) const;

	FString BuildDeleteConfirmationToken(const TArray<FString>& ObjectPaths, bool bFailIfReferenced) const;
	bool ConsumeDeleteConfirmationToken(const FString& Token, const TArray<FString>& ObjectPaths, bool bFailIfReferenced) const;
	void ReclaimExpiredDeleteConfirmationTokens() const;
	FString BuildSettingsConfirmationToken(const FString& Signature) const;
	bool ConsumeSettingsConfirmationToken(const FString& Token, const FString& Signature) const;
	void ReclaimExpiredSettingsConfirmationTokens() const;

private:
	struct FPendingDeleteConfirmation
	{
		TArray<FString> ObjectPaths;
		bool bFailIfReferenced = true;
		FDateTime ExpiresAtUtc;
	};

	struct FPendingSettingsConfirmation
	{
		FString Signature;
		FDateTime ExpiresAtUtc;
	};

private:
	TMap<FString, FMCPToolDefinition> RegisteredTools;
	TMap<FString, TSharedPtr<FJsonObject>> BundleSchemas;
	FString CachedSchemaHash;
	mutable FCriticalSection DeleteConfirmationGuard;
	mutable TMap<FString, FPendingDeleteConfirmation> PendingDeleteConfirmations;
	mutable FCriticalSection SettingsConfirmationGuard;
	mutable TMap<FString, FPendingSettingsConfirmation> PendingSettingsConfirmations;
};
