#!/usr/bin/env python3
"""External supervisor for Unreal MCP editor restart handoff.

This script intentionally lives outside Unreal Editor so it can close/reopen the
editor and then continue MCP extension tests after the plugin DLL/dylib reloads.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


PROJECT_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_UPROJECT = PROJECT_ROOT / "Examples" / "UEvolveExample" / "UEvolveExample.uproject"
DEFAULT_LAUNCH_AGENT_LABEL = "com.uevolve.supervisor"
DEFAULT_URL = os.environ.get("UNREAL_MCP_URL", "http://127.0.0.1:8765/mcp")
DEFAULT_PROTOCOL = os.environ.get("UNREAL_MCP_PROTOCOL_VERSION", "2025-06-18")
AUTH_TOKEN = os.environ.get("UNREAL_MCP_AUTH_TOKEN", "")
LOG_PATH: Path | None = None


def log(message: str) -> None:
    timestamped = f"[{datetime.now(timezone.utc).isoformat()}] {message}"
    print(timestamped, file=sys.stderr, flush=True)
    if LOG_PATH:
        LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
        with LOG_PATH.open("a", encoding="utf-8") as handle:
            handle.write(timestamped + "\n")


def setup_logging(args: argparse.Namespace) -> None:
    global LOG_PATH
    log_file = getattr(args, "log_file", "") or ""
    log_dir = getattr(args, "log_dir", "") or ""
    if log_file:
        LOG_PATH = Path(log_file).expanduser().resolve()
    elif log_dir:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S")
        LOG_PATH = Path(log_dir).expanduser().resolve() / f"unreal_mcp_supervisor_{stamp}.log"


def log_json(event: str, payload: dict) -> None:
    if not LOG_PATH:
        return
    line = json.dumps(
        {
            "time": datetime.now(timezone.utc).isoformat(),
            "event": event,
            "payload": payload,
        },
        ensure_ascii=False,
        separators=(",", ":"),
    )
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with LOG_PATH.open("a", encoding="utf-8") as handle:
        handle.write(line + "\n")


def headers() -> dict[str, str]:
    result = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
        "MCP-Protocol-Version": DEFAULT_PROTOCOL,
    }
    if AUTH_TOKEN:
        result["Authorization"] = f"Bearer {AUTH_TOKEN}"
    return result


def rpc(url: str, method: str, params: dict | None = None, timeout: float = 30.0) -> dict:
    payload = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000) % 1_000_000,
        "method": method,
    }
    if params is not None:
        payload["params"] = params

    request = urllib.request.Request(
        url,
        data=json.dumps(payload, separators=(",", ":")).encode("utf-8"),
        headers=headers(),
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8", errors="replace"))


def call_tool(url: str, name: str, arguments: dict, timeout: float = 120.0) -> dict:
    return rpc(url, "tools/call", {"name": name, "arguments": arguments}, timeout=timeout)


def wait_endpoint(url: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    last_error = ""
    while time.time() < deadline:
        try:
            response = rpc(url, "tools/list", {}, timeout=2.0)
            if "result" in response:
                return True
        except Exception as exc:
            last_error = f"{type(exc).__name__}: {exc}"
            time.sleep(1.0)
    if last_error:
        log(f"Last endpoint probe error: {last_error}")
    return False


def endpoint_probe(url: str, timeout: float = 2.0) -> dict:
    try:
        response = rpc(url, "tools/list", {}, timeout=timeout)
        tools = response.get("result", {}).get("tools", [])
        return {
            "ready": "result" in response,
            "toolCount": len(tools) if isinstance(tools, list) else 0,
            "error": "",
        }
    except Exception as exc:
        return {
            "ready": False,
            "toolCount": 0,
            "error": f"{type(exc).__name__}: {exc}",
        }


def endpoint_port(url: str) -> int | None:
    parsed = urllib.parse.urlparse(url)
    try:
        return parsed.port
    except ValueError:
        return None


def find_port_listeners(port: int | None) -> list[dict]:
    if port is None:
        return []

    listeners: list[dict] = []
    if sys.platform == "win32":
        try:
            completed = subprocess.run(
                ["netstat", "-ano"],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
        except FileNotFoundError:
            return listeners

        marker = f":{port}"
        for line in completed.stdout.splitlines():
            if marker not in line or "LISTENING" not in line.upper():
                continue
            parts = line.split()
            listeners.append(
                {
                    "line": line.strip(),
                    "pid": parts[-1] if parts else "",
                    "command": "",
                }
            )
        return listeners

    try:
        completed = subprocess.run(
            ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError:
        return listeners

    for line in completed.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) < 2:
            continue
        listeners.append(
            {
                "line": line.strip(),
                "pid": parts[1],
                "command": parts[0],
            }
        )
    return listeners


def find_editor_pids(uproject: Path) -> list[int]:
    if sys.platform == "win32":
        script = (
            "$Target = "
            + powershell_single_quote(str(uproject))
            + "; Get-CimInstance Win32_Process | "
            + "Where-Object { $_.Name -like 'UnrealEditor*' -and $_.CommandLine -like ('*' + $Target + '*') } | "
            + "ForEach-Object { $_.ProcessId }"
        )
        try:
            completed = subprocess.run(
                ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
            )
        except FileNotFoundError:
            return []

        pids: list[int] = []
        for line in completed.stdout.splitlines():
            try:
                pids.append(int(line.strip()))
            except ValueError:
                pass
        return pids

    pattern = f"UnrealEditor.*{uproject}"
    try:
        completed = subprocess.run(
            ["pgrep", "-f", pattern],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except FileNotFoundError:
        return []

    pids: list[int] = []
    for line in completed.stdout.splitlines():
        try:
            pids.append(int(line.strip()))
        except ValueError:
            pass
    return pids


def collect_status(url: str, uproject: Path) -> dict:
    port = endpoint_port(url)
    return {
        "url": url,
        "uproject": str(uproject),
        "endpoint": endpoint_probe(url),
        "port": port,
        "portListeners": find_port_listeners(port),
        "editorPids": find_editor_pids(uproject),
    }


def log_status_diagnostics(url: str, uproject: Path) -> None:
    status = collect_status(url, uproject)
    log_json("status_diagnostics", status)
    endpoint = status.get("endpoint", {})
    log(f"Endpoint ready={endpoint.get('ready')} toolCount={endpoint.get('toolCount')} error={endpoint.get('error')}")
    listeners = status.get("portListeners", [])
    if listeners:
        log(f"Port listeners for {status.get('port')}: {listeners}")
    editor_pids = status.get("editorPids", [])
    if editor_pids:
        log(f"UnrealEditor PIDs for project: {editor_pids}")


def start_editor(uproject: Path, editor_cmd: str | None) -> None:
    if editor_cmd:
        subprocess.Popen([editor_cmd, str(uproject)])
        return

    if sys.platform == "darwin":
        subprocess.Popen(["open", str(uproject)])
    elif sys.platform == "win32":
        os.startfile(str(uproject))  # type: ignore[attr-defined]
    else:
        subprocess.Popen(["UnrealEditor", str(uproject)])


def terminate_pid(pid: int, *, force: bool = False) -> None:
    if sys.platform == "win32":
        command = ["taskkill", "/PID", str(pid)]
        if force:
            command.append("/F")
        subprocess.run(command, check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return

    try:
        os.kill(pid, signal.SIGKILL if force else signal.SIGTERM)
    except OSError:
        pass


def wait_until_editor_stopped(uproject: Path, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not find_editor_pids(uproject):
            return True
        time.sleep(1.0)
    return not find_editor_pids(uproject)


def stop_editor(uproject: Path, timeout: float, *, force_kill: bool = False) -> bool:
    if sys.platform == "darwin":
        subprocess.run(
            ["osascript", "-e", 'tell application "UnrealEditor" to quit'],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    if wait_until_editor_stopped(uproject, timeout):
        return True

    for pid in find_editor_pids(uproject):
        terminate_pid(pid)

    if wait_until_editor_stopped(uproject, min(timeout, 10.0)):
        return True

    remaining = find_editor_pids(uproject)
    if remaining and force_kill:
        log(f"Force killing stubborn UnrealEditor PIDs: {remaining}")
        for pid in remaining:
            terminate_pid(pid, force=True)
        return wait_until_editor_stopped(uproject, min(timeout, 10.0))

    if remaining:
        log(f"Unreal Editor did not stop cleanly; remaining PIDs: {remaining}")
    return not remaining


def print_json(data: dict) -> None:
    print(json.dumps(data, indent=2, ensure_ascii=False))
    log_json("json_output", data)


def shell_single_quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"


def powershell_single_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def xml_escape(value: str) -> str:
    return (
        value.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
        .replace("'", "&apos;")
    )


def write_text_file(path: Path, text: str, overwrite: bool) -> bool:
    if path.exists() and not overwrite:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def make_default_args_json(memory_key: str) -> str:
    return json.dumps({"memoryKey": memory_key}, separators=(",", ":"))


def build_supervisor_argv(
    *,
    uproject: Path,
    url: str,
    log_dir: Path,
    args_json: str,
    auto_restart: bool = True,
    editor_cmd: str = "",
) -> list[str]:
    argv = [
        "python3",
        str(PROJECT_ROOT / "Tools" / "unreal_mcp_supervisor.py"),
        "--url",
        url,
        "--uproject",
        str(uproject),
        "--log-dir",
        str(log_dir),
    ]
    if editor_cmd:
        argv.extend(["--editor-cmd", editor_cmd])
    argv.append("pipeline")
    if auto_restart:
        argv.append("--auto-restart")
    argv.extend(["--args-json", args_json])
    return argv


def render_macos_command(argv: list[str], project_root: Path) -> str:
    command = " ".join(shell_single_quote(part) for part in argv)
    return "\n".join(
        [
            "#!/bin/zsh",
            "set -euo pipefail",
            f"cd {shell_single_quote(str(project_root))}",
            command,
            "",
        ]
    )


def render_windows_powershell(
    *,
    uproject: Path,
    url: str,
    log_dir: Path,
    args_json: str,
    editor_cmd: str = "",
) -> str:
    editor_line = ""
    if editor_cmd:
        editor_line = f"  '--editor-cmd', {powershell_single_quote(editor_cmd)},`n"
    return f"""# Unreal MCP supervisor launcher for Windows PowerShell.
