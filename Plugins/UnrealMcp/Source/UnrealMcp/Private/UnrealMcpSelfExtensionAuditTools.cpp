#include "UnrealMcpSelfExtensionTools.h"
#include "UnrealMcpSelfExtensionInternal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
		void AddAuditIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& Path,
			const FString& Message)
		{
			TSharedPtr<FJsonObject> IssueObject = MakeShared<FJsonObject>();
			IssueObject->SetStringField(TEXT("severity"), Severity);
			IssueObject->SetStringField(TEXT("path"), Path);
			IssueObject->SetStringField(TEXT("message"), Message);
			Issues.Add(MakeShared<FJsonValueObject>(IssueObject));
		}

		bool AnalyzeOpenAiSchemaObject(
			const TSharedPtr<FJsonObject>& SchemaObject,
			const FString& Path,
			TArray<TSharedPtr<FJsonValue>>& Issues);

		bool AnalyzeOpenAiSchemaValue(
			const TSharedPtr<FJsonValue>& SchemaValue,
			const FString& Path,
			TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			if (!SchemaValue.IsValid())
			{
				AddAuditIssue(Issues, TEXT("warning"), Path, TEXT("Schema value is null."));
				return true;
			}

			if (SchemaValue->Type == EJson::Object)
			{
				return AnalyzeOpenAiSchemaObject(SchemaValue->AsObject(), Path, Issues);
			}

			if (SchemaValue->Type == EJson::Array)
			{
				bool bCompatible = true;
				const TArray<TSharedPtr<FJsonValue>>& Items = SchemaValue->AsArray();
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					bCompatible &= AnalyzeOpenAiSchemaValue(
						Items[Index],
						FString::Printf(TEXT("%s[%d]"), *Path, Index),
						Issues);
				}
				return bCompatible;
			}

			return true;
		}

		bool AnalyzeOpenAiSchemaObject(
			const TSharedPtr<FJsonObject>& SchemaObject,
			const FString& Path,
			TArray<TSharedPtr<FJsonValue>>& Issues)
		{
			if (!SchemaObject.IsValid())
			{
				AddAuditIssue(Issues, TEXT("error"), Path, TEXT("Schema object is invalid."));
				return false;
			}

			bool bCompatible = true;
			FString TypeString;
			const bool bHasStringType = SchemaObject->TryGetStringField(TEXT("type"), TypeString);
			const TSharedPtr<FJsonValue> TypeField = SchemaObject->TryGetField(TEXT("type"));
			if (TypeField.IsValid() && !bHasStringType)
			{
				AddAuditIssue(Issues, TEXT("warning"), Path + TEXT(".type"), TEXT("Non-string JSON schema type values may not be accepted by OpenAI function calling."));
			}

			if (bHasStringType && TypeString == TEXT("object"))
			{
				const TSharedPtr<FJsonValue> AdditionalProperties = SchemaObject->TryGetField(TEXT("additionalProperties"));
				if (!AdditionalProperties.IsValid())
				{
					AddAuditIssue(Issues, TEXT("warning"), Path, TEXT("Object schema does not explicitly set additionalProperties=false."));
				}
				else if (AdditionalProperties->Type == EJson::Boolean)
				{
					if (AdditionalProperties->AsBool())
					{
						AddAuditIssue(Issues, TEXT("error"), Path + TEXT(".additionalProperties"), TEXT("additionalProperties=true is not accepted by the AI function interface."));
						bCompatible = false;
					}
				}
				else
				{
					AddAuditIssue(Issues, TEXT("warning"), Path + TEXT(".additionalProperties"), TEXT("additionalProperties should be boolean false for AI-facing tools."));
				}

				const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
				if (SchemaObject->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && (*PropertiesObject).IsValid())
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropertiesObject)->Values)
					{
						bCompatible &= AnalyzeOpenAiSchemaValue(Pair.Value, Path + TEXT(".properties.") + Pair.Key, Issues);
					}
				}
			}
			else if (bHasStringType && TypeString == TEXT("array"))
			{
				const TSharedPtr<FJsonObject>* ItemsObject = nullptr;
				if (SchemaObject->TryGetObjectField(TEXT("items"), ItemsObject) && ItemsObject && (*ItemsObject).IsValid())
				{
					bCompatible &= AnalyzeOpenAiSchemaObject(*ItemsObject, Path + TEXT(".items"), Issues);
				}
				else
				{
					AddAuditIssue(Issues, TEXT("warning"), Path + TEXT(".items"), TEXT("Array schema should define an object-valued items schema."));
				}
			}

			return bCompatible;
		}

		bool AnalyzeOpenAiSchemaCompatibility(
			const TSharedPtr<FJsonObject>& InputSchema,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			FString& OutReason,
			TSharedPtr<FJsonObject>& OutNormalizedSchema)
		{
			OutReason.Reset();
			OutNormalizedSchema = NormalizeOpenAiSchemaObject(InputSchema);

			bool bCompatible = AnalyzeOpenAiSchemaObject(OutNormalizedSchema, TEXT("inputSchema"), Issues);
			FString ExistingCompatibilityReason;
			if (!IsOpenAiSchemaCompatibleObject(OutNormalizedSchema, ExistingCompatibilityReason))
			{
				AddAuditIssue(Issues, TEXT("error"), TEXT("inputSchema"), ExistingCompatibilityReason);
				bCompatible = false;
			}

			if (!bCompatible)
			{
				OutReason = ExistingCompatibilityReason.IsEmpty() ? TEXT("Schema contains AI-incompatible fields.") : ExistingCompatibilityReason;
			}
			return bCompatible;
		}

		TSharedPtr<FJsonObject> FindToolDefinitionByName(
			const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
			const FString& ToolName)
		{
			for (const TSharedPtr<FJsonValue>& ToolValue : ToolsArray)
			{
				if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
				{
					continue;
				}

				TSharedPtr<FJsonObject> ToolObject = ToolValue->AsObject();
				FString CandidateName;
				if (ToolObject->TryGetStringField(TEXT("name"), CandidateName) && CandidateName == ToolName)
				{
					return ToolObject;
				}
			}

			return nullptr;
		}

		void AddUniqueAuditCandidate(TArray<FString>& Candidates, const FString& Candidate)
		{
			if (Candidate.IsEmpty())
			{
				return;
			}

			FString NormalizedCandidate = FPaths::ConvertRelativePathToFull(Candidate);
			FPaths::NormalizeFilename(NormalizedCandidate);
			FPaths::CollapseRelativeDirectories(NormalizedCandidate);
			for (const FString& Existing : Candidates)
			{
				if (Existing.Equals(NormalizedCandidate, ESearchCase::IgnoreCase))
				{
					return;
				}
			}
			Candidates.Add(NormalizedCandidate);
		}

		FToolsReadResolution ResolveDocsPathForAudit(const FString& DocsPath)
		{
			FToolsReadResolution Resolution;
			FString DocsFilePath = DocsPath;
			FString DocsAnchor;
			if (DocsPath.Split(TEXT("#"), &DocsFilePath, &DocsAnchor))
			{
				// Keep only the versioned file path portion for existence checks.
			}
			DocsFilePath = DocsFilePath.TrimStartAndEnd();
			if (DocsFilePath.IsEmpty())
			{
				return Resolution;
			}

			if (FPaths::IsRelative(DocsFilePath))
			{
				AddUniqueAuditCandidate(Resolution.Candidates, FPaths::Combine(FPaths::ProjectDir(), DocsFilePath));

				const FToolsReadResolution ToolsRoot = ResolveToolsReadSubpath(FString(), TArray<FString>());
				if (!ToolsRoot.Warning.IsEmpty())
				{
					Resolution.Warning = ToolsRoot.Warning;
				}
				for (const FString& ToolsCandidate : ToolsRoot.Candidates)
				{
					AddUniqueAuditCandidate(Resolution.Candidates, FPaths::Combine(FPaths::GetPath(ToolsCandidate), DocsFilePath));
				}

				const FToolsReadResolution PluginBaseDir = ResolvePluginBaseDir();
				if (!PluginBaseDir.Path.IsEmpty())
				{
					AddUniqueAuditCandidate(Resolution.Candidates, FPaths::Combine(PluginBaseDir.Path, DocsFilePath));
					if (DocsFilePath.Equals(TEXT("README.md"), ESearchCase::IgnoreCase)
						|| DocsFilePath.EndsWith(TEXT("/README.md"), ESearchCase::IgnoreCase)
						|| DocsFilePath.EndsWith(TEXT("\\README.md"), ESearchCase::IgnoreCase))
					{
						AddUniqueAuditCandidate(Resolution.Candidates, FPaths::Combine(PluginBaseDir.Path, TEXT("README.md")));
					}
				}
			}
			else
			{
				AddUniqueAuditCandidate(Resolution.Candidates, DocsFilePath);
			}

			const FToolsReadResolution PluginBaseDir = ResolvePluginBaseDir();
			for (int32 CandidateIndex = 0; CandidateIndex < Resolution.Candidates.Num(); ++CandidateIndex)
			{
				if (!FPaths::FileExists(Resolution.Candidates[CandidateIndex]))
				{
					continue;
				}

				Resolution.Path = Resolution.Candidates[CandidateIndex];
				Resolution.bFound = true;
				if (!PluginBaseDir.Path.IsEmpty() && Resolution.Path.StartsWith(PluginBaseDir.Path, ESearchCase::IgnoreCase))
				{
					Resolution.SourceKind = FToolsReadResolution::ESource::PluginResources;
				}
				else
				{
					Resolution.SourceKind = CandidateIndex == 0
						? FToolsReadResolution::ESource::ProjectLocal
						: FToolsReadResolution::ESource::SharedRepoRoot;
				}
				return Resolution;
			}

			if (Resolution.Candidates.Num() > 0)
			{
				Resolution.Path = Resolution.Candidates[0];
			}
			return Resolution;
		}

		FUnrealMcpExecutionResult ValidateMcpToolSchema(
			const FJsonObject& Arguments,
			const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
		{
			FString ToolName;
			FString SchemaJson;
			bool bReturnNormalizedSchema = true;
			Arguments.TryGetStringField(TEXT("toolName"), ToolName);
			Arguments.TryGetStringField(TEXT("schemaJson"), SchemaJson);
			Arguments.TryGetBoolField(TEXT("returnNormalizedSchema"), bReturnNormalizedSchema);
			ToolName = ToolName.TrimStartAndEnd();
			SchemaJson = SchemaJson.TrimStartAndEnd();

			TSharedPtr<FJsonObject> InputSchema;
			FString Source = TEXT("schemaJson");
			if (!SchemaJson.IsEmpty())
			{
				if (!LoadJsonObject(SchemaJson, InputSchema) || !InputSchema.IsValid())
				{
					return MakeExecutionResult(TEXT("schemaJson must be a valid JSON object."), nullptr, true);
				}
			}
			else
			{
				if (ToolName.IsEmpty())
				{
					return MakeExecutionResult(TEXT("Provide either schemaJson or toolName."), nullptr, true);
				}

				const TSharedPtr<FJsonObject> ToolObject = FindToolDefinitionByName(ToolsArray, ToolName);
				if (!ToolObject.IsValid())
				{
					return MakeExecutionResult(FString::Printf(TEXT("Tool '%s' was not found in current tool definitions."), *ToolName), nullptr, true);
				}

				const TSharedPtr<FJsonObject>* SchemaObject = nullptr;
				if (!ToolObject->TryGetObjectField(TEXT("inputSchema"), SchemaObject) || !SchemaObject || !(*SchemaObject).IsValid())
				{
					return MakeExecutionResult(FString::Printf(TEXT("Tool '%s' does not expose an inputSchema object."), *ToolName), nullptr, true);
				}

				InputSchema = *SchemaObject;
				Source = TEXT("toolName");
			}

			TArray<TSharedPtr<FJsonValue>> Issues;
			FString Reason;
			TSharedPtr<FJsonObject> NormalizedSchema;
			const bool bCompatible = AnalyzeOpenAiSchemaCompatibility(InputSchema, Issues, Reason, NormalizedSchema);

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_validate_tool_schema"));
			StructuredContent->SetStringField(TEXT("source"), Source);
			StructuredContent->SetStringField(TEXT("toolName"), ToolName);
			StructuredContent->SetBoolField(TEXT("compatible"), bCompatible);
			StructuredContent->SetStringField(TEXT("reason"), Reason);
			StructuredContent->SetArrayField(TEXT("issues"), Issues);
			if (bReturnNormalizedSchema && NormalizedSchema.IsValid())
			{
				StructuredContent->SetObjectField(TEXT("normalizedSchema"), NormalizedSchema);
			}

			const FString Text = bCompatible
				? FString::Printf(TEXT("Schema is AI-compatible. warnings=%d"), Issues.Num())
				: FString::Printf(TEXT("Schema is not AI-compatible: %s"), Reason.IsEmpty() ? TEXT("see issues") : *Reason);
			return MakeExecutionResult(Text, StructuredContent, !bCompatible);
		}

		FUnrealMcpExecutionResult AuditMcpTools(const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
		{
			TArray<TSharedPtr<FJsonValue>> ToolReports;
			TArray<FString> MissingHandlers;
			TArray<FString> MissingDocs;
			TArray<FString> MissingRegistryEntries;
			TArray<FString> IncompatibleSchemas;
			int32 CompatibleCount = 0;
			int32 WarningCount = 0;
			int32 ReadOnlyRiskCount = 0;
			int32 LowRiskCount = 0;
			int32 MediumRiskCount = 0;
			int32 HighRiskCount = 0;
			int32 CriticalRiskCount = 0;
			int32 RequiresWriteCount = 0;
			int32 RequiresBuildCount = 0;
			int32 RequiresExternalProcessCount = 0;
			int32 RequiresRestartCount = 0;
			int32 RequiresProjectMemoryCount = 0;
			int32 RequiresLockCount = 0;

			for (const TSharedPtr<FJsonValue>& ToolValue : ToolsArray)
			{
				if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
				{
					continue;
				}

				TSharedPtr<FJsonObject> ToolObject = ToolValue->AsObject();
				FString Name;
				FString Title;
				FString Description;
				ToolObject->TryGetStringField(TEXT("name"), Name);
				ToolObject->TryGetStringField(TEXT("title"), Title);
				ToolObject->TryGetStringField(TEXT("description"), Description);

				const FString HandlerName = ResolveToolHandlerName(Name);
				const FToolHandlerRegistryEntry* HandlerEntry = FindToolHandlerRegistryEntry(HandlerName);
				const bool bHasHandler = HandlerEntry != nullptr;
				const FToolRegistryEntry* RegistryEntry = FindToolRegistryEntry(Name);
				const bool bHasExplicitRegistryEntry = RegistryEntry && RegistryEntry->bLoadedFromExplicitRegistry;
				const FString DocsPath = RegistryEntry ? RegistryEntry->Policy.DocsPath : FString();
				const FToolsReadResolution DocsResolution = ResolveDocsPathForAudit(DocsPath);
				const bool bDocumented = DocsResolution.bFound;

				const TSharedPtr<FJsonObject>* SchemaObject = nullptr;
				TArray<TSharedPtr<FJsonValue>> Issues;
				FString Reason;
				TSharedPtr<FJsonObject> NormalizedSchema;
				bool bCompatible = true;
				if (ToolObject->TryGetObjectField(TEXT("inputSchema"), SchemaObject) && SchemaObject && (*SchemaObject).IsValid())
				{
					bCompatible = AnalyzeOpenAiSchemaCompatibility(*SchemaObject, Issues, Reason, NormalizedSchema);
				}
				else
				{
					AddAuditIssue(Issues, TEXT("error"), TEXT("inputSchema"), TEXT("Tool definition does not include an inputSchema object."));
					Reason = TEXT("missing inputSchema");
					bCompatible = false;
				}

				if (bCompatible)
				{
					++CompatibleCount;
				}
				else
				{
					IncompatibleSchemas.Add(Name);
				}
				if (!bHasHandler)
				{
					MissingHandlers.Add(Name);
				}
				if (!bDocumented)
				{
					MissingDocs.Add(Name);
				}
				if (!bHasExplicitRegistryEntry)
				{
					MissingRegistryEntries.Add(Name);
				}
				if (Issues.Num() > 0)
				{
					++WarningCount;
				}

				const FToolPolicy Policy = GetToolPolicy(Name);
				switch (Policy.RiskLevel)
				{
				case EToolRiskLevel::ReadOnly:
					++ReadOnlyRiskCount;
					break;
				case EToolRiskLevel::Low:
					++LowRiskCount;
					break;
				case EToolRiskLevel::Medium:
					++MediumRiskCount;
					break;
				case EToolRiskLevel::High:
					++HighRiskCount;
					break;
				case EToolRiskLevel::Critical:
					++CriticalRiskCount;
					break;
				default:
					break;
				}
				RequiresWriteCount += Policy.bRequiresWrite ? 1 : 0;
				RequiresBuildCount += Policy.bRequiresBuild ? 1 : 0;
				RequiresExternalProcessCount += Policy.bRequiresExternalProcess ? 1 : 0;
				RequiresRestartCount += Policy.bRequiresRestart ? 1 : 0;
				RequiresProjectMemoryCount += Policy.bRequiresProjectMemory ? 1 : 0;
				RequiresLockCount += Policy.bRequiresLock ? 1 : 0;

				TSharedPtr<FJsonObject> ReportObject = MakeShared<FJsonObject>();
				ReportObject->SetStringField(TEXT("name"), Name);
				ReportObject->SetStringField(TEXT("handlerName"), HandlerName);
				ReportObject->SetStringField(TEXT("title"), Title);
				ReportObject->SetStringField(TEXT("description"), Description);
				ReportObject->SetBoolField(TEXT("hasHandler"), bHasHandler);
				ReportObject->SetStringField(TEXT("handlerCheckSource"), TEXT("explicit_handler_registry"));
				ReportObject->SetStringField(TEXT("handlerCategory"), HandlerEntry ? HandlerEntry->Category : TEXT(""));
				ReportObject->SetStringField(TEXT("handlerSourceFile"), HandlerEntry ? HandlerEntry->SourceFile : TEXT(""));
				ReportObject->SetBoolField(TEXT("hasExplicitRegistryEntry"), bHasExplicitRegistryEntry);
				ReportObject->SetStringField(TEXT("registryCategory"), RegistryEntry ? RegistryEntry->Category : TEXT(""));
				ReportObject->SetStringField(TEXT("docsPath"), DocsPath);
				ReportObject->SetBoolField(TEXT("docsPathFileExists"), bDocumented);
				ReportObject->SetStringField(TEXT("docsPathResolvedPath"), DocsResolution.Path);
				ReportObject->SetStringField(TEXT("docsPathSourceKind"), LexToString(DocsResolution.SourceKind));
				ReportObject->SetArrayField(TEXT("docsPathCandidates"), MakeToolsReadCandidateValues(DocsResolution));
				if (!DocsResolution.Warning.IsEmpty())
				{
					ReportObject->SetStringField(TEXT("docsPathResolutionWarning"), DocsResolution.Warning);
				}
				ReportObject->SetStringField(TEXT("documentationCheckSource"), TEXT("explicit_registry_docsPath"));
				ReportObject->SetBoolField(TEXT("schemaCompatible"), bCompatible);
				ReportObject->SetStringField(TEXT("schemaReason"), Reason);
				ReportObject->SetArrayField(TEXT("schemaIssues"), Issues);
				ReportObject->SetObjectField(TEXT("policy"), MakeToolPolicyObject(Name));
				ToolReports.Add(MakeShared<FJsonValueObject>(ReportObject));
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_tool_audit"));
			StructuredContent->SetNumberField(TEXT("toolCount"), ToolsArray.Num());
			StructuredContent->SetNumberField(TEXT("schemaCompatibleCount"), CompatibleCount);
			StructuredContent->SetNumberField(TEXT("schemaIncompatibleCount"), IncompatibleSchemas.Num());
			StructuredContent->SetNumberField(TEXT("toolsWithSchemaIssues"), WarningCount);
			StructuredContent->SetNumberField(TEXT("missingHandlerCount"), MissingHandlers.Num());
			StructuredContent->SetNumberField(TEXT("missingDocumentationCount"), MissingDocs.Num());
			StructuredContent->SetNumberField(TEXT("missingRegistryEntryCount"), MissingRegistryEntries.Num());
			StructuredContent->SetStringField(TEXT("toolRegistrySourcePath"), GetToolRegistrySourcePath());
			TSharedPtr<FJsonObject> RiskCountsObject = MakeShared<FJsonObject>();
			RiskCountsObject->SetNumberField(TEXT("readOnly"), ReadOnlyRiskCount);
			RiskCountsObject->SetNumberField(TEXT("low"), LowRiskCount);
			RiskCountsObject->SetNumberField(TEXT("medium"), MediumRiskCount);
			RiskCountsObject->SetNumberField(TEXT("high"), HighRiskCount);
			RiskCountsObject->SetNumberField(TEXT("critical"), CriticalRiskCount);
			StructuredContent->SetObjectField(TEXT("riskCounts"), RiskCountsObject);
			StructuredContent->SetNumberField(TEXT("requiresWriteCount"), RequiresWriteCount);
			StructuredContent->SetNumberField(TEXT("requiresBuildCount"), RequiresBuildCount);
			StructuredContent->SetNumberField(TEXT("requiresExternalProcessCount"), RequiresExternalProcessCount);
			StructuredContent->SetNumberField(TEXT("requiresRestartCount"), RequiresRestartCount);
			StructuredContent->SetNumberField(TEXT("requiresProjectMemoryCount"), RequiresProjectMemoryCount);
			StructuredContent->SetNumberField(TEXT("requiresLockCount"), RequiresLockCount);
			StructuredContent->SetObjectField(TEXT("toolRegistryValidation"), MakeToolRegistryValidationObject(&ToolsArray));
			StructuredContent->SetArrayField(TEXT("tools"), ToolReports);

			auto AddStringArray = [](TSharedPtr<FJsonObject> Object, const FString& FieldName, const TArray<FString>& Values)
			{
				TArray<TSharedPtr<FJsonValue>> JsonValues;
				for (const FString& Value : Values)
				{
					JsonValues.Add(MakeShared<FJsonValueString>(Value));
				}
				Object->SetArrayField(FieldName, JsonValues);
			};

			AddStringArray(StructuredContent, TEXT("schemaIncompatibleTools"), IncompatibleSchemas);
			AddStringArray(StructuredContent, TEXT("missingHandlerTools"), MissingHandlers);
			AddStringArray(StructuredContent, TEXT("missingDocumentationTools"), MissingDocs);
			AddStringArray(StructuredContent, TEXT("missingRegistryEntryTools"), MissingRegistryEntries);

			const FString Text = FString::Printf(
				TEXT("Audited %d MCP tools. schemaCompatible=%d incompatible=%d missingHandlers=%d missingDocs=%d missingRegistry=%d"),
				ToolsArray.Num(),
				CompatibleCount,
				IncompatibleSchemas.Num(),
				MissingHandlers.Num(),
				MissingDocs.Num(),
				MissingRegistryEntries.Num());
			return MakeExecutionResult(Text, StructuredContent, false);
		}



}
