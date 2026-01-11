// Copyright Epic Games, Inc. All Rights Reserved.
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
                "MovieRenderPipelineCore",
                "MovieRenderPipelineEditor",
                "MovieRenderPipelineRenderPasses",
                "MovieRenderPipelineSettings",
                "EditorSubsystem",
            }
        );
    }
}
