// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "MoviePipelinePIEExecutor.h"
#include "MoviePipelineOpenCuePIEExecutor.generated.h"

class UMoviePipelineQueue;
class UMoviePipelineExecutorJob;
class UMoviePipelineOutputSetting;
class UMoviePipelineGameOverrideSetting;
class UMoviePipelineCustomEncoder;
class ULevelSequence;

// Worker task status
UENUM(BlueprintType)
enum class EOpenCueWorkerTaskStatus : uint8
{
	Idle,           // Waiting for task lease
	Assigned,       // Task assigned, preparing
	Running,        // Rendering in progress
	Completed,      // Task completed successfully
	Failed          // Task failed
};

// Task info received from Worker Pool
USTRUCT(BlueprintType)
struct FOpenCueTaskInfo
{
	GENERATED_BODY()

	UPROPERTY()
	FString TaskId;

	UPROPERTY()
	FString JobId;

	UPROPERTY()
	FString LevelSequencePath;

	UPROPERTY()
	FString MapPath;

	UPROPERTY()
	int32 MovieQuality = 1;

	UPROPERTY()
	FString MovieFormat;

	UPROPERTY()
	TMap<FString, FString> ExtraParams;

	bool IsValid() const { return !TaskId.IsEmpty() && !LevelSequencePath.IsEmpty(); }
};

/**
 * OpenCue PIE Executor - Persistent worker mode executor
 *
 * This executor runs in Play-In-Editor mode and continuously polls for render tasks
 * from the UE Worker Pool service. It follows the lease-based task assignment pattern:
 *
 * 1. Poll GET /workers/{id}/lease for new tasks
 * 2. Send periodic heartbeats POST /workers/{id}/heartbeat
 * 3. Execute rendering when task is assigned
 * 4. Report progress POST /ue-notifications/job/{id}/progress
 * 5. Complete task POST /workers/{id}/done
 * 6. Return to step 1
 */
UCLASS(BlueprintType)
class OPENCUEFORUNREALEDITOR_API UMoviePipelineOpenCuePIEExecutor : public UMoviePipelinePIEExecutor
{
	GENERATED_BODY()

public:
	UMoviePipelineOpenCuePIEExecutor();
	
	//~ Begin UMoviePipelineExecutorBase Interface
	virtual void Execute_Implementation(UMoviePipelineQueue* InPipelineQueue) override;
	virtual bool IsRendering_Implementation() const override;
	virtual void OnBeginFrame_Implementation() override;
	//~ End UMoviePipelineExecutorBase Interface
	
protected:
	virtual void Start(const UMoviePipelineExecutorJob* InJob) override;

	void HandleIndividualJobFinished(FMoviePipelineOutputData OutputData);

	UFUNCTION()
	void OnReceiveJobInfo(int32 RequestIndex, int32 ResponseCode, const FString& Message);
public:

	/** Initialize worker from command line parameters */
	UFUNCTION(BlueprintCallable, Category = "OpenCue")
	void InitializeWorker();

	/** Start the worker polling loop */
	UFUNCTION(BlueprintCallable, Category = "OpenCue")
	void StartWorkerLoop();

	/** Stop the worker and cleanup */
	UFUNCTION(BlueprintCallable, Category = "OpenCue")
	void StopWorker();

	/** Get current worker status */
	UFUNCTION(BlueprintPure, Category = "OpenCue")
	EOpenCueWorkerTaskStatus GetWorkerStatus() const { return CurrentTaskStatus; }

	/** Get current task info */
	UFUNCTION(BlueprintPure, Category = "OpenCue")
	const FOpenCueTaskInfo& GetCurrentTask() const { return CurrentTask; }

protected:
	/** Poll for new task lease from Worker Pool */
	void PollForLease();

	/** Send heartbeat to Worker Pool */
	void SendHeartbeat();

	/** Notify Worker Pool that task is done */
	void NotifyTaskDone(bool bSuccess);

	/** Report render progress to server */
	void ReportProgress(float Progress, int32 EtaSeconds = -1);

	/** Report render completion to server */
	void ReportRenderComplete(bool bSuccess, const FString& VideoDirectory);

	/** Parse task info from JSON response */
	bool ParseTaskInfo(const FString& JsonString, FOpenCueTaskInfo& OutTaskInfo);

	/** Setup render job from task info */
	bool SetupRenderJob(const FOpenCueTaskInfo& TaskInfo);

	/** Start rendering the current task */
	void StartRenderTask();

	/** Cleanup after render task */
	void CleanupRenderTask();

	/** HTTP response callbacks */
	UFUNCTION()
	void OnLeaseResponse(int32 RequestIndex, int32 ResponseCode, const FString& Message);

	UFUNCTION()
	void OnHeartbeatResponse(int32 RequestIndex, int32 ResponseCode, const FString& Message);

	UFUNCTION()
	void OnTaskDoneResponse(int32 RequestIndex, int32 ResponseCode, const FString& Message);

	/** Ticker callbacks */
	bool TickLeasePoll(float DeltaTime);
	bool TickHeartbeat(float DeltaTime);

	/** Movie pipeline callbacks */
	void OnMoviePipelineFinished(FMoviePipelineOutputData OutputData);
	void OnEnginePreExit();

	/** Find the game world */
	UWorld* FindGameWorld() const;

	/** Get status string for API */
	FString GetStatusString(EOpenCueWorkerTaskStatus Status) const;

protected:
	// Worker configuration
	UPROPERTY()
	FString WorkerId;

	UPROPERTY()
	FString WorkerPoolBaseUrl = TEXT("http://127.0.0.1:9100/");

	UPROPERTY()
	FString MRQServerBaseUrl = TEXT("http://127.0.0.1:8080/");

	// Polling intervals
	UPROPERTY()
	float LeasePollIntervalSec = 2.0f;

	UPROPERTY()
	float HeartbeatIntervalSec = 10.0f;

	// Current state
	UPROPERTY()
	EOpenCueWorkerTaskStatus CurrentTaskStatus = EOpenCueWorkerTaskStatus::Idle;

	UPROPERTY()
	FOpenCueTaskInfo CurrentTask;

	UPROPERTY()
	bool bWorkerRunning = false;

	UPROPERTY()
	bool bIsRendering = false;

	// Render job objects
	UPROPERTY()
	UMoviePipelineQueue* RenderQueue = nullptr;

	UPROPERTY()
	UMoviePipelineExecutorJob* RenderJob = nullptr;

	UPROPERTY()
	UMoviePipelineOutputSetting* OutputSetting = nullptr;

	UPROPERTY()
	UMoviePipelineCustomEncoder* CustomEncoder = nullptr;

	UPROPERTY()
	UMoviePipelineGameOverrideSetting* GameOverrideSetting = nullptr;

	// Ticker handles
	FTSTicker::FDelegateHandle LeasePollTickerHandle;
	FTSTicker::FDelegateHandle HeartbeatTickerHandle;

	// Progress tracking
	double LastProgressReportTime = 0.0;
	float LastReportedProgress = -1.0f;
	const float ProgressReportIntervalSec = 1.0f;
	const float ProgressReportStep = 0.01f; // 1%

	// Frame rate based on quality
	FFrameRate RenderFrameRate = FFrameRate(30, 1);
	bool bRenderingFinished;
	bool bWorkerMode;
	FString CurrentJobId;
	FString MRQDaemonBaseUrl;
};
