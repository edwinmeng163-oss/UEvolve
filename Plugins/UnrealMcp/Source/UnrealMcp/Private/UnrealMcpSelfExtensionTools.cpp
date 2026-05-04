#include "UnrealMcpSelfExtensionTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistry.h"

namespace UnrealMcp
{
	int32 GetPositiveIntArgument(const FJsonObject& Arguments, const FString& FieldName, int32 DefaultValue);
	TSharedPtr<FJsonObject> MakeEmptyObject();
	FUnrealMcpExecutionResult MakeExecutionResult(const FString& Text, const TSharedPtr<FJsonObject>& StructuredContent, bool bIsError);
	FUnrealMcpExecutionResult AuditMcpTools(const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	bool ResolveProjectPathInsideProject(const FString& RequestedPath, FString& OutPath, FString& OutFailureReason);
	bool ResolveProjectOutputDirectory(const FString& RequestedOutputRoot, FString& OutDirectory, FString& OutFailureReason);
	FString SanitizeMcpToolIdForPath(const FString& ToolName);
	FString GetUnrealMcpSavedRoot();
	FString GetProjectMemoryFilePath();
	FString GetMcpExtensionBackupRoot();
	FString GetMcpBuildLogRoot();
	FString GetLatestMcpExtensionManifestPath();
	FString GetMcpProjectStateBackupRoot();
	FString HashTextForManifest(const FString& Text);
	FString MakePathRelativeToProject(const FString& Path);
	FString FileTimeToIsoString(const FDateTime& Time);
	bool IsPathInsideDirectory(const FString& Path, const FString& Directory);
	bool LoadJsonObject(const FString& JsonText, TSharedPtr<FJsonObject>& OutObject);
	bool SaveJsonObjectToFile(const TSharedPtr<FJsonObject>& Object, const FString& FilePath, FString& OutFailureReason);
	TSharedPtr<FJsonObject> NormalizeOpenAiSchemaObject(const TSharedPtr<FJsonObject>& InputObject);
	bool IsOpenAiSchemaCompatibleObject(const TSharedPtr<FJsonObject>& InputObject, FString& OutReason);
	bool LoadProjectMemory(TSharedPtr<FJsonObject>& OutMemory, FString& OutFailureReason);
	bool LoadJsonObjectFromFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObject, FString& OutFailureReason);
	void FindImmediateChildren(const FString& Directory, const FString& Pattern, bool bFiles, bool bDirectories, TArray<FString>& OutChildren);
	bool FindNewestFile(const FString& Directory, const FString& Pattern, FString& OutPath);
	TSharedPtr<FJsonObject> MakeFileInfoObject(const FString& Path);
	TSharedPtr<FJsonObject> MakeMemoryEntrySummary(const TSharedPtr<FJsonObject>& EntryObject, bool bIncludeContent);
	TSharedPtr<FJsonObject> FindMemoryEntryByKey(const TSharedPtr<FJsonObject>& MemoryObject, const FString& Key);
	FString TailLines(const FString& Text, int32 MaxLines);
	FString RecommendPipelineNextStep(const TSharedPtr<FJsonObject>& MemoryEntry);
	void WriteBuildTestMemory(
		const FString& MemoryKey,
		const FString& Summary,
		const FString& Status,
		const FString& NextStep,
		const TSharedPtr<FJsonObject>& Content);
	TSharedPtr<FJsonObject> MakePipelineStepObject(
		const FString& StepName,
		const FString& Status,
		const FString& Message,
		const FUnrealMcpExecutionResult* Result = nullptr);
	void AddAuditIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Location,
		const FString& Message);
	bool ResolveMcpScaffoldDirectory(const FJsonObject& Arguments, FString& OutDirectory, FString& OutToolName, FString& OutFailureReason);
	TSharedPtr<FJsonObject> FindToolDefinitionByName(const TArray<TSharedPtr<FJsonValue>>& ToolsArray, const FString& ToolName);
	bool ExtractRequestedSchemaFromScaffoldReadme(const FString& ScaffoldDirectory, FString& OutSchemaJson);
	FUnrealMcpExecutionResult ValidateMcpToolSchema(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult GenerateMcpTests(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult ApplyMcpScaffold(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult RollbackLastMcpExtension(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult LockExtensionSession(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult BackupProjectState(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult RollbackToManifest(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult BuildEditor(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult SupervisorInstall(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult ListMcpScaffolds(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult InspectMcpScaffold(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray);
	FUnrealMcpExecutionResult ValidateCppSnippet(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult PatchScaffoldSnippet(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult CompileErrorFixPlan(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult DiffLastMcpApply(const FJsonObject& Arguments);
	FUnrealMcpExecutionResult CleanMcpTestArtifacts(const FJsonObject& Arguments);
	bool IsEditorPlaying();
	FUnrealMcpExecutionResult MakePieBlockedResult(const FString& ToolName);
	FString GetMcpExtensionLockPath();
	bool TryAcquireExtensionSessionLock(
		const FString& Owner,
		const FString& Reason,
		int32 TtlSeconds,
		bool bForce,
		FString& OutSessionId,
		TSharedPtr<FJsonObject>& OutLockObject,
		FString& OutFailureReason);
	bool ReleaseExtensionSessionLock(const FString& SessionId, bool bForce, FString& OutFailureReason);
	bool ResolveMcpTestsDirectory(
		const FJsonObject& Arguments,
		FString& OutTestsDirectory,
		FString& OutScaffoldDirectory,
		FString& OutToolName,
		FString& OutFailureReason);





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
				FString DocsFilePath = DocsPath;
				FString DocsAnchor;
				if (DocsPath.Split(TEXT("#"), &DocsFilePath, &DocsAnchor))
				{
					// Keep only the versioned file path portion for existence checks.
				}
				const bool bDocumented = !DocsFilePath.TrimStartAndEnd().IsEmpty()
					&& FPaths::FileExists(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), DocsFilePath)));

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


		bool ShouldIncludeCleanupCandidate(const FString& Path, double MaxAgeDays, const FString& NameContains, FString& OutSkipReason)
		{
			OutSkipReason.Reset();
			if (!NameContains.TrimStartAndEnd().IsEmpty()
				&& !Path.Contains(NameContains.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				OutSkipReason = TEXT("nameContains filter did not match.");
				return false;
			}

			if (MaxAgeDays > 0.0)
			{
				const FFileStatData Stat = IFileManager::Get().GetStatData(*Path);
				if (!Stat.bIsValid || Stat.ModificationTime.GetTicks() <= 0)
				{
					OutSkipReason = TEXT("age filter requested but modification time is unavailable.");
					return false;
				}

				const double AgeDays = (FDateTime::Now() - Stat.ModificationTime).GetTotalDays();
				if (AgeDays < MaxAgeDays)
				{
					OutSkipReason = FString::Printf(TEXT("age %.2f days is newer than maxAgeDays %.2f."), AgeDays, MaxAgeDays);
					return false;
				}
			}
			return true;
		}

		void AddCleanupCandidate(
			const FString& Category,
			const FString& Path,
			const FString& Type,
			double MaxAgeDays,
			const FString& NameContains,
			TArray<TSharedPtr<FJsonValue>>& Candidates,
			TArray<TSharedPtr<FJsonValue>>& Skipped)
		{
			TSharedPtr<FJsonObject> CandidateObject = MakeFileInfoObject(Path);
			CandidateObject->SetStringField(TEXT("category"), Category);
			CandidateObject->SetStringField(TEXT("type"), Type);

			if (!IsPathInsideDirectory(Path, GetUnrealMcpSavedRoot()))
			{
				CandidateObject->SetStringField(TEXT("skipReason"), TEXT("path is outside Saved/UnrealMcp safety root."));
				Skipped.Add(MakeShared<FJsonValueObject>(CandidateObject));
				return;
			}

			FString SkipReason;
			if (!ShouldIncludeCleanupCandidate(Path, MaxAgeDays, NameContains, SkipReason))
			{
				CandidateObject->SetStringField(TEXT("skipReason"), SkipReason);
				Skipped.Add(MakeShared<FJsonValueObject>(CandidateObject));
				return;
			}

			Candidates.Add(MakeShared<FJsonValueObject>(CandidateObject));
		}

		void AddCleanupDirectoryChildren(
			const FString& Category,
			const FString& Directory,
			double MaxAgeDays,
			const FString& NameContains,
			TArray<TSharedPtr<FJsonValue>>& Candidates,
			TArray<TSharedPtr<FJsonValue>>& Skipped)
		{
			TArray<FString> Children;
			FindImmediateChildren(Directory, TEXT("*"), false, true, Children);
			for (const FString& Child : Children)
			{
				AddCleanupCandidate(Category, Child, TEXT("directory"), MaxAgeDays, NameContains, Candidates, Skipped);
			}
		}

		void AddCleanupFiles(
			const FString& Category,
			const FString& Directory,
			const FString& Pattern,
			double MaxAgeDays,
			const FString& NameContains,
			TArray<TSharedPtr<FJsonValue>>& Candidates,
			TArray<TSharedPtr<FJsonValue>>& Skipped)
		{
			TArray<FString> Files;
			FindImmediateChildren(Directory, Pattern, true, false, Files);
			for (const FString& File : Files)
			{
				AddCleanupCandidate(Category, File, TEXT("file"), MaxAgeDays, NameContains, Candidates, Skipped);
			}
		}

		FUnrealMcpExecutionResult CleanMcpTestArtifacts(const FJsonObject& Arguments)
		{
			bool bDryRun = true;
			bool bCleanTestScaffolds = true;
			bool bCleanTestRequests = false;
			bool bCleanBuildLogs = false;
			bool bCleanExtensionBackups = false;
			bool bCleanProjectMemory = false;
			double MaxAgeDays = 0.0;
			FString NameContains;
			Arguments.TryGetBoolField(TEXT("dryRun"), bDryRun);
			Arguments.TryGetBoolField(TEXT("cleanTestScaffolds"), bCleanTestScaffolds);
			Arguments.TryGetBoolField(TEXT("cleanTestRequests"), bCleanTestRequests);
			Arguments.TryGetBoolField(TEXT("cleanBuildLogs"), bCleanBuildLogs);
			Arguments.TryGetBoolField(TEXT("cleanExtensionBackups"), bCleanExtensionBackups);
			Arguments.TryGetBoolField(TEXT("cleanProjectMemory"), bCleanProjectMemory);
			Arguments.TryGetNumberField(TEXT("maxAgeDays"), MaxAgeDays);
			Arguments.TryGetStringField(TEXT("nameContains"), NameContains);
			MaxAgeDays = FMath::Max(0.0, MaxAgeDays);

			if (!bDryRun && IsEditorPlaying())
			{
				return MakePieBlockedResult(TEXT("unreal.mcp_clean_test_artifacts"));
			}

			TArray<TSharedPtr<FJsonValue>> Candidates;
			TArray<TSharedPtr<FJsonValue>> Skipped;
			if (bCleanTestScaffolds)
			{
				AddCleanupDirectoryChildren(TEXT("testScaffolds"), FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestScaffolds")), MaxAgeDays, NameContains, Candidates, Skipped);
			}
			if (bCleanTestRequests)
			{
				AddCleanupDirectoryChildren(TEXT("testRequests"), FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestRequests")), MaxAgeDays, NameContains, Candidates, Skipped);
			}
			if (bCleanBuildLogs)
			{
				AddCleanupFiles(TEXT("buildLogs"), GetMcpBuildLogRoot(), TEXT("*.log"), MaxAgeDays, NameContains, Candidates, Skipped);
			}
			if (bCleanExtensionBackups)
			{
				AddCleanupDirectoryChildren(TEXT("extensionBackups"), GetMcpExtensionBackupRoot(), MaxAgeDays, NameContains, Candidates, Skipped);
			}
			if (bCleanProjectMemory)
			{
				AddCleanupCandidate(TEXT("projectMemory"), GetProjectMemoryFilePath(), TEXT("file"), MaxAgeDays, NameContains, Candidates, Skipped);
			}

			TArray<TSharedPtr<FJsonValue>> Deleted;
			TArray<TSharedPtr<FJsonValue>> DeleteErrors;
			if (!bDryRun)
			{
				for (const TSharedPtr<FJsonValue>& CandidateValue : Candidates)
				{
					if (!CandidateValue.IsValid() || CandidateValue->Type != EJson::Object || !CandidateValue->AsObject().IsValid())
					{
						continue;
					}

					TSharedPtr<FJsonObject> CandidateObject = CandidateValue->AsObject();
					FString Path;
					FString Type;
					CandidateObject->TryGetStringField(TEXT("path"), Path);
					CandidateObject->TryGetStringField(TEXT("type"), Type);

					bool bDeleted = false;
					if (Type == TEXT("directory"))
					{
						bDeleted = IFileManager::Get().DeleteDirectory(*Path, false, true);
					}
					else
					{
						bDeleted = IFileManager::Get().Delete(*Path, false, true, true);
					}

					TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
					ResultObject->SetStringField(TEXT("path"), Path);
					ResultObject->SetStringField(TEXT("type"), Type);
					ResultObject->SetBoolField(TEXT("deleted"), bDeleted);
					if (bDeleted)
					{
						Deleted.Add(MakeShared<FJsonValueObject>(ResultObject));
					}
					else
					{
						ResultObject->SetStringField(TEXT("error"), TEXT("delete operation returned false."));
						DeleteErrors.Add(MakeShared<FJsonValueObject>(ResultObject));
					}
				}
			}

			TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
			StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_clean_test_artifacts"));
			StructuredContent->SetStringField(TEXT("savedRoot"), GetUnrealMcpSavedRoot());
			StructuredContent->SetBoolField(TEXT("dryRun"), bDryRun);
			StructuredContent->SetBoolField(TEXT("cleanTestScaffolds"), bCleanTestScaffolds);
			StructuredContent->SetBoolField(TEXT("cleanTestRequests"), bCleanTestRequests);
			StructuredContent->SetBoolField(TEXT("cleanBuildLogs"), bCleanBuildLogs);
			StructuredContent->SetBoolField(TEXT("cleanExtensionBackups"), bCleanExtensionBackups);
			StructuredContent->SetBoolField(TEXT("cleanProjectMemory"), bCleanProjectMemory);
			StructuredContent->SetNumberField(TEXT("maxAgeDays"), MaxAgeDays);
			StructuredContent->SetStringField(TEXT("nameContains"), NameContains);
			StructuredContent->SetNumberField(TEXT("candidateCount"), Candidates.Num());
			StructuredContent->SetNumberField(TEXT("skippedCount"), Skipped.Num());
			StructuredContent->SetNumberField(TEXT("deletedCount"), Deleted.Num());
			StructuredContent->SetNumberField(TEXT("deleteErrorCount"), DeleteErrors.Num());
			StructuredContent->SetArrayField(TEXT("candidates"), Candidates);
			StructuredContent->SetArrayField(TEXT("skipped"), Skipped);
			StructuredContent->SetArrayField(TEXT("deleted"), Deleted);
			StructuredContent->SetArrayField(TEXT("deleteErrors"), DeleteErrors);

			const FString Text = FString::Printf(
				TEXT("MCP cleanup %s: candidates=%d deleted=%d skipped=%d errors=%d."),
				bDryRun ? TEXT("dry run") : TEXT("applied"),
				Candidates.Num(),
				Deleted.Num(),
				Skipped.Num(),
				DeleteErrors.Num());
			return MakeExecutionResult(Text, StructuredContent, DeleteErrors.Num() > 0);
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


	namespace
	{
		class FScopedSelfExtensionToolLock
		{
		public:
			FScopedSelfExtensionToolLock(const FString& ToolName, const FJsonObject& Arguments)
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

			~FScopedSelfExtensionToolLock()
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

		bool IsSelfExtensionLockRequiredTool(const FString& ToolName)
		{
			return ToolName == TEXT("unreal.mcp_apply_scaffold")
				|| ToolName == TEXT("unreal.mcp_rollback_last_extension")
				|| ToolName == TEXT("unreal.mcp_backup_project_state")
				|| ToolName == TEXT("unreal.mcp_rollback_to_manifest")
				|| ToolName == TEXT("unreal.mcp_supervisor_install")
				|| ToolName == TEXT("unreal.mcp_generate_tests")
				|| ToolName == TEXT("unreal.mcp_build_editor")
				|| ToolName == TEXT("unreal.mcp_run_tool_test")
				|| ToolName == TEXT("unreal.mcp_run_test_suite")
				|| ToolName == TEXT("unreal.mcp_extension_pipeline");
		}

		bool IsSelfExtensionPieBlockedTool(const FString& ToolName)
		{
			return ToolName == TEXT("unreal.mcp_apply_scaffold")
				|| ToolName == TEXT("unreal.mcp_rollback_last_extension")
				|| ToolName == TEXT("unreal.mcp_rollback_to_manifest")
				|| ToolName == TEXT("unreal.mcp_generate_tests")
				|| ToolName == TEXT("unreal.mcp_extension_pipeline");
		}

		FUnrealMcpExecutionResult MakeSelfExtensionLockFailure(const FString& Action, const FScopedSelfExtensionToolLock& ScopedLock)
		{
			return MakeExecutionResult(
				ScopedLock.GetFailureReason(),
				ScopedLock.MakeStructuredContent(Action),
				true);
		}
	}

	bool TryExecuteSelfExtensionTool(
		const FString& ToolName,
		const FJsonObject& Arguments,
		const TArray<TSharedPtr<FJsonValue>>& ToolsArray,
		const FSelfExtensionModuleToolRunner& RunToolTest,
		const FSelfExtensionModuleToolRunner& RunTestSuite,
		const FSelfExtensionModuleToolRunner& RunExtensionPipeline,
		FUnrealMcpExecutionResult& OutResult)
	{
		if (ToolName == TEXT("unreal.mcp_list_scaffolds"))
		{
			OutResult = ListMcpScaffolds(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_inspect_scaffold"))
		{
			OutResult = InspectMcpScaffold(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_validate_cpp_snippet"))
		{
			OutResult = ValidateCppSnippet(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_patch_scaffold_snippet"))
		{
			OutResult = IsEditorPlaying() ? MakePieBlockedResult(ToolName) : PatchScaffoldSnippet(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_validate_tool_schema"))
		{
			OutResult = ValidateMcpToolSchema(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_tool_audit"))
		{
			OutResult = AuditMcpTools(ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_workbench_status"))
		{
			OutResult = WorkbenchStatus(Arguments, ToolsArray);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_compile_error_fix_plan"))
		{
			OutResult = CompileErrorFixPlan(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_pipeline_status"))
		{
			OutResult = PipelineStatus(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_diff_last_apply"))
		{
			OutResult = DiffLastMcpApply(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_clean_test_artifacts"))
		{
			OutResult = CleanMcpTestArtifacts(Arguments);
			return true;
		}

		if (ToolName == TEXT("unreal.mcp_lock_extension_session"))
		{
			OutResult = LockExtensionSession(Arguments);
			return true;
		}

		if (IsSelfExtensionLockRequiredTool(ToolName))
		{
			if (IsSelfExtensionPieBlockedTool(ToolName) && IsEditorPlaying())
			{
				OutResult = MakePieBlockedResult(ToolName);
				return true;
			}

			FScopedSelfExtensionToolLock ScopedLock(ToolName, Arguments);
			if (!ScopedLock.IsAcquired())
			{
				OutResult = MakeSelfExtensionLockFailure(TEXT("mcp_extension_lock_failed"), ScopedLock);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_apply_scaffold"))
			{
				OutResult = ApplyMcpScaffold(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_rollback_last_extension"))
			{
				OutResult = RollbackLastMcpExtension(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_backup_project_state"))
			{
				OutResult = BackupProjectState(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_rollback_to_manifest"))
			{
				OutResult = RollbackToManifest(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_supervisor_install"))
			{
				OutResult = SupervisorInstall(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_generate_tests"))
			{
				OutResult = GenerateMcpTests(Arguments, ToolsArray);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_run_tool_test"))
			{
				OutResult = RunToolTest(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_run_test_suite"))
			{
				OutResult = RunTestSuite(Arguments);
				return true;
			}

			if (ToolName == TEXT("unreal.mcp_extension_pipeline"))
			{
				OutResult = RunExtensionPipeline(Arguments);
				return true;
			}

			OutResult = BuildEditor(Arguments);
			return true;
		}

		return false;
	}

	FUnrealMcpExecutionResult PipelineStatus(const FJsonObject& Arguments)
	{
		FString MemoryKey = TEXT("mcp.extension.pipeline");
		bool bIncludeAllMemory = false;
		bool bIncludeBuildLogTail = true;
		Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
		Arguments.TryGetBoolField(TEXT("includeAllMemory"), bIncludeAllMemory);
		Arguments.TryGetBoolField(TEXT("includeBuildLogTail"), bIncludeBuildLogTail);
		const int32 BuildLogTailLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("buildLogTailLines"), 80), 500);

		FString FailureReason;
		TSharedPtr<FJsonObject> MemoryObject;
		if (!LoadProjectMemory(MemoryObject, FailureReason))
		{
			return MakeExecutionResult(FailureReason, nullptr, true);
		}

		TSharedPtr<FJsonObject> MatchingMemoryEntry = FindMemoryEntryByKey(MemoryObject, MemoryKey.TrimStartAndEnd());
		TArray<TSharedPtr<FJsonValue>> MemorySummaries;
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (MemoryObject->TryGetArrayField(TEXT("entries"), Entries) && Entries)
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
			{
				if (EntryValue.IsValid() && EntryValue->Type == EJson::Object && EntryValue->AsObject().IsValid())
				{
					MemorySummaries.Add(MakeShared<FJsonValueObject>(MakeMemoryEntrySummary(EntryValue->AsObject(), bIncludeAllMemory)));
				}
			}
		}

		TArray<FString> TestScaffolds;
		TArray<FString> ExtensionBackups;
		TArray<FString> TestRequests;
		FindImmediateChildren(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestScaffolds")), TEXT("*"), false, true, TestScaffolds);
		FindImmediateChildren(GetMcpExtensionBackupRoot(), TEXT("*"), false, true, ExtensionBackups);
		FindImmediateChildren(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestRequests")), TEXT("*"), false, true, TestRequests);

		TArray<TSharedPtr<FJsonValue>> TestScaffoldValues;
		for (const FString& Path : TestScaffolds)
		{
			TestScaffoldValues.Add(MakeShared<FJsonValueString>(Path));
		}
		TArray<TSharedPtr<FJsonValue>> BackupValues;
		for (const FString& Path : ExtensionBackups)
		{
			BackupValues.Add(MakeShared<FJsonValueString>(Path));
		}
		TArray<TSharedPtr<FJsonValue>> TestRequestValues;
		for (const FString& Path : TestRequests)
		{
			TestRequestValues.Add(MakeShared<FJsonValueString>(Path));
		}

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_pipeline_status"));
		StructuredContent->SetStringField(TEXT("savedRoot"), GetUnrealMcpSavedRoot());
		StructuredContent->SetStringField(TEXT("memoryPath"), GetProjectMemoryFilePath());
		StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
		StructuredContent->SetBoolField(TEXT("memoryEntryFound"), MatchingMemoryEntry.IsValid());
		StructuredContent->SetObjectField(TEXT("selectedMemoryEntry"), MakeMemoryEntrySummary(MatchingMemoryEntry, true));
		StructuredContent->SetNumberField(TEXT("memoryEntryCount"), MemorySummaries.Num());
		StructuredContent->SetArrayField(TEXT("memoryEntries"), MemorySummaries);
		StructuredContent->SetStringField(TEXT("latestManifestPath"), GetLatestMcpExtensionManifestPath());
		StructuredContent->SetObjectField(TEXT("latestManifestFile"), MakeFileInfoObject(GetLatestMcpExtensionManifestPath()));

		TSharedPtr<FJsonObject> ManifestObject;
		if (FPaths::FileExists(GetLatestMcpExtensionManifestPath()) && LoadJsonObjectFromFile(GetLatestMcpExtensionManifestPath(), ManifestObject, FailureReason))
		{
			StructuredContent->SetObjectField(TEXT("latestManifest"), ManifestObject);
		}
		else if (!FailureReason.IsEmpty())
		{
			StructuredContent->SetStringField(TEXT("latestManifestReadWarning"), FailureReason);
		}

		FString LatestBuildLogPath;
		StructuredContent->SetStringField(TEXT("buildLogRoot"), GetMcpBuildLogRoot());
		if (FindNewestFile(GetMcpBuildLogRoot(), TEXT("*.log"), LatestBuildLogPath))
		{
			StructuredContent->SetObjectField(TEXT("latestBuildLog"), MakeFileInfoObject(LatestBuildLogPath));
			if (bIncludeBuildLogTail)
			{
				FString LogText;
				if (FFileHelper::LoadFileToString(LogText, *LatestBuildLogPath))
				{
					StructuredContent->SetStringField(TEXT("latestBuildLogTail"), TailLines(LogText, BuildLogTailLines));
				}
			}
		}

		StructuredContent->SetNumberField(TEXT("testScaffoldCount"), TestScaffolds.Num());
		StructuredContent->SetArrayField(TEXT("testScaffolds"), TestScaffoldValues);
		StructuredContent->SetNumberField(TEXT("extensionBackupCount"), ExtensionBackups.Num());
		StructuredContent->SetArrayField(TEXT("extensionBackups"), BackupValues);
		StructuredContent->SetNumberField(TEXT("testRequestCount"), TestRequests.Num());
		StructuredContent->SetArrayField(TEXT("testRequests"), TestRequestValues);
		StructuredContent->SetStringField(TEXT("recommendedNextStep"), RecommendPipelineNextStep(MatchingMemoryEntry));

		const FString Text = FString::Printf(
			TEXT("MCP pipeline status: memoryKey=%s found=%s testScaffolds=%d backups=%d."),
			*MemoryKey,
			MatchingMemoryEntry.IsValid() ? TEXT("true") : TEXT("false"),
			TestScaffolds.Num(),
			ExtensionBackups.Num());
		return MakeExecutionResult(Text, StructuredContent, false);
	}

	FUnrealMcpExecutionResult WorkbenchStatus(const FJsonObject& Arguments, const TArray<TSharedPtr<FJsonValue>>& ToolsArray)
	{
		FString MemoryKey = TEXT("mcp.extension.pipeline");
		bool bIncludeBuildLogTail = false;
		Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
		Arguments.TryGetBoolField(TEXT("includeBuildLogTail"), bIncludeBuildLogTail);
		const int32 BuildLogTailLines = FMath::Min(GetPositiveIntArgument(Arguments, TEXT("buildLogTailLines"), 80), 500);

		TSharedPtr<FJsonObject> PipelineArguments = MakeShared<FJsonObject>();
		PipelineArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		PipelineArguments->SetBoolField(TEXT("includeAllMemory"), false);
		PipelineArguments->SetBoolField(TEXT("includeBuildLogTail"), bIncludeBuildLogTail);
		PipelineArguments->SetNumberField(TEXT("buildLogTailLines"), BuildLogTailLines);

		const FUnrealMcpExecutionResult AuditResult = AuditMcpTools(ToolsArray);
		const FUnrealMcpExecutionResult PipelineResult = PipelineStatus(*PipelineArguments);

		double SchemaIncompatibleCount = 0.0;
		double MissingHandlerCount = 0.0;
		double MissingDocumentationCount = 0.0;
		if (AuditResult.StructuredContent.IsValid())
		{
			AuditResult.StructuredContent->TryGetNumberField(TEXT("schemaIncompatibleCount"), SchemaIncompatibleCount);
			AuditResult.StructuredContent->TryGetNumberField(TEXT("missingHandlerCount"), MissingHandlerCount);
			AuditResult.StructuredContent->TryGetNumberField(TEXT("missingDocumentationCount"), MissingDocumentationCount);
		}

		double MemoryEntryCount = 0.0;
		double TestScaffoldCount = 0.0;
		double ExtensionBackupCount = 0.0;
		if (PipelineResult.StructuredContent.IsValid())
		{
			PipelineResult.StructuredContent->TryGetNumberField(TEXT("memoryEntryCount"), MemoryEntryCount);
			PipelineResult.StructuredContent->TryGetNumberField(TEXT("testScaffoldCount"), TestScaffoldCount);
			PipelineResult.StructuredContent->TryGetNumberField(TEXT("extensionBackupCount"), ExtensionBackupCount);
		}

		TArray<FString> TestScaffolds;
		FindImmediateChildren(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("TestScaffolds")), TEXT("*"), false, true, TestScaffolds);

		int32 SavedTestCaseCount = 0;
		for (const FString& TestScaffold : TestScaffolds)
		{
			TArray<FString> TestFiles;
			FindImmediateChildren(FPaths::Combine(TestScaffold, TEXT("Tests")), TEXT("*.json"), true, false, TestFiles);
			SavedTestCaseCount += TestFiles.Num();
		}

		TArray<FString> VersionedTestFiles;
		const FString VersionedTestRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpTests")));
		IFileManager::Get().FindFilesRecursive(VersionedTestFiles, *VersionedTestRoot, TEXT("*.json"), true, false);
		const int32 VersionedTestCaseCount = VersionedTestFiles.Num();
		const int32 TestCaseCount = SavedTestCaseCount + VersionedTestCaseCount;

		FString LatestSupervisorLogPath;
		const bool bHasSupervisorLog = FindNewestFile(FPaths::Combine(GetUnrealMcpSavedRoot(), TEXT("SupervisorLogs")), TEXT("*.log"), LatestSupervisorLogPath);

		TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
		StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_workbench_status"));
		StructuredContent->SetStringField(TEXT("projectName"), FApp::GetProjectName());
		StructuredContent->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);
		StructuredContent->SetObjectField(TEXT("audit"), AuditResult.StructuredContent.IsValid() ? AuditResult.StructuredContent : MakeShared<FJsonObject>());
		StructuredContent->SetObjectField(TEXT("pipeline"), PipelineResult.StructuredContent.IsValid() ? PipelineResult.StructuredContent : MakeShared<FJsonObject>());
		StructuredContent->SetNumberField(TEXT("visibleToolCount"), ToolsArray.Num());
		StructuredContent->SetNumberField(TEXT("testCaseCount"), TestCaseCount);
		StructuredContent->SetNumberField(TEXT("savedTestCaseCount"), SavedTestCaseCount);
		StructuredContent->SetNumberField(TEXT("versionedTestCaseCount"), VersionedTestCaseCount);
		StructuredContent->SetStringField(TEXT("versionedTestRoot"), VersionedTestRoot);
		StructuredContent->SetBoolField(TEXT("supervisorLogFound"), bHasSupervisorLog);
		if (bHasSupervisorLog)
		{
			StructuredContent->SetObjectField(TEXT("latestSupervisorLog"), MakeFileInfoObject(LatestSupervisorLogPath));
		}
		AddToolRegistryStatus(StructuredContent);

		const bool bHealthy = SchemaIncompatibleCount <= 0.0
			&& MissingHandlerCount <= 0.0
			&& MissingDocumentationCount <= 0.0
			&& !PipelineResult.bIsError;
		StructuredContent->SetBoolField(TEXT("healthy"), bHealthy);
		StructuredContent->SetStringField(
			TEXT("recommendedNextStep"),
			bHealthy
				? TEXT("Continue with ToolRegistry modularization and add versioned test fixtures for core self-extension tools.")
				: TEXT("Run unreal.mcp_tool_audit and address schema, handler, documentation, or pipeline warnings before adding more tools."));

		const FString Text = FString::Printf(
			TEXT("MCP workbench status: visibleTools=%d schemaIncompatible=%d missingHandlers=%d memoryEntries=%d testScaffolds=%d testCases=%d healthy=%s"),
			ToolsArray.Num(),
			static_cast<int32>(SchemaIncompatibleCount),
			static_cast<int32>(MissingHandlerCount),
			static_cast<int32>(MemoryEntryCount),
			static_cast<int32>(TestScaffoldCount),
			TestCaseCount,
			bHealthy ? TEXT("true") : TEXT("false"));
		return MakeExecutionResult(Text, StructuredContent, false);
	}

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
			bSucceeded ? TEXT("Continue with tool audit or next MCP extension stage.") : TEXT("Inspect failedCases, patch snippets, rebuild, and rerun the suite."),
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

FUnrealMcpExecutionResult FUnrealMcpModule::RunMcpExtensionPipeline(const FJsonObject& Arguments) const
{
	FString Mode = TEXT("auto");
	FString ToolName;
	FString ScaffoldDir;
	FString OutputRoot = TEXT("Tools/UnrealMcpToolScaffolds");
	FString SchemaJson;
	FString TestRequestPath;
	FString TestsDir;
	FString MemoryKey = TEXT("mcp.extension.pipeline");
	bool bApply = true;
	bool bBuild = true;
	bool bRunTest = true;
	bool bRunTestSuite = true;
	bool bGenerateTests = true;
	bool bOverwriteTests = true;
	bool bDryRunOnly = false;
	bool bApplyChatCommand = true;
	bool bCreateBackup = true;
	bool bBackupProjectState = true;
	bool bWriteProjectMemory = true;

	Arguments.TryGetStringField(TEXT("mode"), Mode);
	Arguments.TryGetStringField(TEXT("toolName"), ToolName);
	Arguments.TryGetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	Arguments.TryGetStringField(TEXT("outputRoot"), OutputRoot);
	Arguments.TryGetStringField(TEXT("schemaJson"), SchemaJson);
	Arguments.TryGetStringField(TEXT("testRequestPath"), TestRequestPath);
	Arguments.TryGetStringField(TEXT("testsDir"), TestsDir);
	Arguments.TryGetStringField(TEXT("memoryKey"), MemoryKey);
	Arguments.TryGetBoolField(TEXT("apply"), bApply);
	Arguments.TryGetBoolField(TEXT("build"), bBuild);
	Arguments.TryGetBoolField(TEXT("runTest"), bRunTest);
	Arguments.TryGetBoolField(TEXT("runTestSuite"), bRunTestSuite);
	Arguments.TryGetBoolField(TEXT("generateTests"), bGenerateTests);
	Arguments.TryGetBoolField(TEXT("overwriteTests"), bOverwriteTests);
	Arguments.TryGetBoolField(TEXT("dryRunOnly"), bDryRunOnly);
	Arguments.TryGetBoolField(TEXT("applyChatCommand"), bApplyChatCommand);
	Arguments.TryGetBoolField(TEXT("createBackup"), bCreateBackup);
	Arguments.TryGetBoolField(TEXT("backupProjectState"), bBackupProjectState);
	Arguments.TryGetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);

	Mode = Mode.TrimStartAndEnd().ToLower();
	ToolName = ToolName.TrimStartAndEnd();
	ScaffoldDir = ScaffoldDir.TrimStartAndEnd();
	SchemaJson = SchemaJson.TrimStartAndEnd();
	TestRequestPath = TestRequestPath.TrimStartAndEnd();
	TestsDir = TestsDir.TrimStartAndEnd();
	MemoryKey = MemoryKey.TrimStartAndEnd();
	if (Mode.IsEmpty())
	{
		Mode = TEXT("auto");
	}
	if (MemoryKey.IsEmpty())
	{
		MemoryKey = TEXT("mcp.extension.pipeline");
	}

	TArray<TSharedPtr<FJsonValue>> Steps;
	TArray<TSharedPtr<FJsonValue>> Issues;
	bool bSucceeded = true;
	bool bRequiresRestart = false;
	bool bAppliedSourceChanges = false;
	bool bBuildSucceeded = false;

	TSharedPtr<FJsonObject> StructuredContent = MakeShared<FJsonObject>();
	StructuredContent->SetStringField(TEXT("action"), TEXT("mcp_extension_pipeline"));
	StructuredContent->SetStringField(TEXT("mode"), Mode);
	StructuredContent->SetStringField(TEXT("memoryKey"), MemoryKey);

	if (Mode == TEXT("resume_test") || Mode == TEXT("test") || Mode == TEXT("test_only"))
	{
		TSharedPtr<FJsonObject> TestArguments = MakeShared<FJsonObject>();
		TestArguments->SetStringField(TEXT("toolName"), ToolName);
		TestArguments->SetStringField(TEXT("testRequestPath"), TestRequestPath);
		TestArguments->SetStringField(TEXT("testsDir"), TestsDir);
		TestArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
		TestArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
		TestArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		TestArguments->SetBoolField(TEXT("readProjectMemory"), true);
		TestArguments->SetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
		const FUnrealMcpExecutionResult TestResult = bRunTestSuite ? RunMcpTestSuite(*TestArguments) : RunMcpToolTest(*TestArguments);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TestResult.bIsError ? TEXT("failed") : TEXT("completed"),
			TestResult.Text,
			&TestResult)));

		StructuredContent->SetStringField(TEXT("toolName"), ToolName);
		StructuredContent->SetBoolField(TEXT("succeeded"), !TestResult.bIsError);
		StructuredContent->SetBoolField(TEXT("requiresRestart"), false);
		StructuredContent->SetArrayField(TEXT("steps"), Steps);
		StructuredContent->SetArrayField(TEXT("issues"), Issues);
		return UnrealMcp::MakeExecutionResult(
			TestResult.bIsError ? TEXT("MCP extension pipeline resume test failed.") : TEXT("MCP extension pipeline resume test completed."),
			StructuredContent,
			TestResult.bIsError);
	}

	FString ResolvedScaffoldDir;
	FString ResolvedToolName;
	FString ResolveFailure;
	TSharedPtr<FJsonObject> ResolveArguments = MakeShared<FJsonObject>();
	ResolveArguments->SetStringField(TEXT("toolName"), ToolName);
	ResolveArguments->SetStringField(TEXT("scaffoldDir"), ScaffoldDir);
	ResolveArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
	if (!UnrealMcp::ResolveMcpScaffoldDirectory(*ResolveArguments, ResolvedScaffoldDir, ResolvedToolName, ResolveFailure))
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("resolve_scaffold"), TEXT("failed"), ResolveFailure)));
		StructuredContent->SetBoolField(TEXT("succeeded"), false);
		StructuredContent->SetBoolField(TEXT("requiresRestart"), false);
		StructuredContent->SetArrayField(TEXT("steps"), Steps);
		StructuredContent->SetArrayField(TEXT("issues"), Issues);
		return UnrealMcp::MakeExecutionResult(ResolveFailure, StructuredContent, true);
	}

	ToolName = ResolvedToolName;
	if (TestRequestPath.IsEmpty())
	{
		TestRequestPath = FPaths::Combine(ResolvedScaffoldDir, TEXT("TestRequest.json"));
	}
	if (TestsDir.IsEmpty())
	{
		TestsDir = FPaths::Combine(ResolvedScaffoldDir, TEXT("Tests"));
	}
	StructuredContent->SetStringField(TEXT("toolName"), ToolName);
	StructuredContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
	StructuredContent->SetStringField(TEXT("testRequestPath"), TestRequestPath);
	StructuredContent->SetStringField(TEXT("testsDir"), TestsDir);
	Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
		TEXT("resolve_scaffold"),
		TEXT("completed"),
		FString::Printf(TEXT("Resolved scaffold for %s."), *ToolName))));

	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	AppendToolDefinitions(ToolsArray);
	const bool bToolAlreadyListed = UnrealMcp::FindToolDefinitionByName(ToolsArray, ToolName).IsValid();
	StructuredContent->SetBoolField(TEXT("toolAlreadyListed"), bToolAlreadyListed);

	if (SchemaJson.IsEmpty())
	{
		UnrealMcp::ExtractRequestedSchemaFromScaffoldReadme(ResolvedScaffoldDir, SchemaJson);
	}

	if (!SchemaJson.IsEmpty() || bToolAlreadyListed)
	{
		TSharedPtr<FJsonObject> ValidateArguments = MakeShared<FJsonObject>();
		if (!SchemaJson.IsEmpty())
		{
			ValidateArguments->SetStringField(TEXT("schemaJson"), SchemaJson);
		}
		else
		{
			ValidateArguments->SetStringField(TEXT("toolName"), ToolName);
		}
		ValidateArguments->SetBoolField(TEXT("returnNormalizedSchema"), true);
		const FUnrealMcpExecutionResult ValidateResult = UnrealMcp::ValidateMcpToolSchema(*ValidateArguments, ToolsArray);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("validate_schema"),
			ValidateResult.bIsError ? TEXT("failed") : TEXT("completed"),
			ValidateResult.Text,
			&ValidateResult)));
		if (ValidateResult.bIsError)
		{
			bSucceeded = false;
		}
	}
	else
	{
		UnrealMcp::AddAuditIssue(
			Issues,
			TEXT("warning"),
			TEXT("schemaJson"),
			TEXT("No schemaJson provided and the tool is not loaded yet; skipped requested schema validation."));
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("validate_schema"),
			TEXT("skipped"),
			TEXT("No schemaJson found; skipped schema validation."))));
	}

	if (bSucceeded && bGenerateTests)
	{
		TSharedPtr<FJsonObject> GenerateArguments = MakeShared<FJsonObject>();
		GenerateArguments->SetStringField(TEXT("toolName"), ToolName);
		GenerateArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		GenerateArguments->SetStringField(TEXT("testsDir"), TestsDir);
		GenerateArguments->SetStringField(TEXT("outputRoot"), OutputRoot);
		GenerateArguments->SetStringField(TEXT("schemaJson"), SchemaJson);
		GenerateArguments->SetBoolField(TEXT("overwrite"), bOverwriteTests);
		GenerateArguments->SetBoolField(TEXT("dryRun"), bDryRunOnly);
		const FUnrealMcpExecutionResult GenerateTestsResult = UnrealMcp::GenerateMcpTests(*GenerateArguments, ToolsArray);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("generate_tests"),
			GenerateTestsResult.bIsError ? TEXT("failed") : TEXT("completed"),
			GenerateTestsResult.Text,
			&GenerateTestsResult)));
		if (GenerateTestsResult.bIsError)
		{
			bSucceeded = false;
		}
	}
	else if (!bGenerateTests)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("generate_tests"), TEXT("skipped"), TEXT("generateTests=false."))));
	}

	if (bSucceeded)
	{
		TSharedPtr<FJsonObject> DryRunArguments = MakeShared<FJsonObject>();
		DryRunArguments->SetStringField(TEXT("toolName"), ToolName);
		DryRunArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		DryRunArguments->SetBoolField(TEXT("dryRun"), true);
		DryRunArguments->SetBoolField(TEXT("applyChatCommand"), bApplyChatCommand);
		DryRunArguments->SetBoolField(TEXT("createBackup"), bCreateBackup);
		const FUnrealMcpExecutionResult DryRunResult = UnrealMcp::ApplyMcpScaffold(*DryRunArguments);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("apply_dry_run"),
			DryRunResult.bIsError ? TEXT("failed") : TEXT("completed"),
			DryRunResult.Text,
			&DryRunResult)));
		if (DryRunResult.bIsError)
		{
			bSucceeded = false;
		}
	}

	if (bSucceeded && bDryRunOnly)
	{
		bApply = false;
		bBuild = false;
		bRunTest = false;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("dry_run_only"),
			TEXT("completed"),
			TEXT("dryRunOnly=true; skipped apply/build/test."))));
	}

	if (bSucceeded && !bDryRunOnly && bBackupProjectState && (bApply || bBuild || bRunTest))
	{
		TSharedPtr<FJsonObject> BackupArguments = MakeShared<FJsonObject>();
		BackupArguments->SetStringField(TEXT("label"), FString::Printf(TEXT("pipeline_%s"), *ToolName));
		BackupArguments->SetStringField(TEXT("reason"), FString::Printf(TEXT("Pre-pipeline snapshot before applying/building/testing MCP tool %s."), *ToolName));
		BackupArguments->SetBoolField(TEXT("includeBuildLogs"), false);
		const FUnrealMcpExecutionResult BackupResult = UnrealMcp::BackupProjectState(*BackupArguments);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("backup_project_state"),
			BackupResult.bIsError ? TEXT("failed") : TEXT("completed"),
			BackupResult.Text,
			&BackupResult)));
		if (BackupResult.bIsError)
		{
			bSucceeded = false;
		}
	}
	else if (bDryRunOnly || !bBackupProjectState)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("backup_project_state"),
			TEXT("skipped"),
			bDryRunOnly ? TEXT("dryRunOnly=true.") : TEXT("backupProjectState=false."))));
	}

	if (bSucceeded && bApply)
	{
		TSharedPtr<FJsonObject> ApplyArguments = MakeShared<FJsonObject>();
		ApplyArguments->SetStringField(TEXT("toolName"), ToolName);
		ApplyArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		ApplyArguments->SetBoolField(TEXT("dryRun"), false);
		ApplyArguments->SetBoolField(TEXT("applyChatCommand"), bApplyChatCommand);
		ApplyArguments->SetBoolField(TEXT("createBackup"), bCreateBackup);
		const FUnrealMcpExecutionResult ApplyResult = UnrealMcp::ApplyMcpScaffold(*ApplyArguments);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("apply"),
			ApplyResult.bIsError ? TEXT("failed") : TEXT("completed"),
			ApplyResult.Text,
			&ApplyResult)));
		if (ApplyResult.bIsError)
		{
			bSucceeded = false;
		}
		else if (ApplyResult.StructuredContent.IsValid())
		{
			ApplyResult.StructuredContent->TryGetBoolField(TEXT("changed"), bAppliedSourceChanges);
		}
	}
	else if (!bApply)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("apply"), TEXT("skipped"), TEXT("apply=false."))));
	}

	if (bSucceeded && bWriteProjectMemory)
	{
		TSharedPtr<FJsonObject> MemoryContent = MakeShared<FJsonObject>();
		MemoryContent->SetStringField(TEXT("toolName"), ToolName);
		MemoryContent->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		MemoryContent->SetStringField(TEXT("testRequestPath"), TestRequestPath);
		MemoryContent->SetStringField(TEXT("testsDir"), TestsDir);
		MemoryContent->SetStringField(TEXT("pipelineMode"), Mode);
		MemoryContent->SetBoolField(TEXT("appliedSourceChanges"), bAppliedSourceChanges);
		MemoryContent->SetBoolField(TEXT("buildRequested"), bBuild);
		MemoryContent->SetBoolField(TEXT("runTestRequested"), bRunTest);
		MemoryContent->SetBoolField(TEXT("runTestSuite"), bRunTestSuite);
		UnrealMcp::WriteBuildTestMemory(
			MemoryKey,
			TEXT("MCP extension pipeline applied scaffold; build/test handoff pending."),
			TEXT("pipeline_apply_complete"),
			bBuild ? TEXT("Run build, restart Unreal Editor if needed, then resume mcp_extension_pipeline with mode=resume_test.") : TEXT("Run mcp_extension_pipeline with mode=resume_test when ready."),
			MemoryContent);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("write_memory"),
			TEXT("completed"),
			FString::Printf(TEXT("Wrote project memory key '%s'."), *MemoryKey))));
	}

	if (bSucceeded && bBuild)
	{
		TSharedPtr<FJsonObject> BuildArguments = MakeShared<FJsonObject>();
		BuildArguments->SetStringField(TEXT("toolName"), ToolName);
		BuildArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		BuildArguments->SetStringField(TEXT("testRequestPath"), TestRequestPath);
		BuildArguments->SetStringField(TEXT("testsDir"), TestsDir);
		BuildArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		BuildArguments->SetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
		const FUnrealMcpExecutionResult BuildResult = UnrealMcp::BuildEditor(*BuildArguments);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("build"),
			BuildResult.bIsError ? TEXT("failed") : TEXT("completed"),
			BuildResult.Text,
			&BuildResult)));
		if (BuildResult.bIsError)
		{
			bSucceeded = false;
		}
		else if (BuildResult.StructuredContent.IsValid())
		{
			BuildResult.StructuredContent->TryGetBoolField(TEXT("succeeded"), bBuildSucceeded);
		}
	}
	else if (!bBuild)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("build"), TEXT("skipped"), TEXT("build=false."))));
	}

	const bool bShouldDeferTestForRestart = bBuild && bBuildSucceeded && bAppliedSourceChanges && !bToolAlreadyListed;
	if (bShouldDeferTestForRestart)
	{
		bRequiresRestart = true;
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			TEXT("restart"),
			TEXT("required"),
			TEXT("New C++ snippets were compiled while the editor was running. Restart Unreal Editor before running the test step."))));
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TEXT("deferred"),
			bRunTestSuite ? TEXT("Run unreal.mcp_extension_pipeline with mode=resume_test after restart to execute the generated test suite.") : TEXT("Run unreal.mcp_extension_pipeline with mode=resume_test after restart, or use Tools/unreal_mcp_supervisor.py resume-test."))));
	}
	else if (bSucceeded && bRunTest)
	{
		TSharedPtr<FJsonObject> TestArguments = MakeShared<FJsonObject>();
		TestArguments->SetStringField(TEXT("toolName"), ToolName);
		TestArguments->SetStringField(TEXT("testRequestPath"), TestRequestPath);
		TestArguments->SetStringField(TEXT("testsDir"), TestsDir);
		TestArguments->SetStringField(TEXT("scaffoldDir"), ResolvedScaffoldDir);
		TestArguments->SetStringField(TEXT("memoryKey"), MemoryKey);
		TestArguments->SetBoolField(TEXT("readProjectMemory"), false);
		TestArguments->SetBoolField(TEXT("writeProjectMemory"), bWriteProjectMemory);
		const FUnrealMcpExecutionResult TestResult = bRunTestSuite ? RunMcpTestSuite(*TestArguments) : RunMcpToolTest(*TestArguments);
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(
			bRunTestSuite ? TEXT("test_suite") : TEXT("test"),
			TestResult.bIsError ? TEXT("failed") : TEXT("completed"),
			TestResult.Text,
			&TestResult)));
		if (TestResult.bIsError)
		{
			bSucceeded = false;
		}
	}
	else if (!bRunTest)
	{
		Steps.Add(MakeShared<FJsonValueObject>(UnrealMcp::MakePipelineStepObject(TEXT("test"), TEXT("skipped"), TEXT("runTest=false."))));
	}

	StructuredContent->SetBoolField(TEXT("succeeded"), bSucceeded);
	StructuredContent->SetBoolField(TEXT("requiresRestart"), bRequiresRestart);
	StructuredContent->SetBoolField(TEXT("appliedSourceChanges"), bAppliedSourceChanges);
	StructuredContent->SetBoolField(TEXT("buildSucceeded"), bBuildSucceeded);
	StructuredContent->SetBoolField(TEXT("generateTests"), bGenerateTests);
	StructuredContent->SetBoolField(TEXT("runTestSuite"), bRunTestSuite);
	StructuredContent->SetBoolField(TEXT("backupProjectState"), bBackupProjectState);
	StructuredContent->SetStringField(TEXT("restartAdvice"), TEXT("If requiresRestart=true, close and reopen Unreal Editor, then call unreal.mcp_extension_pipeline with mode=resume_test and the same memoryKey."));
	StructuredContent->SetStringField(TEXT("supervisorCommand"), FString::Printf(TEXT("python3 Tools/unreal_mcp_supervisor.py resume-test --memory-key %s"), *MemoryKey));
	StructuredContent->SetArrayField(TEXT("steps"), Steps);
	StructuredContent->SetArrayField(TEXT("issues"), Issues);

	const FString Text = bRequiresRestart
		? TEXT("MCP extension pipeline applied and built changes. Restart Unreal Editor, then resume test.")
		: (bSucceeded ? TEXT("MCP extension pipeline completed.") : TEXT("MCP extension pipeline failed. See steps for details."));
	return UnrealMcp::MakeExecutionResult(Text, StructuredContent, !bSucceeded);
}
