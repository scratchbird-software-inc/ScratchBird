#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FPR-P4 refusal and diagnostics reduction proof gate."""

from __future__ import annotations

import argparse
import collections
import csv
import json
import pathlib
import re
import subprocess
import sys


EXPECTED_REFUSAL_ROWS = 288
EXPECTED_CLASSIFICATION_COUNTS = {
    "ARCHITECTURE_REFUSAL": 177,
    "EXTERNAL_AUTHORITY": 110,
    "NORMALIZE_CLUSTER": 1,
}
EXPECTED_DISPOSITION_COUNTS = {
    "exact_fail_closed_refusal": 177,
    "fail_closed_external_authority": 110,
    "exact_pre_provider_refusal": 1,
}
EXPECTED_PROOF_KIND_COUNTS = {
    "runtime_parser_refusal": 129,
    "source_anchor_message_vector": 159,
}
EXTERNAL_REFERENCE_SKIP_CODE = 77
TARGET_DISPOSITIONS = set(EXPECTED_DISPOSITION_COUNTS)

P3_REL = (
    "public_execution_plan/COMPATIBILITY_PARSER_REMAP_MATRIX.csv"
)
P4_REL = (
    "public_execution_plan/PARSER_REFUSAL_REDUCTION_MATRIX.csv"
)
TRACKER_REL = "public_execution_plan"
GATES_REL = "public_execution_plan"

