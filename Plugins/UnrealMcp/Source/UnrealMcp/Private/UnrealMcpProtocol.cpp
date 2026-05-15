#include "UnrealMcpModule.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/Atomic.h"
#include "UnrealMcpActivityLog.h"
#include "UnrealMcpSettings.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
	static const FString ProtocolLatestVersion = TEXT("2025-06-18");
	static constexpr double ProtocolGameThreadTimeoutSeconds = 30.0;

	bool CanBindLocalTcpPort(int32 Port);
	FString GetFirstHeaderValue(const FHttpServerRequest& Request, const FString& HeaderName);
	bool IsSupportedProtocolVersion(const FString& ProtocolVersion);
	FString NormalizeEndpointPath(const FString& EndpointPath);
	FString RequestBodyToString(const FHttpServerRequest& Request);
	TSharedPtr<FJsonValue> MakeIdOrNull(const TSharedPtr<FJsonObject>& Message);
	TSharedPtr<FJsonObject> MakeTextContentObject(const FString& Text);
	FString JsonObjectToString(const TSharedPtr<FJsonObject>& JsonObject);
	TSharedPtr<FJsonObject> MakeEmptyObject();
	TArray<TSharedPtr<FJsonValue>> MakeJsonStringArray(const TArray<FString>& Values);
	bool TryGetMethodAndId(const FHttpServerRequest& Request, FString& OutMethod, TSharedPtr<FJsonValue>& OutId);
}

bool FUnrealMcpModule::StartServer()
{
	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	if (!Settings->bEnableServer)
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("Unreal MCP server is disabled in settings."));
		return false;
	}

	if (!UnrealMcp::CanBindLocalTcpPort(Settings->Port))
	{
		UE_LOG(
			LogUnrealMcp,
			Error,
			TEXT("Unable to start Unreal MCP server because 127.0.0.1:%d is already in use. Close the other Unreal Editor instance or change the MCP port in Project Settings."),
			Settings->Port);
		return false;
	}

	HttpRouter = FHttpServerModule::Get().GetHttpRouter(Settings->Port, true);
	if (!HttpRouter.IsValid())
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("Unable to bind Unreal MCP server to localhost:%d."), Settings->Port);
		return false;
	}

	const FString EndpointPath = UnrealMcp::NormalizeEndpointPath(Settings->EndpointPath);
	McpRouteHandle = HttpRouter->BindRoute(
		FHttpPath(EndpointPath),
		EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_GET | EHttpServerRequestVerbs::VERB_DELETE,
		FHttpRequestHandler::CreateRaw(this, &FUnrealMcpModule::HandleMcpHttpRequest));

	if (!McpRouteHandle.IsValid())
	{
		UE_LOG(LogUnrealMcp, Error, TEXT("Unable to bind route '%s' for Unreal MCP."), *EndpointPath);
		HttpRouter.Reset();
		return false;
	}

	FHttpServerModule::Get().StartAllListeners();
	bServerStarted = true;

	UE_LOG(
		LogUnrealMcp,
		Log,
		TEXT("Unreal MCP listening on http://127.0.0.1:%d%s"),
		Settings->Port,
		*EndpointPath);

	return true;
}

void FUnrealMcpModule::StopServer()
{
	if (HttpRouter.IsValid() && McpRouteHandle.IsValid())
	{
		HttpRouter->UnbindRoute(McpRouteHandle);
	}

	McpRouteHandle.Reset();
	HttpRouter.Reset();
	bServerStarted = false;
}

