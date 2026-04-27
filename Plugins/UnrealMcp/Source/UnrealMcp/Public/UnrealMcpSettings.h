#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UnrealMcpSettings.generated.h"

UCLASS(Config=EditorPerProjectUserSettings, DefaultConfig, meta=(DisplayName="Unreal MCP"))
class UNREALMCP_API UUnrealMcpSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUnrealMcpSettings();

	virtual FName GetContainerName() const override;
	virtual FName GetCategoryName() const override;
	virtual FName GetSectionName() const override;
#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif

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

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Responses API endpoint URL. Leave the default for OpenAI unless you are targeting a compatible gateway."))
	FString OpenAIResponsesUrl = TEXT("https://api.openai.com/v1/responses");

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="API key used for the AI assistant.", PasswordField=true))
	FString OpenAIApiKey;

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Model used for in-editor AI chat."))
	FString OpenAIModel = TEXT("gpt-5.1");

	UPROPERTY(EditAnywhere, Config, Category="AI", meta=(ToolTip="Reasoning effort for GPT-5/o-series models. Use minimal, low, medium, or high. Leave empty to omit."))
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
