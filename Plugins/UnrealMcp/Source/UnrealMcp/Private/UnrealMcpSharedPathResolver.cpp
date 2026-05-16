#include "UnrealMcpSharedPathResolver.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

namespace UnrealMcp
{
	namespace
	{
		FString NormalizeSharedPath(const FString& Path)
		{
			FString FullPath = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(FullPath);
			FPaths::CollapseRelativeDirectories(FullPath);
			return FullPath;
		}

		FString NormalizePurePath(FString Path)
		{
			Path.TrimStartAndEndInline();
			Path.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
			while (Path.Contains(TEXT("//")))
			{
				Path.ReplaceInline(TEXT("//"), TEXT("/"), ESearchCase::CaseSensitive);
			}
			while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
			{
				Path.LeftChopInline(1);
			}
			return Path;
		}

		FString CombinePurePath(const FString& A, const FString& B)
		{
			if (A.IsEmpty())
			{
				return NormalizePurePath(B);
			}
			if (B.IsEmpty())
			{
				return NormalizePurePath(A);
			}
			return NormalizePurePath(A + TEXT("/") + B);
		}

		FString CombinePurePath3(const FString& A, const FString& B, const FString& C)
		{
			return CombinePurePath(CombinePurePath(A, B), C);
		}

		FString GetPureParentPath(const FString& Path)
		{
			const FString CleanPath = NormalizePurePath(Path);
			const int32 SlashIndex = CleanPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (SlashIndex == INDEX_NONE)
			{
				return FString();
			}
			if (SlashIndex == 0)
			{
				return TEXT("/");
			}
			return CleanPath.Left(SlashIndex);
		}

		bool AddUniqueCandidate(TArray<FString>& Candidates, const FString& Candidate)
		{
			const FString CleanCandidate = NormalizePurePath(Candidate);
			for (const FString& Existing : Candidates)
			{
				if (Existing.Equals(CleanCandidate, ESearchCase::IgnoreCase))
				{
					return false;
				}
			}
			Candidates.Add(CleanCandidate);
			return true;
		}

		bool TryCleanToolsSubpath(const FString& ToolsSubpath, FString& OutCleanSubpath, FString& OutWarning)
		{
			FString CleanSubpath = ToolsSubpath;
			CleanSubpath.TrimStartAndEndInline();
			CleanSubpath.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
			CleanSubpath.RemoveFromStart(TEXT("Tools/"));
			if (CleanSubpath.StartsWith(TEXT("/"))
				|| CleanSubpath.StartsWith(TEXT("//"))
				|| CleanSubpath.Contains(TEXT(":")))
			{
				OutWarning = FString::Printf(TEXT("Invalid Tools subpath '%s': absolute, drive-letter, and UNC-style paths are not allowed."), *ToolsSubpath);
				return false;
			}

			TArray<FString> Parts;
			CleanSubpath.ParseIntoArray(Parts, TEXT("/"), false);
			TArray<FString> CleanParts;
			for (const FString& RawPart : Parts)
			{
				const FString Part = RawPart.TrimStartAndEnd();
				if (Part.IsEmpty() || Part == TEXT("."))
				{
					continue;
				}
				if (Part == TEXT(".."))
				{
					OutWarning = FString::Printf(TEXT("Invalid Tools subpath '%s': '..' segments are not allowed."), *ToolsSubpath);
					return false;
				}
				CleanParts.Add(Part);
			}

			OutCleanSubpath = FString::Join(CleanParts, TEXT("/"));
			return true;
		}

		void AddSharedRepoCandidates(const FString& ToolsSubpath, TArray<FString>& OutCandidateRoots)
		{
			OutCandidateRoots.Reset();

			FString CleanSubpath;
			FString Warning;
			if (!TryCleanToolsSubpath(ToolsSubpath, CleanSubpath, Warning))
			{
				return;
			}

			FToolsReadResolution PureResolution = ResolveToolsReadSubpath_Pure(
				NormalizeSharedPath(FPaths::ProjectDir()),
				ResolvePluginBaseDir().Path,
				CleanSubpath,
				[](const FString&) { return false; });
			OutCandidateRoots = PureResolution.Candidates;
		}

		void AddAncestorToolsCandidates(
			const FString& StartDir,
			const FString& CleanSubpath,
			TArray<FString>& OutCandidates)
		{
			FString AncestorDir = NormalizePurePath(StartDir);
			if (AncestorDir.IsEmpty())
			{
				return;
			}

			for (int32 CandidateIndex = 0; CandidateIndex < 9; ++CandidateIndex)
			{
				const FString Candidate = CleanSubpath.IsEmpty()
					? CombinePurePath(AncestorDir, TEXT("Tools"))
					: CombinePurePath3(AncestorDir, TEXT("Tools"), CleanSubpath);
				AddUniqueCandidate(OutCandidates, Candidate);

				const FString ParentDir = GetPureParentPath(AncestorDir);
				if (ParentDir.IsEmpty() || ParentDir.Equals(AncestorDir, ESearchCase::CaseSensitive))
				{
					break;
				}
				AncestorDir = ParentDir;
			}
		}
	}

