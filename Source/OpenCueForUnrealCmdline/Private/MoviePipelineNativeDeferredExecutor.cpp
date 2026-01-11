#include "MoviePipelineNativeDeferredExecutor.h"

#include "JsonObjectWrapper.h"
#include "MoviePipeline.h"
#include "MoviePipelineBlueprintLibrary.h"
#include "RenderGateWorldSubsystem.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelineCustomEncoder.h"
#include "LevelSequence.h"
#include "MoviePipelineDeferredPasses.h"
#include "MoviePipelineImageSequenceOutput.h"
#include "MoviePipelineGameOverrideSetting.h"
#include "ShaderCompiler.h"
#include "HAL/IConsoleManager.h"
#include "Misc/DefaultValueHelper.h"
#include "HttpModule.h"
#include "HttpManager.h"

#define LOCTEXT_NAMESPACE "MoviePipelineExecutorExt"

UMoviePipelineNativeDeferredExecutor::UMoviePipelineNativeDeferredExecutor()
{
}

void UMoviePipelineNativeDeferredExecutor::InitFromCommandLineParams()
{
    FParse::Value(FCommandLine::Get(), TEXT("-JobId="), CurrentJobId);
    UE_LOG(LogTemp, Log, TEXT("%s Init CurrentJobId: %s"), ANSI_TO_TCHAR(__FUNCTION__), *CurrentJobId);

    FParse::Value(FCommandLine::Get(), TEXT("-LevelSequence="), LevelSequencePath);
	FParse::Value(FCommandLine::Get(), TEXT("-MovieQuality="), MovieQuality);
	FParse::Value(FCommandLine::Get(), TEXT("-MovieFormat="), MovieFormat);

    switch (MovieQuality)
    {

    case 0:
        RenderFrameRate = FFrameRate(24, 1);
        break;

    case 1:
        RenderFrameRate = FFrameRate(30, 1);
        break;

    case 2:
        RenderFrameRate = FFrameRate(60, 1);
        break;

    case 3:
        RenderFrameRate = FFrameRate(120, 1);
        break;

    default:
        break;;

    }

	FParse::Value(FCommandLine::Get(), TEXT("-MRQServerBaseUrl="), MRQServerBaseUrl);
}

void UMoviePipelineNativeDeferredExecutor::CheckGameModeOverrides()
{
    UWorld* World = FindGameWorld();
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("%s: Cannot find game world!"), ANSI_TO_TCHAR(__FUNCTION__));
    }

    AGameModeBase* AuthGameMode = World->GetAuthGameMode();
    if (AuthGameMode->GetClass() == AGameModeBase::StaticClass())
    {
        UE_LOG(LogTemp, Log, TEXT("%s: The current map does not specify a custom GameMode, we manually call the SetCinematicMode function."), ANSI_TO_TCHAR(__FUNCTION__));

        APlayerController* PlayerController = UGameplayStatics::GetPlayerController(World, 0);
        if (PlayerController)
        {
            const bool bCinematicMode = true;
            const bool bHidePlayer = true;
            const bool bHideHUD = true;
            const bool bPreventMovement = true;
            const bool bPreventTurning = true;
            PlayerController->SetCinematicMode(bCinematicMode, bHidePlayer, bHideHUD, bPreventMovement, bPreventTurning);
        }
    }
    else if (AuthGameMode->GetClass()->IsChildOf(AGameModeBase::StaticClass()))
    {
        UE_LOG(LogTemp, Log, TEXT("%s: The current map specify a custom GameMode: %s."), ANSI_TO_TCHAR(__FUNCTION__), *AuthGameMode->GetClass()->GetName());
    }
}

