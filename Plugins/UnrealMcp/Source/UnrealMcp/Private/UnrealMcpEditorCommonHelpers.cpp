#include "UnrealMcpModule.h"

#include "Editor.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Paths.h"

namespace UnrealMcp
{
		IPythonScriptPlugin* LoadPythonScriptPlugin()
		{
			static const FName PythonScriptPluginModuleName(TEXT("PythonScriptPlugin"));
			if (IPythonScriptPlugin* PythonPlugin = FModuleManager::GetModulePtr<IPythonScriptPlugin>(PythonScriptPluginModuleName))
			{
				return PythonPlugin;
			}

			return FModuleManager::LoadModulePtr<IPythonScriptPlugin>(PythonScriptPluginModuleName);
		}

		bool TryParsePythonFileExecutionScope(const FString& ScopeString, EPythonFileExecutionScope& OutScope)
		{
			if (ScopeString.Equals(TEXT("private"), ESearchCase::IgnoreCase))
			{
				OutScope = EPythonFileExecutionScope::Private;
				return true;
			}

			if (ScopeString.Equals(TEXT("public"), ESearchCase::IgnoreCase))
			{
				OutScope = EPythonFileExecutionScope::Public;
				return true;
			}

			return false;
		}

		FString QuoteShellArgument(const FString& Value)
		{
			FString Escaped = Value;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));

			const bool bNeedsQuotes = Escaped.Contains(TEXT(" "))
				|| Escaped.Contains(TEXT("\t"))
				|| Escaped.Contains(TEXT("\""));

			return bNeedsQuotes
				? FString::Printf(TEXT("\"%s\""), *Escaped)
				: Escaped;
		}

		bool ResolvePythonScriptPath(
			const FString& RequestedPath,
			bool bAllowOutsideProject,
			FString& OutResolvedPath,
			FString& OutFailureReason)
		{
			OutResolvedPath.Reset();
			OutFailureReason.Reset();

			const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				OutFailureReason = TEXT("The scriptPath argument is required.");
				return false;
			}

			FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeDirectoryName(ProjectDir);
			FPaths::CollapseRelativeDirectories(ProjectDir);

			FString ResolvedPath = FPaths::IsRelative(TrimmedPath)
				? FPaths::Combine(ProjectDir, TrimmedPath)
				: TrimmedPath;
			ResolvedPath = FPaths::ConvertRelativePathToFull(ResolvedPath);
			FPaths::NormalizeFilename(ResolvedPath);
			FPaths::CollapseRelativeDirectories(ResolvedPath);

			if (!ResolvedPath.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(TEXT("scriptPath must point to a .py file. Received '%s'."), *ResolvedPath);
				return false;
			}

			if (!FPaths::FileExists(ResolvedPath))
			{
				OutFailureReason = FString::Printf(TEXT("Python script file does not exist: %s"), *ResolvedPath);
				return false;
			}

			const FString ProjectDirPrefix = ProjectDir.EndsWith(TEXT("/")) ? ProjectDir : ProjectDir + TEXT("/");
			if (!bAllowOutsideProject && !ResolvedPath.StartsWith(ProjectDirPrefix, ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(
					TEXT("scriptPath '%s' is outside the project directory '%s'. Set allowOutsideProject=true to override."),
					*ResolvedPath,
					*ProjectDir);
				return false;
			}

			OutResolvedPath = ResolvedPath;
			return true;
		}

		UWorld* ResolveConsoleWorld(const FString& RequestedTarget, FString& OutResolvedTarget, FString& OutFailureReason)
		{
		OutResolvedTarget = TEXT("editor");
		OutFailureReason.Reset();

		if (!GEditor)
		{
			OutFailureReason = TEXT("GEditor is unavailable.");
			return nullptr;
		}

		const FString NormalizedTarget = RequestedTarget.TrimStartAndEnd().ToLower();
		const bool bIsAuto = NormalizedTarget.IsEmpty() || NormalizedTarget == TEXT("auto");
		const bool bWantsPie = NormalizedTarget == TEXT("pie");
		const bool bWantsEditor = NormalizedTarget == TEXT("editor");

		if (!bIsAuto && !bWantsPie && !bWantsEditor)
		{
			OutFailureReason = FString::Printf(TEXT("Unknown console target '%s'. Use auto, editor, or pie."), *RequestedTarget);
			return nullptr;
		}

		if ((bIsAuto || bWantsPie) && GEditor->PlayWorld != nullptr)
		{
			OutResolvedTarget = TEXT("pie");
			return GEditor->PlayWorld;
		}

		if (bWantsPie)
		{
			OutFailureReason = TEXT("No PIE world is currently running.");
			return nullptr;
		}

		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			OutResolvedTarget = TEXT("editor");
			return EditorWorld;
		}

		OutFailureReason = TEXT("The editor world is unavailable.");
		return nullptr;
	}

}
