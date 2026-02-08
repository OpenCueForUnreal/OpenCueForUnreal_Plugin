#include "MoviePipelineOpenCueCmdExecutor.h"

#include "JsonObjectWrapper.h"
#include "MoviePipeline.h"
#include "MoviePipelineBlueprintLibrary.h"
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
#include "UObject/UnrealType.h"
#include "Misc/ConfigCacheIni.h"

#define LOCTEXT_NAMESPACE "MoviePipelineOpenCueCmdExecutor"

namespace
{
	bool SetBoolProperty(UObject* Obj, const FName PropertyName, const bool bValue)
	{
		if (!Obj)
		{
			return false;
		}

		if (FBoolProperty* Prop = FindFProperty<FBoolProperty>(Obj->GetClass(), PropertyName))
		{
			Prop->SetPropertyValue_InContainer(Obj, bValue);
			return true;
		}

		return false;
	}

	bool SetIntProperty(UObject* Obj, const FName PropertyName, const int32 Value)
	{
		if (!Obj)
		{
			return false;
		}

		if (FIntProperty* Prop = FindFProperty<FIntProperty>(Obj->GetClass(), PropertyName))
		{
			Prop->SetPropertyValue_InContainer(Obj, Value);
			return true;
		}

		return false;
	}

	bool GetStringProperty(const UObject* Obj, const FName PropertyName, FString& OutValue)
	{
		if (!Obj)
		{
			return false;
		}

		if (FStrProperty* Prop = FindFProperty<FStrProperty>(Obj->GetClass(), PropertyName))
		{
			OutValue = Prop->GetPropertyValue_InContainer(Obj);
			return true;
		}

		return false;
	}

	bool SetShotEnabledProperty(UObject* ShotObj, const bool bEnabled)
	{
		// UE Python exposes this as `shot.enabled`, which usually maps to `bEnabled` in C++.
		if (SetBoolProperty(ShotObj, TEXT("bEnabled"), bEnabled))
		{
			return true;
		}
		if (SetBoolProperty(ShotObj, TEXT("Enabled"), bEnabled))
		{
			return true;
		}
		return false;
	}

	bool SetOutputCustomPlaybackRange(UObject* OutputSettingObj, const bool bEnable, const int32 StartFrame, const int32 EndFrame)
	{
		bool bAny = false;

		// UE Python uses: output_settings.use_custom_playback_range / custom_start_frame / custom_end_frame
		bAny |= SetBoolProperty(OutputSettingObj, TEXT("bUseCustomPlaybackRange"), bEnable);
		bAny |= SetBoolProperty(OutputSettingObj, TEXT("bUseCustomFrameRange"), bEnable); // compatibility fallback
		bAny |= SetIntProperty(OutputSettingObj, TEXT("CustomStartFrame"), StartFrame);
		bAny |= SetIntProperty(OutputSettingObj, TEXT("CustomEndFrame"), EndFrame);

		return bAny;
	}

	FString SanitizePathComponent(const FString& InValue)
	{
		FString Out = InValue;
		Out.TrimStartAndEndInline();
		if (Out.IsEmpty())
		{
			return TEXT("unnamed");
		}

		Out = FPaths::MakeValidFileName(Out);
		Out.ReplaceInline(TEXT("."), TEXT("_"));
		return Out;
	}
}

UMoviePipelineOpenCueCmdExecutor::UMoviePipelineOpenCueCmdExecutor()
{
}

