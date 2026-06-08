#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Truth gate for donor procedural-body and datatype enterprise readiness.

This gate prevents route-only, descriptor-only, generated-only, and generic-text
coverage from being counted as enterprise completion.  It intentionally passes
while blockers remain, because its normal CTest role is to regenerate the
current blocker evidence.  Use --strict-release for the final release run.
"""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import sys
from collections import Counter
from typing import Iterable


EXECUTION_PLAN_REL = pathlib.Path(
    "public_execution_plan"
)

EXPECTED_DONORS = (
    "firebird",
    "postgresql",
    "mysql",
    "mariadb",
    "sqlite",
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

REQUIRED_FILES = (
    "README.md",
    "TRACKER.csv",
    "ACCEPTANCE_GATES.csv",
    "PROCEDURAL_LANGUAGE_SCOPE_MATRIX.csv",
    "DATATYPE_EXACTNESS_SCOPE_MATRIX.csv",
    "DONOR_SEMANTIC_DEFAULTS_MATRIX.csv",
    "RUNTIME_STORAGE_INVOCATION_PROOF_MATRIX.csv",
    "ORIGINAL_SOURCE_RETENTION_MATRIX.csv",
    "DONOR_NATIVE_PROCEDURAL_REGRESSION_MATRIX.csv",
    "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
    "OUTPUT_CONTRACT.csv",
)

REQUIRED_SEMANTIC_SURFACE_FAMILIES = {
    "observable_result_equivalence",
    "indexes",
    "constraints",
    "identifiers_and_namespaces",
    "ddl_transaction_behavior",
    "sequences_generators_identity",
    "resources_text_semantics",
    "expressions_operators_casts",
    "routines_triggers_rules",
    "temporary_transient_objects",
    "statistics_optimizer_metadata",
    "locks_isolation_syntax",
    "system_catalog_defaults",
    "session_settings_and_diagnostics",
}

REQUIRED_DONOR_OPERATION_AREAS = {
    "procedural_api_functional_encoding",
    "exact_datatype_semantics",
    "observable_result_equivalence",
    "donor_semantic_defaults",
    "donor_native_regression_ctest",
    "standalone_dialect_isolation",
    "connection_sandbox_catalog_authority",
    "logical_stream_backup_restore_policy",
    "cdc_replication_etl_policy",
    "low_level_repair_verify_denial",
    "cluster_surface_routing_policy",
    "protocol_session_resource_hardening",
    "project_tests_proof_no_stubs",
}

BLOCKER_STATUSES = {
    "active",
    "active_blocker_truth",
    "active_blocker_truth_gate",
    "current_not_proven",
    "current_route_or_descriptor_only",
    "partial_descriptor_coverage_not_enterprise",
    "pending",
    "route_and_descriptor_only_not_enterprise",
    "route_only_not_enterprise",
}

COMPLETION_STATUS = "enterprise_implemented_proven"

SOURCE_EVIDENCE = {
    "sbsql_executable_descriptor_only": {
        "path": pathlib.Path(
            "project/tests/sbsql_parser_worker/"
            "sbsql_create_executable_exact_route_conformance.cpp"
        ),
        "needles": (
            '"body_text_included":false',
            '"body_compilation_included":false',
            '"runtime_invocation_included":false',
            '"parser_executes_sql":false',
            '"sql_text_included":false',
        ),
    },
    "sbsql_lowering_descriptor_only": {
        "path": pathlib.Path("project/src/parsers/sbsql_worker/lowering/lowering.cpp"),
        "needles": (
            '"body_text_included":false,',
            '"body_compilation_included":false,',
            '"runtime_invocation_included":false,',
        ),
    },
    "firebird_routine_route_classifier": {
        "path": pathlib.Path("project/src/parsers/donor/firebird/firebird_dialect.cpp"),
        "needles": (
            "ClassifyRoutineDdlOperation",
            "firebird.ddl.",
            "PROCEDURE",
            "TRIGGER",
            "PACKAGE",
        ),
    },
    "firebird_route_no_body_probe": {
        "path": pathlib.Path(
            "project/tests/firebird_parser_worker/firebird_binder_sblr_probe.cpp"
        ),
        "needles": (
            "create procedure p as begin end",
            '"finite_subset":true',
            '"sql_text_included":false',
            "SBLRExecutionEnvelope.v3",
        ),
    },
    "first_tranche_route_probe": {
        "path": pathlib.Path(
            "project/tests/donor_sql_parser_first_tranche/"
            "donor_sql_parser_first_tranche_probe.cpp"
        ),
        "needles": (
            "create procedure p() select 1",
            "create procedure p() language sql as 'select 1'",
            '"sql_text_included":false',
            '"engine_authority":"scratchbird"',
        ),
    },
}


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise AssertionError(f"{path}: missing CSV header")
        rows: list[dict[str, str]] = []
        for index, row in enumerate(reader, start=2):
            if None in row:
                raise AssertionError(f"{path}:{index}: extra CSV fields: {row[None]}")
            missing = [key for key, value in row.items() if value is None]
            if missing:
                raise AssertionError(f"{path}:{index}: missing CSV fields: {missing}")
            rows.append({key: value.strip() for key, value in row.items()})
    return rows


def require_columns(path: pathlib.Path,
                    rows: list[dict[str, str]],
                    required: Iterable[str]) -> None:
    if not rows:
        raise AssertionError(f"{path}: no rows")
    fields = set(rows[0].keys())
    missing = [column for column in required if column not in fields]
    if missing:
        raise AssertionError(f"{path}: missing required columns: {missing}")


def donor_rows(rows: list[dict[str, str]], path: pathlib.Path) -> dict[str, dict[str, str]]:
    by_donor = {row["donor"]: row for row in rows}
    if len(by_donor) != len(rows):
        raise AssertionError(f"{path}: duplicate donor rows")
    expected = set(EXPECTED_DONORS)
    actual = set(by_donor)
    if actual != expected:
        raise AssertionError(
            f"{path}: donor set mismatch missing={sorted(expected - actual)} "
            f"extra={sorted(actual - expected)}"
        )
    return by_donor


def status_counts(rows: Iterable[dict[str, str]]) -> dict[str, int]:
    counts: Counter[str] = Counter()
    for row in rows:
        for column, value in row.items():
            if column.endswith("_status") or column == "status":
                counts[f"{column}={value}"] += 1
    return dict(sorted(counts.items()))


def validate_procedural_matrix(path: pathlib.Path) -> dict[str, object]:
    rows = read_csv(path)
    require_columns(
        path,
        rows,
        (
            "donor",
            "procedural_surface_scope",
            "required_body_encoding",
            "current_implementation_status",
            "enterprise_required_status",
            "proof_destination",
        ),
    )
    by_donor = donor_rows(rows, path)
    invalid: list[str] = []
    for donor, row in sorted(by_donor.items()):
        current = row["current_implementation_status"]
        required = row["enterprise_required_status"]
        encoding = row["required_body_encoding"]
        if current not in BLOCKER_STATUSES and current != COMPLETION_STATUS:
            invalid.append(f"{donor}: unexpected current status {current}")
        if required != COMPLETION_STATUS:
            invalid.append(f"{donor}: required status is not {COMPLETION_STATUS}")
        if "executable SBLR" not in encoding:
            invalid.append(f"{donor}: required encoding does not require executable SBLR")
        if "UUID-bound AST" not in encoding:
            invalid.append(f"{donor}: required encoding does not require UUID binding")
        if not row["proof_destination"].startswith("project/tests/"):
            invalid.append(f"{donor}: proof destination is outside project/tests")
    if invalid:
        raise AssertionError(f"{path}: " + "; ".join(invalid))
    return {
        "row_count": len(rows),
        "status_counts": status_counts(rows),
        "enterprise_ready_rows": sum(
            1 for row in rows if row["current_implementation_status"] == COMPLETION_STATUS
        ),
    }


def validate_datatype_matrix(path: pathlib.Path) -> dict[str, object]:
    rows = read_csv(path)
    require_columns(
        path,
        rows,
        (
            "donor",
            "datatype_scope",
            "forbidden_completion_shortcut",
            "current_implementation_status",
            "enterprise_required_status",
            "proof_destination",
        ),
    )
    by_donor = donor_rows(rows, path)
    invalid: list[str] = []
    for donor, row in sorted(by_donor.items()):
        current = row["current_implementation_status"]
        required = row["enterprise_required_status"]
        shortcut = row["forbidden_completion_shortcut"].lower()
        if current not in BLOCKER_STATUSES and current != COMPLETION_STATUS:
            invalid.append(f"{donor}: unexpected current status {current}")
        if required != COMPLETION_STATUS:
            invalid.append(f"{donor}: required status is not {COMPLETION_STATUS}")
        if "generic text" not in shortcut:
            invalid.append(f"{donor}: forbidden shortcuts do not ban generic text fallback")
        if not row["proof_destination"].startswith("project/tests/"):
            invalid.append(f"{donor}: proof destination is outside project/tests")
    if invalid:
        raise AssertionError(f"{path}: " + "; ".join(invalid))
    return {
        "row_count": len(rows),
        "status_counts": status_counts(rows),
        "enterprise_ready_rows": sum(
            1 for row in rows if row["current_implementation_status"] == COMPLETION_STATUS
        ),
    }


def validate_semantic_defaults_matrix(path: pathlib.Path) -> dict[str, object]:
    rows = read_csv(path)
    require_columns(
        path,
        rows,
        (
            "semantic_id",
            "donor_scope",
            "surface_family",
            "semantic_defaults_required",
            "sblr_descriptor_fields_required",
            "engine_behavior_required",
            "catalog_projection_required",
            "current_status",
            "required_status",
            "test_destination",
        ),
    )
    surface_families = {row["surface_family"] for row in rows}
    missing = sorted(REQUIRED_SEMANTIC_SURFACE_FAMILIES - surface_families)
    if missing:
        raise AssertionError(f"{path}: missing semantic surface families: {missing}")

    invalid: list[str] = []
    ids: set[str] = set()
    for row in rows:
        row_id = row["semantic_id"]
        if row_id in ids:
            invalid.append(f"{row_id}: duplicate semantic row")
        ids.add(row_id)
        if (
            row["current_status"] not in BLOCKER_STATUSES
            and row["current_status"] != COMPLETION_STATUS
        ):
            invalid.append(f"{row_id}: unexpected current status {row['current_status']}")
        if row["required_status"] != COMPLETION_STATUS:
            invalid.append(f"{row_id}: required status is not {COMPLETION_STATUS}")
        if "donor" not in row["semantic_defaults_required"].lower():
            invalid.append(f"{row_id}: semantic defaults do not mention donor behavior")
        if (
            "generic" not in row["engine_behavior_required"].lower()
            and row["surface_family"] == "indexes"
        ):
            invalid.append(f"{row_id}: index semantic row does not reject generic defaults")
        if "donor_profile_uuid" not in row["sblr_descriptor_fields_required"]:
            invalid.append(f"{row_id}: descriptor fields do not require donor_profile_uuid")
        if not row["test_destination"].startswith("project/tests/"):
            invalid.append(f"{row_id}: test destination is outside project/tests")
    if invalid:
        raise AssertionError(f"{path}: " + "; ".join(invalid))
    return {
        "row_count": len(rows),
        "surface_families": sorted(surface_families),
        "status_counts": status_counts(rows),
        "enterprise_ready_rows": sum(
            1 for row in rows if row["current_status"] == COMPLETION_STATUS
        ),
    }


def validate_status_matrix(path: pathlib.Path,
                           id_column: str,
                           status_column: str = "current_status") -> dict[str, object]:
    rows = read_csv(path)
    require_columns(
        path,
        rows,
        (id_column, status_column, "required_status", "test_destination"),
    )
    invalid: list[str] = []
    for row in rows:
        row_id = row[id_column]
        current = row[status_column]
        if current not in BLOCKER_STATUSES and current != COMPLETION_STATUS:
            invalid.append(f"{row_id}: unexpected current status {current}")
        if row["required_status"] != COMPLETION_STATUS:
            invalid.append(f"{row_id}: required status is not {COMPLETION_STATUS}")
        if not row["test_destination"].startswith("project/tests/"):
            invalid.append(f"{row_id}: test destination is outside project/tests")
    if invalid:
        raise AssertionError(f"{path}: " + "; ".join(invalid))
    return {
        "row_count": len(rows),
        "status_counts": status_counts(rows),
        "enterprise_ready_rows": sum(
            1 for row in rows if row[status_column] == COMPLETION_STATUS
        ),
    }


def validate_tracker(path: pathlib.Path) -> dict[str, object]:
    rows = read_csv(path)
    require_columns(
        path,
        rows,
        ("slice_id", "phase", "title", "status", "outputs", "acceptance"),
    )
    statuses = {row["slice_id"]: row["status"] for row in rows}
    if statuses.get("DPDE-P0") != "active":
        raise AssertionError(f"{path}: DPDE-P0 must remain the active blocker truth slice")
    for row in rows:
        expected = "active" if row["slice_id"] == "DPDE-P0" else "pending"
        if row["status"] != expected:
            raise AssertionError(f"{path}: {row['slice_id']} must be {expected}")
    return {"row_count": len(rows), "status_counts": status_counts(rows)}


def validate_acceptance_gates(path: pathlib.Path) -> dict[str, object]:
    rows = read_csv(path)
    require_columns(
        path,
        rows,
        ("gate_id", "phase", "status", "requirement", "evidence"),
    )
    statuses = {row["gate_id"]: row["status"] for row in rows}
    if statuses.get("DPDE-GATE-001") != "active":
        raise AssertionError(f"{path}: DPDE-GATE-001 must remain the active blocker truth gate")
    for row in rows:
        expected = "active" if row["gate_id"] == "DPDE-GATE-001" else "pending"
        if row["status"] != expected:
            raise AssertionError(f"{path}: {row['gate_id']} must be {expected}")
    return {"row_count": len(rows), "status_counts": status_counts(rows)}


def validate_output_contract(path: pathlib.Path) -> dict[str, object]:
    rows = read_csv(path)
    require_columns(path, rows, ("output_id", "artifact", "status", "required_columns",
                                 "completion_gate"))
    known_artifacts = set(REQUIRED_FILES)
    invalid: list[str] = []
    for row in rows:
        if row["artifact"] not in known_artifacts:
            invalid.append(f"{row['output_id']}: unknown artifact {row['artifact']}")
        if row["status"] not in {"active_blocker_truth", "pending"}:
            invalid.append(f"{row['output_id']}: unexpected output contract status {row['status']}")
    if invalid:
        raise AssertionError(f"{path}: " + "; ".join(invalid))
    return {"row_count": len(rows), "status_counts": status_counts(rows)}


def validate_source_evidence(repo_root: pathlib.Path) -> dict[str, object]:
    evidence: dict[str, object] = {}
    for key, spec in SOURCE_EVIDENCE.items():
        path = repo_root / spec["path"]
        if not path.is_file():
            raise AssertionError(f"missing source evidence for {key}: {path}")
        text = path.read_text(encoding="utf-8")
        missing = [
            needle for needle in spec["needles"]
            if needle not in text and needle.replace('"', '\\"') not in text
        ]
        if missing:
            raise AssertionError(
                f"{path}: source evidence {key} missing required markers: {missing}"
            )
        evidence[key] = {
            "path": str(spec["path"]),
            "markers_checked": len(spec["needles"]),
        }
    return evidence


def validate_donor_operation_execution-plans(repo_root: pathlib.Path) -> dict[str, object]:
    roots = sorted(
        (repo_root / "public_release_evidence").glob("donor-parser-*implementation-readiness")
    )
    full_firebird = (
        repo_root / "public_execution_plan"
    )
    if full_firebird.is_dir():
        roots.append(full_firebird)
    if len(roots) != 26:
        raise AssertionError(
            "expected 25 donor parser execution-plans plus full Firebird closure "
            f"with operation requirements, found {len(roots)}"
        )

    matrix_name = "DONOR_ENTERPRISE_OPERATION_REQUIREMENTS_MATRIX.csv"
    invalid: list[str] = []
    summary: dict[str, object] = {}
    for root in roots:
        rel_root = root.relative_to(repo_root).as_posix()
        matrix_path = root / matrix_name
        if not matrix_path.is_file():
            invalid.append(f"{rel_root}: missing {matrix_name}")
            continue
        rows = read_csv(matrix_path)
        require_columns(
            matrix_path,
            rows,
            (
                "requirement_id",
                "engine_id",
                "requirement_area",
                "implementation_requirement",
                "forbidden_completion",
                "required_project_tests_path",
                "completion_gate",
                "status",
            ),
        )
        areas = {row["requirement_area"] for row in rows}
        missing = sorted(REQUIRED_DONOR_OPERATION_AREAS - areas)
        extra = sorted(areas - REQUIRED_DONOR_OPERATION_AREAS)
        if missing:
            invalid.append(f"{rel_root}: missing operation areas {missing}")
        if extra:
            invalid.append(f"{rel_root}: unexpected operation areas {extra}")
        for row in rows:
            row_id = row["requirement_id"]
            if row["status"] == COMPLETION_STATUS:
                invalid.append(f"{rel_root}:{row_id}: current status claims completion")
            if row["status"] not in {"pending", "active_blocker_truth", "completed"}:
                invalid.append(f"{rel_root}:{row_id}: unexpected status {row['status']}")
            if not row["required_project_tests_path"].startswith("project/tests/"):
                invalid.append(f"{rel_root}:{row_id}: proof path outside project/tests")
            if "generic" not in row["forbidden_completion"] and row["requirement_area"] in {
                "exact_datatype_semantics",
                "donor_semantic_defaults",
                "project_tests_proof_no_stubs",
            }:
                invalid.append(f"{rel_root}:{row_id}: forbidden completion omits generic fallback")
        readme = (root / "README.md").read_text(encoding="utf-8")
        required_readme = (
            "Enterprise Parser Operation Addendum",
            "observable equivalence",
            "exact datatype semantics",
            "donor-profiled semantic defaults",
            "Route-only, descriptor-only, generic-text, generic-default",
        )
        for phrase in required_readme:
            if phrase not in readme:
                invalid.append(f"{rel_root}: README missing phrase {phrase!r}")
        joined = ""
        for name in (
            "TRACKER.csv",
            "ACCEPTANCE_GATES.csv",
            "ENTERPRISE_COMPLETION_PROOF_MATRIX.csv",
            "COMPATIBILITY_VARIANCE_DECISION_REGISTER.csv",
        ):
            path = root / name
            if not path.is_file():
                invalid.append(f"{rel_root}: missing {name}")
                continue
            joined += path.read_text(encoding="utf-8")
        required_tokens = (
            "observable",
            "semantic defaults",
            "exact datatype",
            "cluster stub",
            "low-level repair",
            "CDC",
        )
        for token in required_tokens:
            if token not in joined:
                invalid.append(f"{rel_root}: execution_plan CSVs missing token {token!r}")
        summary[rel_root] = {
            "row_count": len(rows),
            "engine_ids": sorted({row["engine_id"] for row in rows}),
            "areas": sorted(areas),
            "status_counts": status_counts(rows),
        }
    if invalid:
        raise AssertionError("donor operation execution_plan coverage failed: " + "; ".join(invalid))
    return {
        "execution_plan_count": len(roots),
        "required_area_count": len(REQUIRED_DONOR_OPERATION_AREAS),
        "execution-plans": summary,
    }


def validate_readme(path: pathlib.Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8")
    required_phrases = (
        "Route recognition is not completion.",
        "Generic text fallback is not acceptable",
        "Every donor parser must preserve donor semantic defaults",
        "observable equivalence with the original legacy donor engine",
        "generic ScratchBird descriptor",
        "No donor parser may execute donor SQL as storage or transaction authority.",
        "No original donor source may become execution authority",
        "file presence",
        "executable SBLR",
        "JIT",
        "AOT",
    )
    missing = [phrase for phrase in required_phrases if phrase not in text]
    if missing:
        raise AssertionError(f"{path}: README missing required policy text: {missing}")
    return {"policy_phrases_checked": len(required_phrases)}


def build_evidence(repo_root: pathlib.Path, strict_release: bool) -> dict[str, object]:
    execution_plan = repo_root / EXECUTION_PLAN_REL
    if not execution_plan.is_dir():
        raise AssertionError(f"missing execution_plan directory: {execution_plan}")

    missing = [name for name in REQUIRED_FILES if not (execution_plan / name).exists()]
    if missing:
        raise AssertionError(f"{execution_plan}: missing required files: {missing}")

    matrices = {
        "readme": validate_readme(execution_plan / "README.md"),
        "tracker": validate_tracker(execution_plan / "TRACKER.csv"),
        "acceptance_gates": validate_acceptance_gates(execution_plan / "ACCEPTANCE_GATES.csv"),
        "procedural": validate_procedural_matrix(execution_plan / "PROCEDURAL_LANGUAGE_SCOPE_MATRIX.csv"),
        "datatype": validate_datatype_matrix(execution_plan / "DATATYPE_EXACTNESS_SCOPE_MATRIX.csv"),
        "semantic_defaults": validate_semantic_defaults_matrix(
            execution_plan / "DONOR_SEMANTIC_DEFAULTS_MATRIX.csv"
        ),
        "runtime": validate_status_matrix(
            execution_plan / "RUNTIME_STORAGE_INVOCATION_PROOF_MATRIX.csv", "proof_id"
        ),
        "original_source": validate_status_matrix(
            execution_plan / "ORIGINAL_SOURCE_RETENTION_MATRIX.csv", "source_id"
        ),
        "native_regression": validate_status_matrix(
            execution_plan / "DONOR_NATIVE_PROCEDURAL_REGRESSION_MATRIX.csv", "proof_id"
        ),
        "output_contract": validate_output_contract(execution_plan / "OUTPUT_CONTRACT.csv"),
        "source_evidence": validate_source_evidence(repo_root),
        "donor_operation_execution-plans": validate_donor_operation_execution-plans(repo_root),
    }

    enterprise_ready_rows = 0
    enterprise_ready_required_rows = 0
    blocker_rows = 0
    completion_matrix_keys = {
        "procedural",
        "datatype",
        "semantic_defaults",
        "runtime",
        "original_source",
        "native_regression",
    }
    for key, item in matrices.items():
        if isinstance(item, dict):
            enterprise_ready_rows += int(item.get("enterprise_ready_rows", 0))
            if key in completion_matrix_keys:
                enterprise_ready_required_rows += int(item.get("row_count", 0))
            counts = item.get("status_counts", {})
            if isinstance(counts, dict):
                for key, count in counts.items():
                    _, _, status = key.partition("=")
                    if status in BLOCKER_STATUSES:
                        blocker_rows += int(count)

    source_evidence_blocker_rows = len(SOURCE_EVIDENCE)
    enterprise_release_ready = (
        enterprise_ready_rows == enterprise_ready_required_rows
        and enterprise_ready_required_rows > 0
        and blocker_rows == 0
        and source_evidence_blocker_rows == 0
    )
    return {
        "gate": "donor_procedural_datatype_enterprise_truth_gate",
        "strict_release": strict_release,
        "execution_plan": str(EXECUTION_PLAN_REL),
        "donor_count": len(EXPECTED_DONORS),
        "file_presence_is_completion": False,
        "route_recognition_is_completion": False,
        "descriptor_only_is_completion": False,
        "generic_text_fallback_is_completion": False,
        "original_source_is_execution_authority": False,
        "donor_sql_is_storage_or_transaction_authority": False,
        "requires_uuid_bound_ast": True,
        "requires_executable_sblr": True,
        "requires_jit_aot_ready_body_encoding": True,
        "requires_exact_datatype_descriptors": True,
        "requires_donor_semantic_defaults": True,
        "requires_no_generic_defaults": True,
        "requires_all_donor_execution-plans_updated": True,
        "requires_project_tests_proof": True,
        "requires_donor_native_regression": True,
        "enterprise_release_ready": enterprise_release_ready,
        "enterprise_ready_rows": enterprise_ready_rows,
        "enterprise_ready_required_rows": enterprise_ready_required_rows,
        "blocker_status_rows": blocker_rows,
        "source_evidence_blocker_rows": source_evidence_blocker_rows,
        "matrices": matrices,
    }


def write_evidence(path: pathlib.Path, evidence: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-file", required=True, type=pathlib.Path)
    parser.add_argument("--strict-release", action="store_true")
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    evidence = build_evidence(repo_root, args.strict_release)
    write_evidence(args.evidence_file, evidence)

    if args.strict_release and not evidence["enterprise_release_ready"]:
        raise AssertionError(
            "strict release mode requires zero procedural/datatype blockers; "
            f"blocker_status_rows={evidence['blocker_status_rows']} "
            f"source_evidence_blocker_rows={evidence['source_evidence_blocker_rows']} "
            f"enterprise_ready_rows={evidence['enterprise_ready_rows']}"
        )

    print(
        "donor_procedural_datatype_enterprise_truth_gate="
        f"{'release_ready' if evidence['enterprise_release_ready'] else 'active_blocker_truth'} "
        f"blocker_status_rows={evidence['blocker_status_rows']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except AssertionError as exc:
        print(f"donor_procedural_datatype_enterprise_truth_gate: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