$ErrorActionPreference = "Stop"
$ProjectRoot = {powershell_single_quote(str(PROJECT_ROOT))}
Set-Location $ProjectRoot
$ArgsJson = {powershell_single_quote(args_json)}
py -3 .\\Tools\\unreal_mcp_supervisor.py `
  --url {powershell_single_quote(url)} `
  --uproject {powershell_single_quote(str(uproject))} `
  --log-dir {powershell_single_quote(str(log_dir))} `
{editor_line}  pipeline --auto-restart --args-json $ArgsJson
"""


def render_launch_agent_plist(
    *,
    label: str,
    argv: list[str],
    project_root: Path,
    log_dir: Path,
    launch_at_load: bool,
) -> str:
    stdout_path = log_dir / f"{label}.out.log"
    stderr_path = log_dir / f"{label}.err.log"
    program_args = "\n".join(f"    <string>{xml_escape(part)}</string>" for part in argv)
    run_at_load = "true" if launch_at_load else "false"
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>{xml_escape(label)}</string>
  <key>WorkingDirectory</key>
  <string>{xml_escape(str(project_root))}</string>
  <key>ProgramArguments</key>
  <array>
{program_args}
  </array>
  <key>RunAtLoad</key>
  <{run_at_load}/>
  <key>KeepAlive</key>
  <false/>
  <key>StandardOutPath</key>
  <string>{xml_escape(str(stdout_path))}</string>
  <key>StandardErrorPath</key>
  <string>{xml_escape(str(stderr_path))}</string>
</dict>
</plist>
"""


