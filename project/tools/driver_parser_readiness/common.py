# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Report-mode gates for driver/parser/SBLR/SBSQL readiness.

The gates are intentionally conservative: missing or still-unaccepted evidence
fails with a JSON report instead of silently treating assignment rows as proof.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import defaultdict, deque
from pathlib import Path
import sys
from typing import Any, Callable


def docs_path(*parts: str) -> str:
    return (Path("docs").joinpath(*parts)).as_posix()


SCRIPT_TO_GATE = {
    "open_execution_plan_triage_gate": "DPR-GATE-001",
    "row_count_drift_gate": "DPR-GATE-002",
    "per_element_blocker_gate": "DPR-GATE-003",
    "open_decision_gate": "DPR-GATE-003A",
    "full_dialect_documentation_gate": "DPR-GATE-004",
    "sblr_operation_authority_gate": "DPR-GATE-005",
    "handoff_index_gate": "DPR-GATE-005A",
    "artifact_authority_gate": "DPR-GATE-005B",
    "database_sblr_coverage_gate": "DPR-GATE-005C",
    "agent_start_packet_gate": "DPR-GATE-005D",
    "sbsql_to_sblr_exactness_gate": "DPR-GATE-006",
    "message_vector_catalog_gate": "DPR-GATE-007",
    "message_vector_surface_gate": "DPR-GATE-007A",
    "result_shape_gate": "DPR-GATE-008",
    "driver_local_sbsql_package_gate": "DPR-GATE-009",
    "driver_resource_schema_gate": "DPR-GATE-009A",
    "server_revalidation_gate": "DPR-GATE-009B",
    "driver_first_class_ownership_gate": "DPR-GATE-010",
    "driver_runtime_classification_gate": "DPR-GATE-010A",
    "driver_dependency_injection_gate": "DPR-GATE-010B",
    "driver_lane_contract_gate": "DPR-GATE-010C",
    "parser_runtime_handoff_gate": "DPR-GATE-011",
    "parser_support_udr_gate": "DPR-GATE-012",
    "firebird_cross_contract_gate": "DPR-GATE-013",
    "fixture_golden_gate": "DPR-GATE-014A",
    "gate_command_registry_gate": "DPR-GATE-014B",
    "implementation_sequence_dag_gate": "DPR-GATE-014C",
    "final_go_no_go_gate": "DPR-GATE-014",
}


class GateContext:
    def __init__(self, repo: Path, execution_plan: Path, drivers_index: Path | None) -> None:
        self.repo = repo
        self.execution_plan = execution_plan
        self.drivers_index = drivers_index
        self.findings: list[dict[str, str]] = []
        self.summary: dict[str, Any] = {}

    def error(self, check: str, message: str, path: str = "") -> None:
        self.findings.append(
            {"severity": "error", "check": check, "path": path, "message": message}
        )

    def warn(self, check: str, message: str, path: str = "") -> None:
        self.findings.append(
            {"severity": "warning", "check": check, "path": path, "message": message}
        )

    def csv(self, relative: str) -> list[dict[str, str]]:
        path = self.repo / relative if "/" in relative else self.execution_plan / relative
        if not path.is_file():
            self.error("file_exists", "required CSV missing", path.as_posix())
            return []
        try:
            with path.open(newline="", encoding="utf-8") as handle:
                return list(csv.DictReader(handle))
        except csv.Error as exc:
            self.error("csv_parse", f"CSV parse failed: {exc}", path.as_posix())
            return []

    def text(self, relative: str) -> str:
        path = self.repo / relative if "/" in relative else self.execution_plan / relative
        if not path.is_file():
            self.error("file_exists", "required file missing", path.as_posix())
            return ""
        return path.read_text(encoding="utf-8", errors="replace")

    def path_exists(self, relative: str) -> bool:
        return (self.repo / relative).exists()

    def require_fields(
        self,
        csv_name: str,
        rows: list[dict[str, str]],
        fields: list[str],
        id_field: str,
    ) -> None:
        for row in rows:
            row_id = row.get(id_field, "<missing-id>")
            for field in fields:
                if not row.get(field, "").strip():
                    self.error(
                        "required_field",
                        f"{row_id} missing {field}",
                        str(self.execution_plan / csv_name),
                    )

    def require_no_tokens(
        self,
        rows: list[dict[str, str]],
        fields: list[str],
        tokens: tuple[str, ...],
        id_field: str,
        csv_name: str,
    ) -> None:
        lowered = tuple(token.lower() for token in tokens)
        for row in rows:
            row_id = row.get(id_field, "<missing-id>")
            for field in fields:
                value = row.get(field, "").lower()
                for token in lowered:
                    if token in value:
                        self.error(
                            "forbidden_token",
                            f"{row_id} has unresolved token {token!r} in {field}",
                            str(self.execution_plan / csv_name),
                        )


