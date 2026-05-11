# UEvolve Codex Bridge

This is the P7.A bridge daemon between the Codex App Server protocol and the
simple WebSocket API that the Unreal plugin will consume in P7.B.

The bridge starts its own `codex app-server` subprocess on a temporary Unix
socket, initializes the App Server protocol, creates one Codex thread, then
listens for UE-facing WebSocket messages on:

```text
ws://127.0.0.1:8766/uevolve
```

## Wire Framing Decision

Codex CLI version tested:

```text
codex-cli 0.130.0
```

The Unix listener created by:

```bash
codex app-server --listen unix:///tmp/uevolve-codex/srv.sock
```

is not raw newline JSON, LSP `Content-Length`, 4-byte length-prefixed JSON, or
concatenated JSON. Direct `net.connect()` attempts with those four framings were
closed by the server with zero response bytes.

The open-source Codex transport implementation confirms the `unix://` listener
is a WebSocket upgrade endpoint over a Unix domain socket. After the HTTP
WebSocket upgrade, each App Server JSON message is one WebSocket text frame. The
payload is the lightweight Codex JSON-RPC object:

```json
{"id":"1","method":"initialize","params":{...}}
```

Codex does not send or require the standard `jsonrpc:"2.0"` field. For stdio,
the same JSON object is newline-delimited; for Unix sockets, it is WebSocket text
framed. The probe log with raw bytes and successful streaming text is written to:

```text
/tmp/uevolve-codex-bridge-spike.log
```

## Start

From the repository root:

```bash
bun run --cwd Tools/UnrealMcpCodexBridge src/index.ts
```

Expected startup output:

```text
UEvolve Codex Bridge listening at ws://127.0.0.1:8766/uevolve
Codex model=gpt-5.5 effort=xhigh approvalPolicy=reject log=/tmp/uevolve-codex-bridge-<pid>.log
```

Stop with `Ctrl-C`. On shutdown, the bridge interrupts any in-flight Codex turn,
closes connected WebSocket clients, and terminates its spawned app-server
subprocess.

## Configuration

Environment variables:

```text
UEVOLVE_CODEX_BRIDGE_PORT=8766
UEVOLVE_CODEX_BRIDGE_PATH=/uevolve
UEVOLVE_CODEX_CWD=<working directory for Codex turns; default is repo root>
UEVOLVE_CODEX_APPROVAL_POLICY=reject|auto-approve
UEVOLVE_MCP_NAME=unrealmcp
UEVOLVE_MCP_URL=http://127.0.0.1:8765/mcp
UEVOLVE_DISABLE_AUTO_MCP_REGISTER=1
UEVOLVE_MCP_BEARER=<future UE MCP bearer token>
```

The bridge hard-codes:

```text
model: gpt-5.5
reasoning effort: xhigh
thread approvalPolicy: on-request
thread sandbox: workspace-write
```

`workspace-write` is the narrowest App Server sandbox mode above `read-only`.
It is used so the app-server can load and call the local HTTP MCP server, while
the bridge approval handler still denies Codex's built-in file, shell, and
permission paths.

The daemon writes all App Server JSON sent and received to:

```text
/tmp/uevolve-codex-bridge-<pid>.log
```

## MCP Tool Bridging

By default, the bridge starts Codex with the running Unreal MCP endpoint
registered as an App Server MCP server:

```bash
codex app-server --listen unix://<sock> -c mcp_servers.unrealmcp.url="http://127.0.0.1:8765/mcp"
```

That makes Codex see the Unreal tool inventory, including tools such as
`unreal.editor_status`, `unreal.execute_python`, `unreal.spawn_actor`, and the
self-extension `unreal.mcp_*` tools exposed by the editor.

Override the registration with:

```text
UEVOLVE_MCP_NAME=<config key; default unrealmcp>
UEVOLVE_MCP_URL=<HTTP MCP endpoint; default http://127.0.0.1:8765/mcp>
UEVOLVE_DISABLE_AUTO_MCP_REGISTER=1
```

If the Unreal MCP endpoint later enables bearer authentication, set:

```text
UEVOLVE_MCP_BEARER=<token>
```

The bridge passes that value as
`mcp_servers.<name>.bearer_token="<token>"`. Be aware that command-line
configuration can be visible in process listings on local development machines.

Codex CLI 0.130.0 `codex mcp add --help` exposes HTTP URL and bearer-token
configuration, but it does not expose a per-server `trusted` or `auto_approve`
field. The bridge therefore does not set one. Unreal MCP tool execution is
delegated to the UE MCP server, whose audit, dry-run, backup, test, and rollback
layers own Unreal-side safety.

## UE-Facing WebSocket Protocol

Client to bridge:

```json
{"type":"start_turn","requestId":"<uuid>","prompt":"...","context":"optional"}
{"type":"cancel","requestId":"<uuid>"}
{"type":"steer","requestId":"<uuid>","instruction":"..."}
```

Bridge to client:

```json
{"type":"health","state":"ready"}
{"type":"text_delta","requestId":"<uuid>","text":"..."}
{"type":"tool_started","requestId":"<uuid>","toolName":"...","toolCallId":"...","args":{}}
{"type":"tool_finished","requestId":"<uuid>","toolCallId":"...","text":"...","isError":false}
{"type":"turn_complete","requestId":"<uuid>","fullText":"..."}
{"type":"error","requestId":"<uuid>","message":"..."}
```

## Approval Policy

Default policy is `reject`. `auto-approve` is retained as an operator label for
compatibility, but built-in OS-level capabilities are rejected in both modes.

The bridge rejects or withholds all Codex requests that would write files, run
commands, change permissions, request user input, or elicit external MCP input:

```text
applyPatchApproval
execCommandApproval
item/commandExecution/requestApproval
item/fileChange/requestApproval
item/permissions/requestApproval
mcpServer/elicitation/request
item/tool/requestUserInput
item/tool/call
```

Codex CLI 0.130.0 routes MCP tool-call confirmation through
`mcpServer/elicitation/request` with `_meta.codex_approval_kind` set to
`mcp_tool_call`. The bridge accepts only that narrow MCP tool-call approval when
`serverName` matches `UEVOLVE_MCP_NAME`; all other MCP elicitations remain
declined.

This is intentional for the Unreal workflow: Codex should mutate the Unreal
project through MCP tools, not through raw shell commands or direct file edits.

## Smoke Test

With the daemon running:

```bash
bun run --cwd Tools/UnrealMcpCodexBridge test-client.ts
```

The test client connects to `ws://127.0.0.1:8766/uevolve`, sends:

```text
List three Unreal Engine 5 actor classes
```

and exits with code `0` when it receives a non-empty `turn_complete.fullText`.
A captured smoke transcript is available at:

```text
/tmp/uevolve-codex-bridge-smoke.log
```

To verify direct Unreal MCP tool use with the editor running:

```bash
bun run --cwd Tools/UnrealMcpCodexBridge test-mcp-roundtrip.ts
```

The roundtrip test asks Codex to call `unreal.editor_status`, requires matching
`tool_started` and `tool_finished` bridge events, and exits with code `0` only
after a non-empty `turn_complete`.

## Known Limitations

- The bridge always spawns a fresh `codex app-server` subprocess. Connecting to
  a running Codex Desktop IPC socket, such as `ipc-501.sock`, is deferred to v2.
- The bridge does not auto-restart the app-server subprocess. If Codex exits,
  health becomes `failed`.
- V1 supports one active turn at a time on the cached thread.
- The bridge requires a running Unreal Editor with the Unreal MCP plugin loaded
  before MCP tool calls can succeed.
