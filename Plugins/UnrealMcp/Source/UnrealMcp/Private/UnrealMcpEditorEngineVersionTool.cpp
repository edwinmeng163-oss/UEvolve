#include "UnrealMcpModule.h"

#include "Dom/JsonObject.h"
#include "Misc/EngineVersion.h"
#include "Runtime/Launch/Resources/Version.h"

namespace UnrealMcp
{
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);

	FUnrealMcpExecutionResult ExecuteEditorEngineVersion()
	{
		const FString VersionString = FEngineVersion::Current().ToString();

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetNumberField(TEXT("major"), ENGINE_MAJOR_VERSION);
		StructuredContent->SetNumberField(TEXT("minor"), ENGINE_MINOR_VERSION);
		StructuredContent->SetNumberField(TEXT("patch"), ENGINE_PATCH_VERSION);
		StructuredContent->SetStringField(TEXT("version_string"), VersionString);

		const FString Text = FString::Printf(
			TEXT("Unreal Engine version: %d.%d.%d (%s)"),
			ENGINE_MAJOR_VERSION,
			ENGINE_MINOR_VERSION,
			ENGINE_PATCH_VERSION,
			*VersionString);

		return MakeExecutionResult(Text, StructuredContent, false);
	}
}