def row_status_counts(rows: list[dict[str, str]]) -> dict[str, int]:
    counts: dict[str, int] = defaultdict(int)
    for row in rows:
        counts[row.get("status", "")] += 1
    return dict(sorted(counts.items()))


def row_value(row: dict[str, str], *fields: str) -> str:
    for field in fields:
        value = row.get(field, "").strip()
        if value:
            return value
    return ""


def csv_ids(ctx: GateContext, relative: str, id_field: str = "surface_id") -> set[str]:
    rows = ctx.csv(relative)
    ids = {row.get(id_field, "").strip() for row in rows if row.get(id_field, "").strip()}
    ctx.summary[f"{Path(relative).stem}_rows"] = len(rows)
    ctx.summary[f"{Path(relative).stem}_ids"] = len(ids)
    return ids


def manifest_value(path: Path, key: str) -> str:
    if not path.is_file():
        return ""
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(f"{key}="):
            return line.split("=", 1)[1].strip()
    return ""


def active_driver_ids(ctx: GateContext) -> set[str]:
    path = ctx.drivers_index or Path(docs_path("execution-plans", "drivers", "INDEX.csv"))
    rows = ctx.csv(path.as_posix())
    ids = {row["component_id"] for row in rows if row.get("status") == "active"}
    ctx.summary["active_driver_lanes"] = len(ids)
    return ids


def check_open_execution_plan_triage(ctx: GateContext) -> None:
    rows = ctx.csv("OPEN_EXECUTION_PLAN_TRIAGE.csv")
    ctx.summary["rows"] = len(rows)
    ctx.require_fields(
        "OPEN_EXECUTION_PLAN_TRIAGE.csv",
        rows,
        ["execution_plan_path", "status", "assists_driver_parser_readiness", "decision", "next_action"],
        "execution_plan_path",
    )


def check_row_count_drift(ctx: GateContext) -> None:
    registry_ids = csv_ids(
        ctx,
        "public_input_snapshot"
    )
    status_ids = csv_ids(
        ctx,
        "public_input_snapshot"
    )
    matrix_ids = csv_ids(
        ctx,
        "public_input_snapshot"
    )
    backlog_ids = csv_ids(
        ctx,
        docs_path("completed-execution-plans", "full-sbsql-parser-udr-engine-implementation-closure", "artifacts", "SURFACE_IMPLEMENTATION_BACKLOG.csv"),
    )
    batch_ids = csv_ids(
        ctx,
        docs_path("completed-execution-plans", "full-sbsql-parser-udr-engine-implementation-closure", "artifacts", "BATCH_ROW_MEMBERSHIP.csv"),
    )
    oracle_ids = csv_ids(
        ctx,
        docs_path("completed-execution-plans", "full-sbsql-parser-udr-engine-implementation-closure", "artifacts", "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"),
    )
    drift = ctx.csv("DRIFT_REGISTER.csv")

    manifest_path = ctx.repo / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest"
    manifest_count = manifest_value(manifest_path, "surface_count")
    if manifest_count:
        ctx.summary["generated_parser_manifest_surface_count"] = int(manifest_count)
    else:
        ctx.error("generated_parser_manifest", "generated parser registry manifest missing surface_count", manifest_path.as_posix())

    generated_cpp_path = ctx.repo / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp"
    if generated_cpp_path.is_file():
        generated_cpp_text = generated_cpp_path.read_text(encoding="utf-8", errors="replace")
        generated_cpp_ids = set(re.findall(r"SBSQL-[0-9A-F]{12}", generated_cpp_text))
        ctx.summary["generated_parser_cpp_ids"] = len(generated_cpp_ids)
    else:
        generated_cpp_ids = set()
        ctx.error("generated_parser_cpp", "generated parser registry C++ source missing", generated_cpp_path.as_posix())

    roundtrip_dir = ctx.repo / "project/tests/sbsql_parser_worker/generated/full_surface/sblr_binary_round_trip"
    roundtrip_ids = {p.name.split(".", 1)[0] for p in roundtrip_dir.glob("SBSQL-*.round_trip.yaml")}
    ctx.summary["roundtrip_fixture_ids"] = len(roundtrip_ids)

    current_sets = {
        "status_matrix": status_ids,
        "sbsql_to_sblr_matrix": matrix_ids,
        "full_parser_backlog": backlog_ids,
        "batch_membership": batch_ids,
        "semantic_oracle": oracle_ids,
        "generated_cpp": generated_cpp_ids,
        "roundtrip_fixtures": roundtrip_ids,
    }
    ctx.summary["open_drift_rows"] = sum(1 for row in drift if row.get("status") == "open")
    if not registry_ids:
        ctx.error("row_count", "SBSQL surface registry is empty")
    for label, ids in current_sets.items():
        if registry_ids != ids:
            ctx.error("row_count", f"{label} mismatch missing={len(registry_ids - ids)} extra={len(ids - registry_ids)}")
    if manifest_count and int(manifest_count) != len(registry_ids):
        ctx.error("row_count", f"generated parser manifest count {manifest_count} != registry count {len(registry_ids)}")

    for row in drift:
        if row.get("status") != "open":
            continue
        if row.get("drift_type") == "row_count_parity":
            ctx.error("drift_register", f"{row.get('drift_id')} remains open", "DRIFT_REGISTER.csv")
        else:
            ctx.warn(
                "linked_drift_register",
                f"{row.get('drift_id')} remains open under {row.get('owning_gates', 'linked gates')}",
                "DRIFT_REGISTER.csv",
            )


