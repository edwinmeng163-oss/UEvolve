#include "UnrealMcpModule.h"

#include "Editor.h"
#include "HttpModule.h"
#include "HttpServerRequest.h"
#include "Misc/ConfigCacheIni.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UnrealMcpSettings.h"

namespace UnrealMcp
{
	static const FString LatestProtocolVersion = TEXT("2025-06-18");
	static const FString LegacyProtocolVersion = TEXT("2025-03-26");

	void ApplyAiHttpTimeoutOverrides(const UUnrealMcpSettings& Settings)
	{
		if (!GConfig)
		{
			return;
		}

		const float DesiredTotalTimeout = FMath::Max(10.0f, Settings.AiRequestTimeoutSeconds);
		const float DesiredActivityTimeout = FMath::Max(10.0f, Settings.AiRequestActivityTimeoutSeconds);
		const float DesiredConnectionTimeout = FMath::Max(DesiredTotalTimeout, DesiredActivityTimeout);

		GConfig->SetFloat(TEXT("HTTP"), TEXT("HttpTotalTimeout"), DesiredTotalTimeout, GEngineIni);
		GConfig->SetFloat(TEXT("HTTP"), TEXT("HttpConnectionTimeout"), DesiredConnectionTimeout, GEngineIni);
		GConfig->SetFloat(TEXT("HTTP"), TEXT("HttpActivityTimeout"), DesiredActivityTimeout, GEngineIni);
		GConfig->SetFloat(TEXT("HTTP"), TEXT("HttpReceiveTimeout"), DesiredConnectionTimeout, GEngineIni);
		GConfig->SetFloat(TEXT("HTTP"), TEXT("HttpSendTimeout"), DesiredConnectionTimeout, GEngineIni);
		FHttpModule::Get().UpdateConfigs();
	}