def render_supervisor_install_readme(
    *,
    output_dir: Path,
    label: str,
    memory_key: str,
    args_json: str,
) -> str:
    launch_agent_path = output_dir / f"{label}.plist"
    return f"""# Unreal MCP Supervisor Launchers

Generated helper files for running `Tools/unreal_mcp_supervisor.py` outside Unreal Editor.

These files contain local absolute paths and are normally ignored by Git. Shared path-neutral templates live in `Tools/UnrealMcpSupervisorTemplates/`, and the full operating guide lives in `Docs/Supervisor.md`.

## macOS

Run the shortcut command:

```bash
./run_unreal_mcp_pipeline.command
```

Load the LaunchAgent template manually:

```bash
mkdir -p "$HOME/Library/LaunchAgents"
cp "{launch_agent_path}" "$HOME/Library/LaunchAgents/{label}.plist"
launchctl unload "$HOME/Library/LaunchAgents/{label}.plist" 2>/dev/null || true
launchctl load "$HOME/Library/LaunchAgents/{label}.plist"
launchctl start "{label}"
```

## Windows

Run from PowerShell:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\\run_unreal_mcp_pipeline.ps1
```

## Pipeline Defaults

- `memoryKey`: `{memory_key}`
- `argsJson`: `{args_json}`

The supervisor can close/reopen Unreal Editor and resume tests after compiled MCP tools are loaded.

If the endpoint times out, run:

```bash
python3 Tools/unreal_mcp_supervisor.py status
```

This reports MCP readiness, port listeners, and matching UnrealEditor PIDs.

`restart` aborts if the previous editor process remains alive. Add `--force-kill` only after saving or intentionally discarding editor state.
"""