void UMoviePipelineNativeDeferredExecutor::Execute_Implementation(UMoviePipelineQueue* InPipelineQueue)
{

	InitFromCommandLineParams();
	bExportFinalUpdateSent = false;

    CheckGameModeOverrides();

	UWorld* World = FindGameWorld();

	bWaiting = true;
    bRendering = false;
    StartSeconds = FPlatformTime::Seconds();

    
    PendingQueue = NewObject<UMoviePipelineQueue>(FindGameWorld(), TEXT("PendingQueue"));
	PendingJob = PendingQueue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
    PendingJob->Sequence = FSoftObjectPath(LevelSequencePath);
    PendingJob->Map = FSoftObjectPath(World);

	MRQ_OutputSetting = Cast<UMoviePipelineOutputSetting>(PendingJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass()));
    MRQ_CommandLineEncoder = Cast<UMoviePipelineCustomEncoder>(PendingJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineCustomEncoder::StaticClass()));
    MRQ_GameOverrideSetting = Cast<UMoviePipelineGameOverrideSetting>(PendingJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineGameOverrideSetting::StaticClass()));

    ULevelSequence* LevelSequence = Cast<ULevelSequence>(PendingJob->Sequence.TryLoad());
    if (!LevelSequence)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to load Sequence specified by job: %s"), *PendingJob->Sequence.ToString());
        FText FailurReason = LOCTEXT("InvalidSequenceFailureDialog", "One or more jobs in the queue has an invalid/null sequence. See log for details.");
        OnExecutorErroredImpl(nullptr, true, FailurReason);
        return;
    }

    FString SequenceName = LevelSequence->GetName();
    if (!SequenceName.IsEmpty())
    {
        UE_LOG(LogTemp, Log, TEXT("Input sequence name: %s"), *SequenceName);
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("Input sequence name is empty."));
    }

    FString RenderOutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MovieRenders"), SequenceName, CurrentJobId);
    if (!FPaths::DirectoryExists(RenderOutputPath))
    {
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*RenderOutputPath);
    }

    if (FPaths::IsRelative(RenderOutputPath))
    {
        RenderOutputPath = FPaths::ConvertRelativePathToFull(RenderOutputPath);
    }
    FPaths::NormalizeFilename(RenderOutputPath);
    FPaths::CollapseRelativeDirectories(RenderOutputPath);

    MRQ_OutputSetting->OutputDirectory.Path = RenderOutputPath;
    MRQ_OutputSetting->bUseCustomFrameRate = true;
    MRQ_OutputSetting->OutputFrameRate = RenderFrameRate;
    MRQ_OutputSetting->FileNameFormat = TEXT("{sequence_name}.{frame_number}");

    MRQ_CommandLineEncoder->Quality = static_cast<EMoviePipelineEncodeQuality>(MovieQuality);
    MRQ_CommandLineEncoder->bDeleteSourceFiles = true;

    PendingJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineDeferredPassBase::StaticClass());
    PendingJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_PNG::StaticClass());
    PendingJob->GetConfiguration()->InitializeTransientSettings();

    DeferredMoviePipeline = NewObject<UMoviePipeline>(World, UMoviePipeline::StaticClass());
    DeferredMoviePipeline->OnMoviePipelineWorkFinished().AddUObject(this, &UMoviePipelineNativeDeferredExecutor::CallbackOnMoviePipelineWorkFinished);
    FCoreDelegates::OnEnginePreExit.AddUObject(this, &UMoviePipelineNativeDeferredExecutor::CallbackOnEnginePreExit);

    FApp::SetUseFixedTimeStep(true);
    FApp::SetFixedDeltaTime(RenderFrameRate.AsInterval());


    WaitShaderCompilingComplete();

    PollTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UMoviePipelineNativeDeferredExecutor::PollReady),
        PollIntervalSec
    );

}

bool UMoviePipelineNativeDeferredExecutor::IsRendering_Implementation() const
{
    return bRendering || bWaiting;
}

template<typename T>
static FString EnumToString(const T EnumValue)
{
	FString Name = StaticEnum<T>()->GetNameStringByValue(static_cast<__underlying_type(T)>(EnumValue));
	check(Name.Len() != 0);
	return Name;
}

