import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { approvalModeFromEnv } from "./approval-policy";
import { CodexWsClient, startCodexAppServer, waitForSocket } from "./codex-protocol";
import { createBridgeServer, send, type HealthState } from "./server";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const cwd = process.env.UEVOLVE_CODEX_CWD ?? projectRoot;
const port = Number(process.env.UEVOLVE_CODEX_BRIDGE_PORT ?? 8766);
const wsPath = process.env.UEVOLVE_CODEX_BRIDGE_PATH ?? "/uevolve";
const model = "gpt-5.5";
const effort = "xhigh";
const codexApprovalPolicy = "on-request";
const sandbox = "workspace-write";
const approvalMode = approvalModeFromEnv();
const logPath = `/tmp/uevolve-codex-bridge-${process.pid}.log`;
let health: { state: HealthState; reason?: string } = { state: "starting" };
let threadId = "";
let codex: CodexWsClient;
const active = new Map<string, { requestId: string; ws: any; turnId: string; fullText: string }>();

function log(direction: string, payload: any): void {
  fs.appendFileSync(logPath, `${new Date().toISOString()} ${direction} ${JSON.stringify(payload)}\n`);
}

const child = startCodexAppServer((reason) => {
  health = { state: "failed", reason };
  console.error(reason);
});
child.proc.stdout.on("data", (chunk) => process.stdout.write(`[codex] ${chunk}`));
child.proc.stderr.on("data", (chunk) => process.stderr.write(`[codex] ${chunk}`));

function onNotification(message: any): void {
  const params = message.params ?? {};
  const turnId = params.turnId ?? params.turn?.id;
  const record = turnId ? active.get(turnId) : undefined;
  if (message.method === "item/agentMessage/delta" && record) {
    record.fullText += params.delta ?? "";
    send(record.ws, { type: "text_delta", requestId: record.requestId, text: params.delta ?? "" });
  } else if (message.method === "item/started" && record) {
    const item = params.item ?? {};
    if (isToolItem(item)) send(record.ws, { type: "tool_started", requestId: record.requestId, toolName: toolName(item), toolCallId: item.id, args: toolArgs(item) });
  } else if (message.method === "item/completed" && record) {
    const item = params.item ?? {};
    if (isToolItem(item)) send(record.ws, { type: "tool_finished", requestId: record.requestId, toolCallId: item.id, text: toolText(item), isError: toolError(item) });
  } else if (message.method === "turn/completed" && record) {
    const fullText = record.fullText || extractFinalText(params.turn);
    send(record.ws, { type: "turn_complete", requestId: record.requestId, fullText });
    active.delete(record.turnId);
  } else if (message.method === "error") {
    for (const record of active.values()) send(record.ws, { type: "error", requestId: record.requestId, message: params.message ?? JSON.stringify(params) });
  }
}

await waitForSocket(child.socketPath);
codex = new CodexWsClient(child.socketPath, log, approvalMode, onNotification);
await codex.connect();
await codex.initialize();
const mcpStatus = await codex.request("mcpServerStatus/list", { detail: "toolsAndAuthOnly" }).catch((error) => ({ error: String(error), data: [] }));
const started = await codex.request("thread/start", {
  model,
  cwd,
  approvalPolicy: codexApprovalPolicy,
  sandbox,
  config: { model_reasoning_effort: effort },
  ephemeral: true,
  sessionStartSource: "startup",
  developerInstructions: developerInstructions(child.mcpRegistration.name),
});
threadId = started.thread.id;
health = { state: "ready" };

