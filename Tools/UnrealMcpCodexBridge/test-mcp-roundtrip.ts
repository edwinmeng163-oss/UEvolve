const url = process.env.UEVOLVE_CODEX_BRIDGE_URL ?? "ws://127.0.0.1:8766/uevolve";
const requestId = crypto.randomUUID();
const editorStatusCallIds = new Set<string>();
const transcript: any[] = [];
let completed = false;
let fullText = "";

const ws = new WebSocket(url);
const timeout = setTimeout(() => {
  console.error("Timed out waiting for MCP roundtrip");
  process.exit(1);
}, 240000);

ws.addEventListener("open", () => {
  ws.send(
    JSON.stringify({
      type: "start_turn",
      requestId,
      prompt: "Call the unreal.editor_status MCP tool with no arguments and report the editor world name from the result.",
    }),
  );
});

ws.addEventListener("message", (event) => {
  const message = JSON.parse(String(event.data));
  transcript.push(message);
  console.log(JSON.stringify(message));
  if (message.type === "text_delta") fullText += message.text ?? "";
  if (message.type === "tool_started" && String(message.toolName ?? "").includes("editor_status")) {
    editorStatusCallIds.add(String(message.toolCallId));
  }
  if (message.type === "tool_finished" && editorStatusCallIds.has(String(message.toolCallId))) {
    editorStatusCallIds.add(`${message.toolCallId}:finished`);
  }
  if (message.type === "turn_complete") {
    completed = true;
    fullText = message.fullText ?? fullText;
    clearTimeout(timeout);
    ws.close();
  }
  if (message.type === "error" && message.requestId === requestId) {
    clearTimeout(timeout);
    console.error(message.message);
    process.exit(1);
  }
});

ws.addEventListener("close", () => {
  const toolStarted = [...editorStatusCallIds].some((id) => !id.endsWith(":finished"));
  const toolFinished = [...editorStatusCallIds].some((id) => id.endsWith(":finished"));
  const ok = toolStarted && toolFinished && completed && fullText.trim().length > 0;
  console.error(JSON.stringify({ toolStarted, toolFinished, completed, finalTextLength: fullText.trim().length }));
  process.exit(ok ? 0 : 1);
});