void UMoviePipelineNativeDeferredExecutor::OnBeginFrame_Implementation()
{
	EMovieRenderPipelineState PipelineState = UMoviePipelineBlueprintLibrary::GetPipelineState(DeferredMoviePipeline);

	// For states that only fire once, check if the state has changed.
	// ProducingFrames is handled separately as it needs to update continuously (with throttling).
	if (PipelineState == LastPipelineState && PipelineState != EMovieRenderPipelineState::ProducingFrames && PipelineState != EMovieRenderPipelineState::Export)
	{
		return;
	}

	if (PipelineState != EMovieRenderPipelineState::Export)
	{
		bExportFinalUpdateSent = false;
	}
	
	FString PipelineStateName = EnumToString<EMovieRenderPipelineState>(PipelineState);
	UE_LOG(LogTemp, Warning, TEXT("%s MoviePipelineState changed to: %s"), ANSI_TO_TCHAR(__FUNCTION__), *PipelineStateName);

	const auto ComputeEncodingProgress = [this]() -> float
	{
		if (!PendingJob)
		{
			return -1.f;
		}

		double WeightedProgress = 0.0;
		double TotalFrameCount = 0.0;

		for (UMoviePipelineExecutorShot* Shot : PendingJob->ShotInfo)
		{
			if (!Shot || !Shot->ShouldRender())
			{
				continue;
			}

			const int32 ShotFrameCount = Shot->ShotInfo.WorkMetrics.TotalOutputFrameCount;
			if (ShotFrameCount <= 0)
			{
				continue;
			}

			const float ShotProgress = FMath::Clamp(Shot->GetStatusProgress(), 0.f, 1.f);
			WeightedProgress += static_cast<double>(ShotFrameCount) * ShotProgress;
			TotalFrameCount += static_cast<double>(ShotFrameCount);
		}

		if (TotalFrameCount <= 0.0)
		{
			return -1.f;
		}

		const float NormalizedProgress = static_cast<float>(WeightedProgress / TotalFrameCount);
		return FMath::Clamp(NormalizedProgress, 0.f, 1.f);
		
	};

	const auto ExtractEncodingEtaSeconds = [this](int32& OutEtaSeconds) -> bool
	{
		if (!PendingJob)
		{
			return false;
		}

		const FString EtaPrefix(TEXT("Encoding ETA:"));
		for (UMoviePipelineExecutorShot* Shot : PendingJob->ShotInfo)
		{
			if (!Shot || !Shot->ShouldRender())
			{
				continue;
			}

			const FString StatusMessage = Shot->GetStatusMessage();
			if (!StatusMessage.StartsWith(EtaPrefix))
			{
				continue;
			}

			const FString EtaString = StatusMessage.Mid(EtaPrefix.Len()).TrimStartAndEnd();
			int32 ParsedEtaSeconds = 0;
			if (FDefaultValueHelper::ParseInt(EtaString, ParsedEtaSeconds))
			{
				OutEtaSeconds = FMath::Max(ParsedEtaSeconds, 0);
				return true;
			}
		}

		return false;
	};
	
	const FString InURL = FString::Printf(TEXT("%sue-notifications/job/%s/progress"), *MRQServerBaseUrl, *CurrentJobId);
	const FString InVerb = TEXT("POST");
	FString InMessage;
	TMap<FString, FString> InHeaders;
	InHeaders.Add(TEXT("Content-Type"), TEXT("application/json"));
	
	FJsonObjectWrapper JsonWrapper;
	
	switch (PipelineState)
	{
		case EMovieRenderPipelineState::Uninitialized:
		{
			JsonWrapper.JsonObject.Get()->SetStringField(TEXT("status"), GetStatusString(ERenderJobStatus::starting));
			JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_percent"), 0.f);
			JsonWrapper.JsonObjectToString(InMessage);
				
			int32 RequestIndex = SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);
			break;
		}
		case EMovieRenderPipelineState::ProducingFrames:
		{
			const float CompletionPercentage = UMoviePipelineBlueprintLibrary::GetCompletionPercentage(DeferredMoviePipeline);
			const double CurrentTime = FPlatformTime::Seconds();

			if (LastPipelineState != EMovieRenderPipelineState::ProducingFrames ||
				CurrentTime - LastProgressReportTime >= ProgressReportInterval ||
				CompletionPercentage >= LastReportedProgress + ProgressReportStep)
			{
				UE_LOG(LogTemp, Log, TEXT("CompletionPercentage value: %.1f%%"), CompletionPercentage * 100.f);

				
				JsonWrapper.JsonObject.Get()->SetStringField(TEXT("status"), GetStatusString(ERenderJobStatus::rendering));
				JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_percent"), CompletionPercentage);

				FTimespan OutEstimate;
				if (UMoviePipelineBlueprintLibrary::GetEstimatedTimeRemaining(DeferredMoviePipeline, OutEstimate))
				{
					UE_LOG(LogTemp, Log, TEXT("%s: Estimated time remaining: %.1f"), ANSI_TO_TCHAR(__FUNCTION__), OutEstimate.GetTotalSeconds());
					int32 EtaSecs = static_cast<int32>(OutEstimate.GetTotalSeconds());
					JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_eta_seconds"), EtaSecs);
				}
				else
				{
					JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_eta_seconds"), -1);
				}

				JsonWrapper.JsonObjectToString(InMessage);

				int32 RequestIndex = SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);

				LastProgressReportTime = CurrentTime;
				LastReportedProgress = CompletionPercentage;
			}
				
			break;
		}

		case EMovieRenderPipelineState::Finalize:
		{
			if (LastPipelineState != EMovieRenderPipelineState::Finalize)
			{
				JsonWrapper.JsonObject.Get()->SetStringField(TEXT("status"), GetStatusString(ERenderJobStatus::encoding));
				JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_percent"), 1.f);
				JsonWrapper.JsonObjectToString(InMessage);
			
				int32 RequestIndex = SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);

				LastProgressReportTime = FPlatformTime::Seconds();
				LastReportedProgress = 1.f;
			}
				
			break;	
		}
		
		case EMovieRenderPipelineState::Export:
		{
			// Note: FFMPEG log level should be set to "info" in DefaultEngine.ini                                                                                                                                                            
			// under [/Script/MovieRenderPipelineCore.MoviePipelineCommandLineEncoderSettings]                                                                                                                                                
			// by setting -loglevel info in the CommandLineFormat paramete 	
			const float EncodingProgress = ComputeEncodingProgress();
			if (EncodingProgress < 0.f)
			{
				break;
			}

			const float TotalProgress = 1.f + EncodingProgress;
			const double CurrentTime = FPlatformTime::Seconds();
			const bool bStateChanged = LastPipelineState != EMovieRenderPipelineState::Export;
			const bool bIntervalElapsed = (CurrentTime - LastProgressReportTime) >= ProgressReportInterval;
			const bool bProgressStepReached = TotalProgress >= LastReportedProgress + ProgressReportStep;
			const bool bEncodingComplete = EncodingProgress >= 1.f - KINDA_SMALL_NUMBER;

			// If the final completion status has already been sent, it will not be sent again to avoid the accumulation of HTTP requests.
			if (bEncodingComplete && bExportFinalUpdateSent)
			{
				break;
			}

			const bool bForceUpdate = bEncodingComplete && !bExportFinalUpdateSent;

			if (bStateChanged || bForceUpdate || bProgressStepReached || bIntervalElapsed)
			{
				UE_LOG(LogTemp, Log, TEXT("%s: Encoding progress: %.1f%%"), ANSI_TO_TCHAR(__FUNCTION__), EncodingProgress * 100.f);
				
				int32 ProgressEtaSeconds = -1;
				const bool bHasShotEta = ExtractEncodingEtaSeconds(ProgressEtaSeconds);
				if (!bHasShotEta)
				{
					ProgressEtaSeconds = bEncodingComplete ? 0 : -1;
				}
				
				JsonWrapper.JsonObject.Get()->SetStringField(TEXT("status"), GetStatusString(ERenderJobStatus::encoding));
				JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_percent"), TotalProgress); // rendering progress is 1.f already.
				JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_eta_seconds"), ProgressEtaSeconds);

				JsonWrapper.JsonObjectToString(InMessage);

				int32 RequestIndex = SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);

				LastProgressReportTime = CurrentTime;
				LastReportedProgress = TotalProgress;

				if (bEncodingComplete)
				{
					bExportFinalUpdateSent = true;
				}
			}
			break;	
		}
		case EMovieRenderPipelineState::Finished:
		{
			break;	
		}
		default:
			break;
	}

	LastPipelineState = PipelineState;
}

