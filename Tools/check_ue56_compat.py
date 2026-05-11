#!/usr/bin/env python3
"""Scan UnrealMcp source for APIs that exceed the supported UE minimum."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = REPO_ROOT / "Plugins" / "UnrealMcp" / "Source" / "UnrealMcp"
ENGINE_COMPAT_HEADER = SOURCE_ROOT / "Private" / "UnrealMcpEngineCompat.h"
MODULE_FILE = SOURCE_ROOT / "Private" / "UnrealMcpModule.cpp"
SOURCE_SUFFIXES = {".h", ".cpp", ".inl"}


# Each entry: pattern = python regex, reason = short note, added_in = UE version,
# severity = 'error' | 'warning'. Entries should be confirmed by real cross-version
# build, not just speculation. Version-specific shims belong in
# UnrealMcpEngineCompat.h; add forbidden bare includes or raw ENGINE_*_VERSION
# checks here so future PRs keep business code on the central shim.
# To seed candidates after a UE upgrade run: python3 Tools/audit_engine_header_diff.py --format markdown
FORBIDDEN_PATTERNS: list[dict[str, Any]] = [
    {
        "pattern": r'^\s*#include\s+"Misc/StringOutputDevice\.h"\s*$',
        "reason": "Misc/StringOutputDevice.h is 5.7+. Include UnrealMcpEngineCompat.h instead so business code does not carry engine-version shims.",
        "added_in": "5.7",
        "severity": "warning",
        "suppress_when_guarded": True,
    },
    {
        "pattern": r"^\s*#\s*if\b.*\bENGINE_(?:MAJOR|MINOR)_VERSION\b",
        "reason": "Raw ENGINE_*_VERSION checks belong in UnrealMcpEngineCompat.h; business code must use the central compatibility shim.",
        "added_in": "5.7",
        "severity": "warning",
    },
]


@dataclass(frozen=True)
class Finding:
    path: Path
    line_number: int
    reason: str
    added_in: str
    severity: str


def parse_engine_version(value: str) -> tuple[int, int]:
    match = re.fullmatch(r"(\d+)\.(\d+)", value.strip())
    if not match:
        raise argparse.ArgumentTypeError("expected engine version in X.Y format, for example 5.6")
    return int(match.group(1)), int(match.group(2))


def should_enforce_pattern(added_in: str, min_engine: tuple[int, int]) -> bool:
    try:
        added_version = parse_engine_version(added_in)
    except argparse.ArgumentTypeError as exc:
        raise ValueError(f"invalid forbidden pattern added_in value {added_in!r}: {exc}") from exc
    return added_version > min_engine


def iter_source_files(root: Path) -> Iterable[Path]:
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            yield path


_GUARD_OPEN_RE = re.compile(
    r"^\s*#\s*if\s+.*ENGINE_MINOR_VERSION\s*>=\s*(\d+)"
)
_GUARD_ENDIF_RE = re.compile(r"^\s*#\s*endif\b")
_GUARD_IF_RE = re.compile(r"^\s*#\s*if\b")


def is_inside_version_guard(lines: list[str], target_idx: int, min_minor: int) -> bool:
    """Return True when the line at ``target_idx`` is inside a 5.x version
    guard whose minor floor is at least ``min_minor``. Skips both nested
    inner guards and unrelated outer #ifs."""
    depth = 0
    for i in range(target_idx - 1, -1, -1):
        line = lines[i]
        if _GUARD_ENDIF_RE.match(line):
            depth += 1
            continue
        if _GUARD_IF_RE.match(line):
            if depth == 0:
                match = _GUARD_OPEN_RE.match(line)
                if match and int(match.group(1)) >= min_minor:
                    return True
                return False
            depth -= 1
    return False


def is_allowed_engine_version_preprocessor(path: Path, line: str) -> bool:
    if path == ENGINE_COMPAT_HEADER:
        return True
    if path == MODULE_FILE:
        stripped = line.strip()
        return stripped.startswith("#if !defined(ENGINE_MAJOR_VERSION)") or stripped.startswith("#if (ENGINE_MAJOR_VERSION < 5)")
    return False


def scan_file(path: Path, pattern_entry: dict[str, Any]) -> list[Finding]:
    expression = re.compile(pattern_entry["pattern"])
    reason = pattern_entry["reason"]
    added_in = pattern_entry["added_in"]
    severity = pattern_entry["severity"]
    suppress_when_guarded = bool(pattern_entry.get("suppress_when_guarded", False))
    findings: list[Finding] = []

    added_major, added_minor = parse_engine_version(added_in)

    with path.open("r", encoding="utf-8", errors="replace") as source_file:
        lines = source_file.readlines()

    for line_index, line in enumerate(lines):
        if not expression.search(line):
            continue
        if is_allowed_engine_version_preprocessor(path, line):
            continue
        if suppress_when_guarded and added_major == 5 and is_inside_version_guard(lines, line_index, added_minor):
            # The forbidden symbol is gated behind the matching engine-version
            # guard, so the lower-version path uses the correct alternative.
            continue
        findings.append(
            Finding(
                path=path,
                line_number=line_index + 1,
                reason=reason,
                added_in=added_in,
                severity=severity,
            )
        )
    return findings


def scan_sources(min_engine: tuple[int, int]) -> list[Finding]:
    findings: list[Finding] = []
    active_patterns = [
        entry
        for entry in FORBIDDEN_PATTERNS
        if should_enforce_pattern(entry["added_in"], min_engine)
    ]

    for path in iter_source_files(SOURCE_ROOT):
        for entry in active_patterns:
            findings.extend(scan_file(path, entry))
    return findings


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Scan UnrealMcp source for known APIs that are not compatible with the configured UE minimum."
    )
    parser.add_argument(
        "--allow-warning",
        action="store_true",
        help="Accepted for CI readability; warnings are non-fatal by default.",
    )
    parser.add_argument(
        "--min-engine",
        default="5.6",
        type=parse_engine_version,
        metavar="X.Y",
        help="Minimum supported Unreal Engine version to enforce against. Defaults to 5.6.",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if not SOURCE_ROOT.is_dir():
        print(f"Source directory not found: {SOURCE_ROOT}", file=sys.stderr)
        return 1

    min_engine_text = f"{args.min_engine[0]}.{args.min_engine[1]}"
    try:
        findings = scan_sources(args.min_engine)
    except (KeyError, re.error, ValueError) as exc:
        print(f"Invalid linter pattern configuration: {exc}", file=sys.stderr)
        return 1

    errors = sum(1 for finding in findings if finding.severity == "error")
    warnings = sum(1 for finding in findings if finding.severity == "warning")

    if findings:
        for finding in findings:
            display_path = finding.path.relative_to(REPO_ROOT)
            print(f"{display_path}:{finding.line_number}: {finding.reason} (introduced in {finding.added_in})")
    else:
        print("No matches.")

    print(f"{errors} errors, {warnings} warnings; min engine {min_engine_text}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
