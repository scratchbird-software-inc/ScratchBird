#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CEIC-022 formal memory invariant and lane definition gate.

SEARCH_KEY: CEIC_022_MEMORY_INVARIANT_GATE
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Iterable


DEFAULT_SCAN_ROOTS = (
    "project/src/core/memory",
    "project/src/storage/page",
    "project/src/engine/executor",
    "project/src/engine/optimizer",
    "project/src/wire",
)

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".ipp",
}

AUTHORITY_BOUNDARY = (
    "memory_evidence_only_not_transaction_finality_visibility_authorization_security_"
    "recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority"
)

REQUIRED_LANE_KINDS = {
    "asan",
    "lsan",
    "ubsan",
    "tsan",
    "valgrind_or_drmemory",
    "clang_tidy",
    "static_analyzer",
    "memory_policy_fuzz",
    "temp_workspace_fuzz",
    "soak_24h",
    "soak_72h",
}

REQUIRED_CHURN_WORKERS = {8, 32, 64, 128}


@dataclass(frozen=True)
class Violation:
    category: str
    path: str
    line: int
    snippet: str
    rule: str


def compact(value: str) -> str:
    return " ".join(value.strip().split())


def rel(repo_root: pathlib.Path, path: pathlib.Path) -> str:
    return path.resolve().relative_to(repo_root).as_posix()


def fail(message: str) -> int:
    print(f"ceic_memory_invariant_gate=fail:{message}", file=sys.stderr)
    return 1


def iter_sources(repo_root: pathlib.Path, roots: Iterable[str]) -> Iterable[pathlib.Path]:
    for raw_root in roots:
        root = (repo_root / raw_root).resolve()
        if not root.exists():
            raise FileNotFoundError(raw_root)
        if root.is_file():
            if root.suffix in SOURCE_SUFFIXES:
                yield root
            continue
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield path


def sanitize_cpp_lines(lines: list[str]) -> list[str]:
    sanitized: list[str] = []
    in_block_comment = False
    raw_close: str | None = None
    for line in lines:
        i = 0
        out: list[str] = []
        while i < len(line):
            if raw_close is not None:
                end = line.find(raw_close, i)
                if end == -1:
                    out.append(" " * (len(line) - i))
                    i = len(line)
                    continue
                out.append(" " * (end + len(raw_close) - i))
                i = end + len(raw_close)
                raw_close = None
                continue
            if in_block_comment:
                end = line.find("*/", i)
                if end == -1:
                    out.append(" " * (len(line) - i))
                    i = len(line)
                    continue
                out.append(" " * (end + 2 - i))
                i = end + 2
                in_block_comment = False
                continue
            if line.startswith("//", i):
                out.append(" " * (len(line) - i))
                break
            if line.startswith("/*", i):
                out.append("  ")
                i += 2
                in_block_comment = True
                continue
            if line.startswith('R"', i):
                open_paren = line.find("(", i + 2)
                if open_paren != -1:
                    delimiter = line[i + 2 : open_paren]
                    if len(delimiter) <= 16:
                        raw_close = ")" + delimiter + '"'
                        out.append(" " * (open_paren + 1 - i))
                        i = open_paren + 1
                        continue
            if line[i] in {'"', "'"}:
                quote = line[i]
                out.append(" ")
                i += 1
                escaped = False
                while i < len(line):
                    out.append(" ")
                    ch = line[i]
                    i += 1
                    if escaped:
                        escaped = False
                    elif ch == "\\":
                        escaped = True
                    elif ch == quote:
                        break
                continue
            out.append(line[i])
            i += 1
        sanitized.append("".join(out))
    return sanitized


RAW_PAGE_BUFFER_RE = re.compile(
    r"\bnew\s+(?:std::byte|byte|char|unsigned\s+char|std::uint8_t|uint8_t)\s*\[|"
    r"std::vector\s*<\s*(?:std::byte|byte|char|unsigned\s+char|std::uint8_t|uint8_t)\s*>\s+"
    r"(?:raw_)?(?:page_buffer|page_bytes|frame_buffer|page_scratch|page_image_buffer)\b"
)

