#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FPR-P7 independent compatibility parser remap audit gate."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import sys
from collections import Counter


DIALECTS = (
    "firebird",
    "postgresql",
    "mysql",
    "sqlite",
    "mariadb",
    "duckdb",
    "clickhouse",
    "tidb",
    "vitess",
    "cockroachdb",
    "yugabytedb",
    "cassandra",
    "mongodb",
    "redis",
    "opensearch_sql_ppl",
    "opensearch",
    "neo4j",
    "influxdb",
    "milvus",
    "dolt",
    "apache_ignite",
    "tikv",
    "foundationdb",
    "immudb",
    "xtdb",
)

PARSER_REMAP_ROOT = "public_execution_plan"
P7_MATRIX_REL = f"{PARSER_REMAP_ROOT}/PARSER_DIALECT_ISOLATION_AUDIT.csv"
TRACKER_REL = f"{PARSER_REMAP_ROOT}/TRACKER.csv"
GATES_REL = f"{PARSER_REMAP_ROOT}/ACCEPTANCE_GATES.csv"
OUTPUT_CONTRACT_REL = f"{PARSER_REMAP_ROOT}/OUTPUT_CONTRACT.csv"

EXPECTED_MATRIX_ROWS = {
    "PARSER_REMAP_SCOPE_MATRIX.csv": 102,
    "COMPATIBILITY_NONCLUSTER_FUNCTION_REMAP_MATRIX.csv": 1438,
    "COMPATIBILITY_CLUSTER_ROUTE_REMAP_MATRIX.csv": 440,
    "COMPATIBILITY_PARSER_REMAP_MATRIX.csv": 2078,
    "PARSER_REFUSAL_REDUCTION_MATRIX.csv": 288,
    "DIALECT_ISOLATION_GUARD_MATRIX.csv": 125,
    "COMPATIBILITY_REPLAY_PROOF_UPDATE_MATRIX.csv": 181,
}

EXPECTED_P7_COUNTS = {
    "parser_source_independent_review": len(DIALECTS),
    "udr_source_independent_review": len(DIALECTS),
    "runtime_replay_independent_review": len(DIALECTS),
    "upstream_gate_independent_review": 8,
    "completion_semantics_independent_review": 6,
}

EXPECTED_GATE_STATUSES = {
    "FPR-GATE-001": "p0_input_readiness_verified",
    "FPR-GATE-002": "p1_noncluster_remap_verified",
    "FPR-GATE-003": "p2_cluster_route_remap_verified",
    "FPR-GATE-004": "p3_ast_boundast_envelope_verified",
    "FPR-GATE-005": "p4_refusal_reduction_verified",
    "FPR-GATE-006": "p5_dialect_isolation_verified",
    "FPR-GATE-007": "p6_compatibility_replay_proof_verified",
    "FPR-GATE-008": "p7_parser_remap_audit_verified",
}

EXPECTED_TRACKER_STATUSES = {
    "FPR-P0": "p0_input_readiness_verified",
    "FPR-P1": "p1_noncluster_remap_verified",
    "FPR-P2": "p2_cluster_route_remap_verified",
    "FPR-P3": "p3_ast_boundast_envelope_verified",
    "FPR-P4": "p4_refusal_reduction_verified",
    "FPR-P5": "p5_dialect_isolation_verified",
    "FPR-P6": "p6_compatibility_replay_proof_verified",
    "FPR-P7": "p7_parser_remap_audit_verified",
}

REQUIRED_P7_COLUMNS = {
    "audit_id",
    "audit_type",
    "compatibility",
    "parser_package",
    "parser_support_udr",
    "cross_dialect_probe",
    "result",
    "reviewer",
    "source_anchor",
    "runtime_evidence",
    "ctest_dependency",
    "authority_policy",
    "status",
    "notes",
}