const bridge = createBridgeServer({
  port,
  path: wsPath,
  health: () => health,
  async startTurn(ws, message) {
    if (health.state !== "ready") return send(ws, { type: "error", requestId: message.requestId, message: health.reason ?? "Bridge is not ready" });
    if (active.size) return send(ws, { type: "error", requestId: message.requestId, message: "Another Codex turn is already in flight" });
    try {
      const text = message.context ? `${message.context}\n\n${message.prompt}` : message.prompt;
      const result = await codex.request("turn/start", {
        threadId,
        input: [{ type: "text", text, text_elements: [] }],
        model,
        effort,
        approvalPolicy: codexApprovalPolicy,
      });
      const turnId = result.turn.id;
      active.set(turnId, { requestId: message.requestId, ws, turnId, fullText: "" });
    } catch (error) {
      send(ws, { type: "error", requestId: message.requestId, message: String(error) });
    }
  },
  async cancel(ws, message) {
    const record = [...active.values()].find((entry) => entry.requestId === message.requestId);
    if (!record) return send(ws, { type: "error", requestId: message.requestId, message: "No active turn for requestId" });
    await codex.request("turn/interrupt", { threadId, turnId: record.turnId }).catch((error) => send(ws, { type: "error", requestId: message.requestId, message: String(error) }));
  },
  async steer(ws, message) {
    const record = [...active.values()].find((entry) => entry.requestId === message.requestId);
    if (!record) return send(ws, { type: "error", requestId: message.requestId, message: "No active turn for requestId" });
    await codex.request("turn/steer", { threadId, expectedTurnId: record.turnId, input: [{ type: "text", text: message.instruction, text_elements: [] }] }).catch((error) =>
      send(ws, { type: "error", requestId: message.requestId, message: String(error) }),
    );
  },
});

console.log(`UEvolve Codex Bridge listening at ${bridge.url}`);
console.log(`Codex app-server args: ${formatSpawnArgs(child.spawnArgs)}`);
console.log(`MCP registration: ${JSON.stringify(child.mcpRegistration)}`);
console.log(`MCP status: ${summarizeMcpStatus(mcpStatus)}`);
console.log(`Codex model=${model} effort=${effort} approvalPolicy=${approvalMode} sandbox=${sandbox} log=${logPath}`);

async function shutdown(): Promise<void> {
  for (const record of active.values()) await codex.request("turn/interrupt", { threadId, turnId: record.turnId }).catch(() => {});
  bridge.close();
  codex.close();
  child.proc.kill("SIGTERM");
  process.exit(0);
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

function isToolItem(item: any): boolean {
  return ["commandExecution", "fileChange", "mcpToolCall", "dynamicToolCall"].includes(item.type);
}
function toolName(item: any): string {
  return item.tool ?? item.command ?? item.type;
}
function toolArgs(item: any): any {
  return item.arguments ?? { command: item.command, cwd: item.cwd, changes: item.changes };
}
function toolText(item: any): string {
  return item.aggregatedOutput ?? JSON.stringify(item.result ?? item.contentItems ?? item.error ?? "");
}
function toolError(item: any): boolean {
  return Boolean(item.error) || item.success === false || (typeof item.exitCode === "number" && item.exitCode !== 0);
}
function extractFinalText(turn: any): string {
  return (turn?.items ?? []).filter((item: any) => item.type === "agentMessage").map((item: any) => item.text).join("");
}
function developerInstructions(mcpName: string): string {
  return `You are an AI assistant embedded inside the Unreal Editor through a bridge. Use the ${mcpName} MCP server's tools to inspect and mutate the project. Prefer the smallest safe set of tool calls, inspect before concluding for read-only questions, act directly for clear modification requests, and avoid destructive actions unless explicitly asked. Do not attempt to write files outside MCP tool calls or run shell commands; those paths are disabled.`;
}
function formatSpawnArgs(args: string[]): string {
  return args.map((arg) => (arg.includes("bearer_token=") ? arg.replace(/bearer_token=.*/, 'bearer_token="<redacted>"') : arg)).join(" ");
}
function summarizeMcpStatus(status: any): string {
  if (status?.error) return status.error;
  const servers = status?.data ?? [];
  return servers.map((server: any) => `${server.name}:${Object.keys(server.tools ?? {}).length} tools:${server.authStatus}`).join(", ") || "none";
}
