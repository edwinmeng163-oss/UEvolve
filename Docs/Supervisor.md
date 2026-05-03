# External Supervisor

The supervisor is the outside-the-editor half of the self-extension loop. Editor Chat can request a build and write project memory, but it cannot close its own host process and continue executing. `Tools/unreal_mcp_supervisor.py` fills that gap.

By default, the repository supervisor targets the bundled example host at `Examples/UEvolveExample/UEvolveExample.uproject`. Pass `--uproject /path/to/YourProject.uproject` when using the plugin inside another Unreal project.

## What It Does

- Waits for the local MCP endpoint.
- Restarts Unreal Editor for this project.
- Runs `unreal.mcp_extension_pipeline`.
- If the pipeline says `requiresRestart=true`, restarts the editor and resumes tests from project memory.
- Writes timestamped logs under `Saved/UnrealMcp/SupervisorLogs` when `--log-dir` is used.
- Generates local macOS and Windows launchers through `install`.

## Direct Commands

```bash
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs wait
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs status
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs restart
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs resume-test --memory-key mcp.extension.pipeline --pipeline
python3 Tools/unreal_mcp_supervisor.py --log-dir Saved/UnrealMcp/SupervisorLogs pipeline --auto-restart --args-json '{"toolName":"unreal.my_custom_tool","memoryKey":"mcp.extension.pipeline"}'
```

`status` is non-destructive. It checks whether `tools/list` responds, reports which process owns the MCP port, and lists UnrealEditor PIDs matching the project. Use it when `wait`, `restart`, or post-build tests time out.

`restart` aborts if the previous Unreal Editor instance does not stop cleanly, instead of opening a second editor that cannot bind the MCP port. If you are sure there is no unsaved editor state to preserve, pass `--force-kill` to terminate stubborn editor processes after the normal shutdown attempts fail.

## Generate Local Launchers

From Editor Chat:

```text
/tool unreal.mcp_supervisor_install {"platform":"all","outputDir":"Tools/UnrealMcpSupervisor","memoryKey":"mcp.extension.pipeline"}
```

From a terminal:

```bash
python3 Tools/unreal_mcp_supervisor.py install \
  --platform all \
  --output-dir Tools/UnrealMcpSupervisor \
  --memory-key mcp.extension.pipeline \
  --args-json '{"memoryKey":"mcp.extension.pipeline"}' \
  --overwrite
```

Generated files under `Tools/UnrealMcpSupervisor/` are intentionally ignored because they contain machine-specific absolute paths. Versioned templates live in `Tools/UnrealMcpSupervisorTemplates/`.

## macOS LaunchAgent Flow

Generate launchers first, then install the plist:

```bash
mkdir -p "$HOME/Library/LaunchAgents"
cp Tools/UnrealMcpSupervisor/com.uevolve.supervisor.plist "$HOME/Library/LaunchAgents/"
launchctl unload "$HOME/Library/LaunchAgents/com.uevolve.supervisor.plist" 2>/dev/null || true
launchctl load "$HOME/Library/LaunchAgents/com.uevolve.supervisor.plist"
launchctl start "com.uevolve.supervisor"
```

For a one-shot local run:

```bash
./Tools/UnrealMcpSupervisor/run_unreal_mcp_pipeline.command
```

## Windows PowerShell Flow

Generate launchers on the Windows checkout, then run:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\Tools\UnrealMcpSupervisor\run_unreal_mcp_pipeline.ps1
```

If Python is not on `PATH`, install Python 3 from the Microsoft Store or python.org and confirm:

```powershell
py -3 --version
```

## Recovery

- If restart times out, open the project manually and run `resume-test`.
- If restart aborts with remaining UnrealEditor PIDs, close the editor manually or re-run with `--force-kill` only after saving/discarding editor state.
- If the endpoint times out while port `8765` has a listener, run `status`. A stale editor can hold the port while the new editor logs a bind failure.
- If tests fail after restart, run `unreal.mcp_pipeline_status`, `unreal.mcp_compile_error_fix_plan`, and `unreal.mcp_diff_last_apply`.
- If source is unsafe, run `unreal.mcp_rollback_to_manifest` with `dryRun=true` first.
- If two sessions collide, inspect `unreal.mcp_lock_extension_session {"mode":"status"}` and release only stale locks.
