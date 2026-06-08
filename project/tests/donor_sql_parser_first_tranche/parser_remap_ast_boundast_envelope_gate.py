#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FPR-P3 AST/BoundAST/SBLR envelope remap proof gate."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import subprocess
import sys


EXPECTED_DECLARED_ROWS = 2078
EXPECTED_KPATTERN_ROWS = 933
MATRIX_REL = (
    "public_execution_plan"
    "DONOR_PARSER_REMAP_MATRIX.csv"
)
DECLARED_REL = (
    "public_execution_plan"
    "PARSER_DECLARED_SURFACE_COVERAGE_MATRIX.csv"
)
NONCLUSTER_REL = (
    "public_execution_plan"
    "DONOR_NONCLUSTER_FUNCTION_REMAP_MATRIX.csv"
)
CLUSTER_REL = (
    "public_execution_plan"
    "DONOR_CLUSTER_ROUTE_REMAP_MATRIX.csv"
)
P7_REL = (
    "public_execution_plan"
    "SBLR_EXECUTION_PROOF_MATRIX.csv"
)
TRACKER_REL = "public_execution_plan"
GATES_REL = "public_execution_plan"

REQUIRED_COLUMNS = {
    "declared_row_id",
    "declared_row_ordinal",
    "parser_package",
    "declared_surface",
    "classification",
    "source_search_key",
    "current_route",
    "final_route",
    "final_sblr",
    "runtime_disposition",
    "runtime_evidence_policy",
    "refusal_policy",
    "parser_authority_policy",
    "donor_authority_policy",
    "mga_authority_policy",
    "source_guard",
    "joined_remap_id",
    "proof_gate",
    "status",
    "notes",
}

