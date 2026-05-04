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


		FString HashTextForManifest(const FString& Text)
		{
			return FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*Text));
		}

		FString GetMcpModuleSourcePath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Plugins/UnrealMcp/Source/UnrealMcp/Private/UnrealMcpModule.cpp")));
		}

		FString GetMcpExtensionBackupRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ExtensionBackups")));
		}

		FString GetUnrealMcpSavedRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp")));
		}

		FString GetMcpBuildLogRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/BuildLogs")));
		}

		FString GetLatestMcpExtensionManifestPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/LastExtensionApply.json")));
		}

		FString GetMcpExtensionLockPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ExtensionSession.lock")));
		}

		FString GetMcpProjectStateBackupRoot()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/ProjectStateBackups")));
		}

		FString GetMcpModuleHeaderPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Plugins/UnrealMcp/Source/UnrealMcp/Public/UnrealMcpModule.h")));
		}

		FString GetProjectReadmePath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("README.md")));
		}

		FString GetPluginReadmePath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins/UnrealMcp/README.md")));
		}

		bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutFailureReason)
		{
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to read JSON file '%s'."), *FilePath);
				return false;
			}

			if (!LoadJsonObject(JsonText, OutObject) || !OutObject.IsValid())
			{
				OutFailureReason = FString::Printf(TEXT("JSON file '%s' is not a valid object."), *FilePath);
				return false;
			}

			return true;
		}

		bool SaveJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& FilePath, FString& OutFailureReason)
		{
			if (!Object.IsValid())
			{
				OutFailureReason = TEXT("Cannot save an invalid JSON object.");
				return false;
			}

			const FString Directory = FPaths::GetPath(FilePath);
			if (!IFileManager::Get().MakeDirectory(*Directory, true))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to create directory '%s'."), *Directory);
				return false;
			}

			FString JsonText;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
			if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
			{
				OutFailureReason = TEXT("Failed to serialize JSON object.");
				return false;
			}

			if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to write '%s'."), *FilePath);
				return false;
			}
			return true;
		}

		bool ResolveProjectPathInsideProject(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason)
		{
			FString TrimmedPath = RequestedPath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				OutFailureReason = TEXT("Path must not be empty.");
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

			const FString ProjectDirPrefix = ProjectDir.EndsWith(TEXT("/")) ? ProjectDir : ProjectDir + TEXT("/");
			if (!ResolvedPath.Equals(ProjectDir, ESearchCase::IgnoreCase)
				&& !ResolvedPath.StartsWith(ProjectDirPrefix, ESearchCase::IgnoreCase))
			{
				OutFailureReason = FString::Printf(TEXT("Path '%s' resolves outside project directory '%s'."), *ResolvedPath, *ProjectDir);
				return false;
			}

			OutPath = ResolvedPath;
			return true;
		}

		bool ResolveMcpScaffoldDirectory(const FJsonObject& Arguments, FString& OutDirectory, FString& OutToolName, FString& OutFailureReason)
		{
			FString ScaffoldDir;
			FString ToolName;
			FString OutputRoot;
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
			ToolName = ToolName.TrimStartAndEnd();

			if (!ScaffoldDir.TrimStartAndEnd().IsEmpty())
			{
				if (!ResolveProjectPathInsideProject(ScaffoldDir, OutDirectory, OutFailureReason))
				{
					return false;
				}
			}
			else
			{
				if (ToolName.IsEmpty())
				{
					OutFailureReason = TEXT("Provide either scaffoldDir or toolName.");
					return false;
				}

				FString ResolvedOutputRoot;
				if (!ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, OutFailureReason))
				{
					return false;
				}
				OutDirectory = FPaths::Combine(ResolvedOutputRoot, SanitizeMcpToolIdForPath(ToolName));
			}

			FString TestRequestText;
			const FString TestRequestPath = FPaths::Combine(OutDirectory, TEXT("TestRequest.json"));
			if (!ToolName.IsEmpty())
			{
				OutToolName = ToolName;
				return true;
			}

			if (FFileHelper::LoadFileToString(TestRequestText, *TestRequestPath))
			{
				TSharedPtr<FJsonObject> TestRequestObject;
				if (LoadJsonObject(TestRequestText, TestRequestObject) && TestRequestObject.IsValid())
				{
					const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
					if (TestRequestObject->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject && (*ParamsObject).IsValid())
					{
						(*ParamsObject)->TryGetStringField(TEXT("name"), ToolName);
					}
				}
			}

			if (ToolName.TrimStartAndEnd().IsEmpty())
			{
				OutFailureReason = FString::Printf(TEXT("Unable to determine toolName. Provide toolName or include %s."), *TestRequestPath);
				return false;
			}

			OutToolName = ToolName.TrimStartAndEnd();
			return true;
		}

		bool LoadScaffoldSnippet(
			const FString& ScaffoldDirectory,
			const FString& FileName,
			bool bRequired,
			FString& OutSnippet,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			FString& OutFailureReason)
		{
			const FString SnippetPath = FPaths::Combine(ScaffoldDirectory, FileName);
			if (!FPaths::FileExists(SnippetPath))
			{
				AddAuditIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"), SnippetPath, TEXT("Snippet file is missing."));
				if (bRequired)
				{
					OutFailureReason = FString::Printf(TEXT("Required snippet file is missing: %s"), *SnippetPath);
					return false;
				}
				return true;
			}

			if (!FFileHelper::LoadFileToString(OutSnippet, *SnippetPath))
			{
				AddAuditIssue(Issues, TEXT("error"), SnippetPath, TEXT("Failed to read snippet file."));
				OutFailureReason = FString::Printf(TEXT("Failed to read snippet file: %s"), *SnippetPath);
				return false;
			}

			if (OutSnippet.TrimStartAndEnd().IsEmpty())
			{
				AddAuditIssue(Issues, bRequired ? TEXT("error") : TEXT("warning"), SnippetPath, TEXT("Snippet file is empty."));
				if (bRequired)
				{
					OutFailureReason = FString::Printf(TEXT("Required snippet file is empty: %s"), *SnippetPath);
					return false;
				}
			}
			return true;
		}

		TSharedPtr<FJsonObject> MakeTextDiffObject(const FString& BeforeText, const FString& AfterText, int32 MaxPreviewLines);

		bool CanonicalizeScaffoldSnippetName(const FString& SnippetName, FString& OutSnippetName, FString& OutFailureReason)
		{
			const FString CleanName = SnippetName.TrimStartAndEnd();
			if (CleanName.Equals(TEXT("ToolDefinition.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("tool_definition"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("definition"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("ToolDefinition.cpp.snippet");
				return true;
			}

			if (CleanName.Equals(TEXT("ExecuteToolHandler.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("handler"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("execute"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("ExecuteToolHandler.cpp.snippet");
				return true;
			}

			if (CleanName.Equals(TEXT("ChatCommand.cpp.snippet"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("chat"), ESearchCase::IgnoreCase)
				|| CleanName.Equals(TEXT("chat_command"), ESearchCase::IgnoreCase))
			{
				OutSnippetName = TEXT("ChatCommand.cpp.snippet");
				return true;
			}

			OutFailureReason = TEXT("snippetName must be one of ToolDefinition.cpp.snippet, ExecuteToolHandler.cpp.snippet, or ChatCommand.cpp.snippet.");
			return false;
		}

		void AddSnippetIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& Code,
			const FString& Message)
		{
			TSharedPtr<FJsonObject> IssueObject = MakeShared<FJsonObject>();
			IssueObject->SetStringField(TEXT("severity"), Severity);
			IssueObject->SetStringField(TEXT("code"), Code);
			IssueObject->SetStringField(TEXT("message"), Message);
			Issues.Add(MakeShared<FJsonValueObject>(IssueObject));
		}

		bool ContainsAnyPattern(const FString& Text, const TArray<FString>& Patterns, FString& OutPattern)
		{
			for (const FString& Pattern : Patterns)
			{
				if (Text.Contains(Pattern, ESearchCase::IgnoreCase))
				{
					OutPattern = Pattern;
					return true;
				}
			}
			return false;
		}

		TSharedPtr<FJsonObject> ValidateCppSnippetText(
			const FString& SnippetText,
			const FString& SnippetName,
			const FString& ToolName)
		{
			TArray<TSharedPtr<FJsonValue>> Issues;
			int32 ErrorCount = 0;
			int32 WarningCount = 0;
			auto AddIssue = [&](const FString& Severity, const FString& Code, const FString& Message)
			{
				AddSnippetIssue(Issues, Severity, Code, Message);
				if (Severity == TEXT("error"))
				{
					++ErrorCount;
				}
				else
				{
					++WarningCount;
				}
			};

			const FString CleanSnippetName = SnippetName.TrimStartAndEnd();
			const FString CleanToolName = ToolName.TrimStartAndEnd();
			const FString TrimmedSnippet = SnippetText.TrimStartAndEnd();
			if (TrimmedSnippet.IsEmpty())
			{
				AddIssue(TEXT("error"), TEXT("empty_snippet"), TEXT("Snippet text is empty."));
			}
			if (SnippetText.Len() > 50000)
			{
				AddIssue(TEXT("warning"), TEXT("large_snippet"), TEXT("Snippet is larger than 50k characters; review before applying."));
			}

			FString MatchedPattern;
			if (ContainsAnyPattern(SnippetText, {
				TEXT("FPlatformProcess::ExecProcess"),
				TEXT("FPlatformProcess::CreateProc"),
				TEXT(" system("),
				TEXT("\tsystem("),
				TEXT("popen(")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("process_execution"), FString::Printf(TEXT("Snippet contains process execution pattern '%s'."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("IFileManager::Get().Delete"),
				TEXT("DeleteDirectory("),
				TEXT("DeleteDirectoryRecursively"),
				TEXT("FPlatformFileManager::Get().GetPlatformFile().Delete")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("destructive_file_operation"), FString::Printf(TEXT("Snippet contains destructive file operation pattern '%s'."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("FFileHelper::SaveStringToFile"),
				TEXT("FFileHelper::SaveArrayToFile"),
				TEXT("CreateFileWriter("),
				TEXT("std::ofstream")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("file_write_operation"), FString::Printf(TEXT("Snippet contains file write pattern '%s'. Generated tools should route file edits through reviewed MCP utilities."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("/Users/"),
				TEXT("/private/"),
				TEXT("/etc/"),
				TEXT("/tmp/"),
				TEXT("C:\\\\"),
				TEXT("D:\\\\"),
				TEXT("../"),
				TEXT("..\\\\")
			}, MatchedPattern))
			{
				AddIssue(TEXT("warning"), TEXT("external_path_literal"), FString::Printf(TEXT("Snippet contains path-like literal '%s'; verify it cannot write outside the project."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("RunMcpExtensionPipeline("),
				TEXT("TEXT(\"unreal.mcp_extension_pipeline\")"),
				TEXT("ExecuteTool(TEXT(\"unreal.mcp_extension_pipeline\")")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("recursive_pipeline_call"), FString::Printf(TEXT("Snippet contains recursive pipeline call pattern '%s'."), *MatchedPattern));
			}

			if (ContainsAnyPattern(SnippetText, {
				TEXT("while (true"),
				TEXT("while(true"),
				TEXT("for (;;"),
				TEXT("for(;;")
			}, MatchedPattern))
			{
				AddIssue(TEXT("error"), TEXT("obvious_infinite_loop"), FString::Printf(TEXT("Snippet contains obvious infinite loop pattern '%s'."), *MatchedPattern));
			}

			if (SnippetText.Contains(TEXT("ExecuteTool(ToolName"), ESearchCase::IgnoreCase))
			{
				AddIssue(TEXT("warning"), TEXT("self_dispatch_risk"), TEXT("Snippet forwards ExecuteTool(ToolName, ...); verify this cannot recursively dispatch itself."));
			}
			if (SnippetText.Contains(TEXT("TODO"), ESearchCase::IgnoreCase))
			{
				AddIssue(TEXT("warning"), TEXT("todo_marker"), TEXT("Snippet still contains TODO markers."));
			}
			if (SnippetText.Contains(TEXT("MakeFlexibleObjectProperty"), ESearchCase::IgnoreCase)
				|| SnippetText.Contains(TEXT("additionalProperties"), ESearchCase::IgnoreCase))
			{
				AddIssue(TEXT("warning"), TEXT("schema_flexibility"), TEXT("Snippet may introduce flexible object schema fields; validate OpenAI compatibility before applying."));
			}

			if (CleanSnippetName == TEXT("ExecuteToolHandler.cpp.snippet"))
			{
				if (!SnippetText.Contains(TEXT("return UnrealMcp::MakeExecutionResult"), ESearchCase::CaseSensitive)
					&& !SnippetText.Contains(TEXT("return MakeExecutionResult"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_make_execution_result"), TEXT("ExecuteTool handler snippet must return UnrealMcp::MakeExecutionResult or MakeExecutionResult."));
				}
				if (!CleanToolName.IsEmpty() && !SnippetText.Contains(FString::Printf(TEXT("TEXT(\"%s\")"), *CleanToolName), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_tool_name_literal"), TEXT("ExecuteTool handler snippet does not contain the expected tool name literal."));
				}
			}
			else if (CleanSnippetName == TEXT("ToolDefinition.cpp.snippet"))
			{
				if (!SnippetText.Contains(TEXT("AddToolDefinition"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_add_tool_definition"), TEXT("Tool definition snippet must call UnrealMcp::AddToolDefinition."));
				}
				if (!SnippetText.Contains(TEXT("MakeObjectSchema"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("error"), TEXT("missing_object_schema"), TEXT("Tool definition snippet should build a fixed object schema with MakeObjectSchema."));
				}
				if (!CleanToolName.IsEmpty() && !SnippetText.Contains(FString::Printf(TEXT("TEXT(\"%s\")"), *CleanToolName), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_tool_name_literal"), TEXT("Tool definition snippet does not contain the expected tool name literal."));
				}
			}
			else if (CleanSnippetName == TEXT("ChatCommand.cpp.snippet"))
			{
				if (!SnippetText.Contains(TEXT("ExecuteTool"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_execute_tool"), TEXT("Chat command snippet does not call ExecuteTool."));
				}
				if (!SnippetText.Contains(TEXT("return"), ESearchCase::CaseSensitive))
				{
					AddIssue(TEXT("warning"), TEXT("missing_return"), TEXT("Chat command snippet does not return a result."));
				}
			}

			TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
			ResultObject->SetStringField(TEXT("snippetName"), CleanSnippetName);
			ResultObject->SetStringField(TEXT("toolName"), CleanToolName);
			ResultObject->SetBoolField(TEXT("safe"), ErrorCount == 0);
			ResultObject->SetNumberField(TEXT("errorCount"), ErrorCount);
			ResultObject->SetNumberField(TEXT("warningCount"), WarningCount);
			ResultObject->SetNumberField(TEXT("characterCount"), SnippetText.Len());
			ResultObject->SetArrayField(TEXT("issues"), Issues);
			return ResultObject;
		}

		enum class EMcpScaffoldInsertionStatus
		{
			WillInsert,
			Inserted,
			SkippedAlreadyIntegrated,
			SkippedOptionalMissing,
			Conflict,
			MissingAnchor
		};

		const TCHAR* LexToString(EMcpScaffoldInsertionStatus Status)
		{
			switch (Status)
			{
			case EMcpScaffoldInsertionStatus::WillInsert:
				return TEXT("will_insert");
			case EMcpScaffoldInsertionStatus::Inserted:
				return TEXT("inserted");
			case EMcpScaffoldInsertionStatus::SkippedAlreadyIntegrated:
				return TEXT("skipped_already_integrated");
			case EMcpScaffoldInsertionStatus::SkippedOptionalMissing:
				return TEXT("skipped_optional_missing");
			case EMcpScaffoldInsertionStatus::Conflict:
				return TEXT("conflict");
			case EMcpScaffoldInsertionStatus::MissingAnchor:
				return TEXT("missing_anchor");
			default:
				return TEXT("unknown");
			}
		}

		static constexpr int32 GUnrealMcpExtensionManifestSchemaVersion = 1;

		const TCHAR* GetUnrealMcpExtensionManifestSchemaName()
		{
			return TEXT("UnrealMcpExtensionManifest.v1");
		}

		TSharedPtr<FJsonObject> MakeInsertionChangeObject(
			const FString& Section,
			EMcpScaffoldInsertionStatus Status,
			const FString& Message,
			int32 Offset,
			const FString& Preview)
		{
			TSharedPtr<FJsonObject> ChangeObject = MakeShared<FJsonObject>();
			ChangeObject->SetStringField(TEXT("section"), Section);
			ChangeObject->SetStringField(TEXT("status"), LexToString(Status));
			ChangeObject->SetStringField(TEXT("message"), Message);
			ChangeObject->SetNumberField(TEXT("offset"), Offset);
			ChangeObject->SetStringField(TEXT("preview"), Preview);
			return ChangeObject;
		}

		int32 CountScaffoldChangesByStatus(const TArray<TSharedPtr<FJsonValue>>& Changes, const FString& Status)
		{
			int32 Count = 0;
			for (const TSharedPtr<FJsonValue>& ChangeValue : Changes)
			{
				TSharedPtr<FJsonObject> ChangeObject;
				if (ChangeValue.IsValid())
				{
					ChangeObject = ChangeValue->AsObject();
				}
				if (!ChangeObject.IsValid())
				{
					continue;
				}

				FString ChangeStatus;
				if (ChangeObject->TryGetStringField(TEXT("status"), ChangeStatus) && ChangeStatus == Status)
				{
					++Count;
				}
			}
			return Count;
		}

		TSharedPtr<FJsonObject> MakeScaffoldConflictPolicyObject()
		{
			TSharedPtr<FJsonObject> PolicyObject = MakeShared<FJsonObject>();
			PolicyObject->SetBoolField(TEXT("exactSnippetIsIdempotent"), true);
			PolicyObject->SetBoolField(TEXT("conflictNeedleBlocksApply"), true);
			PolicyObject->SetBoolField(TEXT("missingAnchorBlocksApply"), true);
			PolicyObject->SetBoolField(TEXT("unsafeSnippetBlocksApplyByDefault"), true);
			PolicyObject->SetStringField(TEXT("conflictDetector"), TEXT("PlanOrApplyScaffoldInsertion"));
			return PolicyObject;
		}

		FString GetActiveExtensionSessionIdForManifest()
		{
			TSharedPtr<FJsonObject> LockObject;
			FString FailureReason;
			if (!LoadJsonObjectFromFile(GetMcpExtensionLockPath(), LockObject, FailureReason) || !LockObject.IsValid())
			{
				return FString();
			}

			FString SessionId;
			LockObject->TryGetStringField(TEXT("sessionId"), SessionId);
			return SessionId;
		}

		bool PlanOrApplyScaffoldInsertion(
			FString& SourceText,
			const FString& ConflictSourceText,
			const FString& Section,
			const FString& ToolName,
			const FString& Snippet,
			const FString& Anchor,
			const FString& ConflictNeedle,
			bool bDryRun,
			TArray<TSharedPtr<FJsonValue>>& Changes,
			bool& bOutChanged)
		{
			const FString TrimmedSnippet = Snippet.TrimStartAndEnd();
			if (TrimmedSnippet.IsEmpty())
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					Section,
					EMcpScaffoldInsertionStatus::Conflict,
					TEXT("Snippet is empty."),
					INDEX_NONE,
					FString())));
				return false;
			}

			if (SourceText.Contains(TrimmedSnippet, ESearchCase::CaseSensitive))
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					Section,
					EMcpScaffoldInsertionStatus::SkippedAlreadyIntegrated,
					TEXT("Exact snippet is already present."),
					INDEX_NONE,
					FString())));
				return true;
			}

			if (!ConflictNeedle.IsEmpty() && ConflictSourceText.Contains(ConflictNeedle, ESearchCase::CaseSensitive))
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					Section,
					EMcpScaffoldInsertionStatus::Conflict,
					FString::Printf(TEXT("Source already contains conflict marker '%s' but not the exact snippet."), *ConflictNeedle),
					INDEX_NONE,
					TrimmedSnippet.Left(800))));
				return false;
			}

			const int32 AnchorOffset = SourceText.Find(Anchor, ESearchCase::CaseSensitive);
			int32 ResolvedAnchorOffset = AnchorOffset;
			if (ResolvedAnchorOffset == INDEX_NONE && Section == TEXT("AppendToolDefinitions"))
			{
				const int32 CompileBlueprintMarkerOffset = SourceText.Find(TEXT("TEXT(\"unreal.compile_blueprint\")"), ESearchCase::CaseSensitive);
				if (CompileBlueprintMarkerOffset != INDEX_NONE)
				{
					const FString Prefix = SourceText.Left(CompileBlueprintMarkerOffset);
					ResolvedAnchorOffset = Prefix.Find(TEXT("\n\t\t\t{"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					if (ResolvedAnchorOffset == INDEX_NONE)
					{
						ResolvedAnchorOffset = Prefix.Find(TEXT("\n\t\t{"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					}
				}
			}
			if (ResolvedAnchorOffset == INDEX_NONE)
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					Section,
					EMcpScaffoldInsertionStatus::MissingAnchor,
					TEXT("Insertion anchor was not found in UnrealMcpModule.cpp."),
					INDEX_NONE,
					TrimmedSnippet.Left(800))));
				return false;
			}

			const FString InsertionText = FString::Printf(TEXT("\n%s\n"), *TrimmedSnippet);
			Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
				Section,
				bDryRun ? EMcpScaffoldInsertionStatus::WillInsert : EMcpScaffoldInsertionStatus::Inserted,
				bDryRun ? TEXT("Would insert snippet before anchor.") : TEXT("Inserted snippet before anchor."),
				ResolvedAnchorOffset,
				TrimmedSnippet.Left(800))));

			if (!bDryRun)
			{
				SourceText.InsertAt(ResolvedAnchorOffset, InsertionText);
				bOutChanged = true;
			}
			return true;
		}

		FUnrealMcpExecutionResult ApplyMcpScaffold(const FJsonObject& Arguments)
		{
			FString ScaffoldDirectory;
			FString ToolName;
			FString FailureReason;
			if (!ResolveMcpScaffoldDirectory(Arguments, ScaffoldDirectory, ToolName, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			bool bDryRun = true;
			bool bApplyChatCommand = true;
			bool bCreateBackup = true;
			bool bValidateSnippets = true;
			bool bAllowUnsafeSnippets = false;
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("applyChatCommand"), bApplyChatCommand);
			Arguments.TryGetBoolField(TEXT("createBackup"), bCreateBackup);
			Arguments.TryGetBoolField(TEXT("validateSnippets"), bValidateSnippets);
			Arguments.TryGetBoolField(TEXT("allowUnsafeSnippets"), bAllowUnsafeSnippets);
			const int32 TargetDiffPreviewLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("targetDiffPreviewLines"), 120), 1000);

			TArray<TSharedPtr<FJsonValue>> Issues;
			FString DefinitionSnippet;
			FString HandlerSnippet;
			FString ChatCommandSnippet;
			if (!LoadScaffoldSnippet(ScaffoldDirectory, TEXT("ToolDefinition.cpp.snippet"), true, DefinitionSnippet, Issues, FailureReason)
				|| !LoadScaffoldSnippet(ScaffoldDirectory, TEXT("ExecuteToolHandler.cpp.snippet"), true, HandlerSnippet, Issues, FailureReason)
				|| !LoadScaffoldSnippet(ScaffoldDirectory, TEXT("ChatCommand.cpp.snippet"), bApplyChatCommand, ChatCommandSnippet, Issues, FailureReason))
			{
				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
				StructuredContent->SetStringField(TEXT("toolName"), ToolName);
				StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
				StructuredContent->SetArrayField(TEXT("issues"), Issues);
				return MakeExecutionResult(FailureReason, StructuredContent, true);
			}

			TArray<TSharedPtr<FJsonValue>> SnippetValidations;
			bool bSnippetsSafe = true;
			if (bValidateSnippets)
			{
				TSharedPtr<FJsonObject> DefinitionValidation = ValidateCppSnippetText(DefinitionSnippet, TEXT("ToolDefinition.cpp.snippet"), ToolName);
				TSharedPtr<FJsonObject> HandlerValidation = ValidateCppSnippetText(HandlerSnippet, TEXT("ExecuteToolHandler.cpp.snippet"), ToolName);
				bSnippetsSafe &= DefinitionValidation->GetBoolField(TEXT("safe"));
				bSnippetsSafe &= HandlerValidation->GetBoolField(TEXT("safe"));
				SnippetValidations.Add(MakeShared<FJsonValueObject>(DefinitionValidation));
				SnippetValidations.Add(MakeShared<FJsonValueObject>(HandlerValidation));
				if (bApplyChatCommand)
				{
					TSharedPtr<FJsonObject> ChatValidation = ValidateCppSnippetText(ChatCommandSnippet, TEXT("ChatCommand.cpp.snippet"), ToolName);
					bSnippetsSafe &= ChatValidation->GetBoolField(TEXT("safe"));
					SnippetValidations.Add(MakeShared<FJsonValueObject>(ChatValidation));
				}
			}

			const FString SourcePath = GetMcpModuleSourcePath();
			FString SourceText;
			if (!FFileHelper::LoadFileToString(SourceText, *SourcePath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read source file '%s'."), *SourcePath), nullptr, true);
			}

			const FString SourceHashBefore = HashTextForManifest(SourceText);
			const FString OriginalSourceText = SourceText;
			TArray<TSharedPtr<FJsonValue>> Changes;
			bool bChanged = false;
			bool bCanApply = true;

			const FString DefinitionAnchor =
				TEXT("\n\t\t\t{\n\t\t\t\tTSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();\n\t\t\t\tPropertiesObject->SetObjectField(TEXT(\"path\"), UnrealMcp::MakeStringProperty(TEXT(\"Blueprint asset path to compile.\")));");
			const FString HandlerAnchor =
				TEXT("\n\treturn UnrealMcp::MakeExecutionResult(FString::Printf(TEXT(\"Unknown tool '%s'.\"), *ToolName), nullptr, true);");
			const FString ChatCommandAnchor =
				TEXT("\n\t\treturn UnrealMcp::MakeExecutionResult(TEXT(\"Unknown command. Try /help.\"), nullptr, true);");
			const FString ToolNameNeedle = FString::Printf(TEXT("TEXT(\"%s\")"), *ToolName);
			const FString ChatCommandNeedle = FString::Printf(TEXT("TEXT(\"/%s\")"), *SanitizeMcpToolIdForPath(ToolName));

			bCanApply &= PlanOrApplyScaffoldInsertion(
				SourceText,
				OriginalSourceText,
				TEXT("AppendToolDefinitions"),
				ToolName,
				DefinitionSnippet,
				DefinitionAnchor,
				ToolNameNeedle,
				bDryRun,
				Changes,
				bChanged);

			bCanApply &= PlanOrApplyScaffoldInsertion(
				SourceText,
				OriginalSourceText,
				TEXT("ExecuteTool"),
				ToolName,
				HandlerSnippet,
				HandlerAnchor,
				ToolNameNeedle,
				bDryRun,
				Changes,
				bChanged);

			if (bApplyChatCommand)
			{
				bCanApply &= PlanOrApplyScaffoldInsertion(
					SourceText,
					OriginalSourceText,
					TEXT("ExecuteChatCommand"),
					ToolName,
					ChatCommandSnippet,
					ChatCommandAnchor,
					ChatCommandNeedle,
					bDryRun,
					Changes,
					bChanged);
			}
			else
			{
				Changes.Add(MakeShared<FJsonValueObject>(MakeInsertionChangeObject(
					TEXT("ExecuteChatCommand"),
					EMcpScaffoldInsertionStatus::SkippedOptionalMissing,
					TEXT("applyChatCommand=false; skipped optional chat command snippet."),
					INDEX_NONE,
					FString())));
			}

			const bool bInsertionCanApply = bCanApply;
			TSharedPtr<FJsonObject> TargetSourceDiff = MakeShared<FJsonObject>();
			if (bInsertionCanApply)
			{
				FString PlannedSourceText = OriginalSourceText;
				TArray<TSharedPtr<FJsonValue>> PlannedChanges;
				bool bPlannedChanged = false;
				PlanOrApplyScaffoldInsertion(
					PlannedSourceText,
					OriginalSourceText,
					TEXT("AppendToolDefinitions"),
					ToolName,
					DefinitionSnippet,
					DefinitionAnchor,
					ToolNameNeedle,
					false,
					PlannedChanges,
					bPlannedChanged);
				PlanOrApplyScaffoldInsertion(
					PlannedSourceText,
					OriginalSourceText,
					TEXT("ExecuteTool"),
					ToolName,
					HandlerSnippet,
					HandlerAnchor,
					ToolNameNeedle,
					false,
					PlannedChanges,
					bPlannedChanged);
				if (bApplyChatCommand)
				{
					PlanOrApplyScaffoldInsertion(
						PlannedSourceText,
						OriginalSourceText,
						TEXT("ExecuteChatCommand"),
						ToolName,
						ChatCommandSnippet,
						ChatCommandAnchor,
						ChatCommandNeedle,
						false,
						PlannedChanges,
						bPlannedChanged);
				}
				TargetSourceDiff = MakeTextDiffObject(OriginalSourceText, PlannedSourceText, TargetDiffPreviewLines);
			}

			if (bValidateSnippets && !bSnippetsSafe && !bAllowUnsafeSnippets)
			{
				bCanApply = false;
			}

			const int32 ConflictCount = CountScaffoldChangesByStatus(Changes, TEXT("conflict"));
			const int32 MissingAnchorCount = CountScaffoldChangesByStatus(Changes, TEXT("missing_anchor"));
			const FString ExtensionSessionId = GetActiveExtensionSessionIdForManifest();
			const TSharedPtr<FJsonObject> ConflictPolicy = MakeScaffoldConflictPolicyObject();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
			StructuredContent->SetNumberField(TEXT("manifestSchemaVersion"), GUnrealMcpExtensionManifestSchemaVersion);
			StructuredContent->SetStringField(TEXT("manifestSchema"), GetUnrealMcpExtensionManifestSchemaName());
			StructuredContent->SetStringField(TEXT("sessionId"), ExtensionSessionId);
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
			StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
			StructuredContent->SetStringField(TEXT("sourcePath"), SourcePath);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("canApply"), bCanApply);
			StructuredContent->SetBoolField(TEXT("changed"), bChanged);
			StructuredContent->SetBoolField(TEXT("validateSnippets"), bValidateSnippets);
			StructuredContent->SetBoolField(TEXT("snippetsSafe"), bSnippetsSafe);
			StructuredContent->SetBoolField(TEXT("allowUnsafeSnippets"), bAllowUnsafeSnippets);
			StructuredContent->SetStringField(TEXT("sourceHashBefore"), SourceHashBefore);
			StructuredContent->SetNumberField(TEXT("conflictCount"), ConflictCount);
			StructuredContent->SetNumberField(TEXT("missingAnchorCount"), MissingAnchorCount);
			StructuredContent->SetObjectField(TEXT("conflictPolicy"), ConflictPolicy);
			StructuredContent->SetArrayField(TEXT("issues"), Issues);
			StructuredContent->SetArrayField(TEXT("snippetValidations"), SnippetValidations);
			StructuredContent->SetArrayField(TEXT("changes"), Changes);
			StructuredContent->SetObjectField(TEXT("targetSourceDiff"), TargetSourceDiff);

			if (!bCanApply)
			{
				return MakeExecutionResult(TEXT("Scaffold cannot be applied safely. See changes, issues, snippetValidations, and targetSourceDiff."), StructuredContent, true);
			}

			if (bDryRun)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Dry run complete for %s. canApply=true"), *ToolName),
					StructuredContent,
					false);
			}

			if (!bChanged)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("No source changes needed for %s; scaffold appears already integrated."), *ToolName),
					StructuredContent,
					false);
			}

			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString BackupDirectory = FPaths::Combine(GetMcpExtensionBackupRoot(), Timestamp + TEXT("_") + SanitizeMcpToolIdForPath(ToolName));
			const FString BackupSourcePath = FPaths::Combine(BackupDirectory, TEXT("UnrealMcpModule.cpp.before"));
			const FString AfterSourcePath = FPaths::Combine(BackupDirectory, TEXT("UnrealMcpModule.cpp.after"));
			if (bCreateBackup)
			{
				if (!IFileManager::Get().MakeDirectory(*BackupDirectory, true))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to create backup directory '%s'."), *BackupDirectory), StructuredContent, true);
				}
				if (!FFileHelper::SaveStringToFile(SourceText, *AfterSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to write after snapshot '%s'."), *AfterSourcePath), StructuredContent, true);
				}
				FString BackupOriginalSourceText;
				if (!FFileHelper::LoadFileToString(BackupOriginalSourceText, *SourcePath)
					|| !FFileHelper::SaveStringToFile(BackupOriginalSourceText, *BackupSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to write source backup '%s'."), *BackupSourcePath), StructuredContent, true);
				}
			}

			if (!FFileHelper::SaveStringToFile(SourceText, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to write source file '%s'."), *SourcePath), StructuredContent, true);
			}

			const FString SourceHashAfter = HashTextForManifest(SourceText);
			StructuredContent->SetStringField(TEXT("sourceHashAfter"), SourceHashAfter);
			StructuredContent->SetStringField(TEXT("backupDirectory"), BackupDirectory);
			StructuredContent->SetStringField(TEXT("backupSourcePath"), BackupSourcePath);
			StructuredContent->SetStringField(TEXT("afterSourcePath"), AfterSourcePath);

				TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
				ManifestObject->SetStringField(TEXT("action"), TEXT("mcp_apply_scaffold"));
				ManifestObject->SetNumberField(TEXT("schemaVersion"), GUnrealMcpExtensionManifestSchemaVersion);
				ManifestObject->SetStringField(TEXT("manifestSchema"), GetUnrealMcpExtensionManifestSchemaName());
				ManifestObject->SetStringField(TEXT("sessionId"), ExtensionSessionId);
				ManifestObject->SetStringField(TEXT("toolName"), ToolName);
				ManifestObject->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
				ManifestObject->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
				ManifestObject->SetStringField(TEXT("sourcePath"), SourcePath);
				ManifestObject->SetStringField(TEXT("backupDirectory"), BackupDirectory);
			ManifestObject->SetStringField(TEXT("backupSourcePath"), BackupSourcePath);
			ManifestObject->SetStringField(TEXT("afterSourcePath"), AfterSourcePath);
				ManifestObject->SetStringField(TEXT("sourceHashBefore"), SourceHashBefore);
				ManifestObject->SetStringField(TEXT("sourceHashAfter"), SourceHashAfter);
				ManifestObject->SetStringField(TEXT("appliedAtUtc"), FDateTime::UtcNow().ToIso8601());
				ManifestObject->SetNumberField(TEXT("conflictCount"), ConflictCount);
				ManifestObject->SetNumberField(TEXT("missingAnchorCount"), MissingAnchorCount);
				ManifestObject->SetObjectField(TEXT("conflictPolicy"), ConflictPolicy);
				ManifestObject->SetArrayField(TEXT("changes"), Changes);

			if (bCreateBackup)
			{
				FString ManifestFailure;
				const FString ManifestPath = FPaths::Combine(BackupDirectory, TEXT("Manifest.json"));
				if (!SaveJsonObjectToFile(ManifestObject, ManifestPath, ManifestFailure)
					|| !SaveJsonObjectToFile(ManifestObject, GetLatestMcpExtensionManifestPath(), ManifestFailure))
				{
					return MakeExecutionResult(ManifestFailure, StructuredContent, true);
				}
				StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
				StructuredContent->SetStringField(TEXT("latestManifestPath"), GetLatestMcpExtensionManifestPath());
			}

			return MakeExecutionResult(
				FString::Printf(TEXT("Applied scaffold for %s. Backup: %s"), *ToolName, *BackupDirectory),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult RollbackLastMcpExtension(const FJsonObject& Arguments)
		{
			FString ManifestPath = GetLatestMcpExtensionManifestPath();
			bool bDryRun = false;
			bool bForce = false;
			Arguments.TryGetStringField(TEXT("manifestPath"), ManifestPath);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("force"), bForce);

			FString ResolvedManifestPath;
			FString FailureReason;
			if (!ResolveProjectPathInsideProject(ManifestPath, ResolvedManifestPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString ManifestText;
			if (!FFileHelper::LoadFileToString(ManifestText, *ResolvedManifestPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read manifest '%s'."), *ResolvedManifestPath), nullptr, true);
			}

			TSharedPtr<FJsonObject> ManifestObject;
			if (!LoadJsonObject(ManifestText, ManifestObject) || !ManifestObject.IsValid())
			{
				return MakeExecutionResult(FString::Printf(TEXT("Manifest '%s' is not valid JSON."), *ResolvedManifestPath), nullptr, true);
			}

			FString ToolName;
			FString SourcePath;
			FString BackupSourcePath;
			FString ExpectedAfterHash;
			ManifestObject->TryGetStringField(TEXT("toolName"), ToolName);
			ManifestObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
			ManifestObject->TryGetStringField(TEXT("backupSourcePath"), BackupSourcePath);
			ManifestObject->TryGetStringField(TEXT("sourceHashAfter"), ExpectedAfterHash);

			if (SourcePath.IsEmpty() || BackupSourcePath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("Manifest is missing sourcePath or backupSourcePath."), nullptr, true);
			}

			FString ResolvedSourcePath;
			FString ResolvedBackupPath;
			if (!ResolveProjectPathInsideProject(SourcePath, ResolvedSourcePath, FailureReason)
				|| !ResolveProjectPathInsideProject(BackupSourcePath, ResolvedBackupPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString CurrentSourceText;
			FString BackupSourceText;
			if (!FFileHelper::LoadFileToString(CurrentSourceText, *ResolvedSourcePath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read current source '%s'."), *ResolvedSourcePath), nullptr, true);
			}
			if (!FFileHelper::LoadFileToString(BackupSourceText, *ResolvedBackupPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read backup source '%s'."), *ResolvedBackupPath), nullptr, true);
			}

			const FString CurrentHash = HashTextForManifest(CurrentSourceText);
			const FString BackupHash = HashTextForManifest(BackupSourceText);
			const bool bHashMatches = ExpectedAfterHash.IsEmpty() || CurrentHash == ExpectedAfterHash;

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_rollback_last_extension"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("manifestPath"), ResolvedManifestPath);
			StructuredContent->SetStringField(TEXT("sourcePath"), ResolvedSourcePath);
			StructuredContent->SetStringField(TEXT("backupSourcePath"), ResolvedBackupPath);
			StructuredContent->SetStringField(TEXT("currentHash"), CurrentHash);
			StructuredContent->SetStringField(TEXT("expectedAfterHash"), ExpectedAfterHash);
			StructuredContent->SetStringField(TEXT("backupHash"), BackupHash);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("force"), bForce);
			StructuredContent->SetBoolField(TEXT("hashMatchesExpectedAfter"), bHashMatches);

			if (!bHashMatches && !bForce)
			{
				return MakeExecutionResult(
					TEXT("Rollback refused because current source hash differs from the applied manifest. Pass force=true to override."),
					StructuredContent,
					true);
			}

			if (bDryRun)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("Dry run rollback for %s would restore %s."), ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName, *ResolvedSourcePath),
					StructuredContent,
					false);
			}

			if (!FFileHelper::SaveStringToFile(BackupSourceText, *ResolvedSourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to restore source file '%s'."), *ResolvedSourcePath), StructuredContent, true);
			}

			ManifestObject->SetStringField(TEXT("rolledBackAtUtc"), FDateTime::UtcNow().ToIso8601());
			ManifestObject->SetBoolField(TEXT("rolledBack"), true);
			FString ManifestFailure;
			SaveJsonObjectToFile(ManifestObject, ResolvedManifestPath, ManifestFailure);
			SaveJsonObjectToFile(ManifestObject, GetLatestMcpExtensionManifestPath(), ManifestFailure);

			StructuredContent->SetBoolField(TEXT("rolledBack"), true);
			StructuredContent->SetStringField(TEXT("restoredHash"), BackupHash);
			return MakeExecutionResult(
				FString::Printf(TEXT("Rolled back MCP extension for %s."), ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName),
				StructuredContent,
				false);
		}

		FString NormalizeFullPathForCompare(const FString& Path)
		{
			FString NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(NormalizedPath);
			FPaths::CollapseRelativeDirectories(NormalizedPath);
			return NormalizedPath;
		}

		bool IsPathInsideDirectory(const FString& Path, const FString& Directory)
		{
			const FString NormalizedPath = NormalizeFullPathForCompare(Path);
			FString NormalizedDirectory = NormalizeFullPathForCompare(Directory);
			NormalizedDirectory.RemoveFromEnd(TEXT("/"));
			const FString DirectoryPrefix = NormalizedDirectory + TEXT("/");
			return NormalizedPath.Equals(NormalizedDirectory, ESearchCase::IgnoreCase)
				|| NormalizedPath.StartsWith(DirectoryPrefix, ESearchCase::IgnoreCase);
		}

		FString FileTimeToIsoString(const FDateTime& Time)
		{
			return Time.GetTicks() > 0 ? Time.ToIso8601() : FString();
		}

		TSharedPtr<FJsonObject> MakeFileInfoObject(const FString& Path)
		{
			TSharedPtr<FJsonObject> InfoObject = MakeShared<FJsonObject>();
			InfoObject->SetStringField(TEXT("path"), Path);
			InfoObject->SetBoolField(TEXT("exists"), FPaths::FileExists(Path) || FPaths::DirectoryExists(Path));

			const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
			if (Stat.bIsValid)
			{
				InfoObject->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Stat.FileSize));
				InfoObject->SetStringField(TEXT("modifiedAt"), FileTimeToIsoString(Stat.ModificationTime));
			}
			return InfoObject;
		}

		void FindImmediateChildren(const FString& Directory, const FString& Pattern, bool bFiles, bool bDirectories, TArray<FString>& OutChildren)
		{
			TArray<FString> Names;
			IFileManager::Get().FindFiles(Names, *FPaths::Combine(Directory, Pattern), bFiles, bDirectories);
			for (const FString& Name : Names)
			{
				OutChildren.Add(FPaths::Combine(Directory, Name));
			}
			OutChildren.Sort();
		}

		bool FindNewestFile(const FString& Directory, const FString& Pattern, FString& OutPath)
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *Directory, *Pattern, true, false);
			if (Files.Num() == 0)
			{
				return false;
			}

			Files.Sort([](const FString& A, const FString& B)
			{
				const FFileStatData StatA = IFileManager::Get().GetStatData(*A);
				const FFileStatData StatB = IFileManager::Get().GetStatData(*B);
				return StatA.ModificationTime > StatB.ModificationTime;
			});

			OutPath = Files[0];
			return true;
		}

		FString MakePathRelativeToProject(const FString& Path)
		{
			FString RelativePath = FPaths::ConvertRelativePathToFull(Path);
			FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeFilename(RelativePath);
			FPaths::NormalizeDirectoryName(ProjectDir);
			FPaths::MakePathRelativeTo(RelativePath, *ProjectDir);
			return RelativePath;
		}

		bool ParseIsoUtc(const FString& IsoText, FDateTime& OutDateTime)
		{
			if (IsoText.TrimStartAndEnd().IsEmpty())
			{
				return false;
			}
			return FDateTime::ParseIso8601(*IsoText, OutDateTime);
		}

		bool IsExtensionLockStale(const TSharedPtr<FJsonObject>& LockObject)
		{
			if (!LockObject.IsValid())
			{
				return true;
			}

			FString ExpiresAtUtc;
			if (!LockObject->TryGetStringField(TEXT("expiresAtUtc"), ExpiresAtUtc))
			{
				return true;
			}

			FDateTime ExpiresAt;
			if (!ParseIsoUtc(ExpiresAtUtc, ExpiresAt))
			{
				return true;
			}

			return FDateTime::UtcNow() >= ExpiresAt;
		}

		TSharedPtr<FJsonObject> MakeExtensionLockObject(
			const FString& SessionId,
			const FString& Owner,
			const FString& Reason,
			int32 TtlSeconds)
		{
			const FDateTime Now = FDateTime::UtcNow();
			const int32 SafeTtlSeconds = FMath::Clamp(TtlSeconds, 30, 86400);

			TSharedPtr<FJsonObject> LockObject = MakeShared<FJsonObject>();
			LockObject->SetStringField(TEXT("sessionId"), SessionId);
			LockObject->SetStringField(TEXT("owner"), Owner.TrimStartAndEnd().IsEmpty() ? TEXT("Unreal MCP Chat") : Owner.TrimStartAndEnd());
			LockObject->SetStringField(TEXT("reason"), Reason.TrimStartAndEnd());
			LockObject->SetStringField(TEXT("createdAtUtc"), Now.ToIso8601());
			LockObject->SetStringField(TEXT("refreshedAtUtc"), Now.ToIso8601());
			LockObject->SetStringField(TEXT("expiresAtUtc"), (Now + FTimespan::FromSeconds(SafeTtlSeconds)).ToIso8601());
			LockObject->SetNumberField(TEXT("ttlSeconds"), SafeTtlSeconds);
			return LockObject;
		}

		bool TryAcquireExtensionSessionLock(
			const FString& Owner,
			const FString& Reason,
			int32 TtlSeconds,
			bool bForce,
			FString& OutSessionId,
			TSharedPtr<FJsonObject>& OutLockObject,
			FString& OutFailureReason)
		{
			OutSessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
			const FString LockPath = GetMcpExtensionLockPath();

			TSharedPtr<FJsonObject> ExistingLock;
			if (FPaths::FileExists(LockPath))
			{
				FString LoadFailure;
				if (LoadJsonObjectFromFile(LockPath, ExistingLock, LoadFailure) && ExistingLock.IsValid() && !IsExtensionLockStale(ExistingLock) && !bForce)
				{
					FString ExistingSessionId;
					FString ExistingOwner;
					FString ExpiresAtUtc;
					ExistingLock->TryGetStringField(TEXT("sessionId"), ExistingSessionId);
					ExistingLock->TryGetStringField(TEXT("owner"), ExistingOwner);
					ExistingLock->TryGetStringField(TEXT("expiresAtUtc"), ExpiresAtUtc);
					OutFailureReason = FString::Printf(
						TEXT("MCP extension session is locked by '%s' (sessionId=%s, expiresAtUtc=%s). Pass force=true only if you are sure that session is dead."),
						ExistingOwner.IsEmpty() ? TEXT("<unknown>") : *ExistingOwner,
						ExistingSessionId.IsEmpty() ? TEXT("<unknown>") : *ExistingSessionId,
						ExpiresAtUtc.IsEmpty() ? TEXT("<unknown>") : *ExpiresAtUtc);
					OutLockObject = ExistingLock;
					return false;
				}
			}

			OutLockObject = MakeExtensionLockObject(OutSessionId, Owner, Reason, TtlSeconds);
			FString SaveFailure;
			if (!SaveJsonObjectToFile(OutLockObject, LockPath, SaveFailure))
			{
				OutFailureReason = SaveFailure;
				return false;
			}
			OutLockObject->SetStringField(TEXT("lockPath"), LockPath);
			return true;
		}

		bool ReleaseExtensionSessionLock(const FString& SessionId, bool bForce, FString& OutFailureReason)
		{
			const FString LockPath = GetMcpExtensionLockPath();
			if (!FPaths::FileExists(LockPath))
			{
				return true;
			}

			TSharedPtr<FJsonObject> ExistingLock;
			if (!LoadJsonObjectFromFile(LockPath, ExistingLock, OutFailureReason))
			{
				if (bForce)
				{
					IFileManager::Get().Delete(*LockPath, false, true, true);
					return true;
				}
				return false;
			}

			FString ExistingSessionId;
			ExistingLock->TryGetStringField(TEXT("sessionId"), ExistingSessionId);
			if (!bForce && !ExistingSessionId.Equals(SessionId, ESearchCase::CaseSensitive))
			{
				OutFailureReason = FString::Printf(
					TEXT("Refusing to release lock owned by a different session. Existing=%s requested=%s."),
					*ExistingSessionId,
					*SessionId);
				return false;
			}

			if (!IFileManager::Get().Delete(*LockPath, false, true, true))
			{
				OutFailureReason = FString::Printf(TEXT("Failed to delete lock file '%s'."), *LockPath);
				return false;
			}
			return true;
		}

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

		FString TailLines(const FString& Text, int32 MaxLines)
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			const int32 StartIndex = FMath::Max(0, Lines.Num() - FMath::Max(1, MaxLines));
			TArray<FString> Tail;
			for (int32 Index = StartIndex; Index < Lines.Num(); ++Index)
			{
				Tail.Add(Lines[Index]);
			}
			return FString::Join(Tail, TEXT("\n"));
		}

		void CollectExtensionManifestPaths(TArray<FString>& OutManifestPaths)
		{
			OutManifestPaths.Reset();
			IFileManager::Get().FindFilesRecursive(
				OutManifestPaths,
				*GetMcpExtensionBackupRoot(),
				TEXT("Manifest.json"),
				true,
				false);

			const FString LatestManifestPath = GetLatestMcpExtensionManifestPath();
			if (FPaths::FileExists(LatestManifestPath))
			{
				OutManifestPaths.AddUnique(LatestManifestPath);
			}

			OutManifestPaths.Sort([](const FString& A, const FString& B)
			{
				const FFileStatData StatA = IFileManager::Get().GetStatData(*A);
				const FFileStatData StatB = IFileManager::Get().GetStatData(*B);
				if (StatA.ModificationTime == StatB.ModificationTime)
				{
					return A > B;
				}
				return StatA.ModificationTime > StatB.ModificationTime;
			});
		}

		bool AddProjectStateBackupFile(
			const FString& SourcePath,
			const FString& BackupDirectory,
			bool bDryRun,
			TArray<TSharedPtr<FJsonValue>>& Files,
			FString& OutFailureReason)
		{
			const FString FullSourcePath = FPaths::ConvertRelativePathToFull(SourcePath);
			const bool bExists = FPaths::FileExists(FullSourcePath);
			const FString RelativePath = MakePathRelativeToProject(FullSourcePath);
			const FString BackupPath = FPaths::Combine(BackupDirectory, RelativePath);

			TSharedPtr<FJsonObject> FileObject = MakeShared<FJsonObject>();
			FileObject->SetStringField(TEXT("sourcePath"), FullSourcePath);
			FileObject->SetStringField(TEXT("relativePath"), RelativePath);
			FileObject->SetStringField(TEXT("backupPath"), BackupPath);
			FileObject->SetBoolField(TEXT("exists"), bExists);

			if (bExists)
			{
				FString SourceText;
				if (!FFileHelper::LoadFileToString(SourceText, *FullSourcePath))
				{
					OutFailureReason = FString::Printf(TEXT("Failed to read source file '%s'."), *FullSourcePath);
					return false;
				}

				FileObject->SetStringField(TEXT("hash"), HashTextForManifest(SourceText));
				FileObject->SetNumberField(TEXT("sizeBytes"), SourceText.Len());
				if (!bDryRun)
				{
					const FString BackupPathDirectory = FPaths::GetPath(BackupPath);
					if (!IFileManager::Get().MakeDirectory(*BackupPathDirectory, true))
					{
						OutFailureReason = FString::Printf(TEXT("Failed to create backup directory '%s'."), *BackupPathDirectory);
						return false;
					}
					if (!FFileHelper::SaveStringToFile(SourceText, *BackupPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						OutFailureReason = FString::Printf(TEXT("Failed to write backup file '%s'."), *BackupPath);
						return false;
					}
				}
			}

			Files.Add(MakeShared<FJsonValueObject>(FileObject));
			return true;
		}

		FUnrealMcpExecutionResult BackupProjectState(const FJsonObject& Arguments)
		{
			FString Label = TEXT("manual");
			FString Reason;
			bool bIncludeSource = true;
			bool bIncludeReadmes = true;
			bool bIncludeProjectMemory = true;
			bool bIncludeManifests = true;
			bool bIncludeBuildLogs = false;
			bool bDryRun = false;

			Arguments.TryGetStringField(TEXT("label"), Label);
			Arguments.TryGetStringField(TEXT("reason"), Reason);
			Arguments.TryGetBoolField(TEXT("includeSource"), bIncludeSource);
			Arguments.TryGetBoolField(TEXT("includeReadmes"), bIncludeReadmes);
			Arguments.TryGetBoolField(TEXT("includeProjectMemory"), bIncludeProjectMemory);
			Arguments.TryGetBoolField(TEXT("includeManifests"), bIncludeManifests);
			Arguments.TryGetBoolField(TEXT("includeBuildLogs"), bIncludeBuildLogs);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

			const FString SafeLabel = SanitizeMcpToolIdForPath(Label.TrimStartAndEnd().IsEmpty() ? TEXT("manual") : Label.TrimStartAndEnd());
			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString BackupDirectory = FPaths::Combine(GetMcpProjectStateBackupRoot(), FString::Printf(TEXT("%s_%s"), *Timestamp, *SafeLabel));

			TArray<FString> SourcePaths;
			if (bIncludeSource)
			{
				SourcePaths.Add(GetMcpModuleSourcePath());
				SourcePaths.Add(GetMcpModuleHeaderPath());
			}
			if (bIncludeReadmes)
			{
				SourcePaths.Add(GetProjectReadmePath());
				SourcePaths.Add(GetPluginReadmePath());
			}
			if (bIncludeProjectMemory)
			{
				SourcePaths.Add(GetProjectMemoryFilePath());
			}
			if (bIncludeManifests)
			{
				TArray<FString> ManifestPaths;
				CollectExtensionManifestPaths(ManifestPaths);
				for (const FString& ManifestPath : ManifestPaths)
				{
					SourcePaths.AddUnique(ManifestPath);
				}
			}
			if (bIncludeBuildLogs)
			{
				TArray<FString> BuildLogPaths;
				IFileManager::Get().FindFilesRecursive(BuildLogPaths, *GetMcpBuildLogRoot(), TEXT("*.log"), true, false);
				BuildLogPaths.Sort([](const FString& A, const FString& B)
				{
					return IFileManager::Get().GetStatData(*A).ModificationTime > IFileManager::Get().GetStatData(*B).ModificationTime;
				});
				const int32 MaxBuildLogs = FMath::Min(5, BuildLogPaths.Num());
				for (int32 Index = 0; Index < MaxBuildLogs; ++Index)
				{
					SourcePaths.AddUnique(BuildLogPaths[Index]);
				}
			}

			TArray<TSharedPtr<FJsonValue>> Files;
			FString FailureReason;
			for (const FString& SourcePath : SourcePaths)
			{
				if (!AddProjectStateBackupFile(SourcePath, BackupDirectory, bDryRun, Files, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_backup_project_state"));
			StructuredContent->SetStringField(TEXT("label"), Label);
			StructuredContent->SetStringField(TEXT("reason"), Reason);
			StructuredContent->SetStringField(TEXT("backupDirectory"), BackupDirectory);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetArrayField(TEXT("files"), Files);

			if (!bDryRun)
			{
				TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
				ManifestObject->SetStringField(TEXT("action"), TEXT("mcp_backup_project_state"));
				ManifestObject->SetStringField(TEXT("label"), Label);
				ManifestObject->SetStringField(TEXT("reason"), Reason);
				ManifestObject->SetStringField(TEXT("createdAtUtc"), FDateTime::UtcNow().ToIso8601());
				ManifestObject->SetStringField(TEXT("backupDirectory"), BackupDirectory);
				ManifestObject->SetArrayField(TEXT("files"), Files);
				FString ManifestFailure;
				const FString ManifestPath = FPaths::Combine(BackupDirectory, TEXT("Manifest.json"));
				if (!SaveJsonObjectToFile(ManifestObject, ManifestPath, ManifestFailure))
				{
					return MakeExecutionResult(ManifestFailure, StructuredContent, true);
				}
				StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
			}

			return MakeExecutionResult(
				bDryRun
					? FString::Printf(TEXT("Dry run project state backup would capture %d files."), Files.Num())
					: FString::Printf(TEXT("Backed up project state to %s."), *BackupDirectory),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult LockExtensionSession(const FJsonObject& Arguments)
		{
			FString Mode = TEXT("status");
			FString SessionId;
			FString Owner = TEXT("Unreal MCP Chat");
			FString Reason;
			bool bForce = false;
			double TtlSecondsDouble = 900.0;

			Arguments.TryGetStringField(TEXT("mode"), Mode);
			Arguments.TryGetStringField(TEXT("sessionId"), SessionId);
			Arguments.TryGetStringField(TEXT("owner"), Owner);
			Arguments.TryGetStringField(TEXT("reason"), Reason);
			Arguments.TryGetBoolField(TEXT("force"), bForce);
			Arguments.TryGetNumberField(TEXT("ttlSeconds"), TtlSecondsDouble);
			Mode = Mode.TrimStartAndEnd().ToLower();

			const FString LockPath = GetMcpExtensionLockPath();
			TSharedPtr<FJsonObject> ExistingLock;
			FString FailureReason;
			const bool bHasExistingLock = FPaths::FileExists(LockPath) && LoadJsonObjectFromFile(LockPath, ExistingLock, FailureReason);
			const bool bExistingLockStale = !bHasExistingLock || IsExtensionLockStale(ExistingLock);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_lock_extension_session"));
			StructuredContent->SetStringField(TEXT("mode"), Mode);
			StructuredContent->SetStringField(TEXT("lockPath"), LockPath);
			StructuredContent->SetBoolField(TEXT("hasExistingLock"), bHasExistingLock);
			StructuredContent->SetBoolField(TEXT("existingLockStale"), bExistingLockStale);
			if (ExistingLock.IsValid())
			{
				StructuredContent->SetObjectField(TEXT("existingLock"), ExistingLock);
			}

			if (Mode == TEXT("status") || Mode.IsEmpty())
			{
				StructuredContent->SetBoolField(TEXT("locked"), bHasExistingLock && !bExistingLockStale);
				return MakeExecutionResult(
					bHasExistingLock && !bExistingLockStale ? TEXT("MCP extension session is locked.") : TEXT("No active MCP extension session lock."),
					StructuredContent,
					false);
			}

			if (Mode == TEXT("acquire"))
			{
				FString NewSessionId;
				TSharedPtr<FJsonObject> NewLock;
				if (!TryAcquireExtensionSessionLock(Owner, Reason, static_cast<int32>(TtlSecondsDouble), bForce, NewSessionId, NewLock, FailureReason))
				{
					StructuredContent->SetStringField(TEXT("failureReason"), FailureReason);
					return MakeExecutionResult(FailureReason, StructuredContent, true);
				}
				StructuredContent->SetBoolField(TEXT("locked"), true);
				StructuredContent->SetStringField(TEXT("sessionId"), NewSessionId);
				StructuredContent->SetObjectField(TEXT("lock"), NewLock);
				return MakeExecutionResult(FString::Printf(TEXT("Acquired MCP extension session lock %s."), *NewSessionId), StructuredContent, false);
			}

			if (Mode == TEXT("release"))
			{
				if (SessionId.TrimStartAndEnd().IsEmpty() && !bForce)
				{
					return MakeExecutionResult(TEXT("sessionId is required for release unless force=true."), StructuredContent, true);
				}
				if (!ReleaseExtensionSessionLock(SessionId.TrimStartAndEnd(), bForce, FailureReason))
				{
					StructuredContent->SetStringField(TEXT("failureReason"), FailureReason);
					return MakeExecutionResult(FailureReason, StructuredContent, true);
				}
				StructuredContent->SetBoolField(TEXT("locked"), false);
				return MakeExecutionResult(TEXT("Released MCP extension session lock."), StructuredContent, false);
			}

			if (Mode == TEXT("refresh"))
			{
				if (!bHasExistingLock)
				{
					return MakeExecutionResult(TEXT("No lock exists to refresh."), StructuredContent, true);
				}

				FString ExistingSessionId;
				ExistingLock->TryGetStringField(TEXT("sessionId"), ExistingSessionId);
				if (!bForce && !ExistingSessionId.Equals(SessionId.TrimStartAndEnd(), ESearchCase::CaseSensitive))
				{
					return MakeExecutionResult(TEXT("Refusing to refresh lock owned by a different session."), StructuredContent, true);
				}

				TSharedPtr<FJsonObject> RefreshedLock = MakeExtensionLockObject(ExistingSessionId, Owner, Reason, static_cast<int32>(TtlSecondsDouble));
				FString SaveFailure;
				if (!SaveJsonObjectToFile(RefreshedLock, LockPath, SaveFailure))
				{
					return MakeExecutionResult(SaveFailure, StructuredContent, true);
				}
				StructuredContent->SetBoolField(TEXT("locked"), true);
				StructuredContent->SetObjectField(TEXT("lock"), RefreshedLock);
				return MakeExecutionResult(TEXT("Refreshed MCP extension session lock."), StructuredContent, false);
			}

			return MakeExecutionResult(TEXT("mode must be one of status, acquire, release, or refresh."), StructuredContent, true);
		}

		FUnrealMcpExecutionResult RollbackToManifest(const FJsonObject& Arguments)
		{
			FString ManifestPath;
			FString ToolName;
			FString Selector = TEXT("latest");
			bool bDryRun = false;
			bool bForce = false;
			bool bCreatePreRollbackBackup = true;
			double ManifestIndexDouble = -1.0;

			Arguments.TryGetStringField(TEXT("manifestPath"), ManifestPath);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("selector"), Selector);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("force"), bForce);
			Arguments.TryGetBoolField(TEXT("createPreRollbackBackup"), bCreatePreRollbackBackup);
			Arguments.TryGetNumberField(TEXT("manifestIndex"), ManifestIndexDouble);

			TArray<FString> ManifestPaths;
			CollectExtensionManifestPaths(ManifestPaths);

			TArray<FString> CandidatePaths;
			if (!ManifestPath.TrimStartAndEnd().IsEmpty())
			{
				FString ResolvedManifestPath;
				FString FailureReason;
				if (!ResolveProjectPathInsideProject(ManifestPath, ResolvedManifestPath, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				CandidatePaths.Add(ResolvedManifestPath);
			}
			else
			{
				for (const FString& CandidatePath : ManifestPaths)
				{
					if (ToolName.TrimStartAndEnd().IsEmpty())
					{
						CandidatePaths.Add(CandidatePath);
						continue;
					}

					TSharedPtr<FJsonObject> CandidateObject;
					FString FailureReason;
					if (LoadJsonObjectFromFile(CandidatePath, CandidateObject, FailureReason) && CandidateObject.IsValid())
					{
						FString CandidateToolName;
						CandidateObject->TryGetStringField(TEXT("toolName"), CandidateToolName);
						if (CandidateToolName.Equals(ToolName.TrimStartAndEnd(), ESearchCase::IgnoreCase))
						{
							CandidatePaths.Add(CandidatePath);
						}
					}
				}
			}

			TArray<TSharedPtr<FJsonValue>> CandidateObjects;
			for (const FString& CandidatePath : CandidatePaths)
			{
				TSharedPtr<FJsonObject> CandidateObject = MakeShared<FJsonObject>();
				CandidateObject->SetStringField(TEXT("manifestPath"), CandidatePath);
				CandidateObject->SetObjectField(TEXT("file"), MakeFileInfoObject(CandidatePath));
				CandidateObjects.Add(MakeShared<FJsonValueObject>(CandidateObject));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_rollback_to_manifest"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("selector"), Selector);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("force"), bForce);
			StructuredContent->SetArrayField(TEXT("candidates"), CandidateObjects);

			if (CandidatePaths.Num() == 0)
			{
				return MakeExecutionResult(TEXT("No matching extension manifest was found."), StructuredContent, true);
			}

			int32 SelectedIndex = FMath::RoundToInt(ManifestIndexDouble);
			if (SelectedIndex < 0)
			{
				SelectedIndex = Selector.TrimStartAndEnd().Equals(TEXT("oldest"), ESearchCase::IgnoreCase) ? CandidatePaths.Num() - 1 : 0;
			}
			if (!CandidatePaths.IsValidIndex(SelectedIndex))
			{
				return MakeExecutionResult(TEXT("manifestIndex is outside the available candidate range."), StructuredContent, true);
			}

			const FString SelectedManifestPath = CandidatePaths[SelectedIndex];
			StructuredContent->SetNumberField(TEXT("selectedIndex"), SelectedIndex);
			StructuredContent->SetStringField(TEXT("selectedManifestPath"), SelectedManifestPath);

			if (!bDryRun && bCreatePreRollbackBackup)
			{
				TSharedPtr<FJsonObject> BackupArguments = MakeShared<FJsonObject>();
				BackupArguments->SetStringField(TEXT("label"), TEXT("pre_rollback"));
				BackupArguments->SetStringField(TEXT("reason"), FString::Printf(TEXT("Pre-rollback snapshot before restoring manifest %s."), *SelectedManifestPath));
				BackupArguments->SetBoolField(TEXT("includeBuildLogs"), false);
				const FUnrealMcpExecutionResult BackupResult = BackupProjectState(*BackupArguments);
				StructuredContent->SetObjectField(TEXT("preRollbackBackup"), BackupResult.StructuredContent.IsValid() ? BackupResult.StructuredContent : MakeShared<FJsonObject>());
				if (BackupResult.bIsError)
				{
					return MakeExecutionResult(BackupResult.Text, StructuredContent, true);
				}
			}

			TSharedPtr<FJsonObject> RollbackArguments = MakeShared<FJsonObject>();
			RollbackArguments->SetStringField(TEXT("manifestPath"), SelectedManifestPath);
			RollbackArguments->SetBoolField(TEXT("dryRun"), bDryRun);
			RollbackArguments->SetBoolField(TEXT("force"), bForce);
			const FUnrealMcpExecutionResult RollbackResult = RollbackLastMcpExtension(*RollbackArguments);
			if (RollbackResult.StructuredContent.IsValid())
			{
				StructuredContent->SetObjectField(TEXT("rollback"), RollbackResult.StructuredContent);
			}
			return MakeExecutionResult(RollbackResult.Text, StructuredContent, RollbackResult.bIsError);
		}

		TSharedPtr<FJsonObject> MakeMemoryEntrySummary(const TSharedPtr<FJsonObject>& EntryObject, bool bIncludeContent)
		{
			TSharedPtr<FJsonObject> SummaryObject = MakeShared<FJsonObject>();
			if (!EntryObject.IsValid())
			{
				return SummaryObject;
			}

			FString Key;
			FString Summary;
			FString Status;
			FString NextStep;
			FString UpdatedAtUtc;
			EntryObject->TryGetStringField(TEXT("key"), Key);
			EntryObject->TryGetStringField(TEXT("summary"), Summary);
			EntryObject->TryGetStringField(TEXT("status"), Status);
			EntryObject->TryGetStringField(TEXT("nextStep"), NextStep);
			EntryObject->TryGetStringField(TEXT("updatedAtUtc"), UpdatedAtUtc);
			SummaryObject->SetStringField(TEXT("key"), Key);
			SummaryObject->SetStringField(TEXT("summary"), Summary);
			SummaryObject->SetStringField(TEXT("status"), Status);
			SummaryObject->SetStringField(TEXT("nextStep"), NextStep);
			SummaryObject->SetStringField(TEXT("updatedAtUtc"), UpdatedAtUtc);

			const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
			if (EntryObject->TryGetArrayField(TEXT("tags"), Tags) && Tags)
			{
				SummaryObject->SetArrayField(TEXT("tags"), *Tags);
			}

			const TSharedPtr<FJsonObject>* Content = nullptr;
			if (bIncludeContent && EntryObject->TryGetObjectField(TEXT("content"), Content) && Content && (*Content).IsValid())
			{
				SummaryObject->SetObjectField(TEXT("content"), *Content);
			}
			return SummaryObject;
		}

		TSharedPtr<FJsonObject> FindMemoryEntryByKey(const TSharedPtr<FJsonObject>& MemoryObject, const FString& Key)
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (!MemoryObject.IsValid() || !MemoryObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
			{
				return nullptr;
			}

			for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
			{
				if (!EntryValue.IsValid() || EntryValue->Type != EJson::Object || !EntryValue->AsObject().IsValid())
				{
					continue;
				}

				FString ExistingKey;
				if (EntryValue->AsObject()->TryGetStringField(TEXT("key"), ExistingKey) && ExistingKey == Key)
				{
					return EntryValue->AsObject();
				}
			}
			return nullptr;
		}

		FString RecommendPipelineNextStep(const TSharedPtr<FJsonObject>& MemoryEntry)
		{
			if (!MemoryEntry.IsValid())
			{
				return TEXT("No matching project memory entry was found. Run unreal.mcp_extension_pipeline or write memory for the active extension.");
			}

			FString Status;
			MemoryEntry->TryGetStringField(TEXT("status"), Status);
			const FString LowerStatus = Status.ToLower();
			if (LowerStatus.Contains(TEXT("restart")))
			{
				return TEXT("Restart Unreal Editor, then run unreal.mcp_extension_pipeline with mode=resume_test or unreal.mcp_run_tool_test.");
			}
			if (LowerStatus.Contains(TEXT("build_failed")))
			{
				return TEXT("Open the latest build log, fix compile errors, then rerun unreal.mcp_build_editor.");
			}
			if (LowerStatus.Contains(TEXT("tool_test_succeeded")))
			{
				return TEXT("Run unreal.mcp_tool_audit, then optionally run unreal.mcp_clean_test_artifacts in dryRun mode.");
			}
			if (LowerStatus.Contains(TEXT("pipeline_apply_complete")))
			{
				return TEXT("Run unreal.mcp_build_editor, restart if needed, then resume the tool test.");
			}
			return TEXT("Continue with the next pipeline step shown in project memory.");
		}

		void BuildSimpleLineDiffPreview(
			const FString& BeforeText,
			const FString& AfterText,
			int32 MaxPreviewLines,
			TArray<TSharedPtr<FJsonValue>>& OutChangedLines,
			FString& OutPreviewText,
			int32& OutChangedLineCount,
			bool& bOutTruncated)
		{
			TArray<FString> BeforeLines;
			TArray<FString> AfterLines;
			BeforeText.ParseIntoArrayLines(BeforeLines, false);
			AfterText.ParseIntoArrayLines(AfterLines, false);

			const int32 SafeMaxPreviewLines = FMath::Max(1, MaxPreviewLines);
			TArray<FString> PreviewLines;
			OutChangedLineCount = 0;
			bOutTruncated = false;

			auto AddPreviewLine = [&](
				const FString& Kind,
				int32 BeforeLineNumber,
				int32 AfterLineNumber,
				const FString& BeforeLine,
				const FString& AfterLine)
			{
				++OutChangedLineCount;
				if (OutChangedLines.Num() >= SafeMaxPreviewLines)
				{
					bOutTruncated = true;
					return;
				}

				TSharedPtr<FJsonObject> LineObject = MakeShared<FJsonObject>();
				LineObject->SetStringField(TEXT("kind"), Kind);
				if (BeforeLineNumber > 0)
				{
					LineObject->SetNumberField(TEXT("beforeLine"), BeforeLineNumber);
				}
				LineObject->SetStringField(TEXT("before"), BeforeLine.Left(1000));
				if (AfterLineNumber > 0)
				{
					LineObject->SetNumberField(TEXT("afterLine"), AfterLineNumber);
				}
				LineObject->SetStringField(TEXT("after"), AfterLine.Left(1000));
				OutChangedLines.Add(MakeShared<FJsonValueObject>(LineObject));

				PreviewLines.Add(FString::Printf(TEXT("@@ %s before:%d after:%d @@"), *Kind, BeforeLineNumber, AfterLineNumber));
				if (!BeforeLine.IsEmpty())
				{
					PreviewLines.Add(TEXT("- ") + BeforeLine.Left(1000));
				}
				if (!AfterLine.IsEmpty())
				{
					PreviewLines.Add(TEXT("+ ") + AfterLine.Left(1000));
				}
			};

			auto FindLineForward = [](const TArray<FString>& Lines, const FString& Needle, int32 StartIndex, int32 Lookahead) -> int32
			{
				const int32 EndIndex = FMath::Min(Lines.Num(), StartIndex + Lookahead);
				for (int32 Index = StartIndex; Index < EndIndex; ++Index)
				{
					if (Lines[Index] == Needle)
					{
						return Index;
					}
				}
				return INDEX_NONE;
			};

			const int32 Lookahead = 300;
			int32 BeforeIndex = 0;
			int32 AfterIndex = 0;
			while (BeforeIndex < BeforeLines.Num() || AfterIndex < AfterLines.Num())
			{
				if (BeforeIndex < BeforeLines.Num()
					&& AfterIndex < AfterLines.Num()
					&& BeforeLines[BeforeIndex] == AfterLines[AfterIndex])
				{
					++BeforeIndex;
					++AfterIndex;
					continue;
				}

				bool bHandled = false;
				if (BeforeIndex < BeforeLines.Num() && AfterIndex < AfterLines.Num())
				{
					const int32 MatchingAfterIndex = FindLineForward(AfterLines, BeforeLines[BeforeIndex], AfterIndex + 1, Lookahead);
					if (MatchingAfterIndex != INDEX_NONE)
					{
						for (int32 InsertIndex = AfterIndex; InsertIndex < MatchingAfterIndex; ++InsertIndex)
						{
							AddPreviewLine(TEXT("inserted"), BeforeIndex + 1, InsertIndex + 1, FString(), AfterLines[InsertIndex]);
						}
						AfterIndex = MatchingAfterIndex;
						bHandled = true;
					}
				}

				if (!bHandled && BeforeIndex < BeforeLines.Num() && AfterIndex < AfterLines.Num())
				{
					const int32 MatchingBeforeIndex = FindLineForward(BeforeLines, AfterLines[AfterIndex], BeforeIndex + 1, Lookahead);
					if (MatchingBeforeIndex != INDEX_NONE)
					{
						for (int32 DeleteIndex = BeforeIndex; DeleteIndex < MatchingBeforeIndex; ++DeleteIndex)
						{
							AddPreviewLine(TEXT("deleted"), DeleteIndex + 1, AfterIndex + 1, BeforeLines[DeleteIndex], FString());
						}
						BeforeIndex = MatchingBeforeIndex;
						bHandled = true;
					}
				}

				if (bHandled)
				{
					continue;
				}

				const FString BeforeLine = BeforeLines.IsValidIndex(BeforeIndex) ? BeforeLines[BeforeIndex] : FString();
				const FString AfterLine = AfterLines.IsValidIndex(AfterIndex) ? AfterLines[AfterIndex] : FString();
				AddPreviewLine(
					BeforeLines.IsValidIndex(BeforeIndex) && AfterLines.IsValidIndex(AfterIndex) ? TEXT("changed") : (BeforeLines.IsValidIndex(BeforeIndex) ? TEXT("deleted") : TEXT("inserted")),
					BeforeLines.IsValidIndex(BeforeIndex) ? BeforeIndex + 1 : 0,
					AfterLines.IsValidIndex(AfterIndex) ? AfterIndex + 1 : 0,
					BeforeLine,
					AfterLine);
				if (BeforeLines.IsValidIndex(BeforeIndex))
				{
					++BeforeIndex;
				}
				if (AfterLines.IsValidIndex(AfterIndex))
				{
					++AfterIndex;
				}
			}

			if (bOutTruncated)
			{
				PreviewLines.Add(FString::Printf(TEXT("... truncated after %d changed preview lines ..."), SafeMaxPreviewLines));
			}
			OutPreviewText = FString::Join(PreviewLines, TEXT("\n"));
		}

		TSharedPtr<FJsonObject> MakeTextDiffObject(const FString& BeforeText, const FString& AfterText, int32 MaxPreviewLines)
		{
			TArray<FString> BeforeLines;
			TArray<FString> AfterLines;
			BeforeText.ParseIntoArrayLines(BeforeLines, false);
			AfterText.ParseIntoArrayLines(AfterLines, false);

			TArray<TSharedPtr<FJsonValue>> ChangedLines;
			FString PreviewText;
			int32 ChangedLineCount = 0;
			bool bTruncated = false;
			BuildSimpleLineDiffPreview(BeforeText, AfterText, MaxPreviewLines, ChangedLines, PreviewText, ChangedLineCount, bTruncated);

			TSharedPtr<FJsonObject> DiffObject = MakeShared<FJsonObject>();
			DiffObject->SetNumberField(TEXT("beforeLineCount"), BeforeLines.Num());
			DiffObject->SetNumberField(TEXT("afterLineCount"), AfterLines.Num());
			DiffObject->SetNumberField(TEXT("changedLineCount"), ChangedLineCount);
			DiffObject->SetBoolField(TEXT("hasChanges"), ChangedLineCount > 0);
			DiffObject->SetBoolField(TEXT("truncated"), bTruncated);
			DiffObject->SetStringField(TEXT("previewText"), PreviewText);
			DiffObject->SetArrayField(TEXT("changedLines"), ChangedLines);
			return DiffObject;
		}

		FUnrealMcpExecutionResult DiffLastMcpApply(const FJsonObject& Arguments)
		{
			FString ManifestPath = GetLatestMcpExtensionManifestPath();
			bool bIncludeFullText = false;
			Arguments.TryGetStringField(TEXT("manifestPath"), ManifestPath);
			Arguments.TryGetBoolField(TEXT("includeFullText"), bIncludeFullText);
			const int32 MaxPreviewLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("maxPreviewLines"), 120), 1000);

			FString ResolvedManifestPath;
			FString FailureReason;
			if (!ResolveProjectPathInsideProject(ManifestPath, ResolvedManifestPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TSharedPtr<FJsonObject> ManifestObject;
			if (!LoadJsonObjectFromFile(ResolvedManifestPath, ManifestObject, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString ToolName;
			FString SourcePath;
			FString BackupSourcePath;
			FString AfterSourcePath;
			FString SourceHashBefore;
			FString SourceHashAfter;
			ManifestObject->TryGetStringField(TEXT("toolName"), ToolName);
			ManifestObject->TryGetStringField(TEXT("sourcePath"), SourcePath);
			ManifestObject->TryGetStringField(TEXT("backupSourcePath"), BackupSourcePath);
			ManifestObject->TryGetStringField(TEXT("afterSourcePath"), AfterSourcePath);
			ManifestObject->TryGetStringField(TEXT("sourceHashBefore"), SourceHashBefore);
			ManifestObject->TryGetStringField(TEXT("sourceHashAfter"), SourceHashAfter);

			if (BackupSourcePath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("Manifest is missing backupSourcePath."), nullptr, true);
			}
			if (AfterSourcePath.IsEmpty())
			{
				AfterSourcePath = SourcePath;
			}
			if (AfterSourcePath.IsEmpty())
			{
				return MakeExecutionResult(TEXT("Manifest is missing afterSourcePath and sourcePath."), nullptr, true);
			}

			FString ResolvedBeforePath;
			FString ResolvedAfterPath;
			if (!ResolveProjectPathInsideProject(BackupSourcePath, ResolvedBeforePath, FailureReason)
				|| !ResolveProjectPathInsideProject(AfterSourcePath, ResolvedAfterPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString BeforeText;
			FString AfterText;
			if (!FFileHelper::LoadFileToString(BeforeText, *ResolvedBeforePath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read before snapshot '%s'."), *ResolvedBeforePath), nullptr, true);
			}
			if (!FFileHelper::LoadFileToString(AfterText, *ResolvedAfterPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read after snapshot '%s'."), *ResolvedAfterPath), nullptr, true);
			}

			TArray<FString> BeforeLines;
			TArray<FString> AfterLines;
			BeforeText.ParseIntoArrayLines(BeforeLines, false);
			AfterText.ParseIntoArrayLines(AfterLines, false);

			TArray<TSharedPtr<FJsonValue>> ChangedLines;
			FString PreviewText;
			int32 ChangedLineCount = 0;
			bool bTruncated = false;
			BuildSimpleLineDiffPreview(BeforeText, AfterText, MaxPreviewLines, ChangedLines, PreviewText, ChangedLineCount, bTruncated);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_diff_last_apply"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("manifestPath"), ResolvedManifestPath);
			StructuredContent->SetStringField(TEXT("sourcePath"), SourcePath);
			StructuredContent->SetStringField(TEXT("beforePath"), ResolvedBeforePath);
			StructuredContent->SetStringField(TEXT("afterPath"), ResolvedAfterPath);
			StructuredContent->SetStringField(TEXT("sourceHashBefore"), SourceHashBefore);
			StructuredContent->SetStringField(TEXT("sourceHashAfter"), SourceHashAfter);
			StructuredContent->SetStringField(TEXT("computedBeforeHash"), HashTextForManifest(BeforeText));
			StructuredContent->SetStringField(TEXT("computedAfterHash"), HashTextForManifest(AfterText));
			StructuredContent->SetNumberField(TEXT("beforeLineCount"), BeforeLines.Num());
			StructuredContent->SetNumberField(TEXT("afterLineCount"), AfterLines.Num());
			StructuredContent->SetNumberField(TEXT("changedLineCount"), ChangedLineCount);
			StructuredContent->SetBoolField(TEXT("hasChanges"), ChangedLineCount > 0);
			StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
			StructuredContent->SetStringField(TEXT("previewText"), PreviewText);
			StructuredContent->SetArrayField(TEXT("changedLines"), ChangedLines);
			const TArray<TSharedPtr<FJsonValue>>* ManifestChanges = nullptr;
			if (ManifestObject->TryGetArrayField(TEXT("changes"), ManifestChanges) && ManifestChanges)
			{
				StructuredContent->SetArrayField(TEXT("manifestChanges"), *ManifestChanges);
			}
			if (bIncludeFullText)
			{
				StructuredContent->SetStringField(TEXT("beforeText"), BeforeText);
				StructuredContent->SetStringField(TEXT("afterText"), AfterText);
			}

			return MakeExecutionResult(
				FString::Printf(TEXT("Last MCP apply diff for %s: changedLineCount=%d truncated=%s."),
					ToolName.IsEmpty() ? TEXT("<unknown>") : *ToolName,
					ChangedLineCount,
					bTruncated ? TEXT("true") : TEXT("false")),
				StructuredContent,
				false);
		}

		FUnrealMcpExecutionResult ValidateCppSnippet(const FJsonObject& Arguments)
		{
			FString SnippetText;
			FString SnippetName = TEXT("ExecuteToolHandler.cpp.snippet");
			FString ToolName;
			FString ScaffoldDir;
			FString OutputRoot;
			Arguments.TryGetStringField(TEXT("snippetText"), SnippetText);
			Arguments.TryGetStringField(TEXT("snippetName"), SnippetName);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);

			FString CanonicalSnippetName;
			FString FailureReason;
			if (!CanonicalizeScaffoldSnippetName(SnippetName, CanonicalSnippetName, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString Source = TEXT("snippetText");
			FString SnippetPath;
			if (SnippetText.TrimStartAndEnd().IsEmpty())
			{
				TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
				ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
				ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
				ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
				FString ResolvedScaffoldDir;
				FString ResolvedToolName;
				if (!ResolveMcpScaffoldDirectory(*ResolveArguments, ResolvedScaffoldDir, ResolvedToolName, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}
				if (ToolName.TrimStartAndEnd().IsEmpty())
				{
					ToolName = ResolvedToolName;
				}
				SnippetPath = FPaths::Combine(ResolvedScaffoldDir, CanonicalSnippetName);
				Source = TEXT("scaffoldFile");
				if (!FFileHelper::LoadFileToString(SnippetText, *SnippetPath))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to read snippet '%s'."), *SnippetPath), nullptr, true);
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = ValidateCppSnippetText(SnippetText, CanonicalSnippetName, ToolName);
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_validate_cpp_snippet"));
			StructuredContent->SetStringField(TEXT("source"), Source);
			if (!SnippetPath.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("snippetPath"), SnippetPath);
			}

			const bool bSafe = StructuredContent->GetBoolField(TEXT("safe"));
			return MakeExecutionResult(
				FString::Printf(TEXT("C++ snippet validation for %s safe=%s errors=%d warnings=%d."),
					*CanonicalSnippetName,
					bSafe ? TEXT("true") : TEXT("false"),
					static_cast<int32>(StructuredContent->GetNumberField(TEXT("errorCount"))),
					static_cast<int32>(StructuredContent->GetNumberField(TEXT("warningCount")))),
				StructuredContent,
				!bSafe);
		}

		FUnrealMcpExecutionResult PatchScaffoldSnippet(const FJsonObject& Arguments)
		{
			FString SnippetName;
			if (!Arguments.TryGetStringField(TEXT("snippetName"), SnippetName) || SnippetName.TrimStartAndEnd().IsEmpty())
			{
				return MakeExecutionResult(TEXT("snippetName is required."), nullptr, true);
			}

			FString CanonicalSnippetName;
			FString FailureReason;
			if (!CanonicalizeScaffoldSnippetName(SnippetName, CanonicalSnippetName, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString ToolName;
			FString ScaffoldDir;
			FString OutputRoot;
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);

			TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
			ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
			ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
			FString ResolvedScaffoldDir;
			FString ResolvedToolName;
			if (!ResolveMcpScaffoldDirectory(*ResolveArguments, ResolvedScaffoldDir, ResolvedToolName, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			if (ToolName.TrimStartAndEnd().IsEmpty())
			{
				ToolName = ResolvedToolName;
			}

			const FString SnippetPath = FPaths::Combine(ResolvedScaffoldDir, CanonicalSnippetName);
			FString BeforeText;
			if (!FFileHelper::LoadFileToString(BeforeText, *SnippetPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read snippet '%s'."), *SnippetPath), nullptr, true);
			}

			FString Mode;
			FString NewText;
			FString FindText;
			FString ReplaceText;
			FString AppendText;
			FString PrependText;
			bool bDryRun = true;
			bool bCreateBackup = true;
			bool bReplaceAll = false;
			bool bAllowUnsafe = false;
			Arguments.TryGetStringField(TEXT("mode"), Mode);
			Arguments.TryGetStringField(TEXT("newText"), NewText);
			Arguments.TryGetStringField(TEXT("findText"), FindText);
			Arguments.TryGetStringField(TEXT("replaceText"), ReplaceText);
			Arguments.TryGetStringField(TEXT("appendText"), AppendText);
			Arguments.TryGetStringField(TEXT("prependText"), PrependText);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("createBackup"), bCreateBackup);
			Arguments.TryGetBoolField(TEXT("replaceAll"), bReplaceAll);
			Arguments.TryGetBoolField(TEXT("allowUnsafe"), bAllowUnsafe);
			const int32 DiffPreviewLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("diffPreviewLines"), 120), 1000);

			Mode = Mode.TrimStartAndEnd().ToLower();
			if (Mode.IsEmpty())
			{
				if (!FindText.IsEmpty())
				{
					Mode = TEXT("replace_text");
				}
				else if (!AppendText.IsEmpty())
				{
					Mode = TEXT("append");
				}
				else if (!PrependText.IsEmpty())
				{
					Mode = TEXT("prepend");
				}
				else
				{
					Mode = TEXT("replace_all");
				}
			}

			FString AfterText = BeforeText;
			bool bAlreadyApplied = false;
			if (Mode == TEXT("replace_all"))
			{
				AfterText = NewText;
			}
			else if (Mode == TEXT("replace_text"))
			{
				if (FindText.IsEmpty())
				{
					return MakeExecutionResult(TEXT("findText is required when mode=replace_text."), nullptr, true);
				}
				if (!AfterText.Contains(FindText, ESearchCase::CaseSensitive))
				{
					if (!ReplaceText.IsEmpty() && AfterText.Contains(ReplaceText, ESearchCase::CaseSensitive))
					{
						bAlreadyApplied = true;
					}
					else
					{
						return MakeExecutionResult(TEXT("findText was not found and replaceText does not already appear in the snippet."), nullptr, true);
					}
				}
				else if (bReplaceAll)
				{
					AfterText.ReplaceInline(*FindText, *ReplaceText, ESearchCase::CaseSensitive);
				}
				else
				{
					const int32 Index = AfterText.Find(FindText, ESearchCase::CaseSensitive);
					AfterText = AfterText.Left(Index) + ReplaceText + AfterText.Mid(Index + FindText.Len());
				}
			}
			else if (Mode == TEXT("append"))
			{
				if (AppendText.IsEmpty())
				{
					return MakeExecutionResult(TEXT("appendText is required when mode=append."), nullptr, true);
				}
				if (AfterText.Contains(AppendText, ESearchCase::CaseSensitive))
				{
					bAlreadyApplied = true;
				}
				else
				{
					AfterText += (AfterText.EndsWith(TEXT("\n")) ? FString() : FString(TEXT("\n"))) + AppendText;
				}
			}
			else if (Mode == TEXT("prepend"))
			{
				if (PrependText.IsEmpty())
				{
					return MakeExecutionResult(TEXT("prependText is required when mode=prepend."), nullptr, true);
				}
				if (AfterText.Contains(PrependText, ESearchCase::CaseSensitive))
				{
					bAlreadyApplied = true;
				}
				else
				{
					AfterText = PrependText + (PrependText.EndsWith(TEXT("\n")) ? FString() : FString(TEXT("\n"))) + AfterText;
				}
			}
			else
			{
				return MakeExecutionResult(TEXT("mode must be replace_all, replace_text, append, or prepend."), nullptr, true);
			}

			const bool bChanged = BeforeText != AfterText;
			TSharedPtr<FJsonObject> ValidationObject = ValidateCppSnippetText(AfterText, CanonicalSnippetName, ToolName);
			const bool bSafe = ValidationObject->GetBoolField(TEXT("safe"));
			TSharedPtr<FJsonObject> DiffObject = MakeTextDiffObject(BeforeText, AfterText, DiffPreviewLines);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_patch_scaffold_snippet"));
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetStringField(TEXT("toolId"), SanitizeMcpToolIdForPath(ToolName));
			StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
			StructuredContent->SetStringField(TEXT("snippetName"), CanonicalSnippetName);
			StructuredContent->SetStringField(TEXT("snippetPath"), SnippetPath);
			StructuredContent->SetStringField(TEXT("mode"), Mode);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("changed"), bChanged);
			StructuredContent->SetBoolField(TEXT("alreadyApplied"), bAlreadyApplied || !bChanged);
			StructuredContent->SetBoolField(TEXT("createBackup"), bCreateBackup);
			StructuredContent->SetBoolField(TEXT("allowUnsafe"), bAllowUnsafe);
			StructuredContent->SetStringField(TEXT("beforeHash"), HashTextForManifest(BeforeText));
			StructuredContent->SetStringField(TEXT("afterHash"), HashTextForManifest(AfterText));
			StructuredContent->SetObjectField(TEXT("validation"), ValidationObject);
			StructuredContent->SetObjectField(TEXT("snippetDiff"), DiffObject);

			if (!bSafe && !bAllowUnsafe)
			{
				return MakeExecutionResult(TEXT("Patched snippet failed static safety validation. Pass allowUnsafe=true only after manual review."), StructuredContent, true);
			}

			if (bDryRun || !bChanged)
			{
				return MakeExecutionResult(
					FString::Printf(TEXT("%s snippet patch for %s changed=%s safe=%s."),
						bDryRun ? TEXT("Dry run") : TEXT("No-op"),
						*CanonicalSnippetName,
						bChanged ? TEXT("true") : TEXT("false"),
						bSafe ? TEXT("true") : TEXT("false")),
					StructuredContent,
					false);
			}

			FString BackupDirectory;
			FString BackupBeforePath;
			FString BackupAfterPath;
			if (bCreateBackup)
			{
				const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
				const FString SnippetId = FPaths::GetBaseFilename(CanonicalSnippetName).Replace(TEXT("."), TEXT("_"));
				BackupDirectory = FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("SnippetBackups"), Timestamp + TEXT("_") + SanitizeMcpToolIdForPath(ToolName) + TEXT("_") + SnippetId);
				BackupBeforePath = FPaths::Combine(BackupDirectory, CanonicalSnippetName + TEXT(".before"));
				BackupAfterPath = FPaths::Combine(BackupDirectory, CanonicalSnippetName + TEXT(".after"));
				if (!IFileManager::Get().MakeDirectory(*BackupDirectory, true)
					|| !FFileHelper::SaveStringToFile(BeforeText, *BackupBeforePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
					|| !FFileHelper::SaveStringToFile(AfterText, *BackupAfterPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					return MakeExecutionResult(FString::Printf(TEXT("Failed to create snippet backup under '%s'."), *BackupDirectory), StructuredContent, true);
				}
			}

			if (!FFileHelper::SaveStringToFile(AfterText, *SnippetPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to write snippet '%s'."), *SnippetPath), StructuredContent, true);
			}

			if (bCreateBackup)
			{
				TSharedPtr<FJsonObject> ManifestObject = MakeShared<FJsonObject>();
				ManifestObject->SetStringField(TEXT("action"), TEXT("mcp_patch_scaffold_snippet"));
				ManifestObject->SetStringField(TEXT("toolName"), ToolName);
				ManifestObject->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
				ManifestObject->SetStringField(TEXT("snippetName"), CanonicalSnippetName);
				ManifestObject->SetStringField(TEXT("snippetPath"), SnippetPath);
				ManifestObject->SetStringField(TEXT("backupDirectory"), BackupDirectory);
				ManifestObject->SetStringField(TEXT("backupBeforePath"), BackupBeforePath);
				ManifestObject->SetStringField(TEXT("backupAfterPath"), BackupAfterPath);
				ManifestObject->SetStringField(TEXT("beforeHash"), HashTextForManifest(BeforeText));
				ManifestObject->SetStringField(TEXT("afterHash"), HashTextForManifest(AfterText));
				ManifestObject->SetStringField(TEXT("patchedAtUtc"), FDateTime::UtcNow().ToIso8601());
				ManifestObject->SetObjectField(TEXT("validation"), ValidationObject);
				FString ManifestFailure;
				const FString ManifestPath = FPaths::Combine(BackupDirectory, TEXT("Manifest.json"));
				if (!SaveJsonObjectToFile(ManifestObject, ManifestPath, ManifestFailure))
				{
					return MakeExecutionResult(ManifestFailure, StructuredContent, true);
				}
				StructuredContent->SetStringField(TEXT("backupDirectory"), BackupDirectory);
				StructuredContent->SetStringField(TEXT("backupBeforePath"), BackupBeforePath);
				StructuredContent->SetStringField(TEXT("backupAfterPath"), BackupAfterPath);
				StructuredContent->SetStringField(TEXT("manifestPath"), ManifestPath);
			}

			return MakeExecutionResult(
				FString::Printf(TEXT("Patched %s for %s. Backup: %s"), *CanonicalSnippetName, *ToolName, BackupDirectory.IsEmpty() ? TEXT("<none>") : *BackupDirectory),
				StructuredContent,
				false);
		}


		FString GetHostBuildPlatformName()
		{
#if PLATFORM_MAC
			return TEXT("Mac");
#elif PLATFORM_WINDOWS
			return TEXT("Win64");
#elif PLATFORM_LINUX
			return TEXT("Linux");
#else
			return FPlatformProperties::PlatformName();
#endif
		}

		FString GetUnrealBuildScriptPath()
		{
			const FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
#if PLATFORM_MAC
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/Mac/Build.sh"));
#elif PLATFORM_WINDOWS
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/Build.bat"));
#elif PLATFORM_LINUX
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/Linux/Build.sh"));
#else
			return FPaths::Combine(EngineDir, TEXT("Build/BatchFiles/Build.sh"));
#endif
		}

		FString QuoteCommandLineArgument(const FString& Value)
		{
			FString Escaped = Value;
			Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("\"%s\""), *Escaped);
		}

		bool LooksLikeBuildErrorLine(const FString& Line)
		{
			const FString Lower = Line.ToLower();
			return Lower.Contains(TEXT(": error"))
				|| Lower.Contains(TEXT(" error c"))
				|| Lower.Contains(TEXT("fatal error"))
				|| Lower.Contains(TEXT(" error:"))
				|| Lower.Contains(TEXT("error:"))
				|| Lower.Contains(TEXT("error "));
		}

		bool LooksLikeImportantBuildLine(const FString& Line)
		{
			const FString Lower = Line.ToLower();
			return LooksLikeBuildErrorLine(Line)
				|| Lower.Contains(TEXT("warning"))
				|| Lower.Contains(TEXT("result:"))
				|| Lower.Contains(TEXT("total execution time"))
				|| Lower.Contains(TEXT("failed"))
				|| Lower.Contains(TEXT("succeeded"));
		}

		TSharedPtr<FJsonObject> MakeBuildLineObject(const FString& Line)
		{
			TSharedPtr<FJsonObject> LineObject = MakeShared<FJsonObject>();
			LineObject->SetStringField(TEXT("raw"), Line.TrimStartAndEnd());

			FString File;
			int32 LineNumber = 0;
			FString Message = Line.TrimStartAndEnd();

			const int32 MsvcMarker = Line.Find(TEXT("): error"), ESearchCase::IgnoreCase);
			if (MsvcMarker != INDEX_NONE)
			{
				const FString Prefix = Line.Left(MsvcMarker);
				const int32 OpenParen = Prefix.Find(TEXT("("), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
				if (OpenParen != INDEX_NONE)
				{
					File = Prefix.Left(OpenParen);
					const FString LinePart = Prefix.Mid(OpenParen + 1);
					LexTryParseString(LineNumber, *LinePart);
					Message = Line.Mid(MsvcMarker + 3).TrimStartAndEnd();
				}
			}

			if (File.IsEmpty())
			{
				int32 ErrorMarker = Line.Find(TEXT(": error"), ESearchCase::IgnoreCase);
				if (ErrorMarker == INDEX_NONE)
				{
					ErrorMarker = Line.Find(TEXT(": fatal error"), ESearchCase::IgnoreCase);
				}
				if (ErrorMarker != INDEX_NONE)
				{
					const FString Prefix = Line.Left(ErrorMarker);
					Message = Line.Mid(ErrorMarker + 2).TrimStartAndEnd();

					const int32 LastColon = Prefix.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					if (LastColon != INDEX_NONE)
					{
						const FString MaybeLine = Prefix.Mid(LastColon + 1);
						if (LexTryParseString(LineNumber, *MaybeLine))
						{
							File = Prefix.Left(LastColon);
						}
						else
						{
							File = Prefix;
						}
					}
					else
					{
						File = Prefix;
					}
				}
			}

			if (!File.TrimStartAndEnd().IsEmpty())
			{
				LineObject->SetStringField(TEXT("file"), File.TrimStartAndEnd());
			}
			if (LineNumber > 0)
			{
				LineObject->SetNumberField(TEXT("line"), LineNumber);
			}
			LineObject->SetStringField(TEXT("message"), Message);
			return LineObject;
		}

		void ParseBuildLog(const FString& LogText, int32 ReturnCode, const TSharedPtr<FJsonObject>& StructuredContent)
		{
			TArray<FString> Lines;
			LogText.ParseIntoArrayLines(Lines, false);

			TArray<TSharedPtr<FJsonValue>> ErrorLines;
			TArray<TSharedPtr<FJsonValue>> KeyLines;
			for (const FString& Line : Lines)
			{
				if (LooksLikeBuildErrorLine(Line))
				{
					ErrorLines.Add(MakeShared<FJsonValueObject>(MakeBuildLineObject(Line)));
				}

				if (LooksLikeImportantBuildLine(Line))
				{
					KeyLines.Add(MakeShared<FJsonValueString>(Line.TrimStartAndEnd()));
					if (KeyLines.Num() > 120)
					{
						KeyLines.RemoveAt(0);
					}
				}
			}

			const bool bSucceeded = ReturnCode == 0 && !LogText.Contains(TEXT("Result: Failed"), ESearchCase::IgnoreCase);
			StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);
			StructuredContent->SetNumberField(TEXT("returnCode"), ReturnCode);
			StructuredContent->SetNumberField(TEXT("errorCount"), ErrorLines.Num());
			StructuredContent->SetArrayField(TEXT("errors"), ErrorLines);
			StructuredContent->SetArrayField(TEXT("keyLogLines"), KeyLines);
		}

		FString ResolveBuildSourceFilePath(const FString& FilePath)
		{
			FString TrimmedPath = FilePath.TrimStartAndEnd();
			if (TrimmedPath.IsEmpty())
			{
				return FString();
			}

			if (FPaths::FileExists(TrimmedPath))
			{
				return FPaths::ConvertRelativePathToFull(TrimmedPath);
			}

			const TArray<FString> CandidateRoots = {
				FPaths::ProjectDir(),
				FPaths::EngineDir()
			};
			for (const FString& Root : CandidateRoots)
			{
				const FString CandidatePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, TrimmedPath));
				if (FPaths::FileExists(CandidatePath))
				{
					return CandidatePath;
				}
			}

			return TrimmedPath;
		}

		TSharedPtr<FJsonObject> MakeSourceContextObject(const FString& SourcePath, int32 LineNumber, int32 ContextLines)
		{
			TSharedPtr<FJsonObject> ContextObject = MakeShared<FJsonObject>();
			ContextObject->SetStringField(TEXT("path"), SourcePath);
			ContextObject->SetNumberField(TEXT("line"), LineNumber);
			ContextObject->SetBoolField(TEXT("available"), false);

			if (SourcePath.IsEmpty() || LineNumber <= 0 || !FPaths::FileExists(SourcePath))
			{
				return ContextObject;
			}

			FString SourceText;
			if (!FFileHelper::LoadFileToString(SourceText, *SourcePath))
			{
				return ContextObject;
			}

			TArray<FString> Lines;
			SourceText.ParseIntoArrayLines(Lines, false);
			const int32 ZeroBasedLine = LineNumber - 1;
			const int32 FirstLine = FMath::Clamp(ZeroBasedLine - FMath::Max(0, ContextLines), 0, FMath::Max(0, Lines.Num() - 1));
			const int32 LastLine = FMath::Clamp(ZeroBasedLine + FMath::Max(0, ContextLines), 0, FMath::Max(0, Lines.Num() - 1));

			TArray<TSharedPtr<FJsonValue>> ContextLinesArray;
			for (int32 Index = FirstLine; Index <= LastLine && Lines.IsValidIndex(Index); ++Index)
			{
				TSharedPtr<FJsonObject> LineObject = MakeShared<FJsonObject>();
				LineObject->SetNumberField(TEXT("line"), Index + 1);
				LineObject->SetStringField(TEXT("text"), Lines[Index]);
				LineObject->SetBoolField(TEXT("isErrorLine"), Index == ZeroBasedLine);
				ContextLinesArray.Add(MakeShared<FJsonValueObject>(LineObject));
			}

			ContextObject->SetBoolField(TEXT("available"), ContextLinesArray.Num() > 0);
			ContextObject->SetArrayField(TEXT("lines"), ContextLinesArray);
			return ContextObject;
		}

		FString GuessCompileErrorCause(const FString& Message)
		{
			const FString Lower = Message.ToLower();
			if (Lower.Contains(TEXT("undeclared identifier")) || Lower.Contains(TEXT("use of undeclared identifier")) || Lower.Contains(TEXT("was not declared")))
			{
				return TEXT("A symbol is missing, misspelled, out of scope, or requires an include/header declaration.");
			}
			if (Lower.Contains(TEXT("no member named")) || Lower.Contains(TEXT("has no member")) || Lower.Contains(TEXT("is not a member")))
			{
				return TEXT("The code is calling a member that does not exist for this type, often due to an Unreal API mismatch or wrong object type.");
			}
			if (Lower.Contains(TEXT("cannot convert")) || Lower.Contains(TEXT("no viable conversion")) || Lower.Contains(TEXT("cannot initialize")) || Lower.Contains(TEXT("incompatible")))
			{
				return TEXT("A type mismatch or invalid implicit conversion is likely near the reported line.");
			}
			if (Lower.Contains(TEXT("expected ';'")) || Lower.Contains(TEXT("expected expression")) || Lower.Contains(TEXT("expected ')'")) || Lower.Contains(TEXT("expected '}'")))
			{
				return TEXT("A syntax issue such as missing punctuation, mismatched parentheses/braces, or malformed statement is likely near the reported line.");
			}
			if (Lower.Contains(TEXT("cannot open include")) || Lower.Contains(TEXT("file not found")) || Lower.Contains(TEXT("no such file")))
			{
				return TEXT("An include path, module dependency, generated header, or file path is missing.");
			}
			if (Lower.Contains(TEXT("undefined symbol")) || Lower.Contains(TEXT("unresolved external")) || Lower.Contains(TEXT("linker command failed")))
			{
				return TEXT("A function or symbol is declared but not linked/defined, or a Build.cs module dependency is missing.");
			}
			if (Lower.Contains(TEXT("generated.h")))
			{
				return TEXT("Unreal Header Tool ordering or reflection markup may be wrong; generated.h must usually be the final include in a UObject header.");
			}
			return TEXT("Review the nearby source context and preceding build errors; this may be a cascading compiler error.");
		}

		TArray<TSharedPtr<FJsonValue>> MakeSuggestedFixesForCompileError(const FString& Message)
		{
			TArray<TSharedPtr<FJsonValue>> SuggestedFixes;
			const FString Lower = Message.ToLower();
			if (Lower.Contains(TEXT("undeclared identifier")) || Lower.Contains(TEXT("use of undeclared identifier")))
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Check spelling and scope of the reported symbol.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Add the required declaration or include the header that defines the symbol.")));
			}
			else if (Lower.Contains(TEXT("no member named")) || Lower.Contains(TEXT("has no member")))
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Inspect the expression before the member access and confirm its actual type.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Update the call to the correct Unreal API/member for this engine version.")));
			}
			else if (Lower.Contains(TEXT("expected")))
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Check the current and previous line for missing semicolons, braces, parentheses, or commas.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("If this followed an automatic snippet insertion, diff the inserted block boundaries first.")));
			}
			else if (Lower.Contains(TEXT("cannot convert")) || Lower.Contains(TEXT("no viable conversion")))
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Confirm the expected parameter/property type and add an explicit conversion only if safe.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Prefer matching Unreal types exactly, especially FString/FName/FText and TSharedPtr variants.")));
			}
			else
			{
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Fix the earliest reported error first; later errors may be cascading.")));
				SuggestedFixes.Add(MakeShared<FJsonValueString>(TEXT("Run unreal.mcp_diff_last_apply if this happened after applying a scaffold.")));
			}
			return SuggestedFixes;
		}

		FUnrealMcpExecutionResult CompileErrorFixPlan(const FJsonObject& Arguments)
		{
			FString BuildLogPath;
			bool bIncludeSourceContext = true;
			bool bAutoPatch = false;
			bool bDryRun = true;
			double MaxErrorsDouble = 8.0;
			double ContextLinesDouble = 4.0;

			Arguments.TryGetStringField(TEXT("buildLogPath"), BuildLogPath);
			Arguments.TryGetBoolField(TEXT("includeSourceContext"), bIncludeSourceContext);
			Arguments.TryGetBoolField(TEXT("autoPatch"), bAutoPatch);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetNumberField(TEXT("maxErrors"), MaxErrorsDouble);
			Arguments.TryGetNumberField(TEXT("contextLines"), ContextLinesDouble);

			if (BuildLogPath.TrimStartAndEnd().IsEmpty())
			{
				if (!FindNewestFile(GetMcpBuildLogRoot(), TEXT("*.log"), BuildLogPath))
				{
					return MakeExecutionResult(TEXT("No build log was found. Run unreal.mcp_build_editor first or pass buildLogPath."), nullptr, true);
				}
			}

			FString ResolvedBuildLogPath;
			FString FailureReason;
			if (!ResolveProjectPathInsideProject(BuildLogPath, ResolvedBuildLogPath, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString LogText;
			if (!FFileHelper::LoadFileToString(LogText, *ResolvedBuildLogPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Failed to read build log '%s'."), *ResolvedBuildLogPath), nullptr, true);
			}

			TArray<FString> Lines;
			LogText.ParseIntoArrayLines(Lines, false);
			const int32 MaxErrors = FMath::Clamp(static_cast<int32>(MaxErrorsDouble), 1, 50);
			const int32 ContextLines = FMath::Clamp(static_cast<int32>(ContextLinesDouble), 0, 20);

			TArray<TSharedPtr<FJsonValue>> ErrorPlans;
			for (const FString& Line : Lines)
			{
				if (!LooksLikeBuildErrorLine(Line))
				{
					continue;
				}

				TSharedPtr<FJsonObject> ErrorObject = MakeBuildLineObject(Line);
				FString FilePath;
				FString Message;
				double LineNumberDouble = 0.0;
				ErrorObject->TryGetStringField(TEXT("file"), FilePath);
				ErrorObject->TryGetStringField(TEXT("message"), Message);
				ErrorObject->TryGetNumberField(TEXT("line"), LineNumberDouble);

				const FString ResolvedSourcePath = ResolveBuildSourceFilePath(FilePath);
				ErrorObject->SetStringField(TEXT("resolvedFile"), ResolvedSourcePath);
				ErrorObject->SetStringField(TEXT("probableCause"), GuessCompileErrorCause(Message));
				ErrorObject->SetArrayField(TEXT("suggestedFixes"), MakeSuggestedFixesForCompileError(Message));
				if (bIncludeSourceContext)
				{
					ErrorObject->SetObjectField(TEXT("sourceContext"), MakeSourceContextObject(ResolvedSourcePath, static_cast<int32>(LineNumberDouble), ContextLines));
				}
				ErrorObject->SetBoolField(TEXT("autoPatchSupported"), false);
				ErrorObject->SetStringField(TEXT("autoPatchReason"), TEXT("No deterministic safe patch pattern matched. Use this fix plan to patch the scaffold/source intentionally."));

				ErrorPlans.Add(MakeShared<FJsonValueObject>(ErrorObject));
				if (ErrorPlans.Num() >= MaxErrors)
				{
					break;
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_compile_error_fix_plan"));
			StructuredContent->SetStringField(TEXT("buildLogPath"), ResolvedBuildLogPath);
			StructuredContent->SetBoolField(TEXT("includeSourceContext"), bIncludeSourceContext);
			StructuredContent->SetBoolField(TEXT("autoPatch"), bAutoPatch);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("patchApplied"), false);
			StructuredContent->SetStringField(TEXT("autoPatchStatus"), bAutoPatch ? TEXT("requested_but_no_safe_patch_matched") : TEXT("not_requested"));
			StructuredContent->SetNumberField(TEXT("plannedErrorCount"), ErrorPlans.Num());
			StructuredContent->SetArrayField(TEXT("fixPlan"), ErrorPlans);
			StructuredContent->SetStringField(TEXT("nextStep"), TEXT("Patch the earliest root-cause error, rebuild with unreal.mcp_build_editor, then rerun this tool if errors remain."));

			return MakeExecutionResult(
				ErrorPlans.Num() > 0
					? FString::Printf(TEXT("Generated compile error fix plan for %d error(s)."), ErrorPlans.Num())
					: TEXT("No compiler error lines were detected in the build log."),
				StructuredContent,
				false);
		}

		void WriteBuildTestMemory(
			const FString& MemoryKey,
			const FString& Summary,
			const FString& Status,
			const FString& NextStep,
			const TSharedPtr<FJsonObject>& ContentObject)
		{
			TSharedPtr<FJsonObject> MemoryArgs = MakeShared<FJsonObject>();
			MemoryArgs->SetStringField(TEXT("key"), MemoryKey);
			MemoryArgs->SetStringField(TEXT("summary"), Summary);
			MemoryArgs->SetStringField(TEXT("status"), Status);
			MemoryArgs->SetStringField(TEXT("nextStep"), NextStep);
			MemoryArgs->SetStringField(TEXT("contentJson"), JsonObjectToString(ContentObject));
			MemoryArgs->SetArrayField(TEXT("tags"), MakeJsonStringArray({ TEXT("mcp"), TEXT("build"), TEXT("test"), TEXT("restart") }));
			ProjectMemoryWrite(*MemoryArgs);
		}

		TSharedPtr<FJsonObject> MakePipelineStepObject(
			const FString& StepName,
			const FString& Status,
			const FString& Message,
			const FUnrealMcpExecutionResult* Result = nullptr)
		{
			TSharedPtr<FJsonObject> StepObject = MakeShared<FJsonObject>();
			StepObject->SetStringField(TEXT("step"), StepName);
			StepObject->SetStringField(TEXT("status"), Status);
			StepObject->SetStringField(TEXT("message"), Message);
			if (Result)
			{
				StepObject->SetBoolField(TEXT("isError"), Result->bIsError);
				StepObject->SetStringField(TEXT("text"), Result->Text);
				if (Result->StructuredContent.IsValid())
				{
					StepObject->SetObjectField(TEXT("structuredContent"), Result->StructuredContent);
				}
			}
			return StepObject;
		}

		bool ExtractRequestedSchemaFromScaffoldReadme(const FString& ScaffoldDirectory, FString& OutSchemaJson)
		{
			OutSchemaJson.Reset();

			FString ReadmeText;
			const FString ReadmePath = FPaths::Combine(ScaffoldDirectory, TEXT("README.md"));
			if (!FFileHelper::LoadFileToString(ReadmeText, *ReadmePath))
			{
				return false;
			}

			const FString Heading = TEXT("## Requested Argument Schema");
			const int32 HeadingOffset = ReadmeText.Find(Heading, ESearchCase::IgnoreCase);
			if (HeadingOffset == INDEX_NONE)
			{
				return false;
			}

			const int32 FenceStart = ReadmeText.Find(TEXT("```json"), ESearchCase::IgnoreCase, ESearchDir::FromStart, HeadingOffset);
			if (FenceStart == INDEX_NONE)
			{
				return false;
			}

			const int32 JsonStart = FenceStart + FString(TEXT("```json")).Len();
			const int32 FenceEnd = ReadmeText.Find(TEXT("```"), ESearchCase::CaseSensitive, ESearchDir::FromStart, JsonStart);
			if (FenceEnd == INDEX_NONE || FenceEnd <= JsonStart)
			{
				return false;
			}

			OutSchemaJson = ReadmeText.Mid(JsonStart, FenceEnd - JsonStart).TrimStartAndEnd();
			return !OutSchemaJson.IsEmpty();
		}

		TSharedPtr<FJsonObject> MakeScaffoldFileObject(
			const FString& ScaffoldDirectory,
			const FString& FileName,
			bool bRequired,
			bool bIncludeFileText,
			int32 MaxPreviewChars)
		{
			const FString FilePath = FPaths::Combine(ScaffoldDirectory, FileName);
			TSharedPtr<FJsonObject> FileObject = MakeFileInfoObject(FilePath);
			FileObject->SetStringField(TEXT("name"), FileName);
			FileObject->SetBoolField(TEXT("required"), bRequired);
			FileObject->SetBoolField(TEXT("missing"), !FPaths::FileExists(FilePath));

			FString Text;
			if (FFileHelper::LoadFileToString(Text, *FilePath))
			{
				const int32 SafeMaxPreviewChars = FMath::Max(100, MaxPreviewChars);
				FileObject->SetNumberField(TEXT("characterCount"), Text.Len());
				FileObject->SetStringField(TEXT("preview"), Text.Left(SafeMaxPreviewChars));
				FileObject->SetBoolField(TEXT("previewTruncated"), Text.Len() > SafeMaxPreviewChars);
				if (bIncludeFileText)
				{
					FileObject->SetStringField(TEXT("text"), Text);
				}
			}
			return FileObject;
		}

		bool TryReadToolNameFromTestRequest(
			const FString& TestRequestPath,
			FString& OutToolName,
			TSharedPtr<FJsonObject>& OutTestRequestObject,
			FString& OutFailureReason)
		{
			OutToolName.Reset();
			OutTestRequestObject.Reset();
			OutFailureReason.Reset();

			if (!FPaths::FileExists(TestRequestPath))
			{
				OutFailureReason = FString::Printf(TEXT("TestRequest.json is missing at '%s'."), *TestRequestPath);
				return false;
			}

			if (!LoadJsonObjectFromFile(TestRequestPath, OutTestRequestObject, OutFailureReason))
			{
				return false;
			}

			FString Method;
			OutTestRequestObject->TryGetStringField(TEXT("method"), Method);
			if (Method != TEXT("tools/call"))
			{
				OutFailureReason = TEXT("TestRequest.json method is not tools/call.");
				return false;
			}

			const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
			if (!OutTestRequestObject->TryGetObjectField(TEXT("params"), ParamsObject) || !ParamsObject || !(*ParamsObject).IsValid())
			{
				OutFailureReason = TEXT("TestRequest.json is missing params object.");
				return false;
			}

			(*ParamsObject)->TryGetStringField(TEXT("name"), OutToolName);
			OutToolName = OutToolName.TrimStartAndEnd();
			if (OutToolName.IsEmpty())
			{
				OutFailureReason = TEXT("TestRequest.json params.name is empty.");
				return false;
			}
			return true;
		}

		bool ResolveMcpScaffoldForInspection(
			const FJsonObject& Arguments,
			FString& OutDirectory,
			FString& OutToolName,
			FString& OutFailureReason)
		{
			FString ScaffoldDir;
			FString ToolName;
			FString OutputRoot;
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
			ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
			ToolName = ToolName.TrimStartAndEnd();

			if (!ScaffoldDir.IsEmpty())
			{
				if (!ResolveProjectPathInsideProject(ScaffoldDir, OutDirectory, OutFailureReason))
				{
					return false;
				}
			}
			else
			{
				if (ToolName.IsEmpty())
				{
					OutFailureReason = TEXT("Provide either scaffoldDir or toolName.");
					return false;
				}

				FString ResolvedOutputRoot;
				if (!ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, OutFailureReason))
				{
					return false;
				}
				OutDirectory = FPaths::Combine(ResolvedOutputRoot, SanitizeMcpToolIdForPath(ToolName));
			}

			OutToolName = ToolName;
			if (OutToolName.IsEmpty())
			{
				TSharedPtr<FJsonObject> TestRequestObject;
				FString TestRequestFailure;
				TryReadToolNameFromTestRequest(FPaths::Combine(OutDirectory, TEXT("TestRequest.json")), OutToolName, TestRequestObject, TestRequestFailure);
			}
			return true;
		}

		TSharedPtr<FJsonObject> InspectMcpScaffoldDirectory(
			const FString& ScaffoldDirectory,
			const FString& RequestedToolName,
			const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
			bool bIncludeFileText,
			int32 MaxPreviewChars)
		{
			const FString ResolvedScaffoldDirectory = NormalizeFullPathForCompare(ScaffoldDirectory);
			TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
			ResultObject->SetStringField(TEXT("directory"), ResolvedScaffoldDirectory);
			ResultObject->SetStringField(TEXT("toolId"), FPaths::GetCleanFilename(ResolvedScaffoldDirectory));
			ResultObject->SetBoolField(TEXT("exists"), FPaths::DirectoryExists(ResolvedScaffoldDirectory));

			static const TCHAR* RequiredFiles[] = {
				TEXT("README.md"),
				TEXT("ToolDefinition.cpp.snippet"),
				TEXT("ExecuteToolHandler.cpp.snippet"),
				TEXT("TestRequest.json"),
				TEXT("IntegrationChecklist.md")
			};

			TArray<TSharedPtr<FJsonValue>> Files;
			TArray<TSharedPtr<FJsonValue>> MissingRequiredFiles;
			for (const TCHAR* FileName : RequiredFiles)
			{
				TSharedPtr<FJsonObject> FileObject = MakeScaffoldFileObject(ResolvedScaffoldDirectory, FileName, true, bIncludeFileText, MaxPreviewChars);
				if (FileObject->GetBoolField(TEXT("missing")))
				{
					MissingRequiredFiles.Add(MakeShared<FJsonValueString>(FileName));
				}
				Files.Add(MakeShared<FJsonValueObject>(FileObject));
			}
			Files.Add(MakeShared<FJsonValueObject>(MakeScaffoldFileObject(ResolvedScaffoldDirectory, TEXT("ChatCommand.cpp.snippet"), false, bIncludeFileText, MaxPreviewChars)));

			FString ToolName = RequestedToolName;
			FString TestRequestFailure;
			TSharedPtr<FJsonObject> TestRequestObject;
			const bool bValidTestRequest = TryReadToolNameFromTestRequest(
				FPaths::Combine(ResolvedScaffoldDirectory, TEXT("TestRequest.json")),
				ToolName,
				TestRequestObject,
				TestRequestFailure);

			ResultObject->SetStringField(TEXT("toolName"), ToolName);
			ResultObject->SetBoolField(TEXT("validTestRequest"), bValidTestRequest);
			if (!TestRequestFailure.IsEmpty())
			{
				ResultObject->SetStringField(TEXT("testRequestIssue"), TestRequestFailure);
			}
			if (TestRequestObject.IsValid())
			{
				ResultObject->SetObjectField(TEXT("testRequest"), TestRequestObject);
			}

			FString RequestedSchemaJson;
			const bool bHasRequestedSchema = ExtractRequestedSchemaFromScaffoldReadme(ResolvedScaffoldDirectory, RequestedSchemaJson);
			ResultObject->SetBoolField(TEXT("hasRequestedSchema"), bHasRequestedSchema);
			if (bHasRequestedSchema)
			{
				ResultObject->SetStringField(TEXT("requestedSchemaJson"), RequestedSchemaJson);
				TSharedPtr<FJsonObject> RequestedSchemaObject;
				if (LoadJsonObject(RequestedSchemaJson, RequestedSchemaObject) && RequestedSchemaObject.IsValid())
				{
					TArray<TSharedPtr<FJsonValue>> SchemaIssues;
					FString SchemaReason;
					TSharedPtr<FJsonObject> NormalizedSchema;
					const bool bSchemaCompatible = AnalyzeOpenAiSchemaCompatibility(RequestedSchemaObject, SchemaIssues, SchemaReason, NormalizedSchema);
					ResultObject->SetBoolField(TEXT("schemaCompatible"), bSchemaCompatible);
					ResultObject->SetStringField(TEXT("schemaReason"), SchemaReason);
					ResultObject->SetArrayField(TEXT("schemaIssues"), SchemaIssues);
					if (NormalizedSchema.IsValid())
					{
						ResultObject->SetObjectField(TEXT("normalizedSchema"), NormalizedSchema);
					}
				}
				else
				{
					ResultObject->SetBoolField(TEXT("schemaCompatible"), false);
					ResultObject->SetStringField(TEXT("schemaReason"), TEXT("Requested schema block is not valid JSON."));
				}
			}

			const bool bToolListed = !ToolName.IsEmpty() && FindToolDefinitionByName(ToolsArray, ToolName).IsValid();
			ResultObject->SetBoolField(TEXT("toolListed"), bToolListed);
			ResultObject->SetBoolField(TEXT("readyForApply"), MissingRequiredFiles.Num() == 0 && bValidTestRequest);
			ResultObject->SetArrayField(TEXT("missingRequiredFiles"), MissingRequiredFiles);
			ResultObject->SetArrayField(TEXT("files"), Files);
			return ResultObject;
		}

		FUnrealMcpExecutionResult ListMcpScaffolds(
			const FJsonObject& Arguments,
			const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
		{
			FString OutputRoot;
			FString ToolNameFilter;
			bool bIncludeSavedTestScaffolds = true;
			bool bIncludeFileText = false;
			bool bReadyOnly = false;
			Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
			Arguments.TryGetStringField(TEXT("toolNameFilter"), ToolNameFilter);
			Arguments.TryGetBoolField(TEXT("includeSavedTestScaffolds"), bIncludeSavedTestScaffolds);
			Arguments.TryGetBoolField(TEXT("includeFileText"), bIncludeFileText);
			Arguments.TryGetBoolField(TEXT("readyOnly"), bReadyOnly);
			const int32 MaxPreviewChars = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("maxPreviewChars"), 1200), 20000);

			FString ResolvedOutputRoot;
			FString FailureReason;
			if (!ResolveProjectOutputDirectory(OutputRoot, ResolvedOutputRoot, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TArray<FString> CandidateDirectories;
			FindImmediateChildren(ResolvedOutputRoot, TEXT("*"), false, true, CandidateDirectories);
			if (bIncludeSavedTestScaffolds)
			{
				FindImmediateChildren(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestScaffolds")), TEXT("*"), false, true, CandidateDirectories);
			}
			CandidateDirectories.Sort();

			TArray<TSharedPtr<FJsonValue>> ScaffoldObjects;
			TArray<TSharedPtr<FJsonValue>> SkippedObjects;
			for (const FString& CandidateDirectory : CandidateDirectories)
			{
				TSharedPtr<FJsonObject> ScaffoldObject = InspectMcpScaffoldDirectory(CandidateDirectory, FString(), ToolsArray, bIncludeFileText, MaxPreviewChars);
				FString ToolName;
				ScaffoldObject->TryGetStringField(TEXT("toolName"), ToolName);
				const bool bReadyForApply = ScaffoldObject->GetBoolField(TEXT("readyForApply"));

				if (!ToolNameFilter.TrimStartAndEnd().IsEmpty()
					&& !ToolName.Contains(ToolNameFilter.TrimStartAndEnd(), ESearchCase::IgnoreCase)
					&& !CandidateDirectory.Contains(ToolNameFilter.TrimStartAndEnd(), ESearchCase::IgnoreCase))
				{
					ScaffoldObject->SetStringField(TEXT("skipReason"), TEXT("toolNameFilter did not match."));
					SkippedObjects.Add(MakeShared<FJsonValueObject>(ScaffoldObject));
					continue;
				}

				if (bReadyOnly && !bReadyForApply)
				{
					ScaffoldObject->SetStringField(TEXT("skipReason"), TEXT("readyOnly=true and scaffold is not ready for apply."));
					SkippedObjects.Add(MakeShared<FJsonValueObject>(ScaffoldObject));
					continue;
				}

				ScaffoldObjects.Add(MakeShared<FJsonValueObject>(ScaffoldObject));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_list_scaffolds"));
			StructuredContent->SetStringField(TEXT("outputRoot"), ResolvedOutputRoot);
			StructuredContent->SetBoolField(TEXT("includeSavedTestScaffolds"), bIncludeSavedTestScaffolds);
			StructuredContent->SetBoolField(TEXT("includeFileText"), bIncludeFileText);
			StructuredContent->SetBoolField(TEXT("readyOnly"), bReadyOnly);
			StructuredContent->SetStringField(TEXT("toolNameFilter"), ToolNameFilter);
			StructuredContent->SetNumberField(TEXT("scaffoldCount"), ScaffoldObjects.Num());
			StructuredContent->SetNumberField(TEXT("skippedCount"), SkippedObjects.Num());
			StructuredContent->SetArrayField(TEXT("scaffolds"), ScaffoldObjects);
			StructuredContent->SetArrayField(TEXT("skipped"), SkippedObjects);

			return MakeExecutionResult(
				FString::Printf(TEXT("Found %d MCP scaffold%s (%d skipped)."), ScaffoldObjects.Num(), ScaffoldObjects.Num() == 1 ? TEXT("") : TEXT("s"), SkippedObjects.Num()),
				StructuredContent,
				false);
		}

			FUnrealMcpExecutionResult InspectMcpScaffold(
				const FJsonObject& Arguments,
				const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
			{
			bool bIncludeFileText = false;
			Arguments.TryGetBoolField(TEXT("includeFileText"), bIncludeFileText);
			const int32 MaxPreviewChars = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("maxPreviewChars"), 2000), 50000);

			FString ScaffoldDirectory;
			FString ToolName;
			FString FailureReason;
			if (!ResolveMcpScaffoldForInspection(Arguments, ScaffoldDirectory, ToolName, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			TSharedPtr<FJsonObject> InspectionObject = InspectMcpScaffoldDirectory(ScaffoldDirectory, ToolName, ToolsArray, bIncludeFileText, MaxPreviewChars);
			InspectionObject->SetStringField(TEXT("action"), TEXT("mcp_inspect_scaffold"));
			const bool bReadyForApply = InspectionObject->GetBoolField(TEXT("readyForApply"));
			FString InspectedToolName;
			InspectionObject->TryGetStringField(TEXT("toolName"), InspectedToolName);

			return MakeExecutionResult(
				FString::Printf(TEXT("Inspected MCP scaffold %s. readyForApply=%s."),
					InspectedToolName.IsEmpty() ? *InspectionObject->GetStringField(TEXT("toolId")) : *InspectedToolName,
					bReadyForApply ? TEXT("true") : TEXT("false")),
					InspectionObject,
					false);
			}

			TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value);

			TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Object)
			{
				TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
				if (!Object.IsValid())
				{
					return Clone;
				}

				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
				{
					Clone->SetField(Pair.Key, CloneJsonValue(Pair.Value));
				}
				return Clone;
			}

			TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue>& Value)
			{
				if (!Value.IsValid())
				{
					return MakeShared<FJsonValueNull>();
				}

				switch (Value->Type)
				{
				case EJson::String:
					return MakeShared<FJsonValueString>(Value->AsString());
				case EJson::Number:
					return MakeShared<FJsonValueNumber>(Value->AsNumber());
				case EJson::Boolean:
					return MakeShared<FJsonValueBoolean>(Value->AsBool());
				case EJson::Array:
				{
					TArray<TSharedPtr<FJsonValue>> ClonedArray;
					for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
					{
						ClonedArray.Add(CloneJsonValue(Item));
					}
					return MakeShared<FJsonValueArray>(ClonedArray);
				}
				case EJson::Object:
					return MakeShared<FJsonValueObject>(CloneJsonObject(Value->AsObject()));
				case EJson::Null:
				default:
					return MakeShared<FJsonValueNull>();
				}
			}

			TArray<FString> GetSortedJsonObjectKeys(const TSharedPtr<FJsonObject>& Object)
			{
				TArray<FString> Keys;
				if (Object.IsValid())
				{
					Object->Values.GetKeys(Keys);
					Keys.Sort();
				}
				return Keys;
			}

			FString GetSchemaType(const TSharedPtr<FJsonObject>& SchemaObject)
			{
				FString Type;
				if (SchemaObject.IsValid())
				{
					SchemaObject->TryGetStringField(TEXT("type"), Type);
					if (Type.IsEmpty())
					{
						if (SchemaObject->HasTypedField<EJson::Object>(TEXT("properties")))
						{
							Type = TEXT("object");
						}
						else if (SchemaObject->HasField(TEXT("items")))
						{
							Type = TEXT("array");
						}
					}
				}
				return Type.ToLower();
			}

			TSharedPtr<FJsonValue> MakeSampleJsonValueForSchema(const TSharedPtr<FJsonObject>& SchemaObject, bool bBoundaryValue, bool bWrongType)
			{
				const FString Type = GetSchemaType(SchemaObject);
				if (bWrongType)
				{
					if (Type == TEXT("string"))
					{
						return MakeShared<FJsonValueNumber>(12345.0);
					}
					if (Type == TEXT("integer") || Type == TEXT("number"))
					{
						return MakeShared<FJsonValueString>(TEXT("not_a_number"));
					}
					if (Type == TEXT("boolean"))
					{
						return MakeShared<FJsonValueString>(TEXT("not_a_boolean"));
					}
					if (Type == TEXT("array"))
					{
						return MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
					}
					if (Type == TEXT("object"))
					{
						return MakeShared<FJsonValueString>(TEXT("not_an_object"));
					}
					return MakeShared<FJsonValueString>(TEXT("wrong_type_probe"));
				}

				if (!bBoundaryValue && SchemaObject.IsValid())
				{
					const TSharedPtr<FJsonValue> DefaultValue = SchemaObject->TryGetField(TEXT("default"));
					if (DefaultValue.IsValid())
					{
						return CloneJsonValue(DefaultValue);
					}

					const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
					if (SchemaObject->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues && EnumValues->Num() > 0)
					{
						return CloneJsonValue((*EnumValues)[0]);
					}
				}

				if (Type == TEXT("integer"))
				{
					return MakeShared<FJsonValueNumber>(bBoundaryValue ? 0.0 : 1.0);
				}
				if (Type == TEXT("number"))
				{
					return MakeShared<FJsonValueNumber>(bBoundaryValue ? 0.0 : 1.0);
				}
				if (Type == TEXT("boolean"))
				{
					return MakeShared<FJsonValueBoolean>(!bBoundaryValue);
				}
				if (Type == TEXT("array"))
				{
					TArray<TSharedPtr<FJsonValue>> Values;
					if (!bBoundaryValue && SchemaObject.IsValid())
					{
						const TSharedPtr<FJsonObject>* ItemsObject = nullptr;
						if (SchemaObject->TryGetObjectField(TEXT("items"), ItemsObject) && ItemsObject && (*ItemsObject).IsValid())
						{
							Values.Add(MakeSampleJsonValueForSchema(*ItemsObject, false, false));
						}
						else
						{
							Values.Add(MakeShared<FJsonValueString>(TEXT("sample")));
						}
					}
					return MakeShared<FJsonValueArray>(Values);
				}
				if (Type == TEXT("object"))
				{
					TSharedPtr<FJsonObject> ObjectValue = MakeShared<FJsonObject>();
					if (!bBoundaryValue && SchemaObject.IsValid())
					{
						const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
						if (SchemaObject->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && (*PropertiesObject).IsValid())
						{
							for (const FString& Key : GetSortedJsonObjectKeys(*PropertiesObject))
							{
								const TSharedPtr<FJsonObject>* ChildSchema = nullptr;
								if ((*PropertiesObject)->TryGetObjectField(Key, ChildSchema) && ChildSchema && (*ChildSchema).IsValid())
								{
									ObjectValue->SetField(Key, MakeSampleJsonValueForSchema(*ChildSchema, false, false));
								}
							}
						}
					}
					return MakeShared<FJsonValueObject>(ObjectValue);
				}
				return MakeShared<FJsonValueString>(bBoundaryValue ? FString() : FString(TEXT("sample")));
			}

			void GetRequiredSchemaFields(const TSharedPtr<FJsonObject>& SchemaObject, TArray<FString>& OutRequiredFields)
			{
				OutRequiredFields.Reset();
				if (!SchemaObject.IsValid())
				{
					return;
				}

				const TArray<TSharedPtr<FJsonValue>>* RequiredValues = nullptr;
				if (SchemaObject->TryGetArrayField(TEXT("required"), RequiredValues) && RequiredValues)
				{
					for (const TSharedPtr<FJsonValue>& Value : *RequiredValues)
					{
						if (Value.IsValid() && Value->Type == EJson::String)
						{
							OutRequiredFields.Add(Value->AsString());
						}
					}
				}
				OutRequiredFields.Sort();
			}

			TSharedPtr<FJsonObject> MakeSampleArgumentsForSchema(
				const TSharedPtr<FJsonObject>& InputSchema,
				const TSharedPtr<FJsonObject>& ExampleArguments,
				bool bBoundaryValue)
			{
				if (!bBoundaryValue && ExampleArguments.IsValid())
				{
					return CloneJsonObject(ExampleArguments);
				}

				TSharedPtr<FJsonObject> ArgumentsObject = MakeShared<FJsonObject>();
				const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
				if (InputSchema.IsValid()
					&& InputSchema->TryGetObjectField(TEXT("properties"), PropertiesObject)
					&& PropertiesObject
					&& (*PropertiesObject).IsValid())
				{
					for (const FString& Key : GetSortedJsonObjectKeys(*PropertiesObject))
					{
						const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
						if ((*PropertiesObject)->TryGetObjectField(Key, PropertySchema) && PropertySchema && (*PropertySchema).IsValid())
						{
							ArgumentsObject->SetField(Key, MakeSampleJsonValueForSchema(*PropertySchema, bBoundaryValue, false));
						}
					}
				}
				return ArgumentsObject;
			}

			TSharedPtr<FJsonObject> MakeToolCallRequestObject(
				const FString& ToolName,
				const TSharedPtr<FJsonObject>& ArgumentsObject,
				int32 RequestId)
			{
				TSharedPtr<FJsonObject> ParamsObject = MakeShared<FJsonObject>();
				ParamsObject->SetStringField(TEXT("name"), ToolName);
				ParamsObject->SetObjectField(TEXT("arguments"), ArgumentsObject.IsValid() ? ArgumentsObject : MakeShared<FJsonObject>());

				TSharedPtr<FJsonObject> RequestObject = MakeShared<FJsonObject>();
				RequestObject->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
				RequestObject->SetNumberField(TEXT("id"), RequestId);
				RequestObject->SetStringField(TEXT("method"), TEXT("tools/call"));
				RequestObject->SetObjectField(TEXT("params"), ParamsObject);
				return RequestObject;
			}

			TSharedPtr<FJsonObject> MakeMcpTestCaseObject(
				const FString& Name,
				const FString& Description,
				const FString& Category,
				const TSharedPtr<FJsonObject>& RequestObject,
				bool bExpectToolCallError,
				const FString& ExpectationNote)
			{
				TSharedPtr<FJsonObject> TestCaseObject = MakeShared<FJsonObject>();
				TestCaseObject->SetStringField(TEXT("name"), Name);
				TestCaseObject->SetStringField(TEXT("description"), Description);
				TestCaseObject->SetStringField(TEXT("category"), Category);
				TestCaseObject->SetBoolField(TEXT("expectToolListed"), true);
				TestCaseObject->SetBoolField(TEXT("executeTool"), true);
				TestCaseObject->SetBoolField(TEXT("expectToolCallError"), bExpectToolCallError);
				TestCaseObject->SetStringField(TEXT("expectationNote"), ExpectationNote);
				TestCaseObject->SetObjectField(TEXT("request"), RequestObject);
				return TestCaseObject;
			}

			bool WriteMcpGeneratedTestFile(
				const FString& FilePath,
				const TSharedPtr<FJsonObject>& TestObject,
				bool bOverwrite,
				bool bDryRun,
				TArray<TSharedPtr<FJsonValue>>& OutFiles,
				FString& OutFailureReason)
			{
				TSharedPtr<FJsonObject> FileObject = MakeFileInfoObject(FilePath);
				FileObject->SetStringField(TEXT("path"), FilePath);
				FileObject->SetStringField(TEXT("name"), FPaths::GetCleanFilename(FilePath));
				FileObject->SetBoolField(TEXT("dryRun"), bDryRun);

				const FString NewText = JsonObjectToString(TestObject) + TEXT("\n");
				FString ExistingText;
				const bool bExists = FFileHelper::LoadFileToString(ExistingText, *FilePath);
				const bool bUnchanged = bExists && ExistingText == NewText;
				FileObject->SetBoolField(TEXT("exists"), bExists);
				FileObject->SetBoolField(TEXT("unchanged"), bUnchanged);

				if (bDryRun)
				{
					FileObject->SetBoolField(TEXT("wouldWrite"), !bUnchanged);
					OutFiles.Add(MakeShared<FJsonValueObject>(FileObject));
					return true;
				}

				if (bExists && !bOverwrite && !bUnchanged)
				{
					OutFailureReason = FString::Printf(TEXT("Refusing to overwrite existing test file '%s'. Set overwrite=true."), *FilePath);
					return false;
				}

				if (!bUnchanged)
				{
					if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true)
						|| !FFileHelper::SaveStringToFile(NewText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						OutFailureReason = FString::Printf(TEXT("Failed to write test file '%s'."), *FilePath);
						return false;
					}
				}

				FileObject->SetBoolField(TEXT("created"), !bExists);
				FileObject->SetBoolField(TEXT("overwritten"), bExists && !bUnchanged);
				OutFiles.Add(MakeShared<FJsonValueObject>(FileObject));
				return true;
			}

			bool ResolveMcpTestsDirectory(
				const FJsonObject& Arguments,
				FString& OutTestsDirectory,
				FString& OutScaffoldDirectory,
				FString& OutToolName,
				FString& OutFailureReason)
			{
				FString TestsDir;
				FString ToolName;
				FString ScaffoldDir;
				FString OutputRoot;
				Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
				Arguments.TryGetStringField(TEXT("toolName"), ToolName);
				Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
				Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);

				TestsDir = TestsDir.TrimStartAndEnd();
				ToolName = ToolName.TrimStartAndEnd();
				ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
				if (!TestsDir.TrimStartAndEnd().IsEmpty())
				{
					if (!ResolveProjectPathInsideProject(TestsDir, OutTestsDirectory, OutFailureReason))
					{
						return false;
					}

					OutToolName = ToolName;
					if (!ScaffoldDir.IsEmpty())
					{
						if (!ResolveProjectPathInsideProject(ScaffoldDir, OutScaffoldDirectory, OutFailureReason))
						{
							return false;
						}
					}
					else
					{
						OutScaffoldDirectory = FPaths::GetPath(OutTestsDirectory);
					}
					return true;
				}

				TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
				ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
				ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
				ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);

				if (!ResolveMcpScaffoldDirectory(*ResolveArguments, OutScaffoldDirectory, OutToolName, OutFailureReason))
				{
					return false;
				}

				if (OutTestsDirectory.IsEmpty())
				{
					OutTestsDirectory = FPaths::Combine(OutScaffoldDirectory, TEXT("Tests"));
				}
				return true;
			}

			FUnrealMcpExecutionResult GenerateMcpTests(
				const FJsonObject& Arguments,
				const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
			{
				bool bOverwrite = true;
				bool bDryRun = false;
				Arguments.TryGetBoolField(TEXT("overwrite"), bOverwrite);
				Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

				FString TestsDirectory;
				FString ScaffoldDirectory;
				FString ToolName;
				FString FailureReason;
				if (!ResolveMcpTestsDirectory(Arguments, TestsDirectory, ScaffoldDirectory, ToolName, FailureReason))
				{
					return MakeExecutionResult(FailureReason, nullptr, true);
				}

				FString SchemaJson;
				Arguments.TryGetStringField(TEXT("schemaJson"), SchemaJson);
				SchemaJson = SchemaJson.TrimStartAndEnd();

				TSharedPtr<FJsonObject> ExistingTestRequest;
				FString TestRequestToolName;
				FString TestRequestFailure;
				TryReadToolNameFromTestRequest(FPaths::Combine(ScaffoldDirectory, TEXT("TestRequest.json")), TestRequestToolName, ExistingTestRequest, TestRequestFailure);

				TSharedPtr<FJsonObject> ExampleArguments;
				if (ExistingTestRequest.IsValid())
				{
					const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
					if (ExistingTestRequest->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject && (*ParamsObject).IsValid())
					{
						const TSharedPtr<FJsonObject>* ArgumentsObject = nullptr;
						if ((*ParamsObject)->TryGetObjectField(TEXT("arguments"), ArgumentsObject) && ArgumentsObject && (*ArgumentsObject).IsValid())
						{
							ExampleArguments = *ArgumentsObject;
						}
					}
				}

				TSharedPtr<FJsonObject> InputSchema;
				FString SchemaSource = TEXT("none");
				if (!SchemaJson.IsEmpty())
				{
					if (!LoadJsonObject(SchemaJson, InputSchema) || !InputSchema.IsValid())
					{
						return MakeExecutionResult(TEXT("schemaJson is not valid JSON."), nullptr, true);
					}
					SchemaSource = TEXT("schemaJson");
				}

				if (!InputSchema.IsValid())
				{
					const TSharedPtr<FJsonObject> ToolObject = FindToolDefinitionByName(ToolsArray, ToolName);
					const TSharedPtr<FJsonObject>* ToolInputSchema = nullptr;
					if (ToolObject.IsValid()
						&& ToolObject->TryGetObjectField(TEXT("inputSchema"), ToolInputSchema)
						&& ToolInputSchema
						&& (*ToolInputSchema).IsValid())
					{
						InputSchema = *ToolInputSchema;
						SchemaSource = TEXT("loadedToolInputSchema");
					}
				}

				if (!InputSchema.IsValid())
				{
					if (ExtractRequestedSchemaFromScaffoldReadme(ScaffoldDirectory, SchemaJson)
						&& LoadJsonObject(SchemaJson, InputSchema)
						&& InputSchema.IsValid())
					{
						SchemaSource = TEXT("scaffoldReadmeRequestedSchema");
					}
				}

				if (!InputSchema.IsValid())
				{
					InputSchema = MakeObjectSchema();
					TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
					if (ExampleArguments.IsValid())
					{
						for (const FString& Key : GetSortedJsonObjectKeys(ExampleArguments))
						{
							TSharedPtr<FJsonObject> PropertySchema = MakeShared<FJsonObject>();
							const TSharedPtr<FJsonValue> Value = ExampleArguments->TryGetField(Key);
							if (Value.IsValid())
							{
								if (Value->Type == EJson::Number)
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("number"));
								}
								else if (Value->Type == EJson::Boolean)
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("boolean"));
								}
								else if (Value->Type == EJson::Array)
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("array"));
								}
								else if (Value->Type == EJson::Object)
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("object"));
								}
								else
								{
									PropertySchema->SetStringField(TEXT("type"), TEXT("string"));
								}
							}
							PropertiesObject->SetObjectField(Key, PropertySchema);
						}
					}
					InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);
					SchemaSource = TEXT("inferredFromTestRequest");
				}

				TSharedPtr<FJsonObject> HappyArguments = MakeSampleArgumentsForSchema(InputSchema, ExampleArguments, false);
				TSharedPtr<FJsonObject> BoundaryArguments = MakeSampleArgumentsForSchema(InputSchema, nullptr, true);
				TSharedPtr<FJsonObject> MissingArguments = CloneJsonObject(HappyArguments);
				TSharedPtr<FJsonObject> WrongTypeArguments = CloneJsonObject(HappyArguments);

				const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
				TArray<FString> PropertyKeys;
				if (InputSchema->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && (*PropertiesObject).IsValid())
				{
					PropertyKeys = GetSortedJsonObjectKeys(*PropertiesObject);
				}

				TArray<FString> RequiredFields;
				GetRequiredSchemaFields(InputSchema, RequiredFields);
				const bool bHasExplicitRequiredFields = RequiredFields.Num() > 0;
				const FString FieldToRemove = bHasExplicitRequiredFields
					? RequiredFields[0]
					: (PropertyKeys.Num() > 0 ? PropertyKeys[0] : FString());
				if (!FieldToRemove.IsEmpty())
				{
					MissingArguments->RemoveField(FieldToRemove);
				}

				FString WrongTypeField;
				if (PropertyKeys.Num() > 0)
				{
					WrongTypeField = PropertyKeys[0];
					const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
					if (PropertiesObject && (*PropertiesObject)->TryGetObjectField(WrongTypeField, PropertySchema) && PropertySchema && (*PropertySchema).IsValid())
					{
						WrongTypeArguments->SetField(WrongTypeField, MakeSampleJsonValueForSchema(*PropertySchema, false, true));
					}
				}
				else
				{
					WrongTypeArguments->SetStringField(TEXT("__wrong_type_probe"), TEXT("no declared properties"));
				}

				TArray<TSharedPtr<FJsonValue>> GeneratedFiles;
				TArray<TSharedPtr<FJsonValue>> TestCases;
				const bool bMissingShouldError = bHasExplicitRequiredFields;
				const bool bWrongTypeShouldError = PropertyKeys.Num() > 0;

				struct FGeneratedTestCase
				{
					FString FileName;
					TSharedPtr<FJsonObject> Object;
				};
				TArray<FGeneratedTestCase> Cases;
				Cases.Add({
					TEXT("valid_basic.json"),
					MakeMcpTestCaseObject(
						TEXT("valid_basic"),
						TEXT("Happy path generated from the tool schema or TestRequest.json."),
						TEXT("happy_path"),
						MakeToolCallRequestObject(ToolName, HappyArguments, 1),
						false,
						TEXT("Tool should execute without returning an MCP error."))
				});
				Cases.Add({
					TEXT("missing_required.json"),
					MakeMcpTestCaseObject(
						TEXT("missing_required"),
						FieldToRemove.IsEmpty()
							? TEXT("No schema properties were available to remove; executes the base request.")
							: FString::Printf(TEXT("Removes argument '%s' to exercise required-field handling."), *FieldToRemove),
						TEXT("missing_required"),
						MakeToolCallRequestObject(ToolName, MissingArguments, 2),
						bMissingShouldError,
						bHasExplicitRequiredFields ? TEXT("Schema declares required fields, so the tool is expected to reject the request.") : TEXT("Schema has no explicit required array; generated case is informational and may pass."))
				});
				Cases.Add({
					TEXT("boundary_values.json"),
					MakeMcpTestCaseObject(
						TEXT("boundary_values"),
						TEXT("Uses simple boundary values such as empty strings, zero numbers, false booleans, empty arrays, and empty objects."),
						TEXT("boundary"),
						MakeToolCallRequestObject(ToolName, BoundaryArguments, 3),
						false,
						TEXT("Boundary values should not produce an MCP error unless the tool adds stricter validation."))
				});
				Cases.Add({
					TEXT("wrong_type.json"),
					MakeMcpTestCaseObject(
						TEXT("wrong_type"),
						WrongTypeField.IsEmpty()
							? TEXT("No schema properties were available; adds a probe field with an unexpected shape.")
							: FString::Printf(TEXT("Sends a wrong JSON type for argument '%s'."), *WrongTypeField),
						TEXT("wrong_type"),
						MakeToolCallRequestObject(ToolName, WrongTypeArguments, 4),
						bWrongTypeShouldError,
						bWrongTypeShouldError ? TEXT("A robust tool should reject wrong JSON types.") : TEXT("No declared property exists to type-check; generated case is informational and may pass."))
				});

				for (const FGeneratedTestCase& TestCase : Cases)
				{
					const FString FilePath = FPaths::Combine(TestsDirectory, TestCase.FileName);
					if (!WriteMcpGeneratedTestFile(FilePath, TestCase.Object, bOverwrite, bDryRun, GeneratedFiles, FailureReason))
					{
						return MakeExecutionResult(FailureReason, nullptr, true);
					}
					TSharedPtr<FJsonObject> TestSummary = MakeShared<FJsonObject>();
					TestSummary->SetStringField(TEXT("name"), TestCase.Object->GetStringField(TEXT("name")));
					TestSummary->SetStringField(TEXT("path"), FilePath);
					TestSummary->SetBoolField(TEXT("expectToolCallError"), TestCase.Object->GetBoolField(TEXT("expectToolCallError")));
					TestCases.Add(MakeShared<FJsonValueObject>(TestSummary));
				}

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_generate_tests"));
				StructuredContent->SetStringField(TEXT("toolName"), ToolName);
				StructuredContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDirectory);
				StructuredContent->SetStringField(TEXT("testsDir"), TestsDirectory);
				StructuredContent->SetStringField(TEXT("schemaSource"), SchemaSource);
				StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
				StructuredContent->SetBoolField(TEXT("overwrite"), bOverwrite);
				StructuredContent->SetBoolField(TEXT("hasExplicitRequiredFields"), bHasExplicitRequiredFields);
				StructuredContent->SetArrayField(TEXT("requiredFields"), MakeJsonStringArray(RequiredFields));
				StructuredContent->SetArrayField(TEXT("propertyKeys"), MakeJsonStringArray(PropertyKeys));
				StructuredContent->SetArrayField(TEXT("files"), GeneratedFiles);
				StructuredContent->SetArrayField(TEXT("testCases"), TestCases);

				return MakeExecutionResult(
					FString::Printf(TEXT("%s %d MCP test cases for %s under %s."),
						bDryRun ? TEXT("Previewed") : TEXT("Generated"),
						Cases.Num(),
						*ToolName,
						*TestsDirectory),
					StructuredContent,
					false);
			}

			FUnrealMcpExecutionResult BuildEditor(const FJsonObject& Arguments)
			{
			FString Target = FString::Printf(TEXT("%sEditor"), FApp::GetProjectName());
			FString Platform = GetHostBuildPlatformName();
			FString Configuration = TEXT("Development");
			FString ExtraArgs;
			FString ToolName;
			FString TestRequestPath;
			FString TestsDir;
			FString ScaffoldDir;
			FString MemoryKey = TEXT("mcp.extension.build_test");
			bool bWriteProjectMemory = true;

			Arguments.TryGetStringField(TEXT("target"), Target);
			Arguments.TryGetStringField(TEXT("platform"), Platform);
			Arguments.TryGetStringField(TEXT("configuration"), Configuration);
			Arguments.TryGetStringField(TEXT("extraArgs"), ExtraArgs);
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
			Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
			Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
			Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);

			Target = Target.TrimStartAndEnd();
			Platform = Platform.TrimStartAndEnd();
			Configuration = Configuration.TrimStartAndEnd();
			MemoryKey = MemoryKey.TrimStartAndEnd();
			if (Target.IsEmpty() || Platform.IsEmpty() || Configuration.IsEmpty())
			{
				return MakeExecutionResult(TEXT("target, platform, and configuration must not be empty."), nullptr, true);
			}
			if (MemoryKey.IsEmpty())
			{
				MemoryKey = TEXT("mcp.extension.build_test");
			}

			const FString BuildScriptPath = GetUnrealBuildScriptPath();
			if (!FPaths::FileExists(BuildScriptPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Unreal Build script was not found: %s"), *BuildScriptPath), nullptr, true);
			}

			FString ProjectFilePath = FPaths::GetProjectFilePath();
			if (ProjectFilePath.IsEmpty())
			{
				ProjectFilePath = FPaths::Combine(FPaths::ProjectDir(), FString::Printf(TEXT("%s.uproject"), FApp::GetProjectName()));
			}
			ProjectFilePath = FPaths::ConvertRelativePathToFull(ProjectFilePath);

			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"));
			const FString BuildLogDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/BuildLogs")));
			IFileManager::Get().MakeDirectory(*BuildLogDirectory, true);
			const FString BuildLogPath = FPaths::Combine(BuildLogDirectory, FString::Printf(TEXT("Build_%s_%s_%s.log"), *Target, *Configuration, *Timestamp));

			if (TestRequestPath.TrimStartAndEnd().IsEmpty() && !ScaffoldDir.TrimStartAndEnd().IsEmpty())
			{
				FString ResolvedScaffoldDir;
				FString ResolveFailure;
				if (ResolveProjectPathInsideProject(ScaffoldDir, ResolvedScaffoldDir, ResolveFailure))
				{
					TestRequestPath = FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json"));
					if (TestsDir.TrimStartAndEnd().IsEmpty())
					{
						TestsDir = FPaths::Combine(ResolvedScaffoldDir, TEXT("Tests"));
					}
				}
			}

			TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
			MemoryContent->SetStringField(TEXT("target"), Target);
			MemoryContent->SetStringField(TEXT("platform"), Platform);
			MemoryContent->SetStringField(TEXT("configuration"), Configuration);
			MemoryContent->SetStringField(TEXT("toolName"), ToolName);
			MemoryContent->SetStringField(TEXT("testRequestPath"), TestRequestPath);
			MemoryContent->SetStringField(TEXT("testsDir"), TestsDir);
			MemoryContent->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
			MemoryContent->SetStringField(TEXT("buildLogPath"), BuildLogPath);
			MemoryContent->SetBoolField(TEXT("editorWasRunningDuringBuild"), true);
			MemoryContent->SetBoolField(TEXT("editorRestartRequiredBeforeTestingNewTools"), true);
			MemoryContent->SetStringField(TEXT("recommendation"), TEXT("Restart Unreal Editor after a successful plugin build before running unreal.mcp_run_tool_test for newly added tools."));

			if (bWriteProjectMemory)
			{
				WriteBuildTestMemory(
					MemoryKey,
					TEXT("Waiting for Unreal Editor restart before MCP tool testing."),
					TEXT("waiting_for_editor_restart_after_build"),
					TEXT("If build succeeds, restart Unreal Editor, then run unreal.mcp_run_tool_test with this memoryKey."),
					MemoryContent);
			}

			const FString Params = FString::Printf(
				TEXT("%s %s %s -Project=%s -WaitMutex%s%s"),
				*Target,
				*Platform,
				*Configuration,
				*QuoteCommandLineArgument(ProjectFilePath),
				ExtraArgs.TrimStartAndEnd().IsEmpty() ? TEXT("") : TEXT(" "),
				*ExtraArgs.TrimStartAndEnd());

			int32 ReturnCode = -1;
			FString StdOut;
			FString StdErr;
			const bool bLaunched = FPlatformProcess::ExecProcess(
				*BuildScriptPath,
				*Params,
				&ReturnCode,
				&StdOut,
				&StdErr,
				*FPaths::ConvertRelativePathToFull(FPaths::EngineDir()));

			const FString CombinedLog = StdOut + (StdErr.IsEmpty() ? FString() : FString::Printf(TEXT("\n\n[stderr]\n%s"), *StdErr));
			FFileHelper::SaveStringToFile(CombinedLog, *BuildLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_build_editor"));
			StructuredContent->SetStringField(TEXT("target"), Target);
			StructuredContent->SetStringField(TEXT("platform"), Platform);
			StructuredContent->SetStringField(TEXT("configuration"), Configuration);
			StructuredContent->SetStringField(TEXT("extraArgs"), ExtraArgs);
			StructuredContent->SetStringField(TEXT("projectFile"), ProjectFilePath);
			StructuredContent->SetStringField(TEXT("buildScript"), BuildScriptPath);
			StructuredContent->SetStringField(TEXT("buildLogPath"), BuildLogPath);
			StructuredContent->SetBoolField(TEXT("launched"), bLaunched);
			StructuredContent->SetBoolField(TEXT("editorRunningDuringBuild"), true);
			StructuredContent->SetBoolField(TEXT("editorRestartRequiredBeforeTestingNewTools"), true);
			StructuredContent->SetStringField(TEXT("restartAdvice"), TEXT("The editor is running because this tool is invoked from Chat. A successful plugin build still requires restarting Unreal Editor before new tool definitions are loaded."));

			ParseBuildLog(CombinedLog, bLaunched ? ReturnCode : -1, StructuredContent);
			const bool bSucceeded = StructuredContent->GetBoolField(TEXT("succeeded"));

			MemoryContent->SetBoolField(TEXT("buildSucceeded"), bSucceeded);
			MemoryContent->SetNumberField(TEXT("returnCode"), bLaunched ? ReturnCode : -1);
			if (bWriteProjectMemory)
			{
				WriteBuildTestMemory(
					MemoryKey,
					bSucceeded ? TEXT("Editor build succeeded; restart before MCP tool test.") : TEXT("Editor build failed; inspect parsed errors and build log."),
					bSucceeded ? TEXT("build_succeeded_restart_required") : TEXT("build_failed"),
					bSucceeded ? TEXT("Restart Unreal Editor, then run unreal.mcp_run_tool_test.") : TEXT("Fix compile errors, then rerun unreal.mcp_build_editor."),
					MemoryContent);
			}

			const FString Text = bSucceeded
				? FString::Printf(TEXT("Build succeeded for %s %s %s. Restart Unreal Editor before testing newly compiled MCP tools."), *Target, *Platform, *Configuration)
				: FString::Printf(TEXT("Build failed for %s %s %s. See parsed errors and log: %s"), *Target, *Platform, *Configuration, *BuildLogPath);
			return MakeExecutionResult(Text, StructuredContent, !bSucceeded);
		}

		FString GetMcpSupervisorScriptPath()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/unreal_mcp_supervisor.py")));
		}

		FString MakeSupervisorDefaultArgsJson(const FString& MemoryKey)
		{
			FString EscapedMemoryKey = MemoryKey;
			EscapedMemoryKey.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
			EscapedMemoryKey.ReplaceInline(TEXT("\""), TEXT("\\\""));
			return FString::Printf(TEXT("{\"memoryKey\":\"%s\"}"), *EscapedMemoryKey);
		}

		FUnrealMcpExecutionResult SupervisorInstall(const FJsonObject& Arguments)
		{
			FString Platform = TEXT("all");
			FString OutputDir = TEXT("Tools/UnrealMcpSupervisor");
			FString Label = FString::Printf(TEXT("com.unrealmcp.%s"), *SanitizeMcpToolIdForPath(FApp::GetProjectName()).ToLower());
			FString MemoryKey = TEXT("mcp.extension.pipeline");
			FString ArgsJson;
			FString EndpointUrl = TEXT("http://127.0.0.1:8765/mcp");
			FString SupervisorLogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UnrealMcp/SupervisorLogs"));
			FString EditorCmd;
			bool bInstallLaunchAgent = false;
			bool bLaunchAtLoad = false;
			bool bAutoRestart = true;
			bool bOverwrite = true;
			bool bDryRun = false;

			Arguments.TryGetStringField(TEXT("platform"), Platform);
			Arguments.TryGetStringField(TEXT("outputDir"), OutputDir);
			Arguments.TryGetStringField(TEXT("label"), Label);
			Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
			Arguments.TryGetStringField(TEXT("argsJson"), ArgsJson);
			Arguments.TryGetStringField(TEXT("endpointUrl"), EndpointUrl);
			Arguments.TryGetStringField(TEXT("supervisorLogDir"), SupervisorLogDir);
			Arguments.TryGetStringField(TEXT("editorCmd"), EditorCmd);
			Arguments.TryGetBoolField(TEXT("installLaunchAgent"), bInstallLaunchAgent);
			Arguments.TryGetBoolField(TEXT("launchAtLoad"), bLaunchAtLoad);
			Arguments.TryGetBoolField(TEXT("autoRestart"), bAutoRestart);
			Arguments.TryGetBoolField(TEXT("overwrite"), bOverwrite);
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);

			Platform = Platform.TrimStartAndEnd().ToLower();
			if (Platform.IsEmpty())
			{
				Platform = TEXT("all");
			}
			if (MemoryKey.TrimStartAndEnd().IsEmpty())
			{
				MemoryKey = TEXT("mcp.extension.pipeline");
			}
			if (ArgsJson.TrimStartAndEnd().IsEmpty())
			{
				ArgsJson = MakeSupervisorDefaultArgsJson(MemoryKey);
			}

			const FString SupervisorScriptPath = GetMcpSupervisorScriptPath();
			if (!FPaths::FileExists(SupervisorScriptPath))
			{
				return MakeExecutionResult(FString::Printf(TEXT("Supervisor script was not found: %s"), *SupervisorScriptPath), nullptr, true);
			}

			FString ResolvedOutputDir;
			FString FailureReason;
			if (!ResolveProjectPathInsideProject(OutputDir, ResolvedOutputDir, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}

			FString ProjectFilePath = FPaths::GetProjectFilePath();
			if (ProjectFilePath.IsEmpty())
			{
				ProjectFilePath = FPaths::Combine(FPaths::ProjectDir(), FString::Printf(TEXT("%s.uproject"), FApp::GetProjectName()));
			}
			ProjectFilePath = FPaths::ConvertRelativePathToFull(ProjectFilePath);

			TArray<FString> ParamParts;
			ParamParts.Add(QuoteCommandLineArgument(SupervisorScriptPath));
			ParamParts.Add(TEXT("--url"));
			ParamParts.Add(QuoteCommandLineArgument(EndpointUrl));
			ParamParts.Add(TEXT("--uproject"));
			ParamParts.Add(QuoteCommandLineArgument(ProjectFilePath));
			if (!EditorCmd.TrimStartAndEnd().IsEmpty())
			{
				ParamParts.Add(TEXT("--editor-cmd"));
				ParamParts.Add(QuoteCommandLineArgument(EditorCmd.TrimStartAndEnd()));
			}
			ParamParts.Add(TEXT("install"));
			ParamParts.Add(TEXT("--output-dir"));
			ParamParts.Add(QuoteCommandLineArgument(ResolvedOutputDir));
			ParamParts.Add(TEXT("--platform"));
			ParamParts.Add(QuoteCommandLineArgument(Platform));
			ParamParts.Add(TEXT("--label"));
			ParamParts.Add(QuoteCommandLineArgument(Label));
			ParamParts.Add(TEXT("--memory-key"));
			ParamParts.Add(QuoteCommandLineArgument(MemoryKey));
			ParamParts.Add(TEXT("--args-json"));
			ParamParts.Add(QuoteCommandLineArgument(ArgsJson));
			ParamParts.Add(TEXT("--supervisor-log-dir"));
			ParamParts.Add(QuoteCommandLineArgument(FPaths::ConvertRelativePathToFull(SupervisorLogDir)));
			if (bInstallLaunchAgent)
			{
				ParamParts.Add(TEXT("--install-launch-agent"));
			}
			if (bLaunchAtLoad)
			{
				ParamParts.Add(TEXT("--launch-at-load"));
			}
			if (!bAutoRestart)
			{
				ParamParts.Add(TEXT("--no-auto-restart"));
			}
			if (bOverwrite)
			{
				ParamParts.Add(TEXT("--overwrite"));
			}

#if PLATFORM_WINDOWS
			const FString PythonExecutable = TEXT("py");
			const FString Params = TEXT("-3 ") + FString::Join(ParamParts, TEXT(" "));
#else
			const FString PythonExecutable = TEXT("/usr/bin/env");
			const FString Params = TEXT("python3 ") + FString::Join(ParamParts, TEXT(" "));
#endif

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_supervisor_install"));
			StructuredContent->SetStringField(TEXT("platform"), Platform);
			StructuredContent->SetStringField(TEXT("outputDir"), ResolvedOutputDir);
			StructuredContent->SetStringField(TEXT("label"), Label);
			StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
			StructuredContent->SetStringField(TEXT("argsJson"), ArgsJson);
			StructuredContent->SetStringField(TEXT("endpointUrl"), EndpointUrl);
			StructuredContent->SetStringField(TEXT("supervisorLogDir"), FPaths::ConvertRelativePathToFull(SupervisorLogDir));
			StructuredContent->SetStringField(TEXT("supervisorScriptPath"), SupervisorScriptPath);
			StructuredContent->SetStringField(TEXT("executable"), PythonExecutable);
			StructuredContent->SetStringField(TEXT("params"), Params);
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("autoRestart"), bAutoRestart);
			StructuredContent->SetBoolField(TEXT("installLaunchAgent"), bInstallLaunchAgent);

			if (bDryRun)
			{
				return MakeExecutionResult(TEXT("Dry run supervisor install prepared the generator command."), StructuredContent, false);
			}

			int32 ReturnCode = -1;
			FString StdOut;
			FString StdErr;
			const bool bLaunched = FPlatformProcess::ExecProcess(
				*PythonExecutable,
				*Params,
				&ReturnCode,
				&StdOut,
				&StdErr,
				*FPaths::ProjectDir());

			StructuredContent->SetBoolField(TEXT("launched"), bLaunched);
			StructuredContent->SetNumberField(TEXT("returnCode"), ReturnCode);
			StructuredContent->SetStringField(TEXT("stdout"), StdOut);
			StructuredContent->SetStringField(TEXT("stderr"), StdErr);

			TSharedPtr<FJsonObject> InstallResult;
			if (LoadJsonObject(StdOut, InstallResult) && InstallResult.IsValid())
			{
				StructuredContent->SetObjectField(TEXT("installResult"), InstallResult);
			}

			const bool bSucceeded = bLaunched && ReturnCode == 0;
			return MakeExecutionResult(
				bSucceeded
					? FString::Printf(TEXT("Supervisor launcher files generated under %s."), *ResolvedOutputDir)
					: FString::Printf(TEXT("Supervisor install failed with returnCode=%d. stderr: %s"), ReturnCode, *StdErr),
				StructuredContent,
				!bSucceeded);
		}


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

