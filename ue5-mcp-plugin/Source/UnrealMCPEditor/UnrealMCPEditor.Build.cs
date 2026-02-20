using UnrealBuildTool;

public class UnrealMCPEditor : ModuleRules
{
    public UnrealMCPEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Json",
                "JsonUtilities",
                "EditorSubsystem",
                "UnrealEd",
                "UnrealMCP"
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "AssetRegistry",
                "AssetTools",
                "EngineSettings",
                "Projects",
                "WebSocketNetworking",
                "Kismet",
                "KismetCompiler",
                "Slate",
                "SlateCore",
                "UMG",
                "UMGEditor"
            });
    }
}
