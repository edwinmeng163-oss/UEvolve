import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { approvalResponse, type ApprovalMode } from "./approval-policy";

export type Logger = (direction: string, payload: any) => void;
type Pending = { resolve: (value: any) => void; reject: (error: Error) => void };
type ConfigOverrides = Record<string, string>;

type McpRegistration = {
  enabled: boolean;
  name: string;
  url: string;
  bearerConfigured: boolean;
};

type AppServerOptions = {
  configOverrides?: ConfigOverrides;
};

export class CodexWsClient {
  private socket?: net.Socket;
  private buffer = Buffer.alloc(0);
  private upgraded = false;
  private nextId = 1;
  private pending = new Map<string, Pending>();

  constructor(
    private socketPath: string,
    private log: Logger,
    private mode: ApprovalMode,
    private onNotification: (message: any) => void,
  ) {}

  async connect(): Promise<void> {
    await new Promise<void>((resolve, reject) => {
      const sock = net.connect(this.socketPath);
      this.socket = sock;
      const key = crypto.randomBytes(16).toString("base64");
      const timer = setTimeout(() => reject(new Error("Timed out upgrading Codex socket")), 10000);
      sock.once("connect", () => {
        sock.write(
          `GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n\r\n`,
        );
      });
      sock.on("data", (chunk) => {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        if (!this.upgraded) {
          const end = this.buffer.indexOf("\r\n\r\n");
          if (end < 0) return;
          const header = this.buffer.subarray(0, end).toString("utf8");
          if (!header.startsWith("HTTP/1.1 101")) {
            reject(new Error(`Codex WebSocket upgrade failed: ${header}`));
            sock.destroy();
            return;
          }
          this.upgraded = true;
          this.buffer = this.buffer.subarray(end + 4);
          clearTimeout(timer);
          resolve();
        }
        this.readFrames();
      });
      sock.once("error", reject);
      sock.once("close", () => this.failAll(new Error("Codex app-server socket closed")));
    });
  }

  async initialize(): Promise<any> {
    const result = await this.request("initialize", {
      clientInfo: { name: "uevolve-codex-bridge", title: "UEvolve Codex Bridge", version: "0.1.0" },
      capabilities: { experimentalApi: true },
    });
    this.notify("initialized", {});
    return result;
  }

  request(method: string, params: any = {}): Promise<any> {
    const id = String(this.nextId++);
    this.send({ id, method, params });
    return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
  }

  notify(method: string, params: any = {}): void {
    this.send({ method, params });
  }

  close(): void {
    this.socket?.end();
  }

  private send(message: any): void {
    const json = JSON.stringify(message);
    this.log("send", message);
    this.socket?.write(encodeClientTextFrame(json));
  }

  private readFrames(): void {
    while (this.buffer.length >= 2) {
      const b1 = this.buffer[0];
      const b2 = this.buffer[1];
      let len = b2 & 0x7f;
      let offset = 2;
      if (len === 126) {
        if (this.buffer.length < 4) return;
        len = this.buffer.readUInt16BE(2);
        offset = 4;
      } else if (len === 127) {
        if (this.buffer.length < 10) return;
        len = Number(this.buffer.readBigUInt64BE(2));
        offset = 10;
      }
      const masked = (b2 & 0x80) !== 0;
      const maskOffset = offset;
      if (masked) offset += 4;
      if (this.buffer.length < offset + len) return;
      let payload = this.buffer.subarray(offset, offset + len);
      if (masked) {
        const mask = this.buffer.subarray(maskOffset, maskOffset + 4);
        payload = Buffer.from(payload.map((value, index) => value ^ mask[index % 4]));
      }
      this.buffer = this.buffer.subarray(offset + len);
      const opcode = b1 & 0x0f;
      if (opcode === 1) this.handleJson(JSON.parse(payload.toString("utf8")));
      if (opcode === 8) this.socket?.end();
      if (opcode === 9) this.socket?.write(Buffer.from([0x8a, 0x00]));
    }
  }