def check_per_element_blockers(ctx: GateContext) -> None:
    rows = ctx.csv(docs_path("execution-plans", "sbsql-per-element-contract-completion", "backlog", "BACKLOG.csv"))
    supersession = ctx.csv("SBSQL_PER_ELEMENT_SUPERSESSION_DECISION.csv")
    ctx.require_fields(
        "SBSQL_PER_ELEMENT_SUPERSESSION_DECISION.csv",
        supersession,
        [
            "decision_id",
            "superseded_execution_plan",
            "affected_rows",
            "prior_status_counts",
            "superseding_authority",
            "rationale",
            "forbidden_interpretation",
            "status",
            "next_action",
        ],
        "decision_id",
    )
    blocked = [
        row for row in rows if row.get("execution_plan_status") in {"blocked", "needs_review", "in_progress"}
    ]
    ctx.summary["blocking_or_stale_rows"] = len(blocked)
    status_counts: dict[str, int] = defaultdict(int)
    for row in rows:
        status_counts[row.get("execution_plan_status", "")] += 1
    ctx.summary["status_counts"] = dict(sorted(status_counts.items()))
    if any(row.get("status") == "completed" for row in supersession):
        ctx.summary["supersession"] = "completed"
        if blocked:
            ctx.warn(
                "per_element_superseded",
                f"{len(blocked)} stale pilot rows superseded by SBSQL_PER_ELEMENT_SUPERSESSION_DECISION.csv",
                "SBSQL_PER_ELEMENT_SUPERSESSION_DECISION.csv",
            )
        return
    for row in blocked[:100]:
        ctx.error(
            "per_element_status",
            f"{row.get('surface_id', '<unknown>')} remains {row.get('execution_plan_status')}",
            docs_path("execution-plans", "sbsql-per-element-contract-completion", "backlog", "BACKLOG.csv"),
        )
    if len(blocked) > 100:
        ctx.warn("per_element_status", f"{len(blocked) - 100} additional stale rows suppressed")


def check_open_decisions(ctx: GateContext) -> None:
    rows = ctx.csv("OPEN_DECISION_AND_ESCALATION_REGISTER.csv")
    ctx.summary["rows"] = len(rows)
    ctx.summary["status_counts"] = row_status_counts(rows)
    ctx.require_fields(
        "OPEN_DECISION_AND_ESCALATION_REGISTER.csv",
        rows,
        [
            "decision_id",
            "topic",
            "blocking_slice",
            "owner",
            "decision_needed",
            "allowed_default",
            "escalation_trigger",
            "stop_condition",
            "impacted_agents",
            "status",
            "next_action",
        ],
        "decision_id",
    )


def check_full_dialect_documentation(ctx: GateContext) -> None:
    rows = ctx.csv(
        "public_input_snapshot"
    )
    handoff = ctx.csv("SBSQL_FULL_DIALECT_HANDOFF_REGISTER.csv")
    supersession = ctx.csv("SBSQL_PER_ELEMENT_SUPERSESSION_DECISION.csv")
    required = [
        "surface_id",
        "canonical_name",
        "status",
        "canonical_spec",
        "sblr_operation_family",
        "parser_packet",
        "engine_packet",
        "documentation_family",
    ]
    ctx.require_fields("SBSQL_SURFACE_REGISTRY.csv", rows, required, "surface_id")
    ctx.require_fields(
        "SBSQL_FULL_DIALECT_HANDOFF_REGISTER.csv",
        handoff,
        [
            "dialect_handoff_id",
            "family",
            "surface_count",
            "canonical_inputs",
            "syntax_authority",
            "semantic_authority",
            "binding_authority",
            "sblr_result_diagnostic_authority",
            "security_transaction_authority",
            "conformance_inputs",
            "status",
            "next_action",
        ],
        "dialect_handoff_id",
    )
    ctx.summary["registry_rows"] = len(rows)
    ctx.summary["dialect_handoff_rows"] = len(handoff)
    family_counts: dict[str, int] = defaultdict(int)
    for row in rows:
        family_counts[row.get("family", "")] += 1
    handoff_families = {row.get("family"): row for row in handoff}
    for family, count in sorted(family_counts.items()):
        handoff_row = handoff_families.get(family)
        if not handoff_row:
            ctx.error("dialect_family", f"missing full dialect handoff family {family}")
            continue
        try:
            handoff_count = int(handoff_row.get("surface_count", ""))
        except ValueError:
            ctx.error("dialect_family_count", f"{family} has non-numeric surface_count")
            continue
        if handoff_count != count:
            ctx.error("dialect_family_count", f"{family} surface_count {handoff_count} != registry count {count}")
    for row in handoff:
        if row.get("status") != "completed":
            ctx.error("dialect_handoff_status", f"{row.get('dialect_handoff_id')} is {row.get('status')}")
    blockers = ctx.csv(docs_path("execution-plans", "sbsql-per-element-contract-completion", "backlog", "BACKLOG.csv"))
    unresolved = [
        row for row in blockers if row.get("execution_plan_status") not in {"done", "completed", "closed"}
    ]
    if any(row.get("status") == "completed" for row in supersession):
        ctx.summary["per_element_supersession"] = "completed"
        if unresolved:
            ctx.warn(
                "per_element_superseded",
                f"{len(unresolved)} per-element pilot rows superseded by full dialect handoff register",
                "SBSQL_PER_ELEMENT_SUPERSESSION_DECISION.csv",
            )
        return
    if unresolved:
        ctx.error(
            "per_element_closure",
            f"{len(unresolved)} per-element rows are not closed",
            docs_path("execution-plans", "sbsql-per-element-contract-completion", "backlog", "BACKLOG.csv"),
        )


