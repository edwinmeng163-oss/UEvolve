#include "UnrealMcpInstallDoctor.h"

#include "UnrealMcpSelfExtensionInternal.h"
#include "UnrealMcpSharedPathResolver.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace UnrealMcp
{
	namespace
	{
		constexpr double InstallDoctorCacheTtlSeconds = 300.0;
		const TCHAR* InstallDoctorValidatorVersion = TEXT("0.15.0-c5-1b");

		struct FInstallDoctorCheck
		{
			FString Id;
			FString Severity;
			FString Summary;
			TSharedPtr<FJsonObject> Details;
			FString RecommendedFix;
		};

		struct FInstallDoctorContext
		{
			FString ProjectDir;
			FString ProjectSavedDir;
			FString EngineDir;
			FString PluginBaseDir;
			FString MountedPluginPath;
			FString RegistryCanonicalPath;
			bool bRegistryCanonicalFound = false;
			FToolsReadResolution RegistryResolution;
			FString SchemaToolsPath;
			bool bSchemaToolsFound = false;
			FToolsReadResolution SchemaToolsResolution;
			FString SchemaProjectAliasPath;
			bool bSchemaProjectAliasFound = false;
			FString PythonPluginBaseDir;
			bool bPythonPluginFound = false;
			bool bPythonScriptPluginEnabled = false;
		};

		enum class EInstallDoctorPortState : uint8
		{
			NotChecked,
			Reachable,
			ConnectionRefused,
			TimedOut,
			Error
		};

		FString LexToString(EInstallDoctorPortState State)
		{
			switch (State)
			{
			case EInstallDoctorPortState::NotChecked:
				return TEXT("not_checked");
			case EInstallDoctorPortState::Reachable:
				return TEXT("reachable");
			case EInstallDoctorPortState::ConnectionRefused:
				return TEXT("connection_refused");
			case EInstallDoctorPortState::TimedOut:
				return TEXT("timed_out");
			case EInstallDoctorPortState::Error:
			default:
				return TEXT("error");
			}
		}

		FString NormalizeDoctorPath(const FString& Path)
		{
			FString Normalized = Path.TrimStartAndEnd();
			Normalized.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
			FPaths::NormalizeDirectoryName(Normalized);
			FPaths::CollapseRelativeDirectories(Normalized);
			return Normalized;
		}

		FString NormalizeDoctorPathKey(const FString& Path)
		{
			FString Key = NormalizeDoctorPath(Path);
#if PLATFORM_WINDOWS
			Key.ToLowerInline();
#endif
			return Key;
		}

		FString GetInstallDoctorLatestPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("InstallDoctor/latest.json")));
		}

		TSharedPtr<FJsonObject> MakeCheckDetails()
		{
			return MakeShared<FJsonObject>();
		}

		FInstallDoctorCheck MakeCheck(
			const FString& Id,
			const FString& Severity,
			const FString& Summary,
			const TSharedPtr<FJsonObject>& Details = nullptr,
			const FString& RecommendedFix = FString())
		{
			FInstallDoctorCheck Check;
			Check.Id = Id;
			Check.Severity = Severity;
			Check.Summary = Summary;
			Check.Details = Details;
			Check.RecommendedFix = RecommendedFix;
			return Check;
		}

		TSharedPtr<FJsonObject> MakeCheckObject(const FInstallDoctorCheck& Check, bool bIncludeDetails)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), Check.Id);
			Object->SetStringField(TEXT("severity"), Check.Severity);
			Object->SetStringField(TEXT("summary"), Check.Summary);
			if (bIncludeDetails && Check.Details.IsValid())
			{
				Object->SetObjectField(TEXT("details"), Check.Details);
			}
			if (bIncludeDetails && !Check.RecommendedFix.IsEmpty())
			{
				Object->SetStringField(TEXT("recommendedFix"), Check.RecommendedFix);
			}
			return Object;
		}

		TArray<TSharedPtr<FJsonValue>> MakeCheckValues(const TArray<FInstallDoctorCheck>& Checks, bool bIncludeDetails)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FInstallDoctorCheck& Check : Checks)
			{
				Values.Add(MakeShared<FJsonValueObject>(MakeCheckObject(Check, bIncludeDetails)));
			}
			return Values;
		}

		TSharedPtr<FJsonObject> BuildDoctorResult(const TArray<FInstallDoctorCheck>& Checks, bool bIncludeDetails)
		{
			int32 BlockingIssueCount = 0;
			int32 WarningCount = 0;
			for (const FInstallDoctorCheck& Check : Checks)
			{
				if (Check.Severity == TEXT("error"))
				{
					++BlockingIssueCount;
				}
				else if (Check.Severity == TEXT("warning"))
				{
					++WarningCount;
				}
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("mode"), TEXT("editor-runtime"));
			Result->SetStringField(TEXT("scope"), TEXT("editor"));
			Result->SetStringField(TEXT("overallSeverity"), BlockingIssueCount > 0 ? TEXT("error") : (WarningCount > 0 ? TEXT("warning") : TEXT("pass")));
			Result->SetNumberField(TEXT("blockingIssueCount"), BlockingIssueCount);
			Result->SetNumberField(TEXT("warningCount"), WarningCount);
			Result->SetStringField(TEXT("lastRunUtc"), FDateTime::UtcNow().ToIso8601());
			Result->SetStringField(TEXT("validatorVersion"), InstallDoctorValidatorVersion);
			Result->SetArrayField(TEXT("checks"), MakeCheckValues(Checks, bIncludeDetails));
			return Result;
		}

		TSharedPtr<FJsonObject> FilterDoctorResult(const TSharedPtr<FJsonObject>& Source, bool bIncludeDetails)
		{
			if (!Source.IsValid())
			{
				return MakeShared<FJsonObject>();
			}

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			FString StringValue;
			double NumberValue = 0.0;
			if (Source->TryGetStringField(TEXT("mode"), StringValue))
			{
				Result->SetStringField(TEXT("mode"), StringValue);
			}
			if (Source->TryGetStringField(TEXT("scope"), StringValue))
			{
				Result->SetStringField(TEXT("scope"), StringValue);
			}
			if (Source->TryGetStringField(TEXT("overallSeverity"), StringValue))
			{
				Result->SetStringField(TEXT("overallSeverity"), StringValue);
			}
			if (Source->TryGetNumberField(TEXT("blockingIssueCount"), NumberValue))
			{
				Result->SetNumberField(TEXT("blockingIssueCount"), NumberValue);
			}
			if (Source->TryGetNumberField(TEXT("warningCount"), NumberValue))
			{
				Result->SetNumberField(TEXT("warningCount"), NumberValue);
			}
			if (Source->TryGetStringField(TEXT("lastRunUtc"), StringValue))
			{
				Result->SetStringField(TEXT("lastRunUtc"), StringValue);
			}
			if (Source->TryGetStringField(TEXT("validatorVersion"), StringValue))
			{
				Result->SetStringField(TEXT("validatorVersion"), StringValue);
			}

			TArray<TSharedPtr<FJsonValue>> FilteredChecks;
			const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
			if (Source->TryGetArrayField(TEXT("checks"), Checks) && Checks)
			{
				for (const TSharedPtr<FJsonValue>& CheckValue : *Checks)
				{
					TSharedPtr<FJsonObject> CheckObject = CheckValue.IsValid() ? CheckValue->AsObject() : nullptr;
					if (!CheckObject.IsValid())
					{
						continue;
					}

					TSharedPtr<FJsonObject> FilteredCheck = MakeShared<FJsonObject>();
					if (CheckObject->TryGetStringField(TEXT("id"), StringValue))
					{
						FilteredCheck->SetStringField(TEXT("id"), StringValue);
					}
					if (CheckObject->TryGetStringField(TEXT("severity"), StringValue))
					{
						FilteredCheck->SetStringField(TEXT("severity"), StringValue);
					}
					if (CheckObject->TryGetStringField(TEXT("summary"), StringValue))
					{
						FilteredCheck->SetStringField(TEXT("summary"), StringValue);
					}
					if (bIncludeDetails)
					{
						const TSharedPtr<FJsonObject>* Details = nullptr;
						if (CheckObject->TryGetObjectField(TEXT("details"), Details) && Details && (*Details).IsValid())
						{
							FilteredCheck->SetObjectField(TEXT("details"), *Details);
						}
						if (CheckObject->TryGetStringField(TEXT("recommendedFix"), StringValue))
						{
							FilteredCheck->SetStringField(TEXT("recommendedFix"), StringValue);
						}
					}
					FilteredChecks.Add(MakeShared<FJsonValueObject>(FilteredCheck));
				}
			}
			Result->SetArrayField(TEXT("checks"), FilteredChecks);
			return Result;
		}

		bool IsCacheRecent(const TSharedPtr<FJsonObject>& CachedResult)
		{
			if (!CachedResult.IsValid())
			{
				return false;
			}

			FString LastRunUtc;
			FDateTime LastRun;
			if (!CachedResult->TryGetStringField(TEXT("lastRunUtc"), LastRunUtc)
				|| !FDateTime::ParseIso8601(*LastRunUtc, LastRun))
			{
				return false;
			}

			return (FDateTime::UtcNow() - LastRun).GetTotalSeconds() <= InstallDoctorCacheTtlSeconds;
		}

		bool ByteCompareFiles(const FString& LeftPath, const FString& RightPath)
		{
			TArray<uint8> LeftBytes;
			TArray<uint8> RightBytes;
			return FFileHelper::LoadFileToArray(LeftBytes, *LeftPath)
				&& FFileHelper::LoadFileToArray(RightBytes, *RightPath)
				&& LeftBytes == RightBytes;
		}

		bool LooksLikeSymlinkStubTarget(const FString& Line)
		{
			if (Line.StartsWith(TEXT("/")) || Line.StartsWith(TEXT("./")) || Line.StartsWith(TEXT("../")))
			{
				return true;
			}
			return Line.Len() >= 3
				&& FChar::IsAlpha(Line[0])
				&& Line[1] == TEXT(':')
				&& (Line[2] == TEXT('/') || Line[2] == TEXT('\\'));
		}

		bool IsFilesystemSymlink(const FString& Path)
		{
			return FPlatformFileManager::Get().GetPlatformFile().IsSymlink(*Path) == ESymlinkResult::Symlink;
		}

		bool TryDetectSymlinkStub(const FString& Path, TSharedPtr<FJsonObject>& OutDetails)
		{
			const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
			if (!Stat.bIsValid || Stat.FileSize > 200)
			{
				return false;
			}

			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return false;
			}

			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			TArray<FString> NonEmptyLines;
			for (FString Line : Lines)
			{
				Line = Line.TrimStartAndEnd();
				if (!Line.IsEmpty())
				{
					NonEmptyLines.Add(Line);
				}
			}
			if (NonEmptyLines.Num() != 1 || !LooksLikeSymlinkStubTarget(NonEmptyLines[0]))
			{
				return false;
			}

			OutDetails = MakeShared<FJsonObject>();
			OutDetails->SetNumberField(TEXT("size"), static_cast<double>(Stat.FileSize));
			OutDetails->SetStringField(TEXT("targetLine"), NonEmptyLines[0]);
			return true;
		}

		bool ValidateKnownTextFormat(const FString& Path, FString& OutReason)
		{
			const FString Extension = FPaths::GetExtension(Path, true).ToLower();
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				OutReason = TEXT("Failed to read known-format text file.");
				return false;
			}

			if (Extension == TEXT(".json") || Extension == TEXT(".uplugin") || Extension == TEXT(".uproject"))
			{
				TSharedPtr<FJsonValue> Value;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
				if (!FJsonSerializer::Deserialize(Reader, Value) || !Value.IsValid() || (Value->Type != EJson::Object && Value->Type != EJson::Array))
				{
					OutReason = TEXT("JSON-like file must parse to an object or array.");
					return false;
				}
				return true;
			}

			if (Extension == TEXT(".md"))
			{
				if (Text.TrimStartAndEnd().IsEmpty())
				{
					OutReason = TEXT("Markdown file is empty.");
					return false;
				}
				return true;
			}

			if (Extension == TEXT(".yaml") || Extension == TEXT(".yml"))
			{
				if (!Text.Contains(TEXT(":")))
				{
					OutReason = TEXT("YAML-like file has no key delimiter.");
					return false;
				}
				return true;
			}

			if (Extension == TEXT(".cpp") || Extension == TEXT(".h") || Extension == TEXT(".ini"))
			{
				if (Text.TrimStartAndEnd().IsEmpty())
				{
					OutReason = TEXT("Source/config file is empty.");
					return false;
				}
				if ((Extension == TEXT(".cpp") || Extension == TEXT(".h"))
					&& !Text.Contains(TEXT("#"))
					&& !Text.Contains(TEXT("{"))
					&& !Text.Contains(TEXT(";")))
				{
					OutReason = TEXT("C++ file lacks expected source tokens.");
					return false;
				}
				if (Extension == TEXT(".ini") && !Text.Contains(TEXT("[")) && !Text.Contains(TEXT("=")))
				{
					OutReason = TEXT("INI file lacks expected section or assignment syntax.");
					return false;
				}
				return true;
			}

			return true;
		}

		bool ValidateCriticalRealFile(const FString& Path, TSharedPtr<FJsonObject>& OutIssue)
		{
			if (!FPaths::FileExists(Path))
			{
				return false;
			}

			if (IsFilesystemSymlink(Path))
			{
				OutIssue = MakeShared<FJsonObject>();
				OutIssue->SetStringField(TEXT("path"), Path);
				OutIssue->SetStringField(TEXT("reason"), TEXT("filesystem symlink"));
				return true;
			}

			TSharedPtr<FJsonObject> StubDetails;
			if (TryDetectSymlinkStub(Path, StubDetails))
			{
				OutIssue = StubDetails.IsValid() ? StubDetails : MakeShared<FJsonObject>();
				OutIssue->SetStringField(TEXT("path"), Path);
				OutIssue->SetStringField(TEXT("reason"), TEXT("windows symlink stub"));
				return true;
			}

			FString FormatReason;
			if (!ValidateKnownTextFormat(Path, FormatReason))
			{
				OutIssue = MakeShared<FJsonObject>();
				OutIssue->SetStringField(TEXT("path"), Path);
				OutIssue->SetStringField(TEXT("reason"), FormatReason);
				return true;
			}

			return false;
		}

		TArray<TSharedPtr<FJsonValue>> MakeIssueValues(const TArray<TSharedPtr<FJsonObject>>& Issues)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const TSharedPtr<FJsonObject>& Issue : Issues)
			{
				if (Issue.IsValid())
				{
					Values.Add(MakeShared<FJsonValueObject>(Issue));
				}
			}
			return Values;
		}

		FString MakeRepoRootFromToolsPath(const FString& ToolsPath)
		{
			FString Root = FPaths::GetPath(ToolsPath);
			Root = FPaths::GetPath(Root);
			Root = FPaths::GetPath(Root);
			return NormalizeDoctorPath(Root);
		}

		void AddExistingCriticalFile(TArray<FString>& Files, const FString& Path)
		{
			if (!Path.IsEmpty() && FPaths::FileExists(Path))
			{
				Files.AddUnique(NormalizeDoctorPath(Path));
			}
		}

		TArray<FString> BuildCriticalRealFileList(const FInstallDoctorContext& Context)
		{
			TArray<FString> Files;
			AddExistingCriticalFile(Files, Context.RegistryCanonicalPath);
			AddExistingCriticalFile(Files, FPaths::Combine(Context.PluginBaseDir, TEXT("Resources/ToolRegistry/tools.json")));
			AddExistingCriticalFile(Files, Context.SchemaToolsPath);
			AddExistingCriticalFile(Files, Context.SchemaProjectAliasPath);
			AddExistingCriticalFile(Files, FPaths::Combine(Context.PluginBaseDir, TEXT("Resources/ToolRegistry/schema.json")));
			AddExistingCriticalFile(Files, FPaths::Combine(Context.PluginBaseDir, TEXT("UnrealMcp.uplugin")));
			AddExistingCriticalFile(Files, FPaths::Combine(Context.PluginBaseDir, TEXT("Source/UnrealMcp/Private/UnrealMcpToolRegistrar.cpp")));
			AddExistingCriticalFile(Files, FPaths::Combine(Context.PluginBaseDir, TEXT("Source/UnrealMcp/Private/UnrealMcpModule.cpp")));
			AddExistingCriticalFile(Files, FPaths::Combine(Context.PluginBaseDir, TEXT("Source/UnrealMcp/Private/UnrealMcpInstallDoctor.cpp")));
			AddExistingCriticalFile(Files, FPaths::Combine(Context.PluginBaseDir, TEXT("README.md")));
			if (!Context.RegistryCanonicalPath.IsEmpty())
			{
				const FString RepoRoot = MakeRepoRootFromToolsPath(Context.RegistryCanonicalPath);
				AddExistingCriticalFile(Files, FPaths::Combine(RepoRoot, TEXT("README.md")));
				AddExistingCriticalFile(Files, FPaths::Combine(RepoRoot, TEXT("Docs/DeploymentTroubleshooting.md")));
				AddExistingCriticalFile(Files, FPaths::Combine(RepoRoot, TEXT("AGENTS.md")));
			}
			return Files;
		}

		bool FindGitRootFromCandidate(const FString& CandidatePath, FString& OutGitRoot)
		{
			FString Current = NormalizeDoctorPath(CandidatePath);
			if (FPaths::FileExists(Current))
			{
				Current = FPaths::GetPath(Current);
			}

			while (!Current.IsEmpty())
			{
				if (FPaths::DirectoryExists(FPaths::Combine(Current, TEXT(".git")))
					|| FPaths::FileExists(FPaths::Combine(Current, TEXT(".git"))))
				{
					OutGitRoot = Current;
					return true;
				}

				const FString Parent = FPaths::GetPath(Current);
				if (Parent.IsEmpty() || Parent == Current)
				{
					break;
				}
				Current = Parent;
			}
			return false;
		}

		bool FindGitRoot(const FInstallDoctorContext& Context, FString& OutGitRoot)
		{
			TArray<FString> Candidates;
			Candidates.Add(Context.ProjectDir);
			Candidates.Add(Context.PluginBaseDir);
			Candidates.Add(Context.RegistryCanonicalPath);
			for (const FString& Candidate : Candidates)
			{
				if (!Candidate.IsEmpty() && FindGitRootFromCandidate(Candidate, OutGitRoot))
				{
					return true;
				}
			}
			return false;
		}

		EInstallDoctorPortState ConnectLocalhostPort(int32 Port, int32 TimeoutMs)
		{
			ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
			if (!SocketSubsystem)
			{
				return EInstallDoctorPortState::NotChecked;
			}

			bool bIsValidAddress = false;
			const TSharedRef<FInternetAddr> LocalhostAddress = SocketSubsystem->CreateInternetAddr();
			LocalhostAddress->SetIp(TEXT("127.0.0.1"), bIsValidAddress);
			LocalhostAddress->SetPort(Port);
			if (!bIsValidAddress)
			{
				return EInstallDoctorPortState::Error;
			}

			FSocket* Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMcpInstallDoctorPortProbe"), false);
			if (!Socket)
			{
				return EInstallDoctorPortState::NotChecked;
			}

			Socket->SetNonBlocking(true);
			const bool bConnectStarted = Socket->Connect(*LocalhostAddress);
			if (bConnectStarted && Socket->GetConnectionState() == SCS_Connected)
			{
				SocketSubsystem->DestroySocket(Socket);
				return EInstallDoctorPortState::Reachable;
			}

			ESocketErrors LastError = SocketSubsystem->GetLastErrorCode();
			if (!bConnectStarted && LastError == SE_ECONNREFUSED)
			{
				SocketSubsystem->DestroySocket(Socket);
				return EInstallDoctorPortState::ConnectionRefused;
			}
			if (!bConnectStarted && LastError != SE_EWOULDBLOCK && LastError != SE_EINPROGRESS && LastError != SE_NO_ERROR)
			{
				SocketSubsystem->DestroySocket(Socket);
				return LastError == SE_ETIMEDOUT ? EInstallDoctorPortState::TimedOut : EInstallDoctorPortState::Error;
			}

			const bool bReady = Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(TimeoutMs));
			if (!bReady)
			{
				SocketSubsystem->DestroySocket(Socket);
				return EInstallDoctorPortState::TimedOut;
			}

			if (Socket->GetConnectionState() == SCS_Connected)
			{
				SocketSubsystem->DestroySocket(Socket);
				return EInstallDoctorPortState::Reachable;
			}

			LastError = SocketSubsystem->GetLastErrorCode();
			SocketSubsystem->DestroySocket(Socket);
			if (LastError == SE_ECONNREFUSED)
			{
				return EInstallDoctorPortState::ConnectionRefused;
			}
			if (LastError == SE_ETIMEDOUT)
			{
				return EInstallDoctorPortState::TimedOut;
			}
			return EInstallDoctorPortState::Error;
		}

		FInstallDoctorCheck CheckRegistryMirrorEqual(const FInstallDoctorContext& Context)
		{
			const FString PluginMirrorPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Context.PluginBaseDir, TEXT("Resources/ToolRegistry/tools.json")));
			const bool bLeftExists = FPaths::FileExists(Context.RegistryCanonicalPath);
			const bool bRightExists = FPaths::FileExists(PluginMirrorPath);

			TSharedPtr<FJsonObject> Details = MakeCheckDetails();
			Details->SetStringField(TEXT("left"), Context.RegistryCanonicalPath);
			Details->SetStringField(TEXT("right"), PluginMirrorPath);
			Details->SetBoolField(TEXT("leftExists"), bLeftExists);
			Details->SetBoolField(TEXT("rightExists"), bRightExists);
			Details->SetStringField(TEXT("leftSourceKind"), LexToString(Context.RegistryResolution.SourceKind));
			Details->SetArrayField(TEXT("leftCandidates"), MakeToolsReadCandidateValues(Context.RegistryResolution));

			if (!bLeftExists || !bRightExists)
			{
				return MakeCheck(
					TEXT("registry_mirror_equal"),
					TEXT("error"),
					TEXT("Tool registry canonical file or plugin mirror is missing."),
					Details,
					TEXT("Restore Tools/UnrealMcpToolRegistry/tools.json and the plugin Resources/ToolRegistry/tools.json mirror."));
			}

			if (!ByteCompareFiles(Context.RegistryCanonicalPath, PluginMirrorPath))
			{
				return MakeCheck(
					TEXT("registry_mirror_equal"),
					TEXT("error"),
					TEXT("Tool registry canonical file and plugin mirror differ."),
					Details,
					TEXT("Copy the canonical registry to the plugin resource mirror and rerun validation."));
			}

			return MakeCheck(
				TEXT("registry_mirror_equal"),
				TEXT("pass"),
				TEXT("Tool registry canonical file and plugin mirror byte-match."),
				Details);
		}

		FInstallDoctorCheck CheckSchemaCanonicalAliasEqual(const FInstallDoctorContext& Context)
		{
			TSharedPtr<FJsonObject> Details = MakeCheckDetails();
			Details->SetStringField(TEXT("toolsSchema"), Context.SchemaToolsPath);
			Details->SetBoolField(TEXT("toolsSchemaExists"), FPaths::FileExists(Context.SchemaToolsPath));
			Details->SetStringField(TEXT("toolsSchemaSourceKind"), LexToString(Context.SchemaToolsResolution.SourceKind));
			Details->SetArrayField(TEXT("toolsSchemaCandidates"), MakeToolsReadCandidateValues(Context.SchemaToolsResolution));
			Details->SetStringField(TEXT("projectSchemaAlias"), Context.SchemaProjectAliasPath);
			Details->SetBoolField(TEXT("projectSchemaAliasExists"), FPaths::FileExists(Context.SchemaProjectAliasPath));

			TArray<FString> RequiredPaths;
			RequiredPaths.Add(Context.SchemaToolsPath);
			RequiredPaths.Add(Context.SchemaProjectAliasPath);

			TArray<FString> PluginAliasPaths;
			PluginAliasPaths.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(Context.PluginBaseDir, TEXT("Resources/ToolRegistry/schema.json"))));
			PluginAliasPaths.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(Context.PluginBaseDir, TEXT("Resources/Schemas/UnrealMcpToolRegistry.schema.json"))));
			PluginAliasPaths.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(Context.PluginBaseDir, TEXT("Resources/ToolRegistry/UnrealMcpToolRegistry.schema.json"))));

			TArray<FString> ExistingPluginAliasPaths;
			TArray<FString> MissingOptionalPluginAliasPaths;
			for (const FString& PluginAliasPath : PluginAliasPaths)
			{
				if (FPaths::FileExists(PluginAliasPath))
				{
					ExistingPluginAliasPaths.Add(PluginAliasPath);
				}
				else
				{
					MissingOptionalPluginAliasPaths.Add(PluginAliasPath);
				}
			}
			Details->SetArrayField(TEXT("pluginSchemaAliases"), MakeJsonStringArray(ExistingPluginAliasPaths));
			Details->SetArrayField(TEXT("absentOptionalPluginSchemaAliases"), MakeJsonStringArray(MissingOptionalPluginAliasPaths));

			TArray<FString> MissingRequiredPaths;
			for (const FString& RequiredPath : RequiredPaths)
			{
				if (!FPaths::FileExists(RequiredPath))
				{
					MissingRequiredPaths.Add(RequiredPath);
				}
			}
			if (MissingRequiredPaths.Num() > 0)
			{
				Details->SetArrayField(TEXT("missingRequiredPaths"), MakeJsonStringArray(MissingRequiredPaths));
				return MakeCheck(
					TEXT("schema_canonical_alias_equal"),
					TEXT("error"),
					TEXT("One or more required ToolRegistry schema aliases are missing."),
					Details,
					TEXT("Restore Tools/UnrealMcpToolRegistry/schema.json and Schemas/UnrealMcpToolRegistry.schema.json as real files."));
			}

			TArray<TSharedPtr<FJsonObject>> InvalidFiles;
			for (const FString& Path : RequiredPaths)
			{
				TSharedPtr<FJsonObject> Issue;
				if (ValidateCriticalRealFile(Path, Issue))
				{
					InvalidFiles.Add(Issue);
				}
			}
			for (const FString& Path : ExistingPluginAliasPaths)
			{
				TSharedPtr<FJsonObject> Issue;
				if (ValidateCriticalRealFile(Path, Issue))
				{
					InvalidFiles.Add(Issue);
				}
			}
			if (InvalidFiles.Num() > 0)
			{
				Details->SetArrayField(TEXT("invalidKnownFormatOrStubbed"), MakeIssueValues(InvalidFiles));
				return MakeCheck(
					TEXT("schema_canonical_alias_equal"),
					TEXT("error"),
					TEXT("One or more ToolRegistry schema aliases are symlinks, Windows symlink stubs, or invalid known-format files."),
					Details,
					TEXT("Replace package-critical schema aliases with real file contents."));
			}

			TArray<FString> Mismatches;
			for (const FString& ComparePath : RequiredPaths)
			{
				if (ComparePath != Context.SchemaToolsPath && !ByteCompareFiles(Context.SchemaToolsPath, ComparePath))
				{
					Mismatches.Add(ComparePath);
				}
			}
			for (const FString& ComparePath : ExistingPluginAliasPaths)
			{
				if (!ByteCompareFiles(Context.SchemaToolsPath, ComparePath))
				{
					Mismatches.Add(ComparePath);
				}
			}
			if (Mismatches.Num() > 0)
			{
				Details->SetArrayField(TEXT("mismatches"), MakeJsonStringArray(Mismatches));
				return MakeCheck(
					TEXT("schema_canonical_alias_equal"),
					TEXT("error"),
					TEXT("One or more ToolRegistry schema aliases differ from the canonical schema."),
					Details,
					TEXT("Copy Tools/UnrealMcpToolRegistry/schema.json to each schema alias."));
			}

			return MakeCheck(
				TEXT("schema_canonical_alias_equal"),
				TEXT("pass"),
				ExistingPluginAliasPaths.Num() > 0
					? TEXT("ToolRegistry schema aliases byte-match the canonical schema.")
					: TEXT("Required ToolRegistry schema aliases match; no plugin schema alias is present by current repo convention."),
				Details);
		}

		FInstallDoctorCheck CheckNoGitSymlinks(const FInstallDoctorContext& Context)
		{
			FString GitRoot;
			TSharedPtr<FJsonObject> Details = MakeCheckDetails();
			if (!FindGitRoot(Context, GitRoot))
			{
				Details->SetStringField(TEXT("state"), TEXT("not_applicable"));
				Details->SetStringField(TEXT("note"), TEXT("No .git directory or file was found by walking up from project, plugin, and registry candidates."));
				return MakeCheck(
					TEXT("no_git_symlinks"),
					TEXT("pass"),
					TEXT("Git symlink scan not applicable because no git worktree was found."),
					Details);
			}

			const TArray<FString> CriticalFiles = BuildCriticalRealFileList(Context);
			TArray<TSharedPtr<FJsonObject>> Issues;
			for (const FString& CriticalFile : CriticalFiles)
			{
				TSharedPtr<FJsonObject> Issue;
				if (ValidateCriticalRealFile(CriticalFile, Issue))
				{
					Issues.Add(Issue);
				}
			}

			Details->SetStringField(TEXT("state"), TEXT("bounded_runtime_check"));
			Details->SetStringField(TEXT("repoRoot"), GitRoot);
			Details->SetNumberField(TEXT("checkedCount"), CriticalFiles.Num());
			Details->SetArrayField(TEXT("checkedFiles"), MakeJsonStringArray(CriticalFiles));
			Details->SetStringField(TEXT("deepGitIndexCheck"), TEXT("Run Tools/verify_package_integrity.py --repo-root for the full tracked-index symlink check."));

			if (Issues.Num() > 0)
			{
				Details->SetArrayField(TEXT("issues"), MakeIssueValues(Issues));
				return MakeCheck(
					TEXT("no_git_symlinks"),
					TEXT("error"),
					TEXT("Package-critical files include filesystem symlinks, Windows symlink stubs, or invalid known-format files."),
					Details,
					TEXT("Replace package-critical symlinks or stubs with real files and rerun the package integrity verifier."));
			}

			return MakeCheck(
				TEXT("no_git_symlinks"),
				TEXT("pass"),
				TEXT("Bounded runtime check found no symlinks or Windows symlink stubs in package-critical files."),
				Details);
		}

		FInstallDoctorCheck CheckPythonScriptPluginEnabled(const FInstallDoctorContext& Context)
		{
			TSharedPtr<FJsonObject> Details = MakeCheckDetails();
			Details->SetBoolField(TEXT("found"), Context.bPythonPluginFound);
			Details->SetBoolField(TEXT("enabled"), Context.bPythonScriptPluginEnabled);
			Details->SetStringField(TEXT("pluginBaseDir"), Context.PythonPluginBaseDir);

			if (!Context.bPythonPluginFound)
			{
				return MakeCheck(
					TEXT("python_script_plugin_enabled"),
					TEXT("error"),
					TEXT("PythonScriptPlugin was not found by the plugin manager."),
					Details,
					TEXT("Install or enable Unreal's PythonScriptPlugin before using Python-backed UnrealMcp tools."));
			}
			if (!Context.bPythonScriptPluginEnabled)
			{
				return MakeCheck(
					TEXT("python_script_plugin_enabled"),
					TEXT("error"),
					TEXT("PythonScriptPlugin is installed but not enabled."),
					Details,
					TEXT("Enable PythonScriptPlugin in the project, rebuild if needed, and restart Unreal Editor."));
			}

			return MakeCheck(
				TEXT("python_script_plugin_enabled"),
				TEXT("pass"),
				TEXT("PythonScriptPlugin is installed and enabled."),
				Details);
		}

		FInstallDoctorCheck CheckMcpEndpointListening()
		{
			const EInstallDoctorPortState State = ConnectLocalhostPort(8765, 100);
			TSharedPtr<FJsonObject> Details = MakeCheckDetails();
			Details->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
			Details->SetNumberField(TEXT("port"), 8765);
			Details->SetStringField(TEXT("state"), LexToString(State));

			if (State == EInstallDoctorPortState::Reachable)
			{
				return MakeCheck(
					TEXT("mcp_endpoint_listening"),
					TEXT("pass"),
					TEXT("UnrealMcp endpoint is reachable at 127.0.0.1:8765."),
					Details);
			}

			return MakeCheck(
				TEXT("mcp_endpoint_listening"),
				TEXT("error"),
				TEXT("UnrealMcp endpoint is not reachable at 127.0.0.1:8765."),
				Details,
				TEXT("Confirm the UnrealMcp plugin loaded successfully and no stale process is occupying port 8765."));
		}

		FInstallDoctorCheck CheckBridgePortReachable()
		{
			const EInstallDoctorPortState State = ConnectLocalhostPort(8766, 100);
			TSharedPtr<FJsonObject> Details = MakeCheckDetails();
			Details->SetStringField(TEXT("host"), TEXT("127.0.0.1"));
			Details->SetNumberField(TEXT("port"), 8766);
			Details->SetStringField(TEXT("state"), LexToString(State));

			if (State == EInstallDoctorPortState::Reachable)
			{
				return MakeCheck(
					TEXT("bridge_port_reachable"),
					TEXT("pass"),
					TEXT("Optional Codex bridge endpoint is reachable at 127.0.0.1:8766."),
					Details);
			}
			if (State == EInstallDoctorPortState::ConnectionRefused)
			{
				return MakeCheck(
					TEXT("bridge_port_reachable"),
					TEXT("pass"),
					TEXT("Optional Codex bridge is not running; this is expected unless bridge chat is in use."),
					Details);
			}
			if (State == EInstallDoctorPortState::NotChecked)
			{
				return MakeCheck(
					TEXT("bridge_port_reachable"),
					TEXT("pass"),
					TEXT("Optional Codex bridge port was not checked because sockets are unavailable."),
					Details);
			}

			return MakeCheck(
				TEXT("bridge_port_reachable"),
				TEXT("warning"),
				TEXT("Optional Codex bridge port probe did not complete cleanly."),
				Details,
				TEXT("Start Tools/UnrealMcpCodexBridge only when bridge chat is needed; otherwise this warning can be ignored."));
		}

		void AddPluginCandidate(TArray<FInstallDoctorPluginCandidate>& Candidates, const FString& PluginDir)
		{
			const FString DescriptorPath = FPaths::Combine(PluginDir, TEXT("UnrealMcp.uplugin"));
			FInstallDoctorPluginCandidate Candidate;
			Candidate.Path = FPaths::ConvertRelativePathToFull(PluginDir);
			Candidate.bHasPluginDescriptor = FPaths::FileExists(DescriptorPath);
			Candidate.bExists = Candidate.bHasPluginDescriptor;
			Candidates.Add(Candidate);
		}

		void AddDeepEnginePluginCandidates(TArray<FInstallDoctorPluginCandidate>& Candidates, const FString& Root)
		{
			if (!FPaths::DirectoryExists(Root))
			{
				return;
			}

			TArray<FString> FirstLevelNames;
			IFileManager::Get().FindFiles(FirstLevelNames, *FPaths::Combine(Root, TEXT("*")), false, true);
			for (const FString& FirstLevelName : FirstLevelNames)
			{
				const FString FirstLevelPath = FPaths::Combine(Root, FirstLevelName);
				AddPluginCandidate(Candidates, FirstLevelPath);

				TArray<FString> SecondLevelNames;
				IFileManager::Get().FindFiles(SecondLevelNames, *FPaths::Combine(FirstLevelPath, TEXT("*")), false, true);
				for (const FString& SecondLevelName : SecondLevelNames)
				{
					AddPluginCandidate(Candidates, FPaths::Combine(FirstLevelPath, SecondLevelName));
				}
			}
		}

		TArray<FInstallDoctorPluginCandidate> BuildDuplicatePluginCandidates(const FInstallDoctorContext& Context, bool bDeepScanEnginePlugins)
		{
			TArray<FInstallDoctorPluginCandidate> Candidates;
			AddPluginCandidate(Candidates, FPaths::Combine(Context.ProjectDir, TEXT("Plugins/UnrealMcp")));
			AddPluginCandidate(Candidates, Context.PluginBaseDir);
			AddPluginCandidate(Candidates, FPaths::Combine(Context.EngineDir, TEXT("Plugins/Marketplace/UnrealMcp")));
			AddPluginCandidate(Candidates, FPaths::Combine(Context.EngineDir, TEXT("Plugins/UnrealMcp")));

			if (bDeepScanEnginePlugins)
			{
				AddDeepEnginePluginCandidates(Candidates, FPaths::Combine(Context.EngineDir, TEXT("Plugins")));
				AddDeepEnginePluginCandidates(Candidates, FPaths::Combine(Context.EngineDir, TEXT("Plugins/Marketplace")));
			}
			return Candidates;
		}

		FInstallDoctorCheck CheckDuplicatePluginConflict(
			const FInstallDoctorContext& Context,
			bool bDeepScanEnginePlugins,
			const TArray<FInstallDoctorPluginCandidate>* InjectedCandidates,
			const FString* InjectedMountedPath)
		{
			const TArray<FInstallDoctorPluginCandidate> Candidates = InjectedCandidates
				? *InjectedCandidates
				: BuildDuplicatePluginCandidates(Context, bDeepScanEnginePlugins);
			const FString MountedPath = InjectedMountedPath ? *InjectedMountedPath : Context.MountedPluginPath;
			const FInstallDoctorDuplicateScanResult Scan = DetectDuplicateUnrealMcpPlugins(Candidates, MountedPath);

			TSharedPtr<FJsonObject> Details = MakeCheckDetails();
			Details->SetBoolField(TEXT("duplicatePluginConflict"), Scan.bDuplicatePluginConflict);
			Details->SetStringField(TEXT("mountedWinnerPath"), Scan.MountedWinnerPath);
			Details->SetArrayField(TEXT("shadowedPluginPaths"), MakeJsonStringArray(Scan.ShadowedPluginPaths));
			Details->SetArrayField(TEXT("candidatePaths"), MakeJsonStringArray(Scan.CandidatePaths));
			Details->SetBoolField(TEXT("deepScanEnginePlugins"), bDeepScanEnginePlugins);
			Details->SetBoolField(TEXT("usedInjectedCandidates"), InjectedCandidates != nullptr);

			if (Scan.bDuplicatePluginConflict)
			{
				return MakeCheck(
					TEXT("duplicate_plugin_conflict"),
					TEXT("error"),
					TEXT("Multiple UnrealMcp plugin copies were found; stale copies can shadow the mounted plugin."),
					Details,
					TEXT("Remove stale project-level or engine-level UnrealMcp plugin copies, then rebuild and restart the editor."));
			}

			return MakeCheck(
				TEXT("duplicate_plugin_conflict"),
				TEXT("pass"),
				TEXT("No duplicate UnrealMcp plugin conflict detected."),
				Details);
		}

		bool TryParseInjectedDuplicateCandidates(
			const FJsonObject& Arguments,
			TArray<FInstallDoctorPluginCandidate>& OutCandidates,
			FString& OutMountedPath)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Arguments.TryGetArrayField(TEXT("_testDuplicatePluginCandidates"), Values) || !Values)
			{
				return false;
			}

			Arguments.TryGetStringField(TEXT("_testMountedPluginPath"), OutMountedPath);
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Object.IsValid())
				{
					continue;
				}

				FInstallDoctorPluginCandidate Candidate;
				Object->TryGetStringField(TEXT("path"), Candidate.Path);
				Candidate.bExists = true;
				Object->TryGetBoolField(TEXT("exists"), Candidate.bExists);
				Candidate.bHasPluginDescriptor = Candidate.bExists;
				Object->TryGetBoolField(TEXT("hasPluginDescriptor"), Candidate.bHasPluginDescriptor);
				OutCandidates.Add(Candidate);
			}
			return true;
		}

		FInstallDoctorContext CaptureInstallDoctorContext()
		{
			FInstallDoctorContext Context;
			Context.ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			Context.ProjectSavedDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
			Context.EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());

			const FToolsReadResolution PluginBaseResolution = ResolvePluginBaseDir();
			Context.PluginBaseDir = PluginBaseResolution.Path;
			Context.MountedPluginPath = Context.PluginBaseDir;

			Context.RegistryResolution = ResolveToolsReadSubpath(TEXT("UnrealMcpToolRegistry/tools.json"), { TEXT("tools.json") });
			Context.RegistryCanonicalPath = Context.RegistryResolution.Path;
			Context.bRegistryCanonicalFound = Context.RegistryResolution.bFound;

			Context.SchemaToolsResolution = ResolveToolsReadSubpath(TEXT("UnrealMcpToolRegistry/schema.json"), { TEXT("schema.json") });
			Context.SchemaToolsPath = Context.SchemaToolsResolution.Path;
			Context.bSchemaToolsFound = Context.SchemaToolsResolution.bFound;
			if (Context.bSchemaToolsFound)
			{
				Context.SchemaProjectAliasPath = FPaths::Combine(MakeRepoRootFromToolsPath(Context.SchemaToolsPath), TEXT("Schemas/UnrealMcpToolRegistry.schema.json"));
			}
			else
			{
				Context.SchemaProjectAliasPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Context.ProjectDir, TEXT("Schemas/UnrealMcpToolRegistry.schema.json")));
			}
			Context.bSchemaProjectAliasFound = FPaths::FileExists(Context.SchemaProjectAliasPath);

			const TSharedPtr<IPlugin> PythonPlugin = IPluginManager::Get().FindPlugin(TEXT("PythonScriptPlugin"));
			Context.bPythonPluginFound = PythonPlugin.IsValid();
			if (PythonPlugin.IsValid())
			{
				Context.PythonPluginBaseDir = PythonPlugin->GetBaseDir();
				Context.bPythonScriptPluginEnabled = PythonPlugin->IsEnabled();
			}
			return Context;
		}

		TSharedPtr<FJsonObject> RunInstallDoctorChecks(
			const FInstallDoctorContext& Context,
			bool bIncludeDetailsForCache,
			bool bDeepScanEnginePlugins,
			const TArray<FInstallDoctorPluginCandidate>* InjectedCandidates = nullptr,
			const FString* InjectedMountedPath = nullptr)
		{
			TArray<FInstallDoctorCheck> Checks;
			Checks.Add(CheckRegistryMirrorEqual(Context));
			Checks.Add(CheckSchemaCanonicalAliasEqual(Context));
			Checks.Add(CheckNoGitSymlinks(Context));
			Checks.Add(CheckPythonScriptPluginEnabled(Context));
			Checks.Add(CheckMcpEndpointListening());
			Checks.Add(CheckBridgePortReachable());
			Checks.Add(CheckDuplicatePluginConflict(Context, bDeepScanEnginePlugins, InjectedCandidates, InjectedMountedPath));
			return BuildDoctorResult(Checks, bIncludeDetailsForCache);
		}

		void SaveLatestInstallDoctorJsonText(const FString& JsonText)
		{
			const FString LatestPath = GetInstallDoctorLatestPath();
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(LatestPath), true);
			FFileHelper::SaveStringToFile(JsonText, *LatestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		void SaveLatestInstallDoctorResult(const TSharedPtr<FJsonObject>& Result)
		{
			if (!Result.IsValid())
			{
				return;
			}
			SaveLatestInstallDoctorJsonText(JsonObjectToString(Result));
		}

		void RunDeferredFirstDoctor()
		{
			const FInstallDoctorContext Context = CaptureInstallDoctorContext();
			Async(EAsyncExecution::ThreadPool, [Context]()
			{
				const TSharedPtr<FJsonObject> Result = RunInstallDoctorChecks(Context, true, false);
				const FString JsonText = Result.IsValid() ? JsonObjectToString(Result) : FString();
				AsyncTask(ENamedThreads::GameThread, [JsonText]()
				{
					if (!JsonText.IsEmpty())
					{
						SaveLatestInstallDoctorJsonText(JsonText);
					}
				});
			});
		}

		bool RunInstallDoctorStartupTicker(float DeltaTime)
		{
			(void)DeltaTime;
			RunDeferredFirstDoctor();
			return false;
		}
	}

	FInstallDoctorDuplicateScanResult DetectDuplicateUnrealMcpPlugins(
		const TArray<FInstallDoctorPluginCandidate>& Candidates,
		const FString& MountedWinnerPath)
	{
		FInstallDoctorDuplicateScanResult Result;
		Result.MountedWinnerPath = NormalizeDoctorPath(MountedWinnerPath);
		const FString MountedKey = NormalizeDoctorPathKey(MountedWinnerPath);

		TSet<FString> SeenKeys;
		for (const FInstallDoctorPluginCandidate& Candidate : Candidates)
		{
			if (!Candidate.bExists || !Candidate.bHasPluginDescriptor || Candidate.Path.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}

			const FString NormalizedPath = NormalizeDoctorPath(Candidate.Path);
			const FString Key = NormalizeDoctorPathKey(Candidate.Path);
			if (SeenKeys.Contains(Key))
			{
				continue;
			}
			SeenKeys.Add(Key);
			Result.CandidatePaths.Add(NormalizedPath);
			if (!Key.Equals(MountedKey, ESearchCase::CaseSensitive))
			{
				Result.ShadowedPluginPaths.Add(NormalizedPath);
			}
		}

		Result.CandidatePaths.Sort();
		Result.ShadowedPluginPaths.Sort();
		Result.bDuplicatePluginConflict = Result.CandidatePaths.Num() >= 2;
		return Result;
	}

	bool LoadLatestInstallDoctorSummary(TSharedPtr<FJsonObject>& OutSummary, FString& OutPath)
	{
		OutSummary.Reset();
		OutPath = GetInstallDoctorLatestPath();

		TSharedPtr<FJsonObject> Latest;
		FString FailureReason;
		if (!FPaths::FileExists(OutPath) || !LoadJsonObjectFromFile(OutPath, Latest, FailureReason) || !Latest.IsValid())
		{
			return false;
		}

		OutSummary = MakeShared<FJsonObject>();
		FString StringValue;
		double NumberValue = 0.0;
		if (Latest->TryGetStringField(TEXT("mode"), StringValue))
		{
			OutSummary->SetStringField(TEXT("mode"), StringValue);
		}
		if (Latest->TryGetStringField(TEXT("scope"), StringValue))
		{
			OutSummary->SetStringField(TEXT("scope"), StringValue);
		}
		if (Latest->TryGetStringField(TEXT("overallSeverity"), StringValue))
		{
			OutSummary->SetStringField(TEXT("overallSeverity"), StringValue);
		}
		if (Latest->TryGetNumberField(TEXT("blockingIssueCount"), NumberValue))
		{
			OutSummary->SetNumberField(TEXT("blockingIssueCount"), NumberValue);
		}
		if (Latest->TryGetNumberField(TEXT("warningCount"), NumberValue))
		{
			OutSummary->SetNumberField(TEXT("warningCount"), NumberValue);
		}
		if (Latest->TryGetStringField(TEXT("lastRunUtc"), StringValue))
		{
			OutSummary->SetStringField(TEXT("lastRunUtc"), StringValue);
		}
		if (Latest->TryGetStringField(TEXT("validatorVersion"), StringValue))
		{
			OutSummary->SetStringField(TEXT("validatorVersion"), StringValue);
		}

		const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
		const int32 CheckCount = Latest->TryGetArrayField(TEXT("checks"), Checks) && Checks ? Checks->Num() : 0;
		OutSummary->SetNumberField(TEXT("checkCount"), CheckCount);
		return true;
	}

	void ScheduleInstallDoctorFirstRun()
	{
		static bool bScheduled = false;
		if (bScheduled)
		{
			return;
		}
		bScheduled = true;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&RunInstallDoctorStartupTicker), 2.0f);
	}

	FUnrealMcpExecutionResult InstallDoctor(const FJsonObject& Arguments)
	{
		bool bIncludeDetails = false;
		bool bRefresh = false;
		bool bDeepScanEnginePlugins = false;
		Arguments.TryGetBoolField(TEXT("includeDetails"), bIncludeDetails);
		Arguments.TryGetBoolField(TEXT("refresh"), bRefresh);
		Arguments.TryGetBoolField(TEXT("deepScanEnginePlugins"), bDeepScanEnginePlugins);

		TArray<FInstallDoctorPluginCandidate> InjectedCandidates;
		FString InjectedMountedPath;
		const bool bHasInjectedCandidates = TryParseInjectedDuplicateCandidates(Arguments, InjectedCandidates, InjectedMountedPath);

		const FString LatestPath = GetInstallDoctorLatestPath();
		TSharedPtr<FJsonObject> CachedResult;
		FString FailureReason;
		if (!bRefresh
			&& !bHasInjectedCandidates
			&& FPaths::FileExists(LatestPath)
			&& LoadJsonObjectFromFile(LatestPath, CachedResult, FailureReason)
			&& IsCacheRecent(CachedResult))
		{
			const TSharedPtr<FJsonObject> Filtered = FilterDoctorResult(CachedResult, bIncludeDetails);
			return MakeExecutionResult(TEXT("Install doctor returned the recent cached result."), Filtered, false);
		}

		const FInstallDoctorContext Context = CaptureInstallDoctorContext();
		const TSharedPtr<FJsonObject> FullResult = RunInstallDoctorChecks(
			Context,
			true,
			bDeepScanEnginePlugins,
			bHasInjectedCandidates ? &InjectedCandidates : nullptr,
			bHasInjectedCandidates ? &InjectedMountedPath : nullptr);
		if (!bHasInjectedCandidates)
		{
			SaveLatestInstallDoctorResult(FullResult);
		}

		const TSharedPtr<FJsonObject> Filtered = FilterDoctorResult(FullResult, bIncludeDetails);
		FString OverallSeverity;
		Filtered->TryGetStringField(TEXT("overallSeverity"), OverallSeverity);
		return MakeExecutionResult(
			FString::Printf(TEXT("Install doctor completed with overallSeverity=%s."), OverallSeverity.IsEmpty() ? TEXT("unknown") : *OverallSeverity),
			Filtered,
			false);
	}
}
