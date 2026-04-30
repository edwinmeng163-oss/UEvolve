#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"
#include "Modules/ModuleManager.h"

class FJsonObject;
class FJsonValue;
class FUnrealMcpAssistantRun;
class SDockTab;
struct FHttpServerRequest;
struct FHttpServerResponse;
class FSpawnTabArgs;
class IHttpRouter;

DECLARE_LOG_CATEGORY_EXTERN(LogUnrealMcp, Log, All);

enum class EUnrealMcpAssistantEventType : uint8
{
	Status,
	TextDelta,
	ToolCallStarted,
	ToolCallFinished
};

struct FUnrealMcpExecutionResult
{
	FString Text;
	TSharedPtr<FJsonObject> StructuredContent;
	bool bIsError = false;
};

struct FUnrealMcpAssistantEvent
{
	EUnrealMcpAssistantEventType Type = EUnrealMcpAssistantEventType::Status;
	FString Text;
	FString ToolName;
	FString ToolCallId;
	FString ToolArgumentsJson;
	bool bIsError = false;
};

struct FUnrealMcpAssistantTurnResult
{
	FString Text;
	FString ResponseId;
	bool bIsError = false;
	bool bWasCancelled = false;
};

class IUnrealMcpAssistantHandle
{
public:
	virtual ~IUnrealMcpAssistantHandle() = default;
	virtual void Cancel() = 0;
};

class FUnrealMcpModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	FUnrealMcpExecutionResult ExecuteChatCommand(const FString& Input) const;
	TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> ExecuteAssistantTurnAsync(
		const FString& UserPrompt,
		const FString& ConversationContext,
		const FString& PreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete) const;

private:
	friend class FUnrealMcpAssistantRun;

	bool StartServer();
	void StopServer();

	void RegisterTabSpawner();
	void UnregisterTabSpawner();
	void RegisterMenus();
	void OpenChatTab();
	TSharedRef<SDockTab> SpawnChatTab(const FSpawnTabArgs& Args);

	bool HandleMcpHttpRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	TUniquePtr<FHttpServerResponse> HandleMcpHttpRequestInternal(const FHttpServerRequest& Request);

	TUniquePtr<FHttpServerResponse> HandleInitialize(const TSharedPtr<FJsonValue>& Id, const FJsonObject& Params);
	TUniquePtr<FHttpServerResponse> HandlePing(const TSharedPtr<FJsonValue>& Id);
	TUniquePtr<FHttpServerResponse> HandleToolsList(const TSharedPtr<FJsonValue>& Id);
	TUniquePtr<FHttpServerResponse> HandleToolsCall(const TSharedPtr<FJsonValue>& Id, const FJsonObject& Params);

	void AppendToolDefinitions(TArray<TSharedPtr<FJsonValue>>& ToolsArray) const;
	FUnrealMcpExecutionResult ExecuteTool(const FString& ToolName, const FJsonObject& Arguments) const;
	FUnrealMcpExecutionResult RunMcpToolTest(const FJsonObject& Arguments) const;

	TUniquePtr<FHttpServerResponse> MakeJsonResponse(
		const TSharedPtr<FJsonObject>& Payload,
		EHttpServerResponseCodes ResponseCode = EHttpServerResponseCodes::Ok,
		const FString& ProtocolVersion = FString()) const;
	TUniquePtr<FHttpServerResponse> MakeJsonRpcResult(
		const TSharedPtr<FJsonValue>& Id,
		const TSharedPtr<FJsonObject>& Result,
		const FString& ProtocolVersion = FString()) const;
	TUniquePtr<FHttpServerResponse> MakeJsonRpcError(
		const TSharedPtr<FJsonValue>& Id,
		int32 ErrorCode,
		const FString& ErrorMessage,
		EHttpServerResponseCodes ResponseCode = EHttpServerResponseCodes::Ok,
		const FString& ProtocolVersion = FString()) const;
	TUniquePtr<FHttpServerResponse> MakeToolResult(
		const TSharedPtr<FJsonValue>& Id,
		const FString& Text,
		const TSharedPtr<FJsonObject>& StructuredContent = nullptr,
		bool bIsError = false) const;
	TUniquePtr<FHttpServerResponse> MakeAcceptedResponse() const;
	TUniquePtr<FHttpServerResponse> MakeHttpError(EHttpServerResponseCodes ResponseCode, const FString& Message) const;

	bool ValidateOrigin(const FHttpServerRequest& Request, FString& OutFailureReason) const;
	bool ValidateAuthorization(const FHttpServerRequest& Request, FString& OutFailureReason) const;
	bool ValidateTransportProtocolHeader(const FHttpServerRequest& Request, FString& OutFailureReason) const;

	TSharedPtr<IHttpRouter> HttpRouter;
	FHttpRouteHandle McpRouteHandle;
	bool bServerStarted = false;
};