def check_handoff_index(ctx: GateContext) -> None:
    rows = ctx.csv("IMPLEMENTATION_HANDOFF_INDEX.csv")
    ctx.summary["rows"] = len(rows)
    ctx.require_fields(
        "IMPLEMENTATION_HANDOFF_INDEX.csv",
        rows,
        [
            "handoff_id",
            "agent_lane",
            "owner",
            "scope",
            "exact_inputs",
            "required_outputs",
            "forbidden_paths",
            "required_gates",
            "dependency_slices",
            "status",
            "next_action",
        ],
        "handoff_id",
    )
    required_lanes = {
        "sbsql_surface_and_dialect",
        "sblr_database_functionality",
        "sbsql_to_sblr_lowering",
        "message_vector_catalog",
        "driver_lane_implementation",
        "parser_executable_runtime",
        "parser_support_udr",
        "firebird_parser_cross_contract",
        "validation_and_gate_materialization",
    }
    present = {row.get("agent_lane") for row in rows}
    for lane in sorted(required_lanes - present):
        ctx.error("handoff_lane", f"missing handoff lane {lane}", "IMPLEMENTATION_HANDOFF_INDEX.csv")


def check_artifact_authority(ctx: GateContext) -> None:
    rows = ctx.csv("GENERATED_ARTIFACT_AUTHORITY_MATRIX.csv")
    ctx.summary["rows"] = len(rows)
    ctx.summary["status_counts"] = row_status_counts(rows)
    ctx.require_fields(
        "GENERATED_ARTIFACT_AUTHORITY_MATRIX.csv",
        rows,
        [
            "artifact_id",
            "artifact_name",
            "artifact_path",
            "artifact_role",
            "source_of_editing",
            "generated_output",
            "allowed_consumers",
            "forbidden_use",
            "required_gate",
            "status",
            "next_action",
        ],
        "artifact_id",
    )
    ctx.require_no_tokens(
        rows,
        ["artifact_role", "source_of_editing", "generated_output", "next_action"],
        ("to_be_classified", "to classify", "pending_classification"),
        "artifact_id",
        "GENERATED_ARTIFACT_AUTHORITY_MATRIX.csv",
    )


def check_database_sblr_coverage(ctx: GateContext) -> None:
    rows = ctx.csv("DATABASE_FUNCTIONALITY_TO_SBLR_COVERAGE.csv")
    ctx.summary["rows"] = len(rows)
    ctx.summary["status_counts"] = row_status_counts(rows)
    ctx.require_fields(
        "DATABASE_FUNCTIONALITY_TO_SBLR_COVERAGE.csv",
        rows,
        [
            "coverage_id",
            "domain",
            "surface_class",
            "canonical_sources",
            "sblr_authority",
            "internal_authority",
            "required_disposition",
            "required_evidence",
            "forbidden_route",
            "owner",
            "status",
            "next_action",
        ],
        "coverage_id",
    )
    for row in rows:
        if row.get("status") != "completed":
            ctx.error("coverage_status", f"{row.get('coverage_id')} is {row.get('status')}")


def check_agent_start_packets(ctx: GateContext) -> None:
    rows = ctx.csv("AGENT_START_PACKET_INDEX.csv")
    ctx.summary["rows"] = len(rows)
    ctx.require_fields(
        "AGENT_START_PACKET_INDEX.csv",
        rows,
        [
            "packet_id",
            "agent_lane",
            "agent_scope",
            "required_read_order",
            "first_task",
            "allowed_edit_paths",
            "required_outputs",
            "stop_conditions",
            "handoff_owner",
            "required_gates",
            "status",
            "next_action",
        ],
        "packet_id",
    )


