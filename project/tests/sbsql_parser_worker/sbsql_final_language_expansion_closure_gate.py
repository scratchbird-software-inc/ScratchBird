#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Final SBsql language expansion closure gate.

The gate validates the SBsql-facing closure matrices against the final SBLR
operation proof, concrete parser/source anchors, and CTest registration. It is
intentionally stricter than file-exists checks: row sets must be bijective where
required and SBsql implementation anchors must prove native parser, binding,
lowering, diagnostics, and cluster fail-closed coverage.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys


LANG_ROOT = Path("public_execution_plan")
SBLR_ROOT = Path("public_execution_plan")

EXPECTED_TRACKER = {
    "FSL-P0": "p0_sblr_input_intake_classified",
    "FSL-P1": "p1_native_sbsql_design_complete",
    "FSL-P2": "p2_cluster_language_design_complete",
    "FSL-P3": "p3_noncluster_language_design_complete",
    "FSL-P4": "p4_parser_spec_implementation_synced",
    "FSL-P5": "p5_external_authority_policy_complete",
    "FSL-P6": "p6_native_style_review_complete",
    "FSL-P7": "p7_sbsql_to_sblr_proof_verified",
}

EXPECTED_GATES = {
    "FSL-GATE-001": "p0_sblr_input_intake_classified",
    "FSL-GATE-002": "p1_native_sbsql_design_complete",
    "FSL-GATE-003": "p2_cluster_language_design_complete",
    "FSL-GATE-004": "p3_noncluster_language_design_complete",
    "FSL-GATE-005": "p4_parser_spec_implementation_synced",
    "FSL-GATE-006": "p5_external_authority_policy_complete",
    "FSL-GATE-007": "p6_native_style_review_complete",
    "FSL-GATE-008": "p7_sbsql_to_sblr_proof_verified",
}

EXPECTED_COUNTS = {
    "SBSQL_NORMALIZED_FEATURE_EXTENSION_MATRIX.csv": 2760,
    "SBSQL_CLUSTER_COMMAND_GRAMMAR_MATRIX.csv": 59,
    "SBSQL_NONCLUSTER_COMMAND_GRAMMAR_MATRIX.csv": 2701,
    "SBSQL_EXTERNAL_AUTHORITY_COMMAND_MATRIX.csv": 380,
    "SBSQL_PARSER_SPEC_UPDATE_MATRIX.csv": 2760,
    "SBSQL_TO_SBLR_PROOF_MATRIX.csv": 2760,
    "SBSQL_STYLE_REVIEW_MATRIX.csv": 2760,
}

REQUIRED_COLUMNS = {
    "SBSQL_NORMALIZED_FEATURE_EXTENSION_MATRIX.csv": {
        "feature",
        "native_sbsql_surface",
        "sblr_symbol",
        "style_decision",
        "implementation_status",
        "command_family",
        "authority_class",
        "implementation_expectation",
        "source_search_key",
        "ctest_label",
    },
    "SBSQL_CLUSTER_COMMAND_GRAMMAR_MATRIX.csv": {
        "normalized_cluster_command",
        "sbsql_syntax",
        "sblr_symbol",
        "disabled_behavior",
        "enabled_stub_behavior",
        "provider_api",
        "cluster_query_marker",
        "ctest_label",
        "implementation_status",
    },
    "SBSQL_NONCLUSTER_COMMAND_GRAMMAR_MATRIX.csv": {
        "feature",
        "sbsql_syntax",
        "sblr_symbol",
        "result_shape",
        "test_destination",
        "command_family",
        "implementation_status",
    },
    "SBSQL_EXTERNAL_AUTHORITY_COMMAND_MATRIX.csv": {
        "feature",
        "provider_boundary",
        "admission_policy",
        "fail_closed_message",
        "test_destination",
        "sblr_symbol",
        "implementation_status",
    },
    "SBSQL_PARSER_SPEC_UPDATE_MATRIX.csv": {
        "feature",
        "spec_file",
        "parser_file",
        "generated_resource",
        "ast_boundast_status",
        "command_family",
        "lowering_gate",
        "implementation_status",
    },
    "SBSQL_TO_SBLR_PROOF_MATRIX.csv": {
        "feature",
        "parser_test",
        "lowering_test",
        "execution_test",
        "refusal_test",
        "ctest_label",
        "sblr_symbol",
        "implementation_status",
    },
    "SBSQL_STYLE_REVIEW_MATRIX.csv": {
        "feature",
        "donor_syntax_risk",
        "style_result",
        "reviewer",
        "evidence",
        "native_sbsql_surface",
        "implementation_status",
    },
}

