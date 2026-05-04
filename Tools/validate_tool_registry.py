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
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
REGISTRY_PATH = ROOT / "Tools" / "UnrealMcpToolRegistry" / "tools.json"
SCHEMA_PATH = ROOT / "Tools" / "UnrealMcpToolRegistry" / "schema.json"
MIRROR_PATH = ROOT / "Plugins" / "UnrealMcp" / "Resources" / "ToolRegistry" / "tools.json"
HANDLER_REGISTRY_PATH = ROOT / "Plugins" / "UnrealMcp" / "Source" / "UnrealMcp" / "Private" / "UnrealMcpToolHandlerRegistry.cpp"
TESTS_PATH = ROOT / "Tools" / "UnrealMcpTests"
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
BOOLEAN_FIELDS = {
    "requiresWrite",
    "requiresBuild",
    "requiresExternalProcess",
    "requiresRestart",
    "requiresProjectMemory",
    "requiresLock",
    "dryRunSupport",
    "preflightSupport",
    "postcheckSupport",
}
KNOWN_EXPOSURES = {"visible", "legacy_hidden"}
KNOWN_RISK_LEVELS = {"read_only", "low", "medium", "high", "critical"}
KNOWN_TEST_COVERAGE = {"missing", "core", "category", "manual", "external"}


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def validate_registry_shape(registry: dict[str, Any], schema: dict[str, Any], issues: list[str]) -> None:
    """Small dependency-free schema check for CI and fresh workstations."""
    if schema.get("title") != "UEvolve MCP Tool Registry":
        issues.append("schema.json: unexpected or missing title.")
    if not isinstance(registry.get("schemaVersion"), int):
        issues.append("tools.json: schemaVersion must be an integer.")
    if not isinstance(registry.get("registryVersion"), str) or not registry.get("registryVersion", "").strip():
        issues.append("tools.json: registryVersion must be a non-empty string.")
    if not isinstance(registry.get("description"), str) or not registry.get("description", "").strip():
        issues.append("tools.json: description must be a non-empty string.")
    if not isinstance(registry.get("tools"), list):
        issues.append("tools.json: tools must be an array.")


def docs_file_exists(docs_path: str) -> bool:
    file_part = docs_path.split("#", 1)[0]
    return bool(file_part) and (ROOT / file_part).exists()


def collect_test_expectations() -> dict[str, dict[str, Any]]:
    expectations: dict[str, dict[str, Any]] = {}
    if not TESTS_PATH.exists():
        return expectations

    for test_path in sorted(TESTS_PATH.glob("*/*.json")):
        try:
            test_data = load_json(test_path)
        except json.JSONDecodeError:
            continue

        request = test_data.get("request", {})
        params = request.get("params", {}) if isinstance(request, dict) else {}
        if not isinstance(params, dict):
            continue
        tool_name = params.get("name")
        if not isinstance(tool_name, str) or not tool_name:
            continue

        suite_name = test_path.parent.name
        execute_tool = bool(test_data.get("executeTool", True))
        expectation = expectations.setdefault(
            tool_name,
            {
                "executedSuites": set(),
                "manualSuites": set(),
                "files": [],
            },
        )
        expectation["files"].append(str(test_path.relative_to(ROOT)))
        if execute_tool:
            expectation["executedSuites"].add(suite_name)
        else:
            expectation["manualSuites"].add(suite_name)
    return expectations


def expected_coverage_for_test(expectation: dict[str, Any]) -> str:
    executed_suites = expectation["executedSuites"]
    if "Core" in executed_suites:
        return "core"
    if executed_suites:
        return "category"
    if expectation["manualSuites"]:
        return "manual"
    return "missing"


def main() -> int:
    issues: list[str] = []
    registry = load_json(REGISTRY_PATH)
    schema = load_json(SCHEMA_PATH)
    mirror = load_json(MIRROR_PATH)
    tools = registry.get("tools", [])
    mirror_tools = mirror.get("tools", [])
    validate_registry_shape(registry, schema, issues)
    if registry != mirror:
        issues.append("Registry mirror content differs from Tools/UnrealMcpToolRegistry/tools.json.")
    handler_registry_text = HANDLER_REGISTRY_PATH.read_text(encoding="utf-8")
    handler_entries = {
        match.group("name"): match.group("category")
        for match in re.finditer(
            r'MakeHandlerEntry\(TEXT\("(?P<name>[^"]+)"\),\s*TEXT\("(?P<category>[^"]+)"\)',
            handler_registry_text,
        )
    }
    test_expectations = collect_test_expectations()

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
        if tool.get("riskLevel") not in KNOWN_RISK_LEVELS:
            issues.append(f"{name}: invalid riskLevel {tool.get('riskLevel')!r}")
        if tool.get("exposure") not in KNOWN_EXPOSURES:
            issues.append(f"{name}: invalid exposure {tool.get('exposure')!r}")
        if tool.get("testCoverage") not in KNOWN_TEST_COVERAGE:
            issues.append(f"{name}: invalid testCoverage {tool.get('testCoverage')!r}")
        for field in BOOLEAN_FIELDS:
            if not isinstance(tool.get(field), bool):
                issues.append(f"{name}: {field} must be boolean, got {type(tool.get(field)).__name__}.")
        if tool.get("requiresWrite") and not (tool.get("preflightSupport") and tool.get("postcheckSupport")):
            issues.append(f"{name}: write-capable tools should enable preflightSupport and postcheckSupport.")
        if tool.get("dryRunSupport") and not tool.get("requiresWrite"):
            issues.append(f"{name}: dryRunSupport is true but requiresWrite is false.")
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
        expectation = test_expectations.get(name)
        if expectation:
            expected_coverage = expected_coverage_for_test(expectation)
            if expected_coverage == "core" and tool.get("testCoverage") != "core":
                issues.append(f"{name}: core test fixture exists, but testCoverage is {tool.get('testCoverage')!r}.")
            elif expected_coverage == "category" and tool.get("testCoverage") not in {"category", "core", "external"}:
                issues.append(f"{name}: category test fixture exists, but testCoverage is {tool.get('testCoverage')!r}.")
            elif expected_coverage == "manual" and tool.get("testCoverage") == "missing":
                issues.append(f"{name}: manual/list-only test fixture exists, but testCoverage is missing.")

    for name, count in sorted(seen.items()):
        if count > 1:
            issues.append(f"{name}: duplicate registry entries: {count}")

    summary = {
        "toolCount": len(tools),
        "mirrorToolCount": len(mirror_tools),
        "handlerCount": len(handler_entries),
        "issueCount": len(issues),
        "schemaPath": str(SCHEMA_PATH.relative_to(ROOT)),
        "testFixtureToolCount": len(test_expectations),
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
