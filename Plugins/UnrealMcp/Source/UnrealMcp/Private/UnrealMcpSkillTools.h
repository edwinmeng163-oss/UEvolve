#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpModule.h"

class FJsonObject;

namespace UnrealMcp
{
	FUnrealMcpExecutionResult SkillList(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SkillRead(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SkillApply(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SkillRecordingStart(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SkillRecordingStop(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SkillActivityStatus(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SkillDistillFromActivity(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SkillSaveDraft(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SkillPromoteDraft(const FJsonObject& Arguments);
	bool TryExecuteSkillTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult);
	void RecordSkillActivityEvent(const FString& EventType, const FString& Summary, const TSharedPtr<FJsonObject>& Details = nullptr);
	void TickSkillActivityRecorder();
}
