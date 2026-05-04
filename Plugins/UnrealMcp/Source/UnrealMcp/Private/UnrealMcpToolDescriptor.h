#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpToolDescriptor.generated.h"

UENUM()
enum class EUnrealMcpToolExposure : uint8
{
	Visible,
	LegacyHidden
};

UENUM()
enum class EUnrealMcpToolRisk : uint8
{
	ReadOnly,
	Low,
	Medium,
	High,
	Critical
};

UENUM()
enum class EUnrealMcpToolTestCoverage : uint8
{
	Missing,
	Core,
	Category,
	Manual,
	External
};

USTRUCT()
struct FUnrealMcpToolDescriptor
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	FString Title;

	UPROPERTY()
	FString Description;

	UPROPERTY()
	FString Category;

	UPROPERTY()
	FString HandlerName;

	UPROPERTY()
	FString SourceFile;

	UPROPERTY()
	EUnrealMcpToolExposure Exposure = EUnrealMcpToolExposure::Visible;

	UPROPERTY()
	EUnrealMcpToolRisk RiskLevel = EUnrealMcpToolRisk::Low;

	UPROPERTY()
	bool bRequiresWrite = false;

	UPROPERTY()
	bool bRequiresBuild = false;

	UPROPERTY()
	bool bRequiresExternalProcess = false;

	UPROPERTY()
	bool bRequiresRestart = false;

	UPROPERTY()
	bool bRequiresProjectMemory = false;

	UPROPERTY()
	bool bRequiresLock = false;

	UPROPERTY()
	bool bDryRunSupport = false;

	UPROPERTY()
	bool bPreflightSupport = false;

	UPROPERTY()
	bool bPostcheckSupport = false;

	UPROPERTY()
	EUnrealMcpToolTestCoverage TestCoverage = EUnrealMcpToolTestCoverage::Missing;

	UPROPERTY()
	FString Owner = TEXT("UEvolve Core");

	UPROPERTY()
	FString DocsPath = TEXT("README.md#tool-coverage");

	UPROPERTY()
	FString Reason;

	UPROPERTY()
	FString Notes;
};
