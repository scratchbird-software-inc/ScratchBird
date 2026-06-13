#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FPR-P1 non-cluster compatibility remap proof gate."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import sys


EXPECTED_NONCLUSTER_ROWS = 1438
EXPECTED_CLASSIFICATION_COUNTS = {
    "IMPLEMENT_NONCLUSTER": 471,
    "PARSER_REMAP_ONLY": 967,
}
EXTERNAL_REFERENCE_SKIP_CODE = 77
MATRIX_REL = (
    "public_execution_plan/COMPATIBILITY_NONCLUSTER_FUNCTION_REMAP_MATRIX.csv"
)
INVENTORY_REL = (
    "public_execution_plan/COMPATIBILITY_UNSUPPORTED_NONCLUSTER_SURFACE_INVENTORY.csv"
)
DECLARED_REL = (
    "public_execution_plan/PARSER_DECLARED_SURFACE_COVERAGE_MATRIX.csv"
)
P7_REL = (
    "public_execution_plan/SBLR_EXECUTION_PROOF_MATRIX.csv"
)
P1_REL = (
    "public_execution_plan/SBLR_OPERATION_EXPANSION_REGISTER.csv"
)
ENGINE_SBLR_REL = "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"
BACKFILL_REL = (
    "public_execution_plan/SBSQL_COMPATIBILITY_ROUTE_BACKFILL_REGISTER.csv"
)
TRACKER_REL = "public_execution_plan"

