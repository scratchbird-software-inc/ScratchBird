#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate the CEIC-030 index readiness manifest.

SEARCH_KEY: CEIC_030_INDEX_READINESS_MANIFEST_TOOL
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import uuid
from dataclasses import dataclass
from typing import Any, Iterable


EXECUTION_PLAN = pathlib.Path("docs" "/completed-execution-plans/consolidated-enterprise-proof-implementation-closure")
DEFAULT_MANIFEST = EXECUTION_PLAN / "artifacts/CEIC-030_INDEX_READINESS_MANIFEST.yaml"

REGISTRY_HPP = pathlib.Path("project/src/core/index/index_family_registry.hpp")
REGISTRY_CPP = pathlib.Path("project/src/core/index/index_family_registry.cpp")
ROUTE_HPP = pathlib.Path("project/src/core/index/index_route_capability.hpp")
ROUTE_CPP = pathlib.Path("project/src/core/index/index_route_capability.cpp")
CLASSIFICATION_HPP = pathlib.Path("project/src/core/index/index_family_route_classification.hpp")
CLASSIFICATION_CPP = pathlib.Path("project/src/core/index/index_family_route_classification.cpp")
BENCHMARK_GATE = pathlib.Path("project/src/core/index/index_family_benchmark_gate.cpp")
RECHECK_HPP = pathlib.Path("project/src/core/index/index_recheck.hpp")
RECHECK_CPP = pathlib.Path("project/src/core/index/index_recheck.cpp")
TRANSACTION_HPP = pathlib.Path("project/src/core/index/index_transaction.hpp")
INDEX_ACCESS_METHOD_HPP = pathlib.Path("project/src/core/index/index_access_method.hpp")
INDEX_ACCESS_METHOD_CPP = pathlib.Path("project/src/core/index/index_access_method.cpp")
INDEX_MGA_RECOVERY_HPP = pathlib.Path("project/src/core/index/index_mga_recovery_contract.hpp")
INDEX_MGA_RECOVERY_CPP = pathlib.Path("project/src/core/index/index_mga_recovery_contract.cpp")
INDEX_FAULT_MATRIX_HPP = pathlib.Path("project/src/core/index/index_fault_injection_matrix.hpp")
INDEX_FAULT_MATRIX_CPP = pathlib.Path("project/src/core/index/index_fault_injection_matrix.cpp")
INDEX_METRICS_HPP = pathlib.Path("project/src/core/index/index_metrics.hpp")
INDEX_METRICS_CPP = pathlib.Path("project/src/core/index/index_metrics.cpp")
BTREE_UNIQUE_CLOSURE_HPP = pathlib.Path(
    "project/src/core/index/btree_unique_durable_provider_closure.hpp"
)
BTREE_UNIQUE_CLOSURE_CPP = pathlib.Path(
    "project/src/core/index/btree_unique_durable_provider_closure.cpp"
)
CEIC_034_UNIQUE_PROTOCOL_HPP = pathlib.Path(
    "project/src/core/index/ceic_034_unique_reservation_finality_protocol.hpp"
)
CEIC_034_UNIQUE_PROTOCOL_CPP = pathlib.Path(
    "project/src/core/index/ceic_034_unique_reservation_finality_protocol.cpp"
)
HASH_DURABLE_CLOSURE_HPP = pathlib.Path(
    "project/src/core/index/hash_durable_provider_closure.hpp"
)
HASH_DURABLE_CLOSURE_CPP = pathlib.Path(
    "project/src/core/index/hash_durable_provider_closure.cpp"
)
SPECIALIZED_CLOSURE_HPP = pathlib.Path(
    "project/src/core/index/specialized_persistent_provider_closure.hpp"
)
SPECIALIZED_CLOSURE_CPP = pathlib.Path(
    "project/src/core/index/specialized_persistent_provider_closure.cpp"
)
CEIC_036_CANONICAL_KEY_FUZZ_HPP = pathlib.Path(
    "project/src/core/index/ceic_036_canonical_key_ordering_fuzz.hpp"
)
CEIC_036_CANONICAL_KEY_FUZZ_CPP = pathlib.Path(
    "project/src/core/index/ceic_036_canonical_key_ordering_fuzz.cpp"
)
TEST_CMAKE = pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt")
GATE_TEST = pathlib.Path("project/tests/consolidated_enterprise/ceic_030_index_readiness_manifest_gate_test.py")
CEIC_031_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_031_index_access_method_provider_gate.cpp"
)
CEIC_032_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_032_index_mga_recovery_contract_gate.cpp"
)
CEIC_033_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_033_btree_unique_durable_provider_closure_gate.cpp"
)
CEIC_034_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_034_unique_reservation_finality_protocol_gate.cpp"
)
CEIC_035_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_035_hash_durable_provider_closure_gate.cpp"
)
CEIC_036_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_036_canonical_key_ordering_encoding_fuzz_gate.cpp"
)
CEIC_037_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_037_engine_owned_exact_recheck_gate.cpp"
)
CEIC_038_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_038_lossy_pruning_family_classification_gate.cpp"
)
CEIC_039_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_039_specialized_persistent_provider_closure_gate.cpp"
)
CEIC_040_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_040_index_operation_metrics_support_bundle_gate.cpp"
)
CEIC_041_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate.cpp"
)
CEIC_042_GATE_TEST = pathlib.Path(
    "project/tests/consolidated_enterprise/ceic_042_index_readiness_manifest_drift_gate_test.py"
)
EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-030_INDEX_READINESS_MANIFEST_EVIDENCE.md"
CEIC_031_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-031_INDEX_ACCESS_METHOD_PROVIDER_EVIDENCE.md"
CEIC_032_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-032_INDEX_MGA_RECOVERY_CONTRACT_EVIDENCE.md"
CEIC_033_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-033_BTREE_UNIQUE_DURABLE_PROVIDER_CLOSURE_EVIDENCE.md"
CEIC_034_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-034_UNIQUE_RESERVATION_FINALITY_PROTOCOL_EVIDENCE.md"
CEIC_035_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-035_HASH_DURABLE_PROVIDER_CLOSURE_EVIDENCE.md"
CEIC_036_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-036_CANONICAL_KEY_ORDERING_ENCODING_FUZZ_EVIDENCE.md"
CEIC_037_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-037_ENGINE_OWNED_EXACT_RECHECK_EVIDENCE.md"
CEIC_038_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-038_LOSSY_PRUNING_FAMILY_CLASSIFICATION_EVIDENCE.md"
CEIC_039_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-039_SPECIALIZED_PERSISTENT_PROVIDER_CLOSURE_EVIDENCE.md"
CEIC_040_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE_EVIDENCE.md"
CEIC_041_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-041_INDEX_CRASH_CONCURRENCY_CLEANUP_CORRUPTION_MATRIX_EVIDENCE.md"
CEIC_042_EVIDENCE_MD = EXECUTION_PLAN / "artifacts/CEIC-042_INDEX_READINESS_DRIFT_GATE_EVIDENCE.md"

INDEX_FUTURE_SLICES = tuple(f"CEIC-{value:03d}" for value in range(31, 43))
INTEGRATED_INDEX_BOUNDARY_SLICES = tuple(f"CEIC-{value:03d}" for value in range(90, 96))
PERSISTENT_PROOF_SLICES = ("CEIC-031", "CEIC-032", "CEIC-041")
SECURITY_RECHECK_SLICES = ("CEIC-037",)
METRIC_PROOF_SLICES = ("CEIC-040",)
DRIFT_GATE_SLICES = ("CEIC-042",)

COMPLETE_STATUS = {"complete", "completed", "done", "closed", "complete_move_ready"}
PRESENT_STATUS = {"present", "complete", "completed", "generated"}
PENDING_STATUS = {"pending", "planned"}

AUTHORITY_BOUNDARY_TOKEN = (
    "index_readiness_manifest_is_generated_evidence_only_not_row_truth_result_"
    "finality_transaction_finality_visibility_authorization_security_recovery_"
    "parser_reference_wal_benchmark_optimizer_plan_index_finality_local_cluster_"
    "cluster_action_or_agent_action_authority"
)
CEIC_042_SCOPE_PENDING = "future_ci_drift_gate_not_completed_by_CEIC_030"
CEIC_042_SCOPE_COMPLETE = (
    "ci_readiness_drift_gate_complete_for_index_manifest_only_no_all_index_"
    "enterprise_cluster_or_integrated_readiness_claim"
)

ROUTE_KIND_ORDER = (
    "dml_insert",
    "dml_update",
    "dml_delete",
    "sql_select",
    "bulk_build",
    "nosql_document",
    "nosql_graph",
    "nosql_vector",
    "nosql_search",
    "maintenance",
    "validate_repair",
)

CONTROL_INPUTS = (
    EXECUTION_PLAN / "TRACKER.csv",
    EXECUTION_PLAN / "ARTIFACT_INDEX.csv",
    EXECUTION_PLAN / "AUDIT_TRACEABILITY_MATRIX.csv",
    EXECUTION_PLAN / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
    EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv",
    EXECUTION_PLAN / "DEPENDENCIES.csv",
    EXECUTION_PLAN / "ACCEPTANCE_GATES.csv",
    EXECUTION_PLAN / "CLAIM_BOUNDARY_MATRIX.csv",
    REGISTRY_HPP,
    REGISTRY_CPP,
    ROUTE_HPP,
    ROUTE_CPP,
    CLASSIFICATION_HPP,
    CLASSIFICATION_CPP,
    BENCHMARK_GATE,
    RECHECK_HPP,
    RECHECK_CPP,
    TRANSACTION_HPP,
    INDEX_ACCESS_METHOD_HPP,
    INDEX_ACCESS_METHOD_CPP,
    INDEX_MGA_RECOVERY_HPP,
    INDEX_MGA_RECOVERY_CPP,
    INDEX_FAULT_MATRIX_HPP,
    INDEX_FAULT_MATRIX_CPP,
    INDEX_METRICS_HPP,
    INDEX_METRICS_CPP,
    BTREE_UNIQUE_CLOSURE_HPP,
    BTREE_UNIQUE_CLOSURE_CPP,
    CEIC_034_UNIQUE_PROTOCOL_HPP,
    CEIC_034_UNIQUE_PROTOCOL_CPP,
    HASH_DURABLE_CLOSURE_HPP,
    HASH_DURABLE_CLOSURE_CPP,
    SPECIALIZED_CLOSURE_HPP,
    SPECIALIZED_CLOSURE_CPP,
    CEIC_036_CANONICAL_KEY_FUZZ_HPP,
    CEIC_036_CANONICAL_KEY_FUZZ_CPP,
    TEST_CMAKE,
    GATE_TEST,
    CEIC_031_GATE_TEST,
    CEIC_032_GATE_TEST,
    CEIC_033_GATE_TEST,
    CEIC_034_GATE_TEST,
    CEIC_035_GATE_TEST,
    CEIC_036_GATE_TEST,
    CEIC_037_GATE_TEST,
    CEIC_038_GATE_TEST,
    CEIC_039_GATE_TEST,
    CEIC_040_GATE_TEST,
    CEIC_041_GATE_TEST,
    CEIC_042_GATE_TEST,
    EVIDENCE_MD,
    CEIC_031_EVIDENCE_MD,
    CEIC_032_EVIDENCE_MD,
    CEIC_033_EVIDENCE_MD,
    CEIC_034_EVIDENCE_MD,
    CEIC_035_EVIDENCE_MD,
    CEIC_036_EVIDENCE_MD,
    CEIC_037_EVIDENCE_MD,
    CEIC_038_EVIDENCE_MD,
    CEIC_039_EVIDENCE_MD,
    CEIC_040_EVIDENCE_MD,
    CEIC_041_EVIDENCE_MD,
    CEIC_042_EVIDENCE_MD,
)

