// Fill out your copyright notice in the Description page of Project Settings.

#include "MoviePipelineOpenCuePIEExecutor.h"

#include "JsonObjectWrapper.h"
#include "MoviePipeline.h"
#include "MoviePipelineBlueprintLibrary.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelineGameOverrideSetting.h"
#include "LevelSequence.h"
#include "Engine/Engine.h"
#include "Kismet/GameplayStatics.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "MoviePipelineCustomEncoder.h"

#define LOCTEXT_NAMESPACE "MoviePipelineOpenCuePIEExecutor"

namespace  
{
	FIntPoint GetRenderResolution(const FString& InResolution)
	{
		FIntPoint OutResolution(1280, 720);

		TArray<FString> Parts;
		InResolution.ParseIntoArray(Parts, TEXT("x"), true);
		if (Parts.Num()>2)
		{
			OutResolution.X = FCString::Atoi(*Parts[0]);
			OutResolution.Y = FCString::Atoi(*Parts[1]);
		}

		return OutResolution;
	}
}

UMoviePipelineOpenCuePIEExecutor::UMoviePipelineOpenCuePIEExecutor()
{
	LastProgressReportTime = FPlatformTime::Seconds();
	LastReportedProgress = -1.0f;

	OnIndividualJobWorkFinished().AddUObject(this, &UMoviePipelineOpenCuePIEExecutor::HandleIndividualJobFinished);
	// Generate a unique worker ID based on machine name and process ID
	WorkerId = FString::Printf(TEXT("%s_%d"), FPlatformProcess::ComputerName(), FPlatformProcess::GetCurrentProcessId());

	// Keep PIE offscreen disabled by default for debugging
	SetIsRenderingOffscreen(false);
}

void UMoviePipelineOpenCuePIEExecutor::InitializeWorker()
{
	// Parse command line parameters
	bWorkerMode = FParse::Param(FCommandLine::Get(), TEXT("MRQWorkerMode"));
	FParse::Value(FCommandLine::Get(), TEXT("-WorkerId="), WorkerId);
	FParse::Value(FCommandLine::Get(), TEXT("-WorkerPoolBaseUrl="), WorkerPoolBaseUrl);
	FParse::Value(FCommandLine::Get(), TEXT("-MRQServerBaseUrl="), MRQServerBaseUrl);

	// Ensure URLs end with /
	if (!WorkerPoolBaseUrl.EndsWith(TEXT("/")))
	{
		WorkerPoolBaseUrl += TEXT("/");
	}
	if (!MRQServerBaseUrl.EndsWith(TEXT("/")))
	{
		MRQServerBaseUrl += TEXT("/");
	}

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Worker initialized - ID: %s, PoolURL: %s, ServerURL: %s"),
		*WorkerId, *WorkerPoolBaseUrl, *MRQServerBaseUrl);
}

void UMoviePipelineOpenCuePIEExecutor::Execute_Implementation(UMoviePipelineQueue* InPipelineQueue)
{
	UE_LOG(LogTemp, Log, TEXT("%s: Execute_Implementation called"), ANSI_TO_TCHAR(__FUNCTION__));

	InitializeWorker();
	HTTPResponseRecievedDelegate.AddUniqueDynamic(this, &UMoviePipelineOpenCuePIEExecutor::OnReceiveJobInfo);
	StartWorkerLoop();
	
}

bool UMoviePipelineOpenCuePIEExecutor::IsRendering_Implementation() const
{
	return bIsRendering || bWorkerRunning;
}

