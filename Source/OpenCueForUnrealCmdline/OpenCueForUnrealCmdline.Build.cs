// Copyright Epic Games, Inc. All Rights Reserved.
using UnrealBuildTool;

public class OpenCueForUnrealCmdline : ModuleRules
{
    public OpenCueForUnrealCmdline(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
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
                "MovieRenderPipelineCore",
                "MovieRenderPipelineRenderPasses",
                "MovieRenderPipelineSettings",
                "Json",
                "JsonUtilities",
                "HTTP",
                "LevelSequence",
            }
        );
    }
}