P7_STATUS = "p7_parser_remap_audit_verified"
AUTHORITY_POLICY = "parser_evidence_only_engine_mga_security_storage_authority"
EXTERNAL_REFERENCE_SKIP_CODE = 77
FORBIDDEN_SOURCE_RE = re.compile(
    r"todo|fixme|skeleton|placeholder|defer|deferred|"
    r"not implemented|generated-only|generated only|file presence",
    re.I,
)
FORBIDDEN_MATRIX_RE = re.compile(
    r"todo|fixme|skeleton|placeholder|defer|deferred|"
    r"not implemented|generated-only|generated only",
    re.I,
)
FORBIDDEN_LEAK_RE = re.compile(r"(?:/(?:home|Users)/[^\s,;]+|[A-Za-z]:[\\/][^\s,;]+|local workspace|https?://)")


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(repo_root: pathlib.Path, rel: str) -> list[dict[str, str]]:
    path = repo_root / rel
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            rows = list(reader)
    except FileNotFoundError:
        fail(f"missing CSV: {rel}")
    require(reader.fieldnames is not None, f"CSV has no header: {rel}")
    return rows


def row_by(rows: list[dict[str, str]], key: str) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row[key]
        require(value not in result, f"duplicate {key}: {value}")
        result[value] = row
    return result


def require_columns(rows: list[dict[str, str]], required: set[str], rel: str) -> None:
    require(rows, f"CSV has no rows: {rel}")
    missing = sorted(required - set(rows[0]))
    require(not missing, f"{rel} missing columns: {missing}")
    for row in rows:
        for column in required:
            require(row[column], f"{rel} empty {column} in row {row}")


def validate_tracker_and_gates(repo_root: pathlib.Path) -> None:
    tracker = row_by(read_csv(repo_root, TRACKER_REL), "slice_id")
    gates = row_by(read_csv(repo_root, GATES_REL), "gate_id")
    for slice_id, status in EXPECTED_TRACKER_STATUSES.items():
        require(tracker[slice_id]["status"] == status,
                f"{slice_id} status drift: {tracker[slice_id]['status']}")
    for gate_id, status in EXPECTED_GATE_STATUSES.items():
        require(gates[gate_id]["status"] == status,
                f"{gate_id} status drift: {gates[gate_id]['status']}")


def validate_output_contract(repo_root: pathlib.Path) -> None:
    rows = row_by(read_csv(repo_root, OUTPUT_CONTRACT_REL), "output_id")
    row = rows["FPR-OUT-007"]
    require(row["artifact"] == "PARSER_DIALECT_ISOLATION_AUDIT.csv",
            "FPR-OUT-007 artifact drift")
    require(row["status"] == P7_STATUS, "FPR-OUT-007 status drift")
    required = set(row["required_columns"].split())
    missing = sorted(REQUIRED_P7_COLUMNS - required)
    require(not missing, f"FPR-OUT-007 contract missing columns: {missing}")


def validate_matrix_row_counts(repo_root: pathlib.Path) -> dict[str, int]:
    counts: dict[str, int] = {}
    for file_name, expected in EXPECTED_MATRIX_ROWS.items():
        rel = f"{PARSER_REMAP_ROOT}/{file_name}"
        rows = read_csv(repo_root, rel)
        counts[file_name] = len(rows)
        require(len(rows) == expected,
                f"{file_name} row count drift: {len(rows)} != {expected}")
        for row in rows:
            combined = ";".join(str(value) for value in row.values())
            require(not FORBIDDEN_LEAK_RE.search(combined),
                    f"private path or URL leaked in {file_name}")
    return counts


def source_files(root: pathlib.Path) -> list[pathlib.Path]:
    return sorted(
        path for path in root.rglob("*")
        if path.suffix in {".cpp", ".hpp", ".h", ".cc", ".cxx"}
    )


def read_source_tree(root: pathlib.Path) -> str:
    text_parts: list[str] = []
    for path in source_files(root):
        text_parts.append(path.read_text(encoding="utf-8"))
    return "\n".join(text_parts)


def validate_no_incomplete_markers(root: pathlib.Path, label: str) -> None:
    for path in source_files(root):
        text = path.read_text(encoding="utf-8")
        match = FORBIDDEN_SOURCE_RE.search(text)
        if match is not None:
            fail(f"{label} has incomplete marker in {path}: {match.group(0)}")


