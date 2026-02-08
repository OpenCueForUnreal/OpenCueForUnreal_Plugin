// Copyright OpenCue for Unreal contributors. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "GameFramework/GameModeBase.h"
#include "UObject/SoftObjectPath.h"
#include "OpenCueJobSettings.generated.h"

/**
 * Render quality preset for OpenCue jobs
 */
UENUM(BlueprintType)
enum class EOpenCueRenderQuality : uint8
{
	LOW = 0      UMETA(DisplayName = "Low (24fps)"),
	MEDIUM = 1   UMETA(DisplayName = "Medium (30fps)"),
	HIGH = 2     UMETA(DisplayName = "High (60fps)"),
	EPIC = 3     UMETA(DisplayName = "Epic (120fps)")
};

/**
 * Output video format for OpenCue jobs
 */
UENUM(BlueprintType)
enum class EOpenCueOutputFormat : uint8
{
	MP4   UMETA(DisplayName = "MP4"),
	MOV   UMETA(DisplayName = "MOV")
};

/**
 * Global settings for OpenCue integration.
 * Configure in Project Settings > Plugins > OpenCue Settings
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "OpenCue Settings"))
class OPENCUEFORUNREALEDITOR_API UOpenCueDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UOpenCueDeveloperSettings();

	/** OpenCue Cuebot server hostname or IP */
	UPROPERTY(Config, EditAnywhere, Category = "OpenCue Server", meta = (DisplayName = "Cuebot Host"))
	FString CuebotHost = TEXT("localhost");

	/** OpenCue Cuebot server port */
	UPROPERTY(Config, EditAnywhere, Category = "OpenCue Server", meta = (DisplayName = "Cuebot Port", ClampMin = 1, ClampMax = 65535))
	int32 CuebotPort = 8443;

	/** OpenCue Show name (project/show identifier) */
	UPROPERTY(Config, EditAnywhere, Category = "OpenCue Server", meta = (DisplayName = "Show Name"))
	FString ShowName = TEXT("UE_RENDER");

	/** Default render quality for new jobs */
	UPROPERTY(Config, EditAnywhere, Category = "Render Defaults")
	EOpenCueRenderQuality DefaultQuality = EOpenCueRenderQuality::MEDIUM;

	/** Default output format for new jobs */
	UPROPERTY(Config, EditAnywhere, Category = "Render Defaults")
	EOpenCueOutputFormat DefaultFormat = EOpenCueOutputFormat::MP4;

	// ==================== V1 Submitter Settings ====================

	/**
	 * Path to Python executable (developer mode).
	 * If non-empty, submission prefers:
	 *   python -m src.ue_submit submit --spec ...
	 * and Submitter Path is treated as module-root hint (expects src/ue_submit).
	 * If empty, submission uses Submitter Path runtime mode.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Submitter", meta = (DisplayName = "Python Path"))
	FString PythonPath;

	/**
	 * Submitter path.
	 * Supported values:
	 *   - Directory containing src/ue_submit (developer mode)
	 *   - opencue-ue-submitter executable/script path (.exe/.bat/.cmd/.py) (runtime mode)
	 * Priority:
	 *   - Python Path non-empty => developer mode
	 *   - Python Path empty => runtime mode
	 * If empty, auto-discovery tries plugin bundled exe first.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Submitter", meta = (DisplayName = "Submitter Path"))
	FString SubmitterCLIPath;

	/**
	 * Directory where render_plan.json files are published.
	 * Workers must be able to read from this location.
	 * Supports:
	 *   - Local path: C:\RenderPlans\ or /mnt/render_plans/
	 *   - Network share: \\fileserver\render_plans\
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Submitter", meta = (DisplayName = "Plan Publish Directory"))
	FString PlanPublishDirectory;

	/**
	 * URI prefix for plan_uri in submit_spec.
	 * If empty, uses file:// protocol with PlanPublishDirectory.
	 * Examples:
	 *   - file:///mnt/render_plans/
	 *   - http://plan-server:8080/plans/
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Submitter", AdvancedDisplay, meta = (DisplayName = "Plan URI Prefix"))
	FString PlanURIPrefix;

	// Worker runtime configuration is server-side now (RQD / opencue-ue-agent environment).
	// UE submit side intentionally does not carry worker machine paths.

	// ==================== CommandLine (-game) Rendering ====================

	/**
	 * Number of frames to wait before initializing the render pipeline in -game (CommandLine) mode.
	 * Allows the scene to load, stream textures, build Nanite, and settle before rendering begins.
	 *
	 * This is analogous to "Initial Delay Frame Count" in the editor's PIE executor settings,
	 * but applies to command-line renders launched by OpenCue RQD / opencue-ue-agent.
	 *
	 * Set to 0 for no delay (default).
	 * Can also be overridden on the command line: -CmdInitialDelayFrames=<N>
	 */
	UPROPERTY(Config, EditAnywhere, Category = "CommandLine Rendering",
		meta = (DisplayName = "Initial Delay Frames (-game mode)", ClampMin = 0))
	int32 CmdInitialDelayFrameCount = 0;

	/**
	 * Fallback GameMode for command-line (-game) renders.
	 *
	 * Resolution order:
	 *   1) If MRQ per-job "GameMode Override (-game mode)" is set, that class is used.
	 *   2) Otherwise if MRQ native "Game Overrides" setting has GameModeOverride, that class is used.
	 *   3) Otherwise if selected map has WorldSettings "GameMode Override", that class is used.
	 *   4) Otherwise this fallback class is used.
	 *
	 * The resolved class is passed as map option: <MapAssetPath>?game=<ClassPath>.
	 * Leave empty to disable fallback override.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "CommandLine Rendering",
		meta = (DisplayName = "Fallback GameMode Class (-game mode)"))
	TSoftClassPtr<AGameModeBase> CmdGameModeClass =
		TSoftClassPtr<AGameModeBase>(FSoftClassPath(TEXT("/Script/MovieRenderPipelineCore.MoviePipelineGameMode")));

	// UDeveloperSettings interface
	virtual FName GetCategoryName() const override { return FName("Plugins"); }
	virtual FName GetSectionName() const override { return FName("OpenCue Settings"); }

#if WITH_EDITOR
	virtual FText GetSectionText() const override { return NSLOCTEXT("OpenCue", "SettingsSection", "OpenCue Settings"); }
	virtual FText GetSectionDescription() const override { return NSLOCTEXT("OpenCue", "SettingsDesc", "Configure OpenCue render farm integration"); }
#endif

	/** Get effective Python path (configured or "python") */
	FString GetEffectivePythonPath() const;

	/** Get effective Submitter CLI path */
	FString GetEffectiveSubmitterCLIPath() const;

	/** Get effective Plan publish directory */
	FString GetEffectivePlanPublishDirectory() const;
};

