// Copyright OpenCue for Unreal contributors. MIT License.

#include "MoviePipelineOpenCueExecutorJob.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneCinematicShotSection.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "HAL/PlatformProcess.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "MoviePipelineGameOverrideSetting.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MoviePipelineOpenCueExecutorJob"

namespace
{
	bool ExtractSectionFrameRange(const UMovieScene* MovieScene, const TRange<FFrameNumber>& SectionRange, int32& OutStart, int32& OutEnd)
	{
		if (!MovieScene)
		{
			return false;
		}

		if (!SectionRange.HasLowerBound() || !SectionRange.HasUpperBound())
		{
			return false;
		}

		FFrameNumber StartFrame = SectionRange.GetLowerBoundValue();
		FFrameNumber EndFrame = SectionRange.GetUpperBoundValue();

		if (SectionRange.GetUpperBound().IsExclusive())
		{
			EndFrame.Value -= 1;
		}

		if (EndFrame < StartFrame)
		{
			EndFrame = StartFrame;
		}

		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();

		OutStart = FFrameRate::TransformTime(StartFrame, TickResolution, DisplayRate).FloorToFrame().Value;
		OutEnd = FFrameRate::TransformTime(EndFrame, TickResolution, DisplayRate).FloorToFrame().Value;

		return true;
	}

	FString GetShotDisplayName(const UMovieSceneCinematicShotSection* ShotSection)
	{
		if (!ShotSection)
		{
			return FString();
		}

		if (const FTextProperty* TextProp = FindFProperty<FTextProperty>(ShotSection->GetClass(), TEXT("ShotDisplayName")))
		{
			return TextProp->GetPropertyValue_InContainer(ShotSection).ToString();
		}

		if (const FStrProperty* StrProp = FindFProperty<FStrProperty>(ShotSection->GetClass(), TEXT("ShotDisplayName")))
		{
			return StrProp->GetPropertyValue_InContainer(ShotSection);
		}

		if (const FNameProperty* NameProp = FindFProperty<FNameProperty>(ShotSection->GetClass(), TEXT("ShotDisplayName")))
		{
			return NameProp->GetPropertyValue_InContainer(ShotSection).ToString();
		}

		return ShotSection->GetName();
	}

	FString SoftClassToPath(const TSoftClassPtr<AGameModeBase>& SoftClass)
	{
		if (SoftClass.IsNull())
		{
			return FString();
		}

		if (UClass* LoadedClass = SoftClass.Get())
		{
			return LoadedClass->GetPathName();
		}

		const FSoftObjectPath SoftPath = SoftClass.ToSoftObjectPath();
		return SoftPath.IsNull() ? FString() : SoftPath.ToString();
	}
}

// ============================================================================
// FOpenCueRenderTask
// ============================================================================

TSharedPtr<FJsonObject> FOpenCueRenderTask::ToJsonObject() const
{
	TSharedPtr<FJsonObject> TaskObj = MakeShared<FJsonObject>();

	TaskObj->SetNumberField(TEXT("task_index"), TaskIndex);

	// Shot object
	TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
	ShotObj->SetStringField(TEXT("name"), ShotName);
	if (!OuterName.IsEmpty())
	{
		ShotObj->SetStringField(TEXT("outer_name"), OuterName);
	}
	if (!InnerName.IsEmpty())
	{
		ShotObj->SetStringField(TEXT("inner_name"), InnerName);
	}
	TaskObj->SetObjectField(TEXT("shot"), ShotObj);

	// Frame range (optional)
	if (FrameStart >= 0 && FrameEnd >= 0)
	{
		TSharedPtr<FJsonObject> RangeObj = MakeShared<FJsonObject>();
		RangeObj->SetNumberField(TEXT("start"), FrameStart);
		RangeObj->SetNumberField(TEXT("end"), FrameEnd);
		TaskObj->SetObjectField(TEXT("frame_range"), RangeObj);
	}

	if (bDisableShotFilter)
	{
		TSharedPtr<FJsonObject> ExtensionsObj = MakeShared<FJsonObject>();
		ExtensionsObj->SetBoolField(TEXT("disable_shot_filter"), true);
		TaskObj->SetObjectField(TEXT("extensions"), ExtensionsObj);
	}

	return TaskObj;
}

// ============================================================================
// UMoviePipelineOpenCueExecutorJob
// ============================================================================

UMoviePipelineOpenCueExecutorJob::UMoviePipelineOpenCueExecutorJob()
{
	// Initialize with defaults from developer settings
	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();
	if (Settings)
	{
		OpenCueConfig.Quality = Settings->DefaultQuality;
		OpenCueConfig.OutputFormat = Settings->DefaultFormat;
	}
}

