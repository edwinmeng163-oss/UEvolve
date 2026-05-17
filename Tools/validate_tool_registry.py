#!/usr/bin/env python3
"""Validate UEvolve's explicit MCP ToolRegistry files.

This intentionally uses only the Python standard library so Windows/macOS users
can run it before opening Unreal Editor or as a lightweight CI step.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
REGISTRY_PATH = ROOT / "Tools" / "UnrealMcpToolRegistry" / "tools.json"
SCHEMA_PATH = ROOT / "Tools" / "UnrealMcpToolRegistry" / "schema.json"
SCHEMA_MIRROR_PATH = ROOT / "Plugins" / "UnrealMcp" / "Resources" / "ToolRegistry" / "schema.json"
SCHEMA_ALIAS_PATH = ROOT / "Schemas" / "UnrealMcpToolRegistry.schema.json"
MIRROR_PATH = ROOT / "Plugins" / "UnrealMcp" / "Resources" / "ToolRegistry" / "tools.json"
PRIVATE_SOURCE_PATH = ROOT / "Plugins" / "UnrealMcp" / "Source" / "UnrealMcp" / "Private"
TOOL_REGISTRAR_PATH = PRIVATE_SOURCE_PATH / "UnrealMcpToolRegistrar.cpp"
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
KNOWN_IMPLEMENTATION_TRACKS = {"cpp", "python"}
PYTHON_HANDLER_PATH_RE = re.compile(r"^Tools/UnrealMcpPyTools/[^/]+/main\.py$")
PYTHON_HANDLER_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
PYTHON_MODULE_NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*$")
EXPECTED_NON_STANDARD_DISPATCH: set[str] = {
    "unreal.spawn_actor_basic",  # Alias: visible tool shares the unreal.spawn_actor dispatcher branch.
    "unreal.spawn_actor_batch_basic",  # Alias: visible tool shares the unreal.spawn_actor_batch dispatcher branch.
    "unreal.editor.engine_version",  # Forwarded from UnrealMcpEditorTools.cpp to UnrealMcpEditorEngineVersionTool.cpp.
    "unreal.tools.export_package",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpToolPackager.cpp.
    "unreal.tools.list_exportable",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpToolPackager.cpp.
    "unreal.tools.import_package",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpToolPackager.cpp.
    "unreal.install_doctor",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpInstallDoctor.cpp.
    "unreal.knowledge_index_refresh",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpKnowledgeTools.cpp.
    "unreal.knowledge_search",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpKnowledgeTools.cpp.
    "unreal.tool_recommend",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpKnowledgeTools.cpp.
    "unreal.tool_gap_analyze",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpKnowledgeTools.cpp.
    "unreal.workflow_recommend",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpKnowledgeTools.cpp.
    "unreal.knowledge_eval_run",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpKnowledgeTools.cpp.
    "unreal.preview_change_plan",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpSelfExtensionPrecisionTools.cpp.
    "unreal.capture_project_snapshot",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpSelfExtensionPrecisionTools.cpp.
    "unreal.diff_project_snapshot",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpSelfExtensionPrecisionTools.cpp.
    "unreal.verify_task_outcome",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpSelfExtensionPrecisionTools.cpp.
    "unreal.mcp_classify_error",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpSelfExtensionPrecisionTools.cpp.
    "unreal.mcp_prepare_test_sandbox",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpSelfExtensionPrecisionTools.cpp.
    "unreal.workflow_run",  # Forwarded by UnrealMcpSelfExtensionTools.cpp into UnrealMcpWorkflowTools.cpp.
}


def load_json(path: Path) -> dict:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--strict-dispatch",
        action="store_true",
        help="Treat descriptor/dispatcher source-integrity warnings as validation errors.",
    )
    return parser.parse_args(argv)


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


def tool_implementation_track(tool: dict[str, Any]) -> str:
    track = tool.get("implementationTrack", "cpp")
    return track if isinstance(track, str) else ""


def python_handler_file_hash(path_value: str) -> str | None:
    handler_path = ROOT / path_value
    if not handler_path.is_file():
        return None
    digest = hashlib.sha256()
    with handler_path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def python_handler_metadata_valid(tool: dict[str, Any]) -> bool:
    path_value = tool.get("pythonHandlerPath")
    sha_value = tool.get("pythonHandlerSha256")
    if not isinstance(path_value, str) or not PYTHON_HANDLER_PATH_RE.fullmatch(path_value):
        return False
    if not isinstance(sha_value, str) or not PYTHON_HANDLER_SHA256_RE.fullmatch(sha_value):
        return False
    actual_sha = python_handler_file_hash(path_value)
    return actual_sha == sha_value


def validate_implementation_metadata(tool: dict[str, Any], name: str, issues: list[str]) -> None:
    track = tool.get("implementationTrack", "cpp")
    if not isinstance(track, str):
        issues.append(f"{name}: implementationTrack must be a string when present.")
        track = ""
    elif track not in KNOWN_IMPLEMENTATION_TRACKS:
        issues.append(f"{name}: invalid implementationTrack {track!r}")

    path_value = tool.get("pythonHandlerPath")
    if "pythonHandlerPath" in tool:
        if not isinstance(path_value, str) or not PYTHON_HANDLER_PATH_RE.fullmatch(path_value):
            issues.append(
                f"{name}: pythonHandlerPath must match "
                "^Tools/UnrealMcpPyTools/[^/]+/main\\.py$"
            )

    sha_value = tool.get("pythonHandlerSha256")
    if "pythonHandlerSha256" in tool:
        if not isinstance(sha_value, str) or not PYTHON_HANDLER_SHA256_RE.fullmatch(sha_value):
            issues.append(f"{name}: pythonHandlerSha256 must be a lowercase 64-character sha256 hex string.")

    allow_list = tool.get("pythonImportAllowList", [])
    if not isinstance(allow_list, list):
        issues.append(f"{name}: pythonImportAllowList must be an array when present.")
    else:
        for index, module_name in enumerate(allow_list):
            if not isinstance(module_name, str) or not PYTHON_MODULE_NAME_RE.fullmatch(module_name):
                issues.append(f"{name}: pythonImportAllowList[{index}] must be a Python module name string.")

    if track == "python":
        if "pythonHandlerPath" not in tool:
            issues.append(f"{name}: pythonHandlerPath is required when implementationTrack is 'python'.")
        if "pythonHandlerSha256" not in tool:
            issues.append(f"{name}: pythonHandlerSha256 is required when implementationTrack is 'python'.")
        if isinstance(path_value, str) and PYTHON_HANDLER_PATH_RE.fullmatch(path_value):
            actual_sha = python_handler_file_hash(path_value)
            if actual_sha is None:
                issues.append(f"{name}: pythonHandlerPath file does not exist: {path_value!r}")
            elif isinstance(sha_value, str) and PYTHON_HANDLER_SHA256_RE.fullmatch(sha_value) and actual_sha != sha_value:
                issues.append(f"{name}: pythonHandlerSha256 does not match file content for {path_value!r}.")


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


def collect_descriptor_entries(registrar_path: Path) -> dict[str, str]:
    """Return {tool_name: source_file_basename} from MakeDescriptor calls."""
    registrar_text = registrar_path.read_text(encoding="utf-8")
    descriptor_pattern = re.compile(
        r'MakeDescriptor\(\s*'
        r'TEXT\("(?P<name>unreal\.[^"]+)"\)\s*,\s*'
        r'TEXT\("(?:\\.|[^"])*"\)\s*,\s*'
        r'TEXT\("(?:\\.|[^"])*"\)\s*,\s*'
        r'TEXT\("(?:\\.|[^"])*"\)\s*,\s*'
        r'TEXT\("(?P<source>[^"]+\.cpp)"\)',
        re.DOTALL,
    )
    entries: dict[str, str] = {}
    for match in descriptor_pattern.finditer(registrar_text):
        entries[match.group("name")] = Path(match.group("source")).name
    return entries


def verify_dispatcher_branch(tool_name: str, source_file: Path) -> bool:
    """Return true when source_file has a literal if ToolName branch."""
    if not source_file.exists():
        return False
    source_text = source_file.read_text(encoding="utf-8")
    branch_pattern = re.compile(
        r'if\s*\(\s*ToolName\s*==\s*TEXT\("'
        + re.escape(tool_name)
        + r'"\)\s*\)'
    )
    return bool(branch_pattern.search(source_text))


def expected_coverage_for_test(expectation: dict[str, Any]) -> str:
    executed_suites = expectation["executedSuites"]
    if "Core" in executed_suites:
        return "core"
    if executed_suites:
        return "category"
    if expectation["manualSuites"]:
        return "manual"
    return "missing"


def collect_handler_entries(tools: list[dict[str, Any]]) -> dict[str, str]:
    """Mirror the runtime handler registry.

    Runtime handler entries are now derived from the explicit ToolRegistry and
    code descriptors, not from a hand-maintained source scan. This keeps the
    self-extension path honest: if a tool is visible in the registry, audit,
    dispatch, and validation all see the same handler/category mapping.
    """
    entries: dict[str, str] = {}
    for tool in tools:
        name = tool.get("name")
        handler_name = tool.get("handlerName") or name
        category = tool.get("category")
        if not isinstance(handler_name, str) or not isinstance(category, str):
            continue
        if handler_name in entries and entries[handler_name] != category:
            entries[handler_name] = f"{entries[handler_name]}|CONFLICT|{category}"
        else:
            entries[handler_name] = category

    registrar_text = TOOL_REGISTRAR_PATH.read_text(encoding="utf-8")
    descriptor_pattern = re.compile(
        r'MakeDescriptor\(\s*'
        r'TEXT\("(?P<name>unreal\.[^"]+)"\)\s*,\s*'
        r'TEXT\("[^"]*"\)\s*,\s*'
        r'TEXT\("[^"]*"\)\s*,\s*'
        r'TEXT\("(?P<category>[^"]+)"\)',
        re.MULTILINE,
    )
    for match in descriptor_pattern.finditer(registrar_text):
        entries[match.group("name")] = match.group("category")
    return entries


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    issues: list[str] = []
    registry = load_json(REGISTRY_PATH)
    schema = load_json(SCHEMA_PATH)
    mirror = load_json(MIRROR_PATH)
    tools = registry.get("tools", [])
    mirror_tools = mirror.get("tools", [])
    validate_registry_shape(registry, schema, issues)
    if SCHEMA_PATH.read_bytes() != SCHEMA_MIRROR_PATH.read_bytes():
        issues.append("Schema mirror content differs from Tools/UnrealMcpToolRegistry/schema.json.")
    if SCHEMA_ALIAS_PATH.exists() and SCHEMA_PATH.read_bytes() != SCHEMA_ALIAS_PATH.read_bytes():
        issues.append("Schema alias content differs from Tools/UnrealMcpToolRegistry/schema.json.")
    if registry != mirror:
        issues.append("Registry mirror content differs from Tools/UnrealMcpToolRegistry/tools.json.")
    handler_entries = collect_handler_entries(tools)
    descriptor_entries = collect_descriptor_entries(TOOL_REGISTRAR_PATH)
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
        validate_implementation_metadata(tool, str(name), issues)
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

    dispatch_matched = 0
    dispatch_allowlisted = 0
    dispatch_python_exempted = 0
    dispatch_warnings: list[str] = []
    for tool in tools:
        if tool.get("exposure") != "visible":
            continue
        name = str(tool.get("name", ""))
        if tool_implementation_track(tool) == "python" and python_handler_metadata_valid(tool):
            dispatch_python_exempted += 1
            continue
        if name in EXPECTED_NON_STANDARD_DISPATCH:
            dispatch_allowlisted += 1
            continue
        descriptor_source = descriptor_entries.get(name)
        if not descriptor_source:
            dispatch_warnings.append(f"{name}: missing MakeDescriptor entry in {TOOL_REGISTRAR_PATH.relative_to(ROOT)}")
            continue
        source_file = PRIVATE_SOURCE_PATH / descriptor_source
        if not source_file.exists():
            dispatch_warnings.append(f"{name}: descriptor source file does not exist: {source_file.relative_to(ROOT)}")
            continue
        if verify_dispatcher_branch(name, source_file):
            dispatch_matched += 1
        else:
            dispatch_warnings.append(f"{name}: missing ToolName dispatcher branch in {source_file.relative_to(ROOT)}")

    if args.strict_dispatch:
        issues.extend(f"dispatch integrity: {warning}" for warning in dispatch_warnings)

    summary = {
        "toolCount": len(tools),
        "mirrorToolCount": len(mirror_tools),
        "handlerCount": len(handler_entries),
        "issueCount": len(issues),
        "schemaPath": str(SCHEMA_PATH.relative_to(ROOT)),
        "schemaMirrorPath": str(SCHEMA_MIRROR_PATH.relative_to(ROOT)),
        "schemaAliasPath": str(SCHEMA_ALIAS_PATH.relative_to(ROOT)),
        "testFixtureToolCount": len(test_expectations),
        "dispatchCheck": {
            "allowlisted": dispatch_allowlisted,
            "matched": dispatch_matched,
            "pythonExempted": dispatch_python_exempted,
            "strict": args.strict_dispatch,
            "warnings": len(dispatch_warnings),
        },
        "categories": sorted({tool.get("category", "") for tool in tools}),
    }
    print(json.dumps(summary, indent=2, sort_keys=True))
    if dispatch_warnings and not args.strict_dispatch:
        print("\nDispatch warnings:", file=sys.stderr)
        for warning in dispatch_warnings:
            print(f"- {warning}", file=sys.stderr)
    if issues:
        print("\nIssues:", file=sys.stderr)
        for issue in issues:
            print(f"- {issue}", file=sys.stderr)
    print(f"dispatch check: {dispatch_matched} matched, {len(dispatch_warnings)} warnings, {dispatch_allowlisted} allowlisted, {dispatch_python_exempted} python exempted")
    if issues:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
