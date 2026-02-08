// Copyright OpenCue for Unreal contributors. MIT License.

using UnrealBuildTool;

public class OpenCueForUnrealEditor : ModuleRules
{
    public OpenCueForUnrealEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "OpenCueForUnreal",
                "OpenCueForUnrealUtils",
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "Slate",
                "SlateCore",
                "UnrealEd",
                "Json",
                "JsonUtilities",
                "HTTP",
                "LevelSequence",
                "MovieScene",
                "MovieSceneTracks",
                "MovieRenderPipelineCore",
                "MovieRenderPipelineEditor",
                "MovieRenderPipelineRenderPasses",
                "MovieRenderPipelineSettings",
                "EditorSubsystem",
                "PropertyEditor",
                "InputCore",
                "DeveloperSettings",
                "Projects",
            }
        );
    }
}
