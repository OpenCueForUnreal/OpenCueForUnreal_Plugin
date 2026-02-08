// Copyright OpenCue for Unreal contributors. MIT License.

#include "OpenCueJobDetailsCustomization.h"
#include "MoviePipelineOpenCueExecutorJob.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "OpenCueJobDetailsCustomization"

// ============================================================================
// FOpenCueJobDetailsCustomization
// ============================================================================

TSharedRef<IDetailCustomization> FOpenCueJobDetailsCustomization::MakeInstance()
{
	return MakeShareable(new FOpenCueJobDetailsCustomization);
}

void FOpenCueJobDetailsCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Get the object being edited
	TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
	DetailBuilder.GetObjectsBeingCustomized(ObjectsBeingCustomized);
	TSharedPtr<IPropertyHandle> PerJobGameModeHandle;

	FText AutoGameModeText = LOCTEXT("AutoGameModeNone", "None");
	if (ObjectsBeingCustomized.Num() == 1)
	{
		EditingJob = Cast<UMoviePipelineOpenCueExecutorJob>(ObjectsBeingCustomized[0].Get());
		if (EditingJob.IsValid() && EditingJob->OpenCueConfig.JobName.IsEmpty())
		{
			EditingJob->GenerateJobNameFromSequence();
		}
		if (EditingJob.IsValid())
		{
			FString ResolvedGameModeClass;
			FString ResolvedGameModeSource;
			EditingJob->ResolveCmdGameModeClass(ResolvedGameModeClass, ResolvedGameModeSource);

			if (!ResolvedGameModeClass.IsEmpty())
			{
				if (ResolvedGameModeSource == TEXT("JobOverride"))
				{
					AutoGameModeText = FText::FromString(
						FString::Printf(TEXT("%s (from MRQ OpenCue GameMode Override)"), *ResolvedGameModeClass));
				}
				else if (ResolvedGameModeSource == TEXT("MRQGameOverrideSetting"))
				{
					AutoGameModeText = FText::FromString(
						FString::Printf(TEXT("%s (from MRQ Game Overrides setting)"), *ResolvedGameModeClass));
				}
				else if (ResolvedGameModeSource == TEXT("MapOverride"))
				{
					AutoGameModeText = FText::FromString(
						FString::Printf(TEXT("%s (from Map GameMode Override)"), *ResolvedGameModeClass));
				}
				else
				{
					AutoGameModeText = FText::FromString(
						FString::Printf(TEXT("%s (fallback from OpenCue Settings)"), *ResolvedGameModeClass));
				}
			}
		}
	}

	// Add OpenCue category at the top
	IDetailCategoryBuilder& OpenCueCategory = DetailBuilder.EditCategory(
		"OpenCue",
		LOCTEXT("OpenCueCategoryName", "OpenCue"),
		ECategoryPriority::Important
	);

	// Force-show per-job GameMode override in top OpenCue panel as a class picker.
	TSharedRef<IPropertyHandle> OpenCueConfigHandle = DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UMoviePipelineOpenCueExecutorJob, OpenCueConfig),
		UMoviePipelineOpenCueExecutorJob::StaticClass());
	if (OpenCueConfigHandle->IsValidHandle())
	{
		PerJobGameModeHandle = OpenCueConfigHandle->GetChildHandle(
			GET_MEMBER_NAME_CHECKED(FOpenCueJobConfig, CmdGameModeOverrideClass));
	}

	// Add submit button row
	OpenCueCategory.AddCustomRow(LOCTEXT("SubmitRowFilter", "Submit"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SubmitLabel", "Submit OpenCue Job"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(200.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("SubmitButton", "Submit to OpenCue"))
			.ToolTipText(this, &FOpenCueJobDetailsCustomization::GetSubmitButtonTooltip)
			.IsEnabled(this, &FOpenCueJobDetailsCustomization::IsSubmitButtonEnabled)
			.OnClicked(this, &FOpenCueJobDetailsCustomization::OnSubmitToOpenCueClicked)
		];

	if (PerJobGameModeHandle.IsValid() && PerJobGameModeHandle->IsValidHandle())
	{
		OpenCueCategory.AddCustomRow(LOCTEXT("PerJobGameModeFilter", "PerJobGameMode"))
			.NameContent()
			[
				PerJobGameModeHandle->CreatePropertyNameWidget(
					LOCTEXT("PerJobGameModeLabel", "GameMode Override (-game mode)"),
					LOCTEXT("PerJobGameModeTooltip", "Optional per-job GameMode class picker. If empty, auto resolution uses MRQ Game Overrides setting, then map override, then OpenCue Settings fallback."))
			]
			.ValueContent()
			.MinDesiredWidth(420.f)
			[
				PerJobGameModeHandle->CreatePropertyValueWidget()
			];
	}

	OpenCueCategory.AddCustomRow(LOCTEXT("AutoGameModeRowFilter", "GameMode"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("AutoGameModeLabel", "GameMode (Auto)"))
			.ToolTipText(LOCTEXT("AutoGameModeTooltip", "Priority: MRQ OpenCue GameMode Override > MRQ Game Overrides setting > Map GameMode Override > OpenCue Settings fallback."))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(420.f)
		[
			SNew(STextBlock)
			.Text(AutoGameModeText)
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];
}

FReply FOpenCueJobDetailsCustomization::OnSubmitToOpenCueClicked()
{
	if (!EditingJob.IsValid())
	{
		return FReply::Handled();
	}

	FString ErrorMessage;
	bool bSuccess = EditingJob->SubmitToOpenCue(ErrorMessage);

	// Show notification
	FNotificationInfo Info(bSuccess
		? LOCTEXT("SubmitSuccess", "Job submitted to OpenCue")
		: FText::Format(LOCTEXT("SubmitFailed", "Failed to submit: {0}"), FText::FromString(ErrorMessage)));

	Info.bUseLargeFont = false;
	Info.bFireAndForget = true;
	Info.ExpireDuration = 5.0f;
	Info.bUseSuccessFailIcons = true;

	FSlateNotificationManager::Get().AddNotification(Info)->SetCompletionState(
		bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);

	return FReply::Handled();
}

bool FOpenCueJobDetailsCustomization::IsSubmitButtonEnabled() const
{
	if (!EditingJob.IsValid())
	{
		return false;
	}

	FString Reason;
	return EditingJob->CanSubmitToOpenCue(Reason);
}

FText FOpenCueJobDetailsCustomization::GetSubmitButtonTooltip() const
{
	if (!EditingJob.IsValid())
	{
		return LOCTEXT("NoJobTooltip", "No job selected");
	}

	FString Reason;
	if (EditingJob->CanSubmitToOpenCue(Reason))
	{
		return LOCTEXT("ReadyToSubmitTooltip", "Submit this job to OpenCue render farm");
	}

	return FText::Format(LOCTEXT("CannotSubmitTooltip", "Cannot submit: {0}"), FText::FromString(Reason));
}

// ============================================================================
// FOpenCueJobConfigCustomization
// ============================================================================

TSharedRef<IPropertyTypeCustomization> FOpenCueJobConfigCustomization::MakeInstance()
{
	return MakeShareable(new FOpenCueJobConfigCustomization);
}

void FOpenCueJobConfigCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	HeaderRow.NameContent()
		[
			PropertyHandle->CreatePropertyNameWidget()
		];
}

void FOpenCueJobConfigCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Get child property handles
	uint32 NumChildren = 0;
	PropertyHandle->GetNumChildren(NumChildren);

	for (uint32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		TSharedRef<IPropertyHandle> ChildHandle = PropertyHandle->GetChildHandle(ChildIndex).ToSharedRef();
		ChildBuilder.AddProperty(ChildHandle);
	}
}

#undef LOCTEXT_NAMESPACE
