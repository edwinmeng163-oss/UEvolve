#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UnrealMcpSelfExtensionInternal.h"
#include "UnrealMcpSharedPathResolver.h"

namespace
{
	FString TestPath(const FString& Path)
	{
		FString Normalized = Path;
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
		while (Normalized.Contains(TEXT("//")))
		{
			Normalized.ReplaceInline(TEXT("//"), TEXT("/"), ESearchCase::CaseSensitive);
		}
		while (Normalized.Len() > 1 && Normalized.EndsWith(TEXT("/")))
		{
			Normalized.LeftChopInline(1);
		}
		return Normalized;
	}

	TFunction<bool(const FString&)> MakeExistsPredicate(const TArray<FString>& ExistingPaths)
	{
		TSet<FString> Existing;
		for (const FString& ExistingPath : ExistingPaths)
		{
			Existing.Add(TestPath(ExistingPath));
		}
		return [Existing](const FString& Candidate)
		{
			return Existing.Contains(TestPath(Candidate));
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUnrealMcpToolsReadPathResolverPureTest,
	"UnrealMcp.PathResolver.ToolsReadPure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUnrealMcpToolsReadPathResolverPureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	using ESource = UnrealMcp::FToolsReadResolution::ESource;

	{
		const FString ProjectDir = TEXT("/Repo");
		const FString PluginBaseDir = TEXT("/Repo/Plugins/UnrealMcp");
		const auto Exists = MakeExistsPredicate({ TEXT("/Repo/Tools/UnrealMcpToolRegistry/tools.json") });
		const UnrealMcp::FToolsReadResolution Resolution = UnrealMcp::ResolveToolsReadSubpath_Pure(
			ProjectDir,
			PluginBaseDir,
			TEXT("UnrealMcpToolRegistry/tools.json"),
			Exists);
		TestTrue(TEXT("Root host registry resolves project-local Tools."), Resolution.bFound);
		TestTrue(TEXT("Root host registry source kind"), Resolution.SourceKind == ESource::ProjectLocal);
		TestEqual(TEXT("Root host registry path"), Resolution.Path, TEXT("/Repo/Tools/UnrealMcpToolRegistry/tools.json"));
		TestEqual(TEXT("Root host first candidate is ProjectDir Tools."), Resolution.Candidates[0], TEXT("/Repo/Tools/UnrealMcpToolRegistry/tools.json"));
	}

	{
		const auto Exists = MakeExistsPredicate({ TEXT("/Repo/Tools/UnrealMcpToolRegistry/tools.json") });
		const UnrealMcp::FToolsReadResolution Resolution = UnrealMcp::ResolveToolsReadSubpath_Pure(
			TEXT("/Repo/Examples/UEvolveExample"),
			TEXT("/Repo/Plugins/UnrealMcp"),
			TEXT("UnrealMcpToolRegistry/tools.json"),
			Exists);
		TestTrue(TEXT("Example project falls back to repo-root Tools."), Resolution.bFound);
		TestTrue(TEXT("Example fallback source kind"), Resolution.SourceKind == ESource::SharedRepoRoot);
		TestEqual(TEXT("Example fallback path"), Resolution.Path, TEXT("/Repo/Tools/UnrealMcpToolRegistry/tools.json"));
	}

	{
		const auto Exists = MakeExistsPredicate({ TEXT("/Shared/UEvolve/Tools/UnrealMcpToolRegistry/tools.json") });
		const UnrealMcp::FToolsReadResolution Resolution = UnrealMcp::ResolveToolsReadSubpath_Pure(
			TEXT("/User/CopiedGame"),
			TEXT("/Shared/UEvolve/Plugins/UnrealMcp"),
			TEXT("UnrealMcpToolRegistry/tools.json"),
			Exists);
		TestTrue(TEXT("Copied project uses plugin BaseDir anchor to find shared Tools."), Resolution.bFound);
		TestTrue(TEXT("Copied project fallback source kind"), Resolution.SourceKind == ESource::SharedRepoRoot);
		TestEqual(TEXT("Copied project fallback path"), Resolution.Path, TEXT("/Shared/UEvolve/Tools/UnrealMcpToolRegistry/tools.json"));
	}

	{
		const auto Exists = MakeExistsPredicate({
			TEXT("/Repo/Examples/UEvolveExample/Tools/UnrealMcpToolScaffolds/fps_bootstrap"),
			TEXT("/Repo/Tools/UnrealMcpToolScaffolds/fps_bootstrap")
		});
		const UnrealMcp::FToolsReadResolution Resolution = UnrealMcp::ResolveToolsReadSubpath_Pure(
			TEXT("/Repo/Examples/UEvolveExample"),
			TEXT("/Repo/Plugins/UnrealMcp"),
			TEXT("UnrealMcpToolScaffolds/fps_bootstrap"),
			Exists);
		TestTrue(TEXT("Project shadow resolves to project-local draft."), Resolution.bFound);
		TestTrue(TEXT("Project shadow source kind"), Resolution.SourceKind == ESource::ProjectLocal);
		TestTrue(TEXT("Project shadow emits warning"), Resolution.Warning.Contains(TEXT("shadow")));
	}

	{
		const auto Exists = MakeExistsPredicate({});
		const UnrealMcp::FToolsReadResolution Resolution = UnrealMcp::ResolveToolsReadSubpath_Pure(
			TEXT("/Repo/Examples/UEvolveExample"),
			TEXT("/Repo/Plugins/UnrealMcp"),
			TEXT("UnrealMcpToolScaffolds/missing_tool"),
			Exists);
		TestFalse(TEXT("Missing subpath reports not found"), Resolution.bFound);
		TestEqual(TEXT("Missing subpath diagnostic path is project candidate"), Resolution.Path, TEXT("/Repo/Examples/UEvolveExample/Tools/UnrealMcpToolScaffolds/missing_tool"));
		TestTrue(TEXT("Missing subpath source kind"), Resolution.SourceKind == ESource::Unresolved);
	}

	{
		const auto Exists = MakeExistsPredicate({
			TEXT("/Repo/Examples/UEvolveExample/Tools/UnrealMcpPyTools/editor_python_runtime_info/main.py"),
			TEXT("/Repo/Tools/UnrealMcpToolScaffolds/fps_bootstrap")
		});
		const UnrealMcp::FToolsReadResolution FileResolution = UnrealMcp::ResolveToolsReadSubpath_Pure(
			TEXT("/Repo/Examples/UEvolveExample"),
			TEXT("/Repo/Plugins/UnrealMcp"),
			TEXT("UnrealMcpPyTools/editor_python_runtime_info/main.py"),
			Exists);
		TestTrue(TEXT("File subpath resolves exactly"), FileResolution.bFound);
		TestTrue(TEXT("File subpath source kind"), FileResolution.SourceKind == ESource::ProjectLocal);

		const UnrealMcp::FToolsReadResolution DirectoryResolution = UnrealMcp::ResolveToolsReadSubpath_Pure(
			TEXT("/Repo/Examples/UEvolveExample"),
			TEXT("/Repo/Plugins/UnrealMcp"),
			TEXT("UnrealMcpToolScaffolds/fps_bootstrap"),
			Exists);
		TestTrue(TEXT("Directory subpath resolves exactly"), DirectoryResolution.bFound);
		TestTrue(TEXT("Directory subpath source kind"), DirectoryResolution.SourceKind == ESource::SharedRepoRoot);
	}

	{
		const auto Exists = MakeExistsPredicate({
			TEXT("/Repo/Tools/UnrealMcpToolScaffoldStarters/fps_bootstrap")
		});
		const UnrealMcp::FToolsReadResolution StarterResolution = UnrealMcp::ResolveScaffoldReadDirectory_Pure(
			TEXT("/Repo/Examples/UEvolveExample"),
			TEXT("/Repo/Plugins/UnrealMcp"),
			TEXT("unreal.fps.bootstrap"),
			Exists);
		TestTrue(TEXT("Missing working scaffold falls back to canonical starter."), StarterResolution.bFound);
		TestTrue(TEXT("Starter fallback source kind"), StarterResolution.SourceKind == ESource::CanonicalStarter);
		TestEqual(TEXT("Starter fallback path"), StarterResolution.Path, TEXT("/Repo/Tools/UnrealMcpToolScaffoldStarters/fps_bootstrap"));
		TestEqual(TEXT("Starter fallback preserves working-copy candidate first"), StarterResolution.Candidates[0], TEXT("/Repo/Examples/UEvolveExample/Tools/UnrealMcpToolScaffolds/fps_bootstrap"));
		TestTrue(TEXT("Starter fallback candidate list includes starter tree"), StarterResolution.Candidates.Contains(TEXT("/Repo/Tools/UnrealMcpToolScaffoldStarters/fps_bootstrap")));
	}

	{
		const auto Exists = MakeExistsPredicate({});
		const TArray<FString> InvalidSubpaths = {
			TEXT("../UnrealMcpToolRegistry/tools.json"),
			TEXT("/abs/UnrealMcpToolRegistry/tools.json"),
			TEXT("C:/UEvolve/Tools/UnrealMcpToolRegistry/tools.json"),
			TEXT("//server/share/Tools/UnrealMcpToolRegistry/tools.json")
		};
		for (const FString& InvalidSubpath : InvalidSubpaths)
		{
			const UnrealMcp::FToolsReadResolution Resolution = UnrealMcp::ResolveToolsReadSubpath_Pure(
				TEXT("/Repo"),
				TEXT("/Repo/Plugins/UnrealMcp"),
				InvalidSubpath,
				Exists);
			TestFalse(FString::Printf(TEXT("Invalid subpath rejected: %s"), *InvalidSubpath), Resolution.bFound);
			TestTrue(FString::Printf(TEXT("Invalid subpath has warning: %s"), *InvalidSubpath), !Resolution.Warning.IsEmpty());
			TestEqual(FString::Printf(TEXT("Invalid subpath has no diagnostic path: %s"), *InvalidSubpath), Resolution.Path, FString());
		}
	}

	{
		const auto SourceExists = MakeExistsPredicate({ TEXT("/Repo/Plugins/UnrealMcp/Source") });
		const UnrealMcp::FToolsReadResolution SourceResolution = UnrealMcp::ResolvePluginSourceRoot_Pure(
			TEXT("/Repo/Plugins/UnrealMcp"),
			SourceExists);
		TestTrue(TEXT("Plugin source root reports found when Source exists."), SourceResolution.bFound);
		TestEqual(TEXT("Plugin source root path"), SourceResolution.Path, TEXT("/Repo/Plugins/UnrealMcp/Source"));
		TestTrue(TEXT("Plugin source root source kind"), SourceResolution.SourceKind == ESource::PluginResources);

		const auto SourceMissing = MakeExistsPredicate({});
		const UnrealMcp::FToolsReadResolution MissingSourceResolution = UnrealMcp::ResolvePluginSourceRoot_Pure(
			TEXT("/Packaged/Plugins/UnrealMcp"),
			SourceMissing);
		TestFalse(TEXT("Packaged plugin without Source reports not found."), MissingSourceResolution.bFound);
		TestEqual(TEXT("Packaged plugin source root still reports expected path"), MissingSourceResolution.Path, TEXT("/Packaged/Plugins/UnrealMcp/Source"));
		TestTrue(TEXT("Packaged plugin source root source kind"), MissingSourceResolution.SourceKind == ESource::Unresolved);
	}

	FString OutputRoot;
	FString FailureReason;
	TestTrue(TEXT("ResolveProjectOutputDirectory remains project-local"), UnrealMcp::ResolveProjectOutputDirectory(FString(), OutputRoot, FailureReason));
	FString ExpectedOutputRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpToolScaffolds")));
	FPaths::NormalizeDirectoryName(ExpectedOutputRoot);
	FPaths::CollapseRelativeDirectories(ExpectedOutputRoot);
	TestEqual(TEXT("Default scaffold writer root is ProjectDir Tools/UnrealMcpToolScaffolds"), OutputRoot, ExpectedOutputRoot);

	return true;
}

#endif
