#include "UnrealMcpModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "ContentBrowserModule.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ContentWidget.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/Widget.h"
#include "Editor.h"
#include "EditorScriptingHelpers.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Commands/UIAction.h"
#include "HAL/FileManager.h"
#include "GenericPlatform/GenericPlatformOutputDevices.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "IPythonScriptPlugin.h"
#include "IContentBrowserSingleton.h"
#include "IHttpRouter.h"
#include "JsonObjectConverter.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompilerModule.h"
#include "Logging/MessageLog.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Crc.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/StringOutputDevice.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Templates/Atomic.h"
#include "ToolMenus.h"
#include "UnrealMcpChatPanel.h"
#include "UnrealMcpAssistantRun.h"
#include "UnrealMcpActorTools.h"
#include "UnrealMcpBlueprintTools.h"
#include "UnrealMcpEditorTools.h"
#include "UnrealMcpMemoryTools.h"
#include "UnrealMcpScaffoldTools.h"
#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSettings.h"
#include "UnrealMcpSkillTools.h"
#include "UnrealMcpToolExecutionGuard.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistry.h"
#include "UnrealMcpWorkbenchPanel.h"
#include "UnrealMcpWidgetTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "UnrealMcp"

DEFINE_LOG_CATEGORY(LogUnrealMcp);

namespace UnrealMcp
{
	static const FName ChatTabName(TEXT("UnrealMcp.Chat"));
	static const FName WorkbenchTabName(TEXT("UnrealMcp.Workbench"));
	static const FString LatestProtocolVersion = TEXT("2025-06-18");
	static const FString LegacyProtocolVersion = TEXT("2025-03-26");
	static constexpr double GameThreadTimeoutSeconds = 30.0;
	static constexpr int32 DefaultListLimit = 200;

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

	TSharedPtr<FJsonObject> MakeObjectSchema()
	{
		TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
		InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		InputSchema->SetBoolField(TEXT("additionalProperties"), false);
		return InputSchema;
	}

		TSharedPtr<FJsonValue> NormalizeOpenAiSchemaValue(const TSharedPtr<FJsonValue>& Value);
		bool IsOpenAiSchemaCompatibleValue(const TSharedPtr<FJsonValue>& Value, FString& OutReason);
		bool TryGetStringArrayField(const FJsonObject& Arguments, const FString& FieldName, TArray<FString>& OutValues);

		TSharedPtr<FJsonObject> NormalizeOpenAiSchemaObject(const TSharedPtr<FJsonObject>& InputObject)
		{
			if (!InputObject.IsValid())
			{
				return MakeShared<FJsonObject>();
		}

		TSharedPtr<FJsonObject> OutputObject = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InputObject->Values)
		{
			OutputObject->SetField(Pair.Key, NormalizeOpenAiSchemaValue(Pair.Value));
		}

			FString TypeString;
			if (OutputObject->TryGetStringField(TEXT("type"), TypeString) && TypeString == TEXT("object"))
			{
				const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
				if (!OutputObject->TryGetObjectField(TEXT("properties"), PropertiesObject) || !PropertiesObject || !(*PropertiesObject).IsValid())
				{
					OutputObject->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
				}
			}
			else if (TypeString == TEXT("array"))
			{
				const TSharedPtr<FJsonObject>* ItemsObject = nullptr;
				if (OutputObject->TryGetObjectField(TEXT("items"), ItemsObject) && ItemsObject && (*ItemsObject).IsValid())
			{
				OutputObject->SetObjectField(TEXT("items"), NormalizeOpenAiSchemaObject(*ItemsObject));
			}
		}

