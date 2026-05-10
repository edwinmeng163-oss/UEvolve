#include "UnrealMcpActivityLog.h"

#include "UnrealMcpModule.h"
#include "UnrealMcpSession.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UnrealMcp
{
	namespace
	{
		FCriticalSection GActivityLogMutex;
		TMap<FString, int32> GActivityLogEntryCounts;

		FString GetActivityLogRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ActivityLog")));
		}

		FString SanitizeActivitySessionId(FString Value)
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
			return Result.IsEmpty() ? TEXT("activity-session") : Result.Left(80);
		}

		FString GetActivityLogPathForSession(const FString& SessionId)
		{
			return FPaths::Combine(GetActivityLogRoot(), SanitizeActivitySessionId(SessionId) + TEXT(".jsonl"));
		}

		int32 CountJsonLines(const FString& Path)
		{
			TArray<FString> Lines;
			if (!FFileHelper::LoadFileToStringArray(Lines, *Path))
			{
				return 0;
			}

			int32 Count = 0;
			for (const FString& Line : Lines)
			{
				if (!Line.TrimStartAndEnd().IsEmpty())
				{
					++Count;
				}
			}
			return Count;
		}

		int32 GetInitializedEntryCount(const FString& SessionId, const FString& ActivityLogPath)
		{
			if (const int32* ExistingCount = GActivityLogEntryCounts.Find(SessionId))
			{
				return *ExistingCount;
			}

			const int32 InitialCount = CountJsonLines(ActivityLogPath);
			GActivityLogEntryCounts.Add(SessionId, InitialCount);
			return InitialCount;
		}

		int32 GetNextRolloverIndex(const FString& SessionId, const FString& ActivityLogRoot)
		{
			const FString SafeSessionId = SanitizeActivitySessionId(SessionId);
			int32 Index = 1;
			while (FPaths::FileExists(FPaths::Combine(ActivityLogRoot, FString::Printf(TEXT("%s.%d.jsonl"), *SafeSessionId, Index))))
			{
				++Index;
			}
			return Index;
		}

		bool RotateActivityLog(const FString& SessionId, const FString& ActivityLogPath, FString& OutFailureReason)
		{
			if (!FPaths::FileExists(ActivityLogPath))
			{
				GActivityLogEntryCounts.Add(SessionId, 0);
				return true;
			}

			const FString ActivityLogRoot = FPaths::GetPath(ActivityLogPath);
			const FString SafeSessionId = SanitizeActivitySessionId(SessionId);
			int32 RolloverIndex = GetNextRolloverIndex(SessionId, ActivityLogRoot);
			FString RolloverPath;
			do
			{
				RolloverPath = FPaths::Combine(ActivityLogRoot, FString::Printf(TEXT("%s.%d.jsonl"), *SafeSessionId, RolloverIndex));
				++RolloverIndex;
			}
			while (FPaths::FileExists(RolloverPath));

			if (!IFileManager::Get().Move(*RolloverPath, *ActivityLogPath, false, true))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to rotate activity log %s to %s."), *ActivityLogPath, *RolloverPath);
				return false;
			}

			GActivityLogEntryCounts.Add(SessionId, 0);
			return true;
		}

		bool SerializeCompactJson(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
		{
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
			return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		}

		bool ShouldWriteLegacyAliases(const FActivityLogEvent& Event)
		{
			return !Event.LegacyEventType.TrimStartAndEnd().IsEmpty() || !Event.LegacyGoal.TrimStartAndEnd().IsEmpty();
		}

		TSharedPtr<FJsonObject> MakeActivityRecord(const FString& SessionId, const FActivityLogEvent& Event, const FString& TimestampUtc)
		{
			const FString EventKind = Event.EventKind.TrimStartAndEnd().IsEmpty()
				? Event.LegacyEventType.TrimStartAndEnd()
				: Event.EventKind.TrimStartAndEnd();

			TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
			Record->SetStringField(TEXT("sessionId"), SessionId);
			Record->SetStringField(TEXT("ts"), TimestampUtc);
			Record->SetStringField(TEXT("eventKind"), EventKind);
			if (!Event.TaskLabel.TrimStartAndEnd().IsEmpty())
			{
				Record->SetStringField(TEXT("taskLabel"), Event.TaskLabel.TrimStartAndEnd());
			}
			Record->SetStringField(TEXT("summary"), Event.Summary.Left(2000));
			if (Event.Payload.IsValid())
			{
				Record->SetObjectField(TEXT("payload"), Event.Payload);
			}
			if (Event.Refs.IsValid())
			{
				Record->SetObjectField(TEXT("refs"), Event.Refs);
			}
			if (Event.Correlation.IsValid())
			{
				Record->SetObjectField(TEXT("correlation"), Event.Correlation);
			}

			if (ShouldWriteLegacyAliases(Event))
			{
				Record->SetStringField(TEXT("timestampUtc"), TimestampUtc);
				if (!Event.LegacyEventType.TrimStartAndEnd().IsEmpty())
				{
					Record->SetStringField(TEXT("eventType"), Event.LegacyEventType.TrimStartAndEnd());
				}
				if (!Event.LegacyGoal.TrimStartAndEnd().IsEmpty())
				{
					Record->SetStringField(TEXT("goal"), Event.LegacyGoal.TrimStartAndEnd());
				}
				if (Event.Payload.IsValid())
				{
					Record->SetObjectField(TEXT("details"), Event.Payload);
				}
			}

			return Record;
		}
	}

	void WriteActivityEvent(const FActivityLogEvent& Event)
	{
		FString FailureReason;
		if (!TryWriteActivityEvent(Event, FailureReason))
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("%s"), *FailureReason);
		}
	}

	bool TryWriteActivityEvent(const FActivityLogEvent& Event, FString& OutFailureReason)
	{
		return TryWriteActivityEventForSession(GetLaunchSessionId(), Event, OutFailureReason);
	}

	bool TryWriteActivityEventForSession(const FString& SessionId, const FActivityLogEvent& Event, FString& OutFailureReason)
	{
		const FString EffectiveSessionId = SessionId.TrimStartAndEnd().IsEmpty() ? GetLaunchSessionId() : SessionId.TrimStartAndEnd();
		const FString EffectiveEventKind = Event.EventKind.TrimStartAndEnd().IsEmpty()
			? Event.LegacyEventType.TrimStartAndEnd()
			: Event.EventKind.TrimStartAndEnd();
		if (EffectiveEventKind.IsEmpty())
		{
			OutFailureReason = TEXT("Activity eventKind is required.");
			return false;
		}

		const FString TimestampUtc = FDateTime::UtcNow().ToIso8601();
		const TSharedPtr<FJsonObject> Record = MakeActivityRecord(EffectiveSessionId, Event, TimestampUtc);

		FString CompactJson;
		if (!SerializeCompactJson(Record, CompactJson))
		{
			OutFailureReason = TEXT("Failed to serialize activity event.");
			return false;
		}

		const FString Line = CompactJson + LINE_TERMINATOR;
		const FTCHARToUTF8 Utf8Line(*Line);
		const int64 LineBytes = Utf8Line.Length();
		const FString ActivityLogPath = GetActivityLogPathForSession(EffectiveSessionId);

		FScopeLock Lock(&GActivityLogMutex);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ActivityLogPath), true);

		const int64 FileSize = IFileManager::Get().FileSize(*ActivityLogPath);
		const bool bFileExists = FileSize >= 0;
		if (!bFileExists || FileSize == 0)
		{
			GActivityLogEntryCounts.Add(EffectiveSessionId, 0);
		}
		const int32 EntryCount = GetInitializedEntryCount(EffectiveSessionId, ActivityLogPath);
		const bool bRotateForEntryCount = EntryCount >= kActivityLogMaxEntries;
		const bool bRotateForSize = bFileExists && FileSize > 0 && FileSize + LineBytes > kActivityLogMaxFileBytes;
		if ((bRotateForEntryCount || bRotateForSize) && !RotateActivityLog(EffectiveSessionId, ActivityLogPath, OutFailureReason))
		{
			return false;
		}

		if (!FFileHelper::SaveStringToFile(Line, *ActivityLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))
		{
			OutFailureReason = FString::Printf(TEXT("Failed to append activity event to %s."), *ActivityLogPath);
			return false;
		}

		GActivityLogEntryCounts.FindOrAdd(EffectiveSessionId)++;
		return true;
	}
}
