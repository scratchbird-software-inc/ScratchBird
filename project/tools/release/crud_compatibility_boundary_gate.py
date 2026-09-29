#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Freeze compatibility projection use outside canonical MGA DML."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys


PROJECTION_CALL = "BuildCrudCompatibilityStateFromMga("
SOURCE_SUFFIXES = {".cpp", ".hpp", ".inc"}
PROJECTION_PATTERN = re.compile(r"\bBuildCrudCompatibilityStateFromMga\s*\(")
CPP_LEXEMES = re.compile(
    r'(?P<raw>(?:u8|[uUL])?R"(?P<delimiter>[^\s()\\]{0,16})\(.*?\)(?P=delimiter)")'
    r'|(?P<string>"(?:\\.|[^"\\])*")'
    r"|(?P<number>\b[0-9][a-zA-Z0-9._']*)"
    r"|(?P<character>'(?:\\.|[^'\\\r\n])*')"
    r'|(?P<comment>//[^\r\n]*|/\*.*?\*/)',
    re.DOTALL,
)


def without_cpp_comments(source: str) -> str:
    # Translation-phase line splicing happens before comment recognition.
    source = source.replace("\\\r\n", "").replace("\\\n", "")
    def replace(match: re.Match[str]) -> str:
        if match.group("comment") is None:
            return match.group(0)
        return "".join(ch if ch in "\r\n" else " " for ch in match.group(0))
    return CPP_LEXEMES.sub(replace, source)


def projection_use_count(source: str) -> int:
    return len(PROJECTION_PATTERN.findall(without_cpp_comments(source)))


def scanner_contract() -> None:
    # Comments cannot introduce dependencies or conceal code after their end;
    # string paths remain scanned because forbidden durable paths are literals.
    cases = [
        ("// CrudState\nint value;", False),
        ("/* CrudState */ int value;", False),
        ("/* note */ CrudState value;", True),
        ('const char* path=".sb.crud_events";', True),
        ('const char* url="https://host"; CrudState value;', True),
        ('const char* raw=R"tag(// not a comment)tag"; CrudState value;', True),
        ("auto quote='\\\''; CrudState value;", True),
        ("auto count=1'000; /* CrudState */ auto other=2'000;", False),
        ("// continued \\\nCrudState\nint value;", False),
        ("Crud\\\nState value;", True),
    ]
    for source, expected in cases:
        cleaned = without_cpp_comments(source)
        actual = "CrudState" in cleaned or ".sb.crud_events" in cleaned
        if actual != expected:
            raise RuntimeError("C++ boundary scanner control failed")
    calls = "BuildCrudCompatibilityStateFromMga /* gap */ \n (state);"
    if projection_use_count(calls) != 1 or projection_use_count("// " + PROJECTION_CALL) != 0:
        raise RuntimeError("compatibility call scanner control failed")


def main() -> int:
    scanner_contract()
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--inventory", type=Path)
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    inventory_path = (
        args.inventory.resolve()
        if args.inventory
        else project_root
        / "tools/release/crud_compatibility_boundary_inventory.json"
    )
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    allowed_classes = set(inventory["allowed_classifications"])
    expected = inventory["consumers"]

    errors: list[str] = []
    actual: dict[str, int] = {}
    source_root = project_root / "src"
    for path in source_root.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(project_root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        count = projection_use_count(text)
        if count:
            actual[relative] = count

    if actual != {path: item["count"] for path, item in expected.items()}:
        for path in sorted(set(actual) | set(expected)):
            expected_count = expected.get(path, {}).get("count", 0)
            actual_count = actual.get(path, 0)
            if expected_count != actual_count:
                errors.append(
                    f"compatibility projection inventory drift: {path}: "
                    f"expected {expected_count}, found {actual_count}"
                )

    for path, item in expected.items():
        classification = item.get("classification", "")
        if classification not in allowed_classes:
            errors.append(
                f"unrecognized compatibility consumer classification: "
                f"{path}: {classification}"
            )

    canonical_dml = project_root / "src/engine/internal_api/dml"
    forbidden = ("CrudState", PROJECTION_CALL, ".sb.crud_events")
    for path in canonical_dml.rglob("*"):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        text = without_cpp_comments(path.read_text(encoding="utf-8", errors="replace"))
        for token in forbidden:
            if (PROJECTION_PATTERN.search(text) if token == PROJECTION_CALL else token in text):
                errors.append(
                    f"canonical DML compatibility boundary violation: "
                    f"{path.relative_to(project_root)} contains {token!r}"
                )

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(
        "crud compatibility boundary: PASS "
        f"({sum(actual.values())} frozen uses in {len(actual)} classified files)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
