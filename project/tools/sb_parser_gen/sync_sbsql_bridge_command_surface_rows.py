#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Synchronize canonical SBsql bridge command rows into surface matrices."""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path

from sbsql_bridge_command_surface import (
    BRIDGE_COMMAND_SURFACES,
    BRIDGE_CTEST,
    CANONICAL_SPEC,
    DOCUMENTATION_FAMILY,
    ENGINE_PACKET,
    PARSER_PACKET,
    SBLR_OPERATION_FAMILY,
    SOURCE_ANCHOR,
    SOURCE_FILE,
)


REGISTRY = "public_input_snapshot"
STATUS_MATRIX = "public_input_snapshot"
OPERATION_MATRIX = "public_input_snapshot"
FULL_PARSER_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
)
BACKLOG = f"{FULL_PARSER_ARTIFACT_ROOT}/SURFACE_IMPLEMENTATION_BACKLOG.csv"
BATCH_MEMBERSHIP = f"{FULL_PARSER_ARTIFACT_ROOT}/BATCH_ROW_MEMBERSHIP.csv"
ORACLE_MAP = f"{FULL_PARSER_ARTIFACT_ROOT}/SEMANTIC_ORACLE_AUTHORITY_MAP.csv"
REGISTRY_BATCHING_PLAN = f"{FULL_PARSER_ARTIFACT_ROOT}/REGISTRY_FAMILY_BATCHING_PLAN.csv"
COVERAGE_REGISTRY = "public_contract_snapshot"
BRIDGE_BATCH_ID = "BATCH-0077"


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            fail(f"required CSV has no header: {path}")
        return list(reader.fieldnames), list(reader)


def write_csv(path: Path, fieldnames: list[str], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fieldnames})


def upsert(rows: list[dict[str, str]], key: str, replacement: dict[str, str], label: str) -> bool:
    for index, row in enumerate(rows):
        if row.get(key, "") != replacement[key]:
            continue
        existing_name = row.get("canonical_name", "")
        if existing_name and existing_name != replacement["canonical_name"]:
            fail(
                f"{label} {replacement[key]} collision: "
                f"existing canonical_name={existing_name} replacement={replacement['canonical_name']}"
            )
        changed = any(row.get(field, "") != replacement.get(field, "") for field in replacement)
        rows[index] = dict(row)
        rows[index].update(replacement)
        return changed
    rows.append(replacement)
    return True


def registry_row(surface) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "surface_kind": surface.surface_kind,
        "family": surface.family,
        "status": surface.status,
        "cluster_scope": surface.cluster_scope,
        "source_file": SOURCE_FILE,
        "source_anchor": SOURCE_ANCHOR,
        "canonical_spec": CANONICAL_SPEC,
        "sblr_operation_family": SBLR_OPERATION_FAMILY,
        "parser_packet": PARSER_PACKET,
        "engine_packet": ENGINE_PACKET,
        "documentation_family": DOCUMENTATION_FAMILY,
        "notes": f"tracked_bridge_command_surface;{surface.notes_tokens}",
    }


def status_row(surface) -> dict[str, str]:
    diagnostic = ""
    if surface.cluster_route:
        diagnostic = "SBLR.CLUSTER.SUPPORT_NOT_ENABLED;SBLR.CLUSTER.STUB_RESPONSE;SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY;UDR.BRIDGE.UNSUPPORTED;UDR.BRIDGE.UNLICENSED"
    elif surface.expected_refusal_code:
        diagnostic = surface.expected_refusal_code
    return {
        "surface_id": surface.surface_id,
        "canonical_name": surface.canonical_name,
        "status": surface.status,
        "status_reason": "Accepted bridge command surface; exact behavior is specified by universal bridge ABI and bridge route proof.",
        "allowed_lowering": "yes",
        "diagnostic_if_not_allowed": diagnostic,
    }


