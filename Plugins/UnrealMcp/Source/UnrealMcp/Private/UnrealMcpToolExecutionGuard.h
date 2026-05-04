#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FUnrealMcpExecutionResult;

namespace UnrealMcp
{
	TSharedPtr<FJsonObject> BuildToolExecutionPreflight(
		const FString& RequestedToolName,
		const FJsonObject& Arguments);

	void AttachToolExecutionCheck(
		const FString& RequestedToolName,
		const FJsonObject& Arguments,
		FUnrealMcpExecutionResult& Result);

	void AttachToolExecutionCheck(
		const FString& RequestedToolName,
		const FJsonObject& Arguments,
		const TSharedPtr<FJsonObject>& PreflightBeforeExecution,
		FUnrealMcpExecutionResult& Result);
}