			return OutputObject;
		}

		TSharedPtr<FJsonValue> NormalizeOpenAiSchemaValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}

		if (Value->Type == EJson::Object && Value->AsObject().IsValid())
		{
			return MakeShared<FJsonValueObject>(NormalizeOpenAiSchemaObject(Value->AsObject()));
		}

		if (Value->Type == EJson::Array)
		{
			TArray<TSharedPtr<FJsonValue>> NormalizedArray;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				NormalizedArray.Add(NormalizeOpenAiSchemaValue(Item));
			}
			return MakeShared<FJsonValueArray>(NormalizedArray);
		}

			return Value;
		}

		bool IsOpenAiSchemaCompatibleObject(const TSharedPtr<FJsonObject>& InputObject, FString& OutReason)
		{
			OutReason.Reset();
			if (!InputObject.IsValid())
			{
				return true;
			}

			FString TypeString;
			if (InputObject->TryGetStringField(TEXT("type"), TypeString) && TypeString == TEXT("object"))
			{
				bool bAllowsAdditionalProperties = false;
				if (InputObject->TryGetBoolField(TEXT("additionalProperties"), bAllowsAdditionalProperties) && bAllowsAdditionalProperties)
				{
					OutReason = TEXT("object schemas with additionalProperties=true are not accepted by the AI function interface");
					return false;
				}

				const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
				if (InputObject->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && (*PropertiesObject).IsValid())
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObject)->Values)
					{
						FString NestedReason;
						if (!IsOpenAiSchemaCompatibleValue(Pair.Value, NestedReason))
						{
							OutReason = FString::Printf(TEXT("property `%s` is incompatible: %s"), *Pair.Key, *NestedReason);
							return false;
						}
					}
				}
			}
			else if (TypeString == TEXT("array"))
			{
				const TSharedPtr<FJsonObject>* ItemsObject = nullptr;
				if (InputObject->TryGetObjectField(TEXT("items"), ItemsObject) && ItemsObject && (*ItemsObject).IsValid())
				{
					FString NestedReason;
					if (!IsOpenAiSchemaCompatibleObject(*ItemsObject, NestedReason))
					{
						OutReason = FString::Printf(TEXT("array items are incompatible: %s"), *NestedReason);
						return false;
					}
				}
			}

			return true;
		}

		bool IsOpenAiSchemaCompatibleValue(const TSharedPtr<FJsonValue>& Value, FString& OutReason)
		{
			OutReason.Reset();
			if (!Value.IsValid())
			{
				return true;
			}

			if (Value->Type == EJson::Object)
			{
				return IsOpenAiSchemaCompatibleObject(Value->AsObject(), OutReason);
			}

			if (Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					FString NestedReason;
					if (!IsOpenAiSchemaCompatibleValue(Item, NestedReason))
					{
						OutReason = NestedReason;
						return false;
					}
				}
			}

			return true;
		}

		FString GetJsonStringAtPath(const TSharedPtr<FJsonObject>& Object, std::initializer_list<const TCHAR*> PathSegments)
		{
			if (!Object.IsValid())
			{
				return FString();
			}

			TSharedPtr<FJsonObject> CurrentObject = Object;
			for (auto It = PathSegments.begin(); It != PathSegments.end(); ++It)
			{
				const bool bIsLast = (It + 1) == PathSegments.end();
				if (bIsLast)
				{
					FString Value;
					if (CurrentObject.IsValid() && CurrentObject->TryGetStringField(*It, Value))
					{
						return Value;
					}

					return FString();
				}

				const TSharedPtr<FJsonObject>* NextObject = nullptr;
				if (!CurrentObject.IsValid()
					|| !CurrentObject->TryGetObjectField(*It, NextObject)
					|| !NextObject
					|| !(*NextObject).IsValid())
				{
					return FString();
				}

				CurrentObject = *NextObject;
			}

			return FString();
		}

		FString ExtractOpenAiResponseFailureDetails(const TSharedPtr<FJsonObject>& ResponseObject)
		{
			const FString ErrorMessage = GetJsonStringAtPath(ResponseObject, { TEXT("error"), TEXT("message") });
			if (!ErrorMessage.IsEmpty())
			{
				return ErrorMessage;
			}

			const FString NestedStatusErrorMessage = GetJsonStringAtPath(ResponseObject, { TEXT("status_details"), TEXT("error"), TEXT("message") });
			if (!NestedStatusErrorMessage.IsEmpty())
			{
				return NestedStatusErrorMessage;
			}

			const FString IncompleteReason = GetJsonStringAtPath(ResponseObject, { TEXT("incomplete_details"), TEXT("reason") });
			if (!IncompleteReason.IsEmpty())
			{
				return IncompleteReason;
			}

			const FString StatusReason = GetJsonStringAtPath(ResponseObject, { TEXT("status_details"), TEXT("reason") });
			if (!StatusReason.IsEmpty())
			{
				return StatusReason;
			}

			const FString StatusMessage = GetJsonStringAtPath(ResponseObject, { TEXT("status_details"), TEXT("message") });
			if (!StatusMessage.IsEmpty())
			{
				return StatusMessage;
			}

			return FString();
		}

	TSharedPtr<FJsonObject> MakeStringProperty(const FString& Description, const FString& DefaultValue = FString())
	{
		TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
		Property->SetStringField(TEXT("type"), TEXT("string"));
		Property->SetStringField(TEXT("description"), Description);
		if (!DefaultValue.IsEmpty())
		{
			Property->SetStringField(TEXT("default"), DefaultValue);
		}
		return Property;
	}

	TSharedPtr<FJsonObject> MakeBoolProperty(const FString& Description, bool bDefaultValue)
	{
		TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
		Property->SetStringField(TEXT("type"), TEXT("boolean"));
		Property->SetStringField(TEXT("description"), Description);
		Property->SetBoolField(TEXT("default"), bDefaultValue);
		return Property;
	}

		TSharedPtr<FJsonObject> MakeNumberProperty(const FString& Description, double DefaultValue)
		{
			TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), TEXT("number"));
			Property->SetStringField(TEXT("description"), Description);
			Property->SetNumberField(TEXT("default"), DefaultValue);
			return Property;
		}

		TSharedPtr<FJsonObject> MakeStringArrayProperty(const FString& Description)
		{
			TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), TEXT("array"));
			Property->SetStringField(TEXT("description"), Description);

			TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
			Items->SetStringField(TEXT("type"), TEXT("string"));
			Property->SetObjectField(TEXT("items"), Items);
			return Property;
		}

		TSharedPtr<FJsonObject> MakeFlexibleObjectProperty(const FString& Description)
		{
			TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), TEXT("object"));
			Property->SetStringField(TEXT("description"), Description);
			Property->SetBoolField(TEXT("additionalProperties"), true);
			return Property;
		}

		TSharedPtr<FJsonObject> MakeObjectArrayProperty(const FString& Description)
		{
			TSharedPtr<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), TEXT("array"));
			Property->SetStringField(TEXT("description"), Description);

			TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
			Items->SetStringField(TEXT("type"), TEXT("object"));
			Items->SetBoolField(TEXT("additionalProperties"), true);
			Property->SetObjectField(TEXT("items"), Items);
			return Property;
		}

		void AddActorQuerySchemaFields(
			const TSharedPtr<FJsonObject>& PropertiesObject,
			bool bIncludeClassPath = true,
			bool bIncludePaths = true,
			bool bIncludeSelectedOnly = true)
		{
			PropertiesObject->SetObjectField(TEXT("filter"), MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			if (bIncludeClassPath)
			{
				PropertiesObject->SetObjectField(TEXT("classPath"), MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.PointLight.")));
			}
			if (bIncludePaths)
			{
				PropertiesObject->SetObjectField(TEXT("paths"), MakeStringArrayProperty(TEXT("Optional exact actor paths to target.")));
			}
			if (bIncludeSelectedOnly)
			{
				PropertiesObject->SetObjectField(TEXT("selectedOnly"), MakeBoolProperty(TEXT("Whether to target only the currently selected actors. If no selectors are provided, selected actors are used automatically."), false));
			}

			PropertiesObject->SetObjectField(TEXT("limit"), MakeNumberProperty(TEXT("Maximum number of actors to affect."), DefaultListLimit));
		}

		TSharedPtr<FJsonObject> MakeSpawnActorBasicProperties(bool bIncludeClassPath)
		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			if (bIncludeClassPath)
			{
				PropertiesObject->SetObjectField(TEXT("classPath"), MakeStringProperty(TEXT("Native or Blueprint actor class path to spawn.")));
			}

			PropertiesObject->SetObjectField(TEXT("x"), MakeNumberProperty(TEXT("Spawn location X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), MakeNumberProperty(TEXT("Spawn location Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("z"), MakeNumberProperty(TEXT("Spawn location Z."), 0.0));
			PropertiesObject->SetObjectField(TEXT("pitch"), MakeNumberProperty(TEXT("Spawn rotation pitch."), 0.0));
			PropertiesObject->SetObjectField(TEXT("yaw"), MakeNumberProperty(TEXT("Spawn rotation yaw."), 0.0));
			PropertiesObject->SetObjectField(TEXT("roll"), MakeNumberProperty(TEXT("Spawn rotation roll."), 0.0));
			PropertiesObject->SetObjectField(TEXT("sx"), MakeNumberProperty(TEXT("Spawn scale X."), 1.0));
			PropertiesObject->SetObjectField(TEXT("sy"), MakeNumberProperty(TEXT("Spawn scale Y."), 1.0));
			PropertiesObject->SetObjectField(TEXT("sz"), MakeNumberProperty(TEXT("Spawn scale Z."), 1.0));
			PropertiesObject->SetObjectField(TEXT("label"), MakeStringProperty(TEXT("Optional actor label after spawning.")));
			return PropertiesObject;
		}

		void CopyStringFieldIfPresent(const FJsonObject& Source, const FString& FieldName, const TSharedPtr<FJsonObject>& Destination)
		{
			FString Value;
			if (Source.TryGetStringField(FieldName, Value))
			{
				Destination->SetStringField(FieldName, Value);
			}
		}

		void CopyBoolFieldIfPresent(const FJsonObject& Source, const FString& FieldName, const TSharedPtr<FJsonObject>& Destination)
		{
			bool Value = false;
			if (Source.TryGetBoolField(FieldName, Value))
			{
				Destination->SetBoolField(FieldName, Value);
			}
		}

		void CopyNumberFieldIfPresent(const FJsonObject& Source, const FString& FieldName, const TSharedPtr<FJsonObject>& Destination)
		{
			double Value = 0.0;
			if (Source.TryGetNumberField(FieldName, Value))
			{
				Destination->SetNumberField(FieldName, Value);
			}
		}

		void CopyStringArrayFieldIfPresent(const FJsonObject& Source, const FString& FieldName, const TSharedPtr<FJsonObject>& Destination)
		{
			TArray<FString> Values;
			if (!TryGetStringArrayField(Source, FieldName, Values) || Values.Num() == 0)
			{
				return;
			}

			TArray<TSharedPtr<FJsonValue>> JsonValues;
			for (const FString& Value : Values)
			{
				JsonValues.Add(MakeShared<FJsonValueString>(Value));
			}

			Destination->SetArrayField(FieldName, JsonValues);
		}

		void CopyActorQueryArguments(const FJsonObject& Source, const TSharedPtr<FJsonObject>& Destination, bool bIncludeClassPath = true)
		{
			CopyStringFieldIfPresent(Source, TEXT("filter"), Destination);
			if (bIncludeClassPath)
			{
				CopyStringFieldIfPresent(Source, TEXT("classPath"), Destination);
			}
			CopyStringArrayFieldIfPresent(Source, TEXT("paths"), Destination);
			CopyBoolFieldIfPresent(Source, TEXT("selectedOnly"), Destination);
			CopyNumberFieldIfPresent(Source, TEXT("limit"), Destination);
		}

	void AddToolDefinition(
		TArray<TSharedPtr<FJsonValue>>& ToolsArray,
		const FString& Name,
		const FString& Title,
		const FString& Description,
		const TSharedPtr<FJsonObject>& InputSchema)
	{
		if (!ShouldExposeToolToAi(Name))
		{
			return;
		}

		TSharedPtr<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("name"), Name);
		ToolObject->SetStringField(TEXT("title"), Title);
		ToolObject->SetStringField(TEXT("description"), Description);
		if (const FToolRegistryEntry* RegistryEntry = FindToolRegistryEntry(Name))
		{
			ToolObject->SetStringField(TEXT("category"), RegistryEntry->Category);
			ToolObject->SetStringField(TEXT("handlerName"), RegistryEntry->HandlerName.IsEmpty() ? Name : RegistryEntry->HandlerName);
			ToolObject->SetStringField(TEXT("exposure"), RegistryEntry->Exposure == EToolExposure::Visible ? TEXT("visible") : TEXT("legacy_hidden"));
			ToolObject->SetBoolField(TEXT("explicitRegistryEntry"), RegistryEntry->bLoadedFromExplicitRegistry);
			ToolObject->SetStringField(TEXT("registryNotes"), RegistryEntry->Notes);
		}
		else
		{
			ToolObject->SetStringField(TEXT("category"), TEXT("unregistered"));
			ToolObject->SetStringField(TEXT("handlerName"), Name);
			ToolObject->SetStringField(TEXT("exposure"), TEXT("visible"));
			ToolObject->SetBoolField(TEXT("explicitRegistryEntry"), false);
		}
		ToolObject->SetObjectField(TEXT("inputSchema"), InputSchema);
		ToolObject->SetObjectField(TEXT("policy"), MakeToolPolicyObject(Name));
		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObject));
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

		struct FActorQueryResult
		{
			TArray<AActor*> Actors;
			TArray<FString> RequestedPaths;
			FString FilterText;
			FString ClassPathFilter;
			int32 MatchCount = 0;
			int32 Limit = DefaultListLimit;
			bool bSelectedOnly = false;
			bool bTruncated = false;
		};

		bool MatchesActorFilters(
			AActor* Actor,
			const FString& FilterText,
			const FString& ClassPathFilter,
			const TSet<FString>& ExplicitPaths);

		FProperty* FindPropertyByNameLoose(const UStruct* Struct, const FString& PropertyName)
		{
			if (!Struct)
			{
				return nullptr;
			}

			if (FProperty* ExactProperty = Struct->FindPropertyByName(*PropertyName))
			{
				return ExactProperty;
			}

			for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase)
					|| It->GetAuthoredName().Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}

			return nullptr;
		}

		bool ResolveObjectPropertyPath(
			UObject* RootObject,
			const FString& PropertyPath,
			UObject*& OutOwnerObject,
			FProperty*& OutLeafProperty,
			FProperty*& OutNotifyProperty,
			void*& OutValuePtr,
			FString& OutFailureReason)
		{
			OutOwnerObject = nullptr;
			OutLeafProperty = nullptr;
			OutNotifyProperty = nullptr;
			OutValuePtr = nullptr;
			OutFailureReason.Reset();

			if (!RootObject)
			{
				OutFailureReason = TEXT("Cannot resolve a property path on a null object.");
				return false;
			}

			TArray<FString> RawSegments;
			PropertyPath.ParseIntoArray(RawSegments, TEXT("."), true);

			TArray<FString> Segments;
			for (const FString& RawSegment : RawSegments)
			{
				const FString Segment = RawSegment.TrimStartAndEnd();
				if (!Segment.IsEmpty())
				{
					Segments.Add(Segment);
				}
			}

			if (Segments.Num() == 0)
			{
				OutFailureReason = TEXT("The property path is empty.");
				return false;
			}

			UObject* CurrentOwnerObject = RootObject;
			const UStruct* CurrentStruct = RootObject->GetClass();
			void* CurrentContainer = RootObject;
			FProperty* CurrentNotifyProperty = nullptr;

			for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
			{
				const FString& Segment = Segments[SegmentIndex];
				FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segment);
				if (!Property)
				{
					OutFailureReason = FString::Printf(
						TEXT("Property segment '%s' was not found on '%s' while resolving '%s'."),
						*Segment,
						*CurrentStruct->GetPathName(),
						*PropertyPath);
					return false;
				}

				const bool bIsLastSegment = SegmentIndex == Segments.Num() - 1;
				if (bIsLastSegment)
				{
					OutOwnerObject = CurrentOwnerObject;
					OutLeafProperty = Property;
					OutNotifyProperty = CurrentNotifyProperty ? CurrentNotifyProperty : Property;
					OutValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
					return OutOwnerObject != nullptr && OutLeafProperty != nullptr && OutValuePtr != nullptr;
				}

				if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
				{
					if (!CurrentNotifyProperty)
					{
						CurrentNotifyProperty = Property;
					}

					CurrentContainer = StructProperty->ContainerPtrToValuePtr<void>(CurrentContainer);
					CurrentStruct = StructProperty->Struct;
					continue;
				}

				if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
				{
					UObject* NextObject = ObjectProperty->GetObjectPropertyValue_InContainer(CurrentContainer);
					if (!NextObject)
					{
						OutFailureReason = FString::Printf(
							TEXT("Property segment '%s' on '%s' resolved to null while resolving '%s'."),
							*Segment,
							*CurrentOwnerObject->GetPathName(),
							*PropertyPath);
						return false;
					}

					CurrentOwnerObject = NextObject;
					CurrentStruct = NextObject->GetClass();
					CurrentContainer = NextObject;
					CurrentNotifyProperty = nullptr;
					continue;
				}

				OutFailureReason = FString::Printf(
					TEXT("Property segment '%s' on '%s' is neither a struct nor object property, so '%s' cannot be resolved further."),
					*Segment,
					*CurrentStruct->GetPathName(),
					*PropertyPath);
				return false;
			}

			OutFailureReason = FString::Printf(TEXT("Unable to resolve property path '%s'."), *PropertyPath);
			return false;
		}

		TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr)
		{
			if (!Property || !ValuePtr)
			{
				return MakeShared<FJsonValueNull>();
			}

			if (TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(
				Property,
				ValuePtr,
				0,
				0,
				nullptr,
				nullptr,
				EJsonObjectConversionFlags::WriteTextAsComplexString))
			{
				return JsonValue;
			}

			FString ExportedText;
			Property->ExportTextItem_Direct(ExportedText, ValuePtr, nullptr, nullptr, PPF_None);
			return MakeShared<FJsonValueString>(ExportedText);
		}

		bool ApplyPropertyMapToActor(
			AActor* Actor,
			const FJsonObject& PropertyValues,
			TArray<TSharedPtr<FJsonValue>>& OutEditResults,
			int32& OutSuccessCount,
			int32& OutFailureCount)
		{
			OutSuccessCount = 0;
			OutFailureCount = 0;

			if (!Actor)
			{
				return false;
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : PropertyValues.Values)
			{
				const FString PropertyPath = Entry.Key.TrimStartAndEnd();
				TSharedPtr<FJsonObject> EditObject = MakeShared<FJsonObject>();
				EditObject->SetStringField(TEXT("propertyPath"), PropertyPath);

				if (PropertyPath.IsEmpty())
				{
					EditObject->SetBoolField(TEXT("success"), false);
					EditObject->SetStringField(TEXT("error"), TEXT("Property paths cannot be empty."));
					OutEditResults.Add(MakeShared<FJsonValueObject>(EditObject));
					++OutFailureCount;
					continue;
				}

				UObject* OwnerObject = nullptr;
				FProperty* LeafProperty = nullptr;
				FProperty* NotifyProperty = nullptr;
				void* ValuePtr = nullptr;
				FString FailureReason;
				if (!ResolveObjectPropertyPath(Actor, PropertyPath, OwnerObject, LeafProperty, NotifyProperty, ValuePtr, FailureReason))
				{
					EditObject->SetBoolField(TEXT("success"), false);
					EditObject->SetStringField(TEXT("error"), FailureReason);
					OutEditResults.Add(MakeShared<FJsonValueObject>(EditObject));
					++OutFailureCount;
					continue;
					}

					EditObject->SetStringField(TEXT("ownerObjectPath"), OwnerObject->GetPathName());
					const TSharedPtr<FJsonValue> BeforeValue = UnrealMcp::PropertyValueToJson(LeafProperty, ValuePtr);
					if (BeforeValue.IsValid())
					{
						EditObject->SetField(TEXT("before"), BeforeValue);
				}

				OwnerObject->Modify();
				if (OwnerObject != Actor)
				{
					Actor->Modify();
				}

				OwnerObject->PreEditChange(NotifyProperty);

				FText ImportFailureReason;
				const bool bApplied = FJsonObjectConverter::JsonValueToUProperty(
					Entry.Value,
					LeafProperty,
					ValuePtr,
					0,
					0,
					false,
					&ImportFailureReason,
					nullptr);

				if (bApplied)
				{
					FPropertyChangedEvent PropertyChangedEvent(NotifyProperty, EPropertyChangeType::ValueSet);
					OwnerObject->PostEditChangeProperty(PropertyChangedEvent);
					OwnerObject->MarkPackageDirty();
					Actor->MarkPackageDirty();

					EditObject->SetBoolField(TEXT("success"), true);
					const TSharedPtr<FJsonValue> AfterValue = UnrealMcp::PropertyValueToJson(LeafProperty, ValuePtr);
					if (AfterValue.IsValid())
					{
						EditObject->SetField(TEXT("after"), AfterValue);
					}
					++OutSuccessCount;
				}
				else
				{
					OwnerObject->PostEditChange();
					EditObject->SetBoolField(TEXT("success"), false);
					EditObject->SetStringField(
						TEXT("error"),
						ImportFailureReason.IsEmpty()
							? FString::Printf(TEXT("Failed to apply property '%s'."), *PropertyPath)
							: ImportFailureReason.ToString());
					++OutFailureCount;
				}

				OutEditResults.Add(MakeShared<FJsonValueObject>(EditObject));
			}

			return OutFailureCount == 0;
		}

		bool ResolveActorsFromArguments(
			UEditorActorSubsystem* EditorActorSubsystem,
			const FJsonObject& Arguments,
			FActorQueryResult& OutQuery,
			FString& OutFailureReason)
		{
			OutQuery = FActorQueryResult();
			OutFailureReason.Reset();

			if (!EditorActorSubsystem)
			{
				OutFailureReason = TEXT("EditorActorSubsystem is unavailable.");
				return false;
			}

			Arguments.TryGetStringField(TEXT("filter"), OutQuery.FilterText);
			Arguments.TryGetStringField(TEXT("classPath"), OutQuery.ClassPathFilter);
			Arguments.TryGetBoolField(TEXT("selectedOnly"), OutQuery.bSelectedOnly);
			TryGetStringArrayField(Arguments, TEXT("paths"), OutQuery.RequestedPaths);
			OutQuery.Limit = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("limit"), DefaultListLimit), 1000);

			TSet<FString> ExplicitPaths;
			for (const FString& RequestedPath : OutQuery.RequestedPaths)
			{
				const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
				if (!TrimmedPath.IsEmpty())
				{
					ExplicitPaths.Add(TrimmedPath);
				}
			}

			const bool bHasSelectors = !OutQuery.FilterText.TrimStartAndEnd().IsEmpty()
				|| !OutQuery.ClassPathFilter.TrimStartAndEnd().IsEmpty()
				|| ExplicitPaths.Num() > 0;

			if (OutQuery.bSelectedOnly || !bHasSelectors)
			{
				OutQuery.Actors = EditorActorSubsystem->GetSelectedLevelActors();
				OutQuery.Actors.RemoveAll([](AActor* Actor) { return Actor == nullptr; });
				OutQuery.MatchCount = OutQuery.Actors.Num();

				if (OutQuery.Actors.Num() == 0)
				{
					OutFailureReason = OutQuery.bSelectedOnly
						? TEXT("No actors are currently selected.")
						: TEXT("No actor selectors were provided, and there are no selected actors to act on.");
					return false;
				}

				OutQuery.Actors.Sort([](const AActor& A, const AActor& B)
				{
					return A.GetActorLabel() < B.GetActorLabel();
				});

				if (OutQuery.Actors.Num() > OutQuery.Limit)
				{
					OutQuery.Actors.SetNum(OutQuery.Limit);
					OutQuery.bTruncated = true;
				}

				return true;
			}

			TArray<AActor*> AllActors = EditorActorSubsystem->GetAllLevelActors();
			AllActors.Sort([](const AActor& A, const AActor& B)
			{
				return A.GetActorLabel() < B.GetActorLabel();
			});

			for (AActor* Actor : AllActors)
			{
				if (!Actor || !MatchesActorFilters(Actor, OutQuery.FilterText, OutQuery.ClassPathFilter, ExplicitPaths))
				{
					continue;
				}

				++OutQuery.MatchCount;
				if (OutQuery.Actors.Num() >= OutQuery.Limit)
				{
					OutQuery.bTruncated = true;
					continue;
				}

				OutQuery.Actors.Add(Actor);
			}

			if (OutQuery.MatchCount == 0)
			{
				OutFailureReason = TEXT("No actors matched the provided criteria.");
				return false;
			}

			return true;
		}

		IPythonScriptPlugin* LoadPythonScriptPlugin()
		{
			static const FName PythonScriptPluginModuleName(TEXT("PythonScriptPlugin"));
			if (IPythonScriptPlugin* PythonPlugin = FModuleManager::GetModulePtr<IPythonScriptPlugin>(PythonScriptPluginModuleName))
			{
				return PythonPlugin;
			}

			return FModuleManager::LoadModulePtr<IPythonScriptPlugin>(PythonScriptPluginModuleName);
		}

		bool TryParsePythonFileExecutionScope(const FString& ScopeString, EPythonFileExecutionScope& OutScope)
		{
			if (ScopeString.Equals(TEXT("private"), ESearchCase::IgnoreCase))
			{
				OutScope = EPythonFileExecutionScope::Private;
				return true;
			}

			if (ScopeString.Equals(TEXT("public"), ESearchCase::IgnoreCase))
			{
				OutScope = EPythonFileExecutionScope::Public;
				return true;
			}

			return false;
		}

		FString QuoteShellArgument(const FString& Value)
		{
			FString Escaped = Value;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));

			const bool bNeedsQuotes = Escaped.Contains(TEXT(" "))
				|| Escaped.Contains(TEXT("\t"))
				|| Escaped.Contains(TEXT("\""));

			return bNeedsQuotes
				? FString::Printf(TEXT("\"%s\""), *Escaped)
				: Escaped;
		}

		bool ResolvePythonScriptPath(
			const FString& RequestedPath,
			bool bAllowOutsideProject,
			FString& OutResolvedPath,
			FString& OutFailureReason)
		{
			OutResolvedPath.Reset();
			OutFailureReason.Reset();

			const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				OutFailureReason = TEXT("The scriptPath argument is required.");
				return false;
			}

			FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeDirectoryName(ProjectDir);
			FPaths::CollapseRelativeDirectories(ProjectDir);

			FString ResolvedPath = FPaths::IsRelative(TrimmedPath)
				? FPaths::Combine(ProjectDir, TrimmedPath)
				: TrimmedPath;
			ResolvedPath = FPaths::ConvertRelativePathToFull(ResolvedPath);
			FPaths::NormalizeFilename(ResolvedPath);
			FPaths::CollapseRelativeDirectories(ResolvedPath);

			if (!ResolvedPath.EndsWith(TEXT(".py"), ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(TEXT("scriptPath must point to a .py file. Received '%s'."), *ResolvedPath);
				return false;
			}

			if (!FPaths::FileExists(ResolvedPath))
			{
				OutFailureReason = FString::Printf(TEXT("Python script file does not exist: %s"), *ResolvedPath);
				return false;
			}

			const FString ProjectDirPrefix = ProjectDir.EndsWith(TEXT("/")) ? ProjectDir : ProjectDir + TEXT("/");
			if (!bAllowOutsideProject && !ResolvedPath.StartsWith(ProjectDirPrefix, ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(
					TEXT("scriptPath '%s' is outside the project directory '%s'. Set allowOutsideProject=true to override."),
					*ResolvedPath,
					*ProjectDir);
				return false;
			}

			OutResolvedPath = ResolvedPath;
			return true;
		}

	TArray<FAssetData> GetSelectedAssets()
	{
		TArray<FAssetData> SelectedAssets;
		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		ContentBrowserModule.Get().GetSelectedAssets(SelectedAssets);
		return SelectedAssets;
	}

	TSharedPtr<FJsonObject> MakeAssetObject(const FAssetData& Asset)
	{
		TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
		AssetObject->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
		AssetObject->SetStringField(TEXT("assetName"), Asset.AssetName.ToString());
		AssetObject->SetStringField(TEXT("classPath"), Asset.AssetClassPath.ToString());
		AssetObject->SetStringField(TEXT("objectPath"), Asset.GetSoftObjectPath().ToString());
		return AssetObject;
	}

	TSharedPtr<FJsonObject> MakeVectorObject(const FVector& Vector)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), Vector.X);
		Object->SetNumberField(TEXT("y"), Vector.Y);
		Object->SetNumberField(TEXT("z"), Vector.Z);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeRotatorObject(const FRotator& Rotator)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("pitch"), Rotator.Pitch);
		Object->SetNumberField(TEXT("yaw"), Rotator.Yaw);
		Object->SetNumberField(TEXT("roll"), Rotator.Roll);
		return Object;
	}

	TSharedPtr<FJsonObject> MakeActorObject(AActor* Actor)
	{
		TSharedPtr<FJsonObject> ActorObject = MakeShared<FJsonObject>();
		ActorObject->SetStringField(TEXT("label"), Actor->GetActorLabel());
		ActorObject->SetStringField(TEXT("name"), Actor->GetName());
		ActorObject->SetStringField(TEXT("classPath"), Actor->GetClass()->GetPathName());
		ActorObject->SetStringField(TEXT("path"), Actor->GetPathName());
		ActorObject->SetObjectField(TEXT("location"), MakeVectorObject(Actor->GetActorLocation()));
		ActorObject->SetObjectField(TEXT("rotation"), MakeRotatorObject(Actor->GetActorRotation()));
		return ActorObject;
	}

	FString DescribeAsset(const FAssetData& Asset)
	{
		return FString::Printf(TEXT("%s [%s]"), *Asset.GetSoftObjectPath().ToString(), *Asset.AssetClassPath.ToString());
	}

		FString DescribeActor(AActor* Actor)
		{
			const FVector Location = Actor->GetActorLocation();
			return FString::Printf(
				TEXT("%s (%s) @ [%.1f, %.1f, %.1f]"),
			*Actor->GetActorLabel(),
			*Actor->GetClass()->GetName(),
			Location.X,
				Location.Y,
				Location.Z);
		}

		bool MatchesActorFilters(
			AActor* Actor,
			const FString& FilterText,
			const FString& ClassPathFilter,
			const TSet<FString>& ExplicitPaths)
		{
			const FString ActorLabel = Actor->GetActorLabel();
			const FString ActorName = Actor->GetName();
			const FString ActorClassPath = Actor->GetClass()->GetPathName();
			const FString ActorPath = Actor->GetPathName();

			if (ExplicitPaths.Num() > 0 && !ExplicitPaths.Contains(ActorPath))
			{
				return false;
			}

			if (!FilterText.IsEmpty()
				&& !ActorLabel.Contains(FilterText, ESearchCase::IgnoreCase)
				&& !ActorName.Contains(FilterText, ESearchCase::IgnoreCase)
				&& !ActorClassPath.Contains(FilterText, ESearchCase::IgnoreCase)
				&& !ActorPath.Contains(FilterText, ESearchCase::IgnoreCase))
			{
				return false;
			}

			if (!ClassPathFilter.IsEmpty()
				&& !ActorClassPath.Equals(ClassPathFilter, ESearchCase::IgnoreCase)
				&& !ActorClassPath.Contains(ClassPathFilter, ESearchCase::IgnoreCase))
			{
				return false;
			}

			return true;
		}

		AActor* ResolveActorReference(
			UEditorActorSubsystem* EditorActorSubsystem,
			const FString& ActorPath,
			const FString& ActorLabel,
			FString& OutFailureReason)
		{
			OutFailureReason.Reset();

			if (!EditorActorSubsystem)
			{
				OutFailureReason = TEXT("EditorActorSubsystem is unavailable.");
				return nullptr;
			}

			const TArray<AActor*> AllActors = EditorActorSubsystem->GetAllLevelActors();

			if (!ActorPath.TrimStartAndEnd().IsEmpty())
			{
				for (AActor* Actor : AllActors)
				{
					if (Actor && Actor->GetPathName().Equals(ActorPath, ESearchCase::IgnoreCase))
					{
						return Actor;
					}
				}

				OutFailureReason = FString::Printf(TEXT("No actor matched actorPath '%s'."), *ActorPath);
				return nullptr;
			}

			if (!ActorLabel.TrimStartAndEnd().IsEmpty())
			{
				AActor* MatchedActor = nullptr;
				for (AActor* Actor : AllActors)
				{
					if (Actor && Actor->GetActorLabel().Equals(ActorLabel, ESearchCase::IgnoreCase))
					{
						if (MatchedActor)
						{
							OutFailureReason = FString::Printf(TEXT("Multiple actors matched actorLabel '%s'. Use actorPath instead."), *ActorLabel);
							return nullptr;
						}

						MatchedActor = Actor;
					}
				}

				if (MatchedActor)
				{
					return MatchedActor;
				}

				OutFailureReason = FString::Printf(TEXT("No actor matched actorLabel '%s'."), *ActorLabel);
				return nullptr;
			}

			const TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
			if (SelectedActors.Num() == 1 && SelectedActors[0])
			{
				return SelectedActors[0];
			}

			OutFailureReason = SelectedActors.Num() > 1
				? TEXT("Multiple actors are selected. Provide actorPath or actorLabel.")
				: TEXT("No actor reference was provided, and there is no single selected actor.");
			return nullptr;
		}

		UWorld* ResolveConsoleWorld(const FString& RequestedTarget, FString& OutResolvedTarget, FString& OutFailureReason)
		{
		OutResolvedTarget = TEXT("editor");
		OutFailureReason.Reset();

		if (!GEditor)
		{
			OutFailureReason = TEXT("GEditor is unavailable.");
			return nullptr;
		}

		const FString NormalizedTarget = RequestedTarget.TrimStartAndEnd().ToLower();
		const bool bIsAuto = NormalizedTarget.IsEmpty() || NormalizedTarget == TEXT("auto");
		const bool bWantsPie = NormalizedTarget == TEXT("pie");
		const bool bWantsEditor = NormalizedTarget == TEXT("editor");

		if (!bIsAuto && !bWantsPie && !bWantsEditor)
		{
			OutFailureReason = FString::Printf(TEXT("Unknown console target '%s'. Use auto, editor, or pie."), *RequestedTarget);
			return nullptr;
		}

		if ((bIsAuto || bWantsPie) && GEditor->PlayWorld != nullptr)
		{
			OutResolvedTarget = TEXT("pie");
			return GEditor->PlayWorld;
		}

		if (bWantsPie)
		{
			OutFailureReason = TEXT("No PIE world is currently running.");
			return nullptr;
		}

		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			OutResolvedTarget = TEXT("editor");
			return EditorWorld;
		}

		OutFailureReason = TEXT("The editor world is unavailable.");
		return nullptr;
	}


		void AddAuditIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& Location,
			const FString& Message);
		bool AnalyzeOpenAiSchemaCompatibility(
			const TSharedPtr<FJsonObject>& InputSchema,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			FString& OutReason,
			TSharedPtr<FJsonObject>& OutNormalizedSchema);
		TSharedPtr<FJsonObject> FindToolDefinitionByName(
			const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
			const FString& ToolName);
		FString GetProjectMemoryFilePath();
		bool LoadProjectMemory(TSharedPtr<FJsonObject>& OutMemory, FString& OutFailureReason);
		bool SaveProjectMemory(const TSharedPtr<FJsonObject>& MemoryObject, FString& OutFailureReason);
		TSharedPtr<FJsonObject> MakeProjectMemorySummary(const TSharedPtr<FJsonObject>& EntryObject, bool bIncludeContent);
		FUnrealMcpExecutionResult ProjectMemoryWrite(const FJsonObject& Arguments);
		FUnrealMcpExecutionResult ProjectMemoryRead(const FJsonObject& Arguments);
		FUnrealMcpExecutionResult ProjectMemoryView(const FJsonObject& Arguments);
		FUnrealMcpExecutionResult ProjectMemoryEdit(const FJsonObject& Arguments);
		FUnrealMcpExecutionResult ProjectMemoryDelete(const FJsonObject& Arguments);


		FString HashTextForManifest(const FString& Text);
		FString GetMcpModuleSourcePath();
		FString GetMcpExtensionBackupRoot();
		FString GetUnrealMcpSavedRoot();
		FString GetMcpBuildLogRoot();
		FString GetLatestMcpExtensionManifestPath();
		FString GetMcpExtensionLockPath();
		FString GetMcpProjectStateBackupRoot();
		FString GetMcpModuleHeaderPath();
		FString GetProjectReadmePath();
		FString GetPluginReadmePath();
		bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutFailureReason);
		bool SaveJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& FilePath, FString& OutFailureReason);
		bool ResolveProjectPathInsideProject(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason);
		bool ResolveMcpScaffoldDirectory(const FJsonObject& Arguments, FString& OutDirectory, FString& OutToolName, FString& OutFailureReason);

		bool LoadScaffoldSnippet(
			const FString& ScaffoldDirectory,
			const FString& FileName,
			bool bRequired,
			FString& OutSnippet,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			FString& OutFailureReason);
		TSharedPtr<FJsonObject> ValidateCppSnippetText(
			const FString& SnippetText,
			const FString& SnippetName,
			const FString& ToolName);
		TSharedPtr<FJsonObject> MakeTextDiffObject(const FString& BeforeText, const FString& AfterText, int32 MaxPreviewLines);

		bool TryAcquireExtensionSessionLock(
			const FString& Owner,
			const FString& Reason,
			int32 TtlSeconds,
			bool bForce,
			FString& OutSessionId,
			TSharedPtr<FJsonObject>& OutLockObject,
			FString& OutFailureReason);
		bool ReleaseExtensionSessionLock(const FString& SessionId, bool bForce, FString& OutFailureReason);

		class FScopedMcpExtensionSessionLock
		{
		public:
			FScopedMcpExtensionSessionLock(const FString& ToolName, const FJsonObject& Arguments)
			{
				bool bSkipLock = false;
				bool bForceLock = false;
				double TtlSecondsDouble = 900.0;
				FString Owner = TEXT("Unreal MCP Chat");
				Arguments.TryGetBoolField(TEXT("skipLock"), bSkipLock);
				Arguments.TryGetBoolField(TEXT("forceLock"), bForceLock);
				Arguments.TryGetNumberField(TEXT("lockTtlSeconds"), TtlSecondsDouble);
				Arguments.TryGetStringField(TEXT("lockOwner"), Owner);

				if (bSkipLock)
				{
					bAcquired = true;
					bOwnsLock = false;
					return;
				}

				const int32 TtlSeconds = FMath::Clamp(static_cast<int32>(TtlSecondsDouble), 30, 86400);
				const FString Reason = FString::Printf(TEXT("Executing %s"), *ToolName);
				bAcquired = TryAcquireExtensionSessionLock(Owner, Reason, TtlSeconds, bForceLock, SessionId, LockObject, FailureReason);
				bOwnsLock = bAcquired;
			}

			~FScopedMcpExtensionSessionLock()
			{
				if (bOwnsLock && !SessionId.IsEmpty())
				{
					FString ReleaseFailure;
					ReleaseExtensionSessionLock(SessionId, false, ReleaseFailure);
				}
			}

			bool IsAcquired() const
			{
				return bAcquired;
			}

			FString GetFailureReason() const
			{
				return FailureReason;
			}

			TSharedPtr<FJsonObject> MakeStructuredContent(const FString& Action) const
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), Action);
				StructuredContent->SetBoolField(TEXT("locked"), bAcquired);
				StructuredContent->SetStringField(TEXT("lockPath"), GetMcpExtensionLockPath());
				StructuredContent->SetStringField(TEXT("sessionId"), SessionId);
				if (LockObject.IsValid())
				{
					StructuredContent->SetObjectField(TEXT("lock"), LockObject);
				}
				return StructuredContent;
			}

		private:
			bool bAcquired = false;
			bool bOwnsLock = false;
			FString SessionId;
			FString FailureReason;
			TSharedPtr<FJsonObject> LockObject;
		};

		FString GetHostBuildPlatformName();
		FString GetUnrealBuildScriptPath();
		FString QuoteCommandLineArgument(const FString& Value);
		void ParseBuildLog(const FString& LogText, int32 ReturnCode, const TSharedPtr<FJsonObject>& StructuredContent);
		void WriteBuildTestMemory(
			const FString& MemoryKey,
			const FString& Summary,
			const FString& Status,
			const FString& NextStep,
			const TSharedPtr<FJsonObject>& ContentObject);

	FString GetCommandRemainder(const FString& Input, const FString& Command)
	{
		if (Input.Len() <= Command.Len())
		{
			return FString();
		}

		return Input.Mid(Command.Len()).TrimStartAndEnd();
	}

	bool MatchesCommand(const FString& Input, const FString& Command)
	{
		return Input.Equals(Command, ESearchCase::IgnoreCase)
			|| Input.StartsWith(Command + TEXT(" "), ESearchCase::IgnoreCase);
	}
}

