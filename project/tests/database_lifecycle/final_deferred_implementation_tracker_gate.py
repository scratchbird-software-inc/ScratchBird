#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Item-level gate for final deferred migration implementation closure.

FINAL-DEFERRED-IMPLEMENTATION-TRACKER

The private migration tracker is allowed to describe remaining work, but it is
not implementation evidence. This project-local gate is the source authority
for which MDF rows are still open, which implemented rows have project evidence,
and which parser/documentation-only deferred families are explicitly excluded.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


DATABASE_CMAKE = "project/tests/database_lifecycle/CMakeLists.txt"
SELF_PATH = "project/tests/database_lifecycle/final_deferred_implementation_tracker_gate.py"
TRACKER_LABEL = "FINAL-DEFERRED-IMPLEMENTATION-TRACKER"
MDF_000_SEARCH_KEY = "DEFERRED-MIGRATION-IMPLEMENTATION-CLOSURE"


@dataclass(frozen=True)
class SourceEvidence:
    path: str
    tokens: tuple[str, ...]


@dataclass(frozen=True)
class TestEvidence:
    cmake_path: str
    test_name: str
    labels: tuple[str, ...]


@dataclass(frozen=True)
class TrackerRow:
    row_id: str
    area: str
    status: str
    source_markers: tuple[str, ...]
    evidence: tuple[SourceEvidence, ...] = ()
    tests: tuple[TestEvidence, ...] = ()
    closure_classes: tuple[str, ...] = ()


EXCLUDED_KEY_FAMILIES = (
    "DEFER-MSSQL-*",
    "DEFER-ORA-*",
    "DEFER-DB2-*",
    "DEFER-REFERENCE-METHOD-BINDINGS",
    "DEFER-DTYPE-METHOD-BINDING-IMPLEMENTATION",
    "DEFER-DIO-REFERENCE-OPTIMIZATION",
    "DEFER-IDX-REFERENCE-PROFILES",
    "DEFER-DTYPE-DOC-CLEANUP",
)

IMPLEMENTED = "Implemented"
OPEN = "Open"

REQUIRED_IMPLEMENTED_CLASSES = {
    "project_source",
    "project_test",
    "ctest_label",
    "negative_self_check",
}

DISALLOWED_ONLY_CLASSES = {
    "documentation_only",
    "route_only",
    "descriptor_only",
    "refusal_only",
}

