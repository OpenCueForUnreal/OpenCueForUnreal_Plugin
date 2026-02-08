#pragma once

#include "CoreMinimal.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineOpenCueCmdExecutor.generated.h"

class UMoviePipelineCustomEncoder;
class UMoviePipelineBase;
class UMoviePipelineOutputSetting;
class UMoviePipelineGameOverrideSetting;

// Render job status enumeration for server communication
UENUM(BlueprintType)
enum class ERenderJobStatus : uint8
{
	queued,
	starting,
	rendering,
	encoding,
	uploading,
	completed,
	failed,
	canceling,
	canceled
};

/**
 * Movie Pipeline Executor for OpenCue CommandLine Mode.
 *
 * This executor is designed to be launched by OpenCue RQD (Render Queue Daemon).
 * It reads render parameters from command line, executes the render, and exits
 * with an appropriate exit code (0=success, non-zero=failure).
 *
 * This is a C++ equivalent of UE's MoviePipelineExampleRuntimeExecutor.py.
 *
 * Command Line Parameters:
 *   -JobId=<uuid>              : Job identifier for progress tracking
 *   -LevelSequence=<path>      : Level sequence asset path to render
 *   -MovieQuality=<0-3>        : Quality level (0=LOW/24fps, 1=MEDIUM/30fps, 2=HIGH/60fps, 3=EPIC/120fps)
 *   -MovieFormat=<mp4|mov>     : Output video format
 *   -ShotName=<name>           : Optional shot name to render (disables other shots)
 *   -CustomStartFrame=<int>    : Optional playback range start frame (continuous only)
 *   -CustomEndFrame=<int>      : Optional playback range end frame (continuous only)
 *   -CmdInitialDelayFrames=<N> : Optional frames to wait before pipeline init (scene load/streaming)
 *   -MRQServerBaseUrl=<url>    : Optional HTTP server for progress notifications
 *
 * Usage:
 *   UnrealEditor-Cmd.exe <project> <map> -game
 *     -MoviePipelineLocalExecutorClass=/Script/OpenCueForUnrealCmdline.MoviePipelineOpenCueCmdExecutor
 *     -JobId=<uuid> -LevelSequence=/Game/Path/To/Sequence
 *     -MovieQuality=2 -MovieFormat=mp4
 *     -MRQServerBaseUrl=http://server:port/
 *     -RenderOffscreen -Unattended -NOSPLASH
 *
 * For scene warm-up, use UMoviePipelineAntiAliasingSetting (EngineWarmUpCount, RenderWarmUpCount).
 */
UCLASS()
class OPENCUEFORUNREALCMDLINE_API UMoviePipelineOpenCueCmdExecutor : public UMoviePipelineExecutorBase
{
	GENERATED_BODY()

public:
	UMoviePipelineOpenCueCmdExecutor();

	virtual void Execute_Implementation(UMoviePipelineQueue* InPipelineQueue) override;

	virtual bool IsRendering_Implementation() const override;

	virtual void OnBeginFrame_Implementation() override;

	virtual void OnExecutorFinishedImpl() override;

private:
	void InitFromCommandLineParams();
	bool TryApplyShotFilter();

	FString GetStatusString(ERenderJobStatus Status) const;

	void CallbackOnMoviePipelineWorkFinished(FMoviePipelineOutputData MoviePipelineOutputData);

	void SendHttpOnMoviePipelineWorkFinished(const FMoviePipelineOutputData& MoviePipelineOutputData);

	/** Request engine exit with the appropriate exit code */
	void RequestEngineExit(bool bSuccess);

private:
	UPROPERTY()
	UMoviePipeline* ActiveMoviePipeline = nullptr;

	UPROPERTY()
	UMoviePipelineQueue* PipelineQueue = nullptr;

	UPROPERTY()
	UMoviePipelineExecutorJob* CurrentJob = nullptr;

	UPROPERTY()
	UMoviePipelineOutputSetting* OutputSetting = nullptr;

	UPROPERTY()
	UMoviePipelineCustomEncoder* CommandLineEncoder = nullptr;

	UPROPERTY()
	UMoviePipelineGameOverrideSetting* GameOverrideSetting = nullptr;


	// Command line parameters
	FString CurrentJobId;
	FString LevelSequencePath;
	FString MovieFormat;
	FString MRQServerBaseUrl = TEXT("http://127.0.0.1:8080/");
	FString TargetShotName;

	// {"LOW": 0, "MEDIUM": 1, "HIGH": 2, "EPIC": 3}
	int32 MovieQuality = 1;
	FFrameRate RenderFrameRate = FFrameRate(30, 1);

	// Optional custom playback range
	bool bUseCustomPlaybackRange = false;
	int32 CustomStartFrame = 0;
	int32 CustomEndFrame = 0;

	// Initial delay before pipeline initialization (scene load/streaming settle time)
	int32 CmdInitialDelayFrameCount = 0;
	int32 RemainingInitializationFrames = -1;

	// Init/validation
	bool bInitParamsValid = true;
	FString InitParamsError;

	// State
	bool bRendering = false;
	bool bRenderSuccess = false;

	// Progress reporting
	EMovieRenderPipelineState LastPipelineState = EMovieRenderPipelineState::Finished;
	bool bExportFinalUpdateSent = false;
	double LastProgressReportTime = 0.0;
	float LastReportedProgress = -1.f;
	const float ProgressReportInterval = 1.0f;
	const float ProgressReportStep = 0.01f;

	// Shot filtering (applied after pipeline init when ShotInfo becomes available)
	bool bShotFilterApplied = false;
	bool bShotFilterFailed = false;
	double LastShotFilterLogTime = 0.0;
};