void UMoviePipelineOpenCuePIEExecutor::OnBeginFrame_Implementation()
{
	if (!bIsRendering || !ActiveMoviePipeline)
	{
		return;
	}

	// Get current pipeline state and report progress
	EMovieRenderPipelineState PipelineState = UMoviePipelineBlueprintLibrary::GetPipelineState(Cast<UMoviePipeline>(ActiveMoviePipeline));

	switch (PipelineState)
	{
	case EMovieRenderPipelineState::ProducingFrames:
	{
		const float CompletionPercentage = UMoviePipelineBlueprintLibrary::GetCompletionPercentage(Cast<UMoviePipeline>(ActiveMoviePipeline));
		const double CurrentTime = FPlatformTime::Seconds();

		// Throttle progress reports
		if (CurrentTime - LastProgressReportTime >= ProgressReportIntervalSec ||
			CompletionPercentage >= LastReportedProgress + ProgressReportStep)
		{
			FTimespan OutEstimate;
			int32 EtaSeconds = -1;
			if (UMoviePipelineBlueprintLibrary::GetEstimatedTimeRemaining(Cast<UMoviePipeline>(ActiveMoviePipeline), OutEstimate))
			{
				EtaSeconds = static_cast<int32>(OutEstimate.GetTotalSeconds());
			}

			ReportProgress(CompletionPercentage, EtaSeconds);
			LastProgressReportTime = CurrentTime;
			LastReportedProgress = CompletionPercentage;
		}
		break;
	}

	case EMovieRenderPipelineState::Finalize:
	{
		// Encoding phase - progress is 1.0 to 2.0
		ReportProgress(1.0f, -1);
		break;
	}

	case EMovieRenderPipelineState::Export:
	{
		// Still encoding - could extract more detailed progress here
		// For now, report as encoding in progress
		break;
	}

	default:
		break;
	}
}

void UMoviePipelineOpenCuePIEExecutor::Start(const UMoviePipelineExecutorJob* InJob)
{
	UMoviePipelineExecutorJob* Job = const_cast<UMoviePipelineExecutorJob*>(InJob);

	CurrentJobId = Job ? Job->UserData : FString();
	if (CurrentJobId.IsEmpty() && Job)
	{
		CurrentJobId = Job->JobName;
	}

	if (bWorkerMode)
	{
		if (WorkerId.IsEmpty() || MRQDaemonBaseUrl.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("%s: WorkerMode missing MRQWorkerId or MRQDaemonBaseUrl"), ANSI_TO_TCHAR(__FUNCTION__));
		}
	}

	if (Job)
	{
		// Mark job as "rendering" early to avoid repeated leases on "starting".
		//SendJobProgress(TEXT("rendering"), 0.f, -1);
	}
	
	Super::Start(InJob);
	
}

void UMoviePipelineOpenCuePIEExecutor::HandleIndividualJobFinished(FMoviePipelineOutputData OutputData)
{
	
}

void UMoviePipelineOpenCuePIEExecutor::OnReceiveJobInfo(int32 RequestIndex, int32 ResponseCode, const FString& Message)
{
	
}

void UMoviePipelineOpenCuePIEExecutor::StartWorkerLoop()
{
	if (bWorkerRunning)
	{
		UE_LOG(LogTemp, Warning, TEXT("[OpenCue] Worker loop already running"));
		return;
	}

	bWorkerRunning = true;
	CurrentTaskStatus = EOpenCueWorkerTaskStatus::Idle;

	// Register engine pre-exit callback
	FCoreDelegates::OnEnginePreExit.AddUObject(this, &UMoviePipelineOpenCuePIEExecutor::OnEnginePreExit);

	// Start lease polling ticker
	LeasePollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UMoviePipelineOpenCuePIEExecutor::TickLeasePoll),
		LeasePollIntervalSec
	);

	// Start heartbeat ticker
	HeartbeatTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UMoviePipelineOpenCuePIEExecutor::TickHeartbeat),
		HeartbeatIntervalSec
	);

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Worker loop started, polling for tasks..."));

	// Immediately poll for first lease
	PollForLease();
}

void UMoviePipelineOpenCuePIEExecutor::StopWorker()
{
	if (!bWorkerRunning)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Stopping worker..."));

	bWorkerRunning = false;

	// Remove tickers
	if (LeasePollTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(LeasePollTickerHandle);
		LeasePollTickerHandle.Reset();
	}

	if (HeartbeatTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTickerHandle);
		HeartbeatTickerHandle.Reset();
	}

	// Cleanup any active render
	CleanupRenderTask();

	CurrentTaskStatus = EOpenCueWorkerTaskStatus::Idle;

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Worker stopped"));
}

