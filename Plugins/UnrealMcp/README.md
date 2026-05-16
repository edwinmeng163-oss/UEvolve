# Unreal MCP

This plugin runs a local Model Context Protocol server inside Unreal Editor and adds an in-editor command and AI chat window.

## What It Exposes

Read-only and context tools:

- `unreal.editor_status`
- `unreal.tail_log`
- `unreal.map_check`
- `unreal.list_maps`
- `unreal.list_assets`
- `unreal.list_selected_assets`
- `unreal.list_level_actors`
- `unreal.list_selected_actors`
- `unreal.actor_get_property`
- `unreal.actor_get_transform`
- `unreal.project_settings_get`

Editor action tools:

- `unreal.start_pie`
- `unreal.stop_pie`
- `unreal.execute_console_command`
- `unreal.execute_python`
- `unreal.execute_python_file`
- `unreal.select_actors`
- `unreal.set_actor_transform`
- `unreal.batch_set_actor_scale`
- `unreal.batch_set_actor_tags`
- `unreal.batch_set_point_light_properties`
- `unreal.batch_configure_static_mesh_actors`
- `unreal.layout_actors_grid`
- `unreal.layout_actors_circle`
- `unreal.open_map`
- `unreal.open_asset`
- `unreal.sync_content_browser`
- `unreal.asset_move`
- `unreal.redirector_fixup`
- `unreal.dependency_remap`
- `unreal.project_version_migration`
- `unreal.spawn_actor_basic`
- `unreal.spawn_actor_batch_basic`
- `unreal.spawn_static_mesh_actor`
- `unreal.destroy_selected_actors`
- `unreal.clear_level_environment`
- `unreal.compile_blueprint`
- `unreal.compile_blueprints_in_path`
- `unreal.create_blueprint_class`
- `unreal.bp_add_variable`
- `unreal.bp_add_function`
- `unreal.bp_add_macro_graph`
- `unreal.bp_delete_macro_graph`
- `unreal.bp_interface_add`
- `unreal.bp_interface_remove`
- `unreal.bp_add_event_node`
- `unreal.bp_add_call_function_node`
- `unreal.bp_add_branch_node`
- `unreal.bp_add_for_each_node`
- `unreal.bp_delete_node`
- `unreal.bp_delete_variable`
- `unreal.bp_delete_function`
- `unreal.bp_rename_variable`
- `unreal.bp_rename_function`
- `unreal.bp_connect_pins`
- `unreal.bp_set_pin_default`
- `unreal.bp_arrange_graph`
- `unreal.bp_compile_save`
- `unreal.widget_add`
- `unreal.widget_remove`
- `unreal.widget_set_property`
- `unreal.widget_set_slot_layout`
- `unreal.widget_bind_event`
- `unreal.widget_bind_blueprint_variable`
- `unreal.widget_build_template`
- `unreal.scaffold_round_system`
- `unreal.scaffold_shop_system`
- `unreal.scaffold_economy_system`
- `unreal.scaffold_autobattler_ai`
- `unreal.scaffold_result_ui`
- `unreal.scaffold_recipe`
- `unreal.workflow_run`
- `unreal.scaffold_mcp_tool`
- `unreal.mcp_list_scaffolds`
- `unreal.mcp_inspect_scaffold`
- `unreal.mcp_validate_cpp_patch`
- `unreal.mcp_patch_scaffold_patch`
- `unreal.mcp_validate_tool_schema`
- `unreal.mcp_apply_scaffold`
- `unreal.mcp_rollback_last_extension`
- `unreal.mcp_lock_extension_session`
- `unreal.mcp_backup_project_state`
- `unreal.mcp_rollback_to_manifest`
- `unreal.mcp_compile_error_fix_plan`
- `unreal.mcp_supervisor_install`
- `unreal.mcp_generate_tests`
- `unreal.mcp_build_editor`
- `unreal.mcp_build_game`
- `unreal.mcp_build_server`
- `unreal.mcp_build_client`
- `unreal.mcp_build_packaged`
- `unreal.mcp_run_tool_test`
- `unreal.mcp_run_test_suite`
- `unreal.mcp_extension_pipeline`
- `unreal.mcp_pipeline_status`
- `unreal.mcp_workbench_status`
- `unreal.mcp_diff_last_apply`
- `unreal.mcp_clean_test_artifacts`
- `unreal.mcp_tool_audit`
- `unreal.tools.export_package`
- `unreal.tools.import_package`
- `unreal.knowledge_index_refresh`
- `unreal.knowledge_search`
- `unreal.tool_recommend`
- `unreal.tool_gap_analyze`
- `unreal.workflow_recommend`
- `unreal.knowledge_eval_run`
- `unreal.project_memory_write`
- `unreal.project_memory_read`
- `unreal.project_memory_view`
- `unreal.project_memory_edit`
- `unreal.project_memory_delete`
- `unreal.skill_list`
- `unreal.skill_read`
- `unreal.skill_apply`
- `unreal.skill_recording_start`
- `unreal.skill_recording_stop`
- `unreal.skill_activity_status`
- `unreal.skill_distill_from_activity`
- `unreal.skill_save_draft`
- `unreal.skill_promote_draft`
- `unreal.save_dirty_packages`

