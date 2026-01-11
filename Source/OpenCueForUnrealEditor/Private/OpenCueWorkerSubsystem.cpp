// Fill out your copyright notice in the Description page of Project Settings.


#include "OpenCueWorkerSubsystem.h"
#include "Editor.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"

#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"
#include "MoviePipelineOpenCuePIEExecutor.h"
#include "MoviePipelineGameOverrideSetting.h"
#include "AI/NavigationSystemBase.h"

void UOpenCueWorkerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	bWorkerMode = FParse::Param(FCommandLine::Get(), TEXT("MRQWorkerMode"));
	if (!bWorkerMode)
	{
		return;
	}

	FParse::Value(FCommandLine::Get(), TEXT("-MRQWorkerId="), WorkerId);
	FParse::Value(FCommandLine::Get(), TEXT("-WorkerPoolBaseUrl="), WorkerPoolBaseUrl);
	FParse::Value(FCommandLine::Get(), TEXT("-MRQServerBaseUrl="), MRQServerBaseUrl);

	if (!WorkerPoolBaseUrl.IsEmpty() && !WorkerPoolBaseUrl.EndsWith(TEXT("/")))
	{
		WorkerPoolBaseUrl.Append(TEXT("/"));
	}

	if (!MRQServerBaseUrl.IsEmpty() && !MRQServerBaseUrl.EndsWith(TEXT("/")))
	{
		MRQServerBaseUrl.Append(TEXT("/"));
	}

	if (WorkerId.IsEmpty() || WorkerPoolBaseUrl.IsEmpty() || MRQServerBaseUrl.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("%s: Missing args: -MRQWorkerId / -WorkerPoolBaseUrl / -MRQServerBaseUrl"), ANSI_TO_TCHAR(__FUNCTION__));
	}

	UE_LOG(LogTemp, Log, TEXT("%s: Worker mode enabled. wid=%s, daemon=%s, server=%s"), ANSI_TO_TCHAR(__FUNCTION__), *WorkerId, *WorkerPoolBaseUrl, *MRQServerBaseUrl);
	
}

void UOpenCueWorkerSubsystem::Deinitialize()
{
	Super::Deinitialize();
}

void UOpenCueWorkerSubsystem::Tick(float DeltaTime)
{
	if (!bWorkerMode)
	{
		return;
	}

	// First, ensure we've sent the ready signal before doing anything else
	if (!bReady)
	{
		if (!bReadyRequestInFlight)
		{
			SendReadySignal();
		}
		return;  // Don't do anything until we're ready
	}

	// Accumulate time for heartbeat
	TimeSinceLastHeartbeat += DeltaTime;
	if (TimeSinceLastHeartbeat >= HeartbeatPollIntervalSec)
	{
		TimeSinceLastHeartbeat = 0.0f;
		SendHeartbeat();
	}

	if (bLeaseRequestInFlight)
	{
		return;
	}

	if (GEditor)
	{
		if (UMoviePipelineQueueSubsystem* QueueSubsystem = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>())
		{
			if (QueueSubsystem->IsRendering())
			{
				bBusy = true;
				return;
			}
		}
	}

	bBusy = false;

	// Accumulate time for lease poll (since Editor Tick runs every frame)
	TimeSinceLastLease += DeltaTime;
	if (TimeSinceLastLease >= LeasePollIntervalSec)
	{
		TimeSinceLastLease = 0.0f;
		RequestLease();
	}
}

bool UOpenCueWorkerSubsystem::IsTickable() const
{
	return FTickableEditorObject::IsTickable();
}

FString UOpenCueWorkerSubsystem::GetCurrentJobId() const
{
	if (bWorkerMode)
	{
		return CurrentJobId;
	}
	else
	{
		FString CmdLineJobId;
		FParse::Value(FCommandLine::Get(), TEXT("-JobId="), CmdLineJobId);
		return CmdLineJobId;
	}
}

void UOpenCueWorkerSubsystem::RequestLease()
{
	if (bLeaseRequestInFlight)
	{
		return;
	}

	const FString InURL = FString::Printf(TEXT("%sworkers/%s/lease"), *WorkerPoolBaseUrl, *WorkerId);
	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(InURL);
	Request->SetVerb("GET");
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->OnProcessRequestComplete().BindUObject(this, &UOpenCueWorkerSubsystem::OnLeaseResponse);

	bLeaseRequestInFlight = true;
	Request->ProcessRequest();
}