void UMoviePipelineNativeDeferredExecutor::RequestForJobInfo(const FString& JobId)
{
}

void UMoviePipelineNativeDeferredExecutor::OnReceiveJobInfo(int32 RequestIndex, int32 ResponseCode, const FString& Message)
{
}

void UMoviePipelineNativeDeferredExecutor::CallbackOnEnginePreExit()
{
	if (DeferredMoviePipeline && DeferredMoviePipeline->GetPipelineState() != EMovieRenderPipelineState::Finished)
	{
		UE_LOG(LogTemp, Log, TEXT("%s Application quit while Movie Pipeline was still active. Stalling to do full shutdown."), ANSI_TO_TCHAR(__FUNCTION__));
		DeferredMoviePipeline->RequestShutdown();
		DeferredMoviePipeline->Shutdown();

		UE_LOG(LogTemp, Log, TEXT("%s Stalling finished, pipeline has shut down."), ANSI_TO_TCHAR(__FUNCTION__));
	}
}

bool UMoviePipelineNativeDeferredExecutor::PollReady(float DeltaTime)
{
    if (!bWaiting || !PendingQueue)
        return false; // Stop Ticker

    const double Elapsed = FPlatformTime::Seconds() - StartSeconds;
    UE_LOG(LogTemp, Warning, TEXT("[MRQ] %s Poll for rendering, Elapsed: %.0f s; TimeoutSec: %.0f s"), ANSI_TO_TCHAR(__FUNCTION__), Elapsed, TimeoutSec);
    bool bCanStart = false;

    // 1) Find world（-game）
    UWorld* World = FindGameWorld();
    if (World)
    {
        UE_LOG(LogTemp, Log, TEXT("[MRQ] GameWorld: %s"), *World->GetName());

        // 2) Check the level: BeginPlay
        if (!World->HasBegunPlay())
        {
        	UE_LOG(LogTemp, Log, TEXT("[MRQ] Game world: %s has not begun play yet."), *World->GetName());
			return true; // Keep Poll
        }

    	// 3) Waiting for data sync, level generation.
    	if (URenderGateWorldSubsystem* Gate = World->GetSubsystem<URenderGateWorldSubsystem>())
    	{
    		if (Gate->IsReady())
    		{
    			bCanStart = true;
    		}
    		else
    		{
    			if (Gate != BoundGate.Get())
    			{
    				if (BoundGate.IsValid() && OnReadyHandle.IsValid())
    				{
    					BoundGate->OnReadyEvent().Remove(OnReadyHandle);
    					OnReadyHandle.Reset();
    				}
    				BoundGate = Gate;
    				bGateDelegateBound = false;
    			}

    			if (!bGateDelegateBound)
    			{
    				OnReadyHandle = Gate->OnReadyEvent()
						.AddUObject(this, &UMoviePipelineNativeDeferredExecutor::StartRenderNow);
    				bGateDelegateBound = true;
    			}
    		}
    	}
    }
	
    // —— Unified timeout processing exit ——
    if (bCanStart || Elapsed >= TimeoutSec)
    {
        if (!bCanStart)
        {
            UE_LOG(LogTemp, Warning, TEXT("[MRQ] Wait timed out: %.1fs/%.1fs, start anyway."), Elapsed, TimeoutSec);
        }

        StartRenderNow();
        return false; // Stop Ticker
    }

    UE_LOG(LogTemp, Log, TEXT("[MRQ] Check if the Level content is ready, waiting... %.0f s; Exceeding the threshold %.0f s will automatically enter the video rendering process."), Elapsed, TimeoutSec);
    return true; // Continue polling
}

