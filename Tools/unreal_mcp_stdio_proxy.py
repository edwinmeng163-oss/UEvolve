#!/usr/bin/env python3

import json
import os
import sys
import urllib.error
import urllib.request


SERVER_URL = os.environ.get("UNREAL_MCP_URL", "http://127.0.0.1:8765/mcp")
PROTOCOL_VERSION = os.environ.get("UNREAL_MCP_PROTOCOL_VERSION", "2025-06-18")
AUTH_TOKEN = os.environ.get("UNREAL_MCP_AUTH_TOKEN", "")


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def build_headers(message: dict) -> dict:
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
    }

    if message.get("method") != "initialize":
        headers["MCP-Protocol-Version"] = PROTOCOL_VERSION

    if AUTH_TOKEN:
        headers["Authorization"] = f"Bearer {AUTH_TOKEN}"

    return headers


def send_message(message: dict) -> str | None:
    payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
    request = urllib.request.Request(
        SERVER_URL,
        data=payload,
        headers=build_headers(message),
        method="POST",
    )

    try:
        with urllib.request.urlopen(request) as response:
            if response.status in (202, 204):
                return None

            body = response.read().decode("utf-8", errors="replace")
            if not body:
                return None

            return body
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        if body:
            try:
                json.loads(body)
                return body
            except json.JSONDecodeError:
                pass

        error_response = {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {
                "code": -32000,
                "message": f"HTTP {exc.code} from Unreal MCP endpoint",
            },
        }
        return json.dumps(error_response, separators=(",", ":"))
    except Exception as exc:  # noqa: BLE001
        error_response = {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {
                "code": -32001,
                "message": f"Failed to reach Unreal MCP endpoint: {exc}",
            },
        }
        return json.dumps(error_response, separators=(",", ":"))


def main() -> int:
    global PROTOCOL_VERSION  # noqa: PLW0603

    log(f"Unreal MCP stdio proxy forwarding to {SERVER_URL}")

    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue

        try:
            message = json.loads(line)
        except json.JSONDecodeError as exc:
            log(f"Ignoring invalid JSON line: {exc}")
            continue

        response_text = send_message(message)
        if not response_text:
            continue

        try:
            response_json = json.loads(response_text)
        except json.JSONDecodeError:
            log(f"Endpoint returned non-JSON data: {response_text}")
            continue

        if message.get("method") == "initialize":
            result = response_json.get("result") or {}
            negotiated = result.get("protocolVersion")
            if isinstance(negotiated, str) and negotiated:
                PROTOCOL_VERSION = negotiated

        print(json.dumps(response_json, separators=(",", ":")), flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
