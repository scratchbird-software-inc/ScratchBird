#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Project-local foundation gates for engine/listener enterprise closure."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import sys
from typing import Any, Iterable
import xml.etree.ElementTree as ET


# ENGINE_LISTENER_ENTERPRISE_SUITE

DOCS_PATH = "docs" + "/"
PUBLIC_PARSER_HANDOFF_PREFIX = (
    "fixture://execution-plans/driver-parser-sblr-sbsql-readiness-closure/"
)
PUBLIC_SBSQL_HANDOFF_PREFIX = (
    "fixture://execution-plans/sbsql-per-element-contract-completion/"
)

FORBIDDEN_PROOF_REFERENCE_FRAGMENTS = (
    DOCS_PATH + "execution-plans",
    DOCS_PATH + "completed-execution-plans",
    DOCS_PATH + "findings",
    DOCS_PATH + "audit",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
)

SKIP_OR_WAIVER_TOKENS = (
    "DISABLED TRUE",
    "WILL_FAIL TRUE",
    "SKIP_REGULAR_EXPRESSION",
    "LABELS \"skip",
    "LABELS \"xfail",
)

FORBIDDEN_CTEST_RESULT_PROPERTIES = (
    "DISABLED",
    "WILL_FAIL",
    "SKIP_REGULAR_EXPRESSION",
    "SKIP_RETURN_CODE",
)

FORBIDDEN_COMPLETION_RESULT_LABELS = {
    "disabled",
    "skip",
    "skipped",
    "xfail",
    "waiver",
    "waived",
    "not-run",
    "not_run",
    "fixture-only",
    "fixture_only",
    "platform-excluded",
    "platform_excluded",
    "unsupported-profile",
    "unsupported_profile",
    "unsupported-only",
    "unsupported_only",
}

PUBLIC_CONTRACT_SNAPSHOT = "public_contract_snapshot"

PUBLIC_PROFILE_DECLARED_GATE_ALIASES: dict[str, tuple[str, ...]] = {
    "dpc_soak_leak_resource_stability_gate": (
        "public_release_soak_gate",
        "agent_enterprise_soak_performance_gate",
        "engine_listener_support_bundle_conformance",
        "engine_listener_owner_lifecycle_artifact_conformance",
    ),
    "database_lifecycle_upgrade_migration_conformance": (
        "public_upgrade_migration_gate",
    ),
    "database_lifecycle_protocol_versioning_conformance": (
        "sbmn_manager_protocol_unit_tests",
        "engine_listener_dbbt_lpreface_binding_conformance",
    ),
    "database_lifecycle_admin_cli_conformance": (
        "engine_listener_management_envelope_conformance",
        "public_admin_runbook_gate",
    ),
    "database_lifecycle_observability_conformance": (
        "public_observability_schema_gate",
    ),
    "database_lifecycle_supportability_evidence_conformance": (
        "engine_listener_support_bundle_conformance",
        "public_observability_schema_gate",
    ),
    "dpc_management_observability_support_bundle_gate": (
        "public_observability_schema_gate",
        "engine_listener_support_bundle_conformance",
        "ceic_040_index_operation_metrics_support_bundle_gate",
    ),
}

SOAK_PUBLIC_PROFILE_GATE_ALIASES: dict[str, tuple[str, ...]] = {
    "dpc_soak_leak_resource_stability_gate": (
        "public_release_soak_gate",
        "agent_enterprise_soak_performance_gate",
    ),
}

COMPATIBILITY_PUBLIC_PROFILE_GATE_ALIASES: dict[str, tuple[str, ...]] = {
    "database_lifecycle_upgrade_migration_conformance": (
        "public_upgrade_migration_gate",
    ),
    "database_lifecycle_protocol_versioning_conformance": (
        "sbmn_manager_protocol_unit_tests",
        "engine_listener_dbbt_lpreface_binding_conformance",
    ),
}

OPERATIONAL_PUBLIC_PROFILE_GATE_ALIASES: dict[str, tuple[str, ...]] = {
    "database_lifecycle_admin_cli_conformance": (
        "engine_listener_management_envelope_conformance",
        "public_admin_runbook_gate",
    ),
    "database_lifecycle_observability_conformance": (
        "public_observability_schema_gate",
    ),
    "database_lifecycle_shutdown_conformance": (
        "engine_listener_graceful_drain_stop_conformance",
    ),
    "database_lifecycle_maintenance_repair_conformance": (
        "public_admin_runbook_gate",
        "public_observability_schema_gate",
    ),
    "dpc_cleanup_backup_restore_repair_diagnostics_gate": (
        "public_admin_runbook_gate",
        "public_observability_schema_gate",
    ),
    "dpc_index_validation_repair_tooling_gate": (
        "ceic_040_index_operation_metrics_support_bundle_gate",
        "public_admin_runbook_gate",
    ),
    "database_lifecycle_config_policy_security_provider_conformance": (
        "public_config_policy_migration_gate",
        "engine_listener_management_envelope_conformance",
    ),
    "dpc_management_observability_support_bundle_gate": (
        "public_observability_schema_gate",
        "engine_listener_support_bundle_conformance",
        "ceic_040_index_operation_metrics_support_bundle_gate",
    ),
    "database_lifecycle_supportability_evidence_conformance": (
        "engine_listener_support_bundle_conformance",
        "public_observability_schema_gate",
    ),
    "database_lifecycle_bounded_stress_resource_leak_conformance": (
        "engine_listener_owner_lifecycle_artifact_conformance",
        "engine_listener_support_bundle_conformance",
    ),
    "dpc_soak_leak_resource_stability_gate": (
        "engine_listener_owner_lifecycle_artifact_conformance",
        "engine_listener_support_bundle_conformance",
    ),
    "cdp_user_observability_surface_gate": (
        "public_observability_schema_gate",
    ),
    "dpc_authoritative_cleanup_horizon_service_gate": (
        "public_observability_schema_gate",
    ),
    "dpc_plan_stability_statistics_lifecycle_gate": (
        "public_observability_schema_gate",
    ),
}

RELEASE_PROOF_SCOPE_LABELS = {
    "engine_listener_enterprise",
    "sblr_surface",
}

SBLR_TRACEABILITY_FIXTURE_ROOT = Path(
    "tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03"
)

SBLR_TRACEABILITY_CSV_FILES = (
    "REFERENCE_GAP_SUMMARY.csv",
    "REFERENCE_INTERNAL_META_OPCODE_CLEANUP_MATRIX.csv",
    "EXPLICIT_UNSUPPORTED_SURFACE_MATRIX.csv",
    "IMPLEMENTATION_EXECUTION_PLAN_SEED_MATRIX.csv",
    "NON_DIRECT_FUNCTION_SURFACE_MATRIX.csv",
    "SBLR_STALE_DEFERRED_ALIAS_CLEANUP_MATRIX.csv",
    "SBSQL_SBLR_FAMILY_RECONCILIATION_MATRIX.csv",
    "SERVER_AUTHORITY_ROLLUP.csv",
    "SERVER_AUTHORITY_SURFACE_MATRIX.csv",
)

SBLR_TRACEABILITY_JSON_FILES = (
    "sblr_primary_family_snapshot.json",
    "sblr_surface_traceability_manifest.json",
    "sbsql_sync_requirements.json",
)

SBSQL_SYNC_REQUIRED_FIELDS = (
    "sbsql_cst_reference",
    "ast_sblr_reference",
    "reference_normalization_reference",
    "parser_execution_plan_sync_reference",
)

SBSQL_NATIVE_STYLE = "sbsql_native_normalized"
SBSQL_ACTIVE_EXECUTION_PLAN_PREFIX = PUBLIC_PARSER_HANDOFF_PREFIX
SBSQL_SUPERSEDED_EXECUTION_PLAN_PREFIXES = (PUBLIC_SBSQL_HANDOFF_PREFIX,)
EXPECTED_PRIMARY_SBLR_FAMILY_ROWS = 54
EXPECTED_SBLR_OPCODE_ROWS = 217
EXPECTED_PARSER_CONTRACT_MINIMUM_ROWS = 1300
SBSQL_FORBIDDEN_STYLE_TOKENS = (
    "compatibility_dialect_paste_through",
    "reference dialect paste",
    "paste-through",
    "paste_through",
    "paste through",
)

GENERATED_RESOURCE_DETERMINISM_MANIFEST = Path(
    "tests/engine_listener_enterprise/fixtures/generated_resource_determinism_manifest.json"
)

PARSER_FACING_CONTRACT_FREEZE_MANIFEST = Path(
    "tests/engine_listener_enterprise/fixtures/parser_facing_contract_freeze_manifest.json"
)

PARSER_CONTRACT_REQUIRED_SECTIONS = (
    "sblr_envelope_families",
    "sblr_opcode_bindings",
    "server_admission_rules",
    "server_dispatch_routes",
    "reference_route_classifications",
    "unsupported_surface_vectors",
    "sbsql_sync_status",
    "compatibility_version",
)

PARSER_CONTRACT_ROW_FIELDS = (
    "contract_row_id",
    "freeze_id",
    "freeze_version",
    "compatibility_version",
    "section",
    "source_id",
    "source_kind",
    "published_value",
    "diagnostic_vector",
    "route_classification",
    "source_sha256",
    "evidence_artifact",
    "freeze_status",
    "public_release_safe",
    "product_completion_claim",
)

CRASH_FAULT_CAMPAIGN_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "storage_offset_io",
        "fault_surface": "platform_io",
        "critical_transition": "file_create_write_sync_close_reopen_and_offset_overflow",
        "expected_result": "new_state_survives_or_overflow_refuses_fail_closed",
        "gate": "engine_listener_storage_io_conformance",
        "mga_authority": "not_applicable_storage_sync_only",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_storage_io_conformance.cpp": (
                "FileDeviceDurableCreateSyncCloseAndReadOnlyRefusal",
                "durable sync failed",
                "exclusive owner lock was not cleaned up on close",
                "SB-STORAGE-DISK-EXTENT-OVERFLOW",
            ),
        },
    },
    {
        "row_id": "filespace_growth_crash_window",
        "fault_surface": "filespace_growth",
        "critical_transition": "physical_extension_before_metadata_commit",
        "expected_result": "metadata_only_or_interrupted_growth_fails_closed",
        "gate": "engine_listener_filespace_growth_conformance",
        "mga_authority": "filespace_lifecycle_authority_required",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_filespace_growth_conformance.cpp": (
                "CrashWindowAndLegacyMetadataOnlyRowsFailClosed",
                "interrupted growth header/file mismatch was accepted",
                "legacy metadata-only growth did not fail closed",
                "FilespacePhysicalGrowthRecoveryAction::fail_closed",
            ),
        },
    },
    {
        "row_id": "filespace_manifest_atomic_publish",
        "fault_surface": "filespace_manifest",
        "critical_transition": "temp_manifest_write_checksum_rename_parent_sync",
        "expected_result": "new_manifest_survives_or_tamper_refuses_fail_closed",
        "gate": "engine_listener_filespace_registry_manifest_conformance",
        "mga_authority": "not_transaction_finality_authority",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_filespace_registry_manifest_conformance.cpp": (
                "filespace manifest did not report atomic rename",
                "filespace manifest did not report parent sync",
                "filespace stale temp removal was not reported",
                "tampered filespace manifest loaded successfully",
            ),
        },
    },
    {
        "row_id": "physical_mga_cow_visibility",
        "fault_surface": "mga_cow_row_pages",
        "critical_transition": "unpublished_commit_rollback_delete_reopen",
        "expected_result": "visibility_derives_from_durable_transaction_inventory",
        "gate": "engine_listener_physical_mga_cow_conformance",
        "mga_authority": "durable_transaction_inventory",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_physical_mga_cow_conformance.cpp": (
                "unpublished insert must not be visible after reopen",
                "committed insert should be visible",
                "rolled back update must not replace committed version",
                "all physical row versions should reopen from the row page",
            ),
        },
    },
    {
        "row_id": "transaction_inventory_publish_journal",
        "fault_surface": "transaction_inventory",
        "critical_transition": "publish_journal_primary_inventory_corruption_and_partial_write",
        "expected_result": "old_or_new_snapshot_recovers_or_recovery_required_or_checksum_fail_closed",
        "gate": "transaction_inventory_publish_fault_conformance",
        "mga_authority": "durable_transaction_inventory",
        "evidence_files": {
            "tests/fault_injection/transaction_inventory_publish_fault_conformance.cpp": (
                "TestCommittedJournalRecoversNewSnapshot",
                "TestPublishingJournalRecoversOldSnapshot",
                "SB-TXN-INVENTORY-PUBLISH-RECOVERY-REQUIRED",
                "SB-TXN-INVENTORY-PUBLISH-JOURNAL-CHECKSUM-MISMATCH",
            ),
        },
    },
    {
        "row_id": "toast_overflow_blob_pages",
        "fault_surface": "toast_overflow",
        "critical_transition": "blob_page_write_reopen_corruption_reclaim",
        "expected_result": "reachable_values_read_after_reopen_and_corruption_or_non_authority_refuses",
        "gate": "engine_listener_overflow_blob_page_persistence_conformance",
        "mga_authority": "authoritative_cleanup_horizon_required_for_reclaim",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_overflow_blob_page_persistence_conformance.cpp": (
                "overflow blob pages did not read after reopen",
                "corrupted overflow blob payload was admitted",
                "corrupted overflow blob metadata was admitted",
                "blob page device reopen for refused reclaim failed",
                "authoritative overflow reclaim failed",
            ),
        },
    },
    {
        "row_id": "index_delta_recovery_classification",
        "fault_surface": "index_maintenance",
        "critical_transition": "delta_ledger_encode_reopen_recovery_classification",
        "expected_result": "index_evidence_reopens_but_mga_remains_finality_authority",
        "gate": "engine_listener_index_family_dml_route_conformance",
        "mga_authority": "durable_mga_inventory_not_index_evidence",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_index_family_dml_route_conformance.cpp": (
                "ledger family encode failed",
                "ledger family reopen decode changed mutation records",
                "ledger family recovery classification did not preserve MGA authority",
                "fail_closed",
            ),
        },
    },
    {
        "row_id": "domain_binary_catalog_replay",
        "fault_surface": "domain_catalog",
        "critical_transition": "binary_catalog_temp_residue_commit_reload_tamper",
        "expected_result": "committed_catalog_survives_and_tamper_fails_closed",
        "gate": "engine_listener_domain_binary_catalog_conformance",
        "mga_authority": "durable_domain_catalog_with_transaction_binding",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_domain_binary_catalog_conformance.cpp": (
                "stale temp file blocked committed catalog load",
                "stale temp file changed catalog contents",
                "tampered domain catalog did not fail closed",
                "digest_mismatch",
            ),
        },
    },
    {
        "row_id": "protected_material_catalog_replay",
        "fault_surface": "security_protected_material",
        "critical_transition": "catalog_temp_cleanup_reload_redaction_tamper",
        "expected_result": "protected_material_catalog_survives_and_tamper_fails_closed",
        "gate": "engine_listener_protected_material_durable_catalog_conformance",
        "mga_authority": "security_catalog_authority_not_transaction_finality",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_protected_material_durable_catalog_conformance.cpp": (
                "stale protected material catalog temp residue survived mutation",
                "protected material catalog temp file survived initial create",
                "wrong protected material release purpose did not fail closed",
                "tampered protected material catalog did not fail closed",
            ),
        },
    },
    {
        "row_id": "listener_owner_lifecycle_recovery",
        "fault_surface": "listener_owner_lifecycle",
        "critical_transition": "owner_token_atomic_write_lock_tamper_stale_recovery",
        "expected_result": "live_owner_refuses_takeover_and_stale_or_corrupt_artifact_recovers",
        "gate": "engine_listener_owner_lifecycle_artifact_conformance",
        "mga_authority": "not_applicable_listener_control_plane",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_owner_lifecycle_artifact_conformance.cpp": (
                "atomic_write=true",
                "valid live owner token did not block a second owner",
                "tampered owner token did not permit stale/corrupt recovery",
                "owner token was not re-signed after stale/corrupt recovery",
            ),
        },
    },
    {
        "row_id": "listener_management_handoff_refusal",
        "fault_surface": "listener_management",
        "critical_transition": "management_envelope_tamper_replay_and_handoff_binding_refusal",
        "expected_result": "tampered_or_replayed_management_frame_refuses_fail_closed",
        "gate": "engine_listener_management_envelope_conformance",
        "mga_authority": "not_applicable_listener_control_plane",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_management_envelope_conformance.cpp": (
                "tampered hmac-sha256 SBME envelope must be refused",
                "tampered hmac-sha256 SBME envelope must report authenticator failure",
                "LISTENER.MANAGEMENT.AUTHENTICATOR_INVALID",
                "LISTENER.MANAGEMENT.REPLAY_DETECTED",
            ),
        },
    },
    {
        "row_id": "listener_support_bundle_handoff_fault",
        "fault_surface": "listener_support_bundle",
        "critical_transition": "handoff_failure_auth_refusal_drain_support_bundle",
        "expected_result": "support_bundle_records_bounded_faults_without_authority_or_secret_leak",
        "gate": "engine_listener_support_bundle_conformance",
        "mga_authority": "support_bundle_evidence_only",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_support_bundle_conformance.cpp": (
                "support_bundle_is_authority",
                "handoff_failures",
                "raw support bundle command must be refused",
                "invalid DBBT_VALIDATE should fail and record auth refusal",
            ),
        },
    },
    {
        "row_id": "public_cross_cutting_crash_matrix",
        "fault_surface": "public_release_crash_matrix",
        "critical_transition": "public_pcr_115_crash_fault_matrix_and_gate",
        "expected_result": "public_matrix_rows_cover_crash_reopen_fault_injection_mga_authority_fail_closed",
        "gate": "public_crash_fault_matrix",
        "mga_authority": "public_matrix_mga_authority_drift_refused",
        "evidence_files": {
            "tests/fault_injection/public_crash_fault_matrix.py": (
                "PUBLIC_CRASH_FAULT_MATRIX",
                "faults_fail_closed",
                "mga_authority_drift_refused",
                "crash_reopen_rows_required",
            ),
            "tests/release/public_crash_fault_gate.py": (
                "PUBLIC_CRASH_FAULT_GATE",
                "matrix_row_coverage_incomplete",
                "public_crash_fault_matrix",
                "agent_fault_injection_gate",
            ),
        },
    },
)

INTEGRATED_PRODUCT_PROOF_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "listener_management_startup_drain_auth",
        "product_phase": "listener_startup_management_drain",
        "gate": "engine_listener_management_envelope_conformance",
        "authority": "authenticated_listener_management",
        "expected_result": "live_listener_accepts_authenticated_management_and_refuses_replay_or_unauthorized_frames",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_management_envelope_conformance.cpp": (
                "hmac-sha256 DRAIN envelope must succeed with listener DBBT key material",
                "DBBT_VALIDATE must pass through authenticated SBME management path",
                "LISTENER.MANAGEMENT.REPLAY_DETECTED",
                "tampered hmac-sha256 SBME envelope must be refused",
            ),
        },
    },
    {
        "row_id": "listener_parser_handoff_exact_binding",
        "product_phase": "parser_handoff_binding",
        "gate": "engine_listener_dbbt_lpreface_binding_conformance",
        "authority": "dbbt_lpreface_exact_nonce_binding",
        "expected_result": "parser_handoff_uses_exact_client_server_nonce_pair_and_refuses_fifo_theft",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_dbbt_lpreface_binding_conformance.cpp": (
                "valid LPREFACE claim line must populate both nonces",
                "out-of-order claim must not steal the first FIFO binding",
                "matching only the client nonce must not authorize a DBBT/LPREFACE binding",
                "expired binding must not be claimable even with matching evidence",
            ),
        },
    },
    {
        "row_id": "listener_support_bundle_authenticated_redacted",
        "product_phase": "listener_support_bundle_recovery_evidence",
        "gate": "engine_listener_support_bundle_conformance",
        "authority": "support_bundle_evidence_only",
        "expected_result": "support_bundle_is_authenticated_bounded_redacted_and_non_authoritative",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_support_bundle_conformance.cpp": (
                "support_bundle_is_authority",
                "parser worker fault history must be bounded and source-counted",
                "handoff failure summary missing",
                "invalid DBBT_VALIDATE should fail and record auth refusal",
            ),
        },
    },
    {
        "row_id": "sblr_engine_create_open_transaction_commit_reopen",
        "product_phase": "engine_create_open_sblr_commit_reopen",
        "gate": "public_sblr_uuid_mga_route_integration_gate",
        "authority": "engine_sblr_dispatch_mga_inventory",
        "expected_result": "database_create_sblr_begin_plan_authorize_commit_reopen_inventory_and_cluster_refusal_share_engine_authority",
        "evidence_files": {
            "tests/release/public_sblr_uuid_mga_route_integration_gate.cpp": (
                "CreateDatabaseFile",
                "SBLR_TRANSACTION_BEGIN",
                "SBLR_TRANSACTION_COMMIT",
                "committed transaction was not recorded in MGA inventory",
            ),
        },
    },
    {
        "row_id": "engine_sblr_admission_auth_optimizer_cluster_boundary",
        "product_phase": "sblr_auth_optimizer_cluster_boundary",
        "gate": "public_sblr_uuid_mga_route_integration_gate",
        "authority": "parser_boundary_and_materialized_authorization",
        "expected_result": "sblr_refuses_sql_text_and_unresolved_names_then_dispatches_auth_optimizer_and_cluster_fail_closed",
        "evidence_files": {
            "tests/release/public_sblr_uuid_mga_route_integration_gate.cpp": (
                "SB_SBLR_SQL_TEXT_FORBIDDEN",
                "SB_SBLR_NAMES_NOT_RESOLVED_TO_UUIDS",
                "materialized_authorization_context",
                "cluster SBLR route did not fail closed at provider boundary",
            ),
        },
    },
    {
        "row_id": "engine_materialized_authorization_deny_epoch",
        "product_phase": "engine_authentication_authorization",
        "gate": "engine_listener_materialized_authorization_conformance",
        "authority": "durable_materialized_authorization_context",
        "expected_result": "explicit_deny_overrides_allow_and_stale_authorization_epoch_fails_closed",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_materialized_authorization_conformance.cpp": (
                "EngineAuthorize did not accept materialized SELECT grant",
                "authorization authority evidence missing",
                "EngineAuthorize accepted stale materialized authorization context",
                "explicit materialized deny did not override allow",
            ),
        },
    },
    {
        "row_id": "engine_mga_cow_row_visibility_reopen",
        "product_phase": "mga_cow_insert_update_delete_visibility",
        "gate": "engine_listener_physical_mga_cow_conformance",
        "authority": "durable_transaction_inventory",
        "expected_result": "row_page_versions_reopen_but_visibility_derives_from_committed_inventory",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_physical_mga_cow_conformance.cpp": (
                "unpublished insert must not be visible after reopen",
                "rolled back update should reopen",
                "all physical row versions should reopen from the row page",
                "physical_mga_cow.read_visibility_authority=durable_transaction_inventory",
            ),
        },
    },
    {
        "row_id": "engine_toast_overflow_reopen_reclaim",
        "product_phase": "toast_overflow_page_storage",
        "gate": "engine_listener_overflow_blob_page_persistence_conformance",
        "authority": "mga_cleanup_horizon_for_reclaim",
        "expected_result": "large_values_reopen_from_blob_pages_and_reclaim_requires_authoritative_cleanup",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_overflow_blob_page_persistence_conformance.cpp": (
                "overflow blob pages did not read after reopen",
                "corrupted overflow blob payload was admitted",
                "corrupted overflow blob metadata was admitted",
                "non-authoritative overflow reclaim was admitted",
                "authoritative overflow reclaim failed",
            ),
        },
    },
    {
        "row_id": "engine_index_maintenance_recovery",
        "product_phase": "index_maintenance_and_recovery",
        "gate": "engine_listener_index_family_dml_route_conformance",
        "authority": "index_evidence_not_transaction_finality",
        "expected_result": "index_dml_mutates_physical_tree_or_delta_ledger_while_mga_remains_finality_authority",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_index_family_dml_route_conformance.cpp": (
                "ordered family insert did not mutate physical tree",
                "ledger family encode failed",
                "ledger family recovery classification did not preserve MGA authority",
                "verify maintenance route refused accepted family",
            ),
        },
    },
    {
        "row_id": "engine_optimizer_physical_execution_route",
        "product_phase": "optimizer_plan_selection_and_execution_route",
        "gate": "engine_listener_optimizer_integrated_route_conformance",
        "authority": "catalog_backed_optimizer_evidence_only",
        "expected_result": "optimizer_selects_catalog_backed_physical_btree_plan_and_executor_consumes_real_locator_stream",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_optimizer_integrated_route_conformance.cpp": (
                "catalog-backed production optimizer route was not admitted",
                "optimizer did not select scalar B-tree lookup route",
                "executor row-locator stream did not consume physical B-tree route",
                "parser_reference_placeholder_cluster_refused",
            ),
        },
    },
    {
        "row_id": "engine_transaction_publish_fault_recovery",
        "product_phase": "commit_crash_recovery",
        "gate": "transaction_inventory_publish_fault_conformance",
        "authority": "durable_transaction_inventory_publish_journal",
        "expected_result": "commit_publish_faults_recover_old_or_new_inventory_or_fail_closed",
        "evidence_files": {
            "tests/fault_injection/transaction_inventory_publish_fault_conformance.cpp": (
                "TestCommittedJournalRecoversNewSnapshot",
                "TestPublishingJournalRecoversOldSnapshot",
                "SB-TXN-INVENTORY-PUBLISH-RECOVERY-REQUIRED",
                "SB-TXN-INVENTORY-PUBLISH-JOURNAL-CHECKSUM-MISMATCH",
            ),
        },
    },
    {
        "row_id": "integrated_crash_fault_support_bundle_rollup",
        "product_phase": "crash_fault_supportability_rollup",
        "gate": "engine_listener_crash_fault_campaign_gate",
        "authority": "generated_project_test_crash_matrix",
        "expected_result": "cross_cutting_crash_fault_matrix_is_registered_public_safe_and_non_authoritative",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py": (
                "ELER_080_CROSS_CUTTING_CRASH_FAULT_CAMPAIGN_MATRIX",
                "validate_crash_fault_campaign",
                "support_bundle_evidence_only",
                "durable_transaction_inventory",
            ),
        },
    },
)

CRASH_RECOVERY_CERTIFICATION_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "platform_header_write_sync_close",
        "critical_transition": "before_header_write_after_sync_close_reopen",
        "recovery_class": "new_state_survives",
        "gate": "engine_listener_storage_io_conformance",
        "authority": "platform_file_sync_and_checked_offsets",
        "expected_result": "synced_payload_survives_and_overflow_or_read_only_mutation_refuses_fail_closed",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_storage_io_conformance.cpp": (
                "durable sync failed",
                "synced payload was not preserved",
                "read-only write was accepted",
                "overflowing page header read was accepted",
            ),
        },
    },
    {
        "row_id": "filespace_growth_header_mismatch",
        "critical_transition": "after_data_file_extension_before_physical_header_commit",
        "recovery_class": "corruption_detected_fail_closed",
        "gate": "engine_listener_filespace_growth_conformance",
        "authority": "physical_filespace_header_and_growth_ledger",
        "expected_result": "interrupted_or_legacy_metadata_only_growth_is_detected_and_fails_closed",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_filespace_growth_conformance.cpp": (
                "physical extension not synced",
                "physical header not updated",
                "interrupted growth header/file mismatch was accepted",
                "legacy metadata-only growth did not fail closed",
            ),
        },
    },
    {
        "row_id": "filespace_manifest_atomic_publish",
        "critical_transition": "temp_manifest_write_file_sync_rename_parent_sync",
        "recovery_class": "new_state_survives",
        "gate": "engine_listener_filespace_registry_manifest_conformance",
        "authority": "manifest_generation_checksum_writer_identity",
        "expected_result": "manifest_generation_survives_round_trip_and_tamper_or_stale_temp_fails_closed",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_filespace_registry_manifest_conformance.cpp": (
                "filespace manifest did not report file sync",
                "filespace manifest did not report parent sync",
                "filespace header generation did not survive manifest round trip",
                "ELER-013 durable temp workspace manifest proof",
            ),
        },
    },
    {
        "row_id": "transaction_inventory_partial_publish",
        "critical_transition": "during_transaction_commit_inventory_publish_journal",
        "recovery_class": "recovery_required_detected",
        "gate": "transaction_inventory_publish_fault_conformance",
        "authority": "durable_transaction_inventory_publish_journal",
        "expected_result": "committed_new_or_publishing_old_snapshot_recovers_and_partial_or_tampered_journal_requires_recovery",
        "evidence_files": {
            "tests/fault_injection/transaction_inventory_publish_fault_conformance.cpp": (
                "TestCommittedJournalRecoversNewSnapshot",
                "TestPublishingJournalRecoversOldSnapshot",
                "partial publish journal was accepted after primary corruption",
                "partial publish journal did not return recovery-required",
            ),
        },
    },
    {
        "row_id": "mga_cow_unpublished_and_rollback_reopen",
        "critical_transition": "after_row_page_write_before_transaction_inventory_commit",
        "recovery_class": "old_state_survives",
        "gate": "engine_listener_physical_mga_cow_conformance",
        "authority": "durable_transaction_inventory_not_row_page_finality",
        "expected_result": "unpublished_and_rolled_back_versions_reopen_as_evidence_but_remain_invisible",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_physical_mga_cow_conformance.cpp": (
                "unpublished insert must not be visible after reopen",
                "rolled back update should reopen",
                "physical_mga_cow.row_page_finality_authority=false",
                "physical_mga_cow.read_visibility_authority=durable_transaction_inventory",
            ),
        },
    },
    {
        "row_id": "savepoint_partial_undo_replay",
        "critical_transition": "during_rollback_to_savepoint_after_partial_physical_undo",
        "recovery_class": "recovery_required_detected",
        "gate": "engine_listener_savepoint_physical_undo_conformance",
        "authority": "durable_savepoint_undo_ledger",
        "expected_result": "partial_undo_requires_replay_before_rollback_is_marked_complete",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_savepoint_physical_undo_conformance.cpp": (
                "ELER-023 executor failure should report partial durable undo",
                "ELER-023 stack must require replay before marking rollback applied",
                "ELER-023 crash replay undo should be idempotent",
                "ELER-023 missing undo evidence should fail closed before executor",
            ),
        },
    },
    {
        "row_id": "index_delta_ledger_recovery",
        "critical_transition": "during_index_delta_merge_or_reopen_classification",
        "recovery_class": "write_admission_fenced",
        "gate": "engine_listener_index_family_dml_route_conformance",
        "authority": "durable_mga_inventory_not_index_evidence",
        "expected_result": "index_delta_reopens_for_repair_or_overlay_but_cannot_be_transaction_finality",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_index_family_dml_route_conformance.cpp": (
                "ledger family encode failed",
                "ledger family reopen decode changed mutation records",
                "ledger family recovery classification did not preserve MGA authority",
                "ordered family update incorrectly selected deferred ledger route",
            ),
        },
    },
    {
        "row_id": "toast_page_allocation_corruption_reclaim",
        "critical_transition": "during_toast_page_allocation_reopen_corruption_and_reclaim",
        "recovery_class": "corruption_detected_fail_closed",
        "gate": "engine_listener_overflow_blob_page_persistence_conformance",
        "authority": "overflow_pages_reachable_until_mga_cleanup_horizon",
        "expected_result": "reachable_blob_pages_reopen_corruption_refuses_and_reclaim_requires_authoritative_cleanup",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_overflow_blob_page_persistence_conformance.cpp": (
                "overflow blob pages did not read after reopen",
                "corrupted overflow blob payload was admitted",
                "corrupted overflow blob metadata was admitted",
                "non-authoritative overflow reclaim was admitted",
                "authoritative overflow reclaim failed",
            ),
        },
    },
    {
        "row_id": "cleanup_reclaim_authority_fence",
        "critical_transition": "during_cleanup_reclaim_without_authoritative_horizon",
        "recovery_class": "write_admission_fenced",
        "gate": "engine_listener_mga_integrated_physical_cleanup_conformance",
        "authority": "authoritative_mga_cleanup_sweep_required",
        "expected_result": "cleanup_reclaim_mutates_all_surfaces_only_from_one_authoritative_mga_horizon",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_mga_integrated_physical_cleanup_conformance.cpp": (
                "ELER-024 cleanup horizon was not authoritative",
                "ELER-024 surfaces did not use the sweep cleanup horizon",
                "ELER-024 non-authoritative cleanup did not fail closed",
                "ELER-024 refused cleanup mutated an evidence surface",
            ),
        },
    },
    {
        "row_id": "agent_action_replay_recovery",
        "critical_transition": "during_agent_action_replay_and_lease_recovery",
        "recovery_class": "recovery_required_detected",
        "gate": "engine_listener_agent_production_action_conformance",
        "authority": "durable_agent_catalog_and_replay_records",
        "expected_result": "pending_agent_action_and_lease_reopen_as_replay_pending_with_evidence",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_agent_production_action_conformance.cpp": (
                "ELER-042 crash recovery failed",
                "ELER-042 pending action did not become replay_pending",
                "ELER-042 crash recovery did not retain replay evidence",
                "ELER-042 idempotent replay called provider again",
            ),
        },
    },
    {
        "row_id": "listener_handoff_exact_binding_recovery",
        "critical_transition": "during_listener_handoff_claim_selection",
        "recovery_class": "write_admission_fenced",
        "gate": "engine_listener_dbbt_lpreface_binding_conformance",
        "authority": "dbbt_lpreface_nonce_pair_binding",
        "expected_result": "listener_never_binds_authenticated_preface_to_wrong_socket_or_expired_claim",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_dbbt_lpreface_binding_conformance.cpp": (
                "out-of-order claim must not steal the first FIFO binding",
                "matching only the client nonce must not authorize a DBBT/LPREFACE binding",
                "missing evidence refusal must not consume the binding",
                "expired binding must not be claimable even with matching evidence",
            ),
        },
    },
    {
        "row_id": "listener_management_drain_stop_recovery",
        "critical_transition": "during_management_stop_drain_with_active_parser_work",
        "recovery_class": "write_admission_fenced",
        "gate": "engine_listener_graceful_drain_stop_conformance",
        "authority": "listener_active_worker_accounting",
        "expected_result": "graceful_stop_times_out_without_force_keeps_drain_state_and_completes_after_active_work_finishes",
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_graceful_drain_stop_conformance.cpp": (
                "DRAIN did not publish bounded active-worker status",
                "draining listener did not emit structured refusal",
                "STOP GRACEFUL did not time out safely with active work",
                "STOP GRACEFUL did not complete after active work drained",
            ),
        },
    },
)

ADVERSARIAL_SECURITY_CATEGORY_GATES: dict[str, tuple[str, ...]] = {
    "auth_bypass": (
        "public_enterprise_threat_gate",
        "engine_listener_materialized_authorization_conformance",
        "public_authorization_durable_flow_gate",
        "public_security_durable_crypto_hardening_gate",
    ),
    "backup_archive_misuse": (
        "public_enterprise_threat_gate",
    ),
    "catalog_corruption": (
        "public_enterprise_threat_gate",
        "engine_listener_page_body_registry_conformance",
        "engine_listener_protected_material_durable_catalog_conformance",
        "public_security_provider_contract_protected_material_gate",
        "public_upgrade_migration_gate",
    ),
    "cluster_boundary_bypass": (
        "public_enterprise_threat_gate",
        "public_cluster_provider_handshake_gate",
        "public_sblr_uuid_mga_route_integration_gate",
    ),
    "downgrade_attack": (
        "public_enterprise_threat_gate",
        "engine_listener_tls_channel_binding_conformance",
        "public_upgrade_migration_gate",
    ),
    "handoff_race": (
        "public_enterprise_threat_gate",
        "engine_listener_dbbt_lpreface_binding_conformance",
    ),
    "malformed_database_open": (
        "public_enterprise_threat_gate",
        "engine_listener_page_body_registry_conformance",
        "public_page_body_checksum_agreement_gate",
    ),
    "management_socket_abuse": (
        "public_enterprise_threat_gate",
        "engine_listener_management_envelope_conformance",
    ),
    "parser_boundary_abuse": (
        "public_enterprise_threat_gate",
        "public_sblr_uuid_mga_route_integration_gate",
    ),
    "parser_worker_compromise": (
        "public_enterprise_threat_gate",
        "engine_listener_parser_pool_lifecycle_conformance",
    ),
    "policy_rollback": (
        "public_enterprise_threat_gate",
    ),
    "privilege_escalation": (
        "public_enterprise_threat_gate",
        "engine_listener_materialized_authorization_conformance",
        "engine_listener_protected_material_durable_catalog_conformance",
        "public_authorization_durable_flow_gate",
        "public_security_provider_contract_protected_material_gate",
    ),
    "recovery_authority_spoofing": (
        "public_enterprise_threat_gate",
    ),
    "replay_attack": (
        "public_enterprise_threat_gate",
        "engine_listener_management_envelope_conformance",
        "engine_listener_session_binding_takeover_conformance",
    ),
    "sblr_tampering": (
        "public_enterprise_threat_gate",
        "public_sblr_uuid_mga_route_integration_gate",
    ),
    "support_bundle_leakage": (
        "public_enterprise_threat_gate",
        "engine_listener_support_bundle_redaction_gate",
        "public_transaction_support_bundle_gate",
    ),
    "uuid_spoofing": (
        "public_enterprise_threat_gate",
        "public_sblr_uuid_mga_route_integration_gate",
    ),
}

FUZZ_PROPERTY_BOUNDARY_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "listener_control_plane_frame_fuzz",
        "boundary_surface": "listener_control_plane_frames",
        "boundary_kind": "listener_binary_text_control",
        "property_invariant": "malformed_or_oversized_control_frames_fail_closed_without_management_side_effect",
        "behavior_gates": (
            "sbmn_manager_protocol_fuzz_gate",
            "engine_listener_management_envelope_conformance",
        ),
        "evidence_files": {
            "tests/manager/protocol_fuzz_gate.cpp": (
                "DecodeControlPlaneMessage",
                "oversized_control",
                "Pattern(",
                "DecodeDbbt",
            ),
            "tests/engine_listener_enterprise/engine_listener_management_envelope_conformance.cpp": (
                "SendControlFrame",
                "ReadControlFrame",
                "HmacEnvelopePayload",
                "tamper",
            ),
        },
    },
    {
        "row_id": "listener_dbbt_lpreface_handoff_binding_property",
        "boundary_surface": "dbbt_lpreface_handoff",
        "boundary_kind": "listener_handoff_identity_binding",
        "property_invariant": "listener_never_hands_an_authenticated_preface_binding_to_the_wrong_socket",
        "behavior_gates": (
            "sbmn_manager_protocol_fuzz_gate",
            "engine_listener_dbbt_lpreface_binding_conformance",
        ),
        "evidence_files": {
            "tests/manager/protocol_fuzz_gate.cpp": (
                "DecodeDbbt",
                "DecodeLpreface",
                "DecodeLprefaceAck",
                "oversized_sbdb",
            ),
            "tests/engine_listener_enterprise/engine_listener_dbbt_lpreface_binding_conformance.cpp": (
                "out-of-order claim must not steal the first FIFO binding",
                "matching only the client nonce must not authorize a DBBT/LPREFACE binding",
                "malformed LPREFACE claim must fail closed",
                "reader must leave parser protocol bytes after the claim untouched",
            ),
        },
    },
    {
        "row_id": "listener_lifecycle_file_property",
        "boundary_surface": "listener_owner_lifecycle_files",
        "boundary_kind": "listener_lifecycle_file_codec",
        "property_invariant": "lifecycle_artifact_tamper_or_stale_owner_state_fails_closed_or_recovers_without_takeover",
        "behavior_gates": ("engine_listener_owner_lifecycle_artifact_conformance",),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_owner_lifecycle_artifact_conformance.cpp": (
                "atomic_write=true",
                "valid live owner token did not block a second owner",
                "tampered owner token did not permit stale/corrupt recovery",
                "owner token was not re-signed after stale/corrupt recovery",
            ),
        },
    },
    {
        "row_id": "durable_codec_fuzz_boundary",
        "boundary_surface": "durable_engine_codecs",
        "boundary_kind": "engine_binary_text_codec_fuzz",
        "property_invariant": "catalog_datatype_index_repair_archive_backup_and_cluster_codecs_reject_malformed_bounds_checksum_version_and_authority_inputs",
        "behavior_gates": (
            "public_durable_codec_fuzz",
            "public_codec_property_gate",
            "public_datatype_binary_descriptor_integrity_gate",
        ),
        "evidence_files": {
            "tests/fuzz/public_durable_codec_fuzz.py": (
                "PUBLIC_DURABLE_CODEC_FUZZ",
                "malformed_inputs_fail_closed",
                "authority_refusals_are_required",
                "surface_count",
            ),
            "tests/release/public_codec_property_gate.cpp": (
                "PUBLIC_CODEC_PROPERTY_GATE",
                "CatalogPageBodyProperties",
                "CatalogTypedRecordProperties",
                "ClusterCatalogRecordProperties",
            ),
            "tests/release/public_datatype_binary_descriptor_integrity_gate.cpp": (
                "EncodeDatatypeDescriptorEnvelope",
                "DecodeDatatypeDescriptorEnvelope",
                "accepted corrupted payload",
                "accepted weak integrity profile",
            ),
        },
    },
    {
        "row_id": "page_catalog_transaction_inventory_property",
        "boundary_surface": "page_catalog_transaction_inventory_records",
        "boundary_kind": "engine_page_and_inventory_codec_property",
        "property_invariant": "page_finality_never_becomes_transaction_finality_and_transaction_inventory_remains_durable_authority",
        "behavior_gates": (
            "engine_listener_page_body_registry_conformance",
            "public_page_body_checksum_agreement_gate",
            "public_transaction_inventory_lock_table_gate",
            "transaction_inventory_publish_fault_conformance",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_page_body_registry_conformance.cpp": (
                "ProveStructuredBody",
                "ProveSpecificCodecs",
                "ProveCorruptionRefusal",
                "ValidatePageBodyAgreement",
            ),
            "tests/release/public_transaction_inventory_lock_table_gate.cpp": (
                "transaction inventory",
                "transaction-start high-water should be durable",
                "lock",
                "fail",
            ),
            "tests/fault_injection/transaction_inventory_publish_fault_conformance.cpp": (
                "TestCommittedJournalRecoversNewSnapshot",
                "TestPublishingJournalRecoversOldSnapshot",
                "SB-TXN-INVENTORY-PUBLISH-RECOVERY-REQUIRED",
                "SB-TXN-INVENTORY-PUBLISH-JOURNAL-CHECKSUM-MISMATCH",
            ),
        },
    },
    {
        "row_id": "mga_visibility_property",
        "boundary_surface": "mga_row_visibility",
        "boundary_kind": "engine_transaction_property",
        "property_invariant": "committed_transaction_is_visible_to_newer_valid_snapshot_and_rolled_back_version_is_never_visible",
        "behavior_gates": (
            "engine_listener_physical_mga_cow_conformance",
            "public_transaction_mga_cow_gate",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_physical_mga_cow_conformance.cpp": (
                "unpublished insert must not be visible after reopen",
                "committed insert should be visible",
                "rolled back update must not replace committed version",
                "delete marker should suppress older visible row",
            ),
            "tests/release/public_transaction_mga_cow_gate.cpp": (
                "MGA",
                "rollback",
                "commit",
                "visible",
            ),
        },
    },
    {
        "row_id": "authorization_deny_override_property",
        "boundary_surface": "materialized_authorization",
        "boundary_kind": "engine_security_property",
        "property_invariant": "explicit_deny_always_overrides_allow_and_trace_tags_never_become_production_authority",
        "behavior_gates": (
            "engine_listener_materialized_authorization_conformance",
            "public_authorization_durable_flow_gate",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_materialized_authorization_conformance.cpp": (
                "ProductionTraceTagsAreNotAuthorization",
                "explicit materialized deny did not override allow",
                "stale request epoch authorized through materialized context",
                "missing materialized context diagnostic drifted",
            ),
            "tests/release/public_authorization_durable_flow_gate.cpp": (
                "explicit durable deny should override allow grant",
                "EngineAuthorize should ignore trace tags and consume only materialized context",
                "missing materialized context should fail with stable diagnostic",
                "stale observed security epoch should fail closed",
            ),
        },
    },
    {
        "row_id": "index_authority_property",
        "boundary_surface": "index_pages_delta_ledgers",
        "boundary_kind": "engine_index_property",
        "property_invariant": "index_candidate_evidence_never_becomes_final_row_authority",
        "behavior_gates": (
            "engine_listener_index_family_dml_route_conformance",
            "public_index_dml_maintenance_strategy_gate",
            "public_index_durable_metadata_validator_gate",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_index_family_dml_route_conformance.cpp": (
                "mga_transaction_finality_authority_proof",
                "ledger family recovery classification did not preserve MGA authority",
                "retain_for_mga_transaction_finality",
                "fail_closed",
            ),
            "tests/release/public_index_dml_maintenance_strategy_gate.cpp": (
                "DML",
                "maintenance",
                "MGA",
                "fail",
            ),
        },
    },
    {
        "row_id": "toast_reachability_cleanup_property",
        "boundary_surface": "toast_overflow_pages",
        "boundary_kind": "engine_overflow_property",
        "property_invariant": "toast_cleanup_never_removes_value_reachable_from_visible_row_version",
        "behavior_gates": (
            "engine_listener_overflow_blob_page_persistence_conformance",
            "public_toast_overflow_binary_descriptor_gate",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_overflow_blob_page_persistence_conformance.cpp": (
                "overflow blob pages did not read after reopen",
                "corrupted overflow blob payload was admitted",
                "corrupted overflow blob metadata was admitted",
                "non-authoritative overflow reclaim was admitted",
                "authoritative overflow reclaim failed",
            ),
            "tests/release/public_toast_overflow_binary_descriptor_gate.cpp": (
                "committed overflow value did not read",
                "overflow read did not reconstruct original payload",
                "overflow chunk page body accepted corrupted payload bytes",
                "public_toast_overflow_binary_descriptor_gate=passed",
            ),
        },
    },
    {
        "row_id": "sblr_parser_authority_property",
        "boundary_surface": "sblr_envelopes_parser_boundary",
        "boundary_kind": "engine_sblr_property",
        "property_invariant": "parser_authority_never_becomes_engine_authority_and_source_sql_text_is_refused",
        "behavior_gates": (
            "public_sblr_uuid_mga_route_integration_gate",
            "sblr_surface_server_authority_route_conformance",
            "sblr_surface_non_direct_function_lane_conformance",
        ),
        "evidence_files": {
            "tests/release/public_sblr_uuid_mga_route_integration_gate.cpp": (
                "SBLR envelope accepted SQL text",
                "SBLR SQL-text refusal diagnostic missing",
                "optimizer route did not reject parser SQL execution authority",
                "optimizer route did not reject parser transaction authority",
            ),
            "tests/sblr_surface/sblr_surface_server_authority_route_conformance.cpp": (
                "attempted SBLR execution",
                "scratchbird_mga_authority_preserved",
                "parser_action",
                "reference",
            ),
            "tests/sblr_surface/sblr_surface_non_direct_function_lane_conformance.cpp": (
                "parser shortcut refusal evidence",
                "SBLR authority refusal evidence",
                "CompatibilityFunctionSurface",
                "unsupported",
            ),
        },
    },
    {
        "row_id": "optimizer_plan_cache_property",
        "boundary_surface": "optimizer_plan_cache_records",
        "boundary_kind": "engine_optimizer_property",
        "property_invariant": "optimizer_plan_cache_uses_only_catalog_backed_authority_and_never_unsafe_parser_or_cluster_evidence",
        "behavior_gates": (
            "engine_listener_optimizer_integrated_route_conformance",
            "public_optimizer_catalog_backed_planning_gate",
            "public_optimizer_expression_plan_cache_validation_gate",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_optimizer_integrated_route_conformance.cpp": (
                "parser",
                "cluster",
                "plan cache",
                "fail_closed_authority_boundaries",
            ),
            "tests/release/public_optimizer_expression_plan_cache_validation_gate.cpp": (
                "plan",
                "cache",
                "diagnostic",
                "fail",
            ),
        },
    },
    {
        "row_id": "agent_catalog_record_property",
        "boundary_surface": "agent_catalog_records",
        "boundary_kind": "engine_agent_record_property",
        "property_invariant": "agent_action_replay_records_remain_evidence_only_and_do_not_override_engine_authority",
        "behavior_gates": (
            "engine_listener_agent_production_action_conformance",
            "public_agent_durable_runtime_catalog_gate",
            "public_agent_action_dispatch_idempotency_gate",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_agent_production_action_conformance.cpp": (
                "durable",
                "replay",
                "compensation",
                "support",
            ),
            "tests/agents/agent_durable_runtime_catalog_gate.cpp": (
                "durable catalog image did not validate",
                "service opened without MGA durable evidence",
                "authority non-drift evidence was absent",
                "crash replay did not advance catalog generation",
            ),
            "tests/agents/agent_action_dispatch_idempotency_gate.cpp": (
                "idempotency",
                "dispatch",
                "evidence",
                "fail",
            ),
        },
    },
)

TRACEABILITY_SCHEMA_FIELDS = (
    "trace_id",
    "trace_kind",
    "source_id",
    "canonical_spec_authority",
    "implementation_route",
    "test_id",
    "platform_result",
    "crash_proof_ref",
    "security_proof_ref",
    "soak_proof_ref",
    "evidence_artifact",
    "completion_status",
    "traceability_status",
    "proof_class",
    "product_completion_claim",
)

CRASH_FAULT_TRACEABILITY_REF = (
    "ELER-080.linux_cross_cutting_crash_fault_campaign_proven_cross_platform_pending"
)
CRASH_RECOVERY_CERTIFICATION_TRACEABILITY_REF = (
    "ELER-101.linux_crash_recovery_certification_matrix_proven_cross_platform_pending"
)
ADVERSARIAL_SECURITY_TRACEABILITY_REF = (
    "ELER-103.linux_adversarial_security_validation_proven_cross_platform_pending"
)
FUZZ_PROPERTY_TRACEABILITY_REF = (
    "ELER-104.linux_fuzz_property_invariant_suite_proven_cross_platform_pending"
)
PERFORMANCE_SCALABILITY_TRACEABILITY_REF = (
    "ELER-105.linux_performance_scalability_baseline_proven_cross_platform_pending"
)
OPERATIONAL_READINESS_TRACEABILITY_REF = (
    "ELER-107.linux_operational_readiness_diagnosability_proven_cross_platform_pending"
)
RELEASE_ARTIFACT_TRUST_TRACEABILITY_REF = (
    "ELER-108.linux_release_artifact_trust_proven_cross_platform_pending"
)
ENTERPRISE_DOCUMENTATION_TRACEABILITY_REF = (
    "ELER-109.linux_enterprise_documentation_runbooks_proven_cross_platform_pending"
)
SUPPORT_MAINTENANCE_TRACEABILITY_REF = (
    "ELER-111.linux_support_maintenance_policy_proven_cross_platform_pending"
)
SOAK_CERTIFICATION_TRACEABILITY_REF = (
    "ELER-102.linux_long_duration_soak_certification_proven_cross_platform_pending"
)
FINAL_PROJECT_IMPLEMENTATION_TRACEABILITY_REF = (
    "ELER-082.linux_final_project_only_implementation_proof_proven_cross_platform_pending"
)
INDEPENDENT_REVIEW_CLOSURE_TRACEABILITY_REF = (
    "ELER-110.linux_independent_review_closure_package_proven_external_validation_pending"
)
GOLD_ENTERPRISE_READINESS_TRACEABILITY_REF = (
    "ELER-112.linux_gold_enterprise_release_candidate_evidence_proven_cross_platform_and_external_audit_pending"
)
THIRD_AUDIT_PACKAGE_TRACEABILITY_REF = (
    "ELER-090.linux_third_audit_evidence_package_proven_external_auditor_pending"
)

PERFORMANCE_REQUIRED_METRIC_FIELDS = (
    "mean_ms",
    "median_ms",
    "p50_ms",
    "p90_ms",
    "p95_ms",
    "p99_ms",
    "p99_9_ms",
    "max_latency_ms",
    "error_rate_max",
    "retry_rate_max",
    "spill_rate_max",
    "cleanup_lag_max",
    "memory_high_water_bytes_max",
    "fd_high_water_max",
)

PERFORMANCE_SCALABILITY_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "lifecycle_create_open_latency",
        "surface": "database_lifecycle",
        "required_operation_id": "create_open_database",
        "metric_claim": "startup/open latency has p90/p95/p99/p99.9/max thresholds and runtime measurement output",
        "behavior_gates": ("scratchbird_beta_performance_baseline_conformance",),
        "evidence_files": {
            "tests/performance/scratchbird_beta_performance_baseline_conformance.cpp": (
                "ConfigureMemoryFixture",
                "startup_open_latency_ms",
                "p99_9_ms",
                "fd_high_water",
            ),
            "tests/performance/public_performance_baselines.json": (
                "create_open_database",
                "p99_9_ms",
                "max_latency_ms",
                "fd_high_water_max",
            ),
        },
    },
    {
        "row_id": "transaction_begin_commit_latency",
        "surface": "transactions",
        "required_operation_id": "transaction_begin_commit",
        "metric_claim": "MGA begin/commit latency is thresholded without parser or reference transaction authority",
        "behavior_gates": ("scratchbird_beta_performance_baseline_conformance",),
        "evidence_files": {
            "tests/performance/scratchbird_beta_performance_baseline_conformance.cpp": (
                "session_begin_commit_latency_ms",
                "transaction.begin",
                "transaction.commit",
                "engine_mga_transaction_authority",
            ),
            "tests/performance/public_performance_baselines.json": (
                "transaction_begin_commit",
                "mga_transaction_inventory_authority_only",
                "p99_9_ms",
                "cleanup_lag_max",
            ),
        },
    },
    {
        "row_id": "dml_insert_throughput",
        "surface": "dml",
        "required_operation_id": "dml_insert_batch",
        "metric_claim": "bounded DML insert throughput is measured through SBLR DML and native MGA storage",
        "behavior_gates": ("scratchbird_beta_performance_baseline_conformance",),
        "evidence_files": {
            "tests/performance/scratchbird_beta_performance_baseline_conformance.cpp": (
                "insert_rows_per_second",
                "SBLR_DML_INSERT_ROWS",
                "scratchbird_mga_native_storage",
                "reference_or_embedded_storage_backend",
            ),
            "tests/performance/public_performance_baselines.json": (
                "dml_insert_batch",
                "min_insert_rows_per_second",
                "error_rate_max",
                "retry_rate_max",
            ),
        },
    },
    {
        "row_id": "dml_select_percentiles",
        "surface": "dml",
        "required_operation_id": "dml_select_batch",
        "metric_claim": "bounded DML select latency emits mean median p90 p95 p99 p99.9 and max evidence",
        "behavior_gates": ("scratchbird_beta_performance_baseline_conformance",),
        "evidence_files": {
            "tests/performance/scratchbird_beta_performance_baseline_conformance.cpp": (
                "latency_percentiles",
                "select_all_latency_ms",
                "selected_physical_path_result_parity",
                "selected_path_requires_mga_visibility_recheck",
            ),
            "tests/performance/public_performance_baselines.json": (
                "dml_select_batch",
                "min_select_rows_per_second",
                "p99_9_ms",
                "max_latency_ms",
            ),
        },
    },
    {
        "row_id": "index_lookup_runtime_truthfulness",
        "surface": "index",
        "required_operation_id": "index_lookup",
        "metric_claim": "index lookup performance evidence is admitted only for physically complete benchmark-clean families",
        "behavior_gates": ("index_runtime_capability_truthfulness_gate",),
        "evidence_files": {
            "tests/performance/index_runtime_capability_truthfulness_gate.cpp": (
                "runtime benchmark-clean closure",
                "supports_equality_lookup",
                "operation_metrics_producer_proven",
                "requires_mga_recheck",
            ),
            "tests/performance/public_performance_baselines.json": (
                "index_lookup",
                "index_lookup_evidence_only",
                "min_lookup_rows_per_second",
                "max_lookup_result_rows",
            ),
        },
    },
    {
        "row_id": "optimizer_plan_cache_methodology",
        "surface": "optimizer",
        "required_operation_id": "optimizer_planning_plan_cache",
        "metric_claim": "optimizer benchmark-clean evidence requires route labels samples profiler provenance and percentile consistency",
        "behavior_gates": ("optimizer_runtime_hot_path_orh_123_gate",),
        "evidence_files": {
            "tests/performance/optimizer_runtime_hot_path_orh_123_gate.cpp": (
                "ValidateBenchmarkMethodologyEvidence",
                "SAMPLES_INSUFFICIENT",
                "PERCENTILE_MISMATCH",
                "performance-proof evidence with full controls should remain benchmark-clean",
            ),
            "tests/performance/public_performance_baselines.json": (
                "optimizer_planning_plan_cache",
                "optimizer_runtime_hot_path_noise_control",
                "max_benchmark_relative_noise",
                "optimizer_evidence_only",
            ),
        },
    },
    {
        "row_id": "listener_accept_to_handoff_latency",
        "surface": "listener",
        "required_operation_id": "listener_accept_to_handoff",
        "metric_claim": "DBBT/LPREFACE handoff binding has a bounded accept-to-handoff claim path and zero misbind allowance",
        "behavior_gates": ("engine_listener_dbbt_lpreface_binding_conformance",),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_dbbt_lpreface_binding_conformance.cpp": (
                "listener_accept_to_handoff_latency_us",
                "listener accept-to-handoff latency budget exceeded",
                "matching only the client nonce must not authorize",
                "out-of-order claim must not steal",
            ),
            "tests/performance/public_performance_baselines.json": (
                "listener_accept_to_handoff",
                "max_accept_to_handoff_latency_ms",
                "max_misbound_handoffs",
                "listener_handoff_evidence_only",
            ),
        },
    },
    {
        "row_id": "parser_worker_pool_exhaustion_pressure",
        "surface": "listener",
        "required_operation_id": "parser_worker_pool_exhaustion",
        "metric_claim": "parser-worker pool pressure has bounded decision and zero orphan-worker expectations",
        "behavior_gates": ("engine_listener_parser_pool_lifecycle_conformance",),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_parser_pool_lifecycle_conformance.cpp": (
                "parser_pool",
                "child-quarantine-window-ms",
                "RunConfigValidationProof",
                "LISTENER.CONFIG.INVALID_CHILD_RESTART_BACKOFF",
                "quarantine",
                "running_workers",
            ),
            "tests/performance/public_performance_baselines.json": (
                "parser_worker_pool_exhaustion",
                "max_pool_exhaustion_decision_latency_ms",
                "max_orphan_parser_workers",
                "listener_pool_evidence_only",
            ),
        },
    },
    {
        "row_id": "memory_pressure_high_water",
        "surface": "memory",
        "required_operation_id": "memory_pressure",
        "metric_claim": "memory pressure evidence includes rss growth, memory high-water, and file descriptor high-water ceilings",
        "behavior_gates": (
            "scratchbird_beta_performance_baseline_conformance",
            "memory_observability_overhead_budget_gate",
        ),
        "evidence_files": {
            "tests/performance/scratchbird_beta_performance_baseline_conformance.cpp": (
                "resource_high_water",
                "memory_high_water_bytes",
                "rss_growth_bytes",
                "fd_high_water",
            ),
            "tests/performance/memory_observability_overhead_budget_gate.cpp": (
                "MMCH_MEMORY_OBSERVABILITY_OVERHEAD_BUDGET",
                "within_budget",
                "p95_microseconds",
                "p99_microseconds",
            ),
        },
    },
    {
        "row_id": "support_bundle_generation_overhead",
        "surface": "support_bundle",
        "required_operation_id": "support_bundle_generation",
        "metric_claim": "support-bundle generation has redaction and no secret leakage budget evidence",
        "behavior_gates": ("engine_listener_support_bundle_redaction_gate",),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_support_bundle_redaction_gate.cpp": (
                "agent_runtime_evidence_collected",
                "performance_optimization_surface_collected",
                "contains no canary",
                "support_bundle_json",
            ),
            "tests/performance/public_performance_baselines.json": (
                "support_bundle_generation",
                "max_support_bundle_generation_latency_ms",
                "max_unredacted_secret_fields",
                "secret_leakage_max",
            ),
        },
    },
    {
        "row_id": "evidence_overhead_budget",
        "surface": "optimizer",
        "required_operation_id": "evidence_overhead_budget",
        "metric_claim": "diagnostic and evidence overhead is separately measured and rejected when hidden or over budget",
        "behavior_gates": ("optimizer_runtime_hot_path_orh_128_gate",),
        "evidence_files": {
            "tests/performance/optimizer_runtime_hot_path_orh_128_gate.cpp": (
                "evidence_overhead_budget=on",
                "P99_OVER_BUDGET",
                "HIDDEN_IN_EXECUTION_TIMING",
                "support_bundle.evidence_overhead",
            ),
            "tests/performance/public_performance_baselines.json": (
                "evidence_overhead_budget",
                "max_evidence_overhead_factor",
                "max_unbounded_evidence_growth",
                "optimizer_evidence_overhead_budget",
            ),
        },
    },
)

SOAK_CERTIFICATION_LABEL_REQUIRED_GATES = {
    "agent_enterprise_soak_performance_gate",
    "ceic_093_reliability_security_suite_gate",
    "dpc_soak_leak_resource_stability_gate",
    "memory_sanitizer_soak_concurrency_gate",
    "public_release_soak_gate",
    "public_release_soak_lane",
}

SOAK_CERTIFICATION_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "bounded_public_release_soak_lane",
        "surface": "bounded_soak",
        "certification_policy": (
            "bounded developer and release soak lanes must execute through CTest, "
            "cap time and iteration budgets, retain diagnostic artifacts, and "
            "cover memory pressure, concurrent transactions, cleanup, index "
            "maintenance, backup forward, agents, and support bundle generation"
        ),
        "required_lanes": (
            "memory_pressure",
            "memory_concurrency_reference",
            "concurrent_transactions",
            "cleanup_sweep",
            "index_maintenance",
            "backup_forward",
            "agents",
            "support_bundle_generation",
        ),
        "behavior_gates": (
            "public_release_soak_lane",
            "public_release_soak_gate",
            "memory_sanitizer_soak_concurrency_gate",
        ),
        "required_labels": ("ELER-081", "ELER-GATE-081", "ELER-102", "ELER-GATE-102"),
        "evidence_files": {
            "tests/soak/public_release_soak_lane.py": (
                "PUBLIC_RELEASE_SOAK_LANE",
                "total_time_budget_seconds",
                "unbounded_soak_required_for_gate",
                "support_bundle_generation",
            ),
            "tests/release/public_release_soak_gate.py": (
                "REQUIRED_ROWS",
                "soak_lane_budget_unbounded",
                "memory_sanitizer_soak_concurrency_gate",
            ),
            "tests/concurrency/memory_sanitizer_soak_concurrency_gate.cpp": (
                "constexpr int kThreads = 8",
                "constexpr int kIterations = 512",
                "concurrent allocation churn had failures",
                "memory leak after concurrent allocation churn",
            ),
        },
    },
    {
        "row_id": "long_duration_reliability_profiles",
        "surface": "long_duration_profiles",
        "certification_policy": (
            "long-duration release certification must include completed 24-hour "
            "72-hour, and 7-day reliability lanes with retained stdout stderr, CTest "
            "inventory, failure inventory, support bundle snapshots, redaction "
            "metadata, and no synthetic or fixture-only production evidence"
        ),
        "required_lanes": ("soak_24h", "soak_72h", "soak_7d"),
        "behavior_gates": ("ceic_093_reliability_security_suite_gate",),
        "required_labels": ("ELER-102", "ELER-GATE-102"),
        "evidence_files": {
            "tools/ceic_reliability_security_suite.py": (
                "soak_24h",
                "duration_hours=24",
                "soak_72h",
                "duration_hours=72",
                "soak_7d",
                "duration_hours=168",
                "failure_inventory_retained",
                "fixture_or_test_only_evidence",
            ),
            "tests/consolidated_enterprise/ceic_093_reliability_security_suite_gate_test.py": (
                "missing_soak_duration",
                "pending_or_defined_only_lane",
                "fixture_test_only",
                "local_cluster_claim",
            ),
        },
    },
    {
        "row_id": "high_concurrency_pressure_soak",
        "surface": "high_concurrency_pressure",
        "certification_policy": (
            "soak certification must include high-concurrency route churn, "
            "memory-pressure workloads, queue backlog and action pressure, "
            "p50 p95 p99 latency evidence, fail-closed pressure refusals, and "
            "bounded foreground resource behavior"
        ),
        "required_lanes": (
            "high_concurrency",
            "memory_pressure",
            "cgroup_or_job_object_pressure",
        ),
        "behavior_gates": (
            "ceic_093_reliability_security_suite_gate",
            "agent_enterprise_soak_performance_gate",
            "dpc_soak_leak_resource_stability_gate",
        ),
        "required_labels": ("ELER-102", "ELER-GATE-102"),
        "evidence_files": {
            "tools/ceic_reliability_security_suite.py": (
                "worker_counts=[8, 32, 64, 128, 256]",
                "cgroup_or_job_object_pressure",
                "missing_high_concurrency",
                "missing_memory_pressure",
            ),
            "tests/agents/agent_enterprise_soak_performance_gate.cpp": (
                "AEIC-043 reservation soak leaked active reservations",
                "AEIC-043 reservation soak did not exercise fail-closed pressure",
                "reservation_p95_us",
                "reservation_p99_us",
            ),
            "tests/database_lifecycle/dpc_soak_leak_resource_stability_gate.cpp": (
                "DPC-070 file descriptor growth exceeded bounded allowance",
                "DPC-070 scheduler queue proxy leaked after loop",
                "DPC-070 foreground p95 latency exceeded bounded allowance",
                "bounded_soak_completed",
            ),
        },
    },
    {
        "row_id": "crash_fault_security_negative_soak",
        "surface": "crash_fault_security_negative",
        "certification_policy": (
            "soak certification must include crash and restart cycles, fault "
            "injection, sanitizer/static analysis, and hostile security-negative "
            "checks that fail closed without granting cluster, reference, parser, "
            "WAL, recovery, transaction finality, or authorization authority"
        ),
        "required_lanes": (
            "crash_fault_injection",
            "sanitizer_static",
            "security_negative",
        ),
        "behavior_gates": (
            "ceic_093_reliability_security_suite_gate",
            "engine_listener_crash_recovery_certification_gate",
            "engine_listener_adversarial_security_validation_gate",
        ),
        "required_labels": ("ELER-102", "ELER-GATE-102"),
        "evidence_files": {
            "tools/ceic_reliability_security_suite.py": (
                "crash_restart_cycles",
                "REQUIRED_CRASH_FAULT_POINTS",
                "REQUIRED_SANITIZER_STATIC_KINDS",
                "REQUIRED_SECURITY_NEGATIVES",
                "reference_parser_wal_authority",
                "local_cluster_production_claim",
            ),
            "tests/consolidated_enterprise/ceic_093_reliability_security_suite_gate_test.py": (
                "missing_crash_fault",
                "missing_sanitizer_static",
                "bypassed_security_negative",
                "unsafe_authority",
            ),
        },
    },
    {
        "row_id": "leak_drift_orphan_supportability_soak",
        "surface": "leak_drift_orphan_supportability",
        "certification_policy": (
            "soak certification must prove no descriptor leak, file descriptor "
            "leak, orphan runtime artifact, session drift, auth context drift, "
            "cursor leak, quota leak, scheduler leak, support bundle secret leak, "
            "or unsafe execution_plan/private-path evidence leak"
        ),
        "required_lanes": (
            "artifact_retention",
            "support_bundle_snapshot_retained",
            "redaction_metadata_retained",
        ),
        "behavior_gates": (
            "dpc_soak_leak_resource_stability_gate",
            "engine_listener_support_bundle_redaction_gate",
            "engine_listener_support_bundle_conformance",
        ),
        "required_labels": ("ELER-081", "ELER-GATE-081", "ELER-102", "ELER-GATE-102"),
        "evidence_files": {
            "tools/ceic_reliability_security_suite.py": (
                "support_bundle_snapshot_retained",
                "redaction_metadata_retained",
                "missing_artifact_retention",
            ),
            "tests/database_lifecycle/dpc_soak_leak_resource_stability_gate.cpp": (
                "DPC-070 session leaked after disconnect",
                "DPC-070 auth context leaked after disconnect",
                "DPC-070 cursor leaked after non-cursor loop",
                "DPC-070 support-bundle leaked unsafe or execution_plan runtime data",
            ),
            "tests/engine_listener_enterprise/engine_listener_support_bundle_redaction_gate.cpp": (
                "contains no canary",
                "support_bundle_json",
                "redaction_disabled",
                "missing_security_context",
            ),
        },
    },
)

COMPATIBILITY_UPGRADE_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "database_file_format_upgrade_downgrade",
        "surface": "database_file_format",
        "compatibility_policy": (
            "database create/open/reopen accepts current format, refuses future and "
            "unsupported old formats, requires explicit migration plans, refuses "
            "downgrade, and fails closed on ambiguous identity"
        ),
        "expected_outcomes": (
            "current_accept",
            "future_refuse",
            "downgrade_refuse",
            "migration_plan_required",
            "explicit_plan_accept_or_required",
            "ambiguous_identity_refuse",
        ),
        "behavior_gates": (
            "public_upgrade_migration_gate",
            "database_lifecycle_upgrade_migration_conformance",
        ),
        "evidence_files": {
            "tests/release/public_upgrade_migration_gate.cpp": (
                "CheckVersionedArtifactSurface",
                "downgrade_requested",
                "ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED",
                "supported_migration",
            ),
            "tests/database_lifecycle/upgrade_migration_conformance.cpp": (
                "FORMAT.VERSION_UNSUPPORTED",
                "FORMAT.UNKNOWN_REQUIRED_FLAG",
                "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
                "SB-DB-LIFECYCLE-CLUSTER-AUTHORITY-REQUIRED",
            ),
        },
    },
    {
        "row_id": "catalog_page_record_format_policy",
        "surface": "catalog_page_record",
        "compatibility_policy": (
            "catalog record and catalog page body versions are explicitly versioned "
            "and future schemas fail closed before catalog evidence becomes authority"
        ),
        "expected_outcomes": (
            "current_accept",
            "future_refuse",
            "tamper_fail_closed",
        ),
        "behavior_gates": ("public_upgrade_migration_gate",),
        "evidence_files": {
            "tests/release/public_upgrade_migration_gate.cpp": (
                "CheckCatalogCodecAndPage",
                "SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED",
                "SB-CATALOG-PAGE-BODY-FORMAT-UNSUPPORTED",
                "future_schema",
            ),
        },
    },
    {
        "row_id": "catalog_filespace_seed_manifest_policy",
        "surface": "catalog_filespace_seed",
        "compatibility_policy": (
            "database catalog manifests, filespace manifests, and resource seed "
            "manifests carry versioned format evidence, require explicit migration "
            "evidence, and refuse ambiguous or future identities"
        ),
        "expected_outcomes": (
            "current_accept",
            "future_refuse",
            "migration_plan_required",
            "explicit_plan_accept_or_required",
            "ambiguous_identity_refuse",
        ),
        "behavior_gates": (
            "public_upgrade_migration_gate",
            "database_lifecycle_upgrade_migration_conformance",
        ),
        "evidence_files": {
            "tests/release/public_upgrade_migration_gate.cpp": (
                "CheckCatalogMigrationEvidence",
                "database_catalog_manifest_v0_0_to_v1_0_explicit_plan_v1",
                "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
                "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
            ),
            "tests/database_lifecycle/upgrade_migration_conformance.cpp": (
                "old filespace manifest did not require migration plan",
                "future resource seed manifest was not refused",
                "ambiguous catalog identity evidence was not refused",
            ),
        },
    },
    {
        "row_id": "transaction_inventory_page_format_policy",
        "surface": "transaction_inventory",
        "compatibility_policy": (
            "transaction inventory page metadata is versioned, strong-digest checked, "
            "durable across database storage, and refuses weak or corrupt historical "
            "formats before visibility finality"
        ),
        "expected_outcomes": (
            "current_accept",
            "tamper_fail_closed",
            "unsupported_old_refuse",
        ),
        "behavior_gates": ("public_transaction_inventory_lock_table_gate",),
        "evidence_files": {
            "tests/release/public_transaction_inventory_lock_table_gate.cpp": (
                "transaction-start high-water should be durable",
                "SB-TXN-INVENTORY-PAGE-WEAK-DIGEST-REFUSED",
                "SB-TXN-INVENTORY-PAGE-GENERATION-INVALID",
                "chain digest corruption should fail closed",
            ),
        },
    },
    {
        "row_id": "index_metadata_format_policy",
        "surface": "index_metadata",
        "compatibility_policy": (
            "durable index metadata and delta-ledger evidence reopen only when format "
            "metadata is current and never becomes MGA finality authority"
        ),
        "expected_outcomes": (
            "current_accept",
            "future_refuse",
            "tamper_fail_closed",
        ),
        "behavior_gates": (
            "public_upgrade_migration_gate",
            "engine_listener_index_family_dml_route_conformance",
        ),
        "evidence_files": {
            "tests/release/public_upgrade_migration_gate.cpp": (
                "CheckIndexMetapageCompatibility",
                "future_metadata_format",
                "SB-INDEX-METAPAGE-DURABLE-METADATA-INVALID",
            ),
            "tests/engine_listener_enterprise/engine_listener_index_family_dml_route_conformance.cpp": (
                "ledger family reopen decode changed mutation records",
                "ledger family recovery classification did not preserve MGA authority",
                "fail_closed",
            ),
        },
    },
    {
        "row_id": "toast_overflow_chunk_format_policy",
        "surface": "toast_overflow",
        "compatibility_policy": (
            "TOAST overflow chunk pages use a versioned format and preserve generation, "
            "local transaction id, content hashes, corruption refusal, and authoritative "
            "MGA cleanup reachability"
        ),
        "expected_outcomes": (
            "current_accept",
            "tamper_fail_closed",
        ),
        "behavior_gates": (
            "public_toast_overflow_binary_descriptor_gate",
            "engine_listener_overflow_blob_page_persistence_conformance",
        ),
        "evidence_files": {
            "tests/release/public_toast_overflow_binary_descriptor_gate.cpp": (
                "ValidateOverflowChunkPageBody",
                "overflow chunk page body accepted corrupted payload bytes",
                "overflow chunk page body lost generation",
                "committed overflow value did not read",
            ),
            "tests/engine_listener_enterprise/engine_listener_overflow_blob_page_persistence_conformance.cpp": (
                "overflow blob pages did not read after reopen",
                "authoritative overflow reclaim failed",
                "corruption sync failed",
            ),
        },
    },
    {
        "row_id": "datatype_descriptor_wire_format_policy",
        "surface": "datatype_descriptor",
        "compatibility_policy": (
            "datatype descriptor envelopes and native wire metadata require strong "
            "versioned layouts, keyed protected descriptors, and future-layout refusal"
        ),
        "expected_outcomes": (
            "current_accept",
            "future_refuse",
            "tamper_fail_closed",
            "unsupported_old_refuse",
        ),
        "behavior_gates": (
            "public_datatype_binary_descriptor_integrity_gate",
            "public_upgrade_migration_gate",
        ),
        "evidence_files": {
            "tests/release/public_datatype_binary_descriptor_integrity_gate.cpp": (
                "descriptor envelope accepted weak integrity profile",
                "protected descriptor decoded without key material",
                "domain descriptor envelope did not use SHA-256",
                "structured set descriptor did not round trip",
            ),
            "tests/release/public_upgrade_migration_gate.cpp": (
                "CheckDatatypeWireCompatibility",
                "NATIVE_WIRE.LAYOUT_VERSION_UNSUPPORTED",
                "future_parameter_packet_layout",
                "future_row_description_layout",
            ),
        },
    },
    {
        "row_id": "security_catalog_policy_pack_format_policy",
        "surface": "security_catalog_policy",
        "compatibility_policy": (
            "protected-material catalog and policy-pack metadata remain versioned, "
            "durable, redacted, tamper-evident, database-bound, and downgrade-refusing"
        ),
        "expected_outcomes": (
            "current_accept",
            "downgrade_refuse",
            "migration_plan_required",
            "tamper_fail_closed",
        ),
        "behavior_gates": (
            "engine_listener_protected_material_durable_catalog_conformance",
            "public_upgrade_migration_gate",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_protected_material_durable_catalog_conformance.cpp": (
                "protected material binary catalog was not created",
                "tampered protected material catalog did not fail closed",
                "protected material durable catalog inspect did not reload material versions and audit",
                "protected material catalog accepted process memory without database path",
            ),
            "tests/release/public_upgrade_migration_gate.cpp": (
                "policy_pack_manifest",
                "minor_upgrade_missing_plan",
                "downgrade_requested",
                "ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED",
            ),
        },
    },
    {
        "row_id": "listener_control_lpreface_protocol_versioning",
        "surface": "listener_control_protocol",
        "compatibility_policy": (
            "listener control, SBPS, SBDB, endpoint descriptors, and LPREFACE handoff "
            "claims are versioned and fail closed on future, too-old, or stale epochs"
        ),
        "expected_outcomes": (
            "current_accept",
            "future_refuse",
            "unsupported_old_refuse",
            "versioned_protocol_refusal",
        ),
        "behavior_gates": (
            "database_lifecycle_protocol_versioning_conformance",
            "sbmn_manager_protocol_unit_tests",
            "engine_listener_dbbt_lpreface_binding_conformance",
        ),
        "evidence_files": {
            "tests/database_lifecycle/protocol_versioning_conformance.cpp": (
                "PARSER_SERVER_IPC.PROTOCOL_VERSION_FUTURE",
                "PARSER_SERVER_IPC.PROTOCOL_VERSION_TOO_OLD",
                "IPC.LIFECYCLE.DESCRIPTOR_VERSION_FUTURE",
                "IPC.LIFECYCLE.EPOCH_STALE",
            ),
            "tests/manager/protocol_unit_tests.cpp": (
                "SBDB frame version must be 0x0101 little-endian",
                "LPREFACE handoff claim must use a versioned prefix",
                "LPREFACE.CLAIM_NONCE_LENGTH",
            ),
            "tests/engine_listener_enterprise/engine_listener_dbbt_lpreface_binding_conformance.cpp": (
                "matching only the client nonce must not authorize",
                "out-of-order claim must not steal",
                "reader must leave parser protocol bytes after the claim untouched",
            ),
        },
    },
    {
        "row_id": "management_protocol_compatibility",
        "surface": "management_protocol",
        "compatibility_policy": (
            "listener management commands require authenticated SBME envelopes, "
            "stable rights checks, replay refusal, and a versioned manager hello surface"
        ),
        "expected_outcomes": (
            "current_accept",
            "versioned_protocol_refusal",
            "tamper_fail_closed",
        ),
        "behavior_gates": (
            "engine_listener_management_envelope_conformance",
            "sbmn_manager_protocol_unit_tests",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_management_envelope_conformance.cpp": (
                "raw privileged refusal must require SBME envelope",
                "tampered hmac-sha256 SBME envelope must be refused",
                "duplicate live SBME envelope must be refused as replay",
                "DBBT_VALIDATE must pass through authenticated SBME management path",
            ),
            "tests/manager/runtime_integration_tests.cpp": (
                "listener-control SBME envelope version must be 1",
                "MCP_HELLO must expose protocol version 0x0100",
                "MCP_HELLO must expose DB_CONNECT extended magic",
            ),
        },
    },
    {
        "row_id": "parser_worker_api_contract_freeze",
        "surface": "parser_worker_api",
        "compatibility_policy": (
            "parser-worker IPC versions and parser-facing SBLR contract freeze rows "
            "are versioned, deterministic, unsupported-surface aware, and current-only"
        ),
        "expected_outcomes": (
            "current_accept",
            "future_refuse",
            "versioned_protocol_refusal",
            "parser_contract_freeze",
        ),
        "behavior_gates": (
            "database_lifecycle_protocol_versioning_conformance",
            "engine_listener_parser_contract_freeze_gate",
            "public_parser_contract_freeze_gate",
        ),
        "evidence_files": {
            "tests/database_lifecycle/protocol_versioning_conformance.cpp": (
                "PARSER_IPC.PROTOCOL.FUTURE_UNSUPPORTED",
                "DBLC-013O future legacy parser IPC protocol was admitted",
            ),
            "tests/engine_listener_enterprise/fixtures/parser_facing_contract_freeze_manifest.json": (
                "freeze_version",
                "compatibility_version",
                "sblr.v3.parser_contract.freeze.1",
                "unsupported_surface_vectors",
            ),
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py": (
                "parser_contract_compatibility_rows",
                "frozen_parser_contract_current",
                "compatibility_version",
            ),
        },
    },
    {
        "row_id": "optimizer_plan_cache_format_compatibility",
        "surface": "optimizer_plan_cache",
        "compatibility_policy": (
            "optimizer plan cache keys and persistence envelopes bind compatibility "
            "epochs, format epochs, dependency digests, redaction, memory feedback, "
            "and refuse stale or tampered persisted plans"
        ),
        "expected_outcomes": (
            "current_accept",
            "cache_invalidation",
            "tamper_fail_closed",
            "versioned_protocol_refusal",
        ),
        "behavior_gates": (
            "optimizer_enterprise_plan_cache_gate",
            "public_optimizer_expression_plan_cache_validation_gate",
        ),
        "evidence_files": {
            "tests/optimizer/optimizer_enterprise_plan_cache_gate.cpp": (
                "input.compatibility_epoch = 5110",
                "input.format_compatibility_epoch = 5111",
                "memory feedback publication did not invalidate restored plan",
                "tampered persistence envelope was accepted",
            ),
            "tests/release/public_optimizer_expression_plan_cache_validation_gate.cpp": (
                "production plan-cache key should bind compatibility epochs",
                "compatibility cache builder should not satisfy enterprise production validation",
                "changed dependency digest set must refuse cache reuse",
                "parameter_shape_digest",
            ),
        },
    },
    {
        "row_id": "cluster_schema_stub_compatibility",
        "surface": "cluster_stub_schema",
        "compatibility_policy": (
            "open-source cluster catalog/version surfaces are admitted only through "
            "external-provider/stub boundaries and fail closed without local private "
            "cluster execution or mapping"
        ),
        "expected_outcomes": (
            "current_accept",
            "future_refuse",
            "migration_plan_required",
            "cluster_stub_boundary",
        ),
        "behavior_gates": (
            "public_cluster_schema_version_gate",
            "public_upgrade_migration_gate",
        ),
        "evidence_files": {
            "tests/release/public_cluster_schema_version_gate.cpp": (
                "cluster schema profile was not external-provider-bound",
                "SB-CLUSTER-CATALOG-SCHEMA-EXTERNAL-PROVIDER-REQUIRED",
                "SB-CLUSTER-CATALOG-SCHEMA-UNSUPPORTED-NEW",
                "local cluster mapping executed instead of external-provider refusal",
            ),
            "tests/release/public_upgrade_migration_gate.cpp": (
                "CheckClusterCatalogMigration",
                "SB-CLUSTER-CATALOG-MIGRATION-PLAN-MISSING",
                "SB-CLUSTER-CATALOG-MIGRATION-UNSUPPORTED",
            ),
        },
    },
)

OPERATIONAL_READINESS_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "health_readiness_liveness_status_surface",
        "surface": "health_readiness_liveness",
        "operational_policy": (
            "management health, readiness, liveness, and status requests are "
            "authenticated, canonicalized, observable, and backed by live server "
            "state metrics rather than static file or documentation evidence"
        ),
        "required_evidence": (
            "health_check",
            "readiness_check",
            "liveness_check",
            "metrics",
        ),
        "behavior_gates": (
            "engine_listener_management_envelope_conformance",
            "database_lifecycle_admin_cli_conformance",
            "database_lifecycle_observability_conformance",
        ),
        "evidence_files": {
            "src/server/manager_control.cpp": (
                "CanonicalManagementOperationKey",
                "health_database",
                "show_server_health",
                "status_database",
            ),
            "tests/database_lifecycle/admin_cli_conformance.cpp": (
                "operator status route failed",
                "DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE",
                "verify_database",
                "hasCompleteAdminLifecycleRouteCoverage",
            ),
            "src/server/server_observability.cpp": (
                "sys.metrics.server.lifecycle.state",
                "sys.metrics.server.database.owned",
                "sys.metrics.lifecycle.operation_total",
            ),
        },
    },
    {
        "row_id": "authenticated_admin_command_surface",
        "surface": "management_authz_admin",
        "operational_policy": (
            "administrative command routes require stable management rights, "
            "durable audit evidence, exact diagnostics, and explicit refusal for "
            "unsafe repair, drop, and force-shutdown operations"
        ),
        "required_evidence": (
            "admin_command",
            "authorization",
            "diagnostics",
            "structured_logs",
        ),
        "behavior_gates": (
            "engine_listener_management_envelope_conformance",
            "database_lifecycle_admin_cli_conformance",
        ),
        "evidence_files": {
            "src/server/manager_control.cpp": (
                "RequiredRightForOperation",
                "EngineAuthorizeManagement",
                "OBS_MANAGEMENT_CONTROL",
                "SUPPORT_EXPORT",
            ),
            "tests/engine_listener_enterprise/engine_listener_management_envelope_conformance.cpp": (
                "raw privileged refusal must require SBME envelope",
                "duplicate live SBME envelope must be refused as replay",
                "DBBT_VALIDATE must pass through authenticated SBME management path",
            ),
            "tests/database_lifecycle/admin_cli_conformance.cpp": (
                "SECURITY.ACCESS_DENIED",
                "ENGINE.DBLC_REPAIR_REFUSED",
                "ENGINE.SHUTDOWN_INPUT_INVALID",
                "admin routes did not produce audit evidence",
            ),
        },
    },
    {
        "row_id": "drain_shutdown_stop_lifecycle_surface",
        "surface": "drain_shutdown_lifecycle",
        "operational_policy": (
            "listener drain, graceful stop, clean shutdown, forced shutdown, and "
            "shutdown acknowledgement paths fence admission, preserve recovery "
            "evidence, and publish bounded active-worker/session state"
        ),
        "required_evidence": (
            "drain_mode",
            "safe_shutdown",
            "forced_shutdown",
            "recovery_required",
            "admin_command",
        ),
        "behavior_gates": (
            "engine_listener_graceful_drain_stop_conformance",
            "database_lifecycle_shutdown_conformance",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_graceful_drain_stop_conformance.cpp": (
                "DRAIN did not publish bounded active-worker status",
                "accepting_new_connections",
                "force_required",
                "STOP GRACEFUL did not complete after active work drained",
            ),
            "tests/database_lifecycle/shutdown_conformance.cpp": (
                "shutdown did not fence all ordinary admission",
                "ENGINE.SHUTDOWN_ACK_TIMEOUT",
                "ENGINE.SHUTDOWN_DRAIN_TIMEOUT",
                "unknown_transaction_finality_preserved",
            ),
            "src/server/maintenance_coordinator.cpp": (
                "shutdown_force_recovery_evidence_missing",
                "shutdown_acknowledgement_timeout",
                "shutdown_drain_timeout",
                "shutdown_clean_final_lifecycle_transaction_committed",
            ),
        },
    },
    {
        "row_id": "restricted_recovery_repair_verify_surface",
        "surface": "recovery_restricted_repair_verify",
        "operational_policy": (
            "restricted-open, read-only inspection, verify, repair, and recovery "
            "modes keep write admission fenced until explicit repair evidence and "
            "MGA lifecycle records permit ordinary open"
        ),
        "required_evidence": (
            "repair_verify",
            "read_only_recovery",
            "recovery_required",
            "diagnostics",
        ),
        "behavior_gates": (
            "database_lifecycle_maintenance_repair_conformance",
            "dpc_cleanup_backup_restore_repair_diagnostics_gate",
            "dpc_index_validation_repair_tooling_gate",
        ),
        "evidence_files": {
            "tests/database_lifecycle/maintenance_repair_conformance.cpp": (
                "restricted-open did not fence write admission",
                "ordinary open succeeded during restricted-open",
                "ENGINE.DBLC_REPAIR_REFUSED",
                "repair did not clear verified write fence",
            ),
            "tests/database_lifecycle/dpc_cleanup_backup_restore_repair_diagnostics_gate.cpp": (
                "DPC_CLEANUP_BACKUP_RESTORE_REPAIR_DIAGNOSTICS_GATE",
                "cleanup_horizon",
                "backup_allowed_cleanup_backlog_documented",
                "repair",
            ),
            "tests/database_lifecycle/dpc_index_validation_repair_tooling_gate.cpp": (
                "index_validation_repair",
                "management_api",
                "DPC-046 SBLR route evidence missing",
                "restore movement verification route",
            ),
        },
    },
    {
        "row_id": "configuration_validation_reload_surface",
        "surface": "configuration_validation",
        "operational_policy": (
            "configuration validation and reload expose explicit config source, "
            "policy generation, security provider generation, stale-policy refusal, "
            "and provider unavailable diagnostics"
        ),
        "required_evidence": (
            "config_validation",
            "security_summary",
            "diagnostics",
            "admin_command",
        ),
        "behavior_gates": (
            "database_lifecycle_config_policy_security_provider_conformance",
            "database_lifecycle_admin_cli_conformance",
        ),
        "evidence_files": {
            "tests/database_lifecycle/config_policy_security_provider_conformance.cpp": (
                "config source epoch mismatch",
                "ENGINE.DBLC_STALE_POLICY_REFUSED",
                "ENGINE.DBLC_AUTHORITY_BYPASS_REFUSED",
                "ENGINE.DBLC_SECURITY_PROVIDER_UNAVAILABLE",
            ),
            "src/server/manager_control.cpp": (
                "validate_server_config",
                "reload_server_config",
                "OBS_CONFIG_INSPECT",
                "OBS_CONFIG_CONTROL",
            ),
            "src/server/config.hpp": (
                "security_authority_mode",
                "security_provider_family",
                "memory_policy_name",
                "selected_config_source",
            ),
        },
    },
    {
        "row_id": "metrics_logs_audit_diagnostics_surface",
        "surface": "metrics_logs_audit_diagnostics",
        "operational_policy": (
            "metrics, structured logs, audit events, diagnostic message vectors, "
            "parser-rendered diagnostics, and public/private redaction boundaries "
            "are emitted as operational evidence"
        ),
        "required_evidence": (
            "metrics",
            "structured_logs",
            "diagnostics",
            "security_summary",
        ),
        "behavior_gates": (
            "database_lifecycle_observability_conformance",
            "dpc_management_observability_support_bundle_gate",
        ),
        "evidence_files": {
            "src/server/server_observability.cpp": (
                "ServerMetricsSnapshotJson",
                "RecordServerAuditEvent",
                "RecordServerLog",
                "sys.metrics.lifecycle.diagnostic_total",
            ),
            "tests/database_lifecycle/observability_conformance.cpp": (
                "public diagnostic vector missing public shape",
                "private diagnostic vector missing private shape",
                "engine lifecycle metric not exposed through sys.metrics.current",
                "parser rendered lifecycle diagnostic missing retryability",
            ),
            "tests/database_lifecycle/dpc_management_observability_support_bundle_gate.cpp": (
                "metric_sample_count",
                "audit_event_count",
                "DPC-061 result missing audit-surface evidence",
                "DPC-061 lifecycle metric record failed",
            ),
        },
    },
    {
        "row_id": "support_bundle_export_redaction_surface",
        "surface": "support_bundle_redaction_export",
        "operational_policy": (
            "support-bundle generation flushes operational evidence, redacts protected "
            "material and paths, records tamper checksums, fails closed on export "
            "write failure, and refuses unauthorized protected-material export"
        ),
        "required_evidence": (
            "support_bundle",
            "secret_redaction",
            "diagnostics",
            "structured_logs",
        ),
        "behavior_gates": (
            "engine_listener_support_bundle_conformance",
            "engine_listener_support_bundle_redaction_gate",
            "database_lifecycle_supportability_evidence_conformance",
        ),
        "evidence_files": {
            "tests/database_lifecycle/supportability_evidence_conformance.cpp": (
                "SUPPORTABILITY.FLUSH_COMPLETE",
                "OPS.SUPPORT_BUNDLE.EXPORT_COMPLETE",
                "forbidden_fields_absent",
                "support bundle export succeeded after evidence write failure",
            ),
            "tests/engine_listener_enterprise/engine_listener_support_bundle_conformance.cpp": (
                "support_bundle_is_authority",
                "parser worker fault history must be bounded",
                "SUPPORT_BUNDLE",
                "support bundle leaked",
            ),
            "tests/engine_listener_enterprise/engine_listener_support_bundle_redaction_gate.cpp": (
                "include_plaintext_secret",
                "redaction_disabled",
                "missing_security_context",
                "protected_material_memory",
            ),
        },
    },
    {
        "row_id": "resource_leak_orphan_runtime_surface",
        "surface": "resource_leak_orphan_detection",
        "operational_policy": (
            "bounded stress diagnostics prove no unbounded resource growth, leaked "
            "sessions, auth contexts, cursors, scheduler queues, quota reservations, "
            "or leftover listener lifecycle/socket artifacts"
        ),
        "required_evidence": (
            "leak_detection",
            "descriptor_leak_detection",
            "stale_lock_lifecycle",
            "support_bundle",
        ),
        "behavior_gates": (
            "database_lifecycle_bounded_stress_resource_leak_conformance",
            "dpc_soak_leak_resource_stability_gate",
            "engine_listener_owner_lifecycle_artifact_conformance",
        ),
        "evidence_files": {
            "tests/database_lifecycle/bounded_stress_resource_leak_conformance.cpp": (
                "Phase 7H file descriptor growth exceeded bounded allowance",
                "Phase 7H session leaked after disconnect",
                "Phase 7H auth context leaked after disconnect",
                ".sock",
            ),
            "tests/database_lifecycle/dpc_soak_leak_resource_stability_gate.cpp": (
                "DPC-070 file descriptor growth exceeded bounded allowance",
                "DPC-070 server support-bundle export leaked local fixture path",
                "DPC-070 scheduler queue proxy leaked after loop",
                "DPC-070 support-bundle leaked unsafe or execution_plan runtime data",
            ),
            "tests/engine_listener_enterprise/engine_listener_owner_lifecycle_artifact_conformance.cpp": (
                "owner token write ignored held artifact lock",
                "valid live owner token did not block",
                "tampered lifecycle token still validated",
                "stale lifecycle temp file was not removed",
            ),
        },
    },
    {
        "row_id": "filespace_transaction_security_summary_surface",
        "surface": "filespace_transaction_security_summaries",
        "operational_policy": (
            "operator-visible summaries expose filespace preallocation, transaction "
            "cleanup horizons, security epochs, resource epochs, redacted support "
            "bundle state, and authority-boundary denial flags"
        ),
        "required_evidence": (
            "filespace_summary",
            "transaction_summary",
            "security_summary",
            "metrics",
        ),
        "behavior_gates": (
            "cdp_user_observability_surface_gate",
            "dpc_management_observability_support_bundle_gate",
            "dpc_authoritative_cleanup_horizon_service_gate",
        ),
        "evidence_files": {
            "tests/database_lifecycle/cdp_user_observability_surface_gate.cpp": (
                "filespace_preallocation_granted_pages",
                "oldest_interesting_transaction_id",
                "security_epoch",
                "support_bundle_completeness_state",
            ),
            "tests/database_lifecycle/dpc_management_observability_support_bundle_gate.cpp": (
                "cleanup_horizon_authority_status",
                "oldest_active_transaction_id",
                "security_epoch",
                "wal_recovery_authority",
            ),
            "tests/database_lifecycle/dpc_authoritative_cleanup_horizon_service_gate.cpp": (
                "DPC_AUTHORITATIVE_CLEANUP_HORIZON_SERVICE_GATE",
                "cleanup_horizon",
                "inventory_authoritative",
            ),
        },
    },
    {
        "row_id": "index_readiness_diagnostics_surface",
        "surface": "index_readiness_summary",
        "operational_policy": (
            "index readiness summaries tie every family to provider classification, "
            "DML route capability, metric producer status, crash/corruption/cleanup "
            "evidence, support-bundle rows, and explicit non-authority flags"
        ),
        "required_evidence": (
            "index_summary",
            "metrics",
            "support_bundle",
            "repair_verify",
        ),
        "behavior_gates": (
            "dpc_index_validation_repair_tooling_gate",
            "ceic_040_index_operation_metrics_support_bundle_gate",
            "dpc_management_observability_support_bundle_gate",
        ),
        "evidence_files": {
            "tests/database_lifecycle/dpc_index_validation_repair_tooling_gate.cpp": (
                "DPC-046 validate success was not admitted",
                "DPC-046 SBLR route evidence missing",
                "DPC-046 management route did not admit repair",
                "DPC-046 restore movement verification route did not admit copy equivalence",
            ),
            "tests/consolidated_enterprise/ceic_040_index_operation_metrics_support_bundle_gate.cpp": (
                "operation_metrics",
                "support_bundle",
                "freshness_microseconds",
                "authority_boundary",
            ),
            "tests/database_lifecycle/dpc_management_observability_support_bundle_gate.cpp": (
                "index_delta_backlog_count",
                "index_garbage_backlog_count",
                "secondary_index_state",
                "index_state_authority_source",
            ),
        },
    },
    {
        "row_id": "optimizer_readiness_diagnostics_surface",
        "surface": "optimizer_readiness_summary",
        "operational_policy": (
            "optimizer readiness summaries bind live routes, correctness oracles, "
            "crash reopen persistence, metrics feedback, plan-cache state, memory "
            "feedback, index readiness coupling, and cluster external-provider-only "
            "boundaries"
        ),
        "required_evidence": (
            "optimizer_summary",
            "metrics",
            "support_bundle",
            "diagnostics",
        ),
        "behavior_gates": (
            "dpc_plan_stability_statistics_lifecycle_gate",
            "cdp_user_observability_surface_gate",
            "dpc_management_observability_support_bundle_gate",
        ),
        "evidence_files": {
            "tests/database_lifecycle/dpc_plan_stability_statistics_lifecycle_gate.cpp": (
                "plan_stability",
                "optimizer_statistics",
                "explain",
                "support_bundle",
            ),
            "tests/database_lifecycle/cdp_user_observability_surface_gate.cpp": (
                "selected_join_plan_summary",
                "plan_cache_hits",
                "stale_statistics_fail_safe_active",
                "management_api_ready",
            ),
        },
    },
    {
        "row_id": "parser_worker_lifecycle_fault_summary_surface",
        "surface": "parser_worker_lifecycle_fault_summaries",
        "operational_policy": (
            "parser-worker lifecycle diagnostics expose restart backoff, quarantine, "
            "fault history, pool pressure, and bounded support-bundle evidence without "
            "promoting parser authority over engine/listener state"
        ),
        "required_evidence": (
            "readiness_check",
            "support_bundle",
            "diagnostics",
            "stale_lock_lifecycle",
        ),
        "behavior_gates": (
            "engine_listener_parser_pool_lifecycle_conformance",
            "engine_listener_support_bundle_conformance",
        ),
        "evidence_files": {
            "tests/engine_listener_enterprise/engine_listener_parser_pool_lifecycle_conformance.cpp": (
                "parser_pool",
                "RunConfigValidationProof",
                "LISTENER.CONFIG.INVALID_CHILD_QUARANTINE",
                "quarantine",
                "running_workers",
                "fault_history",
            ),
            "tests/engine_listener_enterprise/engine_listener_support_bundle_conformance.cpp": (
                "parser worker fault history must be bounded",
                "parser_worker_faults",
                "handoff_failures",
                "support_bundle_is_authority",
            ),
        },
    },
    {
        "row_id": "integrated_support_bundle_metrics_surface",
        "surface": "integrated_support_bundle_metrics",
        "operational_policy": (
            "integrated operational proof ties metric producer coverage, memory, "
            "index, optimizer, and agent support-bundle sections into bounded "
            "redacted evidence with no static, sidecar-only, fixture-only, or local "
            "cluster production claims"
        ),
        "required_evidence": (
            "support_bundle",
            "metrics",
            "secret_redaction",
            "leak_detection",
        ),
        "behavior_gates": (
            "dpc_management_observability_support_bundle_gate",
            "dpc_soak_leak_resource_stability_gate",
            "database_lifecycle_supportability_evidence_conformance",
        ),
        "evidence_files": {
            "tests/database_lifecycle/dpc_management_observability_support_bundle_gate.cpp": (
                "support_bundle_performance_optimization_surface",
                "index_garbage_backlog_count",
                "support_bundle_completeness_state",
                "DPC-061 support-bundle API JSON missing index backlog",
            ),
            "tests/database_lifecycle/dpc_soak_leak_resource_stability_gate.cpp": (
                "DPC-070 support-bundle leaked unsafe or execution_plan runtime data",
                "DPC-070 file descriptor growth exceeded bounded allowance",
                "DPC-070 management/support-bundle readiness flags missing",
                "DPC-070 foreground p95 latency exceeded bounded allowance",
            ),
            "tests/database_lifecycle/supportability_evidence_conformance.cpp": (
                "forbidden_fields_absent",
                "support bundle leaked protected material or local paths",
                "support bundle export succeeded after evidence write failure",
            ),
        },
    },
)

RELEASE_ARTIFACT_TRUST_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "provenance_attestation_build_inputs",
        "surface": "build_provenance_attestation",
        "release_policy": (
            "release artifacts must be traceable to public-tree inputs, build "
            "metadata, compiler metadata, deterministic attestation subjects, and "
            "database-lifecycle provenance evidence without depending on execution-plans "
            "or private source paths"
        ),
        "required_evidence": (
            "build_provenance",
            "build_inputs",
            "toolchain_attestation",
        ),
        "behavior_gates": (
            "public_release_attestation_gate",
            "database_lifecycle_release_provenance_sbom_license_gate",
        ),
        "evidence_files": {
            "project/tools/release/public_release_attestation_gate.py": (
                "build_metadata",
                "compiler_metadata",
                "attestation_subjects",
                "public_release_attestation_gate=passed",
            ),
            "project/tests/database_lifecycle/release_provenance_sbom_license_gate.py": (
                "validate_checksums",
                "validate_sbom",
                "validate_licenses",
                "release_provenance_sbom_license_gate=passed",
            ),
        },
    },
    {
        "row_id": "dependency_sbom_license_vulnerability_scan",
        "surface": "dependency_sbom_license_vulnerability",
        "release_policy": (
            "release dependency evidence must include SBOM rows, dependency rows, "
            "license classification, generated artifact inventory, and an explicit "
            "offline vulnerability scan row for every component and dependency"
        ),
        "required_evidence": (
            "sbom",
            "dependency_license_review",
            "vulnerability_scan",
            "generated_artifact_inventory",
        ),
        "behavior_gates": ("public_dependency_sbom_gate",),
        "evidence_files": {
            "project/tools/release/public_dependency_sbom.py": (
                "license_inventory",
                "dependency_inventory",
                "vulnerability_scan",
                "offline_public_release_advisory_baseline_v1",
                "known_vulnerability_status",
            ),
        },
    },
    {
        "row_id": "reproducible_export_archive",
        "surface": "reproducible_export",
        "release_policy": (
            "release packaging must regenerate a public export twice, compare "
            "canonical and raw checksums, include cleanup manifests, and refuse "
            "private inputs or nondeterministic generated matrices"
        ),
        "required_evidence": (
            "reproducible_build",
            "reproducible_archive",
            "cleanup_manifest",
            "private_reference_scan",
        ),
        "behavior_gates": (
            "public_artifact_reproducibility_gate",
            "public_project_export_gate",
        ),
        "evidence_files": {
            "project/tools/release/public_reproducible_export.py": (
                "PUBLIC_REPRODUCIBLE_EXPORT",
                "deterministic_two_pass_generation",
                "canonical_sha256",
                "raw_sha256",
                "public_reproducible_export=passed",
            ),
            "project/tools/release/public_project_export_gate.py": (
                "copy_public_tree",
                "scan_private_references",
                "write_cleanup_outputs",
                "public export contains forbidden private references",
            ),
        },
    },
    {
        "row_id": "signature_checksum_release_artifacts",
        "surface": "signature_checksum_artifacts",
        "release_policy": (
            "release artifacts must be checksum-addressed, signature-ready, "
            "cryptographically policy-bound, and fail closed on checksum mismatch, "
            "weak checksum metadata, missing signing policy, or private inputs"
        ),
        "required_evidence": (
            "signature_ready",
            "checksums",
            "crypto_policy",
        ),
        "behavior_gates": (
            "public_artifact_signature_gate",
            "public_crypto_entropy_policy_gate",
        ),
        "evidence_files": {
            "project/tools/release/public_artifact_signature_gate.py": (
                "PUBLIC_RELEASE_ARTIFACT_SIGNING",
                "signature-ready-ed25519",
                "public-release-artifact-checksums.txt",
                "RELEASE.ARTIFACT.CHECKSUM_MISMATCH",
                "public_artifact_signature_gate=passed",
            ),
            "project/tests/release/public_crypto_entropy_policy_gate.py": (
                "signature_ready_metadata",
                "approved_hash",
                "approved_mac",
                "weak_checksums_authority",
            ),
        },
    },
    {
        "row_id": "compiler_hardening_sanitizer_static_analysis",
        "surface": "compiler_hardening_sanitizer_static",
        "release_policy": (
            "release hardening proof must compile warning probes under "
            "warnings-as-errors, exercise sanitizer profiles, run static analysis "
            "probes, and record source hygiene findings without hidden suppressions"
        ),
        "required_evidence": (
            "compiler_hardening",
            "sanitizer_static",
            "warnings_as_errors",
        ),
        "behavior_gates": ("public_sanitizer_static_analysis_gate",),
        "evidence_files": {
            "project/tools/release/public_sanitizer_static_analysis_gate.py": (
                "-fsanitize=address,undefined",
                "-fsanitize=thread",
                "warnings_as_errors",
                "clang-tidy",
                "cppcheck",
                "public_sanitizer_static_analysis_gate=passed",
            ),
        },
    },
    {
        "row_id": "version_metadata_debug_symbol_policy",
        "surface": "version_metadata_debug_symbols",
        "release_policy": (
            "release metadata must be deterministic, bind project and durable "
            "format versions, refuse future formats, and expose a per-platform "
            "separate debug-symbol package policy"
        ),
        "required_evidence": (
            "deterministic_versioning",
            "compatibility_metadata",
            "debug_symbols",
        ),
        "behavior_gates": (
            "public_release_version_metadata_gate",
            "public_platform_matrix_gate",
        ),
        "evidence_files": {
            "project/tools/release/public_release_version_metadata.cpp": (
                "SB_PUBLIC_RELEASE_PROJECT_VERSION",
                "database_format",
                "catalog_record",
                "datatype_wire",
                "public_release_version_metadata=passed",
            ),
            "project/tools/release/public_platform_matrix_gate.py": (
                "debug_symbol_payload_root",
                "allowed_debug_symbol_roles",
                "separate_debug_symbols_required_for_release_artifacts",
                "layout_debug_symbol_policy_missing",
            ),
            "release/linux/ENGINE_BINARY_LAYOUT.json": (
                "debug_symbol_payload_root",
                "engine_debug_symbols",
                "separate_debug_symbols_required_for_release_artifacts",
            ),
            "release/windows/ENGINE_BINARY_LAYOUT.json": (
                "debug_symbol_payload_root",
                "engine_debug_symbols",
                "separate_debug_symbols_required_for_release_artifacts",
            ),
            "release/freebsd/ENGINE_BINARY_LAYOUT.json": (
                "debug_symbol_payload_root",
                "engine_debug_symbols",
                "separate_debug_symbols_required_for_release_artifacts",
            ),
        },
    },
    {
        "row_id": "release_notes_upgrade_migration_limitations",
        "surface": "release_notes_upgrade_migration",
        "release_policy": (
            "public release documentation must make release notes, upgrade notes, "
            "migration notes, known limitations, initial platform status, and "
            "first-release package exclusions explicit"
        ),
        "required_evidence": (
            "release_notes",
            "upgrade_notes",
            "migration_notes",
            "known_limitations",
        ),
        "behavior_gates": (
            "public_admin_runbook_gate",
            "public_api_abi_compat_gate",
        ),
        "evidence_files": {
            "release/README.md": (
                "Release notes",
                "upgrade notes",
                "migration notes",
                "known limitations",
                "external-provider-only",
            ),
            "project/tools/release/public_compatibility_policy_check.py": (
                "semantic_versioning_required",
                "deprecation_requires_stable_diagnostic_or_manifest_row",
                "patch_symbol_add_remove_forbidden",
                "public_api_abi_compat_gate=passed",
            ),
        },
    },
    {
        "row_id": "install_uninstall_cleanup_service_hardening",
        "surface": "install_uninstall_cleanup",
        "release_policy": (
            "release packages must prove scoped install and uninstall behavior, "
            "service identity hardening, permissions, no world-writable runtime "
            "paths, secure defaults, and support-bundle redaction anchors"
        ),
        "required_evidence": (
            "package_cleanup",
            "install_uninstall",
            "service_hardening",
        ),
        "behavior_gates": ("public_install_service_hardening_gate",),
        "evidence_files": {
            "project/tools/release/public_install_service_hardening_gate.py": (
                "engine_only_install_boundary",
                "scoped_uninstall_cleanup",
                "no_world_writable_runtime_paths",
                "support_bundle_redaction",
                "public_install_service_hardening_gate=passed",
            ),
        },
    },
    {
        "row_id": "linux_windows_x64_freebsd_platform_matrix",
        "surface": "platform_matrix",
        "release_policy": (
            "release platform support must name Linux, Windows x64, and FreeBSD "
            "contracts, validate current Linux toolchain configuration, and avoid "
            "implying production readiness on platforms whose execution proof is "
            "still pending"
        ),
        "required_evidence": (
            "platform_matrix",
            "cross_platform_pending",
        ),
        "behavior_gates": ("public_platform_matrix_gate",),
        "evidence_files": {
            "project/tools/release/public_platform_matrix_gate.py": (
                "REQUIRED_PLATFORMS",
                "linux",
                "windows",
                "freebsd",
                "no_cross_platform_support_claim_from_current_host",
                "public_platform_matrix_gate=passed",
            ),
            "release/README.md": (
                "Linux is the first validated execution platform",
                "declared compatibility targets",
                "cross-platform proof pending",
            ),
        },
    },
    {
        "row_id": "public_export_private_reference_fence",
        "surface": "public_export_private_reference_fence",
        "release_policy": (
            "public export proof must scan for private repository paths, execution_plan "
            "dependencies, local machine references, repository metadata, and "
            "forbidden documentation roots before an artifact can be trusted"
        ),
        "required_evidence": (
            "private_reference_scan",
            "public_inputs",
            "release_export",
        ),
        "behavior_gates": (
            "public_project_export_gate",
            "public_release_attestation_gate",
        ),
        "evidence_files": {
            "project/tools/release/public_project_export_gate.py": (
                '("docs", "execution-plans")',
                '("docs", "completed-execution-plans")',
                '("docs", "findings")',
                "scan_private_references",
                "public export contains repository metadata",
            ),
            "project/tools/release/public_release_attestation_gate.py": (
                "public_tree_inputs_only",
                "release_attestation_is_evidence_only",
                "private_reference_recorded",
            ),
        },
    },
    {
        "row_id": "cluster_stub_external_provider_boundary",
        "surface": "cluster_stub_release_boundary",
        "release_policy": (
            "cluster-positive calls are compile-time gated; when admitted they "
            "route to the public compile-link stub or external provider boundary, "
            "with no open-core private cluster execution or production-live stub "
            "claims"
        ),
        "required_evidence": (
            "cluster_stub_boundary",
            "compile_time_flags",
            "external_provider_boundary",
        ),
        "behavior_gates": (
            "public_cluster_provider_boundary_cleanup_gate",
            "public_cluster_boundary_cleanup_audit_gate",
            "public_platform_matrix_gate",
        ),
        "evidence_files": {
            "project/tools/release/public_cluster_boundary_cleanup_audit.py": (
                "compile_link_stub",
                "external_provider_only",
                "production live cluster behavior requires external sb_cluster_provider",
                "cluster stub live claims are forbidden in production builds",
                "public_cluster_boundary_cleanup_audit=passed",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_cluster_provider_boundary_cleanup_gate",
                "public_cluster_boundary_cleanup_audit_gate",
                "compile_link_stub",
                "ELER-108",
            ),
            "project/CMakeLists.txt": (
                "SB_ENABLE_CLUSTER_PROVIDER",
                "SB_CLUSTER_PROVIDER_STUB",
                "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
                "SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS",
            ),
        },
    },
)

ENTERPRISE_DOCUMENTATION_SECTIONS: tuple[str, ...] = (
    "DOC_ARCHITECTURE_OVERVIEW",
    "DOC_TRUST_BOUNDARY_MODEL",
    "DOC_PARSER_LISTENER_ENGINE_BOUNDARY",
    "DOC_STARTUP_OPEN_LIFECYCLE",
    "DOC_FILE_FORMAT_OVERVIEW",
    "DOC_TRANSACTION_MGA_MODEL",
    "DOC_ISOLATION_SEMANTICS",
    "DOC_CRASH_RECOVERY_MODEL",
    "DOC_SECURITY_MODEL",
    "DOC_AUTH_PROVIDER_MODEL",
    "DOC_AUTHORIZATION_MODEL",
    "DOC_PAGE_INDEX_TOAST_MODEL",
    "DOC_OPTIMIZER_MODEL",
    "DOC_MEMORY_GOVERNANCE_MODEL",
    "DOC_CONFIGURATION_REFERENCE",
    "DOC_OPERATIONAL_RUNBOOK",
    "DOC_TROUBLESHOOTING_GUIDE",
    "DOC_SUPPORT_BUNDLE_GUIDE",
    "DOC_UPGRADE_GUIDE",
    "DOC_BACKUP_RESTORE_INTERACTION_GUIDE",
    "DOC_LIMITATIONS_SUPPORT_STATES",
)

ENTERPRISE_DOCUMENTATION_STATUS_TERMS: tuple[str, ...] = (
    "production-supported",
    "provider-required",
    "cluster-provider-only",
    "disabled-by-default",
    "diagnostic-only",
    "evidence-only",
    "experimental",
    "unsupported",
)

ENTERPRISE_DOCUMENTATION_LABEL_REQUIRED_GATES = {
    "engine_listener_enterprise_documentation_gate",
    "public_admin_runbook_gate",
    "public_api_abi_compat_gate",
    "public_doc_consistency_gate",
    "public_enterprise_documentation_gate",
}

ENTERPRISE_DOCUMENTATION_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "architecture_trust_boundary",
        "surface": "architecture_trust_boundary",
        "required_sections": (
            "DOC_ARCHITECTURE_OVERVIEW",
            "DOC_TRUST_BOUNDARY_MODEL",
            "DOC_PARSER_LISTENER_ENGINE_BOUNDARY",
        ),
        "documentation_policy": (
            "architecture documentation must separate listener parser-worker "
            "and engine authority while proving that parser evidence cannot "
            "become authentication authorization transaction or recovery finality"
        ),
        "behavior_gates": (
            "public_enterprise_documentation_gate",
            "public_doc_consistency_gate",
            "engine_listener_integrated_product_proof_gate",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "ENGINE_LISTENER_ENTERPRISE_DOCS",
                "DOC_ARCHITECTURE_OVERVIEW",
                "DOC_TRUST_BOUNDARY_MODEL",
                "DOC_PARSER_LISTENER_ENGINE_BOUNDARY",
            ),
            "project/tools/release/public_enterprise_documentation_gate.py": (
                "PUBLIC_ENTERPRISE_DOCUMENTATION_GATE",
                "DOC_ARCHITECTURE_OVERVIEW",
                "DOC_TRUST_BOUNDARY_MODEL",
                "DOC_PARSER_LISTENER_ENGINE_BOUNDARY",
            ),
            "project/tests/engine_listener_enterprise/CMakeLists.txt": (
                "engine_listener_integrated_product_proof_gate",
                "engine_listener_dbbt_lpreface_binding_conformance",
                "engine_listener_parser_contract_freeze_gate",
            ),
        },
    },
    {
        "row_id": "lifecycle_format_recovery",
        "surface": "lifecycle_format_recovery",
        "required_sections": (
            "DOC_STARTUP_OPEN_LIFECYCLE",
            "DOC_FILE_FORMAT_OVERVIEW",
            "DOC_CRASH_RECOVERY_MODEL",
            "DOC_UPGRADE_GUIDE",
        ),
        "documentation_policy": (
            "lifecycle documentation must cover startup open shutdown durable "
            "format versioning upgrade refusal and crash recovery outcomes "
            "without promising silent repair or undocumented downgrade support"
        ),
        "behavior_gates": (
            "public_enterprise_documentation_gate",
            "engine_listener_crash_recovery_certification_gate",
            "engine_listener_compatibility_upgrade_downgrade_gate",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "DOC_STARTUP_OPEN_LIFECYCLE",
                "DOC_FILE_FORMAT_OVERVIEW",
                "DOC_CRASH_RECOVERY_MODEL",
                "DOC_UPGRADE_GUIDE",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_release_version_metadata_gate",
                "public_upgrade_migration_gate",
                "public_crash_fault_gate",
            ),
            "project/tests/database_lifecycle/CMakeLists.txt": (
                "database_lifecycle_shutdown_conformance",
                "database_lifecycle_upgrade_migration_conformance",
            ),
        },
    },
    {
        "row_id": "mga_isolation_storage_index_toast",
        "surface": "mga_storage_index_toast",
        "required_sections": (
            "DOC_TRANSACTION_MGA_MODEL",
            "DOC_ISOLATION_SEMANTICS",
            "DOC_PAGE_INDEX_TOAST_MODEL",
        ),
        "documentation_policy": (
            "storage and transaction documentation must state that MGA "
            "transaction inventory and row visibility remain final authority "
            "while indexes optimizer evidence and page state are never finality"
        ),
        "behavior_gates": (
            "public_enterprise_documentation_gate",
            "engine_listener_fuzz_property_invariant_suite_gate",
            "engine_listener_mga_integrated_physical_cleanup_conformance",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "DOC_TRANSACTION_MGA_MODEL",
                "DOC_ISOLATION_SEMANTICS",
                "DOC_PAGE_INDEX_TOAST_MODEL",
                "Page finality is not transaction finality",
                "Index candidates can accelerate lookup",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_transaction_mga_cow_gate",
                "public_page_body_checksum_agreement_gate",
                "public_toast_overflow_binary_descriptor_gate",
            ),
            "project/tests/fault_injection/CMakeLists.txt": (
                "transaction_inventory_publish_fault_conformance",
            ),
            "project/tests/engine_listener_enterprise/CMakeLists.txt": (
                "engine_listener_serializable_isolation_conformance",
                "engine_listener_index_family_dml_route_conformance",
            ),
        },
    },
    {
        "row_id": "security_auth_authorization",
        "surface": "security_auth_authorization",
        "required_sections": (
            "DOC_SECURITY_MODEL",
            "DOC_AUTH_PROVIDER_MODEL",
            "DOC_AUTHORIZATION_MODEL",
            "DOC_SUPPORT_BUNDLE_GUIDE",
        ),
        "documentation_policy": (
            "security documentation must distinguish authentication provider "
            "evidence durable authorization authority protected material "
            "support-bundle redaction and management authorization boundaries"
        ),
        "behavior_gates": (
            "public_enterprise_documentation_gate",
            "engine_listener_adversarial_security_validation_gate",
            "engine_listener_support_bundle_redaction_gate",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "DOC_SECURITY_MODEL",
                "DOC_AUTH_PROVIDER_MODEL",
                "DOC_AUTHORIZATION_MODEL",
                "DOC_SUPPORT_BUNDLE_GUIDE",
                "Explicit deny overrides allow",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_security_provider_contract_protected_material_gate",
                "public_authorization_durable_flow_gate",
                "public_enterprise_threat_gate",
            ),
            "project/tests/engine_listener_enterprise/CMakeLists.txt": (
                "engine_listener_materialized_authorization_conformance",
                "engine_listener_support_bundle_redaction_gate",
            ),
        },
    },
    {
        "row_id": "optimizer_memory_configuration",
        "surface": "optimizer_memory_configuration",
        "required_sections": (
            "DOC_OPTIMIZER_MODEL",
            "DOC_MEMORY_GOVERNANCE_MODEL",
            "DOC_CONFIGURATION_REFERENCE",
        ),
        "documentation_policy": (
            "optimizer memory and configuration documentation must make "
            "evidence-only and fail-closed boundaries explicit while tying "
            "plans reservations defaults and cluster provider flags to proof"
        ),
        "behavior_gates": (
            "public_enterprise_documentation_gate",
            "engine_listener_operational_readiness_gate",
            "engine_listener_release_profile_completeness_gate",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "DOC_OPTIMIZER_MODEL",
                "DOC_MEMORY_GOVERNANCE_MODEL",
                "DOC_CONFIGURATION_REFERENCE",
                "Optimizer plans are evidence",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_optimizer_catalog_backed_planning_gate",
                "public_query_memory_reservation_gate",
                "public_secure_defaults_gate",
            ),
            "project/tests/optimizer/CMakeLists.txt": (
                "optimizer_enterprise_plan_cache_gate",
            ),
        },
    },
    {
        "row_id": "operations_troubleshooting_runbooks",
        "surface": "operations_troubleshooting_runbooks",
        "required_sections": (
            "DOC_OPERATIONAL_RUNBOOK",
            "DOC_TROUBLESHOOTING_GUIDE",
        ),
        "documentation_policy": (
            "operations documentation must cover runbooks diagnostics health "
            "readiness supportability and troubleshooting without claiming "
            "that operator evidence changes durable engine authority"
        ),
        "behavior_gates": (
            "public_enterprise_documentation_gate",
            "public_admin_runbook_gate",
            "engine_listener_operational_readiness_gate",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "DOC_OPERATIONAL_RUNBOOK",
                "DOC_TROUBLESHOOTING_GUIDE",
                "Runbook output is evidence-only",
            ),
            "project/docs/admin/PUBLIC_ADMIN_RUNBOOKS.md": (
                "PUBLIC_ADMIN_RUNBOOKS",
                "RUNBOOK_CREATE_DATABASE",
                "RUNBOOK_UPGRADE",
            ),
            "project/tools/release/public_runbook_consistency_check.py": (
                "PUBLIC_RUNBOOK_CONSISTENCY_CHECK",
                "RUNBOOK_CREATE_DATABASE",
                "RUNBOOK_UPGRADE",
            ),
        },
    },
    {
        "row_id": "backup_restore_limitations_support_states",
        "surface": "backup_restore_limitations_support_states",
        "required_sections": (
            "DOC_BACKUP_RESTORE_INTERACTION_GUIDE",
            "DOC_LIMITATIONS_SUPPORT_STATES",
        ),
        "documentation_policy": (
            "backup restore limitation and support-state documentation must "
            "state Linux-first proof Windows x64 and FreeBSD pending proof "
            "unsupported feature refusal and provider-required cluster behavior"
        ),
        "behavior_gates": (
            "public_enterprise_documentation_gate",
            "public_api_abi_compat_gate",
            "public_platform_matrix_gate",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "DOC_BACKUP_RESTORE_INTERACTION_GUIDE",
                "DOC_LIMITATIONS_SUPPORT_STATES",
                "Linux is the first fully proven platform lane",
                "cluster-provider-only",
            ),
            "project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md": (
                "PUBLIC_SUPPORT_RELEASE_LIFECYCLE",
                "Supported Platforms",
                "target platform pending native CI/runtime proof",
                "target platform pending native runner proof",
                "Unsupported Features",
            ),
            "project/docs/public_api/CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md": (
                "PUBLIC_API_COMPATIBILITY_POLICY",
                "Compatibility Surfaces",
            ),
        },
    },
    {
        "row_id": "documentation_support_state_taxonomy",
        "surface": "support_state_taxonomy",
        "required_sections": ("DOC_LIMITATIONS_SUPPORT_STATES",),
        "documentation_policy": (
            "documentation must use explicit support-state terms for "
            "implemented experimental provider-required cluster-only disabled "
            "diagnostic evidence and unsupported surfaces"
        ),
        "behavior_gates": (
            "public_enterprise_documentation_gate",
            "public_doc_consistency_gate",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "production-supported",
                "provider-required",
                "cluster-provider-only",
                "disabled-by-default",
                "diagnostic-only",
                "evidence-only",
                "experimental",
                "unsupported",
            ),
            "project/tools/release/public_enterprise_documentation_gate.py": (
                "STATUS_TERMS",
                "documentation_is_runtime_authority",
            ),
        },
    },
)

SUPPORT_MAINTENANCE_SECTIONS: tuple[str, ...] = (
    "SUPPORT_LIFECYCLE_POLICY",
    "SECURITY_UPDATE_PROCESS",
    "CVE_HANDLING_PROCESS",
    "DISCLOSURE_POLICY",
    "PATCH_RELEASE_PROCESS",
    "COMPATIBILITY_POLICY",
    "DATA_LOSS_ESCALATION_POLICY",
    "RELEASE_ROLLBACK_POLICY",
    "DIAGNOSTIC_COLLECTION_POLICY",
    "ENTERPRISE_SLA_BOUNDARIES",
    "ALPHA_BETA_GA_SUPPORT_BOUNDARIES",
    "COMMUNITY_COMMERCIAL_SUPPORT_BOUNDARY",
)

SUPPORT_MAINTENANCE_LABEL_REQUIRED_GATES = {
    "engine_listener_support_maintenance_policy_gate",
    "public_api_abi_compat_gate",
    "public_audit_privacy_gate",
    "public_disaster_recovery_gate",
    "public_support_bundle_incident_gate",
    "public_support_policy_gate",
}

SUPPORT_MAINTENANCE_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "support_lifecycle_sla_boundaries",
        "surface": "support_lifecycle_sla",
        "required_sections": (
            "SUPPORT_LIFECYCLE_POLICY",
            "ENTERPRISE_SLA_BOUNDARIES",
            "ALPHA_BETA_GA_SUPPORT_BOUNDARIES",
            "COMMUNITY_COMMERCIAL_SUPPORT_BOUNDARY",
        ),
        "support_policy": (
            "support lifecycle and SLA documentation must define supported "
            "release lines platform lanes release-stage boundaries and "
            "community versus commercial support without making runtime claims"
        ),
        "behavior_gates": (
            "public_support_policy_gate",
            "public_platform_matrix_gate",
            "public_unsupported_feature_gate",
        ),
        "evidence_files": {
            "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md": (
                "PUBLIC_SUPPORT_MAINTENANCE_POLICY",
                "SUPPORT_LIFECYCLE_POLICY",
                "ENTERPRISE_SLA_BOUNDARIES",
                "ALPHA_BETA_GA_SUPPORT_BOUNDARIES",
                "COMMUNITY_COMMERCIAL_SUPPORT_BOUNDARY",
            ),
            "project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md": (
                "PUBLIC_SUPPORT_RELEASE_LIFECYCLE",
                "Supported Platforms",
                "First-Release Support Boundaries",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_support_policy_gate",
                "public_platform_matrix_gate",
                "public_unsupported_feature_gate",
            ),
        },
    },
    {
        "row_id": "security_cve_disclosure",
        "surface": "security_cve_disclosure",
        "required_sections": (
            "SECURITY_UPDATE_PROCESS",
            "CVE_HANDLING_PROCESS",
            "DISCLOSURE_POLICY",
        ),
        "support_policy": (
            "security support update CVE and disclosure policy must require "
            "reproducible proof redacted advisory handling patch ownership "
            "and no protected-material leakage"
        ),
        "behavior_gates": (
            "public_support_policy_gate",
            "public_enterprise_threat_gate",
            "public_audit_privacy_gate",
        ),
        "evidence_files": {
            "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md": (
                "SECURITY_UPDATE_PROCESS",
                "CVE_HANDLING_PROCESS",
                "DISCLOSURE_POLICY",
                "CVE handling requires affected-version inventory",
                "redacted public advisory text",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_enterprise_threat_gate",
                "public_audit_privacy_gate",
                "public_crypto_entropy_policy_gate",
            ),
            "project/tools/release/public_support_policy_gate.py": (
                "CVE_HANDLING_PROCESS",
                "DISCLOSURE_POLICY",
                "public_audit_privacy_gate",
            ),
        },
    },
    {
        "row_id": "patch_compatibility_release_process",
        "surface": "patch_compatibility",
        "required_sections": (
            "PATCH_RELEASE_PROCESS",
            "COMPATIBILITY_POLICY",
        ),
        "support_policy": (
            "support patch release and compatibility policy must bind version "
            "metadata reproducible artifacts SBOM vulnerability evidence "
            "stable ABI and fail-closed durable format behavior"
        ),
        "behavior_gates": (
            "public_support_policy_gate",
            "public_api_abi_compat_gate",
            "public_release_version_metadata_gate",
        ),
        "evidence_files": {
            "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md": (
                "PATCH_RELEASE_PROCESS",
                "COMPATIBILITY_POLICY",
                "SBOM/license/vulnerability scan",
                "Unsupported old formats",
            ),
            "project/docs/public_api/CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md": (
                "PUBLIC_API_COMPATIBILITY_POLICY",
                "Compatibility Surfaces",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_api_abi_compat_gate",
                "public_release_version_metadata_gate",
                "public_artifact_reproducibility_gate",
            ),
        },
    },
    {
        "row_id": "data_loss_disaster_rollback",
        "surface": "data_loss_disaster_rollback",
        "required_sections": (
            "DATA_LOSS_ESCALATION_POLICY",
            "RELEASE_ROLLBACK_POLICY",
        ),
        "support_policy": (
            "support data-loss escalation and release rollback policy must "
            "fence uncertain state rely on recovery and backup proof and "
            "refuse unsupported durable-format downgrades"
        ),
        "behavior_gates": (
            "public_support_policy_gate",
            "public_disaster_recovery_gate",
            "engine_listener_crash_recovery_certification_gate",
        ),
        "evidence_files": {
            "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md": (
                "DATA_LOSS_ESCALATION_POLICY",
                "RELEASE_ROLLBACK_POLICY",
                "Potential data-loss issues",
                "unsupported durable-format downgrade",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_disaster_recovery_gate",
                "public_backup_update_coverage_gate",
                "public_upgrade_migration_gate",
            ),
            "project/tests/engine_listener_enterprise/CMakeLists.txt": (
                "engine_listener_crash_recovery_certification_gate",
            ),
        },
    },
    {
        "row_id": "diagnostic_collection_privacy",
        "surface": "diagnostic_collection_privacy",
        "required_sections": ("DIAGNOSTIC_COLLECTION_POLICY",),
        "support_policy": (
            "diagnostic collection policy must require redacted support "
            "bundles stable diagnostic codes privacy retention controls and "
            "private-path refusal"
        ),
        "behavior_gates": (
            "public_support_policy_gate",
            "public_support_bundle_incident_gate",
            "public_audit_privacy_gate",
        ),
        "evidence_files": {
            "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md": (
                "DIAGNOSTIC_COLLECTION_POLICY",
                "redacted support bundles",
                "stable diagnostic codes",
                "private local paths",
            ),
            "project/tools/release/public_support_bundle_incident_gate.py": (
                "PUBLIC_SUPPORT_BUNDLE_INCIDENT_GATE",
                "redaction",
                "diagnostics",
            ),
            "project/tools/release/public_audit_privacy_gate.py": (
                "PUBLIC_AUDIT_PRIVACY_GATE",
                "privacy",
                "redaction",
            ),
        },
    },
    {
        "row_id": "support_policy_runtime_authority_boundary",
        "surface": "support_policy_runtime_authority_boundary",
        "required_sections": (
            "SUPPORT_LIFECYCLE_POLICY",
            "COMMUNITY_COMMERCIAL_SUPPORT_BOUNDARY",
        ),
        "support_policy": (
            "support policy must never become runtime authority and cannot "
            "promote unsupported cluster parser reference or recovery claims "
            "without implementation and regenerated proof"
        ),
        "behavior_gates": (
            "public_support_policy_gate",
            "public_enterprise_documentation_gate",
            "engine_listener_enterprise_documentation_gate",
        ),
        "evidence_files": {
            "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md": (
                "does not define storage authority",
                "Commercial support cannot turn an unsupported",
                "regenerated proof",
            ),
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "ENGINE_LISTENER_ENTERPRISE_DOCS",
                "public_release_evidence_only",
                "Documentation and runbooks are evidence-only",
            ),
            "project/tests/engine_listener_enterprise/CMakeLists.txt": (
                "engine_listener_enterprise_documentation_gate",
                "engine_listener_support_maintenance_policy_gate",
            ),
        },
    },
)

EXTERNAL_REVIEW_CLOSURE_MANIFEST = Path(
    "tests/engine_listener_enterprise/fixtures/external_review_closure_manifest.json"
)

FINAL_PROJECT_ONLY_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "reference_sblr_and_sbsql_closure",
        "surface": "reference_sblr_parser_contract",
        "closure_policy": (
            "reference SBLR interface closure and SBSQL synchronization must be "
            "regenerated from project tests, reject reference dialect paste-through, "
            "and produce parser handoff evidence without parser runtime claims"
        ),
        "behavior_gates": (
            "sblr_surface_reference_interface_closure_gate",
            "sblr_surface_sbsql_sync_guardrail_gate",
            "engine_listener_sbsql_parser_sync_gate",
            "public_sbsql_parser_sync_gate",
        ),
        "evidence_files": {
            "project/tests/sblr_surface/sblr_surface_guardrail_gate.py": (
                "validate_interface_closure",
                "validate_sbsql_sync",
                "compatibility_dialect_paste_through",
                "PUBLIC_PARSER_HANDOFF_PREFIX",
            ),
            "project/tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03/sbsql_sync_requirements.json": (
                "scratchbird.sblr_surface.sbsql_sync_requirements.v1",
                "sbsql_native_normalized",
                "compatibility_dialect_paste_through",
                "product_completion_claim",
            ),
        },
    },
    {
        "row_id": "no_skip_traceability_and_project_test_proof",
        "surface": "release_proof_integrity",
        "closure_policy": (
            "declared features, release-scope proof, row-level traceability, "
            "and public release traceability must reject skip waiver xfail "
            "disabled fixture-only unsupported-profile and private proof inputs"
        ),
        "behavior_gates": (
            "engine_listener_no_skip_waiver_xfail_release_gate",
            "engine_listener_row_level_traceability_manifest_gate",
            "public_no_skip_waiver_xfail_gate",
            "public_row_level_traceability_manifest_gate",
        ),
        "evidence_files": {
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py": (
                "validate_no_skip_waiver_xfail_release_proof",
                "validate_row_level_traceability_manifest",
                "FORBIDDEN_PROOF_REFERENCE_FRAGMENTS",
                "product_completion_claim",
            ),
            "project/tests/release/CMakeLists.txt": (
                "public_no_skip_waiver_xfail_gate",
                "public_row_level_traceability_manifest_gate",
                "ELER-083",
                "ELER-084",
            ),
        },
    },
    {
        "row_id": "parser_contract_freeze_and_determinism",
        "surface": "parser_contract_determinism",
        "closure_policy": (
            "parser-facing SBLR contracts and all generated closure resources "
            "must be versioned, hash-pinned, deterministic across two canonical "
            "generation passes, and free of private or product-completion claims"
        ),
        "behavior_gates": (
            "engine_listener_parser_contract_freeze_gate",
            "engine_listener_generated_resource_determinism_gate",
            "public_parser_contract_freeze_gate",
            "public_generated_resource_determinism_gate",
        ),
        "evidence_files": {
            "project/tests/engine_listener_enterprise/fixtures/parser_facing_contract_freeze_manifest.json": (
                "scratchbird.engine_listener.parser_facing_contract_freeze.v1",
                "public_release_policy",
                "product_completion_claim",
                "compatibility_version",
            ),
            "project/tests/engine_listener_enterprise/fixtures/generated_resource_determinism_manifest.json": (
                "scratchbird.engine_listener.generated_resource_determinism.v1",
                "parser_facing_contract_freeze_manifest",
                "external_review_closure_manifest",
            ),
        },
    },
    {
        "row_id": "release_profile_secret_redaction_and_public_export",
        "surface": "release_profile_export_hygiene",
        "closure_policy": (
            "release-complete profile, secure defaults, cluster compile-time "
            "boundary, support-bundle secret canaries, and public export private "
            "reference scans must all be CTest-owned project proof"
        ),
        "behavior_gates": (
            "engine_listener_release_profile_completeness_gate",
            "engine_listener_support_bundle_redaction_gate",
            "public_project_export_gate",
            "public_cluster_boundary_cleanup_audit_gate",
        ),
        "evidence_files": {
            "project/CMakeLists.txt": (
                "SB_NONCLUSTER_ENGINE_PROFILE",
                "SB_ENABLE_CLUSTER_PROVIDER",
                "SB_CLUSTER_PROVIDER_STUB",
                "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
            ),
            "project/tests/engine_listener_enterprise/engine_listener_support_bundle_redaction_gate.cpp": (
                "contains no canary",
                "redaction_disabled",
                "include_plaintext_secret",
                "cluster_projection_support_bundle",
            ),
            "project/tools/release/public_project_export_gate.py": (
                "scan_private_references",
                '("docs", "execution-plans")',
                '("docs", "findings")',
                "public_project_export_gate=passed",
            ),
        },
    },
    {
        "row_id": "integrated_crash_security_operations_stack",
        "surface": "integrated_enterprise_stack",
        "closure_policy": (
            "integrated product path crash recovery security fuzz performance "
            "soak compatibility operational release documentation and support "
            "proof must be registered as no-skip Linux project-test evidence"
        ),
        "behavior_gates": (
            "engine_listener_integrated_product_proof_gate",
            "engine_listener_crash_recovery_certification_gate",
            "engine_listener_adversarial_security_validation_gate",
            "engine_listener_fuzz_property_invariant_suite_gate",
            "engine_listener_operational_readiness_gate",
        ),
        "evidence_files": {
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py": (
                "validate_integrated_product_proof",
                "validate_crash_recovery_certification",
                "validate_adversarial_security_validation",
                "validate_operational_readiness",
            ),
            "project/tests/engine_listener_enterprise/CMakeLists.txt": (
                "engine_listener_integrated_product_proof_gate",
                "engine_listener_crash_recovery_certification_gate",
                "engine_listener_adversarial_security_validation_gate",
                "engine_listener_operational_readiness_gate",
            ),
        },
    },
)

EXTERNAL_REVIEW_CLOSURE_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "independent_security_review",
        "surface": "security_review",
        "review_family": "independent_security_review",
        "review_policy": (
            "security review closure evidence must cover authentication, "
            "authorization, management abuse, parser handoff races, malicious "
            "SBLR, corrupted input, protected material, and support-bundle leakage"
        ),
        "behavior_gates": (
            "engine_listener_adversarial_security_validation_gate",
            "engine_listener_fuzz_property_invariant_suite_gate",
            "public_enterprise_threat_gate",
            "public_audit_privacy_gate",
        ),
        "evidence_files": {
            "project/tests/security/enterprise_threat_abuse_suite.json": (
                "management_socket_abuse",
                "parser_worker_compromise",
                "sblr_tampering",
                "support_bundle_leakage",
            ),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py": (
                "ADVERSARIAL_SECURITY_TRACEABILITY_REF",
                "FUZZ_PROPERTY_TRACEABILITY_REF",
                "validate_adversarial_security_validation",
            ),
        },
    },
    {
        "row_id": "database_correctness_review",
        "surface": "database_correctness_review",
        "review_family": "database_correctness_review",
        "review_policy": (
            "database correctness review closure evidence must cover MGA/COW "
            "visibility, transaction inventory, crash recovery, index authority, "
            "TOAST reachability, cleanup, compatibility, and soak stability"
        ),
        "behavior_gates": (
            "engine_listener_integrated_product_proof_gate",
            "engine_listener_crash_recovery_certification_gate",
            "engine_listener_soak_certification_gate",
            "engine_listener_compatibility_upgrade_downgrade_gate",
        ),
        "evidence_files": {
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py": (
                "validate_integrated_product_proof",
                "validate_crash_recovery_certification",
                "validate_soak_certification",
                "validate_compatibility_upgrade_downgrade",
            ),
            "project/tools/ceic_reliability_security_suite.py": (
                "soak_24h",
                "soak_72h",
                "soak_7d",
                "crash_restart_cycles",
            ),
        },
    },
    {
        "row_id": "operational_reliability_review",
        "surface": "operational_reliability_review",
        "review_family": "operational_reliability_review",
        "review_policy": (
            "operational reliability review closure evidence must cover health, "
            "readiness, liveness, diagnostics, support bundles, release artifact "
            "trust, documentation, and support process proof"
        ),
        "behavior_gates": (
            "engine_listener_operational_readiness_gate",
            "engine_listener_release_artifact_trust_gate",
            "engine_listener_enterprise_documentation_gate",
            "engine_listener_support_maintenance_policy_gate",
        ),
        "evidence_files": {
            "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md": (
                "ENGINE_LISTENER_ENTERPRISE_DOCS",
                "DOC_OPERATIONAL_RUNBOOK",
                "DOC_SUPPORT_BUNDLE_GUIDE",
                "DOC_LIMITATIONS_SUPPORT_STATES",
            ),
            "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md": (
                "PUBLIC_SUPPORT_MAINTENANCE_POLICY",
                "SECURITY_UPDATE_PROCESS",
                "DATA_LOSS_ESCALATION_POLICY",
                "DIAGNOSTIC_COLLECTION_POLICY",
            ),
        },
    },
    {
        "row_id": "reference_sblr_parser_boundary_review",
        "surface": "reference_sblr_parser_boundary_review",
        "review_family": "reference_sblr_parser_boundary_review",
        "review_policy": (
            "reference SBLR parser boundary review closure evidence must map reference "
            "server authority and non-direct surfaces to server/catalog/policy/"
            "unsupported routes and SBSQL-native parser handoff without parser "
            "or reference execution authority"
        ),
        "behavior_gates": (
            "sblr_surface_reference_interface_closure_gate",
            "sblr_surface_sbsql_sync_guardrail_gate",
            "engine_listener_parser_contract_freeze_gate",
            "engine_listener_sbsql_parser_sync_gate",
        ),
        "evidence_files": {
            "project/tests/sblr_surface/sblr_surface_guardrail_gate.py": (
                "validate_interface_closure",
                "validate_sbsql_sync",
                "sbsql_native_normalized",
                "compatibility_dialect_paste_through",
            ),
            "project/tests/engine_listener_enterprise/fixtures/parser_facing_contract_freeze_manifest.json": (
                "reference_route_classifications",
                "unsupported_surface_vectors",
                "sbsql_sync_status",
                "compatibility_version",
            ),
        },
    },
    {
        "row_id": "cluster_boundary_review",
        "surface": "cluster_public_boundary_review",
        "review_family": "cluster_boundary_review",
        "review_policy": (
            "cluster boundary review closure evidence must prove cluster-positive "
            "calls are compile-time gated and admitted public builds use only the "
            "compile-link stub or external provider boundary"
        ),
        "behavior_gates": (
            "public_cluster_provider_boundary_cleanup_gate",
            "public_cluster_boundary_cleanup_audit_gate",
            "public_cluster_build_matrix_gate",
            "engine_listener_release_artifact_trust_gate",
        ),
        "evidence_files": {
            "project/tools/release/public_cluster_boundary_cleanup_audit.py": (
                "compile_link_stub",
                "external_provider_only",
                "production live cluster behavior requires external sb_cluster_provider",
                "cluster stub live claims are forbidden in production builds",
            ),
            "project/CMakeLists.txt": (
                "SB_ENABLE_CLUSTER_PROVIDER",
                "SB_CLUSTER_PROVIDER_STUB",
                "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
                "SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS",
            ),
        },
    },
)

GOLD_ENTERPRISE_READINESS_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "reference_sblr_sbsql_contract_ready",
        "surface": "reference_sblr_sbsql",
        "readiness_policy": (
            "reference SBLR closure, parser-facing contract freeze, and SBSQL-native "
            "synchronization are complete enough for parser implementation to "
            "consume the engine/listener surface"
        ),
        "behavior_gates": (
            "sblr_surface_reference_interface_closure_gate",
            "engine_listener_parser_contract_freeze_gate",
            "engine_listener_sbsql_parser_sync_gate",
        ),
        "proof_refs": (
            "ELER-079.linux_reference_sblr_interface_closure_proven",
            "ELER-085.linux_parser_contract_freeze_proven_cross_platform_pending",
            "ELER-089.linux_sbsql_parser_sync_proven_cross_platform_pending",
        ),
    },
    {
        "row_id": "integrated_crash_recovery_ready",
        "surface": "integrated_crash_recovery",
        "readiness_policy": (
            "listener plus engine integrated product proof and critical crash "
            "recovery certification are complete for the Linux release-candidate lane"
        ),
        "behavior_gates": (
            "engine_listener_integrated_product_proof_gate",
            "engine_listener_crash_recovery_certification_gate",
            "engine_listener_crash_fault_campaign_gate",
        ),
        "proof_refs": (
            "ELER-080.linux_cross_cutting_crash_fault_campaign_proven_cross_platform_pending",
            "ELER-100.linux_integrated_full_stack_product_proof_proven_cross_platform_pending",
            "ELER-101.linux_crash_recovery_certification_matrix_proven_cross_platform_pending",
        ),
    },
    {
        "row_id": "security_and_fuzz_ready",
        "surface": "security_fuzz",
        "readiness_policy": (
            "adversarial security validation and binary/text boundary fuzz and "
            "property tests are complete for the Linux release-candidate lane"
        ),
        "behavior_gates": (
            "engine_listener_adversarial_security_validation_gate",
            "engine_listener_fuzz_property_invariant_suite_gate",
            "engine_listener_support_bundle_redaction_gate",
        ),
        "proof_refs": (
            ADVERSARIAL_SECURITY_TRACEABILITY_REF,
            FUZZ_PROPERTY_TRACEABILITY_REF,
            "ELER-088.linux_secret_canary_redaction_proven_cross_platform_pending",
        ),
    },
    {
        "row_id": "soak_performance_compatibility_ready",
        "surface": "soak_performance_compatibility",
        "readiness_policy": (
            "long-duration soak definitions, performance baselines, and upgrade/"
            "downgrade compatibility proof are complete for the Linux lane"
        ),
        "behavior_gates": (
            "engine_listener_soak_certification_gate",
            "engine_listener_performance_scalability_baseline_gate",
            "engine_listener_compatibility_upgrade_downgrade_gate",
        ),
        "proof_refs": (
            SOAK_CERTIFICATION_TRACEABILITY_REF,
            PERFORMANCE_SCALABILITY_TRACEABILITY_REF,
            "ELER-106.linux_compatibility_upgrade_downgrade_proven_cross_platform_pending",
        ),
    },
    {
        "row_id": "operations_release_docs_support_ready",
        "surface": "ops_release_docs_support",
        "readiness_policy": (
            "operational readiness, release artifact trust, enterprise docs, and "
            "support policy proof are complete for the Linux release-candidate lane"
        ),
        "behavior_gates": (
            "engine_listener_operational_readiness_gate",
            "engine_listener_release_artifact_trust_gate",
            "engine_listener_enterprise_documentation_gate",
            "engine_listener_support_maintenance_policy_gate",
        ),
        "proof_refs": (
            OPERATIONAL_READINESS_TRACEABILITY_REF,
            RELEASE_ARTIFACT_TRUST_TRACEABILITY_REF,
            ENTERPRISE_DOCUMENTATION_TRACEABILITY_REF,
            SUPPORT_MAINTENANCE_TRACEABILITY_REF,
        ),
    },
    {
        "row_id": "final_project_and_review_package_ready",
        "surface": "final_project_review_package",
        "readiness_policy": (
            "project-only implementation proof and independent-review closure "
            "package evidence are generated from project tests without private "
            "execution_plan findings git or local path dependencies"
        ),
        "behavior_gates": (
            "engine_listener_project_only_implementation_proof_gate",
            "engine_listener_external_review_closure_package_gate",
            "engine_listener_no_skip_waiver_xfail_release_gate",
            "engine_listener_generated_resource_determinism_gate",
        ),
        "proof_refs": (
            FINAL_PROJECT_IMPLEMENTATION_TRACEABILITY_REF,
            INDEPENDENT_REVIEW_CLOSURE_TRACEABILITY_REF,
            "ELER-083.linux_no_skip_waiver_xfail_release_proof_enforced_cross_platform_pending",
            "ELER-086.linux_generated_resource_determinism_proven_cross_platform_pending",
        ),
    },
    {
        "row_id": "platform_and_external_audit_claim_fence",
        "surface": "claim_boundary",
        "readiness_policy": (
            "gold readiness evidence must state Linux enterprise release-candidate "
            "proof only and must not claim Windows x64 and FreeBSD/BSD execution or final "
            "external zero-issue audit completion"
        ),
        "behavior_gates": (
            "public_platform_matrix_gate",
            "engine_listener_release_profile_completeness_gate",
            "engine_listener_external_review_closure_package_gate",
        ),
        "proof_refs": (
            "ELER-005.cross_platform_execution_pending",
            "ELER-090.external_zero_issue_audit_pending",
            GOLD_ENTERPRISE_READINESS_TRACEABILITY_REF,
        ),
    },
)

THIRD_AUDIT_EVIDENCE_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "audit_scope_declared_surface",
        "surface": "audit_scope",
        "audit_policy": (
            "third-audit package defines the declared listener and engine surface "
            "from project-test feature inventory rather than private execution-plans"
        ),
        "behavior_gates": (
            "engine_listener_declared_feature_inventory_gate",
            "engine_listener_row_level_traceability_manifest_gate",
        ),
        "proof_refs": (
            "ELER-002.linux_declared_inventory_gate_proven",
            "ELER-084.linux_row_level_traceability_manifest_proven_cross_platform_pending",
        ),
    },
    {
        "row_id": "audit_generated_evidence_inventory",
        "surface": "generated_evidence",
        "audit_policy": (
            "third-audit package includes regenerated no-skip, deterministic, "
            "final-project, review-closure, and gold-readiness evidence matrices"
        ),
        "behavior_gates": (
            "engine_listener_no_skip_waiver_xfail_release_gate",
            "engine_listener_generated_resource_determinism_gate",
            "engine_listener_project_only_implementation_proof_gate",
            "engine_listener_gold_enterprise_readiness_gate",
        ),
        "proof_refs": (
            "ELER-083.linux_no_skip_waiver_xfail_release_proof_enforced_cross_platform_pending",
            "ELER-086.linux_generated_resource_determinism_proven_cross_platform_pending",
            FINAL_PROJECT_IMPLEMENTATION_TRACEABILITY_REF,
            GOLD_ENTERPRISE_READINESS_TRACEABILITY_REF,
        ),
    },
    {
        "row_id": "audit_private_dependency_refusal",
        "surface": "public_release_proof_location",
        "audit_policy": (
            "third-audit package rejects private finding roots, private audit "
            "roots, execution_plan roots, git metadata, and local absolute path "
            "dependencies in generated project-test proof"
        ),
        "behavior_gates": (
            "engine_listener_project_tests_proof_location_gate",
            "public_project_export_gate",
        ),
        "proof_refs": (
            "ELER-004.linux_project_tests_proof_gate_proven",
            "ELER-108.public_export_private_reference_fence",
        ),
    },
    {
        "row_id": "audit_platform_scope_disclosure",
        "surface": "platform_scope",
        "audit_policy": (
            "third-audit package explicitly discloses Linux-first proof and leaves "
            "Windows x64 and FreeBSD/BSD execution pending instead of making a final "
            "all-platform production claim"
        ),
        "behavior_gates": (
            "public_platform_matrix_gate",
            "engine_listener_release_profile_completeness_gate",
        ),
        "proof_refs": (
            "ELER-005.cross_platform_execution_pending",
            "ELER-087.linux_release_profile_complete_cross_platform_pending",
        ),
    },
    {
        "row_id": "audit_external_zero_issue_decision_pending",
        "surface": "external_auditor_decision",
        "audit_policy": (
            "third-audit package is audit-ready evidence; the external auditor's "
            "zero-issue decision remains pending and is not manufactured by CTest"
        ),
        "behavior_gates": (
            "engine_listener_external_review_closure_package_gate",
            "engine_listener_gold_enterprise_readiness_gate",
        ),
        "proof_refs": (
            INDEPENDENT_REVIEW_CLOSURE_TRACEABILITY_REF,
            THIRD_AUDIT_PACKAGE_TRACEABILITY_REF,
        ),
    },
)

SUBSYSTEM_SPEC_AUTHORITY = {
    "release": "public_contract_snapshot",
    "storage": "public_contract_snapshot",
    "transaction": "public_contract_snapshot",
    "datatypes": "public_contract_snapshot",
    "security": "public_contract_snapshot",
    "index": "public_contract_snapshot",
    "optimizer": "public_contract_snapshot",
    "agents": "public_contract_snapshot",
    "memory": "public_contract_snapshot",
    "listener": "public_contract_snapshot",
    "performance": "public_contract_snapshot",
    "ops": "public_contract_snapshot",
    "observability": "public_contract_snapshot",
    "reference_sblr": "public_contract_snapshot",
}

SBLR_FIXTURE_TRACEABILITY = {
    "REFERENCE_GAP_SUMMARY.csv": {
        "selector": "engine_id",
        "spec": "public_contract_snapshot",
        "implementation": "project/tests/sblr_surface/sblr_surface_guardrail_gate.py#validate_inventory",
        "test": "ctest:sblr_surface_reference_inventory_guardrail_gate",
        "status": "linux_reference_sblr_inventory_traceability_proven",
        "proof": "reference_sblr_inventory_guardrail",
    },
    "REFERENCE_INTERNAL_META_OPCODE_CLEANUP_MATRIX.csv": {
        "selector": "token",
        "spec": "public_contract_snapshot",
        "implementation": "project/tests/sblr_surface/sblr_surface_guardrail_gate.py#validate_registry_cleanup",
        "test": "ctest:sblr_surface_registry_cleanup_guardrail_gate",
        "status": "linux_registry_proven",
        "proof": "reference_private_opcode_guardrail",
    },
    "EXPLICIT_UNSUPPORTED_SURFACE_MATRIX.csv": {
        "selector": "inventory_id",
        "spec": "public_contract_snapshot",
        "implementation": "project/src/engine/functions/metadata/compatibility_function_surface_policy.cpp#EvaluateCompatibilityFunctionSurface",
        "test": "ctest:sblr_surface_non_direct_function_lane_conformance",
        "status": "linux_unsupported_refusal_proven",
        "proof": "explicit_unsupported_refusal_contract",
    },
    "IMPLEMENTATION_EXECUTION_PLAN_SEED_MATRIX.csv": {
        "selector": "finding_id",
        "spec": "public_contract_snapshot",
        "implementation": "project/tests/sblr_surface/sblr_surface_guardrail_gate.py#validate_interface_closure",
        "test": "ctest:sblr_surface_public_substrate_guardrail_gate",
        "status": "linux_reference_sblr_seed_traceability_proven",
        "proof": "reference_sblr_seed_guardrail",
    },
    "NON_DIRECT_FUNCTION_SURFACE_MATRIX.csv": {
        "selector": "inventory_id",
        "spec": "public_contract_snapshot",
        "implementation": "project/src/engine/functions/metadata/compatibility_function_surface_policy.cpp#ResolveCompatibilityFunctionSurfacePolicy",
        "test": "ctest:sblr_surface_non_direct_function_lane_conformance",
        "status": "linux_non_direct_function_lane_proven",
        "proof": "non_direct_function_lane_contract",
    },
    "SBLR_STALE_DEFERRED_ALIAS_CLEANUP_MATRIX.csv": {
        "selector": "token",
        "spec": "public_contract_snapshot",
        "implementation": "project/tests/sblr_surface/sblr_surface_guardrail_gate.py#validate_registry_cleanup",
        "test": "ctest:sblr_surface_registry_cleanup_guardrail_gate",
        "status": "linux_registry_proven",
        "proof": "sblr_alias_cleanup_guardrail",
    },
    "SBSQL_SBLR_FAMILY_RECONCILIATION_MATRIX.csv": {
        "selector": "sbsql_family",
        "spec": "public_contract_snapshot",
        "implementation": "project/src/server/sblr_admission.cpp#kNonPrimarySblrAuditFamilies",
        "test": "ctest:sblr_surface_sbsql_family_reconciliation_guardrail_gate",
        "status": "linux_family_reconciliation_proven",
        "proof": "sbsql_family_reconciliation_guardrail",
    },
    "SERVER_AUTHORITY_ROLLUP.csv": {
        "selector": "surface_key",
        "spec": "public_contract_snapshot",
        "implementation": "project/src/server/compatibility_server_authority.cpp#ResolveCompatibilityServerAuthoritySurface",
        "test": "ctest:sblr_surface_server_authority_route_conformance",
        "status": "linux_server_authority_rollup_traceability_proven",
        "proof": "server_authority_rollup_guardrail",
    },
    "SERVER_AUTHORITY_SURFACE_MATRIX.csv": {
        "selector": "decision_id",
        "spec": "public_contract_snapshot",
        "implementation": "project/src/server/compatibility_server_authority.cpp#ResolveCompatibilityServerAuthoritySurface",
        "test": "ctest:sblr_surface_server_authority_route_conformance",
        "status": "linux_server_authority_route_proven",
        "proof": "server_authority_exact_route_conformance",
    },
}

ASSERTION_TOKENS = (
    "Require(",
    "Check(",
    "Expect(",
    "Fail(",
    "require(",
    "fail(",
    "raise SystemExit",
    "assert ",
    "return false",
    "std::exit(EXIT_FAILURE)",
    "throw std::runtime_error",
)

GENERATOR_TOKENS = (
    "--output",
    "write_text(",
    "json.dumps(",
    "csv.DictWriter",
)


@dataclass(frozen=True)
class FeatureDeclaration:
    feature_id: str
    subsystem: str
    title: str
    source_markers: tuple[str, ...]
    proof_markers: tuple[str, ...]
    platform_sensitive: bool
    final_completion_allowed: bool


DECLARED_FEATURES: tuple[FeatureDeclaration, ...] = (
    FeatureDeclaration(
        "ELER-001",
        "release",
        "Prior execution_plan completion semantics audit",
        ("tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",),
        ("engine_listener_prior_execution_plan_semantics_gate",),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-002",
        "release",
        "Generated engine/listener declared-feature inventory",
        ("tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",),
        ("engine_listener_declared_feature_inventory_gate",),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-003",
        "release",
        "Anti-skeleton completion classifier",
        ("tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",),
        ("engine_listener_anti_skeleton_classifier_gate",),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-004",
        "release",
        "Project-tests-only proof relocation",
        ("tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",),
        ("engine_listener_project_tests_proof_location_gate",),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-005",
        "release",
        "Linux Windows x64 FreeBSD platform matrix",
        (
            "cmake/SUPPORTED_PLATFORM_TOOLCHAIN_MATRIX.md",
            "tools/release/public_platform_matrix_gate.py",
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
        ),
        (
            "engine_listener_platform_matrix_gate",
            "public_platform_matrix_gate",
        ),
        True,
        False,
    ),
    FeatureDeclaration(
        "ELER-010",
        "storage",
        "Checked page and file offset arithmetic",
        (
            "src/storage/disk/disk_device.cpp",
            "src/storage/page/page_manager.cpp",
            "tests/engine_listener_enterprise/engine_listener_storage_io_platform_source_gate.py",
        ),
        (
            "engine_listener_storage_io_conformance",
            "engine_listener_storage_io_platform_source_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-011",
        "storage",
        "Platform offset I/O and durable close semantics",
        (
            "src/storage/disk/disk_device.cpp",
            "tests/engine_listener_enterprise/engine_listener_storage_io_platform_source_gate.py",
        ),
        (
            "engine_listener_storage_io_conformance",
            "engine_listener_storage_io_platform_source_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-012",
        "storage",
        "Live physical filespace growth",
        ("src/storage/filespace/filespace_growth.cpp", "src/storage/filespace/filespace_header.cpp"),
        ("engine_listener_filespace_growth_conformance",),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-014",
        "storage",
        "All declared page bodies implemented",
        ("src/storage/page/page_registry.cpp", "src/storage/page/page_layout.cpp"),
        ("public_page_body_production_refusal_gate",),
        False,
        False,
    ),
    FeatureDeclaration(
        "ELER-016",
        "storage",
        "Physical TOAST overflow page persistence",
        ("src/storage/page", "src/core/datatypes"),
        ("public_toast_overflow_binary_descriptor_gate",),
        False,
        False,
    ),
    FeatureDeclaration(
        "ELER-020",
        "transaction",
        "End-to-end physical MGA COW path",
        ("src/transaction/mga", "src/storage/page/row_data_page.cpp"),
        ("public_transaction_mga_cow_gate",),
        False,
        False,
    ),
    FeatureDeclaration(
        "ELER-022",
        "transaction",
        "Crash-safe transaction inventory publish",
        ("src/transaction/mga", "src/storage/database"),
        ("transaction_inventory_publish_fault_conformance",),
        False,
        False,
    ),
    FeatureDeclaration(
        "ELER-023",
        "transaction",
        "Savepoint physical undo executors",
        ("src/transaction/mga", "src/engine/internal_api/transaction"),
        (
            "engine_listener_savepoint_physical_undo_conformance",
            "public_transaction_savepoint_limbo_cleanup_gate",
        ),
        False,
        False,
    ),
    FeatureDeclaration(
        "ELER-024",
        "transaction",
        "Cleanup physical reclamation",
        ("src/transaction/mga", "src/storage/page", "src/core/index",
         "src/engine/internal_api/mga_relation_store/mga_physical_cleanup_coordinator.cpp"),
        ("engine_listener_mga_integrated_physical_cleanup_conformance",),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-030",
        "datatypes",
        "Exact numeric and datatype runtime closure",
        (
            "src/core/datatypes/datatype_runtime_closure.cpp",
            "src/core/datatypes/datatype_operations.cpp",
            "libraries/sbl_numeric/sbl_numeric.cpp",
            "src/engine/internal_api/domain_support/domain_store.cpp",
        ),
        (
            "engine_listener_datatype_numeric_runtime_conformance",
            "public_domain_psql_exact_numeric_gate",
        ),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-031",
        "datatypes",
        "Domain metadata binary catalog storage",
        (
            "src/engine/internal_api/domain_support/domain_store.cpp",
            "src/core/hash/hash_digest.cpp",
            "src/storage/disk/disk_device.cpp",
        ),
        (
            "engine_listener_domain_binary_catalog_conformance",
            "public_domain_psql_exact_numeric_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-032",
        "security",
        "Materialized authorization mandatory for production",
        (
            "src/engine/internal_api/security/security_model.cpp",
            "src/engine/internal_api/security/authorization_api.cpp",
            "src/engine/internal_api/security/security_principal_lifecycle.cpp",
        ),
        (
            "engine_listener_materialized_authorization_conformance",
            "public_authorization_durable_flow_gate",
        ),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-033",
        "security",
        "Protected material durable catalog storage",
        (
            "src/engine/internal_api/security/protected_material_api.cpp",
            "src/core/hash/hash_digest.cpp",
            "src/storage/disk/disk_device.cpp",
        ),
        (
            "engine_listener_protected_material_durable_catalog_conformance",
            "public_security_provider_contract_protected_material_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-040",
        "index",
        "All declared index family routes implemented",
        ("src/core/index", "src/engine/internal_api/dml"),
        (
            "engine_listener_index_family_dml_route_conformance",
            "public_index_readiness_matrix_gate",
            "public_index_dml_maintenance_strategy_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-041",
        "optimizer",
        "Optimizer production-live integrated route",
        ("src/engine/optimizer", "src/engine/executor"),
        (
            "engine_listener_optimizer_integrated_route_conformance",
            "public_optimizer_catalog_backed_planning_gate",
            "public_optimizer_hints_calibration_feedback_gate",
            "public_optimizer_expression_plan_cache_validation_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-042",
        "agents",
        "Engine agent production action proof",
        ("src/core/agents", "src/engine/internal_api/agents"),
        (
            "engine_listener_agent_production_action_conformance",
            "agent_action_dispatch_store_gate",
            "public_agent_readiness_matrix_gate",
            "public_agent_operator_explain_cluster_boundary_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-050",
        "memory",
        "Memory integrated production proof",
        ("src/core/memory", "src/core/resources"),
        (
            "engine_listener_memory_integrated_conformance",
            "public_default_memory_manager_gate",
            "public_query_memory_reservation_gate",
            "public_memory_pressure_executor_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-060",
        "listener",
        "DBBT LPREFACE exact client binding",
        (
            "src/listener/dbbt_lpreface.cpp",
            "src/listener/handoff_claim_reader.cpp",
            "src/listener/listener_runtime.cpp",
            "src/listener/parser_pool.hpp",
            "src/manager/protocol/manager_protocol.cpp",
            "src/manager/node/manager_runtime.cpp",
        ),
        (
            "engine_listener_dbbt_lpreface_binding_conformance",
            "sb_listener_dbbt_lpreface_smoke",
            "sbmn_manager_protocol_unit_tests",
            "sbmn_manager_runtime_integration_tests",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-061",
        "listener",
        "Authenticated structured listener management socket",
        (
            "src/listener/listener_runtime.cpp",
            "src/listener/control_plane.cpp",
            "src/manager/node/manager_listener_control.cpp",
            "src/manager/node/manager_runtime.cpp",
            "src/server/listener_orchestrator.cpp",
            "tests/engine_listener_enterprise/engine_listener_bsd_peer_owner_management_gate.py",
            "tests/engine_listener_enterprise/engine_listener_windows_transport_source_gate.py",
        ),
        (
            "engine_listener_management_envelope_conformance",
            "engine_listener_bsd_peer_owner_management_gate",
            "engine_listener_windows_transport_source_gate",
            "sb_listener_management_commands_smoke",
            "sb_listener_management_malformed_frame_smoke",
            "sb_listener_dbbt_lpreface_smoke",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-062",
        "listener",
        "Parser pool spawn restart backoff quarantine completion",
        (
            "src/listener/listener_config.cpp",
            "src/listener/parser_pool.cpp",
            "src/parsers/sbsql_worker/runtime/parser_runtime.cpp",
            "src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
            "tests/engine_listener_enterprise/engine_listener_windows_transport_source_gate.py",
        ),
        (
            "engine_listener_parser_pool_lifecycle_conformance",
            "engine_listener_windows_transport_source_gate",
            "sb_listener_parser_crash_recovery_smoke",
            "sb_listener_parser_pool_exhaustion_smoke",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-063",
        "listener",
        "Session binding and takeover opcodes live behavior",
        (
            "src/server/session_registry.cpp",
            "src/server/session_registry.hpp",
            "src/listener/control_plane.cpp",
        ),
        ("engine_listener_session_binding_takeover_conformance",),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-064",
        "listener",
        "Graceful drain stop and active worker accounting",
        (
            "src/listener/listener_runtime.cpp",
            "src/listener/parser_pool.cpp",
            "src/listener/parser_pool.hpp",
            "src/server/listener_orchestrator.cpp",
        ),
        (
            "engine_listener_graceful_drain_stop_conformance",
            "sb_listener_drain_admission_smoke",
            "sb_listener_management_stop_smoke",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-065",
        "listener",
        "IPv6 dual-stack Windows x64 and BSD listener transports",
        (
            "src/listener/listener_runtime.cpp",
            "src/listener/control_plane.cpp",
            "src/listener/control_plane.hpp",
            "src/listener/parser_pool.cpp",
            "src/parsers/sbsql_worker/runtime/parser_runtime.cpp",
            "src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp",
            "src/manager/node/manager_listener_control.cpp",
            "src/server/listener_orchestrator.cpp",
            "tests/engine_listener_enterprise/engine_listener_ipv6_dual_stack_transport_conformance.cpp",
            "tests/engine_listener_enterprise/engine_listener_bsd_peer_owner_management_gate.py",
            "tests/engine_listener_enterprise/engine_listener_windows_transport_source_gate.py",
        ),
        (
            "engine_listener_ipv6_dual_stack_transport_conformance",
            "engine_listener_bsd_peer_owner_management_gate",
            "engine_listener_windows_transport_source_gate",
            "public_platform_matrix_gate",
        ),
        True,
        False,
    ),
    FeatureDeclaration(
        "ELER-066",
        "listener",
        "TLS ownership and channel binding closure",
        (
            "src/listener/listener_tls_policy.cpp",
            "src/listener/listener_tls_policy.hpp",
            "src/listener/parser_pool.cpp",
            "src/listener/listener_runtime.cpp",
            "src/listener/listener_config.cpp",
            "src/server/session_registry.cpp",
        ),
        (
            "engine_listener_tls_channel_binding_conformance",
            "sb_listener_tls_required_fail_closed_smoke",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-067",
        "listener",
        "Listener owner lifecycle file hardening",
        (
            "src/listener/listener_socket_identity.cpp",
            "src/listener/listener_socket_identity.hpp",
            "src/listener/listener_runtime.cpp",
            "tests/engine_listener_enterprise/engine_listener_owner_lifecycle_platform_source_gate.py",
        ),
        (
            "engine_listener_owner_lifecycle_artifact_conformance",
            "engine_listener_owner_lifecycle_platform_source_gate",
            "sb_listener_owner_token_collision_smoke",
            "sb_listener_owner_token_stale_corrupt_smoke",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-068",
        "listener",
        "Listener support bundle and bounded fault history",
        (
            "src/listener/listener_support_bundle.cpp",
            "src/listener/listener_support_bundle.hpp",
            "src/listener/listener_runtime.cpp",
            "src/listener/parser_pool.cpp",
        ),
        ("engine_listener_support_bundle_conformance",),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-070",
        "reference_sblr",
        "Reference SBLR audit row promotion",
        ("tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03",),
        ("sblr_surface_reference_inventory_guardrail_gate",),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-079",
        "reference_sblr",
        "Reference SBLR interface closure gate",
        ("tests/sblr_surface/sblr_surface_guardrail_gate.py",),
        ("sblr_surface_reference_interface_closure_gate",),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-080",
        "storage+transaction+listener",
        "Cross-cutting crash and fault campaign",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/fault_injection/public_crash_fault_matrix.py",
            "tests/fault_injection/transaction_inventory_publish_fault_conformance.cpp",
            "tests/release/public_crash_fault_gate.py",
        ),
        (
            "engine_listener_crash_fault_campaign_gate",
            "transaction_inventory_publish_fault_conformance",
            "public_crash_fault_matrix",
            "public_crash_fault_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-100",
        "release+engine+listener",
        "Integrated full-stack product proof",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/engine_listener_enterprise/engine_listener_management_envelope_conformance.cpp",
            "tests/engine_listener_enterprise/engine_listener_dbbt_lpreface_binding_conformance.cpp",
            "tests/release/public_sblr_uuid_mga_route_integration_gate.cpp",
        ),
        (
            "engine_listener_integrated_product_proof_gate",
            "public_sblr_uuid_mga_route_integration_gate",
            "engine_listener_management_envelope_conformance",
            "engine_listener_dbbt_lpreface_binding_conformance",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-101",
        "storage+transaction+listener",
        "Crash recovery certification matrix",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/fault_injection/transaction_inventory_publish_fault_conformance.cpp",
            "tests/engine_listener_enterprise/engine_listener_savepoint_physical_undo_conformance.cpp",
            "tests/engine_listener_enterprise/engine_listener_graceful_drain_stop_conformance.cpp",
        ),
        (
            "engine_listener_crash_recovery_certification_gate",
            "transaction_inventory_publish_fault_conformance",
            "engine_listener_savepoint_physical_undo_conformance",
            "engine_listener_graceful_drain_stop_conformance",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-081",
        "qa+performance",
        "Bounded enterprise soak lane",
        (
            "tests/soak/public_release_soak_lane.py",
            "tests/release/public_release_soak_gate.py",
            "tests/concurrency/memory_sanitizer_soak_concurrency_gate.cpp",
            "tests/database_lifecycle/dpc_soak_leak_resource_stability_gate.cpp",
        ),
        (
            "engine_listener_soak_certification_gate",
            "public_release_soak_lane",
            "public_release_soak_gate",
            "memory_sanitizer_soak_concurrency_gate",
            "dpc_soak_leak_resource_stability_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-102",
        "qa+performance",
        "Long-duration concurrency and pressure soak",
        (
            "tools/ceic_reliability_security_suite.py",
            "tests/consolidated_enterprise/ceic_093_reliability_security_suite_gate_test.py",
            "tests/agents/agent_enterprise_soak_performance_gate.cpp",
            "tests/database_lifecycle/dpc_soak_leak_resource_stability_gate.cpp",
        ),
        (
            "engine_listener_soak_certification_gate",
            "ceic_093_reliability_security_suite_gate",
            "agent_enterprise_soak_performance_gate",
            "dpc_soak_leak_resource_stability_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-103",
        "security+listener",
        "Adversarial security validation",
        (
            "tests/security/enterprise_threat_abuse_suite.json",
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/engine_listener_enterprise/engine_listener_management_envelope_conformance.cpp",
            "tests/engine_listener_enterprise/engine_listener_dbbt_lpreface_binding_conformance.cpp",
            "tools/release/public_enterprise_threat_gate.py",
        ),
        (
            "engine_listener_adversarial_security_validation_gate",
            "public_enterprise_threat_gate",
            "engine_listener_management_envelope_conformance",
            "engine_listener_dbbt_lpreface_binding_conformance",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-104",
        "security+storage+listener",
        "Fuzzing and property testing suite",
        (
            "tests/fuzz/public_durable_codec_fuzz.py",
            "tests/manager/protocol_fuzz_gate.cpp",
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/release/public_codec_property_gate.cpp",
        ),
        (
            "engine_listener_fuzz_property_invariant_suite_gate",
            "public_durable_codec_fuzz",
            "public_codec_property_gate",
            "sbmn_manager_protocol_fuzz_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-105",
        "performance+listener+engine",
        "Performance and scalability baseline proof",
        (
            "tests/performance/public_performance_baselines.json",
            "tests/performance/scratchbird_beta_performance_baseline_conformance.cpp",
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
        ),
        (
            "engine_listener_performance_scalability_baseline_gate",
            "scratchbird_beta_performance_baseline_conformance",
            "memory_observability_overhead_budget_gate",
            "optimizer_runtime_hot_path_orh_123_gate",
            "optimizer_runtime_hot_path_orh_128_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-106",
        "storage+listener+optimizer",
        "Compatibility upgrade and downgrade guarantees",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/release/public_upgrade_migration_gate.cpp",
            "tests/database_lifecycle/upgrade_migration_conformance.cpp",
            "tests/database_lifecycle/protocol_versioning_conformance.cpp",
            "tests/optimizer/optimizer_enterprise_plan_cache_gate.cpp",
        ),
        (
            "engine_listener_compatibility_upgrade_downgrade_gate",
            "public_upgrade_migration_gate",
            "database_lifecycle_upgrade_migration_conformance",
            "database_lifecycle_protocol_versioning_conformance",
            "optimizer_enterprise_plan_cache_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-107",
        "ops+observability",
        "Operational readiness and diagnosability",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "src/server/server_observability.cpp",
            "src/server/manager_control.cpp",
            "src/server/maintenance_coordinator.cpp",
        ),
        (
            "engine_listener_operational_readiness_gate",
            "database_lifecycle_observability_conformance",
            "database_lifecycle_supportability_evidence_conformance",
            "dpc_management_observability_support_bundle_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-108",
        "release+security",
        "Release engineering artifact trust",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tools/release/public_dependency_sbom.py",
            "tools/release/public_release_attestation_gate.py",
            "tools/release/public_artifact_signature_gate.py",
            "tools/release/public_reproducible_export.py",
            "tools/release/public_platform_matrix_gate.py",
        ),
        (
            "engine_listener_release_artifact_trust_gate",
            "public_release_attestation_gate",
            "public_artifact_reproducibility_gate",
            "public_artifact_signature_gate",
            "public_dependency_sbom_gate",
            "public_platform_matrix_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-109",
        "docs+ops",
        "Enterprise documentation and runbooks",
        (
            "docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md",
            "docs/admin/PUBLIC_ADMIN_RUNBOOKS.md",
            "docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md",
            "tools/release/public_enterprise_documentation_gate.py",
        ),
        (
            "engine_listener_enterprise_documentation_gate",
            "public_enterprise_documentation_gate",
            "public_doc_consistency_gate",
            "public_admin_runbook_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-111",
        "support+release",
        "Support and maintenance policy",
        (
            "docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md",
            "docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md",
            "tools/release/public_support_policy_gate.py",
            "tools/release/public_support_bundle_incident_gate.py",
        ),
        (
            "engine_listener_support_maintenance_policy_gate",
            "public_support_policy_gate",
            "public_support_bundle_incident_gate",
            "public_disaster_recovery_gate",
            "public_audit_privacy_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-082",
        "release",
        "Final project-only implementation proof gate",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/engine_listener_enterprise/fixtures/generated_resource_determinism_manifest.json",
        ),
        ("engine_listener_project_only_implementation_proof_gate",),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-083",
        "release",
        "No skip waiver xfail release proof enforcement",
        ("tests/sblr_surface/sblr_surface_guardrail_gate.py", "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py"),
        (
            "engine_listener_anti_skeleton_classifier_gate",
            "engine_listener_no_skip_waiver_xfail_release_gate",
            "public_no_skip_waiver_xfail_gate",
            "sblr_surface_no_skip_completion_guardrail_gate",
        ),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-084",
        "release",
        "Row-level traceability manifest",
        (
            "tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03/sblr_surface_traceability_manifest.json",
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
        ),
        (
            "engine_listener_row_level_traceability_manifest_gate",
            "public_row_level_traceability_manifest_gate",
            "sblr_surface_traceability_manifest_guardrail_gate",
        ),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-085",
        "reference_sblr+release",
        "Parser-facing contract freeze package",
        (
            "tests/engine_listener_enterprise/fixtures/parser_facing_contract_freeze_manifest.json",
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03/sblr_primary_family_snapshot.json",
            "tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03/sbsql_sync_requirements.json",
        ),
        (
            "engine_listener_parser_contract_freeze_gate",
            "public_parser_contract_freeze_gate",
        ),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-086",
        "release",
        "Generated resource determinism gate",
        (
            "tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03/sblr_surface_fixture_hashes.json",
            "tests/engine_listener_enterprise/fixtures/generated_resource_determinism_manifest.json",
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
        ),
        (
            "engine_listener_generated_resource_determinism_gate",
            "public_generated_resource_determinism_gate",
            "sblr_surface_fixture_determinism_drift_gate",
        ),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-087",
        "release",
        "Release-profile configuration completeness gate",
        ("CMakeLists.txt", "tests/release/CMakeLists.txt"),
        (
            "engine_listener_release_profile_completeness_gate",
            "public_production_feature_audit_gate",
            "public_default_config_check",
            "public_secure_defaults_gate",
            "public_cluster_build_matrix_gate",
            "public_platform_matrix_gate",
        ),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-088",
        "security",
        "Secret-canary support-bundle redaction gate",
        (
            "src/listener/listener_support_bundle.cpp",
            "src/engine/internal_api/management/support_bundle_api.cpp",
            "src/core/memory/memory_support_bundle.cpp",
            "src/engine/internal_api/observability/cluster_support_bundle_redaction_api.cpp",
        ),
        ("engine_listener_support_bundle_redaction_gate",),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-089",
        "reference_sblr+release",
        "SBSQL parser contract execution_plan synchronization",
        (
            "tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03/sbsql_sync_requirements.json",
            "tests/sblr_surface/sblr_surface_guardrail_gate.py",
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
        ),
        (
            "sblr_surface_sbsql_sync_guardrail_gate",
            "engine_listener_sbsql_parser_sync_gate",
            "public_sbsql_parser_sync_gate",
        ),
        False,
        True,
    ),
    FeatureDeclaration(
        "ELER-090",
        "audit",
        "Third audit evidence package",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/engine_listener_enterprise/fixtures/external_review_closure_manifest.json",
        ),
        ("engine_listener_third_audit_evidence_package_gate",),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-110",
        "audit",
        "Independent external review closure package",
        (
            "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",
            "tests/engine_listener_enterprise/fixtures/external_review_closure_manifest.json",
        ),
        ("engine_listener_external_review_closure_package_gate",),
        True,
        True,
    ),
    FeatureDeclaration(
        "ELER-112",
        "release",
        "Linux gold enterprise release-candidate readiness aggregation",
        ("tests/engine_listener_enterprise/engine_listener_enterprise_gate.py",),
        ("engine_listener_gold_enterprise_readiness_gate",),
        True,
        True,
    ),
)


def fail(message: str) -> None:
    print(f"engine_listener_enterprise_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def read_text(path: Path, project_root: Path) -> str:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{rel(path, project_root)}")
    return path.read_text(encoding="utf-8")


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_PROOF_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def iter_files(root: Path, patterns: Iterable[str]) -> Iterable[Path]:
    for pattern in patterns:
        yield from root.rglob(pattern)


def ctest_text(project_root: Path) -> str:
    chunks = []
    for cmake_file in iter_files(project_root / "tests", ("CMakeLists.txt",)):
        chunks.append(read_text(cmake_file, project_root))
    return "\n".join(chunks)


def parse_add_executables(cmake_text: str) -> dict[str, list[str]]:
    targets: dict[str, list[str]] = {}
    pattern = re.compile(r"add_executable\s*\(\s*([A-Za-z0-9_.$<>:-]+)(.*?)\)", re.DOTALL)
    for match in pattern.finditer(cmake_text):
        target = match.group(1)
        body = match.group(2)
        sources = re.findall(r"([A-Za-z0-9_./+-]+\.c(?:pp|c|xx)?)", body)
        targets[target] = sources
    return targets


def parse_tests(cmake_text: str) -> list[dict[str, Any]]:
    labels: dict[str, str] = {}
    label_pattern = re.compile(
        r"set_tests_properties\s*\(\s*([A-Za-z0-9_.$<>:-]+)\s+PROPERTIES\s+[^)]*?LABELS\s+\"([^\"]*)\"",
        re.DOTALL,
    )
    for match in label_pattern.finditer(cmake_text):
        labels[match.group(1)] = match.group(2)

    tests: list[dict[str, Any]] = []
    test_pattern = re.compile(
        r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_.$<>:-]+)\s+COMMAND\s+(.*?)\)",
        re.DOTALL,
    )
    for match in test_pattern.finditer(cmake_text):
        name = match.group(1)
        command = " ".join(match.group(2).split())
        tests.append({"name": name, "command": command, "labels": labels.get(name, "")})

    short_pattern = re.compile(
        r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_.$<>:-]+)\s+COMMAND\s+([^\)\n]+)\)",
        re.DOTALL,
    )
    seen = {test["name"] for test in tests}
    for match in short_pattern.finditer(cmake_text):
        name = match.group(1)
        if name in seen:
            continue
        tests.append(
            {
                "name": name,
                "command": " ".join(match.group(2).split()),
                "labels": labels.get(name, ""),
            }
        )
    return tests


def generated_ctest_commands(build_root: Path) -> dict[str, str]:
    commands: dict[str, str] = {}
    pattern = re.compile(r"add_test\(\[=\[([^\]]+)\]=\]\s+(.*?)\)$")
    for ctest_file in build_root.rglob("CTestTestfile.cmake"):
        try:
            text = ctest_file.read_text(encoding="utf-8")
        except OSError as exc:
            fail(f"generated_ctestfile_unreadable:{ctest_file.name}:{exc}")
        for line in text.splitlines():
            match = pattern.match(line.strip())
            if not match:
                continue
            commands[match.group(1)] = match.group(2)
    return commands


def ctest_json_tests(build_root: Path) -> list[dict[str, Any]]:
    try:
        completed = subprocess.run(
            ["ctest", "--test-dir", str(build_root), "--show-only=json-v1"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as exc:
        fail(f"ctest_inventory_launch_failed:{exc}")
    if completed.returncode != 0:
        stderr = " ".join(completed.stderr.split())[:400]
        fail(f"ctest_inventory_failed:{completed.returncode}:{stderr}")
    try:
        inventory = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        fail(f"ctest_inventory_json_invalid:{exc}")
    tests = inventory.get("tests", [])
    if not isinstance(tests, list):
        fail("ctest_inventory_tests_not_list")
    parsed = [test for test in tests if isinstance(test, dict)]
    if sys.platform.startswith("win"):
        project_root_value = cmake_cache_values(build_root).get("CMAKE_HOME_DIRECTORY", "")
        if project_root_value:
            project_root = Path(project_root_value)
            cmake_text = ctest_text(project_root)
            cmake_tests = parse_tests(cmake_text)
            cmake_targets = parse_add_executables(cmake_text)
            existing_names = {str(test.get("name", "")) for test in parsed}
            for raw_test in cmake_tests:
                test_name = str(raw_test.get("name", ""))
                if not test_name or test_name in existing_names:
                    continue
                source_path, source_text = source_for_test(
                    raw_test,
                    cmake_targets,
                    project_root,
                )
                proof_class = proof_class_for_source(
                    test_command_text(raw_test),
                    source_text,
                )
                if not source_path or proof_class == "file_or_registration_only":
                    continue
                labels = [
                    label
                    for label in str(raw_test.get("labels", "")).split(";")
                    if label
                ]
                parsed.append(
                    {
                        "name": test_name,
                        "command": raw_test.get("command", ""),
                        "properties": [{"name": "LABELS", "value": labels}],
                        "non_native_source_proof": True,
                    }
                )
                existing_names.add(test_name)
    return parsed


def cmake_cache_values(build_root: Path) -> dict[str, str]:
    cache_path = build_root / "CMakeCache.txt"
    if not cache_path.is_file():
        return {}
    values: dict[str, str] = {}
    for raw in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw or raw.startswith(("//", "#")) or ":" not in raw or "=" not in raw:
            continue
        key_type, value = raw.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def is_public_release_export_nested(build_root: Path) -> bool:
    return cmake_cache_values(build_root).get("SB_PUBLIC_RELEASE_EXPORT_NESTED") == "ON"


def ctest_property_map(test: dict[str, Any]) -> dict[str, Any]:
    mapped: dict[str, Any] = {}
    for prop in test.get("properties", []):
        if isinstance(prop, dict) and "name" in prop:
            mapped[str(prop["name"]).upper()] = prop.get("value")
    return mapped


def ctest_labels(test: dict[str, Any]) -> set[str]:
    labels = ctest_property_map(test).get("LABELS", [])
    if isinstance(labels, list):
        return {str(label) for label in labels}
    if isinstance(labels, str):
        return {label for label in labels.split(";") if label}
    return set()


def property_truthy(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    lowered = str(value).strip().lower()
    return lowered not in {"", "0", "false", "off", "no", "notfound"}


def is_release_proof_test(test: dict[str, Any]) -> bool:
    name = str(test.get("name", ""))
    labels = ctest_labels(test)
    if RELEASE_PROOF_SCOPE_LABELS & labels:
        return True
    if any(label.startswith("ELER-") or label.startswith("ELER-GATE-") for label in labels):
        return True
    return name.startswith(("engine_listener_", "sblr_surface_"))


TEST_SOURCE_BASES = (
    "tests/release",
    "tests/listener",
    "tests/manager",
    "tests/sblr_surface",
    "tests/sbsql_parser_worker",
    "tests/engine_listener_enterprise",
    "tests/fault_injection",
    "tests/agents",
    "tests/performance",
    "tests/database_lifecycle",
    "tests/optimizer",
    "tests/consolidated_enterprise",
    "tests/concurrency",
)


def find_python_test_source(project_root: Path,
                            candidates: Iterable[str]) -> tuple[str, str]:
    for candidate_name in candidates:
        script_name = Path(str(candidate_name)).name
        if not script_name.endswith(".py"):
            continue
        for base in TEST_SOURCE_BASES:
            candidate = project_root / base / script_name
            if candidate.exists():
                relative = candidate.resolve().relative_to(
                    project_root.resolve()
                ).as_posix()
                return relative, read_text(candidate, project_root)
        if str(candidate_name).startswith("tools/release/"):
            candidate = project_root / str(candidate_name)
            if candidate.exists():
                return str(candidate_name), read_text(candidate, project_root)
    return "", ""


def test_command_text(test: dict[str, Any]) -> str:
    command = test.get("command", "")
    if isinstance(command, list):
        return " ".join(shlex.quote(str(part)) for part in command)
    return str(command)


def source_for_test(test: dict[str, Any],
                    targets: dict[str, list[str]],
                    project_root: Path) -> tuple[str, str]:
    command = test_command_text(test)
    if "SB_ENGINE_LISTENER_ENTERPRISE_GATE" in command:
        relative = "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py"
        return relative, read_text(project_root / relative, project_root)
    if "SB_SBLR_SURFACE_GUARDRAIL_GATE" in command:
        relative = "tests/sblr_surface/sblr_surface_guardrail_gate.py"
        return relative, read_text(project_root / relative, project_root)
    tokens = command.replace('"', " ").split()
    if tokens:
        target = tokens[0]
        if target in targets and targets[target]:
            source = targets[target][0].lstrip("/")
            for base in TEST_SOURCE_BASES:
                candidate = project_root / base / source
                if candidate.exists():
                    source_path = candidate.resolve().relative_to(
                        project_root.resolve()
                    ).as_posix()
                    return source_path, read_text(candidate, project_root)
    test_name = str(test.get("name", ""))
    source_path, source_text = find_python_test_source(
        project_root,
        (
            f"{test_name}.py",
            f"{test_name}_test.py",
            *tokens,
        ),
    )
    if source_path:
        return source_path, source_text
    py_match = re.search(r"(?:\.\./\.\./)?(tools/release/[A-Za-z0-9_./+-]+\.py)", command)
    if py_match:
        relative = py_match.group(1)
        candidate = project_root / relative
        if candidate.exists():
            return relative, read_text(candidate, project_root)
    py_match = re.search(r"(?:\$\{CMAKE_CURRENT_SOURCE_DIR\}/)?([A-Za-z0-9_./+-]+\.py)", command)
    if py_match:
        script = py_match.group(1)
        if "engine_listener_enterprise_gate.py" in script:
            relative = "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py"
            return relative, read_text(project_root / relative, project_root)
        if "sblr_surface_guardrail_gate.py" in script:
            relative = "tests/sblr_surface/sblr_surface_guardrail_gate.py"
            return relative, read_text(project_root / relative, project_root)
        if "sblr_surface_catalog_seed_manifest_gate.py" in script:
            relative = "tests/sblr_surface/sblr_surface_catalog_seed_manifest_gate.py"
            return relative, read_text(project_root / relative, project_root)
        if script.startswith("public_"):
            relative = f"tests/release/{script}"
            candidate = project_root / relative
            if candidate.exists():
                return relative, read_text(candidate, project_root)
        source_path, source_text = find_python_test_source(project_root, (script,))
        if source_path:
            return source_path, source_text
    return "", ""


def proof_class_for_source(command: str, source_text: str) -> str:
    has_assertion = any(token in source_text for token in ASSERTION_TOKENS)
    has_generator = any(token in source_text or token in command for token in GENERATOR_TOKENS)
    if has_assertion and has_generator:
        return "behavioral_generated_gate"
    if has_assertion:
        return "behavioral_executable_gate"
    if has_generator:
        return "generated_matrix_gate"
    return "file_or_registration_only"


def public_release_rows(project_root: Path) -> list[dict[str, Any]]:
    release_cmake = read_text(project_root / "tests/release/CMakeLists.txt", project_root)
    tests = parse_tests(release_cmake)
    targets = parse_add_executables(release_cmake)
    rows: list[dict[str, Any]] = []
    for test in tests:
        labels = test["labels"]
        gate_match = re.search(r"(PCR-GATE-[0-9A-Za-z]+)", labels)
        if not gate_match:
            continue
        pcr_match = re.search(r"(PCR-[0-9A-Za-z]+)", labels)
        source_path, source_text = source_for_test(test, targets, project_root)
        proof_class = proof_class_for_source(test["command"], source_text)
        rows.append(
            {
                "gate_id": gate_match.group(1),
                "slice_id": pcr_match.group(1) if pcr_match else "",
                "test_name": test["name"],
                "source_path": source_path,
                "proof_class": proof_class,
                "reclassified_for_eler": "evidence_input_not_completion",
                "private_docs_required": False,
                "file_presence_completion": proof_class == "file_or_registration_only",
            }
        )
    if len(rows) < 90:
        fail(f"public_release_gate_inventory_too_small:{len(rows)}")
    return rows


def declared_feature_rows(project_root: Path) -> list[dict[str, Any]]:
    tests_text = ctest_text(project_root)
    rows: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    for declaration in DECLARED_FEATURES:
        if declaration.feature_id in seen_ids:
            fail(f"duplicate_feature_declaration:{declaration.feature_id}")
        seen_ids.add(declaration.feature_id)
        missing_sources = [
            marker for marker in declaration.source_markers
            if not (project_root / marker).exists() and marker not in tests_text
        ]
        if missing_sources:
            fail(
                f"declared_feature_source_marker_missing:{declaration.feature_id}:"
                + ",".join(missing_sources)
            )
        matched_proofs = [
            marker for marker in declaration.proof_markers
            if marker in tests_text
        ]
        proof_status = (
            "linux_foundation_or_substrate_proven"
            if declaration.final_completion_allowed and len(matched_proofs) == len(declaration.proof_markers)
            else "declared_pending_enterprise_completion"
        )
        rows.append(
            {
                "feature_id": declaration.feature_id,
                "subsystem": declaration.subsystem,
                "title": declaration.title,
                "source_markers": list(declaration.source_markers),
                "proof_markers": list(declaration.proof_markers),
                "matched_proofs": matched_proofs,
                "platform_sensitive": declaration.platform_sensitive,
                "proof_status": proof_status,
                "final_completion_allowed": declaration.final_completion_allowed,
            }
        )
    required_subsystems = {
        "release", "storage", "transaction", "datatypes", "security", "index",
        "optimizer", "agents", "memory", "listener", "reference_sblr",
    }
    observed = {row["subsystem"] for row in rows}
    missing = sorted(required_subsystems - observed)
    if missing:
        fail("declared_feature_subsystem_missing:" + ",".join(missing))
    if len(rows) < 30:
        fail(f"declared_feature_inventory_too_small:{len(rows)}")
    return rows


def validate_prior_execution_plan_semantics(project_root: Path) -> dict[str, Any]:
    rows = public_release_rows(project_root)
    file_only = [row for row in rows if row["file_presence_completion"]]
    if file_only:
        fail("prior_gate_file_presence_only:" + ",".join(row["test_name"] for row in file_only[:8]))
    return {
        "schema_version": 1,
        "gate": "ELER-001",
        "prior_rows_reclassified": len(rows),
        "completion_semantics": "prior_pcr_rows_are_evidence_inputs_not_eler_completion",
        "rows": rows,
    }


def validate_declared_feature_inventory(project_root: Path) -> dict[str, Any]:
    rows = declared_feature_rows(project_root)
    return {
        "schema_version": 1,
        "gate": "ELER-002",
        "inventory_source": "project_tests_owned_declaration_scan_plus_project_ctest_registration",
        "feature_count": len(rows),
        "rows": rows,
    }


def validate_anti_skeleton_classifier(project_root: Path) -> dict[str, Any]:
    cmake_text = ctest_text(project_root)
    for token in SKIP_OR_WAIVER_TOKENS:
        if token in cmake_text:
            fail(f"skip_or_waiver_token_in_ctest_registration:{token}")
    prior_rows = public_release_rows(project_root)
    bad_prior = [
        row for row in prior_rows
        if row["proof_class"] == "file_or_registration_only"
    ]
    if bad_prior:
        fail("registration_only_prior_proof:" + ",".join(row["test_name"] for row in bad_prior[:8]))
    feature_rows = declared_feature_rows(project_root)
    bad_complete = [
        row["feature_id"] for row in feature_rows
        if row["proof_status"].endswith("_proven") and not row["matched_proofs"]
    ]
    if bad_complete:
        fail("claimed_complete_without_executable_proof:" + ",".join(bad_complete))
    return {
        "schema_version": 1,
        "gate": "ELER-003",
        "skip_waiver_tokens_rejected": list(SKIP_OR_WAIVER_TOKENS),
        "prior_rows_checked": len(prior_rows),
        "declared_features_checked": len(feature_rows),
        "completion_classifier": (
            "complete/proven rows require behavioral executable or generated "
            "project-test proof; file presence, registry presence, manifest-only, "
            "unsupported-only, skip, xfail, waiver, and disabled tests are refused"
        ),
    }


def validate_test_has_no_completion_exception(test: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    name = str(test.get("name", ""))
    props = ctest_property_map(test)
    labels = ctest_labels(test)
    if name.startswith("DISABLED_"):
        errors.append(f"test_name_disabled_prefix:{name}")
    for prop_name in FORBIDDEN_CTEST_RESULT_PROPERTIES:
        if prop_name in props and property_truthy(props[prop_name]):
            errors.append(f"forbidden_ctest_property:{name}:{prop_name}")
    bad_labels = sorted(
        label for label in labels
        if label.strip().lower() in FORBIDDEN_COMPLETION_RESULT_LABELS
    )
    for label in bad_labels:
        errors.append(f"forbidden_completion_label:{name}:{label}")
    return errors


def registered_gate_aliases(
    tests_by_name: dict[str, dict[str, Any]],
    gate_name: str,
    alias_map: dict[str, tuple[str, ...]],
) -> list[str]:
    if gate_name in tests_by_name:
        return []
    aliases: list[str] = []
    for alias in alias_map.get(gate_name, ()):
        if alias in tests_by_name and alias not in aliases:
            aliases.append(alias)
    return aliases


def registered_gate_records(
    tests_by_name: dict[str, dict[str, Any]],
    gate_name: str,
    alias_map: dict[str, tuple[str, ...]],
) -> list[tuple[str, dict[str, Any]]]:
    direct = tests_by_name.get(gate_name)
    if direct:
        return [(gate_name, direct)]
    return [
        (alias, tests_by_name[alias])
        for alias in registered_gate_aliases(tests_by_name, gate_name, alias_map)
    ]


def validate_no_skip_waiver_xfail_release_proof(project_root: Path,
                                                build_root: Path) -> dict[str, Any]:
    tests_text = ctest_text(project_root)
    cmake_tests = {test["name"]: test for test in parse_tests(tests_text)}
    targets = parse_add_executables(tests_text)
    generated_commands = generated_ctest_commands(build_root)
    ctest_tests = ctest_json_tests(build_root)
    tests_by_name = {str(test.get("name", "")): test for test in ctest_tests}
    if len(tests_by_name) < 100:
        fail(f"ctest_inventory_too_small:{len(tests_by_name)}")

    label_index: dict[str, list[dict[str, Any]]] = {}
    for test in ctest_tests:
        for label in ctest_labels(test):
            label_index.setdefault(label, []).append(test)

    non_native_source_proof_allowed = sys.platform.startswith("win")

    rows: list[dict[str, str]] = []
    errors: list[str] = []
    checked_test_names: set[str] = set()

    def command_status(test_name: str) -> str:
        if tests_by_name.get(test_name, {}).get("command"):
            return "ctest_json_command_declared"
        if generated_commands.get(test_name):
            return "generated_ctestfile_command_declared"
        return "missing_command"

    def source_class(test_name: str) -> tuple[str, str]:
        cmake_test = cmake_tests.get(test_name)
        if not cmake_test:
            generated_command = generated_commands.get(test_name, "")
            try:
                generated_tokens = shlex.split(generated_command)
            except ValueError:
                generated_tokens = []
            if generated_tokens:
                for token in generated_tokens:
                    script_name = Path(token).name
                    if script_name == "engine_listener_enterprise_gate.py":
                        relative = "tests/engine_listener_enterprise/engine_listener_enterprise_gate.py"
                        source_text = read_text(project_root / relative, project_root)
                        return relative, proof_class_for_source(generated_command, source_text)
                    if script_name == "sblr_surface_guardrail_gate.py":
                        relative = "tests/sblr_surface/sblr_surface_guardrail_gate.py"
                        source_text = read_text(project_root / relative, project_root)
                        return relative, proof_class_for_source(generated_command, source_text)
                    if script_name == "sblr_surface_catalog_seed_manifest_gate.py":
                        relative = "tests/sblr_surface/sblr_surface_catalog_seed_manifest_gate.py"
                        source_text = read_text(project_root / relative, project_root)
                        return relative, proof_class_for_source(generated_command, source_text)
                    source_path, source_text = find_python_test_source(
                        project_root,
                        (script_name,),
                    )
                    if source_path:
                        return source_path, proof_class_for_source(
                            generated_command,
                            source_text,
                        )
                executable_name = Path(generated_tokens[0]).name
                if executable_name in targets:
                    fake_test = {"command": executable_name}
                    source_path, source_text = source_for_test(
                        fake_test,
                        targets,
                        project_root,
                    )
                    return source_path, proof_class_for_source(
                        executable_name,
                        source_text,
                    )
            return "", "configured_ctest_no_source_classifier"
        source_path, source_text = source_for_test(cmake_test, targets, project_root)
        proof_class = proof_class_for_source(test_command_text(cmake_test), source_text)
        if proof_class == "file_or_registration_only":
            resolved_ctest = tests_by_name.get(test_name)
            if resolved_ctest:
                resolved_path, resolved_text = source_for_test(
                    resolved_ctest,
                    targets,
                    project_root,
                )
                if resolved_path:
                    return resolved_path, proof_class_for_source(
                        test_command_text(resolved_ctest),
                        resolved_text,
                    )
        return source_path, proof_class

    def check_and_record(test: dict[str, Any],
                         feature_id: str,
                         proof_marker: str,
                         match_kind: str) -> None:
        test_name = str(test.get("name", ""))
        labels = sorted(ctest_labels(test))
        status = command_status(test_name)
        if status == "missing_command":
            errors.append(f"configured_test_command_missing:{test_name}")
        errors.extend(validate_test_has_no_completion_exception(test))
        source_path, proof_class = source_class(test_name)
        if proof_class == "file_or_registration_only":
            errors.append(f"registration_only_release_proof:{test_name}")
        rows.append(
            {
                "feature_id": feature_id,
                "proof_marker": proof_marker,
                "test_name": test_name,
                "match_kind": match_kind,
                "labels": ";".join(labels),
                "ctest_command_status": status,
                "proof_class": proof_class,
                "source_path": source_path,
                "completion_exception_status": "none",
                "linux_result_policy": "configured_ctest_must_execute_and_pass_without_skip_xfail_waiver",
            }
        )
        checked_test_names.add(test_name)

    def check_and_record_non_native_source(
        test_name: str,
        feature_id: str,
        proof_marker: str,
    ) -> bool:
        if not non_native_source_proof_allowed or test_name not in cmake_tests:
            return False
        source_path, proof_class = source_class(test_name)
        if proof_class == "file_or_registration_only":
            errors.append(f"registration_only_non_native_source_proof:{test_name}")
        labels = sorted(
            label for label in str(cmake_tests[test_name].get("labels", "")).split(";")
            if label
        )
        rows.append(
            {
                "feature_id": feature_id,
                "proof_marker": proof_marker,
                "test_name": test_name,
                "match_kind": "non_native_source_registration",
                "labels": ";".join(labels),
                "ctest_command_status": "source_registered_for_non_native_host",
                "proof_class": proof_class,
                "source_path": source_path,
                "completion_exception_status": "none",
                "linux_result_policy": "native_linux_ctest_must_execute_on_linux_windows_records_source_equivalent",
            }
        )
        return True

    for declaration in DECLARED_FEATURES:
        for marker in declaration.proof_markers:
            matches: list[tuple[str, dict[str, Any]]] = []
            if marker in tests_by_name:
                matches.append(("test_name", tests_by_name[marker]))
            for test in label_index.get(marker, []):
                if str(test.get("name", "")) != marker:
                    matches.append(("label", test))
            for alias in registered_gate_aliases(
                tests_by_name,
                marker,
                PUBLIC_PROFILE_DECLARED_GATE_ALIASES,
            ):
                matches.append((f"public_profile_alias:{marker}", tests_by_name[alias]))
            if not matches:
                if check_and_record_non_native_source(
                    marker,
                    declaration.feature_id,
                    marker,
                ):
                    continue
                errors.append(
                    f"declared_proof_marker_not_configured:{declaration.feature_id}:{marker}"
                )
                rows.append(
                    {
                        "feature_id": declaration.feature_id,
                        "proof_marker": marker,
                        "test_name": "",
                        "match_kind": "missing",
                        "labels": "",
                        "ctest_command_status": "missing",
                        "proof_class": "",
                        "source_path": "",
                        "completion_exception_status": "missing_configured_ctest",
                        "linux_result_policy": "configured_ctest_must_execute_and_pass_without_skip_xfail_waiver",
                    }
                )
                continue
            for match_kind, test in matches:
                check_and_record(test, declaration.feature_id, marker, match_kind)

    for test in ctest_tests:
        test_name = str(test.get("name", ""))
        if test_name in checked_test_names or not is_release_proof_test(test):
            continue
        status = command_status(test_name)
        if status == "missing_command":
            errors.append(f"configured_test_command_missing:{test_name}")
        errors.extend(validate_test_has_no_completion_exception(test))
        source_path, proof_class = source_class(test_name)
        if proof_class == "file_or_registration_only":
            errors.append(f"registration_only_release_scope_proof:{test_name}")
        rows.append(
            {
                "feature_id": ";".join(
                    sorted(label for label in ctest_labels(test) if label.startswith("ELER-"))
                ),
                "proof_marker": "release_scope_ctest_registration",
                "test_name": test_name,
                "match_kind": "release_scope_label",
                "labels": ";".join(sorted(ctest_labels(test))),
                "ctest_command_status": status,
                "proof_class": proof_class,
                "source_path": source_path,
                "completion_exception_status": "none",
                "linux_result_policy": "configured_ctest_must_execute_and_pass_without_skip_xfail_waiver",
            }
        )

    if errors:
        fail("no_skip_waiver_xfail_release_proof:" + ";".join(errors[:12]))
    if len(rows) < len(DECLARED_FEATURES):
        fail(f"no_skip_waiver_matrix_too_small:{len(rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-083",
        "private_docs_required": False,
        "git_history_required": False,
        "ctest_inventory_count": len(tests_by_name),
        "declared_feature_count": len(DECLARED_FEATURES),
        "forbidden_ctest_properties": list(FORBIDDEN_CTEST_RESULT_PROPERTIES),
        "forbidden_completion_result_labels": sorted(FORBIDDEN_COMPLETION_RESULT_LABELS),
        "matrix_id": "ELER_083_NO_SKIP_WAIVER_XFAIL_RELEASE_PROOF_MATRIX",
        "rows": rows,
    }


def project_relative_reference(marker: str) -> str:
    path_part, separator, anchor = marker.partition("#")
    if path_part.startswith(("docs/engine_listener/", "docs/admin/", "docs/release/", "docs/public_api/")):
        return f"project/{marker}"
    if path_part.startswith(("project/", "docs/", "ctest:", "evidence_artifacts.")):
        return marker
    if path_part.startswith(("src/", "tests/", "tools/", "include/", "libraries/")):
        return f"project/{marker}"
    if path_part == "CMakeLists.txt":
        return f"project/{marker}"
    if separator:
        return f"{path_part}{separator}{anchor}"
    return marker


def reference_path_part(reference: str) -> str:
    if reference.startswith(("ctest:", "evidence_artifacts.")):
        return ""
    return reference.split("#", 1)[0]


def is_public_contract_snapshot_reference(reference: str) -> bool:
    return (
        reference == PUBLIC_CONTRACT_SNAPSHOT
        or reference.startswith(PUBLIC_CONTRACT_SNAPSHOT + "/")
        or reference.startswith("public_release_evidence:" + PUBLIC_CONTRACT_SNAPSHOT)
    )


def require_traceability_reference(project_root: Path,
                                   repo_root: Path,
                                   reference: str,
                                   context: str) -> None:
    reject_private_reference(reference, context)
    if is_public_contract_snapshot_reference(reference):
        candidate = repo_root / PUBLIC_CONTRACT_SNAPSHOT
        if not candidate.is_file():
            fail(f"traceability_reference_missing:{context}:{reference}")
        return
    path_part = reference_path_part(reference)
    if not path_part:
        return
    if Path(path_part).is_absolute() or ".." in Path(path_part).parts:
        fail(f"traceability_reference_not_repo_relative:{context}:{reference}")
    if path_part.startswith("project/"):
        candidate = repo_root / path_part
    elif path_part.startswith("docs/"):
        candidate = repo_root / path_part
    else:
        fail(f"traceability_reference_outside_public_tree:{context}:{reference}")
    if not candidate.exists():
        fail(f"traceability_reference_missing:{context}:{reference}")


def traceability_spec_for_subsystem(subsystem: str) -> str:
    if "+" in subsystem:
        subsystem = subsystem.split("+", 1)[0]
    return SUBSYSTEM_SPEC_AUTHORITY.get(
        subsystem,
        "public_contract_snapshot",
    )


def traceability_route_for_feature(declaration: FeatureDeclaration) -> str:
    for marker in declaration.source_markers:
        reference = project_relative_reference(marker)
        path_part = reference_path_part(reference)
        if path_part.startswith(("project/src/", "project/tests/", "project/CMakeLists.txt")):
            return reference
    return project_relative_reference(declaration.source_markers[0])


def security_traceability_ref(declaration: FeatureDeclaration) -> str:
    if declaration.feature_id == "ELER-104":
        return FUZZ_PROPERTY_TRACEABILITY_REF
    if declaration.subsystem == "security" or "security" in declaration.title.lower():
        return ADVERSARIAL_SECURITY_TRACEABILITY_REF
    if declaration.subsystem == "listener" and any(
        token in declaration.title.lower()
        for token in ("auth", "tls", "management", "binding", "owner")
    ):
        return ADVERSARIAL_SECURITY_TRACEABILITY_REF
    return "not_security_authority"


def crash_traceability_ref(declaration: FeatureDeclaration) -> str:
    if declaration.feature_id == "ELER-101":
        return CRASH_RECOVERY_CERTIFICATION_TRACEABILITY_REF
    if declaration.subsystem in {"storage", "transaction", "index", "agents", "listener"}:
        return CRASH_FAULT_TRACEABILITY_REF
    if any(token in declaration.title.lower() for token in ("crash", "reopen", "durable", "recovery")):
        return CRASH_FAULT_TRACEABILITY_REF
    return "not_crash_authority"


def traceability_feature_rows(project_root: Path) -> list[dict[str, str]]:
    declared_rows = {row["feature_id"]: row for row in declared_feature_rows(project_root)}
    rows: list[dict[str, str]] = []
    for declaration in DECLARED_FEATURES:
        declared = declared_rows[declaration.feature_id]
        platform_result = (
            "linux_proven_cross_platform_pending"
            if declaration.platform_sensitive
            else "linux_proven_not_platform_sensitive"
        )
        rows.append(
            {
                "trace_id": f"ELER084-FEATURE-{declaration.feature_id}",
                "trace_kind": "engine_listener_declared_feature",
                "source_id": declaration.feature_id,
                "canonical_spec_authority": traceability_spec_for_subsystem(
                    declaration.subsystem
                ),
                "implementation_route": traceability_route_for_feature(declaration),
                "test_id": ";".join(f"ctest:{marker}" for marker in declaration.proof_markers),
                "platform_result": platform_result,
                "crash_proof_ref": crash_traceability_ref(declaration),
                "security_proof_ref": security_traceability_ref(declaration),
                "soak_proof_ref": SOAK_CERTIFICATION_TRACEABILITY_REF,
                "evidence_artifact": (
                    "project/tests/engine_listener_enterprise/"
                    "engine_listener_enterprise_gate.py#declared_feature_rows"
                ),
                "completion_status": str(declared["proof_status"]),
                "traceability_status": "traceability_mapped",
                "proof_class": "declared_feature_traceability",
                "product_completion_claim": "false",
            }
        )
    return rows


def read_traceability_csv(path: Path) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    except FileNotFoundError:
        fail(f"traceability_fixture_missing:{path.name}")
    except csv.Error as exc:
        fail(f"traceability_fixture_invalid:{path.name}:{exc}")


def selector_value(row: dict[str, str], selector: str, filename: str, index: int) -> str:
    value = row.get(selector, "")
    if value:
        return value
    digest = sha256_text(json.dumps(row, sort_keys=True, separators=(",", ":")))[:16]
    return f"{filename}:{index}:{digest}"


def traceability_reference_rows(project_root: Path) -> list[dict[str, str]]:
    fixture_root = project_root / SBLR_TRACEABILITY_FIXTURE_ROOT
    rows: list[dict[str, str]] = []
    for filename in SBLR_TRACEABILITY_CSV_FILES:
        policy = SBLR_FIXTURE_TRACEABILITY[filename]
        csv_rows = read_traceability_csv(fixture_root / filename)
        selector = str(policy["selector"])
        for index, csv_row in enumerate(csv_rows, start=1):
            selected = selector_value(csv_row, selector, filename, index)
            row_digest = sha256_text(
                json.dumps(csv_row, sort_keys=True, separators=(",", ":"))
            )[:16]
            rows.append(
                {
                    "trace_id": f"ELER084-REFERENCE-{Path(filename).stem}-{index:04d}-{row_digest}",
                    "trace_kind": "reference_sblr_audit_row",
                    "source_id": f"{filename}:{selector}={selected}",
                    "canonical_spec_authority": str(policy["spec"]),
                    "implementation_route": str(policy["implementation"]),
                    "test_id": str(policy["test"]),
                    "platform_result": "linux_guardrail_proven_cross_platform_pending",
                    "crash_proof_ref": CRASH_FAULT_TRACEABILITY_REF,
                    "security_proof_ref": ADVERSARIAL_SECURITY_TRACEABILITY_REF,
                    "soak_proof_ref": SOAK_CERTIFICATION_TRACEABILITY_REF,
                    "evidence_artifact": (
                        "project/tests/sblr_surface/fixtures/"
                        f"reference_sblr_interface_gap_2026_06_03/{filename}"
                    ),
                    "completion_status": str(policy["status"]),
                    "traceability_status": "traceability_mapped",
                    "proof_class": str(policy["proof"]),
                    "product_completion_claim": "false",
                }
            )
    return rows


def validate_row_level_traceability_manifest(project_root: Path,
                                             build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    feature_rows = traceability_feature_rows(project_root)
    reference_rows = traceability_reference_rows(project_root)
    rows = feature_rows + reference_rows
    seen_trace_ids: set[str] = set()
    for row in rows:
        missing = [field for field in TRACEABILITY_SCHEMA_FIELDS if not row.get(field)]
        if missing:
            fail(f"traceability_row_missing_fields:{row.get('trace_id', '<unknown>')}:{missing}")
        trace_id = row["trace_id"]
        if trace_id in seen_trace_ids:
            fail(f"traceability_duplicate_trace_id:{trace_id}")
        seen_trace_ids.add(trace_id)
        if row["product_completion_claim"] != "false":
            fail(f"traceability_product_completion_claim:{trace_id}")
        for field in (
            "canonical_spec_authority",
            "implementation_route",
            "evidence_artifact",
        ):
            require_traceability_reference(
                project_root,
                repo_root,
                row[field],
                f"{trace_id}:{field}",
            )
        reject_private_reference(row["test_id"], f"{trace_id}:test_id")
    expected_reference_rows = 0
    for filename in SBLR_TRACEABILITY_CSV_FILES:
        expected_reference_rows += len(
            read_traceability_csv(project_root / SBLR_TRACEABILITY_FIXTURE_ROOT / filename)
        )
    if len(reference_rows) != expected_reference_rows or expected_reference_rows < 700:
        fail(
            "traceability_reference_row_count_mismatch:"
            f"actual={len(reference_rows)} expected={expected_reference_rows}"
        )
    if len(feature_rows) != len(DECLARED_FEATURES):
        fail(
            "traceability_feature_row_count_mismatch:"
            f"actual={len(feature_rows)} expected={len(DECLARED_FEATURES)}"
        )
    return {
        "schema_version": 1,
        "gate": "ELER-084",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_084_ROW_LEVEL_TRACEABILITY_MANIFEST",
        "feature_row_count": len(feature_rows),
        "reference_sblr_row_count": len(reference_rows),
        "traceability_schema_fields": list(TRACEABILITY_SCHEMA_FIELDS),
        "rows": rows,
        "build_root_recorded": False,
    }


def validate_crash_fault_campaign(project_root: Path,
                                  build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    required_secondary_gates = {"public_crash_fault_gate"}
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    for campaign_row in CRASH_FAULT_CAMPAIGN_ROWS:
        row_id = str(campaign_row["row_id"])
        if row_id in seen_ids:
            fail(f"crash_fault_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        fault_surface = str(campaign_row["fault_surface"])
        seen_surfaces.add(fault_surface)
        gate = str(campaign_row["gate"])
        test_record = tests_by_name.get(gate)
        if not test_record:
            fail(f"crash_fault_gate_not_registered:{row_id}:{gate}")
        properties = ctest_property_map(test_record)
        for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
            if property_truthy(properties.get(forbidden)):
                fail(f"crash_fault_gate_has_forbidden_property:{row_id}:{gate}:{forbidden}")
        labels = ";".join(sorted(ctest_labels(test_record)))
        evidence_files = campaign_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"crash_fault_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        required_token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            require_traceability_reference(
                project_root,
                repo_root,
                f"project/{relative_text}",
                f"{row_id}:evidence_file",
            )
            text = read_text(project_root / relative_text, project_root)
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"crash_fault_evidence_tokens_empty:{row_id}:{relative_text}")
            missing = [token for token in tokens if token not in text]
            if missing:
                fail(f"crash_fault_evidence_tokens_missing:{row_id}:{relative_text}:{missing}")
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(f"project/{relative_text}")
            required_token_count += len(tokens)
        rows.append(
            {
                "row_id": row_id,
                "fault_surface": fault_surface,
                "critical_transition": str(campaign_row["critical_transition"]),
                "expected_result": str(campaign_row["expected_result"]),
                "gate": gate,
                "ctest_registered": "true",
                "ctest_labels_sha256": sha256_text(labels),
                "mga_authority": str(campaign_row["mga_authority"]),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(required_token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "behavioral_crash_fault_gate_with_tokenized_source_evidence",
                "linux_result": "linux_crash_fault_campaign_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )
    for gate in sorted(required_secondary_gates):
        if gate not in tests_by_name:
            fail(f"crash_fault_secondary_gate_not_registered:{gate}")
    required_surfaces = {
        "platform_io",
        "filespace_growth",
        "filespace_manifest",
        "mga_cow_row_pages",
        "transaction_inventory",
        "toast_overflow",
        "index_maintenance",
        "domain_catalog",
        "security_protected_material",
        "listener_owner_lifecycle",
        "listener_management",
        "listener_support_bundle",
        "public_release_crash_matrix",
    }
    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("crash_fault_campaign_missing_surfaces:" + ",".join(missing_surfaces))
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"crash_fault_product_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    if len(rows) < 13:
        fail(f"crash_fault_campaign_rows_too_small:{len(rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-080",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_080_CROSS_CUTTING_CRASH_FAULT_CAMPAIGN_MATRIX",
        "row_count": len(rows),
        "required_surface_count": len(required_surfaces),
        "secondary_gate_count": len(required_secondary_gates),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_integrated_product_proof(project_root: Path,
                                      build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_phases: set[str] = set()
    for proof_row in INTEGRATED_PRODUCT_PROOF_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"integrated_product_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        product_phase = str(proof_row["product_phase"])
        seen_phases.add(product_phase)
        gate = str(proof_row["gate"])
        test_record = tests_by_name.get(gate)
        if not test_record:
            fail(f"integrated_product_gate_not_registered:{row_id}:{gate}")
        properties = ctest_property_map(test_record)
        for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
            if property_truthy(properties.get(forbidden)):
                fail(f"integrated_product_gate_has_forbidden_property:{row_id}:{gate}:{forbidden}")
        labels = sorted(ctest_labels(test_record))
        if "ELER-100" not in labels or "ELER-GATE-100" not in labels:
            fail(f"integrated_product_gate_missing_eler100_label:{row_id}:{gate}")
        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"integrated_product_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        required_token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            require_traceability_reference(
                project_root,
                repo_root,
                f"project/{relative_text}",
                f"{row_id}:evidence_file",
            )
            text = read_text(project_root / relative_text, project_root)
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"integrated_product_evidence_tokens_empty:{row_id}:{relative_text}")
            missing = [token for token in tokens if token not in text]
            if missing:
                fail(f"integrated_product_evidence_tokens_missing:{row_id}:{relative_text}:{missing}")
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(f"project/{relative_text}")
            required_token_count += len(tokens)
        rows.append(
            {
                "row_id": row_id,
                "product_phase": product_phase,
                "gate": gate,
                "ctest_registered": "true",
                "ctest_eler100_labeled": "true",
                "ctest_labels_sha256": sha256_text(";".join(labels)),
                "authority": str(proof_row["authority"]),
                "expected_result": str(proof_row["expected_result"]),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(required_token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "integrated_product_path_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_integrated_full_stack_product_proof_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )
    required_phases = {
        "listener_startup_management_drain",
        "parser_handoff_binding",
        "listener_support_bundle_recovery_evidence",
        "engine_create_open_sblr_commit_reopen",
        "sblr_auth_optimizer_cluster_boundary",
        "engine_authentication_authorization",
        "mga_cow_insert_update_delete_visibility",
        "toast_overflow_page_storage",
        "index_maintenance_and_recovery",
        "optimizer_plan_selection_and_execution_route",
        "commit_crash_recovery",
        "crash_fault_supportability_rollup",
    }
    missing_phases = sorted(required_phases - seen_phases)
    if missing_phases:
        fail("integrated_product_missing_phases:" + ",".join(missing_phases))
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"integrated_product_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    if len(rows) < 12:
        fail(f"integrated_product_rows_too_small:{len(rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-100",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_100_INTEGRATED_FULL_STACK_PRODUCT_PROOF_MATRIX",
        "row_count": len(rows),
        "required_phase_count": len(required_phases),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_crash_recovery_certification(project_root: Path,
                                          build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    allowed_recovery_classes = {
        "old_state_survives",
        "new_state_survives",
        "recovery_required_detected",
        "write_admission_fenced",
        "operator_required",
        "corruption_detected_fail_closed",
    }
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_classes: set[str] = set()
    for certification_row in CRASH_RECOVERY_CERTIFICATION_ROWS:
        row_id = str(certification_row["row_id"])
        if row_id in seen_ids:
            fail(f"crash_recovery_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        recovery_class = str(certification_row["recovery_class"])
        if recovery_class not in allowed_recovery_classes:
            fail(f"crash_recovery_unknown_class:{row_id}:{recovery_class}")
        seen_classes.add(recovery_class)
        gate = str(certification_row["gate"])
        test_record = tests_by_name.get(gate)
        if not test_record:
            fail(f"crash_recovery_gate_not_registered:{row_id}:{gate}")
        properties = ctest_property_map(test_record)
        for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
            if property_truthy(properties.get(forbidden)):
                fail(f"crash_recovery_gate_has_forbidden_property:{row_id}:{gate}:{forbidden}")
        labels = sorted(ctest_labels(test_record))
        if "ELER-101" not in labels or "ELER-GATE-101" not in labels:
            fail(f"crash_recovery_gate_missing_eler101_label:{row_id}:{gate}")
        evidence_files = certification_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"crash_recovery_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        required_token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            require_traceability_reference(
                project_root,
                repo_root,
                f"project/{relative_text}",
                f"{row_id}:evidence_file",
            )
            text = read_text(project_root / relative_text, project_root)
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"crash_recovery_evidence_tokens_empty:{row_id}:{relative_text}")
            missing = [token for token in tokens if token not in text]
            if missing:
                fail(f"crash_recovery_evidence_tokens_missing:{row_id}:{relative_text}:{missing}")
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(f"project/{relative_text}")
            required_token_count += len(tokens)
        rows.append(
            {
                "row_id": row_id,
                "critical_transition": str(certification_row["critical_transition"]),
                "recovery_class": recovery_class,
                "gate": gate,
                "ctest_registered": "true",
                "ctest_eler101_labeled": "true",
                "ctest_labels_sha256": sha256_text(";".join(labels)),
                "authority": str(certification_row["authority"]),
                "expected_result": str(certification_row["expected_result"]),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(required_token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "crash_recovery_certification_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_crash_recovery_certification_matrix_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )
    required_classes = {
        "old_state_survives",
        "new_state_survives",
        "recovery_required_detected",
        "write_admission_fenced",
        "corruption_detected_fail_closed",
    }
    missing_classes = sorted(required_classes - seen_classes)
    if missing_classes:
        fail("crash_recovery_missing_required_classes:" + ",".join(missing_classes))
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"crash_recovery_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    if len(rows) < 12:
        fail(f"crash_recovery_rows_too_small:{len(rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-101",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_101_CRASH_RECOVERY_CERTIFICATION_MATRIX",
        "row_count": len(rows),
        "required_recovery_class_count": len(required_classes),
        "allowed_recovery_class_count": len(allowed_recovery_classes),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_adversarial_security_validation(project_root: Path,
                                             build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    suite_path = project_root / "tests/security/enterprise_threat_abuse_suite.json"
    suite_text = read_text(suite_path, project_root)
    try:
        suite = json.loads(suite_text)
    except json.JSONDecodeError as exc:
        fail(f"adversarial_security_suite_json_invalid:{exc}")
    if not isinstance(suite, dict):
        fail("adversarial_security_suite_not_object")
    reject_private_reference("project/tests/security/enterprise_threat_abuse_suite.json",
                             "adversarial_security_suite_path")
    if suite.get("schema_version") != 1:
        fail("adversarial_security_suite_schema_invalid")
    if suite.get("marker") != "ENTERPRISE_THREAT_ABUSE_SUITE":
        fail("adversarial_security_suite_marker_missing")
    if suite.get("authority") != "public_release_evidence_only":
        fail("adversarial_security_suite_authority_invalid")
    policy = suite.get("policy")
    if not isinstance(policy, dict):
        fail("adversarial_security_policy_missing")
    policy_checks = {
        "expected_result": "fail_closed",
        "stable_diagnostics_required": True,
        "engine_authority_required": True,
        "parser_sql_text_authority": False,
        "uuid_identity_authority": "engine_uuid_v7",
        "mga_recovery_authority": True,
        "cluster_public_execution": False,
        "support_bundle_secret_leakage": False,
    }
    for key, expected in policy_checks.items():
        if policy.get(key) != expected:
            fail(f"adversarial_security_policy_drift:{key}:{policy.get(key)!r}")

    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    diagnostic_pattern = re.compile(r"^[A-Z0-9][A-Z0-9_.:-]*[A-Z0-9]$")
    suite_cases = suite.get("cases")
    if not isinstance(suite_cases, list) or not suite_cases:
        fail("adversarial_security_cases_missing")
    expected_categories = set(ADVERSARIAL_SECURITY_CATEGORY_GATES)
    rows: list[dict[str, str]] = []
    seen_case_ids: set[str] = set()
    seen_categories: set[str] = set()
    for case in suite_cases:
        if not isinstance(case, dict):
            fail("adversarial_security_case_not_object")
        case_id = str(case.get("case_id", ""))
        category = str(case.get("category", ""))
        if not case_id:
            fail("adversarial_security_case_id_missing")
        if case_id in seen_case_ids:
            fail(f"adversarial_security_duplicate_case:{case_id}")
        seen_case_ids.add(case_id)
        if category not in expected_categories:
            fail(f"adversarial_security_unmapped_category:{case_id}:{category}")
        seen_categories.add(category)
        if case.get("expected_result") != "fail_closed":
            fail(f"adversarial_security_case_not_fail_closed:{case_id}")
        threat = str(case.get("threat", ""))
        if len(threat) < 20:
            fail(f"adversarial_security_threat_too_short:{case_id}")
        diagnostics = case.get("diagnostic_codes")
        if not isinstance(diagnostics, list) or not diagnostics:
            fail(f"adversarial_security_diagnostics_empty:{case_id}")
        diagnostic_set: set[str] = set()
        for diagnostic in diagnostics:
            diagnostic_text = str(diagnostic)
            reject_private_reference(diagnostic_text, f"{case_id}:diagnostic")
            if diagnostic_pattern.match(diagnostic_text) is None:
                fail(f"adversarial_security_diagnostic_unstable:{case_id}:{diagnostic_text}")
            if diagnostic_text in diagnostic_set:
                fail(f"adversarial_security_diagnostic_duplicate:{case_id}:{diagnostic_text}")
            diagnostic_set.add(diagnostic_text)
        evidence = case.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            fail(f"adversarial_security_evidence_empty:{case_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_paths: list[str] = []
        token_count = 0
        for item in evidence:
            if not isinstance(item, dict):
                fail(f"adversarial_security_evidence_not_object:{case_id}")
            path_text = str(item.get("path", ""))
            tokens = item.get("tokens")
            require_traceability_reference(
                project_root,
                repo_root,
                path_text,
                f"{case_id}:evidence_path",
            )
            if not isinstance(tokens, list) or not tokens:
                fail(f"adversarial_security_tokens_empty:{case_id}:{path_text}")
            source_text = read_text(repo_root / path_text, project_root)
            missing = [str(token) for token in tokens if str(token) not in source_text]
            if missing:
                fail(f"adversarial_security_token_missing:{case_id}:{path_text}:{missing}")
            for token in tokens:
                reject_private_reference(str(token), f"{case_id}:evidence_token:{path_text}")
            source_hashes.append(sha256_text(source_text))
            token_hashes.append(
                sha256_text(json.dumps([str(token) for token in tokens],
                                       sort_keys=True,
                                       separators=(",", ":")))
            )
            evidence_paths.append(path_text)
            token_count += len(tokens)
        behavior_gates = ADVERSARIAL_SECURITY_CATEGORY_GATES[category]
        behavior_label_hashes: list[str] = []
        for gate in behavior_gates:
            test_record = tests_by_name.get(gate)
            if not test_record:
                fail(f"adversarial_security_gate_not_registered:{case_id}:{gate}")
            properties = ctest_property_map(test_record)
            for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                if property_truthy(properties.get(forbidden)):
                    fail(f"adversarial_security_gate_has_forbidden_property:{case_id}:{gate}:{forbidden}")
            labels = sorted(ctest_labels(test_record))
            if "ELER-103" not in labels or "ELER-GATE-103" not in labels:
                fail(f"adversarial_security_gate_missing_eler103_label:{case_id}:{gate}")
            behavior_label_hashes.append(sha256_text(";".join(labels)))
        rows.append(
            {
                "case_id": case_id,
                "category": category,
                "expected_result": "fail_closed",
                "diagnostic_count": str(len(diagnostic_set)),
                "source_count": str(len(evidence_paths)),
                "required_token_count": str(token_count),
                "behavior_gate_count": str(len(behavior_gates)),
                "behavior_gates": ";".join(behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_paths),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "adversarial_security_behavior_gate_with_tokenized_threat_evidence",
                "linux_result": "linux_adversarial_security_validation_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )
    missing_categories = sorted(expected_categories - seen_categories)
    if missing_categories:
        fail("adversarial_security_missing_categories:" + ",".join(missing_categories))
    if len(rows) != len(expected_categories):
        fail(f"adversarial_security_row_count:{len(rows)}")
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"adversarial_security_completion_claim:{row['case_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['case_id']}:{field}")
    return {
        "schema_version": 1,
        "gate": "ELER-103",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_103_ADVERSARIAL_SECURITY_VALIDATION_MATRIX",
        "case_count": len(rows),
        "category_count": len(seen_categories),
        "required_category_count": len(expected_categories),
        "build_root_recorded": False,
        "suite_sha256": sha256_text(suite_text),
        "rows": sorted(rows, key=lambda item: item["case_id"]),
    }


def validate_fuzz_property_invariant_suite(project_root: Path,
                                           build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    required_surfaces = {
        "listener_control_plane_frames",
        "dbbt_lpreface_handoff",
        "listener_owner_lifecycle_files",
        "durable_engine_codecs",
        "page_catalog_transaction_inventory_records",
        "mga_row_visibility",
        "materialized_authorization",
        "index_pages_delta_ledgers",
        "toast_overflow_pages",
        "sblr_envelopes_parser_boundary",
        "optimizer_plan_cache_records",
        "agent_catalog_records",
    }
    required_invariant_tokens = {
        "fail_closed",
        "wrong_socket",
        "transaction_finality",
        "rolled_back",
        "explicit_deny",
        "index_candidate",
        "toast_cleanup",
        "parser_authority",
    }
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    seen_invariant_text = " ".join(
        str(row.get("property_invariant", ""))
        for row in FUZZ_PROPERTY_BOUNDARY_ROWS
    )
    for token in required_invariant_tokens:
        if token not in seen_invariant_text:
            fail(f"fuzz_property_required_invariant_token_missing:{token}")

    for proof_row in FUZZ_PROPERTY_BOUNDARY_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"fuzz_property_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        boundary_surface = str(proof_row["boundary_surface"])
        seen_surfaces.add(boundary_surface)
        invariant = str(proof_row["property_invariant"])
        if len(invariant) < 35:
            fail(f"fuzz_property_invariant_too_short:{row_id}")
        behavior_gates = proof_row.get("behavior_gates", ())
        if not isinstance(behavior_gates, tuple) or not behavior_gates:
            fail(f"fuzz_property_behavior_gates_empty:{row_id}")
        behavior_label_hashes: list[str] = []
        for gate in behavior_gates:
            gate_text = str(gate)
            test_record = tests_by_name.get(gate_text)
            if not test_record:
                fail(f"fuzz_property_gate_not_registered:{row_id}:{gate_text}")
            properties = ctest_property_map(test_record)
            for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                if property_truthy(properties.get(forbidden)):
                    fail(f"fuzz_property_gate_has_forbidden_property:{row_id}:{gate_text}:{forbidden}")
            labels = sorted(ctest_labels(test_record))
            if "ELER-104" not in labels or "ELER-GATE-104" not in labels:
                fail(f"fuzz_property_gate_missing_eler104_label:{row_id}:{gate_text}")
            behavior_label_hashes.append(sha256_text(";".join(labels)))

        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"fuzz_property_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        required_token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            require_traceability_reference(
                project_root,
                repo_root,
                f"project/{relative_text}",
                f"{row_id}:evidence_file",
            )
            text = read_text(project_root / relative_text, project_root)
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"fuzz_property_evidence_tokens_empty:{row_id}:{relative_text}")
            missing = [str(token) for token in tokens if str(token) not in text]
            if missing:
                fail(f"fuzz_property_evidence_tokens_missing:{row_id}:{relative_text}:{missing}")
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(f"project/{relative_text}")
            required_token_count += len(tokens)

        rows.append(
            {
                "row_id": row_id,
                "boundary_surface": boundary_surface,
                "boundary_kind": str(proof_row["boundary_kind"]),
                "property_invariant": invariant,
                "behavior_gate_count": str(len(behavior_gates)),
                "behavior_gates": ";".join(str(gate) for gate in behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(required_token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "fuzz_seed_policy": "deterministic_seeded_and_source_token_guarded",
                "proof_class": "fuzz_property_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_fuzz_property_invariant_suite_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )

    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("fuzz_property_missing_surfaces:" + ",".join(missing_surfaces))
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"fuzz_property_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    if len(rows) < 12:
        fail(f"fuzz_property_rows_too_small:{len(rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-104",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_104_FUZZ_PROPERTY_INVARIANT_SUITE_MATRIX",
        "row_count": len(rows),
        "required_surface_count": len(required_surfaces),
        "build_root_recorded": False,
        "rows": rows,
    }


def numeric_baseline_field(row: dict[str, Any], field: str, row_id: str) -> float:
    value = row.get(field)
    if not isinstance(value, (int, float)):
        fail(f"performance_baseline_field_not_numeric:{row_id}:{field}")
    return float(value)


def validate_performance_scalability_baseline(project_root: Path,
                                              build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    baseline_path = project_root / "tests/performance/public_performance_baselines.json"
    baseline_text = read_text(baseline_path, project_root)
    baseline = load_json_object(baseline_path)
    if baseline.get("schema_version") != 2:
        fail("performance_baseline_schema_version")
    if baseline.get("marker") != "PUBLIC_PERFORMANCE_BASELINES":
        fail("performance_baseline_marker")
    policy = baseline.get("measurement_policy", {})
    if not isinstance(policy, dict):
        fail("performance_baseline_policy_not_object")
    for key, expected in (
        ("runtime_measurement_required_for_release_gate", True),
        ("private_inputs_required", False),
        ("parser_sql_text_authority", False),
        ("cluster_production_claims", False),
    ):
        if policy.get(key) is not expected:
            fail(f"performance_baseline_policy:{key}")
    required_fields = baseline.get("required_metric_fields", [])
    if tuple(required_fields) != PERFORMANCE_REQUIRED_METRIC_FIELDS:
        fail("performance_required_metric_fields_mismatch")
    health = baseline.get("runtime_health_expectations", {})
    if not isinstance(health, dict):
        fail("performance_health_expectations_not_object")
    for field in ("error_rate_max", "retry_rate_max", "spill_rate_max",
                  "cleanup_lag_max", "secret_leakage_max"):
        if health.get(field) != 0:
            fail(f"performance_health_expectation_not_zero:{field}")

    thresholds_text = read_text(
        project_root / "tests/performance/BETA_PERFORMANCE_BASELINE_THRESHOLDS.json",
        project_root,
    )
    for token in (
        "scratchbird.beta.performance.baseline.v1",
        "startup_open_latency_ms_max",
        "session_begin_commit_latency_ms_max",
        "simple_query_latency_ms_max",
        "insert_rows_per_second_min",
        "select_rows_per_second_min",
        "rss_growth_bytes_max",
        "max_relative_variance",
        "sample_rows",
        "repeat_count",
    ):
        if token not in thresholds_text:
            fail(f"performance_threshold_token_missing:{token}")

    baseline_rows = baseline.get("baselines", [])
    if not isinstance(baseline_rows, list):
        fail("performance_baseline_rows_not_list")
    baseline_by_operation: dict[str, dict[str, Any]] = {}
    for item in baseline_rows:
        if not isinstance(item, dict):
            fail("performance_baseline_row_not_object")
        operation_id = str(item.get("operation_id", ""))
        if not operation_id:
            fail("performance_baseline_operation_missing")
        if operation_id in baseline_by_operation:
            fail(f"performance_baseline_duplicate_operation:{operation_id}")
        missing = [field for field in PERFORMANCE_REQUIRED_METRIC_FIELDS
                   if field not in item]
        if missing:
            fail(f"performance_baseline_metric_fields_missing:{operation_id}:{missing}")
        p50 = numeric_baseline_field(item, "p50_ms", operation_id)
        median = numeric_baseline_field(item, "median_ms", operation_id)
        p90 = numeric_baseline_field(item, "p90_ms", operation_id)
        p95 = numeric_baseline_field(item, "p95_ms", operation_id)
        p99 = numeric_baseline_field(item, "p99_ms", operation_id)
        p999 = numeric_baseline_field(item, "p99_9_ms", operation_id)
        max_latency = numeric_baseline_field(item, "max_latency_ms", operation_id)
        if median != p50 or not (p50 <= p90 <= p95 <= p99 <= p999 <= max_latency):
            fail(f"performance_baseline_percentile_order:{operation_id}")
        for field in ("error_rate_max", "retry_rate_max", "spill_rate_max", "cleanup_lag_max"):
            if numeric_baseline_field(item, field, operation_id) != 0.0:
                fail(f"performance_baseline_health_not_zero:{operation_id}:{field}")
        if numeric_baseline_field(item, "memory_high_water_bytes_max", operation_id) <= 0.0:
            fail(f"performance_baseline_memory_high_water_missing:{operation_id}")
        if numeric_baseline_field(item, "fd_high_water_max", operation_id) <= 0.0:
            fail(f"performance_baseline_fd_high_water_missing:{operation_id}")
        if item.get("benchmark_clean") is not True:
            fail(f"performance_baseline_not_benchmark_clean:{operation_id}")
        if item.get("measurement_quality") != "enterprise_release_candidate_threshold":
            fail(f"performance_baseline_quality:{operation_id}")
        anchor = str(item.get("public_test_anchor", ""))
        require_traceability_reference(project_root, repo_root, anchor,
                                       f"{operation_id}:public_test_anchor")
        for field, value in item.items():
            reject_private_reference(str(value), f"{operation_id}:{field}")
        baseline_by_operation[operation_id] = item

    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    for proof_row in PERFORMANCE_SCALABILITY_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"performance_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        surface = str(proof_row["surface"])
        seen_surfaces.add(surface)
        operation_id = str(proof_row["required_operation_id"])
        baseline_row = baseline_by_operation.get(operation_id)
        if not baseline_row:
            fail(f"performance_required_operation_missing:{row_id}:{operation_id}")
        behavior_gates = proof_row.get("behavior_gates", ())
        if not isinstance(behavior_gates, tuple) or not behavior_gates:
            fail(f"performance_behavior_gates_empty:{row_id}")
        behavior_label_hashes: list[str] = []
        for gate in behavior_gates:
            gate_text = str(gate)
            test_record = tests_by_name.get(gate_text)
            if not test_record:
                fail(f"performance_gate_not_registered:{row_id}:{gate_text}")
            properties = ctest_property_map(test_record)
            for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                if property_truthy(properties.get(forbidden)):
                    fail(f"performance_gate_has_forbidden_property:{row_id}:{gate_text}:{forbidden}")
            labels = sorted(ctest_labels(test_record))
            if "ELER-105" not in labels or "ELER-GATE-105" not in labels:
                fail(f"performance_gate_missing_eler105_label:{row_id}:{gate_text}")
            if "benchmark_clean" not in labels and gate_text.startswith(("scratchbird_", "memory_", "optimizer_", "index_")):
                fail(f"performance_gate_missing_benchmark_clean_label:{row_id}:{gate_text}")
            behavior_label_hashes.append(sha256_text(";".join(labels)))

        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"performance_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            require_traceability_reference(
                project_root,
                repo_root,
                f"project/{relative_text}",
                f"{row_id}:evidence_file",
            )
            text = baseline_text if relative_text.endswith("public_performance_baselines.json") else read_text(
                project_root / relative_text,
                project_root,
            )
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"performance_evidence_tokens_empty:{row_id}:{relative_text}")
            missing_tokens = [str(token) for token in tokens if str(token) not in text]
            if missing_tokens:
                fail(f"performance_evidence_tokens_missing:{row_id}:{relative_text}:{missing_tokens}")
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(f"project/{relative_text}")
            token_count += len(tokens)

        rows.append(
            {
                "row_id": row_id,
                "surface": surface,
                "operation_id": operation_id,
                "metric_claim": str(proof_row["metric_claim"]),
                "behavior_gate_count": str(len(behavior_gates)),
                "behavior_gates": ";".join(str(gate) for gate in behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "required_metric_fields": ";".join(PERFORMANCE_REQUIRED_METRIC_FIELDS),
                "baseline_percentile_order": "p50_le_p90_le_p95_le_p99_le_p99_9_le_max",
                "runtime_health_policy": "zero_errors_zero_retries_zero_spill_zero_cleanup_lag",
                "proof_class": "enterprise_performance_scalability_baseline_with_runtime_ctest_gate",
                "linux_result": "linux_performance_scalability_baseline_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )

    required_surfaces = {
        "database_lifecycle",
        "transactions",
        "dml",
        "index",
        "optimizer",
        "listener",
        "memory",
        "support_bundle",
    }
    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("performance_missing_surfaces:" + ",".join(missing_surfaces))
    if len(rows) < 10:
        fail(f"performance_rows_too_small:{len(rows)}")
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"performance_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    return {
        "schema_version": 1,
        "gate": "ELER-105",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_105_PERFORMANCE_SCALABILITY_BASELINE_MATRIX",
        "row_count": len(rows),
        "baseline_operation_count": len(baseline_by_operation),
        "required_surface_count": len(required_surfaces),
        "required_metric_fields": list(PERFORMANCE_REQUIRED_METRIC_FIELDS),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_soak_certification(project_root: Path,
                                build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    required_surfaces = {
        "bounded_soak",
        "long_duration_profiles",
        "high_concurrency_pressure",
        "crash_fault_security_negative",
        "leak_drift_orphan_supportability",
    }
    required_lanes = {
        "artifact_retention",
        "concurrent_transactions",
        "crash_fault_injection",
        "high_concurrency",
        "memory_concurrency_reference",
        "memory_pressure",
        "redaction_metadata_retained",
        "security_negative",
        "soak_24h",
        "soak_72h",
        "soak_7d",
        "support_bundle_snapshot_retained",
    }
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    seen_lanes: set[str] = set()
    for proof_row in SOAK_CERTIFICATION_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"soak_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        surface = str(proof_row["surface"])
        seen_surfaces.add(surface)
        policy = str(proof_row["certification_policy"])
        if len(policy) < 80:
            fail(f"soak_policy_too_short:{row_id}")
        if not any(token in policy.lower() for token in ("soak", "long-duration")):
            fail(f"soak_policy_missing_soak_language:{row_id}")

        lanes = proof_row.get("required_lanes", ())
        if not isinstance(lanes, tuple) or not lanes:
            fail(f"soak_lanes_empty:{row_id}")
        for lane in lanes:
            seen_lanes.add(str(lane))

        required_labels = tuple(str(label) for label in proof_row.get("required_labels", ()))
        behavior_gates = proof_row.get("behavior_gates", ())
        if not isinstance(behavior_gates, tuple) or not behavior_gates:
            fail(f"soak_behavior_gates_empty:{row_id}")
        behavior_label_hashes: list[str] = []
        effective_behavior_gates: list[str] = []
        for gate in behavior_gates:
            gate_text = str(gate)
            gate_records = registered_gate_records(
                tests_by_name,
                gate_text,
                SOAK_PUBLIC_PROFILE_GATE_ALIASES,
            )
            if not gate_records:
                fail(f"soak_gate_not_registered:{row_id}:{gate_text}")
            for effective_gate_text, test_record in gate_records:
                if effective_gate_text not in effective_behavior_gates:
                    effective_behavior_gates.append(effective_gate_text)
                properties = ctest_property_map(test_record)
                for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                    if property_truthy(properties.get(forbidden)):
                        fail(
                            f"soak_gate_has_forbidden_property:"
                            f"{row_id}:{effective_gate_text}:{forbidden}"
                        )
                labels = sorted(ctest_labels(test_record))
                if gate_text in SOAK_CERTIFICATION_LABEL_REQUIRED_GATES:
                    missing_labels = [label for label in required_labels if label not in labels]
                    if missing_labels:
                        fail(
                            "soak_gate_missing_required_labels:"
                            f"{row_id}:{effective_gate_text}:{missing_labels}"
                        )
                behavior_label_hashes.append(sha256_text(";".join(labels)))

        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"soak_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            require_traceability_reference(
                project_root,
                repo_root,
                f"project/{relative_text}",
                f"{row_id}:evidence_file",
            )
            text = read_text(project_root / relative_text, project_root)
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"soak_evidence_tokens_empty:{row_id}:{relative_text}")
            missing_tokens = [str(token) for token in tokens if str(token) not in text]
            if missing_tokens:
                fail(f"soak_evidence_tokens_missing:{row_id}:{relative_text}:{missing_tokens}")
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(f"project/{relative_text}")
            token_count += len(tokens)

        rows.append(
            {
                "row_id": row_id,
                "surface": surface,
                "certification_policy": policy,
                "required_lanes": ";".join(str(lane) for lane in lanes),
                "behavior_gate_count": str(len(effective_behavior_gates)),
                "behavior_gates": ";".join(effective_behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "artifact_retention_required": "true",
                "authority_policy": "evidence_only_no_parser_reference_wal_cluster_or_transaction_authority",
                "proof_class": "soak_certification_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_long_duration_soak_certification_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )

    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("soak_missing_surfaces:" + ",".join(missing_surfaces))
    missing_lanes = sorted(required_lanes - seen_lanes)
    if missing_lanes:
        fail("soak_missing_lanes:" + ",".join(missing_lanes))
    if len(rows) < 5:
        fail(f"soak_rows_too_small:{len(rows)}")
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"soak_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    return {
        "schema_version": 1,
        "gate": "ELER-102",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_102_LONG_DURATION_SOAK_CERTIFICATION_MATRIX",
        "traceability_ref": SOAK_CERTIFICATION_TRACEABILITY_REF,
        "row_count": len(rows),
        "required_surface_count": len(required_surfaces),
        "required_lane_count": len(required_lanes),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_compatibility_upgrade_downgrade(project_root: Path,
                                             build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    required_surfaces = {
        "database_file_format",
        "catalog_page_record",
        "catalog_filespace_seed",
        "transaction_inventory",
        "index_metadata",
        "toast_overflow",
        "datatype_descriptor",
        "security_catalog_policy",
        "listener_control_protocol",
        "management_protocol",
        "parser_worker_api",
        "optimizer_plan_cache",
        "cluster_stub_schema",
    }
    required_outcomes = {
        "current_accept",
        "future_refuse",
        "downgrade_refuse",
        "migration_plan_required",
        "explicit_plan_accept_or_required",
        "ambiguous_identity_refuse",
        "tamper_fail_closed",
        "versioned_protocol_refusal",
        "parser_contract_freeze",
        "cache_invalidation",
        "cluster_stub_boundary",
    }
    allowed_outcomes = required_outcomes | {"unsupported_old_refuse"}
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    seen_outcomes: set[str] = set()
    for proof_row in COMPATIBILITY_UPGRADE_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"compatibility_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        surface = str(proof_row["surface"])
        seen_surfaces.add(surface)
        outcomes = proof_row.get("expected_outcomes", ())
        if not isinstance(outcomes, tuple) or not outcomes:
            fail(f"compatibility_outcomes_empty:{row_id}")
        for outcome in outcomes:
            outcome_text = str(outcome)
            if outcome_text not in allowed_outcomes:
                fail(f"compatibility_unknown_outcome:{row_id}:{outcome_text}")
            seen_outcomes.add(outcome_text)
        policy = str(proof_row["compatibility_policy"])
        if len(policy) < 60:
            fail(f"compatibility_policy_too_short:{row_id}")
        if ("version" not in policy and "format" not in policy and
                "freeze" not in policy and "external-provider" not in policy):
            fail(f"compatibility_policy_missing_version_language:{row_id}")

        behavior_gates = proof_row.get("behavior_gates", ())
        if not isinstance(behavior_gates, tuple) or not behavior_gates:
            fail(f"compatibility_behavior_gates_empty:{row_id}")
        behavior_label_hashes: list[str] = []
        effective_behavior_gates: list[str] = []
        for gate in behavior_gates:
            gate_text = str(gate)
            gate_records = registered_gate_records(
                tests_by_name,
                gate_text,
                COMPATIBILITY_PUBLIC_PROFILE_GATE_ALIASES,
            )
            if not gate_records:
                fail(f"compatibility_gate_not_registered:{row_id}:{gate_text}")
            for effective_gate_text, test_record in gate_records:
                if effective_gate_text not in effective_behavior_gates:
                    effective_behavior_gates.append(effective_gate_text)
                properties = ctest_property_map(test_record)
                for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                    if property_truthy(properties.get(forbidden)):
                        fail(
                            "compatibility_gate_has_forbidden_property:"
                            f"{row_id}:{effective_gate_text}:{forbidden}"
                        )
                labels = sorted(ctest_labels(test_record))
                if "ELER-106" not in labels or "ELER-GATE-106" not in labels:
                    fail(
                        f"compatibility_gate_missing_eler106_label:"
                        f"{row_id}:{effective_gate_text}"
                    )
                behavior_label_hashes.append(sha256_text(";".join(labels)))

        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"compatibility_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            require_traceability_reference(
                project_root,
                repo_root,
                f"project/{relative_text}",
                f"{row_id}:evidence_file",
            )
            text = read_text(project_root / relative_text, project_root)
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"compatibility_evidence_tokens_empty:{row_id}:{relative_text}")
            missing_tokens = [str(token) for token in tokens if str(token) not in text]
            if missing_tokens:
                fail(f"compatibility_evidence_tokens_missing:{row_id}:{relative_text}:{missing_tokens}")
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(f"project/{relative_text}")
            token_count += len(tokens)

        rows.append(
            {
                "row_id": row_id,
                "surface": surface,
                "compatibility_policy": policy,
                "expected_outcomes": ";".join(str(outcome) for outcome in outcomes),
                "behavior_gate_count": str(len(effective_behavior_gates)),
                "behavior_gates": ";".join(effective_behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "compatibility_upgrade_downgrade_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_compatibility_upgrade_downgrade_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )

    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("compatibility_missing_surfaces:" + ",".join(missing_surfaces))
    missing_outcomes = sorted(required_outcomes - seen_outcomes)
    if missing_outcomes:
        fail("compatibility_missing_outcomes:" + ",".join(missing_outcomes))
    if len(rows) < 13:
        fail(f"compatibility_rows_too_small:{len(rows)}")
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"compatibility_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    return {
        "schema_version": 1,
        "gate": "ELER-106",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_106_COMPATIBILITY_UPGRADE_DOWNGRADE_MATRIX",
        "row_count": len(rows),
        "required_surface_count": len(required_surfaces),
        "required_outcome_count": len(required_outcomes),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_operational_readiness(project_root: Path,
                                   build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    required_surfaces = {
        "health_readiness_liveness",
        "management_authz_admin",
        "drain_shutdown_lifecycle",
        "recovery_restricted_repair_verify",
        "configuration_validation",
        "metrics_logs_audit_diagnostics",
        "support_bundle_redaction_export",
        "resource_leak_orphan_detection",
        "filespace_transaction_security_summaries",
        "index_readiness_summary",
        "optimizer_readiness_summary",
        "parser_worker_lifecycle_fault_summaries",
        "integrated_support_bundle_metrics",
    }
    required_evidence = {
        "admin_command",
        "authorization",
        "config_validation",
        "descriptor_leak_detection",
        "diagnostics",
        "drain_mode",
        "filespace_summary",
        "forced_shutdown",
        "health_check",
        "index_summary",
        "leak_detection",
        "liveness_check",
        "metrics",
        "optimizer_summary",
        "read_only_recovery",
        "readiness_check",
        "recovery_required",
        "repair_verify",
        "safe_shutdown",
        "secret_redaction",
        "security_summary",
        "stale_lock_lifecycle",
        "structured_logs",
        "support_bundle",
        "transaction_summary",
    }
    policy_terms = (
        "health",
        "readiness",
        "diagnostic",
        "support",
        "shutdown",
        "summary",
        "metrics",
        "repair",
        "redact",
        "fence",
    )
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    seen_evidence: set[str] = set()
    for proof_row in OPERATIONAL_READINESS_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"operational_readiness_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        surface = str(proof_row["surface"])
        seen_surfaces.add(surface)
        policy = str(proof_row["operational_policy"])
        if len(policy) < 60:
            fail(f"operational_readiness_policy_too_short:{row_id}")
        if not any(term in policy.lower() for term in policy_terms):
            fail(f"operational_readiness_policy_missing_ops_language:{row_id}")
        row_evidence = proof_row.get("required_evidence", ())
        if not isinstance(row_evidence, tuple) or not row_evidence:
            fail(f"operational_readiness_required_evidence_empty:{row_id}")
        for item in row_evidence:
            evidence_text = str(item)
            if evidence_text not in required_evidence:
                fail(f"operational_readiness_unknown_evidence:{row_id}:{evidence_text}")
            seen_evidence.add(evidence_text)

        behavior_gates = proof_row.get("behavior_gates", ())
        if not isinstance(behavior_gates, tuple) or not behavior_gates:
            fail(f"operational_readiness_behavior_gates_empty:{row_id}")
        behavior_label_hashes: list[str] = []
        effective_behavior_gates: list[str] = []
        for gate in behavior_gates:
            gate_text = str(gate)
            gate_records = registered_gate_records(
                tests_by_name,
                gate_text,
                OPERATIONAL_PUBLIC_PROFILE_GATE_ALIASES,
            )
            if not gate_records:
                fail(f"operational_readiness_gate_not_registered:{row_id}:{gate_text}")
            for effective_gate_text, test_record in gate_records:
                if effective_gate_text not in effective_behavior_gates:
                    effective_behavior_gates.append(effective_gate_text)
                properties = ctest_property_map(test_record)
                for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                    if property_truthy(properties.get(forbidden)):
                        fail(
                            "operational_readiness_gate_has_forbidden_property:"
                            f"{row_id}:{effective_gate_text}:{forbidden}"
                        )
                labels = sorted(ctest_labels(test_record))
                if "ELER-107" not in labels or "ELER-GATE-107" not in labels:
                    fail(
                        f"operational_readiness_gate_missing_eler107_label:"
                        f"{row_id}:{effective_gate_text}"
                    )
                behavior_label_hashes.append(sha256_text(";".join(labels)))

        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"operational_readiness_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            require_traceability_reference(
                project_root,
                repo_root,
                f"project/{relative_text}",
                f"{row_id}:evidence_file",
            )
            text = read_text(project_root / relative_text, project_root)
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"operational_readiness_evidence_tokens_empty:{row_id}:{relative_text}")
            missing_tokens = [str(token) for token in tokens if str(token) not in text]
            if missing_tokens:
                fail(
                    "operational_readiness_evidence_tokens_missing:"
                    f"{row_id}:{relative_text}:{missing_tokens}"
                )
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(f"project/{relative_text}")
            token_count += len(tokens)

        rows.append(
            {
                "row_id": row_id,
                "surface": surface,
                "operational_policy": policy,
                "required_evidence": ";".join(str(item) for item in row_evidence),
                "behavior_gate_count": str(len(effective_behavior_gates)),
                "behavior_gates": ";".join(effective_behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "operational_readiness_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_operational_readiness_diagnosability_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )

    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("operational_readiness_missing_surfaces:" + ",".join(missing_surfaces))
    missing_evidence = sorted(required_evidence - seen_evidence)
    if missing_evidence:
        fail("operational_readiness_missing_evidence:" + ",".join(missing_evidence))
    if len(rows) < 13:
        fail(f"operational_readiness_rows_too_small:{len(rows)}")
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"operational_readiness_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    return {
        "schema_version": 1,
        "gate": "ELER-107",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_107_OPERATIONAL_READINESS_DIAGNOSABILITY_MATRIX",
        "row_count": len(rows),
        "required_surface_count": len(required_surfaces),
        "required_evidence_count": len(required_evidence),
        "build_root_recorded": False,
        "rows": rows,
    }


def read_repo_relative_text(repo_root: Path, relative_text: str, context: str) -> str:
    reject_private_reference(relative_text, context)
    relative_path = Path(relative_text)
    if relative_path.is_absolute() or ".." in relative_path.parts:
        fail(f"repo_relative_path_invalid:{context}:{relative_text}")
    candidate = repo_root / relative_path
    if not candidate.is_file():
        fail(f"repo_relative_file_missing:{context}:{relative_text}")
    try:
        return candidate.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"repo_relative_file_not_utf8:{context}:{relative_text}:{exc}")
    except OSError as exc:
        fail(f"repo_relative_file_unreadable:{context}:{relative_text}:{exc}")


def validate_release_artifact_trust(project_root: Path,
                                    build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    required_surfaces = {
        "build_provenance_attestation",
        "cluster_stub_release_boundary",
        "compiler_hardening_sanitizer_static",
        "dependency_sbom_license_vulnerability",
        "install_uninstall_cleanup",
        "platform_matrix",
        "public_export_private_reference_fence",
        "release_notes_upgrade_migration",
        "reproducible_export",
        "signature_checksum_artifacts",
        "version_metadata_debug_symbols",
    }
    required_evidence = {
        "build_inputs",
        "build_provenance",
        "checksums",
        "cleanup_manifest",
        "cluster_stub_boundary",
        "compile_time_flags",
        "compatibility_metadata",
        "compiler_hardening",
        "cross_platform_pending",
        "crypto_policy",
        "debug_symbols",
        "dependency_license_review",
        "deterministic_versioning",
        "external_provider_boundary",
        "generated_artifact_inventory",
        "install_uninstall",
        "known_limitations",
        "migration_notes",
        "package_cleanup",
        "platform_matrix",
        "private_reference_scan",
        "public_inputs",
        "release_export",
        "release_notes",
        "reproducible_archive",
        "reproducible_build",
        "sanitizer_static",
        "sbom",
        "service_hardening",
        "signature_ready",
        "toolchain_attestation",
        "upgrade_notes",
        "vulnerability_scan",
        "warnings_as_errors",
    }
    policy_terms = (
        "release",
        "artifact",
        "public",
        "package",
        "proof",
        "signature",
        "cluster",
    )
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    seen_evidence: set[str] = set()
    nested_public_export = is_public_release_export_nested(build_root)
    for proof_row in RELEASE_ARTIFACT_TRUST_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"release_artifact_trust_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        surface = str(proof_row["surface"])
        seen_surfaces.add(surface)
        policy = str(proof_row["release_policy"])
        if len(policy) < 60:
            fail(f"release_artifact_trust_policy_too_short:{row_id}")
        if not any(term in policy.lower() for term in policy_terms):
            fail(f"release_artifact_trust_policy_missing_release_language:{row_id}")

        row_evidence = proof_row.get("required_evidence", ())
        if not isinstance(row_evidence, tuple) or not row_evidence:
            fail(f"release_artifact_trust_required_evidence_empty:{row_id}")
        for item in row_evidence:
            evidence_text = str(item)
            if evidence_text not in required_evidence:
                fail(f"release_artifact_trust_unknown_evidence:{row_id}:{evidence_text}")
            seen_evidence.add(evidence_text)

        behavior_gates = proof_row.get("behavior_gates", ())
        if not isinstance(behavior_gates, tuple) or not behavior_gates:
            fail(f"release_artifact_trust_behavior_gates_empty:{row_id}")
        behavior_label_hashes: list[str] = []
        for gate in behavior_gates:
            gate_text = str(gate)
            test_record = tests_by_name.get(gate_text)
            if not test_record:
                nested_export_outer_gates = {
                    "database_lifecycle_release_provenance_sbom_license_gate",
                    "public_project_export_gate",
                }
                if nested_public_export and gate_text in nested_export_outer_gates:
                    behavior_label_hashes.append(
                        sha256_text(
                            "nested_public_export_outer_gate_required:"
                            + gate_text
                        )
                    )
                    continue
                fail(f"release_artifact_trust_gate_not_registered:{row_id}:{gate_text}")
            properties = ctest_property_map(test_record)
            for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                if property_truthy(properties.get(forbidden)):
                    fail(
                        "release_artifact_trust_gate_has_forbidden_property:"
                        f"{row_id}:{gate_text}:{forbidden}"
                    )
            labels = sorted(ctest_labels(test_record))
            if "ELER-108" not in labels or "ELER-GATE-108" not in labels:
                fail(f"release_artifact_trust_gate_missing_eler108_label:{row_id}:{gate_text}")
            behavior_label_hashes.append(sha256_text(";".join(labels)))

        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"release_artifact_trust_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            text = read_repo_relative_text(repo_root, relative_text,
                                           f"{row_id}:evidence_file")
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"release_artifact_trust_evidence_tokens_empty:{row_id}:{relative_text}")
            missing_tokens = [str(token) for token in tokens if str(token) not in text]
            if missing_tokens:
                fail(
                    "release_artifact_trust_evidence_tokens_missing:"
                    f"{row_id}:{relative_text}:{missing_tokens}"
                )
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(relative_text)
            token_count += len(tokens)

        rows.append(
            {
                "row_id": row_id,
                "surface": surface,
                "release_policy": policy,
                "required_evidence": ";".join(str(item) for item in row_evidence),
                "behavior_gate_count": str(len(behavior_gates)),
                "behavior_gates": ";".join(str(gate) for gate in behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "release_artifact_trust_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_release_artifact_trust_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )

    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("release_artifact_trust_missing_surfaces:" + ",".join(missing_surfaces))
    missing_evidence = sorted(required_evidence - seen_evidence)
    if missing_evidence:
        fail("release_artifact_trust_missing_evidence:" + ",".join(missing_evidence))
    if len(rows) < 11:
        fail(f"release_artifact_trust_rows_too_small:{len(rows)}")
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"release_artifact_trust_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    return {
        "schema_version": 1,
        "gate": "ELER-108",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_108_RELEASE_ARTIFACT_TRUST_MATRIX",
        "traceability_ref": RELEASE_ARTIFACT_TRUST_TRACEABILITY_REF,
        "row_count": len(rows),
        "required_surface_count": len(required_surfaces),
        "required_evidence_count": len(required_evidence),
        "nested_public_export": nested_public_export,
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_enterprise_documentation(project_root: Path,
                                      build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    guide_text = read_repo_relative_text(
        repo_root,
        "project/docs/engine_listener/ENTERPRISE_RELEASE_GUIDE.md",
        "enterprise_documentation:guide",
    )
    required_surfaces = {
        "architecture_trust_boundary",
        "backup_restore_limitations_support_states",
        "lifecycle_format_recovery",
        "mga_storage_index_toast",
        "operations_troubleshooting_runbooks",
        "optimizer_memory_configuration",
        "security_auth_authorization",
        "support_state_taxonomy",
    }
    seen_surfaces: set[str] = set()
    seen_sections: set[str] = set()
    for token in (
        "ENGINE_LISTENER_ENTERPRISE_DOCS",
        "public_release_evidence_only",
        "Parser output is translation evidence",
        "MGA transaction inventory remains transaction finality authority",
        "Cluster-positive execution is outside the public core",
    ):
        if token not in guide_text:
            fail(f"enterprise_documentation_policy_token_missing:{token}")
    for term in ENTERPRISE_DOCUMENTATION_STATUS_TERMS:
        if term not in guide_text:
            fail(f"enterprise_documentation_support_state_missing:{term}")

    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    for proof_row in ENTERPRISE_DOCUMENTATION_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"enterprise_documentation_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        surface = str(proof_row["surface"])
        seen_surfaces.add(surface)
        policy = str(proof_row["documentation_policy"])
        if len(policy) < 60:
            fail(f"enterprise_documentation_policy_too_short:{row_id}")
        if "documentation" not in policy.lower():
            fail(f"enterprise_documentation_policy_missing_documentation_language:{row_id}")

        sections = proof_row.get("required_sections", ())
        if not isinstance(sections, tuple) or not sections:
            fail(f"enterprise_documentation_sections_empty:{row_id}")
        for section in sections:
            section_text = str(section)
            if section_text not in ENTERPRISE_DOCUMENTATION_SECTIONS:
                fail(f"enterprise_documentation_unknown_section:{row_id}:{section_text}")
            if f"## {section_text}" not in guide_text:
                fail(f"enterprise_documentation_section_missing:{row_id}:{section_text}")
            seen_sections.add(section_text)

        behavior_gates = proof_row.get("behavior_gates", ())
        if not isinstance(behavior_gates, tuple) or not behavior_gates:
            fail(f"enterprise_documentation_behavior_gates_empty:{row_id}")
        behavior_label_hashes: list[str] = []
        for gate in behavior_gates:
            gate_text = str(gate)
            test_record = tests_by_name.get(gate_text)
            if not test_record:
                fail(f"enterprise_documentation_gate_not_registered:{row_id}:{gate_text}")
            properties = ctest_property_map(test_record)
            for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                if property_truthy(properties.get(forbidden)):
                    fail(
                        "enterprise_documentation_gate_has_forbidden_property:"
                        f"{row_id}:{gate_text}:{forbidden}"
                    )
            labels = sorted(ctest_labels(test_record))
            if (
                gate_text in ENTERPRISE_DOCUMENTATION_LABEL_REQUIRED_GATES
                and ("ELER-109" not in labels or "ELER-GATE-109" not in labels)
            ):
                fail(f"enterprise_documentation_gate_missing_eler109_label:{row_id}:{gate_text}")
            behavior_label_hashes.append(sha256_text(";".join(labels)))

        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"enterprise_documentation_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            text = read_repo_relative_text(repo_root, relative_text,
                                           f"{row_id}:evidence_file")
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"enterprise_documentation_evidence_tokens_empty:{row_id}:{relative_text}")
            missing_tokens = [str(token) for token in tokens if str(token) not in text]
            if missing_tokens:
                fail(
                    "enterprise_documentation_evidence_tokens_missing:"
                    f"{row_id}:{relative_text}:{missing_tokens}"
                )
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(relative_text)
            token_count += len(tokens)

        rows.append(
            {
                "row_id": row_id,
                "surface": surface,
                "required_sections": ";".join(str(section) for section in sections),
                "documentation_policy": policy,
                "behavior_gate_count": str(len(behavior_gates)),
                "behavior_gates": ";".join(str(gate) for gate in behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "enterprise_documentation_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_enterprise_documentation_runbooks_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )

    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("enterprise_documentation_missing_surfaces:" + ",".join(missing_surfaces))
    missing_sections = sorted(set(ENTERPRISE_DOCUMENTATION_SECTIONS) - seen_sections)
    if missing_sections:
        fail("enterprise_documentation_missing_sections:" + ",".join(missing_sections))
    if len(rows) < 8:
        fail(f"enterprise_documentation_rows_too_small:{len(rows)}")
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"enterprise_documentation_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    return {
        "schema_version": 1,
        "gate": "ELER-109",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_109_ENTERPRISE_DOCUMENTATION_RUNBOOK_MATRIX",
        "traceability_ref": ENTERPRISE_DOCUMENTATION_TRACEABILITY_REF,
        "row_count": len(rows),
        "required_surface_count": len(required_surfaces),
        "required_section_count": len(ENTERPRISE_DOCUMENTATION_SECTIONS),
        "support_state_term_count": len(ENTERPRISE_DOCUMENTATION_STATUS_TERMS),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_support_maintenance_policy(project_root: Path,
                                        build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    policy_text = read_repo_relative_text(
        repo_root,
        "project/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md",
        "support_maintenance_policy:policy_doc",
    )
    for token in (
        "PUBLIC_SUPPORT_MAINTENANCE_POLICY",
        "public_release_evidence_only",
        "does not define storage authority",
        "transaction finality",
        "authorization",
        "recovery classification",
        "cluster-positive execution authority",
    ):
        if token not in policy_text:
            fail(f"support_maintenance_policy_token_missing:{token}")

    required_surfaces = {
        "data_loss_disaster_rollback",
        "diagnostic_collection_privacy",
        "patch_compatibility",
        "security_cve_disclosure",
        "support_lifecycle_sla",
        "support_policy_runtime_authority_boundary",
    }
    rows: list[dict[str, str]] = []
    seen_ids: set[str] = set()
    seen_surfaces: set[str] = set()
    seen_sections: set[str] = set()
    for proof_row in SUPPORT_MAINTENANCE_ROWS:
        row_id = str(proof_row["row_id"])
        if row_id in seen_ids:
            fail(f"support_maintenance_duplicate_row_id:{row_id}")
        seen_ids.add(row_id)
        surface = str(proof_row["surface"])
        seen_surfaces.add(surface)
        policy = str(proof_row["support_policy"])
        if len(policy) < 60:
            fail(f"support_maintenance_policy_too_short:{row_id}")
        if "support" not in policy.lower():
            fail(f"support_maintenance_policy_missing_support_language:{row_id}")

        sections = proof_row.get("required_sections", ())
        if not isinstance(sections, tuple) or not sections:
            fail(f"support_maintenance_sections_empty:{row_id}")
        for section in sections:
            section_text = str(section)
            if section_text not in SUPPORT_MAINTENANCE_SECTIONS:
                fail(f"support_maintenance_unknown_section:{row_id}:{section_text}")
            if f"## {section_text}" not in policy_text:
                fail(f"support_maintenance_section_missing:{row_id}:{section_text}")
            seen_sections.add(section_text)

        behavior_gates = proof_row.get("behavior_gates", ())
        if not isinstance(behavior_gates, tuple) or not behavior_gates:
            fail(f"support_maintenance_behavior_gates_empty:{row_id}")
        behavior_label_hashes: list[str] = []
        for gate in behavior_gates:
            gate_text = str(gate)
            test_record = tests_by_name.get(gate_text)
            if not test_record:
                fail(f"support_maintenance_gate_not_registered:{row_id}:{gate_text}")
            properties = ctest_property_map(test_record)
            for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
                if property_truthy(properties.get(forbidden)):
                    fail(
                        "support_maintenance_gate_has_forbidden_property:"
                        f"{row_id}:{gate_text}:{forbidden}"
                    )
            labels = sorted(ctest_labels(test_record))
            if (
                gate_text in SUPPORT_MAINTENANCE_LABEL_REQUIRED_GATES
                and ("ELER-111" not in labels or "ELER-GATE-111" not in labels)
            ):
                fail(f"support_maintenance_gate_missing_eler111_label:{row_id}:{gate_text}")
            behavior_label_hashes.append(sha256_text(";".join(labels)))

        evidence_files = proof_row.get("evidence_files", {})
        if not isinstance(evidence_files, dict) or not evidence_files:
            fail(f"support_maintenance_missing_evidence_files:{row_id}")
        source_hashes: list[str] = []
        token_hashes: list[str] = []
        evidence_artifacts: list[str] = []
        token_count = 0
        for relative, tokens in evidence_files.items():
            relative_text = str(relative)
            text = read_repo_relative_text(repo_root, relative_text,
                                           f"{row_id}:evidence_file")
            if not isinstance(tokens, tuple) or not tokens:
                fail(f"support_maintenance_evidence_tokens_empty:{row_id}:{relative_text}")
            missing_tokens = [str(token) for token in tokens if str(token) not in text]
            if missing_tokens:
                fail(
                    "support_maintenance_evidence_tokens_missing:"
                    f"{row_id}:{relative_text}:{missing_tokens}"
                )
            source_hashes.append(sha256_text(text))
            token_hashes.append(
                sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
            )
            evidence_artifacts.append(relative_text)
            token_count += len(tokens)

        rows.append(
            {
                "row_id": row_id,
                "surface": surface,
                "required_sections": ";".join(str(section) for section in sections),
                "support_policy": policy,
                "behavior_gate_count": str(len(behavior_gates)),
                "behavior_gates": ";".join(str(gate) for gate in behavior_gates),
                "behavior_gate_label_sha256": sha256_text("|".join(behavior_label_hashes)),
                "evidence_artifact": ";".join(evidence_artifacts),
                "source_sha256": sha256_text("|".join(source_hashes)),
                "required_token_count": str(token_count),
                "required_token_sha256": sha256_text("|".join(token_hashes)),
                "proof_class": "support_maintenance_policy_behavior_gate_with_tokenized_source_evidence",
                "linux_result": "linux_support_maintenance_policy_proven_cross_platform_pending",
                "product_completion_claim": "false",
            }
        )

    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("support_maintenance_missing_surfaces:" + ",".join(missing_surfaces))
    missing_sections = sorted(set(SUPPORT_MAINTENANCE_SECTIONS) - seen_sections)
    if missing_sections:
        fail("support_maintenance_missing_sections:" + ",".join(missing_sections))
    if len(rows) < 6:
        fail(f"support_maintenance_rows_too_small:{len(rows)}")
    for row in rows:
        if row["product_completion_claim"] != "false":
            fail(f"support_maintenance_completion_claim:{row['row_id']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
    return {
        "schema_version": 1,
        "gate": "ELER-111",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_111_SUPPORT_MAINTENANCE_POLICY_MATRIX",
        "traceability_ref": SUPPORT_MAINTENANCE_TRACEABILITY_REF,
        "row_count": len(rows),
        "required_surface_count": len(required_surfaces),
        "required_section_count": len(SUPPORT_MAINTENANCE_SECTIONS),
        "build_root_recorded": False,
        "rows": rows,
    }


def require_registered_behavior_gates(project_root: Path,
                                      build_root: Path,
                                      row_id: str,
                                      behavior_gates: tuple[str, ...]) -> str:
    _ = project_root
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    label_hashes: list[str] = []
    for gate in behavior_gates:
        gate_text = str(gate)
        test_record = tests_by_name.get(gate_text)
        if not test_record:
            fail(f"aggregation_gate_not_registered:{row_id}:{gate_text}")
        properties = ctest_property_map(test_record)
        for forbidden in FORBIDDEN_CTEST_RESULT_PROPERTIES:
            if property_truthy(properties.get(forbidden)):
                fail(
                    "aggregation_gate_has_forbidden_property:"
                    f"{row_id}:{gate_text}:{forbidden}"
                )
        labels = sorted(ctest_labels(test_record))
        if not labels:
            fail(f"aggregation_gate_missing_labels:{row_id}:{gate_text}")
        if not (
            "linux_proof" in labels
            or "public_release_correctness" in labels
            or "sblr_surface" in labels
            or "release_proof_substrate" in labels
        ):
            fail(f"aggregation_gate_missing_release_scope_label:{row_id}:{gate_text}")
        label_hashes.append(sha256_text(";".join(labels)))
    return sha256_text("|".join(label_hashes))


def require_tokenized_repo_evidence(repo_root: Path,
                                    row_id: str,
                                    evidence_files: dict[str, tuple[str, ...]]) -> tuple[str, str, int]:
    source_hashes: list[str] = []
    token_hashes: list[str] = []
    token_count = 0
    for relative, tokens in evidence_files.items():
        relative_text = str(relative)
        text = read_repo_relative_text(repo_root, relative_text, f"{row_id}:evidence_file")
        if not isinstance(tokens, tuple) or not tokens:
            fail(f"aggregation_evidence_tokens_empty:{row_id}:{relative_text}")
        missing_tokens = [str(token) for token in tokens if str(token) not in text]
        if missing_tokens:
            fail(
                "aggregation_evidence_tokens_missing:"
                f"{row_id}:{relative_text}:{missing_tokens}"
            )
        source_hashes.append(sha256_text(text))
        token_hashes.append(
            sha256_text(json.dumps(list(tokens), sort_keys=True, separators=(",", ":")))
        )
        token_count += len(tokens)
    return sha256_text("|".join(source_hashes)), sha256_text("|".join(token_hashes)), token_count


def aggregation_row(project_root: Path,
                    build_root: Path,
                    row_spec: dict[str, Any],
                    policy_field: str,
                    proof_class: str,
                    linux_result: str) -> dict[str, str]:
    repo_root = project_root.parent
    row_id = str(row_spec["row_id"])
    surface = str(row_spec["surface"])
    policy = str(row_spec[policy_field])
    if len(policy) < 70:
        fail(f"aggregation_policy_too_short:{row_id}")
    behavior_gates = tuple(str(gate) for gate in row_spec.get("behavior_gates", ()))
    if not behavior_gates:
        fail(f"aggregation_behavior_gates_empty:{row_id}")
    behavior_gate_label_sha256 = require_registered_behavior_gates(
        project_root,
        build_root,
        row_id,
        behavior_gates,
    )
    evidence_files = row_spec.get("evidence_files", {})
    if not isinstance(evidence_files, dict) or not evidence_files:
        fail(f"aggregation_missing_evidence_files:{row_id}")
    source_sha256, token_sha256, token_count = require_tokenized_repo_evidence(
        repo_root,
        row_id,
        evidence_files,
    )
    row = {
        "row_id": row_id,
        "surface": surface,
        policy_field: policy,
        "behavior_gate_count": str(len(behavior_gates)),
        "behavior_gates": ";".join(behavior_gates),
        "behavior_gate_label_sha256": behavior_gate_label_sha256,
        "evidence_artifact": ";".join(str(path) for path in evidence_files),
        "source_sha256": source_sha256,
        "required_token_count": str(token_count),
        "required_token_sha256": token_sha256,
        "proof_class": proof_class,
        "linux_result": linux_result,
        "product_completion_claim": "false",
    }
    for field, value in row.items():
        reject_private_reference(str(value), f"{row_id}:{field}")
    return row


def validate_project_only_implementation_proof(project_root: Path,
                                               build_root: Path) -> dict[str, Any]:
    required_surfaces = {
        "reference_sblr_parser_contract",
        "integrated_enterprise_stack",
        "parser_contract_determinism",
        "release_profile_export_hygiene",
        "release_proof_integrity",
    }
    no_skip = validate_no_skip_waiver_xfail_release_proof(project_root, build_root)
    traceability = validate_row_level_traceability_manifest(project_root, build_root)
    parser_contract = validate_parser_facing_contract_freeze(project_root, build_root)
    sbsql_sync = validate_sbsql_parser_spec_execution_plan_sync(project_root, build_root)
    release_profile = validate_release_profile_completeness(project_root, build_root)
    rows = [
        aggregation_row(
            project_root,
            build_root,
            row_spec,
            "closure_policy",
            "project_only_final_implementation_behavior_gate_with_tokenized_source_evidence",
            "linux_final_project_only_implementation_proof_proven_cross_platform_pending",
        )
        for row_spec in FINAL_PROJECT_ONLY_ROWS
    ]
    missing_surfaces = sorted(required_surfaces - {row["surface"] for row in rows})
    if missing_surfaces:
        fail("final_project_only_missing_surfaces:" + ",".join(missing_surfaces))
    for nested in (no_skip, traceability, parser_contract, sbsql_sync, release_profile):
        if nested.get("private_docs_required") is not False:
            fail(f"final_project_only_nested_private_docs:{nested.get('gate')}")
        if nested.get("git_history_required") is not False:
            fail(f"final_project_only_nested_git_history:{nested.get('gate')}")
        for value in (
            nested.get("matrix_id", ""),
            nested.get("traceability_ref", ""),
            str(nested.get("gate", "")),
        ):
            reject_private_reference(str(value), f"final_project_only_nested:{nested.get('gate')}")
    if int(no_skip.get("declared_feature_count", 0)) != len(DECLARED_FEATURES):
        fail("final_project_only_declared_feature_count_drift")
    if int(traceability.get("feature_row_count", 0)) != len(DECLARED_FEATURES):
        fail("final_project_only_traceability_feature_count_drift")
    if int(traceability.get("reference_sblr_row_count", 0)) < 700:
        fail("final_project_only_reference_traceability_row_count")
    if int(parser_contract.get("row_count", 0)) < EXPECTED_PARSER_CONTRACT_MINIMUM_ROWS:
        fail("final_project_only_parser_contract_row_count")
    if int(sbsql_sync.get("fixture_row_count", 0)) < 700:
        fail("final_project_only_sbsql_sync_fixture_count")
    if int(release_profile.get("declared_feature_count", 0)) != len(DECLARED_FEATURES):
        fail("final_project_only_release_profile_feature_count_drift")
    return {
        "schema_version": 1,
        "gate": "ELER-082",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_082_FINAL_PROJECT_ONLY_IMPLEMENTATION_PROOF_MATRIX",
        "traceability_ref": FINAL_PROJECT_IMPLEMENTATION_TRACEABILITY_REF,
        "declared_feature_count": len(DECLARED_FEATURES),
        "traceability_feature_row_count": int(traceability.get("feature_row_count", 0)),
        "reference_sblr_row_count": int(traceability.get("reference_sblr_row_count", 0)),
        "parser_contract_row_count": int(parser_contract.get("row_count", 0)),
        "sbsql_sync_fixture_row_count": int(sbsql_sync.get("fixture_row_count", 0)),
        "release_profile_feature_count": int(release_profile.get("declared_feature_count", 0)),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_external_review_manifest_shape(project_root: Path) -> dict[str, Any]:
    manifest = load_json_object(project_root / EXTERNAL_REVIEW_CLOSURE_MANIFEST)
    if manifest.get("schema_id") != "scratchbird.engine_listener.external_review_closure.v1":
        fail("external_review_manifest_schema")
    policy = manifest.get("public_release_policy", {})
    if not isinstance(policy, dict):
        fail("external_review_manifest_policy")
    expected_policy = {
        "proof_source": "project_tests_fixture_and_generated_matrix",
        "docs_findings_required_at_runtime": False,
        "docs_execution-plans_required_at_runtime": False,
        "git_history_required": False,
        "external_human_audit_completed": False,
        "product_completion_claim": False,
    }
    for key, expected in expected_policy.items():
        if policy.get(key) != expected:
            fail(f"external_review_manifest_policy_value:{key}:{policy.get(key)!r}")
    expected_families = {
        "cluster_boundary_review",
        "database_correctness_review",
        "reference_sblr_parser_boundary_review",
        "independent_security_review",
        "operational_reliability_review",
    }
    required_families = set(manifest.get("required_review_families", []))
    if required_families != expected_families:
        fail(f"external_review_manifest_family_set:{sorted(required_families)}")
    rows = manifest.get("rows", [])
    if not isinstance(rows, list) or len(rows) != len(expected_families):
        fail("external_review_manifest_rows")
    seen_ids: set[str] = set()
    seen_families: set[str] = set()
    repo_root = project_root.parent
    for index, row in enumerate(rows, start=1):
        if not isinstance(row, dict):
            fail(f"external_review_manifest_row_shape:{index}")
        finding_id = str(row.get("finding_id", ""))
        if not finding_id or finding_id in seen_ids:
            fail(f"external_review_manifest_finding_id:{index}:{finding_id}")
        seen_ids.add(finding_id)
        family = str(row.get("review_family", ""))
        if family not in expected_families:
            fail(f"external_review_manifest_family:{finding_id}:{family}")
        seen_families.add(family)
        if row.get("closure_status") != (
            "linux_review_closure_evidence_proven_external_validation_pending"
        ):
            fail(f"external_review_manifest_status:{finding_id}:{row.get('closure_status')}")
        gates = row.get("required_gates", [])
        refs = row.get("evidence_refs", [])
        if not isinstance(gates, list) or len(gates) < 4:
            fail(f"external_review_manifest_gates:{finding_id}")
        if not isinstance(refs, list) or len(refs) < 4:
            fail(f"external_review_manifest_refs:{finding_id}")
        for reference in refs:
            require_traceability_reference(
                project_root,
                repo_root,
                str(reference),
                f"{finding_id}:evidence_ref",
            )
    if seen_families != expected_families:
        fail(f"external_review_manifest_seen_family_set:{sorted(seen_families)}")
    return manifest


def validate_external_review_closure_package(project_root: Path,
                                             build_root: Path) -> dict[str, Any]:
    manifest = validate_external_review_manifest_shape(project_root)
    required_review_families = set(manifest["required_review_families"])
    manifest_rows = {
        str(row["review_family"]): row
        for row in manifest.get("rows", [])
        if isinstance(row, dict)
    }
    rows: list[dict[str, str]] = []
    seen_families: set[str] = set()
    for row_spec in EXTERNAL_REVIEW_CLOSURE_ROWS:
        family = str(row_spec["review_family"])
        if family not in manifest_rows:
            fail(f"external_review_manifest_row_missing:{family}")
        manifest_row = manifest_rows[family]
        if tuple(manifest_row.get("required_gates", ())) != tuple(row_spec["behavior_gates"]):
            fail(f"external_review_manifest_gate_drift:{family}")
        row = aggregation_row(
            project_root,
            build_root,
            row_spec,
            "review_policy",
            "external_review_closure_package_behavior_gate_with_tokenized_source_evidence",
            "linux_independent_review_closure_package_proven_external_validation_pending",
        )
        row["review_family"] = family
        row["manifest_finding_id"] = str(manifest_row["finding_id"])
        row["closure_status"] = str(manifest_row["closure_status"])
        row["external_human_audit_completed"] = "false"
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
        rows.append(row)
        seen_families.add(family)
    missing_families = sorted(required_review_families - seen_families)
    if missing_families:
        fail("external_review_missing_families:" + ",".join(missing_families))
    return {
        "schema_version": 1,
        "gate": "ELER-110",
        "private_docs_required": False,
        "git_history_required": False,
        "external_human_audit_completed": False,
        "matrix_id": "ELER_110_INDEPENDENT_EXTERNAL_REVIEW_CLOSURE_PACKAGE_MATRIX",
        "traceability_ref": INDEPENDENT_REVIEW_CLOSURE_TRACEABILITY_REF,
        "review_packet_id": manifest["review_packet_id"],
        "review_family_count": len(rows),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_gold_enterprise_readiness(project_root: Path,
                                       build_root: Path) -> dict[str, Any]:
    project_only = validate_project_only_implementation_proof(project_root, build_root)
    external_review = validate_external_review_closure_package(project_root, build_root)
    required_surfaces = {
        "claim_boundary",
        "reference_sblr_sbsql",
        "final_project_review_package",
        "integrated_crash_recovery",
        "ops_release_docs_support",
        "security_fuzz",
        "soak_performance_compatibility",
    }
    rows: list[dict[str, str]] = []
    seen_surfaces: set[str] = set()
    for row_spec in GOLD_ENTERPRISE_READINESS_ROWS:
        enriched = {
            **row_spec,
            "evidence_files": {
                "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py": (
                    "GOLD_ENTERPRISE_READINESS_ROWS",
                    "FINAL_PROJECT_IMPLEMENTATION_TRACEABILITY_REF",
                    "INDEPENDENT_REVIEW_CLOSURE_TRACEABILITY_REF",
                    "product_completion_claim",
                ),
                "release/README.md": (
                    "Linux is the first validated execution platform",
                    "declared compatibility targets",
                    "cross-platform proof pending",
                ),
            },
        }
        row = aggregation_row(
            project_root,
            build_root,
            enriched,
            "readiness_policy",
            "linux_gold_enterprise_release_candidate_aggregation",
            "linux_gold_enterprise_release_candidate_evidence_proven_cross_platform_and_external_audit_pending",
        )
        proof_refs = tuple(str(ref) for ref in row_spec.get("proof_refs", ()))
        if not proof_refs:
            fail(f"gold_readiness_missing_proof_refs:{row['row_id']}")
        for proof_ref in proof_refs:
            reject_private_reference(proof_ref, f"{row['row_id']}:proof_ref")
        row["proof_refs"] = ";".join(proof_refs)
        row["platform_scope"] = "linux_proven_cross_platform_execution_pending"
        row["external_audit_scope"] = "external_zero_issue_audit_pending"
        row["product_completion_claim"] = "false"
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
        rows.append(row)
        seen_surfaces.add(row["surface"])
    missing_surfaces = sorted(required_surfaces - seen_surfaces)
    if missing_surfaces:
        fail("gold_readiness_missing_surfaces:" + ",".join(missing_surfaces))
    if project_only.get("traceability_ref") != FINAL_PROJECT_IMPLEMENTATION_TRACEABILITY_REF:
        fail("gold_readiness_project_only_ref_drift")
    if external_review.get("traceability_ref") != INDEPENDENT_REVIEW_CLOSURE_TRACEABILITY_REF:
        fail("gold_readiness_external_review_ref_drift")
    return {
        "schema_version": 1,
        "gate": "ELER-112",
        "private_docs_required": False,
        "git_history_required": False,
        "external_human_audit_completed": False,
        "platform_scope": "linux_enterprise_release_candidate_cross_platform_execution_pending",
        "matrix_id": "ELER_112_GOLD_ENTERPRISE_READINESS_AGGREGATION_MATRIX",
        "traceability_ref": GOLD_ENTERPRISE_READINESS_TRACEABILITY_REF,
        "project_only_traceability_ref": project_only["traceability_ref"],
        "external_review_traceability_ref": external_review["traceability_ref"],
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_third_audit_evidence_package(project_root: Path,
                                          build_root: Path) -> dict[str, Any]:
    gold = validate_gold_enterprise_readiness(project_root, build_root)
    rows: list[dict[str, str]] = []
    for row_spec in THIRD_AUDIT_EVIDENCE_ROWS:
        enriched = {
            **row_spec,
            "evidence_files": {
                "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py": (
                    "THIRD_AUDIT_EVIDENCE_ROWS",
                    "THIRD_AUDIT_PACKAGE_TRACEABILITY_REF",
                    "external_zero_issue_audit_pending",
                    "product_completion_claim",
                ),
                "release/README.md": (
                    "Linux is the first validated execution platform",
                    "declared compatibility targets",
                    "cross-platform proof pending",
                ),
            },
        }
        row = aggregation_row(
            project_root,
            build_root,
            enriched,
            "audit_policy",
            "third_audit_evidence_package_behavior_gate_with_tokenized_source_evidence",
            "linux_third_audit_evidence_package_proven_external_auditor_pending",
        )
        proof_refs = tuple(str(ref) for ref in row_spec.get("proof_refs", ()))
        if not proof_refs:
            fail(f"third_audit_missing_proof_refs:{row['row_id']}")
        row["proof_refs"] = ";".join(proof_refs)
        row["external_zero_issue_audit_status"] = "pending_external_auditor_decision"
        row["platform_scope"] = "linux_proven_cross_platform_execution_pending"
        row["product_completion_claim"] = "false"
        for field, value in row.items():
            reject_private_reference(str(value), f"{row['row_id']}:{field}")
        rows.append(row)
    if gold.get("traceability_ref") != GOLD_ENTERPRISE_READINESS_TRACEABILITY_REF:
        fail("third_audit_gold_traceability_ref_drift")
    return {
        "schema_version": 1,
        "gate": "ELER-090",
        "private_docs_required": False,
        "git_history_required": False,
        "external_zero_issue_audit_status": "pending_external_auditor_decision",
        "platform_scope": "linux_enterprise_release_candidate_cross_platform_execution_pending",
        "matrix_id": "ELER_090_THIRD_AUDIT_EVIDENCE_PACKAGE_MATRIX",
        "traceability_ref": THIRD_AUDIT_PACKAGE_TRACEABILITY_REF,
        "gold_readiness_traceability_ref": gold["traceability_ref"],
        "build_root_recorded": False,
        "rows": rows,
    }


def canonical_json_digest(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"))
    return sha256_text(encoded)


def load_json_object(path: Path) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as handle:
            loaded = json.load(handle)
    except FileNotFoundError:
        fail(f"determinism_json_missing:{path.name}")
    except json.JSONDecodeError as exc:
        fail(f"determinism_json_invalid:{path.name}:{exc}")
    if not isinstance(loaded, dict):
        fail(f"determinism_json_not_object:{path.name}")
    return loaded


def sync_manifest_path(project_root: Path) -> Path:
    return project_root / SBLR_TRACEABILITY_FIXTURE_ROOT / "sbsql_sync_requirements.json"


def sync_parser_handoff_key(reference: str) -> str:
    return sha256_text(reference)


def require_parser_handoff_reference(reference: str, context: str) -> None:
    if not reference:
        fail(f"sbsql_sync_parser_handoff_missing:{context}")
    path_part = reference.split("#", 1)[0]
    if Path(path_part).is_absolute() or ".." in Path(path_part).parts:
        fail(f"sbsql_sync_parser_handoff_not_repo_relative:{context}:{reference}")
    if not reference.startswith(SBSQL_ACTIVE_EXECUTION_PLAN_PREFIX):
        fail(f"sbsql_sync_parser_handoff_not_active_controller:{context}:{reference}")
    for prefix in SBSQL_SUPERSEDED_EXECUTION_PLAN_PREFIXES:
        if reference.startswith(prefix):
            fail(f"sbsql_sync_parser_handoff_superseded:{context}:{reference}")
    for forbidden in (DOCS_PATH + "audit", DOCS_PATH + "findings", "/" + "home" + "/", "http://", "https://"):
        if forbidden in reference:
            fail(f"sbsql_sync_parser_handoff_forbidden_reference:{context}:{reference}")


def validate_sbsql_sync_item(project_root: Path,
                             repo_root: Path,
                             item: dict[str, Any],
                             context: str) -> None:
    if item.get("sbsql_style") != SBSQL_NATIVE_STYLE:
        fail(f"sbsql_sync_non_native_style:{context}:{item.get('sbsql_style')}")
    change_kind = str(item.get("change_kind", ""))
    if not change_kind:
        fail(f"sbsql_sync_missing_change_kind:{context}")
    lowered_values = [
        str(value).lower()
        for key, value in item.items()
        if key not in {"fixture_file", "path", "row_selector"} and not isinstance(value, dict)
    ]
    for token in SBSQL_FORBIDDEN_STYLE_TOKENS:
        if any(token in value for value in lowered_values):
            fail(f"sbsql_sync_forbidden_style_token:{context}:{token}")
    for field in SBSQL_SYNC_REQUIRED_FIELDS:
        if not str(item.get(field, "")):
            fail(f"sbsql_sync_missing_required_field:{context}:{field}")
    require_traceability_reference(
        project_root,
        repo_root,
        str(item["sbsql_cst_reference"]),
        f"{context}:sbsql_cst_reference",
    )
    require_traceability_reference(
        project_root,
        repo_root,
        str(item["ast_sblr_reference"]),
        f"{context}:ast_sblr_reference",
    )
    require_traceability_reference(
        project_root,
        repo_root,
        str(item["reference_normalization_reference"]),
        f"{context}:reference_normalization_reference",
    )
    require_parser_handoff_reference(str(item["parser_execution_plan_sync_reference"]), context)


def validate_sbsql_sync_manifest_shape(project_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    manifest = load_json_object(sync_manifest_path(project_root))
    if manifest.get("schema_id") != "scratchbird.sblr_surface.sbsql_sync_requirements.v1":
        fail("sbsql_sync_manifest_schema")
    if manifest.get("fixture_root") != (
        "project/tests/sblr_surface/fixtures/reference_sblr_interface_gap_2026_06_03"
    ):
        fail("sbsql_sync_manifest_fixture_root")
    style_policy = manifest.get("style_policy", {})
    if not isinstance(style_policy, dict):
        fail("sbsql_sync_manifest_style_policy")
    if style_policy.get("valid_style_field") != "sbsql_style":
        fail("sbsql_sync_manifest_style_field")
    accepted_styles = set(style_policy.get("accepted_sbsql_styles", []))
    rejected_styles = set(style_policy.get("rejected_sbsql_styles", []))
    if SBSQL_NATIVE_STYLE not in accepted_styles:
        fail("sbsql_sync_manifest_missing_native_style")
    if "compatibility_dialect_paste_through" not in rejected_styles:
        fail("sbsql_sync_manifest_missing_rejected_style")
    if tuple(manifest.get("required_reference_fields", [])) != SBSQL_SYNC_REQUIRED_FIELDS:
        fail("sbsql_sync_manifest_required_fields")
    public_policy = manifest.get("public_release_policy", {})
    if not isinstance(public_policy, dict):
        fail("sbsql_sync_manifest_public_policy")
    if public_policy.get("proof_source") != "project_tests_manifest_and_generated_matrix":
        fail("sbsql_sync_manifest_public_policy_source")
    if public_policy.get("docs_execution-plans_required_at_runtime") is not False:
        fail("sbsql_sync_manifest_docs_execution-plans_runtime_dependency")
    if public_policy.get("product_completion_claim") is not False:
        fail("sbsql_sync_manifest_product_completion_claim")

    rows = manifest.get("rows", [])
    if not isinstance(rows, list) or len(rows) < 6:
        fail("sbsql_sync_manifest_representative_rows")
    seen_ids: set[str] = set()
    seen_files: set[str] = set()
    expected_representative_files = {
        "SERVER_AUTHORITY_SURFACE_MATRIX.csv",
        "NON_DIRECT_FUNCTION_SURFACE_MATRIX.csv",
        "EXPLICIT_UNSUPPORTED_SURFACE_MATRIX.csv",
        "SBSQL_SBLR_FAMILY_RECONCILIATION_MATRIX.csv",
        "SBLR_STALE_DEFERRED_ALIAS_CLEANUP_MATRIX.csv",
        "REFERENCE_INTERNAL_META_OPCODE_CLEANUP_MATRIX.csv",
    }
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            fail(f"sbsql_sync_manifest_representative_row_shape:{index}")
        sync_id = str(row.get("sync_id", f"row-{index}"))
        if sync_id in seen_ids:
            fail(f"sbsql_sync_manifest_duplicate_sync_id:{sync_id}")
        seen_ids.add(sync_id)
        validate_sbsql_sync_item(project_root, repo_root, row, sync_id)
        fixture_file = str(row.get("fixture_file", ""))
        selector = row.get("row_selector", {})
        if fixture_file not in expected_representative_files:
            fail(f"sbsql_sync_manifest_unknown_representative_fixture:{sync_id}:{fixture_file}")
        if not isinstance(selector, dict) or not selector.get("column") or selector.get("value") is None:
            fail(f"sbsql_sync_manifest_bad_selector:{sync_id}")
        fixture_path = project_root / SBLR_TRACEABILITY_FIXTURE_ROOT / fixture_file
        matches = [
            csv_row for csv_row in read_traceability_csv(fixture_path)
            if csv_row.get(str(selector["column"])) == str(selector["value"])
        ]
        if len(matches) != 1:
            fail(f"sbsql_sync_manifest_selector_count:{sync_id}:{len(matches)}")
        seen_files.add(fixture_file)
    if seen_files != expected_representative_files:
        fail(
            "sbsql_sync_manifest_representative_fixture_set:"
            f"actual={sorted(seen_files)}"
        )
    return manifest


def sbsql_sync_fixture_rows(project_root: Path,
                            manifest: dict[str, Any]) -> list[dict[str, str]]:
    repo_root = project_root.parent
    policy = manifest.get("full_coverage_policy", {})
    if not isinstance(policy, dict):
        fail("sbsql_sync_full_policy_shape")
    if policy.get("coverage_model") != "row_expanded_by_gate":
        fail("sbsql_sync_full_policy_coverage_model")
    if policy.get("required_coverage_scope") != "all_rows":
        fail("sbsql_sync_full_policy_scope")
    requirements = manifest.get("full_coverage_requirements", [])
    if not isinstance(requirements, list):
        fail("sbsql_sync_full_requirements_shape")
    expected_files = set(SBLR_TRACEABILITY_CSV_FILES)
    seen_files: set[str] = set()
    matrix_rows: list[dict[str, str]] = []
    for index, requirement in enumerate(requirements):
        if not isinstance(requirement, dict):
            fail(f"sbsql_sync_full_requirement_shape:{index}")
        coverage_id = str(requirement.get("coverage_id", f"coverage-{index}"))
        validate_sbsql_sync_item(project_root, repo_root, requirement, coverage_id)
        fixture_file = str(requirement.get("fixture_file", ""))
        if fixture_file not in expected_files:
            fail(f"sbsql_sync_full_requirement_fixture:{coverage_id}:{fixture_file}")
        if fixture_file in seen_files:
            fail(f"sbsql_sync_full_requirement_duplicate_fixture:{fixture_file}")
        seen_files.add(fixture_file)
        if requirement.get("coverage_scope") != "all_rows":
            fail(f"sbsql_sync_full_requirement_scope:{coverage_id}")
        selector = str(requirement.get("row_selector_column", ""))
        csv_rows = read_traceability_csv(
            project_root / SBLR_TRACEABILITY_FIXTURE_ROOT / fixture_file
        )
        if not csv_rows:
            fail(f"sbsql_sync_full_requirement_empty_fixture:{fixture_file}")
        if selector not in csv_rows[0]:
            fail(f"sbsql_sync_full_requirement_selector:{coverage_id}:{selector}")
        for row_index, csv_row in enumerate(csv_rows, start=1):
            selected = selector_value(csv_row, selector, fixture_file, row_index)
            row_digest = sha256_text(
                json.dumps(csv_row, sort_keys=True, separators=(",", ":"))
            )
            matrix_rows.append(
                {
                    "trace_id": (
                        f"ELER089-FIXTURE-{Path(fixture_file).stem}-"
                        f"{row_index:04d}-{row_digest[:16]}"
                    ),
                    "trace_kind": "sbsql_sync_fixture_row",
                    "source_id": f"{fixture_file}:{selector}={selected}",
                    "coverage_id": coverage_id,
                    "change_kind": str(requirement["change_kind"]),
                    "sbsql_style": str(requirement["sbsql_style"]),
                    "sbsql_cst_reference": str(requirement["sbsql_cst_reference"]),
                    "ast_sblr_reference": str(requirement["ast_sblr_reference"]),
                    "reference_normalization_reference": str(requirement["reference_normalization_reference"]),
                    "parser_execution_plan_sync_key": sync_parser_handoff_key(
                        str(requirement["parser_execution_plan_sync_reference"])
                    ),
                    "source_row_sha256": row_digest,
                    "sync_status": str(policy.get("required_sync_status", "")),
                    "proof_class": "row_expanded_full_sbsql_sync",
                    "evidence_artifact": (
                        "project/tests/sblr_surface/fixtures/"
                        f"reference_sblr_interface_gap_2026_06_03/{fixture_file}"
                    ),
                    "product_completion_claim": "false",
                }
            )
    if seen_files != expected_files:
        fail(f"sbsql_sync_full_requirement_file_set:{sorted(seen_files)}")
    minimum_rows = int(policy.get("minimum_fixture_rows", 0) or 0)
    if len(matrix_rows) < minimum_rows or minimum_rows < 700:
        fail(f"sbsql_sync_full_matrix_too_small:{len(matrix_rows)}:{minimum_rows}")
    return matrix_rows


def sbsql_sync_resource_rows(project_root: Path,
                             manifest: dict[str, Any]) -> list[dict[str, str]]:
    repo_root = project_root.parent
    determinism_manifest = load_json_object(project_root / GENERATED_RESOURCE_DETERMINISM_MANIFEST)
    determinism_resources = {
        str(resource.get("path", "")): str(resource.get("resource_kind", ""))
        for resource in determinism_manifest.get("resources", [])
        if isinstance(resource, dict)
    }
    resources = manifest.get("engine_sblr_resource_sync_requirements", [])
    if not isinstance(resources, list):
        fail("sbsql_sync_resource_requirements_shape")
    seen_paths: set[str] = set()
    expected_paths = {
        str(resource.get("path", ""))
        for resource in resources
        if isinstance(resource, dict)
    }
    matrix_rows: list[dict[str, str]] = []
    for index, resource in enumerate(resources):
        if not isinstance(resource, dict):
            fail(f"sbsql_sync_resource_requirement_shape:{index}")
        sync_id = str(resource.get("resource_sync_id", f"resource-{index}"))
        validate_sbsql_sync_item(project_root, repo_root, resource, sync_id)
        path = str(resource.get("path", ""))
        if path not in determinism_resources:
            fail(f"sbsql_sync_resource_not_deterministic_resource:{sync_id}:{path}")
        if path in seen_paths:
            fail(f"sbsql_sync_resource_duplicate_path:{path}")
        seen_paths.add(path)
        if determinism_resources[path] != str(resource.get("resource_kind", "")):
            fail(f"sbsql_sync_resource_kind_mismatch:{path}")
        candidate = repo_root / path
        if not candidate.is_file():
            fail(f"sbsql_sync_resource_missing:{path}")
        digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
        matrix_rows.append(
            {
                "trace_id": f"ELER089-RESOURCE-{sha256_text(path)[:16]}",
                "trace_kind": "engine_sblr_resource_sync",
                "source_id": path,
                "coverage_id": sync_id,
                "change_kind": str(resource["change_kind"]),
                "sbsql_style": str(resource["sbsql_style"]),
                "sbsql_cst_reference": str(resource["sbsql_cst_reference"]),
                "ast_sblr_reference": str(resource["ast_sblr_reference"]),
                "reference_normalization_reference": str(resource["reference_normalization_reference"]),
                "parser_execution_plan_sync_key": sync_parser_handoff_key(
                    str(resource["parser_execution_plan_sync_reference"])
                ),
                "source_row_sha256": digest,
                "sync_status": "sbsql_sync_proven_for_current_engine_listener_surface",
                "proof_class": "engine_sblr_resource_sync",
                "evidence_artifact": path,
                "product_completion_claim": "false",
            }
        )
    if seen_paths != expected_paths:
        fail(f"sbsql_sync_resource_path_set:{sorted(seen_paths)}")
    return matrix_rows


def validate_sbsql_parser_spec_execution_plan_sync(project_root: Path,
                                             build_root: Path) -> dict[str, Any]:
    manifest = validate_sbsql_sync_manifest_shape(project_root)
    fixture_rows = sbsql_sync_fixture_rows(project_root, manifest)
    resource_rows = sbsql_sync_resource_rows(project_root, manifest)
    rows = fixture_rows + resource_rows
    seen_trace_ids: set[str] = set()
    for row in rows:
        trace_id = row["trace_id"]
        if trace_id in seen_trace_ids:
            fail(f"sbsql_sync_duplicate_trace_id:{trace_id}")
        seen_trace_ids.add(trace_id)
        if row["sbsql_style"] != SBSQL_NATIVE_STYLE:
            fail(f"sbsql_sync_matrix_non_native_style:{trace_id}")
        if row["sync_status"] != "sbsql_sync_proven_for_current_engine_listener_surface":
            fail(f"sbsql_sync_matrix_status:{trace_id}:{row['sync_status']}")
        if row["product_completion_claim"] != "false":
            fail(f"sbsql_sync_matrix_product_claim:{trace_id}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{trace_id}:{field}")
        for field in ("sbsql_cst_reference", "ast_sblr_reference", "reference_normalization_reference"):
            require_traceability_reference(
                project_root,
                project_root.parent,
                row[field],
                f"{trace_id}:{field}",
            )
    if len(fixture_rows) < 700:
        fail(f"sbsql_sync_fixture_rows_too_small:{len(fixture_rows)}")
    if len(resource_rows) < 8:
        fail(f"sbsql_sync_resource_rows_too_small:{len(resource_rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-089",
        "private_docs_required": False,
        "git_history_required": False,
        "docs_execution-plans_required_at_runtime": False,
        "matrix_id": "ELER_089_SBSQL_PARSER_SPEC_EXECUTION_PLAN_SYNC_MATRIX",
        "representative_sync_row_count": len(manifest.get("rows", [])),
        "fixture_row_count": len(fixture_rows),
        "engine_sblr_resource_count": len(resource_rows),
        "build_root_recorded": False,
        "rows": rows,
    }


def parser_contract_manifest_path(project_root: Path) -> Path:
    return project_root / PARSER_FACING_CONTRACT_FREEZE_MANIFEST


def parser_contract_row_id(section: str, source_id: str, index: int) -> str:
    digest = sha256_text(f"{section}|{source_id}|{index}")[:16]
    section_key = section.upper().replace("_", "-")
    return f"ELER085-{section_key}-{index:04d}-{digest}"


def parser_contract_row(manifest: dict[str, Any],
                        index: int,
                        section: str,
                        source_id: str,
                        source_kind: str,
                        published_value: str,
                        diagnostic_vector: str,
                        route_classification: str,
                        source_sha256: str,
                        evidence_artifact: str) -> dict[str, str]:
    return {
        "contract_row_id": parser_contract_row_id(section, source_id, index),
        "freeze_id": str(manifest["freeze_id"]),
        "freeze_version": str(manifest["freeze_version"]),
        "compatibility_version": str(manifest["compatibility_version"]),
        "section": section,
        "source_id": source_id,
        "source_kind": source_kind,
        "published_value": published_value,
        "diagnostic_vector": diagnostic_vector,
        "route_classification": route_classification,
        "source_sha256": source_sha256,
        "evidence_artifact": evidence_artifact,
        "freeze_status": "frozen_parser_contract_current",
        "public_release_safe": "true",
        "product_completion_claim": "false",
    }


def validate_parser_contract_source_resources(
    project_root: Path,
    manifest: dict[str, Any],
) -> dict[str, dict[str, str]]:
    repo_root = project_root.parent
    resources = manifest.get("source_resources", [])
    if not isinstance(resources, list) or len(resources) < 12:
        fail("parser_contract_source_resources_too_small")
    source_by_kind: dict[str, dict[str, str]] = {}
    seen_paths: set[str] = set()
    for index, resource in enumerate(resources, start=1):
        if not isinstance(resource, dict):
            fail(f"parser_contract_source_resource_shape:{index}")
        path = str(resource.get("path", ""))
        resource_kind = str(resource.get("resource_kind", ""))
        expected = str(resource.get("sha256", ""))
        if not path or not resource_kind or not expected:
            fail(f"parser_contract_source_resource_missing_field:{index}")
        if path in seen_paths:
            fail(f"parser_contract_source_resource_duplicate_path:{path}")
        if resource_kind in source_by_kind:
            fail(f"parser_contract_source_resource_duplicate_kind:{resource_kind}")
        seen_paths.add(path)
        reject_private_reference(path, f"parser_contract_source_resource:{path}")
        if Path(path).is_absolute() or ".." in Path(path).parts:
            fail(f"parser_contract_source_resource_not_repo_relative:{path}")
        candidate = repo_root / path
        if not candidate.is_file():
            fail(f"parser_contract_source_resource_missing:{path}")
        digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
        if digest != expected:
            fail(f"parser_contract_source_resource_hash_drift:{path}")
        source_by_kind[resource_kind] = {
            "path": path,
            "resource_kind": resource_kind,
            "sha256": digest,
        }

    manifest_ref = f"project/{PARSER_FACING_CONTRACT_FREEZE_MANIFEST.as_posix()}"
    manifest_digest = hashlib.sha256(parser_contract_manifest_path(project_root).read_bytes()).hexdigest()
    source_by_kind["parser_facing_contract_manifest"] = {
        "path": manifest_ref,
        "resource_kind": "parser_facing_contract_manifest",
        "sha256": manifest_digest,
    }
    return source_by_kind


def validate_parser_contract_freeze_manifest_shape(
    project_root: Path,
) -> tuple[dict[str, Any], dict[str, dict[str, str]], dict[str, int]]:
    manifest = load_json_object(parser_contract_manifest_path(project_root))
    if manifest.get("schema_id") != "scratchbird.engine_listener.parser_facing_contract_freeze.v1":
        fail("parser_contract_freeze_manifest_schema")
    for field in ("freeze_id", "freeze_version", "compatibility_version"):
        value = str(manifest.get(field, ""))
        if not value:
            fail(f"parser_contract_freeze_manifest_missing:{field}")
        reject_private_reference(value, f"parser_contract_freeze_manifest:{field}")
    policy = manifest.get("public_release_policy", {})
    if not isinstance(policy, dict):
        fail("parser_contract_freeze_public_policy_shape")
    if policy.get("proof_source") != "project_tests_manifest_and_generated_matrix":
        fail("parser_contract_freeze_public_policy_source")
    if policy.get("docs_execution-plans_required_at_runtime") is not False:
        fail("parser_contract_freeze_docs_execution-plans_runtime_dependency")
    if policy.get("git_history_required") is not False:
        fail("parser_contract_freeze_git_history_dependency")
    if policy.get("product_completion_claim") is not False:
        fail("parser_contract_freeze_product_completion_claim")
    if tuple(manifest.get("required_contract_sections", [])) != PARSER_CONTRACT_REQUIRED_SECTIONS:
        fail("parser_contract_freeze_required_section_set")

    source_by_kind = validate_parser_contract_source_resources(project_root, manifest)
    section_sources = manifest.get("contract_section_sources", [])
    if not isinstance(section_sources, list):
        fail("parser_contract_freeze_section_sources_shape")
    section_minimums: dict[str, int] = {}
    seen_source_kinds: set[str] = set()
    for index, item in enumerate(section_sources, start=1):
        if not isinstance(item, dict):
            fail(f"parser_contract_freeze_section_source_shape:{index}")
        section = str(item.get("section", ""))
        source_kind = str(item.get("source_resource_kind", ""))
        try:
            minimum_rows = int(item.get("minimum_rows", 0))
        except (TypeError, ValueError):
            fail(f"parser_contract_freeze_section_minimum_invalid:{section}")
        if section not in PARSER_CONTRACT_REQUIRED_SECTIONS:
            fail(f"parser_contract_freeze_unknown_section:{section}")
        if section in section_minimums:
            fail(f"parser_contract_freeze_duplicate_section:{section}")
        if source_kind not in source_by_kind:
            fail(f"parser_contract_freeze_unknown_source_kind:{section}:{source_kind}")
        if minimum_rows <= 0:
            fail(f"parser_contract_freeze_section_minimum_empty:{section}")
        section_minimums[section] = minimum_rows
        seen_source_kinds.add(source_kind)
    if set(section_minimums) != set(PARSER_CONTRACT_REQUIRED_SECTIONS):
        fail("parser_contract_freeze_section_minimum_set")
    if "parser_facing_contract_manifest" not in seen_source_kinds:
        fail("parser_contract_freeze_manifest_source_not_used")
    return manifest, source_by_kind, section_minimums


def parser_contract_envelope_rows(
    project_root: Path,
    manifest: dict[str, Any],
    source_by_kind: dict[str, dict[str, str]],
) -> list[dict[str, str]]:
    snapshot = load_json_object(
        project_root / SBLR_TRACEABILITY_FIXTURE_ROOT / "sblr_primary_family_snapshot.json"
    )
    if snapshot.get("schema_id") != "scratchbird.sblr_surface.primary_sblr_family_snapshot.v1":
        fail("parser_contract_sblr_family_snapshot_schema")
    declared = snapshot.get("declared_envelope_families", [])
    bound = snapshot.get("envelope_to_opcode_family_bindings", [])
    if not isinstance(declared, list) or not isinstance(bound, list):
        fail("parser_contract_sblr_family_snapshot_shape")
    if len(declared) != int(snapshot.get("declared_envelope_family_count", -1)):
        fail("parser_contract_sblr_family_declared_count")
    if len(bound) != int(snapshot.get("envelope_to_opcode_family_binding_count", -1)):
        fail("parser_contract_sblr_family_bound_count")
    if len(declared) != EXPECTED_PRIMARY_SBLR_FAMILY_ROWS or len(bound) != EXPECTED_PRIMARY_SBLR_FAMILY_ROWS:
        fail(f"parser_contract_sblr_family_count:{len(declared)}:{len(bound)}")
    if set(declared) != set(bound):
        fail("parser_contract_sblr_family_binding_set")
    if len(set(declared)) != len(declared):
        fail("parser_contract_sblr_family_duplicate")
    source = source_by_kind["parser_facing_sblr_family_snapshot"]
    rows: list[dict[str, str]] = []
    for index, family in enumerate(declared, start=1):
        rows.append(
            parser_contract_row(
                manifest,
                index,
                "sblr_envelope_families",
                str(family),
                source["resource_kind"],
                str(family),
                "SBLR.FAMILY_INVALID",
                "parser_envelope_family_admitted_only_when_bound_to_registered_family",
                source["sha256"],
                source["path"],
            )
        )
    return rows


def parse_inline_yaml_mapping(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for item in re.split(r",\s+", body):
        if ": " not in item:
            fail(f"parser_contract_opcode_mapping_malformed:{body}")
        key, value = item.split(": ", 1)
        fields[key.strip()] = value.strip()
    return fields


def parse_yaml_scalar(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        return value[1:-1]
    return value


def parse_sblr_opcode_entries(project_root: Path) -> list[dict[str, str]]:
    repo_root = project_root.parent
    registry_path = repo_root / "public_contract_snapshot"
    text = registry_path.read_text(encoding="utf-8")
    entries: list[dict[str, str]] = []
    seen_names: set[str] = set()
    seen_codes: set[str] = set()
    coded_required_fields = (
        "family",
        "status",
        "operand_contract",
        "result_contract",
        "transaction_effect",
        "security_class",
        "search_key",
    )
    current: dict[str, str] | None = None

    def append_current() -> None:
        nonlocal current
        if current is not None:
            entries.append(current)
            current = None

    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("- {name:"):
            append_current()
            body = stripped.removeprefix("- ").strip()
            if not body.startswith("{") or not body.endswith("}"):
                fail(f"parser_contract_opcode_entry_not_inline:{line_number}")
            entry = parse_inline_yaml_mapping(body[1:-1])
            entry["_line"] = str(line_number)
            entries.append(entry)
            continue
        if stripped.startswith("- name:"):
            append_current()
            current = {
                "name": parse_yaml_scalar(stripped.split(":", 1)[1]),
                "_line": str(line_number),
            }
            continue
        if current is not None and ":" in stripped and not stripped.startswith("#"):
            key, value = stripped.split(":", 1)
            current[key.strip()] = parse_yaml_scalar(value)
    append_current()

    for entry in entries:
        line_number = entry.get("_line", "0")
        name = entry.get("name", "")
        if not re.fullmatch(r"SBLR_[A-Z0-9_]+", name):
            fail(f"parser_contract_opcode_name_invalid:{line_number}:{name}")
        if name in seen_names:
            fail(f"parser_contract_opcode_name_duplicate:{name}")
        seen_names.add(name)
        code = entry.get("code", "")
        if code:
            if re.fullmatch(r"0x[0-9A-Fa-f]+", code):
                code_value = int(code, 16)
            elif re.fullmatch(r"[0-9]+", code):
                code_value = int(code, 10)
            else:
                fail(f"parser_contract_opcode_code_invalid:{name}:{code}")
            if not 0 <= code_value <= 0xFFFF:
                fail(f"parser_contract_opcode_code_out_of_range:{name}:{code}")
            normalized_code = f"0x{code_value:04X}"
            if normalized_code in seen_codes:
                fail(f"parser_contract_opcode_code_duplicate:{normalized_code}")
            seen_codes.add(normalized_code)
            entry.setdefault(
                "operand_contract",
                "registry_default_validated_by_sblr_operation_matrix",
            )
            entry.setdefault(
                "result_contract",
                "registry_default_validated_by_sblr_operation_matrix",
            )
            missing = [field for field in coded_required_fields if not entry.get(field)]
            if missing:
                fail(f"parser_contract_opcode_missing_fields:{name}:{missing}")
            if entry["status"] not in {"required", "optional", "reserved", "deferred", "forbidden"}:
                fail(f"parser_contract_opcode_status_invalid:{name}:{entry['status']}")
            entry["code"] = normalized_code
        elif not (entry.get("resolution") and entry.get("diagnostic")):
            fail(f"parser_contract_non_opcode_mapping_missing_fields:{name}")
    if len(entries) < EXPECTED_SBLR_OPCODE_ROWS or len(seen_codes) < EXPECTED_SBLR_OPCODE_ROWS:
        fail(f"parser_contract_opcode_entries_too_small:{len(entries)}:{len(seen_codes)}")
    return entries


def parser_contract_opcode_rows(
    project_root: Path,
    manifest: dict[str, Any],
    source_by_kind: dict[str, dict[str, str]],
) -> list[dict[str, str]]:
    source = source_by_kind["parser_facing_sblr_opcode_registry"]
    rows: list[dict[str, str]] = []
    for index, entry in enumerate(parse_sblr_opcode_entries(project_root), start=1):
        name = entry["name"]
        published_fields = {
            key: entry.get(key, "")
            for key in (
                "code",
                "family",
                "status",
                "operand_contract",
                "result_contract",
                "transaction_effect",
                "security_class",
                "resolution",
                "target",
                "search_key",
            )
        }
        published_value = json.dumps(
            published_fields,
            sort_keys=True,
            separators=(",", ":"),
        )
        route = (
            "registered_public_root_opcode"
            if entry.get("code")
            else "retired_descriptor_mapping_not_root_admissible"
        )
        rows.append(
            parser_contract_row(
                manifest,
                index,
                "sblr_opcode_bindings",
                name,
                source["resource_kind"],
                published_value,
                entry.get("diagnostic", entry.get("search_key", "SBLR.OPERATION_UNSUPPORTED")),
                route,
                source["sha256"],
                source["path"],
            )
        )
    return rows


def parser_contract_server_rows(
    project_root: Path,
    manifest: dict[str, Any],
    source_by_kind: dict[str, dict[str, str]],
) -> list[dict[str, str]]:
    repo_root = project_root.parent
    admission_source = source_by_kind["server_sblr_admission_surface"]
    dispatch_source = source_by_kind["server_sblr_dispatch_surface"]
    admission_text = (repo_root / admission_source["path"]).read_text(encoding="utf-8")
    dispatch_text = (repo_root / dispatch_source["path"]).read_text(encoding="utf-8")
    for token in ("SB_SERVER_SBLR_ADMISSION_VALIDATOR", "kServerSblrFamilies", "cluster_private"):
        if token not in admission_text:
            fail(f"parser_contract_admission_token_missing:{token}")
    for token in ("SB_SERVER_SBLR_DISPATCH_RESULTS", "DispatchThroughPublicAbi", "IsClusterDispatchOperation"):
        if token not in dispatch_text:
            fail(f"parser_contract_dispatch_token_missing:{token}")
    return [
        parser_contract_row(
            manifest,
            1,
            "server_admission_rules",
            "server_sblr_admission_validator",
            admission_source["resource_kind"],
            "revalidates_parser_sblr_claims_and_fences_cluster_private_families",
            "SBLR.OPERATION_UNSUPPORTED;SBLR.FAMILY_INVALID;PROCESS.CLUSTER_PATH_ABSENT",
            "server_admission_fail_closed_before_engine_dispatch",
            admission_source["sha256"],
            admission_source["path"],
        ),
        parser_contract_row(
            manifest,
            1,
            "server_dispatch_routes",
            "server_sblr_dispatch_public_abi",
            dispatch_source["resource_kind"],
            "dispatches_admitted_sblr_through_public_abi_or_compile_gated_cluster_provider",
            "SBLR.OPERATION_UNSUPPORTED;SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
            "server_dispatch_uses_public_abi_and_cluster_provider_boundary",
            dispatch_source["sha256"],
            dispatch_source["path"],
        ),
    ]


def parser_contract_reference_route_rows(
    project_root: Path,
    manifest: dict[str, Any],
    source_by_kind: dict[str, dict[str, str]],
) -> list[dict[str, str]]:
    source = source_by_kind["reference_route_classification_matrix"]
    csv_rows = read_traceability_csv(
        project_root / SBLR_TRACEABILITY_FIXTURE_ROOT / "SERVER_AUTHORITY_SURFACE_MATRIX.csv"
    )
    rows: list[dict[str, str]] = []
    for index, csv_row in enumerate(csv_rows, start=1):
        decision_id = selector_value(csv_row, "decision_id", "SERVER_AUTHORITY_SURFACE_MATRIX.csv", index)
        published_value = json.dumps(
            {
                "engine_id": csv_row.get("engine_id", ""),
                "surface_key": csv_row.get("surface_key", ""),
                "parser_action": csv_row.get("parser_action", ""),
                "server_action": csv_row.get("server_action", ""),
                "sblr_action": csv_row.get("sblr_action", ""),
                "is_sblr_execution_surface": csv_row.get("is_sblr_execution_surface", ""),
            },
            sort_keys=True,
            separators=(",", ":"),
        )
        rows.append(
            parser_contract_row(
                manifest,
                index,
                "reference_route_classifications",
                decision_id,
                source["resource_kind"],
                published_value,
                csv_row.get("security_vector", "SB.SECURITY.DATABASE_AUTHORITY_DENIED"),
                csv_row.get("required_execution_plan_lane", "server_route_classification"),
                source["sha256"],
                source["path"],
            )
        )
    if len(rows) < 318:
        fail(f"parser_contract_reference_route_rows_too_small:{len(rows)}")
    return rows


def parser_contract_unsupported_rows(
    project_root: Path,
    manifest: dict[str, Any],
    source_by_kind: dict[str, dict[str, str]],
) -> list[dict[str, str]]:
    source = source_by_kind["unsupported_surface_vector_matrix"]
    csv_rows = read_traceability_csv(
        project_root / SBLR_TRACEABILITY_FIXTURE_ROOT / "EXPLICIT_UNSUPPORTED_SURFACE_MATRIX.csv"
    )
    rows: list[dict[str, str]] = []
    for index, csv_row in enumerate(csv_rows, start=1):
        inventory_id = selector_value(csv_row, "inventory_id", "EXPLICIT_UNSUPPORTED_SURFACE_MATRIX.csv", index)
        published_value = json.dumps(
            {
                "engine_id": csv_row.get("engine_id", ""),
                "item_name": csv_row.get("item_name", ""),
                "item_kind": csv_row.get("item_kind", ""),
                "sb_normalized_target": csv_row.get("sb_normalized_target", ""),
                "implementation_decision": csv_row.get("implementation_decision", ""),
            },
            sort_keys=True,
            separators=(",", ":"),
        )
        rows.append(
            parser_contract_row(
                manifest,
                index,
                "unsupported_surface_vectors",
                inventory_id,
                source["resource_kind"],
                published_value,
                csv_row.get("required_acceptance_gate", "exact_unsupported_refusal"),
                csv_row.get("required_execution_plan_lane", "exact_unsupported_refusal"),
                source["sha256"],
                source["path"],
            )
        )
    if len(rows) < 5:
        fail(f"parser_contract_unsupported_rows_too_small:{len(rows)}")
    return rows


def parser_contract_sbsql_sync_rows(
    project_root: Path,
    build_root: Path,
    manifest: dict[str, Any],
    source_by_kind: dict[str, dict[str, str]],
) -> list[dict[str, str]]:
    source = source_by_kind["parser_facing_sbsql_sync_requirements"]
    sync_evidence = validate_sbsql_parser_spec_execution_plan_sync(project_root, build_root)
    sync_rows = sync_evidence.get("rows", [])
    if not isinstance(sync_rows, list) or len(sync_rows) < 724:
        fail("parser_contract_sbsql_sync_rows_too_small")
    rows: list[dict[str, str]] = []
    for index, sync_row in enumerate(sync_rows, start=1):
        if not isinstance(sync_row, dict):
            fail(f"parser_contract_sbsql_sync_row_shape:{index}")
        source_id = str(sync_row.get("trace_id", f"sbsql-sync-{index}"))
        published_value = json.dumps(
            {
                "trace_kind": sync_row.get("trace_kind", ""),
                "coverage_id": sync_row.get("coverage_id", ""),
                "change_kind": sync_row.get("change_kind", ""),
                "sbsql_style": sync_row.get("sbsql_style", ""),
                "sync_status": sync_row.get("sync_status", ""),
            },
            sort_keys=True,
            separators=(",", ":"),
        )
        rows.append(
            parser_contract_row(
                manifest,
                index,
                "sbsql_sync_status",
                source_id,
                source["resource_kind"],
                published_value,
                "SBSQL.SBLR_SYNC_REQUIRED",
                str(sync_row.get("proof_class", "sbsql_sync_status")),
                source["sha256"],
                str(sync_row.get("evidence_artifact", source["path"])),
            )
        )
    return rows


def parser_contract_compatibility_rows(
    manifest: dict[str, Any],
    source_by_kind: dict[str, dict[str, str]],
) -> list[dict[str, str]]:
    source = source_by_kind["parser_facing_contract_manifest"]
    return [
        parser_contract_row(
            manifest,
            1,
            "compatibility_version",
            str(manifest["compatibility_version"]),
            source["resource_kind"],
            str(manifest["freeze_version"]),
            "SBLR.CONTRACT_VERSION_REQUIRED",
            "parser_worker_must_match_or_negotiate_declared_contract_version",
            source["sha256"],
            source["path"],
        )
    ]


def validate_parser_facing_contract_freeze(project_root: Path,
                                           build_root: Path) -> dict[str, Any]:
    manifest, source_by_kind, section_minimums = (
        validate_parser_contract_freeze_manifest_shape(project_root)
    )
    rows: list[dict[str, str]] = []
    rows.extend(parser_contract_envelope_rows(project_root, manifest, source_by_kind))
    rows.extend(parser_contract_opcode_rows(project_root, manifest, source_by_kind))
    rows.extend(parser_contract_server_rows(project_root, manifest, source_by_kind))
    rows.extend(parser_contract_reference_route_rows(project_root, manifest, source_by_kind))
    rows.extend(parser_contract_unsupported_rows(project_root, manifest, source_by_kind))
    rows.extend(parser_contract_sbsql_sync_rows(project_root, build_root, manifest, source_by_kind))
    rows.extend(parser_contract_compatibility_rows(manifest, source_by_kind))

    seen_ids: set[str] = set()
    section_counts = {section: 0 for section in PARSER_CONTRACT_REQUIRED_SECTIONS}
    for row in rows:
        if tuple(row.keys()) != PARSER_CONTRACT_ROW_FIELDS:
            fail(f"parser_contract_row_field_set:{row.get('contract_row_id', '<unknown>')}")
        contract_row_id = row["contract_row_id"]
        if contract_row_id in seen_ids:
            fail(f"parser_contract_duplicate_row_id:{contract_row_id}")
        seen_ids.add(contract_row_id)
        section = row["section"]
        if section not in section_counts:
            fail(f"parser_contract_unknown_row_section:{contract_row_id}:{section}")
        section_counts[section] += 1
        if row["public_release_safe"] != "true":
            fail(f"parser_contract_public_safe_false:{contract_row_id}")
        if row["product_completion_claim"] != "false":
            fail(f"parser_contract_product_completion_claim:{contract_row_id}")
        if row["freeze_status"] != "frozen_parser_contract_current":
            fail(f"parser_contract_freeze_status:{contract_row_id}:{row['freeze_status']}")
        for field, value in row.items():
            reject_private_reference(str(value), f"{contract_row_id}:{field}")
    for section, minimum_rows in section_minimums.items():
        if section_counts.get(section, 0) < minimum_rows:
            fail(
                "parser_contract_section_count_too_small:"
                f"{section}:{section_counts.get(section, 0)}:{minimum_rows}"
            )
    if len(rows) < EXPECTED_PARSER_CONTRACT_MINIMUM_ROWS:
        fail(f"parser_contract_freeze_row_count_too_small:{len(rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-085",
        "private_docs_required": False,
        "git_history_required": False,
        "docs_execution-plans_required_at_runtime": False,
        "matrix_id": "ELER_085_PARSER_FACING_CONTRACT_FREEZE_PACKAGE",
        "freeze_id": str(manifest["freeze_id"]),
        "freeze_version": str(manifest["freeze_version"]),
        "compatibility_version": str(manifest["compatibility_version"]),
        "section_counts": section_counts,
        "source_resource_count": len(source_by_kind),
        "row_count": len(rows),
        "build_root_recorded": False,
        "rows": rows,
    }


def validate_sblr_fixture_hash_manifest(project_root: Path) -> list[dict[str, str]]:
    fixture_root = project_root / SBLR_TRACEABILITY_FIXTURE_ROOT
    manifest = load_json_object(fixture_root / "sblr_surface_fixture_hashes.json")
    rows: list[dict[str, str]] = []
    csv_hashes = manifest.get("files", {})
    json_hashes = manifest.get("json_files", {})
    if not isinstance(csv_hashes, dict) or not isinstance(json_hashes, dict):
        fail("determinism_fixture_hash_manifest_shape")
    expected_csvs = set(SBLR_TRACEABILITY_CSV_FILES)
    if set(csv_hashes) != expected_csvs:
        fail(
            "determinism_fixture_csv_hash_set:"
            f"expected={sorted(expected_csvs)} actual={sorted(csv_hashes)}"
        )
    expected_jsons = set(SBLR_TRACEABILITY_JSON_FILES)
    if set(json_hashes) != expected_jsons:
        fail(
            "determinism_fixture_json_hash_set:"
            f"expected={sorted(expected_jsons)} actual={sorted(json_hashes)}"
        )
    for filename, expected in sorted(csv_hashes.items()):
        digest = hashlib.sha256((fixture_root / filename).read_bytes()).hexdigest()
        if digest != expected:
            fail(f"determinism_fixture_csv_hash_drift:{filename}")
        rows.append(
            {
                "resource_id": f"sblr_fixture_csv:{filename}",
                "resource_kind": "checked_in_fixture_csv",
                "determinism_policy": "checked_in_sha256_manifest_match",
                "first_sha256": digest,
                "second_sha256": digest,
                "status": "deterministic",
                "evidence": (
                    "project/tests/sblr_surface/fixtures/"
                    f"reference_sblr_interface_gap_2026_06_03/{filename}"
                ),
            }
        )
    for filename, expected in sorted(json_hashes.items()):
        digest = hashlib.sha256((fixture_root / filename).read_bytes()).hexdigest()
        if digest != expected:
            fail(f"determinism_fixture_json_hash_drift:{filename}")
        rows.append(
            {
                "resource_id": f"sblr_fixture_json:{filename}",
                "resource_kind": "checked_in_fixture_json",
                "determinism_policy": "checked_in_sha256_manifest_match",
                "first_sha256": digest,
                "second_sha256": digest,
                "status": "deterministic",
                "evidence": (
                    "project/tests/sblr_surface/fixtures/"
                    f"reference_sblr_interface_gap_2026_06_03/{filename}"
                ),
            }
        )
    return rows


def validate_checked_in_resource_determinism_manifest(project_root: Path) -> list[dict[str, str]]:
    repo_root = project_root.parent
    manifest_path = project_root / GENERATED_RESOURCE_DETERMINISM_MANIFEST
    manifest = load_json_object(manifest_path)
    if manifest.get("schema_id") != "scratchbird.engine_listener.generated_resource_determinism.v1":
        fail("determinism_resource_manifest_schema")
    resources = manifest.get("resources", [])
    if not isinstance(resources, list) or not resources:
        fail("determinism_resource_manifest_resources")
    rows: list[dict[str, str]] = []
    seen_paths: set[str] = set()
    for index, resource in enumerate(resources, start=1):
        if not isinstance(resource, dict):
            fail(f"determinism_resource_manifest_row_not_object:{index}")
        path = str(resource.get("path", ""))
        resource_kind = str(resource.get("resource_kind", ""))
        expected = str(resource.get("sha256", ""))
        if not path or not resource_kind or not expected:
            fail(f"determinism_resource_manifest_row_missing_field:{index}")
        if path in seen_paths:
            fail(f"determinism_resource_manifest_duplicate_path:{path}")
        seen_paths.add(path)
        reject_private_reference(path, f"determinism_resource:{path}")
        if Path(path).is_absolute() or ".." in Path(path).parts:
            fail(f"determinism_resource_path_not_repo_relative:{path}")
        candidate = repo_root / path
        if not candidate.exists() or not candidate.is_file():
            fail(f"determinism_resource_missing:{path}")
        digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
        if digest != expected:
            fail(f"determinism_resource_hash_drift:{path}")
        rows.append(
            {
                "resource_id": f"checked_in_resource:{path}",
                "resource_kind": resource_kind,
                "determinism_policy": "project_tests_checked_in_sha256_manifest_match",
                "first_sha256": digest,
                "second_sha256": digest,
                "status": "deterministic",
                "evidence": path,
            }
        )
    if len(rows) < 8:
        fail(f"determinism_resource_manifest_too_small:{len(rows)}")
    return rows


def deterministic_generated_rows(project_root: Path,
                                 build_root: Path) -> list[dict[str, str]]:
    generators = (
        (
            "declared_feature_inventory",
            "generated_project_test_inventory",
            lambda: declared_feature_rows(project_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#declared_feature_rows",
        ),
        (
            "platform_matrix",
            "generated_project_test_matrix",
            lambda: validate_platform_matrix(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_platform_matrix",
        ),
        (
            "native_platform_proof_package_import",
            "generated_project_test_matrix",
            lambda: validate_native_platform_proof_package_import(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_native_platform_proof_package_import",
        ),
        (
            "no_skip_waiver_xfail_release_proof",
            "generated_project_test_matrix",
            lambda: validate_no_skip_waiver_xfail_release_proof(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_no_skip_waiver_xfail_release_proof",
        ),
        (
            "row_level_traceability_manifest",
            "generated_project_test_manifest",
            lambda: validate_row_level_traceability_manifest(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_row_level_traceability_manifest",
        ),
        (
            "cross_cutting_crash_fault_campaign",
            "generated_project_test_matrix",
            lambda: validate_crash_fault_campaign(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_crash_fault_campaign",
        ),
        (
            "integrated_full_stack_product_proof",
            "generated_project_test_matrix",
            lambda: validate_integrated_product_proof(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_integrated_product_proof",
        ),
        (
            "crash_recovery_certification_matrix",
            "generated_project_test_matrix",
            lambda: validate_crash_recovery_certification(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_crash_recovery_certification",
        ),
        (
            "adversarial_security_validation_matrix",
            "generated_project_test_matrix",
            lambda: validate_adversarial_security_validation(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_adversarial_security_validation",
        ),
        (
            "fuzz_property_invariant_suite",
            "generated_project_test_matrix",
            lambda: validate_fuzz_property_invariant_suite(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_fuzz_property_invariant_suite",
        ),
        (
            "performance_scalability_baseline",
            "generated_project_test_matrix",
            lambda: validate_performance_scalability_baseline(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_performance_scalability_baseline",
        ),
        (
            "soak_certification",
            "generated_project_test_matrix",
            lambda: validate_soak_certification(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_soak_certification",
        ),
        (
            "compatibility_upgrade_downgrade",
            "generated_project_test_matrix",
            lambda: validate_compatibility_upgrade_downgrade(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_compatibility_upgrade_downgrade",
        ),
        (
            "operational_readiness_diagnosability",
            "generated_project_test_matrix",
            lambda: validate_operational_readiness(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_operational_readiness",
        ),
        (
            "release_artifact_trust",
            "generated_project_test_matrix",
            lambda: validate_release_artifact_trust(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_release_artifact_trust",
        ),
        (
            "enterprise_documentation_runbooks",
            "generated_project_test_matrix",
            lambda: validate_enterprise_documentation(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_enterprise_documentation",
        ),
        (
            "support_maintenance_policy",
            "generated_project_test_matrix",
            lambda: validate_support_maintenance_policy(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_support_maintenance_policy",
        ),
        (
            "final_project_only_implementation_proof",
            "generated_project_test_matrix",
            lambda: validate_project_only_implementation_proof(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_project_only_implementation_proof",
        ),
        (
            "external_review_closure_package",
            "generated_project_test_matrix",
            lambda: validate_external_review_closure_package(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_external_review_closure_package",
        ),
        (
            "gold_enterprise_readiness_aggregation",
            "generated_project_test_matrix",
            lambda: validate_gold_enterprise_readiness(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_gold_enterprise_readiness",
        ),
        (
            "third_audit_evidence_package",
            "generated_project_test_matrix",
            lambda: validate_third_audit_evidence_package(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_third_audit_evidence_package",
        ),
        (
            "parser_facing_contract_freeze",
            "generated_project_test_manifest",
            lambda: validate_parser_facing_contract_freeze(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_parser_facing_contract_freeze",
        ),
        (
            "sbsql_parser_spec_execution_plan_sync",
            "generated_project_test_manifest",
            lambda: validate_sbsql_parser_spec_execution_plan_sync(project_root, build_root),
            "project/tests/engine_listener_enterprise/engine_listener_enterprise_gate.py#validate_sbsql_parser_spec_execution_plan_sync",
        ),
    )
    rows: list[dict[str, str]] = []
    for resource_id, resource_kind, generator, evidence in generators:
        first = generator()
        second = generator()
        first_hash = canonical_json_digest(first)
        second_hash = canonical_json_digest(second)
        if first_hash != second_hash:
            fail(f"generated_resource_nondeterministic:{resource_id}")
        rows.append(
            {
                "resource_id": resource_id,
                "resource_kind": resource_kind,
                "determinism_policy": "two_pass_canonical_json_sha256_match",
                "first_sha256": first_hash,
                "second_sha256": second_hash,
                "status": "deterministic",
                "evidence": evidence,
            }
        )
    return rows


def validate_generated_resource_determinism(project_root: Path,
                                            build_root: Path) -> dict[str, Any]:
    rows = validate_sblr_fixture_hash_manifest(project_root)
    rows.extend(validate_checked_in_resource_determinism_manifest(project_root))
    rows.extend(deterministic_generated_rows(project_root, build_root))
    for row in rows:
        reject_private_reference(row["evidence"], row["resource_id"])
        if row["status"] != "deterministic":
            fail(f"generated_resource_not_deterministic:{row['resource_id']}")
        if row["first_sha256"] != row["second_sha256"]:
            fail(f"generated_resource_hash_mismatch:{row['resource_id']}")
    expected_rows = (
        len(SBLR_TRACEABILITY_CSV_FILES)
        + len(SBLR_TRACEABILITY_JSON_FILES)
        + len(validate_checked_in_resource_determinism_manifest(project_root))
        + 23
    )
    if len(rows) != expected_rows:
        fail(f"generated_resource_determinism_row_count:{len(rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-086",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_086_GENERATED_RESOURCE_DETERMINISM_MATRIX",
        "resource_count": len(rows),
        "rows": rows,
    }


def validate_project_tests_proof_location(project_root: Path) -> dict[str, Any]:
    tests_text = ctest_text(project_root)
    tests = parse_tests(tests_text)
    checked: list[dict[str, Any]] = []
    relevant_labels = (
        "engine_listener_enterprise",
        "release_proof_substrate",
        "public_release_correctness",
        "sblr_surface",
    )
    for test in tests:
        if not any(label in test["labels"] for label in relevant_labels):
            continue
        command = test["command"]
        for forbidden in FORBIDDEN_PROOF_REFERENCE_FRAGMENTS:
            if forbidden in command:
                fail(f"private_proof_dependency_in_ctest:{test['name']}:{forbidden}")
        checked.append(
            {
                "test_name": test["name"],
                "labels": test["labels"],
                "command_sha256": sha256_text(command),
                "project_tests_local": True,
            }
        )
    names = {row["test_name"] for row in checked}
    required_markers = {
        "engine_listener_prior_execution_plan_semantics_gate",
        "engine_listener_declared_feature_inventory_gate",
        "engine_listener_anti_skeleton_classifier_gate",
        "engine_listener_project_tests_proof_location_gate",
        "sblr_surface_reference_interface_closure_gate",
        "public_cluster_build_matrix_gate",
    }
    missing = sorted(
        marker for marker in required_markers
        if marker not in names and marker not in tests_text
    )
    if missing:
        fail("required_project_test_gate_missing:" + ",".join(missing))
    for marker in sorted(required_markers):
        if marker not in names:
            checked.append(
                {
                    "test_name": marker,
                    "labels": "registered_by_project_tests_cmake_function",
                    "command_sha256": sha256_text(marker),
                    "project_tests_local": True,
                }
            )
    return {
        "schema_version": 1,
        "gate": "ELER-004",
        "private_docs_required": False,
        "git_history_required": False,
        "checked_ctest_registrations": checked,
    }


def validate_platform_matrix(project_root: Path, build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    cmake_text = ctest_text(project_root)
    cmake_tests = {test["name"]: test for test in parse_tests(cmake_text)}
    cmake_targets = parse_add_executables(cmake_text)
    non_native_source_proof_allowed = sys.platform.startswith("win")
    if len(tests_by_name) < 100:
        fail(f"platform_matrix_ctest_inventory_too_small:{len(tests_by_name)}")

    required_public_tokens = (
        "REQUIRED_PLATFORMS",
        '"linux"',
        '"windows"',
        '"freebsd"',
        "native_runner_required",
        "no_cross_platform_support_claim_from_current_host",
        "SB_DIAG_PLATFORM_NATIVE_PROOF_REQUIRED",
        "cluster_stub_scope",
        "windows_x64_only_no_win32_release_target",
        "configured_windows_x86_hard_fail",
        "configured_windows_x86_not_supported",
        "negative_platform_proofs",
        "Win32 is not a supported release target",
    )
    public_tool = read_repo_relative_text(
        repo_root,
        "project/tools/release/public_platform_matrix_gate.py",
        "platform_matrix_public_tool",
    )
    for token in required_public_tokens:
        require_token(public_tool, token, "platform_matrix_public_tool")

    matrix_text = read_repo_relative_text(
        repo_root,
        "project/cmake/SUPPORTED_PLATFORM_TOOLCHAIN_MATRIX.md",
        "platform_matrix_policy",
    )
    for token in (
        "linux-x86_64-glibc-core",
        "windows-x86_64-msvc-core",
        "freebsd-x86_64-elf-core",
        "native_runner_required",
        "unsupported_platform_fail_closed_before_configure_or_release_claim",
        "win32-first-release-out-of-scope",
        "windows_x64_only_no_win32_release_target",
        "cluster_stub_boundary",
        "routes_to_public_stub_provider_boundary_only",
        "no_execution_plan_ctest_dependency",
    ):
        require_token(matrix_text, token, "platform_matrix_policy")

    release_readme = read_repo_relative_text(repo_root, "release/README.md",
                                             "platform_matrix_release_readme")
    for token in (
        "Linux is the first validated execution platform",
        "Windows x64 and FreeBSD layouts are",
        "Win32 is out of scope",
        "declared compatibility targets",
        "cross-platform proof pending",
        "external-provider-only",
        "public cluster stubs are compile/link boundaries",
    ):
        require_token(release_readme, token, "platform_matrix_release_readme")

    required_gate_sets: tuple[dict[str, Any], ...] = (
        {
            "platform": "linux",
            "native_status": "native_host_linux_ctest_execution_required",
            "required_gates": (
                "public_platform_matrix_gate",
                "engine_listener_storage_io_conformance",
                "engine_listener_storage_io_platform_source_gate",
                "engine_listener_management_envelope_conformance",
                "engine_listener_ipv6_dual_stack_transport_conformance",
                "engine_listener_parser_pool_lifecycle_conformance",
                "engine_listener_tls_channel_binding_conformance",
                "engine_listener_owner_lifecycle_artifact_conformance",
                "public_crypto_entropy_policy_gate",
                "public_install_service_hardening_gate",
                "engine_listener_no_skip_waiver_xfail_release_gate",
                "public_no_skip_waiver_xfail_gate",
            ),
            "evidence_files": (
                "release/linux/ENGINE_BINARY_LAYOUT.json",
                "docs/build_requirements/linux/README.md",
                "project/tests/engine_listener_enterprise/engine_listener_storage_io_conformance.cpp",
                "project/tests/engine_listener_enterprise/engine_listener_storage_io_platform_source_gate.py",
                "project/tests/engine_listener_enterprise/engine_listener_ipv6_dual_stack_transport_conformance.cpp",
            ),
        },
        {
            "platform": "windows",
            "native_status": "native_windows_x64_runner_required",
            "required_gates": (
                "public_platform_matrix_gate",
                "engine_listener_storage_io_platform_source_gate",
                "engine_listener_windows_transport_source_gate",
                "engine_listener_owner_lifecycle_platform_source_gate",
            ),
            "evidence_files": (
                "release/windows/ENGINE_BINARY_LAYOUT.json",
                "docs/build_requirements/windows/README.md",
                "project/tests/engine_listener_enterprise/engine_listener_storage_io_platform_source_gate.py",
                "project/tests/engine_listener_enterprise/engine_listener_windows_transport_source_gate.py",
                "project/tests/engine_listener_enterprise/engine_listener_owner_lifecycle_platform_source_gate.py",
            ),
        },
        {
            "platform": "freebsd",
            "native_status": "native_freebsd_runner_required",
            "required_gates": (
                "public_platform_matrix_gate",
                "engine_listener_storage_io_platform_source_gate",
                "engine_listener_bsd_peer_owner_management_gate",
                "engine_listener_owner_lifecycle_platform_source_gate",
            ),
            "evidence_files": (
                "release/freebsd/ENGINE_BINARY_LAYOUT.json",
                "docs/build_requirements/freebsd/README.md",
                "project/tests/engine_listener_enterprise/engine_listener_storage_io_platform_source_gate.py",
                "project/tests/engine_listener_enterprise/engine_listener_bsd_peer_owner_management_gate.py",
                "project/tests/engine_listener_enterprise/engine_listener_owner_lifecycle_platform_source_gate.py",
            ),
        },
    )

    rows: list[dict[str, str]] = []
    errors: list[str] = []
    for spec in required_gate_sets:
        platform = str(spec["platform"])
        gate_names = tuple(str(gate) for gate in spec["required_gates"])
        label_hashes: list[str] = []
        for gate_name in gate_names:
            test = tests_by_name.get(gate_name)
            if not test:
                if non_native_source_proof_allowed and gate_name in cmake_tests:
                    source_path, source_text = source_for_test(
                        cmake_tests[gate_name],
                        cmake_targets,
                        project_root,
                    )
                    proof_class = proof_class_for_source(
                        test_command_text(cmake_tests[gate_name]),
                        source_text,
                    )
                    if proof_class == "file_or_registration_only" or not source_path:
                        errors.append(f"platform_gate_source_proof_insufficient:{platform}:{gate_name}")
                        continue
                    labels = sorted(
                        label
                        for label in str(cmake_tests[gate_name].get("labels", "")).split(";")
                        if label
                    )
                    label_hashes.append(sha256_text(";".join(labels)))
                    continue
                errors.append(f"platform_gate_not_configured:{platform}:{gate_name}")
                continue
            test_errors = validate_test_has_no_completion_exception(test)
            errors.extend(
                f"platform_gate_completion_exception:{platform}:{error}"
                for error in test_errors
            )
            labels = sorted(ctest_labels(test))
            if gate_name == "public_platform_matrix_gate":
                for label in ("ELER-005", "ELER-GATE-004"):
                    if label not in labels:
                        errors.append(f"platform_public_gate_missing_label:{label}")
            label_hashes.append(sha256_text(";".join(labels)))

        evidence_hashes: list[str] = []
        for relative in spec["evidence_files"]:
            text = read_repo_relative_text(repo_root, str(relative),
                                           f"platform_matrix:{platform}")
            evidence_hashes.append(sha256_text(text))
        rows.append(
            {
                "platform": platform,
                "native_status": str(spec["native_status"]),
                "required_gate_count": str(len(gate_names)),
                "required_gates": ";".join(gate_names),
                "gate_label_sha256": sha256_text("|".join(label_hashes)),
                "evidence_file_count": str(len(spec["evidence_files"])),
                "evidence_sha256": sha256_text("|".join(evidence_hashes)),
                "skip_waiver_xfail_policy": "forbidden_for_declared_engine_listener_proof",
                "product_completion_claim": "false",
            }
        )

    if errors:
        fail("platform_matrix:" + ";".join(errors[:12]))
    if len(rows) != 3:
        fail(f"platform_matrix_row_count:{len(rows)}")
    return {
        "schema_version": 1,
        "gate": "ELER-005",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_005_LINUX_WINDOWS_X64_FREEBSD_PLATFORM_MATRIX",
        "host_execution_scope": "linux_first_native_execution",
        "nonhost_execution_scope": "windows_x64_freebsd_native_runner_required",
        "product_completion_claim": False,
        "row_count": len(rows),
        "rows": rows,
    }


NATIVE_PLATFORM_HANDOFF_LABELS: tuple[str, ...] = (
    "ELER-005",
    "ELER-080",
    "ELER-083",
    "ELER-084",
    "ELER-085",
    "ELER-086",
    "ELER-087",
    "ELER-088",
    "ELER-089",
    "ELER-090",
    "ELER-100",
    "ELER-101",
    "ELER-102",
    "ELER-103",
    "ELER-104",
    "ELER-105",
    "ELER-106",
    "ELER-107",
    "ELER-108",
    "ELER-109",
    "ELER-110",
    "ELER-111",
    "ELER-112",
)

NATIVE_PLATFORM_RETURN_ARTIFACTS: tuple[str, ...] = (
    "native-platform-eler.xml",
    "native-public-release.xml",
    "native-engine-listener-enterprise.xml",
    "native-sblr-surface.xml",
    "native-cluster-boundary.xml",
    "CMakeCache.txt",
    "Testing/Temporary/LastTest.log",
    "tests/engine_listener_enterprise/engine_listener_native_platform_handoff_gate.json",
    "tests/engine_listener_enterprise/engine_listener_native_platform_handoff_gate.csv",
    "tests/engine_listener_enterprise/engine_listener_gold_enterprise_readiness_gate.json",
    "tests/engine_listener_enterprise/engine_listener_third_audit_evidence_package_gate.json",
)

NATIVE_PLATFORM_JUNIT_ARTIFACTS: tuple[str, ...] = (
    "native-platform-eler.xml",
    "native-public-release.xml",
    "native-engine-listener-enterprise.xml",
    "native-sblr-surface.xml",
    "native-cluster-boundary.xml",
)

NATIVE_PLATFORM_GENERATED_JSON_ARTIFACTS: tuple[str, ...] = (
    "tests/engine_listener_enterprise/engine_listener_native_platform_handoff_gate.json",
    "tests/engine_listener_enterprise/engine_listener_gold_enterprise_readiness_gate.json",
    "tests/engine_listener_enterprise/engine_listener_third_audit_evidence_package_gate.json",
)

NATIVE_PLATFORM_GENERATED_CSV_ARTIFACTS: tuple[str, ...] = (
    "tests/engine_listener_enterprise/engine_listener_native_platform_handoff_gate.csv",
)

NATIVE_PLATFORM_PACKAGE_FORBIDDEN_MARKERS: tuple[str, ...] = (
    "xfail",
    "waiver",
    "waived",
    "not-run",
    "not_run",
    "notrun",
    "platform-excluded",
    "platform_excluded",
    "unsupported-profile",
    "unsupported_profile",
)

NATIVE_PLATFORM_PACKAGE_TARGET_CACHE: dict[str, tuple[str, str]] = {
    "windows_x64": ("Windows", "AMD64"),
    "freebsd_x86_64": ("FreeBSD", "x86_64"),
}

NATIVE_PLATFORM_HANDOFF_ROWS: tuple[dict[str, Any], ...] = (
    {
        "platform": "windows",
        "target": "windows_x64",
        "native_status": "native_windows_x64_runner_required",
        "source_gates": (
            "public_platform_matrix_gate",
            "engine_listener_storage_io_platform_source_gate",
            "engine_listener_windows_transport_source_gate",
            "engine_listener_owner_lifecycle_platform_source_gate",
        ),
        "build_requirements": "docs/build_requirements/windows/README.md",
        "release_layout": "release/windows/ENGINE_BINARY_LAYOUT.json",
        "support_policy_token": "target platform pending native CI/runtime proof",
    },
    {
        "platform": "freebsd",
        "target": "freebsd_x86_64",
        "native_status": "native_freebsd_runner_required",
        "source_gates": (
            "public_platform_matrix_gate",
            "engine_listener_storage_io_platform_source_gate",
            "engine_listener_bsd_peer_owner_management_gate",
            "engine_listener_owner_lifecycle_platform_source_gate",
        ),
        "build_requirements": "docs/build_requirements/freebsd/README.md",
        "release_layout": "release/freebsd/ENGINE_BINARY_LAYOUT.json",
        "support_policy_token": "target platform pending native runner proof",
    },
)


def validate_native_platform_handoff(project_root: Path,
                                     build_root: Path) -> dict[str, Any]:
    repo_root = project_root.parent
    platform_matrix = validate_platform_matrix(project_root, build_root)
    platform_rows = {
        str(row.get("platform", "")): row
        for row in platform_matrix.get("rows", [])
        if isinstance(row, dict)
    }
    tests_by_name = {
        str(test.get("name", "")): test
        for test in ctest_json_tests(build_root)
        if isinstance(test, dict)
    }
    labels_to_tests: dict[str, list[dict[str, Any]]] = {}
    for test in tests_by_name.values():
        for label in ctest_labels(test):
            labels_to_tests.setdefault(label, []).append(test)

    errors: list[str] = []
    for label in NATIVE_PLATFORM_HANDOFF_LABELS:
        label_tests = labels_to_tests.get(label, [])
        if not label_tests:
            errors.append(f"native_handoff_label_missing:{label}")
            continue
        for test in label_tests:
            errors.extend(
                f"native_handoff_completion_exception:{label}:{error}"
                for error in validate_test_has_no_completion_exception(test)
            )

    lifecycle_text = read_repo_relative_text(
        repo_root,
        "project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md",
        "native_handoff_lifecycle",
    )
    build_root_text = read_repo_relative_text(
        repo_root,
        "docs/build_requirements/README.md",
        "native_handoff_build_requirements_root",
    )
    for token in (
        "supported first proof lane",
        "target platform pending native CI/runtime proof",
        "target platform pending native runner proof",
        "before support is claimed",
    ):
        require_token(lifecycle_text, token, "native_handoff_lifecycle")
    for token in (
        "Fully proven first target",
        "Target platform pending native CI/runtime proof",
        "Target platform pending native runner proof",
        "All support-eligible platforms must provide before support is claimed",
        "Every support-eligible platform must prove before support is claimed",
    ):
        require_token(build_root_text, token, "native_handoff_build_requirements_root")

    rows: list[dict[str, str]] = []
    for spec in NATIVE_PLATFORM_HANDOFF_ROWS:
        platform = str(spec["platform"])
        matrix_row_for_platform = platform_rows.get(platform)
        if not matrix_row_for_platform:
            errors.append(f"native_handoff_platform_matrix_missing:{platform}")
            continue
        matrix_status = str(matrix_row_for_platform.get("native_status", ""))
        expected_status = str(spec["native_status"])
        if matrix_status != expected_status:
            errors.append(
                f"native_handoff_status:{platform}:expected={expected_status}:actual={matrix_status}"
            )
        source_gate_names = tuple(str(gate) for gate in spec["source_gates"])
        for gate_name in source_gate_names:
            test = tests_by_name.get(gate_name)
            if not test:
                errors.append(f"native_handoff_source_gate_missing:{platform}:{gate_name}")
                continue
            errors.extend(
                f"native_handoff_source_gate_completion_exception:{platform}:{error}"
                for error in validate_test_has_no_completion_exception(test)
            )

        build_requirements = str(spec["build_requirements"])
        release_layout = str(spec["release_layout"])
        build_text = read_repo_relative_text(repo_root, build_requirements,
                                             f"native_handoff_build:{platform}")
        layout_text = read_repo_relative_text(repo_root, release_layout,
                                              f"native_handoff_layout:{platform}")
        require_token(build_text, "ctest --test-dir", f"native_handoff_build:{platform}")
        require_token(build_text, "SB_ENABLE_CLUSTER_PROVIDER=OFF",
                      f"native_handoff_build:{platform}")
        require_token(build_text, "SB_CLUSTER_PROVIDER_STUB=ON",
                      f"native_handoff_build:{platform}")
        require_token(build_text, "Native Platform Handoff Proof Package",
                      f"native_handoff_artifact_contract:{platform}")
        require_token(build_text, "support_claim_before_native_runner=false",
                      f"native_handoff_artifact_contract:{platform}")
        require_token(build_text, "product_completion_claim=false",
                      f"native_handoff_artifact_contract:{platform}")
        require_token(build_text, "external_audit_completion_claim=false",
                      f"native_handoff_artifact_contract:{platform}")
        for artifact in NATIVE_PLATFORM_RETURN_ARTIFACTS:
            require_token(build_text, artifact,
                          f"native_handoff_return_artifact:{platform}")
        require_token(lifecycle_text, str(spec["support_policy_token"]),
                      f"native_handoff_lifecycle:{platform}")
        required_label_expr = "|".join(NATIVE_PLATFORM_HANDOFF_LABELS)
        rows.append(
            {
                "platform": platform,
                "target": str(spec["target"]),
                "handoff_status": "native_runner_required_before_support_claim",
                "platform_matrix_status": matrix_status,
                "source_gate_count": str(len(source_gate_names)),
                "source_gates": ";".join(source_gate_names),
                "required_native_ctest_label_expression": required_label_expr,
                "required_native_ctest_command": (
                    f'ctest --test-dir <native-release-build> -L "{required_label_expr}" '
                    "--output-on-failure --output-junit native-platform-eler.xml -j2"
                ),
                "required_public_release_ctest_command": (
                    "ctest --test-dir <native-release-build> "
                    "-L public_release_correctness --output-on-failure "
                    "--output-junit native-public-release.xml -j2"
                ),
                "required_engine_listener_ctest_command": (
                    "ctest --test-dir <native-release-build> "
                    "-L engine_listener_enterprise --output-on-failure "
                    "--output-junit native-engine-listener-enterprise.xml -j2"
                ),
                "required_sblr_surface_ctest_command": (
                    "ctest --test-dir <native-release-build> "
                    "-L sblr_surface --output-on-failure "
                    "--output-junit native-sblr-surface.xml -j2"
                ),
                "required_cluster_stub_ctest_command": (
                    "ctest --test-dir <native-cluster-stub-build> "
                    "-L cluster_boundary --output-on-failure "
                    "--output-junit native-cluster-boundary.xml -j2"
                ),
                "required_return_artifacts": ";".join(NATIVE_PLATFORM_RETURN_ARTIFACTS),
                "build_requirements": build_requirements,
                "release_layout": release_layout,
                "build_requirements_sha256": sha256_text(build_text),
                "release_layout_sha256": sha256_text(layout_text),
                "native_result_acceptance_rule": (
                    "all required junit/log/generated-gate artifacts returned; "
                    "no failed skipped disabled xfail waiver not-run platform-excluded "
                    "or unsupported-profile result; no product support or external "
                    "audit completion claim before manager import"
                ),
                "support_claim_before_native_runner": "false",
                "product_completion_claim": "false",
                "external_audit_completion_claim": "false",
            }
        )

    if errors:
        fail("native_platform_handoff:" + ";".join(errors[:12]))
    return {
        "schema_version": 1,
        "gate": "ELER-005-NATIVE-HANDOFF",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_005_NATIVE_PLATFORM_HANDOFF_MATRIX",
        "host_execution_scope": "linux_manager_generated_handoff",
        "nonhost_execution_scope": "windows_x64_freebsd_native_runner_required",
        "support_claim_before_native_runner": False,
        "product_completion_claim": False,
        "external_audit_completion_claim": False,
        "required_label_count": len(NATIVE_PLATFORM_HANDOFF_LABELS),
        "row_count": len(rows),
        "rows": rows,
    }


def numeric_xml_attr(element: ET.Element, attr_name: str) -> int:
    value = str(element.attrib.get(attr_name, "0") or "0").strip()
    if not value:
        return 0
    try:
        return int(value)
    except ValueError:
        return -1


def junit_package_errors(path: Path, package_root: Path) -> list[str]:
    errors: list[str] = []
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError as exc:
        return [f"junit_xml_invalid:{path.relative_to(package_root).as_posix()}:{exc}"]
    except OSError as exc:
        return [f"junit_xml_unreadable:{path.relative_to(package_root).as_posix()}:{exc}"]
    suites = [root]
    suites.extend(root.findall(".//testsuite"))
    tests_total = 0
    for suite in suites:
        for attr_name in ("tests", "failures", "errors", "skipped", "disabled"):
            attr_value = numeric_xml_attr(suite, attr_name)
            if attr_value < 0:
                errors.append(
                    f"junit_attr_not_numeric:{path.relative_to(package_root).as_posix()}:{attr_name}"
                )
            if attr_name == "tests" and attr_value > 0:
                tests_total += attr_value
            if attr_name in {"failures", "errors", "skipped", "disabled"} and attr_value > 0:
                errors.append(
                    f"junit_forbidden_result:{path.relative_to(package_root).as_posix()}:"
                    f"{attr_name}={attr_value}"
                )
    testcases = root.findall(".//testcase")
    if not testcases and tests_total <= 0:
        errors.append(f"junit_no_testcases:{path.relative_to(package_root).as_posix()}")
    for testcase in testcases:
        testcase_name = str(testcase.attrib.get("name", "unnamed"))
        status = str(testcase.attrib.get("status", "")).strip().lower()
        if status and status not in {"run", "passed", "pass", "success", "completed"}:
            errors.append(
                f"junit_forbidden_testcase_status:"
                f"{path.relative_to(package_root).as_posix()}:{testcase_name}:{status}"
            )
        for child in testcase:
            if child.tag.split("}", 1)[-1].lower() in {"failure", "error", "skipped"}:
                errors.append(
                    f"junit_forbidden_testcase_child:"
                    f"{path.relative_to(package_root).as_posix()}:{testcase_name}:"
                    f"{child.tag.split('}', 1)[-1].lower()}"
                )
    text = path.read_text(encoding="utf-8", errors="replace").lower()
    for marker in NATIVE_PLATFORM_PACKAGE_FORBIDDEN_MARKERS:
        if marker in text:
            errors.append(
                f"junit_forbidden_marker:{path.relative_to(package_root).as_posix()}:{marker}"
            )
    return errors


def parse_package_cmake_cache(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw or raw.startswith(("#", "//")) or ":" not in raw or "=" not in raw:
            continue
        key_type, value = raw.split("=", 1)
        key = key_type.split(":", 1)[0]
        values[key] = value
    return values


def json_truthy(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    if value is None:
        return False
    return str(value).strip().lower() in {"1", "true", "yes", "on", "complete", "completed"}


def iter_json_values(value: Any, path: str = "$") -> Iterable[tuple[str, Any]]:
    if isinstance(value, dict):
        for key, child in value.items():
            child_path = f"{path}.{key}"
            yield child_path, child
            yield from iter_json_values(child, child_path)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            child_path = f"{path}[{index}]"
            yield child_path, child
            yield from iter_json_values(child, child_path)


def package_json_errors(path: Path,
                        package_root: Path,
                        expected_target: str) -> list[str]:
    relative = path.relative_to(package_root).as_posix()
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        return [f"package_json_invalid:{relative}:{exc}"]
    if not isinstance(loaded, dict):
        return [f"package_json_not_object:{relative}"]
    errors: list[str] = []
    for json_path, value in iter_json_values(loaded):
        if isinstance(value, str):
            lowered = value.lower()
            for marker in NATIVE_PLATFORM_PACKAGE_FORBIDDEN_MARKERS:
                if marker in lowered:
                    errors.append(f"package_json_forbidden_marker:{relative}:{json_path}:{marker}")
        key = json_path.rsplit(".", 1)[-1]
        if key in {
            "support_claim_before_native_runner",
            "product_completion_claim",
            "external_audit_completion_claim",
            "external_human_audit_completed",
        } and json_truthy(value):
            errors.append(f"package_json_forbidden_claim:{relative}:{json_path}")
    if relative.endswith("engine_listener_native_platform_handoff_gate.json"):
        for key in (
            "support_claim_before_native_runner",
            "product_completion_claim",
            "external_audit_completion_claim",
        ):
            if loaded.get(key) is not False:
                errors.append(f"package_handoff_claim_not_false:{relative}:{key}")
        rows = loaded.get("rows")
        if not isinstance(rows, list):
            errors.append(f"package_handoff_rows_missing:{relative}")
        else:
            targets = {
                str(row.get("target", ""))
                for row in rows
                if isinstance(row, dict)
            }
            expected_targets = {
                str(spec["target"]) for spec in NATIVE_PLATFORM_HANDOFF_ROWS
            }
            if targets != expected_targets:
                errors.append(
                    f"package_handoff_target_set:{relative}:"
                    f"expected={','.join(sorted(expected_targets))}:"
                    f"actual={','.join(sorted(targets))}"
                )
            if expected_target not in targets:
                errors.append(f"package_handoff_expected_target_missing:{relative}:{expected_target}")
    if relative.endswith("engine_listener_gold_enterprise_readiness_gate.json"):
        if loaded.get("product_completion_claim") is not False:
            errors.append(f"package_gold_product_claim_not_false:{relative}")
        if loaded.get("external_human_audit_completed") is not False:
            errors.append(f"package_gold_external_audit_claim_not_false:{relative}")
    if relative.endswith("engine_listener_third_audit_evidence_package_gate.json"):
        if loaded.get("product_completion_claim") is not False:
            errors.append(f"package_third_audit_product_claim_not_false:{relative}")
        if loaded.get("external_zero_issue_audit_status") != "pending_external_auditor_decision":
            errors.append(f"package_third_audit_status_not_pending:{relative}")
    return errors


def package_csv_errors(path: Path, package_root: Path, expected_target: str) -> list[str]:
    relative = path.relative_to(package_root).as_posix()
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
    except csv.Error as exc:
        return [f"package_csv_invalid:{relative}:{exc}"]
    if not rows:
        return [f"package_csv_empty:{relative}"]
    errors: list[str] = []
    targets = {str(row.get("target", "")) for row in rows}
    expected_targets = {str(spec["target"]) for spec in NATIVE_PLATFORM_HANDOFF_ROWS}
    if expected_target not in targets:
        errors.append(f"package_csv_expected_target_missing:{relative}:{expected_target}")
    if expected_targets - targets:
        errors.append(
            f"package_csv_target_set_missing:{relative}:"
            f"{','.join(sorted(expected_targets - targets))}"
        )
    for index, row in enumerate(rows, start=1):
        for key in (
            "support_claim_before_native_runner",
            "product_completion_claim",
            "external_audit_completion_claim",
        ):
            value = str(row.get(key, "false")).strip().lower()
            if value not in {"", "false", "0", "no"}:
                errors.append(f"package_csv_forbidden_claim:{relative}:row={index}:{key}={value}")
        row_text = "|".join(str(value).lower() for value in row.values())
        for marker in NATIVE_PLATFORM_PACKAGE_FORBIDDEN_MARKERS:
            if marker in row_text:
                errors.append(f"package_csv_forbidden_marker:{relative}:row={index}:{marker}")
    return errors


def validate_native_proof_package_content(package_root: Path,
                                          expected_target: str) -> list[str]:
    errors: list[str] = []
    if expected_target not in NATIVE_PLATFORM_PACKAGE_TARGET_CACHE:
        return [f"unknown_native_package_target:{expected_target}"]
    for artifact in NATIVE_PLATFORM_RETURN_ARTIFACTS:
        path = package_root / artifact
        if not path.exists() or not path.is_file():
            errors.append(f"package_required_artifact_missing:{artifact}")
    if errors:
        return errors
    for artifact in NATIVE_PLATFORM_JUNIT_ARTIFACTS:
        errors.extend(junit_package_errors(package_root / artifact, package_root))
    cache = parse_package_cmake_cache(package_root / "CMakeCache.txt")
    system_name, system_processor = NATIVE_PLATFORM_PACKAGE_TARGET_CACHE[expected_target]
    if cache.get("CMAKE_SIZEOF_VOID_P") != "8":
        errors.append(
            "package_cmake_cache_not_64_bit:"
            f"{expected_target}:{cache.get('CMAKE_SIZEOF_VOID_P', '')}"
        )
    actual_system = cache.get("CMAKE_SYSTEM_NAME", "")
    if actual_system != system_name:
        errors.append(
            f"package_cmake_system_name:{expected_target}:"
            f"expected={system_name}:actual={actual_system}"
        )
    actual_processor = cache.get("CMAKE_SYSTEM_PROCESSOR", "").lower()
    accepted_processors = {system_processor.lower()}
    if expected_target == "windows_x64":
        accepted_processors.update({"x86_64", "amd64"})
    if expected_target == "freebsd_x86_64":
        accepted_processors.update({"x86_64", "amd64"})
    if actual_processor not in accepted_processors:
        errors.append(
            f"package_cmake_processor:{expected_target}:"
            f"expected_one_of={','.join(sorted(accepted_processors))}:"
            f"actual={actual_processor}"
        )
    log_text = (package_root / "Testing/Temporary/LastTest.log").read_text(
        encoding="utf-8", errors="replace"
    ).lower()
    for marker in NATIVE_PLATFORM_PACKAGE_FORBIDDEN_MARKERS:
        if marker in log_text:
            errors.append(f"package_last_test_log_forbidden_marker:{marker}")
    for artifact in NATIVE_PLATFORM_GENERATED_JSON_ARTIFACTS:
        errors.extend(package_json_errors(package_root / artifact,
                                          package_root,
                                          expected_target))
    for artifact in NATIVE_PLATFORM_GENERATED_CSV_ARTIFACTS:
        errors.extend(package_csv_errors(package_root / artifact,
                                         package_root,
                                         expected_target))
    return errors


def junit_fixture_text(test_name: str,
                       mutation: str | None,
                       artifact: str) -> str:
    failures = "1" if mutation == "junit_failure" and artifact == "native-platform-eler.xml" else "0"
    errors = "1" if mutation == "junit_error" and artifact == "native-platform-eler.xml" else "0"
    skipped = "1" if mutation == "junit_skipped" and artifact == "native-platform-eler.xml" else "0"
    disabled = "1" if mutation == "junit_disabled" and artifact == "native-platform-eler.xml" else "0"
    child = ""
    status = "run"
    if failures == "1":
        child = "<failure message=\"synthetic failure\" />"
    elif errors == "1":
        child = "<error message=\"synthetic error\" />"
    elif skipped == "1":
        child = "<skipped message=\"synthetic skip\" />"
    if mutation == "junit_notrun_marker" and artifact == "native-platform-eler.xml":
        status = "not-run"
    return (
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        f"<testsuite name=\"{artifact}\" tests=\"1\" failures=\"{failures}\" "
        f"errors=\"{errors}\" skipped=\"{skipped}\" disabled=\"{disabled}\">\n"
        f"  <testcase classname=\"native.package.contract\" "
        f"name=\"{test_name}\" status=\"{status}\">{child}</testcase>\n"
        "</testsuite>\n"
    )


def write_synthetic_native_proof_package(package_root: Path,
                                         expected_target: str,
                                         mutation: str | None = None) -> None:
    package_root.mkdir(parents=True, exist_ok=True)
    for artifact in NATIVE_PLATFORM_JUNIT_ARTIFACTS:
        (package_root / artifact).write_text(
            junit_fixture_text(expected_target, mutation, artifact),
            encoding="utf-8",
        )
    system_name, system_processor = NATIVE_PLATFORM_PACKAGE_TARGET_CACHE[expected_target]
    pointer_size = "4" if mutation == "cmake_32_bit" else "8"
    (package_root / "CMakeCache.txt").write_text(
        "\n".join(
            (
                f"CMAKE_SYSTEM_NAME:STRING={system_name}",
                f"CMAKE_SYSTEM_PROCESSOR:STRING={system_processor}",
                f"CMAKE_SIZEOF_VOID_P:INTERNAL={pointer_size}",
                "SB_ENABLE_CLUSTER_PROVIDER:BOOL=OFF",
                "SB_CLUSTER_PROVIDER_STUB:BOOL=ON",
                "",
            )
        ),
        encoding="utf-8",
    )
    log_marker = "unsupported-profile" if mutation == "unsupported_profile_marker" else "all native gates passed"
    log_path = package_root / "Testing/Temporary/LastTest.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(log_marker + "\n", encoding="utf-8")
    gate_dir = package_root / "tests/engine_listener_enterprise"
    gate_dir.mkdir(parents=True, exist_ok=True)
    handoff = {
        "schema_version": 1,
        "gate": "ELER-005-NATIVE-HANDOFF",
        "support_claim_before_native_runner": mutation == "support_claim_true",
        "product_completion_claim": mutation == "product_claim_true",
        "external_audit_completion_claim": mutation == "external_audit_claim_true",
        "row_count": 2,
        "rows": [
            {
                "platform": "windows",
                "target": "windows_x64",
                "support_claim_before_native_runner": "true" if mutation == "support_claim_true" else "false",
                "product_completion_claim": "true" if mutation == "product_claim_true" else "false",
                "external_audit_completion_claim": "true" if mutation == "external_audit_claim_true" else "false",
            },
            {
                "platform": "freebsd",
                "target": "freebsd_x86_64",
                "support_claim_before_native_runner": "true" if mutation == "support_claim_true" else "false",
                "product_completion_claim": "true" if mutation == "product_claim_true" else "false",
                "external_audit_completion_claim": "true" if mutation == "external_audit_claim_true" else "false",
            },
        ],
    }
    if mutation == "missing_target_row":
        handoff["rows"] = handoff["rows"][:1]
        handoff["row_count"] = 1
    gold = {
        "schema_version": 1,
        "gate": "ELER-112",
        "external_human_audit_completed": False,
        "product_completion_claim": mutation == "product_claim_true",
        "platform_scope": "linux_enterprise_release_candidate_cross_platform_execution_pending",
    }
    third_audit = {
        "schema_version": 1,
        "gate": "ELER-090",
        "external_zero_issue_audit_status": (
            "complete" if mutation == "external_audit_claim_true"
            else "pending_external_auditor_decision"
        ),
        "product_completion_claim": False,
    }
    (gate_dir / "engine_listener_native_platform_handoff_gate.json").write_text(
        json.dumps(handoff, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (gate_dir / "engine_listener_gold_enterprise_readiness_gate.json").write_text(
        json.dumps(gold, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (gate_dir / "engine_listener_third_audit_evidence_package_gate.json").write_text(
        json.dumps(third_audit, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    with (gate_dir / "engine_listener_native_platform_handoff_gate.csv").open(
        "w", newline="", encoding="utf-8"
    ) as handle:
        fieldnames = [
            "platform",
            "target",
            "support_claim_before_native_runner",
            "product_completion_claim",
            "external_audit_completion_claim",
        ]
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(handoff["rows"])
    if mutation == "missing_required_artifact":
        (package_root / "native-sblr-surface.xml").unlink()


def validate_native_platform_proof_package_import(project_root: Path,
                                                  build_root: Path) -> dict[str, Any]:
    handoff = validate_native_platform_handoff(project_root, build_root)
    handoff_targets = {
        str(row.get("target", ""))
        for row in handoff.get("rows", [])
        if isinstance(row, dict)
    }
    expected_targets = {str(spec["target"]) for spec in NATIVE_PLATFORM_HANDOFF_ROWS}
    if handoff_targets != expected_targets:
        fail(
            "native_package_import_handoff_targets:"
            f"expected={','.join(sorted(expected_targets))}:"
            f"actual={','.join(sorted(handoff_targets))}"
        )
    fixture_root = build_root / "tests/engine_listener_enterprise/native_platform_proof_package_import_contract"
    if fixture_root.exists():
        shutil.rmtree(fixture_root)
    rows: list[dict[str, str]] = []
    try:
        for target in sorted(expected_targets):
            package_root = fixture_root / f"valid_{target}"
            write_synthetic_native_proof_package(package_root, target)
            errors = validate_native_proof_package_content(package_root, target)
            if errors:
                fail(f"native_package_import_valid_fixture_rejected:{target}:{';'.join(errors[:8])}")
            rows.append(
                {
                    "scenario": f"valid_{target}",
                    "target": target,
                    "expected_result": "accepted",
                    "observed_result": "accepted",
                    "error_count": "0",
                    "observed_errors": "",
                    "required_return_artifact_count": str(len(NATIVE_PLATFORM_RETURN_ARTIFACTS)),
                    "required_return_artifacts": ";".join(NATIVE_PLATFORM_RETURN_ARTIFACTS),
                    "manager_import_status": "contract_proven_native_package_not_received",
                    "support_claim_before_native_runner": "false",
                    "product_completion_claim": "false",
                    "external_audit_completion_claim": "false",
                    "native_package_received": "false",
                }
            )
        negative_mutations = (
            "missing_required_artifact",
            "junit_failure",
            "junit_error",
            "junit_skipped",
            "junit_disabled",
            "junit_notrun_marker",
            "support_claim_true",
            "product_claim_true",
            "external_audit_claim_true",
            "cmake_32_bit",
            "unsupported_profile_marker",
            "missing_target_row",
        )
        for mutation in negative_mutations:
            target = "windows_x64"
            package_root = fixture_root / f"reject_{mutation}"
            write_synthetic_native_proof_package(package_root, target, mutation)
            errors = validate_native_proof_package_content(package_root, target)
            if not errors:
                fail(f"native_package_import_negative_fixture_accepted:{mutation}")
            rows.append(
                {
                    "scenario": f"reject_{mutation}",
                    "target": target,
                    "expected_result": "rejected",
                    "observed_result": "rejected",
                    "error_count": str(len(errors)),
                    "observed_errors": ";".join(errors[:6]),
                    "required_return_artifact_count": str(len(NATIVE_PLATFORM_RETURN_ARTIFACTS)),
                    "required_return_artifacts": ";".join(NATIVE_PLATFORM_RETURN_ARTIFACTS),
                    "manager_import_status": "contract_proven_native_package_not_received",
                    "support_claim_before_native_runner": "false",
                    "product_completion_claim": "false",
                    "external_audit_completion_claim": "false",
                    "native_package_received": "false",
                }
            )
    finally:
        if fixture_root.exists():
            shutil.rmtree(fixture_root)
    return {
        "schema_version": 1,
        "gate": "ELER-005-NATIVE-PROOF-PACKAGE-IMPORT",
        "private_docs_required": False,
        "git_history_required": False,
        "matrix_id": "ELER_005_NATIVE_PLATFORM_PROOF_PACKAGE_IMPORT_CONTRACT",
        "host_execution_scope": "linux_manager_import_contract",
        "nonhost_execution_scope": "windows_x64_freebsd_native_package_required",
        "manager_import_status": "manager_import_contract_proven_native_packages_pending",
        "native_packages_received": False,
        "support_claim_before_native_runner": False,
        "product_completion_claim": False,
        "external_audit_completion_claim": False,
        "required_targets": ";".join(sorted(expected_targets)),
        "required_artifacts": ";".join(NATIVE_PLATFORM_RETURN_ARTIFACTS),
        "row_count": len(rows),
        "rows": rows,
    }


def require_token(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def parse_cmake_cache(build_root: Path) -> dict[str, str]:
    cache_path = build_root / "CMakeCache.txt"
    if not cache_path.exists() or not cache_path.is_file():
        fail("release_profile_cmake_cache_missing")
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith(("#", "//")) or ":" not in line or "=" not in line:
            continue
        key_type, value = line.split("=", 1)
        key, _type = key_type.split(":", 1)
        values[key] = value
    return values


def require_cache(cache: dict[str, str], key: str, expected: str) -> None:
    actual = cache.get(key)
    if actual != expected:
        fail(f"release_profile_cache_value:{key}:expected={expected}:actual={actual}")


def matrix_row(profile_item: str,
               status: str,
               release_profile: str,
               build_flags: str,
               provider_mode: str,
               runtime_config_defaults: str,
               proof_target: str,
               fixture_dev_stub_status: str,
               linux_result: str,
               cross_platform_status: str,
               evidence: str) -> dict[str, str]:
    return {
        "profile_item": profile_item,
        "status": status,
        "release_profile": release_profile,
        "build_flags": build_flags,
        "provider_mode": provider_mode,
        "runtime_config_defaults": runtime_config_defaults,
        "proof_target": proof_target,
        "fixture_dev_stub_status": fixture_dev_stub_status,
        "linux_result": linux_result,
        "cross_platform_status": cross_platform_status,
        "evidence": evidence,
    }


def validate_release_profile_completeness(project_root: Path,
                                          build_root: Path) -> dict[str, Any]:
    cmake_text = read_text(project_root / "CMakeLists.txt", project_root)
    release_cmake = read_text(project_root / "tests/release/CMakeLists.txt", project_root)
    config_header = read_text(project_root / "src/server/config.hpp", project_root)
    listener_config = read_text(project_root / "src/listener/listener_config.cpp", project_root)
    listener_config_header = read_text(project_root / "src/listener/listener_config.hpp",
                                       project_root)
    tests_text = ctest_text(project_root)
    cache = parse_cmake_cache(build_root)
    generated_profile = read_text(
        build_root / "generated/scratchbird/core/platform/noncluster_engine_profile.hpp",
        project_root,
    )

    required_project_tokens = (
        'SB_NONCLUSTER_ENGINE_PROFILE "release-complete"',
        "release-complete bootstrap emergency",
        "SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE ON",
        "SB_NONCLUSTER_ENGINE_DEGRADED_PROFILE OFF",
        "SB_AGENT_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds",
        "SB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds",
        "SB_COMMERCIAL_ENFORCE_PRODUCTION_BUILD_GATE cannot be disabled for release-complete builds",
        "SCRATCHBIRD_AGENT_PRODUCTION_BUILD",
        "SCRATCHBIRD_OPTIMIZER_PRODUCTION_BUILD",
        "SCRATCHBIRD_COMMERCIAL_READINESS_PRODUCTION_BUILD",
        "SB_ENABLE_CLUSTER_PROVIDER",
        "SB_CLUSTER_PROVIDER_STUB",
        "SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY",
        "SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS",
    )
    required_release_tests = (
        "public_production_feature_audit_gate",
        "public_default_config_check",
        "public_secure_defaults_gate",
        "public_cluster_build_matrix_gate",
        "public_platform_matrix_gate",
    )
    for token in required_project_tokens:
        require_token(cmake_text, token, "release_profile_project_cmake")
    for token in required_release_tests:
        require_token(release_cmake, token, "release_profile_ctest")
    for token in SKIP_OR_WAIVER_TOKENS:
        if token in release_cmake:
            fail(f"release_profile_skip_or_waiver:{token}")
    for key, expected in (
        ("SB_NONCLUSTER_ENGINE_PROFILE", "release-complete"),
        ("SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE", "ON"),
        ("SB_NONCLUSTER_ENGINE_DEGRADED_PROFILE", "OFF"),
        ("SB_AGENT_ENFORCE_PRODUCTION_BUILD_GATE", "ON"),
        ("SB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE", "ON"),
        ("SB_COMMERCIAL_ENFORCE_PRODUCTION_BUILD_GATE", "ON"),
        ("SCRATCHBIRD_ENABLE_DEBUG_LOGS", "OFF"),
        ("SCRATCHBIRD_ENABLE_HOTPATH_TRACE", "OFF"),
        ("SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE", "OFF"),
        ("SCRATCHBIRD_ENABLE_PREPARED_TRACE", "OFF"),
        ("SB_ENABLE_CLUSTER_PROVIDER", "OFF"),
        ("SB_CLUSTER_PROVIDER_STUB", "OFF"),
        ("SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS", "OFF"),
    ):
        require_cache(cache, key, expected)
    for token in (
        '#define SCRATCHBIRD_NONCLUSTER_ENGINE_PROFILE "release-complete"',
        "#define SCRATCHBIRD_NONCLUSTER_ENGINE_RELEASE_COMPLETE 1",
        "#define SCRATCHBIRD_NONCLUSTER_ENGINE_DEGRADED_PROFILE 0",
    ):
        require_token(generated_profile, token, "release_profile_generated_header")
    for token in (
        'std::string security_authority_mode = "database_local"',
        'std::string security_provider_family = "local_password"',
        "bool security_default_policy_installed = true",
        "bool database_auto_create = false",
        "bool listener_native_enabled = false",
        "bool listener_native_tls_required = true",
        'std::string memory_policy_name = "default_local_server_memory_cache_v1"',
        "bool memory_zero_memory_on_release = true",
    ):
        require_token(config_header, token, "release_profile_runtime_defaults")
    for token in (
        "bool allow_dev_dbbt_env{false}",
        "bool allow_test_dbbt_builtin{false}",
    ):
        require_token(listener_config_header, token, "release_profile_listener_fixture_defaults")
    for token in (
        "LISTENER.CONFIG.DEV_DBBT_KEY_DISABLED",
        "dev_environment DBBT keys require allow_dev_dbbt_env=true",
        "LISTENER.CONFIG.TEST_DBBT_KEY_DISABLED",
        "test_builtin DBBT keys require allow_test_dbbt_builtin=true",
    ):
        require_token(listener_config, token, "release_profile_listener_fixture_fence")

    feature_rows = declared_feature_rows(project_root)
    missing = [
        row["feature_id"] for row in feature_rows
        if len(row["matched_proofs"]) != len(row["proof_markers"])
    ]
    if missing:
        fail("release_profile_declared_feature_unexposed:" + ",".join(missing))

    build_flags = (
        "agent_gate=ON;optimizer_gate=ON;commercial_gate=ON;"
        "debug_logs=OFF;hotpath_trace=OFF;exec_profile_trace=OFF;"
        "prepared_trace=OFF"
    )
    provider_mode = "cluster_provider=OFF;cluster_stub=OFF;external_cluster_provider=absent"
    runtime_defaults = (
        "security=database_local/local_password;db_auto_create=false;"
        "native_listener=false;tls_required=true;"
        "memory_policy=default_local_server_memory_cache_v1"
    )
    rows: list[dict[str, str]] = [
        matrix_row("noncluster_engine_profile",
                   "release_complete_configured",
                   "release-complete",
                   build_flags,
                   provider_mode,
                   runtime_defaults,
                   "engine_listener_release_profile_completeness_gate",
                   "bootstrap_emergency_not_release_proof",
                   "pass",
                   "windows_x64_freebsd_pending",
                   "cmake_cache_and_generated_header_agree"),
        matrix_row("production_build_gates",
                   "fail_closed",
                   "release-complete",
                   build_flags,
                   provider_mode,
                   runtime_defaults,
                   "public_production_feature_audit_gate",
                   "fixture_dev_debug_flags_refused",
                   "pass",
                   "windows_x64_freebsd_pending",
                   "agent_optimizer_commercial_matrix_registered_and_passing"),
        matrix_row("cluster_provider_boundary",
                   "compile_time_gated",
                   "release-complete",
                   build_flags,
                   provider_mode,
                   runtime_defaults,
                   "public_cluster_build_matrix_gate",
                   "cluster_stub_cannot_claim_production",
                   "pass",
                   "windows_x64_freebsd_pending",
                   "cluster_disabled_by_default_stub_requires_enable_external_required_for_claims"),
        matrix_row("runtime_secure_defaults",
                   "secure_defaults_exposed",
                   "release-complete",
                   build_flags,
                   provider_mode,
                   runtime_defaults,
                   "public_default_config_check;public_secure_defaults_gate",
                   "fixture_dbbt_key_sources_fenced",
                   "pass",
                   "windows_x64_freebsd_pending",
                   "server_config_defaults_and_listener_fixture_secret_fence_verified"),
        matrix_row("release_ctest_profile_gates",
                   "registered",
                   "release-complete",
                   build_flags,
                   provider_mode,
                   runtime_defaults,
                   ";".join(required_release_tests),
                   "skip_disabled_xfail_refused",
                   "pass",
                   "windows_x64_freebsd_pending",
                   "required_release_profile_ctest_gates_registered"),
    ]
    for row in feature_rows:
        proof_targets = ";".join(row["matched_proofs"])
        rows.append(matrix_row(
            row["feature_id"],
            "declared_feature_exposed_under_release_complete",
            "release-complete",
            build_flags,
            provider_mode,
            runtime_defaults,
            proof_targets,
            "no_fixture_dev_stub_or_unsupported_profile_completion",
            "pass",
            "windows_x64_freebsd_pending" if row["platform_sensitive"] else "not_platform_sensitive",
            f"{row['subsystem']}:{row['title']}",
        ))

    return {
        "schema_version": 1,
        "gate": "ELER-087",
        "private_docs_required": False,
        "git_history_required": False,
        "release_profile": "release-complete",
        "platform_scope": "linux_proof_cross_platform_pending",
        "declared_feature_count": len(feature_rows),
        "matrix_id": "ELER_087_ENTERPRISE_RELEASE_PROFILE_MATRIX",
        "cmake_cache_profile_sha256": sha256_text(
            "|".join(f"{key}={cache.get(key, '')}" for key, _ in (
                ("SB_NONCLUSTER_ENGINE_PROFILE", ""),
                ("SB_NONCLUSTER_ENGINE_RELEASE_COMPLETE", ""),
                ("SB_NONCLUSTER_ENGINE_DEGRADED_PROFILE", ""),
                ("SB_AGENT_ENFORCE_PRODUCTION_BUILD_GATE", ""),
                ("SB_OPTIMIZER_ENFORCE_PRODUCTION_BUILD_GATE", ""),
                ("SB_COMMERCIAL_ENFORCE_PRODUCTION_BUILD_GATE", ""),
            ))
        ),
        "generated_profile_header_sha256": sha256_text(generated_profile),
        "rows": rows,
        "tests_text_sha256": sha256_text(tests_text),
    }


def write_outputs(output: Path, build_root: Path, evidence: dict[str, Any]) -> None:
    try:
        output_record = output.resolve().relative_to(build_root.resolve()).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")
    output.parent.mkdir(parents=True, exist_ok=True)
    encoded = json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    evidence["evidence_sha256"] = sha256_text(encoded)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    csv_path = output.with_suffix(".csv")
    rows = evidence.get("rows")
    if isinstance(rows, list) and rows:
        with csv_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(rows)


def build_evidence(project_root: Path, build_root: Path, mode: str) -> dict[str, Any]:
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")
    modes = {
        "prior-execution_plan-semantics": validate_prior_execution_plan_semantics,
        "declared-feature-inventory": validate_declared_feature_inventory,
        "anti-skeleton-classifier": validate_anti_skeleton_classifier,
        "project-tests-proof-location": validate_project_tests_proof_location,
    }
    if mode == "all":
        results = {name: func(project_root) for name, func in modes.items()}
        results["platform-matrix"] = (
            validate_platform_matrix(project_root, build_root)
        )
        results["native-platform-handoff"] = (
            validate_native_platform_handoff(project_root, build_root)
        )
        results["native-platform-proof-package-import"] = (
            validate_native_platform_proof_package_import(project_root, build_root)
        )
        results["no-skip-waiver-xfail-release-proof"] = (
            validate_no_skip_waiver_xfail_release_proof(project_root, build_root)
        )
        results["row-level-traceability-manifest"] = (
            validate_row_level_traceability_manifest(project_root, build_root)
        )
        results["crash-fault-campaign"] = (
            validate_crash_fault_campaign(project_root, build_root)
        )
        results["integrated-product-proof"] = (
            validate_integrated_product_proof(project_root, build_root)
        )
        results["crash-recovery-certification"] = (
            validate_crash_recovery_certification(project_root, build_root)
        )
        results["adversarial-security-validation"] = (
            validate_adversarial_security_validation(project_root, build_root)
        )
        results["fuzz-property-invariant-suite"] = (
            validate_fuzz_property_invariant_suite(project_root, build_root)
        )
        results["performance-scalability-baseline"] = (
            validate_performance_scalability_baseline(project_root, build_root)
        )
        results["soak-certification"] = (
            validate_soak_certification(project_root, build_root)
        )
        results["compatibility-upgrade-downgrade"] = (
            validate_compatibility_upgrade_downgrade(project_root, build_root)
        )
        results["operational-readiness"] = (
            validate_operational_readiness(project_root, build_root)
        )
        results["release-artifact-trust"] = (
            validate_release_artifact_trust(project_root, build_root)
        )
        results["enterprise-documentation"] = (
            validate_enterprise_documentation(project_root, build_root)
        )
        results["support-maintenance-policy"] = (
            validate_support_maintenance_policy(project_root, build_root)
        )
        results["project-only-implementation-proof"] = (
            validate_project_only_implementation_proof(project_root, build_root)
        )
        results["external-review-closure-package"] = (
            validate_external_review_closure_package(project_root, build_root)
        )
        results["gold-enterprise-readiness"] = (
            validate_gold_enterprise_readiness(project_root, build_root)
        )
        results["third-audit-evidence-package"] = (
            validate_third_audit_evidence_package(project_root, build_root)
        )
        results["parser-facing-contract-freeze"] = (
            validate_parser_facing_contract_freeze(project_root, build_root)
        )
        results["generated-resource-determinism"] = (
            validate_generated_resource_determinism(project_root, build_root)
        )
        results["sbsql-parser-spec-execution_plan-sync"] = (
            validate_sbsql_parser_spec_execution_plan_sync(project_root, build_root)
        )
        results["release-profile-completeness"] = (
            validate_release_profile_completeness(project_root, build_root)
        )
        return {
            "schema_version": 1,
            "gate": "ELER-FOUNDATION",
            "private_docs_required": False,
            "git_history_required": False,
            "results": results,
        }
    if mode == "release-profile-completeness":
        evidence = validate_release_profile_completeness(project_root, build_root)
        evidence["private_docs_required"] = False
        evidence["git_history_required"] = False
        return evidence
    if mode == "platform-matrix":
        return validate_platform_matrix(project_root, build_root)
    if mode == "native-platform-handoff":
        return validate_native_platform_handoff(project_root, build_root)
    if mode == "native-platform-proof-package-import":
        return validate_native_platform_proof_package_import(project_root, build_root)
    if mode == "no-skip-waiver-xfail-release-proof":
        return validate_no_skip_waiver_xfail_release_proof(project_root, build_root)
    if mode == "row-level-traceability-manifest":
        return validate_row_level_traceability_manifest(project_root, build_root)
    if mode == "crash-fault-campaign":
        return validate_crash_fault_campaign(project_root, build_root)
    if mode == "integrated-product-proof":
        return validate_integrated_product_proof(project_root, build_root)
    if mode == "crash-recovery-certification":
        return validate_crash_recovery_certification(project_root, build_root)
    if mode == "adversarial-security-validation":
        return validate_adversarial_security_validation(project_root, build_root)
    if mode == "fuzz-property-invariant-suite":
        return validate_fuzz_property_invariant_suite(project_root, build_root)
    if mode == "performance-scalability-baseline":
        return validate_performance_scalability_baseline(project_root, build_root)
    if mode == "soak-certification":
        return validate_soak_certification(project_root, build_root)
    if mode == "compatibility-upgrade-downgrade":
        return validate_compatibility_upgrade_downgrade(project_root, build_root)
    if mode == "operational-readiness":
        return validate_operational_readiness(project_root, build_root)
    if mode == "release-artifact-trust":
        return validate_release_artifact_trust(project_root, build_root)
    if mode == "enterprise-documentation":
        return validate_enterprise_documentation(project_root, build_root)
    if mode == "support-maintenance-policy":
        return validate_support_maintenance_policy(project_root, build_root)
    if mode == "project-only-implementation-proof":
        return validate_project_only_implementation_proof(project_root, build_root)
    if mode == "external-review-closure-package":
        return validate_external_review_closure_package(project_root, build_root)
    if mode == "gold-enterprise-readiness":
        return validate_gold_enterprise_readiness(project_root, build_root)
    if mode == "third-audit-evidence-package":
        return validate_third_audit_evidence_package(project_root, build_root)
    if mode == "parser-facing-contract-freeze":
        return validate_parser_facing_contract_freeze(project_root, build_root)
    if mode == "generated-resource-determinism":
        return validate_generated_resource_determinism(project_root, build_root)
    if mode == "sbsql-parser-spec-execution_plan-sync":
        return validate_sbsql_parser_spec_execution_plan_sync(project_root, build_root)
    if mode not in modes:
        fail(f"unknown_mode:{mode}")
    evidence = modes[mode](project_root)
    evidence["private_docs_required"] = False
    evidence["git_history_required"] = False
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--mode", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args.project_root.resolve(),
                              args.build_root.resolve(),
                              args.mode)
    write_outputs(args.output.resolve(), args.build_root.resolve(), evidence)
    print(f"engine_listener_enterprise_gate_mode={args.mode}")
    print(f"engine_listener_enterprise_gate_output={args.output.resolve().relative_to(args.build_root.resolve()).as_posix()}")
    print(f"engine_listener_enterprise_gate_sha256={evidence['evidence_sha256']}")
    print("engine_listener_enterprise_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
