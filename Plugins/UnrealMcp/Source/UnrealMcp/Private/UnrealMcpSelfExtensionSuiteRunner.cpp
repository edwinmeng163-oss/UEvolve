#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
	FString GetJsonValueTypeName(EJson JsonType)
	{
		switch (JsonType)
		{
		case EJson::None:
			return TEXT("none");
		case EJson::Null:
			return TEXT("null");
		case EJson::String:
			return TEXT("string");
		case EJson::Number:
			return TEXT("number");
		case EJson::Boolean:
			return TEXT("boolean");
		case EJson::Array:
			return TEXT("array");
		case EJson::Object:
			return TEXT("object");
		default:
			return TEXT("unknown");
		}
	}
	FString DescribeJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("<missing>");
		}
		if (Value->Type == EJson::String)
		{
			return Value->AsString();
		}
		if (Value->Type == EJson::Number)
		{
			return FString::SanitizeFloat(Value->AsNumber());
		}
		if (Value->Type == EJson::Boolean)
		{
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		}
		if (Value->Type == EJson::Null)
		{
			return TEXT("null");
		}
		return FString::Printf(TEXT("<%s>"), *GetJsonValueTypeName(Value->Type));
	}

	bool TryGetNestedJsonValue(const TSharedPtr<FJsonObject>& RootObject, const FString& FieldPath, TSharedPtr<FJsonValue>& OutValue)
	{
		if (!RootObject.IsValid())
		{
			return false;
		}

		TArray<FString> Segments;
		FieldPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		TSharedPtr<FJsonObject> CurrentObject = RootObject;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FString& Segment = Segments[Index];
			TSharedPtr<FJsonValue> FieldValue = CurrentObject->TryGetField(Segment);
			if (!FieldValue.IsValid())
			{
				return false;
			}
			if (Index == Segments.Num() - 1)
			{
				OutValue = FieldValue;
				return true;
			}
			if (FieldValue->Type != EJson::Object || !FieldValue->AsObject().IsValid())
			{
				return false;
			}
			CurrentObject = FieldValue->AsObject();
		}

		return false;
	}

	bool JsonScalarValuesMatch(const TSharedPtr<FJsonValue>& ActualValue, const TSharedPtr<FJsonValue>& ExpectedValue)
	{
		if (!ActualValue.IsValid() || !ExpectedValue.IsValid() || ActualValue->Type != ExpectedValue->Type)
		{
			return false;
		}
		if (ExpectedValue->Type == EJson::String)
		{
			return ActualValue->AsString() == ExpectedValue->AsString();
		}
		if (ExpectedValue->Type == EJson::Number)
		{
			return FMath::IsNearlyEqual(ActualValue->AsNumber(), ExpectedValue->AsNumber());
		}
		if (ExpectedValue->Type == EJson::Boolean)
		{
			return ActualValue->AsBool() == ExpectedValue->AsBool();
		}
		return ExpectedValue->Type == EJson::Null;
	}

	bool EvaluateExpectedStructuredFields(
		const TSharedPtr<FJsonObject>& ActualStructuredContent,
		const TSharedPtr<FJsonObject>& ExpectedFieldsObject,
		TArray<TSharedPtr<FJsonValue>>& OutChecks)
	{
		bool bAllMatched = true;
		if (!ExpectedFieldsObject.IsValid())
		{
			return true;
		}

		TArray<FString> ExpectedPaths;
		ExpectedFieldsObject->Values.GetKeys(ExpectedPaths);
		ExpectedPaths.Sort();
		for (const FString& ExpectedPath : ExpectedPaths)
		{
			const TSharedPtr<FJsonValue> ExpectedValue = ExpectedFieldsObject->TryGetField(ExpectedPath);
			TSharedPtr<FJsonValue> ActualValue;
			const bool bFound = TryGetNestedJsonValue(ActualStructuredContent, ExpectedPath, ActualValue);
			const bool bMatched = bFound && JsonScalarValuesMatch(ActualValue, ExpectedValue);
			bAllMatched = bAllMatched && bMatched;

			TSharedPtr<FJsonObject> CheckObject = MakeShared<FJsonObject>();
			CheckObject->SetStringField(TEXT("path"), ExpectedPath);
			CheckObject->SetBoolField(TEXT("found"), bFound);
			CheckObject->SetBoolField(TEXT("matched"), bMatched);
			CheckObject->SetStringField(TEXT("expectedType"), ExpectedValue.IsValid() ? GetJsonValueTypeName(ExpectedValue->Type) : TEXT("missing"));
			CheckObject->SetStringField(TEXT("actualType"), ActualValue.IsValid() ? GetJsonValueTypeName(ActualValue->Type) : TEXT("missing"));
			CheckObject->SetStringField(TEXT("expected"), DescribeJsonValue(ExpectedValue));
			CheckObject->SetStringField(TEXT("actual"), DescribeJsonValue(ActualValue));
			OutChecks.Add(MakeShared<FJsonValueObject>(CheckObject));
		}
		return bAllMatched;
	}
}

