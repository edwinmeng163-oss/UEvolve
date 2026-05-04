#include "UnrealMcpToolHandlerRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UnrealMcp
{
	namespace
	{
		FToolHandlerRegistryEntry MakeHandlerEntry(const TCHAR* HandlerName, const TCHAR* Category, const TCHAR* SourceFile = TEXT("UnrealMcpModule.cpp"))
		{
			FToolHandlerRegistryEntry Entry;
			Entry.HandlerName = HandlerName;
			Entry.Category = Category;
			Entry.SourceFile = SourceFile;
			return Entry;
		}
	}

	const TArray<FToolHandlerRegistryEntry>& GetToolHandlerRegistryEntries()
	{
		static const TArray<FToolHandlerRegistryEntry> Entries = {
			MakeHandlerEntry(TEXT("unreal.batch_configure_static_mesh_actors"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.batch_set_actor_properties"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.batch_set_actor_scale"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.batch_set_actor_tags"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.batch_set_point_light_properties"), TEXT("editor")),
			MakeHandlerEntry(TEXT("unreal.bp_add_branch_node"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_add_call_function_node"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_add_event_node"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_add_for_each_node"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_add_function"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_add_variable"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_arrange_graph"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_compile_save"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_connect_pins"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.bp_set_pin_default"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.clear_level_environment"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.compile_blueprint"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.compile_blueprints_in_path"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.create_blueprint_class"), TEXT("blueprint")),
			MakeHandlerEntry(TEXT("unreal.destroy_selected_actors"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.editor_status"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.execute_console_command"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.execute_python"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.execute_python_file"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.layout_actors_circle"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.layout_actors_grid"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.list_assets"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.list_level_actors"), TEXT("actors"), TEXT("UnrealMcpActorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.list_maps"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.list_selected_actors"), TEXT("actors"), TEXT("UnrealMcpActorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.list_selected_assets"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.map_check"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.mcp_apply_scaffold"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_backup_project_state"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_build_editor"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_clean_test_artifacts"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_compile_error_fix_plan"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_diff_last_apply"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_extension_pipeline"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_generate_tests"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_inspect_scaffold"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_list_scaffolds"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_lock_extension_session"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_patch_scaffold_snippet"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_pipeline_status"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_rollback_last_extension"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_rollback_to_manifest"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_run_test_suite"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_run_tool_test"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_supervisor_install"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_tool_audit"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_validate_cpp_snippet"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_validate_tool_schema"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.mcp_workbench_status"), TEXT("self-extension")),
			MakeHandlerEntry(TEXT("unreal.open_asset"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.open_map"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.project_memory_delete"), TEXT("memory")),
			MakeHandlerEntry(TEXT("unreal.project_memory_edit"), TEXT("memory")),
			MakeHandlerEntry(TEXT("unreal.project_memory_read"), TEXT("memory")),
			MakeHandlerEntry(TEXT("unreal.project_memory_view"), TEXT("memory")),
			MakeHandlerEntry(TEXT("unreal.project_memory_write"), TEXT("memory")),
			MakeHandlerEntry(TEXT("unreal.save_dirty_packages"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.scaffold_autobattler_ai"), TEXT("scaffold")),
			MakeHandlerEntry(TEXT("unreal.scaffold_economy_system"), TEXT("scaffold")),
			MakeHandlerEntry(TEXT("unreal.scaffold_mcp_tool"), TEXT("scaffold")),
			MakeHandlerEntry(TEXT("unreal.scaffold_result_ui"), TEXT("scaffold")),
			MakeHandlerEntry(TEXT("unreal.scaffold_round_system"), TEXT("scaffold")),
			MakeHandlerEntry(TEXT("unreal.scaffold_shop_system"), TEXT("scaffold")),
			MakeHandlerEntry(TEXT("unreal.select_actors"), TEXT("actors"), TEXT("UnrealMcpActorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.set_actor_transform"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.skill_activity_status"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.skill_apply"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.skill_distill_from_activity"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.skill_list"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.skill_promote_draft"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.skill_read"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.skill_recording_start"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.skill_recording_stop"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.skill_save_draft"), TEXT("skills")),
			MakeHandlerEntry(TEXT("unreal.spawn_actor"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.spawn_actor_batch"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.spawn_static_mesh_actor"), TEXT("actors")),
			MakeHandlerEntry(TEXT("unreal.start_pie"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.stop_pie"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.sync_content_browser"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.tail_log"), TEXT("editor"), TEXT("UnrealMcpEditorTools.cpp")),
			MakeHandlerEntry(TEXT("unreal.widget_add"), TEXT("widget")),
			MakeHandlerEntry(TEXT("unreal.widget_bind_blueprint_variable"), TEXT("widget")),
			MakeHandlerEntry(TEXT("unreal.widget_bind_event"), TEXT("widget")),
			MakeHandlerEntry(TEXT("unreal.widget_build_template"), TEXT("widget")),
			MakeHandlerEntry(TEXT("unreal.widget_remove"), TEXT("widget")),
			MakeHandlerEntry(TEXT("unreal.widget_set_property"), TEXT("widget")),
			MakeHandlerEntry(TEXT("unreal.widget_set_slot_layout"), TEXT("widget")),
		};
		return Entries;
	}

	const FToolHandlerRegistryEntry* FindToolHandlerRegistryEntry(const FString& HandlerName)
	{
		for (const FToolHandlerRegistryEntry& Entry : GetToolHandlerRegistryEntries())
		{
			if (Entry.HandlerName.Equals(HandlerName, ESearchCase::CaseSensitive))
			{
				return &Entry;
			}
		}
		return nullptr;
	}

	bool IsRegisteredToolHandler(const FString& HandlerName)
	{
		return FindToolHandlerRegistryEntry(HandlerName) != nullptr;
	}

	TSharedPtr<FJsonObject> MakeToolHandlerRegistryStatusObject()
	{
		TArray<TSharedPtr<FJsonValue>> HandlerValues;
		TMap<FString, int32> CategoryCounts;
		for (const FToolHandlerRegistryEntry& Entry : GetToolHandlerRegistryEntries())
		{
			TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
			EntryObject->SetStringField(TEXT("handlerName"), Entry.HandlerName);
			EntryObject->SetStringField(TEXT("category"), Entry.Category);
			EntryObject->SetStringField(TEXT("sourceFile"), Entry.SourceFile);
			HandlerValues.Add(MakeShared<FJsonValueObject>(EntryObject));
			CategoryCounts.FindOrAdd(Entry.Category)++;
		}

		TSharedPtr<FJsonObject> CategoryCountsObject = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : CategoryCounts)
		{
			CategoryCountsObject->SetNumberField(Pair.Key, Pair.Value);
		}

		TSharedPtr<FJsonObject> StatusObject = MakeShared<FJsonObject>();
		StatusObject->SetNumberField(TEXT("handlerCount"), GetToolHandlerRegistryEntries().Num());
		StatusObject->SetObjectField(TEXT("categoryCounts"), CategoryCountsObject);
		StatusObject->SetArrayField(TEXT("handlers"), HandlerValues);
		return StatusObject;
	}
}