class FUnrealMcpAssistantRun final
	: public IUnrealMcpAssistantHandle
	, public TSharedFromThis<FUnrealMcpAssistantRun, ESPMode::ThreadSafe>
{
public:
	FUnrealMcpAssistantRun(
		const FUnrealMcpModule* InModule,
		FString InUserPrompt,
		FString InConversationContext,
		FString InPreviousResponseId,
		TFunction<void(const FUnrealMcpAssistantEvent&)> InOnEvent,
		TFunction<void(const FUnrealMcpAssistantTurnResult&)> InOnComplete)
		: Module(InModule)
		, UserPrompt(MoveTemp(InUserPrompt))
		, ConversationContext(MoveTemp(InConversationContext))
		, PreviousResponseId(MoveTemp(InPreviousResponseId))
		, OnEvent(MoveTemp(InOnEvent))
		, OnComplete(MoveTemp(InOnComplete))
	{
	}

	void Start()
	{
		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		if (!Settings->bEnableAiAssistant)
		{
			Finish(TEXT("AI assistant is disabled. Enable it in Project Settings > Plugins > Unreal MCP > AI."), FString(), true);
			return;
		}

		if (Settings->OpenAIApiKey.TrimStartAndEnd().IsEmpty())
		{
			Finish(TEXT("OpenAI API key is empty. Set it in Project Settings > Plugins > Unreal MCP > AI."), FString(), true);
			return;
		}

		if (Settings->OpenAIModel.TrimStartAndEnd().IsEmpty())
		{
			Finish(TEXT("OpenAI model is empty. Set it in Project Settings > Plugins > Unreal MCP > AI."), FString(), true);
			return;
		}

		if (Settings->OpenAIResponsesUrl.TrimStartAndEnd().IsEmpty())
		{
			Finish(TEXT("OpenAI Responses URL is empty. Set it in Project Settings > Plugins > Unreal MCP > AI."), FString(), true);
			return;
		}

		BuildOpenAiTools();
		SendModelRequest(BuildInitialInput(), PreviousResponseId);
	}

	virtual void Cancel() override
	{
		TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> RequestToCancel;
		FString ResponseId;
		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted)
			{
				return;
			}

			bCancellationRequested = true;
			RequestToCancel = ActiveRequest;
			ResponseId = ActiveResponseId;
		}

		if (RequestToCancel.IsValid())
		{
			RequestToCancel->CancelRequest();
		}

		Finish(TEXT("Generation stopped."), ResponseId, false, true);
	}

