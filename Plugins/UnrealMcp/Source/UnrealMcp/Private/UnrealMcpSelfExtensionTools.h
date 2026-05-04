#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpModule.h"

class FJsonObject;
class FJsonValue;

namespace UnrealMcp
{
	FUnrealMcpExecutionResult PipelineStatus(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult WorkbenchStatus(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	bool TryExecuteSelfExtensionTool(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
		FUnrealMcpExecutionResult& OutResult);
}