  private handleJson(message: any): void {
    this.log("recv", message);
    if (message.id != null && message.method) {
      this.send({ id: message.id, result: approvalResponse(message.method, message.params, this.mode) });
      return;
    }
    if (message.id != null) {
      const waiter = this.pending.get(String(message.id));
      if (!waiter) return;
      this.pending.delete(String(message.id));
      message.error ? waiter.reject(new Error(message.error.message ?? JSON.stringify(message.error))) : waiter.resolve(message.result);
      return;
    }
    if (message.method) this.onNotification(message);
  }

  private failAll(error: Error): void {
    for (const waiter of this.pending.values()) waiter.reject(error);
    this.pending.clear();
  }
}

export function startCodexAppServer(
  onExit: (reason: string) => void,
  options: AppServerOptions = {},
): { proc: ChildProcessWithoutNullStreams; socketPath: string; dir: string; spawnArgs: string[]; mcpRegistration: McpRegistration } {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "uevolve-codex-bridge-"));
  const socketPath = path.join(dir, "codex.sock");
  const spawnArgs = ["app-server", "--listen", `unix://${socketPath}`];
  const mcpRegistration = mcpRegistrationFromEnv();
  const configOverrides: ConfigOverrides = {};
  if (mcpRegistration.enabled) {
    configOverrides[`mcp_servers.${mcpRegistration.name}.url`] = tomlString(mcpRegistration.url);
    const bearer = process.env.UEVOLVE_MCP_BEARER;
    if (bearer) configOverrides[`mcp_servers.${mcpRegistration.name}.bearer_token`] = tomlString(bearer);
  }
  Object.assign(configOverrides, options.configOverrides ?? {});
  for (const [key, value] of Object.entries(configOverrides)) spawnArgs.push("-c", `${key}=${value}`);
  const proc = spawn("codex", spawnArgs, { stdio: ["ignore", "pipe", "pipe"] });
  proc.once("exit", (code, signal) => onExit(`codex app-server exited code=${code} signal=${signal}`));
  return { proc, socketPath, dir, spawnArgs, mcpRegistration };
}

export async function waitForSocket(socketPath: string): Promise<void> {
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline) {
    if (fs.existsSync(socketPath)) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`Timed out waiting for ${socketPath}`);
}

function encodeClientTextFrame(text: string): Buffer {
  const payload = Buffer.from(text);
  const header =
    payload.length < 126
      ? Buffer.from([0x81, 0x80 | payload.length])
      : payload.length < 65536
        ? Buffer.from([0x81, 0x80 | 126, payload.length >> 8, payload.length & 255])
        : Buffer.concat([Buffer.from([0x81, 0x80 | 127]), u64(payload.length)]);
  const mask = crypto.randomBytes(4);
  const masked = Buffer.alloc(payload.length);
  for (let i = 0; i < payload.length; i++) masked[i] = payload[i] ^ mask[i % 4];
  return Buffer.concat([header, mask, masked]);
}

function u64(value: number): Buffer {
  const buf = Buffer.alloc(8);
  buf.writeBigUInt64BE(BigInt(value));
  return buf;
}

function mcpRegistrationFromEnv(): McpRegistration {
  const name = process.env.UEVOLVE_MCP_NAME ?? "unrealmcp";
  if (!/^[A-Za-z0-9_-]+$/.test(name)) throw new Error(`Invalid UEVOLVE_MCP_NAME: ${name}`);
  return {
    enabled: process.env.UEVOLVE_DISABLE_AUTO_MCP_REGISTER !== "1",
    name,
    url: process.env.UEVOLVE_MCP_URL ?? "http://127.0.0.1:8765/mcp",
    bearerConfigured: Boolean(process.env.UEVOLVE_MCP_BEARER),
  };
}

function tomlString(value: string): string {
  return JSON.stringify(value);
}