bool UMoviePipelineOpenCuePIEExecutor::TickLeasePoll(float DeltaTime)
{
	if (!bWorkerRunning)
	{
		return false; // Stop ticker
	}

	// Only poll when idle
	if (CurrentTaskStatus == EOpenCueWorkerTaskStatus::Idle)
	{
		PollForLease();
	}

	return true; // Continue ticker
}

bool UMoviePipelineOpenCuePIEExecutor::TickHeartbeat(float DeltaTime)
{
	if (!bWorkerRunning)
	{
		return false; // Stop ticker
	}

	SendHeartbeat();
	return true; // Continue ticker
}

void UMoviePipelineOpenCuePIEExecutor::PollForLease()
{
	const FString Url = FString::Printf(TEXT("%sworkers/%s/lease"), *WorkerPoolBaseUrl, *WorkerId);

	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Content-Type"), TEXT("application/json"));

	UE_LOG(LogTemp, Verbose, TEXT("[OpenCue] Polling for lease: %s"), *Url);

	// SendHTTPRequest is inherited from UMoviePipelineLinearExecutorBase
	int32 RequestIndex = SendHTTPRequest(Url, TEXT("GET"), TEXT(""), Headers);
	// Note: Response will be handled asynchronously - you may need to bind to HTTP response delegate
}

void UMoviePipelineOpenCuePIEExecutor::SendHeartbeat()
{
	if (!bWorkerRunning)
	{
		return;
	}

	const FString Url = FString::Printf(TEXT("%sworkers/%s/heartbeat"), *WorkerPoolBaseUrl, *WorkerId);

	FJsonObjectWrapper JsonWrapper;
	JsonWrapper.JsonObject->SetStringField(TEXT("status"), GetStatusString(CurrentTaskStatus));
	if (CurrentTask.IsValid())
	{
		JsonWrapper.JsonObject->SetStringField(TEXT("task_id"), CurrentTask.TaskId);
	}

	FString Message;
	JsonWrapper.JsonObjectToString(Message);

	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Content-Type"), TEXT("application/json"));

	UE_LOG(LogTemp, Verbose, TEXT("[OpenCue] Sending heartbeat: %s"), *GetStatusString(CurrentTaskStatus));

	SendHTTPRequest(Url, TEXT("POST"), Message, Headers);
}

void UMoviePipelineOpenCuePIEExecutor::NotifyTaskDone(bool bSuccess)
{
	if (!CurrentTask.IsValid())
	{
		return;
	}

	const FString Url = FString::Printf(TEXT("%sworkers/%s/done"), *WorkerPoolBaseUrl, *WorkerId);

	FJsonObjectWrapper JsonWrapper;
	JsonWrapper.JsonObject->SetStringField(TEXT("task_id"), CurrentTask.TaskId);
	JsonWrapper.JsonObject->SetBoolField(TEXT("success"), bSuccess);

	FString Message;
	JsonWrapper.JsonObjectToString(Message);

	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Content-Type"), TEXT("application/json"));

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Notifying task done: %s, success: %s"),
		*CurrentTask.TaskId, bSuccess ? TEXT("true") : TEXT("false"));

	SendHTTPRequest(Url, TEXT("POST"), Message, Headers);

	// Flush to ensure the request is sent before we continue
	FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::FullFlush);
}

