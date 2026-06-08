#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate PCR-116 bounded public release soak lane wiring."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_RELEASE_SOAK_GATE

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
    "NAME public_release_soak_lane",
    "NAME public_release_soak_gate",
    "PCR-GATE-116",
    "public_release_soak_gate",
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


def fail(message: str) -> None:
    print(f"public_release_soak_gate=fail:{message}", file=sys.stderr)
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


def load_lane(project_root: Path) -> Any:
    lane_dir = project_root / "tests" / "soak"
    sys.path.insert(0, str(lane_dir))
    try:
        import public_release_soak_lane  # type: ignore
    except Exception as exc:  # pragma: no cover - reported by gate output.
        fail(f"soak_lane_import_failed:{exc}")
    return public_release_soak_lane


def validate_lane(evidence: dict[str, Any]) -> None:
    if evidence.get("gate") != "PCR-GATE-116":
        fail("soak_lane_gate_mismatch")
    if evidence.get("marker") != "PUBLIC_RELEASE_SOAK_LANE":
        fail("soak_lane_marker_missing")
    rows = evidence.get("rows")
    if not isinstance(rows, list):
        fail("soak_lane_rows_missing")
    observed = {row.get("row_id") for row in rows if isinstance(row, dict)}
    missing = sorted(REQUIRED_ROWS - observed)
    if missing:
        fail("soak_lane_missing_rows:" + ",".join(missing))
    if evidence.get("total_time_budget_seconds", 0) > 360:
        fail("soak_lane_budget_unbounded")
    for row in rows:
        if row.get("time_budget_seconds", 0) <= 0:
            fail(f"soak_lane_unbounded_time:{row.get('row_id')}")
        if row.get("iteration_limit", 0) <= 0:
            fail(f"soak_lane_unbounded_iterations:{row.get('row_id')}")
        if not row.get("artifact"):
            fail(f"soak_lane_missing_artifact:{row.get('row_id')}")


def build_evidence(project_root: Path) -> dict[str, Any]:
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")

    lane_module = load_lane(project_root)
    lane_evidence = lane_module.build_evidence(project_root)
    validate_lane(lane_evidence)

    release_cmake = read_required(
        project_root / "tests" / "release" / "CMakeLists.txt",
        "tests/release/CMakeLists.txt",
    )
    require_tokens(release_cmake,
                   "tests/release/CMakeLists.txt",
                   REQUIRED_RELEASE_CMAKE_TOKENS)

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
        "marker": "PUBLIC_RELEASE_SOAK_GATE",
        "lane_sha256": sha256_text(json.dumps(lane_evidence, sort_keys=True)),
        "lane_row_count": lane_evidence["row_count"],
        "lane_total_time_budget_seconds": lane_evidence[
            "total_time_budget_seconds"
        ],
        "lane": lane_evidence,
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
        "public_release_soak_gate=passed "
        f"rows={evidence['lane_row_count']} "
        f"budget={evidence['lane_total_time_budget_seconds']}s "
        f"output={output.name}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
