#ifndef UNREAL_MCP_PYTHON_TOOL_BRIDGE_H
#define UNREAL_MCP_PYTHON_TOOL_BRIDGE_H

class FJsonObject;
struct FUnrealMcpExecutionResult;

namespace UnrealMcp
{
	struct FToolHandlerRegistryEntry;

	namespace UnrealMcpPythonToolBridge
	{
		FUnrealMcpExecutionResult ExecutePythonRegisteredTool(const FToolHandlerRegistryEntry& HandlerEntry, const FJsonObject& Arguments);
	}
}

#endif // UNREAL_MCP_PYTHON_TOOL_BRIDGE_H