INDEX_PROOF_INPUTS = (
    (
        "CEIC-031",
        "CEIC-ART-046",
        CEIC_031_EVIDENCE_MD,
        CEIC_031_GATE_TEST,
        "ceic_031_index_access_method_provider_gate",
        (
            (INDEX_ACCESS_METHOD_HPP, "CEIC_031_INDEX_ACCESS_METHOD_PROVIDER_INTERFACE"),
            (INDEX_ACCESS_METHOD_CPP, "CEIC_031_INDEX_ACCESS_METHOD_PROVIDER_INTERFACE"),
        ),
        "ceic-031-index-access-method-provider-evidence",
    ),
    (
        "CEIC-032",
        "CEIC-ART-047",
        CEIC_032_EVIDENCE_MD,
        CEIC_032_GATE_TEST,
        "ceic_032_index_mga_recovery_contract_gate",
        (
            (INDEX_MGA_RECOVERY_HPP, "CEIC_032_INDEX_MGA_RECOVERY_CONTRACT"),
            (INDEX_MGA_RECOVERY_CPP, "CEIC_032_INDEX_MGA_RECOVERY_CONTRACT"),
        ),
        "ceic-032-index-mga-recovery-contract-evidence",
    ),
    (
        "CEIC-033",
        "CEIC-ART-048",
        CEIC_033_EVIDENCE_MD,
        CEIC_033_GATE_TEST,
        "ceic_033_btree_unique_durable_provider_closure_gate",
        (
            (BTREE_UNIQUE_CLOSURE_HPP, "CEIC_033_BTREE_UNIQUE_DURABLE_PROVIDER_CLOSURE"),
            (BTREE_UNIQUE_CLOSURE_CPP, "CEIC_033_BTREE_UNIQUE_DURABLE_PROVIDER_CLOSURE"),
        ),
        "ceic-033-btree-unique-durable-provider-closure-evidence",
    ),
    (
        "CEIC-034",
        "CEIC-ART-049",
        CEIC_034_EVIDENCE_MD,
        CEIC_034_GATE_TEST,
        "ceic_034_unique_reservation_finality_protocol_gate",
        (
            (CEIC_034_UNIQUE_PROTOCOL_HPP, "CEIC_034_UNIQUE_RESERVATION_FINALITY_PROTOCOL"),
            (CEIC_034_UNIQUE_PROTOCOL_CPP, "CEIC_034_UNIQUE_RESERVATION_FINALITY_PROTOCOL"),
        ),
        "ceic-034-unique-reservation-finality-protocol-evidence",
    ),
    (
        "CEIC-035",
        "CEIC-ART-050",
        CEIC_035_EVIDENCE_MD,
        CEIC_035_GATE_TEST,
        "ceic_035_hash_durable_provider_closure_gate",
        (
            (HASH_DURABLE_CLOSURE_HPP, "CEIC_035_HASH_DURABLE_PROVIDER_CLOSURE"),
            (HASH_DURABLE_CLOSURE_CPP, "CEIC_035_HASH_DURABLE_PROVIDER_CLOSURE"),
        ),
        "ceic-035-hash-durable-provider-closure-evidence",
    ),
    (
        "CEIC-036",
        "CEIC-ART-051",
        CEIC_036_EVIDENCE_MD,
        CEIC_036_GATE_TEST,
        "ceic_036_canonical_key_ordering_encoding_fuzz_gate",
        (
            (CEIC_036_CANONICAL_KEY_FUZZ_HPP, "CEIC-036 canonical key ordering"),
            (CEIC_036_CANONICAL_KEY_FUZZ_CPP, "CEIC_036_CANONICAL_KEY_ORDERING_ENCODING_FUZZ_GATES"),
        ),
        "CEIC_036_CANONICAL_KEY_ORDERING_ENCODING_FUZZ_GATES",
    ),
    (
        "CEIC-037",
        "CEIC-ART-052",
        CEIC_037_EVIDENCE_MD,
        CEIC_037_GATE_TEST,
        "ceic_037_engine_owned_exact_recheck_gate",
        (
            (RECHECK_HPP, "SB-INDEX-RECHECK-CLOSURE-ANCHOR"),
            (RECHECK_CPP, "CEIC_037_ENGINE_OWNED_EXACT_RECHECK_SERVICES"),
        ),
        "CEIC_037_ENGINE_OWNED_EXACT_RECHECK_SERVICES",
    ),
    (
        "CEIC-038",
        "CEIC-ART-053",
        CEIC_038_EVIDENCE_MD,
        CEIC_038_GATE_TEST,
        "ceic_038_lossy_pruning_family_classification_gate",
        (
            (CLASSIFICATION_HPP, "CEIC_038_LOSSY_PRUNING_FAMILY_CLASSIFICATION"),
            (CLASSIFICATION_CPP, "CEIC_038_LOSSY_PRUNING_FAMILY_CLASSIFICATION"),
        ),
        "ceic-038-lossy-candidate-and-pruning-family-classification-evidence",
    ),
    (
        "CEIC-039",
        "CEIC-ART-054",
        CEIC_039_EVIDENCE_MD,
        CEIC_039_GATE_TEST,
        "ceic_039_specialized_persistent_provider_closure_gate",
        (
            (SPECIALIZED_CLOSURE_HPP, "CEIC_039_SPECIALIZED_PERSISTENT_PROVIDER_CLOSURE"),
            (SPECIALIZED_CLOSURE_CPP, "CEIC_039_SPECIALIZED_PERSISTENT_PROVIDER_CLOSURE"),
        ),
        "ceic-039-specialized-persistent-provider-closure-evidence",
    ),
    (
        "CEIC-040",
        "CEIC-ART-055",
        CEIC_040_EVIDENCE_MD,
        CEIC_040_GATE_TEST,
        "ceic_040_index_operation_metrics_support_bundle_gate",
        (
            (INDEX_METRICS_HPP, "CEIC_040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE"),
            (INDEX_METRICS_CPP, "CEIC_040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE"),
        ),
        "ceic-040-index-operation-metrics-support-bundle-evidence",
    ),
    (
        "CEIC-041",
        "CEIC-ART-056",
        CEIC_041_EVIDENCE_MD,
        CEIC_041_GATE_TEST,
        "ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate",
        (
            (INDEX_FAULT_MATRIX_HPP, "scenario_class"),
            (INDEX_FAULT_MATRIX_CPP, "CEIC-041"),
        ),
        "ceic-041-index-crash-concurrency-cleanup-corruption-matrix-evidence",
    ),
)


class ManifestError(Exception):
    pass


@dataclass(frozen=True)
class FamilyDescriptor:
    enum_name: str
    family_id: str
    canonical_name: str
    persistence_class: str
    key_model: str
    completion_status: str
    native_physical_family: str
    default_semantic_profile: str
    baseline: bool
    persistent: bool
    requires_mga_recheck: bool
    supports_ordering: bool
    supports_uniqueness: bool
    approximate: bool
    metrics_prefix: str
    diagnostics_prefix: str
    packet_path: str


@dataclass(frozen=True)
class CapabilityState:
    enum_name: str
    declared_capability: bool
    planner_contract_capability: bool
    implemented: bool
    physical_reader: bool
    physical_writer: bool
    maintenance: bool
    validate: bool
    repair: bool
    recovery_reopen: bool
    rebuild: bool
    runtime_available: bool
    benchmark_clean: bool
    blocker: str
    blocker_diagnostic_code: str
    blocker_message_key: str
    blocker_detail: str

    def physically_complete(self) -> bool:
        return (
            self.implemented
            and self.physical_reader
            and self.physical_writer
            and self.maintenance
            and self.validate
            and self.repair
            and self.recovery_reopen
            and self.rebuild
        )

    def family_complete(self) -> bool:
        return (
            self.runtime_available
            and self.benchmark_clean
            and self.blocker == "none"
            and self.physically_complete()
        )


def normalize_status(value: str) -> str:
    return value.strip().lower().replace(" ", "_").replace("-", "_")


def read_text(repo_root: pathlib.Path, rel: pathlib.Path) -> str:
    path = repo_root / rel
    if not path.exists():
        raise ManifestError(f"missing required file: {rel}")
    return path.read_text(encoding="utf-8")


def read_csv(repo_root: pathlib.Path, rel: pathlib.Path) -> list[dict[str, str]]:
    path = repo_root / rel
    if not path.exists():
        raise ManifestError(f"missing required CSV: {rel}")
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    return [{key: value or "" for key, value in row.items()} for row in rows]


def index_by(rows: Iterable[dict[str, str]], key: str, label: str) -> dict[str, dict[str, str]]:
    indexed: dict[str, dict[str, str]] = {}
    duplicates: set[str] = set()
    for row in rows:
        value = row.get(key, "").strip()
        if not value:
            raise ManifestError(f"{label} contains blank {key}")
        if value in indexed:
            duplicates.add(value)
        indexed[value] = row
    if duplicates:
        raise ManifestError(f"{label} duplicate {key}: {', '.join(sorted(duplicates))}")
    return indexed


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def render_json(data: dict[str, Any]) -> str:
    return json.dumps(data, indent=2, ensure_ascii=True) + "\n"