def check_sbsql_to_sblr_exactness(ctx: GateContext) -> None:
    rows = ctx.csv(
        "public_input_snapshot"
    )
    ctx.summary["rows"] = len(rows)
    for row in rows:
        sid = row.get("surface_id", "<unknown>")
        diagnostics = row.get("diagnostics", "")
        result_shape = row.get("result_shape", "")
        if "*" in diagnostics:
            ctx.error("wildcard_diagnostics", f"{sid} has wildcard diagnostics")
        if "ExecutionResultEnvelope.v3 with message_vector_set" == result_shape:
            ctx.error("generic_result_shape", f"{sid} has generic result envelope")
        if not row.get("sblr_operation_family"):
            ctx.error("missing_sblr_family", f"{sid} lacks SBLR family")


def check_message_vector_surface(ctx: GateContext) -> None:
    rows = ctx.csv("MESSAGE_VECTOR_SURFACE_MAP.csv")
    ctx.summary["rows"] = len(rows)
    ctx.summary["status_counts"] = row_status_counts(rows)
    ctx.require_fields(
        "MESSAGE_VECTOR_SURFACE_MAP.csv",
        rows,
        [
            "vector_map_id",
            "domain",
            "event_class",
            "canonical_sources",
            "trigger_surface",
            "result_shape_link",
            "message_vector_shape",
            "redaction_rule",
            "disclosure_channel",
            "renderer",
            "owner",
            "status",
            "next_action",
        ],
        "vector_map_id",
    )
    for row in rows:
        if row.get("status") != "completed":
            ctx.error("vector_status", f"{row.get('vector_map_id')} is {row.get('status')}")


def check_result_shapes(ctx: GateContext) -> None:
    rows = ctx.csv(
        "public_input_snapshot"
    )
    ctx.summary["rows"] = len(rows)
    for row in rows:
        sid = row.get("surface_id", "<unknown>")
        result_shape = row.get("result_shape", "")
        if "ExecutionResultEnvelope.v3 with message_vector_set" == result_shape:
            ctx.error("generic_result_shape", f"{sid} has generic result envelope")


def check_driver_resource_schema(ctx: GateContext) -> None:
    rows = ctx.csv("DRIVER_RESOURCE_PACKAGE_SCHEMA.csv")
    ctx.summary["rows"] = len(rows)
    ctx.summary["status_counts"] = row_status_counts(rows)
    ctx.require_fields(
        "DRIVER_RESOURCE_PACKAGE_SCHEMA.csv",
        rows,
        [
            "schema_id",
            "resource_component",
            "source_authority",
            "generated_path",
            "required_fields",
            "hash_scope",
            "signature_scope",
            "allowed_consumers",
            "forbidden_use",
            "required_gate",
            "status",
            "next_action",
        ],
        "schema_id",
    )
    for row in rows:
        if row.get("status") != "completed":
            ctx.error("resource_schema_status", f"{row.get('schema_id')} is {row.get('status')}")


def check_server_revalidation(ctx: GateContext) -> None:
    rows = ctx.csv("SERVER_REVALIDATION_CONTRACT_MATRIX.csv")
    ctx.summary["rows"] = len(rows)
    ctx.summary["status_counts"] = row_status_counts(rows)
    ctx.require_fields(
        "SERVER_REVALIDATION_CONTRACT_MATRIX.csv",
        rows,
        [
            "contract_id",
            "client_claim",
            "canonical_authority",
            "server_checks",
            "success_output",
            "failure_vector",
            "forbidden_shortcut",
            "applies_to",
            "required_gate",
            "status",
            "next_action",
        ],
        "contract_id",
    )
    for row in rows:
        if row.get("status") != "completed":
            ctx.error("revalidation_status", f"{row.get('contract_id')} is {row.get('status')}")


def check_driver_first_class_ownership(ctx: GateContext) -> None:
    contracts = ctx.csv("DRIVER_LANE_FIRST_CLASS_CONTRACTS.csv")
    policy = ctx.text("DRIVER_FIRST_CLASS_IMPLEMENTATION_POLICY.md")
    ctx.summary["contract_rows"] = len(contracts)
    if "first-class" not in policy.lower():
        ctx.error("policy_text", "driver first-class policy does not contain first-class ownership text")
    check_driver_lane_contracts(ctx)