void UMoviePipelineOpenCueCmdExecutor::InitFromCommandLineParams()
{
	bInitParamsValid = true;
	InitParamsError.Empty();

	FParse::Value(FCommandLine::Get(), TEXT("-JobId="), CurrentJobId);
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Init JobId: %s"), *CurrentJobId);

	FParse::Value(FCommandLine::Get(), TEXT("-LevelSequence="), LevelSequencePath);
	FParse::Value(FCommandLine::Get(), TEXT("-MovieQuality="), MovieQuality);
	FParse::Value(FCommandLine::Get(), TEXT("-MovieFormat="), MovieFormat);
	FParse::Value(FCommandLine::Get(), TEXT("-ShotName="), TargetShotName);

	const bool bHasCustomStartFrame = FParse::Value(FCommandLine::Get(), TEXT("-CustomStartFrame="), CustomStartFrame);
	const bool bHasCustomEndFrame = FParse::Value(FCommandLine::Get(), TEXT("-CustomEndFrame="), CustomEndFrame);
	bUseCustomPlaybackRange = bHasCustomStartFrame || bHasCustomEndFrame;

	if (bUseCustomPlaybackRange)
	{
		if (!bHasCustomStartFrame || !bHasCustomEndFrame)
		{
			bInitParamsValid = false;
			InitParamsError = TEXT("Custom playback range requires both -CustomStartFrame and -CustomEndFrame.");
		}
		else if (CustomEndFrame < CustomStartFrame)
		{
			bInitParamsValid = false;
			InitParamsError = FString::Printf(TEXT("Invalid custom playback range: %d-%d (end < start)."), CustomStartFrame, CustomEndFrame);
		}
	}

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
		break;
	}

	FParse::Value(FCommandLine::Get(), TEXT("-MRQServerBaseUrl="), MRQServerBaseUrl);

	// Initial delay frames: command-line override > project config > default (0)
	if (!FParse::Value(FCommandLine::Get(), TEXT("-CmdInitialDelayFrames="), CmdInitialDelayFrameCount))
	{
		GConfig->GetInt(
			TEXT("/Script/OpenCueForUnrealEditor.OpenCueDeveloperSettings"),
			TEXT("CmdInitialDelayFrameCount"),
			CmdInitialDelayFrameCount,
			GGameIni);
	}
	CmdInitialDelayFrameCount = FMath::Max(CmdInitialDelayFrameCount, 0);

	const FString RangeString = bUseCustomPlaybackRange ? FString::Printf(TEXT("%d-%d"), CustomStartFrame, CustomEndFrame) : TEXT("<none>");
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] LevelSequence: %s, Quality: %d, Format: %s, ShotName: %s, CustomRange: %s, InitialDelayFrames: %d"),
		*LevelSequencePath, MovieQuality, *MovieFormat, *TargetShotName, *RangeString, CmdInitialDelayFrameCount);

	if (!bInitParamsValid)
	{
		UE_LOG(LogTemp, Error, TEXT("[OpenCueCmdExecutor] Invalid command line params: %s"), *InitParamsError);
	}
}

/**
 * Execute_Implementation - Main entry point for rendering
 *
 * Similar to UE's MoviePipelineExampleRuntimeExecutor.py:
 *   1. Create queue and job from command line parameters
 *   2. Configure output settings
 *   3. Initialize and start the pipeline immediately
 *
 * For scene warm-up delays, configure UMoviePipelineAntiAliasingSetting:
 *   - EngineWarmUpCount: number of frames at the start of each shot that the engine will run without rendering
 *   - RenderWarmUpCount: number of frames at the start of each shot that the engine will render and then discard
 */
