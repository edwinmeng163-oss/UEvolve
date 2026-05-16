#pragma once

#include "CoreMinimal.h"

class FJsonValue;

namespace UnrealMcp
{
	struct FToolsReadResolution
	{
		enum class ESource : uint8
		{
			Unresolved,
			ProjectLocal,
			SharedRepoRoot,
			CanonicalStarter,
			PluginResources
		};

		FString Path;
		bool bFound = false;
		ESource SourceKind = ESource::Unresolved;
		TArray<FString> Candidates;
		FString Warning;
	};

	bool ResolveSharedRepoRoot(
		const FString& ToolsSubpath,
		const TArray<FString>& RequiredRecursivePatterns,
		FString& OutRoot,
		TArray<FString>& OutCandidateRoots);
	FToolsReadResolution ResolvePluginSourceRoot();
	FToolsReadResolution ResolvePluginSourceRoot_Pure(
		const FString& PluginBaseDir,
		TFunctionRef<bool(const FString&)> FileOrDirExists);
	FToolsReadResolution ResolvePluginBaseDir();
	FToolsReadResolution ResolveToolsReadSubpath(
		const FString& ToolsSubpath,
		const TArray<FString>& Sentinels);
	FToolsReadResolution ResolveToolsReadSubpath_Pure(
		const FString& ProjectDir,
		const FString& PluginBaseDir,
		const FString& ToolsSubpath,
		TFunctionRef<bool(const FString&)> FileOrDirExists);
	FString LexToString(FToolsReadResolution::ESource SourceKind);
	TArray<TSharedPtr<FJsonValue>> MakeToolsReadCandidateValues(
		const FToolsReadResolution& Resolution);
	bool SharedRepoRootHasAny(
		const FString& Root,
		const TArray<FString>& RequiredRecursivePatterns);
	TArray<TSharedPtr<FJsonValue>> MakeSharedRepoRootCandidateValues(
		const TArray<FString>& CandidateRoots,
		const TArray<FString>& RequiredRecursivePatterns);
}