Legacy compatibility handlers:

- `unreal.batch_set_actor_properties`
- `unreal.spawn_actor`
- `unreal.spawn_actor_batch`

These tools intentionally use flexible object schemas for compatibility with older direct callers. They are hidden from AI-facing `tools/list` by the ToolRegistry because OpenAI function calling rejects `additionalProperties=true`. Prefer `unreal.spawn_actor_basic`, `unreal.spawn_actor_batch_basic`, `unreal.spawn_static_mesh_actor`, and the fixed batch actor tools.

## Endpoint

By default the plugin listens on:

`http://127.0.0.1:8765/mcp`

You can change the port, path, allowed origins, and optional auth token in:

`Project Settings > Plugins > Unreal MCP`

## Install Into An Existing Project

UEvolve is usually installed as a project-level plugin, not an engine-level plugin. From a UEvolve checkout, install into a target project with:

```bash
python3 Tools/install_unrealmcp_to_project.py --project "/path/to/YourProject/YourProject.uproject"
```

On Windows:

```powershell
py -3 Tools\install_unrealmcp_to_project.py `
  --project "E:\UE5_P\EasyMapper5_7\EasyMapper5_7.uproject"
```

The helper copies the reusable plugin plus the project-root workflow folders:

```text
<YourProject>/Plugins/UnrealMcp
<YourProject>/Tools
<YourProject>/Schemas
<YourProject>/Docs
```

It also enables both `UnrealMcp` and `PythonScriptPlugin` in the target `.uproject` after creating a backup. Use `--dry-run` first to preview the copy/edit plan.

If you install manually, copy those same folders and confirm the target `.uproject` enables:

```json
{
  "Plugins": [
    {
      "Name": "PythonScriptPlugin",
      "Enabled": true,
      "TargetAllowList": ["Editor"]
    },
    {
      "Name": "UnrealMcp",
      "Enabled": true,
      "TargetAllowList": ["Editor"]
    }
  ]
}
```

Do not keep another copy under `Engine/Plugins/Marketplace/UnrealMcp` while using a project-level install. Duplicate plugin copies can load stale binaries or make Windows cleanup fail because `UnrealEditor-UnrealMcp.dll` is still locked by a running editor.

After copying, close Unreal Editor and build the target project. For example:

```powershell
& "E:\3D_SOFTWARE\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  UnrealEditor Win64 Development `
  "-Project=E:\UE5_P\EasyMapper5_7\EasyMapper5_7.uproject" `
  -WaitMutex
```

The MCP endpoint only responds after Unreal Editor opens that target project and successfully loads this plugin. It is not a standalone background service.

## Drop-in install limitations

A source-only drop-in under `<YourProject>/Plugins/UnrealMcp/` can run editor MCP tools, but it does not provision project-root `Tools/` content.
`unreal.tools.import_package` requires a writable `<YourProject>/Tools/UnrealMcpToolRegistry/tools.json`; copy the plugin `Resources/ToolRegistry/tools.json` there before importing reviewed tool packages.
Workbench `Run Core Tests` and `Run RAG Evals` are disabled until `Tools/UnrealMcpTests/Core/` and `Tools/UnrealMcpKnowledge/Evals/` exist with JSON cases.
Use the full install workflow above when you need package import, local test suites, RAG evals, shared skills, docs, and schemas.

## AI Assistant

The chat panel can also call an LLM through the OpenAI Responses API and let it drive the same Unreal MCP tools that the HTTP server exposes.

Configure these fields in:

`Project Settings > Plugins > Unreal MCP > AI`

- `Enable AI Assistant`
- `OpenAI Responses URL`
- `OpenAI API Key`
- `OpenAI Model`
- `OpenAI Reasoning Effort`
- `AI Max Tool Rounds`
- `AI Max Output Tokens`
- `AI Request Timeout Seconds`
- `AI Request Activity Timeout Seconds`
- `Assistant System Prompt`

Behavior notes:

- The chat toolbar has an `AI Settings` button that opens the Unreal MCP project settings page for local API key/model configuration.
- `Test AI` sends a minimal non-streaming Responses API request with the configured endpoint, model, and API key, then renders the HTTP status and response preview as a tool card.
- The chat toolbar has `Refresh Skills`, `Read Skill`, and `Apply Skill` buttons backed by `unreal.skill_list`, `unreal.skill_read`, and `unreal.skill_apply`, so users can select project-local skills without typing raw `/tool` JSON.
- The skill picker shows project skill names with title/description metadata from `SKILL.md` or `*.skill` files.
- `Apply Skill` supports `Read Only`, `Apply to Memory`, `Insert Prompt`, and `Ask Now` modes.
- `Read Memory` and `Write Task Memory` provide quick access to `unreal.project_memory_view` and `unreal.project_memory_write` for Chat handoff state.
- Plain text in the chat panel is treated as an AI request.
- `/ask <prompt>` explicitly starts an AI turn.
- `/reset_ai` clears the assistant's response chain so the next ask starts a fresh conversation.
- `/stop_ai` or the `Stop` button cancels the current generation.
- The assistant streams text deltas into the panel, renders tool calls as cards, sends tool results back to the model, and then posts the final answer.
- If the model hits `max_output_tokens`, Unreal MCP automatically attempts a short continuation instead of immediately failing the turn.
- Long AI turns now use configurable request and activity timeouts so tool-heavy or planning-heavy requests are less likely to fail at the transport layer.
- Unreal MCP also synchronizes Unreal's global HTTP connection/activity timeouts for AI turns so macOS does not fall back to the engine's default 30-second connection timeout.
- The default AI tool-round budget is 16 so scaffold-style requests have more room to inspect, create assets, and then summarize.
- If an AI turn reaches the tool-round budget, the chat now pauses instead of hard-failing: it writes `chat.active_task`, gives a concrete next step, and avoids carrying forward a half-finished response chain.
- The conversation history is persisted at `Saved/UnrealMcp/ChatHistory.json`.
- High-level activity recording is opt-in and starts only after `unreal.skill_recording_start`; while active, it writes local JSONL events under `Saved/UnrealMcp/ActivityLog/*.jsonl`. It records mutating MCP tool call/result metadata and a periodic editor heartbeat roughly once per minute, skips read-only/status tools, and does not store tool result text previews.
- On the first AI turn after the panel reloads, Unreal MCP now also replays a small, compact slice of the persisted local transcript back to the model for continuity, so saved history is not just UI-only.
- `Copy Chat` copies the full visible transcript, and `Copy Log` copies the most recent `/log` or `unreal.tail_log` output.
- For AI-driven editing, prefer the fixed-schema wrapper tools such as `unreal.spawn_actor_basic`, `unreal.spawn_static_mesh_actor`, `unreal.batch_set_actor_scale`, `unreal.batch_set_actor_tags`, `unreal.batch_set_point_light_properties`, `unreal.batch_configure_static_mesh_actors`, the `unreal.bp_*` Blueprint graph editing tools, the `unreal.widget_*` UMG editing tools, and the `unreal.scaffold_*` gameplay scaffold tools.
- Tool names are internally normalized for model compatibility, but the underlying Unreal MCP tools and behavior stay the same.

## In-Editor Chat

Open the command chat window from:

`Window > Unreal MCP Chat`

Open the thin self-extension console from:

`Window > Unreal MCP Workbench`

The chat panel supports both direct slash commands and AI-assisted requests, and uses the same tool execution layer as the HTTP MCP server.

The plugin now depends on Unreal's `Python Script Plugin`, and the project enables it automatically for editor sessions so MCP can run Python automation in-process.

Useful commands:

- `/help`
- `/ask open the TwinStick map and list the main actors`
- `/reset_ai`
- `/stop_ai`
- `/status`
- `/pie`
- `/pie simulate`
- `/stop_pie`
- `/console stat fps`
- `/log 80`
- `/log Error`
- `/map_check`
- `/maps`
- `/assets /Game/Variant_TwinStick`
- `/selected assets`
- `/selected actors`
- `/select PlayerStart`
- `/move_selected 0 0 300`
- `/set_props {"selectedOnly":true,"properties":{"Tags":["Encounter"],"RootComponent.RelativeScale3D":{"X":1.25,"Y":1.25,"Z":1.25}}}`
- `/layout_selected 400 300 5`
- `/layout_circle 1200 0 360`
- `/actors Enemy`
- `/open_map /Game/TopDown/Maps/Lvl_TopDown`
- `/open_asset /Game/Variant_TwinStick/Blueprints/BP_TwinStickCharacter`
- `/browse /Game/Variant_TwinStick`
- `/spawn /Script/Engine.PointLight 0 0 150 ChatLight`
- `/spawn_batch {"classPath":"/Script/Engine.PointLight","items":[{"x":0,"y":0,"z":150,"label":"Light_A"},{"x":300,"y":0,"z":150,"label":"Light_B"}]}`
- `/py import unreal; print(unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world().get_name())`
- `/py_eval unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world().get_name()`
- `/py_file Tools/mcp_test_script.py`
- `/compile_bp /Game/Blueprints/BP_Test`
- `/compile_bps /Game/TopDown`
- `/new_bp /Game/Blueprints/BP_Test /Script/Engine.Actor`
- `/save`

You can also type natural language without a slash prefix, for example:

```text
build the current map, check for errors, and summarize what changed
```

```text
open the TwinStick map, list the enemies in the level, and arrange the selected lights in a circle
```

For direct tool calls from inside the chat window:

```text
/tool unreal.spawn_actor_basic {"classPath":"/Script/Engine.PointLight","x":0,"y":0,"z":150,"label":"ChatLight"}
```

```text
/tool unreal.batch_set_actor_tags {"selectedOnly":true,"tags":["Encounter","Wave1"],"replaceExisting":false}
```

```text
/tool unreal.execute_python {"command":"import unreal\nprint(unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world().get_name())"}
```

`unreal.execute_python` defaults to `mode:"Auto"`: multiline scripts and obvious statements run as `ExecuteFile`, while simple single-line expressions run as `EvaluateStatement`.

```text
/tool unreal.execute_python_file {"scriptPath":"Tools/mcp_test_script.py","args":["--mode","quick"]}
```

Blueprint graph editing example:

```text
/tool unreal.bp_add_event_node {"blueprintPath":"/Game/Blueprints/BP_Test","graphName":"EventGraph","eventName":"ReceiveBeginPlay","ownerClassPath":"/Script/Engine.Actor","x":0,"y":0}
```

```text
/tool unreal.bp_add_call_function_node {"blueprintPath":"/Game/Blueprints/BP_Test","graphName":"EventGraph","functionClassPath":"/Script/Engine.KismetSystemLibrary","functionName":"PrintString","x":280,"y":0}
```

Use the returned `nodeGuid` values with `unreal.bp_connect_pins`, then call `unreal.bp_compile_save`.

UMG Widget Blueprint editing example:

```text
/tool unreal.widget_build_template {"widgetBlueprintPath":"/Game/MCPDemo/Blueprints/UI/WBP_Demo_HUD","templateName":"mcp_demo_hud","title":"MCP Demo","replaceRoot":true,"compile":false,"savePackage":false}
```

Then call `unreal.bp_compile_save` for the Widget Blueprint when the hierarchy edits are done.

```text
/tool unreal.widget_add {"widgetBlueprintPath":"/Game/MCPDemo/Blueprints/UI/WBP_Demo_HUD","parentWidgetName":"RootCanvas","widgetName":"DebugText","widgetClass":"TextBlock","isVariable":true}
```

```text
/tool unreal.widget_set_property {"widgetBlueprintPath":"/Game/MCPDemo/Blueprints/UI/WBP_Demo_HUD","widgetName":"DebugText","propertyName":"Text","value":"Round 1: Preparation"}
```

```text
/tool unreal.widget_set_slot_layout {"widgetBlueprintPath":"/Game/MCPDemo/Blueprints/UI/WBP_Demo_HUD","widgetName":"DebugText","x":32,"y":92,"width":420,"height":40}
```

```text
/tool unreal.widget_bind_event {"widgetBlueprintPath":"/Game/MCPDemo/Blueprints/UI/WBP_Demo_HUD","widgetName":"RefreshButton","eventName":"OnClicked","compile":true}
```

Playable system scaffold examples:

High-level bounded recipe examples:

```text
/tool unreal.scaffold_recipe {"recipeName":"first_person_ground_character","rootPath":"/Game/MCPDemo","writeMemory":true}
```

```text
/tool unreal.scaffold_recipe {"recipeName":"widget_hud","rootPath":"/Game/MCPDemo","writeMemory":true}
```

```text
/tool unreal.scaffold_recipe {"recipeName":"mcp_self_extension_pipeline","writeMemory":true}
```

```text
/tool unreal.scaffold_recipe {"recipeName":"rts_camera","rootPath":"/Game/MCPDemo","writeMemory":true}
```

```text
/tool unreal.scaffold_recipe {"recipeName":"top_down_input","rootPath":"/Game/MCPDemo","writeMemory":true}
```

```text
/tool unreal.scaffold_recipe {"recipeName":"hud_dashboard","rootPath":"/Game/MCPDemo","writeMemory":true}
```

```text
/tool unreal.scaffold_recipe {"recipeName":"asset_naming_audit","rootPath":"/Game/MCPDemo","writeMemory":true}
```

Generic workflow composition example:

```text
/tool unreal.workflow_run {"workflowName":"readonly-check","dryRun":true,"writeMemory":true,"steps":[{"name":"status","tool":"unreal.editor_status","argumentsJson":"{}"},{"name":"workbench","tool":"unreal.mcp_workbench_status","argumentsJson":"{\"includeBuildLogTail\":false}"}]}
```

Legacy direct gameplay scaffold examples:

```text
/tool unreal.scaffold_round_system {"rootPath":"/Game/MCPDemo","compile":true,"savePackage":true}
```

```text
/tool unreal.scaffold_shop_system {"rootPath":"/Game/MCPDemo","compile":true,"savePackage":true,"replaceWidgetRoot":true}
```

