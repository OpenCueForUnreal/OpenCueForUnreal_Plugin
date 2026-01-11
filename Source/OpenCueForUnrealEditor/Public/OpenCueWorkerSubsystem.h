// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Interfaces/IHttpRequest.h"
#include "OpenCueWorkerSubsystem.generated.h"

/**
 * 
 */
UCLASS()
class OPENCUEFORUNREALEDITOR_API UOpenCueWorkerSubsystem : public UEditorSubsystem, public FTickableEditorObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UOpenCueWorkerSubsystem, STATGROUP_Tickables);
	}

	UFUNCTION(BlueprintCallable, Category = "OpenCueWorkerSubsystem")
	bool IsWorkerMode() const { return bWorkerMode; }

	UFUNCTION(BlueprintCallable, Category = "OpenCueWorkerSubsystem")
	FString GetCurrentJobId() const;

	void SetCurrentJobId(const FString& JobId) { CurrentJobId = JobId; }
	
private:
	void RequestLease();

	void OnLeaseResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	void StartRenderFromLease(const FString& JobId, const FString& MapUrl, const FString& LevelSequencePath);

	// URL parsing helpers
	static FString StripMapOptions(const FString& MapUrl);
	static FString GetMapOptions(const FString& MapUrl, const FString& Key);

	// Heartbeat
	void SendHeartbeat();
	void OnHeartbeatResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

	// Ready signal - notify worker pool that this worker is ready to accept tasks
	void SendReadySignal();
	void OnReadyResponse(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful);

private:
	bool bWorkerMode = false;
	bool bBusy = false;
	bool bReady = false;  // True after ready signal is acknowledged by worker pool
	bool bReadyRequestInFlight = false;
	bool bLeaseRequestInFlight = false;
	bool bHeartbeatRequestInFlight = false;

	FString WorkerId;
	FString CurrentJobId;
	FString WorkerPoolBaseUrl;
	FString MRQServerBaseUrl;

	float LeasePollIntervalSec = 1.0f;
	float HeartbeatPollIntervalSec = 5.0f;
	float TimeSinceLastHeartbeat = 0.0f;
	float TimeSinceLastLease = 0.0f;
	double LastHeartbeatTime = 0.0f;
	
};