LINE_RULES: tuple[tuple[str, str, re.Pattern[str]], ...] = (
    (
        "protected_memory_without_zero_on_release",
        "protected_memory_zero_on_release",
        re.compile(r"\bProtectedMemoryEvidence\b.*\bzero_on_release\s*=\s*false\b"),
    ),
    (
        "support_bundle_protected_material_copy",
        "support_bundle_redaction_before_buffering",
        re.compile(r"\ballow_protected_material\s*=\s*true\b|\bprotected_material_copy\b"),
    ),
    (
        "raw_page_buffer_allocation",
        "raw_page_buffer_must_use_scoped_page_buffer",
        RAW_PAGE_BUFFER_RE,
    ),
    (
        "forbidden_authority_claim",
        "memory_evidence_never_authority",
        re.compile(
            r"\b(?:transaction_finality|visibility|authorization|security|recovery|parser|donor|wal|"
            r"benchmark|optimizer_plan|index_finality|agent_action)_authority\s*=\s*true\b"
        ),
    ),
    (
        "production_fixture_hook_leak",
        "fixtures_must_not_be_enabled_in_production",
        re.compile(r"\b(?:fixture_enabled|allow_fixture|test_hook_enabled)\s*=\s*true\b"),
    ),
)

FILE_RULES: tuple[tuple[str, str, re.Pattern[str], tuple[str, ...]], ...] = (
    (
        "protected_memory_without_zero_on_release",
        "protected_memory_zero_on_release",
        re.compile(r"\bProtectedMemoryEvidence\b|\bProtectedMemoryRequest\b"),
        ("zero_on_release = true", "zero_on_release=true"),
    ),
    (
        "allocation_without_scope",
        "allocation_requires_scope_context",
        re.compile(r"\b(?:Allocate|AllocateZeroed|AllocateScoped|Grant)\s*\("),
        (
            "context_id",
            "session_id",
            "transaction_id",
            "statement_id",
            "query_id",
            "owner",
            "scope_chain",
            "MemoryTag",
            "MemoryManager",
            "BoundedAllocator",
            "ReservationBackedMemoryResource",
            "reservation_ledger",
            "ArenaAllocator",
            "TypedArena",
            "TypedSlab",
            "executor.query_memory",
            "RequestExecutorQueryMemory",
        ),
    ),
    (
        "query_grant_without_context",
        "query_grant_requires_session_query_transaction_context",
        re.compile(r"\bQueryMemoryArena\b|\bQueryMemoryGrantRequest\b|\bGrant\s*\("),
        (
            "query_id",
            "session_id",
            "transaction_id",
            "statement_id",
            "database_id",
            "ContextMissing",
            "QueryMemoryContext",
            "RequestExecutorQueryMemory",
            "operation_id",
            "route_label",
            "memory_grant",
            "executor.query_memory",
            "RequestExecutorQueryMemory",
        ),
    ),
    (
        "page_cache_frame_without_memory_manager_ownership",
        "page_cache_frame_requires_scoped_page_buffer",
        re.compile(r"\bPageCacheFrame\b|\bpage_cache_frame\b"),
        ("ScopedPageBuffer", "MemoryManager", "AllocateScopedPageBuffer"),
    ),
    (
        "spill_without_unified_budget_reservation",
        "spill_requires_unified_budget_or_ceic_011_reservation",
        re.compile(
            r"\bSpillOperator\b|\bspill_allocation_id\b|\bspill_reserved_bytes\b|"
            r"\bTempWorkspaceAllocationRequest\b|\bTempWorkspaceBudgetReservationEvidence\b",
            re.IGNORECASE,
        ),
        (
            "UnifiedMemorySpillBudget",
            "TempWorkspaceBudgetReservationEvidence",
            "require_ceic_011_reservation",
            "reservation_ledger",
            "reservation",
            "budget",
            "authority_scope",
            "spillable",
            "estimated_spill",
            "actual_spill",
        ),
    ),
    (
        "reservation_without_release_cleanup",
        "reservation_requires_release_cleanup_path",
        re.compile(r"\bReserve\s*\(|\breservation_created\s*=\s*true\b"),
        ("Release(", "Cleanup", "Cancel(", "Reset(", "~"),
    ),
)