def validate_no_foreign_packages(text: str, own_compatibility: str, label: str) -> None:
    for compatibility in DIALECTS:
        if compatibility == own_compatibility:
            continue
        forbidden = [
            f"sbp_{compatibility}",
            f"sbu_{compatibility}_parser_support",
            f"{compatibility}_dialect.hpp",
        ]
        for token in forbidden:
            pattern = re.compile(
                rf"(?<![A-Za-z0-9_]){re.escape(token)}(?![A-Za-z0-9_])"
            )
            require(pattern.search(text) is None,
                    f"{label} references foreign compatibility package token {token}")


def validate_source_roots(repo_root: pathlib.Path) -> None:
    for compatibility in DIALECTS:
        parser_root = repo_root / "project/src/parsers/compatibility" / compatibility
        udr_root = repo_root / "project/src/udr" / f"sbu_{compatibility}_parser_support"
        require(parser_root.is_dir(), f"missing parser root: {parser_root}")
        require(udr_root.is_dir(), f"missing parser-support UDR root: {udr_root}")
        parser_text = read_source_tree(parser_root)
        udr_text = read_source_tree(udr_root)
        if compatibility == "firebird":
            require("FirebirdMappingStorage" in parser_text,
                    "firebird parser lacks mapping storage implementation")
        else:
            require("DialectProfile" in parser_text and "kPatterns" in parser_text,
                    f"{compatibility} parser lacks profile and pattern implementation")
        require("ParseStatement" in parser_text,
                f"{compatibility} parser lacks parse entry point")
        require("kManagementOperations" in udr_text,
                f"{compatibility} parser-support UDR lacks management operation inventory")
        require("engine_authorizes_before_udr" in udr_text,
                f"{compatibility} parser-support UDR lacks engine authorization contract")
        require("mga_transaction_authority" in udr_text,
                f"{compatibility} parser-support UDR lacks MGA authority contract")
        validate_no_incomplete_markers(parser_root, f"{compatibility} parser")
        validate_no_incomplete_markers(udr_root, f"{compatibility} UDR")
        validate_no_foreign_packages(parser_text, compatibility, f"{compatibility} parser")
        validate_no_foreign_packages(udr_text, compatibility, f"{compatibility} UDR")


def validate_evidence_files(
    isolation_evidence_path: pathlib.Path,
    compatibility_replay_evidence_path: pathlib.Path,
    first_tranche_replay_evidence_path: pathlib.Path,
) -> dict[str, object]:
    isolation = json.loads(isolation_evidence_path.read_text(encoding="utf-8"))
    require(isolation.get("gate") == "parser_dialect_isolation_guard",
            "wrong P5 isolation evidence gate")
    require(isolation.get("cross_compatibility_detection_or_dispatch_allowed") is False,
            "cross-compatibility detection became allowed")
    require(isolation.get("parser_modules_are_standalone") is True,
            "parser modules are not standalone")
    require(isolation.get("parser_support_udr_modules_are_standalone") is True,
            "parser-support UDR modules are not standalone")
    runtime_counts = isolation.get("runtime_counts", {})
    require(runtime_counts.get("runtime_positive_probes") == len(DIALECTS),
            "P5 runtime positive probe count drift")
    require(runtime_counts.get("runtime_sbsql_rejections") == len(DIALECTS),
            "P5 SBsql rejection count drift")
    require(runtime_counts.get("runtime_foreign_rejections") == 599,
            "P5 foreign rejection count drift")

    compatibility_replay = json.loads(compatibility_replay_evidence_path.read_text(encoding="utf-8"))
    require(compatibility_replay.get("gate") == "parser_compatibility_replay_proof_gate",
            "wrong P6 compatibility replay proof gate")
    require(compatibility_replay.get("validated_replay_case_count") == 121,
            "P6 replay case count drift")
    require(compatibility_replay.get("validated_staged_tool_count") == 34,
            "P6 staged tool count drift")
    require(compatibility_replay.get("validated_tool_smoke_count") == 34,
            "P6 tool smoke count drift")
    require(compatibility_replay.get("authority_policy") == AUTHORITY_POLICY,
            "P6 authority policy drift")

    first_tranche = json.loads(first_tranche_replay_evidence_path.read_text(encoding="utf-8"))
    require(first_tranche.get("gate") == "compatibility_sql_first_tranche_original_tool_replay_gate",
            "wrong first-tranche replay evidence gate")
    require(first_tranche.get("regular_ctest_gate") is True,
            "compatibility replay evidence is not from regular CTest")
    require(first_tranche.get("compatibility_tools_are_storage_authority") is False,
            "compatibility tools gained storage authority")
    require(first_tranche.get("compatibility_tools_are_transaction_authority") is False,
            "compatibility tools gained transaction authority")
    replay_counts = first_tranche.get("replay_counts_by_dialect", {})
    for compatibility in DIALECTS:
        require(int(replay_counts.get(compatibility, 0)) > 0,
                f"compatibility replay evidence missing cases for {compatibility}")
    return {
        "isolation": isolation,
        "compatibility_replay": compatibility_replay,
        "first_tranche": first_tranche,
    }