void UMoviePipelineNativeDeferredExecutor::StartRenderNow()
{
    if (!bWaiting || bRendering || !PendingQueue)
        return;

    bWaiting = false;
    bRendering = true;

    DeferredMoviePipeline->Initialize(PendingJob);

    // Progress updates are now handled in OnBeginFrame with throttling.
}

FString UMoviePipelineNativeDeferredExecutor::GetStatusString(ERenderJobStatus Status) const
{
    switch (Status)
    {
    case ERenderJobStatus::queued:
        return TEXT("queued");
    case ERenderJobStatus::starting:
        return TEXT("starting");
    case ERenderJobStatus::rendering:
        return TEXT("rendering");
    case ERenderJobStatus::encoding:
    	return TEXT("encoding");
    case ERenderJobStatus::completed:
        return TEXT("completed");
    case ERenderJobStatus::failed:
        return TEXT("failed");
    case ERenderJobStatus::canceled:
        return TEXT("canceled");

    default:
        return TEXT("failed");
    }
}

void UMoviePipelineNativeDeferredExecutor::CallbackOnExecutorFinished(UMoviePipelineExecutorBase* PipelineExecutor,
    bool bSuccess)
{
}

void UMoviePipelineNativeDeferredExecutor::CallbackOnMoviePipelineWorkFinished(FMoviePipelineOutputData MoviePipelineOutputData)
{
	SendHttpOnMoviePipelineWorkFinished(MoviePipelineOutputData);
	
	if (PollTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollTickerHandle);
		PollTickerHandle.Reset();
	}

	if (ProgressTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ProgressTickerHandle);
		ProgressTickerHandle.Reset();
	}

    OnExecutorFinishedImpl();
}