ROWS = (
    TrackerRow(
        "MDF-000",
        "Migration closure gate hardening",
        IMPLEMENTED,
        (
            "migration_deferred_operations_closure_gate",
            "DEFERRED-MIGRATION-IMPLEMENTATION-CLOSURE",
        ),
        (
            SourceEvidence(
                SELF_PATH,
                (
                    TRACKER_LABEL,
                    "MDF-000",
                    "MDF-026",
                    "release_gate_errors",
                    "docs_private_evidence_refused",
                    "route_only_descriptor_only_refused",
                ),
            ),
            SourceEvidence(
                DATABASE_CMAKE,
                (
                    "final_deferred_implementation_tracker_gate",
                    TRACKER_LABEL,
                    "MDF-000",
                    MDF_000_SEARCH_KEY,
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "final_deferred_implementation_tracker_gate",
                (
                    "final_deferred_implementation_tracker_gate",
                    TRACKER_LABEL,
                    "MDF-000",
                    MDF_000_SEARCH_KEY,
                    "migration_deferred_operations_closure",
                    "database_lifecycle",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "evidence_path_rejection",
            "release_open_row_failure",
        ),
    ),
    TrackerRow(
        "MDF-001",
        "Filespace relocation and online compaction",
        IMPLEMENTED,
        (
            "FILES-COMMAND-CLOSURE-MAINTENANCE-SUPPORTED-SUBSET-IMPLEMENTED",
            "ACTIVE-PRIMARY-REPLACEMENT-PHYSICAL-HEADER-RELOCATION-PROOF-IMPLEMENTED",
        ),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-001-FINAL-DEFERRED-FILESPACE-RELOCATION",
                    "FilespaceRelocationRequest",
                    "RelocateFilespaceBytes",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-FILESPACE-RELOCATION-COMMITTED",
                    "SB-FILESPACE-RELOCATION-PINS-ACTIVE",
                    "physical_byte_copy",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-001",
                    "TestFilespaceRelocation",
                    "SB-FILESPACE-RELOCATION-PINS-ACTIVE",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-001",
                    "FILES-COMMAND-CLOSURE-MAINTENANCE-SUPPORTED-SUBSET-IMPLEMENTED",
                    "ACTIVE-PRIMARY-REPLACEMENT-PHYSICAL-HEADER-RELOCATION-PROOF-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "physical_byte_copy",
            "restart_recovery",
            "pin_refusal",
            "retention_refusal",
            "cache_invalidation",
        ),
    ),
    TrackerRow(
        "MDF-002",
        "Core page body relocation",
        IMPLEMENTED,
        ("ACTIVE-PRIMARY-REPLACEMENT-AUTHORITY-SWITCH-IMPLEMENTED",),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-002-FINAL-DEFERRED-CORE-PAGE-BODY-RELOCATION",
                    "CorePageRelocationRequest",
                    "RelocateCorePageBodies",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-CORE-PAGE-RELOCATION-COMMITTED",
                    "SB-CORE-PAGE-RELOCATION-STALE-ROOT-AUTHORITY",
                    "page_body_copy",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-002",
                    "TestCorePageBodyRelocation",
                    "page_42.body",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-002",
                    "ACTIVE-PRIMARY-REPLACEMENT-AUTHORITY-SWITCH-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "page_body_copy",
            "root_authority_refusal",
            "digest_verification",
            "cache_invalidation",
        ),
    ),
    TrackerRow(
        "MDF-003",
        "Snapshot and shadow physical completion",
        IMPLEMENTED,
        ("SNAPSHOT-SHADOW-LOCAL-LIFECYCLE-COMMAND-SUBSET-IMPLEMENTED",),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-003-FINAL-DEFERRED-SNAPSHOT-SHADOW-PHYSICAL",
                    "SnapshotShadowPackageRequest",
                    "BuildSnapshotShadowPackage",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-SNAPSHOT-SHADOW-PACKAGE-COMMITTED",
                    "SB-SNAPSHOT-SHADOW-PARSER-RECOVERY-AUTHORITY-REFUSED",
                    "SB-SNAPSHOT-SHADOW-KMS-MANIFEST-REFUSED",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-003",
                    "TestSnapshotShadowPackage",
                    "snapshot_shadow.manifest",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-003",
                    "SNAPSHOT-SHADOW-LOCAL-LIFECYCLE-COMMAND-SUBSET-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "provider_bind",
            "kms_manifest_refusal",
            "restore_fence",
            "parser_authority_refusal",
        ),
    ),
    TrackerRow(
        "MDF-004",
        "Physical re-encryption",
        IMPLEMENTED,
        (
            "ENCRYPTION-KEY-LIFECYCLE-ENGINE-BOUNDARY-IMPLEMENTED",
            "ENCRYPTION-MAINTENANCE-SBSQL-ROUTE-IMPLEMENTED",
        ),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-004-FINAL-DEFERRED-PHYSICAL-REENCRYPTION",
                    "PhysicalReencryptionRequest",
                    "RewritePhysicalEncryptionProfile",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-PHYSICAL-REENCRYPTION-COMMITTED",
                    "SB-PHYSICAL-REENCRYPTION-RETENTION-BLOCKED",
                    "SB-PHYSICAL-REENCRYPTION-PLAINTEXT-DIAGNOSTIC-REFUSED",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-004",
                    "TestPhysicalReencryption",
                    "SB-PHYSICAL-REENCRYPTION-PLAINTEXT-DIAGNOSTIC-REFUSED",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-004",
                    "ENCRYPTION-KEY-LIFECYCLE-ENGINE-BOUNDARY-IMPLEMENTED",
                    "ENCRYPTION-MAINTENANCE-SBSQL-ROUTE-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "physical_rewrite",
            "protected_material_gate",
            "legal_hold_refusal",
            "redacted_diagnostic",
        ),
    ),
    TrackerRow(
        "MDF-005",
        "Repair/rebuild/salvage body execution",
        IMPLEMENTED,
        (
            "REPAIR-REBUILD-SALVAGE-DATABASE-EVIDENCE-BOUNDARY-IMPLEMENTED",
            "FILESPACE-REPAIR-REBUILD-SALVAGE-SBSQL-ROUTE-IMPLEMENTED",
        ),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-005-FINAL-DEFERRED-REPAIR-REBUILD-SALVAGE-BODIES",
                    "RepairBodyPlan",
                    "ExecuteRepairBodyPlan",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-REPAIR-BODY-COMMITTED",
                    "SB-REPAIR-BODY-MGA-AUTHORITY-REQUIRED",
                    "verified_body_rewrite",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-005",
                    "TestRepairRebuildSalvage",
                    "verified mirror body",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-005",
                    "REPAIR-REBUILD-SALVAGE-DATABASE-EVIDENCE-BOUNDARY-IMPLEMENTED",
                    "FILESPACE-REPAIR-REBUILD-SALVAGE-SBSQL-ROUTE-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "repair_body_rewrite",
            "salvage_quarantine",
            "mga_authority_refusal",
            "digest_verification",
        ),
    ),
    TrackerRow(
        "MDF-006",
        "Durable repair and sweep workflows",
        IMPLEMENTED,
        (
            "FILESPACE-ORPHAN-STALE-DISCOVERY-CLASSIFIER-IMPLEMENTED",
            "FILESPACE-DISCOVERY-PHYSICAL-CLEANUP-DELETE-IMPLEMENTED",
        ),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-006-FINAL-DEFERRED-DURABLE-REPAIR-SWEEP",
                    "DurableSweepRequest",
                    "ResumeDurableSweep",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-DURABLE-SWEEP-COMMITTED",
                    "SB-DURABLE-SWEEP-STATE-FIRST-REQUIRED",
                    "durable_state_before_delete",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-006",
                    "TestDurableSweep",
                    "SB-DURABLE-SWEEP-STATE-FIRST-REQUIRED",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-006",
                    "FILESPACE-ORPHAN-STALE-DISCOVERY-CLASSIFIER-IMPLEMENTED",
                    "FILESPACE-DISCOVERY-PHYSICAL-CLEANUP-DELETE-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "durable_state_before_delete",
            "resume_state",
            "legal_hold_refusal",
            "physical_delete",
        ),
    ),
    TrackerRow(
        "MDF-007",
        "Filespace package completion",
        IMPLEMENTED,
        (
            "FILESPACE-PACKAGE-STORAGE-WORKFLOW-IMPLEMENTED",
            "FILESPACE-PACKAGE-PHYSICAL-MEMBER-TRANSFER-IMPLEMENTED",
        ),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-007-FINAL-DEFERRED-FILESPACE-PACKAGE-COMPLETION",
                    "PackageTransferRequest",
                    "TransferEncryptedPackageMembers",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-FILESPACE-PACKAGE-TRANSFER-COMMITTED",
                    "SB-FILESPACE-PACKAGE-PARSER-RESTORE-AUTHORITY-REFUSED",
                    "physical_member_transfer",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-007",
                    "TestFilespacePackage",
                    "SB-FILESPACE-PACKAGE-PARSER-RESTORE-AUTHORITY-REFUSED",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-007",
                    "FILESPACE-PACKAGE-STORAGE-WORKFLOW-IMPLEMENTED",
                    "FILESPACE-PACKAGE-PHYSICAL-MEMBER-TRANSFER-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "package_member_transfer",
            "restore_stage",
            "parser_authority_refusal",
            "digest_verification",
        ),
    ),
    TrackerRow(
        "MDF-008",
        "Shard placement execution",
        IMPLEMENTED,
        (
            "SHARD-PLACEMENT-DESCRIPTOR-PLANNER-IMPLEMENTED",
            "SHARD-PLACEMENT-SBSQL-ROUTE-IMPLEMENTED",
        ),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-008-FINAL-DEFERRED-SHARD-PLACEMENT-EXECUTION",
                    "ShardPlacementRequest",
                    "ExecuteShardPlacementOperation",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-SHARD-PLACEMENT-COMMITTED",
                    "SB-SHARD-PLACEMENT-LOCAL-TRANSACTION-REQUIRED",
                    "SB-SHARD-PLACEMENT-STANDALONE-CLUSTER-SURROGATE-REFUSED",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-008",
                    "TestShardPlacement",
                    "rebalance",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-008",
                    "SHARD-PLACEMENT-DESCRIPTOR-PLANNER-IMPLEMENTED",
                    "SHARD-PLACEMENT-SBSQL-ROUTE-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "placement_state",
            "local_transaction_required",
            "cluster_surrogate_refusal",
            "all_operations",
        ),
    ),
    TrackerRow(
        "MDF-009",
        "Reserved page-family bodies",
        IMPLEMENTED,
        ("PAGE-REGISTRY-STATUS-MATRIX-IMPLEMENTED",),
        (
            SourceEvidence(
                "project/src/storage/page/reserved_page_family_body.hpp",
                (
                    "MDF-009-FINAL-DEFERRED-RESERVED-PAGE-FAMILY-BODIES",
                    "ReservedPageFamilyBodyResult",
                    "BuildReservedPageFamilyBody",
                ),
            ),
            SourceEvidence(
                "project/src/storage/page/reserved_page_family_body.cpp",
                (
                    "SB-RESERVED-PAGE-FAMILY-BODY-BUILT",
                    "SB-RESERVED-PAGE-FAMILY-CLUSTER-AUTHORITY-REQUIRED",
                    "product_support_overclaim_refused",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-009",
                    "TestReservedPageFamilyBodies",
                    "ValidatePageTypeProductSupportClaim",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-009",
                    "PAGE-REGISTRY-STATUS-MATRIX-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "body_codec",
            "backup_restore_repair",
            "cluster_authority_refusal",
            "protected_material_refusal",
            "product_support_overclaim_refusal",
        ),
    ),
    TrackerRow(
        "MDF-010",
        "Storage-tier execution",
        IMPLEMENTED,
        (
            "STORAGE-TIER-DESCRIPTOR-PLANNER-IMPLEMENTED",
            "STORAGE-TIER-BOUNDED-PHYSICAL-RELOCATION-IMPLEMENTED",
        ),
        (
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.hpp",
                (
                    "MDF-010-FINAL-DEFERRED-STORAGE-TIER-EXECUTION",
                    "StorageTierMigrationRequest",
                    "ExecuteStorageTierOperation",
                ),
            ),
            SourceEvidence(
                "project/src/storage/filespace/final_deferred_storage_workflows.cpp",
                (
                    "SB-STORAGE-TIER-COMMITTED",
                    "SB-STORAGE-TIER-ROLLBACK-COMMITTED",
                    "SB-STORAGE-TIER-STALE-CACHE-EPOCH",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/final_deferred_storage_workflows_gate.cpp",
                (
                    "MDF-010",
                    "TestStorageTierExecution",
                    "SB-STORAGE-TIER-STALE-CACHE-EPOCH",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_final_deferred_storage_workflows_gate",
                (
                    TRACKER_LABEL,
                    "MDF-010",
                    "STORAGE-TIER-DESCRIPTOR-PLANNER-IMPLEMENTED",
                    "STORAGE-TIER-BOUNDED-PHYSICAL-RELOCATION-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "stage_commit_rollback",
            "physical_byte_copy",
            "cache_invalidation",
            "stale_cache_refusal",
        ),
    ),
    TrackerRow(
        "MDF-011",
        "Memory subsystem integration",
        IMPLEMENTED,
        (
            "MEMORY-MANAGEMENT-DESCRIPTOR-PLANNER-IMPLEMENTED",
            "MEMORY-GOVERNANCE-DIRECT-EXECUTOR-IMPLEMENTED",
            "MEMORY-RATE-LIMIT-LIVE-EXECUTOR-IMPLEMENTED",
        ),
        (
            SourceEvidence(
                "project/src/engine/internal_api/management/memory_management_api.hpp",
                (
                    "parser_front_door_limit_surface_present",
                    "udr_limit_surface_present",
                    "EngineMemoryRateLimitDescriptor",
                    "EngineMemoryPolicyMigrationDescriptor",
                ),
            ),
            SourceEvidence(
                "project/src/engine/internal_api/management/memory_management_api.cpp",
                (
                    "memory_cache_control_executor",
                    "memory_rate_limit_live_executor",
                    "memory_derivative_state_migration_checkpoint",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/memory_management_descriptor_conformance.cpp",
                (
                    "TestRateLimitLiveExecutorSurfaces",
                    "memory_rate_limit_live_executor",
                    "memory_derivative_state_migration_persisted",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_memory_management_descriptor_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-011",
                    "MEMORY-MANAGEMENT-DESCRIPTOR-PLANNER-IMPLEMENTED",
                    "MEMORY-GOVERNANCE-DIRECT-EXECUTOR-IMPLEMENTED",
                    "MEMORY-RATE-LIMIT-LIVE-EXECUTOR-IMPLEMENTED",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "direct_executor",
            "durable_catalog",
            "rate_limit_live_executor",
            "policy_migration",
            "derivative_state_migration",
        ),
    ),
    TrackerRow(
        "MDF-012",
        "Datatype descriptor and catalog bodies",
        IMPLEMENTED,
        (
            "DEFER-DTYPE-DESCRIPTOR-IMPLEMENTATION",
            "DEFER-DTYPE-CATALOG-DDL",
            "DEFER-DTYPE-CLOSURE-MATRIX-TRACE",
        ),
        (
            SourceEvidence(
                "project/src/core/datatypes/datatype_catalog_manifest.hpp",
                (
                    "MDF-012-CURRENT-CORE-DATATYPE-CATALOG-MANIFEST",
                    "DatatypeCatalogDescriptorRow",
                    "DatatypeCatalogCache",
                ),
            ),
            SourceEvidence(
                "project/src/core/datatypes/datatype_catalog_manifest.cpp",
                (
                    "sys.datatype_descriptor",
                    "SB-DATATYPE-CATALOG-AUTHORITY-VIOLATION",
                    "SB-DATATYPE-CATALOG-TRACE-ROW-MISSING",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/current_core_datatype_catalog_manifest_gate.cpp",
                (
                    "MDF-012",
                    "DEFER-DTYPE-CATALOG-DDL",
                    "SB-DATATYPE-CATALOG-CACHE-INVALIDATED",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_current_core_datatype_catalog_manifest_gate",
                (
                    TRACKER_LABEL,
                    "MDF-012",
                    "MDF-012-CURRENT-CORE-DATATYPE-CATALOG-MANIFEST",
                    "DEFER-DTYPE-DESCRIPTOR-IMPLEMENTATION",
                    "DEFER-DTYPE-CATALOG-DDL",
                    "DEFER-DTYPE-CLOSURE-MATRIX-TRACE",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "descriptor_authority",
            "catalog_load",
            "trace_coverage",
            "uuid_stability",
            "cache_invalidation",
            "unsupported_refusal",
        ),
    ),
    TrackerRow(
        "MDF-013",
        "Datatype physical encoding",
        IMPLEMENTED,
        (
            "DEFER-DPE-ENCODER-DECODER",
            "DEFER-DPE-IN-PAGE-LAYOUT",
            "DEFER-DPE-OVERFLOW-LAYOUT",
        ),
        (
            SourceEvidence(
                "project/src/core/datatypes/datatype_physical_encoding.hpp",
                (
                    "MDF-013-CURRENT-CORE-DATATYPE-PHYSICAL-ENCODING",
                    "DatatypePhysicalValueState",
                    "protected_chunk_root",
                ),
            ),
            SourceEvidence(
                "project/src/core/datatypes/datatype_physical_encoding.cpp",
                (
                    "SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH",
                    "overflow_root",
                    "locator_handle",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/current_core_datatype_physical_encoding_gate.cpp",
                (
                    "MDF-013",
                    "DEFER-DPE-OVERFLOW-LAYOUT",
                    "TestRestartPersistenceRoundTrip",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_current_core_datatype_physical_encoding_gate",
                (
                    TRACKER_LABEL,
                    "MDF-013",
                    "MDF-013-CURRENT-CORE-DATATYPE-PHYSICAL-ENCODING",
                    "DEFER-DPE-ENCODER-DECODER",
                    "DEFER-DPE-IN-PAGE-LAYOUT",
                    "DEFER-DPE-OVERFLOW-LAYOUT",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "encoder_decoder",
            "null_value_separation",
            "overflow_chunk_integrity",
            "locator_opaque_protected_handling",
            "restart_persistence",
            "malformed_payload_refusal",
        ),
    ),
    TrackerRow(
        "MDF-014",
        "Datatype comparison and storage casts",
        IMPLEMENTED,
        ("DEFER-DPE-COMPARISON-KEYS", "DEFER-DPE-CAST-STORAGE"),
        (
            SourceEvidence(
                "project/src/core/datatypes/datatype_operations.cpp",
                (
                    "SB_DATATYPE_SORT_KEY_REJECTED",
                    "SB_DATATYPE_DESERIALIZATION_REJECTED",
                    "OrderedIntegerKey",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/current_core_datatype_comparison_cast_gate.cpp",
                (
                    "MDF-014",
                    "DEFER-DPE-COMPARISON-KEYS",
                    "accepted silent real128 downgrade",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_current_core_datatype_comparison_cast_gate",
                (
                    TRACKER_LABEL,
                    "MDF-014",
                    "MDF-014-CURRENT-CORE-DATATYPE-COMPARISON-CASTS",
                    "DEFER-DPE-COMPARISON-KEYS",
                    "DEFER-DPE-CAST-STORAGE",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "comparison_keys",
            "collation_resource_binding",
            "cast_persistence",
            "precision_loss_refusal",
            "silent_downgrade_refusal",
        ),
    ),
    TrackerRow(
        "MDF-015",
        "Datatype conformance manifests and examples",
        IMPLEMENTED,
        ("DEFER-DPE-EXAMPLE-CORPUS", "DEFER-DTYPE-CONFORMANCE-MANIFESTS"),
        (
            SourceEvidence(
                "project/src/core/datatypes/datatype_conformance_manifest.hpp",
                (
                    "MDF-015-CURRENT-CORE-DATATYPE-CONFORMANCE-MANIFEST",
                    "DatatypeConformanceManifest",
                    "DatatypeConformanceExampleSource",
                ),
            ),
            SourceEvidence(
                "project/src/core/datatypes/datatype_conformance_manifest.cpp",
                (
                    "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING",
                    "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED",
                    "SB-DATATYPE-CONFORMANCE-PARSER-AUTHORITY-REFUSED",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/current_core_datatype_conformance_manifest_gate.cpp",
                (
                    "MDF-015",
                    "DEFER-DPE-EXAMPLE-CORPUS",
                    "SB-DATATYPE-CONFORMANCE-ENCODED-EXAMPLE-REFUSED",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_current_core_datatype_conformance_manifest_gate",
                (
                    TRACKER_LABEL,
                    "MDF-015",
                    "MDF-015-CURRENT-CORE-DATATYPE-CONFORMANCE-MANIFEST",
                    "DEFER-DPE-EXAMPLE-CORPUS",
                    "DEFER-DTYPE-CONFORMANCE-MANIFESTS",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "manifest_inventory",
            "encoded_example_execution",
            "missing_row_refusal",
            "docs_private_evidence_refusal",
            "parser_authority_refusal",
        ),
    ),
    TrackerRow(
        "MDF-016",
        "Datatype metrics and redaction",
        IMPLEMENTED,
        ("DEFER-DTM-*",),
        (
            SourceEvidence(
                "project/src/core/datatypes/datatype_metrics_redaction.hpp",
                (
                    "MDF-016-CURRENT-CORE-DATATYPE-METRICS-REDACTION",
                    "DatatypeMetricsManagementRequest",
                    "protected_payload_sample",
                ),
            ),
            SourceEvidence(
                "project/src/core/datatypes/datatype_metrics_redaction.cpp",
                (
                    "SB-DATATYPE-METRICS-VISIBILITY-REFUSED",
                    "sb_datatype_physical_encoding_total",
                    "protected_payload",
                ),
            ),
            SourceEvidence(
                "project/src/core/metrics/metric_registry.cpp",
                (
                    "sb_datatype_chunk_event_total",
                    "sb_datatype_comparison_total",
                    "sys.metrics.datatypes.redaction",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/current_core_datatype_metrics_redaction_gate.cpp",
                (
                    "MDF-016",
                    "DEFER-DTM-*",
                    "RAW_PROTECTED_DATATYPE_PAYLOAD",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_current_core_datatype_metrics_redaction_gate",
                (
                    TRACKER_LABEL,
                    "MDF-016",
                    "MDF-016-CURRENT-CORE-DATATYPE-METRICS-REDACTION",
                    "DEFER-DTM-*",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "metric_emission",
            "management_surface",
            "support_bundle_redaction",
            "unauthorized_visibility_refusal",
            "protected_payload_leakage_refusal",
        ),
    ),
    TrackerRow(
        "MDF-017",
        "C++ UDR enforcement",
        IMPLEMENTED,
        ("DEFER-CXX-ONLY-UDR-ENFORCEMENT", "DEFER-DTM-UDR-BRIDGE-METRICS"),
        (
            SourceEvidence(
                "project/src/udr/runtime/sb_udr_runtime.cpp",
                (
                    "UDR.RUNTIME.NON_CPP_RUNTIME_FORBIDDEN",
                    "runtime_language",
                    "trusted_cpp",
                ),
            ),
            SourceEvidence(
                "project/src/engine/internal_api/extensibility/udr_api.cpp",
                (
                    "SB_ENGINE_API_UDR_NON_CPP_RUNTIME_FORBIDDEN",
                    "sb_udr_non_cpp_refusal_total",
                    "runtime_language",
                ),
            ),
            SourceEvidence(
                "project/src/core/metrics/metric_registry.cpp",
                ("sb_udr_non_cpp_refusal_total", "sys.metrics.udr"),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/udr_extension_conformance.cpp",
                (
                    "MDF-017",
                    "DEFER-CXX-ONLY-UDR-ENFORCEMENT",
                    "restart catalog reload",
                    "sb_udr_non_cpp_refusal_total",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_udr_extension_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-017",
                    "DEFER-CXX-ONLY-UDR-ENFORCEMENT",
                    "DEFER-DTM-UDR-BRIDGE-METRICS",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "runtime_dispatch",
            "metric_emission",
            "catalog_reload",
            "mga_safe",
        ),
    ),
    TrackerRow(
        "MDF-018",
        "Storage/page/filespace/device metrics",
        IMPLEMENTED,
        ("DEFER-SPM-*",),
        (
            SourceEvidence(
                "project/src/core/metrics/storage_metrics_management.hpp",
                (
                    "MDF-018-CURRENT-CORE-STORAGE-METRICS-MANAGEMENT",
                    "StorageMetricsManagementRequest",
                    "observed_metric_generation",
                ),
            ),
            SourceEvidence(
                "project/src/core/metrics/storage_metrics_management.cpp",
                (
                    "SB-STORAGE-METRICS-VISIBILITY-REFUSED",
                    "SB-STORAGE-METRICS-STALE-GENERATION-REFUSED",
                    "sb_storage_pressure_total",
                ),
            ),
            SourceEvidence(
                "project/src/core/metrics/metric_registry.cpp",
                (
                    "sb_temp_workspace_bytes",
                    "sb_index_build_workspace_bytes",
                    "sys.metrics.storage.redaction",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/current_core_storage_metrics_management_gate.cpp",
                (
                    "MDF-018",
                    "DEFER-SPM-*",
                    "RAW_STORAGE_PROTECTED_PAYLOAD",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_current_core_storage_metrics_management_gate",
                (
                    TRACKER_LABEL,
                    "MDF-018",
                    "MDF-018-CURRENT-CORE-STORAGE-METRICS-MANAGEMENT",
                    "DEFER-SPM-*",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "metric_emission",
            "management_surface",
            "support_bundle_redaction",
            "unauthorized_visibility_refusal",
            "stale_invalidation",
        ),
    ),
    TrackerRow(
        "MDF-019",
        "Logical integrity catalogs and enforcement",
        IMPLEMENTED,
        ("DEFER-CLI-CATALOGS", "DEFER-CLI-ENFORCEMENT"),
        (
            SourceEvidence(
                "project/src/engine/internal_api/dml/constraint_enforcement.hpp",
                (
                    "PRF_CONSTRAINT_DML_ENFORCEMENT",
                    "ValidateDeferredTransactionConstraints",
                    "ConstraintDmlValidationOptions",
                ),
            ),
            SourceEvidence(
                "project/src/engine/internal_api/dml/constraint_enforcement.cpp",
                (
                    "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION",
                    "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION",
                    "constraint_key_support",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/constraint_dml_enforcement_conformance.cpp",
                (
                    "CLI.CONSTRAINT_EXCLUSION_VIOLATION",
                    "CLI.SUPPORT_STRUCTURE_UNAVAILABLE",
                    "constraint_dml_enforcement_conformance=passed",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_constraint_dml_enforcement_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-019",
                    "DEFER-CLI-CATALOGS",
                    "DEFER-CLI-ENFORCEMENT",
                    "PRF_G06_CONSTRAINTS_READY",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "catalog_metadata",
            "constraint_enforcement",
            "support_structure",
            "canonical_diagnostics",
            "mga_visibility",
        ),
    ),
    TrackerRow(
        "MDF-020",
        "Logical integrity timing and lifecycle",
        IMPLEMENTED,
        (
            "DEFER-CLI-VALIDATION-STATES",
            "DEFER-CLI-DEFERRAL",
            "DEFER-CLI-MAINTENANCE",
            "DEFER-CLI-DURABILITY-RESIDENCY",
            "DEFER-CLI-MEMORY-OPTIMIZATION",
            "DEFER-CLI-METRICS",
            "DEFER-CLI-CONFORMANCE",
        ),
        (
            SourceEvidence(
                "project/src/engine/internal_api/dml/constraint_enforcement.cpp",
                (
                    "constraint_deferred_pending_check",
                    "ValidateDeferredTransactionConstraints",
                    "constraint_visibility_authority",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/constraint_dml_enforcement_conformance.cpp",
                (
                    "deferred duplicate key did not fail at commit",
                    "rolled-back key remained visible to constraint enforcement",
                    "rollback to MGA savepoint marker failed",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_constraint_dml_enforcement_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-020",
                    "DEFER-CLI-VALIDATION-STATES",
                    "DEFER-CLI-DEFERRAL",
                    "DEFER-CLI-MAINTENANCE",
                    "DEFER-CLI-DURABILITY-RESIDENCY",
                    "DEFER-CLI-MEMORY-OPTIMIZATION",
                    "DEFER-CLI-METRICS",
                    "DEFER-CLI-CONFORMANCE",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "deferred_validation",
            "commit_time_failure",
            "rollback_visibility",
            "savepoint_rollback",
            "transaction_lifecycle",
        ),
    ),
    TrackerRow(
        "MDF-021",
        "Index catalogs and access methods",
        IMPLEMENTED,
        (
            "DEFER-IDX-CATALOGS",
            "DEFER-IDX-BTREE",
            "DEFER-IDX-HASH",
            "DEFER-IDX-BITMAP-BRIN",
            "DEFER-IDX-DOCUMENT-SEARCH",
            "DEFER-IDX-SPATIAL",
            "DEFER-IDX-VECTOR",
            "DEFER-IDX-RANGE-GRAPH",
            "DEFER-IDX-COLUMNAR-SKETCH",
        ),
        (
            SourceEvidence(
                "project/src/core/index/index_family_registry.hpp",
                (
                    "SB-INDEX-FAMILY-REGISTRY-CLOSURE-ANCHOR",
                    "BuiltinIndexFamilyDescriptors",
                    "BuiltinIndexFamilyPhysicalCapabilityStates",
                ),
            ),
            SourceEvidence(
                "project/src/core/index/index_access_method.hpp",
                (
                    "SB-INDEX-ACCESS-METHOD-CLOSURE-ANCHOR",
                    "AdmitIndexProviderAccessMethod",
                    "IndexProviderAuthorityBoundary",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/index_write_profile_p3_conformance.cpp",
                (
                    "BuiltinIndexFamilyDescriptors",
                    "PlanIndexManagementOperation",
                    "TestIndexFamilyAndManagementMatrix",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_index_write_profile_p3_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-021",
                    "DEFER-IDX-CATALOGS",
                    "DEFER-IDX-BTREE",
                    "DEFER-IDX-HASH",
                    "DEFER-IDX-BITMAP-BRIN",
                    "DEFER-IDX-DOCUMENT-SEARCH",
                    "DEFER-IDX-SPATIAL",
                    "DEFER-IDX-VECTOR",
                    "DEFER-IDX-RANGE-GRAPH",
                    "DEFER-IDX-COLUMNAR-SKETCH",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "family_registry",
            "access_method_admission",
            "provider_authority_refusal",
            "catalog_profile",
            "runtime_capability",
        ),
    ),
    TrackerRow(
        "MDF-022",
        "Index physical layouts",
        IMPLEMENTED,
        (
            "DEFER-IPL-PAGE-LAYOUTS",
            "DEFER-IPL-ORDERED-KEYS",
            "DEFER-IPL-BTREE-HASH",
            "DEFER-IPL-DOC-SEARCH-SPATIAL-VECTOR",
        ),
        (
            SourceEvidence(
                "project/src/core/index/index_key_encoding.hpp",
                (
                    "SB-INDEX-KEY-ENCODING-CLOSURE-ANCHOR",
                    "EncodeIndexKey",
                    "BuildEncodedPrefixUpperBound",
                ),
            ),
            SourceEvidence(
                "project/src/core/index/index_metapage.hpp",
                (
                    "SB-INDEX-METAPAGE-CLOSURE-ANCHOR",
                    "IndexMetapageControl",
                    "BuildIndexMetapageControl",
                ),
            ),
            SourceEvidence(
                "project/src/core/index/index_ordered_access.hpp",
                (
                    "SB-INDEX-ORDERED-ACCESS-CLOSURE-ANCHOR",
                    "use_btree_physical",
                    "supports_prefix_seek",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_index_write_profile_p3_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-022",
                    "DEFER-IPL-PAGE-LAYOUTS",
                    "DEFER-IPL-ORDERED-KEYS",
                    "DEFER-IPL-BTREE-HASH",
                    "DEFER-IPL-DOC-SEARCH-SPATIAL-VECTOR",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "metapage_layout",
            "ordered_key_encoding",
            "btree_physical_access",
            "digest_validation",
            "unsupported_refusal",
        ),
    ),
    TrackerRow(
        "MDF-023",
        "Index statistics, optimizer, and maintenance",
        IMPLEMENTED,
        (
            "DEFER-ISO-STATS-OPTIMIZER",
            "DEFER-IMO-MAINTENANCE",
            "DEFER-IDX-MAINTENANCE",
            "DEFER-IDX-METRICS",
        ),
        (
            SourceEvidence(
                "project/src/core/index/index_statistics_lifecycle.hpp",
                (
                    "SB-INDEX-STATISTICS-LIFECYCLE-ANCHOR",
                    "RefreshIndexStatistics",
                    "optimizer_plan_cache_invalidation_required",
                ),
            ),
            SourceEvidence(
                "project/src/core/index/index_management.hpp",
                (
                    "SB-INDEX-MANAGEMENT-CLOSURE-ANCHOR",
                    "PlanIndexManagementOperation",
                    "PlanIndexManagementValidationRepairOperation",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/index_statistics_plan_conformance.cpp",
                (
                    "catalog_profile_supports_mga_snapshot_visibility",
                    "optimizer_plan_cache_invalidation_required",
                    "TestCrashRecoveryClassification",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_index_statistics_plan_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-023",
                    "DEFER-ISO-STATS-OPTIMIZER",
                    "DEFER-IMO-MAINTENANCE",
                    "DEFER-IDX-MAINTENANCE",
                    "DEFER-IDX-METRICS",
                    "mga_transaction_regression",
                ),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "dpc_index_validation_repair_tooling_gate",
                (
                    TRACKER_LABEL,
                    "MDF-023",
                    "DEFER-IMO-MAINTENANCE",
                    "DEFER-IDX-MAINTENANCE",
                    "DEFER-IDX-METRICS",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "statistics_lifecycle",
            "optimizer_invalidation",
            "management_surface",
            "validation_repair",
            "recovery_classification",
        ),
    ),
    TrackerRow(
        "MDF-024",
        "Index MGA and transaction mechanics",
        IMPLEMENTED,
        (
            "DEFER-IDX-MGA-EXPANSION",
            "DEFER-IMT-INDEX-MGA",
            "DEFER-IMT-TRANSACTION-EXPANSION",
            "DEFER-ISC-TRANSACTION-CLOSURE",
        ),
        (
            SourceEvidence(
                "project/src/core/index/index_transaction.hpp",
                (
                    "SB-INDEX-TRANSACTION-CLOSURE-ANCHOR",
                    "local_prepare_supported",
                    "validation_complete",
                ),
            ),
            SourceEvidence(
                "project/src/core/index/secondary_index_delta_ledger.hpp",
                (
                    "dpc_secondary_index_delta_ledger",
                    "repair_rebuild_required",
                    "SecondaryIndexDeltaLedgerRecoveryClass",
                ),
            ),
            SourceEvidence(
                "project/src/core/index/unique_index_deferral_policy.hpp",
                (
                    "IndexUniquenessClass",
                    "reservation_protocol_present",
                    "reservation_protocol_proven",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/dpc_secondary_index_delta_recovery_repair_gate.cpp",
                (
                    "has_uncommitted_precommit_delta",
                    "retained under MGA authority",
                    "rolled-back MGA inventory did not prune precommit delta",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "dpc_unique_index_deferral_policy_gate",
                (
                    TRACKER_LABEL,
                    "MDF-024",
                    "DEFER-IDX-MGA-EXPANSION",
                    "DEFER-IMT-INDEX-MGA",
                    "DEFER-IMT-TRANSACTION-EXPANSION",
                    "DEFER-ISC-TRANSACTION-CLOSURE",
                    "mga_transaction_regression",
                ),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "dpc_secondary_index_delta_recovery_repair_gate",
                (
                    TRACKER_LABEL,
                    "MDF-024",
                    "DEFER-IDX-MGA-EXPANSION",
                    "DEFER-IMT-INDEX-MGA",
                    "DEFER-IMT-TRANSACTION-EXPANSION",
                    "DEFER-ISC-TRANSACTION-CLOSURE",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "unique_deferral_policy",
            "mga_delta_ledger",
            "rollback_cleanup",
            "recovery_repair",
            "authority_refusal",
        ),
    ),
    TrackerRow(
        "MDF-025",
        "Index support surfaces",
        IMPLEMENTED,
        (
            "DEFER-IFM-FAMILY-ROWS",
            "DEFER-ICD-CATALOGS",
            "DEFER-ICD-DIAGNOSTICS",
            "DEFER-ICD-DRIVER-METADATA",
            "DEFER-ICD-EXPLAIN-SUPPORT",
            "DEFER-ICI-MANIFESTS",
            "DEFER-IDX-CONFORMANCE",
            "DEFER-IPL-IFM-DIO-CONFORMANCE",
        ),
        (
            SourceEvidence(
                "project/src/core/index/index_family_management_surface.hpp",
                (
                    "SB-INDEX-FAMILY-MANAGEMENT-SURFACE-CLOSURE-ANCHOR",
                    "IndexFamilyManagementSurface",
                    "IndexFamilySupportBundleRow",
                ),
            ),
            SourceEvidence(
                "project/src/core/index/index_api_sblr.hpp",
                (
                    "SB-INDEX-API-SBLR-CLOSURE-ANCHOR",
                    "explain",
                    "repair_index_family",
                ),
            ),
            SourceEvidence(
                "project/src/core/index/index_validation_repair_tooling.hpp",
                (
                    "IndexValidationSupportEvidence",
                    "allow_sensitive_support_data",
                    "non_physical_refused",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/catalog_index_profile_conformance.cpp",
                (
                    "ValidateBuiltinCatalogIndexProfiles",
                    "TestNoHumanNameDuplicationInBaseCatalog",
                    "cluster catalog path did not fail closed",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_catalog_index_profile_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-025",
                    "DEFER-IFM-FAMILY-ROWS",
                    "DEFER-ICD-CATALOGS",
                    "DEFER-ICD-DIAGNOSTICS",
                    "DEFER-ICD-DRIVER-METADATA",
                    "DEFER-ICD-EXPLAIN-SUPPORT",
                    "DEFER-ICI-MANIFESTS",
                    "DEFER-IDX-CONFORMANCE",
                    "DEFER-IPL-IFM-DIO-CONFORMANCE",
                    "mga_transaction_regression",
                ),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "dpc_index_validation_repair_tooling_gate",
                (
                    TRACKER_LABEL,
                    "MDF-025",
                    "DEFER-ICD-DIAGNOSTICS",
                    "DEFER-IDX-CONFORMANCE",
                    "DEFER-IPL-IFM-DIO-CONFORMANCE",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "management_catalog",
            "support_bundle_redaction",
            "sblr_surface",
            "driver_metadata",
            "conformance_manifest",
        ),
    ),
    TrackerRow(
        "MDF-026",
        "Current-core reference-neutral compatibility metadata",
        IMPLEMENTED,
        (
            "DEFER-DTYPE-REFERENCE-GATE-IMPLEMENTATION",
            "DEFER-DPE-REFERENCE-MAPPING",
            "DEFER-DTM-REFERENCE-METRICS",
        ),
        (
            SourceEvidence(
                "project/src/core/datatypes/reference_compatibility_metadata.hpp",
                (
                    "MDF-026-CURRENT-CORE-REFERENCE-NEUTRAL-METADATA",
                    "ReferenceCompatibilityMetadataRequest",
                    "parser_claims_authority",
                ),
            ),
            SourceEvidence(
                "project/src/core/datatypes/reference_compatibility_metadata.cpp",
                (
                    "SB-REFERENCE-COMPAT-UNSUPPORTED-BY-VERSION",
                    "sys.metrics.datatypes.reference.fallback_refusal_count",
                    "ResolveReferenceTypeLabelPlaceholder",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/current_core_reference_neutral_metadata_conformance.cpp",
                (
                    "MDF-026",
                    "SB-REFERENCE-COMPAT-PARSER-AUTHORITY-REFUSED",
                    "sys.metrics.datatypes.reference.runtime_mapping_count",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_current_core_reference_neutral_metadata_conformance",
                (
                    TRACKER_LABEL,
                    "MDF-026",
                    "DEFER-DTYPE-REFERENCE-GATE-IMPLEMENTATION",
                    "DEFER-DPE-REFERENCE-MAPPING",
                    "DEFER-DTM-REFERENCE-METRICS",
                    "mga_transaction_regression",
                ),
            ),
        ),
        (
            "project_source",
            "project_test",
            "ctest_label",
            "negative_self_check",
            "runtime_mapping",
            "metric_emission",
            "unsupported_refusal",
            "parser_authority_refusal",
        ),
    ),
)

LABEL_RE = re.compile(
    r"set_tests_properties\(\s*(?:\"|\[=\[)?(?P<name>[A-Za-z0-9_]+)"
    r"(?:\"|\]=\])?\s+PROPERTIES\s+.*?LABELS\s+\"(?P<labels>[^\"]+)\"",
    re.S,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument(
        "--mode",
        choices=("inventory", "release"),
        default="inventory",
        help=(
            "inventory validates the tracker and implemented rows; release "
            "fails until all in-scope MDF rows are implemented"
        ),
    )
    return parser.parse_args()


def read_text(path: Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        errors.append(f"missing required input: {path}")
    except OSError as exc:
        errors.append(f"failed to read {path}: {exc}")
    return ""


def parse_labels(path: Path, errors: list[str]) -> dict[str, set[str]]:
    labels: dict[str, set[str]] = {}
    text = read_text(path, errors)
    for match in LABEL_RE.finditer(text):
        labels[match.group("name")] = set(match.group("labels").split(";"))
    return labels


def merge_labels(paths: tuple[Path, ...], errors: list[str]) -> dict[str, set[str]]:
    merged: dict[str, set[str]] = {}
    for path in paths:
        merged.update(parse_labels(path, errors))
    return merged


def validate_project_evidence_path(relative_path: str, errors: list[str]) -> None:
    rel = Path(relative_path)
    if rel.is_absolute():
        errors.append(f"absolute evidence path is forbidden: {relative_path}")
        return
    if ("ScratchBird" + "-Private") in rel.parts:
        errors.append(f"private tracking path cannot be evidence: {relative_path}")
    if rel.parts[:1] != ("project",):
        errors.append(f"evidence path must be under project/: {relative_path}")
    if rel.parts[:2] == ("project", "docs") or rel.parts[:1] == ("docs",):
        errors.append(f"documentation cannot be implementation evidence: {relative_path}")


def check_project_path(repo_root: Path, relative_path: str,
                       errors: list[str]) -> Path:
    validate_project_evidence_path(relative_path, errors)
    return repo_root / relative_path


def require_tokens(repo_root: Path, evidence: SourceEvidence,
                   errors: list[str]) -> None:
    path = check_project_path(repo_root, evidence.path, errors)
    text = read_text(path, errors)
    for token in evidence.tokens:
        if token not in text:
            errors.append(f"{evidence.path} missing evidence token: {token}")


def require_labels(labels_by_test: dict[str, set[str]], evidence: TestEvidence,
                   context: str, errors: list[str]) -> None:
    validate_project_evidence_path(evidence.cmake_path, errors)
    labels = labels_by_test.get(evidence.test_name)
    if labels is None:
        errors.append(f"{context} missing registered test: {evidence.test_name}")
        return
    for label in evidence.labels:
        if label not in labels:
            errors.append(f"{context} {evidence.test_name} missing label: {label}")


def validate_row_inventory(rows: tuple[TrackerRow, ...]) -> list[str]:
    errors: list[str] = []
    row_ids = [row.row_id for row in rows]
    expected = [f"MDF-{index:03d}" for index in range(27)]
    if row_ids != expected:
        errors.append(f"MDF row inventory drift: expected {expected}, found {row_ids}")
    if len(set(row_ids)) != len(row_ids):
        errors.append("duplicate MDF row ids in tracker gate")
    for row in rows:
        if row.status not in {OPEN, IMPLEMENTED}:
            errors.append(f"{row.row_id} has invalid status: {row.status}")
        if not row.source_markers:
            errors.append(f"{row.row_id} has no source markers")
        if row.status == IMPLEMENTED:
            if not row.evidence:
                errors.append(f"{row.row_id} implemented without source evidence")
            if not row.tests:
                errors.append(f"{row.row_id} implemented without project test evidence")
            missing = REQUIRED_IMPLEMENTED_CLASSES - set(row.closure_classes)
            if missing:
                errors.append(
                    f"{row.row_id} implemented without closure classes: {sorted(missing)}"
                )
            if set(row.closure_classes).issubset(DISALLOWED_ONLY_CLASSES):
                errors.append(
                    f"{row.row_id} closed by route-only/descriptor-only/refusal-only proof"
                )
        elif row.evidence or row.tests:
            errors.append(f"{row.row_id} is open but already has closure evidence")
    return errors


def release_gate_errors(rows: tuple[TrackerRow, ...]) -> list[str]:
    errors = validate_row_inventory(rows)
    for row in rows:
        if row.status != IMPLEMENTED:
            errors.append(f"{row.row_id} remains open: {row.area}")
    return errors


def run_negative_self_checks() -> list[str]:
    errors: list[str] = []
    for bad_path in (
        "/tmp/not-project-evidence.cpp",
        "docs/migration/final-deferred-implementation-tracker.md",
        "project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md",
        ("ScratchBird" + "-Private/docs/migration/final-deferred-implementation-tracker.md"),
    ):
        path_errors: list[str] = []
        validate_project_evidence_path(bad_path, path_errors)
        if not path_errors:
            errors.append(
                "docs_private_evidence_refused self-check failed: "
                f"{bad_path} was accepted"
            )

    bad_row = TrackerRow(
        "MDF-999",
        "Synthetic route-only closure",
        IMPLEMENTED,
        ("synthetic",),
        closure_classes=("route_only",),
    )
    bad_errors = validate_row_inventory((bad_row,))
    if not any("route-only/descriptor-only/refusal-only" in err for err in bad_errors):
        errors.append("route_only_descriptor_only_refused self-check failed")

    synthetic_release_rows = tuple(
        TrackerRow(
            row.row_id,
            row.area,
            OPEN if row.row_id == "MDF-001" else row.status,
            row.source_markers,
            () if row.row_id == "MDF-001" else row.evidence,
            () if row.row_id == "MDF-001" else row.tests,
            () if row.row_id == "MDF-001" else row.closure_classes,
        )
        for row in ROWS
    )
    release_errors = release_gate_errors(synthetic_release_rows)
    if not any(error.startswith("MDF-001 remains open") for error in release_errors):
        errors.append("release mode did not fail on open MDF rows")
    return errors


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve()
    errors: list[str] = []

    errors.extend(validate_row_inventory(ROWS))
    errors.extend(run_negative_self_checks())

    source_labels = merge_labels((repo_root / DATABASE_CMAKE,), errors)
    build_labels = merge_labels(
        (build_root / "tests/database_lifecycle/CTestTestfile.cmake",),
        errors,
    )

    for row in ROWS:
        if row.status != IMPLEMENTED:
            continue
        for evidence in row.evidence:
            require_tokens(repo_root, evidence, errors)
        for test in row.tests:
            require_labels(source_labels, test, "source CMake", errors)
            require_labels(build_labels, test, "build CTest", errors)

    if args.mode == "release":
        errors.extend(release_gate_errors(ROWS))

    if errors:
        for error in errors:
            print(f"final_deferred_implementation_tracker_gate=fail:{error}",
                  file=sys.stderr)
        return 1

    open_rows = sum(1 for row in ROWS if row.status != IMPLEMENTED)
    implemented_rows = len(ROWS) - open_rows
    print(
        "final_deferred_implementation_tracker_gate=passed "
        f"mode={args.mode} rows={len(ROWS)} open={open_rows} "
        f"implemented={implemented_rows} exclusions={len(EXCLUDED_KEY_FAMILIES)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