void UOpenCueWorkerSubsystem::OnLeaseResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bLeaseRequestInFlight = false;
	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: lease request failed."), ANSI_TO_TCHAR(__FUNCTION__));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	if (Code == 204)
	{
		return;
	}
	if (Code != 200)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: lease response %d: %s"), ANSI_TO_TCHAR(__FUNCTION__), Code, *Response->GetContentAsString());
		return;
	}

	const FString Body = Response->GetContentAsString();
	TSharedPtr<FJsonObject> RootObj;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: Invalid lease JSON: %s"), ANSI_TO_TCHAR(__FUNCTION__), *Body);
		return;
	}

	FString JobId;
	FString MapUrl;
	FString LevelSequencePath;
	RootObj->TryGetStringField(TEXT("job_id"), JobId);
	RootObj->TryGetStringField(TEXT("map_url"), MapUrl);
	RootObj->TryGetStringField(TEXT("level_sequence"), LevelSequencePath);

	if (JobId.IsEmpty() || MapUrl.IsEmpty() || LevelSequencePath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: lease missing fields: %s"), ANSI_TO_TCHAR(__FUNCTION__), *Body);
		return;
	}

	StartRenderFromLease(JobId, MapUrl, LevelSequencePath);
}

void UOpenCueWorkerSubsystem::StartRenderFromLease(const FString& JobId, const FString& MapUrl,
	const FString& LevelSequencePath)
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Error, TEXT("%s: GEditor not ready; cannot start job: %s"), ANSI_TO_TCHAR(__FUNCTION__), *JobId);
		return;
	}

	UMoviePipelineQueueSubsystem* QueueSubsystem = GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>();
	if (!QueueSubsystem)
	{
		UE_LOG(LogTemp, Error, TEXT("%s: MoviePipelineQueueSubsystem missing; cannot start job %s"), ANSI_TO_TCHAR(__FUNCTION__), *JobId);
		return;
	}

	UMoviePipelineQueue* Queue = QueueSubsystem->GetQueue();
	if (!Queue)
	{
		UE_LOG(LogTemp, Error, TEXT("%s: Queue missing; cannot start job %s"), ANSI_TO_TCHAR(__FUNCTION__), *JobId);
		return;
	}

	Queue->DeleteAllJobs();

	UMoviePipelineExecutorJob* NewJob = Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
	NewJob->JobName = JobId;
	NewJob->UserData = JobId;
	NewJob->Map = FSoftObjectPath(StripMapOptions(MapUrl));
	NewJob->Sequence = FSoftObjectPath(LevelSequencePath);

	// Apply GameMode override from map_url if provided (e.g., ?game=/Script/MyModule.MyGameMode)
	FString GameModeClassPath = GetMapOptions(MapUrl, TEXT("game"));
	if (!GameModeClassPath.IsEmpty())
	{
		UClass* GameModeClass = LoadClass<AGameModeBase>(nullptr, *GameModeClassPath);
		if (GameModeClass)
		{
			UMoviePipelineGameOverrideSetting* GameOverride = Cast<UMoviePipelineGameOverrideSetting>(
				NewJob->GetConfiguration()->FindOrAddSettingByClass(UMoviePipelineGameOverrideSetting::StaticClass()));
			if (GameOverride)
			{
				GameOverride->GameModeOverride = GameModeClass;
				UE_LOG(LogTemp, Log, TEXT("%s: GameMode override: %s"), ANSI_TO_TCHAR(__FUNCTION__), *GameModeClass->GetPathName());
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("%s: Failed to load GameMode class: %s"), ANSI_TO_TCHAR(__FUNCTION__), *GameModeClassPath);
		}
	}

	// Store current JobId for access by other systems (e.g., Executor)
	CurrentJobId = JobId;

	bBusy = true;

	UE_LOG(LogTemp, Log, TEXT("%s: start job=%s map=%s seq=%s"), ANSI_TO_TCHAR(__FUNCTION__), *JobId, *NewJob->Map.ToString(), *NewJob->Sequence.ToString());

	QueueSubsystem->RenderQueueWithExecutor(UMoviePipelineOpenCuePIEExecutor::StaticClass());
}

FString UOpenCueWorkerSubsystem::StripMapOptions(const FString& MapUrl)
{
	int32 QueryIndex = INDEX_NONE;
	if (MapUrl.FindChar(TEXT('?'), QueryIndex))
	{
		return MapUrl.Left(QueryIndex);
	}

	return MapUrl;
}

