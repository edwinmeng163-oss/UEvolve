#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UnrealMcp
{
	TArray<TSharedPtr<FJsonObject>> BuildEvidenceForTask(
		const FString& TaskQuery,
		int32 TopN = 3,
		int32 MaxExcerptChars = 600);

	bool WriteOutcomeKnowledgeCard(
		const FString& ManifestSessionId,
		const FString& Title,
		const FString& Text,
		const FString& SourcePath,
		const TArray<FString>& Tags,
		FString& OutFailureReason);
}
