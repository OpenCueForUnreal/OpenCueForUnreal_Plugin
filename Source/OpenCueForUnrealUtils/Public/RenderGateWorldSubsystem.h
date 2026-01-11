// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RenderGateWorldSubsystem.generated.h"

/**
 * 
 */
UCLASS()
class OPENCUEFORUNREALUTILS_API URenderGateWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
    
public:
    DECLARE_MULTICAST_DELEGATE(FOnReady);

    UFUNCTION(BlueprintCallable)
    void MarkReady()
    {
        if (!bReady)
        {
            bReady = true;
            OnReady.Broadcast();
        }
    }

    UFUNCTION(BlueprintPure)
    bool IsReady() const { return bReady; }

    FOnReady& OnReadyEvent() { return OnReady; }

private:
    bool bReady = false;
    FOnReady OnReady;
};