def operation_row(surface) -> dict[str, str]:
    return {
        "surface_id": surface.surface_id,
        "canonical_name": surface.canonical_name,
        "sblr_operation_family": SBLR_OPERATION_FAMILY,
        "ingress_envelope": "SBLRExecutionEnvelope.v3",
        "required_context": "session_uuid; database_uuid; transaction_context; security_context; language_profile; bridge_context; result_contract",
        "binding_steps": "parse_to_ast; resolve_bridge_connection_uuid; validate_security; bind_bridge_policy; build_sblr_envelope; verify_sblr; dispatch_trusted_parser_support_udr",
        "result_shape": "rs.sbsql.bridge_operation.v1",
        "diagnostics": "diag.parser.syntax.v1; diag.binding.failure.v1; diag.sbsql.sblr_envelope.v1; diag.sbsql.opcode_admission.v1; diag.bridge.policy.v1; diag.server.runtime.v1",
        "notes": surface.notes_tokens,
    }


def validation_fixture_id(surface) -> str:
    return f"SBSQL-SURFACE-{surface.surface_id.removeprefix('SBSQL-')}"


def backlog_row(surface) -> dict[str, str]:
    engine_behavior = "execute_bridge_udr_route_without_sql_text_or_donor_finality"
    if surface.cluster_route:
        engine_behavior = "compile_gate_to_cluster_provider_stub_or_public_unsupported_vector"
    elif surface.expected_refusal_code == "UDR.BRIDGE.SANDBOX_DENIED":
        engine_behavior = "deny_physical_page_copy_stream_without_server_local_file_or_page_access"
    return {
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "surface_kind": surface.surface_kind,
        "family": surface.family,
        "source_status": surface.status,
        "cluster_scope": surface.cluster_scope,
        "source_search_key": surface.surface_id,
        "canonical_spec": CANONICAL_SPEC,
        "sblr_operation_family": SBLR_OPERATION_FAMILY,
        "parser_packet": PARSER_PACKET,
        "engine_packet": ENGINE_PACKET,
        "owner_lane": "bridge parser worker",
        "target_file_group": "project/src/parsers/sbsql_worker/statements;project/src/parsers/sbsql_worker/lowering;project/src/server;project/src/engine/sblr;project/src/udr/sbsql_bridge",
        "parser_target_behavior": "parse_bind_lower_bridge_command_to_row_specific_sblr_bridge_operation",
        "udr_target_behavior": "route_trusted_bridge_operation_to_registered_udr_or_exact_policy_refusal",
        "server_target_behavior": "admit_revalidate_bridge_route_and_return_message_vector",
        "engine_target_behavior": engine_behavior,
        "diagnostic_target": "canonical_message_vector_and_parser_rendering",
        "validation_fixture_id": validation_fixture_id(surface),
        "final_acceptance_rule": "parse_bind_lower_server_engine_diagnostic_and_regression_evidence",
        "closure_action": "implement_full_route_or_exact_canonical_refusal",
        "status": "e2e_passed",
    }


def batch_row(surface) -> dict[str, str]:
    return {
        "batch_id": BRIDGE_BATCH_ID,
        "surface_id": surface.surface_id,
        "fixed_uuid_v7": surface.fixed_uuid_v7,
        "canonical_name": surface.canonical_name,
        "family": surface.family,
        "surface_kind": surface.surface_kind,
        "source_status": surface.status,
        "cluster_scope": surface.cluster_scope,
        "owner_lane": "bridge parser worker",
        "validation_fixture_id": validation_fixture_id(surface),
        "ctest_label": BRIDGE_CTEST,
        "source_search_key": surface.surface_id,
        "status": "ready_for_fixture_generation",
    }


def oracle_row(surface) -> dict[str, str]:
    refusal = ""
    if surface.expected_refusal_code:
        refusal = f", exact refusal {surface.expected_refusal_code}"
    return {
        "fixture_id": validation_fixture_id(surface),
        "surface_id": surface.surface_id,
        "oracle_type": "canonical_spec_plus_sblr_matrix",
        "oracle_source": CANONICAL_SPEC,
        "source_search_key": surface.surface_id,
        "expected_result_summary": (
            "expected parser bridge command route, SBLR bridge operation envelope, "
            f"opcode {surface.opcode}, UDR operation {surface.effective_udr_operation}, "
            "MGA-preserving local and remote transaction authority"
            f"{refusal}, and donor-specific rendering derived from the universal bridge ABI"
        ),
        "status": "closed_by_semantic_oracle_authority_gate",
    }