void UMoviePipelineOpenCueExecutorJob::GenerateJobNameFromSequence()
{
	// Prefer UE MRQ job row naming first (what users see in the queue list).
	FString CandidateName = JobName.TrimStartAndEnd();

	// Fallback to sequence asset name when MRQ row name is empty.
	if (Sequence.IsValid())
	{
		if (CandidateName.IsEmpty())
		{
			ULevelSequence* LevelSequence = Cast<ULevelSequence>(Sequence.TryLoad());
			if (LevelSequence)
			{
				CandidateName = LevelSequence->GetName();
			}
		}
	}

	// Last-resort fallback.
	if (CandidateName.IsEmpty())
	{
		CandidateName = TEXT("Render");
	}

	OpenCueConfig.JobName = CandidateName;
}

void UMoviePipelineOpenCueExecutorJob::ResolveCmdGameModeClass(FString& OutGameModeClass, FString& OutSource) const
{
	OutGameModeClass.Empty();
	OutSource = TEXT("None");

	const FString JobOverrideClass = SoftClassToPath(OpenCueConfig.CmdGameModeOverrideClass).TrimStartAndEnd();
	if (!JobOverrideClass.IsEmpty())
	{
		OutGameModeClass = JobOverrideClass;
		OutSource = TEXT("JobOverride");
		return;
	}

	const UMoviePipelinePrimaryConfig* JobConfig = GetConfiguration();
	if (JobConfig)
	{
		const UMoviePipelineGameOverrideSetting* MRQGameOverride = JobConfig->FindSetting<UMoviePipelineGameOverrideSetting>(true);
		if (MRQGameOverride && MRQGameOverride->GameModeOverride)
		{
			OutGameModeClass = MRQGameOverride->GameModeOverride->GetPathName();
			OutSource = TEXT("MRQGameOverrideSetting");
			return;
		}
	}

	UWorld* MapWorld = nullptr;
	if (Map.IsValid())
	{
		UObject* MapObject = Map.TryLoad();
		MapWorld = Cast<UWorld>(MapObject);

		if (!MapWorld && MapObject)
		{
			UPackage* MapPackage = MapObject->GetPackage();
			if (MapPackage)
			{
				MapWorld = UWorld::FindWorldInPackage(MapPackage);
			}
		}
	}

	if (MapWorld)
	{
		const AWorldSettings* WorldSettings = MapWorld->GetWorldSettings();
		if (WorldSettings)
		{
			UClass* MapGameModeClass = WorldSettings->DefaultGameMode.Get();
			if (MapGameModeClass)
			{
				OutGameModeClass = MapGameModeClass->GetPathName();
				OutSource = TEXT("MapOverride");
				return;
			}
		}
	}

	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();
	if (Settings)
	{
		const FString FallbackClass = SoftClassToPath(Settings->CmdGameModeClass).TrimStartAndEnd();
		if (!FallbackClass.IsEmpty())
		{
			OutGameModeClass = FallbackClass;
			OutSource = TEXT("SettingsFallback");
			return;
		}
	}
}

bool UMoviePipelineOpenCueExecutorJob::CanSubmitToOpenCue(FString& OutReason) const
{
	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();

	// Check sequence is set
	if (!Sequence.IsValid())
	{
		OutReason = TEXT("No Level Sequence selected");
		return false;
	}

	// Check map is set
	if (!Map.IsValid())
	{
		OutReason = TEXT("No Map selected");
		return false;
	}

	// Check job name is set
	if (OpenCueConfig.JobName.IsEmpty())
	{
		OutReason = TEXT("OpenCue job name is empty");
		return false;
	}

	// Check Cuebot host is configured
	FString CuebotHost = OpenCueConfig.GetEffectiveCuebotHost();
	if (CuebotHost.IsEmpty())
	{
		OutReason = TEXT("Cuebot host not configured");
		return false;
	}

	// Check Submitter CLI path
	if (Settings)
	{
		FString SubmitterPath = Settings->GetEffectiveSubmitterCLIPath();
		if (SubmitterPath.IsEmpty())
		{
			OutReason = TEXT("Submitter CLI path not configured. Set it in Project Settings > Plugins > OpenCue Settings.");
			return false;
		}

		FString ResolvedSubmitterPath = SubmitterPath;
		if (FPaths::IsRelative(ResolvedSubmitterPath))
		{
			ResolvedSubmitterPath = FPaths::ConvertRelativePathToFull(ResolvedSubmitterPath);
		}
		FPaths::NormalizeFilename(ResolvedSubmitterPath);

		if (!FPaths::FileExists(ResolvedSubmitterPath) && !FPaths::DirectoryExists(ResolvedSubmitterPath))
		{
			OutReason = FString::Printf(
				TEXT("Submitter path does not exist: %s"),
				*ResolvedSubmitterPath
			);
			return false;
		}
	}

	OutReason.Empty();
	return true;
}