void UMoviePipelineOpenCuePIEExecutor::ReportProgress(float Progress, int32 EtaSeconds)
{
	if (!CurrentTask.IsValid())
	{
		return;
	}

	const FString Url = FString::Printf(TEXT("%sue-notifications/job/%s/progress"),
		*MRQServerBaseUrl, *CurrentTask.JobId);

	FJsonObjectWrapper JsonWrapper;
	JsonWrapper.JsonObject->SetStringField(TEXT("status"), bIsRendering ? TEXT("rendering") : TEXT("encoding"));
	JsonWrapper.JsonObject->SetNumberField(TEXT("progress_percent"), Progress);
	JsonWrapper.JsonObject->SetNumberField(TEXT("progress_eta_seconds"), EtaSeconds);

	FString Message;
	JsonWrapper.JsonObjectToString(Message);

	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Content-Type"), TEXT("application/json"));

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Progress: %.1f%%, ETA: %d sec"), Progress * 100.f, EtaSeconds);

	SendHTTPRequest(Url, TEXT("POST"), Message, Headers);
}

void UMoviePipelineOpenCuePIEExecutor::ReportRenderComplete(bool bSuccess, const FString& VideoDirectory)
{
	if (!CurrentTask.IsValid())
	{
		return;
	}

	const FString Url = FString::Printf(TEXT("%sue-notifications/job/%s/render-complete"),
		*MRQServerBaseUrl, *CurrentTask.JobId);

	FJsonObjectWrapper JsonWrapper;
	JsonWrapper.JsonObject->SetBoolField(TEXT("movie_pipeline_success"), bSuccess);
	JsonWrapper.JsonObject->SetStringField(TEXT("video_directory"), VideoDirectory);

	FString Message;
	JsonWrapper.JsonObjectToString(Message);

	TMap<FString, FString> Headers;
	Headers.Add(TEXT("Content-Type"), TEXT("application/json"));

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Render complete: success=%s, dir=%s"),
		bSuccess ? TEXT("true") : TEXT("false"), *VideoDirectory);

	SendHTTPRequest(Url, TEXT("POST"), Message, Headers);

	// Flush to ensure the request is sent
	FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::FullFlush);
}

bool UMoviePipelineOpenCuePIEExecutor::ParseTaskInfo(const FString& JsonString, FOpenCueTaskInfo& OutTaskInfo)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[OpenCue] Failed to parse task JSON: %s"), *JsonString);
		return false;
	}

	OutTaskInfo.TaskId = JsonObject->GetStringField(TEXT("task_id"));
	OutTaskInfo.JobId = JsonObject->GetStringField(TEXT("job_id"));
	OutTaskInfo.LevelSequencePath = JsonObject->GetStringField(TEXT("level_sequence"));
	OutTaskInfo.MapPath = JsonObject->GetStringField(TEXT("map"));
	OutTaskInfo.MovieQuality = JsonObject->GetIntegerField(TEXT("movie_quality"));
	OutTaskInfo.MovieFormat = JsonObject->GetStringField(TEXT("movie_format"));

	// Parse extra params if present
	const TSharedPtr<FJsonObject>* ExtraParamsObject;
	if (JsonObject->TryGetObjectField(TEXT("extra_params"), ExtraParamsObject))
	{
		for (const auto& Pair : (*ExtraParamsObject)->Values)
		{
			FString Value;
			if (Pair.Value->TryGetString(Value))
			{
				OutTaskInfo.ExtraParams.Add(Pair.Key, Value);
			}
		}
	}

	return OutTaskInfo.IsValid();
}