def batching_plan_row() -> dict[str, str]:
    return {
        "batch_id": BRIDGE_BATCH_ID,
        "source_matrix": "SBSQL_SURFACE_REGISTRY.csv",
        "surface_filter": "family=bridge;surface_kind=grammar_production;source_status=native_now;cluster_scope=all;SBSQL_BRIDGE_COMMAND_SURFACE_FULL_TRACKING",
        "row_count": str(len(BRIDGE_COMMAND_SURFACES)),
        "owner_lane": "bridge parser worker",
        "parser_target": "generated parser registry plus exact bridge-command parser/lowering route",
        "udr_target": "trusted universal bridge UDR route or exact policy refusal",
        "server_target": "server admission/refusal/streaming behavior with bridge operation family",
        "engine_target": "SBLR bridge opcode route through registered UDR and cluster-provider stub gate",
        "diagnostic_target": "message-vector row and parser rendering fixture",
        "fixture_target": f"project/tests/sbsql_parser_worker/generated/{BRIDGE_BATCH_ID}",
        "ctest_label": BRIDGE_CTEST,
        "max_batch_size": "100",
        "depends_on": "SBSQL_BRIDGE_COMMAND_SURFACE_FULL_TRACKING route and SBLR opcode proof",
        "status": "ready_for_fixture_generation",
    }


def ordered_counts(rows: list[dict[str, str]], field: str, order: list[str]) -> list[tuple[str, int]]:
    counts = Counter(row[field] for row in rows)
    out = [(key, counts.pop(key, 0)) for key in order]
    out.extend(sorted(counts.items()))
    return out


def format_count_map(name: str, values: list[tuple[str, int]]) -> list[str]:
    lines = [f"  {name}:"]
    for key, value in values:
        lines.append(f"    {key}: {value}")
    return lines


def baseline_counts_block(rows: list[dict[str, str]]) -> list[str]:
    expression_rows = [row for row in rows if row["family"] == "expression_runtime"]
    expression_counts = Counter(row["status"] for row in expression_rows)
    return (
        ["baseline_counts:", f"  total_surfaces: {len(rows)}"]
        + format_count_map(
            "status",
            ordered_counts(rows, "status", ["native_now", "cluster_private", "native_future"]),
        )
        + format_count_map(
            "cluster_scope",
            ordered_counts(rows, "cluster_scope", ["noncluster_or_profile_scoped", "cluster_private"]),
        )
        + format_count_map(
            "surface_kind",
            ordered_counts(
                rows,
                "surface_kind",
                ["grammar_production", "canonical_surface", "variable", "operator", "function"],
            ),
        )
        + format_count_map(
            "family",
            ordered_counts(
                rows,
                "family",
                [
                    "acceleration",
                    "archive_replication",
                    "bridge",
                    "cluster_private",
                    "ddl_catalog",
                    "dml",
                    "expression_runtime",
                    "general",
                    "jobs_scheduler",
                    "migration",
                    "multi_model",
                    "observability",
                    "query",
                    "runtime_management",
                    "security",
                    "storage_management",
                    "transaction",
                ],
            ),
        )
        + format_count_map(
            "sblr_operation_family",
            ordered_counts(
                rows,
                "sblr_operation_family",
                [
                    "sblr.acceleration.llvm.v3",
                    "sblr.acceleration.operation.v3",
                    "sblr.archive_replication.operation.v3",
                    "sblr.bridge.operation.v3",
                    "sblr.cluster.private_operation.v3",
                    "sblr.catalog.mutation.v3",
                    "sblr.dml.operation.v3",
                    "sblr.expression.runtime.v3",
                    "sblr.general.operation.v3",
                    "sblr.query.values.v3",
                    "sblr.jobs.operation.v3",
                    "sblr.migration.operation.v3",
                    "sblr.query.multimodel_or_ddl.v3",
                    "sblr.observability.inspect.v3",
                    "sblr.query.relational.v3",
                    "sblr.management.runtime_operation.v3",
                    "sblr.security.mutation_or_inspect.v3",
                    "sblr.storage.management_operation.v3",
                    "sblr.transaction.control.v3",
                ],
            ),
        )
        + [
            "  expression_runtime:",
            f"    total_rows: {len(expression_rows)}",
            f"    native_now: {expression_counts.get('native_now', 0)}",
            f"    cluster_private: {expression_counts.get('cluster_private', 0)}",
            f"    native_future: {expression_counts.get('native_future', 0)}",
        ]
    )


