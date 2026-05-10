#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UnrealMcp
{
	constexpr int64 kActivityLogMaxFileBytes = 10LL * 1024LL * 1024LL;
	constexpr int32 kActivityLogMaxEntries = 5000;

	struct FActivityLogEvent
	{
		FString EventKind;
		FString Summary;
		TSharedPtr<FJsonObject> Payload;
		TSharedPtr<FJsonObject> Refs;
		TSharedPtr<FJsonObject> Correlation;
		FString TaskLabel;
		FString LegacyEventType;
		FString LegacyGoal;
	};

	void WriteActivityEvent(const FActivityLogEvent& Event);
	bool TryWriteActivityEvent(const FActivityLogEvent& Event, FString& OutFailureReason);
	bool TryWriteActivityEventForSession(const FString& SessionId, const FActivityLogEvent& Event, FString& OutFailureReason);
}
