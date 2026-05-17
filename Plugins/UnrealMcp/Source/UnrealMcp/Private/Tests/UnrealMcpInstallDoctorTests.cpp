#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UnrealMcpInstallDoctor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealMcpInstallDoctorDuplicateDetectorTest,
	"UnrealMcp.InstallDoctor.DuplicateDetector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealMcpInstallDoctorDuplicateDetectorTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	{
		TArray<UnrealMcp::FInstallDoctorPluginCandidate> Candidates;
		UnrealMcp::FInstallDoctorPluginCandidate Mounted;
		Mounted.Path = TEXT("/Repo/Plugins/UnrealMcp");
		Mounted.bExists = true;
		Mounted.bHasPluginDescriptor = true;
		Candidates.Add(Mounted);

		const UnrealMcp::FInstallDoctorDuplicateScanResult Result =
			UnrealMcp::DetectDuplicateUnrealMcpPlugins(Candidates, TEXT("/Repo/Plugins/UnrealMcp"));

		TestFalse(TEXT("One mounted candidate is not a conflict."), Result.bDuplicatePluginConflict);
		TestEqual(TEXT("Mounted winner path is preserved."), Result.MountedWinnerPath, FString(TEXT("/Repo/Plugins/UnrealMcp")));
		TestEqual(TEXT("One unique candidate is reported."), Result.CandidatePaths.Num(), 1);
		TestEqual(TEXT("No shadowed paths for one mounted candidate."), Result.ShadowedPluginPaths.Num(), 0);
	}

	{
		TArray<UnrealMcp::FInstallDoctorPluginCandidate> Candidates;
		UnrealMcp::FInstallDoctorPluginCandidate Mounted;
		Mounted.Path = TEXT("/Repo/Plugins/UnrealMcp");
		Mounted.bExists = true;
		Mounted.bHasPluginDescriptor = true;
		Candidates.Add(Mounted);

		UnrealMcp::FInstallDoctorPluginCandidate EngineCopy;
		EngineCopy.Path = TEXT("/UE/Engine/Plugins/Marketplace/UnrealMcp");
		EngineCopy.bExists = true;
		EngineCopy.bHasPluginDescriptor = true;
		Candidates.Add(EngineCopy);

		const UnrealMcp::FInstallDoctorDuplicateScanResult Result =
			UnrealMcp::DetectDuplicateUnrealMcpPlugins(Candidates, TEXT("/Repo/Plugins/UnrealMcp"));

		TestTrue(TEXT("Two unique candidates are a conflict."), Result.bDuplicatePluginConflict);
		TestEqual(TEXT("Mounted winner path is preserved with conflict."), Result.MountedWinnerPath, FString(TEXT("/Repo/Plugins/UnrealMcp")));
		TestEqual(TEXT("Two unique candidates are reported."), Result.CandidatePaths.Num(), 2);
		TestFalse(TEXT("Shadowed paths exclude mounted winner."), Result.ShadowedPluginPaths.Contains(TEXT("/Repo/Plugins/UnrealMcp")));
		TestTrue(TEXT("Shadowed paths include engine copy."), Result.ShadowedPluginPaths.Contains(TEXT("/UE/Engine/Plugins/Marketplace/UnrealMcp")));
	}

	{
		TArray<UnrealMcp::FInstallDoctorPluginCandidate> Candidates;
		UnrealMcp::FInstallDoctorPluginCandidate Mounted;
		Mounted.Path = TEXT("/Repo/Plugins/UnrealMcp/");
		Mounted.bExists = true;
		Mounted.bHasPluginDescriptor = true;
		Candidates.Add(Mounted);

		UnrealMcp::FInstallDoctorPluginCandidate EquivalentMounted;
		EquivalentMounted.Path = TEXT("/Repo/Plugins/UnrealMcp");
		EquivalentMounted.bExists = true;
		EquivalentMounted.bHasPluginDescriptor = true;
		Candidates.Add(EquivalentMounted);

		UnrealMcp::FInstallDoctorPluginCandidate MissingDescriptor;
		MissingDescriptor.Path = TEXT("/UE/Engine/Plugins/UnrealMcp");
		MissingDescriptor.bExists = true;
		MissingDescriptor.bHasPluginDescriptor = false;
		Candidates.Add(MissingDescriptor);

		const UnrealMcp::FInstallDoctorDuplicateScanResult Result =
			UnrealMcp::DetectDuplicateUnrealMcpPlugins(Candidates, TEXT("/Repo/Plugins/UnrealMcp"));

		TestFalse(TEXT("Equivalent mounted paths are de-duplicated."), Result.bDuplicatePluginConflict);
		TestEqual(TEXT("Duplicate equivalent paths collapse to one candidate."), Result.CandidatePaths.Num(), 1);
		TestEqual(TEXT("Missing descriptor candidate is ignored."), Result.ShadowedPluginPaths.Num(), 0);
	}

	return true;
}

#endif