bool UMoviePipelineOpenCueExecutorJob::SubmitToOpenCue(FString& OutErrorMessage)
{
	FOpenCueSubmitResult Result = SubmitToOpenCueWithResult();
	OutErrorMessage = Result.ErrorMessage;
	if (!Result.ErrorHint.IsEmpty())
	{
		OutErrorMessage += TEXT(" ") + Result.ErrorHint;
	}
	return Result.bSuccess;
}

FOpenCueSubmitResult UMoviePipelineOpenCueExecutorJob::SubmitToOpenCueWithResult()
{
	FOpenCueSubmitResult Result;

	// Default OpenCue job name from UE naming when left empty.
	if (OpenCueConfig.JobName.IsEmpty())
	{
		GenerateJobNameFromSequence();
	}

	// Validate
	FString ValidationError;
	if (!CanSubmitToOpenCue(ValidationError))
	{
		Result.bSuccess = false;
		Result.ErrorMessage = ValidationError;
		return Result;
	}

	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();
	if (!Settings)
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Failed to get OpenCue settings");
		return Result;
	}

	// Generate Job ID
	FString JobId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Result.JobId = JobId;

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Starting submission for OpenCue job: %s (UE MRQ Job: %s, ID: %s)"), *OpenCueConfig.JobName, *JobName, *JobId);

	// Step 1: Extract shots from sequence
	TArray<FOpenCueRenderTask> Tasks = ExtractShotsFromSequence();
	if (Tasks.Num() == 0)
	{
		// No shots found, create a single task for the whole sequence
		FOpenCueRenderTask WholeSequenceTask;
		WholeSequenceTask.TaskIndex = 0;
		WholeSequenceTask.ShotName = TEXT("WholeSequence");
		WholeSequenceTask.bDisableShotFilter = true;
		Tasks.Add(WholeSequenceTask);
		UE_LOG(LogTemp, Log, TEXT("[OpenCue] No shots specified, will render entire sequence as one task"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[OpenCue] Found %d shots"), Tasks.Num());
	}

	// Step 2: Expand frame ranges (V1: mostly pass-through)
	Tasks = ExpandTasksForFrameRanges(Tasks);
	UE_LOG(LogTemp, Log, TEXT("[OpenCue] After expansion: %d tasks"), Tasks.Num());

	// Step 3: Generate render_plan.json
	FString RenderPlanJson = GenerateRenderPlanJson(JobId, Tasks);

	// Step 4: Publish render plan
	FString PlanUri;
	FString PublishError;
	if (!PublishRenderPlan(JobId, RenderPlanJson, PlanUri, PublishError))
	{
		Result.bSuccess = false;
		Result.ErrorMessage = FString::Printf(TEXT("Failed to publish render plan: %s"), *PublishError);
		return Result;
	}
	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Render plan (worker input) published to: %s"), *PlanUri);

	// Step 5: Generate submit_spec.json
	FString SubmitSpecJson = GenerateSubmitSpecJson(JobId, PlanUri, Tasks.Num());

	// Write submit_spec.json to temp file
	FString SubmitSpecPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("OpenCueSubmitSpecs"), JobId + TEXT("_submit_spec.json"));
	FString SubmitSpecDir = FPaths::GetPath(SubmitSpecPath);
	if (!FPaths::DirectoryExists(SubmitSpecDir))
	{
		IFileManager::Get().MakeDirectory(*SubmitSpecDir, true);
	}
	if (!FFileHelper::SaveStringToFile(SubmitSpecJson, *SubmitSpecPath))
	{
		Result.bSuccess = false;
		Result.ErrorMessage = FString::Printf(TEXT("Failed to write submit_spec.json to %s"), *SubmitSpecPath);
		return Result;
	}
	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Submit spec (--spec for submitter CLI) written to: %s"), *SubmitSpecPath);

	// Step 6: Call Submitter CLI
	if (!CallSubmitterCLI(SubmitSpecPath, Result))
	{
		// Error already set in Result
		return Result;
	}

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Submission complete. Success: %s"), Result.bSuccess ? TEXT("true") : TEXT("false"));
	return Result;
}

