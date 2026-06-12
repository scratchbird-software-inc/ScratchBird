#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Final SBLR/SBsql enterprise proof closure gate."""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path
import sys


FEP_ROOT = Path("public_execution_plan")
LANG_ROOT = Path("public_execution_plan")
PARSER_ROOT = Path("public_execution_plan")
SBLR_ROOT = Path("public_execution_plan")
FIXTURE_ROOT = Path("project/tests/sbsql_parser_worker/fixtures/final_sblr_sbsql_closure")

EXPECTED_TRACKER = {
    "FEP-P0": "p0_project_test_destinations_locked",
    "FEP-P1": "p1_inventory_truth_gates_verified",
    "FEP-P2": "p2_sblr_sbsql_roundtrip_verified",
    "FEP-P3": "p3_parser_remap_replay_verified",
    "FEP-P4": "p4_cluster_stub_dual_mode_verified",
    "FEP-P5": "p5_reliability_security_compat_support_verified",
    "FEP-P6": "p6_release_evidence_retention_verified",
    "FEP-P7": "p7_enterprise_audit_exit_criteria_verified",
}

EXPECTED_COUNTS = {
    "PROJECT_TEST_GATE_IMPLEMENTATION_MATRIX.csv": 25,
    "FULL_BUILD_REGENERATED_PROOF_MATRIX.csv": 10,
    "CLUSTER_STUB_DUAL_MODE_TEST_MATRIX.csv": 59,
    "REFERENCE_SURFACE_COVERAGE_TRUTH_GATE_MATRIX.csv": 25,
    "SBLR_SBSQL_ROUNDTRIP_PROOF_MATRIX.csv": 2760,
    "ENTERPRISE_AUDIT_EXIT_CRITERIA.csv": 8,
    "RELEASE_EVIDENCE_RETENTION_MATRIX.csv": 10,
}

REQUIRED_TEST_LABELS = {
    "sblr_surface_fse_p7_execution_proof_gate",
    "sbsql_final_language_expansion_closure_gate",
    "parser_dialect_isolation_audit_gate",
    "reference_sql_first_tranche_original_tool_replay_gate",
    "public_cluster_provider_handshake_gate",
    "ctest_no_execution_plan_runtime_dependency_gate",
    "database_lifecycle_full_route_conformance",
    "sb_listener_sbp_sbsql_sbwp_tls_engine_auth_route_smoke",
    "final_sblr_sbsql_enterprise_proof_closure_gate",
}