bool UMoviePipelineOpenCuePIEExecutor::SetupRenderJob(const FOpenCueTaskInfo& TaskInfo)
{
	UWorld* World = FindGameWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("[OpenCue] Cannot find game world"));
		return false;
	}

	// Determine frame rate from quality
	switch (TaskInfo.MovieQuality)
	{
	case 0: RenderFrameRate = FFrameRate(24, 1); break;
	case 1: RenderFrameRate = FFrameRate(30, 1); break;
	case 2: RenderFrameRate = FFrameRate(60, 1); break;
	case 3: RenderFrameRate = FFrameRate(120, 1); break;
	default: RenderFrameRate = FFrameRate(30, 1); break;
	}

	// Create queue and job
	RenderQueue = NewObject<UMoviePipelineQueue>(World, TEXT("OpenCueRenderQueue"));
	RenderJob = RenderQueue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());

	RenderJob->Sequence = FSoftObjectPath(TaskInfo.LevelSequencePath);
	if (!TaskInfo.MapPath.IsEmpty())
	{
		RenderJob->Map = FSoftObjectPath(TaskInfo.MapPath);
	}
	else
	{
		RenderJob->Map = FSoftObjectPath(World);
	}

	// Load and validate sequence
	ULevelSequence* LevelSequence = Cast<ULevelSequence>(RenderJob->Sequence.TryLoad());
	if (!LevelSequence)
	{
		UE_LOG(LogTemp, Error, TEXT("[OpenCue] Failed to load sequence: %s"), *TaskInfo.LevelSequencePath);
		return false;
	}

	// Setup output settings
	OutputSetting = Cast<UMoviePipelineOutputSetting>(
		RenderJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass()));

	FString SequenceName = LevelSequence->GetName();
	FString RenderOutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MovieRenders"), SequenceName, TaskInfo.JobId);

	if (!FPaths::DirectoryExists(RenderOutputPath))
	{
		FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*RenderOutputPath);
	}

	RenderOutputPath = FPaths::ConvertRelativePathToFull(RenderOutputPath);
	FPaths::NormalizeFilename(RenderOutputPath);

	OutputSetting->OutputDirectory.Path = RenderOutputPath;
	OutputSetting->bUseCustomFrameRate = true;
	OutputSetting->OutputFrameRate = RenderFrameRate;
	OutputSetting->FileNameFormat = TEXT("{sequence_name}.{frame_number}");

	// Setup game override settings
	GameOverrideSetting = Cast<UMoviePipelineGameOverrideSetting>(
		RenderJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineGameOverrideSetting::StaticClass()));

	RenderJob->GetConfiguration()->InitializeTransientSettings();

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Render job setup complete: %s -> %s"), *SequenceName, *RenderOutputPath);

	return true;
}

void UMoviePipelineOpenCuePIEExecutor::StartRenderTask()
{
	if (!CurrentTask.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[OpenCue] Cannot start render - no valid task"));
		return;
	}

	if (!SetupRenderJob(CurrentTask))
	{
		CurrentTaskStatus = EOpenCueWorkerTaskStatus::Failed;
		NotifyTaskDone(false);
		CleanupRenderTask();
		return;
	}

	CurrentTaskStatus = EOpenCueWorkerTaskStatus::Running;
	bIsRendering = true;
	LastProgressReportTime = 0.0;
	LastReportedProgress = -1.0f;

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Starting render task: %s"), *CurrentTask.TaskId);

	// Report initial progress
	ReportProgress(0.0f, -1);

	// Use the parent class's rendering mechanism
	// PIEExecutor will handle the actual pipeline execution
	if (RenderQueue && RenderQueue->GetJobs().Num() > 0)
	{
		// Bind to pipeline finish event
		if (ActiveMoviePipeline)
		{
			ActiveMoviePipeline->OnMoviePipelineWorkFinished().AddUObject(
				this, &UMoviePipelineOpenCuePIEExecutor::OnMoviePipelineFinished);
		}

		// Start the render through parent class mechanism
		Super::Execute_Implementation(RenderQueue);
	}
}

void UMoviePipelineOpenCuePIEExecutor::CleanupRenderTask()
{
	bIsRendering = false;

	// Clear task info
	CurrentTask = FOpenCueTaskInfo();

	// Clear render objects
	RenderQueue = nullptr;
	RenderJob = nullptr;
	OutputSetting = nullptr;
	CustomEncoder = nullptr;
	GameOverrideSetting = nullptr;

	// Reset progress tracking
	LastProgressReportTime = 0.0;
	LastReportedProgress = -1.0f;
}

