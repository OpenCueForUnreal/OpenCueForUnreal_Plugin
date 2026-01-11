// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "MoviePipelineSetting.h"
#include "MoviePipelineCommandLineEncoder.h"
#include "Engine/EngineTypes.h"
#include "MovieRenderPipelineDataTypes.h"
#include "MoviePipelineCustomEncoder.generated.h"

/**
 * 
 */
UCLASS()
class OPENCUEFORUNREALUTILS_API UMoviePipelineCustomEncoder : public UMoviePipelineSetting
{
	struct FEncoderParams
	{
		FEncoderParams()
			: ExpectedFrameCount(0)
		{
		}
		
		FStringFormatNamedArguments NamedArguments;
		TMap<FString, TArray<FString>> FilesByExtensionType;
		TWeakObjectPtr<class UMoviePipelineExecutorShot> Shot;
		int32 ExpectedFrameCount;
	};

	GENERATED_BODY()
public:
	UMoviePipelineCustomEncoder();
	void StartEncodingProcess(TArray<FMoviePipelineShotOutputData>& InOutData, const bool bInIsShotEncode);
public:
#if WITH_EDITOR
	virtual FText GetDisplayText() const override { return NSLOCTEXT("MovieRenderPipeline", "CommandLineEncode_DisplayText", "Command Line Encoder"); }
	virtual FText GetCategoryText() const override { return NSLOCTEXT("MovieRenderPipeline", "ExportsCategoryName_Text", "Exports"); }
	virtual bool CanBeDisabled() const override { return true; }
#endif
	virtual void SetupForPipelineImpl(UMoviePipeline* InPipeline);
	virtual void ValidateStateImpl() override;
	virtual bool IsValidOnShots() const override { return false; }
	virtual bool IsValidOnPrimary() const override { return true; }
	virtual bool HasFinishedExportingImpl() override;
	virtual void BeginExportImpl() override;
	
protected:
	bool NeedsPerShotFlushing() const;
	void LaunchEncoder(const FEncoderParams& InParams);
	void OnTick();
	FString GetQualitySettingString() const;

public:
	/** 
	* File name format string override. If specified it will override the FileNameFormat from the Output setting.
	* If {shot_name} or {camera_name} is used, encoding will begin after each shot finishes rendering.
	* Can be different from the main one in the Output setting so you can render out frames to individual
	* shot folders but encode to one file.
	*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Line Encoder")
	FString FileNameFormatOverride;

	/** What encoding quality to use for this job? Exact command line arguments for each one are specified in Project Settings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Line Encoder")
	EMoviePipelineEncodeQuality Quality;
	
	/** Any additional arguments to pass to the CLI encode for this particular job. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Line Encoder")
	FString AdditionalCommandLineArgs;
	
	/** Should we delete the source files from disk after encoding? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Line Encoder")
	bool bDeleteSourceFiles;

	/** If a render was canceled (via hitting escape mid render) should we skip trying to encode the files we did produce? */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Line Encoder")
	bool bSkipEncodeOnRenderCanceled;

	/** Write the duration for each frame into the generated text file. Needed for some input types on some CLI encoding software. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Command Line Encoder")
	bool bWriteEachFrameDuration;
	
private:
	struct FActiveJob
	{
		FActiveJob()
			: ReadPipe(nullptr)
			, WritePipe(nullptr)
			, ExpectedFrameCount(0)
			, LastReportedFrame(0)
			, LastProgressSentTimeSeconds(-1.0)
			, EncodeStartTimeSeconds(-1.0)
			, LastReportedEtaSeconds(-1.0)
		{}

		FProcHandle ProcessHandle;
		void* ReadPipe;
		void* WritePipe;

		int32 ExpectedFrameCount;
		int32 LastReportedFrame;
		double LastProgressSentTimeSeconds;
		double EncodeStartTimeSeconds;
		double LastReportedEtaSeconds;
		FString PendingStdOut;
		TWeakObjectPtr<UMoviePipelineExecutorShot> Shot;

		TArray<FString> FilesToDelete;
	};

	TArray<FActiveJob> ActiveEncodeJobs;
};