bool FUnrealMcpModule::HandleMcpHttpRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FString Method;
	TSharedPtr<FJsonValue> IdValue;
	const bool bParsedMethod = UnrealMcp::TryGetMethodAndId(Request, Method, IdValue);
	const bool bRequiresGameThread = bParsedMethod && Method == TEXT("tools/call");

	if (!bRequiresGameThread)
	{
		OnComplete(HandleMcpHttpRequestInternal(Request));
		return true;
	}

	const FHttpServerRequest RequestCopy = Request;
	const FString MethodCopy = Method;
	const TSharedPtr<FJsonValue> IdValueCopy = IdValue.IsValid() ? IdValue : MakeShared<FJsonValueNull>();
	TSharedRef<TAtomic<bool>, ESPMode::ThreadSafe> bDidRespond = MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false);

	auto CompleteOnce = [this, bDidRespond, OnComplete](TUniquePtr<FHttpServerResponse>&& Response) mutable -> bool
	{
		if (bDidRespond->Exchange(true))
		{
			return false;
		}

		if (!Response.IsValid())
		{
			Response = MakeHttpError(EHttpServerResponseCodes::ServerError, TEXT("Failed to build an MCP response."));
		}

		OnComplete(MoveTemp(Response));
		return true;
	};

	UE_LOG(LogUnrealMcp, Verbose, TEXT("Dispatching MCP method '%s' to the Unreal Editor game thread."), *MethodCopy);

	AsyncTask(ENamedThreads::GameThread, [this, RequestCopy, MethodCopy, CompleteOnce]() mutable
	{
		if (CompleteOnce(HandleMcpHttpRequestInternal(RequestCopy)))
		{
			UE_LOG(LogUnrealMcp, Verbose, TEXT("Completed MCP method '%s' on the Unreal Editor game thread."), *MethodCopy);
		}
	});

	Async(EAsyncExecution::ThreadPool, [this, CompleteOnce, MethodCopy, IdValueCopy]() mutable
	{
		FPlatformProcess::SleepNoStats(static_cast<float>(UnrealMcp::ProtocolGameThreadTimeoutSeconds));
		if (CompleteOnce(MakeJsonRpcError(
			IdValueCopy,
			-32001,
			FString::Printf(TEXT("Timed out waiting %.1f seconds for the Unreal Editor game thread."), UnrealMcp::ProtocolGameThreadTimeoutSeconds),
			EHttpServerResponseCodes::GatewayTimeout)))
		{
			UE_LOG(LogUnrealMcp, Warning, TEXT("Timed out waiting for the Unreal Editor game thread to handle MCP method '%s'."), *MethodCopy);
		}
	});

	return true;
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::HandleMcpHttpRequestInternal(const FHttpServerRequest& Request)
{
	FString FailureReason;
	if (!ValidateOrigin(Request, FailureReason) || !ValidateAuthorization(Request, FailureReason) || !ValidateTransportProtocolHeader(Request, FailureReason))
	{
		return MakeHttpError(EHttpServerResponseCodes::BadRequest, FailureReason);
	}

	if (Request.Verb == EHttpServerRequestVerbs::VERB_GET)
	{
		return MakeHttpError(EHttpServerResponseCodes::BadMethod, TEXT("This server does not expose an SSE stream. Use HTTP POST to /mcp."));
	}

	if (Request.Verb == EHttpServerRequestVerbs::VERB_DELETE)
	{
		return MakeHttpError(EHttpServerResponseCodes::BadMethod, TEXT("This server uses stateless requests and does not manage MCP sessions."));
	}

	if (Request.Verb != EHttpServerRequestVerbs::VERB_POST)
	{
		return MakeHttpError(EHttpServerResponseCodes::BadMethod, TEXT("Unsupported HTTP verb for MCP endpoint."));
	}

	const FString BodyString = UnrealMcp::RequestBodyToString(Request);
	if (BodyString.IsEmpty())
	{
		return MakeHttpError(EHttpServerResponseCodes::BadRequest, TEXT("Expected a JSON-RPC message body."));
	}

	TSharedPtr<FJsonObject> MessageObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyString);
	if (!FJsonSerializer::Deserialize(Reader, MessageObject) || !MessageObject.IsValid())
	{
		return MakeJsonRpcError(MakeShared<FJsonValueNull>(), -32700, TEXT("Failed to parse JSON-RPC request."), EHttpServerResponseCodes::BadRequest);
	}

	const TSharedPtr<FJsonValue> IdValue = UnrealMcp::MakeIdOrNull(MessageObject);

	FString JsonRpcVersion;
	if (!MessageObject->TryGetStringField(TEXT("jsonrpc"), JsonRpcVersion) || JsonRpcVersion != TEXT("2.0"))
	{
		return MakeJsonRpcError(IdValue, -32600, TEXT("Only JSON-RPC 2.0 messages are supported."), EHttpServerResponseCodes::BadRequest);
	}

	FString Method;
	if (!MessageObject->TryGetStringField(TEXT("method"), Method) || Method.IsEmpty())
	{
		return MakeJsonRpcError(IdValue, -32600, TEXT("Missing JSON-RPC method."), EHttpServerResponseCodes::BadRequest);
	}

	const bool bIsNotification = !MessageObject->HasField(TEXT("id"));

	const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
	if (MessageObject->HasField(TEXT("params")) && !MessageObject->TryGetObjectField(TEXT("params"), ParamsObject))
	{
		return MakeJsonRpcError(IdValue, -32602, TEXT("The params field must be an object."), EHttpServerResponseCodes::BadRequest);
	}

	if (bIsNotification)
	{
		if (Method == TEXT("notifications/initialized"))
		{
			UE_LOG(LogUnrealMcp, Verbose, TEXT("Client finished MCP initialization."));
			return MakeAcceptedResponse();
		}

		if (Method == TEXT("notifications/cancelled"))
		{
			return MakeAcceptedResponse();
		}

		UE_LOG(LogUnrealMcp, Verbose, TEXT("Ignoring MCP notification '%s'."), *Method);
		return MakeAcceptedResponse();
	}

	if (Method == TEXT("initialize"))
	{
		if (!ParamsObject)
		{
			return MakeJsonRpcError(IdValue, -32602, TEXT("initialize requires params."), EHttpServerResponseCodes::BadRequest);
		}

		return HandleInitialize(IdValue, **ParamsObject);
	}

	if (Method == TEXT("ping"))
	{
		return HandlePing(IdValue);
	}

	if (Method == TEXT("tools/list"))
	{
		return HandleToolsList(IdValue);
	}

	if (Method == TEXT("tools/call"))
	{
		if (!ParamsObject)
		{
			return MakeJsonRpcError(IdValue, -32602, TEXT("tools/call requires params."), EHttpServerResponseCodes::BadRequest);
		}

		return HandleToolsCall(IdValue, **ParamsObject);
	}

	return MakeJsonRpcError(IdValue, -32601, FString::Printf(TEXT("Method '%s' is not supported."), *Method));
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::HandleInitialize(const TSharedPtr<FJsonValue>& Id, const FJsonObject& Params)
{
	FString RequestedProtocolVersion;
	Params.TryGetStringField(TEXT("protocolVersion"), RequestedProtocolVersion);

	FString NegotiatedProtocolVersion = UnrealMcp::ProtocolLatestVersion;
	if (!RequestedProtocolVersion.IsEmpty() && UnrealMcp::IsSupportedProtocolVersion(RequestedProtocolVersion))
	{
		NegotiatedProtocolVersion = RequestedProtocolVersion;
	}

	TSharedPtr<FJsonObject> CapabilitiesObject = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ToolsCapabilities = MakeShared<FJsonObject>();
	ToolsCapabilities->SetBoolField(TEXT("listChanged"), false);
	CapabilitiesObject->SetObjectField(TEXT("tools"), ToolsCapabilities);

	TSharedPtr<FJsonObject> ServerInfoObject = MakeShared<FJsonObject>();
	ServerInfoObject->SetStringField(TEXT("name"), TEXT("unreal-editor-mcp"));
		ServerInfoObject->SetStringField(TEXT("version"), TEXT("0.10.4"));

	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	const FString EndpointUrl = FString::Printf(TEXT("http://127.0.0.1:%d%s"), Settings->Port, *UnrealMcp::NormalizeEndpointPath(Settings->EndpointPath));

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetStringField(TEXT("protocolVersion"), NegotiatedProtocolVersion);
	ResultObject->SetObjectField(TEXT("capabilities"), CapabilitiesObject);
		ResultObject->SetObjectField(TEXT("serverInfo"), ServerInfoObject);
			ResultObject->SetStringField(
				TEXT("instructions"),
				FString::Printf(
				TEXT("Connected to Unreal Editor. This server can inspect assets, drive PIE sessions, tail logs, run console commands, batch-edit actor properties, lay out and spawn actors in bulk (grid and circle), execute Python commands or script files, open maps and assets, create/compile blueprints, edit Blueprint graphs with bp_* tools, edit UMG Widget Blueprints with widget_* tools, scaffold MCP tool extensions with scaffold_mcp_tool and mcp_* pipeline tools, and save dirty packages. Inside the editor you can also open Window > Unreal MCP Chat. Endpoint: %s"),
					*EndpointUrl));

	return MakeJsonRpcResult(Id, ResultObject, NegotiatedProtocolVersion);
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::HandlePing(const TSharedPtr<FJsonValue>& Id)
{
	return MakeJsonRpcResult(Id, UnrealMcp::MakeEmptyObject());
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::HandleToolsList(const TSharedPtr<FJsonValue>& Id)
{
	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	AppendToolDefinitions(ToolsArray);

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetArrayField(TEXT("tools"), ToolsArray);

	return MakeJsonRpcResult(Id, ResultObject);
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const FJsonObject& Params)
{
	FString ToolName;
	if (!Params.TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
	{
		return MakeToolResult(Id, TEXT("Missing tool name."), nullptr, true);
	}

	UE_LOG(LogUnrealMcp, Log, TEXT("MCP tool invoked: %s"), *ToolName);

	const TSharedPtr<FJsonObject>* ArgumentsObject = nullptr;
	if (Params.HasField(TEXT("arguments")) && !Params.TryGetObjectField(TEXT("arguments"), ArgumentsObject))
	{
		return MakeToolResult(Id, TEXT("Tool arguments must be an object."), nullptr, true);
	}

	const TSharedPtr<FJsonObject> EmptyArguments = UnrealMcp::MakeEmptyObject();
	const FJsonObject& Arguments = ArgumentsObject ? **ArgumentsObject : *EmptyArguments;
	const FDateTime ToolStartTimeUtc = FDateTime::UtcNow();
	const FUnrealMcpExecutionResult Result = ExecuteTool(ToolName, Arguments);
	{
		TArray<FString> ArgumentKeys;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments.Values)
		{
			ArgumentKeys.Add(Pair.Key);
		}
		ArgumentKeys.Sort();

		const FString HandlerName = UnrealMcp::ResolveToolHandlerName(ToolName);
		const UnrealMcp::FToolPolicy ActivityPolicy = UnrealMcp::GetToolPolicy(ToolName);
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("toolName"), ToolName);
		Payload->SetStringField(TEXT("handlerName"), HandlerName);
		Payload->SetStringField(TEXT("riskLevel"), UnrealMcp::LexToString(ActivityPolicy.RiskLevel));
		Payload->SetArrayField(TEXT("argumentKeys"), UnrealMcp::MakeJsonStringArray(ArgumentKeys));
		Payload->SetBoolField(TEXT("isError"), Result.bIsError);
		Payload->SetNumberField(TEXT("textLength"), Result.Text.Len());
		Payload->SetBoolField(TEXT("hasStructuredContent"), Result.StructuredContent.IsValid());
		Payload->SetNumberField(TEXT("durationMs"), FMath::Max(0.0, (FDateTime::UtcNow() - ToolStartTimeUtc).GetTotalMilliseconds()));

		const UnrealMcp::FToolHandlerRegistryEntry* ActivityHandlerEntry = UnrealMcp::FindToolHandlerRegistryEntry(HandlerName);
		if (ActivityHandlerEntry && ActivityHandlerEntry->ImplementationTrack == UnrealMcp::EToolImplementationTrack::Python)
		{
			FString PythonActualSha256;
			if (Result.StructuredContent.IsValid())
			{
				Result.StructuredContent->TryGetStringField(TEXT("pythonActualSha256"), PythonActualSha256);
			}
			Payload->SetStringField(TEXT("pythonHandlerPath"), ActivityHandlerEntry->PythonHandlerPath);
			Payload->SetStringField(TEXT("pythonExpectedSha256"), ActivityHandlerEntry->PythonHandlerSha256);
			Payload->SetStringField(TEXT("pythonActualSha256"), PythonActualSha256);
			Payload->SetNumberField(TEXT("pythonImportAllowListSize"), ActivityHandlerEntry->PythonImportAllowList.Num());
		}

		UnrealMcp::FActivityLogEvent Event;
		Event.EventKind = TEXT("tool_call");
		Event.Summary = FString::Printf(TEXT("Called MCP tool %s: %s."), *ToolName, Result.bIsError ? TEXT("failed") : TEXT("completed")).Left(2000);
		Event.Payload = Payload;
		Event.LegacyEventType = FString();
		UnrealMcp::WriteActivityEvent(Event);
	}

	return MakeToolResult(Id, Result.Text, Result.StructuredContent, Result.bIsError);
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::MakeJsonResponse(const TSharedPtr<FJsonObject>& Payload, EHttpServerResponseCodes ResponseCode, const FString& ProtocolVersion) const
{
	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(UnrealMcp::JsonObjectToString(Payload), TEXT("application/json"));
	Response->Code = ResponseCode;
	if (!ProtocolVersion.IsEmpty())
	{
		TArray<FString> HeaderValues;
		HeaderValues.Add(ProtocolVersion);
		Response->Headers.Add(TEXT("MCP-Protocol-Version"), MoveTemp(HeaderValues));
	}
	return Response;
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::MakeJsonRpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result, const FString& ProtocolVersion) const
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Payload->SetField(TEXT("id"), Id);
	Payload->SetObjectField(TEXT("result"), Result);
	return MakeJsonResponse(Payload, EHttpServerResponseCodes::Ok, ProtocolVersion);
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::MakeJsonRpcError(const TSharedPtr<FJsonValue>& Id, int32 ErrorCode, const FString& ErrorMessage, EHttpServerResponseCodes ResponseCode, const FString& ProtocolVersion) const
{
	TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
	ErrorObject->SetNumberField(TEXT("code"), ErrorCode);
	ErrorObject->SetStringField(TEXT("message"), ErrorMessage);

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Payload->SetField(TEXT("id"), Id ? Id : MakeShared<FJsonValueNull>());
	Payload->SetObjectField(TEXT("error"), ErrorObject);
	return MakeJsonResponse(Payload, ResponseCode, ProtocolVersion);
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::MakeToolResult(const TSharedPtr<FJsonValue>& Id, const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError) const
{
	TArray<TSharedPtr<FJsonValue>> ContentArray;
	ContentArray.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakeTextContentObject(Text)));

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetArrayField(TEXT("content"), ContentArray);
	ResultObject->SetBoolField(TEXT("isError"), bIsError);

	if (StructuredContent.IsValid())
	{
		ResultObject->SetObjectField(TEXT("structuredContent"), StructuredContent);
	}

	return MakeJsonRpcResult(Id, ResultObject);
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::MakeAcceptedResponse() const
{
	TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
	Response->Code = EHttpServerResponseCodes::Accepted;
	return Response;
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::MakeHttpError(EHttpServerResponseCodes ResponseCode, const FString& Message) const
{
	TSharedPtr<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
	ErrorObject->SetStringField(TEXT("message"), Message);
	return MakeJsonResponse(ErrorObject, ResponseCode);
}

bool FUnrealMcpModule::ValidateOrigin(const FHttpServerRequest& Request, FString& OutFailureReason) const
{
	const FString Origin = UnrealMcp::GetFirstHeaderValue(Request, TEXT("Origin"));
	if (Origin.IsEmpty())
	{
		return true;
	}

	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	for (const FString& AllowedOrigin : Settings->AllowedOrigins)
	{
		if (Origin.Equals(AllowedOrigin, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}

	OutFailureReason = FString::Printf(TEXT("Origin '%s' is not allowed."), *Origin);
	return false;
}

bool FUnrealMcpModule::ValidateAuthorization(const FHttpServerRequest& Request, FString& OutFailureReason) const
{
	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	if (Settings->AuthToken.IsEmpty())
	{
		return true;
	}

	const FString AuthorizationHeader = UnrealMcp::GetFirstHeaderValue(Request, TEXT("Authorization"));
	const FString ExpectedValue = FString::Printf(TEXT("Bearer %s"), *Settings->AuthToken);
	if (AuthorizationHeader.Equals(ExpectedValue, ESearchCase::CaseSensitive))
	{
		return true;
	}

	OutFailureReason = TEXT("Missing or invalid Authorization header.");
	return false;
}

bool FUnrealMcpModule::ValidateTransportProtocolHeader(const FHttpServerRequest& Request, FString& OutFailureReason) const
{
	const FString HeaderValue = UnrealMcp::GetFirstHeaderValue(Request, TEXT("MCP-Protocol-Version"));
	if (HeaderValue.IsEmpty())
	{
		return true;
	}

	if (UnrealMcp::IsSupportedProtocolVersion(HeaderValue))
	{
		return true;
	}

	OutFailureReason = FString::Printf(TEXT("Unsupported MCP-Protocol-Version '%s'."), *HeaderValue);
	return false;
}