def validate_ctest_registration(build_root: pathlib.Path) -> None:
    ctest_file = build_root / "tests/compatibility_sql_parser_first_tranche/CTestTestfile.cmake"
    require(ctest_file.exists(), "compatibility SQL parser CTest file is not configured")
    text = ctest_file.read_text(encoding="utf-8")
    required_tests = {
        "parser_remap_input_readiness_gate",
        "parser_remap_noncluster_function_gate",
        "parser_remap_cluster_route_gate",
        "parser_remap_ast_boundast_envelope_gate",
        "parser_refusal_reduction_gate",
        "parser_dialect_isolation_guard",
        "compatibility_sql_first_tranche_original_tool_replay_gate",
        "parser_compatibility_replay_proof_gate",
        "parser_dialect_isolation_audit_gate",
    }
    missing = sorted(test for test in required_tests if test not in text)
    require(not missing, f"CTest registration missing parser remap gates: {missing}")


def validate_p7_matrix(repo_root: pathlib.Path) -> dict[str, int]:
    rows = read_csv(repo_root, P7_MATRIX_REL)
    require_columns(rows, REQUIRED_P7_COLUMNS, P7_MATRIX_REL)
    require(len(rows) == sum(EXPECTED_P7_COUNTS.values()),
            "P7 audit matrix row count drift")
    seen: set[str] = set()
    counts: Counter[str] = Counter()
    by_type: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        require(row["audit_id"] not in seen, f"duplicate audit_id: {row['audit_id']}")
        seen.add(row["audit_id"])
        require(row["compatibility"] in DIALECTS or row["compatibility"] == "all_compatibilitys",
                f"unknown compatibility in P7 matrix: {row['compatibility']}")
        require(row["result"] == "passed", f"P7 row not passed: {row['audit_id']}")
        require(row["status"] == P7_STATUS, f"P7 row status drift: {row['audit_id']}")
        require(row["authority_policy"] == AUTHORITY_POLICY,
                f"P7 authority policy drift: {row['audit_id']}")
        require(row["reviewer"] == "independent_parser_remap_auditor",
                f"P7 reviewer drift: {row['audit_id']}")
        combined = ";".join(row.values())
        require(not FORBIDDEN_LEAK_RE.search(combined),
                f"private path or URL leaked in P7 row: {row['audit_id']}")
        require(not FORBIDDEN_MATRIX_RE.search(combined),
                f"incomplete-work marker in P7 row: {row['audit_id']}")
        anchor = row["source_anchor"].split("#", 1)[0]
        if anchor != "not_applicable":
            require((repo_root / anchor).exists(),
                    f"P7 source anchor missing: {row['audit_id']} -> {anchor}")
        counts[row["audit_type"]] += 1
        by_type.setdefault(row["audit_type"], []).append(row)
    require(dict(counts) == EXPECTED_P7_COUNTS,
            f"P7 audit type counts drift: {dict(counts)}")

    for audit_type in (
        "parser_source_independent_review",
        "udr_source_independent_review",
        "runtime_replay_independent_review",
    ):
        compatibilitys = {row["compatibility"] for row in by_type[audit_type]}
        require(compatibilitys == set(DIALECTS), f"{audit_type} compatibility coverage drift")
    gate_ids = {row["ctest_dependency"] for row in by_type["upstream_gate_independent_review"]}
    require(gate_ids == set(EXPECTED_GATE_STATUSES),
            "P7 upstream gate dependency coverage drift")
    return dict(counts)


