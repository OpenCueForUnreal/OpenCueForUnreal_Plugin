// Copyright OpenCue for Unreal contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "MoviePipelineQueue.h"
#include "OpenCueJobSettings.h"
#include "Dom/JsonObject.h"
#include "MoviePipelineOpenCueExecutorJob.generated.h"

/**
 * Represents a single task in the render plan.
 * Each task corresponds to one shot (or one frame range segment of a shot).
 */
USTRUCT()
struct FOpenCueRenderTask
{
	GENERATED_BODY()

	/** Task index (0..N-1), maps to CUE_FRAME */
	int32 TaskIndex = 0;

	/** Shot name (used by -ShotName arg) */
	FString ShotName;

	/** Optional outer name (subscene name) */
	FString OuterName;

	/** Optional inner name */
	FString InnerName;

	/** Frame range start (optional, -1 if not set) */
	int32 FrameStart = -1;

	/** Frame range end (optional, -1 if not set) */
	int32 FrameEnd = -1;

	/** If true, execution command should skip -ShotName filtering even if ShotName is set */
	bool bDisableShotFilter = false;

	/** Convert to JSON object for render_plan.json */
	TSharedPtr<FJsonObject> ToJsonObject() const;
};

/**
 * Result of submitting to OpenCue.
 */
USTRUCT(BlueprintType)
struct FOpenCueSubmitResult
{
	GENERATED_BODY()

	/** Whether submission was successful */
	UPROPERTY(BlueprintReadOnly)
	bool bSuccess = false;

	/** Job ID from render_plan */
	UPROPERTY(BlueprintReadOnly)
	FString JobId;

	/** OpenCue job IDs returned by Cuebot */
	UPROPERTY(BlueprintReadOnly)
	TArray<FString> OpenCueJobIds;

	/** Error message if failed */
	UPROPERTY(BlueprintReadOnly)
	FString ErrorMessage;

	/** Hint for fixing the error */
	UPROPERTY(BlueprintReadOnly)
	FString ErrorHint;
};

/**
 * Movie Pipeline Executor Job for OpenCue submission.
 *
 * This job type appears in the Movie Render Queue and allows users to
 * configure OpenCue-specific settings before submitting to the render farm.
 *
 * V1 Submission Flow:
 *   1. Extract shots from LevelSequence
 *   2. Expand discontinuous frame ranges into multiple tasks
 *   3. Generate render_plan.json
 *   4. Publish plan to get plan_uri
 *   5. Generate submit_spec.json
 *   6. Call Submitter CLI (CreateProc)
 *   7. Parse stdout JSON result
 *
 * Usage:
 *   1. Open Movie Render Queue window
 *   2. Add a new job or select existing job
 *   3. In Job settings, change Job Type to "OpenCue Job"
 *   4. Configure render settings (Quality, Format, etc.)
 *   5. Click "Submit to OpenCue" button
 */
UCLASS(BlueprintType)
class OPENCUEFORUNREALEDITOR_API UMoviePipelineOpenCueExecutorJob : public UMoviePipelineExecutorJob
{
	GENERATED_BODY()

public:
	UMoviePipelineOpenCueExecutorJob();

	/** OpenCue job configuration */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenCue")
	FOpenCueJobConfig OpenCueConfig;

	/** Auto-generate OpenCue job name from UE MRQ Job name (fallback: sequence name) */
	UFUNCTION(BlueprintCallable, Category = "OpenCue")
	void GenerateJobNameFromSequence();

	/** Submit this job to OpenCue (V1 implementation) */
	UFUNCTION(BlueprintCallable, Category = "OpenCue")
	bool SubmitToOpenCue(FString& OutErrorMessage);

	/** Submit and get detailed result */
	UFUNCTION(BlueprintCallable, Category = "OpenCue")
	FOpenCueSubmitResult SubmitToOpenCueWithResult();

	/** Check if job is ready to submit */
	UFUNCTION(BlueprintCallable, Category = "OpenCue")
	bool CanSubmitToOpenCue(FString& OutReason) const;

	/**
	 * Resolve command-line GameMode class for this render job.
	 * Priority:
	 *   1) MRQ OpenCue per-job GameMode override
	 *   2) MRQ native Game Overrides setting (UMoviePipelineGameOverrideSetting::GameModeOverride)
	 *   3) Map's WorldSettings GameMode Override
	 *   4) Project fallback from OpenCue Settings (CmdGameModeClass)
	 *   5) Empty (no override)
	 */
	void ResolveCmdGameModeClass(FString& OutGameModeClass, FString& OutSource) const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

protected:
	// ==================== V1 Submission Pipeline ====================

	/**
	 * Extract shots from the LevelSequence.
	 * Returns tasks with shot info. Frame ranges are NOT expanded yet.
	 */
	TArray<FOpenCueRenderTask> ExtractShotsFromSequence() const;

	/**
	 * Expand any discontinuous frame ranges into multiple tasks.
	 * V1: We don't support discontinuous ranges directly, so this is
	 * mainly for future-proofing. Each shot becomes one task.
	 */
	TArray<FOpenCueRenderTask> ExpandTasksForFrameRanges(const TArray<FOpenCueRenderTask>& InTasks) const;

	/**
	 * Generate render_plan.json content.
	 * @param JobId - UUID for this job
	 * @param Tasks - List of render tasks
	 * @return JSON string of the render plan
	 */
	FString GenerateRenderPlanJson(const FString& JobId, const TArray<FOpenCueRenderTask>& Tasks) const;

	/**
	 * Publish render_plan.json to get a plan_uri.
	 * V1: Simply writes to PlanPublishDirectory and returns file:// URI.
	 * @param JobId - Job UUID (used for filename)
	 * @param RenderPlanJson - Content to write
	 * @param OutPlanUri - Resulting URI that workers can access
	 * @param OutError - Error message if failed
	 * @return true if successful
	 */
	bool PublishRenderPlan(const FString& JobId, const FString& RenderPlanJson, FString& OutPlanUri, FString& OutError) const;

	/**
	 * Generate submit_spec.json content.
	 * @param JobId - UUID for this job
	 * @param PlanUri - URI to the published render_plan.json
	 * @param TaskCount - Number of tasks
	 * @return JSON string of the submit spec
	 */
	FString GenerateSubmitSpecJson(const FString& JobId, const FString& PlanUri, int32 TaskCount) const;

	/**
	 * Build the worker command that OpenCue will execute.
	 * This command is called for each task with CUE_FRAME env var set.
	 */
	FString BuildWrapperCommand(const FString& PlanUri) const;

	/**
	 * Call the Submitter CLI and parse the result.
	 * @param SubmitSpecPath - Path to submit_spec.json
	 * @param OutResult - Parsed result
	 * @return true if CLI executed successfully (even if submission failed)
	 */
	bool CallSubmitterCLI(const FString& SubmitSpecPath, FOpenCueSubmitResult& OutResult) const;

	/**
	 * Parse the stdout JSON from Submitter CLI.
	 * Expected format: {"ok":true/false, "job_id":"...", "opencue_job_ids":["..."], "error":"...", "hint":"..."}
	 */
	bool ParseSubmitterOutput(const FString& StdOut, FOpenCueSubmitResult& OutResult) const;

	// ==================== Legacy (deprecated) ====================

	/** Build command line arguments for the render job (legacy, not used in V1) */
	FString BuildCommandLineArgs() const;
};