void UMoviePipelineOpenCueCmdExecutor::Execute_Implementation(UMoviePipelineQueue* InPipelineQueue)
{
	InitFromCommandLineParams();
	bExportFinalUpdateSent = false;
	bRenderSuccess = false;
	bRendering = true;
	bShotFilterApplied = false;
	bShotFilterFailed = false;
	LastShotFilterLogTime = 0.0;

	// Find game world
	UWorld* World = nullptr;
	if (GEngine)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.World() && Ctx.WorldType == EWorldType::Game)
			{
				World = Ctx.World();
				break;
			}
		}
	}

	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("[OpenCueCmdExecutor] Cannot find game world!"));
		RequestEngineExit(false);
		return;
	}

	if (!bInitParamsValid)
	{
		UE_LOG(LogTemp, Error, TEXT("[OpenCueCmdExecutor] Aborting due to invalid params: %s"), *InitParamsError);
		RequestEngineExit(false);
		return;
	}

	// Create queue and job
	PipelineQueue = NewObject<UMoviePipelineQueue>(World, TEXT("RenderQueue"));
	CurrentJob = PipelineQueue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
	CurrentJob->Sequence = FSoftObjectPath(LevelSequencePath);
	CurrentJob->Map = FSoftObjectPath(World);

	// Configure output settings
	OutputSetting = Cast<UMoviePipelineOutputSetting>(
		CurrentJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass()));
	CommandLineEncoder = Cast<UMoviePipelineCustomEncoder>(
		CurrentJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineCustomEncoder::StaticClass()));
	GameOverrideSetting = Cast<UMoviePipelineGameOverrideSetting>(
		CurrentJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineGameOverrideSetting::StaticClass()));

	// Validate sequence
	ULevelSequence* LevelSequence = Cast<ULevelSequence>(CurrentJob->Sequence.TryLoad());
	if (!LevelSequence)
	{
		UE_LOG(LogTemp, Error, TEXT("[OpenCueCmdExecutor] Failed to load Sequence: %s"), *CurrentJob->Sequence.ToString());
		FText FailureReason = LOCTEXT("InvalidSequenceFailureDialog", "One or more jobs in the queue has an invalid/null sequence. See log for details.");
		OnExecutorErroredImpl(nullptr, true, FailureReason);
		RequestEngineExit(false);
		return;
	}

	// Setup output directory
	FString SequenceName = LevelSequence->GetName();
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Sequence name: %s"), *SequenceName);

	FString RenderOutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MovieRenders"), SequenceName, CurrentJobId);
	if (!TargetShotName.IsEmpty())
	{
		RenderOutputPath = FPaths::Combine(RenderOutputPath, SanitizePathComponent(TargetShotName));
	}
	if (bUseCustomPlaybackRange)
	{
		RenderOutputPath = FPaths::Combine(RenderOutputPath, FString::Printf(TEXT("%d-%d"), CustomStartFrame, CustomEndFrame));
	}

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

	OutputSetting->OutputDirectory.Path = RenderOutputPath;
	OutputSetting->bUseCustomFrameRate = true;
	OutputSetting->OutputFrameRate = RenderFrameRate;
	OutputSetting->FileNameFormat = TEXT("{sequence_name}.{frame_number}");
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Output directory: %s"), *OutputSetting->OutputDirectory.Path);

	if (bUseCustomPlaybackRange)
	{
		const bool bApplied = SetOutputCustomPlaybackRange(OutputSetting, true, CustomStartFrame, CustomEndFrame);
		if (!bApplied)
		{
			UE_LOG(LogTemp, Warning, TEXT("[OpenCueCmdExecutor] Failed to apply custom playback range via reflection. The render may ignore -CustomStartFrame/-CustomEndFrame."));
		}
	}

	CommandLineEncoder->Quality = static_cast<EMoviePipelineEncodeQuality>(MovieQuality);
	CommandLineEncoder->bDeleteSourceFiles = true;

	// Add render passes
	CurrentJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineDeferredPassBase::StaticClass());
	CurrentJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineImageSequenceOutput_PNG::StaticClass());
	CurrentJob->GetConfiguration()->InitializeTransientSettings();

	// Wait for shader compilation before starting render
	if (GShaderCompilingManager)
	{
		while (GShaderCompilingManager->IsCompiling())
		{
			GShaderCompilingManager->ProcessAsyncResults(false, false);
			FPlatformProcess::Sleep(0.5f);
			GLog->Flush();
			UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Waiting for shader compilation..."));
		}
		GShaderCompilingManager->ProcessAsyncResults(false, true);
		GShaderCompilingManager->FinishAllCompilation();
		UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Shader compilation complete."));
	}

	// Setup fixed timestep for deterministic rendering
	FApp::SetUseFixedTimeStep(true);
	FApp::SetFixedDeltaTime(RenderFrameRate.AsInterval());

	// Create the movie pipeline
	ActiveMoviePipeline = NewObject<UMoviePipeline>(World, UMoviePipeline::StaticClass());
	ActiveMoviePipeline->OnMoviePipelineWorkFinished().AddUObject(
		this, &UMoviePipelineOpenCueCmdExecutor::CallbackOnMoviePipelineWorkFinished);

	// Delay initialization to let the scene load, stream textures, and settle.
	// Mirrors UMoviePipelineInProcessExecutor::InitialDelayFrameCount behavior.
	if (CmdInitialDelayFrameCount <= 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] No initial delay, starting render pipeline."));
		ActiveMoviePipeline->Initialize(CurrentJob);
		RemainingInitializationFrames = -1;
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Waiting %d frames before initializing pipeline..."), CmdInitialDelayFrameCount);
		RemainingInitializationFrames = CmdInitialDelayFrameCount;
	}
}

