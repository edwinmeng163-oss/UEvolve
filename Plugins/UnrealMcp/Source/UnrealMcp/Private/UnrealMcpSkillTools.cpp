#include "UnrealMcpSkillTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UnrealMcp
{
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	bool ResolveProjectPathInsideProject(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason);
	TSharedPtr<FJsonObject> MakeFileInfoObject(const FString& Path);
	bool TryGetStringArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<FString>& OutValues);
	FString JsonObjectToString(const TSharedPtr<FJsonObject>& JsonObject);
	TArray<TSharedPtr<FJsonValue>> MakeJsonStringArray(const TArray<FString>& Values);
	FUnrealMcpExecutionResult ProjectMemoryWrite(const FJsonObject& Arguments);

	bool TryExecuteSkillTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult)
	{
		if (ToolName == TEXT("unreal.skill_list"))
		{
			OutResult = SkillList(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.skill_read"))
		{
			OutResult = SkillRead(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.skill_apply"))
		{
			OutResult = SkillApply(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.skill_recording_start"))
		{
			OutResult = SkillRecordingStart(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.skill_recording_stop"))
		{
			OutResult = SkillRecordingStop(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.skill_activity_status"))
		{
			OutResult = SkillActivityStatus(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.skill_distill_from_activity"))
		{
			OutResult = SkillDistillFromActivity(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.skill_save_draft"))
		{
			OutResult = SkillSaveDraft(Arguments);
			return true;
		}

		// skill_promote_draft remains in the module dispatch for now because it owns the extension-session lock.
		return false;
	}

	namespace
	{
		FCriticalSection GSkillActivityMutex;
		FCriticalSection GSkillActivityFileMutex;
		bool GSkillActivityRecording = false;
		FString GSkillActivitySessionId;
		FString GSkillActivityGoal = TEXT("Activity recording is off until unreal.skill_recording_start is called.");
		FDateTime GSkillActivityStartedAtUtc;
		FDateTime GSkillActivityLastHeartbeatAtUtc;
		double GSkillActivityRecordIntervalSeconds = 60.0;
		int32 GSkillActivityEventCount = 0;
		FString GSkillActivityLastLogPath;
		FString GSkillActivityLastError;

		FString GetActivityLogRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ActivityLog")));
		}

		FString GetSkillDraftRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/SkillDrafts")));
		}

		FString GetProjectSkillRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpSkills")));
		}

		FString GetSkillPromotionBackupRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/SkillPromotionBackups")));
		}

		FString MakeSkillActivitySessionId()
		{
			const FString TimePart = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			return FString::Printf(TEXT("%s-%s"), *TimePart, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
		}

		FString SanitizeSkillSlug(FString Value)
		{
			Value = Value.TrimStartAndEnd().ToLower();
			FString Result;
			for (TCHAR Character : Value)
			{
				if (FChar::IsAlnum(Character))
				{
					Result.AppendChar(Character);
				}
				else if (Character == TEXT('-') || Character == TEXT('_') || Character == TEXT('.'))
				{
					Result.AppendChar(Character);
				}
				else if (FChar::IsWhitespace(Character))
				{
					Result.AppendChar(TEXT('-'));
				}
			}
			while (Result.Contains(TEXT("--")))
			{
				Result = Result.Replace(TEXT("--"), TEXT("-"));
			}
			Result.RemoveFromStart(TEXT("-"));
			Result.RemoveFromEnd(TEXT("-"));
			return Result.IsEmpty() ? TEXT("distilled-skill") : Result.Left(80);
		}

		FString GetActivityLogPathForSession(const FString& SessionId)
		{
			return FPaths::Combine(GetActivityLogRoot(), SanitizeSkillSlug(SessionId) + TEXT(".jsonl"));
		}

		FString GetDraftPathForSkill(const FString& SkillName)
		{
			const FString SafeSkillName = SanitizeSkillSlug(SkillName);
			return FPaths::Combine(GetSkillDraftRoot(), SafeSkillName, TEXT("SKILL.md"));
		}

		FString GetPromotedSkillPath(const FString& SkillName)
		{
			const FString SafeSkillName = SanitizeSkillSlug(SkillName);
			return FPaths::Combine(GetProjectSkillRoot(), SafeSkillName, TEXT("SKILL.md"));
		}

		void EnsureActivitySessionLocked()
		{
			if (GSkillActivitySessionId.IsEmpty())
			{
				GSkillActivitySessionId = MakeSkillActivitySessionId();
				GSkillActivityStartedAtUtc = FDateTime::UtcNow();
				GSkillActivityLastHeartbeatAtUtc = FDateTime();
				GSkillActivityEventCount = 0;
				GSkillActivityLastLogPath = GetActivityLogPathForSession(GSkillActivitySessionId);
			}
		}

		bool AppendActivityJsonLine(const FString& ActivityLogPath, const TSharedPtr<FJsonObject>& EventObject, FString& OutFailureReason)
		{
			FScopeLock FileLock(&GSkillActivityFileMutex);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ActivityLogPath), true);
			FString CompactJson;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&CompactJson);
			if (!FJsonSerializer::Serialize(EventObject.ToSharedRef(), Writer))
			{
				OutFailureReason = TEXT("Failed to serialize activity event.");
				return false;
			}
			const FString Line = CompactJson + LINE_TERMINATOR;
			if (!FFileHelper::SaveStringToFile(Line, *ActivityLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to append activity event to %s."), *ActivityLogPath);
				return false;
			}
			return true;
		}

		bool ResetActivityLogFile(const FString& ActivityLogPath, FString& OutFailureReason)
		{
			if (ActivityLogPath.TrimStartAndEnd().IsEmpty())
			{
				return true;
			}

			FScopeLock FileLock(&GSkillActivityFileMutex);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ActivityLogPath), true);
			if (!FPaths::FileExists(ActivityLogPath))
			{
				return true;
			}

			if (!IFileManager::Get().Delete(*ActivityLogPath, false, true, true))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to reset activity log %s."), *ActivityLogPath);
				return false;
			}
			return true;
		}

		TArray<FString> SkillGetArgumentKeys(const FJsonObject& Arguments)
		{
			TArray<FString> Keys;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments.Values)
			{
				Keys.Add(Pair.Key);
			}
			Keys.Sort();
			return Keys;
		}

		TSharedPtr<FJsonObject> MakeActivityStateObject()
		{
			FScopeLock Lock(&GSkillActivityMutex);
			EnsureActivitySessionLocked();

			TSharedPtr<FJsonObject> StateObject = MakeShared<FJsonObject>();
			StateObject->SetBoolField(TEXT("recording"), GSkillActivityRecording);
			StateObject->SetStringField(TEXT("sessionId"), GSkillActivitySessionId);
			StateObject->SetStringField(TEXT("goal"), GSkillActivityGoal);
			StateObject->SetStringField(TEXT("startedAtUtc"), GSkillActivityStartedAtUtc.ToIso8601());
			StateObject->SetStringField(TEXT("lastHeartbeatAtUtc"), GSkillActivityLastHeartbeatAtUtc.ToIso8601());
			StateObject->SetNumberField(TEXT("recordIntervalSeconds"), GSkillActivityRecordIntervalSeconds);
			StateObject->SetNumberField(TEXT("eventCount"), GSkillActivityEventCount);
			StateObject->SetBoolField(TEXT("readOnlyToolCallsRecorded"), false);
			StateObject->SetStringField(TEXT("activityLogPath"), GSkillActivityLastLogPath);
			StateObject->SetStringField(TEXT("activityLogRoot"), GetActivityLogRoot());
			StateObject->SetStringField(TEXT("skillDraftRoot"), GetSkillDraftRoot());
			StateObject->SetStringField(TEXT("promotedSkillRoot"), GetProjectSkillRoot());
			if (!GSkillActivityLastError.IsEmpty())
			{
				StateObject->SetStringField(TEXT("lastError"), GSkillActivityLastError);
			}
			return StateObject;
		}

		bool LoadActivityEvents(const FString& SessionId, int32 MaxEvents, TArray<TSharedPtr<FJsonObject>>& OutEvents, FString& OutLogPath, FString& OutFailureReason)
		{
			const FString EffectiveSessionId = SessionId.TrimStartAndEnd().IsEmpty() ? GSkillActivitySessionId : SessionId.TrimStartAndEnd();
			if (EffectiveSessionId.IsEmpty())
			{
				OutFailureReason = TEXT("No activity session is active. Start recording or provide sessionId.");
				return false;
			}

			OutLogPath = GetActivityLogPathForSession(EffectiveSessionId);
			TArray<FString> Lines;
			{
				FScopeLock FileLock(&GSkillActivityFileMutex);
				if (!FFileHelper::LoadFileToStringArray(Lines, *OutLogPath))
				{
					OutFailureReason = FString::Printf(TEXT("No activity log found at %s."), *OutLogPath);
					return false;
				}
			}

			const int32 SafeMaxEvents = FMath::Clamp(MaxEvents, 1, 5000);
			const int32 StartIndex = FMath::Max(0, Lines.Num() - SafeMaxEvents);
			for (int32 Index = StartIndex; Index < Lines.Num(); ++Index)
			{
				const FString CleanLine = Lines[Index].TrimStartAndEnd();
				if (CleanLine.IsEmpty())
				{
					continue;
				}

				TSharedPtr<FJsonObject> EventObject;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CleanLine);
				if (FJsonSerializer::Deserialize(Reader, EventObject) && EventObject.IsValid())
				{
					OutEvents.Add(EventObject);
				}
			}
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> MakeActivityEventJsonArray(const TArray<TSharedPtr<FJsonObject>>& Events)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const TSharedPtr<FJsonObject>& Event : Events)
			{
				Values.Add(MakeShared<FJsonValueObject>(Event));
			}
			return Values;
		}

		FString GetEventStringField(const TSharedPtr<FJsonObject>& Event, const FString& FieldName)
		{
			FString Value;
			if (Event.IsValid())
			{
				Event->TryGetStringField(FieldName, Value);
			}
			return Value;
		}

		bool GetEventBoolDetail(const TSharedPtr<FJsonObject>& Event, const FString& FieldName)
		{
			const TSharedPtr<FJsonObject>* Details = nullptr;
			bool bValue = false;
			if (Event.IsValid() && Event->TryGetObjectField(TEXT("details"), Details) && Details && Details->IsValid())
			{
				(*Details)->TryGetBoolField(FieldName, bValue);
			}
			return bValue;
		}

		FString GetEventStringDetail(const TSharedPtr<FJsonObject>& Event, const FString& FieldName)
		{
			const TSharedPtr<FJsonObject>* Details = nullptr;
			FString Value;
			if (Event.IsValid() && Event->TryGetObjectField(TEXT("details"), Details) && Details && Details->IsValid())
			{
				(*Details)->TryGetStringField(FieldName, Value);
			}
			return Value;
		}

		FString BuildDistilledSkillText(const FString& SkillName, const FString& Title, const FString& Goal, const FString& SessionId, const TArray<TSharedPtr<FJsonObject>>& Events, bool bIncludeEvents)
		{
			TMap<FString, int32> EventTypeCounts;
			TArray<FString> Steps;
			TArray<FString> Successes;
			TArray<FString> Pitfalls;
			TArray<FString> ToolNames;

			for (const TSharedPtr<FJsonObject>& Event : Events)
			{
				const FString EventType = GetEventStringField(Event, TEXT("eventType"));
				const FString Summary = GetEventStringField(Event, TEXT("summary"));
				const FString Timestamp = GetEventStringField(Event, TEXT("timestampUtc"));
				const FString ToolName = GetEventStringDetail(Event, TEXT("toolName"));
				const bool bIsError = GetEventBoolDetail(Event, TEXT("isError"));

				if (!EventType.IsEmpty())
				{
					EventTypeCounts.FindOrAdd(EventType)++;
				}
				if (!ToolName.IsEmpty())
				{
					ToolNames.AddUnique(ToolName);
				}
				if (EventType != TEXT("heartbeat") && Steps.Num() < 60)
				{
					const FString Prefix = Timestamp.IsEmpty() ? FString() : FString::Printf(TEXT("[%s] "), *Timestamp);
					const FString Suffix = ToolName.IsEmpty() ? FString() : FString::Printf(TEXT(" (`%s`)"), *ToolName);
					Steps.Add(FString::Printf(TEXT("- %s%s%s"), *Prefix, *Summary, *Suffix));
				}
				if (!bIsError && (EventType.Contains(TEXT("tool")) || EventType.Contains(TEXT("draft")) || EventType.Contains(TEXT("promote")) || EventType.Contains(TEXT("recording"))))
				{
					if (Successes.Num() < 20)
					{
						Successes.Add(FString::Printf(TEXT("- %s"), Summary.IsEmpty() ? *EventType : *Summary));
					}
				}
				if (bIsError || Summary.Contains(TEXT("failed"), ESearchCase::IgnoreCase) || Summary.Contains(TEXT("error"), ESearchCase::IgnoreCase))
				{
					if (Pitfalls.Num() < 20)
					{
						Pitfalls.Add(FString::Printf(TEXT("- %s"), Summary.IsEmpty() ? *EventType : *Summary));
					}
				}
			}

			FString EventCountsText;
			for (const TPair<FString, int32>& Pair : EventTypeCounts)
			{
				EventCountsText += FString::Printf(TEXT("- `%s`: %d\n"), *Pair.Key, Pair.Value);
			}

			FString ToolText;
			ToolNames.Sort();
			for (const FString& ToolName : ToolNames)
			{
				ToolText += FString::Printf(TEXT("- `%s`\n"), *ToolName);
			}

			FString EventAppendix;
			if (bIncludeEvents)
			{
				EventAppendix = TEXT("\n## Event Appendix\n");
				for (const TSharedPtr<FJsonObject>& Event : Events)
				{
					EventAppendix += FString::Printf(TEXT("- `%s` %s\n"), *GetEventStringField(Event, TEXT("eventType")), *GetEventStringField(Event, TEXT("summary")));
				}
			}

				const FString SafeTitle = Title.TrimStartAndEnd().IsEmpty() ? SkillName : Title.TrimStartAndEnd();
				const FString LearnedGoal = Goal.TrimStartAndEnd().IsEmpty() ? TEXT("No explicit goal was recorded.") : Goal.TrimStartAndEnd();
				const FString WorkflowText = Steps.Num() == 0 ? TEXT("- No non-heartbeat steps were recorded.") : FString::Join(Steps, TEXT("\n"));
				const FString SuccessfulPatternsText = Successes.Num() == 0 ? TEXT("- No success markers were detected.\n") : FString::Join(Successes, TEXT("\n"));
				const FString PitfallsText = Pitfalls.Num() == 0 ? TEXT("- No explicit pitfalls were detected.\n") : FString::Join(Pitfalls, TEXT("\n"));
				const FString EventCountsOutput = EventCountsText.IsEmpty() ? TEXT("- No event counts available.\n") : EventCountsText;
				return FString::Printf(
				TEXT("# %s\n\n")
				TEXT("Use this skill when repeating a workflow distilled from UEvolve / Unreal MCP activity logs.\n\n")
				TEXT("## Learned Goal\n%s\n\n")
				TEXT("## Session\n- Session ID: `%s`\n- Source: `Saved/UnrealMcp/ActivityLog/*.jsonl`\n\n")
				TEXT("## Workflow\n%s\n\n")
				TEXT("## Tools Observed\n%s\n")
				TEXT("## Successful Patterns\n%s\n")
				TEXT("## Pitfalls To Avoid\n%s\n")
				TEXT("## Reusable Checklist\n")
				TEXT("- Confirm project/editor state before changing assets or source.\n")
				TEXT("- Prefer dry run, audit, backup, build, test, then promote.\n")
				TEXT("- Keep generated assets under project-local folders and avoid external writes unless explicitly approved.\n")
				TEXT("- If a build or MCP test fails, preserve the log path and summarize the fix before retrying.\n\n")
				TEXT("## Event Type Counts\n%s")
					TEXT("%s"),
					*SafeTitle,
					*LearnedGoal,
					*SessionId,
					*WorkflowText,
					ToolText.IsEmpty() ? TEXT("- No MCP tools were observed.\n") : *ToolText,
					*SuccessfulPatternsText,
					*PitfallsText,
					*EventCountsOutput,
					*EventAppendix);
		}

		bool WriteSkillFile(const FString& Path, const FString& Text, bool bOverwrite, FString& OutFailureReason)
		{
			if (FPaths::FileExists(Path) && !bOverwrite)
			{
				OutFailureReason = FString::Printf(TEXT("Refusing to overwrite existing skill file %s. Pass overwrite=true to replace it."), *Path);
				return false;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to write skill file %s."), *Path);
				return false;
			}
			return true;
		}

		FString MakeSkillPromotionBackupDir(const FString& SkillName)
		{
			return FPaths::Combine(
				GetSkillPromotionBackupRoot(),
				FString::Printf(
					TEXT("%s_%s_%s"),
					*FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S")),
					*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8),
					*SanitizeSkillSlug(SkillName)));
		}

		bool WriteSkillPromotionManifest(
			const FString& ManifestPath,
			const FString& SkillName,
			const FString& DraftPath,
			const FString& PromotedPath,
			const FString& BackupPath,
			bool bDryRun,
			bool bExistedBefore,
			bool bChanged,
			bool bOverwrite,
			FString& OutFailureReason)
		{
			TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
			ManifestObject->SetStringField(TEXT("action"), TEXT("skill_promote_draft"));
			ManifestObject->SetStringField(TEXT("timestampUtc"), FDateTime::UtcNow().ToIso8601());
			ManifestObject->SetStringField(TEXT("skillName"), SkillName);
			ManifestObject->SetStringField(TEXT("draftPath"), DraftPath);
			ManifestObject->SetStringField(TEXT("promotedPath"), PromotedPath);
			ManifestObject->SetStringField(TEXT("backupPath"), BackupPath);
			ManifestObject->SetBoolField(TEXT("dryRun"), bDryRun);
			ManifestObject->SetBoolField(TEXT("existedBefore"), bExistedBefore);
			ManifestObject->SetBoolField(TEXT("changed"), bChanged);
			ManifestObject->SetBoolField(TEXT("overwrite"), bOverwrite);

			const FString ManifestJson = JsonObjectToString(ManifestObject);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManifestPath), true);
			if (!FFileHelper::SaveStringToFile(ManifestJson, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to write skill promotion manifest %s."), *ManifestPath);
				return false;
			}
			return true;
		}
	}

	void RecordSkillActivityEvent(const FString& EventType, const FString& Summary, const TSharedPtr<FJsonObject>& Details)
	{
		TSharedPtr<FJsonObject> EventObject = MakeShared<FJsonObject>();
		FString ActivityLogPath;
		{
			FScopeLock Lock(&GSkillActivityMutex);
			if (!GSkillActivityRecording)
			{
				return;
			}
			EnsureActivitySessionLocked();
			ActivityLogPath = GSkillActivityLastLogPath;
			EventObject->SetStringField(TEXT("sessionId"), GSkillActivitySessionId);
			EventObject->SetStringField(TEXT("goal"), GSkillActivityGoal);
		}

		EventObject->SetStringField(TEXT("timestampUtc"), FDateTime::UtcNow().ToIso8601());
		EventObject->SetStringField(TEXT("eventType"), EventType);
		EventObject->SetStringField(TEXT("summary"), Summary.Left(2000));
		if (Details.IsValid())
		{
			EventObject->SetObjectField(TEXT("details"), Details);
		}

		FString FailureReason;
		const bool bWrote = AppendActivityJsonLine(ActivityLogPath, EventObject, FailureReason);
		FScopeLock Lock(&GSkillActivityMutex);
		if (bWrote)
		{
			++GSkillActivityEventCount;
			GSkillActivityLastError.Empty();
		}
		else
		{
			GSkillActivityLastError = FailureReason;
		}
	}

	void TickSkillActivityRecorder()
	{
		bool bShouldWriteHeartbeat = false;
		{
			FScopeLock Lock(&GSkillActivityMutex);
			if (!GSkillActivityRecording)
			{
				return;
			}
			EnsureActivitySessionLocked();
			const FDateTime NowUtc = FDateTime::UtcNow();
			if (GSkillActivityLastHeartbeatAtUtc.GetTicks() == 0 || (NowUtc - GSkillActivityLastHeartbeatAtUtc).GetTotalSeconds() >= GSkillActivityRecordIntervalSeconds)
			{
				GSkillActivityLastHeartbeatAtUtc = NowUtc;
				bShouldWriteHeartbeat = true;
			}
		}

		if (bShouldWriteHeartbeat)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("projectName"), FApp::GetProjectName());
			Details->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
			RecordSkillActivityEvent(TEXT("heartbeat"), TEXT("Periodic editor activity heartbeat."), Details);
		}
	}

			FString GetDefaultProjectSkillRoot()
			{
				return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpSkills")));
			}

		FString SkillNameFromPath(const FString& SkillPath)
		{
			if (FPaths::GetCleanFilename(SkillPath).Equals(TEXT("SKILL.md"), ESearchCase::IgnoreCase))
			{
				return FPaths::GetCleanFilename(FPaths::GetPath(SkillPath));
			}
			FString Name = FPaths::GetBaseFilename(SkillPath);
			Name.RemoveFromEnd(TEXT(".skill"), ESearchCase::IgnoreCase);
			return Name;
		}

		FString ExtractSkillTitle(const FString& SkillText, const FString& Fallback)
		{
			TArray<FString> Lines;
			SkillText.ParseIntoArrayLines(Lines, false);
			for (const FString& Line : Lines)
			{
				FString CleanLine = Line.TrimStartAndEnd();
				if (CleanLine.StartsWith(TEXT("#")))
				{
					CleanLine.RemoveFromStart(TEXT("#"));
					return CleanLine.TrimStartAndEnd();
				}
			}
			return Fallback;
		}

		FString ExtractSkillDescription(const FString& SkillText)
		{
			TArray<FString> Lines;
			SkillText.ParseIntoArrayLines(Lines, false);
			bool bPassedTitle = false;
			for (const FString& Line : Lines)
			{
				const FString CleanLine = Line.TrimStartAndEnd();
				if (CleanLine.IsEmpty())
				{
					continue;
				}
				if (CleanLine.StartsWith(TEXT("#")))
				{
					bPassedTitle = true;
					continue;
				}
				if (bPassedTitle)
				{
					return CleanLine.Left(600);
				}
			}
			return FString();
		}

		void CollectSkillPathsFromRoot(const FString& RootPath, TArray<FString>& OutSkillPaths)
		{
			if (!FPaths::DirectoryExists(RootPath))
			{
				return;
			}
			TArray<FString> SkillMdFiles;
			TArray<FString> SkillFiles;
			IFileManager::Get().FindFilesRecursive(SkillMdFiles, *RootPath, TEXT("SKILL.md"), true, false);
			IFileManager::Get().FindFilesRecursive(SkillFiles, *RootPath, TEXT("*.skill"), true, false);
			for (const FString& SkillPath : SkillMdFiles)
			{
				OutSkillPaths.AddUnique(SkillPath);
			}
			for (const FString& SkillPath : SkillFiles)
			{
				OutSkillPaths.AddUnique(SkillPath);
			}
			OutSkillPaths.Sort();
		}

		TSharedPtr<FJsonObject> MakeSkillInfoObject(const FString& SkillPath, bool bIncludeText, int32 MaxPreviewChars)
		{
			TSharedPtr<FJsonObject> SkillObject = MakeShared<FJsonObject>();
			const FString SkillName = SkillNameFromPath(SkillPath);
			SkillObject->SetStringField(TEXT("name"), SkillName);
			SkillObject->SetStringField(TEXT("path"), SkillPath);
			SkillObject->SetObjectField(TEXT("file"), MakeFileInfoObject(SkillPath));

			FString SkillText;
			if (FFileHelper::LoadFileToString(SkillText, *SkillPath))
			{
				SkillObject->SetStringField(TEXT("title"), ExtractSkillTitle(SkillText, SkillName));
				SkillObject->SetStringField(TEXT("description"), ExtractSkillDescription(SkillText));
				SkillObject->SetStringField(TEXT("preview"), SkillText.Left(FMath::Max(0, MaxPreviewChars)));
				if (bIncludeText)
				{
					SkillObject->SetStringField(TEXT("text"), SkillText);
				}
			}
			else
			{
				SkillObject->SetStringField(TEXT("title"), SkillName);
				SkillObject->SetStringField(TEXT("description"), TEXT("Failed to read skill text."));
			}
			return SkillObject;
		}

		bool ResolveSkillPathFromArguments(const FJsonObject& Arguments, FString& OutSkillPath, FString& OutFailureReason)
		{
			FString SkillPath;
			FString SkillName;
			Arguments.TryGetStringField(TEXT("skillPath"), SkillPath);
			Arguments.TryGetStringField(TEXT("skillName"), SkillName);
			SkillPath = SkillPath.TrimStartAndEnd();
			SkillName = SkillName.TrimStartAndEnd();

			if (!SkillPath.IsEmpty())
			{
				return ResolveProjectPathInsideProject(SkillPath, OutSkillPath, OutFailureReason);
			}

			if (SkillName.IsEmpty())
			{
				OutFailureReason = TEXT("Provide either skillPath or skillName.");
				return false;
			}

			TArray<FString> Roots;
			TryGetStringArrayField(Arguments, TEXT("roots"), Roots);
			if (Roots.Num() == 0)
			{
				Roots.Add(TEXT("Tools/UnrealMcpSkills"));
			}

			TArray<FString> SkillPaths;
			for (const FString& Root : Roots)
			{
				FString ResolvedRoot;
				if (ResolveProjectPathInsideProject(Root, ResolvedRoot, OutFailureReason))
				{
					CollectSkillPathsFromRoot(ResolvedRoot, SkillPaths);
				}
			}

			for (const FString& CandidatePath : SkillPaths)
			{
				if (SkillNameFromPath(CandidatePath).Equals(SkillName, ESearchCase::IgnoreCase))
				{
					OutSkillPath = CandidatePath;
					return true;
				}
			}

			OutFailureReason = FString::Printf(TEXT("No project skill named '%s' was found."), *SkillName);
			return false;
		}

		FUnrealMcpExecutionResult SkillList(const FJsonObject& Arguments)
		{
			TArray<FString> Roots;
			TryGetStringArrayField(Arguments, TEXT("roots"), Roots);
			if (Roots.Num() == 0)
			{
				Roots.Add(TEXT("Tools/UnrealMcpSkills"));
			}
			FString NameFilter;
			bool bIncludeText = false;
			double MaxPreviewCharsDouble = 1200.0;
			Arguments.TryGetStringField(TEXT("nameFilter"), NameFilter);
			Arguments.TryGetBoolField(TEXT("includeText"), bIncludeText);
			Arguments.TryGetNumberField(TEXT("maxPreviewChars"), MaxPreviewCharsDouble);
			const int32 MaxPreviewChars = FMath::Clamp(static_cast<int32>(MaxPreviewCharsDouble), 0, 20000);

			TArray<TSharedPtr<FJsonValue>> RootObjects;
			TArray<FString> SkillPaths;
			FString FailureReason;
			for (const FString& Root : Roots)
			{
				FString ResolvedRoot;
				if (!ResolveProjectPathInsideProject(Root, ResolvedRoot, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				TSharedPtr<FJsonObject> RootObject = MakeShared<FJsonObject>();
				RootObject->SetStringField(TEXT("root"), ResolvedRoot);
				RootObject->SetBoolField(TEXT("exists"), FPaths::DirectoryExists(ResolvedRoot));
				RootObjects.Add(MakeShared<FJsonValueObject>(RootObject));
				CollectSkillPathsFromRoot(ResolvedRoot, SkillPaths);
			}

			TArray<TSharedPtr<FJsonValue>> SkillObjects;
			for (const FString& SkillPath : SkillPaths)
			{
				const FString SkillName = SkillNameFromPath(SkillPath);
				if (!NameFilter.TrimStartAndEnd().IsEmpty() && !SkillName.Contains(NameFilter.TrimStartAndEnd(), ESearchCase::IgnoreCase))
				{
					continue;
				}
				SkillObjects.Add(MakeShared<FJsonValueObject>(MakeSkillInfoObject(SkillPath, bIncludeText, MaxPreviewChars)));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("skill_list"));
			StructuredContent->SetArrayField(TEXT("roots"), RootObjects);
			StructuredContent->SetStringField(TEXT("nameFilter"), NameFilter);
			StructuredContent->SetNumberField(TEXT("skillCount"), SkillObjects.Num());
			StructuredContent->SetArrayField(TEXT("skills"), SkillObjects);
			return MakeExecutionResult(FString::Printf(TEXT("Found %d project skill%s."), SkillObjects.Num(), SkillObjects.Num() == 1 ? TEXT("") : TEXT("s")), StructuredContent, false);
		}

		FUnrealMcpExecutionResult SkillRead(const FJsonObject& Arguments)
		{
			bool bIncludeText = true;
			double MaxPreviewCharsDouble = 4000.0;
			Arguments.TryGetBoolField(TEXT("includeText"), bIncludeText);
			Arguments.TryGetNumberField(TEXT("maxPreviewChars"), MaxPreviewCharsDouble);

			FString SkillPath;
			FString FailureReason;
			if (!ResolveSkillPathFromArguments(Arguments, SkillPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TSharedPtr<FJsonObject> SkillObject = MakeSkillInfoObject(SkillPath, bIncludeText, FMath::Clamp(static_cast<int32>(MaxPreviewCharsDouble), 0, 50000));
			SkillObject->SetStringField(TEXT("action"), TEXT("skill_read"));
			return MakeExecutionResult(FString::Printf(TEXT("Read project skill '%s'."), *SkillObject->GetStringField(TEXT("name"))), SkillObject, false);
		}

			FUnrealMcpExecutionResult SkillApply(const FJsonObject& Arguments)
			{
			FString Task;
			FString MemoryKey;
			bool bWriteMemory = true;
			bool bIncludeFullText = true;
			Arguments.TryGetStringField(TEXT("task"), Task);
			Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
			Arguments.TryGetBoolField(TEXT("writeMemory"), bWriteMemory);
			Arguments.TryGetBoolField(TEXT("includeFullText"), bIncludeFullText);

			FString SkillPath;
			FString FailureReason;
			if (!ResolveSkillPathFromArguments(Arguments, SkillPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString SkillText;
			if (!FFileHelper::LoadFileToString(SkillText, *SkillPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read skill '%s'."), *SkillPath), nullptr, true);
			}

			const FString SkillName = SkillNameFromPath(SkillPath);
			if (MemoryKey.TrimStartAndEnd().IsEmpty())
			{
				MemoryKey = FString::Printf(TEXT("skill.%s.last_apply"), *SkillName);
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("skill_apply"));
			StructuredContent->SetStringField(TEXT("skillName"), SkillName);
			StructuredContent->SetStringField(TEXT("skillPath"), SkillPath);
			StructuredContent->SetStringField(TEXT("task"), Task);
			StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
			StructuredContent->SetStringField(TEXT("title"), ExtractSkillTitle(SkillText, SkillName));
			StructuredContent->SetStringField(TEXT("description"), ExtractSkillDescription(SkillText));
			StructuredContent->SetStringField(TEXT("applicationPrompt"), FString::Printf(
				TEXT("Apply the project skill '%s' from %s to the current task. Follow the SKILL.md instructions first, then continue with normal MCP tool safety checks."),
				*SkillName,
				*SkillPath));
			if (bIncludeFullText)
			{
				StructuredContent->SetStringField(TEXT("skillText"), SkillText);
			}
			else
			{
				StructuredContent->SetStringField(TEXT("skillPreview"), SkillText.Left(4000));
			}

			if (bWriteMemory)
			{
				TSharedPtr<FJsonObject> ContentObject = MakeShared<FJsonObject>();
				ContentObject->SetStringField(TEXT("skillName"), SkillName);
				ContentObject->SetStringField(TEXT("skillPath"), SkillPath);
				ContentObject->SetStringField(TEXT("task"), Task);
				ContentObject->SetStringField(TEXT("appliedAtUtc"), FDateTime::UtcNow().ToIso8601());
				TSharedPtr<FJsonObject> MemoryArgs = MakeShared<FJsonObject>();
				MemoryArgs->SetStringField(TEXT("key"), MemoryKey);
				MemoryArgs->SetStringField(TEXT("summary"), FString::Printf(TEXT("Applied project skill %s."), *SkillName));
				MemoryArgs->SetStringField(TEXT("status"), TEXT("applied"));
				MemoryArgs->SetStringField(TEXT("nextStep"), TEXT("Use returned skillText/applicationPrompt as the instruction context for this task."));
				MemoryArgs->SetStringField(TEXT("contentJson"), JsonObjectToString(ContentObject));
				MemoryArgs->SetArrayField(TEXT("tags"), MakeJsonStringArray({ TEXT("skill"), SkillName }));
				FUnrealMcpExecutionResult MemoryResult = ProjectMemoryWrite(*MemoryArgs);
				if (MemoryResult.StructuredContent.IsValid())
				{
					StructuredContent->SetObjectField(TEXT("memoryWrite"), MemoryResult.StructuredContent);
				}
			}

				return MakeExecutionResult(FString::Printf(TEXT("Applied project skill '%s'."), *SkillName), StructuredContent, false);
			}

			FUnrealMcpExecutionResult SkillRecordingStart(const FJsonObject& Arguments)
			{
				FString Goal;
				FString SessionId;
				FString ActivityLogPathToReset;
				bool bReset = true;
				double IntervalSeconds = 60.0;
				Arguments.TryGetStringField(TEXT("goal"), Goal);
				Arguments.TryGetStringField(TEXT("sessionId"), SessionId);
				Arguments.TryGetBoolField(TEXT("reset"), bReset);
				Arguments.TryGetNumberField(TEXT("recordIntervalSeconds"), IntervalSeconds);

				{
					FScopeLock Lock(&GSkillActivityMutex);
					if (bReset || GSkillActivitySessionId.IsEmpty())
					{
						GSkillActivitySessionId = SessionId.TrimStartAndEnd().IsEmpty() ? MakeSkillActivitySessionId() : SessionId.TrimStartAndEnd();
						GSkillActivityStartedAtUtc = FDateTime::UtcNow();
						GSkillActivityLastHeartbeatAtUtc = FDateTime();
						GSkillActivityEventCount = 0;
						GSkillActivityLastLogPath = GetActivityLogPathForSession(GSkillActivitySessionId);
					}
					else if (!SessionId.TrimStartAndEnd().IsEmpty())
					{
						GSkillActivitySessionId = SessionId.TrimStartAndEnd();
						GSkillActivityLastLogPath = GetActivityLogPathForSession(GSkillActivitySessionId);
					}
					if (!Goal.TrimStartAndEnd().IsEmpty())
					{
						GSkillActivityGoal = Goal.TrimStartAndEnd();
					}
						GSkillActivityRecordIntervalSeconds = FMath::Clamp(IntervalSeconds, 10.0, 3600.0);
						GSkillActivityRecording = true;
						GSkillActivityLastError.Empty();
						if (bReset)
						{
							ActivityLogPathToReset = GSkillActivityLastLogPath;
						}
					}

					if (!ActivityLogPathToReset.IsEmpty())
					{
						FString FailureReason;
						if (!ResetActivityLogFile(ActivityLogPathToReset, FailureReason))
						{
							return MakeExecutionResult(FailureReason, nullptr, true);
						}
					}

				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetBoolField(TEXT("reset"), bReset);
				Details->SetNumberField(TEXT("recordIntervalSeconds"), FMath::Clamp(IntervalSeconds, 10.0, 3600.0));
				RecordSkillActivityEvent(TEXT("recording_started"), TEXT("Skill activity recording started."), Details);

				TSharedPtr<FJsonObject> StructuredContent = MakeActivityStateObject();
				StructuredContent->SetStringField(TEXT("action"), TEXT("skill_recording_start"));
				return MakeExecutionResult(TEXT("Skill activity recording is active."), StructuredContent, false);
			}

			FUnrealMcpExecutionResult SkillRecordingStop(const FJsonObject& Arguments)
			{
				FString Reason;
				Arguments.TryGetStringField(TEXT("reason"), Reason);

				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("reason"), Reason);
				RecordSkillActivityEvent(TEXT("recording_stopped"), Reason.TrimStartAndEnd().IsEmpty() ? TEXT("Skill activity recording stopped.") : Reason.TrimStartAndEnd(), Details);

				{
					FScopeLock Lock(&GSkillActivityMutex);
					GSkillActivityRecording = false;
				}

				TSharedPtr<FJsonObject> StructuredContent = MakeActivityStateObject();
				StructuredContent->SetStringField(TEXT("action"), TEXT("skill_recording_stop"));
				return MakeExecutionResult(TEXT("Skill activity recording stopped."), StructuredContent, false);
			}

			FUnrealMcpExecutionResult SkillActivityStatus(const FJsonObject& Arguments)
			{
				bool bIncludeRecentEvents = false;
				double MaxEventsDouble = 20.0;
				Arguments.TryGetBoolField(TEXT("includeRecentEvents"), bIncludeRecentEvents);
				Arguments.TryGetNumberField(TEXT("maxEvents"), MaxEventsDouble);

				TSharedPtr<FJsonObject> StructuredContent = MakeActivityStateObject();
				StructuredContent->SetStringField(TEXT("action"), TEXT("skill_activity_status"));

				if (bIncludeRecentEvents)
				{
					TArray<TSharedPtr<FJsonObject>> Events;
					FString ActivityLogPath;
					FString FailureReason;
					const FString SessionId = StructuredContent->GetStringField(TEXT("sessionId"));
					if (LoadActivityEvents(SessionId, static_cast<int32>(MaxEventsDouble), Events, ActivityLogPath, FailureReason))
					{
						StructuredContent->SetArrayField(TEXT("recentEvents"), MakeActivityEventJsonArray(Events));
						StructuredContent->SetNumberField(TEXT("recentEventCount"), Events.Num());
					}
					else
					{
						StructuredContent->SetStringField(TEXT("recentEventsWarning"), FailureReason);
					}
				}

				return MakeExecutionResult(
					FString::Printf(TEXT("Skill activity recording is %s. Session: %s"), StructuredContent->GetBoolField(TEXT("recording")) ? TEXT("active") : TEXT("stopped"), *StructuredContent->GetStringField(TEXT("sessionId"))),
					StructuredContent,
					false);
			}

			FUnrealMcpExecutionResult SkillDistillFromActivity(const FJsonObject& Arguments)
			{
				FString SessionId;
				FString SkillName;
				FString Title;
				FString Goal;
				bool bWriteDraft = true;
				bool bIncludeEvents = false;
				bool bOverwrite = true;
				double MaxEventsDouble = 200.0;
				Arguments.TryGetStringField(TEXT("sessionId"), SessionId);
				Arguments.TryGetStringField(TEXT("skillName"), SkillName);
				Arguments.TryGetStringField(TEXT("title"), Title);
				Arguments.TryGetStringField(TEXT("goal"), Goal);
				Arguments.TryGetBoolField(TEXT("writeDraft"), bWriteDraft);
				Arguments.TryGetBoolField(TEXT("includeEvents"), bIncludeEvents);
				Arguments.TryGetBoolField(TEXT("overwrite"), bOverwrite);
				Arguments.TryGetNumberField(TEXT("maxEvents"), MaxEventsDouble);

				TArray<TSharedPtr<FJsonObject>> Events;
				FString ActivityLogPath;
				FString FailureReason;
				{
					FScopeLock Lock(&GSkillActivityMutex);
					if (SessionId.TrimStartAndEnd().IsEmpty())
					{
						SessionId = GSkillActivitySessionId;
					}
					if (Goal.TrimStartAndEnd().IsEmpty())
					{
						Goal = GSkillActivityGoal;
					}
				}

				if (!LoadActivityEvents(SessionId, static_cast<int32>(MaxEventsDouble), Events, ActivityLogPath, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}

				if (Goal.TrimStartAndEnd().IsEmpty() && Events.Num() > 0)
				{
					Events[0]->TryGetStringField(TEXT("goal"), Goal);
				}
				if (Title.TrimStartAndEnd().IsEmpty())
				{
					Title = Goal.TrimStartAndEnd().IsEmpty() ? TEXT("Distilled Unreal MCP Workflow") : Goal.TrimStartAndEnd().Left(80);
				}
				if (SkillName.TrimStartAndEnd().IsEmpty())
				{
					SkillName = SanitizeSkillSlug(Title);
				}
				else
				{
					SkillName = SanitizeSkillSlug(SkillName);
				}

				const FString DraftText = BuildDistilledSkillText(SkillName, Title, Goal, SessionId, Events, bIncludeEvents);
				FString DraftPath;
				if (bWriteDraft)
				{
					DraftPath = GetDraftPathForSkill(SkillName);
					if (!WriteSkillFile(DraftPath, DraftText, bOverwrite, FailureReason))
					{
						return MakeExecutionResult(FailureReason, nullptr, true);
					}
				}

				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("sessionId"), SessionId);
				Details->SetStringField(TEXT("skillName"), SkillName);
				Details->SetBoolField(TEXT("writeDraft"), bWriteDraft);
				Details->SetStringField(TEXT("draftPath"), DraftPath);
				RecordSkillActivityEvent(TEXT("skill_distilled"), FString::Printf(TEXT("Distilled activity session into skill draft '%s'."), *SkillName), Details);

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("skill_distill_from_activity"));
				StructuredContent->SetStringField(TEXT("sessionId"), SessionId);
				StructuredContent->SetStringField(TEXT("activityLogPath"), ActivityLogPath);
				StructuredContent->SetNumberField(TEXT("eventCount"), Events.Num());
				StructuredContent->SetStringField(TEXT("skillName"), SkillName);
				StructuredContent->SetStringField(TEXT("title"), Title);
				StructuredContent->SetStringField(TEXT("goal"), Goal);
				StructuredContent->SetBoolField(TEXT("writeDraft"), bWriteDraft);
				StructuredContent->SetStringField(TEXT("draftPath"), DraftPath);
				StructuredContent->SetStringField(TEXT("draftText"), DraftText);
				return MakeExecutionResult(FString::Printf(TEXT("Distilled %d activity events into skill '%s'."), Events.Num(), *SkillName), StructuredContent, false);
			}

			FUnrealMcpExecutionResult SkillSaveDraft(const FJsonObject& Arguments)
			{
				FString SkillName;
				FString Title;
				FString Goal;
				FString Summary;
				FString DraftText;
				bool bOverwrite = true;
				Arguments.TryGetStringField(TEXT("skillName"), SkillName);
				Arguments.TryGetStringField(TEXT("title"), Title);
				Arguments.TryGetStringField(TEXT("goal"), Goal);
				Arguments.TryGetStringField(TEXT("summary"), Summary);
				Arguments.TryGetStringField(TEXT("draftText"), DraftText);
				Arguments.TryGetBoolField(TEXT("overwrite"), bOverwrite);

				if (SkillName.TrimStartAndEnd().IsEmpty())
				{
					return MakeExecutionResult(TEXT("skillName is required."), nullptr, true);
				}
				SkillName = SanitizeSkillSlug(SkillName);

				if (DraftText.TrimStartAndEnd().IsEmpty())
				{
					const FString SafeTitle = Title.TrimStartAndEnd().IsEmpty() ? SkillName : Title.TrimStartAndEnd();
					const FString GoalText = Goal.TrimStartAndEnd().IsEmpty() ? TEXT("No goal provided.") : Goal.TrimStartAndEnd();
					const FString SummaryText = Summary.TrimStartAndEnd().IsEmpty() ? TEXT("No summary provided.") : Summary.TrimStartAndEnd();
					DraftText = FString::Printf(
						TEXT("# %s\n\n")
						TEXT("Use this skill when repeating the following UEvolve / Unreal MCP workflow.\n\n")
						TEXT("## Goal\n%s\n\n")
						TEXT("## Summary\n%s\n\n")
						TEXT("## Reusable Steps\n- Inspect current project state.\n- Apply the workflow carefully with dry runs where available.\n- Save, test, and document the result.\n"),
						*SafeTitle,
						*GoalText,
						*SummaryText);
				}

				FString FailureReason;
				const FString DraftPath = GetDraftPathForSkill(SkillName);
				if (!WriteSkillFile(DraftPath, DraftText, bOverwrite, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}

				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("skillName"), SkillName);
				Details->SetStringField(TEXT("draftPath"), DraftPath);
				RecordSkillActivityEvent(TEXT("skill_draft_saved"), FString::Printf(TEXT("Saved skill draft '%s'."), *SkillName), Details);

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("skill_save_draft"));
				StructuredContent->SetStringField(TEXT("skillName"), SkillName);
				StructuredContent->SetStringField(TEXT("draftPath"), DraftPath);
				StructuredContent->SetObjectField(TEXT("file"), MakeFileInfoObject(DraftPath));
				return MakeExecutionResult(FString::Printf(TEXT("Saved skill draft '%s'."), *SkillName), StructuredContent, false);
			}

			FUnrealMcpExecutionResult SkillPromoteDraft(const FJsonObject& Arguments)
			{
				FString SkillName;
				FString DraftPath;
				bool bOverwrite = false;
				bool bDryRun = true;
				bool bCreateBackup = true;
				Arguments.TryGetStringField(TEXT("skillName"), SkillName);
				Arguments.TryGetStringField(TEXT("draftPath"), DraftPath);
				Arguments.TryGetBoolField(TEXT("overwrite"), bOverwrite);
				Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
				Arguments.TryGetBoolField(TEXT("createBackup"), bCreateBackup);

				if (SkillName.TrimStartAndEnd().IsEmpty())
				{
					return MakeExecutionResult(TEXT("skillName is required."), nullptr, true);
				}
				SkillName = SanitizeSkillSlug(SkillName);

				FString ResolvedDraftPath;
				FString FailureReason;
				if (DraftPath.TrimStartAndEnd().IsEmpty())
				{
					ResolvedDraftPath = GetDraftPathForSkill(SkillName);
				}
				else if (!ResolveProjectPathInsideProject(DraftPath, ResolvedDraftPath, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}

				FString DraftText;
				if (!FFileHelper::LoadFileToString(DraftText, *ResolvedDraftPath))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to read draft %s."), *ResolvedDraftPath), nullptr, true);
				}

				const FString PromotedPath = GetPromotedSkillPath(SkillName);
				const bool bPromotedExists = FPaths::FileExists(PromotedPath);
				FString ExistingPromotedText;
				const bool bReadExisting = bPromotedExists && FFileHelper::LoadFileToString(ExistingPromotedText, *PromotedPath);
				const bool bSameContent = bReadExisting && ExistingPromotedText.Equals(DraftText, ESearchCase::CaseSensitive);
				const bool bWouldOverwrite = bPromotedExists && !bSameContent;
				const bool bChanged = !bSameContent;
				FString BackupPath;
				FString ManifestPath;

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("skill_promote_draft"));
				StructuredContent->SetStringField(TEXT("skillName"), SkillName);
				StructuredContent->SetStringField(TEXT("draftPath"), ResolvedDraftPath);
				StructuredContent->SetStringField(TEXT("promotedPath"), PromotedPath);
				StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
				StructuredContent->SetBoolField(TEXT("createBackup"), bCreateBackup);
				StructuredContent->SetBoolField(TEXT("overwrite"), bOverwrite);
				StructuredContent->SetBoolField(TEXT("promotedExists"), bPromotedExists);
				StructuredContent->SetBoolField(TEXT("sameContent"), bSameContent);
				StructuredContent->SetBoolField(TEXT("wouldOverwrite"), bWouldOverwrite);
				StructuredContent->SetBoolField(TEXT("changed"), bChanged);
				StructuredContent->SetObjectField(TEXT("draftFile"), MakeFileInfoObject(ResolvedDraftPath));

				if (bPromotedExists && !bSameContent && !bOverwrite)
				{
					StructuredContent->SetObjectField(TEXT("existingFile"), MakeFileInfoObject(PromotedPath));
					return MakeExecutionResult(
						FString::Printf(TEXT("Refusing to overwrite existing promoted skill %s. Re-run with overwrite=true after review."), *PromotedPath),
						StructuredContent,
						true);
				}

				if (bDryRun)
				{
					return MakeExecutionResult(
						FString::Printf(TEXT("Dry run: would promote skill draft '%s'%s."), *SkillName, bWouldOverwrite ? TEXT(" and overwrite existing promoted skill") : TEXT("")),
						StructuredContent,
						false);
				}

				if (bPromotedExists && bChanged && bCreateBackup)
				{
					const FString BackupDir = MakeSkillPromotionBackupDir(SkillName);
					BackupPath = FPaths::Combine(BackupDir, TEXT("SKILL.md"));
					ManifestPath = FPaths::Combine(BackupDir, TEXT("Manifest.json"));
					IFileManager::Get().MakeDirectory(*BackupDir, true);
					if (IFileManager::Get().Copy(*BackupPath, *PromotedPath, true, true) != COPY_OK)
					{
						return MakeExecutionResult(FString::Printf(TEXT("Failed to back up existing promoted skill to %s."), *BackupPath), StructuredContent, true);
					}
					if (!WriteSkillPromotionManifest(ManifestPath, SkillName, ResolvedDraftPath, PromotedPath, BackupPath, false, bPromotedExists, bChanged, bOverwrite, FailureReason))
					{
						return MakeExecutionResult(FailureReason, StructuredContent, true);
					}
					StructuredContent->SetStringField(TEXT("backupPath"), BackupPath);
					StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
					StructuredContent->SetObjectField(TEXT("backupFile"), MakeFileInfoObject(BackupPath));
				}
				else if (bChanged)
				{
					const FString BackupDir = MakeSkillPromotionBackupDir(SkillName);
					ManifestPath = FPaths::Combine(BackupDir, TEXT("Manifest.json"));
					if (!WriteSkillPromotionManifest(ManifestPath, SkillName, ResolvedDraftPath, PromotedPath, BackupPath, false, bPromotedExists, bChanged, bOverwrite, FailureReason))
					{
						return MakeExecutionResult(FailureReason, StructuredContent, true);
					}
					StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
				}

				if (bChanged && !WriteSkillFile(PromotedPath, DraftText, true, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}

				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("skillName"), SkillName);
				Details->SetStringField(TEXT("draftPath"), ResolvedDraftPath);
				Details->SetStringField(TEXT("promotedPath"), PromotedPath);
				Details->SetBoolField(TEXT("changed"), bChanged);
				Details->SetStringField(TEXT("manifestPath"), ManifestPath);
				RecordSkillActivityEvent(TEXT("skill_draft_promoted"), FString::Printf(TEXT("Promoted skill draft '%s' into Tools/UnrealMcpSkills."), *SkillName), Details);

				StructuredContent->SetObjectField(TEXT("file"), MakeFileInfoObject(PromotedPath));
				return MakeExecutionResult(
					bChanged
						? FString::Printf(TEXT("Promoted skill draft '%s'."), *SkillName)
						: FString::Printf(TEXT("Promoted skill draft '%s' is already up to date."), *SkillName),
					StructuredContent,
					false);
			}
	}