private:
	struct FAssistantToolCall
	{
		FString OpenAiFunctionName;
		FString UnrealToolName;
		FString CallId;
		FString ArgumentsJson;
	};

	void EmitEvent(const FUnrealMcpAssistantEvent& Event) const
	{
		if (OnEvent)
		{
			const FUnrealMcpAssistantEvent EventCopy = Event;
			TFunction<void(const FUnrealMcpAssistantEvent&)> EventCallback = OnEvent;
			AsyncTask(ENamedThreads::GameThread, [EventCopy, EventCallback = MoveTemp(EventCallback)]() mutable
			{
				if (EventCallback)
				{
					EventCallback(EventCopy);
				}
			});
		}
	}

	void EmitStatus(const FString& Message, bool bIsError = false) const
	{
		FUnrealMcpAssistantEvent Event;
		Event.Type = EUnrealMcpAssistantEventType::Status;
		Event.Text = Message;
		Event.bIsError = bIsError;
		EmitEvent(Event);
	}

	void EmitTextDelta(const FString& Delta) const
	{
		if (Delta.IsEmpty())
		{
			return;
		}

		FUnrealMcpAssistantEvent Event;
		Event.Type = EUnrealMcpAssistantEventType::TextDelta;
		Event.Text = Delta;
		EmitEvent(Event);
	}

	void EmitToolStarted(const FAssistantToolCall& ToolCall) const
	{
		FUnrealMcpAssistantEvent Event;
		Event.Type = EUnrealMcpAssistantEventType::ToolCallStarted;
		Event.ToolName = ToolCall.UnrealToolName;
		Event.ToolCallId = ToolCall.CallId;
		Event.ToolArgumentsJson = ToolCall.ArgumentsJson;
		EmitEvent(Event);
	}

	void EmitToolFinished(const FAssistantToolCall& ToolCall, const FUnrealMcpExecutionResult& ToolResult) const
	{
		FUnrealMcpAssistantEvent Event;
		Event.Type = EUnrealMcpAssistantEventType::ToolCallFinished;
		Event.ToolName = ToolCall.UnrealToolName;
		Event.ToolCallId = ToolCall.CallId;
		Event.ToolArgumentsJson = ToolCall.ArgumentsJson;
		Event.Text = ToolResult.Text;
		Event.bIsError = ToolResult.bIsError;
		EmitEvent(Event);
	}

	void Finish(const FString& Message, const FString& ResponseId, bool bIsError, bool bWasCancelled = false)
	{
		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted)
			{
				return;
			}

			bCompleted = true;
			ActiveRequest.Reset();
		}

		if (OnComplete)
		{
			FUnrealMcpAssistantTurnResult Result;
			Result.Text = Message;
			Result.ResponseId = ResponseId;
			Result.bIsError = bIsError;
			Result.bWasCancelled = bWasCancelled;
			TFunction<void(const FUnrealMcpAssistantTurnResult&)> CompleteCallback = OnComplete;
			AsyncTask(ENamedThreads::GameThread, [Result = MoveTemp(Result), CompleteCallback = MoveTemp(CompleteCallback)]() mutable
			{
				if (CompleteCallback)
				{
					CompleteCallback(Result);
				}
			});
		}
	}

	void ResetPerRequestState()
	{
		RawResponseBytes.Reset();
		PendingStreamBytes.Reset();
		PendingSseData.Reset();
		CompletedResponseObject.Reset();
		StreamFailureMessage.Reset();
		StreamToolCalls.Reset();
		bResponseIncompleteDueToMaxOutputTokens = false;
	}

	static FString BytesToString(const TArray<uint8>& Bytes)
	{
		if (Bytes.IsEmpty())
		{
			return FString();
		}

		const FUTF8ToTCHAR Converter(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converter.Length(), Converter.Get());
	}

	void BuildOpenAiTools()
	{
		OpenAiTools.Reset();
		FunctionNameToToolName.Reset();

		TArray<TSharedPtr<FJsonValue>> MccTools;
		Module->AppendToolDefinitions(MccTools);

		TMap<FString, int32> SeenFunctionNames;

		for (const TSharedPtr<FJsonValue>& ToolValue : MccTools)
		{
			if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject> ToolObject = ToolValue->AsObject();
			FString OriginalToolName;
			FString Description;
			const TSharedPtr<FJsonObject>* InputSchema = nullptr;
			if (!ToolObject->TryGetStringField(TEXT("name"), OriginalToolName)
				|| !ToolObject->TryGetStringField(TEXT("description"), Description)
				|| !ToolObject->TryGetObjectField(TEXT("inputSchema"), InputSchema)
				|| !InputSchema
				|| !(*InputSchema).IsValid())
			{
				continue;
			}

			FString FunctionName = OriginalToolName;
			for (TCHAR& Character : FunctionName)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
				{
					Character = TEXT('_');
				}
			}

			if (FunctionName.IsEmpty())
			{
				FunctionName = TEXT("tool");
			}

			if (FunctionName.Len() > 64)
			{
				FunctionName.LeftInline(64, EAllowShrinking::No);
			}

			const int32 DuplicateCount = SeenFunctionNames.FindRef(FunctionName);
			SeenFunctionNames.FindOrAdd(FunctionName) = DuplicateCount + 1;
			if (DuplicateCount > 0)
			{
				const FString Suffix = FString::Printf(TEXT("_%d"), DuplicateCount + 1);
				const int32 MaxBaseLength = FMath::Max(1, 64 - Suffix.Len());
				FunctionName = FunctionName.Left(MaxBaseLength) + Suffix;
			}

				TSharedPtr<FJsonObject> OpenAiTool = MakeShared<FJsonObject>();
				OpenAiTool->SetStringField(TEXT("type"), TEXT("function"));
				OpenAiTool->SetStringField(TEXT("name"), FunctionName);
				OpenAiTool->SetStringField(
					TEXT("description"),
					FString::Printf(TEXT("%s Original MCP tool name: %s."), *Description, *OriginalToolName));

				const TSharedPtr<FJsonObject> NormalizedSchema = UnrealMcp::NormalizeOpenAiSchemaObject(*InputSchema);
				FString SchemaCompatibilityReason;
				if (!UnrealMcp::IsOpenAiSchemaCompatibleObject(NormalizedSchema, SchemaCompatibilityReason))
				{
					UE_LOG(
						LogUnrealMcp,
						Display,
						TEXT("Skipping AI tool '%s' because its schema is not compatible with the OpenAI function interface: %s"),
						*OriginalToolName,
						*SchemaCompatibilityReason);
					continue;
				}

				OpenAiTool->SetObjectField(TEXT("parameters"), NormalizedSchema);

				OpenAiTools.Add(MakeShared<FJsonValueObject>(OpenAiTool));
				FunctionNameToToolName.Add(FunctionName, OriginalToolName);
			}
	}

	FString BuildAssistantInstructions() const
	{
		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();

		FString Instructions =
			TEXT("You are Unreal MCP AI running inside Unreal Editor. ")
			TEXT("Help the user build, inspect, and modify the current Unreal project by using the provided function tools when they are helpful. ")
			TEXT("Prefer the smallest safe set of tool calls. ")
			TEXT("For read-only questions, inspect first before concluding. ")
			TEXT("For modifications, act directly when the user clearly asked for a change. ")
			TEXT("Avoid destructive actions such as deleting actors unless the user explicitly asked for that result. ")
			TEXT("Prefer AI-safe wrapper tools such as spawn_actor_basic, spawn_actor_batch_basic, spawn_static_mesh_actor, batch_set_actor_scale, batch_set_actor_tags, batch_set_point_light_properties, batch_configure_static_mesh_actors, bp_* Blueprint graph editing tools, widget_* UMG editing tools, scaffold_* gameplay scaffold tools, and scaffold_mcp_tool for MCP extension scaffolding before falling back to execute_python. ")
			TEXT("Keep answers compact by default and avoid repeating the user's prompt. ")
			TEXT("When a task is blocked because no suitable tool exists, say so plainly and suggest the closest supported path. ")
			TEXT("After tool use, give a concise final answer focused on what you changed or found.");

		if (!Settings->AssistantSystemPrompt.TrimStartAndEnd().IsEmpty())
		{
			Instructions += TEXT("\n\nAdditional instructions:\n");
			Instructions += Settings->AssistantSystemPrompt.TrimStartAndEnd();
		}

		return Instructions;
	}

	TSharedPtr<FJsonValueObject> BuildInputMessage(const FString& Text) const
	{
		TSharedPtr<FJsonObject> TextObject = MakeShared<FJsonObject>();
		TextObject->SetStringField(TEXT("type"), TEXT("input_text"));
		TextObject->SetStringField(TEXT("text"), Text);

		TArray<TSharedPtr<FJsonValue>> ContentArray;
		ContentArray.Add(MakeShared<FJsonValueObject>(TextObject));

		TSharedPtr<FJsonObject> MessageObject = MakeShared<FJsonObject>();
		MessageObject->SetStringField(TEXT("role"), TEXT("user"));
		MessageObject->SetArrayField(TEXT("content"), ContentArray);

		return MakeShared<FJsonValueObject>(MessageObject);
	}

	TArray<TSharedPtr<FJsonValue>> BuildUserInput(const FString& Text) const
	{
		TArray<TSharedPtr<FJsonValue>> InputArray;
		InputArray.Add(BuildInputMessage(Text));
		return InputArray;
	}

	TArray<TSharedPtr<FJsonValue>> BuildInitialInput() const
	{
		TArray<TSharedPtr<FJsonValue>> InputArray;
		if (!ConversationContext.TrimStartAndEnd().IsEmpty())
		{
			InputArray.Add(BuildInputMessage(ConversationContext));
		}
		InputArray.Add(BuildInputMessage(UserPrompt));
		return InputArray;
	}

	TArray<TSharedPtr<FJsonValue>> BuildContinuationInput() const
	{
		return BuildUserInput(
			TEXT("Continue from exactly where you left off. ")
			TEXT("Do not repeat prior completed text unless a very short bridge is needed. ")
			TEXT("Keep the answer concise and finish the response. ")
			TEXT("If more tool use is required, continue using tools."));
	}

	void SendModelRequest(const TArray<TSharedPtr<FJsonValue>>& InputItems, const FString& PriorResponseId)
	{
		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted || bCancellationRequested)
			{
				return;
			}

			ResetPerRequestState();
		}

		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		UnrealMcp::ApplyAiHttpTimeoutOverrides(*Settings);

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("model"), Settings->OpenAIModel);
		Payload->SetStringField(TEXT("instructions"), BuildAssistantInstructions());
		Payload->SetArrayField(TEXT("input"), InputItems);
		Payload->SetArrayField(TEXT("tools"), OpenAiTools);
		Payload->SetStringField(TEXT("tool_choice"), TEXT("auto"));
		Payload->SetBoolField(TEXT("parallel_tool_calls"), true);
		Payload->SetBoolField(TEXT("stream"), true);
		Payload->SetStringField(TEXT("truncation"), TEXT("auto"));
		Payload->SetNumberField(TEXT("max_output_tokens"), Settings->AiMaxOutputTokens);

		const FString ReasoningEffort = Settings->OpenAIReasoningEffort.TrimStartAndEnd();
		if (!ReasoningEffort.IsEmpty())
		{
			TSharedPtr<FJsonObject> ReasoningObject = MakeShared<FJsonObject>();
			ReasoningObject->SetStringField(TEXT("effort"), ReasoningEffort);
			Payload->SetObjectField(TEXT("reasoning"), ReasoningObject);
		}

		if (!PriorResponseId.TrimStartAndEnd().IsEmpty())
		{
			Payload->SetStringField(TEXT("previous_response_id"), PriorResponseId);
		}

		FString PayloadString;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PayloadString);
		FJsonSerializer::Serialize(Payload.ToSharedRef(), Writer);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Settings->OpenAIResponsesUrl);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("Accept"), TEXT("text/event-stream"));
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->OpenAIApiKey));
		Request->SetTimeout(Settings->AiRequestTimeoutSeconds);
		Request->SetActivityTimeout(Settings->AiRequestActivityTimeoutSeconds);
		Request->SetContentAsString(PayloadString);

		FHttpRequestStreamDelegateV2 StreamDelegate;
		TWeakPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> WeakRun = AsShared();
		StreamDelegate.BindLambda(
			[WeakRun](void* Ptr, int64& InOutLength)
			{
				if (const TSharedPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> PinnedThis = WeakRun.Pin())
				{
					PinnedThis->ConsumeResponseBytes(Ptr, InOutLength);
				}
			});
		Request->SetResponseBodyReceiveStreamDelegateV2(StreamDelegate);

		Request->OnProcessRequestComplete().BindLambda(
			[WeakRun](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
			{
				if (const TSharedPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> PinnedThis = WeakRun.Pin())
				{
					PinnedThis->HandleModelRequestFinished(HttpRequest, HttpResponse, bSucceeded);
				}
			});

		{
			const FScopeLock Lock(&StateMutex);
			ActiveRequest = Request;
		}

		if (!Request->ProcessRequest())
		{
			Finish(TEXT("Failed to start the HTTP request to the AI provider."), FString(), true, false);
		}
	}

	void ConsumeResponseBytes(const void* Ptr, int64 Length)
	{
		if (Length <= 0 || Ptr == nullptr)
		{
			return;
		}

		TArray<FString> CompletedEvents;
		{
			const FScopeLock Lock(&StateMutex);
			RawResponseBytes.Append(static_cast<const uint8*>(Ptr), Length);
			PendingStreamBytes.Append(static_cast<const uint8*>(Ptr), Length);

			int32 LineStart = 0;
			for (int32 Index = 0; Index < PendingStreamBytes.Num(); ++Index)
			{
				if (PendingStreamBytes[Index] != '\n')
				{
					continue;
				}

				int32 LineLength = Index - LineStart;
				if (LineLength > 0 && PendingStreamBytes[LineStart + LineLength - 1] == '\r')
				{
					--LineLength;
				}

				FString Line;
				if (LineLength > 0)
				{
					const FUTF8ToTCHAR Converter(
						reinterpret_cast<const UTF8CHAR*>(PendingStreamBytes.GetData() + LineStart),
						LineLength);
					Line = FString(Converter.Length(), Converter.Get());
				}

				if (Line.IsEmpty())
				{
					if (!PendingSseData.IsEmpty())
					{
						CompletedEvents.Add(PendingSseData);
						PendingSseData.Reset();
					}
				}
				else if (Line.StartsWith(TEXT("data:"), ESearchCase::CaseSensitive))
				{
					FString DataLine = Line.Mid(5);
					DataLine.TrimStartInline();
					if (!PendingSseData.IsEmpty())
					{
						PendingSseData += TEXT("\n");
					}
					PendingSseData += DataLine;
				}

				LineStart = Index + 1;
			}

			if (LineStart > 0)
			{
				PendingStreamBytes.RemoveAt(0, LineStart, EAllowShrinking::No);
			}
		}

		for (const FString& EventData : CompletedEvents)
		{
			if (EventData.Equals(TEXT("[DONE]"), ESearchCase::CaseSensitive))
			{
				continue;
			}

			TSharedPtr<FJsonObject> EventObject;
			if (!UnrealMcp::LoadJsonObject(EventData, EventObject) || !EventObject.IsValid())
			{
				continue;
			}

			HandleStreamEvent(EventObject);
		}
	}

	void HandleStreamEvent(const TSharedPtr<FJsonObject>& EventObject)
	{
		if (!EventObject.IsValid())
		{
			return;
		}

		FString EventType;
		EventObject->TryGetStringField(TEXT("type"), EventType);
		if (EventType.IsEmpty())
		{
			return;
		}

		if (EventType == TEXT("response.created"))
		{
			const TSharedPtr<FJsonObject>* ResponseObject = nullptr;
			if (EventObject->TryGetObjectField(TEXT("response"), ResponseObject) && ResponseObject && (*ResponseObject).IsValid())
			{
				FString ResponseId;
				if ((*ResponseObject)->TryGetStringField(TEXT("id"), ResponseId))
				{
					const FScopeLock Lock(&StateMutex);
					ActiveResponseId = ResponseId;
				}
			}

			return;
		}

		if (EventType == TEXT("response.output_text.delta"))
		{
			FString Delta;
			if (EventObject->TryGetStringField(TEXT("delta"), Delta) && !Delta.IsEmpty())
			{
				{
					const FScopeLock Lock(&StateMutex);
					AccumulatedAssistantText += Delta;
				}
				EmitTextDelta(Delta);
			}

			return;
		}

		if (EventType == TEXT("response.function_call_arguments.done"))
		{
			FAssistantToolCall ToolCall;
			if (!EventObject->TryGetStringField(TEXT("name"), ToolCall.OpenAiFunctionName)
				|| !EventObject->TryGetStringField(TEXT("call_id"), ToolCall.CallId))
			{
				return;
			}

			const FString* ToolName = FunctionNameToToolName.Find(ToolCall.OpenAiFunctionName);
			if (!ToolName)
			{
				const FScopeLock Lock(&StateMutex);
				StreamFailureMessage = FString::Printf(TEXT("AI called unknown tool alias `%s`."), *ToolCall.OpenAiFunctionName);
				return;
			}

			ToolCall.UnrealToolName = *ToolName;
			EventObject->TryGetStringField(TEXT("arguments"), ToolCall.ArgumentsJson);

			bool bShouldEmit = false;
			{
				const FScopeLock Lock(&StateMutex);
				bShouldEmit = !StreamToolCalls.Contains(ToolCall.CallId);
				StreamToolCalls.Add(ToolCall.CallId, ToolCall);
			}

			if (bShouldEmit)
			{
				EmitToolStarted(ToolCall);
			}

			return;
		}

		if (EventType == TEXT("response.completed"))
		{
			const TSharedPtr<FJsonObject>* ResponseObject = nullptr;
			if (EventObject->TryGetObjectField(TEXT("response"), ResponseObject) && ResponseObject && (*ResponseObject).IsValid())
			{
				const FScopeLock Lock(&StateMutex);
				CompletedResponseObject = MakeShared<FJsonObject>(**ResponseObject);
				(*ResponseObject)->TryGetStringField(TEXT("id"), ActiveResponseId);
			}

			return;
		}

		if (EventType == TEXT("error"))
		{
			FString ErrorMessage;
			if (!EventObject->TryGetStringField(TEXT("message"), ErrorMessage))
			{
				const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
				if (EventObject->TryGetObjectField(TEXT("error"), ErrorObject) && ErrorObject && (*ErrorObject).IsValid())
				{
					(*ErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage);
				}
			}

			const FScopeLock Lock(&StateMutex);
			StreamFailureMessage = ErrorMessage.IsEmpty() ? TEXT("The AI provider returned a streaming error.") : ErrorMessage;
			return;
		}

		if (EventType == TEXT("response.failed") || EventType == TEXT("response.incomplete"))
		{
			FString ErrorMessage;
			bool bTreatAsSoftIncomplete = false;
			const TSharedPtr<FJsonObject>* ResponseObject = nullptr;
			if (EventObject->TryGetObjectField(TEXT("response"), ResponseObject) && ResponseObject && (*ResponseObject).IsValid())
			{
				FString StatusDetails;
				(*ResponseObject)->TryGetStringField(TEXT("status"), StatusDetails);
				const FString FailureDetails = UnrealMcp::ExtractOpenAiResponseFailureDetails(*ResponseObject);
				const FString IncompleteReason = UnrealMcp::GetJsonStringAtPath(*ResponseObject, { TEXT("incomplete_details"), TEXT("reason") });
				bTreatAsSoftIncomplete =
					EventType == TEXT("response.incomplete")
					&& (IncompleteReason.Equals(TEXT("max_output_tokens"), ESearchCase::IgnoreCase)
						|| FailureDetails.Equals(TEXT("max_output_tokens"), ESearchCase::IgnoreCase));

				{
					const FScopeLock Lock(&StateMutex);
					CompletedResponseObject = MakeShared<FJsonObject>(**ResponseObject);
					(*ResponseObject)->TryGetStringField(TEXT("id"), ActiveResponseId);
					bResponseIncompleteDueToMaxOutputTokens = bTreatAsSoftIncomplete;
				}

				if (!bTreatAsSoftIncomplete)
				{
					if (!StatusDetails.IsEmpty() && !FailureDetails.IsEmpty())
					{
						ErrorMessage = FString::Printf(TEXT("The AI response ended with status `%s`: %s"), *StatusDetails, *FailureDetails);
					}
					else if (!StatusDetails.IsEmpty())
					{
						ErrorMessage = FString::Printf(TEXT("The AI response ended with status `%s`."), *StatusDetails);
					}
					else if (!FailureDetails.IsEmpty())
					{
						ErrorMessage = FailureDetails;
					}
				}
			}

			if (!bTreatAsSoftIncomplete)
			{
				const FScopeLock Lock(&StateMutex);
				StreamFailureMessage = ErrorMessage.IsEmpty() ? TEXT("The AI response ended before completion.") : ErrorMessage;
			}

			return;
		}
	}

	void HandleModelRequestFinished(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
	{
		TSharedPtr<FJsonObject> CompletedObjectCopy;
		FString StreamFailureMessageCopy;
		FString ResponseIdCopy;
		FString BodyString;

		{
			const FScopeLock Lock(&StateMutex);
			if (bCompleted)
			{
				return;
			}

			if (ActiveRequest == HttpRequest)
			{
				ActiveRequest.Reset();
			}

			CompletedObjectCopy = CompletedResponseObject;
			StreamFailureMessageCopy = StreamFailureMessage;
			ResponseIdCopy = ActiveResponseId;
			BodyString = BytesToString(RawResponseBytes);
		}

		if (CompletedObjectCopy.IsValid())
		{
			ProcessCompletedResponseObject(CompletedObjectCopy, BodyString, HttpResponse.IsValid() ? HttpResponse->GetResponseCode() : 0);
			return;
		}

		if (bCancellationRequested)
		{
			Finish(TEXT("Generation stopped."), ResponseIdCopy, false, true);
			return;
		}

		if (!bSucceeded || !HttpResponse.IsValid())
		{
			FString TransportFailureMessage;
			if (HttpRequest.IsValid())
			{
				const EHttpRequestStatus::Type RequestStatus = HttpRequest->GetStatus();
				const EHttpFailureReason FailureReason = HttpRequest->GetFailureReason();
				if (FailureReason == EHttpFailureReason::TimedOut)
				{
					TransportFailureMessage = FString::Printf(
						TEXT("AI request timed out after %.0f seconds. Increase AI Request Timeout Seconds in Project Settings > Plugins > Unreal MCP > AI if you expect long planning turns."),
						GetDefault<UUnrealMcpSettings>()->AiRequestTimeoutSeconds);
				}
				else if (FailureReason == EHttpFailureReason::Cancelled)
				{
					TransportFailureMessage = TEXT("The AI request was cancelled before completion.");
				}
				else if (FailureReason == EHttpFailureReason::ConnectionError)
				{
					TransportFailureMessage = TEXT("The AI request failed because the connection to the AI provider could not be completed.");
				}
				else if (RequestStatus == EHttpRequestStatus::Failed)
				{
					TransportFailureMessage = FString::Printf(
						TEXT("The AI request failed before a valid HTTP response was returned. Request status: %s. Failure reason: %s."),
						EHttpRequestStatus::ToString(RequestStatus),
						LexToString(FailureReason));
				}
			}

			if (!StreamFailureMessageCopy.IsEmpty())
			{
				Finish(StreamFailureMessageCopy, ResponseIdCopy, true, false);
			}
			else if (!TransportFailureMessage.IsEmpty())
			{
				Finish(TransportFailureMessage, ResponseIdCopy, true, false);
			}
			else
			{
				Finish(TEXT("The AI request failed before a valid HTTP response was returned."), ResponseIdCopy, true, false);
			}
			return;
		}

		const int32 ResponseCode = HttpResponse->GetResponseCode();
		if (ResponseCode < 200 || ResponseCode >= 300)
		{
			FString ErrorMessage = BodyString;
			TSharedPtr<FJsonObject> ErrorObject;
			if (UnrealMcp::LoadJsonObject(BodyString, ErrorObject) && ErrorObject.IsValid())
			{
				const TSharedPtr<FJsonObject>* NestedErrorObject = nullptr;
				if (ErrorObject->TryGetObjectField(TEXT("error"), NestedErrorObject) && NestedErrorObject && (*NestedErrorObject).IsValid())
				{
					(*NestedErrorObject)->TryGetStringField(TEXT("message"), ErrorMessage);
				}
			}

			if (ErrorMessage.IsEmpty())
			{
				ErrorMessage = StreamFailureMessageCopy;
			}

			Finish(FString::Printf(TEXT("AI request failed. HTTP %d: %s"), ResponseCode, *ErrorMessage), ResponseIdCopy, true, false);
			return;
		}

		if (!StreamFailureMessageCopy.IsEmpty())
		{
			Finish(StreamFailureMessageCopy, ResponseIdCopy, true, false);
			return;
		}

		TSharedPtr<FJsonObject> ResponseObject;
		if (!BodyString.IsEmpty() && UnrealMcp::LoadJsonObject(BodyString, ResponseObject) && ResponseObject.IsValid())
		{
			ProcessCompletedResponseObject(ResponseObject, BodyString, ResponseCode);
			return;
		}

		Finish(TEXT("The AI stream ended without a completed response object."), ResponseIdCopy, true, false);
	}

	void ProcessCompletedResponseObject(const TSharedPtr<FJsonObject>& ResponseObject, const FString& FallbackBody, int32 ResponseCode)
	{
		if (!ResponseObject.IsValid())
		{
			Finish(FString::Printf(TEXT("Failed to parse the AI response body. HTTP %d"), ResponseCode), FString(), true, false);
			return;
		}

		FString ResponseId;
		ResponseObject->TryGetStringField(TEXT("id"), ResponseId);
		if (!ResponseId.IsEmpty())
		{
			const FScopeLock Lock(&StateMutex);
			ActiveResponseId = ResponseId;
		}

		FString StreamFailureMessageCopy;
		bool bResponseIncompleteDueToMaxTokensCopy = false;
		{
			const FScopeLock Lock(&StateMutex);
			StreamFailureMessageCopy = StreamFailureMessage;
			bResponseIncompleteDueToMaxTokensCopy = bResponseIncompleteDueToMaxOutputTokens;
		}

		if (!StreamFailureMessageCopy.IsEmpty())
		{
			Finish(StreamFailureMessageCopy, ResponseId, true, false);
			return;
		}

		TArray<FAssistantToolCall> ExtractedToolCalls;
		FString FinalText;
		FString ParseFailureReason;
		if (!ExtractAssistantOutput(*ResponseObject, ExtractedToolCalls, FinalText, ParseFailureReason))
		{
			Finish(ParseFailureReason, ResponseId, true, false);
			return;
		}

		TArray<FAssistantToolCall> ToolCalls;
		{
			const FScopeLock Lock(&StateMutex);
			for (const FAssistantToolCall& ToolCall : ExtractedToolCalls)
			{
				const bool bAlreadyKnown = StreamToolCalls.Contains(ToolCall.CallId);
				StreamToolCalls.Add(ToolCall.CallId, ToolCall);
				if (!bAlreadyKnown)
				{
					EmitToolStarted(ToolCall);
				}
			}

			StreamToolCalls.GenerateValueArray(ToolCalls);

			if (AccumulatedAssistantText.IsEmpty() && !FinalText.IsEmpty())
			{
				AccumulatedAssistantText = FinalText;
				EmitTextDelta(FinalText);
			}
		}

		if (ToolCalls.Num() == 0)
		{
			FString FinalMessage;
			{
				const FScopeLock Lock(&StateMutex);
				FinalMessage = AccumulatedAssistantText;
			}

			if (FinalMessage.TrimStartAndEnd().IsEmpty())
			{
				FinalMessage = FinalText;
			}

			if (FinalMessage.TrimStartAndEnd().IsEmpty())
			{
				FinalMessage = FallbackBody;
			}

			if (bResponseIncompleteDueToMaxTokensCopy)
			{
				if (AutoContinuationCount < MaxAutoContinuationCount)
				{
					++AutoContinuationCount;
					EmitStatus(
						FString::Printf(
							TEXT("AI hit the output token limit and is continuing automatically (%d/%d)."),
							AutoContinuationCount,
							MaxAutoContinuationCount));
					SendModelRequest(BuildContinuationInput(), ResponseId);
					return;
				}

				if (!FinalMessage.TrimStartAndEnd().IsEmpty())
				{
					FinalMessage += TEXT("\n\n[The response was truncated after reaching the automatic continuation limit.]");
					Finish(FinalMessage, ResponseId, false, false);
					return;
				}
			}

			if (FinalMessage.TrimStartAndEnd().IsEmpty())
			{
				Finish(TEXT("The AI response completed without text or tool calls."), ResponseId, true, false);
				return;
			}

			Finish(FinalMessage, ResponseId, false, false);
			return;
		}

		++ToolRoundCount;
		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		if (ToolRoundCount > Settings->AiMaxToolRounds)
		{
			Finish(
				FString::Printf(TEXT("Stopped after %d tool rounds to avoid an infinite loop."), Settings->AiMaxToolRounds),
				ResponseId,
				true,
				false);
			return;
		}

		TArray<TSharedPtr<FJsonValue>> ToolOutputs;
		for (const FAssistantToolCall& ToolCall : ToolCalls)
		{
			if (bCancellationRequested)
			{
				Finish(TEXT("Generation stopped."), ResponseId, false, true);
				return;
			}

			TSharedPtr<FJsonObject> ArgumentsObject = UnrealMcp::MakeEmptyObject();
			FUnrealMcpExecutionResult ToolResult;
			if (!ToolCall.ArgumentsJson.TrimStartAndEnd().IsEmpty() && !UnrealMcp::LoadJsonObject(ToolCall.ArgumentsJson, ArgumentsObject))
			{
				ToolResult = UnrealMcp::MakeExecutionResult(
					FString::Printf(TEXT("AI returned invalid JSON arguments for tool `%s`."), *ToolCall.UnrealToolName),
					nullptr,
					true);
			}
			else
			{
				ToolResult = Module->ExecuteTool(ToolCall.UnrealToolName, *ArgumentsObject);
			}

			EmitToolFinished(ToolCall, ToolResult);

			TSharedPtr<FJsonObject> ToolOutputObject = MakeShared<FJsonObject>();
			ToolOutputObject->SetStringField(TEXT("type"), TEXT("function_call_output"));
			ToolOutputObject->SetStringField(TEXT("call_id"), ToolCall.CallId);
			ToolOutputObject->SetStringField(TEXT("output"), SerializeToolResult(ToolResult));
			ToolOutputs.Add(MakeShared<FJsonValueObject>(ToolOutputObject));
		}

		EmitStatus(TEXT("AI is incorporating the tool results."));
		SendModelRequest(ToolOutputs, ResponseId);
	}

	bool ExtractAssistantOutput(
		const FJsonObject& ResponseObject,
		TArray<FAssistantToolCall>& OutToolCalls,
		FString& OutFinalText,
		FString& OutFailureReason) const
	{
		OutToolCalls.Reset();
		OutFinalText.Reset();
		OutFailureReason.Reset();

		const TArray<TSharedPtr<FJsonValue>>* OutputArray = nullptr;
		if (!ResponseObject.TryGetArrayField(TEXT("output"), OutputArray) || !OutputArray)
		{
			OutFailureReason = TEXT("AI response did not contain an output array.");
			return false;
		}

		TArray<FString> TextParts;

		for (const TSharedPtr<FJsonValue>& OutputValue : *OutputArray)
		{
			if (!OutputValue.IsValid() || OutputValue->Type != EJson::Object || !OutputValue->AsObject().IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject> OutputObject = OutputValue->AsObject();
			FString ItemType;
			OutputObject->TryGetStringField(TEXT("type"), ItemType);

			if (ItemType == TEXT("function_call"))
			{
				FAssistantToolCall ToolCall;
				if (!OutputObject->TryGetStringField(TEXT("name"), ToolCall.OpenAiFunctionName)
					|| !OutputObject->TryGetStringField(TEXT("call_id"), ToolCall.CallId))
				{
					OutFailureReason = TEXT("AI returned a function call without a name or call_id.");
					return false;
				}

				const FString* ToolName = FunctionNameToToolName.Find(ToolCall.OpenAiFunctionName);
				if (!ToolName)
				{
					OutFailureReason = FString::Printf(TEXT("AI called unknown tool alias `%s`."), *ToolCall.OpenAiFunctionName);
					return false;
				}

				ToolCall.UnrealToolName = *ToolName;
				OutputObject->TryGetStringField(TEXT("arguments"), ToolCall.ArgumentsJson);
				OutToolCalls.Add(MoveTemp(ToolCall));
				continue;
			}

			if (ItemType == TEXT("message"))
			{
				const TArray<TSharedPtr<FJsonValue>>* ContentArray = nullptr;
				if (!OutputObject->TryGetArrayField(TEXT("content"), ContentArray) || !ContentArray)
				{
					continue;
				}

				for (const TSharedPtr<FJsonValue>& ContentValue : *ContentArray)
				{
					if (!ContentValue.IsValid() || ContentValue->Type != EJson::Object || !ContentValue->AsObject().IsValid())
					{
						continue;
					}

					const TSharedPtr<FJsonObject> ContentObject = ContentValue->AsObject();
					FString ContentType;
					if (ContentObject->TryGetStringField(TEXT("type"), ContentType) && ContentType == TEXT("output_text"))
					{
						FString Text;
						if (ContentObject->TryGetStringField(TEXT("text"), Text) && !Text.TrimStartAndEnd().IsEmpty())
						{
							TextParts.Add(Text.TrimStartAndEnd());
						}
					}
				}
			}
		}

		if (TextParts.Num() > 0)
		{
			OutFinalText = FString::Join(TextParts, TEXT("\n\n"));
		}

		return true;
	}

	FString SerializeToolResult(const FUnrealMcpExecutionResult& ToolResult) const
	{
		TSharedPtr<FJsonObject> OutputObject = MakeShared<FJsonObject>();
		OutputObject->SetStringField(TEXT("text"), ToolResult.Text);
		OutputObject->SetBoolField(TEXT("is_error"), ToolResult.bIsError);
		if (ToolResult.StructuredContent.IsValid())
		{
			OutputObject->SetObjectField(TEXT("structured_content"), ToolResult.StructuredContent);
		}

		return UnrealMcp::JsonObjectToString(OutputObject);
	}

	const FUnrealMcpModule* Module = nullptr;
	FString UserPrompt;
	FString ConversationContext;
	FString PreviousResponseId;
	TFunction<void(const FUnrealMcpAssistantEvent&)> OnEvent;
	TFunction<void(const FUnrealMcpAssistantTurnResult&)> OnComplete;
	TArray<TSharedPtr<FJsonValue>> OpenAiTools;
	TMap<FString, FString> FunctionNameToToolName;
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
	FCriticalSection StateMutex;
	TArray<uint8> RawResponseBytes;
	TArray<uint8> PendingStreamBytes;
	FString PendingSseData;
	TSharedPtr<FJsonObject> CompletedResponseObject;
	FString StreamFailureMessage;
	TMap<FString, FAssistantToolCall> StreamToolCalls;
	FString ActiveResponseId;
	FString AccumulatedAssistantText;
	int32 ToolRoundCount = 0;
	int32 AutoContinuationCount = 0;
	static constexpr int32 MaxAutoContinuationCount = 2;
	bool bResponseIncompleteDueToMaxOutputTokens = false;
	bool bCompleted = false;
	bool bCancellationRequested = false;
};

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
		FPlatformProcess::SleepNoStats(static_cast<float>(UnrealMcp::GameThreadTimeoutSeconds));
		if (CompleteOnce(MakeJsonRpcError(
			IdValueCopy,
			-32001,
			FString::Printf(TEXT("Timed out waiting %.1f seconds for the Unreal Editor game thread."), UnrealMcp::GameThreadTimeoutSeconds),
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

	FString NegotiatedProtocolVersion = UnrealMcp::LatestProtocolVersion;
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
				TEXT("Connected to Unreal Editor. This server can inspect assets, drive PIE sessions, tail logs, run console commands, batch-edit actor properties, lay out and spawn actors in bulk (grid and circle), execute Python commands or script files, open maps and assets, create/compile blueprints, edit Blueprint graphs with bp_* tools, edit UMG Widget Blueprints with widget_* tools, scaffold gameplay systems with scaffold_* tools, and save dirty packages. Inside the editor you can also open Window > Unreal MCP Chat. Endpoint: %s"),
					*EndpointUrl));

	return MakeJsonRpcResult(Id, ResultObject, NegotiatedProtocolVersion);
}

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::HandlePing(const TSharedPtr<FJsonValue>& Id)
{
	return MakeJsonRpcResult(Id, UnrealMcp::MakeEmptyObject());
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

TUniquePtr<FHttpServerResponse> FUnrealMcpModule::HandleToolsList(const TSharedPtr<FJsonValue>& Id)
{
	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	AppendToolDefinitions(ToolsArray);

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetArrayField(TEXT("tools"), ToolsArray);

	return MakeJsonRpcResult(Id, ResultObject);
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
	const TSharedRef<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> Run = MakeShared<FUnrealMcpAssistantRun, ESPMode::ThreadSafe>(
		this,
		UserPrompt,
		ConversationContext,
		PreviousResponseId,
		MoveTemp(OnEvent),
		MoveTemp(OnComplete));
	Run->Start();
	return StaticCastSharedRef<IUnrealMcpAssistantHandle>(Run);
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
	const FUnrealMcpExecutionResult Result = ExecuteTool(ToolName, Arguments);
	{
		const UnrealMcp::FToolPolicy ActivityPolicy = UnrealMcp::GetToolPolicy(ToolName);
		if (ActivityPolicy.RiskLevel != UnrealMcp::EToolRiskLevel::ReadOnly)
		{
			TSharedPtr<FJsonObject> ActivityDetails = MakeShared<FJsonObject>();
			ActivityDetails->SetStringField(TEXT("toolName"), ToolName);
			ActivityDetails->SetStringField(TEXT("riskLevel"), UnrealMcp::LexToString(ActivityPolicy.RiskLevel));
			ActivityDetails->SetBoolField(TEXT("isError"), Result.bIsError);
			ActivityDetails->SetNumberField(TEXT("textLength"), Result.Text.Len());
			ActivityDetails->SetBoolField(TEXT("hasStructuredContent"), Result.StructuredContent.IsValid());
			UnrealMcp::RecordSkillActivityEvent(TEXT("mcp_tool_result"), FString::Printf(TEXT("MCP tool %s %s."), *ToolName, Result.bIsError ? TEXT("failed") : TEXT("completed")), ActivityDetails);
		}
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

IMPLEMENT_MODULE(FUnrealMcpModule, UnrealMcp)

#undef LOCTEXT_NAMESPACE