bool UMoviePipelineOpenCueCmdExecutor::IsRendering_Implementation() const
{
	return bRendering;
}

template<typename T>
static FString EnumToString(const T EnumValue)
{
	FString Name = StaticEnum<T>()->GetNameStringByValue(static_cast<__underlying_type(T)>(EnumValue));
	check(Name.Len() != 0);
	return Name;
}

bool UMoviePipelineOpenCueCmdExecutor::TryApplyShotFilter()
{
	if (bShotFilterApplied || bShotFilterFailed)
	{
		return bShotFilterApplied;
	}

	if (TargetShotName.IsEmpty())
	{
		bShotFilterApplied = true;
		return true;
	}

	if (!CurrentJob)
	{
		return false;
	}

	if (CurrentJob->ShotInfo.Num() <= 0)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now - LastShotFilterLogTime > 2.0)
		{
			UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Waiting for ShotInfo to populate (ShotName=%s)..."), *TargetShotName);
			LastShotFilterLogTime = Now;
		}
		return false;
	}

	TArray<int32> OuterMatches;
	TArray<int32> InnerMatches;
	TArray<FString> AvailableShots;

	for (int32 Index = 0; Index < CurrentJob->ShotInfo.Num(); ++Index)
	{
		UMoviePipelineExecutorShot* Shot = CurrentJob->ShotInfo[Index];
		if (!Shot)
		{
			continue;
		}

		FString OuterName;
		FString InnerName;
		GetStringProperty(Shot, TEXT("OuterName"), OuterName);
		GetStringProperty(Shot, TEXT("InnerName"), InnerName);

		const FString Display = (OuterName.IsEmpty() && InnerName.IsEmpty())
			? FString::Printf(TEXT("#%d:%s"), Index, *Shot->GetName())
			: FString::Printf(TEXT("#%d:%s:%s"), Index, *OuterName, *InnerName);
		AvailableShots.Add(Display);

		if (!OuterName.IsEmpty() && OuterName.Equals(TargetShotName, ESearchCase::IgnoreCase))
		{
			OuterMatches.Add(Index);
		}
		if (!InnerName.IsEmpty() && InnerName.Equals(TargetShotName, ESearchCase::IgnoreCase))
		{
			InnerMatches.Add(Index);
		}
	}

	const TArray<int32>& Matches = (OuterMatches.Num() > 0) ? OuterMatches : InnerMatches;
	if (Matches.Num() != 1)
	{
		bShotFilterFailed = true;

		const FString AvailableJoined = FString::Join(AvailableShots, TEXT(", "));
		if (Matches.Num() == 0)
		{
			UE_LOG(LogTemp, Error, TEXT("[OpenCueCmdExecutor] ShotName not found: '%s'. Available shots: %s"), *TargetShotName, *AvailableJoined);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[OpenCueCmdExecutor] ShotName '%s' is ambiguous (%d matches). Available shots: %s"), *TargetShotName, Matches.Num(), *AvailableJoined);
		}

		RequestEngineExit(false);
		return false;
	}

	const int32 SelectedIndex = Matches[0];

	// Ensure we can control shot enablement.
	if (CurrentJob->ShotInfo[SelectedIndex] && !SetShotEnabledProperty(CurrentJob->ShotInfo[SelectedIndex], true))
	{
		bShotFilterFailed = true;
		UE_LOG(LogTemp, Error, TEXT("[OpenCueCmdExecutor] Cannot set shot enablement property on UMoviePipelineExecutorShot. Shot filtering is unsupported in this UE build."));
		RequestEngineExit(false);
		return false;
	}

	for (int32 Index = 0; Index < CurrentJob->ShotInfo.Num(); ++Index)
	{
		UMoviePipelineExecutorShot* Shot = CurrentJob->ShotInfo[Index];
		if (!Shot)
		{
			continue;
		}

		const bool bEnable = Index == SelectedIndex;
		SetShotEnabledProperty(Shot, bEnable);
	}

	bShotFilterApplied = true;
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Shot filter applied. Rendering only ShotName='%s' (index=%d)."), *TargetShotName, SelectedIndex);
	return true;
}