TArray<FOpenCueRenderTask> UMoviePipelineOpenCueExecutorJob::ExtractShotsFromSequence() const
{
	TArray<FOpenCueRenderTask> Tasks;

	ULevelSequence* LevelSequence = Cast<ULevelSequence>(Sequence.TryLoad());
	if (!LevelSequence)
	{
		return Tasks;
	}

	UMovieScene* MovieScene = LevelSequence->GetMovieScene();
	if (!MovieScene)
	{
		return Tasks;
	}

	// Prefer Cinematic Shot Track if present (matches MRQ shot naming)
	if (UMovieSceneCinematicShotTrack* ShotTrack = Cast<UMovieSceneCinematicShotTrack>(
		MovieScene->FindTrack(UMovieSceneCinematicShotTrack::StaticClass())))
	{
		int32 TaskIndex = 0;
		for (UMovieSceneSection* Section : ShotTrack->GetAllSections())
		{
			if (!Section || !Section->IsActive())
			{
				continue;
			}

			UMovieSceneCinematicShotSection* ShotSection = Cast<UMovieSceneCinematicShotSection>(Section);
			if (!ShotSection)
			{
				continue;
			}

			FOpenCueRenderTask Task;
			Task.TaskIndex = TaskIndex++;

			Task.OuterName = GetShotDisplayName(ShotSection);
			if (UMovieSceneSequence* ShotSequence = ShotSection->GetSequence())
			{
				Task.InnerName = ShotSequence->GetName();
			}

			if (!Task.OuterName.IsEmpty())
			{
				Task.ShotName = Task.OuterName;
			}
			else if (!Task.InnerName.IsEmpty())
			{
				Task.ShotName = Task.InnerName;
			}
			else
			{
				Task.ShotName = ShotSection->GetName();
			}

			int32 StartFrame = -1;
			int32 EndFrame = -1;
			if (ExtractSectionFrameRange(MovieScene, ShotSection->GetRange(), StartFrame, EndFrame))
			{
				Task.FrameStart = StartFrame;
				Task.FrameEnd = EndFrame;
			}

			Tasks.Add(Task);
		}

		if (Tasks.Num() > 0)
		{
			return Tasks;
		}
	}

	// Fallback: Camera cut track (disable shot-name filter, rely on frame ranges)
	UMovieSceneCameraCutTrack* CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(
		MovieScene->FindTrack(UMovieSceneCameraCutTrack::StaticClass()));

	if (!CameraCutTrack)
	{
		// No camera cut track - this is a single-shot sequence
		return Tasks;
	}

	int32 TaskIndex = 0;
	for (UMovieSceneSection* Section : CameraCutTrack->GetAllSections())
	{
		if (!Section || !Section->IsActive())
		{
			continue;
		}

		UMovieSceneCameraCutSection* CameraCutSection = Cast<UMovieSceneCameraCutSection>(Section);
		if (!CameraCutSection)
		{
			continue;
		}

		FOpenCueRenderTask Task;
		Task.TaskIndex = TaskIndex++;
		Task.ShotName = FString::Printf(TEXT("Cut_%d"), Task.TaskIndex);
		Task.bDisableShotFilter = true;

		int32 StartFrame = -1;
		int32 EndFrame = -1;
		if (ExtractSectionFrameRange(MovieScene, CameraCutSection->GetRange(), StartFrame, EndFrame))
		{
			Task.FrameStart = StartFrame;
			Task.FrameEnd = EndFrame;
		}

		Tasks.Add(Task);
	}

	return Tasks;
}

TArray<FOpenCueRenderTask> UMoviePipelineOpenCueExecutorJob::ExpandTasksForFrameRanges(const TArray<FOpenCueRenderTask>& InTasks) const
{
	// V1: No expansion needed. Each shot becomes one task.
	// In future, if we support discontinuous ranges, we would expand them here.

	TArray<FOpenCueRenderTask> OutTasks;

	int32 TaskIndex = 0;
	for (const FOpenCueRenderTask& Task : InTasks)
	{
		FOpenCueRenderTask NewTask = Task;
		NewTask.TaskIndex = TaskIndex++;
		OutTasks.Add(NewTask);
	}

	return OutTasks;
}

