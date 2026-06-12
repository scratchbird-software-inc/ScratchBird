#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DPC-071 operator and UI documentation gate.

This gate reads only product documentation under docs/management-interface. It
must not use execution_plan artifacts as runtime inputs.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


DOC_ROOT = Path("docs/management-interface")
REQUIRED_FILES = (
    DOC_ROOT / "README.md",
    DOC_ROOT / "MANAGEMENT_SURFACE_REFERENCE.md",
    DOC_ROOT / "OPERATOR_WORKFLOWS.md",
)

REQUIRED_TOKENS = (
    "DPC_OPERATOR_UI_DOCS",
    "frontend developers",
    "operators",
    "MANAGEMENT_SURFACE_REFERENCE.md",
    "OPERATOR_WORKFLOWS.md",
    "EngineInspectPerformanceOptimizationSurface",
    "EngineShowManagement",
    "EnginePrepareSupportBundle",
    "EngineIndexManagementOperation",
    "rs.performance_optimization_surface.v1",
    "observability.show_management",
    "performance_optimization_surface",
    "index.management.route_surface.v1",
    "DPC-070 Resource/Soak Evidence Fields",
    "Index Management Operations, Rights, And Message Vectors",
    "The `SELECT` examples are management adapter\nprojections",
    "Engine MGA owns\nvisibility",
    "users are always in a session transaction",
    "optimization_profile",
    "catalog_generation_id",
    "name_resolution_epoch",
    "security_epoch",
    "resource_epoch",
    "statistics_epoch",
    "cleanup_horizon_authority_status",
    "oldest_interesting_transaction_id",
    "oldest_active_transaction_id",
    "oldest_snapshot_transaction_id",
    "oldest_cleanup_transaction_id",
    "storage_row_version_backlog_count",
    "index_delta_backlog_count",
    "index_garbage_backlog_count",
    "page_summary_backlog_count",
    "secondary_index_state",
    "shadow_index_state",
    "summary_index_state",
    "specialized_index_state",
    "index_state_authority_source",
    "resource_governor_state",
    "resource_quota_grants",
    "backpressure_active",
    "exact_refusal_diagnostic_code",
    "exact_refusal_message_vector",
    "message_vector_ready",
    "metric_family",
    "audit_event_family",
    "support_bundle_redaction_state",
    "support_bundle_completeness_state",
    "support_bundle_forbidden_fields_absent",
    "config_precedence_order",
    "parser_finality_authority",
    "reference_finality_authority",
    "client_finality_authority",
    "storage_shortcut_finality_authority",
    "wal_recovery_authority",
    "catalog_uuid_authority",
    "admin_override > cli_option > environment > config_file > packaged_default",
    "optimizer_enabled",
    "copy_append_batching_enabled",
    "native_ingest_enabled",
    "plan_cache_enabled",
    "descriptor_metadata_cache_enabled",
    "statistics_enabled",
    "summary_prune_enabled",
    "agent_workers_enabled",
    "resource_governor_enabled",
    "page_filespace_preallocation_enabled",
    "cancellation_enabled",
    "backpressure_enabled",
    "copy_batch_rows_configured",
    "DPC.CONFIG.OVERRIDE_DENIED_BY_POLICY",
    "OBS_CONFIG_OVERRIDE_REQUIRED",
    "SECURITY.AUTHORIZATION.DENIED",
    "SECURITY.CONTEXT.EXPIRED",
    "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED",
    "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED",
    "OPS.SUPPORT_BUNDLE.ENGINE_AUTHORIZATION_REQUIRED",
    "OPS.SUPPORT_BUNDLE.PROTECTED_MATERIAL_FORBIDDEN",
    "OBS_MANAGEMENT_INSPECT",
    "OBS_INDEX_PROFILE_READ",
    "MGA_CLEANUP_INSPECT",
    "OBS_MANAGEMENT_CONTROL",
    "MGA_CLEANUP_CONTROL",
    "OBS_CONFIG_CONTROL",
    "SUPPORT_EXPORT",
    "bundle_scope",
    "retention_policy_ref",
    "redaction_profile_ref",
    "authority_path",
    "audit_envelope_ref",
    "flush_required_before_export",
    "physical_path",
    "unsafe_payload",
    "<redacted>",
    "no-cluster",
    "cluster-enabled-stub",
    "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
    "index.validate",
    "index.analyze",
    "index.backlog",
    "index.rebuild",
    "index.repair",
    "index.cleanup_mga_versions",
    "index.optimization_control",
    "ordered_table_candidate_set",
    "secondary_delta_ledger",
    "page_extent_summary",
    "time_range_summary",
    "shadow_index_build_state",
    "inverted_search_segment_state",
    "vector_generation_state",
    "DPC.OBSERVABILITY.NON_AUTHORITATIVE_INPUT_REFUSED",
    "MGA.BOUNDARY.ENGINE_OWNS_FINALITY",
    "CDP.USER_OBSERVABILITY_SURFACE.INVALID_SNAPSHOT",
    "AGENT.ZERO_GREY.DIAGNOSTIC_REQUIRED",
    "fd_count",
    "rss_kib",
    "thread_count",
    "database_tree_bytes",
    "foreground_p95_millis",
    "clean_shutdown",
    "read_only_reopen_classification",
    "SELECT",
    "FROM sys.management.performance_optimization_surface",
    "FROM sys.management.performance_optimization_config",
    "FROM sys.management.authorization_decisions",
    "SHOW MANAGEMENT",
    "CALL sys.management.index_validate",
    "CALL sys.management.index_backlog",
    "CALL sys.management.index_repair",
    "CALL sys.management.prepare_support_bundle",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root)
    errors: list[str] = []
    chunks: list[str] = []
    for rel_path in REQUIRED_FILES:
        try:
            chunks.append((repo_root / rel_path).read_text(encoding="utf-8"))
        except FileNotFoundError:
            errors.append(f"missing product documentation: {rel_path}")
    text = "\n".join(chunks)

    for token in REQUIRED_TOKENS:
        if token not in text:
            errors.append(f"{DOC_ROOT} docs missing required token: {token}")

    forbidden_runtime_dependencies = (
        "/".join(("docs", "execution-plans")),
        "/".join(("docs", "completed-execution-plans")),
        "TRACKER" + ".csv",
        "ACCEPTANCE" + "_GATES.csv",
        "DEPENDENCIES" + ".csv",
        "SPEC_IMPLEMENTATION" + "_AUDIT_MATRIX.csv",
        "FINAL_AUDIT" + ".md",
    )
    for token in forbidden_runtime_dependencies:
        if token in text:
            errors.append(f"{DOC_ROOT} docs must not depend on execution_plan token: {token}")

    forbidden_internal_instructions = (
        "grep the test",
        "inspect dpc_",
        "look at c++ tests",
    )
    lowered = text.lower()
    for token in forbidden_internal_instructions:
        if token in lowered:
            errors.append(
                f"{DOC_ROOT} docs contain internal-test-only instruction: {token}"
            )

    if "parser_finality_authority` | bool | Must be `false`" not in text:
        errors.append("parser non-authority row must be explicit")
    if "wal_recovery_authority` | bool | Must be `false`" not in text:
        errors.append("WAL non-authority row must be explicit")
    if "Mutation" in text and "mutating" not in text:
        errors.append("mutating controls must preserve stable mutating field")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("DPC_OPERATOR_UI_DOCUMENTATION_GATE=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
