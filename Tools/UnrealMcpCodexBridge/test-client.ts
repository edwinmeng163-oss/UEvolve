const url = process.env.UEVOLVE_CODEX_BRIDGE_URL ?? "ws://127.0.0.1:8766/uevolve";
const requestId = crypto.randomUUID();
let completed = false;
let fullText = "";

const ws = new WebSocket(url);
const timeout = setTimeout(() => {
  console.error("Timed out waiting for turn_complete");
  process.exit(1);
}, 180000);

ws.addEventListener("open", () => {
  ws.send(JSON.stringify({ type: "start_turn", requestId, prompt: "List three Unreal Engine 5 actor classes" }));
});

ws.addEventListener("message", (event) => {
  const message = JSON.parse(String(event.data));
  console.log(JSON.stringify(message));
  if (message.type === "text_delta") fullText += message.text ?? "";
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
  process.exit(completed && fullText.trim().length > 0 ? 0 : 1);
});