def command_wait(args: argparse.Namespace) -> int:
    if wait_endpoint(args.url, args.timeout):
        log("Unreal MCP endpoint is ready.")
        return 0
    log("Timed out waiting for Unreal MCP endpoint.")
    log_status_diagnostics(args.url, Path(args.uproject).expanduser().resolve())
    return 1


def command_status(args: argparse.Namespace) -> int:
    print_json(collect_status(args.url, Path(args.uproject).expanduser().resolve()))
    return 0


def command_restart(args: argparse.Namespace) -> int:
    uproject = Path(args.uproject).expanduser().resolve()
    log(f"Stopping Unreal Editor for {uproject}")
    if not stop_editor(uproject, args.stop_timeout, force_kill=args.force_kill):
        log_status_diagnostics(args.url, uproject)
        log("Restart aborted because the previous Unreal Editor instance is still running. Re-run with --force-kill only if unsaved editor state can be discarded.")
        return 1
    log(f"Starting Unreal Editor for {uproject}")
    start_editor(uproject, args.editor_cmd)
    return command_wait(args)


def command_resume_test(args: argparse.Namespace) -> int:
    if not wait_endpoint(args.url, args.timeout):
        log("Endpoint is not ready; cannot resume test.")
        return 1

    response = call_tool(
        args.url,
        "unreal.mcp_extension_pipeline" if args.pipeline else "unreal.mcp_run_tool_test",
        {
            "mode": "resume_test",
            "memoryKey": args.memory_key,
            "readProjectMemory": True,
            "writeProjectMemory": True,
        },
        timeout=args.call_timeout,
    )
    print_json(response)
    return 1 if response.get("result", {}).get("isError") else 0


