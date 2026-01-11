// Copyright Epic Games, Inc. All Rights Reserved.
using UnrealBuildTool;

public class OpenCueForUnreal : ModuleRules
{
    public OpenCueForUnreal(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
            }
        );
    }
}