FORBIDDEN_CTEST_RUNTIME_TOKENS = (
    "public_release_evidence",
    "public_release_evidence",
    "--execution_plan-root",
    "--closed-execution_plan-root",
    "--closure-execution_plan-root",
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(root: Path, rel: Path) -> list[dict[str, str]]:
    path = root / rel
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
    except FileNotFoundError:
        fail(f"missing CSV: {rel}")
    require(rows, f"CSV has no rows: {rel}")
    return rows


def keyed(rows: list[dict[str, str]], column: str, label: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row.get(column, "")
        require(value, f"{label} missing {column}")
        require(value not in out, f"{label} duplicate {column}: {value}")
        out[value] = row
    return out


def sha256(root: Path, rel: str) -> str:
    path = root / rel
    require(path.is_file(), f"hash artifact missing: {rel}")
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def validate_statuses(root: Path) -> None:
    tracker = keyed(read_csv(root, FEP_ROOT / "TRACKER.csv"), "slice_id", "FEP tracker")
    for key, status in EXPECTED_TRACKER.items():
        require(tracker[key]["status"] == status, f"{key} status drift: {tracker[key]['status']}")
    gates = read_csv(root, FEP_ROOT / "ACCEPTANCE_GATES.csv")
    for row in gates:
        require(row["status"] != "pending", f"{row['gate_id']} still pending")


def validate_matrix_shapes(root: Path) -> dict[str, list[dict[str, str]]]:
    matrices: dict[str, list[dict[str, str]]] = {}
    for name, count in EXPECTED_COUNTS.items():
        rows = read_csv(root, FEP_ROOT / name)
        require(len(rows) == count, f"{name} row count drift: {len(rows)}")
        matrices[name] = rows
    return matrices


def validate_roundtrip_coverage(root: Path, matrices: dict[str, list[dict[str, str]]]) -> None:
    sblr = keyed(read_csv(root, SBLR_ROOT / "SBLR_EXECUTION_PROOF_MATRIX.csv"),
                 "operation_id",
                 "SBLR proof")
    sbsql = keyed(read_csv(root, LANG_ROOT / "SBSQL_TO_SBLR_PROOF_MATRIX.csv"),
                  "feature",
                  "SBsql proof")
    roundtrip = keyed(matrices["SBLR_SBSQL_ROUNDTRIP_PROOF_MATRIX.csv"],
                      "surface",
                      "roundtrip proof")
    require(set(roundtrip) == set(sblr), "roundtrip proof does not cover all SBLR operations")
    require(set(roundtrip) == set(sbsql), "roundtrip proof does not cover all SBsql proof rows")
    cluster = matrices["CLUSTER_STUB_DUAL_MODE_TEST_MATRIX.csv"]
    require(sum(1 for row in cluster if "unlicensed" in row["message_vector"]) == 59,
            "cluster stub unlicensed message coverage drift")
    references = matrices["REFERENCE_SURFACE_COVERAGE_TRUTH_GATE_MATRIX.csv"]
    for row in references:
        require(row["missing_rows"] == "0", f"reference missing rows: {row['reference']}")
        require(int(row["inventory_rows"]) > 0, f"reference has no inventory rows: {row['reference']}")
        require(int(row["proof_rows"]) > 0, f"reference has no proof rows: {row['reference']}")


def validate_hashes(root: Path, matrices: dict[str, list[dict[str, str]]]) -> None:
    full = matrices["FULL_BUILD_REGENERATED_PROOF_MATRIX.csv"]
    retention = keyed(matrices["RELEASE_EVIDENCE_RETENTION_MATRIX.csv"], "evidence", "retention")
    fixture_hashes = keyed(
        read_csv(root, FIXTURE_ROOT / "FINAL_CLOSURE_EVIDENCE_HASHES.csv"),
        "evidence",
        "project/tests retained hashes",
    )
    for row in full:
        expected = sha256(root, row["generated_output"])
        require(row["hash"] == expected, f"hash drift for {row['generated_output']}")
        evidence = row["proof_family"]
        require(evidence in retention, f"retention missing {evidence}")
        require(retention[evidence]["hash"] == expected, f"retention hash drift for {evidence}")
        require(fixture_hashes[evidence]["hash"] == expected,
                f"project/tests retained hash drift for {evidence}")


def validate_ctest_registration(root: Path, build_root: Path | None) -> None:
    text_parts = [
        (root / "project/tests/sbsql_parser_worker/CMakeLists.txt").read_text(
            encoding="utf-8", errors="replace"
        ),
        (root / "project/tests/sblr_surface/CMakeLists.txt").read_text(
            encoding="utf-8", errors="replace"
        ),
        (root / "project/tests/reference_sql_parser_first_tranche/CMakeLists.txt").read_text(
            encoding="utf-8", errors="replace"
        ),
        (root / "project/tests/release/CMakeLists.txt").read_text(
            encoding="utf-8", errors="replace"
        ),
    ]
    if build_root is not None and build_root.exists():
        for ctest_file in build_root.rglob("CTestTestfile.cmake"):
            text = ctest_file.read_text(encoding="utf-8", errors="replace")
            for token in FORBIDDEN_CTEST_RUNTIME_TOKENS:
                require(token not in text, f"{ctest_file} contains forbidden runtime token {token}")
            text_parts.append(text)
    text = "\n".join(text_parts)
    for label in REQUIRED_TEST_LABELS:
        require(label in text, f"CTest/source registration missing {label}")


def validate_exit_criteria(root: Path, matrices: dict[str, list[dict[str, str]]]) -> None:
    rows = matrices["ENTERPRISE_AUDIT_EXIT_CRITERIA.csv"]
    for row in rows:
        require(row["blocker_count"] == "0", f"exit criterion blockers remain: {row['criterion']}")
        require(row["status"] == "exit_criterion_verified",
                f"exit criterion not verified: {row['criterion']}")
    project_tests = read_csv(root, FEP_ROOT / "PROJECT_TEST_DESTINATION_MATRIX.csv")
    for row in project_tests:
        require(row["project_tests_destination"].startswith("project/tests/"),
                f"proof destination outside project/tests: {row['proof_id']}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root")
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve() if args.build_root else None

    validate_statuses(root)
    matrices = validate_matrix_shapes(root)
    validate_roundtrip_coverage(root, matrices)
    validate_hashes(root, matrices)
    validate_ctest_registration(root, build_root)
    validate_exit_criteria(root, matrices)
    print("final_sblr_sbsql_enterprise_proof_closure_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