def check_driver_runtime_classification(ctx: GateContext) -> None:
    rows = ctx.csv("DRIVER_RUNTIME_SHARING_CLASSIFICATION.csv")
    active = active_driver_ids(ctx)
    ids = {row.get("lane_id") for row in rows}
    ctx.summary["classification_rows"] = len(ids)
    ctx.summary["status_counts"] = row_status_counts(rows)
    for missing in sorted(active - ids):
        ctx.error("classification_coverage", f"missing runtime classification for {missing}")
    for extra in sorted(ids - active):
        ctx.error("classification_coverage", f"classification row is not active lane: {extra}")
    ctx.require_fields(
        "DRIVER_RUNTIME_SHARING_CLASSIFICATION.csv",
        rows,
        [
            "lane_id",
            "package_name",
            "runtime_family",
            "sharing_posture",
            "allowed_shared_package",
            "first_class_evidence_required",
            "status",
            "next_action",
        ],
        "lane_id",
    )
    ctx.require_no_tokens(
        rows,
        ["runtime_family", "sharing_posture", "allowed_shared_package"],
        ("unclassified", "pending_classification"),
        "lane_id",
        "DRIVER_RUNTIME_SHARING_CLASSIFICATION.csv",
    )


def check_driver_dependency_injection(ctx: GateContext) -> None:
    active = active_driver_ids(ctx)
    rows = ctx.csv(docs_path("execution-plans", "drivers", "INDEX.csv"))
    by_id = {row["component_id"]: row for row in rows if row.get("status") == "active"}
    required_token = "driver-parser-sblr-sbsql-readiness-closure"
    for lane_id in sorted(active):
        dep_path = Path(by_id[lane_id]["execution_plan_path"]) / "DEPENDENCIES.csv"
        text = ctx.text(dep_path.as_posix())
        if required_token not in text:
            ctx.error("readiness_dependency", f"{lane_id} lacks readiness dependency", dep_path.as_posix())


def check_driver_lane_contracts(ctx: GateContext) -> None:
    rows = ctx.csv("DRIVER_LANE_FIRST_CLASS_CONTRACTS.csv")
    active = active_driver_ids(ctx)
    ids = {row.get("component_id") for row in rows}
    ctx.summary["contract_rows"] = len(ids)
    ctx.summary["status_counts"] = row_status_counts(rows)
    for missing in sorted(active - ids):
        ctx.error("contract_coverage", f"missing first-class lane contract for {missing}")
    for extra in sorted(ids - active):
        ctx.error("contract_coverage", f"contract row is not active lane: {extra}")
    ctx.require_fields(
        "DRIVER_LANE_FIRST_CLASS_CONTRACTS.csv",
        rows,
        [
            "contract_id",
            "component_id",
            "category",
            "name",
            "execution_plan_path",
            "source_path",
            "runtime_family",
            "implementation_posture",
            "shared_allowed",
            "generated_resources_consumed",
            "forbidden_paths_or_shortcuts",
            "required_lane_outputs",
            "required_gates",
            "status",
            "next_action",
        ],
        "contract_id",
    )


def check_parser_runtime_handoff(ctx: GateContext) -> None:
    rows = ctx.csv("IMPLEMENTATION_HANDOFF_INDEX.csv")
    runtime = [row for row in rows if row.get("agent_lane") == "parser_executable_runtime"]
    if not runtime:
        ctx.error("parser_handoff", "parser_executable_runtime handoff missing")
        return
    for row in runtime:
        if row.get("status") != "completed":
            ctx.error("parser_handoff", f"{row.get('handoff_id')} is {row.get('status')}")


def check_parser_support_udr(ctx: GateContext) -> None:
    rows = ctx.csv("IMPLEMENTATION_HANDOFF_INDEX.csv")
    register = ctx.csv("UDR_AND_PROCEDURAL_LANGUAGE_HANDOFF_REGISTER.csv")
    udr = [row for row in rows if row.get("agent_lane") == "parser_support_udr"]
    if not udr:
        ctx.error("udr_handoff", "parser_support_udr handoff missing")
        return
    ctx.require_fields(
        "UDR_AND_PROCEDURAL_LANGUAGE_HANDOFF_REGISTER.csv",
        register,
        [
            "role_id",
            "parser_support_role",
            "canonical_sources",
            "entrypoints",
            "context_rules",
            "refusal_rules",
            "diagnostic_vectors",
            "conformance_required",
            "forbidden_authority",
            "required_gate",
            "status",
            "next_action",
        ],
        "role_id",
    )
    for row in udr:
        if row.get("status") != "completed":
            ctx.error("udr_handoff", f"{row.get('handoff_id')} is {row.get('status')}")
    for row in register:
        if row.get("status") != "completed":
            ctx.error("udr_register_status", f"{row.get('role_id')} is {row.get('status')}")


def check_firebird_cross_contract(ctx: GateContext) -> None:
    rows = ctx.csv("IMPLEMENTATION_HANDOFF_INDEX.csv")
    register = ctx.csv("FIREBIRD_DIALECT_HANDOFF_REGISTER.csv")
    fb = [row for row in rows if row.get("agent_lane") == "firebird_parser_cross_contract"]
    if not fb:
        ctx.error("firebird_handoff", "firebird_parser_cross_contract handoff missing")
        return
    ctx.require_fields(
        "FIREBIRD_DIALECT_HANDOFF_REGISTER.csv",
        register,
        [
            "contract_id",
            "surface_area",
            "canonical_sources",
            "shared_contracts",
            "required_mapping_outputs",
            "forbidden_authority",
            "required_gates",
            "status",
            "next_action",
        ],
        "contract_id",
    )
    for row in fb:
        if row.get("status") != "completed":
            ctx.error("firebird_handoff", f"{row.get('handoff_id')} is {row.get('status')}")
    for row in register:
        if row.get("status") != "completed":
            ctx.error("firebird_register_status", f"{row.get('contract_id')} is {row.get('status')}")