/**
 * Per-job settings for submitting to OpenCue.
 * These settings are configured in the Movie Render Queue job panel.
 */
USTRUCT(BlueprintType)
struct OPENCUEFORUNREALEDITOR_API FOpenCueJobConfig
{
	GENERATED_BODY()

	/** OpenCue job name used during submission (not the UE MRQ row name) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Job Settings",
		meta = (DisplayName = "OpenCue Job Name",
			ToolTip = "Name submitted to OpenCue as job.name. Defaults from UE MRQ Job name (fallback: Sequence name).",
			DisplayPriority = 0))
	FString JobName;

	/** Optional OpenCue job description/comment */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Job Settings",
		meta = (DisplayName = "OpenCue Job Comment", DisplayPriority = 1))
	FString JobComment;

	/** Render quality preset */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render Settings", meta = (DisplayPriority = 2))
	EOpenCueRenderQuality Quality = EOpenCueRenderQuality::MEDIUM;

	/** Output video format */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render Settings", meta = (DisplayPriority = 3))
	EOpenCueOutputFormat OutputFormat = EOpenCueOutputFormat::MP4;

	/** Optional per-job GameMode override for one-shot -game render */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Render Settings",
		meta = (DisplayName = "GameMode Override (-game mode)", DisplayPriority = 4))
	TSoftClassPtr<AGameModeBase> CmdGameModeOverrideClass;

	/** OpenCue job priority (0-100, higher = more priority) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Job Settings",
		meta = (DisplayName = "OpenCue Priority", ClampMin = 0, ClampMax = 100, DisplayPriority = 5))
	int32 Priority = 50;

	/** Override Cuebot host (leave empty to use default from settings) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", AdvancedDisplay,
		meta = (DisplayName = "Cuebot Host Override", DisplayPriority = 10))
	FString CuebotHostOverride;

	/** Override Show name (leave empty to use default from settings) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Advanced", AdvancedDisplay,
		meta = (DisplayName = "OpenCue Show Name Override", DisplayPriority = 11))
	FString ShowNameOverride;

	/** Get effective Cuebot host (override or default) */
	FString GetEffectiveCuebotHost() const;

	/** Get effective Show name (override or default) */
	FString GetEffectiveShowName() const;

	/** Get quality as integer (0-3) */
	int32 GetQualityAsInt() const { return static_cast<int32>(Quality); }

	/** Get format as string ("mp4" or "mov") */
	FString GetFormatAsString() const { return OutputFormat == EOpenCueOutputFormat::MP4 ? TEXT("mp4") : TEXT("mov"); }
};