REQUIRED_COLUMNS = {
    "refusal_id",
    "declared_row_id",
    "declared_row_ordinal",
    "compatibility",
    "parser_package",
    "refusal_surface",
    "classification",
    "source_search_key",
    "anchor_family",
    "old_reason",
    "implementation_route",
    "final_reason",
    "message_vector",
    "diagnostic_code",
    "runtime_disposition",
    "final_sblr",
    "provider_policy",
    "parser_authority_policy",
    "compatibility_authority_policy",
    "mga_authority_policy",
    "sample_input_or_source_anchor",
    "test_destination",
    "proof_kind",
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

FORBIDDEN_COMPLETION_RE = re.compile(
    r"todo|fixme|stub|skeleton|placeholder|defer|deferred|"
    r"not implemented|future work|file presence",
    re.I,
)
FORBIDDEN_LEAK_RE = re.compile(r"(?:/(?:home|Users)/[^\s,;]+|[A-Za-z]:[\\/][^\s,;]+|local workspace|https?://)")


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
            require(row[column], f"{rel_path} empty {column} in {row}")


def row_by(rows: list[dict[str, str]], key: str) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row[key]
        require(value not in result, f"duplicate {key}: {value}")
        result[value] = row
    return result


def source_path_and_anchor(source_search_key: str) -> tuple[str, str, str]:
    require("#" in source_search_key, f"source key lacks anchor: {source_search_key}")
    rel_path, anchor = source_search_key.split("#", 1)
    family = anchor.split(":", 1)[0]
    value = anchor.split(":", 1)[1] if ":" in anchor else family
    return rel_path, family, value


def route_value(route: str, key: str) -> str:
    marker = f"{key}="
    position = route.find(marker)
    if position == -1:
        return ""
    value = route[position + len(marker):]
    if key in {"surface", "diagnostics"}:
        return value
    return value.split(";", 1)[0]


def source_route(entry: dict[str, str]) -> str:
    return (
        f"k{entry['disp']};match={entry['match']};mapping={entry['mapping']};"
        f"sblr={entry['sblr'] or 'none'};engine_api={entry['engine'] or 'none'};"
        f"diagnostic={entry['diag']}"
    )


def parse_source_entries(
    repo_root: pathlib.Path,
    rows: list[dict[str, str]],
) -> dict[tuple[str, str], list[dict[str, str]]]:
    source_paths = {
        source_path_and_anchor(row["source_search_key"])[0]
        for row in rows
        if row["proof_kind"] == "runtime_parser_refusal"
    }
    entries: dict[tuple[str, str], list[dict[str, str]]] = {}
    for rel_path in source_paths:
        path = repo_root / rel_path
        require(path.exists(), f"source path missing: {rel_path}")
        text = path.read_text(errors="ignore")
        for match in SOURCE_ENTRY_RE.finditer(text):
            groups = match.groupdict()
            entries.setdefault((rel_path, groups["op"]), []).append(groups)
    return entries


def find_source_entry(
    row: dict[str, str],
    entries: dict[tuple[str, str], list[dict[str, str]]],
) -> dict[str, str]:
    rel_path, _family, operation = source_path_and_anchor(row["source_search_key"])
    candidates = entries.get((rel_path, operation), [])
    require(candidates, f"runtime source entry missing: {row['source_search_key']}")
    expected_match = route_value(row["implementation_route"], "match")
    for entry in candidates:
        if source_route(entry) == row["implementation_route"]:
            return entry
    for entry in candidates:
        if expected_match and entry["match"] == expected_match:
            return entry
    fail(f"runtime source entry does not match P4 implementation route: {row['refusal_id']}")


def binary_path(build_root: pathlib.Path, parser_package: str) -> pathlib.Path:
    compatibility = pathlib.PurePosixPath(parser_package).name
    return build_root / "src/parsers/compatibility" / compatibility / f"sbp_{compatibility}"


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


def validate_tracker_and_gate_state(
    tracker_rows: list[dict[str, str]],
    gate_rows: list[dict[str, str]],
) -> None:
    tracker = {row["slice_id"]: row for row in tracker_rows}
    require(tracker["FPR-P0"]["status"] == "p0_input_readiness_verified",
            "FPR-P0 not verified")
    require(tracker["FPR-P1"]["status"] == "p1_noncluster_remap_verified",
            "FPR-P1 not verified")
    require(tracker["FPR-P2"]["status"] == "p2_cluster_route_remap_verified",
            "FPR-P2 not verified")
    require(tracker["FPR-P3"]["status"] == "p3_ast_boundast_envelope_verified",
            "FPR-P3 not verified")
    require(tracker["FPR-P4"]["status"] == "p4_refusal_reduction_verified",
            "FPR-P4 not verified")
    require(tracker["FPR-P5"]["status"] in {"pending", "p5_dialect_isolation_verified"},
            "FPR-P5 must be pending or verified after the dialect isolation gate closes")
    require(tracker["FPR-P6"]["status"] in {"pending", "p6_compatibility_replay_proof_verified"},
            "FPR-P6 must be pending or verified after the compatibility replay proof gate closes")
    row = tracker["FPR-P7"]
    require(row["status"] in {"pending", "p7_parser_remap_audit_verified"},
            f"{row['slice_id']} must be pending or verified after the independent audit closes")

    gates = {row["gate_id"]: row for row in gate_rows}
    for gate, status in {
        "FPR-GATE-001": "p0_input_readiness_verified",
        "FPR-GATE-002": "p1_noncluster_remap_verified",
        "FPR-GATE-003": "p2_cluster_route_remap_verified",
        "FPR-GATE-004": "p3_ast_boundast_envelope_verified",
        "FPR-GATE-005": "p4_refusal_reduction_verified",
    }.items():
        require(gates[gate]["status"] == status, f"{gate} not verified")
    require(gates["FPR-GATE-006"]["status"] in {"pending", "p5_dialect_isolation_verified"},
            "FPR-GATE-006 must be pending or verified after the dialect isolation gate closes")
    require(gates["FPR-GATE-007"]["status"] in {"pending", "p6_compatibility_replay_proof_verified"},
            "FPR-GATE-007 must be pending or verified after the compatibility replay proof gate closes")
    row = gates["FPR-GATE-008"]
    require(row["status"] in {"pending", "p7_parser_remap_audit_verified"},
            f"{row['gate_id']} must be pending or verified after the independent audit closes")


def validate_row_against_p3(row: dict[str, str], p3: dict[str, str]) -> None:
    require(row["declared_row_ordinal"] == p3["declared_row_ordinal"],
            f"ordinal mismatch: {row['refusal_id']}")
    require(row["parser_package"] == p3["parser_package"],
            f"parser package mismatch: {row['refusal_id']}")
    require(row["refusal_surface"] == p3["declared_surface"],
            f"surface mismatch: {row['refusal_id']}")
    require(row["classification"] == p3["classification"],
            f"classification mismatch: {row['refusal_id']}")
    require(row["source_search_key"] == p3["source_search_key"],
            f"source key mismatch: {row['refusal_id']}")
    require(row["old_reason"] == p3["current_route"],
            f"old reason does not preserve P3 route: {row['refusal_id']}")
    require(row["runtime_disposition"] == p3["runtime_disposition"],
            f"runtime disposition mismatch: {row['refusal_id']}")
    require(row["final_sblr"] == p3["final_sblr"],
            f"final SBLR mismatch: {row['refusal_id']}")
    for column in [
        "parser_authority_policy",
        "compatibility_authority_policy",
        "mga_authority_policy",
    ]:
        require(row[column] == p3[column], f"{column} mismatch: {row['refusal_id']}")


def validate_policy_fields(row: dict[str, str]) -> None:
    combined = ";".join(row.values())
    require(not FORBIDDEN_LEAK_RE.search(combined), f"private path or URL leaked: {row['refusal_id']}")
    for column in ["final_reason", "notes"]:
        require(not FORBIDDEN_COMPLETION_RE.search(row[column]),
                f"unclosed wording in {column}: {row['refusal_id']}")
    require(row["status"] == "p4_refusal_reduction_verified",
            f"wrong row status: {row['refusal_id']}")
    require(row["test_destination"] ==
            "project/tests/compatibility_sql_parser_first_tranche/parser_refusal_reduction_gate.py",
            f"wrong test destination: {row['refusal_id']}")
    require("parser_execution_storage_transaction_finality_authority_forbidden" in
            row["parser_authority_policy"],
            f"parser authority policy missing: {row['refusal_id']}")
    require("compatibility_sql_not_executed" in row["compatibility_authority_policy"],
            f"compatibility execution policy missing: {row['refusal_id']}")
    require("engine_owned_MGA_transaction_finality_only" in row["mga_authority_policy"],
            f"MGA authority policy missing: {row['refusal_id']}")
    require(row["proof_kind"] in EXPECTED_PROOF_KIND_COUNTS,
            f"unknown proof kind: {row['refusal_id']}")

    if row["runtime_disposition"] == "exact_pre_provider_refusal":
        require(row["classification"] == "NORMALIZE_CLUSTER",
                f"pre-provider refusal is not cluster-normalized: {row['refusal_id']}")
        require(row["provider_policy"] == "no_cluster_provider_call_before_refusal",
                f"cluster provider pre-refusal policy missing: {row['refusal_id']}")
        require(row["diagnostic_code"] == "APACHE_IGNITE.AUTHORITY.CLUSTER_CONTROL_RESERVED",
                f"wrong pre-provider diagnostic: {row['refusal_id']}")
    elif row["runtime_disposition"] == "fail_closed_external_authority":
        require(row["classification"] == "EXTERNAL_AUTHORITY",
                f"external authority disposition mismatch: {row['refusal_id']}")
        require(row["provider_policy"] ==
                "no_local_or_provider_execution_without_trusted_engine_admission",
                f"external provider policy missing: {row['refusal_id']}")
        require(row["final_sblr"].startswith("sblr.external."),
                f"external authority row lacks external SBLR proof id: {row['refusal_id']}")
    else:
        require(row["classification"] == "ARCHITECTURE_REFUSAL",
                f"architecture refusal disposition mismatch: {row['refusal_id']}")
        require(row["provider_policy"] == "no_provider_or_compatibility_execution_for_exact_refusal",
                f"architecture provider policy missing: {row['refusal_id']}")


def validate_runtime_row(
    row: dict[str, str],
    build_root: pathlib.Path,
    source_entries: dict[tuple[str, str], list[dict[str, str]]],
) -> None:
    entry = find_source_entry(row, source_entries)
    require(source_route(entry) == row["implementation_route"],
            f"implementation route/source drift: {row['refusal_id']}")

    binary = binary_path(build_root, row["parser_package"])
    require(binary.exists(), f"parser binary missing: {binary}")
    code, stdout, stderr = run_parser(binary, row["sample_input_or_source_anchor"])
    require(code == 0, f"parser runtime failed for {row['refusal_id']}: {stderr}")
    output = stdout + stderr

    if entry["disp"] == "ParserSupportUdr":
        require(entry["engine"] != "", f"UDR promotion lacks engine API: {row['refusal_id']}")
        require(entry["sblr"] != "", f"UDR promotion lacks SBLR route: {row['refusal_id']}")
        for token in [
            "SBLRExecutionEnvelope.v3",
            '"cst_materialized":true',
            '"ast_materialized":true',
            '"bound_ast_materialized":true',
            '"descriptor_uuid_required":true',
            '"source_text_redacted":true',
            '"sql_text_included":false',
            '"reference_engine_sql_executed":false',
            '"real_reference_file_effects":false',
            '"parser_transaction_finality_authority":false',
            '"parser_storage_authority":false',
            '"fail_closed_refusal":false',
            '"parser_support_udr_route":true',
            f'"engine_api_function":"{entry["engine"]}"',
            f'"sblr_operation":"{entry["sblr"]}"',
        ]:
            require(token in output, f"UDR promotion evidence token missing {token}: {row['refusal_id']}")
        require(row["diagnostic_code"] == entry["diag"] and row["diagnostic_code"],
                f"UDR promotion diagnostic metadata drift: {row['refusal_id']}")
        require(f'"operation_family":"{row["refusal_surface"]}"' in output,
                f"UDR promotion operation family mismatch: {row['refusal_id']}")
        return

    require(entry["engine"] == "", f"runtime refusal has engine API source route: {row['refusal_id']}")
    require(entry["sblr"] == "", f"runtime refusal has executable SBLR source route: {row['refusal_id']}")
    require(entry["disp"] in {"PolicyRefusal", "SecurityRefusal", "UnsupportedRefusal"},
            f"runtime refusal source disposition is not fail-closed: {row['refusal_id']}")
    for token in [
        "SBLRExecutionEnvelope.v3",
        '"cst_materialized":true',
        '"ast_materialized":true',
        '"bound_ast_materialized":true',
        '"descriptor_uuid_required":true',
        '"source_text_redacted":true',
        '"sql_text_included":false',
        '"reference_engine_sql_executed":false',
        '"real_reference_file_effects":false',
        '"parser_transaction_finality_authority":false',
        '"parser_storage_authority":false',
        '"fail_closed_refusal":true',
        '"parser_support_udr_route":false',
        '"engine_api_function":""',
        '"sblr_operation":""',
    ]:
        require(token in output, f"runtime evidence token missing {token}: {row['refusal_id']}")
    require(row["diagnostic_code"] in output,
            f"diagnostic code missing from runtime output: {row['refusal_id']}")
    require(f'"operation_family":"{row["refusal_surface"]}"' in output,
            f"operation family mismatch: {row['refusal_id']}")
    require("ParserSupportConnectorRoute" not in output,
            f"runtime refusal leaked parser-support connector route: {row['refusal_id']}")
    if row["runtime_disposition"] == "exact_pre_provider_refusal":
        require("cluster.query.plan_distributed" not in output,
                f"pre-provider refusal leaked cluster query route: {row['refusal_id']}")
        require("provider_boundary" not in output,
                f"pre-provider refusal leaked provider boundary: {row['refusal_id']}")


def split_message_vector(message_vector: str) -> list[str]:
    return [token for token in message_vector.split(";") if token]


def validate_source_anchor_row(repo_root: pathlib.Path, row: dict[str, str]) -> None:
    rel_path, family, value = source_path_and_anchor(row["source_search_key"])
    require(row["anchor_family"] == family, f"anchor family mismatch: {row['refusal_id']}")
    path = repo_root / rel_path
    require(path.exists(), f"source path missing: {row['source_search_key']}")
    text = path.read_text(errors="ignore")
    if family not in {"marker", "diagnostic_vectors"}:
        require(family in text, f"anchor family absent from source: {row['refusal_id']}")

    if family == "diagnostic_vectors":
        for diagnostic in split_message_vector(row["message_vector"]):
            require(diagnostic in text, f"UDR diagnostic missing: {row['refusal_id']} {diagnostic}")
        return

    if family == "marker":
        require(value in text or "UNSUPPORTED_SURFACE" in text,
                f"unsupported marker source evidence missing: {row['refusal_id']}")
        require(row["diagnostic_code"] in text,
                f"unsupported marker diagnostic missing: {row['refusal_id']}")
        return

    if family in {"ClassifyNonFileOperation", "ClassifyIsqlOperation"}:
        require(family in text, f"classifier function missing: {row['refusal_id']}")
        require(value in text, f"classifier operation missing: {row['refusal_id']}")
        return

    if family == "FirebirdMappingStorage":
        require("FirebirdMappingStorage" in text, f"Firebird mapping storage missing: {row['refusal_id']}")
        require(value in text, f"Firebird mapping operation missing: {row['refusal_id']}")
        require("FIREBIRD.EMULATION.NON_FILE_SURFACE" in text,
                f"Firebird non-file diagnostic missing: {row['refusal_id']}")
        return

    if family == "ParseStatement":
        require("ParseStatement" in text, f"ParseStatement source missing: {row['refusal_id']}")
        require(row["diagnostic_code"] in text,
                f"ParseStatement diagnostic missing: {row['refusal_id']}")
        return

    require(value in text, f"source anchor value missing: {row['refusal_id']}")
    message_tokens = split_message_vector(row["message_vector"])
    require(message_tokens, f"message vector has no tokens: {row['refusal_id']}")
    require(any(token in text for token in message_tokens),
            f"message vector token absent from source: {row['refusal_id']}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()

    p3_rows = read_csv(repo_root, P3_REL)
    p4_rows = read_csv(repo_root, P4_REL)
    tracker_rows = read_csv(repo_root, TRACKER_REL)
    gate_rows = read_csv(repo_root, GATES_REL)

    require_columns(p4_rows, REQUIRED_COLUMNS, P4_REL)
    require(len(p4_rows) == EXPECTED_REFUSAL_ROWS, "P4 refusal row count drift")
    p3_targets = [
        row for row in p3_rows
        if row["runtime_disposition"] in TARGET_DISPOSITIONS
    ]
    require(len(p3_targets) == EXPECTED_REFUSAL_ROWS, "P3 refusal subset count drift")
    require(
        [row["declared_row_id"] for row in p4_rows] ==
        [row["declared_row_id"] for row in p3_targets],
        "P4 matrix does not preserve P3 refusal subset order",
    )

    p3_by_id = row_by(p3_targets, "declared_row_id")
    source_entries = parse_source_entries(repo_root, p4_rows)
    classification_counts: collections.Counter[str] = collections.Counter()
    disposition_counts: collections.Counter[str] = collections.Counter()
    proof_kind_counts: collections.Counter[str] = collections.Counter()
    implementation_route_repairs = 0
    runtime_rows = 0
    source_anchor_rows = 0

    for row in p4_rows:
        validate_row_against_p3(row, p3_by_id[row["declared_row_id"]])
        validate_policy_fields(row)
        classification_counts[row["classification"]] += 1
        disposition_counts[row["runtime_disposition"]] += 1
        proof_kind_counts[row["proof_kind"]] += 1
        if row["old_reason"] != row["implementation_route"]:
            implementation_route_repairs += 1

        if row["proof_kind"] == "runtime_parser_refusal":
            runtime_rows += 1
            validate_runtime_row(row, build_root, source_entries)
        else:
            source_anchor_rows += 1
            validate_source_anchor_row(repo_root, row)

    require(dict(classification_counts) == EXPECTED_CLASSIFICATION_COUNTS,
            f"classification counts drift: {classification_counts}")
    require(dict(disposition_counts) == EXPECTED_DISPOSITION_COUNTS,
            f"runtime disposition counts drift: {disposition_counts}")
    require(dict(proof_kind_counts) == EXPECTED_PROOF_KIND_COUNTS,
            f"proof kind counts drift: {proof_kind_counts}")
    require(implementation_route_repairs >= 15,
            "P4 did not record connector/external-authority implementation route repairs")
    validate_tracker_and_gate_state(tracker_rows, gate_rows)

    evidence = {
        "gate": "parser_refusal_reduction_gate",
        "diagnostic_output_only_not_source_authority": True,
        "refusal_rows": len(p4_rows),
        "classification_counts": dict(classification_counts),
        "runtime_disposition_counts": dict(disposition_counts),
        "proof_kind_counts": dict(proof_kind_counts),
        "implementation_route_repairs": implementation_route_repairs,
        "runtime_parser_refusal_rows": runtime_rows,
        "source_anchor_message_vector_rows": source_anchor_rows,
    }
    evidence_path = pathlib.Path(args.evidence_file)
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n")
    print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