void UMoviePipelineNativeDeferredExecutor::OnExecutorFinishedImpl()
{
    UE_LOG(LogTemp, Log, TEXT("%s"), ANSI_TO_TCHAR(__FUNCTION__));
    bRendering = false;
    Super::OnExecutorFinishedImpl();
}

void UMoviePipelineNativeDeferredExecutor::SendHttpOnMoviePipelineWorkFinished(
    const FMoviePipelineOutputData& MoviePipelineOutputData)
{
	UE_LOG(LogTemp, Log, TEXT("%s"), ANSI_TO_TCHAR(__FUNCTION__));
	bool bSuccess = MoviePipelineOutputData.bSuccess;

	const FString InURL = FString::Printf(TEXT("%sue-notifications/job/%s/render-complete"), *MRQServerBaseUrl, *CurrentJobId);
	const FString InVerb = TEXT("POST");
	FString InMessage;
	FJsonObjectWrapper JsonObjectWrapper;
	JsonObjectWrapper.JsonObject.Get()->SetBoolField(TEXT("movie_pipeline_success"), bSuccess);

    FString VideoOutputDir = (FPaths::IsRelative(MRQ_OutputSetting->OutputDirectory.Path)) ? FPaths::ConvertRelativePathToFull(MRQ_OutputSetting->OutputDirectory.Path) : MRQ_OutputSetting->OutputDirectory.Path;
	JsonObjectWrapper.JsonObject.Get()->SetStringField(TEXT("video_directory"), VideoOutputDir);
	JsonObjectWrapper.JsonObjectToString(InMessage);

	TMap<FString, FString> InHeaders;
	InHeaders.Add(TEXT("Content-Type"), TEXT("application/json"));
	int32 RequestIndex = SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);

	// After the request is made, block and wait for the HTTP manager to complete the tasks in the queue,
	// ensuring /render-complete
	// actually send it out before the engine exits to avoid staying in the encoding state.
	FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::FullFlush);
}

void UMoviePipelineNativeDeferredExecutor::WaitShaderCompilingComplete()
{
    if (GShaderCompilingManager)
    {
	    while (GShaderCompilingManager->IsCompiling())
	    {
            GShaderCompilingManager->ProcessAsyncResults(false, false);
            FPlatformProcess::Sleep(0.5f);
            GLog->Flush();

            UE_LOG(LogTemp, Warning, TEXT("%s: Wait shader compile complete..."), ANSI_TO_TCHAR(__FUNCTION__));
	    }

        GShaderCompilingManager->ProcessAsyncResults(false, true);
        GShaderCompilingManager->FinishAllCompilation();
        UE_LOG(LogTemp, Warning, TEXT("%s: GShaderCompilingManager->FinishAllCompilation"), ANSI_TO_TCHAR(__FUNCTION__));
    }
}

UWorld* UMoviePipelineNativeDeferredExecutor::FindGameWorld() const
{
    if (!GEngine) return nullptr;

	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
	{
		if (Ctx.World() && Ctx.WorldType == EWorldType::Game)
		{
			return Ctx.World();
		}
	}

    return nullptr;
}


#undef LOCTEXT_NAMESPACE // "MoviePipelineExecutorExt"