FString UMoviePipelineOpenCueExecutorJob::GenerateRenderPlanJson(const FString& JobId, const TArray<FOpenCueRenderTask>& Tasks) const
{
	TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();

	RootObj->SetStringField(TEXT("plan_version"), TEXT("1.0"));
	RootObj->SetStringField(TEXT("job_name"), OpenCueConfig.JobName);
	RootObj->SetStringField(TEXT("job_id"), JobId);

	// Project info
	TSharedPtr<FJsonObject> ProjectObj = MakeShared<FJsonObject>();
	FString UProjectHint = FPaths::GetCleanFilename(FPaths::GetProjectFilePath());
	if (UProjectHint.IsEmpty())
	{
		UProjectHint = TEXT("Project.uproject");
	}
	ProjectObj->SetStringField(TEXT("uproject_hint"), UProjectHint);
	RootObj->SetObjectField(TEXT("project"), ProjectObj);

	// Asset paths
	RootObj->SetStringField(TEXT("map_asset_path"), Map.IsValid() ? Map.GetAssetPathString() : TEXT(""));
	RootObj->SetStringField(TEXT("level_sequence_asset_path"), Sequence.IsValid() ? Sequence.GetAssetPathString() : TEXT(""));
	RootObj->SetStringField(TEXT("executor_class"), TEXT("/Script/OpenCueForUnrealCmdline.MoviePipelineOpenCueCmdExecutor"));

	// Render settings
	TSharedPtr<FJsonObject> RenderObj = MakeShared<FJsonObject>();
	RenderObj->SetNumberField(TEXT("quality"), OpenCueConfig.GetQualityAsInt());
	RenderObj->SetStringField(TEXT("format"), OpenCueConfig.GetFormatAsString());

	FString ResolvedGameModeClass;
	FString ResolvedGameModeSource;
	ResolveCmdGameModeClass(ResolvedGameModeClass, ResolvedGameModeSource);
	if (!ResolvedGameModeClass.IsEmpty())
	{
		RenderObj->SetStringField(TEXT("game_mode_class"), ResolvedGameModeClass);
		UE_LOG(LogTemp, Log, TEXT("[OpenCue] Resolved -game GameMode (%s): %s"), *ResolvedGameModeSource, *ResolvedGameModeClass);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[OpenCue] No GameMode override resolved for -game render."));
	}
	TArray<TSharedPtr<FJsonValue>> AdditionalArgs;
	RenderObj->SetArrayField(TEXT("additional_ue_args"), AdditionalArgs);
	RootObj->SetObjectField(TEXT("render"), RenderObj);

	// Outputs
	TSharedPtr<FJsonObject> OutputsObj = MakeShared<FJsonObject>();
	OutputsObj->SetStringField(TEXT("local_base_dir_relpath"), TEXT("Saved/MovieRenders"));
	TSharedPtr<FJsonObject> PublishHintObj = MakeShared<FJsonObject>();
	PublishHintObj->SetStringField(TEXT("note"), TEXT("V1 does not implement artifact publishing."));
	OutputsObj->SetObjectField(TEXT("publish_hint"), PublishHintObj);
	RootObj->SetObjectField(TEXT("outputs"), OutputsObj);

	// Tasks
	TArray<TSharedPtr<FJsonValue>> TasksArray;
	for (const FOpenCueRenderTask& Task : Tasks)
	{
		TasksArray.Add(MakeShared<FJsonValueObject>(Task.ToJsonObject()));
	}
	RootObj->SetArrayField(TEXT("tasks"), TasksArray);

	// Serialize to string
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);

	return OutputString;
}

bool UMoviePipelineOpenCueExecutorJob::PublishRenderPlan(const FString& JobId, const FString& RenderPlanJson, FString& OutPlanUri, FString& OutError) const
{
	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();
	if (!Settings)
	{
		OutError = TEXT("Failed to get settings");
		return false;
	}

	FString PublishDir = Settings->GetEffectivePlanPublishDirectory();

	// Ensure directory exists
	if (!FPaths::DirectoryExists(PublishDir))
	{
		IFileManager::Get().MakeDirectory(*PublishDir, true);
	}

	// Write file
	FString FileName = JobId + TEXT(".json");
	FString FilePath = FPaths::Combine(PublishDir, FileName);

	if (!FFileHelper::SaveStringToFile(RenderPlanJson, *FilePath))
	{
		OutError = FString::Printf(TEXT("Failed to write to %s"), *FilePath);
		return false;
	}

	// Build URI
	if (!Settings->PlanURIPrefix.IsEmpty())
	{
		// Use configured prefix
		OutPlanUri = Settings->PlanURIPrefix;
		if (!OutPlanUri.EndsWith(TEXT("/")))
		{
			OutPlanUri += TEXT("/");
		}
		OutPlanUri += FileName;
	}
	else
	{
		// Use file:// protocol
		FString FullPath = FPaths::ConvertRelativePathToFull(FilePath);
		// Convert backslashes to forward slashes for URI
		FullPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		OutPlanUri = TEXT("file:///") + FullPath;
	}

	return true;
}

FString UMoviePipelineOpenCueExecutorJob::GenerateSubmitSpecJson(const FString& JobId, const FString& PlanUri, int32 TaskCount) const
{
	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();

	TSharedPtr<FJsonObject> RootObj = MakeShared<FJsonObject>();

	// Cuebot
	TSharedPtr<FJsonObject> CuebotObj = MakeShared<FJsonObject>();
	CuebotObj->SetStringField(TEXT("host"), OpenCueConfig.GetEffectiveCuebotHost());
	CuebotObj->SetNumberField(TEXT("port"), Settings ? Settings->CuebotPort : 8443);
	RootObj->SetObjectField(TEXT("cuebot"), CuebotObj);

	// Show and user
	RootObj->SetStringField(TEXT("show"), OpenCueConfig.GetEffectiveShowName());
	RootObj->SetStringField(TEXT("user"), FPlatformProcess::UserName());

	// Job
	TSharedPtr<FJsonObject> JobObj = MakeShared<FJsonObject>();
	JobObj->SetStringField(TEXT("name"), OpenCueConfig.JobName);
	if (!OpenCueConfig.JobComment.IsEmpty())
	{
		JobObj->SetStringField(TEXT("comment"), OpenCueConfig.JobComment);
	}
	JobObj->SetNumberField(TEXT("priority"), OpenCueConfig.Priority);
	RootObj->SetObjectField(TEXT("job"), JobObj);

	// Plan
	TSharedPtr<FJsonObject> PlanObj = MakeShared<FJsonObject>();
	PlanObj->SetStringField(TEXT("plan_uri"), PlanUri);
	RootObj->SetObjectField(TEXT("plan"), PlanObj);

	// OpenCue
	TSharedPtr<FJsonObject> OpenCueObj = MakeShared<FJsonObject>();
	OpenCueObj->SetStringField(TEXT("layer_name"), TEXT("render"));
	OpenCueObj->SetNumberField(TEXT("task_count"), TaskCount);
	OpenCueObj->SetStringField(TEXT("cmd"), BuildWrapperCommand(PlanUri));
	RootObj->SetObjectField(TEXT("opencue"), OpenCueObj);

	// Serialize
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootObj.ToSharedRef(), Writer);

	return OutputString;
}

