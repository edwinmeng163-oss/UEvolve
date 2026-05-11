#!/usr/bin/env python3
"""Scan UnrealMcp source for APIs that exceed the supported UE minimum."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOT = REPO_ROOT / "Plugins" / "UnrealMcp" / "Source" / "UnrealMcp"
SOURCE_SUFFIXES = {".h", ".cpp", ".inl"}


# Currently EMPTY -- the 2026-05 audit confirmed no 5.7-only API surfaces in use.
# When adding new entries, include 'added_in' to make intent auditable.
FORBIDDEN_PATTERNS: list[dict[str, str]] = []


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


def scan_file(path: Path, pattern_entry: dict[str, str]) -> list[Finding]:
    expression = re.compile(pattern_entry["pattern"])
    reason = pattern_entry["reason"]
    added_in = pattern_entry["added_in"]
    severity = pattern_entry["severity"]
    findings: list[Finding] = []

    with path.open("r", encoding="utf-8", errors="replace") as source_file:
        for line_number, line in enumerate(source_file, start=1):
            if expression.search(line):
                findings.append(
                    Finding(
                        path=path,
                        line_number=line_number,
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