FUnrealMcpExecutionResult FUnrealMcpModule::RunMcpToolTest(const FJsonObject& Arguments) const
{
	FString ToolName;
	FString TestRequestPath;
	FString ScaffoldDir;
	FString OutputRoot = TEXT("Tools/UnrealMcpToolScaffolds");
	FString MemoryKey = TEXT("mcp.extension.build_test");
	bool bReadProjectMemory = true;
	bool bWriteProjectMemory = true;
	bool bExecuteTool = true;
	bool bExpectToolListed = true;
	bool bRunSuite = false;

	Arguments.TryGetStringField(TEXT("toolName"), ToolName);
	Arguments.TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
	Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
	Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
	Arguments.TryGetBoolField(TEXT("readProjectMemory"), bReadProjectMemory);
	Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
	Arguments.TryGetBoolField(TEXT("executeTool"), bExecuteTool);
	Arguments.TryGetBoolField(TEXT("expectToolListed"), bExpectToolListed);
	Arguments.TryGetBoolField(TEXT("runSuite"), bRunSuite);

	if (bRunSuite)
	{
		return RunMcpTestSuite(Arguments);
	}

	ToolName = ToolName.TrimStartAndEnd();
	TestRequestPath = TestRequestPath.TrimStartAndEnd();
	ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
	MemoryKey = MemoryKey.TrimStartAndEnd();
	if (MemoryKey.IsEmpty())
	{
		MemoryKey = TEXT("mcp.extension.build_test");
	}

	TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
	if (bReadProjectMemory)
	{
		FString FailureReason;
		TSharedPtr<FJsonObject> MemoryObject;
		if (UnrealMcp::LoadProjectMemory(MemoryObject, FailureReason) && MemoryObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (MemoryObject->TryGetArrayField(TEXT("entries"), Entries) && Entries)
			{
				for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
				{
					if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
					{
						continue;
					}

					TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
					FString ExistingKey;
					if (!EntryObject->TryGetStringField(TEXT("key"), ExistingKey) || ExistingKey != MemoryKey)
					{
						continue;
					}

					const TSharedPtr<FJsonObject>* ContentObject = nullptr;
					if (EntryObject->TryGetObjectField(TEXT("content"), ContentObject) && ContentObject && (*ContentObject).IsValid())
					{
						MemoryContent = *ContentObject;
						if (ToolName.IsEmpty())
						{
							MemoryContent->TryGetStringField(TEXT("toolName"), ToolName);
						}
						if (TestRequestPath.IsEmpty())
						{
							MemoryContent->TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
						}
						if (ScaffoldDir.IsEmpty())
						{
							MemoryContent->TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
						}
					}
					break;
				}
			}
		}
	}

	if (TestRequestPath.IsEmpty())
	{
		if (!ScaffoldDir.IsEmpty())
		{
			FString ResolvedScaffoldDir;
			FString FailureReason;
			if (!UnrealMcp::ResolveProjectPathInsideProject(ScaffoldDir, ResolvedScaffoldDir, FailureReason))
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}
			TestRequestPath = FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json"));
		}
		else if (!ToolName.IsEmpty())
		{
			FString ResolvedOutputRoot;
			FString FailureReason;
			if (!UnrealMcp::ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, FailureReason))
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}
			TestRequestPath = FPaths::Combine(ResolvedOutputRoot, UnrealMcp::SanitizeMcpToolIdForPath(ToolName), TEXT("TestRequest.json"));
		}
	}

	FString ResolvedTestRequestPath;
	FString ResolveFailure;
	if (!UnrealMcp::ResolveProjectPathInsideProject(TestRequestPath, ResolvedTestRequestPath, ResolveFailure))
	{
		return UnrealMcp::MakeExecutionResult(ResolveFailure, nullptr, true);
	}

	FString TestRequestText;
	if (!FFileHelper::LoadFileToString(TestRequestText, *ResolvedTestRequestPath))
	{
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to read TestRequest.json at '%s'."), *ResolvedTestRequestPath), nullptr, true);
	}

	TSharedPtr<FJsonObject> TestRequestObject;
	if (!UnrealMcp::LoadJsonObject(TestRequestText, TestRequestObject) || !TestRequestObject.IsValid())
	{
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Test request '%s' is not valid JSON."), *ResolvedTestRequestPath), nullptr, true);
	}

	TSharedPtr<FJsonObject> TestCaseObject;
	TSharedPtr<FJsonObject> RequestObject = TestRequestObject;
	FString TestName = FPaths::GetBaseFilename(ResolvedTestRequestPath);
	FString TestDescription;
	FString ExpectationNote;
	bool bExpectToolCallError = false;
	bool bHasExpectToolCallError = false;
	bool bHasExpectedStructuredFields = false;
	bool bStructuredFieldsOk = true;
	const TSharedPtr<FJsonObject>* WrappedRequestObject = nullptr;
	if (TestRequestObject->TryGetObjectField(TEXT("request"), WrappedRequestObject) && WrappedRequestObject && (*WrappedRequestObject).IsValid())
	{
		TestCaseObject = TestRequestObject;
		RequestObject = *WrappedRequestObject;
		TestCaseObject->TryGetStringField(TEXT("name"), TestName);
		TestCaseObject->TryGetStringField(TEXT("description"), TestDescription);
		TestCaseObject->TryGetStringField(TEXT("expectationNote"), ExpectationNote);
		TestCaseObject->TryGetBoolField(TEXT("executeTool"), bExecuteTool);
		TestCaseObject->TryGetBoolField(TEXT("expectToolListed"), bExpectToolListed);
		if (TestCaseObject->TryGetBoolField(TEXT("expectToolCallError"), bExpectToolCallError)
			|| TestCaseObject->TryGetBoolField(TEXT("expectError"), bExpectToolCallError))
		{
			bHasExpectToolCallError = true;
		}
	}

	FString Method;
	RequestObject->TryGetStringField(TEXT("method"), Method);
	if (!Method.IsEmpty() && Method != TEXT("tools/call"))
	{
		return UnrealMcp::MakeExecutionResult(TEXT("TestRequest.json must use JSON-RPC method tools/call."), nullptr, true);
	}

	const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
	if (!RequestObject->TryGetObjectField(TEXT("params"), ParamsObject) || !ParamsObject || !(*ParamsObject).IsValid())
	{
		return UnrealMcp::MakeExecutionResult(TEXT("TestRequest.json is missing params object."), nullptr, true);
	}

	FString RequestToolName;
	(*ParamsObject)->TryGetStringField(TEXT("name"), RequestToolName);
	RequestToolName = RequestToolName.TrimStartAndEnd();
	if (RequestToolName.IsEmpty())
	{
		RequestToolName = ToolName;
	}
	if (RequestToolName.IsEmpty())
	{
		return UnrealMcp::MakeExecutionResult(TEXT("Unable to determine tool name from arguments, project memory, or TestRequest.json."), nullptr, true);
	}

	const TSharedPtr<FJsonObject>* RequestArgumentsObject = nullptr;
	const TSharedPtr<FJsonObject> EmptyArguments = UnrealMcp::MakeEmptyObject();
	const FJsonObject& RequestArguments = ((*ParamsObject)->TryGetObjectField(TEXT("arguments"), RequestArgumentsObject) && RequestArgumentsObject && (*RequestArgumentsObject).IsValid())
		? **RequestArgumentsObject
		: *EmptyArguments;

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	AppendToolDefinitions(ToolsArray);

	bool bToolListed = false;
	TSharedPtr<FJsonObject> ListedToolObject;
	for (const TSharedPtr<FJsonValue>& ToolValue : ToolsArray)
	{
		if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
		{
			continue;
		}

		FString ListedName;
		if (ToolValue->AsObject()->TryGetStringField(TEXT("name"), ListedName) && ListedName == RequestToolName)
		{
			bToolListed = true;
			ListedToolObject = ToolValue->AsObject();
			break;
		}
	}

	bool bToolExecuted = false;
	FUnrealMcpExecutionResult ToolResult;
	bool bInjectedSkipLock = false;
	if (bExecuteTool && bToolListed)
	{
		TSharedPtr<FJsonObject> EffectiveRequestArguments = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RequestArguments.Values)
		{
			EffectiveRequestArguments->SetField(Pair.Key, Pair.Value);
		}

		const UnrealMcp::FToolPolicy RequestToolPolicy = UnrealMcp::GetToolPolicy(RequestToolName);
		if (RequestToolPolicy.bRequiresLock && !EffectiveRequestArguments->HasField(TEXT("skipLock")))
		{
			EffectiveRequestArguments->SetBoolField(TEXT("skipLock"), true);
			bInjectedSkipLock = true;
		}

		ToolResult = ExecuteTool(RequestToolName, *EffectiveRequestArguments);
		bToolExecuted = true;
	}

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_run_tool_test"));
	StructuredContent->SetStringField(TEXT("toolName"), RequestToolName);
	StructuredContent->SetStringField(TEXT("testRequestPath"), ResolvedTestRequestPath);
	StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
	StructuredContent->SetStringField(TEXT("endpointMode"), TEXT("in_process_mcp_handlers"));
	StructuredContent->SetStringField(TEXT("endpointNote"), TEXT("tools/list and tools/call are exercised through the same in-editor MCP handlers. A network self-call to tools/call from inside tools/call would deadlock on the editor game thread."));
	StructuredContent->SetNumberField(TEXT("toolCount"), ToolsArray.Num());
	StructuredContent->SetBoolField(TEXT("toolListed"), bToolListed);
	StructuredContent->SetBoolField(TEXT("toolExecuted"), bToolExecuted);
	StructuredContent->SetBoolField(TEXT("injectedSkipLockForInProcessTest"), bInjectedSkipLock);
	StructuredContent->SetBoolField(TEXT("expectToolListed"), bExpectToolListed);
	if (ListedToolObject.IsValid())
	{
		StructuredContent->SetObjectField(TEXT("listedTool"), ListedToolObject);
	}
	if (bToolExecuted)
	{
		StructuredContent->SetBoolField(TEXT("toolCallIsError"), ToolResult.bIsError);
		StructuredContent->SetStringField(TEXT("toolCallText"), ToolResult.Text);
		if (ToolResult.StructuredContent.IsValid())
		{
			StructuredContent->SetObjectField(TEXT("toolCallStructuredContent"), ToolResult.StructuredContent);
		}
	}
	StructuredContent->SetStringField(TEXT("testName"), TestName);
	StructuredContent->SetStringField(TEXT("testDescription"), TestDescription);
	StructuredContent->SetStringField(TEXT("expectationNote"), ExpectationNote);
	StructuredContent->SetBoolField(TEXT("isWrappedTestCase"), TestCaseObject.IsValid());
	StructuredContent->SetBoolField(TEXT("hasExpectedToolCallError"), bHasExpectToolCallError);
	if (bHasExpectToolCallError)
	{
		StructuredContent->SetBoolField(TEXT("expectToolCallError"), bExpectToolCallError);
	}

	TArray<TSharedPtr<FJsonValue>> StructuredFieldChecks;
	if (TestCaseObject.IsValid())
	{
		const TSharedPtr<FJsonObject>* ExpectedStructuredFields = nullptr;
		if (TestCaseObject->TryGetObjectField(TEXT("expectToolCallStructuredFields"), ExpectedStructuredFields)
			&& ExpectedStructuredFields
			&& (*ExpectedStructuredFields).IsValid())
		{
			bHasExpectedStructuredFields = true;
			bStructuredFieldsOk = bToolExecuted
				&& ToolResult.StructuredContent.IsValid()
				&& UnrealMcp::EvaluateExpectedStructuredFields(ToolResult.StructuredContent, *ExpectedStructuredFields, StructuredFieldChecks);
		}
	}
	StructuredContent->SetBoolField(TEXT("hasExpectedToolCallStructuredFields"), bHasExpectedStructuredFields);
	if (bHasExpectedStructuredFields)
	{
		StructuredContent->SetBoolField(TEXT("structuredFieldExpectationOk"), bStructuredFieldsOk);
		StructuredContent->SetArrayField(TEXT("structuredFieldChecks"), StructuredFieldChecks);
	}

	const bool bListedExpectationOk = !bExpectToolListed || bToolListed;
	const bool bToolCallExpectationOk = !bExecuteTool
		|| (bToolExecuted && (bHasExpectToolCallError ? ToolResult.bIsError == bExpectToolCallError : !ToolResult.bIsError));
	const bool bSucceeded = bListedExpectationOk && bToolCallExpectationOk && bStructuredFieldsOk;
	StructuredContent->SetBoolField(TEXT("listedExpectationOk"), bListedExpectationOk);
	StructuredContent->SetBoolField(TEXT("toolCallExpectationOk"), bToolCallExpectationOk);
	StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);

	if (bWriteProjectMemory)
	{
		TSharedPtr<FJsonObject> UpdatedMemoryContent = MakeShared<FJsonObject>();
		UpdatedMemoryContent->SetStringField(TEXT("toolName"), RequestToolName);
		UpdatedMemoryContent->SetStringField(TEXT("testRequestPath"), ResolvedTestRequestPath);
		UpdatedMemoryContent->SetBoolField(TEXT("toolListed"), bToolListed);
		UpdatedMemoryContent->SetBoolField(TEXT("toolExecuted"), bToolExecuted);
		UpdatedMemoryContent->SetBoolField(TEXT("testSucceeded"), bSucceeded);
		UpdatedMemoryContent->SetStringField(TEXT("testName"), TestName);
		UnrealMcp::WriteBuildTestMemory(
			MemoryKey,
			bSucceeded ? TEXT("MCP tool test succeeded.") : TEXT("MCP tool test failed or tool is not loaded."),
			bSucceeded ? TEXT("tool_test_succeeded") : TEXT("tool_test_failed"),
			bSucceeded ? TEXT("Continue with tool audit or next MCP extension stage.") : TEXT("If the tool is missing, restart Unreal Editor after a successful build, then rerun unreal.mcp_run_tool_test."),
			UpdatedMemoryContent);
	}

	FString Text;
	if (!bToolListed)
	{
		Text = FString::Printf(TEXT("Tool '%s' was not found in tools/list."), *RequestToolName);
	}
	else if (!bExecuteTool)
	{
		Text = FString::Printf(TEXT("Tool '%s' is listed. Execution was skipped by request."), *RequestToolName);
	}
	else
	{
		Text = FString::Printf(TEXT("Test '%s' tool '%s' listed=%s executed=%s isError=%s expectationOk=%s."),
			*TestName,
			*RequestToolName,
			bToolListed ? TEXT("true") : TEXT("false"),
			bToolExecuted ? TEXT("true") : TEXT("false"),
			ToolResult.bIsError ? TEXT("true") : TEXT("false"),
			bToolCallExpectationOk ? TEXT("true") : TEXT("false"));
	}

	return UnrealMcp::MakeExecutionResult(Text, StructuredContent, !bSucceeded);
}

