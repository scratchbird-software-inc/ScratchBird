#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DPC standalone CTest independence gate.

This gate intentionally reads only project test source/CMake files and generated
CTest metadata. It must not use execution_plan artifacts as runtime inputs.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


TOKEN_RE = re.compile(r"\b(?:DPC|CDP|PFAR)-[0-9A-Z]+\b")
ADD_TEST_RE = re.compile(r"add_test\(\[=\[(?P<name>[^\]]+)\]=\](?P<body>.*?)\)", re.S)
LABEL_RE = re.compile(
    r"set_tests_properties\(\[=\[(?P<name>[^\]]+)\]=\]\s+PROPERTIES\s+"
    r".*?LABELS\s+\"(?P<labels>[^\"]+)\"",
    re.S,
)
SOURCE_RE = re.compile(r"(?P<source>[A-Za-z0-9_./-]+\.(?:cpp|py|cmake))")
ENV_RE = re.compile(
    r"(?:getenv|std::getenv|os\.environ(?:\.get)?|environ(?:\.get)?)"
    r"\s*(?:\(|\[)[^\n;]*EXECUTION_PLAN",
    re.I,
)

TEST_ROOTS = (
    Path("project/tests/database_lifecycle"),
    Path("project/tests/agents"),
    Path("project/tests/sbsql_parser_worker"),
)
LABEL_SCOPES = ("database_lifecycle", "agents", "sbsql_parser_worker")
SOURCE_CMAKE_FILES = tuple(root / "CMakeLists.txt" for root in TEST_ROOTS)
BUILD_CTEST_FILES = tuple(
    Path("tests") / root.relative_to("project/tests") / "CTestTestfile.cmake"
    for root in TEST_ROOTS
)
SELF = Path("project/tests/database_lifecycle/dpc_standalone_ctest_independence_gate.py")


@dataclass(frozen=True)
class CTestEntry:
    name: str
    command: str
    labels: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    return parser.parse_args()


