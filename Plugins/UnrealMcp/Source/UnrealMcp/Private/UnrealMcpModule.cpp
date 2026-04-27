#include "UnrealMcpModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
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
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Templates/Atomic.h"
#include "ToolMenus.h"
#include "UnrealMcpChatPanel.h"
#include "UnrealMcpSettings.h"
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
		TSharedPtr<FJsonObject> ToolObject = MakeShared<FJsonObject>();
		ToolObject->SetStringField(TEXT("name"), Name);
		ToolObject->SetStringField(TEXT("title"), Title);
		ToolObject->SetStringField(TEXT("description"), Description);
		ToolObject->SetObjectField(TEXT("inputSchema"), InputSchema);
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

	UObject* LoadAssetFromAnyPath(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& AnyAssetPath,
		FString& OutObjectPath,
		FString& OutFailureReason)
	{
		if (!EditorAssetSubsystem)
		{
			OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
			return nullptr;
		}

		OutObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(AnyAssetPath, OutFailureReason);
		if (OutObjectPath.IsEmpty())
		{
			return nullptr;
		}

		UObject* LoadedAsset = EditorAssetSubsystem->LoadAsset(OutObjectPath);
		if (!LoadedAsset)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to load asset '%s'."), *OutObjectPath);
		}

		return LoadedAsset;
	}

	UClass* ResolveClassPath(const FString& ClassPath, UEditorAssetSubsystem* EditorAssetSubsystem)
	{
		const FString TrimmedPath = ClassPath.TrimStartAndEnd();
		if (TrimmedPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* NativeClass = StaticLoadClass(UObject::StaticClass(), nullptr, *TrimmedPath))
		{
			return NativeClass;
		}

		if (!EditorAssetSubsystem)
		{
			return nullptr;
		}

		if (UClass* BlueprintClass = EditorAssetSubsystem->LoadBlueprintClass(TrimmedPath))
		{
			return BlueprintClass;
		}

		if (UObject* LoadedAsset = EditorAssetSubsystem->LoadAsset(TrimmedPath))
		{
			if (UClass* LoadedClass = Cast<UClass>(LoadedAsset))
			{
				return LoadedClass;
			}

			if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
			{
				return Blueprint->GeneratedClass;
			}
		}

		return nullptr;
	}

	FString PinDirectionToString(EEdGraphPinDirection Direction)
	{
		return Direction == EGPD_Input ? TEXT("input") : TEXT("output");
	}

	TSharedPtr<FJsonObject> DescribeBlueprintPin(const UEdGraphPin* Pin)
	{
		TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
		if (!Pin)
		{
			return PinObject;
		}

		PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObject->SetStringField(TEXT("displayName"), Pin->GetDisplayName().ToString());
		PinObject->SetStringField(TEXT("direction"), PinDirectionToString(Pin->Direction));
		PinObject->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		PinObject->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
		if (Pin->PinType.PinSubCategoryObject.IsValid())
		{
			PinObject->SetStringField(TEXT("subCategoryObject"), Pin->PinType.PinSubCategoryObject.Get()->GetPathName());
		}
		PinObject->SetBoolField(TEXT("isArray"), Pin->PinType.IsArray());
		PinObject->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
		PinObject->SetStringField(TEXT("defaultObject"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString());

		TArray<TSharedPtr<FJsonValue>> LinkedPinsArray;
		for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode())
			{
				continue;
			}

			TSharedPtr<FJsonObject> LinkObject = MakeShared<FJsonObject>();
			LinkObject->SetStringField(TEXT("nodeGuid"), LinkedPin->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			LinkObject->SetStringField(TEXT("pin"), LinkedPin->PinName.ToString());
			LinkedPinsArray.Add(MakeShared<FJsonValueObject>(LinkObject));
		}
		PinObject->SetArrayField(TEXT("linkedTo"), LinkedPinsArray);

		return PinObject;
	}

	TSharedPtr<FJsonObject> DescribeBlueprintNode(const UEdGraphNode* Node)
	{
		TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
		if (!Node)
		{
			return NodeObject;
		}

		NodeObject->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		NodeObject->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetPathName());
		NodeObject->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeObject->SetStringField(TEXT("graph"), Node->GetGraph() ? Node->GetGraph()->GetName() : FString());
		NodeObject->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeObject->SetNumberField(TEXT("y"), Node->NodePosY);

		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			PinsArray.Add(MakeShared<FJsonValueObject>(DescribeBlueprintPin(Pin)));
		}
		NodeObject->SetArrayField(TEXT("pins"), PinsArray);

		return NodeObject;
	}

	TSharedPtr<FJsonObject> MakeBlueprintEditStructuredContent(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		UEdGraphNode* Node,
		const FString& Action)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), Action);
		StructuredContent->SetStringField(TEXT("blueprint"), Blueprint ? Blueprint->GetPathName() : FString());
		StructuredContent->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString());
		if (Node)
		{
			StructuredContent->SetObjectField(TEXT("node"), DescribeBlueprintNode(Node));
		}
		return StructuredContent;
	}

	UBlueprint* LoadBlueprintAsset(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& BlueprintPath,
		FString& OutObjectPath,
		FString& OutFailureReason)
	{
		UObject* LoadedAsset = LoadAssetFromAnyPath(EditorAssetSubsystem, BlueprintPath, OutObjectPath, OutFailureReason);
		if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset))
		{
			return Blueprint;
		}

		if (UClass* LoadedClass = Cast<UClass>(LoadedAsset))
		{
			if (UBlueprint* Blueprint = Cast<UBlueprint>(LoadedClass->ClassGeneratedBy))
			{
				return Blueprint;
			}
		}

		if (LoadedAsset)
		{
			OutFailureReason = FString::Printf(TEXT("Asset '%s' is not a Blueprint."), *OutObjectPath);
		}
		return nullptr;
	}

	UEdGraph* ResolveBlueprintGraph(
		UBlueprint* Blueprint,
		const FString& GraphName,
		bool bCreateEventGraphIfMissing,
		FString& OutFailureReason)
	{
		if (!Blueprint)
		{
			OutFailureReason = TEXT("Blueprint is null.");
			return nullptr;
		}

		const FString RequestedGraphName = GraphName.TrimStartAndEnd().IsEmpty()
			? UEdGraphSchema_K2::GN_EventGraph.ToString()
			: GraphName.TrimStartAndEnd();

		if (RequestedGraphName.Equals(UEdGraphSchema_K2::GN_EventGraph.ToString(), ESearchCase::IgnoreCase))
		{
			if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
			{
				return EventGraph;
			}
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(RequestedGraphName, ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}

		if (bCreateEventGraphIfMissing && RequestedGraphName.Equals(UEdGraphSchema_K2::GN_EventGraph.ToString(), ESearchCase::IgnoreCase))
		{
			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint,
				UEdGraphSchema_K2::GN_EventGraph,
				UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddUbergraphPage(Blueprint, NewGraph);
			return NewGraph;
		}

		OutFailureReason = FString::Printf(TEXT("Blueprint graph '%s' was not found in %s."), *RequestedGraphName, *Blueprint->GetPathName());
		return nullptr;
	}

	UEdGraphNode* FindBlueprintNodeByGuid(UEdGraph* Graph, const FString& NodeGuidString)
	{
		if (!Graph)
		{
			return nullptr;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidString.TrimStartAndEnd(), NodeGuid))
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}

		return nullptr;
	}

	UEdGraphPin* FindBlueprintPinByName(UEdGraphNode* Node, const FString& PinName)
	{
		if (!Node)
		{
			return nullptr;
		}

		const FString TrimmedPinName = PinName.TrimStartAndEnd();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			if (Pin->PinName.ToString().Equals(TrimmedPinName, ESearchCase::IgnoreCase)
				|| Pin->GetDisplayName().ToString().Equals(TrimmedPinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}

		return nullptr;
	}

	FName NormalizePinCategory(const FString& PinCategory)
	{
		const FString Category = PinCategory.TrimStartAndEnd().ToLower();
		if (Category == TEXT("exec")) { return UEdGraphSchema_K2::PC_Exec; }
		if (Category == TEXT("bool") || Category == TEXT("boolean")) { return UEdGraphSchema_K2::PC_Boolean; }
		if (Category == TEXT("byte")) { return UEdGraphSchema_K2::PC_Byte; }
		if (Category == TEXT("class")) { return UEdGraphSchema_K2::PC_Class; }
		if (Category == TEXT("int") || Category == TEXT("integer")) { return UEdGraphSchema_K2::PC_Int; }
		if (Category == TEXT("int64")) { return UEdGraphSchema_K2::PC_Int64; }
		if (Category == TEXT("float")) { return UEdGraphSchema_K2::PC_Real; }
		if (Category == TEXT("double") || Category == TEXT("real")) { return UEdGraphSchema_K2::PC_Real; }
		if (Category == TEXT("name")) { return UEdGraphSchema_K2::PC_Name; }
		if (Category == TEXT("object")) { return UEdGraphSchema_K2::PC_Object; }
		if (Category == TEXT("interface")) { return UEdGraphSchema_K2::PC_Interface; }
		if (Category == TEXT("string")) { return UEdGraphSchema_K2::PC_String; }
		if (Category == TEXT("text")) { return UEdGraphSchema_K2::PC_Text; }
		if (Category == TEXT("struct")) { return UEdGraphSchema_K2::PC_Struct; }
		if (Category == TEXT("wildcard")) { return UEdGraphSchema_K2::PC_Wildcard; }
		if (Category == TEXT("enum")) { return UEdGraphSchema_K2::PC_Enum; }
		if (Category == TEXT("softobject")) { return UEdGraphSchema_K2::PC_SoftObject; }
		if (Category == TEXT("softclass")) { return UEdGraphSchema_K2::PC_SoftClass; }
		return FName(*PinCategory.TrimStartAndEnd());
	}

	UObject* ResolvePinSubCategoryObject(
		const FName& PinCategory,
		const FString& SubCategoryObjectPath,
		UEditorAssetSubsystem* EditorAssetSubsystem,
		FString& OutFailureReason)
	{
		const FString TrimmedPath = SubCategoryObjectPath.TrimStartAndEnd();
		if (TrimmedPath.IsEmpty())
		{
			return nullptr;
		}

		if (PinCategory == UEdGraphSchema_K2::PC_Object
			|| PinCategory == UEdGraphSchema_K2::PC_Class
			|| PinCategory == UEdGraphSchema_K2::PC_Interface
			|| PinCategory == UEdGraphSchema_K2::PC_SoftObject
			|| PinCategory == UEdGraphSchema_K2::PC_SoftClass)
		{
			if (UClass* Class = ResolveClassPath(TrimmedPath, EditorAssetSubsystem))
			{
				return Class;
			}
			OutFailureReason = FString::Printf(TEXT("Unable to resolve class subCategoryObjectPath '%s'."), *TrimmedPath);
			return nullptr;
		}

		if (PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *TrimmedPath))
			{
				return Struct;
			}
			OutFailureReason = FString::Printf(TEXT("Unable to resolve struct subCategoryObjectPath '%s'."), *TrimmedPath);
			return nullptr;
		}

		if (PinCategory == UEdGraphSchema_K2::PC_Enum || PinCategory == UEdGraphSchema_K2::PC_Byte)
		{
			if (UEnum* Enum = LoadObject<UEnum>(nullptr, *TrimmedPath))
			{
				return Enum;
			}
			OutFailureReason = FString::Printf(TEXT("Unable to resolve enum subCategoryObjectPath '%s'."), *TrimmedPath);
			return nullptr;
		}

		FString ObjectPath;
		UObject* LoadedObject = LoadAssetFromAnyPath(EditorAssetSubsystem, TrimmedPath, ObjectPath, OutFailureReason);
		return LoadedObject;
	}

	bool BuildBlueprintPinType(
		const FJsonObject& Arguments,
		UEditorAssetSubsystem* EditorAssetSubsystem,
		FEdGraphPinType& OutPinType,
		FString& OutFailureReason)
	{
		FString PinCategoryString = TEXT("bool");
		Arguments.TryGetStringField(TEXT("pinCategory"), PinCategoryString);

		FName PinCategory = NormalizePinCategory(PinCategoryString);
		if (PinCategory.IsNone())
		{
			OutFailureReason = TEXT("pinCategory cannot be empty.");
			return false;
		}

		FString PinSubCategoryString;
		Arguments.TryGetStringField(TEXT("pinSubCategory"), PinSubCategoryString);

		UObject* PinSubCategoryObject = nullptr;
		FString SubCategoryObjectPath;
		if (Arguments.TryGetStringField(TEXT("subCategoryObjectPath"), SubCategoryObjectPath) && !SubCategoryObjectPath.TrimStartAndEnd().IsEmpty())
		{
			PinSubCategoryObject = ResolvePinSubCategoryObject(PinCategory, SubCategoryObjectPath, EditorAssetSubsystem, OutFailureReason);
			if (!PinSubCategoryObject)
			{
				return false;
			}
		}

		OutPinType = FEdGraphPinType();
		OutPinType.PinCategory = PinCategory;
		OutPinType.PinSubCategory = FName(*PinSubCategoryString.TrimStartAndEnd());
		OutPinType.PinSubCategoryObject = PinSubCategoryObject;

		if (PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			const FString Lower = PinCategoryString.ToLower();
			OutPinType.PinSubCategory = Lower == TEXT("float") ? UEdGraphSchema_K2::PC_Float : UEdGraphSchema_K2::PC_Double;
		}

		FString ContainerTypeString = TEXT("none");
		Arguments.TryGetStringField(TEXT("containerType"), ContainerTypeString);
		ContainerTypeString = ContainerTypeString.TrimStartAndEnd().ToLower();
		if (ContainerTypeString == TEXT("array"))
		{
			OutPinType.ContainerType = EPinContainerType::Array;
		}
		else if (ContainerTypeString == TEXT("set"))
		{
			OutPinType.ContainerType = EPinContainerType::Set;
		}
		else if (ContainerTypeString == TEXT("map"))
		{
			OutPinType.ContainerType = EPinContainerType::Map;
		}
		else
		{
			OutPinType.ContainerType = EPinContainerType::None;
		}

		return true;
	}

	UFunction* ResolveFunctionByClassAndName(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& FunctionClassPath,
		const FString& FunctionName,
		FString& OutFailureReason)
	{
		UClass* FunctionClass = ResolveClassPath(FunctionClassPath, EditorAssetSubsystem);
		if (!FunctionClass)
		{
			OutFailureReason = FString::Printf(TEXT("Unable to resolve functionClassPath '%s'."), *FunctionClassPath);
			return nullptr;
		}

		UFunction* Function = FunctionClass->FindFunctionByName(FName(*FunctionName.TrimStartAndEnd()));
		if (!Function)
		{
			OutFailureReason = FString::Printf(TEXT("Function '%s' was not found on class '%s'."), *FunctionName, *FunctionClass->GetPathName());
			return nullptr;
		}

		return Function;
	}

	UEdGraph* FindStandardMacroGraph(const FString& MacroName)
	{
		UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
		if (!StandardMacros)
		{
			StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros"));
		}
		if (!StandardMacros)
		{
			return nullptr;
		}

		TArray<UEdGraph*> Graphs;
		StandardMacros->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(MacroName.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	UWidgetBlueprint* LoadWidgetBlueprintAsset(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& WidgetBlueprintPath,
		FString& OutObjectPath,
		FString& OutFailureReason)
	{
		UBlueprint* Blueprint = LoadBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, OutObjectPath, OutFailureReason);
		if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
		{
			return WidgetBlueprint;
		}

		if (Blueprint)
		{
			OutFailureReason = FString::Printf(TEXT("Blueprint '%s' is not a Widget Blueprint."), *OutObjectPath);
		}
		return nullptr;
	}

	UWidgetBlueprint* LoadOrCreateWidgetBlueprintAsset(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& WidgetBlueprintPath,
		FString& OutObjectPath,
		bool& bOutCreated,
		FString& OutFailureReason)
	{
		bOutCreated = false;
		OutObjectPath.Reset();
		OutFailureReason.Reset();

		if (!EditorAssetSubsystem)
		{
			OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
			return nullptr;
		}

		const FString ObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(WidgetBlueprintPath, OutFailureReason);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}

		OutObjectPath = ObjectPath;
		if (EditorAssetSubsystem->DoesAssetExist(ObjectPath))
		{
			return LoadWidgetBlueprintAsset(EditorAssetSubsystem, ObjectPath, OutObjectPath, OutFailureReason);
		}

		FString CreateFailureReason;
		if (!EditorScriptingHelpers::IsAValidPathForCreateNewAsset(ObjectPath, CreateFailureReason))
		{
			OutFailureReason = FString::Printf(TEXT("Invalid Widget Blueprint path '%s': %s"), *ObjectPath, *CreateFailureReason);
			return nullptr;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		const FName AssetName(*FPackageName::GetLongPackageAssetName(PackageName));
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create package '%s'."), *PackageName);
			return nullptr;
		}

		UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(),
			Package,
			AssetName,
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UnrealMcp"))));

		if (!WidgetBlueprint)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create Widget Blueprint '%s'."), *ObjectPath);
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(WidgetBlueprint);
		Package->MarkPackageDirty();
		bOutCreated = true;
		return WidgetBlueprint;
	}

	UWidget* FindWidgetByName(UWidgetBlueprint* WidgetBlueprint, const FString& WidgetName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			return nullptr;
		}

		const FString TrimmedName = WidgetName.TrimStartAndEnd();
		if (TrimmedName.IsEmpty()
			|| TrimmedName.Equals(TEXT("Root"), ESearchCase::IgnoreCase)
			|| TrimmedName.Equals(TEXT("RootWidget"), ESearchCase::IgnoreCase))
		{
			return WidgetBlueprint->WidgetTree->RootWidget;
		}

		return WidgetBlueprint->WidgetTree->FindWidget(FName(*TrimmedName));
	}

	UClass* ResolveWidgetClass(const FString& WidgetClassName, UEditorAssetSubsystem* EditorAssetSubsystem)
	{
		const FString TrimmedClassName = WidgetClassName.TrimStartAndEnd();
		const FString LowerClassName = TrimmedClassName.ToLower();
		if (LowerClassName.IsEmpty())
		{
			return nullptr;
		}

		if (LowerClassName == TEXT("border") || LowerClassName == TEXT("uborder")) { return UBorder::StaticClass(); }
		if (LowerClassName == TEXT("button") || LowerClassName == TEXT("ubutton")) { return UButton::StaticClass(); }
		if (LowerClassName == TEXT("canvas") || LowerClassName == TEXT("canvaspanel") || LowerClassName == TEXT("ucanvaspanel")) { return UCanvasPanel::StaticClass(); }
		if (LowerClassName == TEXT("editabletextbox") || LowerClassName == TEXT("ueditabletextbox")) { return UEditableTextBox::StaticClass(); }
		if (LowerClassName == TEXT("horizontalbox") || LowerClassName == TEXT("uhorizontalbox")) { return UHorizontalBox::StaticClass(); }
		if (LowerClassName == TEXT("image") || LowerClassName == TEXT("uimage")) { return UImage::StaticClass(); }
		if (LowerClassName == TEXT("overlay") || LowerClassName == TEXT("uoverlay")) { return UOverlay::StaticClass(); }
		if (LowerClassName == TEXT("progressbar") || LowerClassName == TEXT("uprogressbar")) { return UProgressBar::StaticClass(); }
		if (LowerClassName == TEXT("scalebox") || LowerClassName == TEXT("uscalebox")) { return UScaleBox::StaticClass(); }
		if (LowerClassName == TEXT("scrollbox") || LowerClassName == TEXT("uscrollbox")) { return UScrollBox::StaticClass(); }
		if (LowerClassName == TEXT("sizebox") || LowerClassName == TEXT("usizebox")) { return USizeBox::StaticClass(); }
		if (LowerClassName == TEXT("spacer") || LowerClassName == TEXT("uspacer")) { return USpacer::StaticClass(); }
		if (LowerClassName == TEXT("text") || LowerClassName == TEXT("textblock") || LowerClassName == TEXT("utextblock")) { return UTextBlock::StaticClass(); }
		if (LowerClassName == TEXT("verticalbox") || LowerClassName == TEXT("uverticalbox")) { return UVerticalBox::StaticClass(); }

		UClass* ResolvedClass = ResolveClassPath(TrimmedClassName, EditorAssetSubsystem);
		if (ResolvedClass && ResolvedClass->IsChildOf(UWidget::StaticClass()))
		{
			return ResolvedClass;
		}

		return nullptr;
	}

	FString MakeUniqueWidgetName(UWidgetBlueprint* WidgetBlueprint, UClass* WidgetClass, const FString& RequestedName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			return RequestedName.TrimStartAndEnd().IsEmpty() ? TEXT("Widget") : RequestedName.TrimStartAndEnd();
		}

		FString BaseName = RequestedName.TrimStartAndEnd();
		if (BaseName.IsEmpty())
		{
			BaseName = WidgetClass ? WidgetClass->GetName().Replace(TEXT("U"), TEXT("")) : TEXT("Widget");
		}

		FString CandidateName = BaseName;
		int32 Suffix = 1;
		while (WidgetBlueprint->WidgetTree->FindWidget(FName(*CandidateName)) != nullptr)
		{
			CandidateName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
		}
		return CandidateName;
	}

	TSharedPtr<FJsonObject> DescribeWidget(UWidget* Widget)
	{
		TSharedPtr<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
		if (!Widget)
		{
			return WidgetObject;
		}

		WidgetObject->SetStringField(TEXT("name"), Widget->GetName());
		WidgetObject->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
		WidgetObject->SetBoolField(TEXT("isVariable"), Widget->bIsVariable != 0);
		WidgetObject->SetStringField(TEXT("slotClass"), Widget->Slot ? Widget->Slot->GetClass()->GetPathName() : FString());
		if (UPanelSlot* Slot = Widget->Slot)
		{
			WidgetObject->SetStringField(TEXT("parent"), Slot->Parent ? Slot->Parent->GetName() : FString());
		}
		return WidgetObject;
	}

	TSharedPtr<FJsonObject> DescribeWidgetBlueprint(UWidgetBlueprint* WidgetBlueprint, const FString& Action)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), Action);
		StructuredContent->SetStringField(TEXT("widgetBlueprint"), WidgetBlueprint ? WidgetBlueprint->GetPathName() : FString());
		StructuredContent->SetStringField(TEXT("rootWidget"), WidgetBlueprint && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget
			? WidgetBlueprint->WidgetTree->RootWidget->GetName()
			: FString());

		TArray<TSharedPtr<FJsonValue>> WidgetsArray;
		if (WidgetBlueprint && WidgetBlueprint->WidgetTree)
		{
			TArray<UWidget*> Widgets;
			WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
			for (UWidget* Widget : Widgets)
			{
				WidgetsArray.Add(MakeShared<FJsonValueObject>(DescribeWidget(Widget)));
			}
		}
		StructuredContent->SetArrayField(TEXT("widgets"), WidgetsArray);
		StructuredContent->SetNumberField(TEXT("widgetCount"), WidgetsArray.Num());
		return StructuredContent;
	}

	void EnsureWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, UWidget* Widget)
	{
		if (!WidgetBlueprint || !Widget)
		{
			return;
		}

		const FName WidgetName = Widget->GetFName();
		if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(WidgetName))
		{
			WidgetBlueprint->OnVariableAdded(WidgetName);
		}
	}

	void RemoveWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, const FName& WidgetName)
	{
		if (!WidgetBlueprint || WidgetName.IsNone())
		{
			return;
		}

		if (WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(WidgetName))
		{
			WidgetBlueprint->OnVariableRemoved(WidgetName);
		}
	}

	void RenameWidgetBlueprintGuid(UWidgetBlueprint* WidgetBlueprint, const FName& OldWidgetName, UWidget* Widget)
	{
		if (!WidgetBlueprint || !Widget || OldWidgetName.IsNone())
		{
			return;
		}

		const FName NewWidgetName = Widget->GetFName();
		if (OldWidgetName == NewWidgetName)
		{
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);
			return;
		}

		const bool bHadOldGuid = WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(OldWidgetName);
		const bool bHasNewGuid = WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(NewWidgetName);
		if (bHadOldGuid && !bHasNewGuid)
		{
			WidgetBlueprint->OnVariableRenamed(OldWidgetName, NewWidgetName);
		}
		else
		{
			if (bHadOldGuid && bHasNewGuid)
			{
				WidgetBlueprint->OnVariableRemoved(OldWidgetName);
			}
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);
		}
	}

	void ClearWidgetBlueprintTree(UWidgetBlueprint* WidgetBlueprint)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			return;
		}

		TArray<UWidget*> ExistingWidgets;
		WidgetBlueprint->WidgetTree->GetAllWidgets(ExistingWidgets);
		if (WidgetBlueprint->WidgetTree->RootWidget)
		{
			WidgetBlueprint->WidgetTree->RemoveWidget(WidgetBlueprint->WidgetTree->RootWidget);
			WidgetBlueprint->WidgetTree->RootWidget = nullptr;
		}

		for (UWidget* ExistingWidget : ExistingWidgets)
		{
			if (!ExistingWidget)
			{
				continue;
			}
			const FName ExistingName = ExistingWidget->GetFName();
			ExistingWidget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
			RemoveWidgetBlueprintGuid(WidgetBlueprint, ExistingName);
		}

		WidgetBlueprint->Bindings.Empty();
	}

	void MarkWidgetBlueprintModified(UWidgetBlueprint* WidgetBlueprint, bool bStructurallyModified = true)
	{
		if (!WidgetBlueprint)
		{
			return;
		}

		if (WidgetBlueprint->WidgetTree)
		{
			WidgetBlueprint->WidgetTree->Modify();
		}

		if (bStructurallyModified)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
		}
		WidgetBlueprint->MarkPackageDirty();
	}

	bool ApplyStringToProperty(
		UObject* RootObject,
		const FString& PropertyPath,
		const FString& Value,
		FString& OutFailureReason,
		TSharedPtr<FJsonObject>& OutEditObject)
	{
		OutEditObject = MakeShared<FJsonObject>();
		OutEditObject->SetStringField(TEXT("propertyPath"), PropertyPath);
		OutEditObject->SetStringField(TEXT("value"), Value);

		UObject* OwnerObject = nullptr;
		FProperty* LeafProperty = nullptr;
		FProperty* NotifyProperty = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolveObjectPropertyPath(RootObject, PropertyPath, OwnerObject, LeafProperty, NotifyProperty, ValuePtr, OutFailureReason))
		{
			OutEditObject->SetBoolField(TEXT("success"), false);
			OutEditObject->SetStringField(TEXT("error"), OutFailureReason);
			return false;
		}

		OutEditObject->SetStringField(TEXT("ownerObjectPath"), OwnerObject->GetPathName());
		OutEditObject->SetField(TEXT("before"), PropertyValueToJson(LeafProperty, ValuePtr));

		OwnerObject->Modify();
		OwnerObject->PreEditChange(NotifyProperty);

		bool bApplied = false;
		FStringOutputDevice ImportErrors;

		if (FTextProperty* TextProperty = CastField<FTextProperty>(LeafProperty))
		{
			TextProperty->SetPropertyValue(ValuePtr, FText::FromString(Value));
			bApplied = true;
		}
		else if (FStrProperty* StringProperty = CastField<FStrProperty>(LeafProperty))
		{
			StringProperty->SetPropertyValue(ValuePtr, Value);
			bApplied = true;
		}
		else if (FNameProperty* NameProperty = CastField<FNameProperty>(LeafProperty))
		{
			NameProperty->SetPropertyValue(ValuePtr, FName(*Value));
			bApplied = true;
		}
		else
		{
			bApplied = LeafProperty->ImportText_Direct(*Value, ValuePtr, OwnerObject, PPF_None, &ImportErrors) != nullptr;
		}

		if (bApplied)
		{
			FPropertyChangedEvent PropertyChangedEvent(NotifyProperty, EPropertyChangeType::ValueSet);
			OwnerObject->PostEditChangeProperty(PropertyChangedEvent);
			OwnerObject->MarkPackageDirty();
			OutEditObject->SetBoolField(TEXT("success"), true);
			OutEditObject->SetField(TEXT("after"), PropertyValueToJson(LeafProperty, ValuePtr));
			return true;
		}

		OwnerObject->PostEditChange();
		OutFailureReason = ImportErrors.IsEmpty()
			? FString::Printf(TEXT("Failed to import '%s' into property '%s'."), *Value, *PropertyPath)
			: FString(ImportErrors).TrimStartAndEnd();
		OutEditObject->SetBoolField(TEXT("success"), false);
		OutEditObject->SetStringField(TEXT("error"), OutFailureReason);
		return false;
	}

	EHorizontalAlignment ParseHorizontalAlignment(const FString& Value)
	{
		const FString Lower = Value.TrimStartAndEnd().ToLower();
		if (Lower == TEXT("center") || Lower == TEXT("centre") || Lower == TEXT("middle")) { return HAlign_Center; }
		if (Lower == TEXT("right")) { return HAlign_Right; }
		if (Lower == TEXT("fill") || Lower == TEXT("stretch")) { return HAlign_Fill; }
		return HAlign_Left;
	}

	EVerticalAlignment ParseVerticalAlignment(const FString& Value)
	{
		const FString Lower = Value.TrimStartAndEnd().ToLower();
		if (Lower == TEXT("center") || Lower == TEXT("centre") || Lower == TEXT("middle")) { return VAlign_Center; }
		if (Lower == TEXT("bottom")) { return VAlign_Bottom; }
		if (Lower == TEXT("fill") || Lower == TEXT("stretch")) { return VAlign_Fill; }
		return VAlign_Top;
	}

	bool TryGetMargin(const FJsonObject& Arguments, FMargin& OutMargin)
	{
		double Left = 0.0;
		double Top = 0.0;
		double Right = 0.0;
		double Bottom = 0.0;
		const bool bHasAny =
			Arguments.TryGetNumberField(TEXT("paddingLeft"), Left)
			| Arguments.TryGetNumberField(TEXT("paddingTop"), Top)
			| Arguments.TryGetNumberField(TEXT("paddingRight"), Right)
			| Arguments.TryGetNumberField(TEXT("paddingBottom"), Bottom);

		if (!bHasAny)
		{
			return false;
		}

		OutMargin = FMargin(static_cast<float>(Left), static_cast<float>(Top), static_cast<float>(Right), static_cast<float>(Bottom));
		return true;
	}

	void ApplyPanelSlotLayout(UPanelSlot* Slot, const FJsonObject& Arguments, int32& InOutChangedCount)
	{
		if (!Slot)
		{
			return;
		}

		Slot->Modify();

		if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Slot))
		{
			double X = 0.0;
			double Y = 0.0;
			if (Arguments.TryGetNumberField(TEXT("x"), X) | Arguments.TryGetNumberField(TEXT("y"), Y))
			{
				CanvasSlot->SetPosition(FVector2D(X, Y));
				++InOutChangedCount;
			}

			double Width = 0.0;
			double Height = 0.0;
			if (Arguments.TryGetNumberField(TEXT("width"), Width) | Arguments.TryGetNumberField(TEXT("height"), Height))
			{
				CanvasSlot->SetSize(FVector2D(Width, Height));
				++InOutChangedCount;
			}

			bool bAutoSize = false;
			if (Arguments.TryGetBoolField(TEXT("autoSize"), bAutoSize))
			{
				CanvasSlot->SetAutoSize(bAutoSize);
				++InOutChangedCount;
			}

			double ZOrder = 0.0;
			if (Arguments.TryGetNumberField(TEXT("zOrder"), ZOrder))
			{
				CanvasSlot->SetZOrder(static_cast<int32>(ZOrder));
				++InOutChangedCount;
			}

			double AlignmentX = 0.0;
			double AlignmentY = 0.0;
			if (Arguments.TryGetNumberField(TEXT("alignmentX"), AlignmentX) | Arguments.TryGetNumberField(TEXT("alignmentY"), AlignmentY))
			{
				CanvasSlot->SetAlignment(FVector2D(AlignmentX, AlignmentY));
				++InOutChangedCount;
			}

			double AnchorMinX = 0.0;
			double AnchorMinY = 0.0;
			double AnchorMaxX = 0.0;
			double AnchorMaxY = 0.0;
			const bool bHasAnchor =
				Arguments.TryGetNumberField(TEXT("anchorMinX"), AnchorMinX)
				| Arguments.TryGetNumberField(TEXT("anchorMinY"), AnchorMinY)
				| Arguments.TryGetNumberField(TEXT("anchorMaxX"), AnchorMaxX)
				| Arguments.TryGetNumberField(TEXT("anchorMaxY"), AnchorMaxY);
			if (bHasAnchor)
			{
				CanvasSlot->SetAnchors(FAnchors(AnchorMinX, AnchorMinY, AnchorMaxX, AnchorMaxY));
				++InOutChangedCount;
			}
		}

		FMargin Padding;
		const bool bHasPadding = TryGetMargin(Arguments, Padding);
		FString HorizontalAlignmentString;
		const bool bHasHorizontalAlignment = Arguments.TryGetStringField(TEXT("hAlign"), HorizontalAlignmentString) && !HorizontalAlignmentString.TrimStartAndEnd().IsEmpty();
		FString VerticalAlignmentString;
		const bool bHasVerticalAlignment = Arguments.TryGetStringField(TEXT("vAlign"), VerticalAlignmentString) && !VerticalAlignmentString.TrimStartAndEnd().IsEmpty();
		FString SizeRuleString;
		const bool bHasSizeRule = Arguments.TryGetStringField(TEXT("sizeRule"), SizeRuleString) && !SizeRuleString.TrimStartAndEnd().IsEmpty();
		double SizeValue = 1.0;
		const bool bHasSizeValue = Arguments.TryGetNumberField(TEXT("sizeValue"), SizeValue);

		if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(Slot))
		{
			if (bHasPadding) { VerticalSlot->SetPadding(Padding); ++InOutChangedCount; }
			if (bHasHorizontalAlignment) { VerticalSlot->SetHorizontalAlignment(ParseHorizontalAlignment(HorizontalAlignmentString)); ++InOutChangedCount; }
			if (bHasVerticalAlignment) { VerticalSlot->SetVerticalAlignment(ParseVerticalAlignment(VerticalAlignmentString)); ++InOutChangedCount; }
			if (bHasSizeRule || bHasSizeValue)
			{
				FSlateChildSize ChildSize(SizeRuleString.TrimStartAndEnd().Equals(TEXT("automatic"), ESearchCase::IgnoreCase) ? ESlateSizeRule::Automatic : ESlateSizeRule::Fill);
				ChildSize.Value = static_cast<float>(SizeValue);
				VerticalSlot->SetSize(ChildSize);
				++InOutChangedCount;
			}
		}
		else if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Slot))
		{
			if (bHasPadding) { HorizontalSlot->SetPadding(Padding); ++InOutChangedCount; }
			if (bHasHorizontalAlignment) { HorizontalSlot->SetHorizontalAlignment(ParseHorizontalAlignment(HorizontalAlignmentString)); ++InOutChangedCount; }
			if (bHasVerticalAlignment) { HorizontalSlot->SetVerticalAlignment(ParseVerticalAlignment(VerticalAlignmentString)); ++InOutChangedCount; }
			if (bHasSizeRule || bHasSizeValue)
			{
				FSlateChildSize ChildSize(SizeRuleString.TrimStartAndEnd().Equals(TEXT("automatic"), ESearchCase::IgnoreCase) ? ESlateSizeRule::Automatic : ESlateSizeRule::Fill);
				ChildSize.Value = static_cast<float>(SizeValue);
				HorizontalSlot->SetSize(ChildSize);
				++InOutChangedCount;
			}
		}
		else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
		{
			if (bHasPadding) { OverlaySlot->SetPadding(Padding); ++InOutChangedCount; }
			if (bHasHorizontalAlignment) { OverlaySlot->SetHorizontalAlignment(ParseHorizontalAlignment(HorizontalAlignmentString)); ++InOutChangedCount; }
			if (bHasVerticalAlignment) { OverlaySlot->SetVerticalAlignment(ParseVerticalAlignment(VerticalAlignmentString)); ++InOutChangedCount; }
		}
	}

	UWidget* AddTemplateWidget(UWidgetBlueprint* WidgetBlueprint, UPanelWidget* Parent, UClass* WidgetClass, const FString& WidgetName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !Parent || !WidgetClass)
		{
			return nullptr;
		}

		UWidget* Widget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
		if (!Widget)
		{
			return nullptr;
		}

		Parent->AddChild(Widget);
		EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);
		return Widget;
	}

	void SetCanvasLayout(UWidget* Widget, double X, double Y, double Width, double Height, int32 ZOrder = 0)
	{
		if (UCanvasPanelSlot* CanvasSlot = Widget ? Cast<UCanvasPanelSlot>(Widget->Slot) : nullptr)
		{
			CanvasSlot->SetPosition(FVector2D(X, Y));
			CanvasSlot->SetSize(FVector2D(Width, Height));
			CanvasSlot->SetZOrder(ZOrder);
		}
	}

	void SetBoxSlotPadding(UWidget* Widget, const FMargin& Padding)
	{
		if (UVerticalBoxSlot* VerticalSlot = Widget ? Cast<UVerticalBoxSlot>(Widget->Slot) : nullptr)
		{
			VerticalSlot->SetPadding(Padding);
		}
		else if (UHorizontalBoxSlot* HorizontalSlot = Widget ? Cast<UHorizontalBoxSlot>(Widget->Slot) : nullptr)
		{
			HorizontalSlot->SetPadding(Padding);
		}
	}

	bool BuildDefaultWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			OutFailureReason = TEXT("Widget Blueprint or WidgetTree is unavailable.");
			return false;
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		ClearWidgetBlueprintTree(WidgetBlueprint);

		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		if (!RootCanvas)
		{
			OutFailureReason = TEXT("Failed to create RootCanvas.");
			return false;
		}
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, RootCanvas);

		UTextBlock* Title = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UTextBlock::StaticClass(), TEXT("TitleText")));
		if (Title)
		{
			Title->SetText(FText::FromString(TitleText.IsEmpty() ? TEXT("Imperial Tavern") : TitleText));
			SetCanvasLayout(Title, 32.0, 24.0, 760.0, 56.0, 1);
		}

		UTextBlock* Economy = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UTextBlock::StaticClass(), TEXT("EconomyText")));
		if (Economy)
		{
			Economy->SetText(FText::FromString(TEXT("Food 3/10    Gold 1    Tavern Lv.1")));
			SetCanvasLayout(Economy, 820.0, 32.0, 420.0, 40.0, 1);
			Economy->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Economy);
		}

		UVerticalBox* BoardPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("BoardPanel")));
		if (BoardPanel)
		{
			BoardPanel->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, BoardPanel);
			SetCanvasLayout(BoardPanel, 64.0, 130.0, 760.0, 420.0, 1);

			UTextBlock* BoardLabel = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, BoardPanel, UTextBlock::StaticClass(), TEXT("BoardLabel")));
			if (BoardLabel)
			{
				BoardLabel->SetText(FText::FromString(TEXT("FIELD")));
				SetBoxSlotPadding(BoardLabel, FMargin(0.0f, 0.0f, 0.0f, 12.0f));
			}

			UHorizontalBox* FieldSlots = Cast<UHorizontalBox>(AddTemplateWidget(WidgetBlueprint, BoardPanel, UHorizontalBox::StaticClass(), TEXT("FieldSlots")));
			if (FieldSlots)
			{
				FieldSlots->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, FieldSlots);
				for (int32 Index = 0; Index < 5; ++Index)
				{
					UButton* CardSlot = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, FieldSlots, UButton::StaticClass(), FString::Printf(TEXT("FieldCardSlot_%d"), Index + 1)));
					if (CardSlot)
					{
						CardSlot->bIsVariable = true;
						EnsureWidgetBlueprintGuid(WidgetBlueprint, CardSlot);
						SetBoxSlotPadding(CardSlot, FMargin(0.0f, 0.0f, 12.0f, 0.0f));
						UTextBlock* CardLabel = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*FString::Printf(TEXT("FieldCardSlotLabel_%d"), Index + 1)));
						if (CardLabel)
						{
							CardLabel->SetText(FText::FromString(FString::Printf(TEXT("Card %d"), Index + 1)));
							EnsureWidgetBlueprintGuid(WidgetBlueprint, CardLabel);
							CardSlot->AddChild(CardLabel);
						}
					}
				}
			}
		}

		UVerticalBox* ShopPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("ShopPanel")));
		if (ShopPanel)
		{
			ShopPanel->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopPanel);
			SetCanvasLayout(ShopPanel, 64.0, 590.0, 840.0, 180.0, 1);

			UTextBlock* ShopLabel = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UTextBlock::StaticClass(), TEXT("ShopLabel")));
			if (ShopLabel)
			{
				ShopLabel->SetText(FText::FromString(TEXT("TAVERN SHOP")));
				SetBoxSlotPadding(ShopLabel, FMargin(0.0f, 0.0f, 0.0f, 10.0f));
			}

			UHorizontalBox* ShopSlots = Cast<UHorizontalBox>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UHorizontalBox::StaticClass(), TEXT("ShopSlots")));
			if (ShopSlots)
			{
				ShopSlots->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopSlots);
				for (int32 Index = 0; Index < 5; ++Index)
				{
					UButton* ShopCard = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, ShopSlots, UButton::StaticClass(), FString::Printf(TEXT("ShopCard_%d"), Index + 1)));
					if (ShopCard)
					{
						ShopCard->bIsVariable = true;
						EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopCard);
						SetBoxSlotPadding(ShopCard, FMargin(0.0f, 0.0f, 10.0f, 0.0f));
						UTextBlock* ShopCardLabel = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*FString::Printf(TEXT("ShopCardLabel_%d"), Index + 1)));
						if (ShopCardLabel)
						{
							ShopCardLabel->SetText(FText::FromString(FString::Printf(TEXT("Offer %d"), Index + 1)));
							EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopCardLabel);
							ShopCard->AddChild(ShopCardLabel);
						}
					}
				}
			}
		}

		UVerticalBox* ActionPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("ActionPanel")));
		if (ActionPanel)
		{
			ActionPanel->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, ActionPanel);
			SetCanvasLayout(ActionPanel, 940.0, 130.0, 280.0, 300.0, 1);

			struct FButtonSpec
			{
				const TCHAR* Name;
				const TCHAR* Label;
			};

			const FButtonSpec Buttons[] = {
				{ TEXT("RefreshButton"), TEXT("Refresh") },
				{ TEXT("UpgradeTavernButton"), TEXT("Upgrade Tavern") },
				{ TEXT("ReadyButton"), TEXT("Ready For Combat") },
			};

			for (const FButtonSpec& ButtonSpec : Buttons)
			{
				UButton* Button = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, ActionPanel, UButton::StaticClass(), ButtonSpec.Name));
				if (!Button)
				{
					continue;
				}

				Button->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, Button);
				SetBoxSlotPadding(Button, FMargin(0.0f, 0.0f, 0.0f, 12.0f));
				UTextBlock* ButtonLabel = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*(FString(ButtonSpec.Name) + TEXT("Label"))));
				if (ButtonLabel)
				{
					ButtonLabel->SetText(FText::FromString(ButtonSpec.Label));
					EnsureWidgetBlueprintGuid(WidgetBlueprint, ButtonLabel);
					Button->AddChild(ButtonLabel);
				}
			}
		}

		return true;
	}

	bool BuildShopWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			OutFailureReason = TEXT("Widget Blueprint or WidgetTree is unavailable.");
			return false;
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		ClearWidgetBlueprintTree(WidgetBlueprint);

		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		if (!RootCanvas)
		{
			OutFailureReason = TEXT("Failed to create RootCanvas.");
			return false;
		}
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, RootCanvas);

		UVerticalBox* ShopPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("ShopPanel")));
		if (!ShopPanel)
		{
			OutFailureReason = TEXT("Failed to create ShopPanel.");
			return false;
		}
		ShopPanel->bIsVariable = true;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, ShopPanel);
		SetCanvasLayout(ShopPanel, 32.0, 32.0, 980.0, 260.0, 1);

		UTextBlock* HeaderText = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UTextBlock::StaticClass(), TEXT("ShopHeaderText")));
		if (HeaderText)
		{
			HeaderText->SetText(FText::FromString(TitleText.IsEmpty() ? TEXT("Tavern Shop") : TitleText));
			SetBoxSlotPadding(HeaderText, FMargin(0.0f, 0.0f, 0.0f, 12.0f));
		}

		UHorizontalBox* OfferSlots = Cast<UHorizontalBox>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UHorizontalBox::StaticClass(), TEXT("ShopOfferSlots")));
		if (OfferSlots)
		{
			OfferSlots->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, OfferSlots);
			for (int32 Index = 0; Index < 5; ++Index)
			{
				UButton* OfferButton = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, OfferSlots, UButton::StaticClass(), FString::Printf(TEXT("ShopOfferButton_%d"), Index + 1)));
				if (!OfferButton)
				{
					continue;
				}
				OfferButton->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, OfferButton);
				SetBoxSlotPadding(OfferButton, FMargin(0.0f, 0.0f, 10.0f, 0.0f));
				UTextBlock* OfferText = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*FString::Printf(TEXT("ShopOfferText_%d"), Index + 1)));
				if (OfferText)
				{
					OfferText->SetText(FText::FromString(FString::Printf(TEXT("Offer %d"), Index + 1)));
					EnsureWidgetBlueprintGuid(WidgetBlueprint, OfferText);
					OfferButton->AddChild(OfferText);
				}
			}
		}

		UHorizontalBox* ActionBar = Cast<UHorizontalBox>(AddTemplateWidget(WidgetBlueprint, ShopPanel, UHorizontalBox::StaticClass(), TEXT("ShopActionBar")));
		if (ActionBar)
		{
			ActionBar->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, ActionBar);

			const TCHAR* ButtonNames[] = { TEXT("RefreshButton"), TEXT("BuySelectedButton"), TEXT("SellSelectedButton") };
			const TCHAR* ButtonLabels[] = { TEXT("Refresh"), TEXT("Buy Selected"), TEXT("Sell Selected") };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				UButton* Button = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, ActionBar, UButton::StaticClass(), ButtonNames[Index]));
				if (!Button)
				{
					continue;
				}
				Button->bIsVariable = true;
				EnsureWidgetBlueprintGuid(WidgetBlueprint, Button);
				SetBoxSlotPadding(Button, FMargin(0.0f, 12.0f, 12.0f, 0.0f));
				UTextBlock* ButtonText = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(*(FString(ButtonNames[Index]) + TEXT("Label"))));
				if (ButtonText)
				{
					ButtonText->SetText(FText::FromString(ButtonLabels[Index]));
					EnsureWidgetBlueprintGuid(WidgetBlueprint, ButtonText);
					Button->AddChild(ButtonText);
				}
			}
		}

		return true;
	}

	bool BuildResultWidgetTemplate(UWidgetBlueprint* WidgetBlueprint, const FString& TitleText, FString& OutFailureReason)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
		{
			OutFailureReason = TEXT("Widget Blueprint or WidgetTree is unavailable.");
			return false;
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		ClearWidgetBlueprintTree(WidgetBlueprint);

		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
		if (!RootCanvas)
		{
			OutFailureReason = TEXT("Failed to create RootCanvas.");
			return false;
		}
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, RootCanvas);

		UVerticalBox* ResultPanel = Cast<UVerticalBox>(AddTemplateWidget(WidgetBlueprint, RootCanvas, UVerticalBox::StaticClass(), TEXT("ResultPanel")));
		if (!ResultPanel)
		{
			OutFailureReason = TEXT("Failed to create ResultPanel.");
			return false;
		}
		ResultPanel->bIsVariable = true;
		EnsureWidgetBlueprintGuid(WidgetBlueprint, ResultPanel);
		SetCanvasLayout(ResultPanel, 360.0, 160.0, 560.0, 360.0, 10);

		UTextBlock* Title = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ResultPanel, UTextBlock::StaticClass(), TEXT("ResultTitleText")));
		if (Title)
		{
			Title->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Title);
			Title->SetText(FText::FromString(TitleText.IsEmpty() ? TEXT("Combat Result") : TitleText));
			SetBoxSlotPadding(Title, FMargin(0.0f, 0.0f, 0.0f, 16.0f));
		}

		UTextBlock* Outcome = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ResultPanel, UTextBlock::StaticClass(), TEXT("OutcomeText")));
		if (Outcome)
		{
			Outcome->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Outcome);
			Outcome->SetText(FText::FromString(TEXT("Victory / Defeat")));
			SetBoxSlotPadding(Outcome, FMargin(0.0f, 0.0f, 0.0f, 10.0f));
		}

		UTextBlock* Damage = Cast<UTextBlock>(AddTemplateWidget(WidgetBlueprint, ResultPanel, UTextBlock::StaticClass(), TEXT("DamageText")));
		if (Damage)
		{
			Damage->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, Damage);
			Damage->SetText(FText::FromString(TEXT("Damage: 0")));
			SetBoxSlotPadding(Damage, FMargin(0.0f, 0.0f, 0.0f, 18.0f));
		}

		UButton* ContinueButton = Cast<UButton>(AddTemplateWidget(WidgetBlueprint, ResultPanel, UButton::StaticClass(), TEXT("ContinueButton")));
		if (ContinueButton)
		{
			ContinueButton->bIsVariable = true;
			EnsureWidgetBlueprintGuid(WidgetBlueprint, ContinueButton);
			UTextBlock* ButtonText = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ContinueButtonLabel"));
			if (ButtonText)
			{
				ButtonText->SetText(FText::FromString(TEXT("Continue")));
				EnsureWidgetBlueprintGuid(WidgetBlueprint, ButtonText);
				ContinueButton->AddChild(ButtonText);
			}
		}

		return true;
	}

	FString NormalizeScaffoldRootPath(const FString& RequestedRootPath)
	{
		FString RootPath = RequestedRootPath.TrimStartAndEnd();
		if (RootPath.IsEmpty())
		{
			RootPath = TEXT("/Game/ImperialTavern");
		}
		while (RootPath.EndsWith(TEXT("/")))
		{
			RootPath.LeftChopInline(1);
		}
		return RootPath;
	}

	FString MakeScaffoldPath(const FString& RootPath, const FString& RelativePath)
	{
		FString CleanRelative = RelativePath.TrimStartAndEnd();
		while (CleanRelative.StartsWith(TEXT("/")))
		{
			CleanRelative.RightChopInline(1);
		}
		return RootPath / CleanRelative;
	}

	void AddScaffoldRecord(TArray<TSharedPtr<FJsonValue>>& Array, const FString& Kind, const FString& Path, bool bCreated)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("kind"), Kind);
		Object->SetStringField(TEXT("path"), Path);
		Object->SetBoolField(TEXT("created"), bCreated);
		Array.Add(MakeShared<FJsonValueObject>(Object));
	}

	void AddScaffoldNamedRecord(TArray<TSharedPtr<FJsonValue>>& Array, const FString& Kind, const FString& OwnerPath, const FString& Name, bool bCreated)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("kind"), Kind);
		Object->SetStringField(TEXT("owner"), OwnerPath);
		Object->SetStringField(TEXT("name"), Name);
		Object->SetBoolField(TEXT("created"), bCreated);
		Array.Add(MakeShared<FJsonValueObject>(Object));
	}

	bool EnsureScaffoldDirectory(UEditorAssetSubsystem* EditorAssetSubsystem, const FString& DirectoryPath, TArray<TSharedPtr<FJsonValue>>& Directories, FString& OutFailureReason)
	{
		if (!EditorAssetSubsystem)
		{
			OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
			return false;
		}
		if (!DirectoryPath.StartsWith(TEXT("/Game")))
		{
			OutFailureReason = FString::Printf(TEXT("Scaffold directory '%s' must be under /Game."), *DirectoryPath);
			return false;
		}

		const bool bExists = EditorAssetSubsystem->DoesDirectoryExist(DirectoryPath);
		if (!bExists && !EditorAssetSubsystem->MakeDirectory(DirectoryPath))
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create directory '%s'."), *DirectoryPath);
			return false;
		}
		AddScaffoldRecord(Directories, TEXT("directory"), DirectoryPath, !bExists);
		return true;
	}

	UBlueprint* LoadOrCreateBlueprintScaffoldAsset(
		UEditorAssetSubsystem* EditorAssetSubsystem,
		const FString& AssetPath,
		const FString& ParentClassPath,
		FString& OutObjectPath,
		bool& bOutCreated,
		TArray<TSharedPtr<FJsonValue>>& Assets,
		FString& OutFailureReason)
	{
		bOutCreated = false;
		OutObjectPath.Reset();
		OutFailureReason.Reset();

		if (!EditorAssetSubsystem)
		{
			OutFailureReason = TEXT("EditorAssetSubsystem is unavailable.");
			return nullptr;
		}

		const FString ObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(AssetPath, OutFailureReason);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		OutObjectPath = ObjectPath;

		if (EditorAssetSubsystem->DoesAssetExist(ObjectPath))
		{
			UBlueprint* ExistingBlueprint = LoadBlueprintAsset(EditorAssetSubsystem, ObjectPath, OutObjectPath, OutFailureReason);
			if (!ExistingBlueprint)
			{
				return nullptr;
			}
			AddScaffoldRecord(Assets, TEXT("blueprint"), OutObjectPath, false);
			return ExistingBlueprint;
		}

		UClass* ParentClass = ResolveClassPath(ParentClassPath, EditorAssetSubsystem);
		if (!ParentClass)
		{
			OutFailureReason = FString::Printf(TEXT("Unable to resolve parent class '%s'."), *ParentClassPath);
			return nullptr;
		}
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			OutFailureReason = FString::Printf(TEXT("Cannot create a Blueprint from class '%s'."), *ParentClass->GetPathName());
			return nullptr;
		}
		if (!EditorScriptingHelpers::IsAValidPathForCreateNewAsset(ObjectPath, OutFailureReason))
		{
			OutFailureReason = FString::Printf(TEXT("Invalid asset path '%s': %s"), *ObjectPath, *OutFailureReason);
			return nullptr;
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		const FName AssetName(*FPackageName::GetLongPackageAssetName(PackageName));
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create package '%s'."), *PackageName);
			return nullptr;
		}

		UClass* BlueprintClass = nullptr;
		UClass* BlueprintGeneratedClass = nullptr;
		IKismetCompilerInterface& KismetCompilerModule = FModuleManager::LoadModuleChecked<IKismetCompilerInterface>(KISMET_COMPILER_MODULENAME);
		KismetCompilerModule.GetBlueprintTypesForClass(ParentClass, BlueprintClass, BlueprintGeneratedClass);

		UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			AssetName,
			BPTYPE_Normal,
			BlueprintClass,
			BlueprintGeneratedClass,
			FName(TEXT("UnrealMcpScaffold")));

		if (!NewBlueprint)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create Blueprint '%s'."), *ObjectPath);
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(NewBlueprint);
		Package->MarkPackageDirty();
		bOutCreated = true;
		AddScaffoldRecord(Assets, TEXT("blueprint"), ObjectPath, true);
		return NewBlueprint;
	}

	FEdGraphPinType MakeScaffoldPinType(const FName& PinCategory, EPinContainerType ContainerType = EPinContainerType::None, UObject* SubCategoryObject = nullptr)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = PinCategory;
		PinType.ContainerType = ContainerType;
		PinType.PinSubCategoryObject = SubCategoryObject;
		if (PinCategory == UEdGraphSchema_K2::PC_Real)
		{
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		return PinType;
	}

	bool EnsureBlueprintScaffoldVariable(
		UBlueprint* Blueprint,
		const FName& VariableName,
		const FEdGraphPinType& PinType,
		const FString& DefaultValue,
		TArray<TSharedPtr<FJsonValue>>& Variables,
		FString& OutFailureReason)
	{
		if (!Blueprint)
		{
			OutFailureReason = TEXT("Blueprint is null.");
			return false;
		}
		if (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, VariableName) != INDEX_NONE)
		{
			AddScaffoldNamedRecord(Variables, TEXT("variable"), Blueprint->GetPathName(), VariableName.ToString(), false);
			return true;
		}

		Blueprint->Modify();
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, VariableName, PinType, DefaultValue))
		{
			OutFailureReason = FString::Printf(TEXT("Failed to add variable '%s' to %s."), *VariableName.ToString(), *Blueprint->GetPathName());
			return false;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Blueprint->MarkPackageDirty();
		AddScaffoldNamedRecord(Variables, TEXT("variable"), Blueprint->GetPathName(), VariableName.ToString(), true);
		return true;
	}

	bool EnsureBlueprintScaffoldFunction(UBlueprint* Blueprint, const FString& FunctionName, TArray<TSharedPtr<FJsonValue>>& Functions, FString& OutFailureReason)
	{
		if (!Blueprint)
		{
			OutFailureReason = TEXT("Blueprint is null.");
			return false;
		}

		const FString CleanFunctionName = FunctionName.TrimStartAndEnd();
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName().Equals(CleanFunctionName, ESearchCase::IgnoreCase))
			{
				AddScaffoldNamedRecord(Functions, TEXT("function"), Blueprint->GetPathName(), Graph->GetName(), false);
				return true;
			}
		}

		Blueprint->Modify();
		UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			FName(*CleanFunctionName),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!FunctionGraph)
		{
			OutFailureReason = FString::Printf(TEXT("Failed to create function graph '%s' in %s."), *CleanFunctionName, *Blueprint->GetPathName());
			return false;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Blueprint->MarkPackageDirty();
		AddScaffoldNamedRecord(Functions, TEXT("function"), Blueprint->GetPathName(), FunctionGraph->GetName(), true);
		return true;
	}

	void SetBlueprintClassDefault(UBlueprint* Blueprint, const FName& PropertyName, UClass* ClassValue, TArray<TSharedPtr<FJsonValue>>& Defaults)
	{
		if (!Blueprint || !Blueprint->GeneratedClass || !ClassValue)
		{
			return;
		}
		UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
		FClassProperty* ClassProperty = CDO ? FindFProperty<FClassProperty>(CDO->GetClass(), PropertyName) : nullptr;
		if (!CDO || !ClassProperty)
		{
			return;
		}

		CDO->Modify();
		CDO->PreEditChange(ClassProperty);
		ClassProperty->SetPropertyValue_InContainer(CDO, ClassValue);
		FPropertyChangedEvent PropertyChangedEvent(ClassProperty, EPropertyChangeType::ValueSet);
		CDO->PostEditChangeProperty(PropertyChangedEvent);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Object->SetStringField(TEXT("property"), PropertyName.ToString());
		Object->SetStringField(TEXT("class"), ClassValue->GetPathName());
		Defaults.Add(MakeShared<FJsonValueObject>(Object));
	}

	void FinalizeScaffoldBlueprint(UBlueprint* Blueprint, UEditorAssetSubsystem* EditorAssetSubsystem, bool bCompile, bool bSavePackage, TArray<TSharedPtr<FJsonValue>>& Finalized)
	{
		if (!Blueprint)
		{
			return;
		}

		bool bCompileSucceeded = true;
		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			bCompileSucceeded = Blueprint->Status != BS_Error;
		}

		bool bSaved = false;
		if (bSavePackage && EditorAssetSubsystem)
		{
			bSaved = EditorAssetSubsystem->SaveLoadedAsset(Blueprint, false);
		}

		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Object->SetBoolField(TEXT("compiled"), bCompile);
		Object->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
		Object->SetBoolField(TEXT("saved"), bSaved);
		Finalized.Add(MakeShared<FJsonValueObject>(Object));
	}

	TSharedPtr<FJsonObject> MakeScaffoldStructuredContent(
		const FString& Action,
		const FString& RootPath,
		const TArray<TSharedPtr<FJsonValue>>& Directories,
		const TArray<TSharedPtr<FJsonValue>>& Assets,
		const TArray<TSharedPtr<FJsonValue>>& Variables,
		const TArray<TSharedPtr<FJsonValue>>& Functions,
		const TArray<TSharedPtr<FJsonValue>>& Defaults,
		const TArray<TSharedPtr<FJsonValue>>& Finalized,
		const TArray<TSharedPtr<FJsonValue>>& NextSteps)
	{
		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), Action);
		StructuredContent->SetStringField(TEXT("rootPath"), RootPath);
		StructuredContent->SetArrayField(TEXT("directories"), Directories);
		StructuredContent->SetArrayField(TEXT("assets"), Assets);
		StructuredContent->SetArrayField(TEXT("variables"), Variables);
		StructuredContent->SetArrayField(TEXT("functions"), Functions);
		StructuredContent->SetArrayField(TEXT("classDefaults"), Defaults);
		StructuredContent->SetArrayField(TEXT("finalized"), Finalized);
		StructuredContent->SetArrayField(TEXT("nextSteps"), NextSteps);
		return StructuredContent;
	}

	void AddNextStep(TArray<TSharedPtr<FJsonValue>>& NextSteps, const FString& Text)
	{
		NextSteps.Add(MakeShared<FJsonValueString>(Text));
	}

	FUnrealMcpExecutionResult ScaffoldRoundSystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Core")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems")),
			MakeScaffoldPath(RootPath, TEXT("Data")),
			MakeScaffoldPath(RootPath, TEXT("Maps")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		auto CreateBlueprint = [&](const FString& RelativePath, const FString& ParentClassPath) -> UBlueprint*
		{
			FString ObjectPath;
			bool bCreated = false;
			return LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, RelativePath), ParentClassPath, ObjectPath, bCreated, Assets, FailureReason);
		};

		UBlueprint* GameMode = CreateBlueprint(TEXT("Blueprints/Core/BP_IT_GameMode"), TEXT("/Script/Engine.GameModeBase"));
		UBlueprint* GameState = CreateBlueprint(TEXT("Blueprints/Core/BP_IT_GameState"), TEXT("/Script/Engine.GameStateBase"));
		UBlueprint* PlayerState = CreateBlueprint(TEXT("Blueprints/Core/BP_IT_PlayerState"), TEXT("/Script/Engine.PlayerState"));
		UBlueprint* PlayerController = CreateBlueprint(TEXT("Blueprints/Core/BP_IT_PlayerController"), TEXT("/Script/Engine.PlayerController"));
		UBlueprint* RoundManager = CreateBlueprint(TEXT("Blueprints/Systems/BP_IT_RoundManagerComponent"), TEXT("/Script/Engine.ActorComponent"));
		if (!GameMode || !GameState || !PlayerState || !PlayerController || !RoundManager)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		EnsureBlueprintScaffoldVariable(GameState, TEXT("CurrentRound"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(GameState, TEXT("CurrentPhase"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name), TEXT("Initialization"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(GameState, TEXT("PreparationDuration"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("45.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(GameState, TEXT("CombatDuration"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("90.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(PlayerState, TEXT("PlayerHealth"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("100"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(PlayerState, TEXT("bEliminated"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Boolean), TEXT("false"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(RoundManager, TEXT("CurrentRound"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(RoundManager, TEXT("CurrentPhase"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name), TEXT("Initialization"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(RoundManager, TEXT("RoundPairings"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);

		const FString RoundFunctions[] = {
			TEXT("StartMatchFlow"),
			TEXT("AdvanceRound"),
			TEXT("BeginPreparationPhase"),
			TEXT("BeginCombatPhase"),
			TEXT("BeginResolutionPhase"),
			TEXT("PairPlayersForCombat"),
			TEXT("ApplyRoundDamage"),
			TEXT("CheckVictoryCondition"),
		};
		for (const FString& FunctionName : RoundFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(RoundManager, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		TArray<UBlueprint*> Blueprints = { GameState, PlayerState, PlayerController, RoundManager, GameMode };
		for (UBlueprint* Blueprint : Blueprints)
		{
			FinalizeScaffoldBlueprint(Blueprint, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		}

		SetBlueprintClassDefault(GameMode, TEXT("GameStateClass"), GameState->GeneratedClass, Defaults);
		SetBlueprintClassDefault(GameMode, TEXT("PlayerStateClass"), PlayerState->GeneratedClass, Defaults);
		SetBlueprintClassDefault(GameMode, TEXT("PlayerControllerClass"), PlayerController->GeneratedClass, Defaults);
		if (bSavePackage)
		{
			EditorAssetSubsystem->SaveLoadedAsset(GameMode, false);
		}

		AddNextStep(NextSteps, TEXT("Attach BP_IT_RoundManagerComponent to BP_IT_GameMode or a dedicated arena actor, then implement phase transition logic in the generated function graphs."));
		AddNextStep(NextSteps, TEXT("Set LVL_ImperialTavern_MVP World Settings to BP_IT_GameMode if it is not already selected."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_round_system"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded Imperial Tavern round system under %s."), *RootPath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ScaffoldShopSystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		bool bReplaceWidgetRoot = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);
		Arguments.TryGetBoolField(TEXT("replaceWidgetRoot"), bReplaceWidgetRoot);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/UI")),
			MakeScaffoldPath(RootPath, TEXT("Data")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FString ObjectPath;
		bool bCreated = false;
		UBlueprint* ShopManager = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems/BP_IT_ShopManagerComponent")), TEXT("/Script/Engine.ActorComponent"), ObjectPath, bCreated, Assets, FailureReason);
		if (!ShopManager)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("ShopOfferCardIds"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("OwnedCardInstanceIds"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("RefreshCostFood"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("1"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("CardCostFood"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("3"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("ShopSize"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("5"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("PityRefreshLimit"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("13"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ShopManager, TEXT("RefreshesWithoutDuplicate"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);

		const FString ShopFunctions[] = {
			TEXT("RefreshShop"),
			TEXT("GenerateShopOffers"),
			TEXT("BuyShopOffer"),
			TEXT("SellOwnedCard"),
			TEXT("MoveCardBetweenZones"),
			TEXT("CheckTripleCandidates"),
			TEXT("ApplyPityOffer"),
			TEXT("ClearShopOffers"),
		};
		for (const FString& FunctionName : ShopFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(ShopManager, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}
		FinalizeScaffoldBlueprint(ShopManager, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		FString WidgetObjectPath;
		bool bWidgetCreated = false;
		UWidgetBlueprint* ShopWidget = LoadOrCreateWidgetBlueprintAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/UI/WBP_IT_ShopPanel")), WidgetObjectPath, bWidgetCreated, FailureReason);
		if (!ShopWidget)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		AddScaffoldRecord(Assets, TEXT("widget_blueprint"), WidgetObjectPath, bWidgetCreated);
		if (bReplaceWidgetRoot || !ShopWidget->WidgetTree || !ShopWidget->WidgetTree->RootWidget)
		{
			if (!BuildShopWidgetTemplate(ShopWidget, TEXT("Tavern Shop"), FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			MarkWidgetBlueprintModified(ShopWidget, true);
		}
		FinalizeScaffoldBlueprint(ShopWidget, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		AddNextStep(NextSteps, TEXT("Connect RefreshButton.OnClicked to BP_IT_ShopManagerComponent.RefreshShop with unreal.widget_bind_event or manual Blueprint wiring."));
		AddNextStep(NextSteps, TEXT("Replace Name arrays with FCardInstanceData/FCardData structs once the C++ or Blueprint struct layer exists."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_shop_system"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded Imperial Tavern shop system under %s."), *RootPath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ScaffoldEconomySystem(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Core")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FString ObjectPath;
		bool bCreated = false;
		UBlueprint* EconomyComponent = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems/BP_IT_EconomyComponent")), TEXT("/Script/Engine.ActorComponent"), ObjectPath, bCreated, Assets, FailureReason);
		UBlueprint* PlayerState = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Core/BP_IT_PlayerState")), TEXT("/Script/Engine.PlayerState"), ObjectPath, bCreated, Assets, FailureReason);
		if (!EconomyComponent || !PlayerState)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		UBlueprint* EconomyTargets[] = { EconomyComponent, PlayerState };
		for (UBlueprint* Target : EconomyTargets)
		{
			EnsureBlueprintScaffoldVariable(Target, TEXT("CurrentFood"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("3"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("MaxFood"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("10"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("CurrentGold"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("1"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("TavernLevel"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("1"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("WinStreak"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
			EnsureBlueprintScaffoldVariable(Target, TEXT("LossStreak"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		}

		const FString EconomyFunctions[] = {
			TEXT("InitializeEconomy"),
			TEXT("ApplyRoundIncome"),
			TEXT("ResetRoundFood"),
			TEXT("SpendFood"),
			TEXT("SpendGold"),
			TEXT("AddFood"),
			TEXT("AddGold"),
			TEXT("UpgradeTavern"),
			TEXT("ApplyStreakBonus"),
		};
		for (const FString& FunctionName : EconomyFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(EconomyComponent, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FinalizeScaffoldBlueprint(EconomyComponent, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		FinalizeScaffoldBlueprint(PlayerState, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		AddNextStep(NextSteps, TEXT("Attach BP_IT_EconomyComponent to PlayerController or PlayerState once ownership is decided."));
		AddNextStep(NextSteps, TEXT("Move server validation into PlayerController RPCs after the resource functions have Blueprint bodies."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_economy_system"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded Imperial Tavern economy system under %s."), *RootPath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ScaffoldAutobattlerAi(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Units")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/AI")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Combat")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FString ObjectPath;
		bool bCreated = false;
		UBlueprint* UnitBase = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Units/BP_IT_UnitBase")), TEXT("/Script/Engine.Character"), ObjectPath, bCreated, Assets, FailureReason);
		UBlueprint* AIController = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/AI/BP_IT_AutoBattleAIController")), TEXT("/Script/AIModule.AIController"), ObjectPath, bCreated, Assets, FailureReason);
		UBlueprint* CombatManager = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Combat/BP_IT_CombatManager")), TEXT("/Script/Engine.Actor"), ObjectPath, bCreated, Assets, FailureReason);
		if (!UnitBase || !AIController || !CombatManager)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("TeamId"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("UnitHealth"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("100.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("UnitDamage"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("10.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("AttackRange"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("160.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("AttackCooldown"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Real), TEXT("1.0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(UnitBase, TEXT("SourceCardInstanceId"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(CombatManager, TEXT("TeamAUnitIds"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(CombatManager, TEXT("TeamBUnitIds"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name, EPinContainerType::Array), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(CombatManager, TEXT("bCombatRunning"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Boolean), TEXT("false"), Variables, FailureReason);

		const FString UnitFunctions[] = {
			TEXT("FindNearestEnemy"),
			TEXT("MoveToEnemy"),
			TEXT("PerformAttack"),
			TEXT("ApplyDamage"),
			TEXT("HandleDeath"),
		};
		for (const FString& FunctionName : UnitFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(UnitBase, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		const FString CombatFunctions[] = {
			TEXT("SpawnTeamsFromFieldCards"),
			TEXT("StartCombat"),
			TEXT("TickCombatState"),
			TEXT("ResolveCombat"),
			TEXT("CalculateSurvivorDamage"),
			TEXT("CleanupCombatUnits"),
		};
		for (const FString& FunctionName : CombatFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(CombatManager, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FinalizeScaffoldBlueprint(AIController, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		FinalizeScaffoldBlueprint(UnitBase, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		FinalizeScaffoldBlueprint(CombatManager, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);
		SetBlueprintClassDefault(UnitBase, TEXT("AIControllerClass"), AIController->GeneratedClass, Defaults);
		if (bSavePackage)
		{
			EditorAssetSubsystem->SaveLoadedAsset(UnitBase, false);
		}

		AddNextStep(NextSteps, TEXT("Implement FindNearestEnemy with TeamId filtering, then wire MoveToEnemy and PerformAttack from Tick or a Behavior Tree."));
		AddNextStep(NextSteps, TEXT("Keep spawned unit counts small until combat rules are proven; move to pooling or Mass later if needed."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_autobattler_ai"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded Imperial Tavern autobattler AI under %s."), *RootPath), StructuredContent, false);
	}

	FUnrealMcpExecutionResult ScaffoldResultUi(UEditorAssetSubsystem* EditorAssetSubsystem, const FJsonObject& Arguments)
	{
		FString RootArg;
		Arguments.TryGetStringField(TEXT("rootPath"), RootArg);
		const FString RootPath = NormalizeScaffoldRootPath(RootArg);
		bool bCompile = true;
		bool bSavePackage = true;
		bool bReplaceWidgetRoot = true;
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);
		Arguments.TryGetBoolField(TEXT("replaceWidgetRoot"), bReplaceWidgetRoot);

		TArray<TSharedPtr<FJsonValue>> Directories;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Functions;
		TArray<TSharedPtr<FJsonValue>> Defaults;
		TArray<TSharedPtr<FJsonValue>> Finalized;
		TArray<TSharedPtr<FJsonValue>> NextSteps;
		FString FailureReason;

		const FString Folders[] = {
			RootPath,
			MakeScaffoldPath(RootPath, TEXT("Blueprints/UI")),
			MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems")),
		};
		for (const FString& Folder : Folders)
		{
			if (!EnsureScaffoldDirectory(EditorAssetSubsystem, Folder, Directories, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}

		FString ObjectPath;
		bool bCreated = false;
		UBlueprint* ResultPresenter = LoadOrCreateBlueprintScaffoldAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/Systems/BP_IT_ResultPresenterComponent")), TEXT("/Script/Engine.ActorComponent"), ObjectPath, bCreated, Assets, FailureReason);
		if (!ResultPresenter)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		EnsureBlueprintScaffoldVariable(ResultPresenter, TEXT("LastOutcome"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Name), FString(), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ResultPresenter, TEXT("LastDamage"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Int), TEXT("0"), Variables, FailureReason);
		EnsureBlueprintScaffoldVariable(ResultPresenter, TEXT("bResultVisible"), MakeScaffoldPinType(UEdGraphSchema_K2::PC_Boolean), TEXT("false"), Variables, FailureReason);

		const FString ResultFunctions[] = {
			TEXT("ShowCombatResult"),
			TEXT("HideCombatResult"),
			TEXT("SetResultSummary"),
			TEXT("BindContinueButton"),
			TEXT("ContinueToPreparation"),
		};
		for (const FString& FunctionName : ResultFunctions)
		{
			if (!EnsureBlueprintScaffoldFunction(ResultPresenter, FunctionName, Functions, FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
		}
		FinalizeScaffoldBlueprint(ResultPresenter, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		FString WidgetObjectPath;
		bool bWidgetCreated = false;
		UWidgetBlueprint* ResultWidget = LoadOrCreateWidgetBlueprintAsset(EditorAssetSubsystem, MakeScaffoldPath(RootPath, TEXT("Blueprints/UI/WBP_IT_ResultPanel")), WidgetObjectPath, bWidgetCreated, FailureReason);
		if (!ResultWidget)
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}
		AddScaffoldRecord(Assets, TEXT("widget_blueprint"), WidgetObjectPath, bWidgetCreated);
		if (bReplaceWidgetRoot || !ResultWidget->WidgetTree || !ResultWidget->WidgetTree->RootWidget)
		{
			if (!BuildResultWidgetTemplate(ResultWidget, TEXT("Combat Result"), FailureReason))
			{
				return MakeExecutionResult(FailureReason, nullptr, true);
			}
			MarkWidgetBlueprintModified(ResultWidget, true);
		}
		FinalizeScaffoldBlueprint(ResultWidget, EditorAssetSubsystem, bCompile, bSavePackage, Finalized);

		AddNextStep(NextSteps, TEXT("Bind ContinueButton.OnClicked to ContinueToPreparation, then have RoundManager call ShowCombatResult after ResolveCombat."));
		AddNextStep(NextSteps, TEXT("Feed OutcomeText and DamageText from combat resolution data once the result struct exists."));

		TSharedPtr<FJsonObject> StructuredContent = MakeScaffoldStructuredContent(TEXT("scaffold_result_ui"), RootPath, Directories, Assets, Variables, Functions, Defaults, Finalized, NextSteps);
		return MakeExecutionResult(FString::Printf(TEXT("Scaffolded Imperial Tavern result UI under %s."), *RootPath), StructuredContent, false);
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
			TEXT("Prefer AI-safe wrapper tools such as spawn_actor_basic, spawn_actor_batch_basic, spawn_static_mesh_actor, batch_set_actor_scale, batch_set_actor_tags, batch_set_point_light_properties, batch_configure_static_mesh_actors, bp_* Blueprint graph editing tools, widget_* UMG editing tools, and scaffold_* gameplay scaffold tools before falling back to execute_python. ")
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
		TWeakPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> WeakThis = AsShared();
		StreamDelegate.BindLambda(
			[WeakThis](void* Ptr, int64& InOutLength)
			{
				if (const TSharedPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> PinnedThis = WeakThis.Pin())
				{
					PinnedThis->ConsumeResponseBytes(Ptr, InOutLength);
				}
			});
		Request->SetResponseBodyReceiveStreamDelegateV2(StreamDelegate);

		Request->OnProcessRequestComplete().BindLambda(
			[WeakThis](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded)
			{
				if (const TSharedPtr<FUnrealMcpAssistantRun, ESPMode::ThreadSafe> PinnedThis = WeakThis.Pin())
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
	RegisterTabSpawner();
	UToolMenus::Get()->RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealMcpModule::RegisterMenus));
}

void FUnrealMcpModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	UnregisterTabSpawner();
	StopServer();
}

bool FUnrealMcpModule::StartServer()
{
	const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
	if (!Settings->bEnableServer)
	{
		UE_LOG(LogUnrealMcp, Log, TEXT("Unreal MCP server is disabled in settings."));
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
}

void FUnrealMcpModule::UnregisterTabSpawner()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UnrealMcp::ChatTabName);
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
	}
}

void FUnrealMcpModule::OpenChatTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(UnrealMcp::ChatTabName);
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
			PropertiesObject->SetObjectField(TEXT("command"), UnrealMcp::MakeStringProperty(TEXT("Literal Python code, an expression, or a .py file path with optional arguments.")));
			PropertiesObject->SetObjectField(TEXT("mode"), UnrealMcp::MakeStringProperty(TEXT("Python execution mode: ExecuteFile, ExecuteStatement, or EvaluateStatement."), TEXT("ExecuteFile")));
			PropertiesObject->SetObjectField(TEXT("scope"), UnrealMcp::MakeStringProperty(TEXT("Python file execution scope: Private or Public."), TEXT("Private")));
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
			PropertiesObject->SetObjectField(TEXT("templateName"), UnrealMcp::MakeStringProperty(TEXT("Template preset name. Currently supports imperial_tavern_mvp."), TEXT("imperial_tavern_mvp")));
			PropertiesObject->SetObjectField(TEXT("title"), UnrealMcp::MakeStringProperty(TEXT("Title text for the generated template."), TEXT("Imperial Tavern")));
			PropertiesObject->SetObjectField(TEXT("replaceRoot"), UnrealMcp::MakeBoolProperty(TEXT("Whether to replace the existing root widget tree."), true));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile after building the template. For large first-time templates, prefer false then call unreal.bp_compile_save."), false));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save the Widget Blueprint package after building. For large first-time templates, prefer false then call unreal.bp_compile_save."), false));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.widget_build_template"),
				TEXT("Widget Build Template"),
				TEXT("Creates or rebuilds a Widget Blueprint with a practical Imperial Tavern MVP HUD hierarchy."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/ImperialTavern")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_round_system"),
				TEXT("Scaffold Round System"),
				TEXT("Creates or updates high-level Imperial Tavern round-flow Blueprint scaffolding: GameMode, GameState, PlayerState, PlayerController, and RoundManagerComponent."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/ImperialTavern")));
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
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/ImperialTavern")));
			PropertiesObject->SetObjectField(TEXT("compile"), UnrealMcp::MakeBoolProperty(TEXT("Whether to compile scaffolded Blueprints."), true));
			PropertiesObject->SetObjectField(TEXT("savePackage"), UnrealMcp::MakeBoolProperty(TEXT("Whether to save scaffolded packages."), true));

			TSharedPtr<FJsonObject> InputSchema = UnrealMcp::MakeObjectSchema();
			InputSchema->SetObjectField(TEXT("properties"), PropertiesObject);

			UnrealMcp::AddToolDefinition(
				ToolsArray,
				TEXT("unreal.scaffold_economy_system"),
				TEXT("Scaffold Economy System"),
				TEXT("Creates or updates food/gold/tavern-level economy Blueprint component scaffolding and matching PlayerState variables."),
				InputSchema);
		}

		{
			TSharedPtr<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/ImperialTavern")));
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
			PropertiesObject->SetObjectField(TEXT("rootPath"), UnrealMcp::MakeStringProperty(TEXT("Feature root content path."), TEXT("/Game/ImperialTavern")));
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

FUnrealMcpExecutionResult FUnrealMcpModule::ExecuteTool(const FString& ToolName, const FJsonObject& Arguments) const
{
	UEditorAssetSubsystem* EditorAssetSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	UEditorActorSubsystem* EditorActorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

		if (ToolName == TEXT("unreal.editor_status"))
		{
			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const FString CurrentMap = EditorWorld ? EditorWorld->GetOutermost()->GetName() : TEXT("");
			const bool bIsPIE = GEditor && GEditor->PlayWorld != nullptr;
			const bool bIsSimulating = GEditor && GEditor->bIsSimulatingInEditor;
			const bool bPlayRequestPending = GEditor && GEditor->GetPlaySessionRequest().IsSet();
			const FString EngineVersion = FEngineVersion::Current().ToString();
			const TArray<FAssetData> SelectedAssets = UnrealMcp::GetSelectedAssets();
			const int32 SelectedActorCount = EditorActorSubsystem ? EditorActorSubsystem->GetSelectedLevelActors().Num() : 0;

		const UUnrealMcpSettings* Settings = GetDefault<UUnrealMcpSettings>();
		const FString EndpointUrl = FString::Printf(TEXT("http://127.0.0.1:%d%s"), Settings->Port, *UnrealMcp::NormalizeEndpointPath(Settings->EndpointPath));

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		StructuredContent->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
			StructuredContent->SetStringField(TEXT("engineVersion"), EngineVersion);
			StructuredContent->SetStringField(TEXT("currentMap"), CurrentMap);
			StructuredContent->SetBoolField(TEXT("isPlayInEditor"), bIsPIE);
			StructuredContent->SetBoolField(TEXT("isSimulatingInEditor"), bIsSimulating);
			StructuredContent->SetBoolField(TEXT("playRequestPending"), bPlayRequestPending);
			StructuredContent->SetNumberField(TEXT("selectedAssetCount"), SelectedAssets.Num());
			StructuredContent->SetNumberField(TEXT("selectedActorCount"), SelectedActorCount);
			StructuredContent->SetStringField(TEXT("endpoint"), EndpointUrl);

			const FString Text = FString::Printf(
				TEXT("Project: %s\nEngine: %s\nMap: %s\nPIE: %s\nSimulating: %s\nPlay request pending: %s\nSelected assets: %d\nSelected actors: %d\nEndpoint: %s"),
				FApp::GetProjectName(),
				*EngineVersion,
				CurrentMap.IsEmpty() ? TEXT("<none>") : *CurrentMap,
				bIsPIE ? TEXT("true") : TEXT("false"),
				bIsSimulating ? TEXT("true") : TEXT("false"),
				bPlayRequestPending ? TEXT("true") : TEXT("false"),
				SelectedAssets.Num(),
				SelectedActorCount,
				*EndpointUrl);

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
		}

		if (ToolName == TEXT("unreal.start_pie"))
		{
			if (!GEditor)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("GEditor is unavailable."), nullptr, true);
			}

			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("A Play In Editor session is already active or queued."), nullptr, true);
			}

			bool bSimulate = false;
			Arguments.TryGetBoolField(TEXT("simulate"), bSimulate);

			FRequestPlaySessionParams SessionParams;
			if (bSimulate)
			{
				SessionParams.WorldType = EPlaySessionWorldType::SimulateInEditor;
			}

			GEditor->RequestPlaySession(SessionParams);

			const bool bQueued = GEditor->GetPlaySessionRequest().IsSet();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetBoolField(TEXT("requested"), true);
			StructuredContent->SetBoolField(TEXT("simulate"), bSimulate);
			StructuredContent->SetBoolField(TEXT("playRequestPending"), bQueued);

			return UnrealMcp::MakeExecutionResult(
				FString::Printf(
					TEXT("Requested %s session. queued=%s"),
					bSimulate ? TEXT("Simulate In Editor") : TEXT("Play In Editor"),
					bQueued ? TEXT("true") : TEXT("false")),
				StructuredContent,
				false);
		}

		if (ToolName == TEXT("unreal.stop_pie"))
		{
			if (!GEditor)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("GEditor is unavailable."), nullptr, true);
			}

			const bool bWasPIE = GEditor->PlayWorld != nullptr;
			const bool bWasSimulating = GEditor->bIsSimulatingInEditor;
			const bool bHadQueuedRequest = GEditor->GetPlaySessionRequest().IsSet();
			if (!bWasPIE && !bWasSimulating && !bHadQueuedRequest)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("No Play In Editor session is running or queued."), nullptr, true);
			}

			GEditor->RequestEndPlayMap();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetBoolField(TEXT("requested"), true);
			StructuredContent->SetBoolField(TEXT("wasPlayInEditor"), bWasPIE);
			StructuredContent->SetBoolField(TEXT("wasSimulatingInEditor"), bWasSimulating);
			StructuredContent->SetBoolField(TEXT("hadQueuedPlayRequest"), bHadQueuedRequest);

			return UnrealMcp::MakeExecutionResult(TEXT("Requested Play In Editor shutdown."), StructuredContent, false);
		}

		if (ToolName == TEXT("unreal.execute_console_command"))
		{
			FString Command;
			if (!Arguments.TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'command'."), nullptr, true);
			}

			FString RequestedTarget = TEXT("auto");
			Arguments.TryGetStringField(TEXT("target"), RequestedTarget);

			FString ResolvedTarget;
			FString FailureReason;
			UWorld* TargetWorld = UnrealMcp::ResolveConsoleWorld(RequestedTarget, ResolvedTarget, FailureReason);
			if (!TargetWorld)
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}

			FStringOutputDevice OutputDevice;
			const bool bExecuted = GEditor && GEditor->Exec(TargetWorld, *Command, OutputDevice);
			const FString CapturedOutput = FString(OutputDevice).TrimStartAndEnd();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("command"), Command);
			StructuredContent->SetStringField(TEXT("target"), ResolvedTarget);
			StructuredContent->SetStringField(TEXT("worldPath"), TargetWorld->GetPathName());
			StructuredContent->SetBoolField(TEXT("success"), bExecuted);
			StructuredContent->SetStringField(TEXT("output"), CapturedOutput);

			FString Text = FString::Printf(
				TEXT("Console command '%s' executed on %s world. success=%s"),
				*Command,
				*ResolvedTarget,
				bExecuted ? TEXT("true") : TEXT("false"));
			if (!CapturedOutput.IsEmpty())
			{
				Text += FString::Printf(TEXT("\n%s"), *CapturedOutput);
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, !bExecuted);
		}

		if (ToolName == TEXT("unreal.execute_python_file"))
		{
			FString ScriptPath;
			if (!Arguments.TryGetStringField(TEXT("scriptPath"), ScriptPath) || ScriptPath.TrimStartAndEnd().IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'scriptPath'."), nullptr, true);
			}

			TArray<FString> ScriptArgs;
			UnrealMcp::TryGetStringArrayField(Arguments, TEXT("args"), ScriptArgs);

			bool bAllowOutsideProject = false;
			Arguments.TryGetBoolField(TEXT("allowOutsideProject"), bAllowOutsideProject);

			FString ResolvedScriptPath;
			FString ResolveFailureReason;
			if (!UnrealMcp::ResolvePythonScriptPath(ScriptPath, bAllowOutsideProject, ResolvedScriptPath, ResolveFailureReason))
			{
				return UnrealMcp::MakeExecutionResult(ResolveFailureReason, nullptr, true);
			}

			FString Command = UnrealMcp::QuoteShellArgument(ResolvedScriptPath);
			for (const FString& ScriptArg : ScriptArgs)
			{
				Command += TEXT(" ");
				Command += UnrealMcp::QuoteShellArgument(ScriptArg);
			}

			FString ScopeString = TEXT("Private");
			bool bForceEnable = true;
			bool bUnattended = true;
			Arguments.TryGetStringField(TEXT("scope"), ScopeString);
			Arguments.TryGetBoolField(TEXT("forceEnable"), bForceEnable);
			Arguments.TryGetBoolField(TEXT("unattended"), bUnattended);

			TSharedPtr<FJsonObject> ForwardArguments = UnrealMcp::MakeEmptyObject();
			ForwardArguments->SetStringField(TEXT("command"), Command);
			ForwardArguments->SetStringField(TEXT("mode"), TEXT("ExecuteFile"));
			ForwardArguments->SetStringField(TEXT("scope"), ScopeString);
			ForwardArguments->SetBoolField(TEXT("forceEnable"), bForceEnable);
			ForwardArguments->SetBoolField(TEXT("unattended"), bUnattended);

			FUnrealMcpExecutionResult ExecutionResult = ExecuteTool(TEXT("unreal.execute_python"), *ForwardArguments);
			if (ExecutionResult.StructuredContent.IsValid())
			{
				ExecutionResult.StructuredContent->SetStringField(TEXT("scriptPath"), ResolvedScriptPath);

				TArray<TSharedPtr<FJsonValue>> ArgsArray;
				for (const FString& ScriptArg : ScriptArgs)
				{
					ArgsArray.Add(MakeShared<FJsonValueString>(ScriptArg));
				}
				ExecutionResult.StructuredContent->SetArrayField(TEXT("args"), ArgsArray);
				ExecutionResult.StructuredContent->SetBoolField(TEXT("allowOutsideProject"), bAllowOutsideProject);
			}

			ExecutionResult.Text = FString::Printf(TEXT("Executed Python script file %s.\n%s"), *ResolvedScriptPath, *ExecutionResult.Text);
			return ExecutionResult;
		}

		if (ToolName == TEXT("unreal.execute_python"))
		{
			FString Command;
			if (!Arguments.TryGetStringField(TEXT("command"), Command) || Command.TrimStartAndEnd().IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'command'."), nullptr, true);
			}

			FString ModeString = TEXT("ExecuteFile");
			FString ScopeString = TEXT("Private");
			bool bForceEnable = true;
			bool bUnattended = true;
			Arguments.TryGetStringField(TEXT("mode"), ModeString);
			Arguments.TryGetStringField(TEXT("scope"), ScopeString);
			Arguments.TryGetBoolField(TEXT("forceEnable"), bForceEnable);
			Arguments.TryGetBoolField(TEXT("unattended"), bUnattended);

			EPythonCommandExecutionMode ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
			if (!LexTryParseString(ExecutionMode, *ModeString))
			{
				return UnrealMcp::MakeExecutionResult(
					FString::Printf(TEXT("Unknown Python execution mode '%s'. Use ExecuteFile, ExecuteStatement, or EvaluateStatement."), *ModeString),
					nullptr,
					true);
			}

			EPythonFileExecutionScope FileExecutionScope = EPythonFileExecutionScope::Private;
			if (!UnrealMcp::TryParsePythonFileExecutionScope(ScopeString, FileExecutionScope))
			{
				return UnrealMcp::MakeExecutionResult(
					FString::Printf(TEXT("Unknown Python scope '%s'. Use Private or Public."), *ScopeString),
					nullptr,
					true);
			}

			IPythonScriptPlugin* PythonPlugin = UnrealMcp::LoadPythonScriptPlugin();
			if (!PythonPlugin)
			{
				return UnrealMcp::MakeExecutionResult(
					TEXT("PythonScriptPlugin is not loaded. Enable the Python Script Plugin for the editor and restart Unreal Editor."),
					nullptr,
					true);
			}

			if (bForceEnable && !PythonPlugin->IsPythonInitialized())
			{
				PythonPlugin->ForceEnablePythonAtRuntime();
			}

			if (!PythonPlugin->IsPythonAvailable())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Python support is not available in the current editor session."), nullptr, true);
			}

			if (!PythonPlugin->IsPythonInitialized())
			{
				return UnrealMcp::MakeExecutionResult(
					TEXT("Python is not initialized. Re-open the editor after enabling the Python Script Plugin, or retry with forceEnable=true."),
					nullptr,
					true);
			}

			FPythonCommandEx PythonCommand;
			PythonCommand.Command = Command;
			PythonCommand.ExecutionMode = ExecutionMode;
			PythonCommand.FileExecutionScope = FileExecutionScope;
			PythonCommand.Flags = bUnattended ? EPythonCommandFlags::Unattended : EPythonCommandFlags::None;

			const bool bSucceeded = PythonPlugin->ExecPythonCommandEx(PythonCommand);

			TArray<TSharedPtr<FJsonValue>> LogOutputArray;
			TArray<FString> LogLines;
			LogLines.Reserve(PythonCommand.LogOutput.Num());

			for (const FPythonLogOutputEntry& LogEntry : PythonCommand.LogOutput)
			{
				TSharedPtr<FJsonObject> LogObject = MakeShared<FJsonObject>();
				LogObject->SetStringField(TEXT("type"), LexToString(LogEntry.Type));
				LogObject->SetStringField(TEXT("output"), LogEntry.Output);
				LogOutputArray.Add(MakeShared<FJsonValueObject>(LogObject));
				LogLines.Add(FString::Printf(TEXT("[%s] %s"), LexToString(LogEntry.Type), *LogEntry.Output));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("command"), Command);
			StructuredContent->SetStringField(TEXT("mode"), LexToString(ExecutionMode));
			StructuredContent->SetStringField(TEXT("scope"), FileExecutionScope == EPythonFileExecutionScope::Public ? TEXT("Public") : TEXT("Private"));
			StructuredContent->SetBoolField(TEXT("forceEnable"), bForceEnable);
			StructuredContent->SetBoolField(TEXT("unattended"), bUnattended);
			StructuredContent->SetBoolField(TEXT("success"), bSucceeded);
			StructuredContent->SetStringField(TEXT("commandResult"), PythonCommand.CommandResult);
			StructuredContent->SetNumberField(TEXT("logCount"), PythonCommand.LogOutput.Num());
			StructuredContent->SetArrayField(TEXT("logOutput"), LogOutputArray);

			FString Text = FString::Printf(
				TEXT("Executed Python command. success=%s mode=%s scope=%s"),
				bSucceeded ? TEXT("true") : TEXT("false"),
				LexToString(ExecutionMode),
				FileExecutionScope == EPythonFileExecutionScope::Public ? TEXT("Public") : TEXT("Private"));

			if (!PythonCommand.CommandResult.IsEmpty())
			{
				Text += FString::Printf(TEXT("\nResult:\n%s"), *PythonCommand.CommandResult);
			}

			if (LogLines.Num() > 0)
			{
				Text += TEXT("\nLog:\n") + FString::Join(LogLines, TEXT("\n"));
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, !bSucceeded);
		}

		if (ToolName == TEXT("unreal.tail_log"))
		{
			const int32 RequestedLines = FMath::Min(UnrealMcp::GetPositiveIntArgument(Arguments, TEXT("lines"), 120), 500);
			FString ContainsFilter;
			Arguments.TryGetStringField(TEXT("contains"), ContainsFilter);
			ContainsFilter = ContainsFilter.TrimStartAndEnd();

			const FString EditorLogPath = FGenericPlatformOutputDevices::GetAbsoluteLogFilename();
			FString FullLogText;
			if (!FFileHelper::LoadFileToString(FullLogText, *EditorLogPath))
			{
				return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to read editor log '%s'."), *EditorLogPath), nullptr, true);
			}

			TArray<FString> AllLines;
			FullLogText.ParseIntoArrayLines(AllLines);

			TArray<FString> MatchingLines;
			MatchingLines.Reserve(AllLines.Num());
			if (ContainsFilter.IsEmpty())
			{
				MatchingLines = AllLines;
			}
			else
			{
				for (const FString& Line : AllLines)
				{
					if (Line.Contains(ContainsFilter, ESearchCase::IgnoreCase))
					{
						MatchingLines.Add(Line);
					}
				}
			}

			const int32 StartIndex = FMath::Max(0, MatchingLines.Num() - RequestedLines);
			TArray<FString> ReturnedLines;
			for (int32 Index = StartIndex; Index < MatchingLines.Num(); ++Index)
			{
				ReturnedLines.Add(MatchingLines[Index]);
			}

			const FString TailText = ReturnedLines.Num() > 0
				? FString::Join(ReturnedLines, TEXT("\n"))
				: (ContainsFilter.IsEmpty()
					? TEXT("The editor log is empty.")
					: FString::Printf(TEXT("No log lines matched '%s'."), *ContainsFilter));

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("logPath"), EditorLogPath);
			StructuredContent->SetNumberField(TEXT("requestedLines"), RequestedLines);
			StructuredContent->SetNumberField(TEXT("matchedLineCount"), MatchingLines.Num());
			StructuredContent->SetNumberField(TEXT("returnedLineCount"), ReturnedLines.Num());
			StructuredContent->SetStringField(TEXT("text"), TailText);
			if (!ContainsFilter.IsEmpty())
			{
				StructuredContent->SetStringField(TEXT("contains"), ContainsFilter);
			}

			return UnrealMcp::MakeExecutionResult(TailText, StructuredContent, false);
		}

		if (ToolName == TEXT("unreal.map_check"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!EditorWorld)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("The editor world is unavailable."), nullptr, true);
			}

			FStringOutputDevice OutputDevice;
			const bool bExecuted = GEditor && GEditor->Exec(EditorWorld, TEXT("MAP CHECK DONTDISPLAYDIALOG"), OutputDevice);

			FMessageLog MapCheckLog(TEXT("MapCheck"));
			const int32 ErrorCount = MapCheckLog.NumMessages(EMessageSeverity::Error);
			const int32 WarningOrHigherCount = MapCheckLog.NumMessages(EMessageSeverity::Warning);
			const int32 WarningCount = FMath::Max(0, WarningOrHigherCount - ErrorCount);
			const FString CapturedOutput = FString(OutputDevice).TrimStartAndEnd();

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("map"), EditorWorld->GetOutermost()->GetName());
			StructuredContent->SetBoolField(TEXT("success"), bExecuted);
			StructuredContent->SetNumberField(TEXT("errorCount"), ErrorCount);
			StructuredContent->SetNumberField(TEXT("warningCount"), WarningCount);
			StructuredContent->SetStringField(TEXT("output"), CapturedOutput);

			FString Text = FString::Printf(
				TEXT("Map Check completed for %s. success=%s errors=%d warnings=%d"),
				*EditorWorld->GetOutermost()->GetName(),
				bExecuted ? TEXT("true") : TEXT("false"),
				ErrorCount,
				WarningCount);
			if (!CapturedOutput.IsEmpty())
			{
				Text += FString::Printf(TEXT("\n%s"), *CapturedOutput);
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, !bExecuted || ErrorCount > 0);
		}

		if (ToolName == TEXT("unreal.list_maps"))
		{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		FARFilter Filter;
		Filter.PackagePaths.Add(TEXT("/Game"));
		Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
		Filter.bRecursivePaths = true;

		TArray<FAssetData> AssetData;
		AssetRegistryModule.Get().GetAssets(Filter, AssetData);
		AssetData.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.PackageName.ToString() < B.PackageName.ToString();
		});

		TArray<TSharedPtr<FJsonValue>> MapsArray;
		TArray<FString> TextLines;
		for (const FAssetData& Asset : AssetData)
		{
			TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
			AssetObject->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
			AssetObject->SetStringField(TEXT("assetName"), Asset.AssetName.ToString());
			AssetObject->SetStringField(TEXT("objectPath"), Asset.GetSoftObjectPath().ToString());
			MapsArray.Add(MakeShared<FJsonValueObject>(AssetObject));
			TextLines.Add(Asset.PackageName.ToString());
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetNumberField(TEXT("count"), AssetData.Num());
		StructuredContent->SetArrayField(TEXT("maps"), MapsArray);

		const FString Text = TextLines.Num() > 0
			? FString::Printf(TEXT("Found %d maps:\n%s"), TextLines.Num(), *FString::Join(TextLines, TEXT("\n")))
			: TEXT("Found 0 maps under /Game.");

		return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
	}

	if (ToolName == TEXT("unreal.list_assets"))
	{
		FString Path = TEXT("/Game");
		bool bRecursive = true;
		FString ClassPathFilter;
		Arguments.TryGetStringField(TEXT("path"), Path);
		Arguments.TryGetBoolField(TEXT("recursive"), bRecursive);
		Arguments.TryGetStringField(TEXT("classPath"), ClassPathFilter);
		const int32 Limit = UnrealMcp::GetPositiveIntArgument(Arguments, TEXT("limit"), UnrealMcp::DefaultListLimit);

		if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The path argument must be a Content Browser path like /Game."), nullptr, true);
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		FARFilter Filter;
		Filter.PackagePaths.Add(*Path);
		Filter.bRecursivePaths = bRecursive;

		TArray<FAssetData> AssetData;
		AssetRegistryModule.Get().GetAssets(Filter, AssetData);
		AssetData.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
		});

		int32 TotalMatches = 0;
		bool bTruncated = false;
		TArray<TSharedPtr<FJsonValue>> AssetsArray;
		TArray<FString> TextLines;

		for (const FAssetData& Asset : AssetData)
		{
			if (!ClassPathFilter.IsEmpty())
			{
				const FString AssetClassPath = Asset.AssetClassPath.ToString();
				if (!AssetClassPath.Equals(ClassPathFilter, ESearchCase::IgnoreCase)
					&& !AssetClassPath.Contains(ClassPathFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			++TotalMatches;

			if (AssetsArray.Num() >= Limit)
			{
				bTruncated = true;
				continue;
			}

			AssetsArray.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakeAssetObject(Asset)));
			TextLines.Add(UnrealMcp::DescribeAsset(Asset));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("path"), Path);
		StructuredContent->SetBoolField(TEXT("recursive"), bRecursive);
		StructuredContent->SetStringField(TEXT("classPath"), ClassPathFilter);
		StructuredContent->SetNumberField(TEXT("count"), TotalMatches);
		StructuredContent->SetNumberField(TEXT("returnedCount"), AssetsArray.Num());
		StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
		StructuredContent->SetArrayField(TEXT("assets"), AssetsArray);

		FString Text;
		if (TextLines.Num() > 0)
		{
			Text = FString::Printf(TEXT("Found %d assets under %s"), TotalMatches, *Path);
			if (!ClassPathFilter.IsEmpty())
			{
				Text += FString::Printf(TEXT(" filtered by %s"), *ClassPathFilter);
			}
			if (bTruncated)
			{
				Text += FString::Printf(TEXT(" (showing first %d)"), AssetsArray.Num());
			}
			Text += TEXT(":\n") + FString::Join(TextLines, TEXT("\n"));
		}
		else
		{
			Text = FString::Printf(TEXT("Found 0 assets under %s."), *Path);
		}

		return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
	}

	if (ToolName == TEXT("unreal.list_selected_assets"))
	{
		const TArray<FAssetData> SelectedAssets = UnrealMcp::GetSelectedAssets();

		TArray<TSharedPtr<FJsonValue>> AssetsArray;
		TArray<FString> TextLines;
		for (const FAssetData& Asset : SelectedAssets)
		{
			AssetsArray.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakeAssetObject(Asset)));
			TextLines.Add(UnrealMcp::DescribeAsset(Asset));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetNumberField(TEXT("count"), SelectedAssets.Num());
		StructuredContent->SetArrayField(TEXT("assets"), AssetsArray);

		const FString Text = TextLines.Num() > 0
			? FString::Printf(TEXT("Selected assets (%d):\n%s"), TextLines.Num(), *FString::Join(TextLines, TEXT("\n")))
			: TEXT("No assets are currently selected in the Content Browser.");

		return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
	}

	if (ToolName == TEXT("unreal.list_level_actors"))
	{
		if (!EditorActorSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
		}

		FString FilterText;
		FString ClassPathFilter;
		Arguments.TryGetStringField(TEXT("filter"), FilterText);
		Arguments.TryGetStringField(TEXT("classPath"), ClassPathFilter);
		const int32 Limit = UnrealMcp::GetPositiveIntArgument(Arguments, TEXT("limit"), UnrealMcp::DefaultListLimit);

		const TArray<AActor*> AllActors = EditorActorSubsystem->GetAllLevelActors();
		TArray<AActor*> SortedActors = AllActors;
		SortedActors.Sort([](const AActor& A, const AActor& B)
		{
			return A.GetActorLabel() < B.GetActorLabel();
		});

		int32 TotalMatches = 0;
		bool bTruncated = false;
		TArray<TSharedPtr<FJsonValue>> ActorsArray;
		TArray<FString> TextLines;

		for (AActor* Actor : SortedActors)
		{
			const FString ActorLabel = Actor->GetActorLabel();
			const FString ActorName = Actor->GetName();
			const FString ActorClassPath = Actor->GetClass()->GetPathName();

			if (!FilterText.IsEmpty()
				&& !ActorLabel.Contains(FilterText, ESearchCase::IgnoreCase)
				&& !ActorName.Contains(FilterText, ESearchCase::IgnoreCase)
				&& !ActorClassPath.Contains(FilterText, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (!ClassPathFilter.IsEmpty()
				&& !ActorClassPath.Equals(ClassPathFilter, ESearchCase::IgnoreCase)
				&& !ActorClassPath.Contains(ClassPathFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			++TotalMatches;
			if (ActorsArray.Num() >= Limit)
			{
				bTruncated = true;
				continue;
			}

			ActorsArray.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakeActorObject(Actor)));
			TextLines.Add(UnrealMcp::DescribeActor(Actor));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("filter"), FilterText);
		StructuredContent->SetStringField(TEXT("classPath"), ClassPathFilter);
		StructuredContent->SetNumberField(TEXT("count"), TotalMatches);
		StructuredContent->SetNumberField(TEXT("returnedCount"), ActorsArray.Num());
		StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
		StructuredContent->SetArrayField(TEXT("actors"), ActorsArray);

		FString Text;
		if (TextLines.Num() > 0)
		{
			Text = FString::Printf(TEXT("Found %d actors"), TotalMatches);
			if (!FilterText.IsEmpty())
			{
				Text += FString::Printf(TEXT(" matching '%s'"), *FilterText);
			}
			if (!ClassPathFilter.IsEmpty())
			{
				Text += FString::Printf(TEXT(" in class '%s'"), *ClassPathFilter);
			}
			if (bTruncated)
			{
				Text += FString::Printf(TEXT(" (showing first %d)"), ActorsArray.Num());
			}
			Text += TEXT(":\n") + FString::Join(TextLines, TEXT("\n"));
		}
		else
		{
			Text = TEXT("Found 0 matching actors in the current level.");
		}

		return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
	}

		if (ToolName == TEXT("unreal.list_selected_actors"))
		{
			if (!EditorActorSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
		}

		const TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
		TArray<TSharedPtr<FJsonValue>> ActorsArray;
		TArray<FString> TextLines;

		for (AActor* Actor : SelectedActors)
		{
			ActorsArray.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakeActorObject(Actor)));
			TextLines.Add(UnrealMcp::DescribeActor(Actor));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetNumberField(TEXT("count"), SelectedActors.Num());
		StructuredContent->SetArrayField(TEXT("actors"), ActorsArray);

		const FString Text = TextLines.Num() > 0
			? FString::Printf(TEXT("Selected actors (%d):\n%s"), TextLines.Num(), *FString::Join(TextLines, TEXT("\n")))
			: TEXT("No actors are currently selected in the level editor.");

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
		}

		if (ToolName == TEXT("unreal.select_actors"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			if (!EditorActorSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			FString FilterText;
			FString ClassPathFilter;
			bool bClearSelection = true;
			TArray<FString> RequestedPaths;
			const int32 Limit = UnrealMcp::GetPositiveIntArgument(Arguments, TEXT("limit"), UnrealMcp::DefaultListLimit);

			Arguments.TryGetStringField(TEXT("filter"), FilterText);
			Arguments.TryGetStringField(TEXT("classPath"), ClassPathFilter);
			Arguments.TryGetBoolField(TEXT("clearSelection"), bClearSelection);
			UnrealMcp::TryGetStringArrayField(Arguments, TEXT("paths"), RequestedPaths);

			TSet<FString> ExplicitPaths;
			for (const FString& RequestedPath : RequestedPaths)
			{
				const FString TrimmedPath = RequestedPath.TrimStartAndEnd();
				if (!TrimmedPath.IsEmpty())
				{
					ExplicitPaths.Add(TrimmedPath);
				}
			}

			if (FilterText.TrimStartAndEnd().IsEmpty() && ClassPathFilter.TrimStartAndEnd().IsEmpty() && ExplicitPaths.Num() == 0)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Provide at least one of filter, classPath, or paths to select actors."), nullptr, true);
			}

			if (bClearSelection)
			{
				EditorActorSubsystem->SelectNothing();
			}

			TArray<AActor*> SortedActors = EditorActorSubsystem->GetAllLevelActors();
			SortedActors.Sort([](const AActor& A, const AActor& B)
			{
				return A.GetActorLabel() < B.GetActorLabel();
			});

			int32 TotalMatches = 0;
			bool bTruncated = false;
			TArray<TSharedPtr<FJsonValue>> ActorsArray;
			TArray<FString> TextLines;

			for (AActor* Actor : SortedActors)
			{
				if (!Actor || !UnrealMcp::MatchesActorFilters(Actor, FilterText, ClassPathFilter, ExplicitPaths))
				{
					continue;
				}

				++TotalMatches;
				if (ActorsArray.Num() >= Limit)
				{
					bTruncated = true;
					continue;
				}

				EditorActorSubsystem->SetActorSelectionState(Actor, true);
				ActorsArray.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakeActorObject(Actor)));
				TextLines.Add(UnrealMcp::DescribeActor(Actor));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("filter"), FilterText);
			StructuredContent->SetStringField(TEXT("classPath"), ClassPathFilter);
			StructuredContent->SetBoolField(TEXT("clearSelection"), bClearSelection);
			StructuredContent->SetNumberField(TEXT("count"), TotalMatches);
			StructuredContent->SetNumberField(TEXT("returnedCount"), ActorsArray.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
			StructuredContent->SetArrayField(TEXT("actors"), ActorsArray);

			FString Text;
			if (TextLines.Num() > 0)
			{
				Text = FString::Printf(TEXT("Selected %d actors"), TotalMatches);
				if (!FilterText.IsEmpty())
				{
					Text += FString::Printf(TEXT(" matching '%s'"), *FilterText);
				}
				if (!ClassPathFilter.IsEmpty())
				{
					Text += FString::Printf(TEXT(" in class '%s'"), *ClassPathFilter);
				}
				if (bTruncated)
				{
					Text += FString::Printf(TEXT(" (showing first %d)"), ActorsArray.Num());
				}
				Text += TEXT(":\n") + FString::Join(TextLines, TEXT("\n"));
			}
			else
			{
				Text = TEXT("Selected 0 actors because nothing matched the provided criteria.");
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
		}

		if (ToolName == TEXT("unreal.set_actor_transform"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			if (!EditorActorSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			FString ActorPath;
			FString ActorLabel;
			Arguments.TryGetStringField(TEXT("actorPath"), ActorPath);
			Arguments.TryGetStringField(TEXT("actorLabel"), ActorLabel);

			FString FailureReason;
			AActor* Actor = UnrealMcp::ResolveActorReference(EditorActorSubsystem, ActorPath, ActorLabel, FailureReason);
			if (!Actor)
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}

			const FVector OriginalLocation = Actor->GetActorLocation();
			const FRotator OriginalRotation = Actor->GetActorRotation();
			FVector NewLocation = OriginalLocation;
			FRotator NewRotation = OriginalRotation;
			bool bHasTransformField = false;

			double Value = 0.0;
			if (Arguments.TryGetNumberField(TEXT("x"), Value))
			{
				NewLocation.X = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("y"), Value))
			{
				NewLocation.Y = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("z"), Value))
			{
				NewLocation.Z = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("pitch"), Value))
			{
				NewRotation.Pitch = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("yaw"), Value))
			{
				NewRotation.Yaw = Value;
				bHasTransformField = true;
			}
			if (Arguments.TryGetNumberField(TEXT("roll"), Value))
			{
				NewRotation.Roll = Value;
				bHasTransformField = true;
			}

			if (!bHasTransformField)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Provide at least one of x, y, z, pitch, yaw, or roll."), nullptr, true);
			}

			const bool bMoved = EditorActorSubsystem->SetActorTransform(
				Actor,
				FTransform(NewRotation, NewLocation, Actor->GetActorScale3D()));

			TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeActorObject(Actor);
			StructuredContent->SetObjectField(TEXT("beforeLocation"), UnrealMcp::MakeVectorObject(OriginalLocation));
			StructuredContent->SetObjectField(TEXT("beforeRotation"), UnrealMcp::MakeRotatorObject(OriginalRotation));
			StructuredContent->SetObjectField(TEXT("afterLocation"), UnrealMcp::MakeVectorObject(Actor->GetActorLocation()));
			StructuredContent->SetObjectField(TEXT("afterRotation"), UnrealMcp::MakeRotatorObject(Actor->GetActorRotation()));

			return UnrealMcp::MakeExecutionResult(
				FString::Printf(TEXT("Set transform on actor %s. success=%s"), *Actor->GetActorLabel(), bMoved ? TEXT("true") : TEXT("false")),
				StructuredContent,
				!bMoved);
		}

		if (ToolName == TEXT("unreal.batch_set_actor_scale"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			if (!EditorActorSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			double ScaleX = 1.0;
			double ScaleY = 1.0;
			double ScaleZ = 1.0;
			const bool bHasScaleX = Arguments.TryGetNumberField(TEXT("scaleX"), ScaleX);
			const bool bHasScaleY = Arguments.TryGetNumberField(TEXT("scaleY"), ScaleY);
			const bool bHasScaleZ = Arguments.TryGetNumberField(TEXT("scaleZ"), ScaleZ);
			if (!bHasScaleX && !bHasScaleY && !bHasScaleZ)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Provide at least one of scaleX, scaleY, or scaleZ."), nullptr, true);
			}

			UnrealMcp::FActorQueryResult Query;
			FString FailureReason;
			if (!UnrealMcp::ResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchSetActorScale", "Unreal MCP Batch Set Actor Scale"));

			int32 SuccessCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				const FVector BeforeScale = Actor->GetActorScale3D();
				const FVector RequestedScale(
					bHasScaleX ? ScaleX : BeforeScale.X,
					bHasScaleY ? ScaleY : BeforeScale.Y,
					bHasScaleZ ? ScaleZ : BeforeScale.Z);

				Actor->Modify();
				Actor->SetActorScale3D(RequestedScale);
				Actor->MarkPackageDirty();

				const FVector AfterScale = Actor->GetActorScale3D();
				const bool bSucceeded = AfterScale.Equals(RequestedScale, KINDA_SMALL_NUMBER);
				if (bSucceeded)
				{
					++SuccessCount;
				}

				TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
				ActorObject->SetObjectField(TEXT("beforeScale"), UnrealMcp::MakeVectorObject(BeforeScale));
				ActorObject->SetObjectField(TEXT("afterScale"), UnrealMcp::MakeVectorObject(AfterScale));
				ActorObject->SetBoolField(TEXT("success"), bSucceeded);
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TextLines.Add(FString::Printf(
					TEXT("%s -> scale [%.2f, %.2f, %.2f] success=%s"),
					*Actor->GetActorLabel(),
					AfterScale.X,
					AfterScale.Y,
					AfterScale.Z,
					bSucceeded ? TEXT("true") : TEXT("false")));
			}

			const int32 FailureCount = Query.Actors.Num() - SuccessCount;

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Updated scale on %d actors. success=%d failure=%d"),
				Query.Actors.Num(),
				SuccessCount,
				FailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		if (ToolName == TEXT("unreal.batch_set_actor_tags"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			if (!EditorActorSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			TArray<FString> TagStrings;
			UnrealMcp::TryGetStringArrayField(Arguments, TEXT("tags"), TagStrings);
			TArray<FName> Tags;
			for (const FString& TagString : TagStrings)
			{
				const FString TrimmedTag = TagString.TrimStartAndEnd();
				if (!TrimmedTag.IsEmpty())
				{
					Tags.Add(FName(*TrimmedTag));
				}
			}

			if (Tags.Num() == 0)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("The tags argument must contain at least one non-empty tag."), nullptr, true);
			}

			bool bReplaceExisting = false;
			Arguments.TryGetBoolField(TEXT("replaceExisting"), bReplaceExisting);

			UnrealMcp::FActorQueryResult Query;
			FString FailureReason;
			if (!UnrealMcp::ResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchSetActorTags", "Unreal MCP Batch Set Actor Tags"));

			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				TArray<TSharedPtr<FJsonValue>> BeforeTags;
				for (const FName& ExistingTag : Actor->Tags)
				{
					BeforeTags.Add(MakeShared<FJsonValueString>(ExistingTag.ToString()));
				}

				Actor->Modify();
				if (bReplaceExisting)
				{
					Actor->Tags.Reset();
				}

				for (const FName& Tag : Tags)
				{
					Actor->Tags.AddUnique(Tag);
				}
				Actor->MarkPackageDirty();

				TArray<TSharedPtr<FJsonValue>> AfterTags;
				for (const FName& ExistingTag : Actor->Tags)
				{
					AfterTags.Add(MakeShared<FJsonValueString>(ExistingTag.ToString()));
				}

				TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
				ActorObject->SetArrayField(TEXT("beforeTags"), BeforeTags);
				ActorObject->SetArrayField(TEXT("afterTags"), AfterTags);
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TArray<FString> TagLineParts;
				for (const FName& ExistingTag : Actor->Tags)
				{
					TagLineParts.Add(ExistingTag.ToString());
				}
				TextLines.Add(FString::Printf(TEXT("%s -> [%s]"), *Actor->GetActorLabel(), *FString::Join(TagLineParts, TEXT(", "))));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetBoolField(TEXT("replaceExisting"), bReplaceExisting);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("%s tags on %d actors."),
				bReplaceExisting ? TEXT("Replaced") : TEXT("Updated"),
				Query.Actors.Num());
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
		}

		if (ToolName == TEXT("unreal.batch_set_point_light_properties"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			if (!EditorActorSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			double Intensity = 0.0;
			double AttenuationRadius = 0.0;
			double SourceRadius = 0.0;
			double SoftSourceRadius = 0.0;
			double Temperature = 6500.0;
			bool bUseTemperature = false;
			bool bCastShadows = true;
			bool bVisible = true;
			const bool bHasIntensity = Arguments.TryGetNumberField(TEXT("intensity"), Intensity);
			const bool bHasAttenuationRadius = Arguments.TryGetNumberField(TEXT("attenuationRadius"), AttenuationRadius);
			const bool bHasSourceRadius = Arguments.TryGetNumberField(TEXT("sourceRadius"), SourceRadius);
			const bool bHasSoftSourceRadius = Arguments.TryGetNumberField(TEXT("softSourceRadius"), SoftSourceRadius);
			const bool bHasTemperature = Arguments.TryGetNumberField(TEXT("temperature"), Temperature);
			const bool bHasUseTemperature = Arguments.TryGetBoolField(TEXT("useTemperature"), bUseTemperature);
			const bool bHasCastShadows = Arguments.TryGetBoolField(TEXT("castShadows"), bCastShadows);
			const bool bHasVisible = Arguments.TryGetBoolField(TEXT("visible"), bVisible);
			if (!bHasIntensity
				&& !bHasAttenuationRadius
				&& !bHasSourceRadius
				&& !bHasSoftSourceRadius
				&& !bHasTemperature
				&& !bHasUseTemperature
				&& !bHasCastShadows
				&& !bHasVisible)
			{
				return UnrealMcp::MakeExecutionResult(
					TEXT("Provide at least one point light property such as intensity, attenuationRadius, sourceRadius, temperature, castShadows, or visible."),
					nullptr,
					true);
			}

			UnrealMcp::FActorQueryResult Query;
			FString FailureReason;
			if (!UnrealMcp::ResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchSetPointLightProperties", "Unreal MCP Batch Set Point Light Properties"));

			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				UPointLightComponent* PointLightComponent = Actor ? Actor->FindComponentByClass<UPointLightComponent>() : nullptr;
				if (!PointLightComponent)
				{
					++FailureCount;
					TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
					ActorObject->SetBoolField(TEXT("success"), false);
					ActorObject->SetStringField(TEXT("error"), TEXT("No PointLightComponent was found on this actor."));
					ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));
					TextLines.Add(FString::Printf(TEXT("%s: missing PointLightComponent"), *Actor->GetActorLabel()));
					continue;
				}

				Actor->Modify();
				PointLightComponent->Modify();

				if (bHasIntensity)
				{
					PointLightComponent->SetIntensity(Intensity);
				}
				if (bHasAttenuationRadius)
				{
					PointLightComponent->SetAttenuationRadius(AttenuationRadius);
				}
				if (bHasSourceRadius)
				{
					PointLightComponent->SetSourceRadius(SourceRadius);
				}
				if (bHasSoftSourceRadius)
				{
					PointLightComponent->SetSoftSourceRadius(SoftSourceRadius);
				}
				if (bHasUseTemperature)
				{
					PointLightComponent->SetUseTemperature(bUseTemperature);
				}
				if (bHasTemperature)
				{
					PointLightComponent->SetTemperature(Temperature);
				}
				if (bHasCastShadows)
				{
					PointLightComponent->SetCastShadows(bCastShadows);
				}
				if (bHasVisible)
				{
					PointLightComponent->SetVisibility(bVisible);
				}

				PointLightComponent->MarkPackageDirty();
				Actor->MarkPackageDirty();

				++SuccessCount;
				TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
				ActorObject->SetBoolField(TEXT("success"), true);
				if (bHasIntensity)
				{
					ActorObject->SetNumberField(TEXT("intensity"), Intensity);
				}
				if (bHasAttenuationRadius)
				{
					ActorObject->SetNumberField(TEXT("attenuationRadius"), AttenuationRadius);
				}
				if (bHasSourceRadius)
				{
					ActorObject->SetNumberField(TEXT("sourceRadius"), SourceRadius);
				}
				if (bHasSoftSourceRadius)
				{
					ActorObject->SetNumberField(TEXT("softSourceRadius"), SoftSourceRadius);
				}
				if (bHasUseTemperature)
				{
					ActorObject->SetBoolField(TEXT("useTemperature"), bUseTemperature);
				}
				if (bHasTemperature)
				{
					ActorObject->SetNumberField(TEXT("temperature"), Temperature);
				}
				if (bHasCastShadows)
				{
					ActorObject->SetBoolField(TEXT("castShadows"), bCastShadows);
				}
				if (bHasVisible)
				{
					ActorObject->SetBoolField(TEXT("visible"), bVisible);
				}
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TextLines.Add(FString::Printf(TEXT("%s: updated point light properties"), *Actor->GetActorLabel()));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Updated point light properties on %d actors. success=%d failure=%d"),
				Query.Actors.Num(),
				SuccessCount,
				FailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		if (ToolName == TEXT("unreal.batch_configure_static_mesh_actors"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			if (!EditorActorSubsystem || !EditorAssetSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Editor actor subsystems are unavailable."), nullptr, true);
			}

			FString StaticMeshPath;
			FString MaterialPath;
			Arguments.TryGetStringField(TEXT("staticMeshPath"), StaticMeshPath);
			Arguments.TryGetStringField(TEXT("materialPath"), MaterialPath);
			if (StaticMeshPath.TrimStartAndEnd().IsEmpty() && MaterialPath.TrimStartAndEnd().IsEmpty())
			{
				return UnrealMcp::MakeExecutionResult(TEXT("Provide at least one of staticMeshPath or materialPath."), nullptr, true);
			}

			double MaterialSlotValue = 0.0;
			Arguments.TryGetNumberField(TEXT("materialSlot"), MaterialSlotValue);
			const int32 MaterialSlot = FMath::Max(0, static_cast<int32>(MaterialSlotValue));

			UStaticMesh* StaticMeshAsset = nullptr;
			FString MeshObjectPath;
			FString FailureReason;
			if (!StaticMeshPath.TrimStartAndEnd().IsEmpty())
			{
				UObject* LoadedMeshAsset = UnrealMcp::LoadAssetFromAnyPath(EditorAssetSubsystem, StaticMeshPath, MeshObjectPath, FailureReason);
				StaticMeshAsset = Cast<UStaticMesh>(LoadedMeshAsset);
				if (!StaticMeshAsset)
				{
					return UnrealMcp::MakeExecutionResult(
						LoadedMeshAsset
							? FString::Printf(TEXT("Asset '%s' is not a StaticMesh."), *MeshObjectPath)
							: FailureReason,
						nullptr,
						true);
				}
			}

			UMaterialInterface* MaterialAsset = nullptr;
			FString MaterialObjectPath;
			if (!MaterialPath.TrimStartAndEnd().IsEmpty())
			{
				UObject* LoadedMaterialAsset = UnrealMcp::LoadAssetFromAnyPath(EditorAssetSubsystem, MaterialPath, MaterialObjectPath, FailureReason);
				MaterialAsset = Cast<UMaterialInterface>(LoadedMaterialAsset);
				if (!MaterialAsset)
				{
					return UnrealMcp::MakeExecutionResult(
						LoadedMaterialAsset
							? FString::Printf(TEXT("Asset '%s' is not a material or material instance."), *MaterialObjectPath)
							: FailureReason,
						nullptr,
						true);
				}
			}

			UnrealMcp::FActorQueryResult Query;
			if (!UnrealMcp::ResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
			{
				return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
			}

			FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchConfigureStaticMeshActors", "Unreal MCP Batch Configure Static Mesh Actors"));

			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> ActorResults;
			TArray<FString> TextLines;

			for (AActor* Actor : Query.Actors)
			{
				UStaticMeshComponent* StaticMeshComponent = Actor ? Actor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
				if (!StaticMeshComponent)
				{
					++FailureCount;
					TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
					ActorObject->SetBoolField(TEXT("success"), false);
					ActorObject->SetStringField(TEXT("error"), TEXT("No StaticMeshComponent was found on this actor."));
					ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));
					TextLines.Add(FString::Printf(TEXT("%s: missing StaticMeshComponent"), *Actor->GetActorLabel()));
					continue;
				}

				Actor->Modify();
				StaticMeshComponent->Modify();
				if (StaticMeshAsset)
				{
					StaticMeshComponent->SetStaticMesh(StaticMeshAsset);
				}
				if (MaterialAsset)
				{
					StaticMeshComponent->SetMaterial(MaterialSlot, MaterialAsset);
				}
				StaticMeshComponent->MarkPackageDirty();
				Actor->MarkPackageDirty();

				++SuccessCount;
				TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
				ActorObject->SetBoolField(TEXT("success"), true);
				if (StaticMeshAsset)
				{
					ActorObject->SetStringField(TEXT("staticMeshPath"), MeshObjectPath);
				}
				if (MaterialAsset)
				{
					ActorObject->SetStringField(TEXT("materialPath"), MaterialObjectPath);
					ActorObject->SetNumberField(TEXT("materialSlot"), MaterialSlot);
				}
				ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

				TextLines.Add(FString::Printf(TEXT("%s: updated static mesh configuration"), *Actor->GetActorLabel()));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
			StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
			StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			if (StaticMeshAsset)
			{
				StructuredContent->SetStringField(TEXT("staticMeshPath"), MeshObjectPath);
			}
			if (MaterialAsset)
			{
				StructuredContent->SetStringField(TEXT("materialPath"), MaterialObjectPath);
				StructuredContent->SetNumberField(TEXT("materialSlot"), MaterialSlot);
			}
			StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

			FString Text = FString::Printf(
				TEXT("Configured static mesh actors. success=%d failure=%d"),
				SuccessCount,
				FailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		if (ToolName == TEXT("unreal.batch_set_actor_properties"))
		{
				if (UnrealMcp::IsEditorPlaying())
				{
					return UnrealMcp::MakePieBlockedResult(ToolName);
				}

				if (!EditorActorSubsystem)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
				}

				const TSharedPtr<FJsonObject>* PropertyValuesObject = nullptr;
				if (!Arguments.TryGetObjectField(TEXT("properties"), PropertyValuesObject) || !PropertyValuesObject || (*PropertyValuesObject)->Values.Num() == 0)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("The properties argument must be a non-empty object."), nullptr, true);
				}

				UnrealMcp::FActorQueryResult Query;
				FString FailureReason;
				if (!UnrealMcp::ResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
				{
					return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
				}

				FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "BatchSetActorProperties", "Unreal MCP Batch Set Actor Properties"));

				int32 TotalSuccessCount = 0;
				int32 TotalFailureCount = 0;
				TArray<TSharedPtr<FJsonValue>> ActorResults;
				TArray<FString> TextLines;

				for (AActor* Actor : Query.Actors)
				{
					int32 ActorSuccessCount = 0;
					int32 ActorFailureCount = 0;
					TArray<TSharedPtr<FJsonValue>> EditResults;
					UnrealMcp::ApplyPropertyMapToActor(Actor, **PropertyValuesObject, EditResults, ActorSuccessCount, ActorFailureCount);

					TotalSuccessCount += ActorSuccessCount;
					TotalFailureCount += ActorFailureCount;

					TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
					ActorObject->SetNumberField(TEXT("propertySuccessCount"), ActorSuccessCount);
					ActorObject->SetNumberField(TEXT("propertyFailureCount"), ActorFailureCount);
					ActorObject->SetArrayField(TEXT("propertyEdits"), EditResults);
					ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

					TextLines.Add(FString::Printf(
						TEXT("%s: propertySuccess=%d propertyFailure=%d"),
						*Actor->GetActorLabel(),
						ActorSuccessCount,
						ActorFailureCount));
				}

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("filter"), Query.FilterText);
				StructuredContent->SetStringField(TEXT("classPath"), Query.ClassPathFilter);
				StructuredContent->SetBoolField(TEXT("selectedOnly"), Query.bSelectedOnly);
				StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
				StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
				StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
				StructuredContent->SetNumberField(TEXT("propertyCount"), (*PropertyValuesObject)->Values.Num());
				StructuredContent->SetNumberField(TEXT("successCount"), TotalSuccessCount);
				StructuredContent->SetNumberField(TEXT("failureCount"), TotalFailureCount);
				StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

				FString Text = FString::Printf(
					TEXT("Applied %d property edits across %d actors. success=%d failure=%d"),
					(TotalSuccessCount + TotalFailureCount),
					Query.Actors.Num(),
					TotalSuccessCount,
					TotalFailureCount);
				if (TextLines.Num() > 0)
				{
					Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
				}

				return UnrealMcp::MakeExecutionResult(Text, StructuredContent, TotalFailureCount > 0);
			}

			if (ToolName == TEXT("unreal.layout_actors_grid"))
			{
				if (UnrealMcp::IsEditorPlaying())
				{
					return UnrealMcp::MakePieBlockedResult(ToolName);
				}

				if (!EditorActorSubsystem)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
				}

				UnrealMcp::FActorQueryResult Query;
				FString FailureReason;
				if (!UnrealMcp::ResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
				{
					return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
				}

				const int32 Columns = FMath::Max(1, UnrealMcp::GetPositiveIntArgument(Arguments, TEXT("columns"), 5));

				double SpacingX = 300.0;
				double SpacingY = 300.0;
				double SpacingZ = 0.0;
				Arguments.TryGetNumberField(TEXT("spacingX"), SpacingX);
				Arguments.TryGetNumberField(TEXT("spacingY"), SpacingY);
				Arguments.TryGetNumberField(TEXT("spacingZ"), SpacingZ);

				FVector Origin = Query.Actors[0]->GetActorLocation();
				double NumericValue = 0.0;
				if (Arguments.TryGetNumberField(TEXT("startX"), NumericValue))
				{
					Origin.X = NumericValue;
				}
				if (Arguments.TryGetNumberField(TEXT("startY"), NumericValue))
				{
					Origin.Y = NumericValue;
				}
				if (Arguments.TryGetNumberField(TEXT("startZ"), NumericValue))
				{
					Origin.Z = NumericValue;
				}

				bool bHasPitch = false;
				bool bHasYaw = false;
				bool bHasRoll = false;
				double Pitch = 0.0;
				double Yaw = 0.0;
				double Roll = 0.0;
				bHasPitch = Arguments.TryGetNumberField(TEXT("pitch"), Pitch);
				bHasYaw = Arguments.TryGetNumberField(TEXT("yaw"), Yaw);
				bHasRoll = Arguments.TryGetNumberField(TEXT("roll"), Roll);

				FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "LayoutActorsGrid", "Unreal MCP Layout Actors Grid"));

				int32 SuccessCount = 0;
				int32 FailureCount = 0;
				TArray<TSharedPtr<FJsonValue>> ActorResults;
				TArray<FString> TextLines;

				for (int32 ActorIndex = 0; ActorIndex < Query.Actors.Num(); ++ActorIndex)
				{
					AActor* Actor = Query.Actors[ActorIndex];
					const FVector BeforeLocation = Actor->GetActorLocation();
					const FRotator BeforeRotation = Actor->GetActorRotation();

					const int32 ColumnIndex = ActorIndex % Columns;
					const int32 RowIndex = ActorIndex / Columns;

					const FVector NewLocation(
						Origin.X + static_cast<double>(ColumnIndex) * SpacingX,
						Origin.Y + static_cast<double>(RowIndex) * SpacingY,
						Origin.Z + static_cast<double>(RowIndex) * SpacingZ);

					FRotator NewRotation = BeforeRotation;
					if (bHasPitch)
					{
						NewRotation.Pitch = Pitch;
					}
					if (bHasYaw)
					{
						NewRotation.Yaw = Yaw;
					}
					if (bHasRoll)
					{
						NewRotation.Roll = Roll;
					}

					const bool bMoved = EditorActorSubsystem->SetActorTransform(
						Actor,
						FTransform(NewRotation, NewLocation, Actor->GetActorScale3D()));

					TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
					ActorObject->SetNumberField(TEXT("index"), ActorIndex);
					ActorObject->SetObjectField(TEXT("beforeLocation"), UnrealMcp::MakeVectorObject(BeforeLocation));
					ActorObject->SetObjectField(TEXT("beforeRotation"), UnrealMcp::MakeRotatorObject(BeforeRotation));
					ActorObject->SetObjectField(TEXT("afterLocation"), UnrealMcp::MakeVectorObject(Actor->GetActorLocation()));
					ActorObject->SetObjectField(TEXT("afterRotation"), UnrealMcp::MakeRotatorObject(Actor->GetActorRotation()));
					ActorObject->SetBoolField(TEXT("success"), bMoved);
					ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

					if (bMoved)
					{
						++SuccessCount;
					}
					else
					{
						++FailureCount;
					}

					TextLines.Add(FString::Printf(
						TEXT("%s -> [%.1f, %.1f, %.1f] success=%s"),
						*Actor->GetActorLabel(),
						Actor->GetActorLocation().X,
						Actor->GetActorLocation().Y,
						Actor->GetActorLocation().Z,
						bMoved ? TEXT("true") : TEXT("false")));
				}

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("filter"), Query.FilterText);
				StructuredContent->SetStringField(TEXT("classPath"), Query.ClassPathFilter);
				StructuredContent->SetBoolField(TEXT("selectedOnly"), Query.bSelectedOnly);
				StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
				StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
				StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
				StructuredContent->SetNumberField(TEXT("columns"), Columns);
				StructuredContent->SetNumberField(TEXT("spacingX"), SpacingX);
				StructuredContent->SetNumberField(TEXT("spacingY"), SpacingY);
				StructuredContent->SetNumberField(TEXT("spacingZ"), SpacingZ);
				StructuredContent->SetObjectField(TEXT("origin"), UnrealMcp::MakeVectorObject(Origin));
				StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
				StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
				StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

				FString Text = FString::Printf(
					TEXT("Laid out %d actors into a grid. success=%d failure=%d columns=%d"),
					Query.Actors.Num(),
					SuccessCount,
					FailureCount,
					Columns);
				if (TextLines.Num() > 0)
				{
					Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
				}

				return UnrealMcp::MakeExecutionResult(Text, StructuredContent, FailureCount > 0);
			}

			if (ToolName == TEXT("unreal.layout_actors_circle"))
			{
				if (UnrealMcp::IsEditorPlaying())
				{
					return UnrealMcp::MakePieBlockedResult(ToolName);
				}

				if (!EditorActorSubsystem)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
				}

				UnrealMcp::FActorQueryResult Query;
				FString FailureReason;
				if (!UnrealMcp::ResolveActorsFromArguments(EditorActorSubsystem, Arguments, Query, FailureReason))
				{
					return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
				}

				double Radius = 1000.0;
				double StartAngleDegrees = 0.0;
				double ArcDegrees = 360.0;
				double YawOffset = 0.0;
				bool bAlignYawToCenter = false;
				Arguments.TryGetNumberField(TEXT("radius"), Radius);
				Arguments.TryGetNumberField(TEXT("startAngleDegrees"), StartAngleDegrees);
				Arguments.TryGetNumberField(TEXT("arcDegrees"), ArcDegrees);
				Arguments.TryGetNumberField(TEXT("yawOffset"), YawOffset);
				Arguments.TryGetBoolField(TEXT("alignYawToCenter"), bAlignYawToCenter);

				if (Radius <= 0.0)
				{
					return UnrealMcp::MakeExecutionResult(TEXT("radius must be greater than zero."), nullptr, true);
				}

				FVector Center = Query.Actors[0]->GetActorLocation();
				double NumericValue = 0.0;
				const bool bHasCenterX = Arguments.TryGetNumberField(TEXT("centerX"), NumericValue);
				if (bHasCenterX)
				{
					Center.X = NumericValue;
				}
				const bool bHasCenterY = Arguments.TryGetNumberField(TEXT("centerY"), NumericValue);
				if (bHasCenterY)
				{
					Center.Y = NumericValue;
				}
				const bool bHasCenterZ = Arguments.TryGetNumberField(TEXT("centerZ"), NumericValue);
				if (bHasCenterZ)
				{
					Center.Z = NumericValue;
				}

				const int32 ActorCount = Query.Actors.Num();
				const double NormalizedArc = FMath::Fmod(FMath::Abs(ArcDegrees), 360.0);
				const bool bFullCircle = FMath::IsNearlyZero(NormalizedArc, KINDA_SMALL_NUMBER);
				const double StepDenominator = ActorCount <= 1
					? 1.0
					: (bFullCircle ? static_cast<double>(ActorCount) : static_cast<double>(ActorCount - 1));

				FScopedTransaction Transaction(NSLOCTEXT("UnrealMcp", "LayoutActorsCircle", "Unreal MCP Layout Actors Circle"));

				int32 SuccessCount = 0;
				int32 FailureCount = 0;
				TArray<TSharedPtr<FJsonValue>> ActorResults;
				TArray<FString> TextLines;

				for (int32 ActorIndex = 0; ActorIndex < ActorCount; ++ActorIndex)
				{
					AActor* Actor = Query.Actors[ActorIndex];
					const FVector BeforeLocation = Actor->GetActorLocation();
					const FRotator BeforeRotation = Actor->GetActorRotation();

					const double Alpha = ActorCount <= 1 ? 0.0 : static_cast<double>(ActorIndex) / StepDenominator;
					const double AngleDegrees = StartAngleDegrees + (ArcDegrees * Alpha);
					const double AngleRadians = FMath::DegreesToRadians(AngleDegrees);

					const FVector NewLocation(
						Center.X + FMath::Cos(AngleRadians) * Radius,
						Center.Y + FMath::Sin(AngleRadians) * Radius,
						bHasCenterZ ? Center.Z : BeforeLocation.Z);

					FRotator NewRotation = BeforeRotation;
					if (bAlignYawToCenter)
					{
						const FVector DirectionToCenter = Center - NewLocation;
						NewRotation.Yaw = FMath::RadiansToDegrees(FMath::Atan2(DirectionToCenter.Y, DirectionToCenter.X)) + YawOffset;
					}

					const bool bMoved = EditorActorSubsystem->SetActorTransform(
						Actor,
						FTransform(NewRotation, NewLocation, Actor->GetActorScale3D()));

					TSharedPtr<FJsonObject> ActorObject = UnrealMcp::MakeActorObject(Actor);
					ActorObject->SetNumberField(TEXT("index"), ActorIndex);
					ActorObject->SetNumberField(TEXT("angleDegrees"), AngleDegrees);
					ActorObject->SetObjectField(TEXT("beforeLocation"), UnrealMcp::MakeVectorObject(BeforeLocation));
					ActorObject->SetObjectField(TEXT("beforeRotation"), UnrealMcp::MakeRotatorObject(BeforeRotation));
					ActorObject->SetObjectField(TEXT("afterLocation"), UnrealMcp::MakeVectorObject(Actor->GetActorLocation()));
					ActorObject->SetObjectField(TEXT("afterRotation"), UnrealMcp::MakeRotatorObject(Actor->GetActorRotation()));
					ActorObject->SetBoolField(TEXT("success"), bMoved);
					ActorResults.Add(MakeShared<FJsonValueObject>(ActorObject));

					if (bMoved)
					{
						++SuccessCount;
					}
					else
					{
						++FailureCount;
					}

					TextLines.Add(FString::Printf(
						TEXT("%s -> [%.1f, %.1f, %.1f] angle=%.2f success=%s"),
						*Actor->GetActorLabel(),
						Actor->GetActorLocation().X,
						Actor->GetActorLocation().Y,
						Actor->GetActorLocation().Z,
						AngleDegrees,
						bMoved ? TEXT("true") : TEXT("false")));
				}

				TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
				StructuredContent->SetStringField(TEXT("filter"), Query.FilterText);
				StructuredContent->SetStringField(TEXT("classPath"), Query.ClassPathFilter);
				StructuredContent->SetBoolField(TEXT("selectedOnly"), Query.bSelectedOnly);
				StructuredContent->SetNumberField(TEXT("matchCount"), Query.MatchCount);
				StructuredContent->SetNumberField(TEXT("returnedCount"), Query.Actors.Num());
				StructuredContent->SetBoolField(TEXT("truncated"), Query.bTruncated);
				StructuredContent->SetNumberField(TEXT("radius"), Radius);
				StructuredContent->SetNumberField(TEXT("startAngleDegrees"), StartAngleDegrees);
				StructuredContent->SetNumberField(TEXT("arcDegrees"), ArcDegrees);
				StructuredContent->SetObjectField(TEXT("center"), UnrealMcp::MakeVectorObject(Center));
				StructuredContent->SetBoolField(TEXT("alignYawToCenter"), bAlignYawToCenter);
				StructuredContent->SetNumberField(TEXT("yawOffset"), YawOffset);
				StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
				StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
				StructuredContent->SetArrayField(TEXT("actors"), ActorResults);

				FString Text = FString::Printf(
					TEXT("Laid out %d actors into a circle/arc. success=%d failure=%d radius=%.1f arc=%.1f"),
					Query.Actors.Num(),
					SuccessCount,
					FailureCount,
					Radius,
					ArcDegrees);
				if (TextLines.Num() > 0)
				{
					Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
				}

				return UnrealMcp::MakeExecutionResult(Text, StructuredContent, FailureCount > 0);
			}

		if (ToolName == TEXT("unreal.open_map"))
		{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}

		FString MapPath;
		if (!Arguments.TryGetStringField(TEXT("path"), MapPath) || MapPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The path argument is required."), nullptr, true);
		}

		FString FailureReason;
		const FString ObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(MapPath, FailureReason);
		if (ObjectPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to resolve map path: %s"), *FailureReason), nullptr, true);
		}

		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		const bool bLoaded = UEditorLoadingAndSavingUtils::LoadMap(ObjectPath) != nullptr;
		if (!bLoaded)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to open map '%s'."), *ObjectPath), nullptr, true);
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("path"), ObjectPath);
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Opened map %s."), *ObjectPath), StructuredContent, false);
	}

	if (ToolName == TEXT("unreal.open_asset"))
	{
		if (!EditorAssetSubsystem || !AssetEditorSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Asset editor subsystems are unavailable."), nullptr, true);
		}

		FString AssetPath;
		if (!Arguments.TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The path argument is required."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UObject* Asset = UnrealMcp::LoadAssetFromAnyPath(EditorAssetSubsystem, AssetPath, ObjectPath, FailureReason);
		if (!Asset)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		const bool bOpened = AssetEditorSubsystem->OpenEditorForAsset(Asset);
		if (!bOpened)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to open an editor for '%s'."), *ObjectPath), nullptr, true);
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
		StructuredContent->SetStringField(TEXT("classPath"), Asset->GetClass()->GetPathName());
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Opened asset %s."), *ObjectPath), StructuredContent, false);
	}

	if (ToolName == TEXT("unreal.sync_content_browser"))
	{
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString RequestedPath;
		if (!Arguments.TryGetStringField(TEXT("path"), RequestedPath) || RequestedPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The path argument is required."), nullptr, true);
		}

		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		const FAssetData AssetData = EditorAssetSubsystem->FindAssetData(RequestedPath);
		if (AssetData.IsValid())
		{
			ContentBrowserModule.Get().SyncBrowserToAssets(TArray<FAssetData>{AssetData}, false, true);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("objectPath"), AssetData.GetSoftObjectPath().ToString());
			return UnrealMcp::MakeExecutionResult(
				FString::Printf(TEXT("Synced Content Browser to asset %s."), *AssetData.GetSoftObjectPath().ToString()),
				StructuredContent,
				false);
		}

		FString FailureReason;
		const FString FolderPath = EditorScriptingHelpers::ConvertAnyPathToLongPackagePath(RequestedPath, FailureReason);
		if (FolderPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to resolve path '%s': %s"), *RequestedPath, *FailureReason), nullptr, true);
		}

		ContentBrowserModule.Get().SyncBrowserToFolders(TArray<FString>{FolderPath}, false, true);

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("folderPath"), FolderPath);
		return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Synced Content Browser to folder %s."), *FolderPath), StructuredContent, false);
	}

	if (ToolName == TEXT("unreal.spawn_actor_basic"))
	{
		return ExecuteTool(TEXT("unreal.spawn_actor"), Arguments);
	}

	if (ToolName == TEXT("unreal.spawn_actor_batch_basic"))
	{
		return ExecuteTool(TEXT("unreal.spawn_actor_batch"), Arguments);
	}

	if (ToolName == TEXT("unreal.spawn_static_mesh_actor"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}

		if (!EditorActorSubsystem || !EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Editor actor subsystems are unavailable."), nullptr, true);
		}

		FString StaticMeshPath;
		if (!Arguments.TryGetStringField(TEXT("staticMeshPath"), StaticMeshPath) || StaticMeshPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The staticMeshPath argument is required."), nullptr, true);
		}

			FString MaterialPath;
			Arguments.TryGetStringField(TEXT("materialPath"), MaterialPath);
			double MaterialSlotValue = 0.0;
			Arguments.TryGetNumberField(TEXT("materialSlot"), MaterialSlotValue);
			const int32 MaterialSlot = FMath::Max(0, static_cast<int32>(MaterialSlotValue));

		FString MeshObjectPath;
		FString FailureReason;
		UObject* LoadedMeshAsset = UnrealMcp::LoadAssetFromAnyPath(EditorAssetSubsystem, StaticMeshPath, MeshObjectPath, FailureReason);
		UStaticMesh* StaticMeshAsset = Cast<UStaticMesh>(LoadedMeshAsset);
		if (!StaticMeshAsset)
		{
			return UnrealMcp::MakeExecutionResult(
				LoadedMeshAsset
					? FString::Printf(TEXT("Asset '%s' is not a StaticMesh."), *MeshObjectPath)
					: FailureReason,
				nullptr,
				true);
		}

		UMaterialInterface* MaterialAsset = nullptr;
		FString MaterialObjectPath;
		if (!MaterialPath.TrimStartAndEnd().IsEmpty())
		{
			UObject* LoadedMaterialAsset = UnrealMcp::LoadAssetFromAnyPath(EditorAssetSubsystem, MaterialPath, MaterialObjectPath, FailureReason);
			MaterialAsset = Cast<UMaterialInterface>(LoadedMaterialAsset);
			if (!MaterialAsset)
			{
				return UnrealMcp::MakeExecutionResult(
					LoadedMaterialAsset
						? FString::Printf(TEXT("Asset '%s' is not a material or material instance."), *MaterialObjectPath)
						: FailureReason,
					nullptr,
					true);
			}
		}

		TSharedPtr<FJsonObject> SpawnArguments = UnrealMcp::MakeEmptyObject();
		SpawnArguments->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.StaticMeshActor"));
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("x"), SpawnArguments);
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("y"), SpawnArguments);
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("z"), SpawnArguments);
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("pitch"), SpawnArguments);
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("yaw"), SpawnArguments);
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("roll"), SpawnArguments);
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("sx"), SpawnArguments);
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("sy"), SpawnArguments);
		UnrealMcp::CopyNumberFieldIfPresent(Arguments, TEXT("sz"), SpawnArguments);
		UnrealMcp::CopyStringFieldIfPresent(Arguments, TEXT("label"), SpawnArguments);

		FUnrealMcpExecutionResult SpawnResult = ExecuteTool(TEXT("unreal.spawn_actor"), *SpawnArguments);
		if (SpawnResult.bIsError || !SpawnResult.StructuredContent.IsValid())
		{
			return SpawnResult;
		}

		FString SpawnedActorPath;
		if (!SpawnResult.StructuredContent->TryGetStringField(TEXT("path"), SpawnedActorPath) || SpawnedActorPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The spawned StaticMeshActor did not report a valid actor path."), nullptr, true);
		}

		AActor* SpawnedActor = UnrealMcp::ResolveActorReference(EditorActorSubsystem, SpawnedActorPath, FString(), FailureReason);
		if (!SpawnedActor)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UStaticMeshComponent* StaticMeshComponent = SpawnedActor->FindComponentByClass<UStaticMeshComponent>();
		if (!StaticMeshComponent)
		{
			return UnrealMcp::MakeExecutionResult(
				FString::Printf(TEXT("Spawned actor '%s' does not have a StaticMeshComponent."), *SpawnedActor->GetActorLabel()),
				nullptr,
				true);
		}

		SpawnedActor->Modify();
		StaticMeshComponent->Modify();
		StaticMeshComponent->SetStaticMesh(StaticMeshAsset);
		if (MaterialAsset)
		{
			StaticMeshComponent->SetMaterial(MaterialSlot, MaterialAsset);
		}
		StaticMeshComponent->MarkPackageDirty();
		SpawnedActor->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeActorObject(SpawnedActor);
		StructuredContent->SetObjectField(TEXT("scale"), UnrealMcp::MakeVectorObject(SpawnedActor->GetActorScale3D()));
		StructuredContent->SetStringField(TEXT("staticMeshPath"), MeshObjectPath);
		if (MaterialAsset)
		{
			StructuredContent->SetStringField(TEXT("materialPath"), MaterialObjectPath);
			StructuredContent->SetNumberField(TEXT("materialSlot"), MaterialSlot);
		}

		FString Text = FString::Printf(TEXT("Spawned StaticMeshActor %s with mesh %s."), *SpawnedActor->GetActorLabel(), *MeshObjectPath);
		if (MaterialAsset)
		{
			Text += FString::Printf(TEXT(" Applied material %s to slot %d."), *MaterialObjectPath, MaterialSlot);
		}

		return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
	}

	if (ToolName == TEXT("unreal.spawn_actor"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}

		if (!EditorActorSubsystem || !EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Editor actor subsystems are unavailable."), nullptr, true);
		}

		FString ClassPath;
		if (!Arguments.TryGetStringField(TEXT("classPath"), ClassPath) || ClassPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The classPath argument is required."), nullptr, true);
		}

		UClass* ResolvedClass = UnrealMcp::ResolveClassPath(ClassPath, EditorAssetSubsystem);
		if (!ResolvedClass)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to resolve class '%s'."), *ClassPath), nullptr, true);
		}

		if (!ResolvedClass->IsChildOf(AActor::StaticClass()))
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Class '%s' is not an Actor class."), *ResolvedClass->GetPathName()), nullptr, true);
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
			double Pitch = 0.0;
			double Yaw = 0.0;
			double Roll = 0.0;
			double ScaleX = 1.0;
			double ScaleY = 1.0;
			double ScaleZ = 1.0;
			bool bHasScaleX = false;
			bool bHasScaleY = false;
			bool bHasScaleZ = false;
			FString Label;

			Arguments.TryGetNumberField(TEXT("x"), X);
			Arguments.TryGetNumberField(TEXT("y"), Y);
			Arguments.TryGetNumberField(TEXT("z"), Z);
			Arguments.TryGetNumberField(TEXT("pitch"), Pitch);
			Arguments.TryGetNumberField(TEXT("yaw"), Yaw);
			Arguments.TryGetNumberField(TEXT("roll"), Roll);
			bHasScaleX = Arguments.TryGetNumberField(TEXT("sx"), ScaleX);
			bHasScaleY = Arguments.TryGetNumberField(TEXT("sy"), ScaleY);
			bHasScaleZ = Arguments.TryGetNumberField(TEXT("sz"), ScaleZ);
			Arguments.TryGetStringField(TEXT("label"), Label);

			AActor* NewActor = EditorActorSubsystem->SpawnActorFromClass(
				ResolvedClass,
				FVector(X, Y, Z),
			FRotator(Pitch, Yaw, Roll),
			false);

		if (!NewActor)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to spawn actor from class '%s'."), *ResolvedClass->GetPathName()), nullptr, true);
		}

			if (!Label.IsEmpty())
			{
				FActorLabelUtilities::SetActorLabelUnique(NewActor, Label);
			}

			if (bHasScaleX || bHasScaleY || bHasScaleZ)
			{
				const FVector CurrentScale = NewActor->GetActorScale3D();
				NewActor->SetActorScale3D(FVector(
					bHasScaleX ? ScaleX : CurrentScale.X,
					bHasScaleY ? ScaleY : CurrentScale.Y,
					bHasScaleZ ? ScaleZ : CurrentScale.Z));
			}

			TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeActorObject(NewActor);
			StructuredContent->SetObjectField(TEXT("scale"), UnrealMcp::MakeVectorObject(NewActor->GetActorScale3D()));

			const TSharedPtr<FJsonObject>* PropertyValuesObject = nullptr;
			int32 PropertySuccessCount = 0;
			int32 PropertyFailureCount = 0;
			TArray<TSharedPtr<FJsonValue>> PropertyEdits;
			if (Arguments.TryGetObjectField(TEXT("properties"), PropertyValuesObject) && PropertyValuesObject && (*PropertyValuesObject)->Values.Num() > 0)
			{
				UnrealMcp::ApplyPropertyMapToActor(NewActor, **PropertyValuesObject, PropertyEdits, PropertySuccessCount, PropertyFailureCount);
				StructuredContent->SetNumberField(TEXT("propertySuccessCount"), PropertySuccessCount);
				StructuredContent->SetNumberField(TEXT("propertyFailureCount"), PropertyFailureCount);
				StructuredContent->SetArrayField(TEXT("propertyEdits"), PropertyEdits);
			}

			FString Text = FString::Printf(TEXT("Spawned actor %s."), *NewActor->GetActorLabel());
			if (PropertySuccessCount > 0 || PropertyFailureCount > 0)
			{
				Text += FString::Printf(
					TEXT(" propertySuccess=%d propertyFailure=%d"),
					PropertySuccessCount,
					PropertyFailureCount);
			}

			return UnrealMcp::MakeExecutionResult(
				Text,
				StructuredContent,
				PropertyFailureCount > 0);
		}

		if (ToolName == TEXT("unreal.spawn_actor_batch"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			if (!EditorActorSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
			}

			TArray<TSharedPtr<FJsonObject>> SpawnItems;
			if (!UnrealMcp::TryGetObjectArrayField(Arguments, TEXT("items"), SpawnItems) || SpawnItems.Num() == 0)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("The items argument must be a non-empty array of objects."), nullptr, true);
			}

			FString DefaultClassPath;
			bool bSelectSpawned = true;
			Arguments.TryGetStringField(TEXT("classPath"), DefaultClassPath);
			Arguments.TryGetBoolField(TEXT("selectSpawned"), bSelectSpawned);

			TArray<TSharedPtr<FJsonValue>> ResultItems;
			TArray<AActor*> SpawnedActors;
			TArray<FString> TextLines;
			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			int32 PropertyFailureCount = 0;

				for (int32 ItemIndex = 0; ItemIndex < SpawnItems.Num(); ++ItemIndex)
				{
					TSharedPtr<FJsonObject> MergedArguments = MakeShared<FJsonObject>();
					MergedArguments->Values = SpawnItems[ItemIndex]->Values;

				if (!MergedArguments->HasField(TEXT("classPath")) && !DefaultClassPath.IsEmpty())
				{
					MergedArguments->SetStringField(TEXT("classPath"), DefaultClassPath);
				}

				FUnrealMcpExecutionResult SpawnResult = ExecuteTool(TEXT("unreal.spawn_actor"), *MergedArguments);

					TSharedPtr<FJsonObject> ItemResult = SpawnResult.StructuredContent.IsValid()
						? MakeShared<FJsonObject>(*SpawnResult.StructuredContent)
						: MakeShared<FJsonObject>();
					bool bSpawnedActor = false;

					if (SpawnResult.StructuredContent.IsValid())
					{
						double ItemPropertyFailureCount = 0.0;
						if (SpawnResult.StructuredContent->TryGetNumberField(TEXT("propertyFailureCount"), ItemPropertyFailureCount))
						{
							PropertyFailureCount += static_cast<int32>(ItemPropertyFailureCount);
						}

						FString ActorPath;
						if (SpawnResult.StructuredContent->TryGetStringField(TEXT("path"), ActorPath))
						{
							bSpawnedActor = true;

							FString ResolveFailureReason;
							if (AActor* SpawnedActor = UnrealMcp::ResolveActorReference(EditorActorSubsystem, ActorPath, FString(), ResolveFailureReason))
							{
								SpawnedActors.Add(SpawnedActor);
							}
						}
					}

					ItemResult->SetNumberField(TEXT("index"), ItemIndex);
					ItemResult->SetBoolField(TEXT("success"), !SpawnResult.bIsError);
					ItemResult->SetBoolField(TEXT("spawned"), bSpawnedActor);
					ItemResult->SetBoolField(TEXT("toolError"), SpawnResult.bIsError);
					ItemResult->SetStringField(TEXT("text"), SpawnResult.Text);
					ResultItems.Add(MakeShared<FJsonValueObject>(ItemResult));

					if (bSpawnedActor)
					{
						++SuccessCount;
					}
					else
					{
						++FailureCount;
					}

					TextLines.Add(FString::Printf(TEXT("[%d] %s"), ItemIndex, *SpawnResult.Text));
				}

			if (bSelectSpawned)
			{
				EditorActorSubsystem->SelectNothing();
				for (AActor* SpawnedActor : SpawnedActors)
				{
					EditorActorSubsystem->SetActorSelectionState(SpawnedActor, true);
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("defaultClassPath"), DefaultClassPath);
			StructuredContent->SetBoolField(TEXT("selectSpawned"), bSelectSpawned);
			StructuredContent->SetNumberField(TEXT("requestedCount"), SpawnItems.Num());
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetNumberField(TEXT("propertyFailureCount"), PropertyFailureCount);
			StructuredContent->SetArrayField(TEXT("items"), ResultItems);

			FString Text = FString::Printf(
				TEXT("Spawned %d/%d actors in batch. failures=%d propertyFailures=%d"),
				SuccessCount,
				SpawnItems.Num(),
				FailureCount,
				PropertyFailureCount);
			if (TextLines.Num() > 0)
			{
				Text += TEXT("\n") + FString::Join(TextLines, TEXT("\n"));
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, FailureCount > 0 || PropertyFailureCount > 0);
		}

		if (ToolName == TEXT("unreal.destroy_selected_actors"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}

		if (!EditorActorSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorActorSubsystem is unavailable."), nullptr, true);
		}

		const TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
		if (SelectedActors.Num() == 0)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("No selected actors to destroy."), nullptr, false);
		}

		const int32 ActorCount = SelectedActors.Num();
		const bool bDestroyed = EditorActorSubsystem->DestroyActors(SelectedActors);

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetNumberField(TEXT("count"), ActorCount);
		StructuredContent->SetBoolField(TEXT("destroyed"), bDestroyed);

		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Destroy selected actors result: destroyed=%s count=%d"), bDestroyed ? TEXT("true") : TEXT("false"), ActorCount),
			StructuredContent,
			!bDestroyed);
	}

	if (ToolName == TEXT("unreal.bp_add_variable"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		FString VariableName;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("name"), VariableName) || VariableName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'name'."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		FEdGraphPinType PinType;
		if (!UnrealMcp::BuildBlueprintPinType(Arguments, EditorAssetSubsystem, PinType, FailureReason))
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		FString DefaultValue;
		Arguments.TryGetStringField(TEXT("defaultValue"), DefaultValue);

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddVariable", "Unreal MCP Add Blueprint Variable"));
		Blueprint->Modify();
		const FName VarFName(*VariableName.TrimStartAndEnd());
		const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, VarFName, PinType, DefaultValue);
		if (!bAdded)
		{
			return UnrealMcp::MakeExecutionResult(
				FString::Printf(TEXT("Failed to add variable '%s' to %s. It may already exist or conflict with a parent member."), *VariableName, *ObjectPath),
				nullptr,
				true);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, nullptr, nullptr, TEXT("bp_add_variable"));
		StructuredContent->SetStringField(TEXT("variableName"), VarFName.ToString());
		StructuredContent->SetStringField(TEXT("pinCategory"), PinType.PinCategory.ToString());
		StructuredContent->SetStringField(TEXT("pinSubCategory"), PinType.PinSubCategory.ToString());
		StructuredContent->SetStringField(TEXT("defaultValue"), DefaultValue);

		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Added Blueprint variable %s to %s."), *VarFName.ToString(), *ObjectPath),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_add_function"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		FString FunctionName;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("functionName"), FunctionName) || FunctionName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'functionName'."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* ExistingGraph : Graphs)
		{
			if (ExistingGraph && ExistingGraph->GetName().Equals(FunctionName.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, ExistingGraph, nullptr, TEXT("bp_add_function"));
				StructuredContent->SetBoolField(TEXT("created"), false);
				return UnrealMcp::MakeExecutionResult(
					FString::Printf(TEXT("Function graph %s already exists in %s."), *ExistingGraph->GetName(), *ObjectPath),
					StructuredContent,
					false);
			}
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddFunction", "Unreal MCP Add Blueprint Function"));
		Blueprint->Modify();
		UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			FName(*FunctionName.TrimStartAndEnd()),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, FunctionGraph, true, nullptr);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, FunctionGraph, nullptr, TEXT("bp_add_function"));
		StructuredContent->SetBoolField(TEXT("created"), true);
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Created function graph %s in %s."), *FunctionGraph->GetName(), *ObjectPath),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_add_event_node"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}

		FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
		FString EventName = TEXT("ReceiveBeginPlay");
		FString OwnerClassPath = TEXT("/Script/Engine.Actor");
		double X = 0.0;
		double Y = 0.0;
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);
		Arguments.TryGetStringField(TEXT("eventName"), EventName);
		Arguments.TryGetStringField(TEXT("ownerClassPath"), OwnerClassPath);
		Arguments.TryGetNumberField(TEXT("x"), X);
		Arguments.TryGetNumberField(TEXT("y"), Y);

		if (EventName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'eventName'."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UEdGraph* Graph = UnrealMcp::ResolveBlueprintGraph(Blueprint, GraphName, true, FailureReason);
		if (!Graph)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		const FName EventFName(*EventName.TrimStartAndEnd());
		UEdGraphNode* NewNode = nullptr;
		const bool bCustomEvent = OwnerClassPath.TrimStartAndEnd().Equals(TEXT("custom"), ESearchCase::IgnoreCase)
			|| OwnerClassPath.TrimStartAndEnd().IsEmpty();

		if (!bCustomEvent)
		{
			UClass* OwnerClass = UnrealMcp::ResolveClassPath(OwnerClassPath, EditorAssetSubsystem);
			if (!OwnerClass)
			{
				return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to resolve ownerClassPath '%s'."), *OwnerClassPath), nullptr, true);
			}
			if (!OwnerClass->FindFunctionByName(EventFName))
			{
				return UnrealMcp::MakeExecutionResult(
					FString::Printf(TEXT("Event function '%s' was not found on class '%s'."), *EventName, *OwnerClass->GetPathName()),
					nullptr,
					true);
			}

			if (UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, OwnerClass, EventFName))
			{
				TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, ExistingEvent->GetGraph(), ExistingEvent, TEXT("bp_add_event_node"));
				StructuredContent->SetBoolField(TEXT("created"), false);
				return UnrealMcp::MakeExecutionResult(
					FString::Printf(TEXT("Override event %s already exists in %s."), *EventName, *ObjectPath),
					StructuredContent,
					false);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddEventNode", "Unreal MCP Add Blueprint Event Node"));
			Graph->Modify();
			Blueprint->Modify();
			NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Event>(
				Graph,
				FVector2D(X, Y),
				EK2NewNodeFlags::None,
				[EventFName, OwnerClass](UK2Node_Event* NewInstance)
				{
					NewInstance->EventReference.SetExternalMember(EventFName, OwnerClass);
					NewInstance->bOverrideFunction = true;
				});
		}
		else
		{
			if (UK2Node_Event* ExistingEvent = FBlueprintEditorUtils::FindCustomEventNode(Blueprint, EventFName))
			{
				TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, ExistingEvent->GetGraph(), ExistingEvent, TEXT("bp_add_event_node"));
				StructuredContent->SetBoolField(TEXT("created"), false);
				return UnrealMcp::MakeExecutionResult(
					FString::Printf(TEXT("Custom event %s already exists in %s."), *EventName, *ObjectPath),
					StructuredContent,
					false);
			}

			const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddCustomEventNode", "Unreal MCP Add Blueprint Custom Event Node"));
			Graph->Modify();
			Blueprint->Modify();
			NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CustomEvent>(
				Graph,
				FVector2D(X, Y),
				EK2NewNodeFlags::None,
				[EventFName](UK2Node_CustomEvent* NewInstance)
				{
					NewInstance->CustomFunctionName = EventFName;
				});
		}

		if (!NewNode)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Failed to create event node."), nullptr, true);
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, Graph, NewNode, TEXT("bp_add_event_node"));
		StructuredContent->SetBoolField(TEXT("created"), true);
		StructuredContent->SetBoolField(TEXT("customEvent"), bCustomEvent);
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Added event node %s to %s:%s."), *EventName, *ObjectPath, *Graph->GetName()),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_add_call_function_node"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		FString FunctionClassPath;
		FString FunctionName;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("functionClassPath"), FunctionClassPath) || FunctionClassPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'functionClassPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("functionName"), FunctionName) || FunctionName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'functionName'."), nullptr, true);
		}

		FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
		double X = 200.0;
		double Y = 0.0;
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);
		Arguments.TryGetNumberField(TEXT("x"), X);
		Arguments.TryGetNumberField(TEXT("y"), Y);

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}
		UEdGraph* Graph = UnrealMcp::ResolveBlueprintGraph(Blueprint, GraphName, true, FailureReason);
		if (!Graph)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}
		UFunction* Function = UnrealMcp::ResolveFunctionByClassAndName(EditorAssetSubsystem, FunctionClassPath, FunctionName, FailureReason);
		if (!Function)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddCallFunctionNode", "Unreal MCP Add Blueprint Call Function Node"));
		Graph->Modify();
		Blueprint->Modify();
		UK2Node_CallFunction* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(
			Graph,
			FVector2D(X, Y),
			EK2NewNodeFlags::None,
			[Function](UK2Node_CallFunction* NewInstance)
			{
				NewInstance->SetFromFunction(Function);
			});
		if (!NewNode)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Failed to create call-function node."), nullptr, true);
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, Graph, NewNode, TEXT("bp_add_call_function_node"));
		StructuredContent->SetStringField(TEXT("function"), Function->GetPathName());
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Added call-function node %s to %s:%s."), *FunctionName, *ObjectPath, *Graph->GetName()),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_add_branch_node"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}

		FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
		double X = 400.0;
		double Y = 0.0;
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);
		Arguments.TryGetNumberField(TEXT("x"), X);
		Arguments.TryGetNumberField(TEXT("y"), Y);

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}
		UEdGraph* Graph = UnrealMcp::ResolveBlueprintGraph(Blueprint, GraphName, true, FailureReason);
		if (!Graph)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddBranchNode", "Unreal MCP Add Blueprint Branch Node"));
		Graph->Modify();
		Blueprint->Modify();
		UK2Node_IfThenElse* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_IfThenElse>(
			Graph,
			FVector2D(X, Y),
			EK2NewNodeFlags::None);
		if (!NewNode)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Failed to create Branch node."), nullptr, true);
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, Graph, NewNode, TEXT("bp_add_branch_node"));
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Added Branch node to %s:%s."), *ObjectPath, *Graph->GetName()),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_add_for_each_node"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}

		FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
		FString MacroName = TEXT("ForEachLoop");
		double X = 400.0;
		double Y = 180.0;
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);
		Arguments.TryGetStringField(TEXT("macroName"), MacroName);
		Arguments.TryGetNumberField(TEXT("x"), X);
		Arguments.TryGetNumberField(TEXT("y"), Y);

		UEdGraph* MacroGraph = UnrealMcp::FindStandardMacroGraph(MacroName);
		if (!MacroGraph)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to find StandardMacros graph '%s'."), *MacroName), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}
		UEdGraph* Graph = UnrealMcp::ResolveBlueprintGraph(Blueprint, GraphName, true, FailureReason);
		if (!Graph)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpAddForEachNode", "Unreal MCP Add Blueprint ForEach Node"));
		Graph->Modify();
		Blueprint->Modify();
		UK2Node_MacroInstance* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_MacroInstance>(
			Graph,
			FVector2D(X, Y),
			EK2NewNodeFlags::None,
			[MacroGraph](UK2Node_MacroInstance* NewInstance)
			{
				NewInstance->SetMacroGraph(MacroGraph);
			});
		if (!NewNode)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Failed to create ForEach macro node."), nullptr, true);
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, Graph, NewNode, TEXT("bp_add_for_each_node"));
		StructuredContent->SetStringField(TEXT("macroName"), MacroGraph->GetName());
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Added %s node to %s:%s."), *MacroGraph->GetName(), *ObjectPath, *Graph->GetName()),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_connect_pins"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		FString FromNodeGuid;
		FString FromPinName;
		FString ToNodeGuid;
		FString ToPinName;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("fromNodeGuid"), FromNodeGuid) || FromNodeGuid.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'fromNodeGuid'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("fromPin"), FromPinName) || FromPinName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'fromPin'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("toNodeGuid"), ToNodeGuid) || ToNodeGuid.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'toNodeGuid'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("toPin"), ToPinName) || ToPinName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'toPin'."), nullptr, true);
		}

		FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}
		UEdGraph* Graph = UnrealMcp::ResolveBlueprintGraph(Blueprint, GraphName, false, FailureReason);
		if (!Graph)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UEdGraphNode* FromNode = UnrealMcp::FindBlueprintNodeByGuid(Graph, FromNodeGuid);
		UEdGraphNode* ToNode = UnrealMcp::FindBlueprintNodeByGuid(Graph, ToNodeGuid);
		if (!FromNode || !ToNode)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Unable to find one or both nodes by GUID in the target graph."), nullptr, true);
		}

		UEdGraphPin* FromPin = UnrealMcp::FindBlueprintPinByName(FromNode, FromPinName);
		UEdGraphPin* ToPin = UnrealMcp::FindBlueprintPinByName(ToNode, ToPinName);
		if (!FromPin || !ToPin)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Unable to find one or both pins by name/displayName."), nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpConnectPins", "Unreal MCP Connect Blueprint Pins"));
		Graph->Modify();
		FromNode->Modify();
		ToNode->Modify();
		const bool bConnected = Graph->GetSchema() && Graph->GetSchema()->TryCreateConnection(FromPin, ToPin);
		if (!bConnected)
		{
			return UnrealMcp::MakeExecutionResult(
				FString::Printf(TEXT("K2 schema rejected connection %s.%s -> %s.%s."), *FromNodeGuid, *FromPinName, *ToNodeGuid, *ToPinName),
				nullptr,
				true);
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, Graph, nullptr, TEXT("bp_connect_pins"));
		StructuredContent->SetObjectField(TEXT("fromNode"), UnrealMcp::DescribeBlueprintNode(FromNode));
		StructuredContent->SetObjectField(TEXT("toNode"), UnrealMcp::DescribeBlueprintNode(ToNode));
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Connected %s.%s -> %s.%s in %s:%s."), *FromNodeGuid, *FromPinName, *ToNodeGuid, *ToPinName, *ObjectPath, *Graph->GetName()),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_set_pin_default"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		FString NodeGuid;
		FString PinName;
		FString Value;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("nodeGuid"), NodeGuid) || NodeGuid.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'nodeGuid'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("pinName"), PinName) || PinName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'pinName'."), nullptr, true);
		}
		Arguments.TryGetStringField(TEXT("value"), Value);

		FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}
		UEdGraph* Graph = UnrealMcp::ResolveBlueprintGraph(Blueprint, GraphName, false, FailureReason);
		if (!Graph)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UEdGraphNode* Node = UnrealMcp::FindBlueprintNodeByGuid(Graph, NodeGuid);
		if (!Node)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to find node '%s' in graph '%s'."), *NodeGuid, *Graph->GetName()), nullptr, true);
		}

		UEdGraphPin* Pin = UnrealMcp::FindBlueprintPinByName(Node, PinName);
		if (!Pin)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to find pin '%s' on node '%s'."), *PinName, *NodeGuid), nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpSetPinDefault", "Unreal MCP Set Blueprint Pin Default"));
		Graph->Modify();
		Node->Modify();
		if (!Graph->GetSchema())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Target graph has no schema."), nullptr, true);
		}

		Graph->GetSchema()->TrySetDefaultValue(*Pin, Value);
		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, Graph, Node, TEXT("bp_set_pin_default"));
		StructuredContent->SetObjectField(TEXT("pin"), UnrealMcp::DescribeBlueprintPin(Pin));
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Set default value for %s.%s to '%s' in %s:%s."), *NodeGuid, *PinName, *Value, *ObjectPath, *Graph->GetName()),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_arrange_graph"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}

		FString GraphName = UEdGraphSchema_K2::GN_EventGraph.ToString();
		double OriginX = 0.0;
		double OriginY = 0.0;
		double ColumnSpacing = 320.0;
		double RowSpacing = 180.0;
		Arguments.TryGetStringField(TEXT("graphName"), GraphName);
		Arguments.TryGetNumberField(TEXT("originX"), OriginX);
		Arguments.TryGetNumberField(TEXT("originY"), OriginY);
		Arguments.TryGetNumberField(TEXT("columnSpacing"), ColumnSpacing);
		Arguments.TryGetNumberField(TEXT("rowSpacing"), RowSpacing);
		const int32 Columns = FMath::Max(1, UnrealMcp::GetPositiveIntArgument(Arguments, TEXT("columns"), 4));

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}
		UEdGraph* Graph = UnrealMcp::ResolveBlueprintGraph(Blueprint, GraphName, false, FailureReason);
		if (!Graph)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpBpArrangeGraph", "Unreal MCP Arrange Blueprint Graph"));
		Graph->Modify();

		int32 ArrangedCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			const int32 Column = ArrangedCount % Columns;
			const int32 Row = ArrangedCount / Columns;
			Node->Modify();
			Node->NodePosX = static_cast<int32>(OriginX + Column * ColumnSpacing);
			Node->NodePosY = static_cast<int32>(OriginY + Row * RowSpacing);
			++ArrangedCount;
		}

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, Graph, nullptr, TEXT("bp_arrange_graph"));
		StructuredContent->SetNumberField(TEXT("arrangedCount"), ArrangedCount);
		StructuredContent->SetNumberField(TEXT("columns"), Columns);
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Arranged %d nodes in %s:%s."), ArrangedCount, *ObjectPath, *Graph->GetName()),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.bp_compile_save"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString BlueprintPath;
		if (!Arguments.TryGetStringField(TEXT("blueprintPath"), BlueprintPath) || BlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'blueprintPath'."), nullptr, true);
		}

		bool bSavePackage = true;
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

		FString ObjectPath;
		FString FailureReason;
		UBlueprint* Blueprint = UnrealMcp::LoadBlueprintAsset(EditorAssetSubsystem, BlueprintPath, ObjectPath, FailureReason);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		const bool bCompileSucceeded = Blueprint->Status != BS_Error;
		bool bSaved = false;
		if (bSavePackage)
		{
			bSaved = EditorAssetSubsystem->SaveLoadedAsset(Blueprint, false);
		}

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::MakeBlueprintEditStructuredContent(Blueprint, nullptr, nullptr, TEXT("bp_compile_save"));
		StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
		StructuredContent->SetStringField(TEXT("status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
		StructuredContent->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
		StructuredContent->SetBoolField(TEXT("savePackage"), bSavePackage);
		StructuredContent->SetBoolField(TEXT("saved"), bSaved);

		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Compiled Blueprint %s. status=%s saved=%s"), *ObjectPath, bCompileSucceeded ? TEXT("success") : TEXT("error"), bSaved ? TEXT("true") : TEXT("false")),
			StructuredContent,
			!bCompileSucceeded || (bSavePackage && !bSaved));
	}

	if (ToolName == TEXT("unreal.widget_add"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString WidgetBlueprintPath;
		FString WidgetClassName;
		if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("widgetClass"), WidgetClassName) || WidgetClassName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetClass'."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UWidgetBlueprint* WidgetBlueprint = UnrealMcp::LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
		if (!WidgetBlueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}
		if (!WidgetBlueprint->WidgetTree)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Widget Blueprint has no WidgetTree."), nullptr, true);
		}

		UClass* WidgetClass = UnrealMcp::ResolveWidgetClass(WidgetClassName, EditorAssetSubsystem);
		if (!WidgetClass)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to resolve widgetClass '%s'."), *WidgetClassName), nullptr, true);
		}

		FString RequestedWidgetName;
		FString ParentWidgetName;
		double IndexValue = -1.0;
		bool bIsVariable = true;
		Arguments.TryGetStringField(TEXT("widgetName"), RequestedWidgetName);
		Arguments.TryGetStringField(TEXT("parentWidgetName"), ParentWidgetName);
		Arguments.TryGetNumberField(TEXT("index"), IndexValue);
		Arguments.TryGetBoolField(TEXT("isVariable"), bIsVariable);

		const FString WidgetName = UnrealMcp::MakeUniqueWidgetName(WidgetBlueprint, WidgetClass, RequestedWidgetName);
		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetAdd", "Unreal MCP Add Widget"));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();

		UWidget* NewWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
		if (!NewWidget)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to construct widget '%s' of class '%s'."), *WidgetName, *WidgetClass->GetPathName()), nullptr, true);
		}
		NewWidget->Modify();
		NewWidget->bIsVariable = bIsVariable;
		NewWidget->OnCreationFromPalette();

		if (!WidgetBlueprint->WidgetTree->RootWidget && ParentWidgetName.TrimStartAndEnd().IsEmpty())
		{
			WidgetBlueprint->WidgetTree->RootWidget = NewWidget;
		}
		else
		{
			UWidget* ParentWidget = UnrealMcp::FindWidgetByName(WidgetBlueprint, ParentWidgetName);
			if (!ParentWidget)
			{
				return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Parent widget '%s' was not found."), *ParentWidgetName), nullptr, true);
			}

			UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
			if (!ParentPanel)
			{
				return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Parent widget '%s' is not a panel/content widget that can receive children."), *ParentWidget->GetName()), nullptr, true);
			}

			if (!ParentPanel->CanAddMoreChildren())
			{
				return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Parent widget '%s' cannot accept children."), *ParentWidget->GetName()), nullptr, true);
			}

			UPanelSlot* NewSlot = nullptr;
			const int32 Index = static_cast<int32>(IndexValue);
			if (Index >= 0)
			{
				NewSlot = ParentPanel->InsertChildAt(FMath::Clamp(Index, 0, ParentPanel->GetChildrenCount()), NewWidget);
			}
			else
			{
				NewSlot = ParentPanel->AddChild(NewWidget);
			}

			if (!NewSlot)
			{
				return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to add widget '%s' to parent '%s'."), *WidgetName, *ParentWidget->GetName()), nullptr, true);
			}
		}

		UnrealMcp::EnsureWidgetBlueprintGuid(WidgetBlueprint, NewWidget);

		UnrealMcp::MarkWidgetBlueprintModified(WidgetBlueprint, true);

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_add"));
		StructuredContent->SetObjectField(TEXT("widget"), UnrealMcp::DescribeWidget(NewWidget));
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Added widget %s (%s) to %s."), *NewWidget->GetName(), *WidgetClass->GetPathName(), *ObjectPath),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.widget_remove"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString WidgetBlueprintPath;
		FString WidgetName;
		if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UWidgetBlueprint* WidgetBlueprint = UnrealMcp::LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
		if (!WidgetBlueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UWidget* Widget = UnrealMcp::FindWidgetByName(WidgetBlueprint, WidgetName);
		if (!Widget)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
		}

		const FString RemovedWidgetName = Widget->GetName();
		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetRemove", "Unreal MCP Remove Widget"));
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		Widget->Modify();

		TArray<UWidget*> RemovedWidgets;
		RemovedWidgets.Add(Widget);
		TArray<UWidget*> ChildWidgets;
		UWidgetTree::GetChildWidgets(Widget, ChildWidgets);
		RemovedWidgets.Append(ChildWidgets);

		TSet<FString> RemovedWidgetNames;
		for (UWidget* RemovedWidget : RemovedWidgets)
		{
			if (RemovedWidget)
			{
				RemovedWidgetNames.Add(RemovedWidget->GetName());
			}
		}
		WidgetBlueprint->Bindings.RemoveAll([&RemovedWidgetNames](const FDelegateEditorBinding& Binding)
		{
			return RemovedWidgetNames.Contains(Binding.ObjectName);
		});

		const bool bRemoved = WidgetBlueprint->WidgetTree->RemoveWidget(Widget);
		if (WidgetBlueprint->WidgetTree->RootWidget == Widget)
		{
			WidgetBlueprint->WidgetTree->RootWidget = nullptr;
		}

		if (!bRemoved)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to remove widget '%s' from %s."), *RemovedWidgetName, *ObjectPath), nullptr, true);
		}

		for (UWidget* RemovedWidget : RemovedWidgets)
		{
			if (!RemovedWidget)
			{
				continue;
			}
			const FName RemovedName = RemovedWidget->GetFName();
			RemovedWidget->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
			UnrealMcp::RemoveWidgetBlueprintGuid(WidgetBlueprint, RemovedName);
		}

		UnrealMcp::MarkWidgetBlueprintModified(WidgetBlueprint, true);
		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_remove"));
		StructuredContent->SetStringField(TEXT("removedWidgetName"), RemovedWidgetName);
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Removed widget %s from %s."), *RemovedWidgetName, *ObjectPath),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.widget_set_property"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString WidgetBlueprintPath;
		FString WidgetName;
		FString PropertyName;
		FString Value;
		if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("propertyName"), PropertyName) || PropertyName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'propertyName'."), nullptr, true);
		}
		Arguments.TryGetStringField(TEXT("value"), Value);

		FString ObjectPath;
		FString FailureReason;
		UWidgetBlueprint* WidgetBlueprint = UnrealMcp::LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
		if (!WidgetBlueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UWidget* Widget = UnrealMcp::FindWidgetByName(WidgetBlueprint, WidgetName);
		if (!Widget)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetSetProperty", "Unreal MCP Set Widget Property"));
		WidgetBlueprint->Modify();
		TSharedPtr<FJsonObject> EditObject;
		if (!UnrealMcp::ApplyStringToProperty(Widget, PropertyName, Value, FailureReason, EditObject))
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, EditObject, true);
		}

		UnrealMcp::MarkWidgetBlueprintModified(WidgetBlueprint, false);
		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_set_property"));
		StructuredContent->SetObjectField(TEXT("widget"), UnrealMcp::DescribeWidget(Widget));
		StructuredContent->SetObjectField(TEXT("edit"), EditObject);

		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Set %s.%s on %s."), *Widget->GetName(), *PropertyName, *ObjectPath),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.widget_set_slot_layout"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString WidgetBlueprintPath;
		FString WidgetName;
		if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UWidgetBlueprint* WidgetBlueprint = UnrealMcp::LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
		if (!WidgetBlueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UWidget* Widget = UnrealMcp::FindWidgetByName(WidgetBlueprint, WidgetName);
		if (!Widget)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
		}
		if (!Widget->Slot)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Widget '%s' has no parent slot. Root widget layout cannot be edited with widget_set_slot_layout."), *Widget->GetName()), nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetSetSlotLayout", "Unreal MCP Set Widget Slot Layout"));
		WidgetBlueprint->Modify();
		Widget->Modify();
		int32 ChangedCount = 0;
		UnrealMcp::ApplyPanelSlotLayout(Widget->Slot, Arguments, ChangedCount);
		UnrealMcp::MarkWidgetBlueprintModified(WidgetBlueprint, false);

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_set_slot_layout"));
		StructuredContent->SetObjectField(TEXT("widget"), UnrealMcp::DescribeWidget(Widget));
		StructuredContent->SetNumberField(TEXT("changedFieldGroups"), ChangedCount);
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Updated slot layout for widget %s in %s. changedFieldGroups=%d"), *Widget->GetName(), *ObjectPath, ChangedCount),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.widget_bind_event"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString WidgetBlueprintPath;
		FString WidgetName;
		FString EventName = TEXT("OnClicked");
		FString FunctionName;
		double X = 0.0;
		double Y = 0.0;
		bool bCompile = true;
		if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
		}
		Arguments.TryGetStringField(TEXT("eventName"), EventName);
		Arguments.TryGetStringField(TEXT("functionName"), FunctionName);
		const bool bHasX = Arguments.TryGetNumberField(TEXT("x"), X);
		const bool bHasY = Arguments.TryGetNumberField(TEXT("y"), Y);
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);

		const FName EventFName(*EventName.TrimStartAndEnd());
		if (EventFName.IsNone())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("eventName cannot be empty."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UWidgetBlueprint* WidgetBlueprint = UnrealMcp::LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
		if (!WidgetBlueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UWidget* Widget = UnrealMcp::FindWidgetByName(WidgetBlueprint, WidgetName);
		if (!Widget)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
		}

		FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), EventFName);
		if (!DelegateProperty)
		{
			for (TFieldIterator<FMulticastDelegateProperty> It(Widget->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				if (It->GetName().Equals(EventName, ESearchCase::IgnoreCase))
				{
					DelegateProperty = *It;
					break;
				}
			}
		}
		if (!DelegateProperty)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Event delegate '%s' was not found on widget class '%s'."), *EventName, *Widget->GetClass()->GetPathName()), nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetBindEvent", "Unreal MCP Bind Widget Event"));
		WidgetBlueprint->Modify();
		Widget->Modify();
		if (!Widget->bIsVariable)
		{
			Widget->bIsVariable = true;
		}
		UnrealMcp::EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);
		UnrealMcp::ResolveBlueprintGraph(WidgetBlueprint, UEdGraphSchema_K2::GN_EventGraph.ToString(), true, FailureReason);
		UnrealMcp::MarkWidgetBlueprintModified(WidgetBlueprint, true);
		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		}

		FObjectProperty* VariableProperty = WidgetBlueprint->SkeletonGeneratedClass
			? FindFProperty<FObjectProperty>(WidgetBlueprint->SkeletonGeneratedClass, Widget->GetFName())
			: nullptr;
		if (!VariableProperty)
		{
			return UnrealMcp::MakeExecutionResult(
				FString::Printf(TEXT("Widget '%s' is not available as an object property on the Widget Blueprint skeleton class. Try compile=true or widget_bind_blueprint_variable first."), *Widget->GetName()),
				nullptr,
				true);
		}

		bool bCreated = false;
		const UK2Node_ComponentBoundEvent* ExistingNode = FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, DelegateProperty->GetFName(), VariableProperty->GetFName());
		if (!ExistingNode)
		{
			FKismetEditorUtilities::CreateNewBoundEventForClass(Widget->GetClass(), DelegateProperty->GetFName(), WidgetBlueprint, VariableProperty);
			ExistingNode = FKismetEditorUtilities::FindBoundEventForComponent(WidgetBlueprint, DelegateProperty->GetFName(), VariableProperty->GetFName());
			bCreated = ExistingNode != nullptr;
		}

		UK2Node_ComponentBoundEvent* EventNode = const_cast<UK2Node_ComponentBoundEvent*>(ExistingNode);
		if (!EventNode)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to create bound event %s for widget %s."), *DelegateProperty->GetName(), *Widget->GetName()), nullptr, true);
		}

		EventNode->Modify();
		if (!FunctionName.TrimStartAndEnd().IsEmpty())
		{
			EventNode->CustomFunctionName = FName(*FunctionName.TrimStartAndEnd());
		}
		if (bHasX || bHasY)
		{
			EventNode->NodePosX = static_cast<int32>(X);
			EventNode->NodePosY = static_cast<int32>(Y);
		}

		if (UEdGraph* Graph = EventNode->GetGraph())
		{
			Graph->NotifyGraphChanged();
		}
		UnrealMcp::MarkWidgetBlueprintModified(WidgetBlueprint, true);

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_bind_event"));
		StructuredContent->SetObjectField(TEXT("widget"), UnrealMcp::DescribeWidget(Widget));
		StructuredContent->SetObjectField(TEXT("node"), UnrealMcp::DescribeBlueprintNode(EventNode));
		StructuredContent->SetBoolField(TEXT("created"), bCreated);
		StructuredContent->SetStringField(TEXT("eventName"), DelegateProperty->GetName());
		StructuredContent->SetStringField(TEXT("functionName"), EventNode->CustomFunctionName.ToString());
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("%s bound event %s for widget %s in %s."),
				bCreated ? TEXT("Created") : TEXT("Found existing"),
				*DelegateProperty->GetName(),
				*Widget->GetName(),
				*ObjectPath),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.widget_bind_blueprint_variable"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString WidgetBlueprintPath;
		FString WidgetName;
		FString VariableName;
		bool bExpose = true;
		bool bCompile = true;
		if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
		}
		if (!Arguments.TryGetStringField(TEXT("widgetName"), WidgetName) || WidgetName.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetName'."), nullptr, true);
		}
		Arguments.TryGetStringField(TEXT("variableName"), VariableName);
		Arguments.TryGetBoolField(TEXT("expose"), bExpose);
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);

		FString ObjectPath;
		FString FailureReason;
		UWidgetBlueprint* WidgetBlueprint = UnrealMcp::LoadWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, FailureReason);
		if (!WidgetBlueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UWidget* Widget = UnrealMcp::FindWidgetByName(WidgetBlueprint, WidgetName);
		if (!Widget)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Widget '%s' was not found in %s."), *WidgetName, *ObjectPath), nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetBindBlueprintVariable", "Unreal MCP Bind Widget Blueprint Variable"));
		WidgetBlueprint->Modify();
		Widget->Modify();

		const FName OldWidgetName = Widget->GetFName();
		FString FinalVariableName = VariableName.TrimStartAndEnd();
		if (!FinalVariableName.IsEmpty() && !FinalVariableName.Equals(Widget->GetName(), ESearchCase::CaseSensitive))
		{
			if (WidgetBlueprint->WidgetTree->FindWidget(FName(*FinalVariableName)))
			{
				return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Cannot rename widget '%s' to '%s' because that widget name already exists."), *Widget->GetName(), *FinalVariableName), nullptr, true);
			}

			const bool bHadGuid = WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(OldWidgetName);
			if (!Widget->Rename(*FinalVariableName, Widget->GetOuter(), REN_DontCreateRedirectors))
			{
				return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to rename widget '%s' to '%s'."), *OldWidgetName.ToString(), *FinalVariableName), nullptr, true);
			}
			if (bHadGuid)
			{
				UnrealMcp::RenameWidgetBlueprintGuid(WidgetBlueprint, OldWidgetName, Widget);
			}
		}

		if (bExpose && !Widget->bIsVariable)
		{
			Widget->bIsVariable = true;
		}
		else if (!bExpose && Widget->bIsVariable)
		{
			Widget->bIsVariable = false;
		}
		UnrealMcp::EnsureWidgetBlueprintGuid(WidgetBlueprint, Widget);

		UnrealMcp::MarkWidgetBlueprintModified(WidgetBlueprint, true);
		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		}

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_bind_blueprint_variable"));
		StructuredContent->SetObjectField(TEXT("widget"), UnrealMcp::DescribeWidget(Widget));
		StructuredContent->SetBoolField(TEXT("expose"), bExpose);
		StructuredContent->SetBoolField(TEXT("compiled"), bCompile);
		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Widget %s is now %s as a Blueprint variable in %s."),
				*Widget->GetName(),
				bExpose ? TEXT("exposed") : TEXT("hidden"),
				*ObjectPath),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.widget_build_template"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString WidgetBlueprintPath;
		FString TemplateName = TEXT("imperial_tavern_mvp");
		FString TitleText = TEXT("Imperial Tavern");
		bool bReplaceRoot = true;
		bool bCompile = false;
		bool bSavePackage = false;
		if (!Arguments.TryGetStringField(TEXT("widgetBlueprintPath"), WidgetBlueprintPath) || WidgetBlueprintPath.TrimStartAndEnd().IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("Missing required field 'widgetBlueprintPath'."), nullptr, true);
		}
		Arguments.TryGetStringField(TEXT("templateName"), TemplateName);
		Arguments.TryGetStringField(TEXT("title"), TitleText);
		Arguments.TryGetBoolField(TEXT("replaceRoot"), bReplaceRoot);
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);
		Arguments.TryGetBoolField(TEXT("savePackage"), bSavePackage);

		if (!TemplateName.TrimStartAndEnd().Equals(TEXT("imperial_tavern_mvp"), ESearchCase::IgnoreCase))
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unsupported widget template '%s'. Currently supported: imperial_tavern_mvp."), *TemplateName), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		bool bCreated = false;
		UWidgetBlueprint* WidgetBlueprint = UnrealMcp::LoadOrCreateWidgetBlueprintAsset(EditorAssetSubsystem, WidgetBlueprintPath, ObjectPath, bCreated, FailureReason);
		if (!WidgetBlueprint)
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		if (!bReplaceRoot && WidgetBlueprint->WidgetTree && WidgetBlueprint->WidgetTree->RootWidget)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("replaceRoot=false but the Widget Blueprint already has a root widget."), nullptr, true);
		}

		const FScopedTransaction Transaction(LOCTEXT("UnrealMcpWidgetBuildTemplate", "Unreal MCP Build Widget Template"));
		if (!UnrealMcp::BuildDefaultWidgetTemplate(WidgetBlueprint, TitleText, FailureReason))
		{
			return UnrealMcp::MakeExecutionResult(FailureReason, nullptr, true);
		}

		UnrealMcp::MarkWidgetBlueprintModified(WidgetBlueprint, true);

		bool bCompileSucceeded = true;
		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
			bCompileSucceeded = WidgetBlueprint->Status != BS_Error;
		}

		bool bSaved = false;
		if (bSavePackage)
		{
			bSaved = EditorAssetSubsystem->SaveLoadedAsset(WidgetBlueprint, false);
		}

		TSharedPtr<FJsonObject> StructuredContent = UnrealMcp::DescribeWidgetBlueprint(WidgetBlueprint, TEXT("widget_build_template"));
		StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
		StructuredContent->SetStringField(TEXT("templateName"), TemplateName);
		StructuredContent->SetBoolField(TEXT("created"), bCreated);
		StructuredContent->SetBoolField(TEXT("compiled"), bCompile);
		StructuredContent->SetBoolField(TEXT("compileSucceeded"), bCompileSucceeded);
		StructuredContent->SetBoolField(TEXT("savePackage"), bSavePackage);
		StructuredContent->SetBoolField(TEXT("saved"), bSaved);

		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Built widget template %s in %s. created=%s compiled=%s saved=%s"),
				*TemplateName,
				*ObjectPath,
				bCreated ? TEXT("true") : TEXT("false"),
				bCompile ? (bCompileSucceeded ? TEXT("true") : TEXT("error")) : TEXT("false"),
				bSaved ? TEXT("true") : TEXT("false")),
			StructuredContent,
			!bCompileSucceeded || (bSavePackage && !bSaved));
	}

	if (ToolName == TEXT("unreal.scaffold_round_system"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}
		return UnrealMcp::ScaffoldRoundSystem(EditorAssetSubsystem, Arguments);
	}

	if (ToolName == TEXT("unreal.scaffold_shop_system"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}
		return UnrealMcp::ScaffoldShopSystem(EditorAssetSubsystem, Arguments);
	}

	if (ToolName == TEXT("unreal.scaffold_economy_system"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}
		return UnrealMcp::ScaffoldEconomySystem(EditorAssetSubsystem, Arguments);
	}

	if (ToolName == TEXT("unreal.scaffold_autobattler_ai"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}
		return UnrealMcp::ScaffoldAutobattlerAi(EditorAssetSubsystem, Arguments);
	}

	if (ToolName == TEXT("unreal.scaffold_result_ui"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}
		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}
		return UnrealMcp::ScaffoldResultUi(EditorAssetSubsystem, Arguments);
	}

		if (ToolName == TEXT("unreal.compile_blueprint"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
		}

		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString AssetPath;
		if (!Arguments.TryGetStringField(TEXT("path"), AssetPath) || AssetPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The path argument is required."), nullptr, true);
		}

		FString ObjectPath;
		FString FailureReason;
		UObject* LoadedAsset = UnrealMcp::LoadAssetFromAnyPath(EditorAssetSubsystem, AssetPath, ObjectPath, FailureReason);
		UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset);
		if (!Blueprint)
		{
			return UnrealMcp::MakeExecutionResult(
				LoadedAsset
					? FString::Printf(TEXT("Asset '%s' is not a Blueprint."), *ObjectPath)
					: FailureReason,
				nullptr,
				true);
		}

		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		const bool bSucceeded = Blueprint->Status != BS_Error;

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
		StructuredContent->SetStringField(TEXT("status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
			return UnrealMcp::MakeExecutionResult(
				FString::Printf(TEXT("Compiled Blueprint %s. status=%s"), *ObjectPath, bSucceeded ? TEXT("success") : TEXT("error")),
				StructuredContent,
				!bSucceeded);
		}

		if (ToolName == TEXT("unreal.compile_blueprints_in_path"))
		{
			if (UnrealMcp::IsEditorPlaying())
			{
				return UnrealMcp::MakePieBlockedResult(ToolName);
			}

			if (!EditorAssetSubsystem)
			{
				return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
			}

			FString Path = TEXT("/Game");
			bool bRecursive = true;
			Arguments.TryGetStringField(TEXT("path"), Path);
			Arguments.TryGetBoolField(TEXT("recursive"), bRecursive);
			const int32 Limit = FMath::Min(UnrealMcp::GetPositiveIntArgument(Arguments, TEXT("limit"), 100), 500);

			if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
			{
				return UnrealMcp::MakeExecutionResult(TEXT("The path argument must be a Content Browser path like /Game."), nullptr, true);
			}

			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

			FARFilter Filter;
			Filter.PackagePaths.Add(*Path);
			Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursivePaths = bRecursive;
			Filter.bRecursiveClasses = true;

			TArray<FAssetData> AssetData;
			AssetRegistryModule.Get().GetAssets(Filter, AssetData);
			AssetData.Sort([](const FAssetData& A, const FAssetData& B)
			{
				return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
			});

			int32 CompiledCount = 0;
			int32 SuccessCount = 0;
			int32 FailureCount = 0;
			bool bTruncated = false;
			TArray<TSharedPtr<FJsonValue>> ResultsArray;
			TArray<FString> FailureLines;

			for (const FAssetData& Asset : AssetData)
			{
				if (CompiledCount >= Limit)
				{
					bTruncated = true;
					break;
				}

				UObject* LoadedAsset = EditorAssetSubsystem->LoadAsset(Asset.GetSoftObjectPath().ToString());
				UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset);
				if (!Blueprint)
				{
					continue;
				}

				FKismetEditorUtilities::CompileBlueprint(Blueprint);

				++CompiledCount;
				const bool bSucceeded = Blueprint->Status != BS_Error;
				if (bSucceeded)
				{
					++SuccessCount;
				}
				else
				{
					++FailureCount;
					FailureLines.Add(Asset.GetSoftObjectPath().ToString());
				}

				TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
				ResultObject->SetStringField(TEXT("objectPath"), Asset.GetSoftObjectPath().ToString());
				ResultObject->SetStringField(TEXT("status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Blueprint->Status)));
				ResultObject->SetBoolField(TEXT("success"), bSucceeded);
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObject));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("path"), Path);
			StructuredContent->SetBoolField(TEXT("recursive"), bRecursive);
			StructuredContent->SetNumberField(TEXT("limit"), Limit);
			StructuredContent->SetNumberField(TEXT("totalBlueprintAssets"), AssetData.Num());
			StructuredContent->SetNumberField(TEXT("compiledCount"), CompiledCount);
			StructuredContent->SetNumberField(TEXT("successCount"), SuccessCount);
			StructuredContent->SetNumberField(TEXT("failureCount"), FailureCount);
			StructuredContent->SetBoolField(TEXT("truncated"), bTruncated);
			StructuredContent->SetArrayField(TEXT("results"), ResultsArray);

			FString Text = FString::Printf(
				TEXT("Compiled %d Blueprint assets under %s. success=%d failure=%d"),
				CompiledCount,
				*Path,
				SuccessCount,
				FailureCount);
			if (bTruncated)
			{
				Text += FString::Printf(TEXT(" (stopped at limit %d)"), Limit);
			}
			if (FailureLines.Num() > 0)
			{
				Text += TEXT("\nFailed:\n") + FString::Join(FailureLines, TEXT("\n"));
			}

			return UnrealMcp::MakeExecutionResult(Text, StructuredContent, FailureCount > 0);
		}

		if (ToolName == TEXT("unreal.create_blueprint_class"))
		{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}

		if (!EditorAssetSubsystem)
		{
			return UnrealMcp::MakeExecutionResult(TEXT("EditorAssetSubsystem is unavailable."), nullptr, true);
		}

		FString AssetPath;
		if (!Arguments.TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(TEXT("The assetPath argument is required."), nullptr, true);
		}

		FString ParentClassPath = TEXT("/Script/Engine.Actor");
		bool bOpenAfterCreate = true;
		bool bCompile = true;
		Arguments.TryGetStringField(TEXT("parentClass"), ParentClassPath);
		Arguments.TryGetBoolField(TEXT("openAfterCreate"), bOpenAfterCreate);
		Arguments.TryGetBoolField(TEXT("compile"), bCompile);

		UClass* ParentClass = UnrealMcp::ResolveClassPath(ParentClassPath, EditorAssetSubsystem);
		if (!ParentClass)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to resolve parent class '%s'."), *ParentClassPath), nullptr, true);
		}

		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Cannot create a Blueprint from class '%s'."), *ParentClass->GetPathName()), nullptr, true);
		}

		FString FailureReason;
		const FString ObjectPath = EditorScriptingHelpers::ConvertAnyPathToObjectPath(AssetPath, FailureReason);
		if (ObjectPath.IsEmpty())
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Unable to resolve asset path: %s"), *FailureReason), nullptr, true);
		}

		if (!EditorScriptingHelpers::IsAValidPathForCreateNewAsset(ObjectPath, FailureReason))
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Invalid asset path '%s': %s"), *ObjectPath, *FailureReason), nullptr, true);
		}

		if (EditorAssetSubsystem->DoesAssetExist(ObjectPath))
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Asset '%s' already exists."), *ObjectPath), nullptr, true);
		}

		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		const FName AssetName(*FPackageName::GetLongPackageAssetName(PackageName));
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to create package '%s'."), *PackageName), nullptr, true);
		}

		UClass* BlueprintClass = nullptr;
		UClass* BlueprintGeneratedClass = nullptr;
		IKismetCompilerInterface& KismetCompilerModule = FModuleManager::LoadModuleChecked<IKismetCompilerInterface>(KISMET_COMPILER_MODULENAME);
		KismetCompilerModule.GetBlueprintTypesForClass(ParentClass, BlueprintClass, BlueprintGeneratedClass);

		UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			AssetName,
			BPTYPE_Normal,
			BlueprintClass,
			BlueprintGeneratedClass,
			FName(TEXT("UnrealMcp")));

		if (!NewBlueprint)
		{
			return UnrealMcp::MakeExecutionResult(FString::Printf(TEXT("Failed to create Blueprint '%s'."), *ObjectPath), nullptr, true);
		}

		FAssetRegistryModule::AssetCreated(NewBlueprint);
		Package->MarkPackageDirty();

		if (bCompile)
		{
			FKismetEditorUtilities::CompileBlueprint(NewBlueprint);
		}

		if (bOpenAfterCreate && AssetEditorSubsystem)
		{
			AssetEditorSubsystem->OpenEditorForAsset(NewBlueprint);
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("objectPath"), ObjectPath);
		StructuredContent->SetStringField(TEXT("packageName"), PackageName);
		StructuredContent->SetStringField(TEXT("parentClass"), ParentClass->GetPathName());
		StructuredContent->SetBoolField(TEXT("compiled"), bCompile);
		StructuredContent->SetBoolField(TEXT("opened"), bOpenAfterCreate);
		if (NewBlueprint->GeneratedClass)
		{
			StructuredContent->SetStringField(TEXT("generatedClass"), NewBlueprint->GeneratedClass->GetPathName());
		}

		return UnrealMcp::MakeExecutionResult(
			FString::Printf(TEXT("Created Blueprint %s with parent %s."), *ObjectPath, *ParentClass->GetPathName()),
			StructuredContent,
			false);
	}

	if (ToolName == TEXT("unreal.save_dirty_packages"))
	{
		if (UnrealMcp::IsEditorPlaying())
		{
			return UnrealMcp::MakePieBlockedResult(ToolName);
		}

		bool bSaveMaps = true;
		bool bSaveAssets = true;
		Arguments.TryGetBoolField(TEXT("saveMaps"), bSaveMaps);
		Arguments.TryGetBoolField(TEXT("saveAssets"), bSaveAssets);

		const bool bSaved = UEditorLoadingAndSavingUtils::SaveDirtyPackages(bSaveMaps, bSaveAssets);

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetBoolField(TEXT("saved"), bSaved);
		StructuredContent->SetBoolField(TEXT("saveMaps"), bSaveMaps);
		StructuredContent->SetBoolField(TEXT("saveAssets"), bSaveAssets);

		const FString Text = FString::Printf(
			TEXT("SaveDirtyPackages completed. saved=%s saveMaps=%s saveAssets=%s"),
			bSaved ? TEXT("true") : TEXT("false"),
			bSaveMaps ? TEXT("true") : TEXT("false"),
			bSaveAssets ? TEXT("true") : TEXT("false"));

		return UnrealMcp::MakeExecutionResult(Text, StructuredContent, false);
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