void FUnrealMcpModule::StartupModule()
{
	StartServer();
	SkillActivityTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FUnrealMcpModule::TickSkillActivity), 60.0f);
	RegisterTabSpawner();
	UToolMenus::Get()->RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealMcpModule::RegisterMenus));
}

void FUnrealMcpModule::ShutdownModule()
{
	if (SkillActivityTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(SkillActivityTickerHandle);
		SkillActivityTickerHandle.Reset();
	}
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	UnregisterTabSpawner();
	StopServer();
}

bool FUnrealMcpModule::TickSkillActivity(float DeltaTime)
{
	UnrealMcp::TickSkillActivityRecorder();
	return true;
}

void FUnrealMcpModule::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		UnrealMcp::ChatTabName,
		FOnSpawnTab::CreateRaw(this, &FUnrealMcpModule::SpawnChatTab))
		.SetDisplayName(LOCTEXT("ChatTabTitle", "Unreal MCP Chat"))
		.SetTooltipText(LOCTEXT("ChatTabTooltip", "Open the Unreal MCP command chat window."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		UnrealMcp::WorkbenchTabName,
		FOnSpawnTab::CreateRaw(this, &FUnrealMcpModule::SpawnWorkbenchTab))
		.SetDisplayName(LOCTEXT("WorkbenchTabTitle", "Unreal MCP Workbench"))
		.SetTooltipText(LOCTEXT("WorkbenchTabTooltip", "Open the Unreal MCP self-extension workbench."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FUnrealMcpModule::UnregisterTabSpawner()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UnrealMcp::ChatTabName);
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UnrealMcp::WorkbenchTabName);
	}
}

void FUnrealMcpModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Window")))
	{
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("UnrealMcp"));
		Section.AddMenuEntry(
			TEXT("OpenUnrealMcpChat"),
			LOCTEXT("OpenChatMenuLabel", "Unreal MCP Chat"),
			LOCTEXT("OpenChatMenuTooltip", "Open the Unreal MCP command chat window."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMcpModule::OpenChatTab)));
		Section.AddMenuEntry(
			TEXT("OpenUnrealMcpWorkbench"),
			LOCTEXT("OpenWorkbenchMenuLabel", "Unreal MCP Workbench"),
			LOCTEXT("OpenWorkbenchMenuTooltip", "Open the thin self-extension workbench console."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FUnrealMcpModule::OpenWorkbenchTab)));
	}
}

void FUnrealMcpModule::OpenChatTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(UnrealMcp::ChatTabName);
}

void FUnrealMcpModule::OpenWorkbenchTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(UnrealMcp::WorkbenchTabName);
}

TSharedRef<SDockTab> FUnrealMcpModule::SpawnChatTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab =
		SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SUnrealMcpChatPanel, this)
		];

	return Tab;
}

TSharedRef<SDockTab> FUnrealMcpModule::SpawnWorkbenchTab(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> Tab =
		SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SUnrealMcpWorkbenchPanel, this)
		];

	return Tab;
}