FString UMoviePipelineOpenCueExecutorJob::BuildWrapperCommand(const FString& PlanUri) const
{
	FString PlanPath = PlanUri;
	const FString FileScheme = TEXT("file:///");
	if (PlanPath.StartsWith(FileScheme, ESearchCase::IgnoreCase))
	{
		PlanPath.RightChopInline(FileScheme.Len());
		PlanPath.ReplaceInline(TEXT("/"), TEXT("\\"));
	}

	return FString::Printf(
		TEXT("opencue-ue-agent.bat run-one-shot-plan --plan-path \"%s\""),
		*PlanPath
	);
}

bool UMoviePipelineOpenCueExecutorJob::CallSubmitterCLI(const FString& SubmitSpecPath, FOpenCueSubmitResult& OutResult) const
{
	const UOpenCueDeveloperSettings* Settings = GetDefault<UOpenCueDeveloperSettings>();
	if (!Settings)
	{
		OutResult.bSuccess = false;
		OutResult.ErrorMessage = TEXT("Failed to get settings");
		return false;
	}

	FString SubmitterCLIPath = Settings->GetEffectiveSubmitterCLIPath();
	const FString ExplicitPythonPath = Settings->PythonPath.TrimStartAndEnd();
	const bool bPreferDeveloperMode = !ExplicitPythonPath.IsEmpty();

	// Always pass absolute --spec path so process working directory does not affect lookup.
	FString SubmitSpecPathForCLI = SubmitSpecPath;
	if (FPaths::IsRelative(SubmitSpecPathForCLI))
	{
		SubmitSpecPathForCLI = FPaths::ConvertRelativePathToFull(SubmitSpecPathForCLI);
	}
	FPaths::NormalizeFilename(SubmitSpecPathForCLI);

	FString ExecutablePath;
	FString CommandArgs;
	FString WorkingDirectory;

	auto LooksLikeFilesystemPath = [](const FString& InPath) -> bool
	{
		return InPath.Contains(TEXT("\\")) || InPath.Contains(TEXT("/")) || InPath.Contains(TEXT(":"));
	};

	if (bPreferDeveloperMode)
	{
		ExecutablePath = ExplicitPythonPath;
		if (LooksLikeFilesystemPath(ExecutablePath) && FPaths::IsRelative(ExecutablePath))
		{
			ExecutablePath = FPaths::ConvertRelativePathToFull(ExecutablePath);
		}
		FPaths::NormalizeFilename(ExecutablePath);

		TArray<FString> CandidateRoots;
		auto AddCandidateRoot = [&](const FString& InCandidate) -> void
		{
			if (InCandidate.IsEmpty())
			{
				return;
			}

			FString Candidate = InCandidate;
			if (LooksLikeFilesystemPath(Candidate) && FPaths::IsRelative(Candidate))
			{
				Candidate = FPaths::ConvertRelativePathToFull(Candidate);
			}
			FPaths::NormalizeFilename(Candidate);

			if (FPaths::FileExists(Candidate))
			{
				const FString ParentDir = FPaths::GetPath(Candidate);
				if (!ParentDir.IsEmpty())
				{
					CandidateRoots.AddUnique(ParentDir);

					const FString ParentName = FPaths::GetCleanFilename(ParentDir).ToLower();
					if (ParentName == TEXT("dist"))
					{
						const FString ParentParent = FPaths::GetPath(ParentDir);
						if (!ParentParent.IsEmpty())
						{
							CandidateRoots.AddUnique(ParentParent);
						}
					}
				}
				return;
			}

			if (FPaths::DirectoryExists(Candidate))
			{
				CandidateRoots.AddUnique(Candidate);
			}
		};

		// Priority in developer mode:
		// 1) User provided Submitter Path (if any)
		// 2) Effective submitter path resolution fallback
		// 3) Local sibling source tree (common dev layout)
		AddCandidateRoot(Settings->SubmitterCLIPath);
		AddCandidateRoot(SubmitterCLIPath);
		AddCandidateRoot(FPaths::Combine(FPaths::ProjectDir(), TEXT("../opencue-ue-services")));

		FString DeveloperModuleRoot;
		for (const FString& Root : CandidateRoots)
		{
			const FString ModuleEntry = FPaths::Combine(Root, TEXT("src/ue_submit/__main__.py"));
			if (FPaths::FileExists(ModuleEntry))
			{
				DeveloperModuleRoot = Root;
				break;
			}
		}

		if (DeveloperModuleRoot.IsEmpty())
		{
			OutResult.bSuccess = false;
			OutResult.ErrorMessage = TEXT("Python Path is set, but src/ue_submit module root was not found.");
			OutResult.ErrorHint = TEXT("In developer mode, set Submitter Path to the opencue-ue-services source directory (contains src/ue_submit).");
			return false;
		}

		WorkingDirectory = DeveloperModuleRoot;
		CommandArgs = FString::Printf(
			TEXT("-m src.ue_submit submit --spec \"%s\""),
			*SubmitSpecPathForCLI
		);
		UE_LOG(LogTemp, Log, TEXT("[OpenCue] Submitter mode: Developer (Python Path priority)"));
	}
	else
	{
		if (SubmitterCLIPath.IsEmpty())
		{
			OutResult.bSuccess = false;
			OutResult.ErrorMessage = TEXT("Submitter path not configured");
			OutResult.ErrorHint = TEXT("Configure Submitter Path, or set Python Path for developer mode.");
			return false;
		}

		// Packaged/runtime mode:
		// 1) Directory mode: python -m src.ue_submit (working dir = directory)
		// 2) File mode (.exe/.bat/.cmd/.py): run file directly (or via python/cmd)
		FString ResolvedSubmitterPath = SubmitterCLIPath;
		if (LooksLikeFilesystemPath(ResolvedSubmitterPath) && FPaths::IsRelative(ResolvedSubmitterPath))
		{
			ResolvedSubmitterPath = FPaths::ConvertRelativePathToFull(ResolvedSubmitterPath);
		}
		FPaths::NormalizeFilename(ResolvedSubmitterPath);

		const bool bSubmitterIsFile = FPaths::FileExists(ResolvedSubmitterPath);
		const bool bSubmitterIsDirectory = FPaths::DirectoryExists(ResolvedSubmitterPath);

		if (bSubmitterIsFile)
		{
			const FString Extension = FPaths::GetExtension(ResolvedSubmitterPath, true).ToLower();
			WorkingDirectory = FPaths::GetPath(ResolvedSubmitterPath);

			if (Extension == TEXT(".py"))
			{
				ExecutablePath = Settings->GetEffectivePythonPath();
				CommandArgs = FString::Printf(
					TEXT("\"%s\" submit --spec \"%s\""),
					*ResolvedSubmitterPath,
					*SubmitSpecPathForCLI
				);
			}
			else if (Extension == TEXT(".bat") || Extension == TEXT(".cmd"))
			{
				ExecutablePath = TEXT("cmd.exe");
				CommandArgs = FString::Printf(
					TEXT("/c \"\"%s\" submit --spec \"%s\"\""),
					*ResolvedSubmitterPath,
					*SubmitSpecPathForCLI
				);
			}
			else
			{
				ExecutablePath = ResolvedSubmitterPath;
				CommandArgs = FString::Printf(
					TEXT("submit --spec \"%s\""),
					*SubmitSpecPathForCLI
				);
			}
		}
		else if (bSubmitterIsDirectory)
		{
			ExecutablePath = Settings->GetEffectivePythonPath();
			CommandArgs = FString::Printf(
				TEXT("-m src.ue_submit submit --spec \"%s\""),
				*SubmitSpecPathForCLI
			);
			WorkingDirectory = ResolvedSubmitterPath;
		}
		else
		{
			OutResult.bSuccess = false;
			OutResult.ErrorMessage = FString::Printf(
				TEXT("Submitter path does not exist: %s"),
				*ResolvedSubmitterPath
			);
			OutResult.ErrorHint = TEXT("Set Submitter Path to a valid directory, .exe, .bat, .cmd, or .py.");
			return false;
		}

		UE_LOG(LogTemp, Log, TEXT("[OpenCue] Submitter mode: Runtime (Submitter Path)"));
	}

	if (WorkingDirectory.IsEmpty())
	{
		WorkingDirectory = FPaths::ProjectDir();
	}

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Calling: %s %s"), *ExecutablePath, *CommandArgs);
	UE_LOG(LogTemp, Log, TEXT("[OpenCue] Working dir: %s"), *WorkingDirectory);

	// Execute the process
	int32 ReturnCode = 0;
	FString StdOut;
	FString StdErr;

	bool bSuccess = FPlatformProcess::ExecProcess(
		*ExecutablePath,
		*CommandArgs,
		&ReturnCode,
		&StdOut,
		&StdErr,
		*WorkingDirectory
	);

	if (!bSuccess)
	{
		OutResult.bSuccess = false;
		OutResult.ErrorMessage = TEXT("Failed to execute Submitter CLI");
		OutResult.ErrorHint = FString::Printf(
			TEXT("Check executable path and working directory. Executable: %s, WorkingDir: %s"),
			*ExecutablePath,
			*WorkingDirectory
		);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("[OpenCue] CLI return code: %d"), ReturnCode);
	UE_LOG(LogTemp, Log, TEXT("[OpenCue] CLI stdout: %s"), *StdOut);
	if (!StdErr.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[OpenCue] CLI stderr: %s"), *StdErr);
	}

	// Parse the stdout JSON (last line)
	if (!ParseSubmitterOutput(StdOut, OutResult))
	{
		OutResult.bSuccess = false;
		OutResult.ErrorMessage = TEXT("Failed to parse Submitter CLI output");
		OutResult.ErrorHint = FString::Printf(TEXT("Raw output: %s"), *StdOut);
		return false;
	}

	return true;
}

