#pragma once

#include "CoreMinimal.h"
#include "UnrealMcpToolDescriptor.h"

class FJsonObject;
class FJsonValue;

namespace UnrealMcp
{
	struct FRegisteredUnrealMcpToolDescriptor
	{
		FUnrealMcpToolDescriptor Descriptor;
		TSharedPtr<FJsonObject> InputSchema;
	};

	class FUnrealMcpToolRegistrar
	{
	public:
		void Add(const FUnrealMcpToolDescriptor& Descriptor, const TSharedPtr<FJsonObject>& InputSchema);
		const TArray<FRegisteredUnrealMcpToolDescriptor>& GetTools() const;

	private:
		TArray<FRegisteredUnrealMcpToolDescriptor> Tools;
	};

	const TArray<FRegisteredUnrealMcpToolDescriptor>& GetRegisteredMcpToolDescriptors();
	const FRegisteredUnrealMcpToolDescriptor* FindRegisteredMcpToolDescriptor(const FString& ToolName);
	void AppendRegisteredToolDefinitions(TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	TSharedPtr<FJsonObject> MakeToolDescriptorStatusObject();
	FString LexToString(EUnrealMcpToolExposure Exposure);
	FString LexToString(EUnrealMcpToolRisk Risk);
	FString LexToString(EUnrealMcpToolTestCoverage TestCoverage);
}
