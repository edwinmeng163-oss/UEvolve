#include "UnrealMcpToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealMcpToolHandlerRegistry.h"
#include "UnrealMcpToolRegistrar.h"

namespace UnrealMcp
{
	namespace
	{
		struct FLoadedToolRegistry
		{
			TArray<FToolRegistryEntry> Entries;
			FString SourcePath;
			bool bLoadedExplicitRegistry = false;
		};

		EToolExposure ParseExposure(const FString& Value)
		{
			return Value.Equals(TEXT("legacy_hidden"), ESearchCase::IgnoreCase)
				? EToolExposure::LegacyHidden
				: EToolExposure::Visible;
		}

		EToolRiskLevel ParseRiskLevel(const FString& Value)
		{
			if (Value.Equals(TEXT("read_only"), ESearchCase::IgnoreCase))
			{
				return EToolRiskLevel::ReadOnly;
			}
			if (Value.Equals(TEXT("medium"), ESearchCase::IgnoreCase))
			{
				return EToolRiskLevel::Medium;
			}
			if (Value.Equals(TEXT("high"), ESearchCase::IgnoreCase))
			{
				return EToolRiskLevel::High;
			}
			if (Value.Equals(TEXT("critical"), ESearchCase::IgnoreCase))
			{
				return EToolRiskLevel::Critical;
			}
			return EToolRiskLevel::Low;
		}

		EToolExposure ConvertDescriptorExposure(EUnrealMcpToolExposure Exposure)
		{
			return Exposure == EUnrealMcpToolExposure::LegacyHidden ? EToolExposure::LegacyHidden : EToolExposure::Visible;
		}

		EToolRiskLevel ConvertDescriptorRisk(EUnrealMcpToolRisk Risk)
		{
			switch (Risk)
			{
			case EUnrealMcpToolRisk::ReadOnly:
				return EToolRiskLevel::ReadOnly;
			case EUnrealMcpToolRisk::Medium:
				return EToolRiskLevel::Medium;
			case EUnrealMcpToolRisk::High:
				return EToolRiskLevel::High;
			case EUnrealMcpToolRisk::Critical:
				return EToolRiskLevel::Critical;
			case EUnrealMcpToolRisk::Low:
			default:
				return EToolRiskLevel::Low;
			}
		}

		FString GetStringFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, const FString& DefaultValue = FString())
		{
			FString Value;
			return Object.IsValid() && Object->TryGetStringField(FieldName, Value) ? Value : DefaultValue;
		}

		bool GetBoolFieldOrDefault(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, bool bDefaultValue = false)
		{
			bool bValue = bDefaultValue;
			if (Object.IsValid())
			{
				Object->TryGetBoolField(FieldName, bValue);
			}
			return bValue;
		}

