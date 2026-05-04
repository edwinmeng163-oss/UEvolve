#include "UnrealMcpMemoryTools.h"

#include "UnrealMcpModule.h"

#include "Dom/JsonObject.h"

namespace UnrealMcp
{
	FUnrealMcpExecutionResult ProjectMemoryWrite(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ProjectMemoryRead(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ProjectMemoryView(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ProjectMemoryEdit(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ProjectMemoryDelete(const FJsonObject& Arguments);

	bool TryExecuteMemoryTool(const FString& ToolName, const FJsonObject& Arguments, FUnrealMcpExecutionResult& OutResult)
	{
		if (ToolName == TEXT("unreal.project_memory_write"))
		{
			OutResult = ProjectMemoryWrite(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.project_memory_read"))
		{
			OutResult = ProjectMemoryRead(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.project_memory_view"))
		{
			OutResult = ProjectMemoryView(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.project_memory_edit"))
		{
			OutResult = ProjectMemoryEdit(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.project_memory_delete"))
		{
			OutResult = ProjectMemoryDelete(Arguments);
			return true;
		}

		return false;
	}
}
