// Copyright Epic Games, Inc. All Rights Reserved.
using UnrealBuildTool;

public class OpenCueForUnrealUtils : ModuleRules
{
    public OpenCueForUnrealUtils(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "MovieRenderPipelineCore",
                "MovieRenderPipelineSettings",
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",
                "Engine",
                "MovieRenderPipelineRenderPasses",
            }
        );
    }
}