```text
/tool unreal.scaffold_economy_system {"rootPath":"/Game/MCPDemo","compile":true,"savePackage":true}
```

```text
/tool unreal.scaffold_autobattler_ai {"rootPath":"/Game/MCPDemo","compile":true,"savePackage":true}
```

```text
/tool unreal.scaffold_result_ui {"rootPath":"/Game/MCPDemo","compile":true,"savePackage":true,"replaceWidgetRoot":true}
```

MCP tool extension scaffold example:

```text
/tool unreal.scaffold_mcp_tool {"toolName":"unreal.my_custom_tool","title":"My Custom Tool","description":"Creates scaffold files for a custom MCP tool.","argumentSchemaJson":"{\"type\":\"object\",\"properties\":{\"message\":{\"type\":\"string\"}}}","exampleArgumentsJson":"{\"message\":\"hello\"}","overwrite":false}
```

This generates reviewable descriptor-first C++ patch fragments, `ToolRegistryPatch.json`, a test request, and an integration checklist under `Tools/UnrealMcpToolScaffolds`. It does not hot-load C++ into the running editor; apply the patches, rebuild the current `<ProjectName>Editor` target, and reopen the editor if needed.

MCP extension safety checks:

```text
/tool unreal.mcp_list_scaffolds {"includeSavedTestScaffolds":true}
```

```text
/tool unreal.mcp_inspect_scaffold {"toolName":"unreal.my_custom_tool"}
```

`unreal.mcp_list_scaffolds` gives Chat a project-local inventory of generated scaffold folders, including readiness, missing files, schema status, test request validity, and whether each tool is already loaded. `unreal.mcp_inspect_scaffold` drills into a single scaffold by `toolName` or `scaffoldDir` and returns required file status, patch previews, requested schema compatibility, and the generated `TestRequest.json`.

```text
/tool unreal.mcp_validate_tool_schema {"toolName":"unreal.scaffold_mcp_tool"}
```

```text
/tool unreal.mcp_apply_scaffold {"toolName":"unreal.my_custom_tool","dryRun":true}
```

```text
/tool unreal.mcp_apply_scaffold {"toolName":"unreal.my_custom_tool","dryRun":false}
```

Validate or patch generated descriptor-first fragments before applying them:

```text
/tool unreal.mcp_validate_cpp_patch {"toolName":"unreal.my_custom_tool","patchName":"CategoryHandlerFunction.patch.cpp"}
```

```text
/tool unreal.mcp_patch_scaffold_patch {"toolName":"unreal.my_custom_tool","patchName":"CategoryHandlerFunction.patch.cpp","findText":"TODO","replaceText":"Reviewed by the patch safety layer.","dryRun":true}
```

`unreal.mcp_validate_cpp_patch` checks generated C++ patch fragments for risky patterns such as process execution, destructive file operations, external path literals, recursive pipeline calls, obvious infinite loops, missing handler returns, and flexible schema warnings. `unreal.mcp_patch_scaffold_patch` can edit descriptor-first patches (`ToolRegistrar.patch.cpp`, `ToolRegistrarCall.patch.cpp`, `CategoryHandlerFunction.patch.cpp`, `CategoryDispatcherBranch.patch.cpp`) or optional `ChatCommand.patch.cpp` with dry-run diff previews, idempotence checks, backups, and the same static validation gate. Legacy ToolDefinition/ExecuteToolHandler fragments are no longer generated by default; legacy snippet-named aliases are hidden from default AI tool exposure.

Protect a risky MCP extension session:

```text
/tool unreal.mcp_lock_extension_session {"mode":"status"}
```

`unreal.mcp_lock_extension_session` manages `Saved/UnrealMcp/ExtensionSession.lock`. The high-risk extension tools also acquire this lock automatically while applying, building, testing, running the pipeline, or rolling back, which helps prevent two Chat/supervisor sessions from editing plugin source at once.

Create a project-state snapshot:

```text
/tool unreal.mcp_backup_project_state {"label":"before_custom_tool","reason":"Snapshot before applying a new MCP extension."}
```

This captures MCP source/header files, root/plugin README files, project memory, extension manifests, and optionally recent build logs under `Saved/UnrealMcp/ProjectStateBackups`.

Generate a schema-derived test suite:

```text
/tool unreal.mcp_generate_tests {"toolName":"unreal.my_custom_tool","scaffoldDir":"Tools/UnrealMcpToolScaffolds/my_custom_tool"}
```

This writes `Tests/valid_basic.json`, `Tests/missing_required.json`, `Tests/boundary_values.json`, and `Tests/wrong_type.json`. Test files can wrap a JSON-RPC `tools/call` request with metadata such as `name`, `description`, `expectToolListed`, `executeTool`, and `expectToolCallError`.

Build the editor after applying patches:

```text
/tool unreal.mcp_build_editor {"toolName":"unreal.my_custom_tool","scaffoldDir":"Tools/UnrealMcpToolScaffolds/my_custom_tool","writeProjectMemory":true}
```

`unreal.mcp_build_editor`, `unreal.mcp_build_game`, `unreal.mcp_build_server`, and `unreal.mcp_build_client` capture UBT output under `Saved/UnrealMcp/BuildLogs`, parse success/failure plus key error lines, and write build status memory entries. `unreal.mcp_build_packaged` runs RunUAT BuildCookRun and writes packaged output under `Saved/StagedBuilds/<Platform>` or a project-local archive directory. Since the editor build command runs from an already-open editor, newly built plugin code still requires an Unreal Editor restart before fresh tools appear in `tools/list`.

If the build fails, generate a fix plan:

```text
/tool unreal.mcp_compile_error_fix_plan {"maxErrors":5,"contextLines":4}
```

The fix planner reads the newest build log unless `buildLogPath` is provided, extracts compiler errors with file/line/source context, guesses likely causes, and returns suggested fixes. It only reports `patchApplied=true` when a deterministic safe auto-patch is actually available.

After restart, run the generated test request:

```text
/tool unreal.mcp_run_tool_test {"memoryKey":"mcp.extension.build_test","readProjectMemory":true}
```

The single test runner resolves `TestRequest.json` or a wrapped test case, verifies the tool is listed, executes the recorded `tools/call` through the in-editor MCP handlers, compares expected error state, and records the result back into project memory.

Run the full generated suite:

```text
/tool unreal.mcp_run_test_suite {"memoryKey":"mcp.extension.build_test","readProjectMemory":true}
```

The suite runner executes all `Tests/*.json` files, reports pass rate, failed cases, failure text, and failed `structuredContent`.

Run the high-level extension pipeline:

```text
/tool unreal.mcp_extension_pipeline {"toolName":"unreal.my_custom_tool","memoryKey":"mcp.extension.pipeline"}
```

The pipeline resolves the scaffold, validates the requested schema when available, generates or refreshes `Tests/*.json`, runs `mcp_apply_scaffold` in dry-run mode, applies descriptor-first patches with backup support, writes project memory, builds the editor, and either runs the test suite immediately or returns `requiresRestart=true` with a resume command.

After an editor restart:

```text
/tool unreal.mcp_extension_pipeline {"mode":"resume_test","memoryKey":"mcp.extension.pipeline"}
```

Inspect pipeline state and last source apply:

```text
/tool unreal.mcp_pipeline_status {"memoryKey":"mcp.extension.pipeline"}
```

```text
/tool unreal.mcp_workbench_status {"memoryKey":"mcp.extension.pipeline","includeBuildLogTail":false}
```

```text
/tool unreal.mcp_diff_last_apply {"maxPreviewLines":80}
```

`unreal.mcp_pipeline_status` collects project memory, the latest apply manifest, the newest build log tail, test scaffolds, test requests, and extension backups into one status report. `unreal.mcp_workbench_status` adds tool audit health, ToolRegistry legacy-hidden tools, handler aliases, supervisor logs, and aggregate test counts for a higher-level self-extension dashboard. `unreal.mcp_diff_last_apply` reads the backup snapshots written by `mcp_apply_scaffold`, so it can explain exactly what the last automatic source integration changed.

`unreal.workflow_run` is the generic composition executor for bounded high-level tool combinations. It accepts inline `steps`, a `workflowJson` string, or a project-local `workflowPath`, defaults to `dryRun:true`, blocks nested workflows plus high/critical step tools unless explicitly allowed, executes every step through the normal MCP handler path, and can write `chat.active_task` for continuation after planned, paused, or completed workflows.

`unreal.knowledge_search`, `unreal.tool_recommend`, `unreal.tool_gap_analyze`, `unreal.workflow_recommend`, and `unreal.knowledge_eval_run` are the local RAG-facing planning tools. Chat builds a compact RAG/tool-planning capsule before AI turns; if the local KnowledgeCard index is missing, it can run `unreal.knowledge_index_refresh` and retry. The index is written under `Saved/UnrealMcp/KnowledgeIndex`, while fetched official documentation caches stay under `Saved/UnrealMcp/KnowledgeSources`.

The Workbench UI is intentionally thin: it calls the same MCP tools used by Chat and displays the latest structured result. It currently exposes safe operational buttons for status refresh, audit, core test suite, pipeline status, lock status, Skill Activity status, draft distillation, promote dry-run, Knowledge refresh/search/tool recommendation/eval, and copying the latest result. High-risk actions such as apply, build, restart, real promote, and rollback should stay behind the existing dry-run, lock, supervisor, and manifest workflow until the UI adds explicit confirmation surfaces.

`tools/list`, audit output, and workbench status include ToolRegistry policy metadata for every visible tool:

- `riskLevel`
- `requiresWrite`
- `requiresBuild`
- `requiresExternalProcess`
- `requiresRestart`
- `requiresProjectMemory`
- `requiresLock`

Stable core test fixtures live under `Tools/UnrealMcpTests/Core`. Runtime-generated scaffold tests remain under `Saved/UnrealMcp/TestScaffolds`.

External restart supervisor:

```bash
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs wait
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs status
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs restart
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs resume-test --memory-key mcp.extension.pipeline --pipeline
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs pipeline --auto-restart --args-json '{"toolName":"unreal.my_custom_tool","memoryKey":"mcp.extension.pipeline"}'
```

The supervisor script runs outside Unreal Editor, so it can close/reopen the editor and resume MCP tests after plugin code reloads. This is the safe path for strict self-extension; the in-editor Chat can request a restart, but it cannot keep executing while its own host process is closed. Use `status` when the endpoint times out; it reports `tools/list` readiness, MCP port listeners, and matching UnrealEditor PIDs. `restart` aborts if the old editor does not stop cleanly; pass `--force-kill` only after saving or intentionally discarding editor state.

Generate local supervisor launchers:

```text
/tool unreal.mcp_supervisor_install {"platform":"all","outputDir":"Tools/UnrealMcpSupervisor","memoryKey":"mcp.extension.pipeline"}
```

The installer generates a macOS `.command` shortcut, a macOS LaunchAgent plist, a Windows PowerShell launcher, and a small local README. The default output directory is ignored by Git because generated files contain machine-specific absolute paths. Shared templates and the full operating guide live in `Tools/UnrealMcpSupervisorTemplates/` and `Docs/Supervisor.md`.

```text
/tool unreal.mcp_rollback_last_extension {"dryRun":true}
```

```text
/tool unreal.mcp_rollback_to_manifest {"toolName":"unreal.my_custom_tool","selector":"latest","dryRun":true}
```

`unreal.mcp_rollback_to_manifest` can restore a selected historical `Saved/UnrealMcp/ExtensionBackups/*/Manifest.json`, not only the latest apply manifest. A real rollback creates a pre-rollback project-state backup by default.

```text
/tool unreal.mcp_tool_audit {}
```

Clean generated test artifacts safely:

```text
/tool unreal.mcp_clean_test_artifacts {"dryRun":true}
```

```text
/tool unreal.mcp_clean_test_artifacts {"dryRun":false,"nameContains":"phase5_cleanup_probe"}
```

The cleanup tool is intentionally conservative: by default it only previews `Saved/UnrealMcp/TestScaffolds` candidates. Build logs, extension backups, test requests, and project memory require explicit boolean flags, and all cleanup is constrained to `Saved/UnrealMcp`.

Restart handoff memory:

```text
/tool unreal.project_memory_write {"key":"mcp_extension","summary":"Resume MCP extension work after editor restart.","status":"in_progress","nextStep":"Run tool audit after rebuild.","contentJson":"{\"target\":\"self-extension\"}","tags":["mcp","restart"]}
```

```text
/tool unreal.project_memory_read {"key":"mcp_extension","includeContent":true}
```

```text
/tool unreal.project_memory_view {"tag":"mcp","includeContent":false}
```

```text
/tool unreal.project_memory_edit {"key":"mcp_extension","status":"in_progress","contentJson":"{\"phase\":\"memory-skill\"}","contentMode":"merge","tags":["mcp","memory"],"tagsMode":"append"}
```

```text
/tool unreal.project_memory_delete {"key":"temporary_memory","dryRun":true}
```

Project-local skills:

```text
/tool unreal.skill_list {}
```

```text
/tool unreal.skill_read {"skillName":"mcp-self-extension"}
```

```text
/tool unreal.skill_apply {"skillName":"mcp-self-extension","task":"Extend Unreal MCP safely from Editor Chat."}
```

The project skill tools scan the active project's `Tools/UnrealMcpSkills`, then parent repo roots, for `SKILL.md` or `*.skill` files. Applying a skill returns the instruction text to Chat and can record the skill/task in project memory.

Tool package sharing:

```text
/tool unreal.tools.export_package {"toolName":"unreal.skill_list","dryRun":true}
/tool unreal.tools.import_package {"packagePath":"Saved/UnrealMcp/Packages/unreal.skill_list-reviewed.zip","dryRun":true}
```

Export writes reviewed packages under `Saved/UnrealMcp/Packages`; import validates `manifest.json` SHA-256 entries before previewing or applying registry/scaffold/test changes. See `Docs/SelfExtensionPipeline.md#tool-sharing`.

Skill distillation tools:

```text
/tool unreal.skill_recording_start {"goal":"Capture a repeatable editor workflow.","recordIntervalSeconds":60}
```

```text
/tool unreal.skill_activity_status {"includeRecentEvents":true,"maxEvents":10}
```

```text
/tool unreal.skill_distill_from_activity {"skillName":"repeatable-editor-workflow","title":"Repeatable Editor Workflow","writeDraft":true}
```

