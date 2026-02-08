// Copyright OpenCue for Unreal contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "Input/Reply.h"

class UMoviePipelineOpenCueExecutorJob;

/**
 * Details panel customization for OpenCue Executor Job.
 * Adds a "Submit to OpenCue" button and customizes property display.
 */
class OPENCUEFORUNREALEDITOR_API FOpenCueJobDetailsCustomization : public IDetailCustomization
{
public:
	static TSharedRef<IDetailCustomization> MakeInstance();

	// IDetailCustomization interface
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	/** Handle submit button click */
	FReply OnSubmitToOpenCueClicked();

	/** Get submit button enabled state */
	bool IsSubmitButtonEnabled() const;

	/** Get submit button tooltip */
	FText GetSubmitButtonTooltip() const;

	/** Weak reference to the job being edited */
	TWeakObjectPtr<UMoviePipelineOpenCueExecutorJob> EditingJob;
};

/**
 * Property customization for FOpenCueJobConfig struct.
 */
class OPENCUEFORUNREALEDITOR_API FOpenCueJobConfigCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	// IPropertyTypeCustomization interface
	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& CustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& CustomizationUtils) override;
};