def git_value(repo_root: pathlib.Path, args: list[str], fallback: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        return fallback
    return result.stdout.strip() or fallback


def parse_bool(value: str | None, default: bool = False) -> bool:
    if value is None:
        return default
    if value == "true":
        return True
    if value == "false":
        return False
    raise ManifestError(f"unexpected C++ bool literal: {value}")


def parse_enum_order(header: str) -> list[str]:
    match = re.search(r"enum class IndexFamily\s*:[^{]+{(?P<body>.*?)};", header, re.S)
    if not match:
        raise ManifestError("IndexFamily enum not found")
    names: list[str] = []
    for raw in match.group("body").split(","):
        name = raw.strip()
        if not name:
            continue
        name = name.split("=", 1)[0].strip()
        if name and name != "unknown":
            names.append(name)
    if not names:
        raise ManifestError("IndexFamily enum has no canonical families")
    return names


def parse_route_kinds(header: str) -> list[str]:
    match = re.search(r"enum class IndexRouteKind\s*:[^{]+{(?P<body>.*?)};", header, re.S)
    if not match:
        raise ManifestError("IndexRouteKind enum not found")
    names = []
    for raw in match.group("body").split(","):
        name = raw.strip()
        if not name:
            continue
        name = name.split("=", 1)[0].strip()
        if name and name != "unknown":
            names.append(name)
    if tuple(names) != ROUTE_KIND_ORDER:
        raise ManifestError("IndexRouteKind order drifted; update CEIC-030 manifest route extraction")
    return names


def parse_descriptors(source: str, enum_order: list[str]) -> dict[str, FamilyDescriptor]:
    descriptors: dict[str, FamilyDescriptor] = {}
    descriptor_re = re.compile(
        r'D\(IndexFamily::(?P<family>\w+),\s*"(?P<id>[^"]+)",\s*'
        r"IndexPersistenceClass::(?P<persistence>\w+),\s*"
        r"IndexKeyModel::(?P<key_model>\w+),\s*"
        r'"(?P<native>[^"]+)",\s*"(?P<profile>[^"]+)",\s*'
        r"(?P<baseline>true|false),\s*(?P<ordering>true|false),\s*"
        r"(?P<unique>true|false)(?:,\s*(?P<approx>true|false))?\)"
    )
    policy_re = re.compile(
        r'P\(IndexFamily::(?P<family>\w+),\s*"(?P<id>[^"]+)",\s*"(?P<policy>[^"]+)"\)'
    )
    for match in descriptor_re.finditer(source):
        family = match.group("family")
        persistence = match.group("persistence")
        family_id = match.group("id")
        descriptors[family] = FamilyDescriptor(
            enum_name=family,
            family_id=family_id,
            canonical_name=family_id,
            persistence_class=persistence,
            key_model=match.group("key_model"),
            completion_status="accepted_requires_full_implementation",
            native_physical_family=match.group("native"),
            default_semantic_profile=match.group("profile"),
            baseline=parse_bool(match.group("baseline")),
            persistent=persistence == "persistent",
            requires_mga_recheck=True,
            supports_ordering=parse_bool(match.group("ordering")),
            supports_uniqueness=parse_bool(match.group("unique")),
            approximate=parse_bool(match.group("approx"), default=False),
            metrics_prefix=f"sys.metrics.index.{family_id}",
            diagnostics_prefix=f"INDEX.{family_id}",
            packet_path=f"index_family_implementation_packet:{family_id}",
        )
    for match in policy_re.finditer(source):
        family = match.group("family")
        family_id = match.group("id")
        descriptors[family] = FamilyDescriptor(
            enum_name=family,
            family_id=family_id,
            canonical_name=family_id,
            persistence_class="policy_blocked",
            key_model="reference_defined",
            completion_status="policy_blocked_alpha",
            native_physical_family="policy_blocked",
            default_semantic_profile=match.group("policy"),
            baseline=False,
            persistent=False,
            requires_mga_recheck=True,
            supports_ordering=False,
            supports_uniqueness=False,
            approximate=True,
            metrics_prefix=f"sys.metrics.index.policy_blocked.{family_id}",
            diagnostics_prefix=f"INDEX.POLICY_BLOCKED.{family_id}",
            packet_path=f"index_family_implementation_packet:{family_id}",
        )

    missing = [name for name in enum_order if name not in descriptors]
    extra = [name for name in descriptors if name not in enum_order]
    if missing or extra:
        raise ManifestError(
            "index descriptor registry mismatch; "
            f"missing={','.join(missing) or 'none'} extra={','.join(extra) or 'none'}"
        )
    return descriptors


def complete_capability(family: str) -> CapabilityState:
    return CapabilityState(
        enum_name=family,
        declared_capability=True,
        planner_contract_capability=True,
        implemented=True,
        physical_reader=True,
        physical_writer=True,
        maintenance=True,
        validate=True,
        repair=True,
        recovery_reopen=True,
        rebuild=True,
        runtime_available=True,
        benchmark_clean=True,
        blocker="none",
        blocker_diagnostic_code="",
        blocker_message_key="",
        blocker_detail="",
    )


def parse_capabilities(source: str, enum_order: list[str]) -> dict[str, CapabilityState]:
    states: dict[str, CapabilityState] = {}
    for match in re.finditer(r"CompleteCapability\(IndexFamily::(?P<family>\w+)\)", source):
        states[match.group("family")] = complete_capability(match.group("family"))

    capability_re = re.compile(
        r"Capability\(IndexFamily::(?P<family>\w+),\s*"
        r"IndexFamilyPhysicalCapabilityBlocker::(?P<blocker>\w+),\s*"
        r'"(?P<code>[^"]+)",\s*"(?P<key>[^"]+)",\s*"(?P<detail>[^"]*)"'
        r"(?:,\s*(?P<planner>false|true))?\)",
        re.S,
    )
    for match in capability_re.finditer(source):
        family = match.group("family")
        states[family] = CapabilityState(
            enum_name=family,
            declared_capability=True,
            planner_contract_capability=parse_bool(match.group("planner"), default=True),
            implemented=False,
            physical_reader=False,
            physical_writer=False,
            maintenance=False,
            validate=False,
            repair=False,
            recovery_reopen=False,
            rebuild=False,
            runtime_available=False,
            benchmark_clean=False,
            blocker=match.group("blocker"),
            blocker_diagnostic_code=match.group("code"),
            blocker_message_key=match.group("key"),
            blocker_detail=" ".join(match.group("detail").split()),
        )

    missing = [name for name in enum_order if name not in states]
    extra = [name for name in states if name not in enum_order]
    if missing or extra:
        raise ManifestError(
            "index capability registry mismatch; "
            f"missing={','.join(missing) or 'none'} extra={','.join(extra) or 'none'}"
        )
    return states


def ordered_write_family(family: str) -> bool:
    return family in {"btree", "unique_btree", "expression", "partial", "covering"}


def token_search_family(family: str) -> bool:
    return family in {"full_text", "gin", "inverted", "ngram", "sparse_wand"}


def vector_family(family: str) -> bool:
    return family in {"vector_exact", "vector_hnsw", "vector_ivf"}


def approximate_vector_family(family: str) -> bool:
    return family in {"vector_hnsw", "vector_ivf"}


def spatial_family(family: str) -> bool:
    return family in {"spatial", "rtree", "gist", "spgist"}


def candidate_family(family: str) -> bool:
    return (
        family in {"bitmap", "document_path", "graph"}
        or token_search_family(family)
        or vector_family(family)
        or spatial_family(family)
    )


def summary_segment_prune_family(family: str) -> bool:
    return family in {"brin_zone", "columnar_zone"}


def pruning_family(family: str) -> bool:
    return family == "bloom" or summary_segment_prune_family(family)


def specialized_persistent_family(family: str) -> bool:
    return (
        family in {"bitmap", "bloom", "document_path", "graph"}
        or token_search_family(family)
        or vector_family(family)
        or spatial_family(family)
        or summary_segment_prune_family(family)
    )


def ranking_or_seed_family(family: str) -> bool:
    return (
        family in {"sparse_wand", "document_path", "graph"}
        or vector_family(family)
        or spatial_family(family)
    )


def equality_lookup_family(family: str) -> bool:
    return ordered_write_family(family) or family in {"hash", "document_path", "graph"}


def ordered_range_family(family: str) -> bool:
    return ordered_write_family(family)


def route_supports_family(route: str, family: str, capability: CapabilityState) -> bool:
    if not capability.family_complete():
        return False
    if route in {"dml_insert", "dml_update", "dml_delete"}:
        return ordered_write_family(family)
    if route in {"sql_select", "bulk_build", "maintenance", "validate_repair"}:
        return family not in {"reference_emulated", "policy_blocked"}
    if route == "nosql_document":
        return family == "document_path"
    if route == "nosql_graph":
        return family == "graph"
    if route == "nosql_vector":
        return vector_family(family)
    if route == "nosql_search":
        return token_search_family(family)
    return False


def build_route_state(route: str, descriptor: FamilyDescriptor, capability: CapabilityState) -> dict[str, Any]:
    family = descriptor.enum_name
    route_supported = route_supports_family(route, family, capability)
    route_complete = capability.family_complete() and route_supported
    supports_read = route_supported and route in {
        "sql_select",
        "nosql_document",
        "nosql_graph",
        "nosql_vector",
        "nosql_search",
        "maintenance",
        "validate_repair",
    }
    supports_write = route_supported and route in {"dml_insert", "dml_update", "dml_delete"}
    return {
        "route": route,
        "route_declared": True,
        "route_supported": route_supported,
        "route_complete_static_capability_input": route_complete,
        "supports_read": supports_read,
        "supports_write": supports_write,
        "supports_mutation": supports_write or (route_supported and route == "maintenance"),
        "supports_bulk_build": route_supported and route == "bulk_build",
        "supports_reopen": route_supported and route in {"maintenance", "validate_repair", "bulk_build"},
        "supports_ordered_range": route_supported and ordered_range_family(family),
        "supports_equality_lookup": route_supported and equality_lookup_family(family),
        "supports_negative_prune": route_supported and family == "bloom",
        "supports_summary_segment_prune": route_supported and summary_segment_prune_family(family),
        "produces_candidate_set": route_supported and candidate_family(family),
        "produces_ranking_or_seed": route_supported and ranking_or_seed_family(family),
        "approximate_candidate_source": route_supported and approximate_vector_family(family),
        "requires_exact_recheck": route_supported and (candidate_family(family) or pruning_family(family) or family == "hash"),
        "requires_mga_recheck": True,
        "requires_security_recheck": True,
        "requires_exact_rerank": route_supported and (approximate_vector_family(family) or family == "sparse_wand"),
        "requires_exact_fallback": route_supported
        and (
            pruning_family(family)
            or family == "bitmap"
            or token_search_family(family)
            or spatial_family(family)
            or approximate_vector_family(family)
            or family in {"document_path", "graph"}
        ),
        "hash_requires_keyed_algorithm": family == "hash"
        and route in {"sql_select", "maintenance", "validate_repair"},
        "hash_equality_only": family == "hash",
        "row_truth_authority": False,
        "final_row_authority": False,
    }


def family_semantic_class(family: str) -> str:
    if ordered_write_family(family):
        return "exact_candidate"
    if family == "hash":
        return "hash_equality_candidate"
    if family == "bitmap":
        return "bitmap_candidate"
    if family == "sparse_wand":
        return "token_ranking_candidate"
    if family in {"full_text", "gin", "inverted", "ngram"}:
        return "token_candidate"
    if spatial_family(family):
        return "spatial_candidate"
    if family == "vector_exact":
        return "vector_exact_candidate"
    if approximate_vector_family(family):
        return "vector_approximate_candidate"
    if family == "document_path":
        return "document_path_candidate"
    if family == "graph":
        return "graph_seed_candidate"
    if family == "bloom":
        return "bloom_negative_prune"
    if summary_segment_prune_family(family):
        return "summary_segment_prune"
    if family == "temporary_work":
        return "temporary_work_candidate"
    if family == "in_memory":
        return "in_memory_candidate"
    if family == "reference_emulated":
        return "reference_emulated_non_runtime"
    if family == "policy_blocked":
        return "policy_blocked_non_runtime"
    return "unsupported"


def family_classification_summary(
    descriptor: FamilyDescriptor,
    successor_status_by_slice: dict[str, str],
) -> dict[str, Any]:
    family = descriptor.enum_name
    ceic_038_complete = successor_status_by_slice.get("CEIC-038", "pending") in COMPLETE_STATUS
    blocked = descriptor.persistence_class in {"reference_emulated", "policy_blocked"}
    return {
        "source": "project/src/core/index/index_family_route_classification.*",
        "gate": "project/tests/consolidated_enterprise/ceic_038_lossy_pruning_family_classification_gate.cpp",
        "proof_status": "complete" if ceic_038_complete else "pending",
        "semantic_class": family_semantic_class(family),
        "runtime_admissible": False if blocked else True,
        "exact_candidate": ordered_write_family(family),
        "hash_equality_candidate": family == "hash",
        "hash_equality_only": family == "hash",
        "hash_ordered_range_authority": False,
        "candidate_only": candidate_family(family),
        "bitmap_candidate": family == "bitmap",
        "token_or_inverted_candidate": token_search_family(family),
        "ranking_or_seed_producer": ranking_or_seed_family(family),
        "vector_candidate": vector_family(family),
        "approximate_candidate": approximate_vector_family(family),
        "document_candidate": family == "document_path",
        "graph_seed_candidate": family == "graph",
        "spatial_candidate": spatial_family(family),
        "bloom_negative_prune_only": family == "bloom",
        "summary_segment_prune_only": summary_segment_prune_family(family),
        "row_truth_authority": False,
        "final_row_authority": False,
        "requires_exact_source": not blocked,
        "requires_mga_recheck": True,
        "requires_security_recheck": True,
        "requires_authorization_recheck": True,
        "requires_predicate_recheck": True,
        "requires_ceic_037_exact_recheck_handoff": not blocked,
        "requires_exact_fallback": (
            pruning_family(family)
            or family == "bitmap"
            or token_search_family(family)
            or spatial_family(family)
            or approximate_vector_family(family)
            or family in {"document_path", "graph"}
        ),
        "requires_exact_rerank": approximate_vector_family(family) or family == "sparse_wand",
        "false_positive_accounting_future_proof": (
            pruning_family(family)
            or candidate_family(family)
        ),
        "non_runtime_fail_closed": blocked,
        "cluster_external_provider_only": True,
        "ceic_039_specialized_provider_closure_claimed": False,
        "ceic_040_runtime_metrics_claimed": False,
        "ceic_041_crash_matrix_claimed": False,
        "ceic_042_readiness_drift_claimed": False,
        "all_index_readiness_claimed": False,
        "reference_dominance_claimed": False,
        "enterprise_readiness_claimed": False,
    }


def provider_classification(descriptor: FamilyDescriptor) -> str:
    if descriptor.persistence_class == "policy_blocked":
        return "policy_blocked_non_runtime"
    if descriptor.persistence_class == "reference_emulated":
        return "reference_emulated_mapping_non_authority"
    if descriptor.persistence_class == "memory_only":
        return "temporary_memory_only"
    if descriptor.persistence_class == "memory_primary_persisted_cold_start":
        return "in_memory_primary_with_persisted_cold_start"
    if descriptor.enum_name == "bloom":
        return "persistent_bloom_negative_prune_only"
    if summary_segment_prune_family(descriptor.enum_name):
        return "persistent_summary_segment_prune_requires_exact_handoff"
    if descriptor.enum_name == "bitmap":
        return "persistent_bitmap_candidate_requires_exact_recheck"
    if descriptor.enum_name == "sparse_wand":
        return "persistent_token_ranking_candidate_requires_exact_rerank"
    if token_search_family(descriptor.enum_name):
        return "persistent_token_candidate_requires_exact_recheck"
    if approximate_vector_family(descriptor.enum_name):
        return "persistent_approximate_vector_candidate_requires_exact_rerank"
    if descriptor.enum_name == "vector_exact":
        return "persistent_vector_exact_candidate_requires_exact_recheck"
    if spatial_family(descriptor.enum_name):
        return "persistent_spatial_candidate_requires_exact_recheck"
    if descriptor.enum_name == "document_path":
        return "persistent_document_candidate_requires_exact_recheck"
    if descriptor.enum_name == "graph":
        return "persistent_graph_seed_candidate_requires_exact_recheck"
    return "persistent_exact_native_pending_durable_provider_proof"


def proof_state(status: str, owner: str, slices: Iterable[str], note: str) -> dict[str, Any]:
    return {
        "status": status,
        "owner": owner,
        "required_slices": list(slices),
        "note": note,
    }


def pending_successor_slices(
    successor_status_by_slice: dict[str, str],
    *slice_ids: str,
) -> tuple[str, ...]:
    return tuple(
        slice_id for slice_id in slice_ids
        if successor_status_by_slice.get(slice_id, "pending") in PENDING_STATUS
    )


def storage_authority_status(
    descriptor: FamilyDescriptor,
    successor_status_by_slice: dict[str, str],
) -> dict[str, Any]:
    if descriptor.persistence_class in {"reference_emulated", "policy_blocked"}:
        return proof_state(
            "blocked",
            "non_authority",
            (),
            "family is not allowed to own ScratchBird storage, visibility, finality, or recovery",
        )
    if descriptor.persistence_class == "memory_only":
        return proof_state(
            "not_applicable",
            "memory_only_runtime",
            (),
            "temporary work indexes are not persistent storage authority",
        )
    if successor_status_by_slice.get("CEIC-042", "pending") in COMPLETE_STATUS:
        return proof_state(
            "complete",
            "CEIC-031..CEIC-042 index readiness evidence chain",
            (),
            "CEIC-042 verifies CEIC-031 provider evidence, family durable provider proof where applicable, CEIC-040 metrics, CEIC-041 crash/corruption/cleanup proof, route capability, artifact registration, and fresh manifest state without granting row truth, storage finality, all-index readiness, reference dominance, or enterprise readiness",
        )
    if descriptor.enum_name == "hash" and successor_status_by_slice.get("CEIC-035", "pending") in COMPLETE_STATUS:
        return proof_state(
            "pending",
            "project/src/core/index/hash_durable_provider_closure.*",
            pending_successor_slices(
                successor_status_by_slice,
                "CEIC-040",
                "CEIC-041",
                "CEIC-042",
            ),
            "CEIC-035 hash durable provider evidence is present for directory bucket overflow collision-chain full-key recheck cleanup compaction and repair/rebuild proof; CEIC-040 runtime metrics and CEIC-041 crash matrix evidence are tracked separately while CEIC-042 readiness drift proof remains pending",
        )
    if (
        specialized_persistent_family(descriptor.enum_name)
        and successor_status_by_slice.get("CEIC-039", "pending") in COMPLETE_STATUS
    ):
        return proof_state(
            "pending",
            "project/src/core/index/specialized_persistent_provider_closure.*",
            pending_successor_slices(
                successor_status_by_slice,
                "CEIC-040",
                "CEIC-041",
                "CEIC-042",
            ),
            "CEIC-039 specialized persistent provider evidence is present for family physical payload storage integration COW generation identity cleanup validation repair rebuild candidate/prune/ranking/seed discipline and CEIC-037 exact recheck consumption; CEIC-040 runtime metrics and CEIC-041 crash matrix evidence are tracked separately while CEIC-042 readiness drift proof remains pending",
        )
    return proof_state(
        "pending",
        "family-specific persistent index provider closure",
        pending_successor_slices(
            successor_status_by_slice,
            "CEIC-033",
            "CEIC-035",
            "CEIC-039",
            "CEIC-041",
        ),
        "common CEIC-031/CEIC-032 contracts may be available, but durable provider, storage integration, family generation publish, cleanup, crash, and corruption proof remain unclosed for claimed families",
    )


def mga_cow_recovery_status(
    descriptor: FamilyDescriptor,
    successor_status_by_slice: dict[str, str],
) -> dict[str, Any]:
    if descriptor.persistence_class in {"reference_emulated", "policy_blocked", "memory_only"}:
        return proof_state(
            "not_applicable" if descriptor.persistence_class == "memory_only" else "blocked",
            "MGA authority boundary",
            (),
            "this family cannot supply MGA/COW recovery authority",
        )
    if successor_status_by_slice.get("CEIC-042", "pending") in COMPLETE_STATUS:
        return proof_state(
            "complete",
            "CEIC-032 MGA/COW recovery contract plus CEIC-041/042 freshness evidence",
            (),
            "CEIC-042 verifies generated manifest freshness against CEIC-032 MGA/COW contract evidence, CEIC-041 crash/corruption/cleanup matrix, route capability, and artifact/test registration while MGA remains the transaction visibility and finality authority",
        )
    if (
        specialized_persistent_family(descriptor.enum_name)
        and successor_status_by_slice.get("CEIC-039", "pending") in COMPLETE_STATUS
    ):
        return proof_state(
            "pending",
            "project/src/core/index/specialized_persistent_provider_closure.*",
            pending_successor_slices(
                successor_status_by_slice,
                "CEIC-041",
                "CEIC-042",
            ),
            "CEIC-039 consumes the CEIC-032 common evidence contract for specialized family generation identity cleanup horizon validation repair and rebuild evidence; CEIC-041 crash/corruption proof is tracked separately and CEIC-042 readiness drift remains pending",
        )
    return proof_state(
        "pending",
        "family-specific MGA/COW generation and crash matrix",
        pending_successor_slices(
            successor_status_by_slice,
            "CEIC-033",
            "CEIC-035",
            "CEIC-039",
            "CEIC-041",
        ),
        "CEIC-032 supplies the common evidence contract when complete; family-specific generation publish, crash classification, cleanup horizon, and reopen proof remain unclosed",
    )


def security_recheck_status(
    descriptor: FamilyDescriptor,
    successor_status_by_slice: dict[str, str],
) -> dict[str, Any]:
    if descriptor.persistence_class in {"reference_emulated", "policy_blocked"}:
        return proof_state(
            "blocked",
            "non_runtime_family",
            (),
            "blocked/non-runtime families cannot authorize rows",
        )
    if successor_status_by_slice.get("CEIC-037", "pending") in COMPLETE_STATUS:
        return proof_state(
            "complete",
            "project/src/core/index/index_recheck.*;project/tests/consolidated_enterprise/ceic_037_engine_owned_exact_recheck_gate.cpp",
            (),
            "CEIC-037 engine-owned exact recheck service proof is present for MGA visibility inventory snapshot security authorization predicate exact source fallback and rerank admission, with non-authority evidence and no CEIC-038 CEIC-039 CEIC-040 CEIC-041 CEIC-042 all-index or enterprise readiness claim",
        )
    if candidate_family(descriptor.enum_name) or descriptor.enum_name == "hash":
        return proof_state(
            "pending",
            "CEIC-037 engine-owned exact/MGA/security recheck services",
            SECURITY_RECHECK_SLICES,
            "candidate, lossy, approximate, token, spatial, document, graph, vector, and hash routes require exact/MGA/security proof",
        )
    return proof_state(
        "pending",
        "CEIC-037 security and predicate recheck confirmation",
        SECURITY_RECHECK_SLICES,
        "exact provider routes still need engine-owned security and MGA recheck proof before enterprise readiness",
    )


def evidence_status(
    descriptor: FamilyDescriptor,
    successor_status_by_slice: dict[str, str],
    ceic_040_runtime_metric_proof_complete: bool,
    ceic_041_crash_matrix_proof_complete: bool,
    ceic_042_readiness_drift_proof_complete: bool,
) -> dict[str, Any]:
    blocked = descriptor.persistence_class in {"reference_emulated", "policy_blocked"}
    return {
        "metric_producer_status": proof_state(
            "blocked" if blocked else "complete" if ceic_040_runtime_metric_proof_complete else "pending",
            "project/src/core/index/index_metrics.*;project/tests/consolidated_enterprise/ceic_040_index_operation_metrics_support_bundle_gate.cpp",
            ()
            if blocked or ceic_040_runtime_metric_proof_complete
            else METRIC_PROOF_SLICES,
            "CEIC-040 operation metric producer and bounded/redacted support-bundle row proof is present; metrics remain evidence-only and do not close CEIC-041 crash matrix CEIC-042 readiness drift CEIC-090 integrated metrics or CEIC-091 integrated support bundles"
            if ceic_040_runtime_metric_proof_complete and not blocked
            else "real operation metric producers are future proof; descriptors and static fields are not enough",
        ),
        "benchmark_evidence_status": proof_state(
            "blocked" if blocked else "complete" if ceic_042_readiness_drift_proof_complete else "pending",
            "CEIC-042 freshness and benchmark readiness gate",
            ()
            if blocked or ceic_042_readiness_drift_proof_complete
            else DRIFT_GATE_SLICES,
            "CEIC-042 drift gate validates the generated manifest against registry route capability CEIC-031..041 proof inputs focused test registration and non-smoke benchmark evidence; it does not grant enterprise, all-index, reference-dominance, or cluster readiness"
            if ceic_042_readiness_drift_proof_complete and not blocked
            else "CEIC-030 records benchmark-clean inputs but does not convert static or smoke-only fields into readiness",
        ),
        "scale_evidence_status": proof_state(
            "blocked" if blocked else "pending",
            "CEIC-093 integrated long soak and scale lanes",
            ("CEIC-093",) if not blocked else (),
            "scale and long soak proof remain integrated-future evidence",
        ),
        "crash_evidence_status": proof_state(
            "blocked" if blocked else "complete" if ceic_041_crash_matrix_proof_complete else "pending",
            "CEIC-041 crash matrix",
            ()
            if blocked or ceic_041_crash_matrix_proof_complete
            else ("CEIC-041",),
            "CEIC-041 crash matrix proof is present for old-or-new root visibility exactly-one generation unsafe half-publish refusal reopen backup/restore and concurrent generation serialization; evidence remains non-authoritative and CEIC-042 stays pending"
            if ceic_041_crash_matrix_proof_complete and not blocked
            else "crash/reopen proof is not supplied by static registry flags",
        ),
        "corruption_evidence_status": proof_state(
            "blocked" if blocked else "complete" if ceic_041_crash_matrix_proof_complete else "pending",
            "CEIC-041 corruption matrix",
            ()
            if blocked or ceic_041_crash_matrix_proof_complete
            else ("CEIC-041",),
            "CEIC-041 corruption classification repair/rebuild recommendation and deterministic diagnostic proof is present; it does not claim CEIC-042 benchmark/readiness drift"
            if ceic_041_crash_matrix_proof_complete and not blocked
            else "corruption classify/repair/rebuild proof remains future work",
        ),
        "cleanup_evidence_status": proof_state(
            "blocked" if blocked else "complete" if ceic_041_crash_matrix_proof_complete else "pending",
            "family-specific cleanup horizon and crash cleanup proof",
            ()
            if blocked or ceic_041_crash_matrix_proof_complete
            else pending_successor_slices(
                successor_status_by_slice,
                "CEIC-033",
                "CEIC-035",
                "CEIC-039",
                "CEIC-041",
            ),
            "CEIC-041 cleanup-horizon binding and crash cleanup matrix proof is present for non-blocked families; CEIC-042 benchmark/readiness drift remains pending"
            if ceic_041_crash_matrix_proof_complete and not blocked
            else "CEIC-032 common cleanup-horizon contract may be available, but family-specific cleanup and crash cleanup proof remain unclosed",
        ),
    }


def unique_reservation_protocol_status(
    descriptor: FamilyDescriptor,
    successor_status_by_slice: dict[str, str],
) -> dict[str, Any]:
    ceic_034_status = successor_status_by_slice.get("CEIC-034", "pending")
    if descriptor.enum_name != "unique_btree":
        return proof_state(
            "not_applicable",
            "CEIC-034 unique_btree protocol",
            (),
            "CEIC-034 applies only to the unique_btree family",
        )
    if ceic_034_status in COMPLETE_STATUS:
        return proof_state(
            "complete",
            "project/src/core/index/ceic_034_unique_reservation_finality_protocol.*",
            (),
            "CEIC-034 unique_btree reservation protocol evidence is present; CEIC-041 crash matrix proof is tracked separately and CEIC-042 readiness drift proof remains pending",
        )
    return proof_state(
        "pending",
        "CEIC-034 unique_btree reservation protocol",
        ("CEIC-034",),
        "unique_btree reservation finality protocol evidence remains pending",
    )


def claim_boundary(descriptor: FamilyDescriptor) -> dict[str, Any]:
    policy = "blocked" if descriptor.persistence_class == "policy_blocked" else "not_policy_blocked"
    reference = "blocked_non_authority" if descriptor.persistence_class == "reference_emulated" else "not_reference_emulated"
    runtime = "blocked" if descriptor.persistence_class in {"reference_emulated", "policy_blocked"} else "pending_future_proof"
    return {
        "policy_claim_boundary": policy,
        "reference_claim_boundary": reference,
        "runtime_claim_boundary": runtime,
        "cluster_claim_boundary": "blocked_external_provider_only",
        "transaction_finality_authority": False,
        "visibility_authority": False,
        "authorization_security_authority": False,
        "security_authority": False,
        "recovery_authority": False,
        "parser_authority": False,
        "reference_authority": False,
        "wal_authority": False,
        "benchmark_authority": False,
        "optimizer_plan_authority": False,
        "optimizer_plan_finality_authority": False,
        "index_finality_authority": False,
        "row_truth_authority": False,
        "final_row_authority": False,
        "result_finality_authority": False,
        "local_cluster_authority": False,
        "cluster_authority": False,
        "cluster_action_authority": False,
        "agent_action_authority": False,
    }


def build_family_row(
    descriptor: FamilyDescriptor,
    capability: CapabilityState,
    route_kinds: list[str],
    generated_at: str,
    successor_status_by_slice: dict[str, str],
    ceic_040_runtime_metric_proof_complete: bool,
    ceic_041_crash_matrix_proof_complete: bool,
    ceic_042_readiness_drift_proof_complete: bool,
) -> dict[str, Any]:
    routes = [build_route_state(route, descriptor, capability) for route in route_kinds]
    supported_routes = [row["route"] for row in routes if row["route_supported"]]
    route_summary = {
        "source": "project/src/core/index/index_route_capability.*",
        "route_count": len(routes),
        "supported_routes": supported_routes,
        "unsupported_routes": [row["route"] for row in routes if not row["route_supported"]],
        "supports_any_read": any(row["supports_read"] for row in routes),
        "supports_any_write": any(row["supports_write"] for row in routes),
        "supports_bulk_build": any(row["supports_bulk_build"] for row in routes),
        "supports_reopen": any(row["supports_reopen"] for row in routes),
        "supports_ordered_range": any(row["supports_ordered_range"] for row in routes),
        "supports_equality_lookup": any(row["supports_equality_lookup"] for row in routes),
        "produces_candidate_set": any(row["produces_candidate_set"] for row in routes),
        "approximate_candidate_source": any(row["approximate_candidate_source"] for row in routes),
        "requires_exact_recheck": any(row["requires_exact_recheck"] for row in routes),
        "requires_mga_recheck": all(row["requires_mga_recheck"] for row in routes),
        "requires_security_recheck": all(row["requires_security_recheck"] for row in routes),
        "supports_summary_segment_prune": any(row["supports_summary_segment_prune"] for row in routes),
        "supports_negative_prune": any(row["supports_negative_prune"] for row in routes),
        "produces_ranking_or_seed": any(row["produces_ranking_or_seed"] for row in routes),
        "requires_exact_fallback": any(row["requires_exact_fallback"] for row in routes),
        "row_truth_authority": False,
        "final_row_authority": False,
        "routes": routes,
    }
    runtime_blocked = descriptor.persistence_class in {"reference_emulated", "policy_blocked"}
    blockers = []
    for slice_id, reason in (
        ("CEIC-031", "persistent_provider_api_pending_CEIC_031"),
        ("CEIC-032", "mga_cow_recovery_proof_pending_CEIC_032"),
        ("CEIC-034", "unique_reservation_protocol_pending_CEIC_034"),
        ("CEIC-035", "hash_durable_provider_closure_pending_CEIC_035"),
        ("CEIC-036", "canonical_key_ordering_encoding_fuzz_pending_CEIC_036"),
        ("CEIC-037", "security_exact_recheck_proof_pending_CEIC_037"),
        ("CEIC-039", "specialized_persistent_provider_closure_pending_CEIC_039"),
        ("CEIC-040", "operation_metric_producers_pending_CEIC_040"),
        ("CEIC-041", "crash_cleanup_corruption_matrix_pending_CEIC_041"),
        ("CEIC-042", "freshness_drift_ci_gate_pending_CEIC_042"),
    ):
        if slice_id == "CEIC-034" and descriptor.enum_name != "unique_btree":
            continue
        if slice_id == "CEIC-035" and descriptor.enum_name != "hash":
            continue
        if slice_id == "CEIC-039" and not specialized_persistent_family(descriptor.enum_name):
            continue
        if successor_status_by_slice.get(slice_id, "pending") in PENDING_STATUS:
            blockers.append(reason)
    if runtime_blocked:
        blockers = ["family_is_non_runtime_non_authority"]
    elif descriptor.persistence_class == "memory_only":
        blockers = [
            "temporary_memory_only_not_enterprise_persistent_ready",
            *[blocker for blocker in blockers
              if not blocker.startswith("persistent_provider_api_pending_")
              and not blocker.startswith("mga_cow_recovery_proof_pending_")],
        ]
    if not blockers and not runtime_blocked:
        blockers = ["enterprise_readiness_requires_CEIC_042_manifest_gate"]

    row: dict[str, Any] = {
        "family_id": descriptor.family_id,
        "family_name": descriptor.canonical_name,
        "enum_name": descriptor.enum_name,
        "provider_classification": provider_classification(descriptor),
        "persistence_class": descriptor.persistence_class,
        "persistent": descriptor.persistent,
        "runtime_availability": {
            "declared_capability": capability.declared_capability,
            "planner_contract_capability": capability.planner_contract_capability,
            "implemented_static_input": capability.implemented,
            "physical_reader_static_input": capability.physical_reader,
            "physical_writer_static_input": capability.physical_writer,
            "maintenance_static_input": capability.maintenance,
            "validate_static_input": capability.validate,
            "repair_static_input": capability.repair,
            "recovery_reopen_static_input": capability.recovery_reopen,
            "rebuild_static_input": capability.rebuild,
            "runtime_available_static_input": capability.runtime_available,
            "benchmark_clean_static_input": capability.benchmark_clean,
            "physically_complete_static_input": capability.physically_complete(),
            "blocker": capability.blocker,
            "blocker_diagnostic_code": capability.blocker_diagnostic_code,
            "blocker_message_key": capability.blocker_message_key,
            "blocker_detail": capability.blocker_detail,
        },
        "descriptor": {
            "key_model": descriptor.key_model,
            "native_physical_family": descriptor.native_physical_family,
            "default_semantic_profile": descriptor.default_semantic_profile,
            "completion_status": descriptor.completion_status,
            "metrics_prefix": descriptor.metrics_prefix,
            "diagnostics_prefix": descriptor.diagnostics_prefix,
            "packet_path": descriptor.packet_path,
            "baseline": descriptor.baseline,
            "requires_mga_recheck": descriptor.requires_mga_recheck,
            "supports_ordering": descriptor.supports_ordering,
            "supports_uniqueness": descriptor.supports_uniqueness,
            "approximate": descriptor.approximate,
        },
        "route_capability_summary": route_summary,
        "family_classification_summary": family_classification_summary(
            descriptor,
            successor_status_by_slice,
        ),
        "specialized_persistent_provider_closure_status": proof_state(
            "complete"
            if specialized_persistent_family(descriptor.enum_name)
            and successor_status_by_slice.get("CEIC-039", "pending") in COMPLETE_STATUS
            else "blocked"
            if descriptor.persistence_class in {"reference_emulated", "policy_blocked"}
            else "not_applicable"
            if not specialized_persistent_family(descriptor.enum_name)
            else "pending",
            "project/src/core/index/specialized_persistent_provider_closure.*",
            ()
            if specialized_persistent_family(descriptor.enum_name)
            and successor_status_by_slice.get("CEIC-039", "pending") in COMPLETE_STATUS
            else ("CEIC-039",)
            if specialized_persistent_family(descriptor.enum_name)
            and descriptor.persistence_class not in {"reference_emulated", "policy_blocked"}
            else (),
            "CEIC-039 specialized provider closure evidence is tracked separately from CEIC-040 runtime metrics CEIC-041 crash/corruption matrix CEIC-042 readiness drift all-index readiness reference dominance and enterprise readiness claims",
        ),
        "storage_authority_status": storage_authority_status(
            descriptor,
            successor_status_by_slice,
        ),
        "mga_cow_recovery_proof_status": mga_cow_recovery_status(
            descriptor,
            successor_status_by_slice,
        ),
        "unique_reservation_protocol_status": unique_reservation_protocol_status(
            descriptor,
            successor_status_by_slice,
        ),
        "security_exact_recheck_proof_status": security_recheck_status(
            descriptor,
            successor_status_by_slice,
        ),
        **evidence_status(
            descriptor,
            successor_status_by_slice,
            ceic_040_runtime_metric_proof_complete,
            ceic_041_crash_matrix_proof_complete,
            ceic_042_readiness_drift_proof_complete,
        ),
        "readiness_drift_gate_status": proof_state(
            "blocked"
            if runtime_blocked
            else "complete"
            if ceic_042_readiness_drift_proof_complete
            else "pending",
            "project/tools/ceic_index_readiness_manifest.py;project/tests/consolidated_enterprise/ceic_042_index_readiness_manifest_drift_gate_test.py",
            ()
            if runtime_blocked or ceic_042_readiness_drift_proof_complete
            else DRIFT_GATE_SLICES,
            "CEIC-042 manifest drift/freshness gate is complete for this family row while remaining evidence-only and non-authoritative"
            if ceic_042_readiness_drift_proof_complete and not runtime_blocked
            else "blocked/non-runtime family cannot participate in readiness"
            if runtime_blocked
            else "CEIC-042 manifest drift/freshness gate remains pending",
        ),
        "policy_reference_cluster_claim_boundary": claim_boundary(descriptor),
        "enterprise_ready": False,
        "enterprise_ready_blockers": blockers,
        "auditor_timestamp_utc": generated_at,
        "generation_metadata": {
            "generated_by": "project/tools/ceic_index_readiness_manifest.py#CEIC_030_INDEX_READINESS_MANIFEST_TOOL",
            "source_registry": "project/src/core/index/index_family_registry.*",
            "source_route_capability": "project/src/core/index/index_route_capability.*",
            "source_family_classification": "project/src/core/index/index_family_route_classification.*",
            "ceic_scope": "CEIC-030 schema and generated manifest with CEIC-038 family classification indexed when complete; successor provider/runtime proof is tracked separately",
        },
    }
    return row


def validate_execution_plan_inputs(repo_root: pathlib.Path, *, writing: bool) -> tuple[dict[str, dict[str, str]], dict[str, dict[str, str]]]:
    tracker = index_by(read_csv(repo_root, EXECUTION_PLAN / "TRACKER.csv"), "slice_id", "TRACKER.csv")
    artifacts = index_by(read_csv(repo_root, EXECUTION_PLAN / "ARTIFACT_INDEX.csv"), "artifact_id", "ARTIFACT_INDEX.csv")

    ceic_030_status = normalize_status(tracker.get("CEIC-030", {}).get("status", ""))
    if ceic_030_status not in COMPLETE_STATUS | PENDING_STATUS:
        raise ManifestError("CEIC-030 tracker row must be pending or complete during manifest generation")
    for slice_id in INDEX_FUTURE_SLICES:
        status = normalize_status(tracker.get(slice_id, {}).get("status", ""))
        if status not in COMPLETE_STATUS | PENDING_STATUS:
            raise ManifestError(
                f"{slice_id} must be pending or complete; CEIC-030 manifest tracks successor proof but cannot close it"
            )

    manifest_row = artifacts.get("CEIC-ART-012")
    if not manifest_row:
        raise ManifestError("CEIC-ART-012 index readiness manifest artifact row missing")
    if manifest_row.get("path", "").strip() != DEFAULT_MANIFEST.as_posix():
        raise ManifestError("CEIC-ART-012 path must be the CEIC-030 generated index readiness manifest")
    manifest_status = normalize_status(manifest_row.get("status", ""))
    if not writing and manifest_status not in PRESENT_STATUS:
        raise ManifestError("CEIC-ART-012 must be present/generated for CEIC-030 validation")

    evidence_row = artifacts.get("CEIC-ART-045")
    if not evidence_row:
        raise ManifestError("CEIC-ART-045 CEIC-030 evidence artifact row missing")
    if evidence_row.get("path", "").strip() != EVIDENCE_MD.as_posix():
        raise ManifestError("CEIC-ART-045 path must be the CEIC-030 evidence markdown artifact")
    if not writing and normalize_status(evidence_row.get("status", "")) not in PRESENT_STATUS:
        raise ManifestError("CEIC-ART-045 must be present/generated for CEIC-030 validation")
    if not writing and not (repo_root / EVIDENCE_MD).exists():
        raise ManifestError("CEIC-ART-045 is present but evidence markdown is missing")

    ceic_038_status = normalize_status(tracker.get("CEIC-038", {}).get("status", ""))
    if ceic_038_status in COMPLETE_STATUS:
        classification_row = artifacts.get("CEIC-ART-053")
        if not classification_row:
            raise ManifestError("CEIC-ART-053 CEIC-038 classification evidence artifact row missing")
        if classification_row.get("path", "").strip() != CEIC_038_EVIDENCE_MD.as_posix():
            raise ManifestError("CEIC-ART-053 path must be the CEIC-038 classification evidence markdown artifact")
        if not writing and normalize_status(classification_row.get("status", "")) not in PRESENT_STATUS:
            raise ManifestError("CEIC-ART-053 must be present/generated when CEIC-038 is complete")
        if not writing and not (repo_root / CEIC_038_EVIDENCE_MD).exists():
            raise ManifestError("CEIC-ART-053 is present but evidence markdown is missing")

    ceic_039_status = normalize_status(tracker.get("CEIC-039", {}).get("status", ""))
    if ceic_039_status in COMPLETE_STATUS:
        closure_row = artifacts.get("CEIC-ART-054")
        if not closure_row:
            raise ManifestError("CEIC-ART-054 CEIC-039 specialized provider closure evidence artifact row missing")
        if closure_row.get("path", "").strip() != CEIC_039_EVIDENCE_MD.as_posix():
            raise ManifestError("CEIC-ART-054 path must be the CEIC-039 specialized provider closure evidence markdown artifact")
        if not writing and normalize_status(closure_row.get("status", "")) not in PRESENT_STATUS:
            raise ManifestError("CEIC-ART-054 must be present/generated when CEIC-039 is complete")
        if not writing and not (repo_root / CEIC_039_EVIDENCE_MD).exists():
            raise ManifestError("CEIC-ART-054 is present but evidence markdown is missing")

    return tracker, artifacts


def index_successor_slice_statuses(tracker: dict[str, dict[str, str]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for slice_id in INDEX_FUTURE_SLICES:
        row = tracker.get(slice_id, {})
        status = normalize_status(row.get("status", ""))
        rows.append({
            "slice_id": slice_id,
            "title": row.get("title", ""),
            "status": status,
            "acceptance": row.get("acceptance", ""),
        })
    return rows


def collect_input_records(repo_root: pathlib.Path) -> tuple[list[dict[str, Any]], str]:
    records: list[dict[str, Any]] = []
    digest = hashlib.sha256()
    for rel_path in sorted(set(CONTROL_INPUTS), key=lambda path: path.as_posix()):
        path = repo_root / rel_path
        if not path.exists():
            raise ManifestError(f"manifest input missing: {rel_path.as_posix()}")
        file_hash = sha256_file(path)
        rel_text = rel_path.as_posix()
        records.append({"path": rel_text, "sha256": file_hash, "bytes": path.stat().st_size})
        digest.update(rel_text.encode("utf-8"))
        digest.update(b"\0")
        digest.update(file_hash.encode("ascii"))
        digest.update(b"\0")
    return records, digest.hexdigest()


def collect_index_metrics(repo_root: pathlib.Path) -> list[dict[str, str]]:
    rows = read_csv(repo_root, EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv")
    metric_rows = [
        row
        for row in rows
        if "index" in row.get("subsystem", "").lower()
        or row.get("metric_family", "").startswith("index_")
    ]
    if not metric_rows:
        raise ManifestError("METRICS_PRODUCER_COVERAGE_MATRIX.csv has no index metric rows")
    return metric_rows


def require_text_token(repo_root: pathlib.Path, rel: pathlib.Path, token: str) -> None:
    text = read_text(repo_root, rel)
    if token not in text:
        raise ManifestError(f"{rel.as_posix()} missing CEIC-040 token {token}")


def require_token(repo_root: pathlib.Path, rel: pathlib.Path, token: str, label: str) -> None:
    text = read_text(repo_root, rel)
    if token not in text:
        raise ManifestError(f"{rel.as_posix()} missing {label} token {token}")


def validate_registered_index_proof_inputs(
    repo_root: pathlib.Path,
    tracker: dict[str, dict[str, str]],
    artifacts: dict[str, dict[str, str]],
    *,
    writing: bool,
) -> None:
    cmake_text = read_text(repo_root, TEST_CMAKE)
    for (
        slice_id,
        artifact_id,
        evidence_path,
        gate_path,
        gate_name,
        source_tokens,
        evidence_token,
    ) in INDEX_PROOF_INPUTS:
        if normalize_status(tracker.get(slice_id, {}).get("status", "")) not in COMPLETE_STATUS:
            raise ManifestError(f"{slice_id} must be complete before CEIC-042 readiness drift closure")

        artifact_row = artifacts.get(artifact_id)
        if not artifact_row:
            raise ManifestError(f"{artifact_id} {slice_id} evidence artifact row missing")
        if artifact_row.get("path", "").strip() != evidence_path.as_posix():
            raise ManifestError(f"{artifact_id} path must be {evidence_path.as_posix()}")
        if not writing and normalize_status(artifact_row.get("status", "")) not in PRESENT_STATUS:
            raise ManifestError(f"{artifact_id} must be present/generated for CEIC-042 readiness drift closure")
        if not (repo_root / evidence_path).exists():
            raise ManifestError(f"{artifact_id} evidence markdown is missing: {evidence_path.as_posix()}")
        if not (repo_root / gate_path).exists():
            raise ManifestError(f"{slice_id} focused gate is missing: {gate_path.as_posix()}")
        if gate_name not in cmake_text:
            raise ManifestError(f"{slice_id} focused gate is not registered in CMake: {gate_name}")
        require_token(repo_root, gate_path, gate_name, f"{slice_id} focused gate")
        require_token(repo_root, evidence_path, evidence_token, f"{slice_id} evidence")
        for source_path, token in source_tokens:
            if not (repo_root / source_path).exists():
                raise ManifestError(f"{slice_id} source proof input missing: {source_path.as_posix()}")
            require_token(repo_root, source_path, token, f"{slice_id} source proof")


def integrated_boundary_slice_statuses(tracker: dict[str, dict[str, str]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for slice_id in INTEGRATED_INDEX_BOUNDARY_SLICES:
        row = tracker.get(slice_id, {})
        status = normalize_status(row.get("status", ""))
        rows.append({
            "slice_id": slice_id,
            "title": row.get("title", ""),
            "status": status,
            "acceptance": row.get("acceptance", ""),
        })
    return rows


def validate_integrated_boundary_pending(tracker: dict[str, dict[str, str]]) -> None:
    for row in integrated_boundary_slice_statuses(tracker):
        if row["status"] not in PENDING_STATUS:
            raise ManifestError(f"{row['slice_id']} must remain pending integrated proof for CEIC-042")


def ceic_042_readiness_drift_proof_complete(
    repo_root: pathlib.Path,
    tracker: dict[str, dict[str, str]],
    artifacts: dict[str, dict[str, str]],
    *,
    writing: bool,
) -> bool:
    status = normalize_status(tracker.get("CEIC-042", {}).get("status", ""))
    if status not in COMPLETE_STATUS:
        return False

    validate_integrated_boundary_pending(tracker)
    validate_registered_index_proof_inputs(repo_root, tracker, artifacts, writing=writing)

    gates = index_by(
        read_csv(repo_root, EXECUTION_PLAN / "ACCEPTANCE_GATES.csv"),
        "gate_id",
        "ACCEPTANCE_GATES.csv",
    )
    if normalize_status(gates.get("CEIC-GATE-020", {}).get("status", "")) not in COMPLETE_STATUS:
        raise ManifestError("CEIC-GATE-020 must be complete when CEIC-042 is complete")

    artifact_row = artifacts.get("CEIC-ART-057")
    if not artifact_row:
        raise ManifestError("CEIC-ART-057 CEIC-042 drift gate evidence artifact row missing")
    if artifact_row.get("path", "").strip() != CEIC_042_EVIDENCE_MD.as_posix():
        raise ManifestError("CEIC-ART-057 path must be the CEIC-042 drift gate evidence markdown artifact")
    if not writing and normalize_status(artifact_row.get("status", "")) not in PRESENT_STATUS:
        raise ManifestError("CEIC-ART-057 must be present/generated when CEIC-042 is complete")
    if not (repo_root / CEIC_042_EVIDENCE_MD).exists():
        raise ManifestError("CEIC-ART-057 is present but evidence markdown is missing")
    if not (repo_root / CEIC_042_GATE_TEST).exists():
        raise ManifestError("CEIC-042 focused drift gate test is missing")

    require_token(
        repo_root,
        CEIC_042_GATE_TEST,
        "CEIC_042_INDEX_READINESS_MANIFEST_DRIFT_GATE_TEST",
        "CEIC-042 focused gate",
    )
    require_token(
        repo_root,
        TEST_CMAKE,
        "ceic_042_index_readiness_manifest_drift_gate",
        "CEIC-042 CMake registration",
    )
    require_token(
        repo_root,
        CEIC_042_EVIDENCE_MD,
        "ceic-042-index-readiness-drift-gate-evidence",
        "CEIC-042 evidence",
    )
    require_token(
        repo_root,
        BENCHMARK_GATE,
        "concrete_runtime_consumed",
        "CEIC-042 benchmark non-smoke proof",
    )
    for token in (
        "point_lookup",
        "range_lookup",
        "bulk_build",
        "crash_reopen",
        "validate",
        "repair_rebuild",
        "fallback_disabled",
        "route_consumed_gate",
        "standalone_provider_gate",
    ):
        require_token(repo_root, BENCHMARK_GATE, token, "CEIC-042 benchmark workload proof")
    return True


def ceic_040_runtime_metric_proof_complete(
    repo_root: pathlib.Path,
    tracker: dict[str, dict[str, str]],
    artifacts: dict[str, dict[str, str]],
    *,
    writing: bool,
) -> bool:
    status = normalize_status(tracker.get("CEIC-040", {}).get("status", ""))
    if status not in COMPLETE_STATUS:
        return False
    artifact_row = artifacts.get("CEIC-ART-055")
    if not artifact_row:
        raise ManifestError("CEIC-ART-055 CEIC-040 evidence artifact row missing")
    if artifact_row.get("path", "").strip() != CEIC_040_EVIDENCE_MD.as_posix():
        raise ManifestError("CEIC-ART-055 path must be the CEIC-040 evidence markdown artifact")
    if not writing and normalize_status(artifact_row.get("status", "")) not in PRESENT_STATUS:
        raise ManifestError("CEIC-ART-055 must be present/generated when CEIC-040 is complete")
    if not writing and not (repo_root / CEIC_040_EVIDENCE_MD).exists():
        raise ManifestError("CEIC-ART-055 is present but evidence markdown is missing")

    for rel in (INDEX_METRICS_HPP, INDEX_METRICS_CPP, CEIC_040_GATE_TEST, CEIC_040_EVIDENCE_MD):
        if not (repo_root / rel).exists():
            raise ManifestError(f"CEIC-040 proof input missing: {rel.as_posix()}")
    require_text_token(repo_root, INDEX_METRICS_HPP, "CEIC_040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE")
    require_text_token(repo_root, INDEX_METRICS_CPP, "CEIC_040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE")
    require_text_token(repo_root, CEIC_040_GATE_TEST, "ceic_040_index_operation_metrics_support_bundle_gate")
    require_text_token(repo_root, TEST_CMAKE, "ceic_040_index_operation_metrics_support_bundle_gate")
    require_text_token(repo_root, CEIC_040_EVIDENCE_MD, "ceic-040-index-operation-metrics-support-bundle-evidence")
    return True


def ceic_041_crash_matrix_proof_complete(
    repo_root: pathlib.Path,
    tracker: dict[str, dict[str, str]],
    artifacts: dict[str, dict[str, str]],
    *,
    writing: bool,
) -> bool:
    status = normalize_status(tracker.get("CEIC-041", {}).get("status", ""))
    if status not in COMPLETE_STATUS:
        return False
    artifact_row = artifacts.get("CEIC-ART-056")
    if not artifact_row:
        raise ManifestError("CEIC-ART-056 CEIC-041 evidence artifact row missing")
    if artifact_row.get("path", "").strip() != CEIC_041_EVIDENCE_MD.as_posix():
        raise ManifestError("CEIC-ART-056 path must be the CEIC-041 evidence markdown artifact")
    if not writing and normalize_status(artifact_row.get("status", "")) not in PRESENT_STATUS:
        raise ManifestError("CEIC-ART-056 must be present/generated when CEIC-041 is complete")
    if not writing and not (repo_root / CEIC_041_EVIDENCE_MD).exists():
        raise ManifestError("CEIC-ART-056 is present but evidence markdown is missing")

    for rel in (
        INDEX_FAULT_MATRIX_HPP,
        INDEX_FAULT_MATRIX_CPP,
        CEIC_041_GATE_TEST,
        CEIC_041_EVIDENCE_MD,
    ):
        if not (repo_root / rel).exists():
            raise ManifestError(f"CEIC-041 proof input missing: {rel.as_posix()}")
    require_text_token(repo_root, INDEX_FAULT_MATRIX_HPP, "scenario_class")
    require_text_token(repo_root, INDEX_FAULT_MATRIX_CPP, "CEIC-041")
    require_text_token(
        repo_root,
        CEIC_041_GATE_TEST,
        "ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate",
    )
    require_text_token(repo_root, TEST_CMAKE, "ceic_041_index_crash_concurrency_cleanup_corruption_matrix_gate")
    require_text_token(
        repo_root,
        CEIC_041_EVIDENCE_MD,
        "ceic-041-index-crash-concurrency-cleanup-corruption-matrix-evidence",
    )
    return True


def build_manifest(
    repo_root: pathlib.Path,
    *,
    writing: bool = False,
    carried: dict[str, str] | None = None,
) -> dict[str, Any]:
    repo_root = repo_root.resolve()
    carried = carried or {}
    tracker, artifacts = validate_execution_plan_inputs(repo_root, writing=writing)
    successor_statuses = index_successor_slice_statuses(tracker)
    pending_successors = [
        row["slice_id"] for row in successor_statuses
        if row["status"] in PENDING_STATUS
    ]

    registry_header = read_text(repo_root, REGISTRY_HPP)
    registry_source = read_text(repo_root, REGISTRY_CPP)
    route_header = read_text(repo_root, ROUTE_HPP)
    route_source = read_text(repo_root, ROUTE_CPP)
    if "CompleteCapability" not in registry_source:
        raise ManifestError("registry source missing CompleteCapability static input")
    if "BuildStates()" not in route_source:
        raise ManifestError("route source missing BuildStates route capability surface")

    enum_order = parse_enum_order(registry_header)
    route_kinds = parse_route_kinds(route_header)
    descriptors = parse_descriptors(registry_source, enum_order)
    capabilities = parse_capabilities(registry_source, enum_order)
    input_records, input_digest = collect_input_records(repo_root)

    source_commit = carried.get("source_commit") or git_value(repo_root, ["rev-parse", "HEAD"], "unknown")
    source_branch = carried.get("source_branch") or git_value(
        repo_root, ["rev-parse", "--abbrev-ref", "HEAD"], "unknown"
    )
    generated_at = carried.get("generated_at_utc") or git_value(
        repo_root, ["show", "-s", "--format=%cI", "HEAD"], "1970-01-01T00:00:00+00:00"
    )
    manifest_uuid = str(uuid.uuid5(uuid.NAMESPACE_URL, f"scratchbird.ceic.030.index:{input_digest}"))
    successor_status_by_slice = {
        row["slice_id"]: row["status"] for row in successor_statuses
    }
    ceic_040_complete = ceic_040_runtime_metric_proof_complete(
        repo_root,
        tracker,
        artifacts,
        writing=writing,
    )
    ceic_041_complete = ceic_041_crash_matrix_proof_complete(
        repo_root,
        tracker,
        artifacts,
        writing=writing,
    )
    ceic_042_complete = ceic_042_readiness_drift_proof_complete(
        repo_root,
        tracker,
        artifacts,
        writing=writing,
    )

    family_rows = [
        build_family_row(
            descriptors[name],
            capabilities[name],
            route_kinds,
            generated_at,
            successor_status_by_slice,
            ceic_040_complete,
            ceic_041_complete,
            ceic_042_complete,
        )
        for name in enum_order
    ]
    metric_rows = collect_index_metrics(repo_root)
    integrated_statuses = integrated_boundary_slice_statuses(tracker)

    return {
        "schema_id": "scratchbird.ceic.evidence_manifest",
        "schema_version": 1,
        "manifest_kind": "index_readiness_manifest",
        "search_key": "CEIC_030_INDEX_READINESS_MANIFEST",
        "manifest_uuid": manifest_uuid,
        "subsystem": "indexes",
        "slice_ids": ["CEIC-030", "CEIC-042"] if ceic_042_complete else ["CEIC-030"],
        "future_index_slices_pending": pending_successors,
        "index_successor_slice_statuses": successor_statuses,
        "integrated_boundary_slice_statuses": integrated_statuses,
        "source_commit": source_commit,
        "source_branch": source_branch,
        "source_evidence_digest": input_digest,
        "generated_at_utc": generated_at,
        "generated_by": "project/tools/ceic_index_readiness_manifest.py#CEIC_030_INDEX_READINESS_MANIFEST_TOOL",
        "build_profile": "focused_ceic_index_manifest",
        "authority_boundary": {
            "boundary_token": AUTHORITY_BOUNDARY_TOKEN,
            "docs_alone_runtime_proof": False,
            "static_registry_ready_authority": False,
            "mga_finality_authority": False,
            "transaction_finality_authority": False,
            "visibility_authority": False,
            "authorization_security_authority": False,
            "recovery_authority": False,
            "parser_authority": False,
            "reference_authority": False,
            "wal_authority": False,
            "benchmark_authority": False,
            "optimizer_plan_authority": False,
            "optimizer_plan_finality_authority": False,
            "index_finality_authority": False,
            "row_truth_authority": False,
            "final_row_authority": False,
            "result_finality_authority": False,
            "local_cluster_authority": False,
            "cluster_authority": False,
            "cluster_action_authority": False,
            "agent_action_authority": False,
        },
        "readiness_policy": {
            "enterprise_ready_source": "generated_manifest_plus_future_provider_runtime_proof",
            "persistent_enterprise_ready_default": "blocked_pending_successor_runtime_proof_through_CEIC_042",
            "reference_emulated_runtime_claim": "blocked_non_authority",
            "policy_blocked_runtime_claim": "blocked_non_runtime",
            "cluster_production_claim": "blocked_external_provider_only",
            "ceic_030_scope": "schema_generator_checked_in_manifest_and_focused_gate_only",
            "ceic_038_scope": "lossy_candidate_pruning_family_classification_only_no_provider_metrics_crash_drift_readiness_claim",
            "ceic_042_scope": CEIC_042_SCOPE_COMPLETE if ceic_042_complete else CEIC_042_SCOPE_PENDING,
        },
        "readiness_drift_gate_evidence": {
            "state": "complete" if ceic_042_complete else "pending",
            "slice_id": "CEIC-042",
            "manifest_path": DEFAULT_MANIFEST.as_posix(),
            "gate": CEIC_042_GATE_TEST.as_posix(),
            "evidence": CEIC_042_EVIDENCE_MD.as_posix() if ceic_042_complete else "",
            "artifact_id": "CEIC-ART-057" if ceic_042_complete else "",
            "static_or_smoke_only": False,
            "missing_family_manifest_count": 0,
            "stale_manifest_allowed": False,
            "registry_route_capability_required": True,
            "provider_metrics_crash_corruption_required_slices": list(INDEX_FUTURE_SLICES[:-1]),
            "integrated_slices_must_remain_pending": list(INTEGRATED_INDEX_BOUNDARY_SLICES),
            "cluster_production_claim": "blocked_external_provider_only",
            "reference_emulated_runtime_claim": "blocked_non_authority",
            "policy_blocked_runtime_claim": "blocked_non_runtime",
            "reference_dominance_claimed": False,
            "all_index_readiness_claimed": False,
            "enterprise_readiness_claimed": False,
        },
        "source_anchors": [
            {
                "path": rel.as_posix(),
                "sha256": sha256_file(repo_root / rel),
            }
            for rel in (
                REGISTRY_HPP,
                REGISTRY_CPP,
                ROUTE_HPP,
                ROUTE_CPP,
                CLASSIFICATION_HPP,
                CLASSIFICATION_CPP,
                BENCHMARK_GATE,
                RECHECK_HPP,
                RECHECK_CPP,
                TRANSACTION_HPP,
                INDEX_ACCESS_METHOD_HPP,
                INDEX_ACCESS_METHOD_CPP,
                INDEX_MGA_RECOVERY_HPP,
                INDEX_MGA_RECOVERY_CPP,
                INDEX_FAULT_MATRIX_HPP,
                INDEX_FAULT_MATRIX_CPP,
                INDEX_METRICS_HPP,
                INDEX_METRICS_CPP,
                BTREE_UNIQUE_CLOSURE_HPP,
                BTREE_UNIQUE_CLOSURE_CPP,
                CEIC_034_UNIQUE_PROTOCOL_HPP,
                CEIC_034_UNIQUE_PROTOCOL_CPP,
                CEIC_036_CANONICAL_KEY_FUZZ_HPP,
                CEIC_036_CANONICAL_KEY_FUZZ_CPP,
            )
        ],
        "input_records": input_records,
        "route_kinds": route_kinds,
        "family_count": len(family_rows),
        "families": family_rows,
        "metrics_evidence": {
            "state": "index_operation_metric_producer_complete_ceic041_complete_ceic042_drift_gate_complete"
            if ceic_040_complete and ceic_042_complete
            else "index_operation_metric_producer_complete_ceic041_complete_ceic042_pending"
            if ceic_040_complete
            else "index_metric_rows_indexed_pending_real_operation_producer_closure",
            "matrix": (EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv").as_posix(),
            "rows": [
                {
                    "metric_family": row["metric_family"],
                    "subsystem": row["subsystem"],
                    "required_producer_path": row["required_producer_path"],
                    "operation_path": row["operation_path"],
                    "support_bundle_path": row["support_bundle_path"],
                    "validation_gate": row["validation_gate"],
                    "matrix_status": row["status"],
                    "manifest_state": (
                        "CEIC_040_runtime_operation_metric_support_bundle_proof_complete_CEIC_041_042_index_evidence_indexed_no_CEIC_090_091_claim"
                        if ceic_042_complete
                        else "CEIC_040_runtime_operation_metric_support_bundle_proof_complete_no_CEIC_041_042_090_091_claim"
                    )
                    if ceic_040_complete
                    else "indexed_pending_CEIC_040_real_operation_producer_proof",
                }
                for row in metric_rows
            ],
        },
        "auditor": {
            "timestamp_utc": generated_at,
            "generator": "project/tools/ceic_index_readiness_manifest.py",
            "gate": "project/tests/consolidated_enterprise/ceic_030_index_readiness_manifest_gate_test.py",
            "evidence": EVIDENCE_MD.as_posix(),
        },
    }


def load_manifest(path: pathlib.Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ManifestError(f"{path} is not deterministic JSON/YAML-subset manifest: {exc}") from exc
    if not isinstance(data, dict):
        raise ManifestError(f"{path} must contain a JSON object")
    return data


def validate_family_semantics(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    families = data.get("families")
    if not isinstance(families, list) or not families:
        return ["families must be a non-empty list"]
    family_ids = [row.get("family_id") for row in families if isinstance(row, dict)]
    if len(family_ids) != len(set(family_ids)):
        errors.append("families must be present exactly once by family_id")
    if data.get("family_count") != len(families):
        errors.append("family_count must match families length")

    required_fields = {
        "family_id",
        "family_name",
        "provider_classification",
        "persistent",
        "runtime_availability",
        "route_capability_summary",
        "family_classification_summary",
        "storage_authority_status",
        "mga_cow_recovery_proof_status",
        "security_exact_recheck_proof_status",
        "metric_producer_status",
        "benchmark_evidence_status",
        "scale_evidence_status",
        "crash_evidence_status",
        "corruption_evidence_status",
        "cleanup_evidence_status",
        "readiness_drift_gate_status",
        "policy_reference_cluster_claim_boundary",
        "auditor_timestamp_utc",
        "generation_metadata",
        "enterprise_ready",
    }
    future_fields = (
        "storage_authority_status",
        "mga_cow_recovery_proof_status",
        "security_exact_recheck_proof_status",
        "metric_producer_status",
        "benchmark_evidence_status",
        "scale_evidence_status",
        "crash_evidence_status",
        "corruption_evidence_status",
        "cleanup_evidence_status",
        "readiness_drift_gate_status",
    )
    successor_statuses = data.get("index_successor_slice_statuses", [])
    successor = {
        row.get("slice_id"): row.get("status")
        for row in successor_statuses
        if isinstance(row, dict)
    }
    ceic_042_complete = successor.get("CEIC-042") in COMPLETE_STATUS
    for row in families:
        if not isinstance(row, dict):
            errors.append("family row must be an object")
            continue
        missing = sorted(required_fields - set(row))
        if missing:
            errors.append(f"{row.get('family_id', '<unknown>')} missing fields: {', '.join(missing)}")
        if row.get("enterprise_ready") is not False:
            errors.append(f"{row.get('family_id', '<unknown>')} enterprise_ready must be false in CEIC-030")
        if row.get("persistent") is True:
            for field in future_fields:
                status = row.get(field, {}).get("status")
                allowed_statuses = {"pending", "blocked", "not_applicable"}
                if field == "security_exact_recheck_proof_status":
                    allowed_statuses = allowed_statuses | {"complete", "completed"}
                if field == "metric_producer_status":
                    allowed_statuses = allowed_statuses | {"complete", "completed"}
                if field in {
                    "storage_authority_status",
                    "mga_cow_recovery_proof_status",
                    "benchmark_evidence_status",
                    "crash_evidence_status",
                    "corruption_evidence_status",
                    "cleanup_evidence_status",
                    "readiness_drift_gate_status",
                }:
                    allowed_statuses = allowed_statuses | {"complete", "completed"}
                if status not in allowed_statuses:
                    errors.append(f"{row.get('family_id')} {field} must be pending/blocked/not_applicable, got {status}")
            if ceic_042_complete:
                if row.get("benchmark_evidence_status", {}).get("status") != "complete":
                    errors.append(f"{row.get('family_id')} CEIC-042 benchmark_evidence_status must be complete")
                if row.get("readiness_drift_gate_status", {}).get("status") != "complete":
                    errors.append(f"{row.get('family_id')} CEIC-042 readiness_drift_gate_status must be complete")
                for field in (
                    "security_exact_recheck_proof_status",
                    "metric_producer_status",
                    "crash_evidence_status",
                    "corruption_evidence_status",
                    "cleanup_evidence_status",
                ):
                    if row.get(field, {}).get("status") != "complete":
                        errors.append(f"{row.get('family_id')} CEIC-042 requires {field} complete")
        if row.get("persistence_class") in {"reference_emulated", "policy_blocked"}:
            if row.get("readiness_drift_gate_status", {}).get("status") != "blocked":
                errors.append(f"{row.get('family_id')} readiness drift gate status must be blocked")
        if (
            ceic_042_complete
            and row.get("persistence_class") not in {"reference_emulated", "policy_blocked", "memory_only"}
        ):
            for field in ("storage_authority_status", "mga_cow_recovery_proof_status"):
                if row.get(field, {}).get("status") != "complete":
                    errors.append(f"{row.get('family_id')} CEIC-042 requires {field} complete")
        boundary = row.get("policy_reference_cluster_claim_boundary", {})
        for key in (
            "transaction_finality_authority",
            "visibility_authority",
            "authorization_security_authority",
            "security_authority",
            "recovery_authority",
            "parser_authority",
            "reference_authority",
            "wal_authority",
            "benchmark_authority",
            "optimizer_plan_authority",
            "optimizer_plan_finality_authority",
            "index_finality_authority",
            "row_truth_authority",
            "final_row_authority",
            "result_finality_authority",
            "local_cluster_authority",
            "cluster_authority",
            "cluster_action_authority",
            "agent_action_authority",
        ):
            if boundary.get(key) is not False:
                errors.append(f"{row.get('family_id')} {key} must be false")
        classification = row.get("family_classification_summary", {})
        for key in (
            "row_truth_authority",
            "final_row_authority",
            "ceic_039_specialized_provider_closure_claimed",
            "ceic_040_runtime_metrics_claimed",
            "ceic_041_crash_matrix_claimed",
            "ceic_042_readiness_drift_claimed",
            "all_index_readiness_claimed",
            "reference_dominance_claimed",
            "enterprise_readiness_claimed",
        ):
            if classification.get(key) is not False:
                errors.append(f"{row.get('family_id')} classification {key} must be false")
        if row.get("enum_name") == "bloom":
            if classification.get("semantic_class") != "bloom_negative_prune":
                errors.append("bloom semantic_class must be bloom_negative_prune")
            if classification.get("bloom_negative_prune_only") is not True:
                errors.append("bloom must be negative-prune only")
            if row.get("route_capability_summary", {}).get("produces_candidate_set") is not False:
                errors.append("bloom must not produce candidate sets")
        if row.get("enum_name") in {"brin_zone", "columnar_zone"}:
            if classification.get("semantic_class") != "summary_segment_prune":
                errors.append(f"{row.get('family_id')} must be summary_segment_prune")
            if classification.get("summary_segment_prune_only") is not True:
                errors.append(f"{row.get('family_id')} must be summary/segment-prune only")
            if row.get("route_capability_summary", {}).get("produces_candidate_set") is not False:
                errors.append(f"{row.get('family_id')} must not produce candidate sets directly")
        if row.get("enum_name") in {"vector_hnsw", "vector_ivf", "sparse_wand"}:
            if classification.get("requires_exact_rerank") is not True:
                errors.append(f"{row.get('family_id')} must require exact rerank")
            if classification.get("requires_exact_fallback") is not True:
                errors.append(f"{row.get('family_id')} must require exact fallback")
        if row.get("enum_name") == "hash":
            if classification.get("hash_equality_only") is not True:
                errors.append("hash must be equality-only")
            if row.get("route_capability_summary", {}).get("supports_ordered_range") is not False:
                errors.append("hash must not support ordered ranges")
        if row.get("enum_name") == "reference_emulated":
            if row.get("provider_classification") != "reference_emulated_mapping_non_authority":
                errors.append("reference_emulated classification must remain non-authority")
            if row.get("runtime_availability", {}).get("runtime_available_static_input") is not False:
                errors.append("reference_emulated runtime availability must be false")
            if row.get("storage_authority_status", {}).get("status") != "blocked":
                errors.append("reference_emulated storage authority must be blocked")
        if row.get("enum_name") == "policy_blocked":
            if row.get("provider_classification") != "policy_blocked_non_runtime":
                errors.append("policy_blocked classification must remain non-runtime")
            if row.get("runtime_availability", {}).get("runtime_available_static_input") is not False:
                errors.append("policy_blocked runtime availability must be false")
            if row.get("storage_authority_status", {}).get("status") != "blocked":
                errors.append("policy_blocked storage authority must be blocked")
        route_summary = row.get("route_capability_summary", {})
        for field in (
            "supported_routes",
            "unsupported_routes",
            "requires_exact_recheck",
            "requires_mga_recheck",
            "requires_security_recheck",
            "requires_exact_fallback",
            "supports_negative_prune",
            "supports_summary_segment_prune",
            "row_truth_authority",
            "final_row_authority",
            "routes",
        ):
            if field not in route_summary:
                errors.append(f"{row.get('family_id')} route_capability_summary missing {field}")
    return errors


def validate_manifest_semantics(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if data.get("schema_id") != "scratchbird.ceic.evidence_manifest":
        errors.append("schema_id mismatch")
    if data.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    if data.get("manifest_kind") != "index_readiness_manifest":
        errors.append("manifest_kind mismatch")
    if data.get("search_key") != "CEIC_030_INDEX_READINESS_MANIFEST":
        errors.append("search_key mismatch")
    successor_statuses = data.get("index_successor_slice_statuses")
    ceic_042_complete = False
    if not isinstance(successor_statuses, list):
        errors.append("index_successor_slice_statuses is required")
    else:
        seen = {row.get("slice_id"): row.get("status") for row in successor_statuses if isinstance(row, dict)}
        ceic_042_complete = seen.get("CEIC-042") in COMPLETE_STATUS
        for slice_id in INDEX_FUTURE_SLICES:
            if seen.get(slice_id) not in COMPLETE_STATUS | PENDING_STATUS:
                errors.append(f"{slice_id} successor status must be pending or complete")
        pending = data.get("future_index_slices_pending")
        if not isinstance(pending, list):
            errors.append("future_index_slices_pending must be a list")
        else:
            expected_pending = [
                slice_id for slice_id in INDEX_FUTURE_SLICES
                if seen.get(slice_id) in PENDING_STATUS
            ]
            if pending != expected_pending:
                errors.append("future_index_slices_pending must match successor pending statuses")
    expected_slices = ["CEIC-030", "CEIC-042"] if ceic_042_complete else ["CEIC-030"]
    if data.get("slice_ids") != expected_slices:
        errors.append(f"slice_ids must be {expected_slices}")
    integrated_statuses = data.get("integrated_boundary_slice_statuses")
    if not isinstance(integrated_statuses, list):
        errors.append("integrated_boundary_slice_statuses is required")
    else:
        integrated_seen = {
            row.get("slice_id"): row.get("status")
            for row in integrated_statuses
            if isinstance(row, dict)
        }
        for slice_id in INTEGRATED_INDEX_BOUNDARY_SLICES:
            if integrated_seen.get(slice_id) not in PENDING_STATUS:
                errors.append(f"{slice_id} must remain pending integrated proof")
    for field in ("source_commit", "source_branch", "source_evidence_digest", "generated_at_utc", "generated_by"):
        if not data.get(field):
            errors.append(f"{field} is required")
    if not data.get("auditor", {}).get("timestamp_utc"):
        errors.append("auditor timestamp is required")
    if data.get("authority_boundary", {}).get("boundary_token") != AUTHORITY_BOUNDARY_TOKEN:
        errors.append("authority boundary token mismatch")
    for key, value in data.get("authority_boundary", {}).items():
        if key.endswith("_authority") or key == "static_registry_ready_authority":
            if value is not False:
                errors.append(f"{key} must be false")
    readiness = data.get("readiness_policy", {})
    if readiness.get("cluster_production_claim") != "blocked_external_provider_only":
        errors.append("cluster production must remain external-provider-only")
    if readiness.get("ceic_038_scope") != "lossy_candidate_pruning_family_classification_only_no_provider_metrics_crash_drift_readiness_claim":
        errors.append("CEIC-038 scope must remain classification-only")
    expected_ceic_042_scope = CEIC_042_SCOPE_COMPLETE if ceic_042_complete else CEIC_042_SCOPE_PENDING
    if readiness.get("ceic_042_scope") != expected_ceic_042_scope:
        errors.append("CEIC-042 scope state mismatch")
    drift = data.get("readiness_drift_gate_evidence", {})
    if not isinstance(drift, dict):
        errors.append("readiness_drift_gate_evidence is required")
    else:
        expected_state = "complete" if ceic_042_complete else "pending"
        if drift.get("state") != expected_state:
            errors.append("CEIC-042 readiness drift gate state mismatch")
        if drift.get("static_or_smoke_only") is not False:
            errors.append("CEIC-042 readiness drift gate must reject static/smoke-only proof")
        if drift.get("missing_family_manifest_count") != 0:
            errors.append("CEIC-042 readiness drift gate must not allow missing family manifests")
        if drift.get("stale_manifest_allowed") is not False:
            errors.append("CEIC-042 readiness drift gate must not allow stale manifests")
        for key in (
            "reference_dominance_claimed",
            "all_index_readiness_claimed",
            "enterprise_readiness_claimed",
        ):
            if drift.get(key) is not False:
                errors.append(f"CEIC-042 {key} must be false")
        if drift.get("cluster_production_claim") != "blocked_external_provider_only":
            errors.append("CEIC-042 cluster routes must remain external-provider-only")
    errors.extend(validate_family_semantics(data))
    return errors


def validate_manifest(repo_root: pathlib.Path, manifest_path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    if not manifest_path.exists():
        return [f"manifest missing: {manifest_path}"]
    try:
        actual = load_manifest(manifest_path)
    except ManifestError as exc:
        return [str(exc)]
    errors.extend(validate_manifest_semantics(actual))

    carried = {
        "source_commit": str(actual.get("source_commit", "")),
        "source_branch": str(actual.get("source_branch", "")),
        "generated_at_utc": str(actual.get("generated_at_utc", "")),
    }
    try:
        expected = build_manifest(repo_root, writing=False, carried=carried)
    except ManifestError as exc:
        errors.append(str(exc))
        return errors

    if render_json(actual) != render_json(expected):
        errors.append("stale manifest differs from generated CEIC-030 proof")
    return errors


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--write", action="store_true", help="write the generated manifest before validating it")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest
    if not manifest_path.is_absolute():
        manifest_path = repo_root / manifest_path

    try:
        if args.write:
            data = build_manifest(repo_root, writing=True)
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            manifest_path.write_text(render_json(data), encoding="utf-8")
        errors = validate_manifest(repo_root, manifest_path)
    except ManifestError as exc:
        errors = [str(exc)]

    if errors:
        for error in errors:
            print(f"ceic_index_readiness_manifest=fail:{error}", file=sys.stderr)
        return 1
    try:
        display_path = manifest_path.relative_to(repo_root).as_posix()
    except ValueError:
        display_path = manifest_path.as_posix()
    print(f"ceic_index_readiness_manifest=pass:{display_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
