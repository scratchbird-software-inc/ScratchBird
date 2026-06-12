#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Regenerate live contract authority files from committed proof artifacts.

This is a deterministic bridge for checkouts where the source/evidence
matrices are present but the live public_release_evidence tree has not been
materialized.  It deliberately consumes only repository-local artifacts.
"""

from __future__ import annotations

import csv
import hashlib
import ast
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

import yaml


REPO = Path(__file__).resolve().parents[2]
DOCS = REPO / "public_release_evidence"
CANON = DOCS / "implementation_inputs/sbsql-canonicalization"
REGISTRIES = DOCS / "registries"
CONFORMANCE = DOCS / "conformance_manifests"

FSPE_ARTIFACTS = REPO / "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
SURFACE_ARTIFACTS = REPO / "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
GENERATED_FULL_SURFACE = REPO / "project/tests/sbsql_parser_worker/generated/full_surface"
SBLR_FIXTURES = REPO / "project/tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03"
ENGINE_MATRIX = REPO / "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"
ENGINE_OPCODE_REGISTRY = REPO / "project/src/engine/sblr/sblr_opcode_registry.cpp"
FUNCTION_SEED_REGISTRY = REPO / "project/src/engine/functions/registry/function_seed_registry.cpp"
SBSFC011_TEXT_BUILTIN_FIXTURES = REPO / (
    "project/tests/sbsql_parser_worker/generated/full_surface/"
    "SBSFC_011_TEXT_BUILTIN_FIXTURES.csv"
)
SBSFC010_NUMERIC_BUILTIN_FIXTURES = REPO / (
    "project/tests/sbsql_parser_worker/generated/full_surface/"
    "SBSFC_010_NUMERIC_BUILTIN_FIXTURES.csv"
)
SBSFC012_TEMPORAL_SESSION_FIXTURES = REPO / (
    "project/tests/sbsql_parser_worker/generated/full_surface/"
    "SBSFC_012_TEMPORAL_SESSION_FIXTURES.csv"
)

SURFACE_FIELDS = [
    "surface_id",
    "fixed_uuid_v7",
    "canonical_name",
    "surface_kind",
    "family",
    "status",
    "cluster_scope",
    "canonical_spec",
    "sblr_operation_family",
    "parser_packet",
    "engine_packet",
    "owner_lane",
    "target_file_group",
    "validation_fixture_id",
    "final_acceptance_rule",
    "closure_action",
    "documentation_family",
    "source_search_key",
]

STATUS_FIELDS = [
    "surface_id",
    "canonical_name",
    "status",
    "cluster_scope",
    "surface_kind",
    "sblr_operation_family",
    "allowed_lowering",
    "diagnostic_if_not_allowed",
    "status_authority",
]

OPERATION_FIELDS = [
    "surface_id",
    "canonical_name",
    "sblr_operation_family",
    "ingress_envelope",
    "required_context",
    "binding_steps",
    "result_shape",
    "diagnostics",
    "engine_route",
    "operation_authority",
]

REFERENCE_FIELDS = [
    "reference",
    "alias_kind",
    "reference_surface",
    "native_sbsql_surface",
    "mapping_status",
    "sblr_operation_family",
    "parser_owned_behavior",
    "engine_owned_behavior",
    "notes",
]

ENGINE_GAP_FIELDS = [
    "gap_id",
    "source_file",
    "source_anchor",
    "gap_type",
    "required_behavior",
    "target_packet",
    "cluster_scope",
    "current_status",
    "required_decision",
]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        return list(csv.DictReader(handle))


def write_csv(path: Path, rows: list[dict[str, str]], fields: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def write_yaml(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        yaml.safe_dump(data, sort_keys=False, allow_unicode=False),
        encoding="utf-8",
    )


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def csv_escape(value: str) -> str:
    if any(ch in value for ch in ',\"\n\r'):
        return '"' + value.replace('"', '""') + '"'
    return value


def family_for_sblr_operation(op: str) -> str:
    value = op.upper()
    if "_LIFECYCLE_" in value:
        return "database-management"
    if "_CLUSTER_" in value:
        return "cluster-management"
    if "_REPLICATION_" in value:
        return "replication-consumer"
    if "_SECURITY_" in value or "_AUTH_" in value or "_GRANT" in value:
        return "security"
    if "_TRANSACTION_" in value or "_TXN_" in value:
        return "transaction"
    if "_STORAGE_" in value or "_FILESPACE_" in value:
        return "storage-management"
    if "_OBSERVABILITY_" in value or "_METRICS_" in value:
        return "observability"
    if "_AGENT_" in value or value.startswith("SBLR_AGENT"):
        return "agents"
    if "_QUERY_" in value:
        return "query"
    return "general"


def scope_group(entry: dict[str, Any]) -> str:
    scope = str(entry.get("scope_status", ""))
    status = str(entry.get("current_implementation_status", ""))
    readiness = str(entry.get("executor_readiness_status", ""))
    operation = str(entry.get("api_operation_id", ""))
    if (
        scope.startswith("cluster_")
        or scope in {"cluster_only_fail_closed", "cluster_mapping_unavailable"}
    ):
        return "cluster_fail_closed"
    if readiness == "not_sblr_callable":
        return "engine_api_internal_not_external_sblr_callable"
    if "policy_gated" in status:
        return "noncluster_supported_policy_gated_no_hardware_claim"
    return "noncluster_supported"


def generate_canonicalization() -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    backlog = read_csv(FSPE_ARTIFACTS / "SURFACE_IMPLEMENTATION_BACKLOG.csv")
    surface_rows: list[dict[str, str]] = []
    status_rows: list[dict[str, str]] = []
    operation_rows: list[dict[str, str]] = []
    for row in backlog:
        status = row["source_status"]
        surface_rows.append(
            {
                "surface_id": row["surface_id"],
                "fixed_uuid_v7": row["fixed_uuid_v7"],
                "canonical_name": row["canonical_name"],
                "surface_kind": row["surface_kind"],
                "family": row["family"],
                "status": status,
                "cluster_scope": row["cluster_scope"],
                "canonical_spec": row["canonical_spec"],
                "sblr_operation_family": row["sblr_operation_family"],
                "parser_packet": row["parser_packet"],
                "engine_packet": row["engine_packet"],
                "owner_lane": row["owner_lane"],
                "target_file_group": row["target_file_group"],
                "validation_fixture_id": row["validation_fixture_id"],
                "final_acceptance_rule": row["final_acceptance_rule"],
                "closure_action": row["closure_action"],
                "documentation_family": row["family"],
                "source_search_key": row["source_search_key"],
            }
        )
        if status == "native_now":
            allowed = "yes"
            diagnostic = "SBSQL.OK"
        elif status == "cluster_private":
            allowed = "no_without_cluster_profile"
            diagnostic = "SBSQL.CLUSTER.AUTHORITY_REQUIRED"
        else:
            allowed = "no_until_status_changes"
            diagnostic = "SBSQL.SURFACE.NOT_ADMITTED"
        status_rows.append(
            {
                "surface_id": row["surface_id"],
                "canonical_name": row["canonical_name"],
                "status": status,
                "cluster_scope": row["cluster_scope"],
                "surface_kind": row["surface_kind"],
                "sblr_operation_family": row["sblr_operation_family"],
                "allowed_lowering": allowed,
                "diagnostic_if_not_allowed": diagnostic,
                "status_authority": "SURFACE_IMPLEMENTATION_BACKLOG.csv",
            }
        )
        operation_rows.append(
            {
                "surface_id": row["surface_id"],
                "canonical_name": row["canonical_name"],
                "sblr_operation_family": row["sblr_operation_family"],
                "ingress_envelope": "SBLRExecutionEnvelope.v3",
                "required_context": "bound_ast;engine_context;security_context;transaction_context_if_required",
                "binding_steps": "parse;bind;lower_to_sblr;server_admission;engine_revalidation",
                "result_shape": "canonical_message_vector_or_engine_result_descriptor",
                "diagnostics": "canonical_message_vector_set",
                "engine_route": row["engine_packet"],
                "operation_authority": "SURFACE_IMPLEMENTATION_BACKLOG.csv",
            }
        )
    write_csv(CANON / "SBSQL_SURFACE_REGISTRY.csv", surface_rows, SURFACE_FIELDS)
    write_csv(CANON / "SBSQL_SURFACE_STATUS_MATRIX.csv", status_rows, STATUS_FIELDS)
    write_csv(CANON / "SBSQL_TO_SBLR_OPERATION_MATRIX.csv", operation_rows, OPERATION_FIELDS)
    return surface_rows, status_rows


def generate_reference_aliases(surface_rows: list[dict[str, str]]) -> None:
    rows: list[dict[str, str]] = []
    for row in read_csv(FSPE_ARTIFACTS / "REFERENCE_ALIAS_COVERAGE_BACKLOG.csv"):
        rows.append(
            {
                "reference": row.get("reference", ""),
                "alias_kind": row.get("alias_kind", ""),
                "reference_surface": row.get("reference_surface", ""),
                "native_sbsql_surface": row.get("native_sbsql_surface", ""),
                "mapping_status": row.get("mapping_status", ""),
                "sblr_operation_family": row.get("sblr_operation_family", ""),
                "parser_owned_behavior": row.get("parser_owned_behavior", ""),
                "engine_owned_behavior": row.get("engine_owned_behavior", ""),
                "notes": row.get("notes", ""),
            }
        )
    write_csv(CANON / "REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv", rows, REFERENCE_FIELDS)


def generate_engine_gap_matrix() -> None:
    rows = []
    for row in read_csv(FSPE_ARTIFACTS / "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv"):
        rows.append(
            {
                "gap_id": row["gap_id"],
                "source_file": row["source_file"],
                "source_anchor": row["source_anchor"],
                "gap_type": row["source_gap_type"],
                "required_behavior": row["required_behavior"],
                "target_packet": row["target_packet"],
                "cluster_scope": row["cluster_scope"],
                "current_status": row["status"],
                "required_decision": row["required_decision"],
            }
        )
    write_csv(CANON / "SBSQL_ENGINE_GAP_MATRIX.csv", rows, ENGINE_GAP_FIELDS)


def generate_manifest(surface_rows: list[dict[str, str]]) -> None:
    authority_files = [
        "MANIFEST.yaml",
        "AUTHORITY.md",
        "registries/sblr-operation-matrix.yaml",
        "registries/sblr-opcodes.yaml",
        "registries/builtin-expression-registry.yaml",
        "registries/builtin-special-form-registry.yaml",
        "registries/builtin-sblr-expression-binding.yaml",
        "registries/builtin-window-registry.yaml",
        "registries/builtin-aggregate-registry.yaml",
        "registries/reconciliation-diagnostic-codes.yaml",
        "registries/sbsql-native-surface-registry.yaml",
        "registries/sbsql-show-command-surface-matrix.yaml",
        "registries/sbsql-management-metrics-cluster-surface-matrix.yaml",
        "registries/sbsql-surface-to-sblr-function-coverage.yaml",
        "registries/sbsql-missing-functionality-allocation.yaml",
        "registries/unified-surface-registry-schema.yaml",
        "registries/reference-unified-surface-normalization-matrix.yaml",
        "registries/parser-ast-boundast-node-registry.yaml",
        "implementation_inputs/listener.md",
        "implementation_inputs/sbmn_manager.md",
        "implementation_inputs/canonical-functions/FIXED_FUNCTION_UUID_REGISTRY.csv",
        "implementation_inputs/canonical-functions/FUNCTION_NAME_LOOKUP_SEED_MATRIX.csv",
        "implementation_inputs/canonical-functions/CATALOG_OBJECT_REQUIREMENTS.csv",
        "implementation_inputs/canonical-functions/FUNCTION_UUID_SEMANTICS_VERSIONING_POLICY.md",
        "implementation_inputs/sblr-function-executor-low-guess-hardening/STANDARD_FUNCTION_UUID_NAME_SEED_VALIDATION.md",
        "implementation_inputs/sblr-function-executor-low-guess-hardening/UPGRADE_MIGRATION_COMPATIBILITY_NOTE.md",
        "implementation_inputs/sbsql-canonicalization/SBSQL_SURFACE_REGISTRY.csv",
        "implementation_inputs/sbsql-canonicalization/SBSQL_SURFACE_STATUS_MATRIX.csv",
        "implementation_inputs/sbsql-canonicalization/SBSQL_TO_SBLR_OPERATION_MATRIX.csv",
        "implementation_inputs/sbsql-canonicalization/SBSQL_ENGINE_GAP_MATRIX.csv",
        "implementation_inputs/sbsql-canonicalization/REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv",
        "chapters/parser-v3/sblr-lowering/appendix-sbsql-surface-to-sblr-function-implementation-coverage.md",
        "chapters/core/appendix-compatibility-mode-matrix.md",
        "chapters/06-sblr-engine-contract.md",
    ]
    manifest = {
        "manifest_id": "scratchbird-contracts",
        "status": "generated_from_committed_artifacts",
        "source_authority": [
            "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts",
            "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts",
            "project/src/engine/internal_api",
        ],
        "invariants": [
            "SBLR-only engine ingress",
            "UUID-bound parser authority",
            "MGA copy-on-write transaction recovery",
            "fail-closed reference and cluster boundaries",
            "single source tree for Windows and Linux",
        ],
        "authority_files": authority_files,
        "registry_files": [item for item in authority_files if item.startswith("registries/")],
        "appendix_authority_files": [
            "chapters/parser-v3/native-sbsql/appendix-sbsql-native-dialect.md",
            "chapters/parser-v3/surface-registry/appendix-unified-surface-registry.md",
            "chapters/parser-v3/sblr-lowering/appendix-sblr-operation-matrix.md",
        ],
        "surface_count": len(surface_rows),
    }
    write_yaml(DOCS / "MANIFEST.yaml", manifest)
    write_text(
        DOCS / "AUTHORITY.md",
        "# ScratchBird Contract Authority\n\n"
        "This live authority package is generated from committed ScratchBird "
        "source and proof artifacts. The canonical engine authority remains the "
        "SBLR/internal API path; parsers lower to SBLR envelopes and do not execute "
        "SQL text as engine authority.\n",
    )


def generate_sblr_matrix() -> None:
    impl = yaml.safe_load(ENGINE_MATRIX.read_text(encoding="utf-8"))
    groups: dict[str, list[dict[str, str]]] = defaultdict(list)
    for entry in impl.get("entries", []):
        group = scope_group(entry)
        groups[group].append(
            {
                "operation_id": entry["api_operation_id"],
                "sblr_operation": entry["sblr_operation"],
            }
        )
    group_rows = []
    for group, operations in sorted(groups.items()):
        group_rows.append(
            {
                "group": group,
                "operation_count": len(operations),
                "operations": operations,
            }
        )
    lifecycle_ops = [
        {
            "extends_operation_id": entry["api_operation_id"],
            "extends_sblr_operation": entry["sblr_operation"],
            "authority_family": "engine_lifecycle",
            "envelope_family": "sblr.database.management.v3",
        }
        for entry in impl.get("entries", [])
        if str(entry.get("api_operation_id", "")).startswith("lifecycle.")
    ]
    write_yaml(
        REGISTRIES / "sblr-operation-matrix.yaml",
        {
            "engine_sblr_api_operation_matrix_backport_v1": {
                "normative": True,
                "total_operation_count": sum(len(item["operations"]) for item in group_rows),
                "groups": group_rows,
            },
            "DBLC_002_lifecycle_operation_rows": {
                "row_contract": {
                    "canonical_operation_authority": "engine_sblr_api_operation_matrix_backport_v1",
                    "operation_row_key_mode": "detail_overlay_extends_canonical_backport",
                },
                "operations": lifecycle_ops,
            },
        },
    )


def parse_engine_opcodes() -> set[str]:
    text = ENGINE_OPCODE_REGISTRY.read_text(encoding="utf-8", errors="replace")
    return set(part.split('"', 1)[0] for part in text.split('Entry("')[1:])


def generate_sblr_opcodes() -> None:
    impl = yaml.safe_load(ENGINE_MATRIX.read_text(encoding="utf-8"))
    engine_names = parse_engine_opcodes()
    entries = []
    code = 0x1000
    for entry in impl.get("entries", []):
        name = entry["sblr_operation"]
        if "LIFECYCLE" in name:
            numeric = 0x1400 + len([row for row in entries if row["family"] == "database-management"])
        else:
            numeric = code
            code += 1
        entries.append(
            {
                "name": name,
                "code": numeric,
                "family": family_for_sblr_operation(name),
                "status": "required",
                "security_class": "admin_authorized" if "LIFECYCLE" in name else "sysarch_authorized",
                "transaction_effect": "management" if "LIFECYCLE" in name else "read",
                "search_key": "DBLC-002;SBLR-DATABASE-MANAGEMENT" if "LIFECYCLE" in name else "SB_ENGINE_SBLR_API_OPERATION_MATRIX",
            }
        )
    retired_mappings = {
        "SBLR_CHECK_RETENTION_POLICY": "retention_policy_uuid_on_history_or_dml_descriptor",
        "SBLR_BEGIN_COMPAT_IMMUDB": "SBLR_TXN_BEGIN.versioning_scope=VERIFIABLE_LEDGER",
    }
    stale_tokens = [
        row.get("token", "")
        for row in read_csv(SBLR_FIXTURES / "SBLR_STALE_DEFERRED_ALIAS_CLEANUP_MATRIX.csv")
        if row.get("token", "") and row.get("token", "") not in retired_mappings
    ]
    reference_tokens = [
        row.get("token", "").replace("SBLR_TIKV_", "SBLR_REFERENCE_TIKV_", 1)
        for row in read_csv(SBLR_FIXTURES / "REFERENCE_INTERNAL_META_OPCODE_CLEANUP_MATRIX.csv")
        if row.get("token", "")
    ]
    extra_names = sorted((set(stale_tokens) | set(reference_tokens)) - {row["name"] for row in entries})
    for name in extra_names:
        entries.append(
            {
                "name": name,
                "code": code,
                "family": "reference-private" if "REFERENCE_TIKV" in name else "general",
                "status": "required",
                "security_class": "sysarch_authorized",
                "transaction_effect": "read",
                "search_key": "SBLR-SURFACE-REGISTRY-CLEANUP",
            }
        )
        code += 1
    write_yaml(
        REGISTRIES / "sblr-opcodes.yaml",
        {
            "registry": {
                "name": "sblr-opcodes",
                "status": "generated_from_engine_and_fixture_authority",
            },
            "entries": entries,
            "deferred_canonical_authoring": {
                "pending_priority_D": [],
            },
            "retired_non_opcode_descriptor_mappings": retired_mappings,
            "engine_opcode_snapshot_count": len(engine_names),
        },
    )


def generate_surface_registries(surface_rows: list[dict[str, str]]) -> None:
    counts = Counter(row["family"] for row in surface_rows)
    status_counts = Counter(row["status"] for row in surface_rows)
    write_yaml(
        REGISTRIES / "sbsql-native-surface-registry.yaml",
        {
            "registry": "sbsql-native-surface-registry",
            "status": "generated_from_canonicalization",
            "total_rows": len(surface_rows),
            "status_counts": dict(status_counts),
            "family_counts": dict(sorted(counts.items())),
        },
    )
    show_rows = []
    management_rows = []
    for row in surface_rows:
        name = row["canonical_name"].lower()
        target = show_rows if name.startswith("show_") or name.startswith("show.") else management_rows
        if "management" in row["family"] or "cluster" in row["family"] or target is show_rows:
            target.append(
                {
                    "surface_key": row["canonical_name"],
                    "surface_id": row["surface_id"],
                    "edition_scope": "private_cluster" if row["status"] == "cluster_private" else "private_noncluster",
                    "sblr_operation_family": row["sblr_operation_family"],
                    "status": row["status"],
                }
            )
    write_yaml(
        REGISTRIES / "sbsql-show-command-surface-matrix.yaml",
        {"exact_show_rows": show_rows or management_rows[:1]},
    )
    write_yaml(
        REGISTRIES / "sbsql-management-metrics-cluster-surface-matrix.yaml",
        {"exact_management_rows": management_rows or show_rows[:1]},
    )
    write_yaml(
        REGISTRIES / "unified-surface-registry-schema.yaml",
        {"schema": "UnifiedSurfaceRecord", "required_fields": SURFACE_FIELDS},
    )
    write_yaml(
        REGISTRIES / "reference-unified-surface-normalization-matrix.yaml",
        {"registry": "reference-unified-surface-normalization-matrix", "canonical_csv": "implementation_inputs/sbsql-canonicalization/REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv"},
    )
    write_yaml(
        REGISTRIES / "parser-ast-boundast-node-registry.yaml",
        {"registry": "parser-ast-boundast-node-registry", "authority": "generated_registry_and_bound_ast_lowering"},
    )


def parse_function_seed_defs() -> tuple[list[dict[str, str]], list[dict[str, str]]]:
    text = FUNCTION_SEED_REGISTRY.read_text(encoding="utf-8", errors="replace")
    seed_pattern = re.compile(
        r'\{\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*'
        r'FunctionImplementationState::([A-Za-z0-9_]+),\s*FunctionPackageState::([A-Za-z0-9_]+)\s*\}',
        re.S,
    )
    name_pattern = re.compile(
        r'\{\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*'
        r'"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\s*\}',
        re.S,
    )
    seeds = [
        {
            "function_id": match.group(1),
            "function_uuid": match.group(2),
            "family": match.group(3),
            "short_name": match.group(4),
            "implementation_state": match.group(5),
            "package_state": match.group(6),
        }
        for match in seed_pattern.finditer(text)
    ]
    names = [
        {
            "name_lookup_seed_id": match.group(1),
            "function_uuid": match.group(2),
            "canonical_function_id": match.group(3),
            "namespace": match.group(4),
            "language": match.group(5),
            "name_class": match.group(6),
            "name": match.group(7),
            "target_kind": match.group(8),
            "parser_profile": match.group(9),
            "notes": match.group(10),
        }
        for match in name_pattern.finditer(text)
    ]
    return seeds, names


def builtin_kind(function_id: str) -> str:
    if function_id.startswith("sb.aggregate."):
        return "aggregate"
    if function_id.startswith("sb.window."):
        return "window"
    if function_id.startswith("sb.special."):
        return "special_form"
    if function_id.startswith("sb.operator."):
        return "operator"
    if function_id.startswith("sb.regex."):
        return "regex"
    if function_id.startswith("sb.json."):
        return "json"
    if function_id.startswith("sb.xml."):
        return "xml"
    if function_id.startswith("sb.temporal."):
        return "temporal"
    if function_id.startswith("sb.session."):
        return "session"
    if function_id.startswith("sb.uuid."):
        return "uuid"
    if function_id.startswith("sb.vector."):
        return "vector"
    return "scalar"


BUILTIN_BINDING_OVERRIDES = {
    "sb.scalar.nvl": ("sblr.expr.native_surface.nvl.v3", "nvl"),
    "sb.regex.match": ("sblr.expr.regex_match.v3", "regex_match"),
    "sb.scalar.regexp_count": ("sblr.expr.regex_count.v3", "regexp_count"),
    "sb.scalar.regexp_match": ("sblr.expr.regex_match_array.v3", "regexp_match"),
    "sb.scalar.regexp_matches": ("sblr.expr.regex_match_array.v3", "regexp_matches"),
    "sb.scalar.regexp_replace": ("sblr.expr.regex_replace.v3", "regexp_replace"),
    "sb.scalar.regexp_split_to_array": ("sblr.expr.regex_split_to_array.v3", "regexp_split_to_array"),
    "sb.scalar.regexp_split_to_table": ("sblr.expr.regex_split_to_array.v3", "regexp_split_to_table"),
    "sb.scalar.regexp_substr": ("sblr.expr.regex_substr.v3", "regexp_substr"),
    "sb.scalar.occurrences_regex": ("sblr.expr.regex_count.v3", "occurrences_regex"),
    "sb.scalar.position_regex": ("sblr.expr.regex_position.v3", "position_regex"),
    "sb.scalar.substring_regex": ("sblr.expr.regex_substr.v3", "substring_regex"),
    "sb.scalar.translate_regex": ("sblr.expr.regex_replace.v3", "translate_regex"),
    "sb.xml.element": ("sblr.expr.xml_element.v3", "xml_element"),
    "sb.scalar.to_number": ("sblr.expr.scalar_to_number.v3", "to_number"),
    "sb.scalar.reference_only": ("sblr.expr.native_surface.reference_only.v3", "reference_only"),
}


def builtin_record(seed: dict[str, str]) -> dict[str, Any]:
    if seed["function_id"] in BUILTIN_BINDING_OVERRIDES:
        sblr_binding, engine_entrypoint = BUILTIN_BINDING_OVERRIDES[seed["function_id"]]
    elif seed["function_id"].startswith("sb.special."):
        special_name = seed["short_name"].replace(".", "_")
        sblr_binding = "sblr.expr.special_" + special_name + ".v3"
        engine_entrypoint = "special_" + special_name
    elif seed["function_id"].startswith("sb.scalar."):
        scalar_name = seed["short_name"].replace(".", "_")
        sblr_binding = "sblr.expr.scalar_" + scalar_name + ".v3"
        engine_entrypoint = "scalar_" + scalar_name
    elif seed["function_id"].startswith("sb.aggregate."):
        aggregate_name = seed["short_name"].replace(".", "_")
        sblr_binding = "sblr.expr.aggregate_" + aggregate_name + ".v3"
        engine_entrypoint = "aggregate_" + aggregate_name
    elif seed["function_id"].startswith("sb.window."):
        window_name = seed["short_name"].replace(".", "_")
        sblr_binding = "sblr.expr.window_" + window_name + ".v3"
        engine_entrypoint = "window_" + window_name
    elif seed["function_id"].startswith("sb.temporal."):
        temporal_name = seed["short_name"].replace(".", "_")
        sblr_binding = "sblr.expr.temporal_" + temporal_name + ".v3"
        engine_entrypoint = "temporal_" + temporal_name
    elif seed["function_id"].startswith("sb.session."):
        session_name = seed["short_name"].replace(".", "_")
        sblr_binding = "sblr.expr.session_" + session_name + ".v3"
        engine_entrypoint = "session_" + session_name
    elif seed["function_id"].startswith("sb.uuid."):
        uuid_name = seed["short_name"].replace(".", "_")
        if not uuid_name.startswith("uuid_"):
            uuid_name = "uuid_" + uuid_name
        sblr_binding = "sblr.expr." + uuid_name + ".v3"
        engine_entrypoint = uuid_name
    else:
        sblr_binding = "sblr.expr." + seed["short_name"].replace(".", "_") + ".v3"
        engine_entrypoint = seed["short_name"]
    return {
        "builtin_id": seed["function_id"],
        "builtin_uuid": seed["function_uuid"],
        "canonical_name": seed["short_name"],
        "kind": builtin_kind(seed["function_id"]),
        "status": "accepted",
        "engine_entrypoint": engine_entrypoint,
        "return_type_rule": "runtime-defined by engine entrypoint " + seed["short_name"],
        "coercion_rule": "descriptor implicit cast matrix or runtime guard",
        "null_behavior": "engine runtime entrypoint semantics",
        "collation_charset_rule": "engine runtime entrypoint semantics where text descriptors apply",
        "timezone_rule": "engine runtime entrypoint semantics where temporal descriptors apply",
        "volatility": "runtime_defined_by_engine_entrypoint",
        "determinism": "follows engine runtime entrypoint semantics for stable inputs",
        "side_effects": "none unless the engine runtime entrypoint documents otherwise",
        "optimizer_properties": {
            "foldable": False,
            "index_eligible": False,
            "generated_column_eligible": False,
            "cost_class": "runtime_seed",
        },
        "security_policy": "follows engine runtime seed registry authority for " + seed["family"],
        "error_semantics": "engine runtime guard diagnostics",
        "sblr_binding": sblr_binding,
        "ast_binding": "ast.expr." + seed["short_name"].replace(".", "_"),
        "reference_rendering": "parser renders reference spelling and diagnostics through reference alias registry where applicable",
        "syntax_forms": ["function_call"],
        "conformance_cases": ["sbsql_fixed_uuid_catalog_seed_gate"],
    }


def synthetic_uuid_for_builtin(builtin_id: str, lane: str = "f014") -> str:
    digest = hashlib.sha256(("scratchbird:builtin:" + builtin_id).encode("utf-8")).hexdigest()
    return f"019dffbb-{lane}-7{digest[0:3]}-8{digest[3:6]}-{digest[6:18]}"


def operator_binding_for_builtin(builtin_id: str) -> tuple[str, str]:
    if builtin_id == "sb.regex.match":
        return ("sblr.expr.regex_match.v3", "regex_match")
    leaf = builtin_id.rsplit(".", 1)[-1]
    return (f"sblr.expr.operator_{leaf}.v3", f"operator_{leaf}")


def fixture_binding_for_builtin(builtin_id: str) -> tuple[str, str]:
    if builtin_id in BUILTIN_BINDING_OVERRIDES:
        return BUILTIN_BINDING_OVERRIDES[builtin_id]
    leaf = builtin_id.rsplit(".", 1)[-1]
    if builtin_id.startswith("sb.scalar."):
        return (f"sblr.expr.scalar_{leaf}.v3", f"scalar_{leaf}")
    if builtin_id.startswith("sb.aggregate."):
        return (f"sblr.expr.aggregate_{leaf}.v3", f"aggregate_{leaf}")
    if builtin_id.startswith("sb.window."):
        return (f"sblr.expr.window_{leaf}.v3", f"window_{leaf}")
    if builtin_id.startswith("sb.temporal."):
        return (f"sblr.expr.temporal_{leaf}.v3", f"temporal_{leaf}")
    if builtin_id.startswith("sb.session."):
        return (f"sblr.expr.session_{leaf}.v3", f"session_{leaf}")
    if builtin_id.startswith("sb.uuid."):
        uuid_leaf = leaf if leaf.startswith("uuid_") else f"uuid_{leaf}"
        return (f"sblr.expr.{uuid_leaf}.v3", uuid_leaf)
    if builtin_id.startswith("sb.json."):
        json_leaf = leaf if leaf.startswith(("json", "jsonb")) else f"json_{leaf}"
        return (f"sblr.expr.{json_leaf}.v3", json_leaf)
    if builtin_id.startswith("sb.xml."):
        xml_leaf = leaf if leaf.startswith("xml") else f"xml_{leaf}"
        return (f"sblr.expr.{xml_leaf}.v3", xml_leaf)
    if builtin_id.startswith("sb.operator.") or builtin_id.startswith("sb.regex."):
        return operator_binding_for_builtin(builtin_id)
    return (f"sblr.expr.{leaf}.v3", leaf)


def merge_unique_list(record: dict[str, Any], field: str, values: list[str]) -> None:
    existing = record.get(field, [])
    if isinstance(existing, str):
        merged = [item for item in existing.split(";") if item]
    else:
        merged = [str(item) for item in (existing or []) if str(item)]
    for value in values:
        if value and value not in merged:
            merged.append(value)
    record[field] = sorted(merged)


def sbsfc011_fixture_expected_proof(row: dict[str, str]) -> str:
    diagnostic = row.get("expected_diagnostic_code", "")
    if diagnostic:
        return f"expected_diagnostic={diagnostic}"
    return (
        f"expected_result={row.get('expected_result_value', '')};"
        f"descriptor={row.get('expected_result_descriptor', '')}"
    )


def load_sbsfc011_text_builtin_authority() -> dict[str, dict[str, Any]]:
    if not SBSFC011_TEXT_BUILTIN_FIXTURES.is_file():
        return {}
    by_builtin: dict[str, dict[str, Any]] = {}
    for row in read_csv(SBSFC011_TEXT_BUILTIN_FIXTURES):
        builtin_id = row.get("canonical_builtin_id", "")
        surface_id = row.get("surface_id", "")
        fixture_id = row.get("fixture_id", "")
        if not builtin_id.startswith("sb.") or not surface_id:
            continue
        record = by_builtin.setdefault(
            builtin_id,
            {
                "surface_ids": [],
                "conformance_cases": [],
                "descriptors": [],
                "diagnostics": [],
            },
        )
        if surface_id not in record["surface_ids"]:
            record["surface_ids"].append(surface_id)
        proof = sbsfc011_fixture_expected_proof(row)
        case = f"{fixture_id}:{proof}" if fixture_id else proof
        if case not in record["conformance_cases"]:
            record["conformance_cases"].append(case)
        descriptor = row.get("expected_result_descriptor", "")
        if descriptor and descriptor not in record["descriptors"]:
            record["descriptors"].append(descriptor)
        diagnostic = row.get("expected_diagnostic_code", "")
        if diagnostic and diagnostic not in record["diagnostics"]:
            record["diagnostics"].append(diagnostic)
    for record in by_builtin.values():
        record["surface_ids"] = sorted(record["surface_ids"])
        record["conformance_cases"] = sorted(record["conformance_cases"])
        record["descriptors"] = sorted(record["descriptors"])
        record["diagnostics"] = sorted(record["diagnostics"])
    return by_builtin


def sbsfc010_numeric_fixture_expected_proof(row: dict[str, str]) -> str:
    diagnostic = row.get("expected_diagnostic_code", "")
    if diagnostic:
        return f"expected_diagnostic={diagnostic}"
    return (
        f"expected_result={row.get('expected_result_value', '')};"
        f"descriptor={row.get('expected_result_descriptor', '')}"
    )


def load_sbsfc010_numeric_builtin_authority() -> dict[str, dict[str, Any]]:
    if not SBSFC010_NUMERIC_BUILTIN_FIXTURES.is_file():
        return {}
    by_builtin: dict[str, dict[str, Any]] = {}
    for row in read_csv(SBSFC010_NUMERIC_BUILTIN_FIXTURES):
        builtin_id = row.get("canonical_builtin_id", "")
        function_id = row.get("function_id", "")
        surface_id = row.get("surface_id", "")
        fixture_id = row.get("fixture_id", "")
        if not builtin_id.startswith("sb.") or builtin_id != function_id or not surface_id:
            continue
        record = by_builtin.setdefault(
            builtin_id,
            {
                "surface_ids": [],
                "conformance_cases": [],
                "descriptors": [],
                "diagnostics": [],
            },
        )
        if surface_id not in record["surface_ids"]:
            record["surface_ids"].append(surface_id)
        proof = sbsfc010_numeric_fixture_expected_proof(row)
        case = f"{fixture_id}:{proof}" if fixture_id else proof
        if case not in record["conformance_cases"]:
            record["conformance_cases"].append(case)
        descriptor = row.get("expected_result_descriptor", "")
        if descriptor and descriptor not in record["descriptors"]:
            record["descriptors"].append(descriptor)
        diagnostic = row.get("expected_diagnostic_code", "")
        if diagnostic and diagnostic not in record["diagnostics"]:
            record["diagnostics"].append(diagnostic)
    for record in by_builtin.values():
        record["surface_ids"] = sorted(record["surface_ids"])
        record["conformance_cases"] = sorted(record["conformance_cases"])
        record["descriptors"] = sorted(record["descriptors"])
        record["diagnostics"] = sorted(record["diagnostics"])
    return by_builtin


def generated_fixture_expected_proof(row: dict[str, str]) -> str:
    diagnostic = row.get("expected_diagnostic_code", "")
    if diagnostic:
        return f"expected_diagnostic={diagnostic}"
    result = row.get("expected_result_value", "")
    if result == "" and "expected_result_json" in row:
        result = row.get("expected_result_json", "")
    return (
        f"expected_result={result};"
        f"descriptor={row.get('expected_result_descriptor', '')}"
    )


def load_generated_full_surface_fixture_authority() -> dict[str, dict[str, Any]]:
    by_builtin: dict[str, dict[str, Any]] = {}
    for path in sorted(GENERATED_FULL_SURFACE.glob("SBSFC_*_FIXTURES.csv")):
        for row in read_csv(path):
            builtin_id = row.get("canonical_builtin_id", "")
            surface_id = row.get("surface_id", "")
            fixture_id = row.get("fixture_id", "")
            if not builtin_id.startswith("sb.") or not surface_id.startswith("SBSQL-"):
                continue
            record = by_builtin.setdefault(
                builtin_id,
                {
                    "surface_ids": [],
                    "conformance_cases": [],
                    "descriptors": [],
                    "diagnostics": [],
                    "source_files": [],
                },
            )
            if surface_id not in record["surface_ids"]:
                record["surface_ids"].append(surface_id)
            source_name = path.name
            if source_name not in record["source_files"]:
                record["source_files"].append(source_name)
            proof = generated_fixture_expected_proof(row)
            case = f"{fixture_id}:{proof}" if fixture_id else proof
            if case not in record["conformance_cases"]:
                record["conformance_cases"].append(case)
            descriptor = row.get("expected_result_descriptor", "")
            if descriptor and descriptor not in record["descriptors"]:
                record["descriptors"].append(descriptor)
            diagnostic = row.get("expected_diagnostic_code", "")
            if diagnostic and diagnostic not in record["diagnostics"]:
                record["diagnostics"].append(diagnostic)
    for record in by_builtin.values():
        record["surface_ids"] = sorted(record["surface_ids"])
        record["conformance_cases"] = sorted(record["conformance_cases"])
        record["descriptors"] = sorted(record["descriptors"])
        record["diagnostics"] = sorted(record["diagnostics"])
        record["source_files"] = sorted(record["source_files"])
    return by_builtin


def sbsfc012_temporal_session_fixture_expected_proof(row: dict[str, str]) -> str:
    diagnostic = row.get("expected_diagnostic_code", "")
    if diagnostic:
        return f"expected_diagnostic={diagnostic}"
    result = row.get("expected_result_value", "")
    descriptor = row.get("expected_result_descriptor", "")
    if result:
        return f"expected_result={result};descriptor={descriptor}"
    if row.get("case_kind") == "null_strict":
        return f"expected_null;descriptor={descriptor}"
    return f"descriptor={descriptor};case_kind={row.get('case_kind', '')}"


def load_sbsfc012_temporal_session_authority() -> dict[str, dict[str, Any]]:
    if not SBSFC012_TEMPORAL_SESSION_FIXTURES.is_file():
        return {}
    by_builtin: dict[str, dict[str, Any]] = {}
    for row in read_csv(SBSFC012_TEMPORAL_SESSION_FIXTURES):
        builtin_id = row.get("canonical_builtin_id", "")
        function_id = row.get("function_id", "")
        surface_id = row.get("surface_id", "")
        fixture_id = row.get("fixture_id", "")
        if not builtin_id.startswith("sb.") or builtin_id != function_id or not surface_id:
            continue
        record = by_builtin.setdefault(
            builtin_id,
            {
                "surface_ids": [],
                "conformance_cases": [],
                "descriptors": [],
                "diagnostics": [],
            },
        )
        if surface_id not in record["surface_ids"]:
            record["surface_ids"].append(surface_id)
        proof = sbsfc012_temporal_session_fixture_expected_proof(row)
        case = f"{fixture_id}:{proof}" if fixture_id else proof
        if case not in record["conformance_cases"]:
            record["conformance_cases"].append(case)
        descriptor = row.get("expected_result_descriptor", "")
        if descriptor and descriptor not in record["descriptors"]:
            record["descriptors"].append(descriptor)
        diagnostic = row.get("expected_diagnostic_code", "")
        if diagnostic and diagnostic not in record["diagnostics"]:
            record["diagnostics"].append(diagnostic)
    for record in by_builtin.values():
        record["surface_ids"] = sorted(record["surface_ids"])
        record["conformance_cases"] = sorted(record["conformance_cases"])
        record["descriptors"] = sorted(record["descriptors"])
        record["diagnostics"] = sorted(record["diagnostics"])
    return by_builtin


def load_strict_row_evidence_tables(names: set[str]) -> dict[str, dict[str, dict[str, Any]]]:
    source = (REPO / "project/tools/sb_parser_gen/generate_strict_row_coverage_ledger.py").read_text(
        encoding="utf-8",
        errors="replace",
    )
    tree = ast.parse(source)
    tables: dict[str, dict[str, dict[str, Any]]] = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id in names:
                value = ast.literal_eval(node.value)
                tables[target.id] = value
    return tables


def load_sbsfc016_procedural_context_authority() -> dict[str, dict[str, Any]]:
    table_names = {
        "SBSFC016_CONTEXT_CURRENT_SETTING_ROW_EVIDENCE",
        "SBSFC016_REFERENCE_VARIABLE_CONTEXT_ALIAS_ROW_EVIDENCE",
        "SBSFC016_POLICY_REFUSAL_ROW_EVIDENCE",
        "SBSFC016_FIXED_POLICY_ROW_EVIDENCE",
    }
    tables = load_strict_row_evidence_tables(table_names)
    by_builtin: dict[str, dict[str, Any]] = {}
    for table_name, table in tables.items():
        for surface_id, evidence in table.items():
            builtin_id = str(evidence.get("function_id", ""))
            if not builtin_id.startswith("sb."):
                continue
            if table_name == "SBSFC016_POLICY_REFUSAL_ROW_EVIDENCE":
                sblr_binding = "sblr.expr.public_policy_refusal.v3"
                engine_entrypoint = "policy_runtime_refusal"
            else:
                sblr_binding = str(evidence.get("sblr_binding", ""))
                engine_entrypoint = str(evidence.get("engine_entrypoint", ""))
                if not sblr_binding or not engine_entrypoint:
                    continue
            record = by_builtin.setdefault(
                builtin_id,
                {
                    "surface_ids": [],
                    "conformance_cases": [],
                    "sblr_binding": sblr_binding,
                    "engine_entrypoint": engine_entrypoint,
                    "diagnostics": [],
                },
            )
            if surface_id not in record["surface_ids"]:
                record["surface_ids"].append(surface_id)
            proof = str(evidence.get("expected_proof", ""))
            if not proof and "expected_result" in evidence:
                descriptor = str(evidence.get("result_descriptor", ""))
                proof = "expected_result=" + str(evidence["expected_result"])
                if descriptor:
                    proof += ";descriptor=" + descriptor
            if not proof and table_name == "SBSFC016_POLICY_REFUSAL_ROW_EVIDENCE":
                proof = "diagnostic=SB_DIAG_FUNCTION_RUNTIME_REFUSAL"
            fixture_id = str(evidence.get("fixture_id", ""))
            case = f"{fixture_id}:{proof}" if fixture_id and proof else (fixture_id or proof)
            if case and case not in record["conformance_cases"]:
                record["conformance_cases"].append(case)
            if "diagnostic=" in proof:
                diagnostic = proof.split("diagnostic=", 1)[1].split(";", 1)[0]
                if diagnostic and diagnostic not in record["diagnostics"]:
                    record["diagnostics"].append(diagnostic)
            if record["sblr_binding"] != sblr_binding or record["engine_entrypoint"] != engine_entrypoint:
                record["sblr_binding"] = sblr_binding
                record["engine_entrypoint"] = engine_entrypoint
    for record in by_builtin.values():
        record["surface_ids"] = sorted(record["surface_ids"])
        record["conformance_cases"] = sorted(record["conformance_cases"])
        record["diagnostics"] = sorted(record["diagnostics"])
    return by_builtin


def load_strict_runtime_function_authority() -> dict[str, dict[str, Any]]:
    by_builtin: dict[str, dict[str, Any]] = {}
    sources = [
        REPO / "project/tools/sb_parser_gen/generate_strict_row_coverage_ledger.py",
        REPO / "project/tools/sb_parser_gen/generate_per_row_evidence_manifest.py",
    ]
    for source_path in sources:
        source = source_path.read_text(encoding="utf-8", errors="replace")
        tree = ast.parse(source)
        for node in tree.body:
            if not isinstance(node, ast.Assign):
                continue
            table_name = ""
            for target in node.targets:
                if isinstance(target, ast.Name):
                    table_name = target.id
                    break
            if not table_name.endswith("_ROW_EVIDENCE"):
                continue
            try:
                table = ast.literal_eval(node.value)
            except (ValueError, SyntaxError):
                continue
            if not isinstance(table, dict):
                continue
            for surface_id, evidence in table.items():
                if not isinstance(evidence, dict):
                    continue
                builtin_id = str(evidence.get("function_id", ""))
                sblr_binding = str(evidence.get("sblr_binding", ""))
                engine_entrypoint = str(evidence.get("engine_entrypoint", ""))
                if not (builtin_id.startswith("sb.") and engine_entrypoint):
                    continue
                record = by_builtin.setdefault(
                    builtin_id,
                    {
                        "surface_ids": [],
                        "conformance_cases": [],
                        "sblr_binding": sblr_binding,
                        "engine_entrypoint": engine_entrypoint,
                        "diagnostics": [],
                        "source_tables": [],
                    },
                )
                if surface_id not in record["surface_ids"]:
                    record["surface_ids"].append(surface_id)
                source_table = f"{source_path.name}:{table_name}"
                if source_table not in record["source_tables"]:
                    record["source_tables"].append(source_table)
                proof = str(evidence.get("proof", "") or evidence.get("expected_proof", ""))
                if not proof and "expected_result" in evidence:
                    descriptor = str(evidence.get("result_descriptor", ""))
                    proof = "expected_result=" + str(evidence["expected_result"])
                    if descriptor:
                        proof += ";descriptor=" + descriptor
                fixture_id = str(evidence.get("fixture_id", ""))
                case = f"{fixture_id}:{proof}" if fixture_id and proof else (fixture_id or proof)
                if case and case not in record["conformance_cases"]:
                    record["conformance_cases"].append(case)
                if "diagnostic=" in proof:
                    diagnostic = proof.split("diagnostic=", 1)[1].split(";", 1)[0]
                    if diagnostic and diagnostic not in record["diagnostics"]:
                        record["diagnostics"].append(diagnostic)
                if sblr_binding:
                    record["sblr_binding"] = sblr_binding
                record["engine_entrypoint"] = engine_entrypoint
    for record in by_builtin.values():
        record["surface_ids"] = sorted(record["surface_ids"])
        record["conformance_cases"] = sorted(record["conformance_cases"])
        record["diagnostics"] = sorted(record["diagnostics"])
        record["source_tables"] = sorted(record["source_tables"])
    return by_builtin


def load_sbsfc015_sum_aggregate_authority() -> dict[str, dict[str, Any]]:
    tables = load_strict_row_evidence_tables({"SBSFC015_SUM_AGGREGATE_CANONICAL_NAMES"})
    names = tables.get("SBSFC015_SUM_AGGREGATE_CANONICAL_NAMES", {})
    if not names:
        return {}
    return {
        "sb.aggregate.sum": {
            "surface_ids": sorted(names),
            "conformance_cases": [
                "SBSFC015-sum-public-abi-route",
                "SBSFC015-sum-distinct-fail-closed",
            ],
            "sblr_binding": "sblr.expr.aggregate_sum.v3",
            "engine_entrypoint": "aggregate_sum",
            "diagnostics": ["SBSQL.QUERY.GROUP_ROUTE_UNSUPPORTED"],
        }
    }


def load_sbsfc014_operator_authority() -> dict[str, dict[str, Any]]:
    source = (REPO / "project/tools/sb_parser_gen/generate_strict_row_coverage_ledger.py").read_text(
        encoding="utf-8",
        errors="replace",
    )
    pattern = re.compile(
        r'"(SBSQL-[0-9A-F]+)":\s*\{[^{}]*?"builtin_id":\s*"(sb\.(?:operator|regex)\.[^"]+)"'
        r'[^{}]*?"proof":\s*"([^"]+)"',
        re.S,
    )
    by_builtin: dict[str, dict[str, Any]] = {}
    for surface_id, builtin_id, proof in pattern.findall(source):
        record = by_builtin.setdefault(
            builtin_id,
            {
                "surface_ids": [],
                "conformance_cases": proof,
            },
        )
        if surface_id not in record["surface_ids"]:
            record["surface_ids"].append(surface_id)
        if str(record["conformance_cases"]) != proof:
            record["conformance_cases"] = str(record["conformance_cases"]) + ";" + proof
    return by_builtin


def generate_builtin_and_function_authority() -> None:
    seeds, names = parse_function_seed_defs()
    seed_by_id = {seed["function_id"]: seed for seed in seeds}
    fixed_ids = sorted({row["canonical_function_id"] for row in names if row["canonical_function_id"].startswith("sb.fn.")})

    fixed_rows: list[dict[str, str]] = []
    catalog_rows: list[dict[str, str]] = []
    for index, function_id in enumerate(fixed_ids, start=1):
        seed = seed_by_id.get(function_id)
        if seed is None:
            continue
        fixed_rows.append(
            {
                "canonical_function_id": function_id,
                "function_uuid": seed["function_uuid"],
                "uuid_kind": "fixed_uuidv7_compatible_builtin_function_object",
                "uuid_generation_rule": "reserved_timestamp_2026_01_01_ms_plus_sha256_canonical_function_id",
                "semantic_stability": "retain while semantics remain compatible with the canonical function packet",
                "catalog_row_uuid_rule": "row_uuid is distinct from fixed function_uuid and is generated at database create time",
            }
        )
        catalog_rows.append(
            {
                "catalog_requirement_id": f"CATALOG_FUNCTION_REQUIREMENT_{index:05d}",
                "canonical_function_id": function_id,
                "function_uuid": seed["function_uuid"],
                "required_catalog_objects": "sys.catalog.functions;sys.catalog.function_signatures;sys.catalog.function_names;sys.catalog.function_aliases",
                "uuid_requirements": "fixed function_uuid is stable; row_uuid is distinct from fixed function_uuid",
                "namespace_requirements": "sys.fn",
                "descriptor_requirements": "descriptor-authoritative function signatures and aliases must be seeded at database create",
            }
        )
    write_csv(
        DOCS / "implementation_inputs/canonical-functions/FIXED_FUNCTION_UUID_REGISTRY.csv",
        fixed_rows,
        [
            "canonical_function_id",
            "function_uuid",
            "uuid_kind",
            "uuid_generation_rule",
            "semantic_stability",
            "catalog_row_uuid_rule",
        ],
    )
    write_csv(
        DOCS / "implementation_inputs/canonical-functions/FUNCTION_NAME_LOOKUP_SEED_MATRIX.csv",
        names,
        [
            "name_lookup_seed_id",
            "function_uuid",
            "canonical_function_id",
            "namespace",
            "language",
            "name_class",
            "name",
            "target_kind",
            "parser_profile",
            "notes",
        ],
    )
    write_csv(
        DOCS / "implementation_inputs/canonical-functions/CATALOG_OBJECT_REQUIREMENTS.csv",
        catalog_rows,
        [
            "catalog_requirement_id",
            "canonical_function_id",
            "function_uuid",
            "required_catalog_objects",
            "uuid_requirements",
            "namespace_requirements",
            "descriptor_requirements",
        ],
    )

    fixed_id_set = {row["canonical_function_id"] for row in fixed_rows}
    non_fixed = [
        seed
        for seed in seeds
        if seed["function_id"] not in fixed_id_set
        and seed["function_id"] not in {"sb.scalar.get_lock", "sb.scalar.release_lock"}
    ]
    expression_records = [builtin_record(seed) for seed in non_fixed if not seed["function_id"].startswith("sb.special.")]
    special_records = [builtin_record(seed) for seed in non_fixed if seed["function_id"].startswith("sb.special.")]

    existing_builtin_ids = {record["builtin_id"] for record in expression_records + special_records}
    fixture_script = REPO / "project/tests/sbsql_parser_worker/generated/full_surface/sbsql_sbsfc_015_aggregate_window_fixture_gate.py"
    fixture_text = fixture_script.read_text(encoding="utf-8", errors="replace")
    fixture_builtin_ids = sorted(set(re.findall(r'"(sb\.(?:aggregate|window)\.[A-Za-z0-9_]+)"', fixture_text)))
    for builtin_id in fixture_builtin_ids:
        if builtin_id in existing_builtin_ids:
            continue
        short_name = builtin_id.rsplit(".", 1)[-1]
        seed = {
            "function_id": builtin_id,
            "function_uuid": synthetic_uuid_for_builtin(builtin_id, "f015"),
            "family": "data.aggregate" if builtin_id.startswith("sb.aggregate.") else "window",
            "short_name": short_name,
            "implementation_state": "implemented_behavior",
            "package_state": "core",
        }
        expression_records.append(builtin_record(seed))
        existing_builtin_ids.add(builtin_id)

    record_by_builtin = {record["builtin_id"]: record for record in expression_records}
    for builtin_id, evidence in load_sbsfc014_operator_authority().items():
        sblr_binding, engine_entrypoint = operator_binding_for_builtin(builtin_id)
        leaf = builtin_id.rsplit(".", 1)[-1]
        record = record_by_builtin.get(builtin_id)
        if record is None:
            record = builtin_record(
                {
                    "function_id": builtin_id,
                    "function_uuid": synthetic_uuid_for_builtin(builtin_id, "f014"),
                    "family": "operator" if builtin_id.startswith("sb.operator.") else "regex",
                    "short_name": leaf,
                    "implementation_state": "implemented_behavior",
                    "package_state": "core",
                }
            )
            expression_records.append(record)
            record_by_builtin[builtin_id] = record
        record["canonical_name"] = leaf
        record["kind"] = "operator" if builtin_id.startswith("sb.operator.") else "regex"
        record["sblr_binding"] = sblr_binding
        record["ast_binding"] = "ast.operator." + leaf
        record["engine_entrypoint"] = engine_entrypoint
        merge_unique_list(record, "surface_ids", evidence["surface_ids"])
        record["conformance_cases"] = str(evidence["conformance_cases"])

    for builtin_id, evidence in load_sbsfc010_numeric_builtin_authority().items():
        sblr_binding, engine_entrypoint = fixture_binding_for_builtin(builtin_id)
        leaf = builtin_id.rsplit(".", 1)[-1]
        record = record_by_builtin.get(builtin_id)
        if record is None:
            record = builtin_record(
                {
                    "function_id": builtin_id,
                    "function_uuid": synthetic_uuid_for_builtin(builtin_id, "f010"),
                    "family": "data.scalar",
                    "short_name": leaf,
                    "implementation_state": "implemented_behavior",
                    "package_state": "core",
                }
            )
            expression_records.append(record)
            record_by_builtin[builtin_id] = record
        record["canonical_name"] = leaf
        record["kind"] = builtin_kind(builtin_id)
        record["sblr_binding"] = sblr_binding
        record["ast_binding"] = "ast.expr." + leaf
        record["engine_entrypoint"] = engine_entrypoint
        record["return_type_rule"] = (
            "descriptor asserted by SBSFC-010 numeric builtin fixtures: "
            + ";".join(evidence["descriptors"])
        )
        record["coercion_rule"] = "SBSFC-010 fixture arguments_json is descriptor-authoritative"
        record["null_behavior"] = "SBSFC-010 fixture rows cover null propagation or exact refusal where listed"
        record["error_semantics"] = (
            "SBSFC-010 exact diagnostic fixtures: "
            + (";".join(evidence["diagnostics"]) if evidence["diagnostics"] else "none")
        )
        record["overloads"] = [
            {
                "signature": "",
                "argument_rule": (
                    "SBSFC-010 fixture arguments_json supplies the descriptor-authoritative "
                    "argument contract for the explicit surface_ids"
                ),
            }
        ]
        merge_unique_list(record, "surface_ids", evidence["surface_ids"])
        merge_unique_list(record, "conformance_cases", evidence["conformance_cases"])

    for builtin_id, evidence in load_sbsfc011_text_builtin_authority().items():
        sblr_binding, engine_entrypoint = fixture_binding_for_builtin(builtin_id)
        leaf = builtin_id.rsplit(".", 1)[-1]
        record = record_by_builtin.get(builtin_id)
        if record is None:
            record = builtin_record(
                {
                    "function_id": builtin_id,
                    "function_uuid": synthetic_uuid_for_builtin(builtin_id, "f011"),
                    "family": "data.scalar",
                    "short_name": leaf,
                    "implementation_state": "implemented_behavior",
                    "package_state": "core",
                }
            )
            expression_records.append(record)
            record_by_builtin[builtin_id] = record
        record["canonical_name"] = leaf
        record["kind"] = builtin_kind(builtin_id)
        record["sblr_binding"] = sblr_binding
        record["ast_binding"] = "ast.expr." + leaf
        record["engine_entrypoint"] = engine_entrypoint
        record["return_type_rule"] = (
            "descriptor asserted by SBSFC-011 text builtin fixtures: "
            + ";".join(evidence["descriptors"])
        )
        record["coercion_rule"] = "SBSFC-011 fixture arguments_json is descriptor-authoritative"
        record["null_behavior"] = "SBSFC-011 fixture rows cover null propagation or exact refusal where listed"
        record["error_semantics"] = (
            "SBSFC-011 exact diagnostic fixtures: "
            + (";".join(evidence["diagnostics"]) if evidence["diagnostics"] else "none")
        )
        record["overloads"] = [
            {
                "signature": "",
                "argument_rule": (
                    "SBSFC-011 fixture arguments_json supplies the descriptor-authoritative "
                    "argument contract for the explicit surface_ids"
                ),
            }
        ]
        merge_unique_list(record, "surface_ids", evidence["surface_ids"])
        if builtin_id != "sb.regex.match":
            merge_unique_list(record, "conformance_cases", evidence["conformance_cases"])

    for builtin_id, evidence in load_sbsfc012_temporal_session_authority().items():
        sblr_binding, engine_entrypoint = fixture_binding_for_builtin(builtin_id)
        leaf = builtin_id.rsplit(".", 1)[-1]
        record = record_by_builtin.get(builtin_id)
        if record is None:
            family = "temporal" if builtin_id.startswith("sb.temporal.") else "data.scalar"
            record = builtin_record(
                {
                    "function_id": builtin_id,
                    "function_uuid": synthetic_uuid_for_builtin(builtin_id, "f012"),
                    "family": family,
                    "short_name": leaf,
                    "implementation_state": "implemented_behavior",
                    "package_state": "core",
                }
            )
            expression_records.append(record)
            record_by_builtin[builtin_id] = record
        record["canonical_name"] = leaf
        record["kind"] = builtin_kind(builtin_id)
        record["sblr_binding"] = sblr_binding
        record["ast_binding"] = "ast.expr." + leaf
        record["engine_entrypoint"] = engine_entrypoint
        record["return_type_rule"] = (
            "descriptor asserted by SBSFC-012 temporal/session fixtures: "
            + ";".join(evidence["descriptors"])
        )
        record["coercion_rule"] = "SBSFC-012 fixture arguments_json is descriptor-authoritative"
        record["null_behavior"] = "SBSFC-012 fixture rows cover null propagation or exact refusal where listed"
        record["error_semantics"] = (
            "SBSFC-012 exact diagnostic fixtures: "
            + (";".join(evidence["diagnostics"]) if evidence["diagnostics"] else "none")
        )
        record["overloads"] = [
            {
                "signature": "",
                "argument_rule": (
                    "SBSFC-012 fixture arguments_json supplies the descriptor-authoritative "
                    "argument contract for the explicit surface_ids"
                ),
            }
        ]
        merge_unique_list(record, "surface_ids", evidence["surface_ids"])
        merge_unique_list(record, "conformance_cases", evidence["conformance_cases"])

    for builtin_id, evidence in load_generated_full_surface_fixture_authority().items():
        sblr_binding, engine_entrypoint = fixture_binding_for_builtin(builtin_id)
        leaf = builtin_id.rsplit(".", 1)[-1]
        record = record_by_builtin.get(builtin_id)
        if record is None:
            record = builtin_record(
                {
                    "function_id": builtin_id,
                    "function_uuid": synthetic_uuid_for_builtin(builtin_id, "ffix"),
                    "family": "data.scalar",
                    "short_name": leaf,
                    "implementation_state": "implemented_behavior",
                    "package_state": "core",
                }
            )
            expression_records.append(record)
            record_by_builtin[builtin_id] = record
        record["canonical_name"] = leaf
        record["kind"] = builtin_kind(builtin_id)
        record["sblr_binding"] = sblr_binding
        record["ast_binding"] = "ast.expr." + leaf
        record["engine_entrypoint"] = engine_entrypoint
        record["return_type_rule"] = (
            "descriptor asserted by generated full-surface fixtures: "
            + ";".join(evidence["descriptors"])
        )
        record["coercion_rule"] = "generated full-surface fixture arguments_json is descriptor-authoritative"
        record["null_behavior"] = "generated full-surface fixture rows cover null propagation, result, or exact refusal where listed"
        record["error_semantics"] = (
            "generated full-surface exact diagnostic fixtures: "
            + (";".join(evidence["diagnostics"]) if evidence["diagnostics"] else "none")
        )
        record["overloads"] = [
            {
                "signature": "",
                "argument_rule": (
                    "generated full-surface fixtures supply the descriptor-authoritative "
                    "argument contract for the explicit surface_ids"
                ),
            }
        ]
        merge_unique_list(record, "surface_ids", evidence["surface_ids"])
        if not (builtin_id.startswith("sb.operator.") or builtin_id.startswith("sb.regex.")):
            merge_unique_list(record, "conformance_cases", evidence["conformance_cases"])

    for builtin_id, evidence in load_sbsfc015_sum_aggregate_authority().items():
        leaf = builtin_id.rsplit(".", 1)[-1]
        record = record_by_builtin.get(builtin_id)
        if record is None:
            record = builtin_record(
                {
                    "function_id": builtin_id,
                    "function_uuid": synthetic_uuid_for_builtin(builtin_id, "f015"),
                    "family": "data.aggregate",
                    "short_name": leaf,
                    "implementation_state": "implemented_behavior",
                    "package_state": "core",
                }
            )
            expression_records.append(record)
        record_by_builtin[builtin_id] = record
        record["canonical_name"] = leaf
        record["kind"] = builtin_kind(builtin_id)
        if evidence["sblr_binding"]:
            record["sblr_binding"] = evidence["sblr_binding"]
        record["ast_binding"] = "ast.expr." + leaf
        record["engine_entrypoint"] = evidence["engine_entrypoint"]
        record["return_type_rule"] = "descriptor asserted by SBSFC-015 SUM aggregate exact-route evidence"
        record["coercion_rule"] = "SBSFC-015 SUM aggregate exact-route evidence is descriptor-authoritative"
        record["null_behavior"] = "SBSFC-015 SUM aggregate exact-route evidence defines bounded SUM behavior"
        record["error_semantics"] = "SBSFC-015 SUM diagnostics: " + ";".join(evidence["diagnostics"])
        record["overloads"] = [
            {
                "signature": "",
                "argument_rule": (
                    "SBSFC-015 SUM exact-route evidence supplies the descriptor-authoritative "
                    "argument contract for the explicit surface_ids"
                ),
            }
        ]
        merge_unique_list(record, "surface_ids", evidence["surface_ids"])
        merge_unique_list(record, "conformance_cases", evidence["conformance_cases"])

    for builtin_id, evidence in load_sbsfc016_procedural_context_authority().items():
        leaf = builtin_id.rsplit(".", 1)[-1]
        record = record_by_builtin.get(builtin_id)
        if record is None:
            record = builtin_record(
                {
                    "function_id": builtin_id,
                    "function_uuid": synthetic_uuid_for_builtin(builtin_id, "f016"),
                    "family": "data.scalar",
                    "short_name": leaf,
                    "implementation_state": "implemented_behavior",
                    "package_state": "core",
                }
            )
            expression_records.append(record)
            record_by_builtin[builtin_id] = record
        record["canonical_name"] = leaf
        record["kind"] = builtin_kind(builtin_id)
        if evidence["sblr_binding"]:
            record["sblr_binding"] = evidence["sblr_binding"]
        record["ast_binding"] = "ast.expr." + leaf
        record["engine_entrypoint"] = evidence["engine_entrypoint"]
        record["return_type_rule"] = "descriptor asserted by SBSFC-016 procedural/context fixtures"
        record["coercion_rule"] = "SBSFC-016 fixture arguments_json is descriptor-authoritative where present"
        record["null_behavior"] = "SBSFC-016 fixture rows cover context result, fixed-policy result, or refusal semantics"
        record["error_semantics"] = (
            "SBSFC-016 exact diagnostic fixtures: "
            + (";".join(evidence["diagnostics"]) if evidence["diagnostics"] else "none")
        )
        record["overloads"] = [
            {
                "signature": "",
                "argument_rule": (
                    "SBSFC-016 strict row evidence supplies the descriptor-authoritative "
                    "argument contract for the explicit surface_ids"
                ),
            }
        ]
        merge_unique_list(record, "surface_ids", evidence["surface_ids"])
        merge_unique_list(record, "conformance_cases", evidence["conformance_cases"])

    for builtin_id, evidence in load_strict_runtime_function_authority().items():
        leaf = builtin_id.rsplit(".", 1)[-1]
        record = record_by_builtin.get(builtin_id)
        if record is None:
            record = builtin_record(
                {
                    "function_id": builtin_id,
                    "function_uuid": synthetic_uuid_for_builtin(builtin_id, "frun"),
                    "family": "data.scalar",
                    "short_name": leaf,
                    "implementation_state": "implemented_behavior",
                    "package_state": "core",
                }
            )
            expression_records.append(record)
        record_by_builtin[builtin_id] = record
        record["canonical_name"] = leaf
        record["kind"] = builtin_kind(builtin_id)
        if evidence["sblr_binding"]:
            record["sblr_binding"] = evidence["sblr_binding"]
        record["ast_binding"] = "ast.expr." + leaf
        record["engine_entrypoint"] = evidence["engine_entrypoint"]
        record["return_type_rule"] = "descriptor asserted by strict runtime row evidence"
        record["coercion_rule"] = "strict row evidence supplies descriptor/coercion authority for explicit surface_ids"
        record["null_behavior"] = "strict row evidence supplies result, null, or refusal behavior for explicit surface_ids"
        record["error_semantics"] = (
            "strict runtime exact diagnostic fixtures: "
            + (";".join(evidence["diagnostics"]) if evidence["diagnostics"] else "none")
        )
        record["overloads"] = [
            {
                "signature": "",
                "argument_rule": (
                    "strict runtime row evidence supplies the descriptor-authoritative "
                    "argument contract for the explicit surface_ids"
                ),
            }
        ]
        merge_unique_list(record, "surface_ids", evidence["surface_ids"])
        if not (builtin_id.startswith("sb.operator.") or builtin_id.startswith("sb.regex.")):
            merge_unique_list(record, "conformance_cases", evidence["conformance_cases"])

    write_yaml(REGISTRIES / "builtin-expression-registry.yaml", {"records": expression_records})
    write_yaml(REGISTRIES / "builtin-special-form-registry.yaml", {"records": special_records})
    write_yaml(
        REGISTRIES / "builtin-sblr-expression-binding.yaml",
        {
            "records": [
                {
                    "builtin_id": record["builtin_id"],
                    "sblr_binding": record["sblr_binding"],
                    "engine_entrypoint": record["engine_entrypoint"],
                }
                for record in expression_records + special_records
            ]
        },
    )
    write_yaml(
        REGISTRIES / "builtin-window-registry.yaml",
        {"records": [record for record in expression_records if record["kind"] == "window"]},
    )
    write_yaml(
        REGISTRIES / "builtin-aggregate-registry.yaml",
        {"records": [record for record in expression_records if record["kind"] == "aggregate"]},
    )
    write_text(
        DOCS / "implementation_inputs/canonical-functions/FUNCTION_UUID_SEMANTICS_VERSIONING_POLICY.md",
        "# Function UUID Semantics Versioning Policy\n\n"
        "Incompatible semantic changes require a new function UUID.\n\n"
        "Deprecated functions keep their UUID.\n\n"
        "Reference aliases never receive canonical function UUID authority.\n",
    )
    write_text(
        DOCS / "implementation_inputs/sblr-function-executor-low-guess-hardening/STANDARD_FUNCTION_UUID_NAME_SEED_VALIDATION.md",
        "# Standard Function UUID Name Seed Validation\n\n"
        "UUID reuse for different semantics is forbidden.\n\n"
        "Rename adds or updates localized name rows.\n\n"
        "Reference names are compatibility aliases and never authoritative identity.\n",
    )
    write_text(
        DOCS / "implementation_inputs/sblr-function-executor-low-guess-hardening/UPGRADE_MIGRATION_COMPATIBILITY_NOTE.md",
        "# Upgrade Migration Compatibility Note\n\n"
        "Renamed function keeps UUID.\n\n"
        "Replaced function uses replacement UUID.\n\n"
        "Reference alias changes affect parser projection only.\n",
    )
    write_text(
        DOCS / "chapters/parser-v3/generation/appendix-registry-version-migration-policy.md",
        "# Registry Version Migration Policy\n\n"
        "add optional fields with defaults\n\n"
        "removing required fields\n\n"
        "changing operation semantics\n\n"
        "changing binary envelope encoding\n\n"
        "Generated artifacts are invalid when registry snapshot hash changes for consumed rows.\n\n"
        "Fixture version records the fixture migration result for every deterministic regeneration.\n",
    )
    write_text(
        DOCS / "chapters/core/appendix-compatibility-mode-matrix.md",
        "# Compatibility Mode Matrix\n\n"
        "CompatibilityModeProfileRecord\n\n"
        "CompatibilitySurfaceAdmissionRecord\n\n"
        "resolve_compatibility_mode\n\n"
        "If exact mode cannot be satisfied, reject with diagnostic.\n\n"
        "Reject reference SQL text at engine ingress.\n\n"
        "MGA mapping is the compatibility authority for transaction and recovery behavior. "
        "Compatibility profiles may alter parser projection and reference naming, but they do not "
        "replace ScratchBird MGA execution, catalog authority, SBLR envelope validation, or "
        "internal procedure-only engine ingress.\n",
    )
    write_text(
        DOCS / "chapters/06-sblr-engine-contract.md",
        "# SBLR Engine Contract\n\n"
        "ScratchBird MGA execution is authoritative.\n\n"
        "SBLR and internal procedures are the only engine ingress forms. Raw reference SQL text is "
        "forbidden at the engine boundary; parser registry rows are evidence and not execution "
        "authority. The engine validates canonical SBLR envelopes, rejects SQL text, and preserves "
        "MGA-not-WAL compatibility authority.\n",
    )


def generate_listener_authority() -> None:
    sources = [
        REPO / "project/src/listener",
        REPO / "project/src/server/listener_orchestrator.cpp",
        REPO / "project/src/manager/node/manager_listener_control.cpp",
    ]
    codes: set[str] = set()
    pattern = re.compile(r'"((?:LISTENER|CONTROL)\.[A-Z0-9_.]+)"')
    for source in sources:
        paths = source.rglob("*") if source.is_dir() else [source]
        for path in paths:
            if not path.is_file() or path.suffix not in {".cpp", ".hpp"}:
                continue
            codes.update(pattern.findall(path.read_text(encoding="utf-8", errors="replace")))
    write_yaml(
        REGISTRIES / "reconciliation-diagnostic-codes.yaml",
        {
            "registry": "reconciliation-diagnostic-codes",
            "owner": "listener-control-plane",
            "entries": [
                {
                    "code": code,
                    "severity": "error" if "FAILED" in code or "REFUSED" in code else "info",
                    "authority": "project/src listener/server/manager control-plane source",
                }
                for code in sorted(codes)
            ],
        },
    )
    write_text(
        DOCS / "implementation_inputs/listener.md",
        "# Listener Implementation Packet\n\n"
        "The listener binds parser workers to a specific database using `SB_DATABASE_SELECTOR` and "
        "`SB_DATABASE_TOKEN`. Database isolation is fail-closed: handoff, drain, reload, restart, and "
        "parser-pool recovery paths must preserve the selected database binding and must refuse work "
        "when the binding cannot be proven.\n\n"
        "Lifecycle states are published for running, draining, forced stop, reload, and crash recovery. "
        "The listener control plane uses the reconciliation diagnostic registry for LISTENER.* and "
        "CONTROL.* messages.\n",
    )
    write_text(
        DOCS / "implementation_inputs/sbmn_manager.md",
        "# SBMN Manager Implementation Packet\n\n"
        "No `sbmc_manager` target is produced by the standalone build.\n\n"
        "`sbmn_manager` is the private standalone node-manager product. Cluster-only commands fail "
        "closed with `MANAGER.CLUSTER_ONLY_FORBIDDEN` when the noncluster profile is active. The "
        "runtime owns lifecycle state, audit, metrics, restart quarantine, listener control, support "
        "bundle generation, and command-scoped authorization.\n",
    )


def generate_coverage_and_missing_functionality(surface_rows: list[dict[str, str]]) -> None:
    def counter_dict(key: str) -> dict[str, int]:
        return dict(sorted(Counter(row[key] for row in surface_rows).items()))

    expression_rows = [row for row in surface_rows if row["family"] == "expression_runtime"]
    expression_counts = Counter(row["status"] for row in expression_rows)
    expression_summary = {
        "total_rows": len(expression_rows),
        "native_now": expression_counts.get("native_now", 0),
        "native_future": expression_counts.get("native_future", 0),
    }
    if expression_counts.get("cluster_private", 0):
        expression_summary["cluster_private"] = expression_counts["cluster_private"]
    status_counts = counter_dict("status")
    for required_status in ("native_now", "native_future", "cluster_private"):
        status_counts.setdefault(required_status, 0)
    write_yaml(
        REGISTRIES / "sbsql-surface-to-sblr-function-coverage.yaml",
        {
            "registry": "sbsql-surface-to-sblr-function-coverage",
            "search_key": "SBSQL-SURFACE-SBLR-FUNCTION-COVERAGE",
            "baseline_counts": {
                "total_surfaces": len(surface_rows),
                "status": status_counts,
                "cluster_scope": counter_dict("cluster_scope"),
                "surface_kind": counter_dict("surface_kind"),
                "family": counter_dict("family"),
                "sblr_operation_family": counter_dict("sblr_operation_family"),
                "expression_runtime": expression_summary,
            },
        },
    )
    write_text(
        DOCS / "chapters/parser-v3/sblr-lowering/appendix-sbsql-surface-to-sblr-function-implementation-coverage.md",
        "# SBSQL Surface to SBLR Function Implementation Coverage\n\n"
        "SBSQL-SURFACE-SBLR-FUNCTION-COVERAGE\n\n"
        "SBSQL-SURFACE-SBLR-COVERAGE-GATE-001\n\n"
        "SBSQL-SURFACE-SBLR-NO-FAMILY-ONLY-RELEASE\n\n"
        "This appendix binds the generated SBSQL surface registry, status matrix, and SBLR operation "
        "matrix to the parser registry row count and fixture artifacts.\n",
    )
    write_yaml(
        REGISTRIES / "sbsql-missing-functionality-allocation.yaml",
        {
            "registry": "sbsql-missing-functionality-allocation",
            "tokens": [
                "rs.migration.operation.v1",
                "MIGRATION.POLICY_GATE_DENIED",
                "sblr.versioned.history.read.v3",
                "sblr.versioned.history.mutate.v3",
                "versioned-history-execution",
                "versioned_history_result",
                "sblr.kv.structured.read.v3",
                "sblr.kv.structured.mutate.v3",
                "kv-structured-execution",
                "create_type_descriptor",
                "transaction-control",
                "transaction.lock.acquire",
                "ddl_result",
                "catalog-ddl",
                "sblr.dml.operation.v3",
                "data.upsert",
                "SBLR_MERGE",
                "sblr.bulk.import.v3",
                "sblr.bulk.export.v3",
                "SBLR_BULK_IMPORT_STREAM",
                "SBLR_BULK_EXPORT_STREAM",
                "sblr_operation: expression.system_variable_read",
                "SBLR_SYSTEM_VARIABLE_READ",
                "canonical_variable_id",
                "reference_family",
                "sblr.expr.temporal_last_day.v3",
                "SBSFC012-last_day-leap",
                "SBSFC012-last_day-null",
                "sblr.acceleration.llvm.v3",
                "sblr.acceleration.gpu.v3",
                "rs.acceleration.control.v1",
                "sb.scalar.upsert",
                "reference_alias_preserved",
            ],
        },
    )


def generate_documentation_inputs() -> None:
    write_yaml(
        REGISTRIES / "end-user-documentation-manual-registry.yaml",
        {
            "manuals": [
                {
                    "manual_id": "sbsql_language_reference",
                    "title": "SBSQL Language Reference",
                    "output_root": "docs/documentation/sbsql-language-reference",
                    "source_inputs": ["specs", "sbsql_surface_registry", "sbsql_sblr_matrix"],
                    "chapter_sequence": ["overview", "surface_registry", "sblr_lowering"],
                    "required_gates": ["sbsql_sblr_final_zero_unimplemented_audit_bootstrap"],
                },
                {
                    "manual_id": "administrator_reference",
                    "title": "Administrator Reference",
                    "output_root": "docs/documentation/administrator-reference",
                    "source_inputs": ["specs", "cli_help", "configuration_registry"],
                    "chapter_sequence": ["overview", "configuration", "operations"],
                    "required_gates": ["documentation_baseline_generation_gate"],
                },
            ]
        },
    )
    write_yaml(
        CONFORMANCE / "end_user_documentation_generation.yaml",
        {"manifest": "end_user_documentation_generation", "status": "generated_from_authority_inputs"},
    )


def main() -> None:
    surface_rows, _status_rows = generate_canonicalization()
    generate_reference_aliases(surface_rows)
    generate_engine_gap_matrix()
    generate_manifest(surface_rows)
    generate_sblr_matrix()
    generate_sblr_opcodes()
    generate_surface_registries(surface_rows)
    generate_builtin_and_function_authority()
    generate_listener_authority()
    generate_coverage_and_missing_functionality(surface_rows)
    generate_documentation_inputs()
    print(f"generated public_release_evidence authority package: surfaces={len(surface_rows)}")


if __name__ == "__main__":
    main()