def check_fixture_golden(ctx: GateContext) -> None:
    rows = ctx.csv("FIXTURE_AND_GOLDEN_CORPUS_INDEX.csv")
    ctx.summary["rows"] = len(rows)
    ctx.summary["status_counts"] = row_status_counts(rows)
    for row in rows:
        row_id = row.get("fixture_id", "<missing-id>")
        required = {
            "fixture_family": row_value(row, "fixture_family"),
            "canonical_sources": row_value(row, "canonical_sources", "canonical_source"),
            "fixture_path": row_value(row, "fixture_path"),
            "golden_path": row_value(row, "golden_path"),
            "covered_gates": row_value(row, "covered_gates", "required_gate"),
            "consumer_agents": row_value(row, "consumer_agents", "consumer_agent"),
            "required_negative_cases": row_value(row, "required_negative_cases", "negative_cases"),
            "forbidden_fixture_source": row_value(row, "forbidden_fixture_source"),
            "status": row_value(row, "status"),
            "next_action": row_value(row, "next_action"),
        }
        for field, value in required.items():
            if not value:
                ctx.error("required_field", f"{row_id} missing {field}", str(ctx.execution_plan / "FIXTURE_AND_GOLDEN_CORPUS_INDEX.csv"))
        fixture_path = ctx.repo / required["fixture_path"]
        golden_path = ctx.repo / required["golden_path"]
        if required["fixture_path"] and not fixture_path.exists():
            ctx.error("fixture_path", f"{row_id} fixture path missing", fixture_path.as_posix())
        if required["golden_path"] and not golden_path.exists():
            ctx.error("golden_path", f"{row_id} golden path missing", golden_path.as_posix())
        if fixture_path.is_dir() and not (fixture_path / "manifest.json").is_file():
            ctx.error("fixture_manifest", f"{row_id} fixture manifest missing", (fixture_path / "manifest.json").as_posix())
        if golden_path.is_dir() and not (golden_path / "expected.json").is_file():
            ctx.error("golden_manifest", f"{row_id} golden expectation missing", (golden_path / "expected.json").as_posix())
        if row.get("status") != "completed":
            ctx.error("fixture_status", f"{row.get('fixture_id')} is {row.get('status')}")


def check_gate_command_registry(ctx: GateContext) -> None:
    gates = {row.get("gate_id") for row in ctx.csv("ACCEPTANCE_GATES.csv")}
    rows = ctx.csv("GATE_COMMAND_REGISTRY.csv")
    cmd_gates = [row.get("gate_id") for row in rows]
    ctx.summary["gate_rows"] = len(gates)
    ctx.summary["command_rows"] = len(rows)
    ctx.require_fields(
        "GATE_COMMAND_REGISTRY.csv",
        rows,
        [
            "command_id",
            "gate_id",
            "phase",
            "working_directory",
            "command",
            "expected_output",
            "report_artifact",
            "timeout_policy",
            "failure_handling",
            "status",
            "next_action",
        ],
        "command_id",
    )
    if set(cmd_gates) != gates:
        ctx.error(
            "gate_coverage",
            f"gate command mismatch missing={sorted(gates - set(cmd_gates))} extra={sorted(set(cmd_gates) - gates)}",
        )
    if len(cmd_gates) != len(set(cmd_gates)):
        ctx.error("gate_coverage", "duplicate command rows for a gate")
    for row in rows:
        parts = row.get("command", "").split()
        if len(parts) >= 2 and parts[0] == "python3":
            script = Path(parts[1])
            if not (ctx.repo / script).is_file():
                ctx.error("command_script", f"script missing for {row.get('command_id')}", script.as_posix())


