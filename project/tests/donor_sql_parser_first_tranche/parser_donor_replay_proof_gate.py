#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FPR-P6 donor-native replay and full-path proof verifier."""

from __future__ import annotations

import argparse
import ast
import csv
import hashlib
import importlib.util
import json
import pathlib
import re
import sys


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

MATRIX_REL = (
    "public_execution_plan"
    "DONOR_REPLAY_PROOF_UPDATE_MATRIX.csv"
)
TRACKER_REL = "public_execution_plan"
GATES_REL = "public_execution_plan"
REPLAY_GATE_REL = "project/tests/donor_regression/first_tranche_original_tool_replay_gate.py"

EXPECTED_MATRIX_COUNTS = {
    "donor_replay_summary": len(DIALECTS),
    "native_tool_staging": 34,
    "native_replay_case": 121,
    "regular_ctest_dependency": 1,
}

REQUIRED_COLUMNS = {
    "proof_id",
    "proof_type",
    "donor",
    "native_tool",
    "case_id",
    "source_or_staged_locator",
    "route_coverage",
    "expected_operation_family",
    "expected_statement_kind",
    "expected_disposition",
    "expected_diagnostic_code",
    "proof_status",
    "ctest_label",
    "test_destination",
    "evidence_key",
    "authority_policy",
    "status",
    "notes",
}

AUTHORITY_POLICY = "parser_evidence_only_engine_mga_security_storage_authority"
P6_STATUS = "p6_donor_replay_proof_verified"
FORBIDDEN_LEAK_RE = re.compile(r"(?:/(?:home|Users)/[^\s,;]+|[A-Za-z]:[\\/][^\s,;]+|local workspace|https?://)")
FORBIDDEN_COMPLETION_RE = re.compile(
    r"todo|fixme|stub|skeleton|placeholder|defer|deferred|"
    r"not implemented|future work|file presence",
    re.I,
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


def validate_tracker_and_gate_state(
    tracker_rows: list[dict[str, str]],
    gate_rows: list[dict[str, str]],
) -> None:
    tracker = {row["slice_id"]: row for row in tracker_rows}
    for phase, status in {
        "FPR-P0": "p0_input_readiness_verified",
        "FPR-P1": "p1_noncluster_remap_verified",
        "FPR-P2": "p2_cluster_route_remap_verified",
        "FPR-P3": "p3_ast_boundast_envelope_verified",
        "FPR-P4": "p4_refusal_reduction_verified",
        "FPR-P5": "p5_dialect_isolation_verified",
        "FPR-P6": P6_STATUS,
    }.items():
        require(tracker[phase]["status"] == status, f"{phase} not verified")
    require(tracker["FPR-P7"]["status"] in {"pending", "p7_parser_remap_audit_verified"},
            "FPR-P7 must be pending or verified after the independent audit closes")

    gates = {row["gate_id"]: row for row in gate_rows}
    for gate, status in {
        "FPR-GATE-001": "p0_input_readiness_verified",
        "FPR-GATE-002": "p1_noncluster_remap_verified",
        "FPR-GATE-003": "p2_cluster_route_remap_verified",
        "FPR-GATE-004": "p3_ast_boundast_envelope_verified",
        "FPR-GATE-005": "p4_refusal_reduction_verified",
        "FPR-GATE-006": "p5_dialect_isolation_verified",
        "FPR-GATE-007": P6_STATUS,
    }.items():
        require(gates[gate]["status"] == status, f"{gate} not verified")
    require(gates["FPR-GATE-008"]["status"] in {"pending", "p7_parser_remap_audit_verified"},
            "FPR-GATE-008 must be pending or verified after the independent audit closes")


def import_replay_gate(repo_root: pathlib.Path):
    path = repo_root / REPLAY_GATE_REL
    spec = importlib.util.spec_from_file_location("sb_replay_gate_source", path)
    require(spec is not None and spec.loader is not None, "could not import replay gate source")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def source_tool_specs(repo_root: pathlib.Path) -> list[dict[str, str]]:
    source = (repo_root / REPLAY_GATE_REL).read_text(encoding="utf-8")
    module = ast.parse(source)
    tools: list[dict[str, str]] = []
    for fn in module.body:
        if not isinstance(fn, ast.FunctionDef) or fn.name != "build_tool_specs":
            continue
        for node in ast.walk(fn):
            if not (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Name)
                and node.func.id == "ToolSpec"
            ):
                continue
            require(len(node.args) >= 4, "ToolSpec source call missing positional args")
            dialect = ast.literal_eval(node.args[0])
            tool_id = ast.literal_eval(node.args[1])
            staged_rel = ast.literal_eval(node.args[3])
            tools.append({
                "dialect": dialect,
                "tool_id": tool_id,
                "staged_locator": staged_rel,
            })
    require(len(tools) == EXPECTED_MATRIX_COUNTS["native_tool_staging"],
            "ToolSpec source count drift")
    return tools


