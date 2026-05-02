# UEvolve

**Unreal Editor MCP Self-Extension Workbench**

This repository is an Unreal Engine 5.7 project focused on editor automation, AI-assisted project inspection, Blueprint scaffolding, UMG setup, and local Model Context Protocol workflows.

It includes an in-project editor plugin, **Unreal MCP**, which exposes Unreal Editor operations through a localhost MCP endpoint and an in-editor chat panel. The current product direction is an **Unreal Editor MCP self-extension workbench**: AI can call tools, then safely scaffold, validate, dry-run, apply, build, test, roll back, and remember MCP tool extensions.

## 中文概览

本项目当前定位为 **面向 Unreal Editor 的 MCP 自扩展工作台**。

它不只是让 AI 调用 Unreal Editor 工具，而是尝试把“新增 MCP 能力”本身产品化：AI 可以在安全审计、dry run、备份、编译、测试、回滚、长期记忆和外部 supervisor 的保护下，为当前项目持续扩展新的编辑器自动化工具。

当前重点：

- 在 Unreal Editor 内运行本地 MCP server，并提供 `Window > Unreal MCP Chat` 对话入口。
- 通过 `Window > Unreal MCP Workbench` 提供轻量自扩展控制台，聚合状态、审计、核心测试、pipeline、lock 等能力。
- 支持项目检查、地图/资产查询、PIE 控制、日志读取、Map Check、Python/Console 执行等基础编辑器自动化。
- 支持 Blueprint 图编辑、UMG Widget 编辑、玩法系统脚手架、MCP 工具脚手架和项目本地 `.skill` 工作流。
- 自扩展链路包含 schema 校验、snippet 校验、dry-run diff、备份 manifest、UBT 编译、测试套件、rollback、project memory 和 supervisor 重启恢复。
- 引入多人协作保护：CODEOWNERS、工具命名规范、Manifest schema、extension session lock、ToolRegistry 风险元数据和冲突检测规则。
- 保留 UE 模板内容作为本地测试与原型资源，并使用 Git LFS 管理 Unreal 二进制资产，方便上传和协作。

## Current Status

The project currently contains:

- Unreal Engine 5.7 C++ project foundation.
- TopDown, Strategy, and TwinStick template content for local testing.
- `Plugins/UnrealMcp`, an editor plugin for local MCP and in-editor AI/chat workflows.
- Git LFS setup for Unreal binary assets.
- Project-level README and ignore rules suitable for public GitHub hosting.
- Self-extension safety rails: schema validation, snippet validation, dry-run diffs, backups, build/test handoff, rollback manifests, project memory, and project-local skills.
- Versioned core MCP test fixtures under `Tools/UnrealMcpTests`.
- ToolRegistry policy metadata for risk level, write/build/process/restart/memory/lock requirements.

## Planning Docs

- [Roadmap](Docs/Roadmap.md)
- [Architecture](Docs/Architecture.md)
- [Contributing](Docs/Contributing.md)
- [Security Model](Docs/SecurityModel.md)
- [Self-Extension Pipeline](Docs/SelfExtensionPipeline.md)
- [Tool Naming](Docs/ToolNaming.md)
- [Manifest Schema](Docs/ManifestSchema.md)
- [External Supervisor](Docs/Supervisor.md)

## Unreal MCP Plugin

Plugin path:

```text
Plugins/UnrealMcp
```

Default MCP endpoint:

```text
http://127.0.0.1:8765/mcp
```

Editor chat panel:

```text
Window > Unreal MCP Chat
```

Self-extension workbench:

```text
Window > Unreal MCP Workbench
```

Full plugin documentation:

```text
Plugins/UnrealMcp/README.md
```

## Tool Coverage

Unreal MCP currently supports:

- Editor and project inspection.
- Log tailing and map checks.
- Map and asset listing.
- PIE start/stop.
- Actor selection, transforms, spawning, layout, and batch property edits.
- Python execution inside Unreal Editor.
- Blueprint class creation and compilation.
- Blueprint graph editing:
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
- UMG Widget Blueprint editing:
  - `unreal.widget_add`
  - `unreal.widget_remove`
  - `unreal.widget_set_property`
  - `unreal.widget_set_slot_layout`
  - `unreal.widget_bind_event`
  - `unreal.widget_bind_blueprint_variable`
  - `unreal.widget_build_template`
- Gameplay scaffold helpers:
  - `unreal.scaffold_round_system`
  - `unreal.scaffold_shop_system`
  - `unreal.scaffold_economy_system`
  - `unreal.scaffold_autobattler_ai`
  - `unreal.scaffold_result_ui`
- MCP extension scaffolding:
  - `unreal.scaffold_mcp_tool`
  - `unreal.mcp_list_scaffolds`
  - `unreal.mcp_inspect_scaffold`
  - `unreal.mcp_validate_cpp_snippet`
  - `unreal.mcp_patch_scaffold_snippet`
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
  - `unreal.mcp_run_tool_test`
  - `unreal.mcp_run_test_suite`
  - `unreal.mcp_extension_pipeline`
  - `unreal.mcp_pipeline_status`
  - `unreal.mcp_workbench_status`
  - `unreal.mcp_diff_last_apply`
  - `unreal.mcp_clean_test_artifacts`
  - `unreal.mcp_tool_audit`
- Restart-resilient project memory:
  - `unreal.project_memory_write`
  - `unreal.project_memory_read`
  - `unreal.project_memory_view`
  - `unreal.project_memory_edit`
  - `unreal.project_memory_delete`
- Project-local skills:
  - `unreal.skill_list`
  - `unreal.skill_read`
  - `unreal.skill_apply`
- Saving dirty packages.

Legacy flexible-schema tools such as `unreal.spawn_actor`, `unreal.spawn_actor_batch`, and `unreal.batch_set_actor_properties` still have handlers for compatibility, but are hidden from AI-facing `tools/list` by default. Use the fixed-schema wrappers such as `unreal.spawn_actor_basic`, `unreal.spawn_actor_batch_basic`, `unreal.spawn_static_mesh_actor`, and the fixed batch actor tools instead.

## In-Editor Chat Usage

Open the chat panel from:

```text
Window > Unreal MCP Chat
```

Examples:

```text
/status
/maps
/assets /Game/TopDown
/map_check
/log 80
```

Direct MCP tool call example:

```text
/tool unreal.editor_status {}
```

AI-assisted request example:

```text
inspect the current project and summarize the maps, selected actors, and available Blueprint assets
```

## Chat-Driven MCP Extension Workflow

The chat panel can already call existing MCP tools directly through natural language or `/tool`.

For extending MCP itself, use:

```text
/tool unreal.scaffold_mcp_tool {"toolName":"unreal.my_custom_tool","title":"My Custom Tool","description":"Scaffold a new MCP tool.","exampleArgumentsJson":"{\"message\":\"hello\"}"}
```

This generates reviewable files under:

```text
Tools/UnrealMcpToolScaffolds
```

Generated output includes:

- C++ tool definition snippet.
- C++ `ExecuteTool` handler snippet.
- Optional direct Chat slash-command snippet.
- Test JSON request.
- Optional `Tests/*.json` suite generated by `unreal.mcp_generate_tests`.
- Integration checklist.
- Per-tool README.

Current boundary: generated MCP tool snippets still need to be reviewed, integrated into the plugin source, compiled, and loaded by Unreal Editor. The project does not hot-load arbitrary new C++ tools into a running editor session.

Useful first-stage extension checks:

```text
/tool unreal.mcp_validate_tool_schema {"toolName":"unreal.scaffold_mcp_tool"}
/tool unreal.mcp_list_scaffolds {"includeSavedTestScaffolds":true}
/tool unreal.mcp_inspect_scaffold {"toolName":"unreal.my_custom_tool"}
/tool unreal.mcp_validate_cpp_snippet {"toolName":"unreal.my_custom_tool","snippetName":"ExecuteToolHandler.cpp.snippet"}
/tool unreal.mcp_patch_scaffold_snippet {"toolName":"unreal.my_custom_tool","snippetName":"ExecuteToolHandler.cpp.snippet","findText":"TODO","replaceText":"Reviewed by the snippet safety layer.","dryRun":true}
/tool unreal.mcp_apply_scaffold {"toolName":"unreal.my_custom_tool","dryRun":true}
/tool unreal.mcp_apply_scaffold {"toolName":"unreal.my_custom_tool","dryRun":false}
/tool unreal.mcp_lock_extension_session {"mode":"status"}
/tool unreal.mcp_backup_project_state {"label":"before_custom_tool","reason":"Snapshot before applying a new MCP extension."}
/tool unreal.mcp_rollback_last_extension {"dryRun":true}
/tool unreal.mcp_rollback_to_manifest {"toolName":"unreal.my_custom_tool","selector":"latest","dryRun":true}
/tool unreal.mcp_generate_tests {"toolName":"unreal.my_custom_tool","scaffoldDir":"Tools/UnrealMcpToolScaffolds/my_custom_tool"}
/tool unreal.mcp_build_editor {"toolName":"unreal.my_custom_tool","scaffoldDir":"Tools/UnrealMcpToolScaffolds/my_custom_tool","writeProjectMemory":true}
/tool unreal.mcp_compile_error_fix_plan {"maxErrors":5,"contextLines":4}
/tool unreal.mcp_supervisor_install {"platform":"all","outputDir":"Tools/UnrealMcpSupervisor","memoryKey":"mcp.extension.pipeline"}
/tool unreal.mcp_run_tool_test {"memoryKey":"mcp.extension.build_test","readProjectMemory":true}
/tool unreal.mcp_run_test_suite {"memoryKey":"mcp.extension.build_test","readProjectMemory":true}
/tool unreal.mcp_extension_pipeline {"toolName":"unreal.my_custom_tool","memoryKey":"mcp.extension.pipeline"}
/tool unreal.mcp_pipeline_status {"memoryKey":"mcp.extension.pipeline"}
/tool unreal.mcp_diff_last_apply {"maxPreviewLines":80}
/tool unreal.mcp_clean_test_artifacts {"dryRun":true}
/tool unreal.mcp_tool_audit {}
/tool unreal.mcp_workbench_status {"memoryKey":"mcp.extension.pipeline","includeBuildLogTail":false}
/tool unreal.project_memory_write {"key":"mcp_extension","summary":"Resume MCP extension work after editor restart.","status":"in_progress","nextStep":"Run schema validation and tool audit after rebuilding.","contentJson":"{\"target\":\"self-extension\"}","tags":["mcp","restart"]}
/tool unreal.project_memory_read {"key":"mcp_extension","includeContent":true}
/tool unreal.project_memory_view {"tag":"mcp","includeContent":false}
/tool unreal.project_memory_edit {"key":"mcp_extension","status":"in_progress","contentJson":"{\"phase\":\"memory-skill\"}","contentMode":"merge","tags":["mcp","memory"],"tagsMode":"append"}
/tool unreal.project_memory_delete {"key":"temporary_memory","dryRun":true}
/tool unreal.skill_list {}
/tool unreal.skill_read {"skillName":"mcp-self-extension"}
/tool unreal.skill_apply {"skillName":"mcp-self-extension","task":"Extend Unreal MCP safely from Editor Chat."}
```

Build/test handoff note:

- `unreal.mcp_list_scaffolds` scans generated scaffold folders under `Tools/UnrealMcpToolScaffolds` and optionally `Saved/UnrealMcp/TestScaffolds`, reporting readiness, missing files, schema status, test request validity, and whether the tool is already loaded.
- `unreal.mcp_inspect_scaffold` inspects one scaffold by `toolName` or `scaffoldDir`, including required file status, snippet previews, requested schema compatibility, and the generated `TestRequest.json`.
- `unreal.mcp_validate_cpp_snippet` statically checks generated C++ snippets for risky operations before source integration, including process execution, destructive file operations, recursive pipeline calls, obvious infinite loops, missing handler returns, and flexible schema warnings.
- `unreal.mcp_patch_scaffold_snippet` edits `ToolDefinition.cpp.snippet`, `ExecuteToolHandler.cpp.snippet`, or `ChatCommand.cpp.snippet` with dry-run diff preview, idempotence checks, backup creation, and the same static validation gate.
- `unreal.mcp_apply_scaffold` validates snippets before integration by default and returns both snippet validation results and a target source diff preview so Chat can review the change before writing plugin source. Real applies write a schema-versioned manifest with the active extension session id, conflict policy, conflict count, and source hashes.
- `unreal.mcp_lock_extension_session` creates a short-lived lock under `Saved/UnrealMcp/ExtensionSession.lock` so two Chat/supervisor sessions do not apply, build, test, or roll back source at the same time. Extension apply manifests record the lock `sessionId` for audit and rollback traceability.
- `unreal.mcp_backup_project_state` snapshots MCP source/header files, root/plugin README files, project memory, extension manifests, and optional build logs under `Saved/UnrealMcp/ProjectStateBackups`.
- `unreal.mcp_rollback_to_manifest` can restore a selected historical `ExtensionBackups/*/Manifest.json`, not only `LastExtensionApply.json`, and can create a pre-rollback project-state backup first.
- `unreal.mcp_compile_error_fix_plan` reads the newest build log or a passed `buildLogPath`, extracts compiler errors with file/line/source context, guesses likely causes, and returns suggested fixes before another build.
- `unreal.mcp_supervisor_install` generates local macOS LaunchAgent/shortcut and Windows PowerShell launcher files for the external supervisor. Generated launchers live under `Tools/UnrealMcpSupervisor/` by default and are intentionally ignored because they contain machine-specific paths; versioned path-neutral templates live under `Tools/UnrealMcpSupervisorTemplates/`.
- `unreal.mcp_generate_tests` creates `Tests/valid_basic.json`, `Tests/missing_required.json`, `Tests/boundary_values.json`, and `Tests/wrong_type.json` from a loaded tool schema, scaffold README schema, or `TestRequest.json`.
- `unreal.mcp_build_editor` runs Unreal Build Tool for `MyProjectEditor`, captures a build log under `Saved/UnrealMcp/BuildLogs`, parses key error lines, and writes restart handoff state into project memory.
- Because the tool is invoked from a running editor, newly compiled plugin code is not loaded until Unreal Editor is restarted.
- After restart, `unreal.mcp_run_tool_test` can read the memory entry, locate the generated `TestRequest.json`, confirm the tool appears in `tools/list`, and execute the recorded `tools/call` request through the in-editor MCP handlers.
- `unreal.mcp_run_test_suite` runs all `Tests/*.json` cases and reports pass rate, failed cases, failure text, and failed structured content.
- `unreal.mcp_extension_pipeline` orchestrates validate, test generation, apply dry run, apply, memory write, build, restart handoff, and post-restart test suite resume.
- `unreal.mcp_pipeline_status` summarizes the current extension memory entry, last apply manifest, latest build log, saved test scaffolds, extension backups, and recommended next step.
- `unreal.mcp_workbench_status` aggregates tool audit health, ToolRegistry legacy-hidden tools, pipeline state, latest build/supervisor artifacts, test scaffold counts, and project memory into one self-extension dashboard response.
- `Window > Unreal MCP Workbench` provides a thin Slate control panel over existing MCP tools. It can refresh workbench status, run audit, run the versioned core tests, inspect pipeline status, inspect the extension lock, and copy the last structured result without duplicating backend logic.
- `tools/list`, `unreal.mcp_tool_audit`, and `unreal.mcp_workbench_status` include per-tool policy metadata such as `riskLevel`, `requiresWrite`, `requiresBuild`, `requiresExternalProcess`, `requiresRestart`, `requiresProjectMemory`, and `requiresLock`.
- Stable core MCP test fixtures live in `Tools/UnrealMcpTests/Core`; runtime-generated test scaffolds stay under ignored `Saved/UnrealMcp`.
- `unreal.mcp_diff_last_apply` reads `Saved/UnrealMcp/LastExtensionApply.json` and returns a before/after source diff preview from the backup snapshots created by `mcp_apply_scaffold`.
- `unreal.mcp_clean_test_artifacts` defaults to `dryRun:true` and only previews generated `Saved/UnrealMcp/TestScaffolds`; destructive cleanup must explicitly set `dryRun:false`, and optional filters such as `nameContains` should be used for targeted cleanup.
- `unreal.project_memory_view`, `unreal.project_memory_edit`, and `unreal.project_memory_delete` turn `Saved/UnrealMcp/ProjectMemory.json` into a manageable long-term project memory store with filters, field-level edits, content merge/replace, tag modes, dry-run edits, and safe dry-run deletion.
- `unreal.skill_list`, `unreal.skill_read`, and `unreal.skill_apply` scan project-local `SKILL.md` or `*.skill` files under `Tools/UnrealMcpSkills` by default. Applying a skill returns its instructions to Chat and can write a memory record of the applied skill/task.

