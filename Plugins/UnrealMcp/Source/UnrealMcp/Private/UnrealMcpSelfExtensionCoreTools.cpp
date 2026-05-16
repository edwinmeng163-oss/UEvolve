#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealMcpSharedPathResolver.h"

namespace UnrealMcp
{
		FString HashTextForManifest(const FString& Text)
		{
			return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Text));
		}

		FString GetMcpModuleSourcePath()
		{
			const FToolsReadResolution PluginSourceDirectory = ResolvePluginSourceRoot();
			return !PluginSourceDirectory.bFound
				? FString()
				: FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginSourceDirectory.Path, TEXT("UnrealMcp/Private/UnrealMcpModule.cpp")));
		}

		FString GetMcpExtensionBackupRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ExtensionBackups")));
		}

		FString GetUnrealMcpSavedRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp")));
		}

		FString GetMcpBuildLogRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/BuildLogs")));
		}

		FString GetLatestMcpExtensionManifestPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/LastExtensionApply.json")));
		}

		FString GetMcpExtensionLockPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ExtensionSession.lock")));
		}

		FString GetMcpProjectStateBackupRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ProjectStateBackups")));
		}

		FString GetMcpModuleHeaderPath()
		{
			const FToolsReadResolution PluginSourceDirectory = ResolvePluginSourceRoot();
			return !PluginSourceDirectory.bFound
				? FString()
				: FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginSourceDirectory.Path, TEXT("UnrealMcp/Public/UnrealMcpModule.h")));
		}

		FString GetProjectReadmePath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("README.md")));
		}

		FString GetPluginReadmePath()
		{
			const FToolsReadResolution PluginBaseDir = ResolvePluginBaseDir();
			return PluginBaseDir.Path.IsEmpty()
				? FString()
				: FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginBaseDir.Path, TEXT("README.md")));
		}

		bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutFailureReason)
		{
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to read JSON file '%s'."), *FilePath);
				return false;
			}

			if (!LoadJsonObject(JsonText, OutObject) || !OutObject.IsValid())
			{
				OutFailureReason = FString::Printf(TEXT("JSON file '%s' is not a valid object."), *FilePath);
				return false;
			}

			return true;
		}

		bool SaveJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& FilePath, FString& OutFailureReason)
		{
			if (!Object.IsValid())
			{
				OutFailureReason = TEXT("Cannot save an invalid JSON object.");
				return false;
			}

			const FString Directory = FPaths::GetPath(FilePath);
			if (!IFileManager::Get().MakeDirectory(*Directory, true))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to create directory '%s'."), *Directory);
				return false;
			}

			FString JsonText;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
			if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
			{
				OutFailureReason = TEXT("Failed to serialize JSON object.");
				return false;
			}

			if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to write '%s'."), *FilePath);
				return false;
			}
			return true;
		}

		bool ResolveProjectPathInsideProject(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason)
		{
			FString TrimmedPath = RequestedPath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				OutFailureReason = TEXT("Path must not be empty.");
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

			const FString ProjectDirPrefix = ProjectDir.EndsWith(TEXT("/")) ? ProjectDir : ProjectDir + TEXT("/");
			if (!ResolvedPath.Equals(ProjectDir, ESearchCase::IgnoreCase)
				&& !ResolvedPath.StartsWith(ProjectDirPrefix, ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(TEXT("Path '%s' resolves outside project directory '%s'."), *ResolvedPath, *ProjectDir);
				return false;
			}

			OutPath = ResolvedPath;
			return true;
		}

		bool ResolveMcpScaffoldDirectory(
			const FJsonObject& Arguments,
			FString& OutDirectory,
			FString& OutToolName,
			FString& OutFailureReason,
			FToolsReadResolution* OutResolution)
		{
			if (OutResolution)
			{
				*OutResolution = FToolsReadResolution();
			}

			FString ScaffoldDir;
			FString ToolName;
			FString OutputRoot;
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
			ToolName = ToolName.TrimStartAndEnd();

			if (!ScaffoldDir.TrimStartAndEnd().IsEmpty())
			{
				if (!ResolveProjectPathInsideProject(ScaffoldDir, OutDirectory, OutFailureReason))
				{
					return false;
				}
				if (OutResolution)
				{
					OutResolution->Path = OutDirectory;
					OutResolution->bFound = FPaths::DirectoryExists(OutDirectory);
					OutResolution->SourceKind = FToolsReadResolution::ESource::ProjectLocal;
					OutResolution->Candidates.Add(OutDirectory);
					OutResolution->Warning = TEXT("Explicit scaffoldDir is project-local only; pass toolName with scaffoldDir empty to use shared repo recipe fallback.");
				}
			}
			else
			{
				if (ToolName.IsEmpty())
				{
					OutFailureReason = TEXT("Provide either scaffoldDir or toolName.");
					return false;
				}

				if (OutputRoot.TrimStartAndEnd().IsEmpty())
				{
					if (!ResolveScaffoldReadDirectory(SanitizeMcpToolIdForPath(ToolName), OutDirectory, OutFailureReason, OutResolution))
					{
						return false;
					}
				}
				else
				{
					FString ResolvedOutputRoot;
					if (!ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, OutFailureReason))
					{
						return false;
					}
					OutDirectory = FPaths::Combine(ResolvedOutputRoot, SanitizeMcpToolIdForPath(ToolName));
					if (OutResolution)
					{
						OutResolution->Path = OutDirectory;
						OutResolution->bFound = FPaths::DirectoryExists(OutDirectory);
						OutResolution->SourceKind = FToolsReadResolution::ESource::ProjectLocal;
						OutResolution->Candidates.Add(OutDirectory);
						OutResolution->Warning = TEXT("Explicit outputRoot is project-local only; omit outputRoot to use shared repo recipe fallback.");
					}
				}
			}

			FString TestRequestText;
			const FString TestRequestPath = FPaths::Combine(OutDirectory, TEXT("TestRequest.json"));
			if (!ToolName.IsEmpty())
			{
				OutToolName = ToolName;
				return true;
			}

			if (FFileHelper::LoadFileToString(TestRequestText, *TestRequestPath))
			{
				TSharedPtr<FJsonObject> TestRequestObject;
				if (LoadJsonObject(TestRequestText, TestRequestObject) && TestRequestObject.IsValid())
				{
					const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
					if (TestRequestObject->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject && (*ParamsObject).IsValid())
					{
						(*ParamsObject)->TryGetStringField(TEXT("name"), ToolName);
					}
				}
			}

			if (ToolName.TrimStartAndEnd().IsEmpty())
			{
				OutFailureReason = FString::Printf(TEXT("Unable to determine toolName. Provide toolName or include %s."), *TestRequestPath);
				return false;
			}

			OutToolName = ToolName.TrimStartAndEnd();
			return true;
		}

}
