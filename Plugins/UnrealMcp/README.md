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
- `unreal.batch_set_actor_properties`
- `unreal.layout_actors_grid`
- `unreal.layout_actors_circle`
- `unreal.open_map`
- `unreal.open_asset`
- `unreal.sync_content_browser`
- `unreal.spawn_actor_basic`
- `unreal.spawn_actor_batch_basic`
- `unreal.spawn_static_mesh_actor`
- `unreal.spawn_actor`
- `unreal.spawn_actor_batch`
- `unreal.destroy_selected_actors`
- `unreal.compile_blueprint`
- `unreal.compile_blueprints_in_path`
- `unreal.create_blueprint_class`
- `unreal.bp_add_variable`
- `unreal.bp_add_function`
- `unreal.bp_add_event_node`
- `unreal.bp_add_call_function_node`
- `unreal.bp_add_branch_node`
- `unreal.bp_add_for_each_node`
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
- `unreal.scaffold_mcp_tool`
- `unreal.mcp_list_scaffolds`
- `unreal.mcp_inspect_scaffold`
- `unreal.mcp_validate_cpp_snippet`
- `unreal.mcp_patch_scaffold_snippet`
- `unreal.mcp_validate_tool_schema`
- `unreal.mcp_apply_scaffold`
- `unreal.mcp_rollback_last_extension`
- `unreal.mcp_build_editor`
- `unreal.mcp_run_tool_test`
- `unreal.mcp_extension_pipeline`
- `unreal.mcp_pipeline_status`
- `unreal.mcp_diff_last_apply`
- `unreal.mcp_clean_test_artifacts`
- `unreal.mcp_tool_audit`
- `unreal.project_memory_write`
- `unreal.project_memory_read`
- `unreal.save_dirty_packages`

## Endpoint

By default the plugin listens on:

`http://127.0.0.1:8765/mcp`

You can change the port, path, allowed origins, and optional auth token in:

`Project Settings > Plugins > Unreal MCP`

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

- Plain text in the chat panel is treated as an AI request.
- `/ask <prompt>` explicitly starts an AI turn.
- `/reset_ai` clears the assistant's response chain so the next ask starts a fresh conversation.
- `/stop_ai` or the `Stop` button cancels the current generation.
- The assistant streams text deltas into the panel, renders tool calls as cards, sends tool results back to the model, and then posts the final answer.
- If the model hits `max_output_tokens`, Unreal MCP automatically attempts a short continuation instead of immediately failing the turn.
- Long AI turns now use configurable request and activity timeouts so tool-heavy or planning-heavy requests are less likely to fail at the transport layer.
- Unreal MCP also synchronizes Unreal's global HTTP connection/activity timeouts for AI turns so macOS does not fall back to the engine's default 30-second connection timeout.
- The default AI tool-round budget is 16 so scaffold-style requests have more room to inspect, create assets, and then summarize.
- The conversation history is persisted at `Saved/UnrealMcp/ChatHistory.json`.
- On the first AI turn after the panel reloads, Unreal MCP now also replays a small, compact slice of the persisted local transcript back to the model for continuity, so saved history is not just UI-only.
- `Copy Chat` copies the full visible transcript, and `Copy Log` copies the most recent `/log` or `unreal.tail_log` output.
- For AI-driven editing, prefer the fixed-schema wrapper tools such as `unreal.spawn_actor_basic`, `unreal.spawn_static_mesh_actor`, `unreal.batch_set_actor_scale`, `unreal.batch_set_actor_tags`, `unreal.batch_set_point_light_properties`, `unreal.batch_configure_static_mesh_actors`, the `unreal.bp_*` Blueprint graph editing tools, the `unreal.widget_*` UMG editing tools, and the `unreal.scaffold_*` gameplay scaffold tools.
- Tool names are internally normalized for model compatibility, but the underlying Unreal MCP tools and behavior stay the same.

## In-Editor Chat

Open the command chat window from:

`Window > Unreal MCP Chat`

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
/tool unreal.spawn_actor {"classPath":"/Script/Engine.PointLight","x":0,"y":0,"z":150,"label":"ChatLight"}
```

```text
/tool unreal.batch_set_actor_properties {"selectedOnly":true,"properties":{"Tags":["Encounter","Wave1"]}}
```

```text
/tool unreal.execute_python {"command":"import unreal\nprint(unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world().get_name())"}
```

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

This generates reviewable C++ snippets, a test request, and an integration checklist under `Tools/UnrealMcpToolScaffolds`. It does not hot-load C++ into the running editor; integrate the snippets, rebuild `MyProjectEditor`, and reopen the editor if needed.

MCP extension safety checks:

```text
/tool unreal.mcp_list_scaffolds {"includeSavedTestScaffolds":true}
```

```text
/tool unreal.mcp_inspect_scaffold {"toolName":"unreal.my_custom_tool"}
```

`unreal.mcp_list_scaffolds` gives Chat a project-local inventory of generated scaffold folders, including readiness, missing files, schema status, test request validity, and whether each tool is already loaded. `unreal.mcp_inspect_scaffold` drills into a single scaffold by `toolName` or `scaffoldDir` and returns required file status, snippet previews, requested schema compatibility, and the generated `TestRequest.json`.

```text
/tool unreal.mcp_validate_tool_schema {"toolName":"unreal.scaffold_mcp_tool"}
```

```text
/tool unreal.mcp_apply_scaffold {"toolName":"unreal.my_custom_tool","dryRun":true}
```

```text
/tool unreal.mcp_apply_scaffold {"toolName":"unreal.my_custom_tool","dryRun":false}
```

Validate or patch generated snippets before applying them:

```text
/tool unreal.mcp_validate_cpp_snippet {"toolName":"unreal.my_custom_tool","snippetName":"ExecuteToolHandler.cpp.snippet"}
```

```text
/tool unreal.mcp_patch_scaffold_snippet {"toolName":"unreal.my_custom_tool","snippetName":"ExecuteToolHandler.cpp.snippet","findText":"TODO","replaceText":"Reviewed by the snippet safety layer.","dryRun":true}
```

`unreal.mcp_validate_cpp_snippet` checks generated C++ snippets for risky patterns such as process execution, destructive file operations, external path literals, recursive pipeline calls, obvious infinite loops, missing handler returns, and flexible schema warnings. `unreal.mcp_patch_scaffold_snippet` can edit `ToolDefinition.cpp.snippet`, `ExecuteToolHandler.cpp.snippet`, or `ChatCommand.cpp.snippet` with dry-run diff previews, idempotence checks, backups, and the same static validation gate.

Build the editor after applying snippets:

```text
/tool unreal.mcp_build_editor {"toolName":"unreal.my_custom_tool","scaffoldDir":"Tools/UnrealMcpToolScaffolds/my_custom_tool","writeProjectMemory":true}
```

`unreal.mcp_build_editor` captures UBT output under `Saved/UnrealMcp/BuildLogs`, parses success/failure plus key error lines, and writes a restart handoff memory entry. Since this command runs from an already-open editor, newly built plugin code still requires an Unreal Editor restart before fresh tools appear in `tools/list`.

After restart, run the generated test request:

```text
/tool unreal.mcp_run_tool_test {"memoryKey":"mcp.extension.build_test","readProjectMemory":true}
```

The test runner resolves `TestRequest.json`, verifies the tool is listed, executes the recorded `tools/call` through the in-editor MCP handlers, and records the result back into project memory.

Run the high-level extension pipeline:

```text
/tool unreal.mcp_extension_pipeline {"toolName":"unreal.my_custom_tool","memoryKey":"mcp.extension.pipeline"}
```

The pipeline resolves the scaffold, validates the requested schema when available, runs `mcp_apply_scaffold` in dry-run mode, applies snippets with backup support, writes project memory, builds the editor, and either runs the test immediately or returns `requiresRestart=true` with a resume command.

After an editor restart:

```text
/tool unreal.mcp_extension_pipeline {"mode":"resume_test","memoryKey":"mcp.extension.pipeline"}
```

Inspect pipeline state and last source apply:

```text
/tool unreal.mcp_pipeline_status {"memoryKey":"mcp.extension.pipeline"}
```

```text
/tool unreal.mcp_diff_last_apply {"maxPreviewLines":80}
```

`unreal.mcp_pipeline_status` collects project memory, the latest apply manifest, the newest build log tail, test scaffolds, test requests, and extension backups into one status report. `unreal.mcp_diff_last_apply` reads the backup snapshots written by `mcp_apply_scaffold`, so it can explain exactly what the last automatic source integration changed.

External restart supervisor:

```bash
python3 Tools/unreal_mcp_supervisor.py wait
python3 Tools/unreal_mcp_supervisor.py restart
python3 Tools/unreal_mcp_supervisor.py resume-test --memory-key mcp.extension.pipeline --pipeline
python3 Tools/unreal_mcp_supervisor.py pipeline --auto-restart --args-json '{"toolName":"unreal.my_custom_tool","memoryKey":"mcp.extension.pipeline"}'
```

The supervisor script runs outside Unreal Editor, so it can close/reopen the editor and resume MCP tests after plugin code reloads. This is the safe path for strict self-extension; the in-editor Chat can request a restart, but it cannot keep executing while its own host process is closed.

```text
/tool unreal.mcp_rollback_last_extension {"dryRun":true}
```

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

## Quick Test

After the editor is running with the plugin enabled:

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

Batch Blueprint compile example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"unreal.compile_blueprints_in_path","arguments":{"path":"/Game/TopDown","recursive":true,"limit":5}}}' \
  http://127.0.0.1:8765/mcp
```

Batch actor property edit example:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -H 'MCP-Protocol-Version: 2025-06-18' \
  -d '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"unreal.batch_set_actor_properties","arguments":{"selectedOnly":true,"properties":{"Tags":["Encounter"],"RootComponent.RelativeScale3D":{"X":1.1,"Y":1.1,"Z":1.1}}}}}' \
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
python3 /absolute/path/to/MyProject/Tools/unreal_mcp_stdio_proxy.py
```