REQUIRED_SOURCE_TOKENS = {
    "project/tests/sbsql_parser_worker/fixtures/final_sblr_sbsql_closure/SBSQL_CONTEXT_SENSITIVE_KEYWORD_POLICY.csv": [
        "SBSQL is context-sensitive",
        "near-empty global reserved-word set",
        "sbsql_scalar_syntax_exact_route_conformance",
    ],
    "public_execution_plan": [
        "FSL-STYLE-007",
        "context-sensitive grammar tokens",
        "near-empty global reserved-word set",
    ],
    "project/src/parsers/sbsql_worker/lexer/lexer.cpp": [
        "SBsql is context-sensitive",
        "not define a broad SQL reserved",
        "contextual_native",
        "contextual_literal",
    ],
    "project/src/parsers/shared/sbsql_v3_ast/sbsql_v3_ast_catalog.cpp": [
        "sbsql.private_cluster",
        "PrivateClusterAst",
        "sbsql.observability",
        "ArchiveReplicationMigrationAst",
    ],
    "project/src/parsers/shared/sbsql_v3_binding/sbsql_v3_binding_catalog.cpp": [
        "sbsql.private_cluster",
        "CLUSTER_AUTHORITY_REQUIRED",
        "private_cluster_catalog",
    ],
    "project/src/parsers/shared/sbsql_v3_api_mapping/sbsql_v3_api_mapping_catalog.cpp": [
        "SBSQL_V3_RAW_SQL_FALLBACK_FORBIDDEN",
        "SBSQL_V3_CLUSTER_AUTHORITY_REQUIRED",
        "cluster authority mappings must fail closed",
    ],
    "project/src/parsers/sbsql_worker/statement/statement_catalog.cpp": [
        "sbsql.emulated.backup_restore_non_file",
        "SBSQL.EMULATION.NON_FILE_OPERATION",
        "observability",
    ],
}

REQUIRED_CTEST_TOKENS = {
    "sbsql_surface_to_sblr_function_coverage_gate",
    "sbsql_no_stub_source_integrity_gate",
    "sbsql_sblr_binary_round_trip_fixture_gate",
    "sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance",
    "sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance",
    "sbsql_cluster_private_fail_closed_conformance",
    "sbsql_non_core_optional_provider_classification_conformance",
    "sbsql_observability_exact_route_conformance",
    "sbsql_final_language_expansion_closure_gate",
}

