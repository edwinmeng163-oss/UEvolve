#include "UnrealMcpToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UnrealMcp
{
	const TArray<FToolRegistryEntry>& GetToolRegistryEntries()
	{
		static const TArray<FToolRegistryEntry> Entries = {
			{
				TEXT("unreal.batch_set_actor_properties"),
				TEXT("actors"),
				TEXT("unreal.batch_set_actor_properties"),
				EToolExposure::LegacyHidden,
				TEXT("Legacy flexible property map uses additionalProperties=true; prefer fixed-schema batch actor tools.")
			},
			{
				TEXT("unreal.spawn_actor"),
				TEXT("actors"),
				TEXT("unreal.spawn_actor"),
				EToolExposure::LegacyHidden,
				TEXT("Legacy spawn tool supports freeform property overrides; prefer unreal.spawn_actor_basic or unreal.spawn_static_mesh_actor.")
			},
			{
				TEXT("unreal.spawn_actor_batch"),
				TEXT("actors"),
				TEXT("unreal.spawn_actor_batch"),
				EToolExposure::LegacyHidden,
				TEXT("Legacy batch spawn supports freeform item objects; prefer unreal.spawn_actor_batch_basic.")
			},
			{
				TEXT("unreal.spawn_actor_basic"),
				TEXT("actors"),
				TEXT("unreal.spawn_actor"),
				EToolExposure::Visible,
				TEXT("AI-facing fixed-schema wrapper routed to the legacy spawn handler.")
			},
			{
				TEXT("unreal.spawn_actor_batch_basic"),
				TEXT("actors"),
				TEXT("unreal.spawn_actor_batch"),
				EToolExposure::Visible,
				TEXT("AI-facing fixed-schema wrapper routed to the legacy batch spawn handler.")
			},
			{
				TEXT("unreal.mcp_workbench_status"),
				TEXT("mcp-workbench"),
				TEXT("unreal.mcp_workbench_status"),
				EToolExposure::Visible,
				TEXT("Read-only self-extension workbench health summary.")
			}
		};
		return Entries;
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
		FToolPolicy Policy;
		Policy.Reason = TEXT("Default low-risk MCP tool.");

		if (ToolName.StartsWith(TEXT("unreal.list_"))
			|| ToolName == TEXT("unreal.editor_status")
			|| ToolName == TEXT("unreal.tail_log")
			|| ToolName == TEXT("unreal.map_check")
			|| ToolName == TEXT("unreal.mcp_validate_tool_schema")
			|| ToolName == TEXT("unreal.mcp_tool_audit")
			|| ToolName == TEXT("unreal.mcp_workbench_status")
			|| ToolName == TEXT("unreal.mcp_pipeline_status")
			|| ToolName == TEXT("unreal.mcp_diff_last_apply")
			|| ToolName == TEXT("unreal.mcp_list_scaffolds")
			|| ToolName == TEXT("unreal.mcp_inspect_scaffold")
			|| ToolName == TEXT("unreal.mcp_validate_cpp_snippet")
			|| ToolName == TEXT("unreal.project_memory_read")
			|| ToolName == TEXT("unreal.project_memory_view")
			|| ToolName == TEXT("unreal.skill_list")
			|| ToolName == TEXT("unreal.skill_read")
			|| ToolName == TEXT("unreal.skill_activity_status"))
		{
			Policy.RiskLevel = EToolRiskLevel::ReadOnly;
			Policy.Reason = TEXT("Read-only inspection, audit, status, memory read, or skill read tool.");
			return Policy;
		}

		if (ToolName == TEXT("unreal.execute_console_command")
			|| ToolName == TEXT("unreal.execute_python")
			|| ToolName == TEXT("unreal.execute_python_file"))
		{
			Policy.RiskLevel = EToolRiskLevel::Critical;
			Policy.bRequiresWrite = true;
			Policy.bRequiresExternalProcess = ToolName == TEXT("unreal.execute_python_file");
			Policy.Reason = TEXT("Dynamic code or console execution can mutate editor/project state.");
			return Policy;
		}

		if (ToolName == TEXT("unreal.clear_level_environment"))
		{
			Policy.RiskLevel = EToolRiskLevel::High;
			Policy.bRequiresWrite = true;
			Policy.Reason = TEXT("Destructively clears level actors from the current editor world.");
			return Policy;
		}

		if (ToolName == TEXT("unreal.mcp_build_editor"))
		{
			Policy.RiskLevel = EToolRiskLevel::High;
			Policy.bRequiresBuild = true;
			Policy.bRequiresExternalProcess = true;
			Policy.bRequiresProjectMemory = true;
			Policy.bRequiresLock = true;
			Policy.Reason = TEXT("Runs Unreal Build Tool and writes build handoff state.");
			return Policy;
		}

		if (ToolName == TEXT("unreal.mcp_extension_pipeline"))
		{
			Policy.RiskLevel = EToolRiskLevel::Critical;
			Policy.bRequiresWrite = true;
			Policy.bRequiresBuild = true;
			Policy.bRequiresExternalProcess = true;
			Policy.bRequiresRestart = true;
			Policy.bRequiresProjectMemory = true;
			Policy.bRequiresLock = true;
			Policy.Reason = TEXT("Orchestrates self-extension source changes, build, restart handoff, and tests.");
			return Policy;
		}

		if (ToolName == TEXT("unreal.mcp_apply_scaffold")
			|| ToolName == TEXT("unreal.mcp_patch_scaffold_snippet")
			|| ToolName == TEXT("unreal.mcp_rollback_last_extension")
			|| ToolName == TEXT("unreal.mcp_rollback_to_manifest")
			|| ToolName == TEXT("unreal.mcp_backup_project_state")
			|| ToolName == TEXT("unreal.mcp_clean_test_artifacts")
			|| ToolName == TEXT("unreal.mcp_supervisor_install"))
		{
			Policy.RiskLevel = EToolRiskLevel::High;
			Policy.bRequiresWrite = true;
			Policy.bRequiresProjectMemory = ToolName.Contains(TEXT("rollback")) || ToolName.Contains(TEXT("backup"));
			Policy.bRequiresExternalProcess = ToolName == TEXT("unreal.mcp_supervisor_install");
			Policy.bRequiresLock = true;
			Policy.Reason = TEXT("Writes source, snippets, backups, generated artifacts, supervisor launchers, or rollback state.");
			return Policy;
		}

		if (ToolName == TEXT("unreal.skill_promote_draft"))
		{
			Policy.RiskLevel = EToolRiskLevel::High;
			Policy.bRequiresWrite = true;
			Policy.bRequiresLock = true;
			Policy.Reason = TEXT("Promotes reviewed skill drafts into versioned Tools/UnrealMcpSkills and may overwrite team-shared skill files.");
			return Policy;
		}

		if (ToolName == TEXT("unreal.mcp_generate_tests")
			|| ToolName == TEXT("unreal.mcp_run_tool_test")
			|| ToolName == TEXT("unreal.mcp_run_test_suite")
			|| ToolName == TEXT("unreal.project_memory_write")
			|| ToolName == TEXT("unreal.project_memory_edit")
			|| ToolName == TEXT("unreal.project_memory_delete")
			|| ToolName == TEXT("unreal.skill_apply")
			|| ToolName == TEXT("unreal.skill_recording_start")
			|| ToolName == TEXT("unreal.skill_recording_stop")
			|| ToolName == TEXT("unreal.skill_distill_from_activity")
			|| ToolName == TEXT("unreal.skill_save_draft")
			|| ToolName == TEXT("unreal.scaffold_mcp_tool"))
		{
			Policy.RiskLevel = EToolRiskLevel::Medium;
			Policy.bRequiresWrite = true;
			Policy.bRequiresProjectMemory = ToolName.Contains(TEXT("project_memory")) || ToolName == TEXT("unreal.skill_apply") || ToolName.Contains(TEXT("run_"));
			Policy.bRequiresLock = ToolName.StartsWith(TEXT("unreal.mcp_"));
			Policy.Reason = TEXT("Writes generated tests, scaffold files, project memory, or test result state.");
			return Policy;
		}

		if (ToolName == TEXT("unreal.start_pie")
			|| ToolName == TEXT("unreal.stop_pie")
			|| ToolName == TEXT("unreal.open_map")
			|| ToolName == TEXT("unreal.open_asset")
			|| ToolName == TEXT("unreal.sync_content_browser"))
		{
			Policy.RiskLevel = EToolRiskLevel::Low;
			Policy.Reason = TEXT("Changes editor session state without directly writing source or assets.");
			return Policy;
		}

		if (ToolName.Contains(TEXT("spawn"))
			|| ToolName.Contains(TEXT("set_actor"))
			|| ToolName.Contains(TEXT("layout_"))
			|| ToolName.Contains(TEXT("destroy_"))
			|| ToolName.Contains(TEXT("compile_"))
			|| ToolName.Contains(TEXT("create_blueprint"))
			|| ToolName.Contains(TEXT("bp_"))
			|| ToolName.Contains(TEXT("widget_"))
			|| ToolName.Contains(TEXT("scaffold_"))
			|| ToolName == TEXT("unreal.save_dirty_packages")
			|| ToolName == TEXT("unreal.select_actors"))
		{
			Policy.RiskLevel = EToolRiskLevel::Medium;
			Policy.bRequiresWrite = !ToolName.Contains(TEXT("compile_")) && ToolName != TEXT("unreal.select_actors");
			Policy.Reason = TEXT("Edits or compiles Unreal editor assets, actors, Blueprint graphs, widgets, or generated gameplay scaffolds.");
			return Policy;
		}

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
		PolicyObject->SetStringField(TEXT("reason"), Policy.Reason);
		return PolicyObject;
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

		for (const FToolRegistryEntry& Entry : GetToolRegistryEntries())
		{
			TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
			EntryObject->SetStringField(TEXT("name"), Entry.Name);
			EntryObject->SetStringField(TEXT("category"), Entry.Category);
			EntryObject->SetStringField(TEXT("handlerName"), Entry.HandlerName.IsEmpty() ? Entry.Name : Entry.HandlerName);
			EntryObject->SetStringField(TEXT("exposure"), Entry.Exposure == EToolExposure::Visible ? TEXT("visible") : TEXT("legacy_hidden"));
			EntryObject->SetStringField(TEXT("notes"), Entry.Notes);
			EntryObject->SetObjectField(TEXT("policy"), MakeToolPolicyObject(Entry.Name));

			TSharedPtr<FJsonValue> EntryValue = MakeShared<FJsonValueObject>(EntryObject);
			EntryValues.Add(EntryValue);

			if (Entry.Exposure == EToolExposure::LegacyHidden)
			{
				HiddenValues.Add(EntryValue);
			}
			else if (!Entry.HandlerName.IsEmpty() && !Entry.HandlerName.Equals(Entry.Name, ESearchCase::CaseSensitive))
			{
				AliasValues.Add(EntryValue);
			}
		}

		StructuredContent->SetNumberField(TEXT("toolRegistryEntryCount"), GetToolRegistryEntries().Num());
		StructuredContent->SetNumberField(TEXT("legacyHiddenToolCount"), HiddenValues.Num());
		StructuredContent->SetNumberField(TEXT("handlerAliasCount"), AliasValues.Num());
		StructuredContent->SetArrayField(TEXT("toolRegistryEntries"), EntryValues);
		StructuredContent->SetArrayField(TEXT("legacyHiddenTools"), HiddenValues);
		StructuredContent->SetArrayField(TEXT("handlerAliases"), AliasValues);
	}
}