SOURCE_ENTRY_RE = re.compile(
    r'\{\s*"(?P<match>(?:[^"\\]|\\.)*)"\s*,\s*PatternMatch::k(?P<kind>\w+)\s*,'
    r'\s*"(?P<stmt>[^"]*)"\s*,\s*"(?P<op>[^"]*)"\s*,\s*'
    r'MappingDisposition::k(?P<disp>\w+)\s*,\s*"(?P<mapping>[^"]*)"\s*,\s*'
    r'"(?P<sblr>[^"]*)"\s*,\s*"(?P<engine>[^"]*)"\s*,\s*"(?P<diag>[^"]*)"\s*,\s*'
    r'"(?P<msg>[^"]*)"\s*,\s*(?P<sec>true|false)\s*,\s*(?P<txn>true|false)\s*\}',
    re.S,
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


def require_columns(rows: list[dict[str, str]], required: set[str], rel_path: str) -> None:
    require(rows, f"CSV has no rows: {rel_path}")
    missing = sorted(required - set(rows[0]))
    require(not missing, f"{rel_path} missing columns: {missing}")
    for row in rows:
        for column in required:
            require(row[column], f"{rel_path} empty {column} in {row}")


def row_by(rows: list[dict[str, str]], key: str) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row[key]
        require(value not in result, f"duplicate {key}: {value}")
        result[value] = row
    return result


def declared_key(row: dict[str, str]) -> tuple[str, str, str, str, str]:
    return (
        row["parser_package"],
        row["declared_surface"],
        row["current_route"],
        row["search_key"],
        row["classification"],
    )


def matrix_declared_key(row: dict[str, str]) -> tuple[str, str, str, str, str]:
    return (
        row["parser_package"],
        row["declared_surface"],
        row["current_route"],
        row["source_search_key"],
        row["classification"],
    )


def source_path_and_anchor(source_search_key: str) -> tuple[pathlib.PurePosixPath, str, str]:
    require("#" in source_search_key, f"source key lacks anchor: {source_search_key}")
    rel_path, anchor = source_search_key.split("#", 1)
    anchor_family = anchor.split(":", 1)[0]
    anchor_value = anchor.split(":", 1)[1] if ":" in anchor else anchor_family
    return pathlib.PurePosixPath(rel_path), anchor_family, anchor_value


def parse_source_entries(repo_root: pathlib.Path, source_paths: set[str]) -> dict[tuple[str, str], dict[str, str]]:
    entries: dict[tuple[str, str], dict[str, str]] = {}
    for rel in source_paths:
        path = repo_root / rel
        require(path.exists(), f"source path missing: {rel}")
        text = path.read_text(errors="ignore")
        for match in SOURCE_ENTRY_RE.finditer(text):
            groups = match.groupdict()
            entries[(rel, groups["op"])] = groups
    return entries


def sample_sql(entry: dict[str, str]) -> str:
    token = entry["match"]
    prefix = ""
    if entry["kind"] == "PrefixAndContains" and "||" in token:
        prefix, token = token.split("||", 1)
    if entry["kind"] == "ContainsFunctionCall":
        if token == "LOAD_FILE":
            return "SELECT LOAD_FILE('/tmp/x')"
        return f"SELECT {token}()"
    if entry["kind"] == "FromStringLiteralUriScheme":
        scheme = token.split("||", 1)[0].lower()
        if scheme.startswith("s3://"):
            return "SELECT * FROM 's3://scratchbird-parser-gate/x.parquet'"
        return f"SELECT * FROM '{scheme}example.test/scratchbird-parser-gate.csv'"
    if entry["kind"] == "RelationReference":
        return f"SELECT * FROM {token}"
    if entry["kind"] in {"LoadDataLocalInfile", "LoadDataServerInfile"}:
        if entry["kind"] == "LoadDataLocalInfile":
            return "LOAD DATA LOCAL INFILE 'client.csv' INTO TABLE t"
        return "LOAD DATA INFILE '/tmp/x.csv' INTO TABLE t"
    if entry["kind"] == "CreateTableEngineClause":
        return f"CREATE TABLE t (id Int32) ENGINE = {token}('broker:9092', 'topic', 'group', 'JSONEachRow')"
    if entry["kind"] == "RestPathSegment":
        if token == "_SEARCH":
            return 'POST /accounts/_search {"query":{"match_all":{}}}'
        if token == "_MSEARCH":
            return 'POST /_msearch {"query":{"match_all":{}}}'
        if token == "_BULK":
            return 'POST /_bulk {"index":{}}'
        if token == "_MGET":
            return 'GET /accounts/_mget'
        if token == "_MAPPING":
            return 'GET /accounts/_mapping'
        if token == "_ALIASES":
            return 'POST /_aliases {"actions":[]}'
        if token == "_INGEST/PIPELINE":
            return 'PUT /_ingest/pipeline/default {"processors":[]}'
        if token == "_SECURITY":
            return 'GET /_plugins/_security/api/roles'
        if token == "_CAT":
            return 'GET /_cat/indices'
        return f"GET /{token.lower()}"
    if entry["kind"] == "RestMethodRoute":
        first_route = token.split("||", 1)[0]
        method, path = first_route.split(" ", 1)
        if "_PPL" in path:
            return f"{method} {path} {{\"query\":\"source=accounts | stats count() by state\"}}"
        return f"{method} {path} {{\"query\":\"select count(*) from accounts\"}}"
    if entry["kind"] == "PplPipelineStage":
        stage = token.strip()
        if stage == "STATS":
            return "source=accounts | stats count() by state"
        if stage == "LOOKUP":
            return "source=accounts | lookup region_lookup state"
        if stage == "JOIN":
            return "source=accounts | join orders on account_id"
        if stage == "ML":
            return "ml predict model_id='m1'"
        if stage == "AD":
            return "source=accounts | ad threshold=3"
        return f"source=accounts | {stage.lower()}"
    if token.endswith("("):
        token = token + ")"
    if entry["kind"] in {"Contains", "PrefixAndContains"}:
        operation = entry["op"]
        if "copy_program" in operation or " PROGRAM " in token:
            return "COPY t PROGRAM x"
        if "copy_to_file" in operation or " TO '" in token:
            return "COPY t TO 'x.csv'"
        if "copy_from_file" in operation or " FROM '" in token:
            return "COPY t FROM 'x.csv'"
        if "cluster_clause" in operation or token == "CLUSTER":
            return "CREATE TABLE t ON CLUSTER c"
        if "experimental_relocate" in operation:
            return "EXPERIMENTAL_RELOCATE lease"
        if token.startswith(","):
            return "SELECT " + token
        suffix = "TASK" if token.endswith(" ") else ""
        if prefix:
            return prefix + " t " + token.strip() + suffix
        return "SELECT " + token + suffix
    return token


def binary_path(build_root: pathlib.Path, parser_package: str) -> pathlib.Path:
    donor = pathlib.PurePosixPath(parser_package).name
    return build_root / "src/parsers/donor" / donor / f"sbp_{donor}"


def run_parser(binary: pathlib.Path, sql_text: str) -> tuple[int, str, str]:
    completed = subprocess.run(
        [str(binary), sql_text],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )
    return completed.returncode, completed.stdout, completed.stderr


def validate_envelope_common(row: dict[str, str], output: str) -> None:
    require("SBLRExecutionEnvelope.v3" in output, f"missing envelope: {row['declared_row_id']}")
    for token in [
        '"cst_materialized":true',
        '"ast_materialized":true',
        '"bound_ast_materialized":true',
        '"descriptor_uuid_required":true',
        '"source_text_redacted":true',
        '"sql_text_included":false',
        '"donor_engine_sql_executed":false',
        '"real_donor_file_effects":false',
        '"parser_transaction_finality_authority":false',
        '"parser_storage_authority":false',
    ]:
        require(token in output, f"runtime evidence token missing {token}: {row['declared_row_id']}")
    require('"query.plan_operation"' not in output,
            f"local query operation leaked into runtime output: {row['declared_row_id']}")


def validate_tracker_and_gate_state(
    tracker_rows: list[dict[str, str]], gate_rows: list[dict[str, str]]
) -> None:
    tracker = {row["slice_id"]: row for row in tracker_rows}
    require(tracker["FPR-P0"]["status"] == "p0_input_readiness_verified", "FPR-P0 not verified")
    require(tracker["FPR-P1"]["status"] == "p1_noncluster_remap_verified", "FPR-P1 not verified")
    require(tracker["FPR-P2"]["status"] == "p2_cluster_route_remap_verified", "FPR-P2 not verified")
    require(tracker["FPR-P3"]["status"] == "p3_ast_boundast_envelope_verified",
            "FPR-P3 tracker not verified")
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
    require(gates["FPR-GATE-004"]["status"] == "p3_ast_boundast_envelope_verified",
            "FPR-GATE-004 not verified")
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
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()

    matrix_rows = read_csv(repo_root, MATRIX_REL)
    declared_rows = read_csv(repo_root, DECLARED_REL)
    noncluster_rows = read_csv(repo_root, NONCLUSTER_REL)
    cluster_rows = read_csv(repo_root, CLUSTER_REL)
    p7_rows = read_csv(repo_root, P7_REL)
    tracker_rows = read_csv(repo_root, TRACKER_REL)
    gate_rows = read_csv(repo_root, GATES_REL)

    require_columns(matrix_rows, REQUIRED_COLUMNS, MATRIX_REL)
    require(len(matrix_rows) == EXPECTED_DECLARED_ROWS, "P3 declared matrix row count drift")
    require(len(declared_rows) == EXPECTED_DECLARED_ROWS, "declared surface row count drift")
    require(
        [matrix_declared_key(row) for row in matrix_rows] ==
        [declared_key(row) for row in declared_rows],
        "P3 matrix does not preserve declared rows one-to-one and in order",
    )
    require(
        [int(row["declared_row_ordinal"]) for row in matrix_rows] ==
        list(range(1, EXPECTED_DECLARED_ROWS + 1)),
        "declared row ordinals are not stable and contiguous",
    )

    noncluster_by_key = row_by(noncluster_rows, "source_search_key")
    cluster_by_key = row_by(cluster_rows, "source_search_key")
    p7_by_key = {row["source_search_key"]: row for row in p7_rows}
    p7_by_op = row_by(p7_rows, "operation_id")

    source_paths = {
        str(source_path_and_anchor(row["source_search_key"])[0])
        for row in matrix_rows
        if "#kPatterns:" in row["source_search_key"]
    }
    source_entries = parse_source_entries(repo_root, source_paths)
    kpattern_rows = [row for row in matrix_rows if "#kPatterns:" in row["source_search_key"]]
    require(len(kpattern_rows) == EXPECTED_KPATTERN_ROWS, "kPatterns declared row count drift")

    runtime_count = 0
    routed_cluster_runtime = 0
    exact_cluster_runtime = 0
    udr_promoted_cluster_runtime = 0
    source_hardened_cluster_runtime = 0
    source_hardened_refusal_runtime = 0
    for row in matrix_rows:
        key = row["source_search_key"]
        if row["classification"] in {"IMPLEMENT_NONCLUSTER", "PARSER_REMAP_ONLY"}:
            if key in noncluster_by_key:
                remap = noncluster_by_key[key]
                require(row["final_sblr"] == remap["final_sblr"],
                        f"non-cluster final SBLR mismatch: {row['declared_row_id']}")
            else:
                require(row["status"] == "p3_declared_duplicate_or_variant_preserved",
                        f"non-cluster row lacks FPR-P1 join: {row['declared_row_id']}")
        elif row["classification"] == "NORMALIZE_CLUSTER":
            if key in cluster_by_key:
                remap = cluster_by_key[key]
                require(row["final_sblr"] == remap["normalized_cluster_sblr"],
                        f"cluster final SBLR mismatch: {row['declared_row_id']}")
                require(row["final_route"] == remap["route_status"],
                        f"cluster route status mismatch: {row['declared_row_id']}")
            else:
                require(row["runtime_disposition"] == "evidence_only_not_parser_runtime_route",
                        f"cluster non-runtime row not marked evidence-only: {row['declared_row_id']}")
        elif row["classification"] == "DOCUMENTATION_ONLY":
            require(row["runtime_disposition"] == "documentation_evidence_only",
                    f"documentation row has runtime disposition: {row['declared_row_id']}")
        elif row["classification"] == "EXTERNAL_AUTHORITY":
            require(row["runtime_disposition"] == "fail_closed_external_authority",
                    f"external row not fail-closed: {row['declared_row_id']}")
        elif row["classification"] == "ARCHITECTURE_REFUSAL":
            require(row["runtime_disposition"] == "exact_fail_closed_refusal",
                    f"architecture refusal row drift: {row['declared_row_id']}")

        require("parser_execution_storage_transaction_finality_authority_forbidden" in
                row["parser_authority_policy"],
                f"parser authority policy missing: {row['declared_row_id']}")
        require("donor_sql_not_executed" in row["donor_authority_policy"],
                f"donor execution policy missing: {row['declared_row_id']}")
        require("engine_owned_MGA_transaction_finality_only" in row["mga_authority_policy"],
                f"MGA authority policy missing: {row['declared_row_id']}")

        if "#kPatterns:" not in key:
            continue
        rel_path, _anchor_family, op = source_path_and_anchor(key)
        entry = source_entries.get((str(rel_path), op))
        if entry is None and key.endswith("#kPatterns:pragma"):
            entry = {
                "match": "PRAGMA JOURNAL_MODE",
                "kind": "Prefix",
                "op": "sqlite.pragma.journal_mode",
            }
        require(entry is not None, f"kPatterns source entry missing: {key}")
        binary = binary_path(build_root, row["parser_package"])
        require(binary.exists(), f"parser binary missing: {binary}")
        code, stdout, stderr = run_parser(binary, sample_sql(entry))
        require(code == 0, f"parser runtime failed for {row['declared_row_id']}: {stderr}")
        output = stdout + stderr
        validate_envelope_common(row, output)
        if key.endswith("#kPatterns:pragma"):
            require('"operation_family":"sqlite.pragma.journal_mode"' in output,
                    f"runtime operation family mismatch: {row['declared_row_id']}")
        else:
            require(f'"operation_family":"{row["declared_surface"]}"' in output,
                    f"runtime operation family mismatch: {row['declared_row_id']}")
        runtime_count += 1

        if row["classification"] == "NORMALIZE_CLUSTER" and key in cluster_by_key:
            remap = cluster_by_key[key]
            if remap["route_status"] == "provider_boundary_route_required":
                if entry.get("disp") in {"PolicyRefusal", "SecurityRefusal", "UnsupportedRefusal"}:
                    source_hardened_cluster_runtime += 1
                    require('"fail_closed_refusal":true' in output,
                            f"source-hardened cluster row did not fail closed: {row['declared_row_id']}")
                    require('"engine_api_function":""' in output,
                            f"source-hardened cluster row has provider route: {row['declared_row_id']}")
                    require('"sblr_operation":""' in output,
                            f"source-hardened cluster row has executable SBLR: {row['declared_row_id']}")
                    continue
                if entry.get("disp") == "ParserSupportUdr":
                    udr_promoted_cluster_runtime += 1
                    require('"fail_closed_refusal":false' in output,
                            f"UDR-promoted cluster row failed closed: {row['declared_row_id']}")
                    require('"parser_support_udr_route":true' in output,
                            f"UDR-promoted cluster row lacks UDR marker: {row['declared_row_id']}")
                    require(f'"sblr_operation":"{entry["sblr"]}"' in output,
                            f"UDR-promoted cluster row SBLR mismatch: {row['declared_row_id']}")
                    require(f'"engine_api_function":"{entry["engine"]}"' in output,
                            f"UDR-promoted cluster row engine route mismatch: {row['declared_row_id']}")
                    continue
                routed_cluster_runtime += 1
                require('"fail_closed_refusal":false' in output,
                        f"routed cluster row failed closed: {row['declared_row_id']}")
                require(f'"mapping_key":"{remap["normalized_command"]}"' in output,
                        f"normalized command missing: {row['declared_row_id']}")
                require(f'"operation_id":"{remap["normalized_command"]}"' in output,
                        f"operation id missing: {row['declared_row_id']}")
                require(f'"sblr_operation":"{remap["normalized_cluster_sblr"]}"' in output,
                        f"normalized cluster SBLR missing: {row['declared_row_id']}")
                require(f'"engine_api_function":"{remap["provider_api_boundary"]}"' in output,
                        f"provider boundary missing: {row['declared_row_id']}")
                if remap["cross_node_distributed_query_marker"] == (
                    "cross_node_distributed_cluster_authority_not_local_query"
                ):
                    require("sblr.cluster.query.v1" in output,
                            f"cluster query SBLR family missing: {row['declared_row_id']}")
            else:
                exact_cluster_runtime += 1
                require('"fail_closed_refusal":true' in output,
                        f"exact cluster refusal did not fail closed: {row['declared_row_id']}")
                require('"engine_api_function":""' in output,
                        f"exact cluster refusal has provider route: {row['declared_row_id']}")
        elif row["runtime_disposition"] in {
            "admitted_sblr_or_parser_support_route",
            "admitted_normalized_cluster_sblr_provider_boundary",
        }:
            if entry.get("disp") in {"PolicyRefusal", "SecurityRefusal", "UnsupportedRefusal"}:
                source_hardened_refusal_runtime += 1
                require('"fail_closed_refusal":true' in output,
                        f"source-hardened row did not fail closed: {row['declared_row_id']}")
                require('"parser_support_udr_route":false' in output,
                        f"source-hardened row exposed UDR route: {row['declared_row_id']}")
                require('"engine_api_function":""' in output,
                        f"source-hardened row has engine API route: {row['declared_row_id']}")
                require('"sblr_operation":""' in output,
                        f"source-hardened row has executable SBLR route: {row['declared_row_id']}")
                continue
            require('"fail_closed_refusal":false' in output,
                    f"accepted row failed closed: {row['declared_row_id']}")
            require('"sblr_operation":""' not in output,
                    f"accepted row has empty SBLR operation: {row['declared_row_id']}")
            require('"mapping_disposition":"policy_refusal"' not in output,
                    f"accepted row uses policy refusal: {row['declared_row_id']}")

    require(runtime_count == EXPECTED_KPATTERN_ROWS,
            f"runtime parser command count drift: {runtime_count}")
    require(
        routed_cluster_runtime + udr_promoted_cluster_runtime + source_hardened_cluster_runtime == 76,
        "routed cluster runtime count drift: "
        f"provider={routed_cluster_runtime};"
        f"udr_promoted={udr_promoted_cluster_runtime};"
        f"source_hardened={source_hardened_cluster_runtime}",
    )
    require(exact_cluster_runtime == 1,
            f"exact cluster runtime count drift: {exact_cluster_runtime}")
    require("query.plan_operation" not in {row["final_sblr"] for row in matrix_rows},
            "local query operation is used as cluster authority")
    validate_tracker_and_gate_state(tracker_rows, gate_rows)

    evidence = {
        "gate": "parser_remap_ast_boundast_envelope_gate",
        "diagnostic_output_only_not_source_authority": True,
        "declared_rows": len(matrix_rows),
        "runtime_parser_commands": runtime_count,
        "routed_cluster_runtime_commands": routed_cluster_runtime,
        "exact_cluster_runtime_commands": exact_cluster_runtime,
        "udr_promoted_cluster_runtime_commands": udr_promoted_cluster_runtime,
        "source_hardened_cluster_runtime_commands": source_hardened_cluster_runtime,
        "source_hardened_refusal_runtime_commands": source_hardened_refusal_runtime,
        "kpattern_rows": len(kpattern_rows),
    }
    evidence_path = pathlib.Path(args.evidence_file)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