bool UMoviePipelineOpenCueExecutorJob::ParseSubmitterOutput(const FString& StdOut, FOpenCueSubmitResult& OutResult) const
{
	// Find the last line (should be JSON)
	TArray<FString> Lines;
	StdOut.ParseIntoArrayLines(Lines);

	if (Lines.Num() == 0)
	{
		return false;
	}

	FString LastLine = Lines.Last().TrimStartAndEnd();
	if (LastLine.IsEmpty() && Lines.Num() > 1)
	{
		LastLine = Lines[Lines.Num() - 2].TrimStartAndEnd();
	}

	// Parse JSON
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(LastLine);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		return false;
	}

	// Extract fields
	OutResult.bSuccess = JsonObject->GetBoolField(TEXT("ok"));

	if (JsonObject->HasField(TEXT("job_id")))
	{
		OutResult.JobId = JsonObject->GetStringField(TEXT("job_id"));
	}

	if (JsonObject->HasField(TEXT("opencue_job_ids")))
	{
		const TArray<TSharedPtr<FJsonValue>>* JobIdsArray;
		if (JsonObject->TryGetArrayField(TEXT("opencue_job_ids"), JobIdsArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *JobIdsArray)
			{
				OutResult.OpenCueJobIds.Add(Value->AsString());
			}
		}
	}

	if (JsonObject->HasField(TEXT("error")))
	{
		OutResult.ErrorMessage = JsonObject->GetStringField(TEXT("error"));
	}

	if (JsonObject->HasField(TEXT("hint")))
	{
		OutResult.ErrorHint = JsonObject->GetStringField(TEXT("hint"));
	}

	return true;
}

