#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpModule.h"

class FJsonObject;
class FJsonValue;

namespace UnrealMcp
{
	using FSelfExtensionModuleToolRunner = TFunction<FUnrealMcpExecutionResult(const FJsonObject& Arguments)>;

	FUnrealMcpExecutionResult PipelineStatus(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult WorkbenchStatus(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	bool TryExecuteSelfExtensionTool(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
		const FSelfExtensionModuleToolRunner& RunToolTest,
		const FSelfExtensionModuleToolRunner& RunTestSuite,
		const FSelfExtensionModuleToolRunner& RunExtensionPipeline,
		FUnrealMcpExecutionResult& OutResult);
}