void UMoviePipelineOpenCueCmdExecutor::OnBeginFrame_Implementation()
{
	if (!ActiveMoviePipeline)
	{
		return;
	}

	// Countdown initial delay frames before initializing the pipeline
	if (RemainingInitializationFrames > 0)
	{
		--RemainingInitializationFrames;
		return;
	}
	if (RemainingInitializationFrames == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Initial delay finished, starting render pipeline."));
		ActiveMoviePipeline->Initialize(CurrentJob);
		RemainingInitializationFrames = -1;
		return;
	}

	EMovieRenderPipelineState PipelineState = UMoviePipelineBlueprintLibrary::GetPipelineState(ActiveMoviePipeline);

	if (!bShotFilterApplied && !bShotFilterFailed)
	{
		const bool bAppliedNow = TryApplyShotFilter();

		// If we already started producing frames and still can't apply, fail fast to avoid rendering the wrong shots.
		if (!bAppliedNow && !TargetShotName.IsEmpty() && PipelineState == EMovieRenderPipelineState::ProducingFrames)
		{
			bShotFilterFailed = true;
			UE_LOG(LogTemp, Error, TEXT("[OpenCueCmdExecutor] Shot filter not applied before ProducingFrames. Aborting to avoid rendering unintended shots."));
			RequestEngineExit(false);
			return;
		}

		if (bShotFilterFailed)
		{
			return;
		}
	}

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
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Pipeline state: %s"), *PipelineStateName);

	const auto ComputeEncodingProgress = [this]() -> float
	{
		if (!CurrentJob)
		{
			return -1.f;
		}

		double WeightedProgress = 0.0;
		double TotalFrameCount = 0.0;

		for (UMoviePipelineExecutorShot* Shot : CurrentJob->ShotInfo)
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
		if (!CurrentJob)
		{
			return false;
		}

		const FString EtaPrefix(TEXT("Encoding ETA:"));
		for (UMoviePipelineExecutorShot* Shot : CurrentJob->ShotInfo)
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

			SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);
			break;
		}
		case EMovieRenderPipelineState::ProducingFrames:
		{
			const float CompletionPercentage = UMoviePipelineBlueprintLibrary::GetCompletionPercentage(ActiveMoviePipeline);
			const double CurrentTime = FPlatformTime::Seconds();

			if (LastPipelineState != EMovieRenderPipelineState::ProducingFrames ||
				CurrentTime - LastProgressReportTime >= ProgressReportInterval ||
				CompletionPercentage >= LastReportedProgress + ProgressReportStep)
			{
				UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Render progress: %.1f%%"), CompletionPercentage * 100.f);

				JsonWrapper.JsonObject.Get()->SetStringField(TEXT("status"), GetStatusString(ERenderJobStatus::rendering));
				JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_percent"), CompletionPercentage);

				FTimespan OutEstimate;
				if (UMoviePipelineBlueprintLibrary::GetEstimatedTimeRemaining(ActiveMoviePipeline, OutEstimate))
				{
					int32 EtaSecs = static_cast<int32>(OutEstimate.GetTotalSeconds());
					JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_eta_seconds"), EtaSecs);
				}
				else
				{
					JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_eta_seconds"), -1);
				}

				JsonWrapper.JsonObjectToString(InMessage);
				SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);

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

				SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);

				LastProgressReportTime = FPlatformTime::Seconds();
				LastReportedProgress = 1.f;
			}
			break;
		}

		case EMovieRenderPipelineState::Export:
		{
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

			if (bEncodingComplete && bExportFinalUpdateSent)
			{
				break;
			}

			const bool bForceUpdate = bEncodingComplete && !bExportFinalUpdateSent;

			if (bStateChanged || bForceUpdate || bProgressStepReached || bIntervalElapsed)
			{
				UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Encoding progress: %.1f%%"), EncodingProgress * 100.f);

				int32 ProgressEtaSeconds = -1;
				const bool bHasShotEta = ExtractEncodingEtaSeconds(ProgressEtaSeconds);
				if (!bHasShotEta)
				{
					ProgressEtaSeconds = bEncodingComplete ? 0 : -1;
				}

				JsonWrapper.JsonObject.Get()->SetStringField(TEXT("status"), GetStatusString(ERenderJobStatus::encoding));
				JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_percent"), TotalProgress);
				JsonWrapper.JsonObject.Get()->SetNumberField(TEXT("progress_eta_seconds"), ProgressEtaSeconds);

				JsonWrapper.JsonObjectToString(InMessage);
				SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);

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

FString UMoviePipelineOpenCueCmdExecutor::GetStatusString(ERenderJobStatus Status) const
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
	case ERenderJobStatus::uploading:
		return TEXT("uploading");
	case ERenderJobStatus::completed:
		return TEXT("completed");
	case ERenderJobStatus::failed:
		return TEXT("failed");
	case ERenderJobStatus::canceling:
		return TEXT("canceling");
	case ERenderJobStatus::canceled:
		return TEXT("canceled");
	default:
		return TEXT("failed");
	}
}

