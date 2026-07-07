#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Block reference parser release completion while proof rows remain open.

This gate is intentionally stricter than the structural reference gates. The
structural gates prove that manifests, package surfaces, and acquisition
contracts are shaped correctly. This gate proves that those release-facing
rows no longer describe future, pending, generated-only, stubbed, or
not-implemented work.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


REFERENCE_ROOT = "project/tests/reference_regression/"
COMPATIBILITY_ROOT = "project/src/parsers/compatibility/"
PACKAGE_ROOT = "packaging/2026.07.03/reference-parsers/"
PUBLIC_EXECUTION_PLAN_ROOT = "public_execution_plan/"
PUBLIC_RELEASE_EVIDENCE_ROOT = "public_release_evidence/"
SCAN_ROOTS = (
    REFERENCE_ROOT,
    COMPATIBILITY_ROOT,
    PACKAGE_ROOT,
    PUBLIC_EXECUTION_PLAN_ROOT,
    PUBLIC_RELEASE_EVIDENCE_ROOT,
)

BLOCKING_RE = re.compile(
    r"(^|[_\\s-])("
    r"pending|blocked|not[_\\s-]?implemented|stub|skeleton|future|"
    r"defer(?:red|ral|)?|todo|tbd|fixme|placeholder"
    r")($|[_\\s-])",
    re.IGNORECASE,
)

STATUS_FIELD_NAMES = {
    "status",
    "current_status",
    "source_status",
    "source_matrix_status",
    "source_review_status",
    "official_resource_status",
    "implementation_status",
    "completion_status",
    "gate_status",
    "proof_status",
    "replay_status",
    "release_status",
    "readiness_status",
    "state",
    "result",
    "verdict",
}

STATUS_SUFFIXES = ("_status", "_state", "_result", "_verdict")

ALLOWED_CLUSTER_RE = re.compile(r"cluster.*stub|stub.*cluster", re.IGNORECASE)


def git_files(repo_root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", *SCAN_ROOTS],
        cwd=repo_root,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    files: list[Path] = []
    for line in result.stdout.splitlines():
        if not line:
            continue
        path = repo_root / line
        if path.suffix.lower() in {".csv", ".json"}:
            files.append(path)
    return files


def rel(path: Path, repo_root: Path) -> str:
    return path.relative_to(repo_root).as_posix()


def is_blocking_value(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    text = value.strip()
    if not text:
        return False
    if ALLOWED_CLUSTER_RE.search(text):
        return False
    return bool(BLOCKING_RE.search(text))


def scan_csv(path: Path, repo_root: Path) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    with path.open(newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        fieldnames = reader.fieldnames or []
        status_columns = [name for name in fieldnames if is_status_field(name)]
        for row_number, row in enumerate(reader, start=2):
            for column in status_columns:
                value = (row.get(column) or "").strip()
                if is_blocking_value(value):
                    findings.append({
                        "path": rel(path, repo_root),
                        "row": row_number,
                        "column": column,
                        "value": value,
                    })
    return findings


def scan_json_value(path: Path,
                    repo_root: Path,
                    value: Any,
                    key_path: str,
                    findings: list[dict[str, Any]]) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            child_path = f"{key_path}.{key}" if key_path else str(key)
            if is_status_field(str(key)) and is_blocking_value(item):
                findings.append({
                    "path": rel(path, repo_root),
                    "json_path": child_path,
                    "value": item,
                })
            scan_json_value(path, repo_root, item, child_path, findings)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            child_path = f"{key_path}[{index}]" if key_path else f"[{index}]"
            scan_json_value(path, repo_root, item, child_path, findings)


def is_status_field(name: str) -> bool:
    lower = name.strip().lower()
    return lower in STATUS_FIELD_NAMES or lower.endswith(STATUS_SUFFIXES)


def scan_json(path: Path, repo_root: Path) -> list[dict[str, Any]]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [{
            "path": rel(path, repo_root),
            "json_path": "$",
            "value": f"invalid_json:{exc}",
        }]
    findings: list[dict[str, Any]] = []
    scan_json_value(path, repo_root, payload, "", findings)
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--evidence-file", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    findings: list[dict[str, Any]] = []
    scanned = 0
    for path in git_files(repo_root):
        scanned += 1
        if path.suffix.lower() == ".csv":
            findings.extend(scan_csv(path, repo_root))
        elif path.suffix.lower() == ".json":
            findings.extend(scan_json(path, repo_root))

    by_value: dict[str, int] = {}
    by_path: dict[str, int] = {}
    for finding in findings:
        value = str(finding.get("value", ""))
        by_value[value] = by_value.get(value, 0) + 1
        path = str(finding.get("path", ""))
        by_path[path] = by_path.get(path, 0) + 1

    evidence = {
        "schema_id": "scratchbird.reference_release_completion_blocker_gate.v1",
        "status": "fail" if findings else "pass",
        "scanned_files": scanned,
        "blocking_finding_count": len(findings),
        "blocking_values": dict(sorted(by_value.items(), key=lambda item: (-item[1], item[0]))),
        "blocking_paths": dict(sorted(by_path.items(), key=lambda item: (-item[1], item[0]))),
        "findings": findings[:1000],
    }

    if args.evidence_file:
        args.evidence_file.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_file.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if findings:
        print(
            "reference_release_completion_blocker_gate: FAIL "
            f"blocking_finding_count={len(findings)} scanned_files={scanned}"
        )
        for value, count in sorted(by_value.items(), key=lambda item: (-item[1], item[0]))[:20]:
            print(f"{count}: {value}")
        return 1

    print(f"reference_release_completion_blocker_gate: PASS scanned_files={scanned}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
