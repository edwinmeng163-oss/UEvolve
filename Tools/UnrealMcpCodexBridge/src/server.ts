export type HealthState = "starting" | "ready" | "failed";
export type ClientMessage =
  | { type: "start_turn"; requestId: string; prompt: string; context?: string }
  | { type: "cancel"; requestId: string }
  | { type: "steer"; requestId: string; instruction: string };

type Handlers = {
  port: number;
  path: string;
  health: () => { state: HealthState; reason?: string };
  startTurn: (ws: any, message: Extract<ClientMessage, { type: "start_turn" }>) => void;
  cancel: (ws: any, message: Extract<ClientMessage, { type: "cancel" }>) => void;
  steer: (ws: any, message: Extract<ClientMessage, { type: "steer" }>) => void;
};

export function createBridgeServer(handlers: Handlers) {
  const clients = new Set<any>();
  const server = Bun.serve({
    port: handlers.port,
    fetch(request, server) {
      const url = new URL(request.url);
      if (url.pathname !== handlers.path) return new Response("Not found", { status: 404 });
      return server.upgrade(request) ? undefined : new Response("Upgrade failed", { status: 400 });
    },
    websocket: {
      open(ws) {
        clients.add(ws);
        ws.send(JSON.stringify({ type: "health", ...handlers.health() }));
      },
      message(ws, raw) {
        let message: ClientMessage;
        try {
          message = JSON.parse(String(raw));
        } catch {
          ws.send(JSON.stringify({ type: "error", requestId: null, message: "Invalid JSON" }));
          return;
        }
        if (message.type === "start_turn") handlers.startTurn(ws, message);
        else if (message.type === "cancel") handlers.cancel(ws, message);
        else if (message.type === "steer") handlers.steer(ws, message);
        else ws.send(JSON.stringify({ type: "error", requestId: (message as any).requestId, message: "Unknown message type" }));
      },
      close(ws) {
        clients.delete(ws);
      },
    },
  });
  return {
    url: `ws://127.0.0.1:${server.port}${handlers.path}`,
    close() {
      for (const ws of clients) ws.close(1001, "Bridge shutting down");
      server.stop();
    },
  };
}

export function send(ws: any, payload: any): void {
  ws.send(JSON.stringify(payload));
}