void UMoviePipelineOpenCueCmdExecutor::CallbackOnMoviePipelineWorkFinished(FMoviePipelineOutputData MoviePipelineOutputData)
{
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Render finished. Success: %s"),
		MoviePipelineOutputData.bSuccess ? TEXT("true") : TEXT("false"));

	bRenderSuccess = MoviePipelineOutputData.bSuccess;

	SendHttpOnMoviePipelineWorkFinished(MoviePipelineOutputData);

	OnExecutorFinishedImpl();

	// Exit with appropriate code for OpenCue RQD
	RequestEngineExit(bRenderSuccess);
}

void UMoviePipelineOpenCueCmdExecutor::OnExecutorFinishedImpl()
{
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Executor finished."));
	bRendering = false;
	Super::OnExecutorFinishedImpl();
}

void UMoviePipelineOpenCueCmdExecutor::SendHttpOnMoviePipelineWorkFinished(
	const FMoviePipelineOutputData& MoviePipelineOutputData)
{
	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Sending render-complete notification."));
	bool bSuccess = MoviePipelineOutputData.bSuccess;

	const FString InURL = FString::Printf(TEXT("%sue-notifications/job/%s/render-complete"), *MRQServerBaseUrl, *CurrentJobId);
	const FString InVerb = TEXT("POST");
	FString InMessage;
	FJsonObjectWrapper JsonObjectWrapper;
	JsonObjectWrapper.JsonObject.Get()->SetBoolField(TEXT("movie_pipeline_success"), bSuccess);

	FString VideoOutputDir = (FPaths::IsRelative(OutputSetting->OutputDirectory.Path))
		? FPaths::ConvertRelativePathToFull(OutputSetting->OutputDirectory.Path)
		: OutputSetting->OutputDirectory.Path;
	JsonObjectWrapper.JsonObject.Get()->SetStringField(TEXT("video_directory"), VideoOutputDir);
	JsonObjectWrapper.JsonObjectToString(InMessage);

	TMap<FString, FString> InHeaders;
	InHeaders.Add(TEXT("Content-Type"), TEXT("application/json"));
	SendHTTPRequest(InURL, InVerb, InMessage, InHeaders);

	// Block and wait for HTTP to complete before engine exits
	FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::FullFlush);

	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] HTTP notification sent. VideoDir: %s"), *VideoOutputDir);
}

void UMoviePipelineOpenCueCmdExecutor::RequestEngineExit(bool bSuccess)
{
	// Exit code: 0 = success, 1 = failure
	// This is critical for OpenCue RQD to determine task status
	const uint8 ExitCode = bSuccess ? 0 : 1;

	UE_LOG(LogTemp, Log, TEXT("[OpenCueCmdExecutor] Requesting engine exit with code: %d (%s)"),
		ExitCode, bSuccess ? TEXT("SUCCESS") : TEXT("FAILURE"));

	FPlatformMisc::RequestExitWithStatus(true, ExitCode);
}

#undef LOCTEXT_NAMESPACE