void FUnrealMcpModule::AppendToolDefinitions(TArray<TSharedPtr<FJsonValue>>& ToolsArray) const
{
	{
		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.editor_status"),
			TEXT("Editor Status"),
			TEXT("Returns the current Unreal Editor status, map, selected counts, engine version, and PIE state."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("simulate"), UnrealMcp::MakeBoolProperty(TEXT("Whether to start Simulate In Editor instead of Play In Editor."), false));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.start_pie"),
			TEXT("Start PIE"),
			TEXT("Requests a Play In Editor or Simulate In Editor session."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.stop_pie"),
			TEXT("Stop PIE"),
			TEXT("Stops or cancels the current Play In Editor session."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("command"), UnrealMcp::MakeStringProperty(TEXT("Editor or PIE console command to execute.")));
		PropertiesObject->SetObjectField(TEXT("target"), UnrealMcp::MakeStringProperty(TEXT("Where to run the command: auto, editor, or pie."), TEXT("auto")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.execute_console_command"),
				TEXT("Execute Console Command"),
				TEXT("Runs an editor or PIE console command and returns the captured output."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("command"), UnrealMcp::MakeStringProperty(TEXT("Literal Python code, an expression, or a .py file path with optional arguments. Multiline code is automatically run as ExecuteFile unless autoMode=false.")));
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Python execution mode: Auto, ExecuteFile, ExecuteStatement, or EvaluateStatement."), TEXT("Auto")));
			PropertiesObject->SetObjectField(TEXT("scope"), UnrealMcp::MakeStringProperty(TEXT("Python file execution scope: Private or Public."), TEXT("Private")));
			PropertiesObject->SetObjectField(TEXT("autoMode"), UnrealMcp::MakeBoolProperty(TEXT("Whether to protect explicit ExecuteStatement/EvaluateStatement requests by switching multiline code to ExecuteFile."), true));
			PropertiesObject->SetObjectField(TEXT("forceEnable"), UnrealMcp::MakeBoolProperty(TEXT("Whether to force Python initialization if the plugin is loaded but not initialized."), true));
			PropertiesObject->SetObjectField(TEXT("unattended"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run the Python command in unattended mode."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.execute_python"),
				TEXT("Execute Python"),
				TEXT("Runs a Python command inside the Unreal Editor and returns its result and captured log output."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("scriptPath"), UnrealMcp::MakeStringProperty(TEXT("Relative or absolute .py file path. Relative paths are resolved against the project directory.")));
			PropertiesObject->SetObjectField(TEXT("args"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional command-line arguments passed to the Python script.")));
			PropertiesObject->SetObjectField(TEXT("scope"), UnrealMcp::MakeStringProperty(TEXT("Python file execution scope: Private or Public."), TEXT("Private")));
			PropertiesObject->SetObjectField(TEXT("forceEnable"), UnrealMcp::MakeBoolProperty(TEXT("Whether to force Python initialization if the plugin is loaded but not initialized."), true));
			PropertiesObject->SetObjectField(TEXT("unattended"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run the Python command in unattended mode."), true));
			PropertiesObject->SetObjectField(TEXT("allowOutsideProject"), UnrealMcp::MakeBoolProperty(TEXT("Allow scriptPath outside this Unreal project directory."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.execute_python_file"),
				TEXT("Execute Python File"),
				TEXT("Runs a Python script file with optional arguments, with project-directory safety checks."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("lines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of log lines to return."), 120.0));
			PropertiesObject->SetObjectField(TEXT("contains"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive substring filter.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.tail_log"),
				TEXT("Tail Editor Log"),
				TEXT("Returns recent lines from the active Unreal Editor log file."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.map_check"),
				TEXT("Map Check"),
				TEXT("Runs Unreal Editor Map Check on the current level and returns summary counts."),
				InputSchema);
		}

	{
		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.list_maps"),
			TEXT("List Maps"),
			TEXT("Lists all map assets under /Game."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Content Browser path such as /Game or /Game/Variant_TwinStick."), TEXT("/Game")));
		PropertiesObject->SetObjectField(TEXT("recursive"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include child paths."), true));
		PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional asset class path filter such as /Script/Engine.Blueprint.")));
		PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of assets to return."), UnrealMcp::DefaultListLimit));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.list_assets"),
			TEXT("List Assets"),
			TEXT("Lists assets under a Content Browser path."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.list_selected_assets"),
			TEXT("List Selected Assets"),
			TEXT("Lists the assets currently selected in the Content Browser."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, and classes.")));
		PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.PointLight.")));
		PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to return."), UnrealMcp::DefaultListLimit));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.list_level_actors"),
			TEXT("List Level Actors"),
			TEXT("Lists actors in the current editor world."),
			InputSchema);
	}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.list_selected_actors"),
			TEXT("List Selected Actors"),
				TEXT("Lists the actors currently selected in the level editor."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.PlayerStart.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to select.")));
			PropertiesObject->SetObjectField(TEXT("clearSelection"), UnrealMcp::MakeBoolProperty(TEXT("Whether to clear the current actor selection before selecting matches."), true));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to select."), UnrealMcp::DefaultListLimit));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.select_actors"),
				TEXT("Select Actors"),
				TEXT("Selects actors in the current level using filters or exact actor paths."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("actorPath"), UnrealMcp::MakeStringProperty(TEXT("Exact actor path to move. If omitted, a single selected actor or actorLabel will be used.")));
			PropertiesObject->SetObjectField(TEXT("actorLabel"), UnrealMcp::MakeStringProperty(TEXT("Exact actor label to move when actorPath is not provided.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("New world X location. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("New world Y location. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("z"), UnrealMcp::MakeNumberProperty(TEXT("New world Z location. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("pitch"), UnrealMcp::MakeNumberProperty(TEXT("New world pitch. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("yaw"), UnrealMcp::MakeNumberProperty(TEXT("New world yaw. Omit to keep current value."), 0.0));
			PropertiesObject->SetObjectField(TEXT("roll"), UnrealMcp::MakeNumberProperty(TEXT("New world roll. Omit to keep current value."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.set_actor_transform"),
				TEXT("Set Actor Transform"),
				TEXT("Sets world location and rotation values on a level actor."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.PointLight.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to edit.")));
			PropertiesObject->SetObjectField(TEXT("selectedOnly"), UnrealMcp::MakeBoolProperty(TEXT("Whether to target only the currently selected actors. If no selectors are provided, selected actors are used automatically."), false));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to edit."), UnrealMcp::DefaultListLimit));
			PropertiesObject->SetObjectField(TEXT("properties"), UnrealMcp::MakeFlexibleObjectProperty(TEXT("Map of property paths to JSON values. Supports nested paths such as RootComponent.RelativeScale3D, PointLightComponent.Intensity, or Tags.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_set_actor_properties"),
				TEXT("Batch Set Actor Properties"),
				TEXT("Applies one or more property edits to a set of actors selected by query or current selection."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UnrealMcp::AddActorQuerySchemaFields(PropertiesObject);
			PropertiesObject->SetObjectField(TEXT("scaleX"), UnrealMcp::MakeNumberProperty(TEXT("New relative scale X. Omit to preserve the current value."), 1.0));
			PropertiesObject->SetObjectField(TEXT("scaleY"), UnrealMcp::MakeNumberProperty(TEXT("New relative scale Y. Omit to preserve the current value."), 1.0));
			PropertiesObject->SetObjectField(TEXT("scaleZ"), UnrealMcp::MakeNumberProperty(TEXT("New relative scale Z. Omit to preserve the current value."), 1.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_set_actor_scale"),
				TEXT("Batch Set Actor Scale"),
				TEXT("Sets relative scale values on one or more actors chosen by query or current selection."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UnrealMcp::AddActorQuerySchemaFields(PropertiesObject);
			PropertiesObject->SetObjectField(TEXT("tags"), UnrealMcp::MakeStringArrayProperty(TEXT("Tags to add or assign to the target actors.")));
			PropertiesObject->SetObjectField(TEXT("replaceExisting"), UnrealMcp::MakeBoolProperty(TEXT("Whether to replace the existing actor tags instead of appending unique tags."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_set_actor_tags"),
				TEXT("Batch Set Actor Tags"),
				TEXT("Adds or replaces actor Tags on one or more actors."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UnrealMcp::AddActorQuerySchemaFields(PropertiesObject);
			PropertiesObject->SetObjectField(TEXT("intensity"), UnrealMcp::MakeNumberProperty(TEXT("Point light intensity."), 5000.0));
			PropertiesObject->SetObjectField(TEXT("attenuationRadius"), UnrealMcp::MakeNumberProperty(TEXT("Point light attenuation radius."), 1000.0));
			PropertiesObject->SetObjectField(TEXT("sourceRadius"), UnrealMcp::MakeNumberProperty(TEXT("Point light source radius."), 0.0));
			PropertiesObject->SetObjectField(TEXT("softSourceRadius"), UnrealMcp::MakeNumberProperty(TEXT("Point light soft source radius."), 0.0));
			PropertiesObject->SetObjectField(TEXT("temperature"), UnrealMcp::MakeNumberProperty(TEXT("Point light temperature in Kelvin."), 6500.0));
			PropertiesObject->SetObjectField(TEXT("useTemperature"), UnrealMcp::MakeBoolProperty(TEXT("Whether the point light should use color temperature."), false));
			PropertiesObject->SetObjectField(TEXT("castShadows"), UnrealMcp::MakeBoolProperty(TEXT("Whether the point light should cast shadows."), true));
			PropertiesObject->SetObjectField(TEXT("visible"), UnrealMcp::MakeBoolProperty(TEXT("Whether the point light component should be visible."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_set_point_light_properties"),
				TEXT("Batch Set Point Light Properties"),
				TEXT("Updates common PointLight properties on one or more actors with PointLightComponents."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			UnrealMcp::AddActorQuerySchemaFields(PropertiesObject);
			PropertiesObject->SetObjectField(TEXT("staticMeshPath"), UnrealMcp::MakeStringProperty(TEXT("Static mesh asset path to assign, for example /Game/LevelPrototyping/Meshes/SM_Cube.")));
			PropertiesObject->SetObjectField(TEXT("materialPath"), UnrealMcp::MakeStringProperty(TEXT("Optional material or material instance asset path to assign.")));
			PropertiesObject->SetObjectField(TEXT("materialSlot"), UnrealMcp::MakeNumberProperty(TEXT("Material slot index to update when materialPath is provided."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.batch_configure_static_mesh_actors"),
				TEXT("Batch Configure Static Mesh Actors"),
				TEXT("Assigns static meshes and optional materials to one or more actors with StaticMeshComponents."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.StaticMeshActor.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to lay out.")));
			PropertiesObject->SetObjectField(TEXT("selectedOnly"), UnrealMcp::MakeBoolProperty(TEXT("Whether to target only the currently selected actors. If no selectors are provided, selected actors are used automatically."), true));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to reposition."), UnrealMcp::DefaultListLimit));
			PropertiesObject->SetObjectField(TEXT("columns"), UnrealMcp::MakeNumberProperty(TEXT("Number of columns before wrapping to a new row."), 5.0));
			PropertiesObject->SetObjectField(TEXT("spacingX"), UnrealMcp::MakeNumberProperty(TEXT("Horizontal spacing between actors in Unreal units."), 300.0));
			PropertiesObject->SetObjectField(TEXT("spacingY"), UnrealMcp::MakeNumberProperty(TEXT("Vertical spacing between actors in Unreal units."), 300.0));
			PropertiesObject->SetObjectField(TEXT("spacingZ"), UnrealMcp::MakeNumberProperty(TEXT("Z offset applied per row."), 0.0));
			PropertiesObject->SetObjectField(TEXT("startX"), UnrealMcp::MakeNumberProperty(TEXT("Optional origin X. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("startY"), UnrealMcp::MakeNumberProperty(TEXT("Optional origin Y. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("startZ"), UnrealMcp::MakeNumberProperty(TEXT("Optional origin Z. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("pitch"), UnrealMcp::MakeNumberProperty(TEXT("Optional uniform pitch applied to every actor. Omit to keep current rotation."), 0.0));
			PropertiesObject->SetObjectField(TEXT("yaw"), UnrealMcp::MakeNumberProperty(TEXT("Optional uniform yaw applied to every actor. Omit to keep current rotation."), 0.0));
			PropertiesObject->SetObjectField(TEXT("roll"), UnrealMcp::MakeNumberProperty(TEXT("Optional uniform roll applied to every actor. Omit to keep current rotation."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.layout_actors_grid"),
				TEXT("Layout Actors Grid"),
				TEXT("Repositions a set of actors into a configurable grid layout."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example /Script/Engine.StaticMeshActor.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to lay out.")));
			PropertiesObject->SetObjectField(TEXT("selectedOnly"), UnrealMcp::MakeBoolProperty(TEXT("Whether to target only the currently selected actors. If no selectors are provided, selected actors are used automatically."), true));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of actors to reposition."), UnrealMcp::DefaultListLimit));
			PropertiesObject->SetObjectField(TEXT("radius"), UnrealMcp::MakeNumberProperty(TEXT("Circle radius in Unreal units."), 1000.0));
			PropertiesObject->SetObjectField(TEXT("startAngleDegrees"), UnrealMcp::MakeNumberProperty(TEXT("Start angle in degrees."), 0.0));
			PropertiesObject->SetObjectField(TEXT("arcDegrees"), UnrealMcp::MakeNumberProperty(TEXT("Arc coverage in degrees. Use 360 for a full circle."), 360.0));
			PropertiesObject->SetObjectField(TEXT("centerX"), UnrealMcp::MakeNumberProperty(TEXT("Optional center X. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("centerY"), UnrealMcp::MakeNumberProperty(TEXT("Optional center Y. Omit to use the first target actor's location."), 0.0));
			PropertiesObject->SetObjectField(TEXT("centerZ"), UnrealMcp::MakeNumberProperty(TEXT("Optional center Z. Omit to keep each actor's current Z."), 0.0));
			PropertiesObject->SetObjectField(TEXT("alignYawToCenter"), UnrealMcp::MakeBoolProperty(TEXT("Whether each actor should face the center point."), false));
			PropertiesObject->SetObjectField(TEXT("yawOffset"), UnrealMcp::MakeNumberProperty(TEXT("Additional yaw offset in degrees when alignYawToCenter is enabled."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.layout_actors_circle"),
				TEXT("Layout Actors Circle"),
				TEXT("Repositions actors around a circular or arc layout."),
				InputSchema);
		}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Map asset path such as /Game/TopDown/Maps/Lvl_TopDown.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.open_map"),
			TEXT("Open Map"),
			TEXT("Loads a map asset into the editor."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Asset path such as /Game/Variant_TwinStick/Blueprints/BP_TwinStickCharacter.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.open_asset"),
			TEXT("Open Asset"),
			TEXT("Loads an asset and opens it in the appropriate editor."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Asset or folder path to focus in the Content Browser.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.sync_content_browser"),
			TEXT("Sync Content Browser"),
			TEXT("Focuses the Content Browser on an asset or folder."),
			InputSchema);
	}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Native or Blueprint actor class path to spawn.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Spawn location X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Spawn location Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("z"), UnrealMcp::MakeNumberProperty(TEXT("Spawn location Z."), 0.0));
			PropertiesObject->SetObjectField(TEXT("pitch"), UnrealMcp::MakeNumberProperty(TEXT("Spawn rotation pitch."), 0.0));
			PropertiesObject->SetObjectField(TEXT("yaw"), UnrealMcp::MakeNumberProperty(TEXT("Spawn rotation yaw."), 0.0));
			PropertiesObject->SetObjectField(TEXT("roll"), UnrealMcp::MakeNumberProperty(TEXT("Spawn rotation roll."), 0.0));
			PropertiesObject->SetObjectField(TEXT("sx"), UnrealMcp::MakeNumberProperty(TEXT("Spawn scale X. Omit to keep the class default."), 1.0));
			PropertiesObject->SetObjectField(TEXT("sy"), UnrealMcp::MakeNumberProperty(TEXT("Spawn scale Y. Omit to keep the class default."), 1.0));
			PropertiesObject->SetObjectField(TEXT("sz"), UnrealMcp::MakeNumberProperty(TEXT("Spawn scale Z. Omit to keep the class default."), 1.0));
			PropertiesObject->SetObjectField(TEXT("label"), UnrealMcp::MakeStringProperty(TEXT("Optional actor label after spawning.")));
			PropertiesObject->SetObjectField(TEXT("properties"), UnrealMcp::MakeFlexibleObjectProperty(TEXT("Optional property path map to apply immediately after spawn.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.spawn_actor"),
			TEXT("Spawn Actor"),
				TEXT("Spawns an actor into the current editor world."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Default actor class path used by any item that does not override classPath.")));
			PropertiesObject->SetObjectField(TEXT("items"), UnrealMcp::MakeObjectArrayProperty(TEXT("Array of actor spawn specs. Each item can override classPath, transform, label, scale, and properties.")));
			PropertiesObject->SetObjectField(TEXT("selectSpawned"), UnrealMcp::MakeBoolProperty(TEXT("Whether to select the spawned actors after the batch completes."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.spawn_actor_batch"),
					TEXT("Spawn Actor Batch"),
					TEXT("Spawns multiple actors in one call, with per-item transform and property overrides."),
					InputSchema);
			}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), UnrealMcp::MakeSpawnActorBasicProperties(true));

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.spawn_actor_basic"),
				TEXT("Spawn Actor Basic"),
				TEXT("Spawns a single actor with fixed transform, scale, and label fields, without freeform property overrides."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> BatchProperties = MakeShared<FJsonObject>();
			BatchProperties->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Default actor class path used by any batch item that does not override classPath.")));
			BatchProperties->SetObjectField(TEXT("selectSpawned"), UnrealMcp::MakeBoolProperty(TEXT("Whether to select the spawned actors after the batch completes."), true));

			TSharedPtr<FJsonObject> ItemSchema = UnrealMcp::MakeObjectSchema();
			ItemSchema->SetObjectField(TEXT("properties"), UnrealMcp::MakeSpawnActorBasicProperties(true));

			TSharedPtr<FJsonObject> ItemsProperty = MakeShared<FJsonObject>();
			ItemsProperty->SetStringField(TEXT("type"), TEXT("array"));
			ItemsProperty->SetStringField(TEXT("description"), TEXT("Array of actor spawn specs with fixed transform, scale, class, and label fields."));
			ItemsProperty->SetObjectField(TEXT("items"), ItemSchema);
			BatchProperties->SetObjectField(TEXT("items"), ItemsProperty);

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), BatchProperties);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.spawn_actor_batch_basic"),
				TEXT("Spawn Actor Batch Basic"),
				TEXT("Spawns multiple actors in batch using a fixed item schema and no freeform property overrides."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = UnrealMcp::MakeSpawnActorBasicProperties(false);
			PropertiesObject->SetObjectField(TEXT("staticMeshPath"), UnrealMcp::MakeStringProperty(TEXT("Static mesh asset path to assign after spawning, for example /Game/LevelPrototyping/Meshes/SM_Cube.")));
			PropertiesObject->SetObjectField(TEXT("materialPath"), UnrealMcp::MakeStringProperty(TEXT("Optional material or material instance asset path to assign.")));
			PropertiesObject->SetObjectField(TEXT("materialSlot"), UnrealMcp::MakeNumberProperty(TEXT("Material slot index to update when materialPath is provided."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.spawn_static_mesh_actor"),
				TEXT("Spawn Static Mesh Actor"),
				TEXT("Spawns a StaticMeshActor and assigns a static mesh, with optional material override."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.destroy_selected_actors"),
				TEXT("Destroy Selected Actors"),
				TEXT("Destroys the currently selected actors."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("filter"), UnrealMcp::MakeStringProperty(TEXT("Optional substring filter applied to actor labels, names, classes, and paths. If omitted with no classPath or paths, all level actors are targeted.")));
			PropertiesObject->SetObjectField(TEXT("classPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class path filter, for example RecastNavMesh or /Script/Engine.PlayerStart.")));
			PropertiesObject->SetObjectField(TEXT("paths"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional exact actor paths to clear.")));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview actors that would be destroyed without deleting them."), true));
			PropertiesObject->SetObjectField(TEXT("confirmClearAll"), UnrealMcp::MakeBoolProperty(TEXT("Required with dryRun=false when no filter, classPath, or paths are provided."), false));
			PropertiesObject->SetObjectField(TEXT("clearSelection"), UnrealMcp::MakeBoolProperty(TEXT("Whether to clear actor selection before and after the operation."), true));
			PropertiesObject->SetObjectField(TEXT("maxPasses"), UnrealMcp::MakeNumberProperty(TEXT("Maximum destroy passes. Extra passes catch actors such as navigation data that can be recreated after the first pass."), 3.0));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum actors to destroy per pass."), 10000.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.clear_level_environment"),
				TEXT("Clear Level Environment"),
				TEXT("Destructively clears actors from the current editor level. With no filters it targets all level actors and repeats a few passes to catch regenerated actors."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("name"), UnrealMcp::MakeStringProperty(TEXT("New member variable name.")));
			PropertiesObject->SetObjectField(TEXT("pinCategory"), UnrealMcp::MakeStringProperty(TEXT("Blueprint pin category: bool, int, int64, float, double, string, name, text, object, class, struct, enum, byte, or wildcard."), TEXT("bool")));
			PropertiesObject->SetObjectField(TEXT("pinSubCategory"), UnrealMcp::MakeStringProperty(TEXT("Optional pin subcategory. For real pins use float or double; otherwise usually blank.")));
			PropertiesObject->SetObjectField(TEXT("subCategoryObjectPath"), UnrealMcp::MakeStringProperty(TEXT("Optional class, struct, or enum path used by object/class/struct/enum pins.")));
			PropertiesObject->SetObjectField(TEXT("containerType"), UnrealMcp::MakeStringProperty(TEXT("Optional container type: none, array, set, or map."), TEXT("none")));
			PropertiesObject->SetObjectField(TEXT("defaultValue"), UnrealMcp::MakeStringProperty(TEXT("Optional default value stored as Blueprint default text.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_variable"),
				TEXT("Blueprint Add Variable"),
				TEXT("Adds a member variable to a Blueprint using a fixed K2 pin type schema."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("functionName"), UnrealMcp::MakeStringProperty(TEXT("New function graph name.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_function"),
				TEXT("Blueprint Add Function"),
				TEXT("Creates a user function graph in a Blueprint, or returns the existing graph if it already exists."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("eventName"), UnrealMcp::MakeStringProperty(TEXT("Event function name, for example ReceiveBeginPlay. If ownerClassPath is custom, creates a custom event with this name."), TEXT("ReceiveBeginPlay")));
			PropertiesObject->SetObjectField(TEXT("ownerClassPath"), UnrealMcp::MakeStringProperty(TEXT("Class that owns the override event, for example /Script/Engine.Actor. Use custom to create a custom event."), TEXT("/Script/Engine.Actor")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Graph X position."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Graph Y position."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_event_node"),
				TEXT("Blueprint Add Event Node"),
				TEXT("Adds an override event node or custom event node to a Blueprint graph."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("functionClassPath"), UnrealMcp::MakeStringProperty(TEXT("Class path that owns the function, for example /Script/Engine.KismetSystemLibrary.")));
			PropertiesObject->SetObjectField(TEXT("functionName"), UnrealMcp::MakeStringProperty(TEXT("Function name to call.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Graph X position."), 200.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Graph Y position."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_call_function_node"),
				TEXT("Blueprint Add Call Function Node"),
				TEXT("Adds a K2 call-function node for an existing UFunction."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Graph X position."), 400.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Graph Y position."), 0.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_branch_node"),
				TEXT("Blueprint Add Branch Node"),
				TEXT("Adds a Branch node to a Blueprint graph."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("macroName"), UnrealMcp::MakeStringProperty(TEXT("StandardMacros macro graph name, usually ForEachLoop or ForEachLoopWithBreak."), TEXT("ForEachLoop")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Graph X position."), 400.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Graph Y position."), 180.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_add_for_each_node"),
				TEXT("Blueprint Add ForEach Node"),
				TEXT("Adds a StandardMacros ForEach macro instance node to a Blueprint graph."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("fromNodeGuid"), UnrealMcp::MakeStringProperty(TEXT("Source node GUID returned by a bp_add_* tool.")));
			PropertiesObject->SetObjectField(TEXT("fromPin"), UnrealMcp::MakeStringProperty(TEXT("Source pin name or display name.")));
			PropertiesObject->SetObjectField(TEXT("toNodeGuid"), UnrealMcp::MakeStringProperty(TEXT("Target node GUID returned by a bp_add_* tool.")));
			PropertiesObject->SetObjectField(TEXT("toPin"), UnrealMcp::MakeStringProperty(TEXT("Target pin name or display name.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_connect_pins"),
				TEXT("Blueprint Connect Pins"),
				TEXT("Connects two pins in a Blueprint graph using the K2 schema validation rules."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("nodeGuid"), UnrealMcp::MakeStringProperty(TEXT("Node GUID returned by a bp_add_* tool.")));
			PropertiesObject->SetObjectField(TEXT("pinName"), UnrealMcp::MakeStringProperty(TEXT("Pin name or display name to update.")));
			PropertiesObject->SetObjectField(TEXT("value"), UnrealMcp::MakeStringProperty(TEXT("New default value text.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_set_pin_default"),
				TEXT("Blueprint Set Pin Default"),
				TEXT("Sets a pin default value using the K2 schema."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("graphName"), UnrealMcp::MakeStringProperty(TEXT("Target graph name. Defaults to EventGraph."), TEXT("EventGraph")));
			PropertiesObject->SetObjectField(TEXT("originX"), UnrealMcp::MakeNumberProperty(TEXT("Layout origin X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("originY"), UnrealMcp::MakeNumberProperty(TEXT("Layout origin Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("columnSpacing"), UnrealMcp::MakeNumberProperty(TEXT("Horizontal graph spacing."), 320.0));
			PropertiesObject->SetObjectField(TEXT("rowSpacing"), UnrealMcp::MakeNumberProperty(TEXT("Vertical graph spacing."), 180.0));
			PropertiesObject->SetObjectField(TEXT("columns"), UnrealMcp::MakeNumberProperty(TEXT("Number of columns before wrapping."), 4.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.bp_arrange_graph"),
				TEXT("Blueprint Arrange Graph"),
				TEXT("Arranges top-level nodes in a Blueprint graph into a simple grid."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("blueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to compile and optionally save.")));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save the Blueprint package after compiling."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.bp_compile_save"),
			TEXT("Blueprint Compile Save"),
			TEXT("Compiles a Blueprint and optionally saves its package."),
			InputSchema);
	}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("parentWidgetName"), UnrealMcp::MakeStringProperty(TEXT("Parent panel widget name. Empty uses the root widget; if there is no root, the new widget becomes root.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Name for the new widget. If omitted, a unique name is generated from the widget class.")));
			PropertiesObject->SetObjectField(TEXT("widgetClass"), UnrealMcp::MakeStringProperty(TEXT("Widget class simple name such as CanvasPanel, VerticalBox, Button, TextBlock, or a class path."), TEXT("TextBlock")));
			PropertiesObject->SetObjectField(TEXT("index"), UnrealMcp::MakeNumberProperty(TEXT("Optional child index for panel insertion. Use -1 to append."), -1.0));
			PropertiesObject->SetObjectField(TEXT("isVariable"), UnrealMcp::MakeBoolProperty(TEXT("Whether the widget should be exposed as a Blueprint variable."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_add"),
				TEXT("Widget Add"),
				TEXT("Adds a widget to a Widget Blueprint tree, optionally as root or as a child of a panel widget."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Widget name to remove. Use Root or RootWidget to remove the root.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_remove"),
				TEXT("Widget Remove"),
				TEXT("Removes a widget from a Widget Blueprint tree."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Target widget name. Use Root or RootWidget for the root widget.")));
			PropertiesObject->SetObjectField(TEXT("propertyName"), UnrealMcp::MakeStringProperty(TEXT("Property path to set on the widget, for example Text, RenderOpacity, Visibility, or RenderTransform.Translation.X.")));
			PropertiesObject->SetObjectField(TEXT("value"), UnrealMcp::MakeStringProperty(TEXT("Value text imported into the target property. FText properties accept plain text.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_set_property"),
				TEXT("Widget Set Property"),
				TEXT("Sets a widget property by reflection using a string value, with plain-text handling for FText fields."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Target widget name whose parent slot layout should be edited.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot X position."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot Y position."), 0.0));
			PropertiesObject->SetObjectField(TEXT("width"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot width."), 100.0));
			PropertiesObject->SetObjectField(TEXT("height"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot height."), 40.0));
			PropertiesObject->SetObjectField(TEXT("autoSize"), UnrealMcp::MakeBoolProperty(TEXT("Canvas slot auto-size flag."), false));
			PropertiesObject->SetObjectField(TEXT("zOrder"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot z-order."), 0.0));
			PropertiesObject->SetObjectField(TEXT("alignmentX"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot alignment X 0..1."), 0.0));
			PropertiesObject->SetObjectField(TEXT("alignmentY"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot alignment Y 0..1."), 0.0));
			PropertiesObject->SetObjectField(TEXT("anchorMinX"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot minimum anchor X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("anchorMinY"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot minimum anchor Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("anchorMaxX"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot maximum anchor X."), 0.0));
			PropertiesObject->SetObjectField(TEXT("anchorMaxY"), UnrealMcp::MakeNumberProperty(TEXT("Canvas slot maximum anchor Y."), 0.0));
			PropertiesObject->SetObjectField(TEXT("paddingLeft"), UnrealMcp::MakeNumberProperty(TEXT("Panel slot left padding."), 0.0));
			PropertiesObject->SetObjectField(TEXT("paddingTop"), UnrealMcp::MakeNumberProperty(TEXT("Panel slot top padding."), 0.0));
			PropertiesObject->SetObjectField(TEXT("paddingRight"), UnrealMcp::MakeNumberProperty(TEXT("Panel slot right padding."), 0.0));
			PropertiesObject->SetObjectField(TEXT("paddingBottom"), UnrealMcp::MakeNumberProperty(TEXT("Panel slot bottom padding."), 0.0));
			PropertiesObject->SetObjectField(TEXT("hAlign"), UnrealMcp::MakeStringProperty(TEXT("Horizontal alignment for box/overlay slots: left, center, right, fill.")));
			PropertiesObject->SetObjectField(TEXT("vAlign"), UnrealMcp::MakeStringProperty(TEXT("Vertical alignment for box/overlay slots: top, center, bottom, fill.")));
			PropertiesObject->SetObjectField(TEXT("sizeRule"), UnrealMcp::MakeStringProperty(TEXT("Box slot size rule: fill or automatic."), TEXT("fill")));
			PropertiesObject->SetObjectField(TEXT("sizeValue"), UnrealMcp::MakeNumberProperty(TEXT("Box slot fill size value."), 1.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_set_slot_layout"),
				TEXT("Widget Set Slot Layout"),
				TEXT("Updates common slot layout fields for CanvasPanel, VerticalBox, HorizontalBox, and Overlay parent slots."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Widget variable name to bind the event on, for example RefreshButton.")));
			PropertiesObject->SetObjectField(TEXT("eventName"), UnrealMcp::MakeStringProperty(TEXT("Multicast delegate property name, for example OnClicked on Button."), TEXT("OnClicked")));
			PropertiesObject->SetObjectField(TEXT("functionName"), UnrealMcp::MakeStringProperty(TEXT("Optional generated event function name override. Usually leave blank.")));
			PropertiesObject->SetObjectField(TEXT("x"), UnrealMcp::MakeNumberProperty(TEXT("Optional event node X position after creation."), 0.0));
			PropertiesObject->SetObjectField(TEXT("y"), UnrealMcp::MakeNumberProperty(TEXT("Optional event node Y position after creation."), 0.0));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile first so exposed widget variables exist in the skeleton class."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_bind_event"),
				TEXT("Widget Bind Event"),
				TEXT("Creates or returns a Widget Blueprint component-bound event node for a widget multicast delegate such as Button.OnClicked."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to edit.")));
			PropertiesObject->SetObjectField(TEXT("widgetName"), UnrealMcp::MakeStringProperty(TEXT("Widget name to expose or hide as a Blueprint variable.")));
			PropertiesObject->SetObjectField(TEXT("variableName"), UnrealMcp::MakeStringProperty(TEXT("Optional new variable/widget name. Leave blank to keep the current widget name.")));
			PropertiesObject->SetObjectField(TEXT("expose"), UnrealMcp::MakeBoolProperty(TEXT("Whether the widget should be exposed as a Blueprint variable."), true));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile after changing variable exposure."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_bind_blueprint_variable"),
				TEXT("Widget Bind Blueprint Variable"),
				TEXT("Exposes a designer widget as a Blueprint variable, optionally renaming it first."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("widgetBlueprintPath"), UnrealMcp::MakeStringProperty(TEXT("Widget Blueprint asset path to create or rebuild.")));
			PropertiesObject->SetObjectField(TEXT("templateName"), UnrealMcp::MakeStringProperty(TEXT("Template preset name. Currently supports mcp_demo_hud."), TEXT("mcp_demo_hud")));
			PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Title text for the generated template."), TEXT("MCP Demo")));
			PropertiesObject->SetObjectField(TEXT("replaceRoot"), UnrealMcp::MakeBoolProperty(TEXT("Whether to replace the existing root widget tree."), true));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile after building the template. For large first-time templates, prefer false then call unreal.bp_compile_save."), false));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save the Widget Blueprint package after building. For large first-time templates, prefer false then call unreal.bp_compile_save."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_build_template"),
				TEXT("Widget Build Template"),
				TEXT("Creates or rebuilds a Widget Blueprint with a practical demo HUD hierarchy."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_round_system"),
				TEXT("Scaffold Round System"),
				TEXT("Creates or updates high-level round-flow Blueprint scaffolding: GameMode, GameState, PlayerState, PlayerController, and RoundManagerComponent."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));
			PropertiesObject->SetObjectField(TEXT("replaceWidgetRoot"), UnrealMcp::MakeBoolProperty(TEXT("Whether to rebuild the generated shop Widget Blueprint hierarchy."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_shop_system"),
				TEXT("Scaffold Shop System"),
				TEXT("Creates or updates shop manager Blueprint scaffolding plus a basic shop Widget Blueprint template."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_economy_system"),
				TEXT("Scaffold Economy System"),
				TEXT("Creates or updates food/gold/level economy Blueprint component scaffolding and matching PlayerState variables."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_autobattler_ai"),
				TEXT("Scaffold Autobattler AI"),
				TEXT("Creates or updates placeholder unit, AI controller, and combat manager Blueprint scaffolding for auto-battler combat."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/MCPDemo")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));
			PropertiesObject->SetObjectField(TEXT("replaceWidgetRoot"), UnrealMcp::MakeBoolProperty(TEXT("Whether to rebuild the generated result Widget Blueprint hierarchy."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_result_ui"),
				TEXT("Scaffold Result UI"),
				TEXT("Creates or updates combat result presenter Blueprint scaffolding and a basic result Widget Blueprint template."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("New MCP tool name. Must start with unreal., for example unreal.my_custom_tool.")));
			PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Human-readable tool title.")));
			PropertiesObject->SetObjectField(TEXT("description"), UnrealMcp::MakeStringProperty(TEXT("Short tool description for tools/list and AI tool selection.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative output directory for generated scaffold files."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("argumentSchemaJson"), UnrealMcp::MakeStringProperty(TEXT("Reference JSON schema for the intended arguments. Keep it fixed-schema for AI compatibility.")));
			PropertiesObject->SetObjectField(TEXT("exampleArgumentsJson"), UnrealMcp::MakeStringProperty(TEXT("Example arguments object JSON for the generated test request."), TEXT("{\"message\":\"hello\"}")));
			PropertiesObject->SetObjectField(TEXT("implementationNotes"), UnrealMcp::MakeStringProperty(TEXT("Optional implementation notes to include in the generated README.")));
			PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Whether to overwrite an existing scaffold folder."), false));
			PropertiesObject->SetObjectField(TEXT("includeChatCommandSnippet"), UnrealMcp::MakeBoolProperty(TEXT("Whether to generate an optional direct slash-command snippet."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_mcp_tool"),
				TEXT("Scaffold MCP Tool"),
				TEXT("Generates C++ snippet files, docs, and a test request for adding a new Unreal MCP tool after review and rebuild."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root to scan."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("includeSavedTestScaffolds"), UnrealMcp::MakeBoolProperty(TEXT("Whether to also scan Saved/UnrealMcp/TestScaffolds."), true));
			PropertiesObject->SetObjectField(TEXT("toolNameFilter"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive filter applied to tool names and scaffold paths.")));
			PropertiesObject->SetObjectField(TEXT("readyOnly"), UnrealMcp::MakeBoolProperty(TEXT("Only return scaffolds with all required files and a valid TestRequest.json."), false));
			PropertiesObject->SetObjectField(TEXT("includeFileText"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include full file text for each scaffold file."), false));
			PropertiesObject->SetObjectField(TEXT("maxPreviewChars"), UnrealMcp::MakeNumberProperty(TEXT("Maximum per-file preview characters."), 1200.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_list_scaffolds"),
				TEXT("List MCP Scaffolds"),
				TEXT("Lists generated MCP tool scaffold directories, readiness, tool names, schema status, and test request status."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("MCP tool name whose scaffold should be inspected. Used when scaffoldDir is empty.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory to inspect.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("includeFileText"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include full file text for scaffold files."), false));
			PropertiesObject->SetObjectField(TEXT("maxPreviewChars"), UnrealMcp::MakeNumberProperty(TEXT("Maximum per-file preview characters."), 2000.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_inspect_scaffold"),
				TEXT("Inspect MCP Scaffold"),
				TEXT("Inspects one generated MCP scaffold for required files, schema compatibility, test request validity, snippets, and whether the tool is already loaded."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("snippetText"), UnrealMcp::MakeStringProperty(TEXT("Raw C++ snippet text to validate. If empty, reads scaffoldDir/toolName + snippetName.")));
			PropertiesObject->SetObjectField(TEXT("snippetName"), UnrealMcp::MakeStringProperty(TEXT("Snippet file or alias: ToolDefinition.cpp.snippet, ExecuteToolHandler.cpp.snippet, ChatCommand.cpp.snippet."), TEXT("ExecuteToolHandler.cpp.snippet")));
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Expected MCP tool name for tool-name literal checks.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory to read when snippetText is empty.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_validate_cpp_snippet"),
				TEXT("Validate C++ Snippet"),
				TEXT("Runs static safety checks against MCP scaffold C++ snippets before applying them to the plugin source."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("MCP tool name whose scaffold snippet should be patched. Used when scaffoldDir is empty.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory containing the snippet.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("snippetName"), UnrealMcp::MakeStringProperty(TEXT("Snippet file or alias: ToolDefinition.cpp.snippet, ExecuteToolHandler.cpp.snippet, ChatCommand.cpp.snippet.")));
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Patch mode: replace_all, replace_text, append, or prepend. Auto-selected when empty.")));
			PropertiesObject->SetObjectField(TEXT("newText"), UnrealMcp::MakeStringProperty(TEXT("Replacement text for replace_all mode.")));
			PropertiesObject->SetObjectField(TEXT("findText"), UnrealMcp::MakeStringProperty(TEXT("Exact text to find for replace_text mode.")));
			PropertiesObject->SetObjectField(TEXT("replaceText"), UnrealMcp::MakeStringProperty(TEXT("Replacement text for replace_text mode.")));
			PropertiesObject->SetObjectField(TEXT("appendText"), UnrealMcp::MakeStringProperty(TEXT("Text to append when mode=append.")));
			PropertiesObject->SetObjectField(TEXT("prependText"), UnrealMcp::MakeStringProperty(TEXT("Text to prepend when mode=prepend.")));
			PropertiesObject->SetObjectField(TEXT("replaceAll"), UnrealMcp::MakeBoolProperty(TEXT("Replace all findText occurrences instead of just the first."), false));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview snippet changes without writing the file."), true));
			PropertiesObject->SetObjectField(TEXT("createBackup"), UnrealMcp::MakeBoolProperty(TEXT("Create a timestamped snippet backup before writing."), true));
			PropertiesObject->SetObjectField(TEXT("allowUnsafe"), UnrealMcp::MakeBoolProperty(TEXT("Allow writing snippets that fail static validation. Use only after manual review."), false));
			PropertiesObject->SetObjectField(TEXT("diffPreviewLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum snippet diff preview lines."), 120.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_patch_scaffold_snippet"),
				TEXT("Patch MCP Scaffold Snippet"),
				TEXT("Safely patches a generated MCP scaffold snippet with dry-run diff, static validation, idempotence checks, and backups."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Existing MCP tool name to validate. Used when schemaJson is empty.")));
			PropertiesObject->SetObjectField(TEXT("schemaJson"), UnrealMcp::MakeStringProperty(TEXT("Raw JSON object schema to validate. If set, this takes precedence over toolName.")));
			PropertiesObject->SetObjectField(TEXT("returnNormalizedSchema"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include the normalized schema in structured output."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_validate_tool_schema"),
				TEXT("Validate MCP Tool Schema"),
				TEXT("Checks whether a tool input schema is compatible with OpenAI function calling, including additionalProperties risks."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_tool_audit"),
				TEXT("Audit MCP Tools"),
				TEXT("Read-only audit of registered MCP tools, handlers, README documentation, and AI schema compatibility."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key to highlight in pipeline/workbench status."), TEXT("mcp.extension.pipeline")));
			PropertiesObject->SetObjectField(TEXT("includeBuildLogTail"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include the latest build log tail."), false));
			PropertiesObject->SetObjectField(TEXT("buildLogTailLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum latest build log tail lines."), 80.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_workbench_status"),
				TEXT("MCP Workbench Status"),
				TEXT("Read-only dashboard summary for self-extension health: tools, audit, memory, manifests, build/test artifacts, and supervisor status."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("key"), UnrealMcp::MakeStringProperty(TEXT("Memory entry key."), TEXT("current")));
			PropertiesObject->SetObjectField(TEXT("summary"), UnrealMcp::MakeStringProperty(TEXT("Short human-readable memory summary.")));
			PropertiesObject->SetObjectField(TEXT("status"), UnrealMcp::MakeStringProperty(TEXT("Current status, for example pending, in_progress, blocked, or done.")));
			PropertiesObject->SetObjectField(TEXT("nextStep"), UnrealMcp::MakeStringProperty(TEXT("Next action to resume after restart.")));
			PropertiesObject->SetObjectField(TEXT("contentJson"), UnrealMcp::MakeStringProperty(TEXT("Optional JSON object payload with detailed state.")));
			PropertiesObject->SetObjectField(TEXT("tags"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional tags for this memory entry.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_write"),
				TEXT("Project Memory Write"),
				TEXT("Writes a resumable project memory entry under Saved/UnrealMcp for editor restart handoff."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("key"), UnrealMcp::MakeStringProperty(TEXT("Optional memory entry key. Empty returns all entries.")));
			PropertiesObject->SetObjectField(TEXT("includeContent"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include detailed content payloads."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_read"),
				TEXT("Project Memory Read"),
				TEXT("Reads resumable project memory entries from Saved/UnrealMcp."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("keyFilter"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive substring filter for memory keys.")));
			PropertiesObject->SetObjectField(TEXT("status"), UnrealMcp::MakeStringProperty(TEXT("Optional exact status filter.")));
			PropertiesObject->SetObjectField(TEXT("tag"), UnrealMcp::MakeStringProperty(TEXT("Optional tag filter.")));
			PropertiesObject->SetObjectField(TEXT("includeContent"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include detailed content payloads."), false));
			PropertiesObject->SetObjectField(TEXT("maxEntries"), UnrealMcp::MakeNumberProperty(TEXT("Maximum entries to return."), 50.0));
			PropertiesObject->SetObjectField(TEXT("sortDescending"), UnrealMcp::MakeBoolProperty(TEXT("Sort newest updatedAtUtc entries first."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_view"),
				TEXT("Project Memory View"),
				TEXT("Views persistent project memory with key/status/tag filters and optional content payloads."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("key"), UnrealMcp::MakeStringProperty(TEXT("Memory entry key to edit.")));
			PropertiesObject->SetObjectField(TEXT("summary"), UnrealMcp::MakeStringProperty(TEXT("Optional new summary. Omit to preserve.")));
			PropertiesObject->SetObjectField(TEXT("status"), UnrealMcp::MakeStringProperty(TEXT("Optional new status. Omit to preserve.")));
			PropertiesObject->SetObjectField(TEXT("nextStep"), UnrealMcp::MakeStringProperty(TEXT("Optional new next step. Omit to preserve.")));
			PropertiesObject->SetObjectField(TEXT("contentJson"), UnrealMcp::MakeStringProperty(TEXT("Optional JSON object to merge or replace into content.")));
			PropertiesObject->SetObjectField(TEXT("contentMode"), UnrealMcp::MakeStringProperty(TEXT("Content update mode: merge or replace."), TEXT("merge")));
			PropertiesObject->SetObjectField(TEXT("tags"), UnrealMcp::MakeStringArrayProperty(TEXT("Optional tags to replace, append, or remove.")));
			PropertiesObject->SetObjectField(TEXT("tagsMode"), UnrealMcp::MakeStringProperty(TEXT("Tags update mode: replace, append, or remove."), TEXT("replace")));
			PropertiesObject->SetObjectField(TEXT("createIfMissing"), UnrealMcp::MakeBoolProperty(TEXT("Create the memory entry if it does not exist."), false));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview the edit without writing ProjectMemory.json."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_edit"),
				TEXT("Project Memory Edit"),
				TEXT("Edits one persistent project memory entry with field-level updates, content merge/replace, tags modes, and dry run."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("key"), UnrealMcp::MakeStringProperty(TEXT("Memory entry key to delete.")));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview deletion without writing ProjectMemory.json."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.project_memory_delete"),
				TEXT("Project Memory Delete"),
				TEXT("Deletes one persistent project memory entry. Defaults to dryRun=true for safety."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("roots"), UnrealMcp::MakeStringArrayProperty(TEXT("Project-relative skill roots to scan. Defaults to Tools/UnrealMcpSkills.")));
			PropertiesObject->SetObjectField(TEXT("nameFilter"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive skill name filter.")));
			PropertiesObject->SetObjectField(TEXT("includeText"), UnrealMcp::MakeBoolProperty(TEXT("Include full skill text in results."), false));
			PropertiesObject->SetObjectField(TEXT("maxPreviewChars"), UnrealMcp::MakeNumberProperty(TEXT("Maximum preview characters per skill."), 1200.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.skill_list"),
				TEXT("Skill List"),
				TEXT("Lists project-local SKILL.md or *.skill files that Chat can read and apply as reusable task instructions."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Project skill name to read. Used when skillPath is empty.")));
			PropertiesObject->SetObjectField(TEXT("skillPath"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute path to SKILL.md or *.skill.")));
			PropertiesObject->SetObjectField(TEXT("roots"), UnrealMcp::MakeStringArrayProperty(TEXT("Project-relative skill roots to search by skillName.")));
			PropertiesObject->SetObjectField(TEXT("includeText"), UnrealMcp::MakeBoolProperty(TEXT("Include full skill text."), true));
			PropertiesObject->SetObjectField(TEXT("maxPreviewChars"), UnrealMcp::MakeNumberProperty(TEXT("Maximum preview characters."), 4000.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.skill_read"),
				TEXT("Skill Read"),
				TEXT("Reads one project-local skill and returns title, description, preview, and optionally full text."),
				InputSchema);
		}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Project skill name to apply. Used when skillPath is empty.")));
			PropertiesObject->SetObjectField(TEXT("skillPath"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute path to SKILL.md or *.skill.")));
			PropertiesObject->SetObjectField(TEXT("roots"), UnrealMcp::MakeStringArrayProperty(TEXT("Project-relative skill roots to search by skillName.")));
			PropertiesObject->SetObjectField(TEXT("task"), UnrealMcp::MakeStringProperty(TEXT("Current task/context this skill should be applied to.")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Optional project memory key for recording skill application.")));
			PropertiesObject->SetObjectField(TEXT("writeMemory"), UnrealMcp::MakeBoolProperty(TEXT("Record skill application into project memory."), true));
			PropertiesObject->SetObjectField(TEXT("includeFullText"), UnrealMcp::MakeBoolProperty(TEXT("Return full skill text instead of a shorter preview."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.skill_apply"),
				TEXT("Skill Apply"),
					TEXT("Applies a project-local skill by returning its instructions and optionally recording the application in project memory."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("goal"), UnrealMcp::MakeStringProperty(TEXT("Human-readable goal for this activity recording session.")));
				PropertiesObject->SetObjectField(TEXT("sessionId"), UnrealMcp::MakeStringProperty(TEXT("Optional explicit session id. Defaults to timestamp-guid.")));
				PropertiesObject->SetObjectField(TEXT("recordIntervalSeconds"), UnrealMcp::MakeNumberProperty(TEXT("Heartbeat interval in seconds. Clamped to 10..3600; default 60."), 60.0));
				PropertiesObject->SetObjectField(TEXT("reset"), UnrealMcp::MakeBoolProperty(TEXT("Start a new session instead of resuming current state."), true));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_recording_start"),
					TEXT("Skill Recording Start"),
					TEXT("Starts local high-level activity recording for later skill distillation. Writes JSONL under Saved/UnrealMcp/ActivityLog."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("reason"), UnrealMcp::MakeStringProperty(TEXT("Optional stop reason or session summary.")));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_recording_stop"),
					TEXT("Skill Recording Stop"),
					TEXT("Stops local activity recording after writing a final recording_stopped event."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("includeRecentEvents"), UnrealMcp::MakeBoolProperty(TEXT("Include recent JSONL events from the active session."), false));
				PropertiesObject->SetObjectField(TEXT("maxEvents"), UnrealMcp::MakeNumberProperty(TEXT("Maximum recent events to include."), 20.0));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_activity_status"),
					TEXT("Skill Activity Status"),
					TEXT("Reports the active activity recording session, log paths, draft paths, counters, and optionally recent events."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("sessionId"), UnrealMcp::MakeStringProperty(TEXT("Activity session id to distill. Defaults to current session.")));
				PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Output skill folder name. Defaults to sanitized title/goal.")));
				PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Draft SKILL.md title.")));
				PropertiesObject->SetObjectField(TEXT("goal"), UnrealMcp::MakeStringProperty(TEXT("Override learned goal text.")));
				PropertiesObject->SetObjectField(TEXT("writeDraft"), UnrealMcp::MakeBoolProperty(TEXT("Write draft SKILL.md under Saved/UnrealMcp/SkillDrafts."), true));
				PropertiesObject->SetObjectField(TEXT("includeEvents"), UnrealMcp::MakeBoolProperty(TEXT("Append event summaries to the draft for review."), false));
				PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite an existing draft when writeDraft=true."), true));
				PropertiesObject->SetObjectField(TEXT("maxEvents"), UnrealMcp::MakeNumberProperty(TEXT("Maximum JSONL events to distill."), 200.0));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_distill_from_activity"),
					TEXT("Skill Distill From Activity"),
					TEXT("Summarizes one activity session into a reusable SKILL.md draft and optionally writes it under Saved/UnrealMcp/SkillDrafts."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
				PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Draft skill folder name.")));
				PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Draft title used when draftText is empty.")));
				PropertiesObject->SetObjectField(TEXT("goal"), UnrealMcp::MakeStringProperty(TEXT("Draft goal used when draftText is empty.")));
				PropertiesObject->SetObjectField(TEXT("summary"), UnrealMcp::MakeStringProperty(TEXT("Draft summary used when draftText is empty.")));
				PropertiesObject->SetObjectField(TEXT("draftText"), UnrealMcp::MakeStringProperty(TEXT("Complete SKILL.md text to save as a draft.")));
				PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite an existing draft."), true));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_save_draft"),
					TEXT("Skill Save Draft"),
					TEXT("Writes or replaces a local skill draft under Saved/UnrealMcp/SkillDrafts without promoting it into project skills."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
					PropertiesObject->SetObjectField(TEXT("skillName"), UnrealMcp::MakeStringProperty(TEXT("Skill name to promote under Tools/UnrealMcpSkills.")));
					PropertiesObject->SetObjectField(TEXT("draftPath"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local draft path. Defaults to Saved/UnrealMcp/SkillDrafts/<skillName>/SKILL.md.")));
					PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite existing promoted skill."), false));
					PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview promotion without writing Tools/UnrealMcpSkills."), true));
					PropertiesObject->SetObjectField(TEXT("createBackup"), UnrealMcp::MakeBoolProperty(TEXT("Back up existing promoted SKILL.md and write a manifest before overwriting."), true));

				TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
				InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

				UnrealMcp::AddToolDefinition(
					ToolsArray,
					TEXT("unreal.skill_promote_draft"),
					TEXT("Skill Promote Draft"),
					TEXT("Promotes a reviewed draft into Tools/UnrealMcpSkills/<skillName>/SKILL.md for future Chat skill discovery."),
					InputSchema);
			}

			{
				TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Tool name whose scaffold should be applied. Used when scaffoldDir is empty.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory containing generated snippet files.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview changes without modifying source."), true));
			PropertiesObject->SetObjectField(TEXT("applyChatCommand"), UnrealMcp::MakeBoolProperty(TEXT("Whether to apply ChatCommand.cpp.snippet."), true));
			PropertiesObject->SetObjectField(TEXT("createBackup"), UnrealMcp::MakeBoolProperty(TEXT("Whether to create rollback backup and manifest when dryRun=false."), true));
			PropertiesObject->SetObjectField(TEXT("validateSnippets"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run C++ snippet safety validation before applying."), true));
			PropertiesObject->SetObjectField(TEXT("allowUnsafeSnippets"), UnrealMcp::MakeBoolProperty(TEXT("Allow applying snippets that fail static validation. Use only after manual review."), false));
			PropertiesObject->SetObjectField(TEXT("targetDiffPreviewLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum target source diff preview lines returned during dry run/apply."), 120.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_apply_scaffold"),
				TEXT("Apply MCP Scaffold"),
				TEXT("Safely previews or applies generated MCP tool scaffold snippets into the UnrealMcpModule source with idempotence checks and backups."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("manifestPath"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local manifest path. Defaults to Saved/UnrealMcp/LastExtensionApply.json.")));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview rollback without restoring source."), false));
			PropertiesObject->SetObjectField(TEXT("force"), UnrealMcp::MakeBoolProperty(TEXT("Restore even if current source hash differs from the apply manifest."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_rollback_last_extension"),
				TEXT("Rollback Last MCP Extension"),
				TEXT("Restores UnrealMcpModule.cpp from the last mcp_apply_scaffold backup, with hash safety checks."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Lock mode: status, acquire, release, or refresh."), TEXT("status")));
			PropertiesObject->SetObjectField(TEXT("sessionId"), UnrealMcp::MakeStringProperty(TEXT("Session id for release/refresh.")));
			PropertiesObject->SetObjectField(TEXT("owner"), UnrealMcp::MakeStringProperty(TEXT("Human-readable lock owner."), TEXT("Unreal MCP Chat")));
			PropertiesObject->SetObjectField(TEXT("reason"), UnrealMcp::MakeStringProperty(TEXT("Why this extension session is locked.")));
			PropertiesObject->SetObjectField(TEXT("ttlSeconds"), UnrealMcp::MakeNumberProperty(TEXT("Lock TTL in seconds, clamped to 30..86400."), 900.0));
			PropertiesObject->SetObjectField(TEXT("force"), UnrealMcp::MakeBoolProperty(TEXT("Override a stale or foreign lock. Use carefully."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_lock_extension_session"),
				TEXT("Lock MCP Extension Session"),
				TEXT("Acquires, refreshes, releases, or inspects the MCP extension lock used to avoid simultaneous source edits/builds/tests."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("label"), UnrealMcp::MakeStringProperty(TEXT("Short label for the snapshot directory."), TEXT("manual")));
			PropertiesObject->SetObjectField(TEXT("reason"), UnrealMcp::MakeStringProperty(TEXT("Why this project state backup is being created.")));
			PropertiesObject->SetObjectField(TEXT("includeSource"), UnrealMcp::MakeBoolProperty(TEXT("Include Unreal MCP source/header files."), true));
			PropertiesObject->SetObjectField(TEXT("includeReadmes"), UnrealMcp::MakeBoolProperty(TEXT("Include root and plugin README files."), true));
			PropertiesObject->SetObjectField(TEXT("includeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/ProjectMemory.json."), true));
			PropertiesObject->SetObjectField(TEXT("includeManifests"), UnrealMcp::MakeBoolProperty(TEXT("Include extension apply manifests."), true));
			PropertiesObject->SetObjectField(TEXT("includeBuildLogs"), UnrealMcp::MakeBoolProperty(TEXT("Include the latest few build logs."), false));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview snapshot contents without writing backup files."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_backup_project_state"),
				TEXT("Backup MCP Project State"),
				TEXT("Snapshots Unreal MCP source, README, project memory, manifests, and optional build logs before high-risk extension changes."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("manifestPath"), UnrealMcp::MakeStringProperty(TEXT("Specific project-local/absolute manifest to restore. If empty, selects from ExtensionBackups.")));
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Optional toolName filter when manifestPath is empty.")));
			PropertiesObject->SetObjectField(TEXT("selector"), UnrealMcp::MakeStringProperty(TEXT("Manifest selector when manifestPath is empty: latest or oldest."), TEXT("latest")));
			PropertiesObject->SetObjectField(TEXT("manifestIndex"), UnrealMcp::MakeNumberProperty(TEXT("Optional zero-based candidate index after filtering/sorting; -1 uses selector."), -1.0));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview rollback without restoring source."), false));
			PropertiesObject->SetObjectField(TEXT("force"), UnrealMcp::MakeBoolProperty(TEXT("Restore even if current source hash differs from the apply manifest."), false));
			PropertiesObject->SetObjectField(TEXT("createPreRollbackBackup"), UnrealMcp::MakeBoolProperty(TEXT("Snapshot current project state before a real rollback."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_rollback_to_manifest"),
				TEXT("Rollback MCP To Manifest"),
				TEXT("Restores an MCP extension from a selected historical apply manifest, not only the latest one, with optional pre-rollback snapshot."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("buildLogPath"), UnrealMcp::MakeStringProperty(TEXT("Project-local/absolute build log path. Defaults to newest Saved/UnrealMcp/BuildLogs/*.log.")));
			PropertiesObject->SetObjectField(TEXT("maxErrors"), UnrealMcp::MakeNumberProperty(TEXT("Maximum compiler errors to analyze."), 8.0));
			PropertiesObject->SetObjectField(TEXT("contextLines"), UnrealMcp::MakeNumberProperty(TEXT("Source context lines before/after each error."), 4.0));
			PropertiesObject->SetObjectField(TEXT("includeSourceContext"), UnrealMcp::MakeBoolProperty(TEXT("Include nearby source lines for each parsed error."), true));
			PropertiesObject->SetObjectField(TEXT("autoPatch"), UnrealMcp::MakeBoolProperty(TEXT("Attempt only deterministic safe patches when available."), false));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview any autoPatch changes without writing files."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_compile_error_fix_plan"),
				TEXT("Compile Error Fix Plan"),
				TEXT("Parses build logs into error file/line/source context, probable cause, suggested fixes, and safe auto-patch status."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("platform"), UnrealMcp::MakeStringProperty(TEXT("Launcher platform to generate: all, macos, or windows."), TEXT("all")));
			PropertiesObject->SetObjectField(TEXT("outputDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative output directory for generated supervisor launchers."), TEXT("Tools/UnrealMcpSupervisor")));
			PropertiesObject->SetObjectField(TEXT("label"), UnrealMcp::MakeStringProperty(TEXT("macOS LaunchAgent label.")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Pipeline memory key embedded in generated commands."), TEXT("mcp.extension.pipeline")));
			PropertiesObject->SetObjectField(TEXT("argsJson"), UnrealMcp::MakeStringProperty(TEXT("Pipeline args JSON embedded in generated commands. Defaults to {\"memoryKey\": memoryKey}.")));
			PropertiesObject->SetObjectField(TEXT("endpointUrl"), UnrealMcp::MakeStringProperty(TEXT("MCP endpoint URL used by generated supervisor commands."), TEXT("http://127.0.0.1:8765/mcp")));
			PropertiesObject->SetObjectField(TEXT("supervisorLogDir"), UnrealMcp::MakeStringProperty(TEXT("Directory where generated supervisor commands should write logs."), TEXT("Saved/UnrealMcp/SupervisorLogs")));
			PropertiesObject->SetObjectField(TEXT("editorCmd"), UnrealMcp::MakeStringProperty(TEXT("Optional UnrealEditor executable path for generated commands.")));
			PropertiesObject->SetObjectField(TEXT("installLaunchAgent"), UnrealMcp::MakeBoolProperty(TEXT("Also copy the generated macOS plist to ~/Library/LaunchAgents."), false));
			PropertiesObject->SetObjectField(TEXT("launchAtLoad"), UnrealMcp::MakeBoolProperty(TEXT("Set RunAtLoad=true in the generated macOS LaunchAgent."), false));
			PropertiesObject->SetObjectField(TEXT("autoRestart"), UnrealMcp::MakeBoolProperty(TEXT("Generate commands with supervisor pipeline --auto-restart."), true));
			PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite existing generated launcher files."), true));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview the install generator command without writing launcher files."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_supervisor_install"),
				TEXT("Install MCP Supervisor Launchers"),
				TEXT("Generates macOS LaunchAgent/command and Windows PowerShell launchers for the external Unreal MCP supervisor."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Tool name whose schema/scaffold should drive generated MCP tests.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory. Tests are written under scaffoldDir/Tests by default.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute test output directory. Defaults to scaffoldDir/Tests.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("schemaJson"), UnrealMcp::MakeStringProperty(TEXT("Optional raw input schema JSON. If omitted, the loaded tool schema, scaffold README schema, or TestRequest.json is used.")));
			PropertiesObject->SetObjectField(TEXT("overwrite"), UnrealMcp::MakeBoolProperty(TEXT("Whether to overwrite generated test files when content changes."), true));
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview generated test files without writing them."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_generate_tests"),
				TEXT("Generate MCP Tests"),
				TEXT("Generates a Tests/*.json suite for an MCP tool from its schema, including happy path, missing required, boundary value, and wrong type cases."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("target"), UnrealMcp::MakeStringProperty(TEXT("UBT target to build."), FString::Printf(TEXT("%sEditor"), FApp::GetProjectName())));
			PropertiesObject->SetObjectField(TEXT("platform"), UnrealMcp::MakeStringProperty(TEXT("UBT platform. Empty/default uses the host editor platform."), UnrealMcp::GetHostBuildPlatformName()));
			PropertiesObject->SetObjectField(TEXT("configuration"), UnrealMcp::MakeStringProperty(TEXT("UBT configuration."), TEXT("Development")));
			PropertiesObject->SetObjectField(TEXT("extraArgs"), UnrealMcp::MakeStringProperty(TEXT("Optional additional UBT arguments appended to the build command.")));
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Optional newly integrated tool name to persist into project memory for post-restart testing.")));
			PropertiesObject->SetObjectField(TEXT("testRequestPath"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local TestRequest.json path to persist for post-restart testing.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local Tests directory to persist for post-restart suite testing.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Optional scaffold directory. If testRequestPath is empty, scaffoldDir/TestRequest.json is remembered.")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key used for restart handoff."), TEXT("mcp.extension.build_test")));
			PropertiesObject->SetObjectField(TEXT("writeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to write restart handoff state before and after the build."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_build_editor"),
				TEXT("Build Editor"),
				TEXT("Runs Unreal Build Tool for the editor target, captures build logs, parses errors, and writes restart handoff state for MCP extension testing."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Optional tool name. If empty, read from TestRequest.json or project memory.")));
			PropertiesObject->SetObjectField(TEXT("testRequestPath"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute TestRequest.json path.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute Tests directory. Used when runSuite=true.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory containing TestRequest.json.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName if no testRequestPath/scaffoldDir is provided."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key used to resume after editor restart."), TEXT("mcp.extension.build_test")));
			PropertiesObject->SetObjectField(TEXT("readProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to read test path/tool name from project memory when arguments are omitted."), true));
			PropertiesObject->SetObjectField(TEXT("writeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to write test result back to project memory."), true));
			PropertiesObject->SetObjectField(TEXT("expectToolListed"), UnrealMcp::MakeBoolProperty(TEXT("Whether missing tools/list entry should fail the test."), true));
			PropertiesObject->SetObjectField(TEXT("executeTool"), UnrealMcp::MakeBoolProperty(TEXT("Whether to execute the tools/call request from TestRequest.json after tools/list check."), true));
			PropertiesObject->SetObjectField(TEXT("runSuite"), UnrealMcp::MakeBoolProperty(TEXT("Delegate to unreal.mcp_run_test_suite instead of running one test request."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_run_tool_test"),
				TEXT("Run MCP Tool Test"),
				TEXT("Reads a TestRequest.json or wrapped test case, checks whether the tool is listed, executes the tool call through in-editor MCP handlers, compares expected error state, and records the result."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("Optional tool name. If empty, read from project memory or scaffold TestRequest.json.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute Tests directory containing *.json test cases.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory. Defaults testsDir to scaffoldDir/Tests.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key used to resume after editor restart."), TEXT("mcp.extension.build_test")));
			PropertiesObject->SetObjectField(TEXT("readProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to read tool/scaffold/tests paths from project memory."), true));
			PropertiesObject->SetObjectField(TEXT("writeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to write suite result back to project memory."), true));
			PropertiesObject->SetObjectField(TEXT("executeTool"), UnrealMcp::MakeBoolProperty(TEXT("Whether each test should execute the tools/call request."), true));
			PropertiesObject->SetObjectField(TEXT("stopOnFailure"), UnrealMcp::MakeBoolProperty(TEXT("Stop after the first failed test case."), false));
			PropertiesObject->SetObjectField(TEXT("fallbackToSingleTest"), UnrealMcp::MakeBoolProperty(TEXT("If no Tests/*.json files exist, fall back to scaffoldDir/TestRequest.json."), true));
			PropertiesObject->SetObjectField(TEXT("includePassedStructuredContent"), UnrealMcp::MakeBoolProperty(TEXT("Include structuredContent for passed cases, not only failed cases."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_run_test_suite"),
				TEXT("Run MCP Test Suite"),
				TEXT("Runs all JSON test cases under a scaffold Tests directory and reports pass rate, failed cases, failure text, and structuredContent."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Pipeline mode: auto, apply_build, dry_run, resume_test, test_only."), TEXT("auto")));
			PropertiesObject->SetObjectField(TEXT("toolName"), UnrealMcp::MakeStringProperty(TEXT("MCP tool name to integrate/test.")));
			PropertiesObject->SetObjectField(TEXT("scaffoldDir"), UnrealMcp::MakeStringProperty(TEXT("Project-relative or absolute scaffold directory containing snippets and TestRequest.json.")));
			PropertiesObject->SetObjectField(TEXT("outputRoot"), UnrealMcp::MakeStringProperty(TEXT("Project-relative scaffold root used with toolName."), TEXT("Tools/UnrealMcpToolScaffolds")));
			PropertiesObject->SetObjectField(TEXT("schemaJson"), UnrealMcp::MakeStringProperty(TEXT("Optional schema JSON to validate before applying snippets. If omitted, the scaffold README schema is used when present.")));
			PropertiesObject->SetObjectField(TEXT("testRequestPath"), UnrealMcp::MakeStringProperty(TEXT("Optional TestRequest.json path. Defaults to scaffoldDir/TestRequest.json.")));
			PropertiesObject->SetObjectField(TEXT("testsDir"), UnrealMcp::MakeStringProperty(TEXT("Optional Tests directory. Defaults to scaffoldDir/Tests.")));
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key for restart handoff."), TEXT("mcp.extension.pipeline")));
			PropertiesObject->SetObjectField(TEXT("apply"), UnrealMcp::MakeBoolProperty(TEXT("Whether to apply scaffold snippets after dry run."), true));
			PropertiesObject->SetObjectField(TEXT("build"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run Unreal Build Tool after applying snippets."), true));
			PropertiesObject->SetObjectField(TEXT("runTest"), UnrealMcp::MakeBoolProperty(TEXT("Whether to run the generated tool test when safe in the current editor session."), true));
			PropertiesObject->SetObjectField(TEXT("runTestSuite"), UnrealMcp::MakeBoolProperty(TEXT("Run Tests/*.json suite instead of only TestRequest.json."), true));
			PropertiesObject->SetObjectField(TEXT("generateTests"), UnrealMcp::MakeBoolProperty(TEXT("Generate or refresh Tests/*.json before apply/build/test."), true));
			PropertiesObject->SetObjectField(TEXT("overwriteTests"), UnrealMcp::MakeBoolProperty(TEXT("Overwrite generated test files when content changes."), true));
			PropertiesObject->SetObjectField(TEXT("dryRunOnly"), UnrealMcp::MakeBoolProperty(TEXT("Only run validate and apply dry run; skip apply/build/test."), false));
			PropertiesObject->SetObjectField(TEXT("applyChatCommand"), UnrealMcp::MakeBoolProperty(TEXT("Whether to apply optional ChatCommand.cpp.snippet."), true));
			PropertiesObject->SetObjectField(TEXT("createBackup"), UnrealMcp::MakeBoolProperty(TEXT("Whether to create rollback backup during real apply."), true));
			PropertiesObject->SetObjectField(TEXT("backupProjectState"), UnrealMcp::MakeBoolProperty(TEXT("Create a broad project-state snapshot before real apply/build/test changes."), true));
			PropertiesObject->SetObjectField(TEXT("writeProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether to write pipeline state into project memory."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_extension_pipeline"),
				TEXT("MCP Extension Pipeline"),
				TEXT("High-level MCP extension workflow: validate schema, dry-run apply, apply snippets, write memory, build editor, request restart, and resume tool test."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("memoryKey"), UnrealMcp::MakeStringProperty(TEXT("Project memory key to inspect."), TEXT("mcp.extension.pipeline")));
			PropertiesObject->SetObjectField(TEXT("includeAllMemory"), UnrealMcp::MakeBoolProperty(TEXT("Whether memory summaries should include full content payloads."), false));
			PropertiesObject->SetObjectField(TEXT("includeBuildLogTail"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include the latest build log tail."), true));
			PropertiesObject->SetObjectField(TEXT("buildLogTailLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum latest build log tail lines."), 80.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_pipeline_status"),
				TEXT("MCP Pipeline Status"),
				TEXT("Summarizes project memory, last apply manifest, latest build log, test scaffolds, and recommended next extension step."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("manifestPath"), UnrealMcp::MakeStringProperty(TEXT("Optional project-local manifest path. Defaults to Saved/UnrealMcp/LastExtensionApply.json.")));
			PropertiesObject->SetObjectField(TEXT("maxPreviewLines"), UnrealMcp::MakeNumberProperty(TEXT("Maximum changed lines to include in the diff preview."), 120.0));
			PropertiesObject->SetObjectField(TEXT("includeFullText"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include full before/after source snapshots in structured output."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_diff_last_apply"),
				TEXT("Diff Last MCP Apply"),
				TEXT("Reads the last mcp_apply_scaffold manifest and returns a safe before/after source diff preview from backup snapshots."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("dryRun"), UnrealMcp::MakeBoolProperty(TEXT("Preview cleanup candidates without deleting anything."), true));
			PropertiesObject->SetObjectField(TEXT("cleanTestScaffolds"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/TestScaffolds child directories."), true));
			PropertiesObject->SetObjectField(TEXT("cleanTestRequests"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/TestRequests child directories."), false));
			PropertiesObject->SetObjectField(TEXT("cleanBuildLogs"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/BuildLogs/*.log files."), false));
			PropertiesObject->SetObjectField(TEXT("cleanExtensionBackups"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/ExtensionBackups child directories."), false));
			PropertiesObject->SetObjectField(TEXT("cleanProjectMemory"), UnrealMcp::MakeBoolProperty(TEXT("Include Saved/UnrealMcp/ProjectMemory.json. Use carefully."), false));
			PropertiesObject->SetObjectField(TEXT("maxAgeDays"), UnrealMcp::MakeNumberProperty(TEXT("Only include artifacts at least this many days old. 0 disables age filtering."), 0.0));
			PropertiesObject->SetObjectField(TEXT("nameContains"), UnrealMcp::MakeStringProperty(TEXT("Optional case-insensitive path substring filter for targeted cleanup.")));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.mcp_clean_test_artifacts"),
				TEXT("Clean MCP Test Artifacts"),
				TEXT("Safely previews or deletes generated MCP test scaffolds, test requests, build logs, extension backups, and memory artifacts under Saved/UnrealMcp."),
				InputSchema);
		}

				{
					TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
					PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to compile.")));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.compile_blueprint"),
				TEXT("Compile Blueprint"),
				TEXT("Compiles a Blueprint asset."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("path"), UnrealMcp::MakeStringProperty(TEXT("Content Browser path to scan for Blueprints."), TEXT("/Game")));
			PropertiesObject->SetObjectField(TEXT("recursive"), UnrealMcp::MakeBoolProperty(TEXT("Whether to include child paths."), true));
			PropertiesObject->SetObjectField(TEXT("limit"), UnrealMcp::MakeNumberProperty(TEXT("Maximum number of Blueprints to compile in one call."), 100.0));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.compile_blueprints_in_path"),
				TEXT("Compile Blueprints In Path"),
				TEXT("Finds Blueprint assets under a path and compiles them in batch."),
				InputSchema);
		}

	{
		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("assetPath"), UnrealMcp::MakeStringProperty(TEXT("Blueprint asset path to create, for example /Game/Blueprints/BP_NewActor.")));
		PropertiesObject->SetObjectField(TEXT("parentClass"), UnrealMcp::MakeStringProperty(TEXT("Native or Blueprint parent class path."), TEXT("/Script/Engine.Actor")));
		PropertiesObject->SetObjectField(TEXT("openAfterCreate"), UnrealMcp::MakeBoolProperty(TEXT("Whether to open the asset editor after creation."), true));
		PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile the new Blueprint immediately."), true));

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.create_blueprint_class"),
			TEXT("Create Blueprint Class"),
			TEXT("Creates a Blueprint asset from a parent class."),
			InputSchema);
	}

	{
		TSharedPtr<FJsonObject> SaveMapsProperty = UnrealMcp::MakeBoolProperty(TEXT("Whether to save dirty maps."), true);
		TSharedPtr<FJsonObject> SaveAssetsProperty = UnrealMcp::MakeBoolProperty(TEXT("Whether to save dirty content assets."), true);

		TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
		PropertiesObject->SetObjectField(TEXT("saveMaps"), SaveMapsProperty);
		PropertiesObject->SetObjectField(TEXT("saveAssets"), SaveAssetsProperty);

		TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
		InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

		UnrealMcp::AddToolDefinition(
			ToolsArray,
			TEXT("unreal.save_dirty_packages"),
			TEXT("Save Dirty Packages"),
			TEXT("Saves dirty map packages and/or dirty content packages without prompting."),
			InputSchema);
	}
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteToolFromEditorUI(const FString& ToolName, const FJsonObject& Arguments) const
{
	return ExecuteTool(ToolName, Arguments);
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteTool(const FString& ToolName, const FJsonObject& Arguments) const
{
	const FString RegisteredHandlerName = UnrealMcp::ResolveToolHandlerName(ToolName);
	const UnrealMcp::FToolPolicy ActivityPolicy = UnrealMcp::GetToolPolicy(ToolName);
	if (ActivityPolicy.RiskLevel != UnrealMcp::EToolRiskLevel::ReadOnly)
	{
		TArray<FString> ArgumentKeys;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments.Values)
		{
			ArgumentKeys.Add(Pair.Key);
		}
		ArgumentKeys.Sort();
		TSharedPtr<FJsonObject> ActivityDetails = MakeShared<FJsonObject>();
		ActivityDetails->SetStringField(TEXT("toolName"), ToolName);
		ActivityDetails->SetStringField(TEXT("handlerName"), RegisteredHandlerName);
		ActivityDetails->SetStringField(TEXT("riskLevel"), UnrealMcp::LexToString(ActivityPolicy.RiskLevel));
		ActivityDetails->SetArrayField(TEXT("argumentKeys"), UnrealMcp::MakeJsonStringArray(ArgumentKeys));
		UnrealMcp::RecordSkillActivityEvent(TEXT("mcp_tool_call"), FString::Printf(TEXT("Called MCP tool %s."), *ToolName), ActivityDetails);
	}

	FUnrealMcpExecutionResult Result = ExecuteToolInternal(RegisteredHandlerName, Arguments);
	UnrealMcp::AttachToolExecutionCheck(ToolName, Arguments, Result);
	return Result;
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteToolInternal(const FString& ToolName, const FJsonObject& Arguments) const
{
	FUnrealMcpExecutionResult EditorToolResult;
	if (UnrealMcp::TryExecuteEditorTool(ToolName, Arguments, EditorToolResult))
	{
		return EditorToolResult;
	}

	FUnrealMcpExecutionResult ActorToolResult;
	if (UnrealMcp::TryExecuteActorTool(ToolName, Arguments, ActorToolResult))
	{
		return ActorToolResult;
	}

	FUnrealMcpExecutionResult BlueprintToolResult;
	if (UnrealMcp::TryExecuteBlueprintTool(ToolName, Arguments, BlueprintToolResult))
	{
		return BlueprintToolResult;
	}

	FUnrealMcpExecutionResult WidgetToolResult;
	if (UnrealMcp::TryExecuteWidgetTool(ToolName, Arguments, WidgetToolResult))
	{
		return WidgetToolResult;
	}

	FUnrealMcpExecutionResult ScaffoldToolResult;
	if (UnrealMcp::TryExecuteScaffoldTool(ToolName, Arguments, ScaffoldToolResult))
	{
		return ScaffoldToolResult;
	}

	FUnrealMcpExecutionResult MemoryToolResult;
	if (UnrealMcp::TryExecuteMemoryTool(ToolName, Arguments, MemoryToolResult))
	{
		return MemoryToolResult;
	}

	FUnrealMcpExecutionResult SkillToolResult;
	if (UnrealMcp::TryExecuteSkillTool(
		ToolName,
		Arguments,
		[&ToolName](const FJsonObject& ToolArguments)
		{
			UnrealMcp::FScopedMcpExtensionSessionLock ScopedLock(ToolName, ToolArguments);
			if (!ScopedLock.IsAcquired())
			{
				return UnrealMcp::MakeExecutionResult(ScopedLock.GetFailureReason(), ScopedLock.MakeStructuredContent(TEXT("mcp_extension_lock_failed")), true);
			}
			return UnrealMcp::SkillPromoteDraft(ToolArguments);
		},
		SkillToolResult))
	{
		return SkillToolResult;
	}

	TArray<TSharedPtr<FJsonValue>> ToolDefinitions;
	AppendToolDefinitions(ToolDefinitions);
	FUnrealMcpExecutionResult SelfExtensionToolResult;
	if (UnrealMcp::TryExecuteSelfExtensionTool(
		ToolName,
		Arguments,
		ToolDefinitions,
		[this](const FJsonObject& ToolArguments) { return RunMcpToolTest(ToolArguments); },
		[this](const FJsonObject& ToolArguments) { return RunMcpTestSuite(ToolArguments); },
		[this](const FJsonObject& ToolArguments) { return RunMcpExtensionPipeline(ToolArguments); },
		SelfExtensionToolResult))
	{
		return SelfExtensionToolResult;
	}

	return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unknown tool '%s'."), *ToolName), nullptr, true);
}

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteChatCommand(const FString& Input) const
{
	const FString TrimmedInput = Input.TrimStartAndEnd();
	if (TrimmedInput.IsEmpty())
	{
		return UnrealMcp::MakeExecutionResult(TEXT("Enter a command. Try /help."), nullptr, true);
	}

	if (TrimmedInput.StartsWith(TEXT("/tool "), ESearchCase::IgnoreCase))
	{
		const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/tool"));
		if (Remainder.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /tool <tool-name> <json-args>"), nullptr, true);
		}

		FString ToolName = Remainder;
		FString JsonText;
		int32 SeparatorIndex = INDEX_NONE;
		if (Remainder.FindChar(TEXT(' '), SeparatorIndex))
		{
			ToolName = Remainder.Left(SeparatorIndex).TrimStartAndEnd();
			JsonText = Remainder.Mid(SeparatorIndex + 1).TrimStartAndEnd();
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		if (!JsonText.IsEmpty() && !UnrealMcp::LoadJsonObject(JsonText, ArgumentsObject))
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Failed to parse JSON arguments for /tool."), nullptr, true);
		}

		return ExecuteTool(ToolName, *ArgumentsObject);
	}

		if (TrimmedInput.Equals(TEXT("/help"), ESearchCase::IgnoreCase))
		{
				return UnrealMcp::MakeExecutionResult(
					TEXT("Commands:\n")
					TEXT("/help\n")
					TEXT("/ask <prompt>  (handled by the chat panel AI)\n")
					TEXT("/reset_ai  (handled by the chat panel AI)\n")
					TEXT("/stop_ai  (handled by the chat panel AI)\n")
					TEXT("Plain text in the chat panel is also treated as an AI ask.\n")
					TEXT("/status\n")
					TEXT("/pie [simulate]\n")
					TEXT("/stop_pie\n")
					TEXT("/console stat fps\n")
					TEXT("/log [lines]\n")
					TEXT("/map_check\n")
					TEXT("/maps\n")
					TEXT("/assets [/Game/Path]\n")
					TEXT("/selected assets\n")
					TEXT("/selected actors\n")
						TEXT("/select PlayerStart\n")
						TEXT("/move_selected 0 0 300 [pitch yaw roll]\n")
						TEXT("/set_props {\"selectedOnly\":true,\"properties\":{\"Tags\":[\"Encounter\"],\"RootComponent.RelativeScale3D\":{\"X\":1.25,\"Y\":1.25,\"Z\":1.25}}}\n")
						TEXT("/layout_selected 400 300 [columns] [startX startY startZ]\n")
						TEXT("/layout_circle 1200 [startAngle] [arcDegrees] [centerX centerY centerZ]\n")
						TEXT("/actors [filter]\n")
					TEXT("/open_map /Game/TopDown/Maps/Lvl_TopDown\n")
					TEXT("/open_asset /Game/Variant_TwinStick/Blueprints/BP_TwinStickCharacter\n")
						TEXT("/browse /Game/Variant_TwinStick\n")
						TEXT("/spawn /Script/Engine.PointLight 0 0 150 ChatLight\n")
						TEXT("/spawn_batch {\"classPath\":\"/Script/Engine.PointLight\",\"items\":[{\"x\":0,\"y\":0,\"z\":150,\"label\":\"Light_A\"},{\"x\":300,\"y\":0,\"z\":150,\"label\":\"Light_B\"}]}\n")
						TEXT("/py import unreal; print(unreal.EditorLevelLibrary.get_selected_level_actors())\n")
						TEXT("/py_eval unreal.EditorLevelLibrary.get_editor_world().get_name()\n")
						TEXT("/py_file Tools/mcp_test_script.py\n")
						TEXT("/compile_bp /Game/Blueprints/BP_Test\n")
						TEXT("/compile_bps /Game/TopDown\n")
						TEXT("/new_bp /Game/Blueprints/BP_Test /Script/Engine.Actor\n")
						TEXT("/delete_selected\n")
						TEXT("/save\n")
						TEXT("/tool unreal.spawn_actor {\"classPath\":\"/Script/Engine.PointLight\",\"x\":0,\"y\":0,\"z\":150,\"label\":\"ChatLight\"}\n")
						TEXT("/tool unreal.tail_log {\"lines\":80,\"contains\":\"Error\"}\n")
						TEXT("/tool unreal.select_actors {\"paths\":[\"/Game/TopDown/Lvl_TopDown.Lvl_TopDown:PersistentLevel.PlayerStart_0\"]}\n")
						TEXT("/tool unreal.execute_python {\"command\":\"import unreal\\nprint(unreal.EditorLevelLibrary.get_editor_world())\"}"),
						UnrealMcp::MakeExecutionStructuredMessage(TEXT("help")),
						false);
				}

		if (TrimmedInput.Equals(TEXT("/status"), ESearchCase::IgnoreCase))
		{
			return ExecuteTool(TEXT("unreal.editor_status"), *UnrealMcp::MakeEmptyObject());
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/pie")))
		{
			const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/pie"));
			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetBoolField(TEXT("simulate"), Remainder.Equals(TEXT("simulate"), ESearchCase::IgnoreCase));
			return ExecuteTool(TEXT("unreal.start_pie"), *ArgumentsObject);
		}

		if (TrimmedInput.Equals(TEXT("/stop_pie"), ESearchCase::IgnoreCase) || TrimmedInput.Equals(TEXT("/stop pie"), ESearchCase::IgnoreCase))
		{
			return ExecuteTool(TEXT("unreal.stop_pie"), *UnrealMcp::MakeEmptyObject());
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/console")))
		{
			const FString Command = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/console"));
			if (Command.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /console <command>"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("command"), Command);
			ArgumentsObject->SetStringField(TEXT("target"), TEXT("auto"));
			return ExecuteTool(TEXT("unreal.execute_console_command"), *ArgumentsObject);
		}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/log")))
			{
			const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/log"));
			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			if (!Remainder.IsEmpty())
			{
				TArray<FString> Tokens;
				Remainder.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() > 0)
				{
					int32 ParsedLines = 0;
					if (LexTryParseString(ParsedLines, *Tokens[0]))
					{
						ArgumentsObject->SetNumberField(TEXT("lines"), FMath::Max(1, ParsedLines));
						if (Tokens.Num() > 1)
						{
							TArray<FString> FilterTokens;
							for (int32 Index = 1; Index < Tokens.Num(); ++Index)
							{
								FilterTokens.Add(Tokens[Index]);
							}
							ArgumentsObject->SetStringField(TEXT("contains"), FString::Join(FilterTokens, TEXT(" ")));
						}
					}
					else
					{
						ArgumentsObject->SetStringField(TEXT("contains"), Remainder);
					}
				}
			}

				return ExecuteTool(TEXT("unreal.tail_log"), *ArgumentsObject);
			}

			if (TrimmedInput.Equals(TEXT("/map_check"), ESearchCase::IgnoreCase) || TrimmedInput.Equals(TEXT("/map check"), ESearchCase::IgnoreCase))
			{
				return ExecuteTool(TEXT("unreal.map_check"), *UnrealMcp::MakeEmptyObject());
			}

			if (TrimmedInput.Equals(TEXT("/maps"), ESearchCase::IgnoreCase))
			{
				return ExecuteTool(TEXT("unreal.list_maps"), *UnrealMcp::MakeEmptyObject());
			}

	if (TrimmedInput.Equals(TEXT("/selected assets"), ESearchCase::IgnoreCase) || TrimmedInput.Equals(TEXT("/selected_assets"), ESearchCase::IgnoreCase))
	{
		return ExecuteTool(TEXT("unreal.list_selected_assets"), *UnrealMcp::MakeEmptyObject());
	}

	if (TrimmedInput.Equals(TEXT("/selected actors"), ESearchCase::IgnoreCase) || TrimmedInput.Equals(TEXT("/selected_actors"), ESearchCase::IgnoreCase))
	{
		return ExecuteTool(TEXT("unreal.list_selected_actors"), *UnrealMcp::MakeEmptyObject());
	}

	if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/assets")))
	{
		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/assets"));
		ArgumentsObject->SetStringField(TEXT("path"), Path.IsEmpty() ? TEXT("/Game") : Path);
		ArgumentsObject->SetBoolField(TEXT("recursive"), true);
		return ExecuteTool(TEXT("unreal.list_assets"), *ArgumentsObject);
	}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/actors")))
		{
			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			const FString Filter = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/actors"));
			if (!Filter.IsEmpty())
		{
			ArgumentsObject->SetStringField(TEXT("filter"), Filter);
			}
			return ExecuteTool(TEXT("unreal.list_level_actors"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/select")))
		{
			const FString Filter = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/select"));
			if (Filter.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /select <filter>"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("filter"), Filter);
			ArgumentsObject->SetBoolField(TEXT("clearSelection"), true);
			return ExecuteTool(TEXT("unreal.select_actors"), *ArgumentsObject);
		}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/move_selected")))
			{
			const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/move_selected"));
			TArray<FString> Tokens;
			Remainder.ParseIntoArrayWS(Tokens);
			if (Tokens.Num() < 3)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /move_selected <x> <y> <z> [pitch yaw roll]"), nullptr, true);
			}

			double NumericValues[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
			for (int32 Index = 0; Index < Tokens.Num() && Index < 6; ++Index)
			{
				if (!LexTryParseString(NumericValues[Index], *Tokens[Index]))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /move_selected <x> <y> <z> [pitch yaw roll]"), nullptr, true);
				}
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetNumberField(TEXT("x"), NumericValues[0]);
			ArgumentsObject->SetNumberField(TEXT("y"), NumericValues[1]);
			ArgumentsObject->SetNumberField(TEXT("z"), NumericValues[2]);
			if (Tokens.Num() > 3)
			{
				ArgumentsObject->SetNumberField(TEXT("pitch"), NumericValues[3]);
			}
			if (Tokens.Num() > 4)
			{
				ArgumentsObject->SetNumberField(TEXT("yaw"), NumericValues[4]);
			}
			if (Tokens.Num() > 5)
			{
				ArgumentsObject->SetNumberField(TEXT("roll"), NumericValues[5]);
			}

				return ExecuteTool(TEXT("unreal.set_actor_transform"), *ArgumentsObject);
			}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/set_props")) || UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/setprops")))
			{
				const FString JsonText = UnrealMcp::GetCommandRemainder(
					TrimmedInput,
					TrimmedInput.StartsWith(TEXT("/setprops"), ESearchCase::IgnoreCase) ? TEXT("/setprops") : TEXT("/set_props"));
				if (JsonText.IsEmpty())
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /set_props {\"selectedOnly\":true,\"properties\":{\"Tags\":[\"Encounter\"]}}"), nullptr, true);
				}

				TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
				if (!UnrealMcp::LoadJsonObject(JsonText, ArgumentsObject))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Failed to parse JSON arguments for /set_props."), nullptr, true);
				}

				if (!ArgumentsObject->HasField(TEXT("selectedOnly"))
					&& !ArgumentsObject->HasField(TEXT("filter"))
					&& !ArgumentsObject->HasField(TEXT("classPath"))
					&& !ArgumentsObject->HasField(TEXT("paths")))
				{
					ArgumentsObject->SetBoolField(TEXT("selectedOnly"), true);
				}

				return ExecuteTool(TEXT("unreal.batch_set_actor_properties"), *ArgumentsObject);
			}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/layout_selected")))
			{
				const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/layout_selected"));
				TArray<FString> Tokens;
				Remainder.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() < 2)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /layout_selected <spacingX> <spacingY> [columns] [startX startY startZ]"), nullptr, true);
				}

				double SpacingX = 0.0;
				double SpacingY = 0.0;
				if (!LexTryParseString(SpacingX, *Tokens[0]) || !LexTryParseString(SpacingY, *Tokens[1]))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /layout_selected <spacingX> <spacingY> [columns] [startX startY startZ]"), nullptr, true);
				}

				int32 Columns = 5;
				int32 StartTokenIndex = 2;
				if (Tokens.Num() >= 3)
				{
					if (!LexTryParseString(Columns, *Tokens[2]))
					{
						return UnrealMcp::MakeExecutionResult(TEXT("The optional columns value must be an integer."), nullptr, true);
					}
					StartTokenIndex = 3;
				}

				TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
				ArgumentsObject->SetBoolField(TEXT("selectedOnly"), true);
				ArgumentsObject->SetNumberField(TEXT("spacingX"), SpacingX);
				ArgumentsObject->SetNumberField(TEXT("spacingY"), SpacingY);
				ArgumentsObject->SetNumberField(TEXT("columns"), FMath::Max(1, Columns));

				if (Tokens.Num() > StartTokenIndex)
				{
					if (Tokens.Num() - StartTokenIndex != 3)
					{
						return UnrealMcp::MakeExecutionResult(TEXT("If you provide a start position, include all three values: startX startY startZ."), nullptr, true);
					}

					double StartX = 0.0;
					double StartY = 0.0;
					double StartZ = 0.0;
					if (!LexTryParseString(StartX, *Tokens[StartTokenIndex])
						|| !LexTryParseString(StartY, *Tokens[StartTokenIndex + 1])
						|| !LexTryParseString(StartZ, *Tokens[StartTokenIndex + 2]))
					{
						return UnrealMcp::MakeExecutionResult(TEXT("The optional start position values must be numeric."), nullptr, true);
					}

					ArgumentsObject->SetNumberField(TEXT("startX"), StartX);
					ArgumentsObject->SetNumberField(TEXT("startY"), StartY);
					ArgumentsObject->SetNumberField(TEXT("startZ"), StartZ);
				}

				return ExecuteTool(TEXT("unreal.layout_actors_grid"), *ArgumentsObject);
			}

			if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/layout_circle")))
			{
				const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/layout_circle"));
				TArray<FString> Tokens;
				Remainder.ParseIntoArrayWS(Tokens);
				if (Tokens.Num() < 1)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Usage: /layout_circle <radius> [startAngle] [arcDegrees] [centerX centerY centerZ]"), nullptr, true);
				}

				double Radius = 0.0;
				if (!LexTryParseString(Radius, *Tokens[0]) || Radius <= 0.0)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("The radius value must be a positive number."), nullptr, true);
				}

				double StartAngle = 0.0;
				double ArcDegrees = 360.0;
				if (Tokens.Num() > 1 && !LexTryParseString(StartAngle, *Tokens[1]))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("The optional startAngle value must be numeric."), nullptr, true);
				}
				if (Tokens.Num() > 2 && !LexTryParseString(ArcDegrees, *Tokens[2]))
				{
					return UnrealMcp::MakeExecutionResult(TEXT("The optional arcDegrees value must be numeric."), nullptr, true);
				}

				if (Tokens.Num() != 1 && Tokens.Num() != 2 && Tokens.Num() != 3 && Tokens.Num() != 6)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("Provide either only radius, radius+angles, or radius+angles+centerX centerY centerZ."), nullptr, true);
				}

				TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
				ArgumentsObject->SetBoolField(TEXT("selectedOnly"), true);
				ArgumentsObject->SetNumberField(TEXT("radius"), Radius);
				ArgumentsObject->SetNumberField(TEXT("startAngleDegrees"), StartAngle);
				ArgumentsObject->SetNumberField(TEXT("arcDegrees"), ArcDegrees);

				if (Tokens.Num() == 6)
				{
					double CenterX = 0.0;
					double CenterY = 0.0;
					double CenterZ = 0.0;
					if (!LexTryParseString(CenterX, *Tokens[3])
						|| !LexTryParseString(CenterY, *Tokens[4])
						|| !LexTryParseString(CenterZ, *Tokens[5]))
					{
						return UnrealMcp::MakeExecutionResult(TEXT("centerX, centerY, and centerZ must be numeric."), nullptr, true);
					}

					ArgumentsObject->SetNumberField(TEXT("centerX"), CenterX);
					ArgumentsObject->SetNumberField(TEXT("centerY"), CenterY);
					ArgumentsObject->SetNumberField(TEXT("centerZ"), CenterZ);
				}

				return ExecuteTool(TEXT("unreal.layout_actors_circle"), *ArgumentsObject);
			}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/open_map")))
		{
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/open_map"));
		if (Path.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /open_map /Game/Path/To/Map"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("path"), Path);
		return ExecuteTool(TEXT("unreal.open_map"), *ArgumentsObject);
	}

	if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/open_asset")))
	{
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/open_asset"));
		if (Path.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /open_asset /Game/Path/To/Asset"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("path"), Path);
		return ExecuteTool(TEXT("unreal.open_asset"), *ArgumentsObject);
	}

	if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/browse")))
	{
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/browse"));
		if (Path.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /browse /Game/Path/Or/Asset"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("path"), Path);
		return ExecuteTool(TEXT("unreal.sync_content_browser"), *ArgumentsObject);
	}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/compile_bp")))
		{
		const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/compile_bp"));
		if (Path.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /compile_bp /Game/Path/To/Blueprint"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("path"), Path);
			return ExecuteTool(TEXT("unreal.compile_blueprint"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/compile_bps")))
		{
			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			const FString Path = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/compile_bps"));
			ArgumentsObject->SetStringField(TEXT("path"), Path.IsEmpty() ? TEXT("/Game") : Path);
			ArgumentsObject->SetBoolField(TEXT("recursive"), true);
			return ExecuteTool(TEXT("unreal.compile_blueprints_in_path"), *ArgumentsObject);
		}

	if (TrimmedInput.Equals(TEXT("/delete_selected"), ESearchCase::IgnoreCase))
	{
		return ExecuteTool(TEXT("unreal.destroy_selected_actors"), *UnrealMcp::MakeEmptyObject());
	}

	if (TrimmedInput.Equals(TEXT("/save"), ESearchCase::IgnoreCase))
	{
		return ExecuteTool(TEXT("unreal.save_dirty_packages"), *UnrealMcp::MakeEmptyObject());
	}

	if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/new_bp")))
	{
		const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/new_bp"));
		TArray<FString> Tokens;
		Remainder.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() < 1)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /new_bp /Game/Blueprints/BP_Name [/Script/Engine.Actor]"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("assetPath"), Tokens[0]);
		ArgumentsObject->SetStringField(TEXT("parentClass"), Tokens.Num() >= 2 ? Tokens[1] : TEXT("/Script/Engine.Actor"));
		ArgumentsObject->SetBoolField(TEXT("openAfterCreate"), true);
		ArgumentsObject->SetBoolField(TEXT("compile"), true);
		return ExecuteTool(TEXT("unreal.create_blueprint_class"), *ArgumentsObject);
	}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/spawn")))
		{
		const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/spawn"));
		TArray<FString> Tokens;
		Remainder.ParseIntoArrayWS(Tokens);
		if (Tokens.Num() < 1)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Usage: /spawn <classPath> [x y z [pitch yaw roll]] [label]"), nullptr, true);
		}

		TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
		ArgumentsObject->SetStringField(TEXT("classPath"), Tokens[0]);

		int32 TokenIndex = 1;
		double NumericValues[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
		int32 ParsedNumericCount = 0;
		for (; TokenIndex < Tokens.Num() && ParsedNumericCount < 6; ++TokenIndex)
		{
			double ParsedValue = 0.0;
			if (!LexTryParseString(ParsedValue, *Tokens[TokenIndex]))
			{
				break;
			}

			NumericValues[ParsedNumericCount++] = ParsedValue;
		}

		if (ParsedNumericCount > 0)
		{
			ArgumentsObject->SetNumberField(TEXT("x"), NumericValues[0]);
		}
		if (ParsedNumericCount > 1)
		{
			ArgumentsObject->SetNumberField(TEXT("y"), NumericValues[1]);
		}
		if (ParsedNumericCount > 2)
		{
			ArgumentsObject->SetNumberField(TEXT("z"), NumericValues[2]);
		}
		if (ParsedNumericCount > 3)
		{
			ArgumentsObject->SetNumberField(TEXT("pitch"), NumericValues[3]);
		}
		if (ParsedNumericCount > 4)
		{
			ArgumentsObject->SetNumberField(TEXT("yaw"), NumericValues[4]);
		}
		if (ParsedNumericCount > 5)
		{
			ArgumentsObject->SetNumberField(TEXT("roll"), NumericValues[5]);
		}

		if (TokenIndex < Tokens.Num())
		{
			TArray<FString> LabelTokens;
			for (int32 LabelIndex = TokenIndex; LabelIndex < Tokens.Num(); ++LabelIndex)
			{
				LabelTokens.Add(Tokens[LabelIndex]);
			}

			FString Label = FString::Join(LabelTokens, TEXT(" "));
			if (!Label.IsEmpty())
			{
				ArgumentsObject->SetStringField(TEXT("label"), Label);
			}
		}

			return ExecuteTool(TEXT("unreal.spawn_actor"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/spawn_batch")))
		{
			const FString JsonText = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/spawn_batch"));
			if (JsonText.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /spawn_batch {\"classPath\":\"/Script/Engine.PointLight\",\"items\":[{\"x\":0,\"y\":0,\"z\":150}]}"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			if (!UnrealMcp::LoadJsonObject(JsonText, ArgumentsObject))
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Failed to parse JSON arguments for /spawn_batch."), nullptr, true);
			}

			if (!ArgumentsObject->HasField(TEXT("selectSpawned")))
			{
				ArgumentsObject->SetBoolField(TEXT("selectSpawned"), true);
			}

			return ExecuteTool(TEXT("unreal.spawn_actor_batch"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/py_eval")))
		{
			const FString Command = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/py_eval"));
			if (Command.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /py_eval <python-expression>"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("command"), Command);
			ArgumentsObject->SetStringField(TEXT("mode"), TEXT("EvaluateStatement"));
			ArgumentsObject->SetStringField(TEXT("scope"), TEXT("Private"));
			ArgumentsObject->SetBoolField(TEXT("forceEnable"), true);
			ArgumentsObject->SetBoolField(TEXT("unattended"), true);
			return ExecuteTool(TEXT("unreal.execute_python"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/py_file")))
		{
			const FString Remainder = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/py_file"));
			TArray<FString> Tokens;
			Remainder.ParseIntoArrayWS(Tokens);
			if (Tokens.Num() < 1)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /py_file <scriptPath> [arg1 arg2 ...]"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("scriptPath"), Tokens[0]);
			ArgumentsObject->SetStringField(TEXT("scope"), TEXT("Private"));
			ArgumentsObject->SetBoolField(TEXT("forceEnable"), true);
			ArgumentsObject->SetBoolField(TEXT("unattended"), true);
			ArgumentsObject->SetBoolField(TEXT("allowOutsideProject"), false);

			if (Tokens.Num() > 1)
			{
				TArray<TSharedPtr<FJsonValue>> ArgsArray;
				for (int32 Index = 1; Index < Tokens.Num(); ++Index)
				{
					ArgsArray.Add(MakeShared<FJsonValueString>(Tokens[Index]));
				}
				ArgumentsObject->SetArrayField(TEXT("args"), ArgsArray);
			}

			return ExecuteTool(TEXT("unreal.execute_python_file"), *ArgumentsObject);
		}

		if (UnrealMcp::MatchesCommand(TrimmedInput, TEXT("/py")))
		{
			const FString Command = UnrealMcp::GetCommandRemainder(TrimmedInput, TEXT("/py"));
			if (Command.IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Usage: /py <python-code-or-file>"), nullptr, true);
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			ArgumentsObject->SetStringField(TEXT("command"), Command);
			ArgumentsObject->SetStringField(TEXT("mode"), TEXT("ExecuteFile"));
			ArgumentsObject->SetStringField(TEXT("scope"), TEXT("Private"));
			ArgumentsObject->SetBoolField(TEXT("forceEnable"), true);
			ArgumentsObject->SetBoolField(TEXT("unattended"), true);
			return ExecuteTool(TEXT("unreal.execute_python"), *ArgumentsObject);
		}

		return UnrealMcp::MakeExecutionResult(TEXT("Unknown command. Try /help."), nullptr, true);
	}

TSharedRef<IUnrealMcpAssistantHandle, ESPMode::ThreadSafe> FUnrealMcpModule::ExecuteAssistantTurnAsync(
	const FString& UserPrompt,
	const FString& ConversationContext,
	const FString& PreviousResponseId,
	TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent,
	TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete) const
{
	return UnrealMcp::CreateAssistantRun(
		this,
		UserPrompt,
		ConversationContext,
		PreviousResponseId,
		MoveTemp(OnEvent),
		MoveTemp(OnComplete));
}

IMPLEMENT_MODULE(FUnrealMcpModule, UnrealMcp)

#undef LOCTEXT_NAMESPACE