def source_replay_cases(repo_root: pathlib.Path) -> list[dict[str, object]]:
    module = import_replay_gate(repo_root)
    cases = []
    for case in module.replay_cases():
        cases.append({
            "dialect": case.dialect,
            "case_id": case.case_id,
            "source": case.source_rel,
            "source_fragment": case.source_fragment,
            "sql": case.sql,
            "expected_operation_family": case.expected_operation_family,
            "expected_statement_kind": case.expected_statement_kind,
            "expected_disposition": case.expected_disposition or "not_applicable",
            "expected_diagnostic_code": case.expected_diagnostic_code or "not_applicable",
            "validation_tags": list(case.validation_tags),
            "expected_datatype_families": list(case.expected_datatype_families),
        })
    require(len(cases) == EXPECTED_MATRIX_COUNTS["native_replay_case"],
            "ReplayCase source count drift")
    return cases


def string_list(value: object, label: str) -> list[str]:
    require(isinstance(value, list), f"{label} must be a list")
    for item in value:
        require(isinstance(item, str), f"{label} must contain only strings")
    return value


def datatype_families_digest(families: list[str]) -> str:
    payload = json.dumps(families, separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def require_no_tagged_source_text_leak(
    source_case: dict[str, object],
    evidence_case: dict[str, object],
) -> None:
    case_id = str(source_case["case_id"])
    serialized = json.dumps(evidence_case, sort_keys=True)
    sql = source_case.get("sql")
    source_fragment = source_case.get("source_fragment")
    if isinstance(sql, str) and sql and sql in serialized:
        fail(f"{case_id} leaked donor SQL text in replay evidence")
    if (
        isinstance(source_fragment, str)
        and source_fragment
        and source_fragment in serialized
    ):
        fail(f"{case_id} leaked donor source fragment in replay evidence")


def validate_validation_metadata(
    source_case: dict[str, object],
    evidence_case: dict[str, object],
) -> None:
    case_id = str(source_case["case_id"])
    expected_tags = string_list(source_case.get("validation_tags"), f"{case_id} source tags")
    expected_families = string_list(
        source_case.get("expected_datatype_families"),
        f"{case_id} source datatype families",
    )
    evidence_tags = string_list(
        evidence_case.get("validation_tags"),
        f"{case_id} evidence validation_tags",
    )
    evidence_families = string_list(
        evidence_case.get("expected_datatype_families"),
        f"{case_id} evidence expected_datatype_families",
    )
    require(evidence_tags == expected_tags,
            f"{case_id} validation_tags evidence mismatch")
    require(evidence_families == expected_families,
            f"{case_id} expected_datatype_families evidence mismatch")

    if expected_tags:
        require_no_tagged_source_text_leak(source_case, evidence_case)

    if "datatype" in expected_tags:
        require(expected_families, f"{case_id} datatype case has no expected families")
        require(
            evidence_case.get("asserted_datatype_family_count") == len(expected_families),
            f"{case_id} datatype asserted family count mismatch",
        )
        require(
            evidence_case.get("asserted_datatype_families_digest") ==
            datatype_families_digest(expected_families),
            f"{case_id} datatype asserted families digest mismatch",
        )
    if "procedural" in expected_tags:
        require(
            evidence_case.get("procedural_source_retention_asserted") is True,
            f"{case_id} missing procedural source retention assertion flag",
        )
        require(
            evidence_case.get("procedural_functional_encoding_asserted") is True,
            f"{case_id} missing procedural functional encoding assertion flag",
        )


def validate_evidence(evidence_path: pathlib.Path) -> dict[str, object]:
    require(evidence_path.exists(), f"missing donor replay evidence: {evidence_path}")
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    require(evidence.get("gate") == "donor_sql_first_tranche_original_tool_replay_gate",
            "wrong replay evidence gate")
    require(evidence.get("regular_ctest_gate") is True,
            "replay evidence is not from regular CTest gate")
    require(evidence.get("donor_tools_are_storage_authority") is False,
            "donor tools gained storage authority")
    require(evidence.get("donor_tools_are_transaction_authority") is False,
            "donor tools gained transaction authority")
    require("ScratchBird authority" in evidence.get("parser_authority_rule", ""),
            "parser authority rule missing ScratchBird authority statement")
    staged_tools = evidence.get("staged_tools")
    tool_smokes = evidence.get("tool_smokes")
    replay_results = evidence.get("replay_results")
    require(isinstance(staged_tools, list), "staged_tools evidence missing")
    require(isinstance(tool_smokes, list), "tool_smokes evidence missing")
    require(isinstance(replay_results, list), "replay_results evidence missing")
    require(len(staged_tools) == EXPECTED_MATRIX_COUNTS["native_tool_staging"],
            "staged tool count drift")
    require(len(tool_smokes) == EXPECTED_MATRIX_COUNTS["native_tool_staging"],
            "tool smoke count drift")
    require(evidence.get("replay_case_count") == EXPECTED_MATRIX_COUNTS["native_replay_case"],
            "replay case count drift")
    for tool in staged_tools:
        require(tool.get("staged") is True, f"tool not staged: {tool}")
        locator = str(tool.get("staged_locator", ""))
        require(locator.startswith("project/tests/donor_regression/"),
                f"tool staged outside project/tests: {locator}")
        require(not FORBIDDEN_LEAK_RE.search(";".join(str(v) for v in tool.values())),
                f"private path or URL leaked in tool evidence: {tool.get('tool_id')}")
    for result in replay_results:
        for key in [
            "case_id",
            "dialect",
            "operation_family",
            "statement_kind",
            "mapping_disposition",
            "source",
            "provenance",
            "validation_tags",
            "expected_datatype_families",
        ]:
            require(key in result, f"replay result missing {key}: {result}")
        string_list(result.get("validation_tags"),
                    f"{result.get('case_id')} validation_tags")
        string_list(result.get("expected_datatype_families"),
                    f"{result.get('case_id')} expected_datatype_families")
        require(not FORBIDDEN_LEAK_RE.search(";".join(str(v) for v in result.values())),
                f"private path or URL leaked in replay evidence: {result.get('case_id')}")
    return evidence


def validate_matrix(
    repo_root: pathlib.Path,
    rows: list[dict[str, str]],
    evidence: dict[str, object],
) -> dict[str, int]:
    require_columns(rows, REQUIRED_COLUMNS, MATRIX_REL)
    require(len(rows) == sum(EXPECTED_MATRIX_COUNTS.values()), "P6 matrix row count drift")
    grouped: dict[str, list[dict[str, str]]] = {}
    seen: set[str] = set()
    for row in rows:
        require(row["proof_id"] not in seen, f"duplicate proof_id: {row['proof_id']}")
        seen.add(row["proof_id"])
        grouped.setdefault(row["proof_type"], []).append(row)
        require(row["donor"] in DIALECTS or row["donor"] == "all_donors",
                f"unknown donor in P6 matrix: {row['donor']}")
        require(row["proof_status"] == P6_STATUS, f"wrong proof status: {row['proof_id']}")
        require(row["status"] == P6_STATUS, f"wrong row status: {row['proof_id']}")
        require(row["ctest_label"] == "donor_sql_first_tranche_original_tool_replay_gate",
                f"wrong CTest label: {row['proof_id']}")
        require(row["test_destination"] ==
                "project/tests/donor_sql_parser_first_tranche/parser_donor_replay_proof_gate.py",
                f"wrong test destination: {row['proof_id']}")
        require(row["authority_policy"] == AUTHORITY_POLICY,
                f"wrong authority policy: {row['proof_id']}")
        combined = ";".join(row.values())
        require(not FORBIDDEN_LEAK_RE.search(combined),
                f"private path or URL leaked: {row['proof_id']}")
        require(not FORBIDDEN_COMPLETION_RE.search(row["notes"]),
                f"unclosed wording in notes: {row['proof_id']}")
    for proof_type, expected in EXPECTED_MATRIX_COUNTS.items():
        require(len(grouped.get(proof_type, [])) == expected,
                f"{proof_type} count drift")

    source_tools = row_by(source_tool_specs(repo_root), "tool_id")
    evidence_tools = row_by(evidence["staged_tools"], "tool_id")
    matrix_tools = row_by(grouped["native_tool_staging"], "native_tool")
    require(set(matrix_tools) == set(source_tools) == set(evidence_tools),
            "native tool ids differ between matrix source and evidence")
    for tool_id, row in matrix_tools.items():
        source_tool = source_tools[tool_id]
        evidence_tool = evidence_tools[tool_id]
        require(row["donor"] == source_tool["dialect"] == evidence_tool["dialect"],
                f"native tool donor mismatch: {tool_id}")
        require(row["source_or_staged_locator"] == source_tool["staged_locator"] ==
                evidence_tool["staged_locator"],
                f"native tool staged locator mismatch: {tool_id}")
        require(row["route_coverage"] == "native_tool_staged_and_smoked",
                f"native tool route coverage mismatch: {tool_id}")

    source_cases = row_by(source_replay_cases(repo_root), "case_id")
    evidence_cases = row_by(evidence["replay_results"], "case_id")
    matrix_cases = row_by(grouped["native_replay_case"], "case_id")
    require(set(matrix_cases) == set(source_cases) == set(evidence_cases),
            "replay case ids differ between matrix source and evidence")
    for case_id, row in matrix_cases.items():
        source_case = source_cases[case_id]
        evidence_case = evidence_cases[case_id]
        require(row["donor"] == source_case["dialect"] == evidence_case["dialect"],
                f"replay case donor mismatch: {case_id}")
        require(row["source_or_staged_locator"] == source_case["source"] ==
                evidence_case["source"],
                f"replay case source mismatch: {case_id}")
        require(row["expected_operation_family"] ==
                source_case["expected_operation_family"] ==
                evidence_case["operation_family"],
                f"replay case operation family mismatch: {case_id}")
        require(row["expected_statement_kind"] ==
                source_case["expected_statement_kind"] ==
                evidence_case["statement_kind"],
                f"replay case statement kind mismatch: {case_id}")
        require(row["expected_disposition"] == source_case["expected_disposition"],
                f"replay case expected disposition mismatch: {case_id}")
        if source_case["expected_disposition"] != "not_applicable":
            require(evidence_case["mapping_disposition"] == source_case["expected_disposition"],
                    f"replay case disposition evidence mismatch: {case_id}")
        require(row["expected_diagnostic_code"] == source_case["expected_diagnostic_code"],
                f"replay case diagnostic mismatch: {case_id}")
        validate_validation_metadata(source_case, evidence_case)

    expected_counts = evidence["replay_counts_by_dialect"]
    summary_rows = row_by(grouped["donor_replay_summary"], "donor")
    require(set(summary_rows) == set(DIALECTS), "donor replay summary coverage drift")
    tools_by_donor: dict[str, int] = {dialect: 0 for dialect in DIALECTS}
    for tool in source_tools.values():
        tools_by_donor[tool["dialect"]] += 1
    for donor, row in summary_rows.items():
        expected_route = (
            f"replay_cases={expected_counts[donor]};native_tools={tools_by_donor[donor]}"
        )
        require(row["route_coverage"] == expected_route,
                f"donor summary route coverage mismatch: {donor}")

    dependency_rows = grouped["regular_ctest_dependency"]
    require(dependency_rows[0]["donor"] == "all_donors",
            "regular CTest dependency row must cover all donors")
    return {proof_type: len(values) for proof_type, values in grouped.items()}


def validate_ctest_registration(build_root: pathlib.Path) -> None:
    ctest_file = build_root / "tests/donor_sql_parser_first_tranche/CTestTestfile.cmake"
    require(ctest_file.exists(), "donor parser CTest file is not configured")
    text = ctest_file.read_text(encoding="utf-8")
    for test in [
        "donor_sql_first_tranche_original_tool_replay_gate",
        "parser_donor_replay_proof_gate",
    ]:
        require(test in text, f"CTest missing {test}")


def write_evidence(
    path: pathlib.Path,
    matrix_counts: dict[str, int],
    replay_evidence: dict[str, object],
) -> None:
    replay_results = replay_evidence["replay_results"]
    tagged_results = [
        result for result in replay_results
        if result.get("validation_tags")
    ]
    procedural_results = [
        result for result in replay_results
        if "procedural" in result.get("validation_tags", [])
    ]
    datatype_results = [
        result for result in replay_results
        if "datatype" in result.get("validation_tags", [])
    ]
    datatype_family_count = sum(
        int(result.get("asserted_datatype_family_count", 0))
        for result in datatype_results
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "gate": "parser_donor_replay_proof_gate",
        "authority_note": "diagnostic_output_only_not_source_authority",
        "matrix_counts": matrix_counts,
        "source_replay_gate": REPLAY_GATE_REL,
        "validated_replay_gate": replay_evidence["gate"],
        "validated_replay_case_count": replay_evidence["replay_case_count"],
        "validated_staged_tool_count": len(replay_evidence["staged_tools"]),
        "validated_tool_smoke_count": len(replay_evidence["tool_smokes"]),
        "validated_replay_counts_by_dialect": replay_evidence["replay_counts_by_dialect"],
        "validated_tagged_replay_case_count": len(tagged_results),
        "validated_procedural_replay_case_count": len(procedural_results),
        "validated_datatype_replay_case_count": len(datatype_results),
        "validated_datatype_family_assertion_count": datatype_family_count,
        "authority_policy": AUTHORITY_POLICY,
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--first-tranche-replay-evidence", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()
    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()
    replay_evidence = validate_evidence(
        pathlib.Path(args.first_tranche_replay_evidence).resolve()
    )
    matrix_rows = read_csv(repo_root, MATRIX_REL)
    tracker_rows = read_csv(repo_root, TRACKER_REL)
    gate_rows = read_csv(repo_root, GATES_REL)
    validate_tracker_and_gate_state(tracker_rows, gate_rows)
    matrix_counts = validate_matrix(repo_root, matrix_rows, replay_evidence)
    validate_ctest_registration(build_root)
    write_evidence(pathlib.Path(args.evidence_file), matrix_counts, replay_evidence)
    print("parser_donor_replay_proof_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
