#pragma once

#include "CoreMinimal.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineNativeDeferredExecutor.generated.h"

class UMoviePipelineCustomEncoder;
class URenderGateWorldSubsystem;
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
 * 
 */
UCLASS()
class OPENCUEFORUNREALCMDLINE_API UMoviePipelineNativeDeferredExecutor : public UMoviePipelineExecutorBase
{
	GENERATED_BODY()

public:
	UMoviePipelineNativeDeferredExecutor();

	virtual void Execute_Implementation(UMoviePipelineQueue* InPipelineQueue) override;

	virtual bool IsRendering_Implementation() const override;

	virtual void OnBeginFrame_Implementation() override;

	virtual void OnExecutorFinishedImpl() override;

	UWorld* FindGameWorld() const;

private:
	void InitFromCommandLineParams();

	void CheckGameModeOverrides();

	void RequestForJobInfo(const FString& JobId);

	UFUNCTION()
	void OnReceiveJobInfo(int32 RequestIndex, int32 ResponseCode, const FString& Message);

	void CallbackOnEnginePreExit();

	bool PollReady(float DeltaTime);

	void StartRenderNow();
	
	FString GetStatusString(ERenderJobStatus Status) const;

	void CallbackOnExecutorFinished(UMoviePipelineExecutorBase* PipelineExecutor, bool bSuccess);

	void CallbackOnMoviePipelineWorkFinished(FMoviePipelineOutputData MoviePipelineOutputData);

	void SendHttpOnMoviePipelineWorkFinished(const FMoviePipelineOutputData& MoviePipelineOutputData);

	void WaitShaderCompilingComplete();

private:
	UPROPERTY()
	UMoviePipeline* DeferredMoviePipeline = nullptr;

	UPROPERTY()
	UMoviePipelineQueue* PendingQueue = nullptr;

	UPROPERTY()
	UMoviePipelineExecutorJob* PendingJob = nullptr;

	UPROPERTY()
	UMoviePipelineOutputSetting* MRQ_OutputSetting = nullptr;

	UPROPERTY()
	UMoviePipelineCustomEncoder* MRQ_CommandLineEncoder = nullptr;

	UPROPERTY()
	UMoviePipelineGameOverrideSetting* MRQ_GameOverrideSetting = nullptr;


	// {"LOW": 0, "MEDIUM": 1, "HIGH": 2, "EPIC": 3}
	int32 MovieQuality = 1;

	FFrameRate RenderFrameRate = FFrameRate(30, 1);

	FString MovieFormat;
	FString MRQServerBaseUrl = "http://127.0.0.1:8080/";
	FString CurrentJobId;
	FString LevelSequencePath;

	bool bWaiting = false;
	bool bRendering = false;

	float TimeoutSec = 120.f; // Waiting for scene data synchronization
	float PollIntervalSec = 1.f;

	double StartSeconds = 0.0;
	FTSTicker::FDelegateHandle PollTickerHandle;

	TWeakObjectPtr<URenderGateWorldSubsystem> BoundGate;
	FDelegateHandle OnReadyHandle;
	bool bGateDelegateBound = false;

	FTSTicker::FDelegateHandle ProgressTickerHandle;

	// State tracking for optimized status notifications
	EMovieRenderPipelineState LastPipelineState = EMovieRenderPipelineState::Finished;
	ERenderJobStatus LastReportedStatus = ERenderJobStatus::queued;
	bool bExportFinalUpdateSent = false;

	// Throttling configuration and state
	double LastProgressReportTime = 0.0;
	float LastReportedProgress = -1.f;
	const float ProgressReportInterval = 1.0f; // In seconds
	const float ProgressReportStep = 0.01f;	// 1% step
};
