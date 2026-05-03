#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UnrealMcp
{
	struct FToolHandlerRegistryEntry
	{
		FString HandlerName;
		FString Category;
		FString SourceFile;
	};

	const TArray<FToolHandlerRegistryEntry>& GetToolHandlerRegistryEntries();
	const FToolHandlerRegistryEntry* FindToolHandlerRegistryEntry(const FString& HandlerName);
	bool IsRegisteredToolHandler(const FString& HandlerName);
	TSharedPtr<FJsonObject> MakeToolHandlerRegistryStatusObject();
}