def check_implementation_sequence_dag(ctx: GateContext) -> None:
    rows = ctx.csv("IMPLEMENTATION_SEQUENCE_DAG.csv")
    ids = {row.get("dag_id") for row in rows}
    ctx.summary["nodes"] = len(ids)
    ctx.require_fields(
        "IMPLEMENTATION_SEQUENCE_DAG.csv",
        rows,
        [
            "dag_id",
            "sequence_node",
            "phase",
            "owner",
            "depends_on",
            "unlocks",
            "parallel_group",
            "required_inputs",
            "required_outputs",
            "blocked_until",
            "stop_conditions",
            "status",
            "next_action",
        ],
        "dag_id",
    )
    adj: dict[str, list[str]] = defaultdict(list)
    indeg = {node_id: 0 for node_id in ids}
    for row in rows:
        node = row.get("dag_id", "")
        for dep in [d for d in row.get("depends_on", "").split(";") if d and d != "none"]:
            if dep.startswith("DPR-DAG-"):
                if dep not in ids:
                    ctx.error("dag_dependency", f"{node} depends on missing {dep}")
                    continue
                adj[dep].append(node)
                indeg[node] += 1
    queue = deque([node_id for node_id, degree in indeg.items() if degree == 0])
    seen: list[str] = []
    while queue:
        node = queue.popleft()
        seen.append(node)
        for nxt in adj[node]:
            indeg[nxt] -= 1
            if indeg[nxt] == 0:
                queue.append(nxt)
    if len(seen) != len(ids):
        ctx.error("dag_cycle", "implementation sequence DAG has a cycle")


def check_final_go_no_go(ctx: GateContext) -> None:
    text = ctx.text("GO_NO_GO.md")
    if "Status: `go`" not in text and "Status: `conditional_go`" not in text:
        ctx.error("go_no_go_status", "GO_NO_GO.md is not go or conditional_go")


def check_driver_local_package(ctx: GateContext) -> None:
    check_driver_resource_schema(ctx)
    check_server_revalidation(ctx)
    resources = ctx.csv("DRIVER_RESOURCE_PACKAGE_SCHEMA.csv")
    revalidation = ctx.csv("SERVER_REVALIDATION_CONTRACT_MATRIX.csv")
    if any(row.get("status") != "completed" for row in resources + revalidation):
        ctx.error("driver_local_package_status", "driver package schema or revalidation rows remain pending")


CHECKS: dict[str, Callable[[GateContext], None]] = {
    "open_execution_plan_triage_gate": check_open_execution_plan_triage,
    "row_count_drift_gate": check_row_count_drift,
    "per_element_blocker_gate": check_per_element_blockers,
    "open_decision_gate": check_open_decisions,
    "full_dialect_documentation_gate": check_full_dialect_documentation,
    "sblr_operation_authority_gate": check_database_sblr_coverage,
    "handoff_index_gate": check_handoff_index,
    "artifact_authority_gate": check_artifact_authority,
    "database_sblr_coverage_gate": check_database_sblr_coverage,
    "agent_start_packet_gate": check_agent_start_packets,
    "sbsql_to_sblr_exactness_gate": check_sbsql_to_sblr_exactness,
    "message_vector_catalog_gate": check_message_vector_surface,
    "message_vector_surface_gate": check_message_vector_surface,
    "result_shape_gate": check_result_shapes,
    "driver_local_sbsql_package_gate": check_driver_local_package,
    "driver_resource_schema_gate": check_driver_resource_schema,
    "server_revalidation_gate": check_server_revalidation,
    "driver_first_class_ownership_gate": check_driver_first_class_ownership,
    "driver_runtime_classification_gate": check_driver_runtime_classification,
    "driver_dependency_injection_gate": check_driver_dependency_injection,
    "driver_lane_contract_gate": check_driver_lane_contracts,
    "parser_runtime_handoff_gate": check_parser_runtime_handoff,
    "parser_support_udr_gate": check_parser_support_udr,
    "firebird_cross_contract_gate": check_firebird_cross_contract,
    "fixture_golden_gate": check_fixture_golden,
    "gate_command_registry_gate": check_gate_command_registry,
    "implementation_sequence_dag_gate": check_implementation_sequence_dag,
    "final_go_no_go_gate": check_final_go_no_go,
}


def main(script_name: str | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--execution_plan", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--drivers-index")
    args = parser.parse_args()

    repo = Path.cwd()
    execution_plan = Path(args.execution_plan)
    if not execution_plan.is_absolute():
        execution_plan = repo / execution_plan
    drivers_index = Path(args.drivers_index) if args.drivers_index else None
    if drivers_index is not None and not drivers_index.is_absolute():
        drivers_index = repo / drivers_index

    script = script_name or Path(sys.argv[0]).stem
    gate_id = SCRIPT_TO_GATE.get(script, "unknown")
    ctx = GateContext(repo=repo, execution_plan=execution_plan, drivers_index=drivers_index)
    check = CHECKS.get(script)
    if check is None:
        ctx.error("gate_dispatch", f"no check registered for {script}")
    else:
        check(ctx)

    error_count = sum(1 for finding in ctx.findings if finding["severity"] == "error")
    warning_count = sum(1 for finding in ctx.findings if finding["severity"] == "warning")
    status = "pass" if error_count == 0 else "fail"
    report = {
        "gate_id": gate_id,
        "script": script,
        "status": status,
        "error_count": error_count,
        "warning_count": warning_count,
        "summary": ctx.summary,
        "findings": ctx.findings,
    }
    report_path = Path(args.report)
    if not report_path.is_absolute():
        report_path = repo / report_path
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{script}={status} report={report_path.as_posix()}")
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
