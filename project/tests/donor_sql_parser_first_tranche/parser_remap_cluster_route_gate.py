#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FPR-P2 cluster donor route remap proof gate."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import sys


EXPECTED_CLUSTER_ROWS = 440
EXPECTED_CLUSTER_COMMANDS = 59
EXPECTED_QUERY_DONOR_ROWS = 28
MATRIX_REL = (
    "public_execution_plan"
    "DONOR_CLUSTER_ROUTE_REMAP_MATRIX.csv"
)
INTAKE_REL = (
    "public_execution_plan"
    "CLUSTER_PARSER_REMAP_INTAKE_MATRIX.csv"
)
CLUSTER_MAP_REL = (
    "public_execution_plan"
    "DONOR_CLUSTER_TO_NORMALIZED_CLUSTER_MAP.csv"
)
HANDOFF_REL = (
    "public_execution_plan"
    "CLUSTER_DOWNSTREAM_HANDOFF_BUNDLE.csv"
)
STUB_MATRIX_REL = (
    "public_execution_plan"
    "CLUSTER_STUB_API_COMPLETENESS_MATRIX.csv"
)
AUTHORITY_MATRIX_REL = (
    "public_execution_plan"
    "CLUSTER_AUTHORITY_AND_LICENSE_POLICY_MATRIX.csv"
)
P7_REL = (
    "public_execution_plan"
    "SBLR_EXECUTION_PROOF_MATRIX.csv"
)
CLUSTER_PROVIDER_HEADER_REL = "project/src/cluster_provider/cluster_provider.hpp"
TRACKER_REL = "public_execution_plan"
GATES_REL = "public_execution_plan"

REQUIRED_COLUMNS = {
    "remap_id",
    "donor",
    "donor_cluster_command",
    "donor_surface",
    "normalized_command",
    "normalized_cluster_sblr",
    "sblr_route",
    "source_search_key",
    "donor_isolation",
    "downstream_phase",
    "cross_node_distributed_query_marker",
    "provider_api_boundary",
    "cluster_provider_boundary_status",
    "disabled_behavior",
    "enabled_stub_behavior",
    "private_provider_behavior",
    "refusal_policy",
    "route_status",
    "sblr_proof_id",
    "cluster_proof_gate",
    "local_query_authority_policy",
    "parser_authority_policy",
    "donor_authority_policy",
    "test_destination",
    "proof_gate",
    "status",
    "notes",
}

FORBIDDEN_LEAK_RE = re.compile(
    "|".join([
        re.escape("/home/" + "dcalford"),
        re.escape("Cli" + "Work"),
        "http" + "s?://",
    ])
)


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
    except FileNotFoundError:
        fail(f"missing CSV: {rel_path}")
    require(reader.fieldnames is not None, f"CSV has no header: {rel_path}")
    return rows


def row_by(rows: list[dict[str, str]], key: str) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row[key]
        require(value not in result, f"duplicate {key}: {value}")
        result[value] = row
    return result


def row_by_value(
    rows: list[dict[str, str]], key_name: str, key_function
) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for row in rows:
        value = key_function(row)
        require(value not in result, f"duplicate {key_name}: {value}")
        result[value] = row
    return result


def require_columns(rows: list[dict[str, str]], required: set[str], rel_path: str) -> None:
    require(rows, f"CSV has no rows: {rel_path}")
    missing = sorted(required - set(rows[0]))
    require(not missing, f"{rel_path} missing columns: {missing}")
    for row in rows:
        for column in required:
            require(row[column], f"{rel_path} empty {column} in row {row}")


def map_source_key(row: dict[str, str]) -> str:
    match = re.search(r"source_search_key=([^;]+)", row["parser_route"])
    require(match is not None, f"cluster map parser_route lacks source key: {row['parser_route']}")
    return match.group(1)


def parse_boundary_header(repo_root: pathlib.Path) -> tuple[dict[str, dict[str, object]], set[str], set[str]]:
    text = (repo_root / CLUSTER_PROVIDER_HEADER_REL).read_text()
    entries = re.findall(
        r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*(true|false)\s*\}',
        text,
        flags=re.S,
    )
    boundary: dict[str, dict[str, object]] = {}
    for normalized_command, provider_operation_id, routed in entries:
        require(normalized_command not in boundary,
                f"duplicate provider boundary command: {normalized_command}")
        boundary[normalized_command] = {
            "provider_operation_id": provider_operation_id,
            "provider_routed": routed == "true",
        }
    preadmission_body = text.split("RequiredClusterProviderPreAdmissionRefusalSet()", 1)[1]
    preadmission_body = preadmission_body.split("return refusals;", 1)[0]
    preadmission = set(re.findall(r'"(cluster\.[^"]+)"', preadmission_body))
    operation_set: set[str] = set()
    for command, info in boundary.items():
        if not info["provider_routed"]:
            continue
        operation_set.add(command)
        provider_operation_id = str(info["provider_operation_id"])
        if provider_operation_id != "exact_refusal_no_provider_call":
            operation_set.add(provider_operation_id)
    return boundary, operation_set, preadmission


