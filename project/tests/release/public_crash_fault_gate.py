#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate PCR-115 public crash/fault release-gate wiring."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_CRASH_FAULT_GATE

REQUIRED_RELEASE_CMAKE_TOKENS = (
    "NAME public_crash_fault_matrix",
    "NAME public_crash_fault_gate",
    "PCR-GATE-115",
    "public_crash_fault_gate",
)

REQUIRED_EXISTING_GATES = (
    "public_disk_file_device_gate",
    "public_transaction_mga_cow_gate",
    "public_index_readiness_matrix_gate",
    "public_index_durable_metadata_validator_gate",
    "public_archive_before_reclaim_gate",
    "public_backup_forward_session_gate",
    "public_backup_update_coverage_gate",
    "public_repair_tamper_retention_crash_resume_gate",
    "public_security_durable_crypto_hardening_gate",
    "public_agent_action_dispatch_idempotency_gate",
)

REQUIRED_AGENT_CMAKE_TOKENS = (
    "agent_fault_injection_gate",
    "agent_security_crash_fixture_hardening_gate",
    "agent_action_dispatch_idempotency_gate",
)


def fail(message: str) -> None:
    print(f"public_crash_fault_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_required(path: Path, relative: str) -> str:
    if not path.is_file():
        fail(f"missing_file:{relative}")
    return path.read_text(encoding="utf-8")


def require_tokens(text: str, context: str, tokens: tuple[str, ...]) -> None:
    missing = [token for token in tokens if token not in text]
    if missing:
        fail(f"missing_tokens:{context}:{','.join(missing)}")


def load_matrix(project_root: Path) -> Any:
    matrix_dir = project_root / "tests" / "fault_injection"
    sys.path.insert(0, str(matrix_dir))
    try:
        import public_crash_fault_matrix  # type: ignore
    except Exception as exc:  # pragma: no cover - reported by gate output.
        fail(f"matrix_import_failed:{exc}")
    return public_crash_fault_matrix


def validate_matrix(evidence: dict[str, Any]) -> None:
    if evidence.get("gate") != "PCR-GATE-115":
        fail("matrix_gate_mismatch")
    if evidence.get("marker") != "PUBLIC_CRASH_FAULT_MATRIX":
        fail("matrix_marker_missing")
    rows = evidence.get("rows")
    if not isinstance(rows, list) or len(rows) < 9:
        fail("matrix_row_coverage_incomplete")

    required_rows = {
        "create_sync",
        "transaction_publish",
        "index_update",
        "archive_before_reclaim",
        "backup_forward",
        "repair_ledger",
        "security_state",
        "agent_action_record",
        "agent_fault_injection",
    }
    observed = {row.get("row_id") for row in rows if isinstance(row, dict)}
    missing = sorted(required_rows - observed)
    if missing:
        fail("matrix_missing_rows:" + ",".join(missing))
    for row in rows:
        if not isinstance(row, dict):
            fail("matrix_row_not_object")
        if "mga_authority" not in row or not row["mga_authority"]:
            fail(f"matrix_row_missing_mga_authority:{row.get('row_id')}")
        if row.get("file_count", 0) <= 0:
            fail(f"matrix_row_missing_evidence_files:{row.get('row_id')}")


def build_evidence(project_root: Path) -> dict[str, Any]:
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")

    matrix_module = load_matrix(project_root)
    matrix_evidence = matrix_module.build_evidence(project_root)
    validate_matrix(matrix_evidence)

    release_cmake = read_required(
        project_root / "tests" / "release" / "CMakeLists.txt",
        "tests/release/CMakeLists.txt",
    )
    require_tokens(release_cmake,
                   "tests/release/CMakeLists.txt",
                   REQUIRED_RELEASE_CMAKE_TOKENS + REQUIRED_EXISTING_GATES)

    agent_cmake = read_required(
        project_root / "tests" / "agents" / "CMakeLists.txt",
        "tests/agents/CMakeLists.txt",
    )
    require_tokens(agent_cmake,
                   "tests/agents/CMakeLists.txt",
                   REQUIRED_AGENT_CMAKE_TOKENS)

    return {
        "schema_version": 1,
        "gate": "PCR-GATE-115",
        "marker": "PUBLIC_CRASH_FAULT_GATE",
        "matrix_sha256": sha256_text(
            json.dumps(matrix_evidence, sort_keys=True)
        ),
        "matrix_row_count": matrix_evidence["row_count"],
        "existing_release_gate_count": len(REQUIRED_EXISTING_GATES),
        "agent_gate_count": len(REQUIRED_AGENT_CMAKE_TOKENS),
        "matrix": matrix_evidence,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    evidence = build_evidence(Path(args.project_root).resolve())
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(
        "public_crash_fault_gate=passed "
        f"rows={evidence['matrix_row_count']} output={output.name}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
