FUnrealMcpExecutionResult ExecuteGeneratedRegistryPatchFixtureTool(const FString& ToolName, const FJsonObject& Arguments)
{
	FString Message;
	Arguments.TryGetStringField(TEXT("message"), Message);

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("registry_patch_fixture"));
	StructuredContent->SetStringField(TEXT("toolName"), ToolName);
	StructuredContent->SetStringField(TEXT("message"), Message);

	FUnrealMcpExecutionResult Result;
	Result.Text = FString::Printf(TEXT("Registry patch fixture completed. message=%s"), *Message);
	Result.StructuredContent = StructuredContent;
	Result.bIsError = false;
	return Result;
}
