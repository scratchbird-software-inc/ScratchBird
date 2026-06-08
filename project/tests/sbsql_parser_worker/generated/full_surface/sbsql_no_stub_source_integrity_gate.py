#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""No-stub source and fixture integrity gate for SBsql regression-owned files.

Statically scans source, generators, gates, and regression fixture code for
patterns that the SBsql closure policy forbids: work markers, disabled
assertions, skipped tests, stub comments, explicit `NotImplementedError`, and
trivially-true assertions.

Scope is intentionally limited to runtime/generator/test-owned paths. Completed
execution_plan documents are not a regression dependency and are not scanned.

Allowed exception tokens (documented execution_plan-state markers that look like
"pending" but are normative transition states): `pending_row_level`,
`oracle_pending`, `pending_canonical_authority_entry`,
`pending_canonical_authority_entry_oracle_pending`, `pending_authoring`,
`default_recorded_pending_coordinator_review`,
`blocked_until_canonical_oracle_entry`. These are CSV/string values that
record the row's promotion state per the execution_plan; they are not source
stubs.

Architecture invariant compliance: read-only static scan; no transaction
model touched; no engine, parser worker, server, listener, storage, or MGA
file modified; no WAL surface introduced. MGA copy-on-write authority
remains the sole transaction recovery model.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


SCOPE = [
    "project/tools/sb_parser_gen",
    "project/tests/sbsql_parser_worker/generated/full_surface",
]


FORBIDDEN_PATTERNS = [
    # Work markers
    (re.compile(r"\bTODO\b"), "todo_work_marker"),
    (re.compile(r"\bFIXME\b"), "fixme_work_marker"),
    (re.compile(r"\bXXX\b"), "xxx_work_marker"),
    (re.compile(r"\bHACK\b"), "hack_work_marker"),
    # Disabled/skipped tests
    (re.compile(r"\bGTEST_SKIP\b"), "gtest_skip"),
    (re.compile(r"\bDISABLED_[A-Z][A-Za-z0-9_]+"), "gtest_disabled_test_name"),
    (re.compile(r"@pytest\.mark\.skip"), "pytest_skip_decorator"),
    (re.compile(r"@unittest\.skip"), "unittest_skip_decorator"),
    (re.compile(r"\bpytest\.skip\("), "pytest_skip_call"),
    # Not-implemented sentinels
    (re.compile(r"\bNotImplementedError\b"), "not_implemented_error"),
    # Trivially-true assertions
    (re.compile(r"\bassert\s+(True|1)\b"), "trivially_true_python_assert"),
    (re.compile(r"\bEXPECT_TRUE\(\s*true\s*\)"), "trivially_true_gtest_expect"),
    (re.compile(r"\bASSERT_TRUE\(\s*true\s*\)"), "trivially_true_gtest_assert"),
    # Explicit stub markers in comments
    (re.compile(r"//\s*stub\b", re.IGNORECASE), "cxx_stub_comment"),
    (re.compile(r"#\s*stub\b", re.IGNORECASE), "py_stub_comment"),
    (re.compile(r"//\s*not\s+implemented\b", re.IGNORECASE), "cxx_not_implemented_comment"),
    (re.compile(r"#\s*not\s+implemented\b", re.IGNORECASE), "py_not_implemented_comment"),
    (re.compile(r"//\s*placeholder\b", re.IGNORECASE), "cxx_placeholder_comment"),
    (re.compile(r"#\s*placeholder\b", re.IGNORECASE), "py_placeholder_comment"),
]