		void AddValidationIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& Code,
			const FString& ToolName,
			const FString& Message)
		{
			TSharedPtr<FJsonObject> IssueObject = MakeShared<FJsonObject>();
			IssueObject->SetStringField(TEXT("severity"), Severity);
			IssueObject->SetStringField(TEXT("code"), Code);
			IssueObject->SetStringField(TEXT("toolName"), ToolName);
			IssueObject->SetStringField(TEXT("message"), Message);
			Issues.Add(MakeShared<FJsonValueObject>(IssueObject));
		}

		bool IsKnownToolCategory(const FString& Category)
		{
			static const TSet<FString> KnownCategories = {
				TEXT("actors"),
				TEXT("blueprint"),
				TEXT("editor"),
				TEXT("memory"),
				TEXT("scaffold"),
				TEXT("self-extension"),
				TEXT("skills"),
				TEXT("widget")
			};
			return KnownCategories.Contains(Category);
		}

		FString StripDocsAnchor(const FString& DocsPath)
		{
			FString FilePath;
			FString Anchor;
			if (DocsPath.Split(TEXT("#"), &FilePath, &Anchor))
			{
				return FilePath;
			}
			return DocsPath;
		}

		bool RegistryDocsPathExists(const FString& DocsPath)
		{
			const FString FilePart = StripDocsAnchor(DocsPath).TrimStartAndEnd();
			return !FilePart.IsEmpty() && FPaths::FileExists(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), FilePart)));
		}

		FToolPolicy MakeBuiltInPolicy(EToolRiskLevel RiskLevel, const FString& Reason)
		{
			FToolPolicy Policy;
			Policy.RiskLevel = RiskLevel;
			Policy.Reason = Reason;
			Policy.TestCoverage = TEXT("missing");
			Policy.Owner = TEXT("UEvolve Core");
			Policy.DocsPath = TEXT("README.md#tool-coverage");
			return Policy;
		}

		FToolRegistryEntry MakeBuiltInEntry(
			const FString& Name,
			const FString& Category,
			const FString& HandlerName,
			EToolExposure Exposure,
			const FString& Notes,
			const FToolPolicy& Policy)
		{
			FToolRegistryEntry Entry;
			Entry.Name = Name;
			Entry.Category = Category;
			Entry.HandlerName = HandlerName.IsEmpty() ? Name : HandlerName;
			Entry.Exposure = Exposure;
			Entry.Notes = Notes;
			Entry.Policy = Policy;
			Entry.bLoadedFromExplicitRegistry = false;
			Entry.bLoadedFromDescriptor = false;
			return Entry;
		}

		FToolRegistryEntry MakeDescriptorRegistryEntry(const FUnrealMcpToolDescriptor& Descriptor)
		{
			FToolRegistryEntry Entry;
			Entry.Name = Descriptor.Name;
			Entry.Category = Descriptor.Category;
			Entry.HandlerName = Descriptor.HandlerName.IsEmpty() ? Descriptor.Name : Descriptor.HandlerName;
			Entry.Exposure = ConvertDescriptorExposure(Descriptor.Exposure);
			Entry.Notes = Descriptor.Notes;
			Entry.bLoadedFromExplicitRegistry = false;
			Entry.bLoadedFromDescriptor = true;
			Entry.Policy.RiskLevel = ConvertDescriptorRisk(Descriptor.RiskLevel);
			Entry.Policy.bRequiresWrite = Descriptor.bRequiresWrite;
			Entry.Policy.bRequiresBuild = Descriptor.bRequiresBuild;
			Entry.Policy.bRequiresExternalProcess = Descriptor.bRequiresExternalProcess;
			Entry.Policy.bRequiresRestart = Descriptor.bRequiresRestart;
			Entry.Policy.bRequiresProjectMemory = Descriptor.bRequiresProjectMemory;
			Entry.Policy.bRequiresLock = Descriptor.bRequiresLock;
			Entry.Policy.bDryRunSupport = Descriptor.bDryRunSupport;
			Entry.Policy.bPreflightSupport = Descriptor.bPreflightSupport;
			Entry.Policy.bPostcheckSupport = Descriptor.bPostcheckSupport;
			Entry.Policy.TestCoverage = UnrealMcp::LexToString(Descriptor.TestCoverage);
			Entry.Policy.Owner = Descriptor.Owner;
			Entry.Policy.DocsPath = Descriptor.DocsPath;
			Entry.Policy.Reason = Descriptor.Reason;
			return Entry;
		}

		TArray<FToolRegistryEntry> MakeDescriptorRegistryEntries()
		{
			TArray<FToolRegistryEntry> Entries;
			for (const FRegisteredUnrealMcpToolDescriptor& RegisteredTool : GetRegisteredMcpToolDescriptors())
			{
				Entries.Add(MakeDescriptorRegistryEntry(RegisteredTool.Descriptor));
			}
			return Entries;
		}

		void MergeRegistryOverrides(TArray<FToolRegistryEntry>& BaseEntries, const TArray<FToolRegistryEntry>& OverrideEntries)
		{
			TMap<FString, int32> NameToIndex;
			for (int32 Index = 0; Index < BaseEntries.Num(); ++Index)
			{
				NameToIndex.Add(BaseEntries[Index].Name, Index);
			}

			for (const FToolRegistryEntry& OverrideEntry : OverrideEntries)
			{
				if (int32* ExistingIndex = NameToIndex.Find(OverrideEntry.Name))
				{
					const bool bWasDescriptorBacked = BaseEntries[*ExistingIndex].bLoadedFromDescriptor;
					BaseEntries[*ExistingIndex] = OverrideEntry;
					BaseEntries[*ExistingIndex].bLoadedFromDescriptor = bWasDescriptorBacked;
				}
				else
				{
					NameToIndex.Add(OverrideEntry.Name, BaseEntries.Num());
					BaseEntries.Add(OverrideEntry);
				}
			}
		}

		TArray<FToolRegistryEntry> MakeBuiltInFallbackEntries()
		{
			FToolPolicy LegacyPolicy = MakeBuiltInPolicy(EToolRiskLevel::Medium, TEXT("Built-in fallback for legacy flexible-schema compatibility tool."));
			LegacyPolicy.bRequiresWrite = true;

			FToolPolicy AliasPolicy = MakeBuiltInPolicy(EToolRiskLevel::Medium, TEXT("Built-in fallback for fixed-schema actor spawn wrapper."));
			AliasPolicy.bRequiresWrite = true;

			FToolPolicy WorkbenchPolicy = MakeBuiltInPolicy(EToolRiskLevel::ReadOnly, TEXT("Built-in fallback for read-only self-extension workbench health summary."));

			return {
				MakeBuiltInEntry(TEXT("unreal.batch_set_actor_properties"), TEXT("actors"), TEXT("unreal.batch_set_actor_properties"), EToolExposure::LegacyHidden, TEXT("Legacy flexible property map uses additionalProperties=true; prefer fixed-schema batch actor tools."), LegacyPolicy),
				MakeBuiltInEntry(TEXT("unreal.spawn_actor"), TEXT("actors"), TEXT("unreal.spawn_actor"), EToolExposure::LegacyHidden, TEXT("Legacy spawn tool supports freeform property overrides; prefer unreal.spawn_actor_basic or unreal.spawn_static_mesh_actor."), LegacyPolicy),
				MakeBuiltInEntry(TEXT("unreal.spawn_actor_batch"), TEXT("actors"), TEXT("unreal.spawn_actor_batch"), EToolExposure::LegacyHidden, TEXT("Legacy batch spawn supports freeform item objects; prefer unreal.spawn_actor_batch_basic."), LegacyPolicy),
				MakeBuiltInEntry(TEXT("unreal.spawn_actor_basic"), TEXT("actors"), TEXT("unreal.spawn_actor"), EToolExposure::Visible, TEXT("AI-facing fixed-schema wrapper routed to the legacy spawn handler."), AliasPolicy),
				MakeBuiltInEntry(TEXT("unreal.spawn_actor_batch_basic"), TEXT("actors"), TEXT("unreal.spawn_actor_batch"), EToolExposure::Visible, TEXT("AI-facing fixed-schema wrapper routed to the legacy batch spawn handler."), AliasPolicy),
				MakeBuiltInEntry(TEXT("unreal.mcp_workbench_status"), TEXT("self-extension"), TEXT("unreal.mcp_workbench_status"), EToolExposure::Visible, TEXT("Read-only self-extension workbench health summary."), WorkbenchPolicy)
			};
		}

		void AddRegistryCandidatePaths(TArray<FString>& OutPaths)
		{
			OutPaths.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Tools/UnrealMcpToolRegistry/tools.json"))));

			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealMcp"));
			if (Plugin.IsValid())
			{
				OutPaths.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/ToolRegistry/tools.json"))));
			}
		}

		bool LoadRegistryEntriesFromPath(const FString& RegistryPath, TArray<FToolRegistryEntry>& OutEntries)
		{
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *RegistryPath))
			{
				return false;
			}

			TSharedPtr<FJsonObject> RootObject;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
			{
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* ToolValues = nullptr;
			if (!RootObject->TryGetArrayField(TEXT("tools"), ToolValues) || ToolValues == nullptr)
			{
				return false;
			}

			TArray<FToolRegistryEntry> LoadedEntries;
			for (const TSharedPtr<FJsonValue>& ToolValue : *ToolValues)
			{
				if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
				{
					continue;
				}

				const TSharedPtr<FJsonObject> ToolObject = ToolValue->AsObject();
				const FString Name = GetStringFieldOrDefault(ToolObject, TEXT("name")).TrimStartAndEnd();
				if (Name.IsEmpty())
				{
					continue;
				}

				FToolRegistryEntry Entry;
				Entry.Name = Name;
				Entry.Category = GetStringFieldOrDefault(ToolObject, TEXT("category"), TEXT("uncategorized"));
				Entry.HandlerName = GetStringFieldOrDefault(ToolObject, TEXT("handlerName"), Name);
				Entry.Exposure = ParseExposure(GetStringFieldOrDefault(ToolObject, TEXT("exposure"), TEXT("visible")));
				Entry.Notes = GetStringFieldOrDefault(ToolObject, TEXT("notes"));
				Entry.bLoadedFromExplicitRegistry = true;
				Entry.bLoadedFromDescriptor = false;

				Entry.Policy.RiskLevel = ParseRiskLevel(GetStringFieldOrDefault(ToolObject, TEXT("riskLevel"), TEXT("low")));
				Entry.Policy.bRequiresWrite = GetBoolFieldOrDefault(ToolObject, TEXT("requiresWrite"));
				Entry.Policy.bRequiresBuild = GetBoolFieldOrDefault(ToolObject, TEXT("requiresBuild"));
				Entry.Policy.bRequiresExternalProcess = GetBoolFieldOrDefault(ToolObject, TEXT("requiresExternalProcess"));
				Entry.Policy.bRequiresRestart = GetBoolFieldOrDefault(ToolObject, TEXT("requiresRestart"));
				Entry.Policy.bRequiresProjectMemory = GetBoolFieldOrDefault(ToolObject, TEXT("requiresProjectMemory"));
				Entry.Policy.bRequiresLock = GetBoolFieldOrDefault(ToolObject, TEXT("requiresLock"));
				Entry.Policy.bDryRunSupport = GetBoolFieldOrDefault(ToolObject, TEXT("dryRunSupport"));
				Entry.Policy.bPreflightSupport = GetBoolFieldOrDefault(ToolObject, TEXT("preflightSupport"));
				Entry.Policy.bPostcheckSupport = GetBoolFieldOrDefault(ToolObject, TEXT("postcheckSupport"));
				Entry.Policy.TestCoverage = GetStringFieldOrDefault(ToolObject, TEXT("testCoverage"), TEXT("missing"));
				Entry.Policy.Owner = GetStringFieldOrDefault(ToolObject, TEXT("owner"), TEXT("Unowned"));
				Entry.Policy.DocsPath = GetStringFieldOrDefault(ToolObject, TEXT("docsPath"));
				Entry.Policy.Reason = GetStringFieldOrDefault(ToolObject, TEXT("reason"), TEXT("Explicit registry policy."));

				LoadedEntries.Add(MoveTemp(Entry));
			}

			if (LoadedEntries.Num() == 0)
			{
				return false;
			}

			OutEntries = MoveTemp(LoadedEntries);
			return true;
		}

		FLoadedToolRegistry LoadToolRegistry()
		{
			TArray<FToolRegistryEntry> DescriptorEntries = MakeDescriptorRegistryEntries();
			TArray<FString> CandidatePaths;
			AddRegistryCandidatePaths(CandidatePaths);

			for (const FString& CandidatePath : CandidatePaths)
			{
				TArray<FToolRegistryEntry> LoadedEntries;
				if (LoadRegistryEntriesFromPath(CandidatePath, LoadedEntries))
				{
					MergeRegistryOverrides(DescriptorEntries, LoadedEntries);
					FLoadedToolRegistry Registry;
					Registry.Entries = MoveTemp(DescriptorEntries);
					Registry.SourcePath = FString::Printf(TEXT("<code descriptors> + %s"), *CandidatePath);
					Registry.bLoadedExplicitRegistry = true;
					return Registry;
				}
			}

			FLoadedToolRegistry Registry;
			const bool bHasDescriptorEntries = DescriptorEntries.Num() > 0;
			Registry.Entries = bHasDescriptorEntries ? MoveTemp(DescriptorEntries) : MakeBuiltInFallbackEntries();
			Registry.SourcePath = bHasDescriptorEntries ? TEXT("<code descriptors>") : TEXT("<built-in fallback>");
			Registry.bLoadedExplicitRegistry = false;
			return Registry;
		}

		const FLoadedToolRegistry& GetLoadedToolRegistry()
		{
			static const FLoadedToolRegistry Registry = LoadToolRegistry();
			return Registry;
		}
	}

	const TArray<FToolRegistryEntry>& GetToolRegistryEntries()
	{
		return GetLoadedToolRegistry().Entries;
	}

	const FToolRegistryEntry* FindToolRegistryEntry(const FString& ToolName)
	{
		for (const FToolRegistryEntry& Entry : GetToolRegistryEntries())
		{
			if (Entry.Name.Equals(ToolName, ESearchCase::CaseSensitive))
			{
				return &Entry;
			}
		}
		return nullptr;
	}

	bool HasExplicitToolRegistryEntry(const FString& ToolName)
	{
		if (const FToolRegistryEntry* Entry = FindToolRegistryEntry(ToolName))
		{
			return Entry->bLoadedFromExplicitRegistry;
		}
		return false;
	}

	FString GetToolRegistrySourcePath()
	{
		return GetLoadedToolRegistry().SourcePath;
	}

	bool ShouldExposeToolToAi(const FString& ToolName)
	{
		if (const FToolRegistryEntry* Entry = FindToolRegistryEntry(ToolName))
		{
			return Entry->Exposure == EToolExposure::Visible;
		}
		return true;
	}

	FString ResolveToolHandlerName(const FString& ToolName)
	{
		if (const FToolRegistryEntry* Entry = FindToolRegistryEntry(ToolName))
		{
			if (!Entry->HandlerName.IsEmpty())
			{
				return Entry->HandlerName;
			}
		}
		return ToolName;
	}

	FString LexToString(EToolRiskLevel RiskLevel)
	{
		switch (RiskLevel)
		{
		case EToolRiskLevel::ReadOnly:
			return TEXT("read_only");
		case EToolRiskLevel::Low:
			return TEXT("low");
		case EToolRiskLevel::Medium:
			return TEXT("medium");
		case EToolRiskLevel::High:
			return TEXT("high");
		case EToolRiskLevel::Critical:
			return TEXT("critical");
		default:
			return TEXT("unknown");
		}
	}

	FToolPolicy GetToolPolicy(const FString& ToolName)
	{
		if (const FToolRegistryEntry* Entry = FindToolRegistryEntry(ToolName))
		{
			return Entry->Policy;
		}

		FToolPolicy Policy;
		Policy.RiskLevel = EToolRiskLevel::Medium;
		Policy.bRequiresWrite = true;
		Policy.TestCoverage = TEXT("missing");
		Policy.Owner = TEXT("Unregistered");
		Policy.DocsPath = TEXT("");
		Policy.Reason = TEXT("Tool is missing from the explicit registry; defaulting to conservative medium write risk until Tools/UnrealMcpToolRegistry/tools.json is updated.");
		return Policy;
	}

	TSharedPtr<FJsonObject> MakeToolPolicyObject(const FString& ToolName)
	{
		const FToolPolicy Policy = GetToolPolicy(ToolName);
		TSharedPtr<FJsonObject> PolicyObject = MakeShared<FJsonObject>();
		PolicyObject->SetStringField(TEXT("riskLevel"), LexToString(Policy.RiskLevel));
		PolicyObject->SetBoolField(TEXT("requiresWrite"), Policy.bRequiresWrite);
		PolicyObject->SetBoolField(TEXT("requiresBuild"), Policy.bRequiresBuild);
		PolicyObject->SetBoolField(TEXT("requiresExternalProcess"), Policy.bRequiresExternalProcess);
		PolicyObject->SetBoolField(TEXT("requiresRestart"), Policy.bRequiresRestart);
		PolicyObject->SetBoolField(TEXT("requiresProjectMemory"), Policy.bRequiresProjectMemory);
		PolicyObject->SetBoolField(TEXT("requiresLock"), Policy.bRequiresLock);
		PolicyObject->SetBoolField(TEXT("dryRunSupport"), Policy.bDryRunSupport);
		PolicyObject->SetBoolField(TEXT("preflightSupport"), Policy.bPreflightSupport);
		PolicyObject->SetBoolField(TEXT("postcheckSupport"), Policy.bPostcheckSupport);
		PolicyObject->SetStringField(TEXT("testCoverage"), Policy.TestCoverage);
		PolicyObject->SetStringField(TEXT("owner"), Policy.Owner);
		PolicyObject->SetStringField(TEXT("docsPath"), Policy.DocsPath);
		PolicyObject->SetStringField(TEXT("reason"), Policy.Reason);
		PolicyObject->SetBoolField(TEXT("explicitRegistryEntry"), HasExplicitToolRegistryEntry(ToolName));
		if (const FToolRegistryEntry* Entry = FindToolRegistryEntry(ToolName))
		{
			PolicyObject->SetBoolField(TEXT("descriptorBacked"), Entry->bLoadedFromDescriptor);
		}
		return PolicyObject;
	}

	TSharedPtr<FJsonObject> MakeToolRegistryValidationObject(const TArray<TSharedPtr<FJsonValue>>* VisibleToolsArray)
	{
		const TArray<FToolRegistryEntry>& Entries = GetToolRegistryEntries();
		TArray<TSharedPtr<FJsonValue>> Issues;
		TMap<FString, int32> NameCounts;
		TSet<FString> VisibleToolNames;
		TSet<FString> VisibleRegistryNames;
		TMap<FString, int32> CategoryCounts;
		TMap<FString, int32> TestCoverageCounts;
		int32 ExplicitEntryCount = 0;
		int32 DescriptorBackedCount = 0;
		int32 DescriptorOnlyCount = 0;
		int32 HiddenEntryCount = 0;
		int32 AliasEntryCount = 0;
		int32 DocsPathExistsCount = 0;

		if (VisibleToolsArray)
		{
			for (const TSharedPtr<FJsonValue>& ToolValue : *VisibleToolsArray)
			{
				if (!ToolValue.IsValid() || ToolValue->Type != EJson::Object || !ToolValue->AsObject().IsValid())
				{
					continue;
				}
				FString Name;
				if (ToolValue->AsObject()->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
				{
					VisibleToolNames.Add(Name);
				}
			}
		}

		for (const FToolRegistryEntry& Entry : Entries)
		{
			NameCounts.FindOrAdd(Entry.Name)++;
			CategoryCounts.FindOrAdd(Entry.Category)++;
			TestCoverageCounts.FindOrAdd(Entry.Policy.TestCoverage)++;
			ExplicitEntryCount += Entry.bLoadedFromExplicitRegistry ? 1 : 0;
			DescriptorBackedCount += Entry.bLoadedFromDescriptor ? 1 : 0;
			DescriptorOnlyCount += Entry.bLoadedFromDescriptor && !Entry.bLoadedFromExplicitRegistry ? 1 : 0;
			HiddenEntryCount += Entry.Exposure == EToolExposure::LegacyHidden ? 1 : 0;
			AliasEntryCount += (!Entry.HandlerName.IsEmpty() && !Entry.HandlerName.Equals(Entry.Name, ESearchCase::CaseSensitive)) ? 1 : 0;
			DocsPathExistsCount += RegistryDocsPathExists(Entry.Policy.DocsPath) ? 1 : 0;
			if (Entry.Exposure == EToolExposure::Visible)
			{
				VisibleRegistryNames.Add(Entry.Name);
			}

			if (Entry.Name.TrimStartAndEnd().IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("missing_name"), Entry.Name, TEXT("Registry entry name is empty."));
			}
			if (Entry.Category.TrimStartAndEnd().IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("missing_category"), Entry.Name, TEXT("Registry entry category is empty."));
			}
			else if (!IsKnownToolCategory(Entry.Category))
			{
				AddValidationIssue(Issues, TEXT("warning"), TEXT("unknown_category"), Entry.Name, FString::Printf(TEXT("Registry category '%s' is not in the reviewed category set."), *Entry.Category));
			}
			if (Entry.HandlerName.TrimStartAndEnd().IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("missing_handler_name"), Entry.Name, TEXT("Registry entry handlerName is empty."));
			}
			else
			{
				const FToolHandlerRegistryEntry* HandlerEntry = FindToolHandlerRegistryEntry(Entry.HandlerName);
				if (!HandlerEntry)
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("handler_not_registered"), Entry.Name, FString::Printf(TEXT("handlerName '%s' is missing from UnrealMcpToolHandlerRegistry."), *Entry.HandlerName));
				}
				else if (!HandlerEntry->Category.Equals(Entry.Category, ESearchCase::CaseSensitive))
				{
					AddValidationIssue(Issues, TEXT("warning"), TEXT("handler_category_mismatch"), Entry.Name, FString::Printf(TEXT("handlerName '%s' is registered under category '%s', but tool registry category is '%s'."), *Entry.HandlerName, *HandlerEntry->Category, *Entry.Category));
				}
			}
			if (Entry.Policy.Owner.TrimStartAndEnd().IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("missing_owner"), Entry.Name, TEXT("Registry entry owner is empty."));
			}
			if (Entry.Policy.DocsPath.TrimStartAndEnd().IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("missing_docs_path"), Entry.Name, TEXT("Registry entry docsPath is empty."));
			}
			else if (!RegistryDocsPathExists(Entry.Policy.DocsPath))
			{
				AddValidationIssue(Issues, TEXT("warning"), TEXT("docs_path_missing_file"), Entry.Name, FString::Printf(TEXT("docsPath file was not found: %s"), *Entry.Policy.DocsPath));
			}
			if (Entry.Policy.TestCoverage.TrimStartAndEnd().IsEmpty())
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("missing_test_coverage"), Entry.Name, TEXT("Registry entry testCoverage is empty."));
			}
			if (Entry.Policy.bDryRunSupport && !Entry.Policy.bRequiresWrite)
			{
				AddValidationIssue(Issues, TEXT("warning"), TEXT("dry_run_without_write"), Entry.Name, TEXT("dryRunSupport is true on a tool that is not marked requiresWrite."));
			}
			if ((Entry.Policy.bRequiresWrite || Entry.Policy.bRequiresBuild || Entry.Policy.bRequiresExternalProcess || Entry.Policy.bRequiresRestart)
				&& (!Entry.Policy.bPreflightSupport || !Entry.Policy.bPostcheckSupport))
			{
				AddValidationIssue(Issues, TEXT("warning"), TEXT("side_effect_without_execution_checks"), Entry.Name, TEXT("Side-effect tool should normally enable both preflightSupport and postcheckSupport."));
			}
		}

		for (const TPair<FString, int32>& Pair : NameCounts)
		{
			if (Pair.Value > 1)
			{
				AddValidationIssue(Issues, TEXT("error"), TEXT("duplicate_tool_name"), Pair.Key, FString::Printf(TEXT("Tool appears %d times in registry."), Pair.Value));
			}
		}

		if (VisibleToolsArray)
		{
			for (const FString& VisibleToolName : VisibleToolNames)
			{
				if (!FindToolRegistryEntry(VisibleToolName))
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("visible_tool_missing_registry"), VisibleToolName, TEXT("AI-visible tool definition has no explicit registry entry."));
				}
				const FToolRegistryEntry* Entry = FindToolRegistryEntry(VisibleToolName);
				if (Entry && Entry->Exposure == EToolExposure::LegacyHidden)
				{
					AddValidationIssue(Issues, TEXT("error"), TEXT("legacy_hidden_tool_visible"), VisibleToolName, TEXT("ToolRegistry marks this tool legacy_hidden but it appears in visible tools/list output."));
				}
			}

			for (const FString& VisibleRegistryName : VisibleRegistryNames)
			{
				if (!VisibleToolNames.Contains(VisibleRegistryName))
				{
					AddValidationIssue(Issues, TEXT("warning"), TEXT("visible_registry_missing_tool_definition"), VisibleRegistryName, TEXT("ToolRegistry marks this tool visible but tools/list did not include a definition."));
				}
			}
		}

		auto MapToJsonObject = [](const TMap<FString, int32>& Counts)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& Pair : Counts)
			{
				Object->SetNumberField(Pair.Key, Pair.Value);
			}
			return Object;
		};

		TSharedPtr<FJsonObject> ValidationObject = MakeShared<FJsonObject>();
		ValidationObject->SetBoolField(TEXT("complete"), Issues.Num() == 0);
		ValidationObject->SetStringField(TEXT("sourcePath"), GetToolRegistrySourcePath());
		ValidationObject->SetNumberField(TEXT("entryCount"), Entries.Num());
		ValidationObject->SetNumberField(TEXT("explicitEntryCount"), ExplicitEntryCount);
		ValidationObject->SetNumberField(TEXT("descriptorBackedCount"), DescriptorBackedCount);
		ValidationObject->SetNumberField(TEXT("descriptorOnlyCount"), DescriptorOnlyCount);
		ValidationObject->SetNumberField(TEXT("legacyHiddenCount"), HiddenEntryCount);
		ValidationObject->SetNumberField(TEXT("handlerAliasCount"), AliasEntryCount);
		ValidationObject->SetNumberField(TEXT("registeredHandlerCount"), GetToolHandlerRegistryEntries().Num());
		ValidationObject->SetNumberField(TEXT("docsPathExistsCount"), DocsPathExistsCount);
		ValidationObject->SetNumberField(TEXT("issueCount"), Issues.Num());
		ValidationObject->SetObjectField(TEXT("categoryCounts"), MapToJsonObject(CategoryCounts));
		ValidationObject->SetObjectField(TEXT("testCoverageCounts"), MapToJsonObject(TestCoverageCounts));
		ValidationObject->SetArrayField(TEXT("issues"), Issues);
		if (VisibleToolsArray)
		{
			ValidationObject->SetNumberField(TEXT("visibleToolDefinitionCount"), VisibleToolNames.Num());
			ValidationObject->SetNumberField(TEXT("visibleRegistryCount"), VisibleRegistryNames.Num());
		}
		return ValidationObject;
	}

	void AddToolRegistryStatus(const TSharedPtr<FJsonObject>& StructuredContent)
	{
		if (!StructuredContent.IsValid())
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> EntryValues;
		TArray<TSharedPtr<FJsonValue>> HiddenValues;
		TArray<TSharedPtr<FJsonValue>> AliasValues;
		int32 ExplicitEntryCount = 0;
		int32 DescriptorBackedCount = 0;
		int32 DescriptorOnlyCount = 0;

		for (const FToolRegistryEntry& Entry : GetToolRegistryEntries())
		{
			TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
			EntryObject->SetStringField(TEXT("name"), Entry.Name);
			EntryObject->SetStringField(TEXT("category"), Entry.Category);
			EntryObject->SetStringField(TEXT("handlerName"), Entry.HandlerName.IsEmpty() ? Entry.Name : Entry.HandlerName);
			EntryObject->SetStringField(TEXT("exposure"), Entry.Exposure == EToolExposure::Visible ? TEXT("visible") : TEXT("legacy_hidden"));
			EntryObject->SetStringField(TEXT("notes"), Entry.Notes);
			EntryObject->SetBoolField(TEXT("explicitRegistryEntry"), Entry.bLoadedFromExplicitRegistry);
			EntryObject->SetBoolField(TEXT("descriptorBacked"), Entry.bLoadedFromDescriptor);
			EntryObject->SetObjectField(TEXT("policy"), MakeToolPolicyObject(Entry.Name));

			TSharedPtr<FJsonValue> EntryValue = MakeShared<FJsonValueObject>(EntryObject);
			EntryValues.Add(EntryValue);

			ExplicitEntryCount += Entry.bLoadedFromExplicitRegistry ? 1 : 0;
			DescriptorBackedCount += Entry.bLoadedFromDescriptor ? 1 : 0;
			DescriptorOnlyCount += Entry.bLoadedFromDescriptor && !Entry.bLoadedFromExplicitRegistry ? 1 : 0;
			if (Entry.Exposure == EToolExposure::LegacyHidden)
			{
				HiddenValues.Add(EntryValue);
			}
			else if (!Entry.HandlerName.IsEmpty() && !Entry.HandlerName.Equals(Entry.Name, ESearchCase::CaseSensitive))
			{
				AliasValues.Add(EntryValue);
			}
		}

		StructuredContent->SetStringField(TEXT("toolRegistrySourcePath"), GetToolRegistrySourcePath());
		StructuredContent->SetNumberField(TEXT("toolRegistryEntryCount"), GetToolRegistryEntries().Num());
		StructuredContent->SetNumberField(TEXT("explicitToolRegistryEntryCount"), ExplicitEntryCount);
		StructuredContent->SetNumberField(TEXT("descriptorBackedToolCount"), DescriptorBackedCount);
		StructuredContent->SetNumberField(TEXT("descriptorOnlyToolCount"), DescriptorOnlyCount);
		StructuredContent->SetNumberField(TEXT("legacyHiddenToolCount"), HiddenValues.Num());
		StructuredContent->SetNumberField(TEXT("handlerAliasCount"), AliasValues.Num());
		StructuredContent->SetArrayField(TEXT("toolRegistryEntries"), EntryValues);
		StructuredContent->SetArrayField(TEXT("legacyHiddenTools"), HiddenValues);
		StructuredContent->SetArrayField(TEXT("handlerAliases"), AliasValues);
		StructuredContent->SetObjectField(TEXT("toolHandlerRegistry"), MakeToolHandlerRegistryStatusObject());
		StructuredContent->SetObjectField(TEXT("toolDescriptorRegistry"), MakeToolDescriptorStatusObject());
		StructuredContent->SetObjectField(TEXT("toolRegistryValidation"), MakeToolRegistryValidationObject());
	}
}