	bool CanBindLocalTcpPort(int32 Port)
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return true;
		}

		bool bIsValidAddress = false;
		const TSharedRef<FInternetAddr> LocalhostAddress = SocketSubsystem->CreateInternetAddr();
		LocalhostAddress->SetIp(TEXT("127.0.0.1"), bIsValidAddress);
		LocalhostAddress->SetPort(Port);
		if (!bIsValidAddress)
		{
			return true;
		}

		FSocket* ProbeSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("UnrealMcpPortProbe"), false);
		if (!ProbeSocket)
		{
			return true;
		}

		ProbeSocket->SetReuseAddr(false);
		const bool bCanBind = ProbeSocket->Bind(*LocalhostAddress);
		SocketSubsystem->DestroySocket(ProbeSocket);
		return bCanBind;
	}

	FString GetFirstHeaderValue(const FHttpServerRequest& Request, const FString& HeaderName)
	{
		for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
		{
			if (Header.Key.Equals(HeaderName, ESearchCase::IgnoreCase) && Header.Value.Num() > 0)
			{
				return Header.Value[0];
			}
		}

		return FString();
	}

	bool IsSupportedProtocolVersion(const FString& ProtocolVersion)
	{
		return ProtocolVersion.Equals(LatestProtocolVersion, ESearchCase::CaseSensitive)
			|| ProtocolVersion.Equals(LegacyProtocolVersion, ESearchCase::CaseSensitive);
	}

	FString NormalizeEndpointPath(const FString& EndpointPath)
	{
		FString Normalized = EndpointPath.TrimStartAndEnd();
		if (Normalized.IsEmpty())
		{
			Normalized = TEXT("/mcp");
		}

		if (!Normalized.StartsWith(TEXT("/")))
		{
			Normalized = TEXT("/") + Normalized;
		}

		return Normalized;
	}

	FString RequestBodyToString(const FHttpServerRequest& Request)
	{
		if (Request.Body.IsEmpty())
		{
			return FString();
		}

		FUTF8ToTCHAR Converter(reinterpret_cast<const UTF8CHAR*>(Request.Body.GetData()), Request.Body.Num());
		return FString(Converter.Length(), Converter.Get());
	}

	TSharedPtr<FJsonValue> MakeIdOrNull(const TSharedPtr<FJsonObject>& Message)
	{
		if (Message.IsValid() && Message->HasField(TEXT("id")))
		{
			return Message->TryGetField(TEXT("id"));
		}

		return MakeShared<FJsonValueNull>();
	}

	TSharedPtr<FJsonObject> MakeTextContentObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> ContentObject = MakeShared<FJsonObject>();
		ContentObject->SetStringField(TEXT("type"), TEXT("text"));
		ContentObject->SetStringField(TEXT("text"), Text);
		return ContentObject;
	}

	FString JsonObjectToString(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString JsonString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return JsonString;
	}

	TSharedPtr<FJsonObject> MakeEmptyObject()
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> MakeExecutionStructuredMessage(const FString& Text)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("message"), Text);
		return StructuredContent;
	}

	FUnrealMcpExecutionResult MakeExecutionResult(
		const FString& Text,
		const TSharedPtr<FJsonObject>& StructuredContent = nullptr,
		bool bIsError = false)
	{
		FUnrealMcpExecutionResult Result;
		Result.Text = Text;
		Result.StructuredContent = StructuredContent;
		Result.bIsError = bIsError;
		return Result;
	}

	bool TryGetMethodAndId(const FHttpServerRequest& Request, FString& OutMethod, TSharedPtr<FJsonValue>& OutId)
	{
		OutMethod.Reset();
		OutId = MakeShared<FJsonValueNull>();

		if (Request.Verb != EHttpServerRequestVerbs::VERB_POST)
		{
			return false;
		}

		const FString BodyString = RequestBodyToString(Request);
		if (BodyString.IsEmpty())
		{
			return false;
		}

		TSharedPtr<FJsonObject> MessageObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyString);
		if (!FJsonSerializer::Deserialize(Reader, MessageObject) || !MessageObject.IsValid())
		{
			return false;
		}

		OutId = MakeIdOrNull(MessageObject);
		return MessageObject->TryGetStringField(TEXT("method"), OutMethod);
	}


	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue)
	{
		double Value = static_cast<double>(DefaultValue);
		if (Arguments.TryGetNumberField(FieldName, Value))
		{
			return FMath::Max(1, static_cast<int32>(Value));
		}

		return DefaultValue;
	}

	bool IsEditorPlaying()
	{
		return GEditor
			&& (GEditor->PlayWorld != nullptr
				|| GEditor->bIsSimulatingInEditor
				|| GEditor->GetPlaySessionRequest().IsSet());
	}

	FString SanitizeMcpToolIdForPath(const FString& ToolName);
	bool ResolveProjectOutputDirectory(const FString& RequestedOutputRoot, FString& OutDirectory, FString& OutFailureReason);

	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName)
	{
		return MakeExecutionResult(
			FString::Printf(TEXT("Tool '%s' is blocked while Play In Editor is active or starting."), *ToolName),
			nullptr,
			true);
	}

		bool LoadJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject)
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
		}

		bool TryGetStringArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<FString>& OutValues)
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!Arguments.TryGetArrayField(FieldName, JsonArray) || !JsonArray)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
			{
				FString StringValue;
				if (Value.IsValid() && Value->TryGetString(StringValue))
				{
					OutValues.Add(StringValue);
				}
			}

			return true;
		}

		TArray<TSharedPtr<FJsonValue>> MakeJsonStringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}
			return JsonValues;
		}

		bool TryGetObjectArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<TSharedPtr<FJsonObject>>& OutValues)
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!Arguments.TryGetArrayField(FieldName, JsonArray) || !JsonArray)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& Value : *JsonArray)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
				{
					return false;
				}

				OutValues.Add(Value->AsObject());
			}

			return true;
		}
}