	bool SharedRepoRootHasAny(
		const FString& Root,
		const TArray<FString>& RequiredRecursivePatterns)
	{
		if (FPaths::FileExists(Root))
		{
			if (RequiredRecursivePatterns.Num() == 0)
			{
				return true;
			}

			const FString FileName = FPaths::GetCleanFilename(Root);
			for (const FString& Pattern : RequiredRecursivePatterns)
			{
				if (FileName.MatchesWildcard(Pattern, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
			return false;
		}

		if (!FPaths::DirectoryExists(Root))
		{
			return false;
		}
		if (RequiredRecursivePatterns.Num() == 0)
		{
			return true;
		}

		for (const FString& Pattern : RequiredRecursivePatterns)
		{
			TArray<FString> Matches;
			IFileManager::Get().FindFilesRecursive(Matches, *Root, *Pattern, true, false);
			if (Matches.Num() > 0)
			{
				return true;
			}
		}
		return false;
	}

	FToolsReadResolution ResolvePluginBaseDir()
	{
		FToolsReadResolution Resolution;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMcp"));
		if (!Plugin.IsValid())
		{
			Resolution.Warning = TEXT("UnrealMcp plugin base directory could not be resolved (IPluginManager not initialized).");
			return Resolution;
		}

		FString BaseDir = Plugin->GetBaseDir();
		if (BaseDir.IsEmpty())
		{
			Resolution.Warning = TEXT("UnrealMcp plugin base directory could not be resolved (empty plugin BaseDir).");
			return Resolution;
		}
		BaseDir = FPaths::ConvertRelativePathToFull(BaseDir);
		FPaths::NormalizeDirectoryName(BaseDir);
		FPaths::CollapseRelativeDirectories(BaseDir);
		Resolution.Path = BaseDir;
		Resolution.Candidates.Add(BaseDir);
		Resolution.bFound = FPaths::DirectoryExists(BaseDir);
		Resolution.SourceKind = Resolution.bFound
			? FToolsReadResolution::ESource::PluginResources
			: FToolsReadResolution::ESource::Unresolved;
		return Resolution;
	}

	FToolsReadResolution ResolvePluginSourceRoot_Pure(
		const FString& PluginBaseDir,
		TFunctionRef<bool(const FString&)> FileOrDirExists)
	{
		FToolsReadResolution Resolution;
		const FString CleanBaseDir = NormalizePurePath(PluginBaseDir);
		if (CleanBaseDir.IsEmpty())
		{
			Resolution.Warning = TEXT("UnrealMcp plugin source directory could not be resolved (empty plugin BaseDir).");
			return Resolution;
		}

		Resolution.Path = CombinePurePath(CleanBaseDir, TEXT("Source"));
		Resolution.Candidates.Add(Resolution.Path);
		Resolution.bFound = FileOrDirExists(Resolution.Path);
		Resolution.SourceKind = Resolution.bFound
			? FToolsReadResolution::ESource::PluginResources
			: FToolsReadResolution::ESource::Unresolved;
		if (!Resolution.bFound)
		{
			Resolution.Warning = FString::Printf(TEXT("UnrealMcp plugin source directory does not exist: %s"), *Resolution.Path);
		}
		return Resolution;
	}

	FToolsReadResolution ResolvePluginSourceRoot()
	{
		const FToolsReadResolution BaseDir = ResolvePluginBaseDir();
		if (BaseDir.Path.IsEmpty())
		{
			FToolsReadResolution Resolution;
			Resolution.Warning = BaseDir.Warning;
			return Resolution;
		}

		return ResolvePluginSourceRoot_Pure(
			BaseDir.Path,
			[](const FString& Candidate)
			{
				return FPaths::DirectoryExists(Candidate);
			});
	}

	FToolsReadResolution ResolveToolsReadSubpath_Pure(
		const FString& ProjectDir,
		const FString& PluginBaseDir,
		const FString& ToolsSubpath,
		TFunctionRef<bool(const FString&)> FileOrDirExists)
	{
		FToolsReadResolution Resolution;
		FString CleanSubpath;
		if (!TryCleanToolsSubpath(ToolsSubpath, CleanSubpath, Resolution.Warning))
		{
			return Resolution;
		}

		const FString CleanProjectDir = NormalizePurePath(ProjectDir);
		const FString CleanPluginBaseDir = NormalizePurePath(PluginBaseDir);
		const FString ProjectCandidate = CleanSubpath.IsEmpty()
			? CombinePurePath(CleanProjectDir, TEXT("Tools"))
			: CombinePurePath3(CleanProjectDir, TEXT("Tools"), CleanSubpath);
		if (!CleanProjectDir.IsEmpty())
		{
			AddUniqueCandidate(Resolution.Candidates, ProjectCandidate);
		}
		AddAncestorToolsCandidates(GetPureParentPath(CleanProjectDir), CleanSubpath, Resolution.Candidates);
		AddAncestorToolsCandidates(GetPureParentPath(GetPureParentPath(CleanPluginBaseDir)), CleanSubpath, Resolution.Candidates);

		int32 FoundIndex = INDEX_NONE;
		for (int32 CandidateIndex = 0; CandidateIndex < Resolution.Candidates.Num(); ++CandidateIndex)
		{
			if (FileOrDirExists(Resolution.Candidates[CandidateIndex]))
			{
				FoundIndex = CandidateIndex;
				break;
			}
		}

		if (FoundIndex != INDEX_NONE)
		{
			Resolution.Path = Resolution.Candidates[FoundIndex];
			Resolution.bFound = true;
			Resolution.SourceKind = FoundIndex == 0
				? FToolsReadResolution::ESource::ProjectLocal
				: FToolsReadResolution::ESource::SharedRepoRoot;

			if (FoundIndex == 0)
			{
				for (int32 CandidateIndex = 1; CandidateIndex < Resolution.Candidates.Num(); ++CandidateIndex)
				{
					if (!Resolution.Candidates[CandidateIndex].Equals(Resolution.Path, ESearchCase::IgnoreCase)
						&& FileOrDirExists(Resolution.Candidates[CandidateIndex]))
					{
						Resolution.Warning = TEXT("project-local shadow exists with same name as shared canonical");
						break;
					}
				}
			}
			return Resolution;
		}

		if (Resolution.Candidates.Num() > 0)
		{
			Resolution.Path = Resolution.Candidates[0];
		}
		return Resolution;
	}

	FToolsReadResolution ResolveToolsReadSubpath(
		const FString& ToolsSubpath,
		const TArray<FString>& Sentinels)
	{
		return ResolveToolsReadSubpath_Pure(
			NormalizeSharedPath(FPaths::ProjectDir()),
			ResolvePluginBaseDir().Path,
			ToolsSubpath,
			[&Sentinels](const FString& Candidate)
			{
				return SharedRepoRootHasAny(Candidate, Sentinels);
			});
	}

	FString LexToString(FToolsReadResolution::ESource SourceKind)
	{
		switch (SourceKind)
		{
		case FToolsReadResolution::ESource::ProjectLocal:
			return TEXT("ProjectLocal");
		case FToolsReadResolution::ESource::SharedRepoRoot:
			return TEXT("SharedRepoRoot");
		case FToolsReadResolution::ESource::CanonicalStarter:
			return TEXT("CanonicalStarter");
		case FToolsReadResolution::ESource::PluginResources:
			return TEXT("PluginResources");
		case FToolsReadResolution::ESource::Unresolved:
		default:
			return TEXT("Unresolved");
		}
	}

	TArray<TSharedPtr<FJsonValue>> MakeToolsReadCandidateValues(
		const FToolsReadResolution& Resolution)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Candidate : Resolution.Candidates)
		{
			Values.Add(MakeShared<FJsonValueString>(Candidate));
		}
		return Values;
	}

	bool ResolveSharedRepoRoot(
		const FString& ToolsSubpath,
		const TArray<FString>& RequiredRecursivePatterns,
		FString& OutRoot,
		TArray<FString>& OutCandidateRoots)
	{
		OutRoot.Reset();
		AddSharedRepoCandidates(ToolsSubpath, OutCandidateRoots);

		for (const FString& Candidate : OutCandidateRoots)
		{
			if (SharedRepoRootHasAny(Candidate, RequiredRecursivePatterns))
			{
				OutRoot = Candidate;
				return true;
			}
		}

		for (const FString& Candidate : OutCandidateRoots)
		{
			if (FPaths::DirectoryExists(Candidate))
			{
				OutRoot = Candidate;
				return false;
			}
		}

		if (OutCandidateRoots.Num() > 0)
		{
			OutRoot = OutCandidateRoots[0];
		}
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> MakeSharedRepoRootCandidateValues(
		const TArray<FString>& CandidateRoots,
		const TArray<FString>& RequiredRecursivePatterns)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& CandidateRoot : CandidateRoots)
		{
			TSharedPtr<FJsonObject> CandidateObject = MakeShared<FJsonObject>();
			CandidateObject->SetStringField(TEXT("root"), CandidateRoot);
			CandidateObject->SetBoolField(TEXT("exists"), FPaths::DirectoryExists(CandidateRoot));
			CandidateObject->SetBoolField(TEXT("hasExpectedContent"), SharedRepoRootHasAny(CandidateRoot, RequiredRecursivePatterns));
			Values.Add(MakeShared<FJsonValueObject>(CandidateObject));
		}
		return Values;
	}
}
