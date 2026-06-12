#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate that migration-deferred project issues have project evidence."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import sys


DEFER_KEYS = (
    "DEFER-CLI-CATALOGS",
    "DEFER-CLI-CONFORMANCE",
    "DEFER-CLI-CONSTRAINTS-KEYS-LOGICAL-INTEGRITY",
    "DEFER-CLI-DEFERRAL",
    "DEFER-CLI-REFERENCE-MAPPING",
    "DEFER-CLI-DURABILITY-RESIDENCY",
    "DEFER-CLI-ENFORCEMENT",
    "DEFER-CLI-MAINTENANCE",
    "DEFER-CLI-MEMORY-OPTIMIZATION",
    "DEFER-CLI-METRICS",
    "DEFER-CLI-PRE-INDEX-AUTHORITY",
    "DEFER-CLI-SUPPORT-STRUCTURES",
    "DEFER-CLI-VALIDATION-STATES",
    "DEFER-CXX-ONLY-UDR-COMPAT",
    "DEFER-CXX-ONLY-UDR-ENFORCEMENT",
    "DEFER-DB2-UDT",
    "DEFER-DB2-VECTOR",
    "DEFER-DIO-REFERENCE-INDEX-OPTIMIZATION",
    "DEFER-DIO-REFERENCE-OPTIMIZATION",
    "DEFER-REFERENCE-METHOD-BINDINGS",
    "DEFER-DPE-CAST-STORAGE",
    "DEFER-DPE-COMPARISON-KEYS",
    "DEFER-DPE-REFERENCE-MAPPING",
    "DEFER-DPE-ENCODER-DECODER",
    "DEFER-DPE-EXAMPLE-CORPUS",
    "DEFER-DPE-IMPLEMENTATION-READY",
    "DEFER-DPE-IN-PAGE-LAYOUT",
    "DEFER-DPE-OVERFLOW-LAYOUT",
    "DEFER-DPE-PHYSICAL-ENCODING",
    "DEFER-DTM-CAST-METRICS",
    "DEFER-DTM-CHUNK-METRICS",
    "DEFER-DTM-COMPARISON-METRICS",
    "DEFER-DTM-DATATYPE-METRICS",
    "DEFER-DTM-DESCRIPTOR-METRICS",
    "DEFER-DTM-REFERENCE-METRICS",
    "DEFER-DTM-ENCODING-METRICS",
    "DEFER-DTM-INDEX-PREREQUISITE-METRICS",
    "DEFER-DTM-LOCATOR-OPAQUE-METRICS",
    "DEFER-DTM-MANAGEMENT-SURFACES",
    "DEFER-DTM-REDACTION",
    "DEFER-DTM-SYS-VIEWS",
    "DEFER-DTM-TYPED-FILESPACE-METRICS",
    "DEFER-DTM-UDR-BRIDGE-METRICS",
    "DEFER-DTYPE-CATALOG-DDL",
    "DEFER-DTYPE-CLOSURE-MATRIX-TRACE",
    "DEFER-DTYPE-COMMERCIAL-CLOSURE",
    "DEFER-DTYPE-CONFORMANCE",
    "DEFER-DTYPE-CONFORMANCE-MANIFESTS",
    "DEFER-DTYPE-DESCRIPTOR-IMPLEMENTATION",
    "DEFER-DTYPE-DOC-CLEANUP",
    "DEFER-DTYPE-REFERENCE-GATE-IMPLEMENTATION",
    "DEFER-DTYPE-METHOD-BINDING-IMPLEMENTATION",
    "DEFER-DTYPE-TRACEABILITY",
    "DEFER-ICD-CATALOGS",
    "DEFER-ICD-DIAGNOSTICS",
    "DEFER-ICD-DRIVER-METADATA",
    "DEFER-ICD-EXPLAIN-SUPPORT",
    "DEFER-ICD-INDEX-CATALOG-DIAGNOSTICS-DRIVER",
    "DEFER-ICI-INDEX-CONFORMANCE",
    "DEFER-ICI-MANIFESTS",
    "DEFER-IDX-BITMAP-BRIN",
    "DEFER-IDX-BTREE",
    "DEFER-IDX-CATALOGS",
    "DEFER-IDX-COLUMNAR-SKETCH",
    "DEFER-IDX-CONFORMANCE",
    "DEFER-IDX-DOCUMENT-SEARCH",
    "DEFER-IDX-REFERENCE-COMPATIBILITY",
    "DEFER-IDX-REFERENCE-PROFILES",
    "DEFER-IDX-HASH",
    "DEFER-IDX-INDEXES-ACCESS-METHODS",
    "DEFER-IDX-MAINTENANCE",
    "DEFER-IDX-MEMORY",
    "DEFER-IDX-METRICS",
    "DEFER-IDX-MGA-EXPANSION",
    "DEFER-IDX-MGA-INTERACTION",
    "DEFER-IDX-ORDER-COMPATIBILITY",
    "DEFER-IDX-RANGE-GRAPH",
    "DEFER-IDX-SPATIAL",
    "DEFER-IDX-VECTOR",
    "DEFER-IFM-FAMILY-ROWS",
    "DEFER-IFM-INDEX-FAMILY-MATRIX",
    "DEFER-IMO-INDEX-MAINTENANCE",
    "DEFER-IMO-MAINTENANCE",
    "DEFER-IMT-INDEX-MGA",
    "DEFER-IMT-TRANSACTION-EXPANSION",
    "DEFER-IPL-BTREE-HASH",
    "DEFER-IPL-DOC-SEARCH-SPATIAL-VECTOR",
    "DEFER-IPL-IFM-DIO-CONFORMANCE",
    "DEFER-IPL-INDEX-PHYSICAL-LAYOUTS",
    "DEFER-IPL-ORDERED-KEYS",
    "DEFER-IPL-PAGE-LAYOUTS",
    "DEFER-ISC-INDEX-CLOSURE",
    "DEFER-ISC-TRANSACTION-CLOSURE",
    "DEFER-ISO-IMO-IMT-CONFORMANCE",
    "DEFER-ISO-INDEX-STATISTICS-OPTIMIZER",
    "DEFER-ISO-STATS-OPTIMIZER",
    "DEFER-MSSQL-CLR-UDT",
    "DEFER-MSSQL-HIERARCHYID",
    "DEFER-MSSQL-JSON",
    "DEFER-MSSQL-ORACLE-DB2-COMPAT-TYPES",
    "DEFER-MSSQL-VECTOR",
    "DEFER-ORA-ANY",
    "DEFER-ORA-OBJECT-REF",
    "DEFER-ORA-URI-MEDIA",
    "DEFER-ORA-VECTOR",
    "DEFER-SPM-ARCHIVE-IMPORT",
    "DEFER-SPM-BACKUP-RESTORE",
    "DEFER-SPM-COMPACTION-TRUNCATION",
    "DEFER-SPM-DEVICE-CAPACITY",
    "DEFER-SPM-DEVICE-METRICS",
    "DEFER-SPM-FILESPACE-SIZE",
    "DEFER-SPM-FILESPACE-USAGE",
    "DEFER-SPM-FRAGMENTATION",
    "DEFER-SPM-INDEX-PREREQUISITE-STORAGE-METRICS",
    "DEFER-SPM-IO-PERFORMANCE",
    "DEFER-SPM-PAGE-FAMILY",
    "DEFER-SPM-PAGE-HEALTH",
    "DEFER-SPM-PREALLOCATION",
    "DEFER-SPM-REDACTION",
    "DEFER-SPM-STORAGE-PAGE-FILESPACE-METRICS",
    "DEFER-SPM-STORAGE-PRESSURE",
    "DEFER-SPM-SYS-VIEWS",
    "DEFER-SPM-TEMP-WORK",
)


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
class EvidenceGroup:
    name: str
    prefixes: tuple[str, ...]
    source: tuple[SourceEvidence, ...]
    tests: tuple[TestEvidence, ...]


