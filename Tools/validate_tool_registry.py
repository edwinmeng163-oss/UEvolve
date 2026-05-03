#!/usr/bin/env python3
"""Validate UEvolve's explicit MCP ToolRegistry files.

This intentionally uses only the Python standard library so Windows/macOS users
can run it before opening Unreal Editor or as a lightweight CI step.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REGISTRY_PATH = ROOT / "Tools" / "UnrealMcpToolRegistry" / "tools.json"
MIRROR_PATH = ROOT / "Plugins" / "UnrealMcp" / "Resources" / "ToolRegistry" / "tools.json"
HANDLER_REGISTRY_PATH = ROOT / "Plugins" / "UnrealMcp" / "Source" / "UnrealMcp" / "Private" / "UnrealMcpToolHandlerRegistry.cpp"
KNOWN_CATEGORIES = {
    "actors",
    "blueprint",
    "editor",
    "memory",
    "scaffold",
    "self-extension",
    "skills",
    "widget",
}
REQUIRED_FIELDS = {
    "name",
    "category",
    "handlerName",
    "exposure",
    "riskLevel",
    "requiresWrite",
    "requiresBuild",
    "requiresExternalProcess",
    "requiresRestart",
    "requiresProjectMemory",
    "requiresLock",
    "dryRunSupport",
    "preflightSupport",
    "postcheckSupport",
    "testCoverage",
    "owner",
    "docsPath",
    "reason",
}


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def docs_file_exists(docs_path: str) -> bool:
    file_part = docs_path.split("#", 1)[0]
    return bool(file_part) and (ROOT / file_part).exists()


def main() -> int:
    issues: list[str] = []
    registry = load_json(REGISTRY_PATH)
    mirror = load_json(MIRROR_PATH)
    tools = registry.get("tools", [])
    mirror_tools = mirror.get("tools", [])
    handler_registry_text = HANDLER_REGISTRY_PATH.read_text(encoding="utf-8")
    handler_entries = {
        match.group("name"): match.group("category")
        for match in re.finditer(
            r'MakeHandlerEntry\(TEXT\("(?P<name>[^"]+)"\),\s*TEXT\("(?P<category>[^"]+)"\)',
            handler_registry_text,
        )
    }

    if [tool.get("name") for tool in tools] != [tool.get("name") for tool in mirror_tools]:
        issues.append("Registry mirror tool names/order differ from Tools/UnrealMcpToolRegistry/tools.json.")

    seen: dict[str, int] = {}
    for index, tool in enumerate(tools):
        name = tool.get("name", f"<missing-name-{index}>")
        seen[name] = seen.get(name, 0) + 1
        missing = sorted(REQUIRED_FIELDS.difference(tool))
        if missing:
            issues.append(f"{name}: missing required fields: {', '.join(missing)}")
        if tool.get("category") not in KNOWN_CATEGORIES:
            issues.append(f"{name}: unknown category {tool.get('category')!r}")
        if tool.get("exposure") not in {"visible", "legacy_hidden"}:
            issues.append(f"{name}: invalid exposure {tool.get('exposure')!r}")
        if tool.get("riskLevel") not in {"read_only", "low", "medium", "high", "critical"}:
            issues.append(f"{name}: invalid riskLevel {tool.get('riskLevel')!r}")
        if tool.get("testCoverage") not in {"missing", "core", "category", "manual", "external"}:
            issues.append(f"{name}: invalid testCoverage {tool.get('testCoverage')!r}")
        if tool.get("requiresWrite") and not (tool.get("preflightSupport") and tool.get("postcheckSupport")):
            issues.append(f"{name}: write-capable tools should enable preflightSupport and postcheckSupport.")
        if not docs_file_exists(str(tool.get("docsPath", ""))):
            issues.append(f"{name}: docsPath file does not exist: {tool.get('docsPath')!r}")
        handler_name = str(tool.get("handlerName", ""))
        if handler_name not in handler_entries:
            issues.append(f"{name}: handlerName {handler_name!r} is missing from UnrealMcpToolHandlerRegistry.cpp")
        elif handler_entries[handler_name] != tool.get("category"):
            issues.append(
                f"{name}: handlerName {handler_name!r} category mismatch: "
                f"handler registry has {handler_entries[handler_name]!r}, tool registry has {tool.get('category')!r}"
            )

    for name, count in sorted(seen.items()):
        if count > 1:
            issues.append(f"{name}: duplicate registry entries: {count}")

    summary = {
        "toolCount": len(tools),
        "mirrorToolCount": len(mirror_tools),
        "handlerCount": len(handler_entries),
        "issueCount": len(issues),
        "categories": sorted({tool.get("category", "") for tool in tools}),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    if issues:
        print("\nIssues:", file=sys.stderr)
        for issue in issues:
            print(f"- {issue}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