REQUIRED_COLUMNS = {
    "remap_id",
    "compatibility",
    "parser_surface",
    "compatibility_surface",
    "classification",
    "source_search_key",
    "current_scratchbird_status",
    "final_sblr",
    "sblr_proof_id",
    "route_status",
    "executable_status",
    "refusal_policy",
    "parser_source_guard",
    "test_destination",
    "proof_gate",
    "status",
    "notes",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(repo_root: pathlib.Path, rel_path: str) -> list[dict[str, str]]:
    path = repo_root / rel_path
    try:
        with path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            rows = list(reader)
    except (FileNotFoundError, NotADirectoryError):
        if rel_path.startswith("public_execution_plan/") or rel_path == "public_execution_plan":
            print(f"missing CSV: {rel_path}", file=sys.stderr)
            raise SystemExit(EXTERNAL_REFERENCE_SKIP_CODE)
        fail(f"missing CSV: {rel_path}")
    require(reader.fieldnames is not None, f"CSV has no header: {rel_path}")
    return rows


def require_columns(rows: list[dict[str, str]], required: set[str], rel_path: str) -> None:
    require(rows, f"CSV has no rows: {rel_path}")
    missing = sorted(required - set(rows[0]))
    require(not missing, f"{rel_path} missing columns: {missing}")
    for row in rows:
        for column in required:
            require(row[column], f"{rel_path} empty {column} in row {row}")


def row_by(rows: list[dict[str, str]], key: str) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row[key]
        require(value not in result, f"duplicate {key}: {value}")
        result[value] = row
    return result


def parse_engine_sblr_symbols(repo_root: pathlib.Path) -> set[str]:
    text = (repo_root / ENGINE_SBLR_REL).read_text()
    return set(re.findall(r"\bsblr_operation:\s*(SBLR_[A-Z0-9_]+)", text))


def parse_backfill_symbols(rows: list[dict[str, str]]) -> set[str]:
    symbols: set[str] = set()
    for row in rows:
        for token in re.split(r"[;,]\s*", row["parser_sblr_symbols"]):
            token = token.strip()
            if token:
                symbols.add(token)
    return symbols


def source_anchor_tokens(source_search_key: str) -> tuple[pathlib.PurePosixPath, list[str]]:
    require("#" in source_search_key, f"source key lacks anchor: {source_search_key}")
    rel_path, anchor = source_search_key.split("#", 1)
    tokens: list[str] = []
    if ":" in anchor:
        tokens.append(anchor.split(":", 1)[1])
    tokens.append(anchor.split(":", 1)[0])
    return pathlib.PurePosixPath(rel_path), [token for token in tokens if token]


def validate_source_anchor(repo_root: pathlib.Path, source_search_key: str) -> None:
    rel_path, tokens = source_anchor_tokens(source_search_key)
    path = repo_root / rel_path
    require(path.exists(), f"parser source missing for source key: {source_search_key}")
    text = path.read_text(errors="ignore")
    require(
        any(token in text for token in tokens),
        f"parser source anchor not found for source key: {source_search_key}",
    )


def validate_tracker(rows: list[dict[str, str]]) -> None:
    by_id = {row["slice_id"]: row for row in rows}
    require(
        by_id["FPR-P0"]["status"] == "p0_input_readiness_verified",
        "FPR-P0 must be verified before FPR-P1 accepts remap rows",
    )
    require(
        by_id["FPR-P1"]["status"] == "p1_noncluster_remap_verified",
        "FPR-P1 tracker must record verified non-cluster remap status",
    )
    require(
        by_id["FPR-P2"]["status"] in {"pending", "p2_cluster_route_remap_verified"},
        "FPR-P2 must be pending or verified after the cluster route remap gate closes",
    )
    require(
        by_id["FPR-P3"]["status"] in {"pending", "p3_ast_boundast_envelope_verified"},
        "FPR-P3 must be pending or verified after the AST/BoundAST envelope gate closes",
    )
    require(
        by_id["FPR-P4"]["status"] in {"pending", "p4_refusal_reduction_verified"},
        "FPR-P4 must be pending or verified after the refusal reduction gate closes",
    )
    require(
        by_id["FPR-P5"]["status"] in {"pending", "p5_dialect_isolation_verified"},
        "FPR-P5 must be pending or verified after the dialect isolation gate closes",
    )
    require(
        by_id["FPR-P6"]["status"] in {"pending", "p6_compatibility_replay_proof_verified"},
        "FPR-P6 must be pending or verified after the compatibility replay proof gate closes",
    )
    row = by_id["FPR-P7"]
    require(row["status"] in {"pending", "p7_parser_remap_audit_verified"},
            f"{row['slice_id']} must be pending or verified after the independent audit closes")


def validate_authority(row: dict[str, str], proof: dict[str, str]) -> None:
    combined = ";".join(
        [
            row["refusal_policy"],
            row["notes"],
            proof["parser_authority_policy"],
            proof["compatibility_authority_policy"],
            proof["raw_sql_authority_policy"],
            proof["mga_finality_policy"],
        ]
    )
    require("parser_emits_sblr_only" in combined, f"parser emission policy missing: {row['remap_id']}")
    require("compatibility_execution_storage_transaction_finality_forbidden" in combined,
            f"compatibility authority policy missing: {row['remap_id']}")
    require("raw_sql_text_not_authoritative" in combined,
            f"raw SQL authority policy missing: {row['remap_id']}")
    if proof["authority_class"] == "MGA_TRANSACTION_FINALITY":
        require("engine" in combined and "finality" in combined,
                f"MGA finality row lacks engine-owned policy: {row['remap_id']}")


def validate_symbols(
    matrix_rows: list[dict[str, str]],
    engine_symbols: set[str],
    backfill_symbols: set[str],
) -> dict[str, int]:
    compatibility_symbol_rows = 0
    native_symbol_rows = 0
    for row in matrix_rows:
        final_sblr = row["final_sblr"]
        uppercase_tokens = set(re.findall(r"\bSBLR_[A-Z0-9_]+", final_sblr))
        for token in uppercase_tokens:
            if token.startswith("SBLR_COMPATIBILITY_"):
                compatibility_symbol_rows += 1
                require(token in backfill_symbols, f"compatibility SBLR symbol missing from backfill: {token}")
            else:
                native_symbol_rows += 1
                require(token in engine_symbols, f"native SBLR symbol missing from engine matrix: {token}")
    return {
        "compatibility_prefixed_sblr_symbol_rows": compatibility_symbol_rows,
        "native_sblr_symbol_rows": native_symbol_rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    matrix_rows = read_csv(repo_root, MATRIX_REL)
    inventory_rows = read_csv(repo_root, INVENTORY_REL)
    declared_rows = read_csv(repo_root, DECLARED_REL)
    p7_rows = read_csv(repo_root, P7_REL)
    p1_rows = read_csv(repo_root, P1_REL)
    backfill_rows = read_csv(repo_root, BACKFILL_REL)
    tracker_rows = read_csv(repo_root, TRACKER_REL)

    require_columns(matrix_rows, REQUIRED_COLUMNS, MATRIX_REL)
    require(len(matrix_rows) == EXPECTED_NONCLUSTER_ROWS, "non-cluster remap row count drift")
    require(len(inventory_rows) == EXPECTED_NONCLUSTER_ROWS, "non-cluster inventory row count drift")

    inventory_by_key = row_by(inventory_rows, "source_search_key")
    matrix_by_key = row_by(matrix_rows, "source_search_key")
    declared_keys = {row["search_key"] for row in declared_rows}
    p7_by_key = row_by(p7_rows, "source_search_key")
    p1_by_operation = row_by(p1_rows, "operation_id")

    require(set(matrix_by_key) == set(inventory_by_key), "remap matrix source keys differ from inventory")

    classification_counts = dict.fromkeys(EXPECTED_CLASSIFICATION_COUNTS, 0)
    parser_guard_counts: dict[str, int] = {}
    for row in matrix_rows:
        inventory = inventory_by_key[row["source_search_key"]]
        proof = p7_by_key.get(row["source_search_key"])
        require(proof is not None, f"SBLR P7 proof missing for {row['source_search_key']}")
        operation = p1_by_operation.get(row["final_sblr"])
        require(operation is not None, f"SBLR P1 operation missing for {row['final_sblr']}")
        require(row["compatibility"] == inventory["compatibility"], f"compatibility mismatch for {row['remap_id']}")
        require(row["parser_surface"] == inventory["parser_surface"],
                f"parser surface mismatch for {row['remap_id']}")
        require(row["compatibility_surface"] == inventory["compatibility_surface"],
                f"compatibility surface mismatch for {row['remap_id']}")
        require(row["classification"] == inventory["classification"],
                f"classification mismatch for {row['remap_id']}")
        require(row["classification"] in EXPECTED_CLASSIFICATION_COUNTS,
                f"unexpected classification: {row['classification']}")
        classification_counts[row["classification"]] += 1
        require(row["final_sblr"] == proof["operation_id"], f"final SBLR proof mismatch: {row['remap_id']}")
        require(row["sblr_proof_id"] == proof["proof_id"], f"SBLR proof id mismatch: {row['remap_id']}")
        require(operation["source_import_id"] == proof["source_import_id"],
                f"P1/P7 source import mismatch: {row['remap_id']}")
        require(operation["source_search_key"] == row["source_search_key"],
                f"P1 source search key mismatch: {row['remap_id']}")
        require(operation["source_type"] != "cluster_normalization",
                f"non-cluster remap points at cluster P1 row: {row['remap_id']}")
        require(not operation["operation_id"].startswith("cluster."),
                f"non-cluster remap points at cluster operation: {row['remap_id']}")
        require(operation["implementation_expectation"] == "executable_engine_api",
                f"P1 operation is not executable engine API: {row['remap_id']}")
        require(operation["implementation_expectation"] == proof["implementation_expectation"],
                f"P1/P7 implementation expectation mismatch: {row['remap_id']}")
        require(operation["authority_class"] == proof["authority_class"],
                f"P1/P7 authority class mismatch: {row['remap_id']}")
        require(operation["sblr_family"] == proof["sblr_family"],
                f"P1/P7 SBLR family mismatch: {row['remap_id']}")
        require(operation["status"] == "p1_operation_contract_defined",
                f"P1 operation contract is not closed: {row['remap_id']}")
        require(proof["implementation_expectation"] == "executable_engine_api",
                f"non-cluster proof is not executable engine API: {row['remap_id']}")
        require(proof["proof_test_name"] == "sblr_surface_fse_p7_execution_proof_gate",
                f"wrong SBLR proof gate for {row['remap_id']}")
        require(row["proof_gate"] == "parser_remap_noncluster_function_gate",
                f"wrong FPR proof gate for {row['remap_id']}")
        require(row["executable_status"] == "executable_sblr_proof_available",
                f"row is not executable proven: {row['remap_id']}")
        require("route_only" not in row["route_status"],
                f"route-only remap status is not accepted: {row['remap_id']}")
        require(row["source_search_key"] in declared_keys,
                f"parser declared surface row missing: {row['source_search_key']}")
        require(row["parser_source_guard"] == "parser_declared_surface_match",
                f"parser source guard not verified: {row['remap_id']}")
        parser_guard_counts[row["parser_source_guard"]] = (
            parser_guard_counts.get(row["parser_source_guard"], 0) + 1
        )
        validate_source_anchor(repo_root, row["source_search_key"])
        validate_authority(row, proof)

    require(classification_counts == EXPECTED_CLASSIFICATION_COUNTS,
            f"classification counts drift: {classification_counts}")

    engine_symbols = parse_engine_sblr_symbols(repo_root)
    backfill_symbols = parse_backfill_symbols(backfill_rows)
    require(engine_symbols, "engine SBLR operation matrix has no symbols")
    require(backfill_symbols, "compatibility backfill register has no parser symbols")
    symbol_counts = validate_symbols(matrix_rows, engine_symbols, backfill_symbols)

    validate_tracker(tracker_rows)

    evidence = {
        "gate": "parser_remap_noncluster_function_gate",
        "diagnostic_output_only_not_source_authority": True,
        "remap_rows": len(matrix_rows),
        "classification_counts": classification_counts,
        "parser_source_guard_counts": parser_guard_counts,
        "sblr_p1_operation_rows_read": len(p1_rows),
        "sblr_p7_rows_read": len(p7_rows),
        "declared_surface_rows_read": len(declared_rows),
        "engine_sblr_symbols_read": len(engine_symbols),
        "compatibility_backfill_symbols_read": len(backfill_symbols),
        **symbol_counts,
    }
    evidence_path = pathlib.Path(args.evidence_file)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