DATABASE_CMAKE = "project/tests/database_lifecycle/CMakeLists.txt"
SBSQL_CMAKE = "project/tests/sbsql_parser_worker/CMakeLists.txt"


GROUPS = (
    EvidenceGroup(
        "datatype",
        (
            "DEFER-CXX-",
            "DEFER-DB2-",
            "DEFER-REFERENCE-",
            "DEFER-DPE-",
            "DEFER-DTM-",
            "DEFER-DTYPE-",
            "DEFER-MSSQL-",
            "DEFER-ORA-",
        ),
        (
            SourceEvidence(
                "project/include/scratchbird/engine/value.hpp",
                (
                    "CanonicalDatatypeFamilyRegistry",
                    "TypeOperationRegistry",
                    "TypeMetadataDiagnosticsDriverContract",
                    "sys.metrics.datatypes.scalar",
                    "ReferenceTypeMappingDescriptor",
                ),
            ),
            SourceEvidence(
                "project/src/core/datatypes/datatype_runtime_closure.cpp",
                ("RuntimeNumericType", "DatatypeSortKeyResult", "DatatypeHashResult"),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/canonical_datatype_families_conformance.cpp",
                (
                    "sys.metrics.datatypes.vector.dimension_mismatches_total",
                    "transport_recovery_authority_violation",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/type_operation_registry_conformance.cpp",
                (
                    "non_cpp_udr_forbidden",
                    "reference_method_binding_invalid",
                    "sblr_binding_recheck_required",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/type_metadata_diagnostics_driver_conformance.cpp",
                (
                    "TypeMetadataDiagnosticsDriverContract",
                    "driver metadata",
                    "backup/restore profile",
                    "replication profile",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/execution_reference_type_capability_conformance.cpp",
                (
                    "ReferenceTypeMappingDescriptor",
                    "accepted C++ UDR capability",
                    "LLVM acceleration",
                    "opaque reference mapping",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/datatype_numeric_p3_conformance.cpp",
                (
                    "NumericRequest",
                    "driver metadata int128",
                    "RecordDatatypeCast",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_canonical_datatype_families_conformance",
                (
                    "CDT-CANONICAL-DATATYPE-FAMILY-IMPLEMENTATION",
                    "datatype_commercial_closure_gate",
                ),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_type_operation_registry_conformance",
                ("TOR-TYPE-OPERATION-REGISTRY", "type_operation_registry"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_type_metadata_diagnostics_driver_conformance",
                (
                    "TMD-TYPE-METADATA-DIAGNOSTICS-DRIVER",
                    "type_metadata_diagnostics_driver",
                ),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_execution_reference_type_capability_conformance",
                ("EDR-GATE-037", "execution_reference_type_capability"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_datatype_numeric_p3_conformance",
                ("numeric_128_backend_gate", "datatype_commercial_closure_gate"),
            ),
        ),
    ),
    EvidenceGroup(
        "storage_metrics",
        ("DEFER-SPM-",),
        (
            SourceEvidence(
                "project/src/core/metrics/metric_registry.cpp",
                (
                    "sys.metrics.storage.disk",
                    "sys.metrics.storage.filespaces",
                    "sys.metrics.storage.pages",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/storage_page_filespace_p2_conformance.cpp",
                ("metrics_emitted", "durable_state_changed"),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/filespace_preallocation_runtime_gate.cpp",
                ("preallocation_evidence", "durable_state_changed", "metrics_emitted"),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/storage_tier_migration_conformance.cpp",
                ("storage tier descriptor plan", "private provider", "durable_state_changed"),
            ),
            SourceEvidence(
                "project/src/engine/internal_api/observability/performance_optimization_surface.cpp",
                ("page_preallocation_demand_pages", "filespace_preallocation_granted_pages"),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_storage_page_filespace_p2_conformance",
                ("storage_metrics_gate", "filespace_lifecycle_gate"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "filespace_preallocation_runtime_gate",
                ("PFAR-009", "filespace_preallocation_runtime_gate"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_storage_tier_migration_conformance",
                ("storage_tier_migration", "database_lifecycle_storage"),
            ),
        ),
    ),
    EvidenceGroup(
        "cli_constraints_catalog_memory",
        ("DEFER-CLI-",),
        (
            SourceEvidence(
                "project/src/engine/internal_api/dml/constraint_enforcement.cpp",
                (
                    "validation_state",
                    "CLI.SUPPORT_STRUCTURE_UNAVAILABLE",
                    "constraint_key_support",
                    "deferrable",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/constraint_dml_enforcement_conformance.cpp",
                (
                    "foreign_key",
                    "deferrable=true",
                    "CLI.CONSTRAINT_CHECK_VIOLATION",
                    "constraint_deferred_pending_check",
                ),
            ),
            SourceEvidence(
                "project/tests/sbsql_parser_worker/sbsql_column_constraint_exact_route_conformance.cpp",
                (
                    "SBLR_DDL_CONSTRAINT_CREATE",
                    "authority.engine.catalog_constraint_descriptor_required",
                    "authority.parser.no_sql_text_execution",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/catalog_object_conformance.cpp",
                (
                    "constraint descriptor was not persisted in catalog lifecycle",
                    "metadata_cache_epoch",
                    "kCatalogObjectDiagnosticCacheEpochStale",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/memory_management_descriptor_conformance.cpp",
                (
                    "sys.memory_report_catalog",
                    "memory_report_catalog_persisted",
                    "metrics_contract_present",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_constraint_dml_enforcement_conformance",
                (
                    "constraint_dml_enforcement_gate",
                    "constraint_index_dependency_gate",
                    "PRF_G06_CONSTRAINTS_READY",
                ),
            ),
            TestEvidence(
                SBSQL_CMAKE,
                "sbsql_column_constraint_exact_route_conformance",
                ("sbsql_column_constraint_exact_route_conformance", "SBSFC-030"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_catalog_object_conformance",
                (
                    "constraint_catalog_descriptor_gate",
                    "catalog_generation_mga_gate",
                ),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_memory_management_descriptor_conformance",
                (
                    "MEMORY-MANAGEMENT-DESCRIPTOR-PLANNER-IMPLEMENTED",
                    "memory_governance",
                ),
            ),
        ),
    ),
    EvidenceGroup(
        "indexes",
        (
            "DEFER-DIO-",
            "DEFER-ICD-",
            "DEFER-ICI-",
            "DEFER-IDX-",
            "DEFER-IFM-",
            "DEFER-IMO-",
            "DEFER-IMT-",
            "DEFER-IPL-",
            "DEFER-ISC-",
            "DEFER-ISO-",
        ),
        (
            SourceEvidence(
                "project/src/core/index/index_family_registry.cpp",
                ("BuiltinIndexFamilyDescriptors", "CompleteCapability", "vector_hnsw"),
            ),
            SourceEvidence(
                "project/src/core/index/index_metrics.cpp",
                (
                    "sys.metrics.indexes",
                    "index_operation_support_bundle",
                    "sb_index_reference_compatibility_diagnostics_total",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/index_write_profile_p3_conformance.cpp",
                ("TestIndexFamilyAndManagementMatrix", "vector_hnsw", "TestIndexMetrics"),
            ),
            SourceEvidence(
                "project/tests/sbsql_parser_worker/sbsql_index_family_runtime_closure_conformance.cpp",
                (
                    "physically_complete",
                    "index_family=btree",
                    "index_family=bitmap",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/index_statistics_plan_conformance.cpp",
                (
                    "statistics refresh was not MGA transaction visible",
                    "optimizer_plan_cache_invalidation_required",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/catalog_index_profile_conformance.cpp",
                (
                    "ValidateBuiltinCatalogIndexProfiles",
                    "CatalogIndexProfileHasOrderedNeed",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_index_write_profile_p3_conformance",
                ("index_family_matrix_gate", "index_metrics_gate"),
            ),
            TestEvidence(
                SBSQL_CMAKE,
                "sbsql_index_family_runtime_closure_conformance",
                ("CBQ_GATE_INDEX_FAMILY_COMPLETE", "index_family_runtime_closure"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_index_statistics_plan_conformance",
                ("database_lifecycle_index_statistics_plan", "mga_transaction_regression"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_catalog_index_profile_conformance",
                ("catalog_physical_index_profile_gate", "database_lifecycle_catalog"),
            ),
        ),
    ),
)


MIGRATION_TEST = TestEvidence(
    DATABASE_CMAKE,
    "migration_deferred_operations_closure_gate",
    (
        "MIGRATION-DEFERRED-OPERATIONS-CLOSURE",
        "DEFERRED-MIGRATION-IMPLEMENTATION-CLOSURE",
        "TEMP-TABLE-GATE-017",
    ),
)

TEMP_TABLE_TEST = TestEvidence(
    SBSQL_CMAKE,
    "sbsql_temporary_table_proof_closure_conformance",
    (
        "SBSQL-TEMPORARY-TABLE-PROOF-CLOSURE",
        "TEMP-TABLE-GATE-015",
        "TEMP-TABLE-GATE-016",
        "TEMP-TABLE-GATE-017",
    ),
)


MIGRATION_FAMILY_GROUPS = (
    EvidenceGroup(
        "execution_data_representation",
        (),
        (
            SourceEvidence(
                "project/include/scratchbird/engine/value.hpp",
                (
                    "ExecutionResultEnvelope",
                    "ExecutionRoutineSignatureDescriptor",
                    "ReferenceTypeMappingDescriptor",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/execution_data_gate_matrix_conformance.py",
                ("EDR-GATE-001", "EDR-GATE-037", "generated CTest missing labels"),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_execution_data_gate_matrix_conformance",
                ("EDR-GATE-014", "execution_data_gate_matrix"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_execution_reference_type_capability_conformance",
                ("EDR-GATE-037", "execution_reference_type_capability"),
            ),
        ),
    ),
    EvidenceGroup(
        "reference_type_coverage",
        (),
        (
            SourceEvidence(
                "project/include/scratchbird/engine/value.hpp",
                (
                    "DTC-REFERENCE-TYPE-COVERAGE",
                    "ReferenceTypeMappingDescriptor",
                    "ExecutionTypeCapabilityDescriptor",
                    "reference_superiority_matrix_required",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/execution_reference_type_capability_conformance.cpp",
                (
                    "accepted opaque reference mapping",
                    "accepted C++ UDR capability",
                    "accepted LLVM acceleration",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/reference_mapping_static.py",
                (
                    "DBLC_STATIC_NO_REFERENCE_ENGINE_SQL",
                    "reference_engine_sql_executed",
                    "parser_executes_sql",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_execution_reference_type_capability_conformance",
                ("DTC-REFERENCE-TYPE-COVERAGE", "DTC-GATE-001", "DTC-GATE-012"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_reference_mapping_static",
                ("DBLC_STATIC_NO_REFERENCE_ENGINE_SQL", "database_lifecycle_reference_mapping"),
            ),
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_reference_mapping_conformance",
                ("DBLC_P14_REFERENCE_MAPPING_COMPLETE", "database_lifecycle_reference_mapping"),
            ),
        ),
    ),
    EvidenceGroup(
        "memory_management",
        (),
        (
            SourceEvidence(
                "project/src/engine/internal_api/management/memory_management_api.cpp",
                (
                    "EnginePlanMemoryManagementOperation",
                    "RunBackgroundMemoryReclamation",
                    "sys.memory_report_catalog",
                    "memory_rate_limit_catalog_persisted",
                ),
            ),
            SourceEvidence(
                "project/src/core/memory/memory_policy_config.hpp",
                ("MemoryPolicyConfig", "policy_generation", "hard_limit_bytes"),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/memory_management_descriptor_conformance.cpp",
                (
                    "memory_report_catalog_persisted",
                    "EngineMemoryRateLimitClass::integrity_or_corruption_signal",
                    "memory_derivative_state_migration_checkpoint",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_memory_management_descriptor_conformance",
                (
                    "MM-MEMORY-MANAGEMENT-CORRECTIVE-CLOSURE",
                    "MM-GATE-001",
                    "MM-GATE-024",
                ),
            ),
        ),
    ),
    EvidenceGroup(
        "commercial_grade_completion",
        (),
        (
            SourceEvidence(
                "project/include/scratchbird/engine/value.hpp",
                (
                    "CommercialGradeCompletionEvidenceRegistry",
                    "CGC-GATE-001",
                    "CGC-GATE-025",
                ),
            ),
            SourceEvidence(
                "project/tests/database_lifecycle/commercial_grade_completion_evidence_conformance.cpp",
                (
                    "commercial_grade_completion_evidence",
                    "CGC-COMMERCIAL-GRADE-COMPLETION-GATES",
                    "CGC-GATE-001..CGC-GATE-025",
                ),
            ),
        ),
        (
            TestEvidence(
                DATABASE_CMAKE,
                "database_lifecycle_commercial_grade_completion_evidence_conformance",
                (
                    "CGC-COMMERCIAL-GRADE-COMPLETION-GATES",
                    "CGC-GATE-001",
                    "CGC-GATE-025",
                ),
            ),
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


def check_project_path(repo_root: Path, relative_path: str, errors: list[str]) -> Path:
    rel = Path(relative_path)
    if rel.is_absolute():
        errors.append(f"absolute evidence path is forbidden: {relative_path}")
    if rel.parts[:1] != ("project",):
        errors.append(f"evidence path must remain under project/: {relative_path}")
    if len(rel.parts) > 1 and rel.parts[1] == "docs":
        errors.append(f"public project docs cannot be migration evidence: {relative_path}")
    if rel.parts[:1] == ("docs",):
        errors.append(f"public docs cannot be migration evidence: {relative_path}")
    if ("ScratchBird" + "-Private") in rel.parts:
        errors.append(f"private tracking path cannot be test input: {relative_path}")
    return repo_root / rel


def require_tokens(repo_root: Path, evidence: SourceEvidence,
                   errors: list[str]) -> None:
    path = check_project_path(repo_root, evidence.path, errors)
    text = read_text(path, errors)
    for token in evidence.tokens:
        if token not in text:
            errors.append(f"{evidence.path} missing evidence token: {token}")


def require_labels(labels_by_test: dict[str, set[str]], evidence: TestEvidence,
                   context: str, errors: list[str]) -> None:
    labels = labels_by_test.get(evidence.test_name)
    if labels is None:
        errors.append(f"{context} missing registered test: {evidence.test_name}")
        return
    for label in evidence.labels:
        if label not in labels:
            errors.append(
                f"{context} {evidence.test_name} missing label: {label}"
            )


def assign_defer_keys(errors: list[str]) -> dict[str, list[str]]:
    by_group = {group.name: [] for group in GROUPS}
    seen: set[str] = set()
    for key in DEFER_KEYS:
        if key in seen:
            errors.append(f"duplicate deferred key: {key}")
        seen.add(key)
        matches = [
            group.name
            for group in GROUPS
            if any(key.startswith(prefix) for prefix in group.prefixes)
        ]
        if len(matches) != 1:
            errors.append(f"{key} matched {len(matches)} evidence groups: {matches}")
            continue
        by_group[matches[0]].append(key)
    for group_name, keys in by_group.items():
        if not keys:
            errors.append(f"evidence group has no deferred keys: {group_name}")
    return by_group


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve()
    errors: list[str] = []

    if len(set(DEFER_KEYS)) != len(DEFER_KEYS):
        errors.append("deferred key universe contains duplicates")

    assigned = assign_defer_keys(errors)
    source_cmake_labels = merge_labels(
        (
            repo_root / DATABASE_CMAKE,
            repo_root / SBSQL_CMAKE,
        ),
        errors,
    )
    build_ctest_labels = merge_labels(
        (
            build_root / "tests/database_lifecycle/CTestTestfile.cmake",
            build_root / "tests/sbsql_parser_worker/CTestTestfile.cmake",
        ),
        errors,
    )

    for group in GROUPS:
        for evidence in group.source:
            require_tokens(repo_root, evidence, errors)
        for evidence in group.tests:
            require_labels(source_cmake_labels, evidence, "source CMake", errors)
            require_labels(build_ctest_labels, evidence, "build CTest", errors)

    for group in MIGRATION_FAMILY_GROUPS:
        for evidence in group.source:
            require_tokens(repo_root, evidence, errors)
        for evidence in group.tests:
            require_labels(source_cmake_labels, evidence, "source CMake", errors)
            require_labels(build_ctest_labels, evidence, "build CTest", errors)

    require_labels(source_cmake_labels, MIGRATION_TEST, "source CMake", errors)
    require_labels(build_ctest_labels, MIGRATION_TEST, "build CTest", errors)
    require_labels(source_cmake_labels, TEMP_TABLE_TEST, "source CMake", errors)
    require_labels(build_ctest_labels, TEMP_TABLE_TEST, "build CTest", errors)
    require_tokens(
        repo_root,
        SourceEvidence(
            "project/tests/sbsql_parser_worker/sbsql_temporary_table_proof_closure_conformance.cpp",
            ("TEMP-TABLE-GATE-017", "temporary_on_commit_deleted_rows"),
        ),
        errors,
    )

    total_keys = sum(len(keys) for keys in assigned.values())
    if total_keys != len(DEFER_KEYS):
        errors.append(
            f"deferred key assignment count drift: {total_keys} != {len(DEFER_KEYS)}"
        )

    if errors:
        for error in errors:
            print(f"migration_deferred_operations_closure_gate=fail:{error}",
                  file=sys.stderr)
        return 1

    print(
        "migration_deferred_operations_closure_gate=passed "
        f"deferred_keys={len(DEFER_KEYS)} groups={len(GROUPS)} "
        f"families={len(MIGRATION_FAMILY_GROUPS)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