FUnrealMcpExecutionResult FUnrealMcpModule::RunMcpTestSuite(const FJsonObject& Arguments) const
{
	FString ToolName;
	FString TestsDir;
	FString ScaffoldDir;
	FString OutputRoot = TEXT("Tools/UnrealMcpToolScaffolds");
	FString MemoryKey = TEXT("mcp.extension.build_test");
	bool bReadProjectMemory = true;
	bool bWriteProjectMemory = true;
	bool bExecuteTool = true;
	bool bStopOnFailure = false;
	bool bFallbackToSingleTest = true;
	bool bIncludePassedStructuredContent = false;

	Arguments.TryGetStringField(TEXT("toolName"), ToolName);
	Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
	Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
	Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
	Arguments.TryGetBoolField(TEXT("readProjectMemory"), bReadProjectMemory);
	Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
	Arguments.TryGetBoolField(TEXT("executeTool"), bExecuteTool);
	Arguments.TryGetBoolField(TEXT("stopOnFailure"), bStopOnFailure);
	Arguments.TryGetBoolField(TEXT("fallbackToSingleTest"), bFallbackToSingleTest);
	Arguments.TryGetBoolField(TEXT("includePassedStructuredContent"), bIncludePassedStructuredContent);

	ToolName = ToolName.TrimStartAndEnd();
	TestsDir = TestsDir.TrimStartAndEnd();
	ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
	MemoryKey = MemoryKey.TrimStartAndEnd();
	if (MemoryKey.IsEmpty())
	{
		MemoryKey = TEXT("mcp.extension.build_test");
	}

	if (bReadProjectMemory)
	{
		FString FailureReason;
		TSharedPtr<FJsonObject> MemoryObject;
		if (UnrealMcp::LoadProjectMemory(MemoryObject, FailureReason) && MemoryObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (MemoryObject->TryGetArrayField(TEXT("entries"), Entries) && Entries)
			{
				for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
				{
					if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
					{
						continue;
					}

					TSharedPtr<FJsonObject> EntryObject = EntryValue->AsObject();
					FString ExistingKey;
					if (!EntryObject->TryGetStringField(TEXT("key"), ExistingKey) || ExistingKey != MemoryKey)
					{
						continue;
					}

					const TSharedPtr<FJsonObject>* ContentObject = nullptr;
					if (EntryObject->TryGetObjectField(TEXT("content"), ContentObject) && ContentObject && (*ContentObject).IsValid())
					{
						if (ToolName.IsEmpty())
						{
							(*ContentObject)->TryGetStringField(TEXT("toolName"), ToolName);
						}
						if (TestsDir.IsEmpty())
						{
							(*ContentObject)->TryGetStringField(TEXT("testsDir"), TestsDir);
						}
						if (ScaffoldDir.IsEmpty())
						{
							(*ContentObject)->TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
						}
					}
					break;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
	ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
	ResolveArguments->SetStringField(TEXT("testsDir"), TestsDir);
	ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);

	FString ResolvedTestsDir;
	FString ResolvedScaffoldDir;
	FString ResolvedToolName;
	FString ResolveFailure;
	if (!UnrealMcp::ResolveMcpTestsDirectory(*ResolveArguments, ResolvedTestsDir, ResolvedScaffoldDir, ResolvedToolName, ResolveFailure))
	{
		return UnrealMcp::MakeExecutionResult(ResolveFailure, nullptr, true);
	}
	ToolName = ResolvedToolName;

	TArray<FString> TestFiles;
	if (FPaths::DirectoryExists(ResolvedTestsDir))
	{
		UnrealMcp::FindImmediateChildren(ResolvedTestsDir, TEXT("*.json"), true, false, TestFiles);
	}
	TestFiles.Sort();

	if (TestFiles.Num() == 0 && bFallbackToSingleTest)
	{
		TSharedPtr<FJsonObject> SingleArguments = MakeShared<FJsonObject>();
		SingleArguments->SetStringField(TEXT("toolName"), ToolName);
		SingleArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		SingleArguments->SetStringField(TEXT("testRequestPath"), FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json")));
		SingleArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		SingleArguments->SetBoolField(TEXT("readProjectMemory"), false);
		SingleArguments->SetBoolField(TEXT("writeProjectMemory"), false);
		SingleArguments->SetBoolField(TEXT("executeTool"), bExecuteTool);
		const FUnrealMcpExecutionResult SingleResult = RunMcpToolTest(*SingleArguments);

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_run_test_suite"));
		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetStringField(TEXT("testsDir"), ResolvedTestsDir);
		StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		StructuredContent->SetBoolField(TEXT("fallbackToSingleTest"), true);
		StructuredContent->SetBoolField(TEXT("succeeded"), !SingleResult.bIsError);
		StructuredContent->SetNumberField(TEXT("total"), 1);
		StructuredContent->SetNumberField(TEXT("passed"), SingleResult.bIsError ? 0 : 1);
		StructuredContent->SetNumberField(TEXT("failed"), SingleResult.bIsError ? 1 : 0);
		StructuredContent->SetNumberField(TEXT("passRate"), SingleResult.bIsError ? 0.0 : 1.0);
		if (SingleResult.StructuredContent.IsValid())
		{
			StructuredContent->SetObjectField(TEXT("singleTest"), SingleResult.StructuredContent);
		}
		return UnrealMcp::MakeExecutionResult(
			SingleResult.bIsError ? TEXT("MCP test suite fallback single test failed.") : TEXT("MCP test suite fallback single test passed."),
			StructuredContent,
			SingleResult.bIsError);
	}

	if (TestFiles.Num() == 0)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_run_test_suite"));
		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetStringField(TEXT("testsDir"), ResolvedTestsDir);
		StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		StructuredContent->SetBoolField(TEXT("succeeded"), false);
		StructuredContent->SetNumberField(TEXT("total"), 0);
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("No JSON test cases found under '%s'."), *ResolvedTestsDir), StructuredContent, true);
	}

	TArray<TSharedPtr<FJsonValue>> TestResults;
	TArray<TSharedPtr<FJsonValue>> FailedCases;
	int32 PassedCount = 0;
	int32 FailedCount = 0;

	for (const FString& TestFile : TestFiles)
	{
		TSharedPtr<FJsonObject> TestArguments = MakeShared<FJsonObject>();
		TestArguments->SetStringField(TEXT("toolName"), ToolName);
		TestArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		TestArguments->SetStringField(TEXT("testRequestPath"), TestFile);
		TestArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		TestArguments->SetBoolField(TEXT("readProjectMemory"), false);
		TestArguments->SetBoolField(TEXT("writeProjectMemory"), false);
		TestArguments->SetBoolField(TEXT("executeTool"), bExecuteTool);

		const FUnrealMcpExecutionResult TestResult = RunMcpToolTest(*TestArguments);
		const bool bPassed = !TestResult.bIsError;
		PassedCount += bPassed ? 1 : 0;
		FailedCount += bPassed ? 0 : 1;

		TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetStringField(TEXT("path"), TestFile);
		ResultObject->SetStringField(TEXT("fileName"), FPaths::GetCleanFilename(TestFile));
		ResultObject->SetBoolField(TEXT("passed"), bPassed);
		ResultObject->SetBoolField(TEXT("isError"), TestResult.bIsError);
		ResultObject->SetStringField(TEXT("text"), TestResult.Text);
		if (TestResult.StructuredContent.IsValid())
		{
			FString TestName;
			if (TestResult.StructuredContent->TryGetStringField(TEXT("testName"), TestName))
			{
				ResultObject->SetStringField(TEXT("name"), TestName);
			}
			if (!bPassed || bIncludePassedStructuredContent)
			{
				ResultObject->SetObjectField(TEXT("structuredContent"), TestResult.StructuredContent);
			}
		}

		TestResults.Add(MakeShared<FJsonValueObject>(ResultObject));
		if (!bPassed)
		{
			FailedCases.Add(MakeShared<FJsonValueObject>(ResultObject));
			if (bStopOnFailure)
			{
				break;
			}
		}
	}

	const int32 ExecutedCount = PassedCount + FailedCount;
	const double PassRate = ExecutedCount > 0 ? static_cast<double>(PassedCount) / static_cast<double>(ExecutedCount) : 0.0;
	const bool bSucceeded = FailedCount == 0 && ExecutedCount == TestFiles.Num();

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_run_test_suite"));
	StructuredContent->SetStringField(TEXT("toolName"), ToolName);
	StructuredContent->SetStringField(TEXT("testsDir"), ResolvedTestsDir);
	StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
	StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
	StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);
	StructuredContent->SetBoolField(TEXT("executeTool"), bExecuteTool);
	StructuredContent->SetBoolField(TEXT("stopOnFailure"), bStopOnFailure);
	StructuredContent->SetNumberField(TEXT("total"), TestFiles.Num());
	StructuredContent->SetNumberField(TEXT("executed"), ExecutedCount);
	StructuredContent->SetNumberField(TEXT("passed"), PassedCount);
	StructuredContent->SetNumberField(TEXT("failed"), FailedCount);
	StructuredContent->SetNumberField(TEXT("passRate"), PassRate);
	StructuredContent->SetArrayField(TEXT("results"), TestResults);
	StructuredContent->SetArrayField(TEXT("failedCases"), FailedCases);

	if (bWriteProjectMemory)
	{
		TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
		MemoryContent->SetStringField(TEXT("toolName"), ToolName);
		MemoryContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		MemoryContent->SetStringField(TEXT("testsDir"), ResolvedTestsDir);
		MemoryContent->SetBoolField(TEXT("testSuiteSucceeded"), bSucceeded);
		MemoryContent->SetNumberField(TEXT("total"), TestFiles.Num());
		MemoryContent->SetNumberField(TEXT("passed"), PassedCount);
		MemoryContent->SetNumberField(TEXT("failed"), FailedCount);
		MemoryContent->SetNumberField(TEXT("passRate"), PassRate);
		UnrealMcp::WriteBuildTestMemory(
			MemoryKey,
			bSucceeded ? TEXT("MCP test suite succeeded.") : TEXT("MCP test suite failed."),
			bSucceeded ? TEXT("test_suite_succeeded") : TEXT("test_suite_failed"),
			bSucceeded ? TEXT("Continue with tool audit or next MCP extension stage.") : TEXT("Inspect failedCases, patch fragments, rebuild, and rerun the suite."),
			MemoryContent);
	}

	return UnrealMcp::MakeExecutionResult(
		FString::Printf(TEXT("MCP test suite for %s: %d/%d passed (%.0f%%)."),
			*ToolName,
			PassedCount,
			TestFiles.Num(),
			PassRate * 100.0),
		StructuredContent,
		!bSucceeded);
}