FString UOpenCueWorkerSubsystem::GetMapOptions(const FString& MapUrl, const FString& Key)
{
	// Find the query string start
	int32 QueryIndex = INDEX_NONE;
	if (!MapUrl.FindChar(TEXT('?'), QueryIndex))
	{
		return MapUrl;
	}

	// Parse options: ?key1=val1?key2=val2
	FString OptionsStr = MapUrl.Mid(QueryIndex + 1);
	TArray<FString> Options;
	OptionsStr.ParseIntoArray(Options, TEXT("?"));

	for (const FString& Option : Options)
	{
		FString OptionKey, OptionValue;
		if (Option.Split(TEXT("="), &OptionKey, &OptionValue))
		{
			if (OptionKey.Equals(Key, ESearchCase::IgnoreCase))
			{
				return OptionValue;
			}
		}
	}

	return FString();
}

void UOpenCueWorkerSubsystem::SendHeartbeat()
{
	UE_LOG(LogTemp, Log, TEXT("%s: Sending heartbeat busy=%d, inFlight=%d"), ANSI_TO_TCHAR(__FUNCTION__), bBusy, bHeartbeatRequestInFlight);

	const double Now = FPlatformTime::Seconds();
	if (bHeartbeatRequestInFlight)
	{
		// Safety check: if heartbeat has been in flight far too long ( >15s ), assume it failed/stuck and reset
		if (Now - LastHeartbeatTime > 15.0f)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s: Heartbeat request stuck for %.2fs, forcing reset"), ANSI_TO_TCHAR(__FUNCTION__), Now - LastHeartbeatTime);
			bHeartbeatRequestInFlight = false;
		}
		else
		{
			return;
		}
	}

	const FString InURL = FString::Printf(TEXT("%sworkers/%s/heartbeat"), *WorkerPoolBaseUrl, *WorkerId);

	// Build JSON body: {"busy": true/false}
	FString JsonBody;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonBody);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("busy"), bBusy);
	Writer->WriteObjectEnd();
	Writer->Close();

	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(InURL);
	Request->SetVerb("POST");
	Request->SetHeader("Content-Type", "application/json");
	// Disable connection reuse to prevent libcurl 'rewind' failures when server resets connection
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetContentAsString(JsonBody);
	Request->SetTimeout(10.0f);
	Request->OnProcessRequestComplete().BindUObject(this, &UOpenCueWorkerSubsystem::OnHeartbeatResponse);

	bHeartbeatRequestInFlight = true;
	LastHeartbeatTime = Now;
	Request->ProcessRequest();
}

void UOpenCueWorkerSubsystem::OnHeartbeatResponse(FHttpRequestPtr Request, FHttpResponsePtr Response,
	bool bWasSuccessful)
{
	bHeartbeatRequestInFlight = false;

	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: heartbeat request failed"), ANSI_TO_TCHAR(__FUNCTION__));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	if (Code != 200)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: heartbeat response %d: %s"), ANSI_TO_TCHAR(__FUNCTION__), Code, *Response->GetContentAsString());
	}
}

void UOpenCueWorkerSubsystem::SendReadySignal()
{
	if (bReadyRequestInFlight || bReady)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("%s: Sending ready signal to worker pool..."), ANSI_TO_TCHAR(__FUNCTION__));

	const FString InURL = FString::Printf(TEXT("%sworkers/%s/ready"), *WorkerPoolBaseUrl, *WorkerId);

	FHttpRequestRef Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(InURL);
	Request->SetVerb("POST");
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetTimeout(10.0f);
	Request->OnProcessRequestComplete().BindUObject(this, &UOpenCueWorkerSubsystem::OnReadyResponse);

	bReadyRequestInFlight = true;
	Request->ProcessRequest();
}

void UOpenCueWorkerSubsystem::OnReadyResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
	bReadyRequestInFlight = false;

	if (!bWasSuccessful || !Response.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: Ready signal failed, will retry..."), ANSI_TO_TCHAR(__FUNCTION__));
		return;
	}

	const int32 Code = Response->GetResponseCode();
	if (Code == 200)
	{
		bReady = true;
		UE_LOG(LogTemp, Log, TEXT("%s: Worker is now READY. Starting lease polling and heartbeat."), ANSI_TO_TCHAR(__FUNCTION__));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("%s: Ready signal response %d: %s, will retry..."),
			ANSI_TO_TCHAR(__FUNCTION__), Code, *Response->GetContentAsString());
	}
}