void UMoviePipelineOpenCuePIEExecutor::OnLeaseResponse(int32 RequestIndex, int32 ResponseCode, const FString& Message)
{
	if (ResponseCode == 200)
	{
		// Task assigned
		FOpenCueTaskInfo TaskInfo;
		if (ParseTaskInfo(Message, TaskInfo))
		{
			UE_LOG(LogTemp, Log, TEXT("[OpenCue] Lease acquired - Task: %s, Job: %s"),
				*TaskInfo.TaskId, *TaskInfo.JobId);

			CurrentTask = TaskInfo;
			CurrentTaskStatus = EOpenCueWorkerTaskStatus::Assigned;

			// Start the render task
			StartRenderTask();
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[OpenCue] Failed to parse lease response"));
		}
	}
	else if (ResponseCode == 204)
	{
		// No task available - keep polling
		UE_LOG(LogTemp, Verbose, TEXT("[OpenCue] No tasks available"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[OpenCue] Lease request failed: %d - %s"), ResponseCode, *Message);
	}
}

void UMoviePipelineOpenCuePIEExecutor::OnHeartbeatResponse(int32 RequestIndex, int32 ResponseCode, const FString& Message)
{
	if (ResponseCode != 200)
	{
		UE_LOG(LogTemp, Warning, TEXT("[OpenCue] Heartbeat failed: %d - %s"), ResponseCode, *Message);
	}
}

void UMoviePipelineOpenCuePIEExecutor::OnTaskDoneResponse(int32 RequestIndex, int32 ResponseCode, const FString& Message)
{
	if (ResponseCode == 200)
	{
		UE_LOG(LogTemp, Log, TEXT("[OpenCue] Task done acknowledged"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[OpenCue] Task done notification failed: %d - %s"), ResponseCode, *Message);
	}

	// Return to idle state regardless
	CurrentTaskStatus = EOpenCueWorkerTaskStatus::Idle;
	CleanupRenderTask();
}

void UMoviePipelineOpenCuePIEExecutor::OnMoviePipelineFinished(FMoviePipelineOutputData OutputData)
{
	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Movie pipeline finished: %s"),
		OutputData.bSuccess ? TEXT("Success") : TEXT("Failed"));

	bIsRendering = false;

	// Get output directory
	FString VideoDirectory;
	if (OutputSetting)
	{
		VideoDirectory = OutputSetting->OutputDirectory.Path;
		if (FPaths::IsRelative(VideoDirectory))
		{
			VideoDirectory = FPaths::ConvertRelativePathToFull(VideoDirectory);
		}
	}

	// Report completion to MRQ server
	ReportRenderComplete(OutputData.bSuccess, VideoDirectory);

	// Notify worker pool
	NotifyTaskDone(OutputData.bSuccess);

	CurrentTaskStatus = OutputData.bSuccess ?
		EOpenCueWorkerTaskStatus::Completed :
		EOpenCueWorkerTaskStatus::Failed;

	// Cleanup and return to idle
	CleanupRenderTask();
	CurrentTaskStatus = EOpenCueWorkerTaskStatus::Idle;
}

void UMoviePipelineOpenCuePIEExecutor::OnEnginePreExit()
{
	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Engine pre-exit - stopping worker"));
	StopWorker();
}

UWorld* UMoviePipelineOpenCuePIEExecutor::FindGameWorld() const
{
	if (!GEngine)
	{
		return nullptr;
	}

	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		if (Ctx.World() && (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE))
		{
			return Ctx.World();
		}
	}

	return nullptr;
}

FString UMoviePipelineOpenCuePIEExecutor::GetStatusString(EOpenCueWorkerTaskStatus Status) const
{
	switch (Status)
	{
	case EOpenCueWorkerTaskStatus::Idle:
		return TEXT("idle");
	case EOpenCueWorkerTaskStatus::Assigned:
		return TEXT("assigned");
	case EOpenCueWorkerTaskStatus::Running:
		return TEXT("running");
	case EOpenCueWorkerTaskStatus::Completed:
		return TEXT("completed");
	case EOpenCueWorkerTaskStatus::Failed:
		return TEXT("failed");
	default:
		return TEXT("unknown");
	}
}

#undef LOCTEXT_NAMESPACE