def validate_source_anchor(repo_root: pathlib.Path, source_search_key: str) -> None:
    require("#" in source_search_key, f"source key lacks anchor: {source_search_key}")
    rel_path, anchor = source_search_key.split("#", 1)
    path = repo_root / rel_path
    require(path.exists(), f"source artifact missing for source key: {source_search_key}")
    tokens = [anchor.split(":", 1)[-1], anchor.split(":", 1)[0]]
    text = path.read_text(errors="ignore").lower()
    require(any(token and token.lower() in text for token in tokens),
            f"source artifact anchor missing for source key: {source_search_key}")


def validate_tracker_and_gate_state(
    tracker_rows: list[dict[str, str]], gate_rows: list[dict[str, str]]
) -> None:
    tracker = {row["slice_id"]: row for row in tracker_rows}
    require(tracker["FPR-P0"]["status"] == "p0_input_readiness_verified",
            "FPR-P0 must be verified")
    require(tracker["FPR-P1"]["status"] == "p1_noncluster_remap_verified",
            "FPR-P1 must be verified")
    require(tracker["FPR-P2"]["status"] == "p2_cluster_route_remap_verified",
            "FPR-P2 must be verified")
    require(tracker["FPR-P3"]["status"] in {"pending", "p3_ast_boundast_envelope_verified"},
            "FPR-P3 must be pending or verified after the AST/BoundAST envelope gate closes")
    require(tracker["FPR-P4"]["status"] in {"pending", "p4_refusal_reduction_verified"},
            "FPR-P4 must be pending or verified after the refusal reduction gate closes")
    require(tracker["FPR-P5"]["status"] in {"pending", "p5_dialect_isolation_verified"},
            "FPR-P5 must be pending or verified after the dialect isolation gate closes")
    require(tracker["FPR-P6"]["status"] in {"pending", "p6_donor_replay_proof_verified"},
            "FPR-P6 must be pending or verified after the donor replay proof gate closes")
    row = tracker["FPR-P7"]
    require(row["status"] in {"pending", "p7_parser_remap_audit_verified"},
            f"{row['slice_id']} must be pending or verified after the independent audit closes")
    gates = {row["gate_id"]: row for row in gate_rows}
    require(gates["FPR-GATE-003"]["status"] == "p2_cluster_route_remap_verified",
            "FPR-GATE-003 must be verified")
    require(gates["FPR-GATE-004"]["status"] in {"pending", "p3_ast_boundast_envelope_verified"},
            "FPR-GATE-004 must be pending or verified after the AST/BoundAST envelope gate closes")
    require(gates["FPR-GATE-005"]["status"] in {"pending", "p4_refusal_reduction_verified"},
            "FPR-GATE-005 must be pending or verified after the refusal reduction gate closes")
    require(gates["FPR-GATE-006"]["status"] in {"pending", "p5_dialect_isolation_verified"},
            "FPR-GATE-006 must be pending or verified after the dialect isolation gate closes")
    require(gates["FPR-GATE-007"]["status"] in {"pending", "p6_donor_replay_proof_verified"},
            "FPR-GATE-007 must be pending or verified after the donor replay proof gate closes")
    row = gates["FPR-GATE-008"]
    require(row["status"] in {"pending", "p7_parser_remap_audit_verified"},
            f"{row['gate_id']} must be pending or verified after the independent audit closes")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()

    matrix_rows = read_csv(repo_root, MATRIX_REL)
    intake_rows = read_csv(repo_root, INTAKE_REL)
    map_rows = read_csv(repo_root, CLUSTER_MAP_REL)
    handoff_rows = read_csv(repo_root, HANDOFF_REL)
    stub_rows = read_csv(repo_root, STUB_MATRIX_REL)
    authority_rows = read_csv(repo_root, AUTHORITY_MATRIX_REL)
    p7_rows = read_csv(repo_root, P7_REL)
    tracker_rows = read_csv(repo_root, TRACKER_REL)
    gate_rows = read_csv(repo_root, GATES_REL)

    require_columns(matrix_rows, REQUIRED_COLUMNS, MATRIX_REL)
    require(len(matrix_rows) == EXPECTED_CLUSTER_ROWS, "cluster remap row count drift")
    require(len(intake_rows) == EXPECTED_CLUSTER_ROWS, "cluster parser intake row count drift")
    require(len(map_rows) == EXPECTED_CLUSTER_ROWS, "cluster donor map row count drift")
    require(len(handoff_rows) == EXPECTED_CLUSTER_COMMANDS, "cluster handoff command count drift")
    require(len(stub_rows) == EXPECTED_CLUSTER_COMMANDS, "cluster stub command count drift")
    require(len(authority_rows) == EXPECTED_CLUSTER_COMMANDS,
            "cluster authority command count drift")

    intake_by_key = row_by(intake_rows, "source_search_key")
    matrix_by_key = row_by(matrix_rows, "source_search_key")
    map_by_key = row_by_value(map_rows, "source_search_key", map_source_key)
    handoff_by_command = row_by(handoff_rows, "normalized_command")
    stub_by_command = row_by(stub_rows, "normalized_command")
    authority_by_command = row_by(authority_rows, "normalized_command")
    p7_by_operation = row_by(
        [row for row in p7_rows if row["source_type"] == "cluster_normalization"],
        "operation_id",
    )
    require(len(p7_by_operation) == EXPECTED_CLUSTER_COMMANDS, "SBLR P7 cluster proof count drift")
    require(set(matrix_by_key) == set(intake_by_key), "matrix source keys differ from intake")
    require(set(map_by_key) == set(intake_by_key), "cluster map source keys differ from intake")

    boundary, operation_set, preadmission = parse_boundary_header(repo_root)
    require(len(boundary) == EXPECTED_CLUSTER_COMMANDS, "provider boundary command count drift")
    require(set(boundary) == set(handoff_by_command), "provider boundary commands differ from handoff")
    require("query.plan_operation" not in operation_set,
            "local query operation is admitted as cluster authority")
    require("cluster.query.refuse_local_query_as_cluster_authority" in preadmission,
            "local-query-as-cluster exact refusal missing")

    query_rows = 0
    exact_rows = 0
    routed_rows = 0
    for row in matrix_rows:
        intake = intake_by_key[row["source_search_key"]]
        cluster_map = map_by_key[row["source_search_key"]]
        for column in ["donor", "donor_surface", "normalized_command", "sblr_route", "refusal_policy"]:
            require(row[column if column != "donor_surface" else "donor_surface"] == intake[column],
                    f"matrix/intake mismatch {column}: {row['remap_id']}")
            require(intake[column] == cluster_map[column],
                    f"intake/cluster-map mismatch {column}: {row['remap_id']}")
        require(row["donor_cluster_command"] == intake["donor_surface"],
                f"donor cluster command mismatch: {row['remap_id']}")
        require(row["donor_isolation"] == "own_donor_surface_only",
                f"donor isolation is not preserved: {row['remap_id']}")
        require(row["downstream_phase"] == "FPR-P2", f"wrong downstream phase: {row['remap_id']}")
        require(row["proof_gate"] == "parser_remap_cluster_route_gate",
                f"wrong proof gate: {row['remap_id']}")
        require(row["status"] == "p2_cluster_route_remap_verified",
                f"wrong row status: {row['remap_id']}")
        require(not FORBIDDEN_LEAK_RE.search(";".join(row.values())),
                f"private path or URL leaked in row: {row['remap_id']}")
        validate_source_anchor(repo_root, row["source_search_key"])

        command = row["normalized_command"]
        handoff = handoff_by_command[command]
        stub = stub_by_command[command]
        authority = authority_by_command[command]
        proof = p7_by_operation[command]
        boundary_info = boundary[command]
        require(row["normalized_cluster_sblr"] == handoff["sblr_requirement"],
                f"SBLR requirement mismatch: {row['remap_id']}")
        require(row["sblr_route"] == handoff["sblr_requirement"],
                f"SBLR route mismatch: {row['remap_id']}")
        require(row["provider_api_boundary"] == handoff["required_provider_api"],
                f"provider API mismatch: {row['remap_id']}")
        require(row["provider_api_boundary"] == boundary_info["provider_operation_id"],
                f"provider boundary header mismatch: {row['remap_id']}")
        require(row["provider_api_boundary"] == stub["stub_api_symbol"],
                f"stub API matrix mismatch: {row['remap_id']}")
        require(authority["authority"] == handoff["authority"],
                f"authority matrix mismatch: {row['remap_id']}")
        require(row["sblr_proof_id"] == proof["proof_id"], f"P7 proof id mismatch: {row['remap_id']}")
        require(proof["proof_test_name"] == "sblr_surface_fse_p7_execution_proof_gate",
                f"wrong SBLR P7 proof gate: {row['remap_id']}")
        require(proof["parser_authority_policy"].startswith("parser_may_emit_sblr_only"),
                f"P7 parser authority policy drift: {row['remap_id']}")
        require("donor_execution_storage_transaction_finality_authority_forbidden" in
                proof["donor_authority_policy"],
                f"P7 donor authority policy drift: {row['remap_id']}")

        exact = not bool(boundary_info["provider_routed"])
        if exact:
            exact_rows += 1
            require(command in preadmission, f"exact refusal missing preadmission set: {command}")
            require(command not in operation_set, f"exact refusal admitted to provider route: {command}")
            require(row["provider_api_boundary"] == "exact_refusal_no_provider_call",
                    f"exact refusal has provider API: {row['remap_id']}")
            require(row["route_status"] == "exact_pre_provider_refusal_no_provider_route",
                    f"exact refusal route status drift: {row['remap_id']}")
            require("exact_pre_provider_refusal_no_provider_call" in row["disabled_behavior"],
                    f"exact disabled behavior drift: {row['remap_id']}")
            require("no_public_stub_provider_call" in row["enabled_stub_behavior"],
                    f"exact stub behavior drift: {row['remap_id']}")
            require("no_private_provider_call" in row["private_provider_behavior"],
                    f"exact private behavior drift: {row['remap_id']}")
            require(proof["implementation_expectation"] == "exact_refusal",
                    f"exact refusal P7 expectation drift: {row['remap_id']}")
        else:
            routed_rows += 1
            require(command in operation_set, f"routed command missing operation set: {command}")
            require(row["provider_api_boundary"] in operation_set,
                    f"provider API alias missing operation set: {row['provider_api_boundary']}")
            require(row["route_status"] == "provider_boundary_route_required",
                    f"routed command route status drift: {row['remap_id']}")
            require("kClusterSupportNotEnabledCode" in row["disabled_behavior"],
                    f"disabled message vector missing: {row['remap_id']}")
            require("unsupported_feature=cluster.provider" in row["disabled_behavior"],
                    f"disabled unsupported feature missing: {row['remap_id']}")
            require("no_provider_call" in row["disabled_behavior"],
                    f"disabled provider call guard missing: {row['remap_id']}")
            require("kClusterHandshakeStubCompileLinkOnlyCode" in row["enabled_stub_behavior"],
                    f"stub message vector missing: {row['remap_id']}")
            require("unsupported_feature=cluster.provider.stub" in row["enabled_stub_behavior"],
                    f"stub unsupported feature missing: {row['remap_id']}")
            require("no_cluster_authority_or_production_behavior" in row["enabled_stub_behavior"],
                    f"stub production guard missing: {row['remap_id']}")
            require(f"provider_api_boundary={row['provider_api_boundary']}" in
                    row["private_provider_behavior"],
                    f"private provider boundary name missing: {row['remap_id']}")
            require("no_private_source_or_implementation_detail" in row["private_provider_behavior"],
                    f"private detail guard missing: {row['remap_id']}")
            require(proof["implementation_expectation"] in {
                    "provider_boundary_fail_closed",
                    "diagnostic_or_observability",
                    },
                    f"routed P7 expectation drift: {row['remap_id']}")

        if row["cross_node_distributed_query_marker"] == (
            "cross_node_distributed_cluster_authority_not_local_query"
        ):
            query_rows += 1
            require(command.startswith("cluster.query."), f"query marker on non-query command: {command}")
            require("sblr.cluster.query.v1" in row["normalized_cluster_sblr"],
                    f"cluster query SBLR family missing: {row['remap_id']}")
            require("query.plan_operation_never_cluster_authority" in
                    row["local_query_authority_policy"],
                    f"local query authority guard missing: {row['remap_id']}")
            require(proof["cluster_query_authority_marker"] ==
                    "cross_node_distributed_cluster_authority_not_local_query",
                    f"P7 cluster query marker drift: {row['remap_id']}")
        else:
            require(row["cross_node_distributed_query_marker"] == "not_cluster_query",
                    f"unknown cluster query marker: {row['remap_id']}")

        require("parser_emits_normalized_cluster_sblr_only" in row["parser_authority_policy"],
                f"parser authority policy missing: {row['remap_id']}")
        require("donor_cluster_execution_storage_transaction_finality_authority_forbidden" in
                row["donor_authority_policy"],
                f"donor authority policy missing: {row['remap_id']}")

    require(query_rows == EXPECTED_QUERY_DONOR_ROWS, f"cluster query donor row count drift: {query_rows}")
    validate_tracker_and_gate_state(tracker_rows, gate_rows)

    evidence = {
        "gate": "parser_remap_cluster_route_gate",
        "diagnostic_output_only_not_source_authority": True,
        "cluster_remap_rows": len(matrix_rows),
        "unique_source_search_keys": len(matrix_by_key),
        "unique_normalized_commands_used": len({row["normalized_command"] for row in matrix_rows}),
        "provider_boundary_commands": len(boundary),
        "provider_routed_rows": routed_rows,
        "exact_pre_provider_refusal_rows": exact_rows,
        "cross_node_distributed_query_rows": query_rows,
        "local_query_operation_admitted": "query.plan_operation" in operation_set,
    }
    evidence_path = pathlib.Path(args.evidence_file)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
