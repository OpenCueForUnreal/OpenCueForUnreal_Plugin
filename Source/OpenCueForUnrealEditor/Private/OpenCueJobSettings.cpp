// Copyright OpenCue for Unreal contributors. MIT License.

#include "OpenCueJobSettings.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"

UOpenCueDeveloperSettings::UOpenCueDeveloperSettings()
{
}

FString UOpenCueDeveloperSettings::GetEffectivePythonPath() const
{
	if (!PythonPath.IsEmpty())
	{
		return PythonPath;
	}
	// Default to system Python
	return TEXT("python");
}

FString UOpenCueDeveloperSettings::GetEffectiveSubmitterCLIPath() const
{
	if (!SubmitterCLIPath.IsEmpty())
	{
		FString ExplicitPath = SubmitterCLIPath;
		if (FPaths::IsRelative(ExplicitPath))
		{
			ExplicitPath = FPaths::ConvertRelativePathToFull(ExplicitPath);
		}
		if (FPaths::FileExists(ExplicitPath) || FPaths::DirectoryExists(ExplicitPath))
		{
			return ExplicitPath;
		}
	}

	// Preferred runtime mode: bundled submitter executable inside plugin package.
	if (TSharedPtr<IPlugin> OpenCuePlugin = IPluginManager::Get().FindPlugin(TEXT("OpenCueForUnreal")))
	{
		const FString PluginBaseDir = OpenCuePlugin->GetBaseDir();
		const FString PluginExeCandidates[] = {
			FPaths::Combine(PluginBaseDir, TEXT("Source/ThirdParty/opencue-ue-submitter.exe")),
			FPaths::Combine(PluginBaseDir, TEXT("Binaries/Win64/opencue-ue-submitter.exe")),
			FPaths::Combine(PluginBaseDir, TEXT("Binaries/ThirdParty/opencue-ue-submitter.exe"))
		};

		for (const FString& Candidate : PluginExeCandidates)
		{
			const FString FullCandidate = FPaths::ConvertRelativePathToFull(Candidate);
			if (FPaths::FileExists(FullCandidate))
			{
				return FullCandidate;
			}
		}
	}

	// Dev fallback: source-tree Python module layout.
	FString ProjectDir = FPaths::ProjectDir();
	FString PossibleExePath = FPaths::Combine(ProjectDir, TEXT("../opencue-ue-services/dist/opencue-ue-submitter.exe"));
	if (FPaths::FileExists(PossibleExePath))
	{
		return FPaths::ConvertRelativePathToFull(PossibleExePath);
	}

	FString PossiblePath = FPaths::Combine(ProjectDir, TEXT("../opencue-ue-services"));
	if (FPaths::DirectoryExists(PossiblePath))
	{
		return FPaths::ConvertRelativePathToFull(PossiblePath);
	}

	return FString();
}

FString UOpenCueDeveloperSettings::GetEffectivePlanPublishDirectory() const
{
	if (!PlanPublishDirectory.IsEmpty())
	{
		return PlanPublishDirectory;
	}

	// Default to project's Saved/OpenCueRenderPlans directory
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OpenCueRenderPlans"));
}

FString FOpenCueJobConfig::GetEffectiveCuebotHost() const
{
	if (!CuebotHostOverride.IsEmpty())
	{
		return CuebotHostOverride;
	}

	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();
	return Settings ? Settings->CuebotHost : TEXT("localhost");
}

FString FOpenCueJobConfig::GetEffectiveShowName() const
{
	if (!ShowNameOverride.IsEmpty())
	{
		return ShowNameOverride;
	}

	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();
	return Settings ? Settings->ShowName : TEXT("UE_RENDER");
}
