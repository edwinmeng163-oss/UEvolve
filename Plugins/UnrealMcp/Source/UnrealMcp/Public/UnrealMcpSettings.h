#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UnrealMcpSettings.generated.h"

UENUM()
enum class EAiProviderKind : uint8
{
	OpenAiResponses    UMETA(DisplayName="OpenAI Responses API"),
	OpenAiChatCompat   UMETA(DisplayName="OpenAI-Compatible (chat/completions: Kimi/GLM/DeepSeek/Qwen/Ollama)"),
	AnthropicMessages  UMETA(DisplayName="Anthropic Messages"),
	Codex              UMETA(DisplayName="Codex CLI (local subprocess)"),
	CodexAppServer     UMETA(DisplayName="Codex Desktop / App Server (Plan B bridge)"),
};

USTRUCT()
struct UNREALMCP_API FAiProviderConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Stable provider key used by ActiveProviderId, for example openai-default."))
	FString Id;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Human-readable provider name shown in settings."))
	FString DisplayName;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Provider protocol or integration type."))
	EAiProviderKind Kind = EAiProviderKind::OpenAiResponses;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Provider endpoint URL. For chat-compatible providers, use the chat/completions endpoint."))
	FString BaseUrl;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="API key for this provider. Leave empty for local providers that do not require one.", PasswordField=true))
	FString ApiKey;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Model identifier sent to this provider."))
	FString Model;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Optional reasoning effort. Used only by OpenAI Responses and Anthropic Messages providers."))
	FString ReasoningEffort;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ClampMin="0", ToolTip="Per-provider output token limit. Set to 0 to fall back to the global AiMaxOutputTokens setting."))
	int32 MaxOutputTokens = 4096;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Path to the local Codex CLI bridge executable. Ignored for non-Codex provider kinds."))
	FString CodexBinaryPath;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Extra arguments passed to the local Codex CLI bridge. Ignored for non-Codex provider kinds."))
	FString CodexExtraArgs;
};

UCLASS(Config=EditorPerProjectUserSettings, DefaultConfig, meta=(DisplayName="Unreal MCP"))
class UNREALMCP_API UUnrealMcpSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUnrealMcpSettings();

	virtual void PostInitProperties() override;
	virtual FName GetContainerName() const override;
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
	const FAiProviderConfig* FindActiveProvider() const;

	UPROPERTY(EditAnywhere, Config, Category="Server")
	bool bEnableServer = true;

	UPROPERTY(EditAnywhere, Config, Category="Server", meta=(ClampMin="1024", ClampMax="65535"))
	int32 Port = 8765;

	UPROPERTY(EditAnywhere, Config, Category="Server")
	FString EndpointPath = TEXT("/mcp");

	UPROPERTY(EditAnywhere, Config, Category="Security", meta=(ToolTip="Optional bearer token. When set, clients must send Authorization: Bearer <token>."))
	FString AuthToken;

	UPROPERTY(EditAnywhere, Config, Category="Security", meta=(ToolTip="Requests with an Origin header must match one of these values. Leave the defaults unless you know you need more."))
	TArray<FString> AllowedOrigins;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Enable in-editor AI chat requests such as /ask."))
	bool bEnableAiAssistant = false;

	//   Example: Kimi (Moonshot)  Kind=OpenAiChatCompat BaseUrl=https://api.moonshot.cn/v1/chat/completions Model=moonshot-v1-8k
	//   Example: GLM (Zhipu)      Kind=OpenAiChatCompat BaseUrl=https://open.bigmodel.cn/api/paas/v4/chat/completions Model=glm-4
	//   Example: DeepSeek         Kind=OpenAiChatCompat BaseUrl=https://api.deepseek.com/v1/chat/completions Model=deepseek-chat
	//   Example: Anthropic        Kind=AnthropicMessages BaseUrl=https://api.anthropic.com/v1/messages Model=claude-sonnet-4-5
	//   Example: Codex            Kind=Codex CodexBinaryPath=/Users/.../codex-orchestrator/bin/codex-agent CodexExtraArgs="-m gpt-5.5 -r xhigh"
	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(TitleProperty="DisplayName"))
	TArray<FAiProviderConfig> Providers;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Id of the provider currently used by the chat panel and /ask. Must match one of Providers[].Id."))
	FString ActiveProviderId;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Responses API endpoint URL. Leave the default for OpenAI unless you are targeting a compatible gateway.", DeprecatedProperty, DeprecationMessage="Use Providers[] + ActiveProviderId instead. This field will be removed in a future version."))
	FString OpenAIResponsesUrl = TEXT("https://api.openai.com/v1/responses");

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="API key used for the AI assistant.", PasswordField=true, DeprecatedProperty, DeprecationMessage="Use Providers[] + ActiveProviderId instead. This field will be removed in a future version."))
	FString OpenAIApiKey;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Model used for in-editor AI chat.", DeprecatedProperty, DeprecationMessage="Use Providers[] + ActiveProviderId instead. This field will be removed in a future version."))
	FString OpenAIModel = TEXT("gpt-5.1");

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Reasoning effort for GPT-5/o-series models. Use minimal, low, medium, or high. Leave empty to omit.", DeprecatedProperty, DeprecationMessage="Use Providers[] + ActiveProviderId instead. This field will be removed in a future version."))
	FString OpenAIReasoningEffort = TEXT("medium");

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ClampMin="1", ClampMax="24", ToolTip="Maximum number of model->tool->model iterations allowed for one /ask request."))
	int32 AiMaxToolRounds = 16;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ClampMin="128", ClampMax="8192", ToolTip="Upper bound for response tokens returned by the model for each request. Larger values reduce truncation but may cost more."))
	int32 AiMaxOutputTokens = 4096;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ClampMin="10", ClampMax="600", ToolTip="Total timeout in seconds for one OpenAI Responses request. Increase this for longer planning or tool-heavy turns."))
	float AiRequestTimeoutSeconds = 180.0f;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ClampMin="10", ClampMax="600", ToolTip="Idle timeout in seconds while waiting for more streamed data from the AI provider."))
	float AiRequestActivityTimeoutSeconds = 120.0f;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(MultiLine=true, ToolTip="Optional additional instructions appended to the built-in assistant prompt."))
	FString AssistantSystemPrompt;
};