```text
/tool unreal.skill_promote_draft {"skillName":"repeatable-editor-workflow","dryRun":true,"overwrite":false}
```

Activity logs are local-only runtime files under `Saved/UnrealMcp/ActivityLog/*.jsonl`. Distilled drafts are written to `Saved/UnrealMcp/SkillDrafts/<skill-name>/SKILL.md`; only `unreal.skill_promote_draft` writes a reviewed skill into versioned `Tools/UnrealMcpSkills/<skill-name>/SKILL.md`. Promotion defaults to `dryRun:true`; real promotion should use `dryRun:false` after review, and overwrites can create backups/manifests under `Saved/UnrealMcp/SkillPromotionBackups`.

## Quick Test

After the editor is running with the plugin enabled:

The MCP endpoint is hosted by the running Unreal Editor process. It will not respond after only cloning or building the project; open the project in Unreal Editor and wait for the `Unreal MCP listening on http://127.0.0.1:8765/mcp` log line first.

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}' \
  http://127.0.0.1:8765/mcp
```

Then:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
  http://127.0.0.1:8765/mcp
```

Example tool call:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"unreal.editor_status","arguments":{}}}' \
  http://127.0.0.1:8765/mcp
```

On Windows PowerShell, avoid hand-escaped JSON when possible. This form is more reliable:

```powershell
$Body = @{
  jsonrpc = "2.0"
  id = 3
  method = "tools/call"
  params = @{
    name = "unreal.editor_status"
    arguments = @{}
  }
} | ConvertTo-Json -Depth 8

Invoke-WebRequest `
  -Uri "http://127.0.0.1:8765/mcp" `
  -Method POST `
  -Headers @{
    "Content-Type" = "application/json"
    "Accept" = "application/json, text/event-stream"
    "MCP-Protocol-Version" = "2025-06-18"
  } `
  -Body $Body
```

PIE control example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"unreal.start_pie","arguments":{}}}' \
  http://127.0.0.1:8765/mcp
```

Log tail example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"unreal.tail_log","arguments":{"lines":80,"contains":"Error"}}}' \
  http://127.0.0.1:8765/mcp
```

Actor selection example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"unreal.select_actors","arguments":{"filter":"PlayerStart","clearSelection":true}}}' \
  http://127.0.0.1:8765/mcp
```

Preview the current level environment clear operation:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"unreal.clear_level_environment","arguments":{"dryRun":true,"maxPasses":3}}}' \
  http://127.0.0.1:8765/mcp
```

To clear every level actor, call it again with `dryRun:false` and `confirmClearAll:true`. If you provide `filter`, `classPath`, or `paths`, the confirmation flag is not required.

Batch Blueprint compile example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"unreal.compile_blueprints_in_path","arguments":{"path":"/Game/TopDown","recursive":true,"limit":5}}}' \
  http://127.0.0.1:8765/mcp
```

Batch actor scale example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"unreal.batch_set_actor_scale","arguments":{"selectedOnly":true,"scaleX":1.1,"scaleY":1.1,"scaleZ":1.1}}}' \
  http://127.0.0.1:8765/mcp
```

Grid layout example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"unreal.layout_actors_grid","arguments":{"selectedOnly":true,"columns":3,"spacingX":300,"spacingY":220}}}' \
  http://127.0.0.1:8765/mcp
```

Circle layout example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"unreal.layout_actors_circle","arguments":{"selectedOnly":true,"radius":1200,"startAngleDegrees":0,"arcDegrees":360,"alignYawToCenter":true}}}' \
  http://127.0.0.1:8765/mcp
```

Python execution example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"unreal.execute_python","arguments":{"command":"unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world().get_name()","mode":"EvaluateStatement","scope":"Private","forceEnable":true,"unattended":true}}}' \
  http://127.0.0.1:8765/mcp
```

Python file execution example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"unreal.execute_python_file","arguments":{"scriptPath":"Tools/mcp_test_script.py","args":["--check"],"scope":"Private","forceEnable":true,"unattended":true}}}' \
  http://127.0.0.1:8765/mcp
```

Note: `tools/call` is now dispatched asynchronously to the Unreal Editor game thread. If the game thread stalls during startup or heavy asset work, the HTTP request returns a timeout instead of hanging forever.

## stdio Proxy

Some MCP hosts still want a local stdio process. This repo includes a tiny proxy at:

`Tools/unreal_mcp_stdio_proxy.py`

It forwards newline-delimited stdio MCP traffic to the Unreal Editor HTTP endpoint.

Example environment variables:

- `UNREAL_MCP_URL=http://127.0.0.1:8765/mcp`
- `UNREAL_MCP_AUTH_TOKEN=...`

Example host command:

```bash
python3 /absolute/path/to/UEvolve/Tools/unreal_mcp_stdio_proxy.py
```