def read_text(path: Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        errors.append(f"required gate input missing: {path}")
    except OSError as exc:
        errors.append(f"failed to read gate input {path}: {exc}")
    return ""


def parse_ctest_metadata(build_root: Path, errors: list[str]) -> dict[str, CTestEntry]:
    entries: dict[str, CTestEntry] = {}
    labels_by_name: dict[str, str] = {}
    for rel_path in BUILD_CTEST_FILES:
        text = read_text(build_root / rel_path, errors)
        for match in LABEL_RE.finditer(text):
            labels_by_name[match.group("name")] = match.group("labels")
        for match in ADD_TEST_RE.finditer(text):
            name = match.group("name")
            entries[name] = CTestEntry(
                name=name,
                command=match.group("body"),
                labels=labels_by_name.get(name, ""),
            )
    return entries


def registered_source_paths(repo_root: Path, entries: dict[str, CTestEntry],
                            errors: list[str]) -> set[Path]:
    ctest_names = set(entries)
    paths: set[Path] = set()
    resolved_repo = repo_root.resolve()
    for entry in entries.values():
        if not TOKEN_RE.search(entry.labels):
            continue
        for source_match in SOURCE_RE.finditer(entry.command):
            candidate = Path(source_match.group("source"))
            if not candidate.is_absolute():
                continue
            try:
                rel_candidate = candidate.resolve().relative_to(resolved_repo)
            except ValueError:
                continue
            if any(rel_candidate.is_relative_to(root) for root in TEST_ROOTS):
                if (repo_root / rel_candidate).is_file():
                    paths.add(rel_candidate)

    for rel_cmake in SOURCE_CMAKE_FILES:
        source_dir = (repo_root / rel_cmake).parent
        text = read_text(repo_root / rel_cmake, errors)
        for statement in re.finditer(r"([^()\n]+_gate[^)]*\))", text, re.S):
            block = statement.group(0)
            if not TOKEN_RE.search(block):
                continue
            if not any(name in block for name in ctest_names):
                continue
            for source_match in SOURCE_RE.finditer(block):
                source = source_match.group("source")
                if source == "CMakeLists.txt":
                    continue
                candidate = (source_dir / source).resolve()
                try:
                    rel_candidate = candidate.relative_to(repo_root.resolve())
                except ValueError:
                    continue
                if (repo_root / rel_candidate).is_file():
                    paths.add(rel_candidate)

    # Some local CMake helper functions hide add_executable from simple source
    # extraction. Keep the source set tied to registered CTest names.
    for test_name, entry in entries.items():
        if not TOKEN_RE.search(entry.labels):
            continue
        for test_dir in TEST_ROOTS:
            for suffix in (".cpp", ".py", ".cmake"):
                candidate = test_dir / f"{test_name}{suffix}"
                if (repo_root / candidate).is_file():
                    paths.add(candidate)
    return paths


def dpc_cdp_gate_sources(repo_root: Path) -> set[Path]:
    paths: set[Path] = set()
    for root in TEST_ROOTS:
        for candidate in (repo_root / root).iterdir():
            if candidate.suffix not in {".cpp", ".py", ".cmake"}:
                continue
            if "_gate" not in candidate.stem and "conformance" not in candidate.stem:
                continue
            if candidate.name.startswith(("dpc_", "cdp_", "pfar_")):
                paths.add(root / candidate.name)
    return paths


def local_python_dependencies(repo_root: Path, rel_paths: set[Path]) -> set[Path]:
    imports = re.compile(r"^\s*(?:import|from)\s+([A-Za-z_][A-Za-z0-9_]*)", re.M)
    dependencies: set[Path] = set()
    for rel_path in sorted(rel_paths):
        if rel_path.suffix != ".py":
            continue
        text = read_text(repo_root / rel_path, [])
        for match in imports.finditer(text):
            module_name = match.group(1)
            candidate = rel_path.parent / f"{module_name}.py"
            if candidate.name.startswith(("dpc_", "cdp_", "pfar_")):
                if (repo_root / candidate).is_file():
                    dependencies.add(candidate)
    return dependencies


def is_allowed_negative_execution_plan_assertion(statement: str) -> bool:
    lowered = statement.lower()
    negative_markers = (
        "!contains",
        "not in",
        "require(not",
        "not (",
        "must not",
        "must_not",
        "no_execution_plan",
        "not read",
        "not be a execution_plan",
        "execution_plan runtime dependency",
        "depends on execution_plan path",
        "execution_plan path must be rejected",
    )
    return any(marker in lowered for marker in negative_markers)


def iter_statements(text: str) -> list[tuple[int, str]]:
    statements: list[tuple[int, str]] = []
    current: list[str] = []
    start_line = 1
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not current:
            start_line = line_number
        current.append(line)
        if ";" in line or line.rstrip().endswith(")") or line.strip().startswith(("require(", "Require(")):
            statements.append((start_line, "\n".join(current)))
            current = []
    if current:
        statements.append((start_line, "\n".join(current)))
    return statements


def scan_for_runtime_execution_plan_dependencies(repo_root: Path, rel_paths: set[Path],
                                           errors: list[str]) -> None:
    forbidden_paths = ("docs" "/execution-plans", "docs" "/completed-execution-plans")
    forbidden_file_names = (
        "TRACKER.csv",
        "ACCEPTANCE_GATES.csv",
        "DEPENDENCIES.csv",
        "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
        "FINAL_AUDIT.md",
    )
    for rel_path in sorted(rel_paths):
        if rel_path == SELF:
            continue
        text = read_text(repo_root / rel_path, errors)
        for start_line, statement in iter_statements(text):
            normalized = statement.replace("\\", "/")
            if any(path in normalized for path in forbidden_paths):
                if not is_allowed_negative_execution_plan_assertion(statement):
                    errors.append(
                        f"{rel_path}:{start_line}: forbidden runtime execution_plan path reference"
                    )
            if ENV_RE.search(statement):
                errors.append(f"{rel_path}:{start_line}: forbidden EXECUTION_PLAN environment dependency")
            if any(name in statement for name in forbidden_file_names):
                if not is_allowed_negative_execution_plan_assertion(statement):
                    errors.append(
                        f"{rel_path}:{start_line}: forbidden execution_plan artifact fixture reference"
                    )


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve()
    errors: list[str] = []

    if "docs" "/execution-plans" in repo_root.as_posix() or "docs" "/execution-plans" in build_root.as_posix():
        errors.append("gate roots must not point into private execution_plan directories")

    entries = parse_ctest_metadata(build_root, errors)
    registered = {
        name: entry
        for name, entry in entries.items()
        if TOKEN_RE.search(entry.labels)
        and any(scope in entry.labels for scope in LABEL_SCOPES)
    }
    if not registered:
        errors.append("no DPC/CDP/PFAR CTest registrations found in regression metadata")

    required_sources = dpc_cdp_gate_sources(repo_root)
    registered_sources = registered_source_paths(repo_root, entries, errors)
    missing_sources = sorted(required_sources - registered_sources)
    for rel_path in missing_sources:
        errors.append(f"{rel_path}: DPC/CDP/PFAR gate source is not tied to CTest registration")

    for name, entry in sorted(registered.items()):
        if "docs" "/execution-plans" in entry.command or "docs" "/completed-execution-plans" in entry.command:
            errors.append(f"{name}: CTest command reads execution_plan path")
        if name not in entry.labels:
            errors.append(f"{name}: CTest labels do not include the test name")

    scanned_sources = required_sources | registered_sources
    scanned_sources |= local_python_dependencies(repo_root, scanned_sources)
    scan_for_runtime_execution_plan_dependencies(repo_root, scanned_sources, errors)

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(
        "DPC standalone CTest independence gate passed: "
        f"{len(registered)} CTest registrations and "
        f"{len(scanned_sources)} source files checked."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
