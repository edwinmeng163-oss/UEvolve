#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpModule.h"

class FJsonObject;

namespace UnrealMcp
{
	struct FInstallDoctorPluginCandidate
	{
		FString Path;
		bool bExists = false;
		bool bHasPluginDescriptor = false;
	};

	struct FInstallDoctorDuplicateScanResult
	{
		bool bDuplicatePluginConflict = false;
		FString MountedWinnerPath;
		TArray<FString> CandidatePaths;
		TArray<FString> ShadowedPluginPaths;
	};

	FUnrealMcpExecutionResult InstallDoctor(const FJsonObject& Arguments);
	void ScheduleInstallDoctorFirstRun();
	bool LoadLatestInstallDoctorSummary(TSharedPtr<FJsonObject>& OutSummary, FString& OutPath);
	FInstallDoctorDuplicateScanResult DetectDuplicateUnrealMcpPlugins(
		const TArray<FInstallDoctorPluginCandidate>& Candidates,
		const FString& MountedWinnerPath);
}