def command_pipeline(args: argparse.Namespace) -> int:
    if not wait_endpoint(args.url, args.timeout):
        log("Endpoint is not ready; cannot run pipeline.")
        return 1

    try:
        pipeline_args = json.loads(args.args_json)
    except json.JSONDecodeError as exc:
        log(f"--args-json is not valid JSON: {exc}")
        return 1

    response = call_tool(args.url, "unreal.mcp_extension_pipeline", pipeline_args, timeout=args.call_timeout)
    print_json(response)

    structured = response.get("result", {}).get("structuredContent", {})
    if not args.auto_restart or not structured.get("requiresRestart"):
        return 1 if response.get("result", {}).get("isError") else 0

    log("Pipeline requires restart; restarting editor and resuming test.")
    restart_args = argparse.Namespace(**vars(args))
    restart_code = command_restart(restart_args)
    if restart_code != 0:
        log("Restart failed; not attempting resume-test.")
        return restart_code

    memory_key = pipeline_args.get("memoryKey") or structured.get("memoryKey") or "mcp.extension.pipeline"
    resume_args = argparse.Namespace(**vars(args))
    resume_args.memory_key = memory_key
    resume_args.pipeline = True
    return command_resume_test(resume_args)


def command_install(args: argparse.Namespace) -> int:
    uproject = Path(args.uproject).expanduser().resolve()
    output_dir = Path(args.output_dir).expanduser()
    if not output_dir.is_absolute():
        output_dir = (PROJECT_ROOT / output_dir).resolve()

    log_dir = Path(args.supervisor_log_dir).expanduser() if args.supervisor_log_dir else PROJECT_ROOT / "Saved" / "UnrealMcp" / "SupervisorLogs"
    if not log_dir.is_absolute():
        log_dir = (PROJECT_ROOT / log_dir).resolve()

    args_json = args.args_json or make_default_args_json(args.memory_key)
    label = args.label or DEFAULT_LAUNCH_AGENT_LABEL
    platform = args.platform.lower()
    generate_macos = platform in {"all", "macos", "darwin"}
    generate_windows = platform in {"all", "windows", "win32"}

    argv = build_supervisor_argv(
        uproject=uproject,
        url=args.url,
        log_dir=log_dir,
        args_json=args_json,
        auto_restart=not args.no_auto_restart,
        editor_cmd=args.editor_cmd,
    )

    generated: list[str] = []
    if generate_macos:
        command_path = output_dir / "run_unreal_mcp_pipeline.command"
        plist_path = output_dir / f"{label}.plist"
        write_text_file(command_path, render_macos_command(argv, PROJECT_ROOT), args.overwrite)
        command_path.chmod(0o755)
        write_text_file(
            plist_path,
            render_launch_agent_plist(
                label=label,
                argv=argv,
                project_root=PROJECT_ROOT,
                log_dir=log_dir,
                launch_at_load=args.launch_at_load,
            ),
            args.overwrite,
        )
        generated.extend([str(command_path), str(plist_path)])

        if args.install_launch_agent:
            launch_agent_dir = Path.home() / "Library" / "LaunchAgents"
            launch_agent_path = launch_agent_dir / f"{label}.plist"
            launch_agent_dir.mkdir(parents=True, exist_ok=True)
            launch_agent_path.write_text(plist_path.read_text(encoding="utf-8"), encoding="utf-8")
            generated.append(str(launch_agent_path))

    if generate_windows:
        ps_path = output_dir / "run_unreal_mcp_pipeline.ps1"
        write_text_file(
            ps_path,
            render_windows_powershell(
                uproject=uproject,
                url=args.url,
                log_dir=log_dir,
                args_json=args_json,
                editor_cmd=args.editor_cmd,
            ),
            args.overwrite,
        )
        generated.append(str(ps_path))

    readme_path = output_dir / "README.md"
    write_text_file(
        readme_path,
        render_supervisor_install_readme(
            output_dir=output_dir,
            label=label,
            memory_key=args.memory_key,
            args_json=args_json,
        ),
        args.overwrite,
    )
    generated.append(str(readme_path))

    print_json(
        {
            "generated": generated,
            "label": label,
            "outputDir": str(output_dir),
            "logDir": str(log_dir),
            "argsJson": args_json,
            "installLaunchAgent": args.install_launch_agent,
        }
    )
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=DEFAULT_URL, help="Unreal MCP endpoint URL.")
    parser.add_argument("--uproject", default=str(DEFAULT_UPROJECT), help="Path to the .uproject file.")
    parser.add_argument("--editor-cmd", default="", help="Optional UnrealEditor executable path.")
    parser.add_argument("--timeout", type=float, default=120.0, help="Endpoint wait timeout in seconds.")
    parser.add_argument("--call-timeout", type=float, default=180.0, help="MCP tool call timeout in seconds.")
    parser.add_argument("--stop-timeout", type=float, default=30.0, help="Graceful editor stop timeout in seconds.")
    parser.add_argument("--force-kill", action="store_true", help="Force-kill stubborn UnrealEditor processes during restart. Use only when unsaved editor state can be discarded.")
    parser.add_argument("--log-file", default="", help="Optional supervisor log file.")
    parser.add_argument("--log-dir", default="", help="Optional directory for timestamped supervisor logs.")

    subparsers = parser.add_subparsers(dest="command", required=True)

    wait_parser = subparsers.add_parser("wait", help="Wait until the MCP endpoint is ready.")
    wait_parser.set_defaults(func=command_wait)

    status_parser = subparsers.add_parser("status", help="Diagnose MCP endpoint readiness, port listeners, and matching UnrealEditor processes.")
    status_parser.set_defaults(func=command_status)

    restart_parser = subparsers.add_parser("restart", help="Restart Unreal Editor and wait for MCP.")
    restart_parser.set_defaults(func=command_restart)

    resume_parser = subparsers.add_parser("resume-test", help="Resume post-restart MCP tool testing from project memory.")
    resume_parser.add_argument("--memory-key", default="mcp.extension.pipeline")
    resume_parser.add_argument("--pipeline", action="store_true", help="Use mcp_extension_pipeline mode=resume_test instead of mcp_run_tool_test.")
    resume_parser.set_defaults(func=command_resume_test)

    pipeline_parser = subparsers.add_parser("pipeline", help="Run mcp_extension_pipeline, optionally restart and resume.")
    pipeline_parser.add_argument("--args-json", required=True, help="JSON object passed to unreal.mcp_extension_pipeline.")
    pipeline_parser.add_argument("--auto-restart", action="store_true", help="Restart Unreal Editor if the pipeline requires it, then resume test.")
    pipeline_parser.set_defaults(func=command_pipeline)

    install_parser = subparsers.add_parser("install", help="Generate macOS/Windows supervisor launcher templates.")
    install_parser.add_argument("--output-dir", default=str(PROJECT_ROOT / "Tools" / "UnrealMcpSupervisor"))
    install_parser.add_argument("--platform", default="all", choices=["all", "macos", "darwin", "windows", "win32"])
    install_parser.add_argument("--label", default="")
    install_parser.add_argument("--memory-key", default="mcp.extension.pipeline")
    install_parser.add_argument("--args-json", default="", help="Pipeline args JSON. Defaults to {\"memoryKey\": memoryKey}.")
    install_parser.add_argument("--supervisor-log-dir", default=str(PROJECT_ROOT / "Saved" / "UnrealMcp" / "SupervisorLogs"))
    install_parser.add_argument("--install-launch-agent", action="store_true", help="Also copy the plist into ~/Library/LaunchAgents.")
    install_parser.add_argument("--launch-at-load", action="store_true", help="Set RunAtLoad=true in the generated macOS LaunchAgent.")
    install_parser.add_argument("--no-auto-restart", action="store_true", help="Generate pipeline commands without --auto-restart.")
    install_parser.add_argument("--overwrite", action="store_true", default=True)
    install_parser.set_defaults(func=command_install)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    setup_logging(args)
    try:
        return args.func(args)
    except (urllib.error.URLError, TimeoutError, subprocess.SubprocessError, OSError) as exc:
        log(f"Supervisor failed: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