def rewrite_coverage_registry_baselines(path: Path, rows: list[dict[str, str]]) -> bool:
    text = path.read_text(encoding="utf-8")
    start = text.find("baseline_counts:\n")
    end = text.find("\ncoverage_row_required_fields:", start)
    if start == -1 or end == -1:
        fail(f"coverage registry missing baseline_counts block: {path}")
    replacement = "\n".join(baseline_counts_block(rows))
    updated = text[:start] + replacement + text[end:]
    if updated == text:
        return False
    path.write_text(updated, encoding="utf-8")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    registry_header, registry_rows = read_csv(root / REGISTRY)
    status_header, status_rows = read_csv(root / STATUS_MATRIX)
    op_header, op_rows = read_csv(root / OPERATION_MATRIX)
    backlog_header, backlog_rows = read_csv(root / BACKLOG)
    batch_header, batch_rows = read_csv(root / BATCH_MEMBERSHIP)
    oracle_header, oracle_rows = read_csv(root / ORACLE_MAP)
    batching_plan_header, batching_plan_rows = read_csv(root / REGISTRY_BATCHING_PLAN)

    changed = 0
    seen_surface_ids: set[str] = set()
    seen_names: set[str] = set()
    for surface in BRIDGE_COMMAND_SURFACES:
        if surface.surface_id in seen_surface_ids:
            fail(f"duplicate generated bridge surface id {surface.surface_id}")
        if surface.canonical_name in seen_names:
            fail(f"duplicate bridge canonical_name {surface.canonical_name}")
        seen_surface_ids.add(surface.surface_id)
        seen_names.add(surface.canonical_name)
        changed += int(upsert(registry_rows, "surface_id", registry_row(surface), "SBSQL_SURFACE_REGISTRY"))
        changed += int(upsert(status_rows, "surface_id", status_row(surface), "SBSQL_SURFACE_STATUS_MATRIX"))
        changed += int(upsert(op_rows, "surface_id", operation_row(surface), "SBSQL_TO_SBLR_OPERATION_MATRIX"))
        changed += int(upsert(backlog_rows, "surface_id", backlog_row(surface), "SURFACE_IMPLEMENTATION_BACKLOG"))
        changed += int(upsert(batch_rows, "surface_id", batch_row(surface), "BATCH_ROW_MEMBERSHIP"))
        changed += int(upsert(oracle_rows, "surface_id", oracle_row(surface), "SEMANTIC_ORACLE_AUTHORITY_MAP"))
    changed += int(upsert(batching_plan_rows, "batch_id", batching_plan_row(), "REGISTRY_FAMILY_BATCHING_PLAN"))

    registry_rows.sort(key=lambda row: row["surface_id"])
    status_rows.sort(key=lambda row: row["surface_id"])
    op_rows.sort(key=lambda row: row["surface_id"])
    write_csv(root / REGISTRY, registry_header, registry_rows)
    write_csv(root / STATUS_MATRIX, status_header, status_rows)
    write_csv(root / OPERATION_MATRIX, op_header, op_rows)
    write_csv(root / BACKLOG, backlog_header, backlog_rows)
    write_csv(root / BATCH_MEMBERSHIP, batch_header, batch_rows)
    write_csv(root / ORACLE_MAP, oracle_header, oracle_rows)
    write_csv(root / REGISTRY_BATCHING_PLAN, batching_plan_header, batching_plan_rows)
    changed += int(rewrite_coverage_registry_baselines(root / COVERAGE_REGISTRY, registry_rows))

    print(
        "sbsql_bridge_command_surface_rows=synchronized "
        f"rows={len(BRIDGE_COMMAND_SURFACES)} changed={changed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