External supervisor:

```bash
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs wait
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs restart
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs resume-test --memory-key mcp.extension.pipeline --pipeline
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs pipeline --auto-restart --args-json '{"toolName":"unreal.my_custom_tool","memoryKey":"mcp.extension.pipeline"}'
```

The supervisor runs outside Unreal Editor, so it can close/reopen the editor and resume MCP tests after plugin code reloads.

Full supervisor documentation:

```text
Docs/Supervisor.md
```

Install local supervisor launchers from Editor Chat:

```text
/tool unreal.mcp_supervisor_install {"platform":"all","outputDir":"Tools/UnrealMcpSupervisor","memoryKey":"mcp.extension.pipeline"}
```

Or from the terminal:

```bash
python3 Tools/unreal_mcp_supervisor.py install \
  --platform all \
  --output-dir Tools/UnrealMcpSupervisor \
  --memory-key mcp.extension.pipeline \
  --args-json '{"memoryKey":"mcp.extension.pipeline"}'
```

macOS generates:

- `Tools/UnrealMcpSupervisor/run_unreal_mcp_pipeline.command`
- `Tools/UnrealMcpSupervisor/com.unrealmcp.<project>.plist`

To install the LaunchAgent manually:

```bash
mkdir -p "$HOME/Library/LaunchAgents"
cp Tools/UnrealMcpSupervisor/com.unrealmcp.myproject.plist "$HOME/Library/LaunchAgents/"
launchctl unload "$HOME/Library/LaunchAgents/com.unrealmcp.myproject.plist" 2>/dev/null || true
launchctl load "$HOME/Library/LaunchAgents/com.unrealmcp.myproject.plist"
launchctl start "com.unrealmcp.myproject"
```

Windows generates:

- `Tools/UnrealMcpSupervisor/run_unreal_mcp_pipeline.ps1`

Run it from PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\Tools\UnrealMcpSupervisor\run_unreal_mcp_pipeline.ps1
```

## Opening The Project

Requirements:

- Unreal Engine 5.7.
- Git LFS.
- macOS is the most tested development path for this repository.

Clone and pull LFS assets:

```bash
git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve
git lfs install
git lfs pull
```

Open:

```text
MyProject.uproject
```

## Deployment Guide / 部署指南

This project is an Unreal Editor plugin workflow rather than a packaged game/server deployment. A normal deployment means cloning the repository, pulling LFS assets, opening the project in Unreal Engine, compiling the editor modules, and verifying that the local MCP endpoint is available.

### 1. Prepare The Machine

Install:

- Unreal Engine 5.7.
- Xcode command line tools on macOS.
- Visual Studio 2022 with C++ tools on Windows.
- Git.
- Git LFS.

Verify Git LFS:

```bash
git lfs install
git lfs version
```

### 2. Clone And Pull Assets

```bash
git clone https://github.com/edwinmeng163-oss/UEvolve.git
cd UEvolve
git lfs install
git lfs pull
```

If binary assets look very small or fail to load in Unreal, run `git lfs pull` again.

### 3. Open Or Build The Project

Open `MyProject.uproject` directly in Unreal Engine 5.7.

For a command-line editor build on macOS:

```bash
"/Users/Shared/Epic Games/UE_5.7/Engine/Build/BatchFiles/Mac/Build.sh" \
  MyProjectEditor Mac Development \
  -Project="$(pwd)/MyProject.uproject" \
  -WaitMutex