ALLOWED_TOKENS = {
    "pending_row_level",
    "oracle_pending",
    "pending_canonical_authority_entry",
    "pending_canonical_authority_entry_oracle_pending",
    "pending_authoring",
    "default_recorded_pending_coordinator_review",
    "blocked_until_canonical_oracle_entry",
    "blocked_by_engine_gap",
    "blocked_by_status_no_until_changes",
    "not_applicable_no_round_trip_until_status_promote",
    "not_applicable_no_round_trip_in_public_build",
    "remove_by_spec_change",
    "private_profile_gate",
    "covered_by_family_diagnostic_set",
    "covered_by_parser_packet_family_only",
    "covered_by_sblr_admission_family_only",
    "covered_by_sblr_operation_matrix",
    "covered_by_sblr_operation_matrix_family_only",
    "covered_by_sblr_operation_matrix_family_only_refusal_route",
    "covered_by_sbps_admission_refusal_route_family_only",
}


# Files explicitly carved out from the scan because their entire purpose is
# to document or enforce the forbidden patterns (self-reference would
# otherwise trigger the gate against itself).
SELF_REFERENCE_EXEMPT = {
    "project/tests/sbsql_parser_worker/generated/full_surface/sbsql_no_stub_source_integrity_gate.py",
}


SCANNABLE_SUFFIXES = {".py", ".cpp", ".hpp", ".cc", ".h", ".c", ".cxx", ".inc", ".yaml", ".yml"}
# Markdown and CSV are intentionally excluded: they are regression fixture data
# and documentation that may legitimately reference forbidden patterns in
# explanatory prose. The gate's purpose is to catch stubs in executable source
# and fixture inputs.


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def is_in_scope(repo_root: Path, path: Path) -> bool:
    rel = path.relative_to(repo_root).as_posix()
    if rel in SELF_REFERENCE_EXEMPT:
        return False
    return any(rel.startswith(prefix + "/") or rel == prefix for prefix in SCOPE)


def iter_scope_files(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for prefix in SCOPE:
        base = repo_root / prefix
        if not base.exists():
            continue
        for child in base.rglob("*"):
            if not child.is_file():
                continue
            if child.suffix not in SCANNABLE_SUFFIXES:
                continue
            rel = child.relative_to(repo_root).as_posix()
            if rel in SELF_REFERENCE_EXEMPT:
                continue
            files.append(child)
    return sorted(files)


def scan_file(path: Path, repo_root: Path) -> list[tuple[int, str, str, str]]:
    findings: list[tuple[int, str, str, str]] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return findings
    rel = path.relative_to(repo_root).as_posix()
    for line_num, line in enumerate(text.splitlines(), start=1):
        for pattern, label in FORBIDDEN_PATTERNS:
            for match in pattern.finditer(line):
                # Allow if any allowed token surrounds the match on the same line.
                if any(tok in line for tok in ALLOWED_TOKENS):
                    # Still report work markers (TODO/FIXME/XXX/HACK) regardless of token presence
                    # because execution_plan transition tokens are status markers, not stubs.
                    if label in {"todo_work_marker", "fixme_work_marker", "xxx_work_marker", "hack_work_marker"}:
                        pass
                    else:
                        continue
                findings.append((line_num, label, match.group(0), line.strip()[:200]))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    files = iter_scope_files(root)
    if not files:
        fail("source integrity gate: no in-scope files found; check SCOPE configuration")

    total_findings = 0
    by_label: dict[str, int] = {}
    samples: list[str] = []

    for path in files:
        findings = scan_file(path, root)
        if findings:
            for line_num, label, match, snippet in findings:
                rel = path.relative_to(root).as_posix()
                by_label[label] = by_label.get(label, 0) + 1
                total_findings += 1
                if len(samples) < 20:
                    samples.append(f"{rel}:{line_num} [{label}] {match!r}  -- {snippet}")

    print(f"sbsql_no_stub_source_integrity_gate files_scanned={len(files)} findings={total_findings}")
    for label, count in sorted(by_label.items()):
        print(f"  {label}={count}")

    if total_findings > 0:
        print("", file=sys.stderr)
        print("sbsql_no_stub_source_integrity_gate=failed", file=sys.stderr)
        for sample in samples:
            print(f"  {sample}", file=sys.stderr)
        return 1

    print("sbsql_no_stub_source_integrity_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
