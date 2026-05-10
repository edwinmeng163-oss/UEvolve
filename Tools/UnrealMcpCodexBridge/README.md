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
```

The bridge hard-codes:

```text
model: gpt-5.5
reasoning effort: xhigh
thread approvalPolicy: on-request
thread sandbox: read-only
```

The daemon writes all App Server JSON sent and received to:

```text
/tmp/uevolve-codex-bridge-<pid>.log
```

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

Default policy is `reject`.

V1 rejects or withholds all Codex requests that would write files, run commands,
change permissions, request user input, or elicit external MCP input:

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

This is intentional for the Unreal workflow: Codex is used as a reasoning and
text-generation service, while Unreal project mutation remains under the UE MCP
tooling, audit, dry-run, backup, build, test, and rollback path.

For local bridge development only, set:

```bash
UEVOLVE_CODEX_APPROVAL_POLICY=auto-approve
```

This auto-accepts command/file approval requests and grants requested permission
profiles for the current turn. It is not the default and should not be used for
normal UE project work.

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

## Known Limitations

- The bridge always spawns a fresh `codex app-server` subprocess. Connecting to
  a running Codex Desktop IPC socket, such as `ipc-501.sock`, is deferred to v2.
- The bridge does not auto-restart the app-server subprocess. If Codex exits,
  health becomes `failed`.
- V1 supports one active turn at a time on the cached thread.
- The UE plugin does not consume this bridge yet. That integration is P7.B.