```

If the editor asks to rebuild modules on first open, allow it to rebuild.

### 4. Windows Deployment Notes

Windows is supported as a source-build target, but it has a few extra setup requirements because Unreal C++ plugins must be compiled locally for Win64.

Recommended Windows environment:

- Windows 10 or Windows 11 64-bit.
- Unreal Engine 5.7 installed through Epic Games Launcher or from source.
- Visual Studio 2022.
- Visual Studio workload: `Desktop development with C++`.
- MSVC v143 toolchain.
- Windows 10 or Windows 11 SDK.
- .NET support supplied by Unreal Engine's bundled DotNet runtime.
- Git for Windows.
- Git LFS for Windows.

Recommended clone location:

```text
C:\UnrealProjects\UEvolve
```

Avoid these locations when possible:

- Very deep paths, because Windows path length limits can still affect tooling.
- OneDrive-synced folders, because file locking and cloud sync can interfere with Unreal assets.
- Paths with non-ASCII characters if you hit build or plugin load issues.
- Protected folders such as `Program Files` for the project checkout.

Clone on Windows:

```powershell
git clone https://github.com/edwinmeng163-oss/UEvolve.git C:\UnrealProjects\UEvolve
cd C:\UnrealProjects\UEvolve
git lfs install
git lfs pull
```

Build from PowerShell:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" `
  MyProjectEditor Win64 Development `
  "-Project=$((Get-Location).Path)\MyProject.uproject" `
  -WaitMutex
```

Build from Command Prompt:

```bat
"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" ^
  MyProjectEditor Win64 Development ^
  -Project="%CD%\MyProject.uproject" ^
  -WaitMutex
```

If your engine is installed somewhere else, change the `UE_5.7` path accordingly.

Windows-specific issues to expect:

- Engine version mismatch: install Unreal Engine 5.7 or update `EngineAssociation` intentionally.
- Missing C++ compiler: install Visual Studio 2022 and the C++ desktop workload.
- Missing Windows SDK: add it through Visual Studio Installer.
- Plugin binary mismatch: macOS binaries do not run on Windows; rebuild to generate `Binaries/Win64`.
- Git LFS pointer files: if assets fail to load or are tiny text files, run `git lfs pull`.
- Path length errors: move the project closer to a drive root or enable long paths in Windows.
- OneDrive file locks: move the checkout out of OneDrive/Documents/Desktop if Unreal reports save/load issues.
- Antivirus or Windows Defender delays: allow Unreal Editor and UnrealBuildTool if builds are blocked or extremely slow.
- Controlled Folder Access: disable it for the project folder or allow Unreal tools.
- Firewall prompt: allow local/private network access if Windows asks when the MCP server starts.
- Port conflict: if `8765` is already in use, change the port in Project Settings or stop the other process.
- PowerShell `curl` behavior: use `curl.exe` explicitly if `curl` behaves like an alias.
- Generated project files: if Visual Studio cannot find targets, right-click `MyProject.uproject` and choose `Generate Visual Studio project files`.
- Case sensitivity: avoid manually renaming Unreal assets only by letter case on Windows.
- Line endings: keep Git-managed text files normalized; avoid mass CRLF rewrites.
- Editor rebuild prompt loop: close Unreal, delete local `Binaries/` and `Intermediate/`, then rebuild.
- Plugin not visible: confirm `Plugins/UnrealMcp/UnrealMcp.uplugin` exists and the plugin is enabled in the project.
- Python tools unavailable: confirm Unreal's Python Script Plugin is enabled.
- AI requests fail but direct commands work: verify API key, model name, rate limits, and request timeout settings.

Verify MCP from Windows PowerShell with `curl.exe`:

```powershell
curl.exe -s `
  -H "Content-Type: application/json" `
  -H "Accept: application/json, text/event-stream" `
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"unreal.editor_status\",\"arguments\":{}}}" `
  http://127.0.0.1:8765/mcp