def scan_file(repo_root: pathlib.Path, path: pathlib.Path) -> list[Violation]:
    original_lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    lines = sanitize_cpp_lines(original_lines)
    joined = "\n".join(lines)
    violations: list[Violation] = []
    rel_path = rel(repo_root, path)

    for category, rule, regex in LINE_RULES:
        for line_no, line in enumerate(lines, start=1):
            match = regex.search(line)
            if match:
                violations.append(Violation(category, rel_path, line_no, compact(line), rule))

    for category, rule, regex, required_markers in FILE_RULES:
        if not regex.search(joined):
            continue
        if any(marker in joined for marker in required_markers):
            continue
        first_line = 1
        for line_no, line in enumerate(lines, start=1):
            if regex.search(line):
                first_line = line_no
                break
        violations.append(
            Violation(
                category,
                rel_path,
                first_line,
                f"missing markers: {', '.join(required_markers)}",
                rule,
            )
        )

    return violations


def validate_manifest(repo_root: pathlib.Path, manifest_path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("search_key") != "CEIC_022_MEMORY_VERIFICATION_LANES":
        errors.append("lane manifest search_key mismatch")
    if manifest.get("authority_boundary") != AUTHORITY_BOUNDARY:
        errors.append("lane manifest authority boundary mismatch")

    kinds: set[str] = set()
    for section in ("sanitizer_lanes", "dynamic_analysis_lanes", "static_analysis_lanes", "fuzz_lanes", "soak_lanes"):
        entries = manifest.get(section, [])
        if not isinstance(entries, list) or not entries:
            errors.append(f"{section} must be a non-empty list")
            continue
        for entry in entries:
            kind = entry.get("kind", "")
            kinds.add(kind)
            for field in ("id", "kind", "platforms", "artifact_policy"):
                if field not in entry or not entry[field]:
                    errors.append(f"{section} entry {entry.get('id', '<missing>')} missing {field}")
            if section == "soak_lanes":
                if entry.get("duration_hours") not in (24, 72):
                    errors.append(f"soak lane {entry.get('id', '<missing>')} must be 24h or 72h")
                if "authority_boundary" not in entry.get("required_evidence", []):
                    errors.append(f"soak lane {entry.get('id', '<missing>')} must require authority_boundary evidence")

    missing = sorted(REQUIRED_LANE_KINDS - kinds)
    if missing:
        errors.append(f"lane manifest missing kinds: {', '.join(missing)}")

    churn_entries = manifest.get("churn_lanes", [])
    workers = {entry.get("worker_count") for entry in churn_entries if isinstance(entry, dict)}
    missing_workers = sorted(REQUIRED_CHURN_WORKERS - workers)
    if missing_workers:
        errors.append(f"lane manifest missing churn workers: {missing_workers}")
    for entry in churn_entries:
        required_summary = set(entry.get("required_summary", []))
        for field in ("p50_ns", "p95_ns", "p99_ns", "cleanup_released"):
            if field not in required_summary:
                errors.append(f"churn lane {entry.get('id', '<missing>')} missing {field} summary")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--scan-root", action="append", default=[])
    parser.add_argument("--manifest", default="project/tools/ceic_memory_verification_lanes.json")
    parser.add_argument("--expect-category", action="append", default=[])
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    roots = args.scan_root or list(DEFAULT_SCAN_ROOTS)
    manifest_path = (repo_root / args.manifest).resolve()

    violations: list[Violation] = []
    try:
        for path in iter_sources(repo_root, roots):
            violations.extend(scan_file(repo_root, path))
    except FileNotFoundError as exc:
        return fail(f"missing scan root {exc}")

    manifest_errors = validate_manifest(repo_root, manifest_path)
    categories = {violation.category for violation in violations}
    for expected in args.expect_category:
        if expected not in categories:
            manifest_errors.append(f"expected violation category absent: {expected}")

    if manifest_errors:
        for error in manifest_errors:
            print(error, file=sys.stderr)
        return fail("manifest validation failed")

    if violations:
        for violation in violations:
            print(
                f"{violation.category}:{violation.path}:{violation.line}:"
                f"{violation.rule}:{violation.snippet}",
                file=sys.stderr,
            )
        return fail(f"{len(violations)} invariant violation(s)")

    print("ceic_memory_invariant_gate=pass")
    print(f"scan_roots={len(roots)}")
    print(f"lane_kinds={len(REQUIRED_LANE_KINDS)}")
    print("churn_workers=8,32,64,128")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
