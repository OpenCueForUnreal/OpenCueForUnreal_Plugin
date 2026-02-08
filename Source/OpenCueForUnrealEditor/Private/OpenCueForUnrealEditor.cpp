// Copyright OpenCue for Unreal contributors. MIT License.

#include "OpenCueForUnrealEditor.h"
#include "OpenCueJobSettings.h"
#include "MoviePipelineOpenCueExecutorJob.h"
#include "OpenCueJobDetailsCustomization.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "FOpenCueForUnrealEditorModule"

void FOpenCueForUnrealEditorModule::StartupModule()
{
	UE_LOG(LogTemp, Display, TEXT("[OpenCue] Editor module starting..."));

	// Register property customizations
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	// Register OpenCue Executor Job customization
	PropertyModule.RegisterCustomClassLayout(
		UMoviePipelineOpenCueExecutorJob::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FOpenCueJobDetailsCustomization::MakeInstance)
	);

	// Register OpenCue Job Config struct customization
	PropertyModule.RegisterCustomPropertyTypeLayout(
		FOpenCueJobConfig::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FOpenCueJobConfigCustomization::MakeInstance)
	);

	PropertyModule.NotifyCustomizationModuleChanged();

	UE_LOG(LogTemp, Display, TEXT("[OpenCue] Editor module startup complete. Registered customizations for MRQ integration."));
}

void FOpenCueForUnrealEditorModule::ShutdownModule()
{
	// Unregister customizations if PropertyEditor module is still loaded
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");

		PropertyModule.UnregisterCustomClassLayout(UMoviePipelineOpenCueExecutorJob::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomPropertyTypeLayout(FOpenCueJobConfig::StaticStruct()->GetFName());
	}

	UE_LOG(LogTemp, Display, TEXT("[OpenCue] Editor module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpenCueForUnrealEditorModule, OpenCueForUnrealEditor)