```

Check whether port `8765` is already in use:

```powershell
netstat -ano | findstr :8765
```

If another process owns the port, either stop that process or change the Unreal MCP port in:

```text
Edit > Project Settings > Plugins > Unreal MCP
```

### 5. Confirm Plugin Settings

In Unreal Editor, open:

```text
Edit > Project Settings > Plugins > Unreal MCP
```

Recommended defaults:

- MCP server enabled.
- Host: `127.0.0.1`
- Port: `8765`
- Endpoint path: `/mcp`
- Auth token: empty for local-only testing, or configured if another app will connect.

The plugin is editor-focused. Keep the endpoint bound to localhost unless you intentionally know how you want to secure remote access.

### 6. Verify The MCP Endpoint

With the Unreal Editor running, test from a terminal:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"curl","version":"1.0"}}}' \
  http://127.0.0.1:8765/mcp
```

List available tools:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
  http://127.0.0.1:8765/mcp
```

Call a basic status tool:

```bash
curl -s \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"unreal.editor_status","arguments":{}}}' \
  http://127.0.0.1:8765/mcp
```

### 7. Use The In-Editor Chat

Open:

```text
Window > Unreal MCP Chat
```

Try direct commands:

```text
/status
/maps
/assets /Game/TopDown
/map_check
```

Try a direct tool call:

```text
/tool unreal.editor_status {}
```

### 8. Optional AI Assistant Setup

To use natural-language AI requests from the chat panel, configure:

```text
Project Settings > Plugins > Unreal MCP > AI
```

Set:

- Enable AI Assistant.
- OpenAI Responses URL.
- OpenAI API Key.
- OpenAI Model.
- AI Max Tool Rounds.
- AI Request Timeout Seconds.

Keep API keys local. Do not commit editor user settings or key files.

### 9. Troubleshooting

- If `/status` works but AI requests fail, check the AI settings and API key.
- If `curl` cannot connect, confirm the Unreal Editor is open and the plugin is enabled.
- If tools time out during heavy editor work, increase the AI request/activity timeout settings.
- If assets are missing, run `git lfs pull`.
- If the plugin does not compile, rebuild `MyProjectEditor` from the command line and inspect the UnrealBuildTool log.

## Git / Repository Notes

This repository uses Git LFS for Unreal binary assets:

- `.uasset`
- `.umap`
- `.ubulk`
- `.uexp`

Generated Unreal folders are intentionally ignored:

- `Binaries/`
- `Intermediate/`
- `Saved/`
- `DerivedDataCache/`
- plugin build caches

Do not commit local API keys, editor user settings, logs, or generated local test assets.

## AI / API Key Safety

Unreal MCP can connect to the OpenAI Responses API from inside the editor. Configure API keys locally in:

```text
Project Settings > Plugins > Unreal MCP > AI
```

Do not commit API keys to Git. User-specific editor settings are ignored by `.gitignore`.

## License Notice

No standalone open-source license file has been added yet.

Important:

- Unreal Engine, Epic template assets, Starter Content, Mannequin assets, and other Epic-provided content remain governed by the Epic Unreal Engine EULA.
- Project-specific source code and original plugin code should receive a clear license before others rely on this repository for reuse.
- If this repository is intended to stay public, a common next step is to add an MIT license for original project/plugin code plus a notice excluding Epic/third-party content from that license.

## Repository

GitHub:

```text
https://github.com/edwinmeng163-oss/UEvolve
```