FString UMoviePipelineOpenCueExecutorJob::BuildCommandLineArgs() const
{
	// Legacy method - kept for reference but not used in V1
	FString Args;

	Args += FString::Printf(TEXT("--job-name \"%s\" "), *OpenCueConfig.JobName);
	Args += FString::Printf(TEXT("--show \"%s\" "), *OpenCueConfig.GetEffectiveShowName());

	if (Sequence.IsValid())
	{
		Args += FString::Printf(TEXT("--sequence \"%s\" "), *Sequence.GetAssetPathString());
	}

	if (Map.IsValid())
	{
		Args += FString::Printf(TEXT("--map \"%s\" "), *Map.GetAssetPathString());
	}

	Args += FString::Printf(TEXT("--quality %d "), OpenCueConfig.GetQualityAsInt());
	Args += FString::Printf(TEXT("--format %s "), *OpenCueConfig.GetFormatAsString());
	Args += FString::Printf(TEXT("--priority %d "), OpenCueConfig.Priority);
	Args += FString::Printf(TEXT("--cuebot \"%s\" "), *OpenCueConfig.GetEffectiveCuebotHost());

	if (!OpenCueConfig.JobComment.IsEmpty())
	{
		Args += FString::Printf(TEXT("--comment \"%s\" "), *OpenCueConfig.JobComment);
	}

	return Args;
}

#if WITH_EDITOR
void UMoviePipelineOpenCueExecutorJob::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Auto-generate job name when sequence changes
	FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UMoviePipelineExecutorJob, Sequence))
	{
		if (OpenCueConfig.JobName.IsEmpty() || OpenCueConfig.JobName == TEXT("UE5_Render"))
		{
			GenerateJobNameFromSequence();
		}
	}
}
#endif

#undef LOCTEXT_NAMESPACE
