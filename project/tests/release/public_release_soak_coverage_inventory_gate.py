#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate PCR-116 public release soak coverage-inventory wiring."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


# PUBLIC_RELEASE_SOAK_COVERAGE_INVENTORY_GATE

REQUIRED_ROWS = {
    "memory_pressure",
    "memory_concurrency_reference",
    "concurrent_transactions",
    "cleanup_sweep",
    "index_maintenance",
    "backup_forward",
    "agents",
    "support_bundle_generation",
}

REQUIRED_RELEASE_CMAKE_TOKENS = (
    "NAME public_release_soak_coverage_inventory",
    "NAME public_release_soak_coverage_inventory_gate",
    "PCR-GATE-116",
    "public_release_soak_coverage_inventory_gate",
    "public_memory_pressure_executor_gate",
    "public_transaction_inventory_lock_table_gate",
    "public_transaction_savepoint_limbo_cleanup_gate",
    "public_transaction_support_bundle_gate",
    "public_index_readiness_matrix_gate",
    "public_index_durable_metadata_validator_gate",
    "public_backup_forward_session_gate",
    "public_backup_update_coverage_gate",
    "public_agent_readiness_matrix_gate",
)

REQUIRED_CONCURRENCY_CMAKE_TOKENS = (
    "memory_sanitizer_soak_concurrency_gate",
    "MMCH_MEMORY_SANITIZER_SOAK_CONCURRENCY",
)

REQUIRED_INVENTORY_LABELS = {"source_contract", "coverage_inventory", "evidence_gate"}
FORBIDDEN_BEHAVIOR_LABELS = {"fuzz", "fault_injection", "crash_reopen", "soak"}


def fail(message: str) -> None:
    print(f"public_release_soak_coverage_inventory_gate=fail:{message}", file=sys.stderr)
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


def require_inventory_labels(cmake_text: str, test_name: str) -> None:
    match = re.search(
        rf"set_tests_properties\({re.escape(test_name)}\s+PROPERTIES\s+"
        r'LABELS\s+"([^"]+)"',
        cmake_text,
        re.DOTALL,
    )
    if not match:
        fail(f"missing_test_labels:{test_name}")
    labels = set(match.group(1).split(";"))
    missing = sorted(REQUIRED_INVENTORY_LABELS - labels)
    forbidden = sorted(FORBIDDEN_BEHAVIOR_LABELS & labels)
    if missing:
        fail(f"missing_inventory_labels:{test_name}:{','.join(missing)}")
    if forbidden:
        fail(f"forbidden_behavior_labels:{test_name}:{','.join(forbidden)}")


def load_inventory(project_root: Path) -> Any:
    inventory_dir = project_root / "tests" / "soak"
    sys.path.insert(0, str(inventory_dir))
    try:
        import public_release_soak_coverage_inventory  # type: ignore
    except Exception as exc:  # pragma: no cover - reported by gate output.
        fail(f"soak_inventory_import_failed:{exc}")
    return public_release_soak_coverage_inventory


def validate_inventory(evidence: dict[str, Any]) -> None:
    if evidence.get("gate") != "PCR-GATE-116":
        fail("soak_inventory_gate_mismatch")
    if evidence.get("marker") != "PUBLIC_RELEASE_SOAK_COVERAGE_INVENTORY":
        fail("soak_inventory_marker_missing")
    execution = evidence.get("execution")
    if not isinstance(execution, dict) or any(execution.values()):
        fail("soak_inventory_must_not_claim_workload_execution")
    rows = evidence.get("rows")
    if not isinstance(rows, list):
        fail("soak_inventory_rows_missing")
    observed = {row.get("row_id") for row in rows if isinstance(row, dict)}
    missing = sorted(REQUIRED_ROWS - observed)
    if missing:
        fail("soak_inventory_missing_rows:" + ",".join(missing))
    if evidence.get("total_referenced_time_budget_seconds", 0) > 360:
        fail("soak_inventory_referenced_budget_unbounded")
    for row in rows:
        if row.get("referenced_time_budget_seconds", 0) <= 0:
            fail(f"soak_inventory_unbounded_referenced_time:{row.get('row_id')}")
        if row.get("referenced_iteration_limit", 0) <= 0:
            fail(f"soak_inventory_unbounded_referenced_iterations:{row.get('row_id')}")
        if not row.get("artifact"):
            fail(f"soak_inventory_missing_artifact:{row.get('row_id')}")


def build_evidence(project_root: Path) -> dict[str, Any]:
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")

    inventory_module = load_inventory(project_root)
    inventory_evidence = inventory_module.build_evidence(project_root)
    validate_inventory(inventory_evidence)

    release_cmake = read_required(
        project_root / "tests" / "release" / "CMakeLists.txt",
        "tests/release/CMakeLists.txt",
    )
    require_tokens(release_cmake,
                   "tests/release/CMakeLists.txt",
                   REQUIRED_RELEASE_CMAKE_TOKENS)
    for test_name in (
        "public_release_soak_coverage_inventory",
        "public_release_soak_coverage_inventory_gate",
    ):
        require_inventory_labels(release_cmake, test_name)

    concurrency_cmake = read_required(
        project_root / "tests" / "concurrency" / "CMakeLists.txt",
        "tests/concurrency/CMakeLists.txt",
    )
    require_tokens(concurrency_cmake,
                   "tests/concurrency/CMakeLists.txt",
                   REQUIRED_CONCURRENCY_CMAKE_TOKENS)

    return {
        "schema_version": 1,
        "gate": "PCR-GATE-116",
        "marker": "PUBLIC_RELEASE_SOAK_COVERAGE_INVENTORY_GATE",
        "artifact_class": "coverage_inventory_gate",
        "inventory_sha256": sha256_text(
            json.dumps(inventory_evidence, sort_keys=True)
        ),
        "inventory_row_count": inventory_evidence["row_count"],
        "inventory_total_referenced_time_budget_seconds": inventory_evidence[
            "total_referenced_time_budget_seconds"
        ],
        "inventory": inventory_evidence,
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
        "public_release_soak_coverage_inventory_gate=passed "
        f"rows={evidence['inventory_row_count']} "
        f"referenced_budget={evidence['inventory_total_referenced_time_budget_seconds']}s "
        f"output={output.name}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
