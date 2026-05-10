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
	FUnrealMcpExecutionResult KnowledgeIndexRefresh(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult KnowledgeSearch(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ToolRecommend(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult ToolGapAnalyze(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult WorkflowRecommend(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult KnowledgeEvalRun(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	bool TryExecuteSelfExtensionTool(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
		const FSelfExtensionModuleToolRunner& RunToolTest,
		const FSelfExtensionModuleToolRunner& RunTestSuite,
		const FSelfExtensionModuleToolRunner& RunExtensionPipeline,
		const FSelfExtensionModuleToolRunner& RunWorkflow,
		FUnrealMcpExecutionResult& OutResult);
}