def write_evidence(
    path: pathlib.Path,
    matrix_counts: dict[str, int],
    p7_counts: dict[str, int],
    evidence: dict[str, object],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "gate": "parser_dialect_isolation_audit_gate",
        "authority_note": "diagnostic_output_only_not_source_authority",
        "status": P7_STATUS,
        "matrix_counts": matrix_counts,
        "p7_audit_counts": p7_counts,
        "dialects": list(DIALECTS),
        "source_roots_checked": len(DIALECTS) * 2,
        "runtime_positive_probes": evidence["isolation"]["runtime_counts"]["runtime_positive_probes"],
        "runtime_foreign_rejections": evidence["isolation"]["runtime_counts"]["runtime_foreign_rejections"],
        "runtime_sbsql_rejections": evidence["isolation"]["runtime_counts"]["runtime_sbsql_rejections"],
        "compatibility_replay_case_count": evidence["compatibility_replay"]["validated_replay_case_count"],
        "staged_native_tool_count": evidence["compatibility_replay"]["validated_staged_tool_count"],
        "authority_policy": AUTHORITY_POLICY,
        "file_existence_alone_is_completion": False,
        "generated_artifacts_alone_are_completion": False,
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def evidence_is_external_reference_skip(path: pathlib.Path) -> bool:
    if not path.is_file():
        return True
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return False
    return (
        payload.get("external_reference_fixture_status") == "missing"
        or payload.get("status") == "skipped"
    )


def write_external_reference_skip_evidence(
    path: pathlib.Path,
    skipped_inputs: list[str],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "gate": "parser_dialect_isolation_audit_gate",
        "status": "skipped",
        "skip_reason": "external_reference_fixture_missing",
        "external_reference_fixture_required": True,
        "external_reference_fixture_status": "missing",
        "skipped_inputs": skipped_inputs,
        "authority_policy": AUTHORITY_POLICY,
        "file_existence_alone_is_completion": False,
        "generated_artifacts_alone_are_completion": False,
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--dialect-isolation-evidence", required=True)
    parser.add_argument("--compatibility-replay-proof-evidence", required=True)
    parser.add_argument("--first-tranche-replay-evidence", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()
    compatibility_replay_proof_path = pathlib.Path(args.compatibility_replay_proof_evidence).resolve()
    first_tranche_replay_path = pathlib.Path(args.first_tranche_replay_evidence).resolve()
    skipped_inputs = [
        label
        for label, path in (
            ("parser_compatibility_replay_proof_gate", compatibility_replay_proof_path),
            ("compatibility_sql_first_tranche_original_tool_replay_gate", first_tranche_replay_path),
        )
        if evidence_is_external_reference_skip(path)
    ]
    if skipped_inputs:
        write_external_reference_skip_evidence(
            pathlib.Path(args.evidence_file),
            skipped_inputs,
        )
        print("parser_dialect_isolation_audit_gate=skipped external_reference_fixture_missing")
        return EXTERNAL_REFERENCE_SKIP_CODE

    validate_tracker_and_gates(repo_root)
    validate_output_contract(repo_root)
    matrix_counts = validate_matrix_row_counts(repo_root)
    validate_source_roots(repo_root)
    evidence = validate_evidence_files(
        pathlib.Path(args.dialect_isolation_evidence).resolve(),
        compatibility_replay_proof_path,
        first_tranche_replay_path,
    )
    validate_ctest_registration(build_root)
    p7_counts = validate_p7_matrix(repo_root)
    write_evidence(pathlib.Path(args.evidence_file), matrix_counts, p7_counts, evidence)
    print("parser_dialect_isolation_audit_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