FORBIDDEN_STATUS_TOKENS = (
    "pending",
    "todo",
    "tbd",
    "skeleton",
    "placeholder",
    "file_presence",
    "generated_only",
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
        require(value, f"{label} row missing {column}")
        require(value not in out, f"{label} duplicate {column}: {value}")
        out[value] = row
    return out


def validate_statuses(root: Path) -> None:
    tracker = keyed(read_csv(root, LANG_ROOT / "TRACKER.csv"), "slice_id", "FSL tracker")
    gates = keyed(read_csv(root, LANG_ROOT / "ACCEPTANCE_GATES.csv"), "gate_id", "FSL gates")
    for key, status in EXPECTED_TRACKER.items():
        require(tracker[key]["status"] == status, f"{key} status drift: {tracker[key]['status']}")
    for key, status in EXPECTED_GATES.items():
        require(gates[key]["status"] == status, f"{key} status drift: {gates[key]['status']}")


def validate_matrix_shapes(root: Path) -> dict[str, list[dict[str, str]]]:
    matrices: dict[str, list[dict[str, str]]] = {}
    for name, expected_count in EXPECTED_COUNTS.items():
        rows = read_csv(root, LANG_ROOT / name)
        require(len(rows) == expected_count, f"{name} row count drift: {len(rows)}")
        missing = sorted(REQUIRED_COLUMNS[name] - set(rows[0]))
        require(not missing, f"{name} missing columns: {missing}")
        for row in rows:
            for column in REQUIRED_COLUMNS[name]:
                require(row[column], f"{name} empty {column}")
        matrices[name] = rows
    return matrices


def validate_cross_matrix_coverage(root: Path, matrices: dict[str, list[dict[str, str]]]) -> None:
    sblr_rows = read_csv(root, SBLR_ROOT / "SBLR_EXECUTION_PROOF_MATRIX.csv")
    sblr_by_op = keyed(sblr_rows, "operation_id", "SBLR execution proof")
    sblr_ops = set(sblr_by_op)
    cluster_intake = read_csv(root, LANG_ROOT / "CLUSTER_SBSQL_LANGUAGE_INTAKE_MATRIX.csv")
    cluster_commands = {row["normalized_command"] for row in cluster_intake}
    external_ops = {
        row["operation_id"]
        for row in sblr_rows
        if row["authority_class"] == "EXTERNAL_PROVIDER_ACCESS"
        or row["implementation_expectation"] == "external_authority_fail_closed"
    }
    cluster_ops = {row["operation_id"] for row in sblr_rows if row["operation_id"].startswith("cluster.")}

    for name in (
        "SBSQL_NORMALIZED_FEATURE_EXTENSION_MATRIX.csv",
        "SBSQL_PARSER_SPEC_UPDATE_MATRIX.csv",
        "SBSQL_TO_SBLR_PROOF_MATRIX.csv",
        "SBSQL_STYLE_REVIEW_MATRIX.csv",
    ):
        features = {row["feature"] for row in matrices[name]}
        require(features == sblr_ops, f"{name} feature set does not match SBLR proof")

    noncluster = {row["feature"] for row in matrices["SBSQL_NONCLUSTER_COMMAND_GRAMMAR_MATRIX.csv"]}
    require(noncluster == sblr_ops - cluster_ops, "non-cluster grammar matrix coverage drift")

    external = {row["feature"] for row in matrices["SBSQL_EXTERNAL_AUTHORITY_COMMAND_MATRIX.csv"]}
    require(external == external_ops, "external authority matrix coverage drift")

    cluster = {
        row["normalized_cluster_command"]
        for row in matrices["SBSQL_CLUSTER_COMMAND_GRAMMAR_MATRIX.csv"]
    }
    require(cluster == cluster_commands, "cluster grammar matrix coverage drift")

    query_rows = [
        row
        for row in matrices["SBSQL_CLUSTER_COMMAND_GRAMMAR_MATRIX.csv"]
        if row["cluster_query_marker"] != "not_cluster_query"
    ]
    require(len(query_rows) == 9, "cluster query grammar coverage drift")
    for row in query_rows:
        require(row["sbsql_syntax"].startswith("PLAN CLUSTER QUERY"),
                f"cluster query syntax is not native PLAN CLUSTER QUERY: {row['normalized_cluster_command']}")

    for name, rows in matrices.items():
        for row in rows:
            if name in {
                "SBSQL_NORMALIZED_FEATURE_EXTENSION_MATRIX.csv",
                "SBSQL_STYLE_REVIEW_MATRIX.csv",
            }:
                policy_col = (
                    "style_decision"
                    if name == "SBSQL_NORMALIZED_FEATURE_EXTENSION_MATRIX.csv"
                    else "style_result"
                )
                require(
                    "context_sensitive_keyword_policy" in row[policy_col],
                    f"{name} missing context-sensitive keyword policy marker for {row['feature']}",
                )
            for key, value in row.items():
                if key.endswith("status") or key in {"style_result", "style_decision"}:
                    lowered = value.lower().replace("-", "_").replace(" ", "_")
                    for token in FORBIDDEN_STATUS_TOKENS:
                        require(token not in lowered, f"{name} {key} contains {token}: {value}")


def validate_auxiliary_policy_matrices(root: Path) -> None:
    style_rows = read_csv(root, LANG_ROOT / "SBSQL_STYLE_AND_DONOR_NEUTRALITY_MATRIX.csv")
    require(len(style_rows) == 7, "style neutrality matrix must include context-sensitive keyword rule")
    require(
        any(row["review_id"] == "FSL-STYLE-007" for row in style_rows),
        "style neutrality matrix missing FSL-STYLE-007 context-sensitive keyword rule",
    )
    for row in style_rows:
        require(
            "pending" not in row["status"].lower(),
            f"style neutrality row is not closed: {row['review_id']}",
        )
        if row["review_id"] == "FSL-STYLE-007":
            require(
                "context-sensitive grammar tokens" in row["requirement"],
                "FSL-STYLE-007 does not encode context-sensitive keyword policy",
            )

    scope_rows = read_csv(root, LANG_ROOT / "SBSQL_EXPANSION_SCOPE_MATRIX.csv")
    require(len(scope_rows) == 6, "SBsql expansion scope matrix row count drift")
    for row in scope_rows:
        require(
            row["status"] == "closed_by_final_language_expansion_gate",
            f"scope row is not closed: {row['scope_id']} {row['status']}",
        )


def validate_source_anchors(root: Path) -> None:
    for rel, tokens in REQUIRED_SOURCE_TOKENS.items():
        path = root / rel
        require(path.is_file(), f"source anchor missing: {rel}")
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in tokens:
            require(token in text, f"{rel} missing token {token}")


def validate_context_sensitive_keyword_policy(root: Path) -> None:
    lexer = (root / "project/src/parsers/sbsql_worker/lexer/lexer.cpp").read_text(
        encoding="utf-8", errors="replace"
    )
    forbidden_lexer_tokens = (
        "static const std::unordered_set<std::string> reserved",
        "Contains(reserved",
        'return "reserved_native"',
        '"reserved_native",',
    )
    for token in forbidden_lexer_tokens:
        require(token not in lexer, f"SBsql lexer encodes a broad reserved-word model: {token}")
    require('"SELECT"' in lexer and '"WHERE"' in lexer,
            "SBsql lexer lost core command-word classification evidence")
    require('return "contextual_native"' in lexer,
            "SBsql lexer does not classify command words as contextual")


def validate_ctest_registration(root: Path, build_root: Path | None) -> None:
    text = (root / "project/tests/sbsql_parser_worker/CMakeLists.txt").read_text(
        encoding="utf-8", errors="replace"
    )
    if build_root is not None:
        for ctest_file in build_root.rglob("CTestTestfile.cmake"):
            if "sbsql_parser_worker" in ctest_file.as_posix():
                text += "\n" + ctest_file.read_text(encoding="utf-8", errors="replace")
    for token in REQUIRED_CTEST_TOKENS:
        require(token in text, f"CTest registration missing {token}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve() if args.build_root else None

    validate_statuses(root)
    matrices = validate_matrix_shapes(root)
    validate_cross_matrix_coverage(root, matrices)
    validate_auxiliary_policy_matrices(root)
    validate_source_anchors(root)
    validate_context_sensitive_keyword_policy(root)
    validate_ctest_registration(root, build_root)
    print("sbsql_final_language_expansion_closure_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
