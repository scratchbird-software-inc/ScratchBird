#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-013N existing lifecycle reconciliation audit.

The audit is intentionally static: it reconciles the execution_plan bookkeeping,
CTest label materialization, lifecycle surface inventory, and authority-drift
signals without executing engine behavior.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable


EXECUTION_PLAN = Path("project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure")
ARTIFACTS = EXECUTION_PLAN / "artifacts"
TRACKER = EXECUTION_PLAN / "TRACKER.csv"
GATES = ARTIFACTS / "DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv"
QUEUE = ARTIFACTS / "DATABASE_LIFECYCLE_SLICE_EXECUTION_QUEUE.csv"
AGENT_STATUS = ARTIFACTS / "DATABASE_LIFECYCLE_AGENT_STATUS.csv"
GAP_MATRIX = ARTIFACTS / "DATABASE_LIFECYCLE_IMPLEMENTATION_GAP_MATRIX.csv"
DEFAULT_REPORT = ARTIFACTS / "DATABASE_LIFECYCLE_RECONCILIATION_REPORT.md"

RECONCILED_SLICES = (
    "DBLC-003",
    "DBLC-004",
    "DBLC-004A",
    "DBLC-005",
    "DBLC-006",
    "DBLC-007",
    "DBLC-008",
    "DBLC-009",
    "DBLC-010",
    "DBLC-011",
    "DBLC-012",
    "DBLC-013",
    "DBLC-013A",
    "DBLC-013B",
    "DBLC-013C",
    "DBLC-013D",
    "DBLC-013E",
    "DBLC-013F",
    "DBLC-013G",
    "DBLC-013H",
    "DBLC-013I",
    "DBLC-013J",
    "DBLC-013K",
    "DBLC-013L",
    "DBLC-013M",
    "DBLC-013U",
    "DBLC-013U1",
    "DBLC-013AA",
    "DBLC-013V",
    "DBLC-013W",
    "DBLC-013X",
    "DBLC-013Y",
    "DBLC-013Z",
    "DBLC-013AB",
    "DBLC-013AC",
    "DBLC-013AD",
    "DBLC-013AE",
    "DBLC-013AF",
    "DBLC-013AG",
    "DBLC-013AH",
    "DBLC-013AI",
    "DBLC-013AJ",
    "DBLC-013AK",
)

EXPECTED_DBLC_013N_LABELS = {
    "database_lifecycle_existing_reconciliation",
    "DBLC_STATIC_NO_LEGACY_LIFECYCLE_DRIFT",
}

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inc",
    ".py",
    ".rs",
    ".go",
    ".java",
    ".kt",
    ".cs",
    ".ts",
    ".js",
    ".dart",
    ".rb",
    ".php",
    ".r",
}

SOURCE_ROOTS = (
    Path("project/src"),
    Path("project/include"),
    Path("project/drivers"),
)

EXCLUDED_SOURCE_PARTS = {
    "generated",
    "third_party",
    "vendor",
    "node_modules",
    ".dart_tool",
    ".build",
    "build",
}


@dataclass(frozen=True)
class SurfaceRequirement:
    name: str
    slices: tuple[str, ...]
    source_paths: tuple[str, ...]
    test_paths: tuple[str, ...]
    labels: tuple[str, ...]
    tokens: tuple[str, ...]


@dataclass
class Finding:
    severity: str
    code: str
    location: str
    detail: str


SURFACES = (
    SurfaceRequirement(
        "manager",
        ("DBLC-013A",),
        ("project/src/manager/node/manager_runtime.cpp", "project/src/manager/node/manager_lifecycle.cpp"),
        ("project/tests/manager/database_lifecycle_manager_conformance.py",),
        ("database_lifecycle_manager",),
        ("RunManager", "ManagerLifecycle"),
    ),
    SurfaceRequirement(
        "listener",
        ("DBLC-013B",),
        ("project/src/listener/listener_runtime.cpp", "project/src/listener/parser_pool.cpp"),
        ("project/tests/listener/database_lifecycle_listener_conformance.py",),
        ("database_lifecycle_listener",),
        ("listener", "parser_pool"),
    ),
    SurfaceRequirement(
        "parser",
        ("DBLC-013C",),
        (
            "project/src/parsers/sbsql_worker/lifecycle/parser_lifecycle.cpp",
            "project/src/parsers/sbsql_worker/runtime/parser_runtime.cpp",
            "project/src/server/parser_package_registry.cpp",
        ),
        ("project/tests/sbsql_parser_worker/sbsql_database_lifecycle_parser_conformance.cpp",),
        ("database_lifecycle_parser",),
        ("ParserLifecycle", "no_authority_bypass"),
    ),
    SurfaceRequirement(
        "server daemon",
        ("DBLC-013E",),
        ("project/src/server/server_daemon_lifecycle.cpp", "project/src/server/engine_host.cpp"),
        ("project/tests/database_lifecycle/server_daemon_lifecycle_conformance.cpp",),
        ("database_lifecycle_server_daemon",),
        ("HostedDatabase", "service_ready"),
    ),
    SurfaceRequirement(
        "server IPC",
        ("DBLC-013F",),
        ("project/src/server/server_ipc_lifecycle.cpp", "project/src/server/ipc_server.cpp"),
        ("project/tests/database_lifecycle/ipc_lifecycle_conformance.cpp",),
        ("database_lifecycle_ipc",),
        ("EndpointLifecycle", "auth"),
    ),
    SurfaceRequirement(
        "session request cursor",
        ("DBLC-013G", "DBLC-009"),
        ("project/src/server/session_registry.cpp", "project/src/server/sblr_dispatch_server.cpp"),
        ("project/tests/database_lifecycle/session_request_cursor_conformance.cpp",),
        ("database_lifecycle_session_request_cursor",),
        ("unknown_outcome", "cursor"),
    ),
    SurfaceRequirement(
        "process association",
        ("DBLC-013D",),
        ("project/src/server/process_association_registry.cpp", "project/src/server/maintenance_coordinator.cpp"),
        ("project/tests/database_lifecycle/process_association_conformance.cpp",),
        ("database_lifecycle_process_association",),
        ("shutdown_generation", "database_uuid"),
    ),
    SurfaceRequirement(
        "filespace",
        ("DBLC-004A",),
        ("project/src/storage/database/database_lifecycle.cpp",),
        ("project/tests/database_lifecycle/filespace_lifecycle_conformance.cpp",),
        ("database_lifecycle_filespace",),
        ("filespace_uuid", "manifest"),
    ),
    SurfaceRequirement(
        "catalog object",
        ("DBLC-013U",),
        ("project/src/engine/internal_api/catalog/catalog_object_lifecycle.cpp",),
        ("project/tests/database_lifecycle/catalog_object_conformance.cpp",),
        ("database_lifecycle_catalog_object",),
        ("CatalogObjectLifecycle", "mga"),
    ),
    SurfaceRequirement(
        "catalog index and projection",
        ("DBLC-013U1",),
        (
            "project/src/core/catalog/catalog_index_profile.cpp",
            "project/src/engine/internal_api/catalog/sys_information_projection.cpp",
        ),
        (
            "project/tests/database_lifecycle/catalog_index_profile_conformance.cpp",
            "project/tests/database_lifecycle/sys_information_projection_conformance.cpp",
        ),
        ("database_lifecycle_catalog_index_profile", "database_lifecycle_sys_information_projection"),
        ("identity_resolver", "sys.information"),
    ),
    SurfaceRequirement(
        "index statistics and optimizer plan",
        ("DBLC-013V",),
        (
            "project/src/core/index/index_statistics_lifecycle.cpp",
            "project/src/engine/internal_api/query/optimizer_plan_lifecycle.cpp",
        ),
        ("project/tests/database_lifecycle/index_statistics_plan_conformance.cpp",),
        ("database_lifecycle_index_statistics_plan",),
        ("PlanCache", "statistics"),
    ),
    SurfaceRequirement(
        "concurrency lock wait",
        ("DBLC-013W",),
        ("project/src/transaction/mga/lock_wait_lifecycle.cpp",),
        ("project/tests/database_lifecycle/lock_wait_deadlock_conformance.cpp",),
        ("database_lifecycle_lock_wait_deadlock",),
        ("deadlock", "MGAWait"),
    ),
    SurfaceRequirement(
        "temporary workspace",
        ("DBLC-013X",),
        ("project/src/core/memory/temp_workspace_lifecycle.cpp",),
        ("project/tests/database_lifecycle/temp_workspace_conformance.cpp",),
        ("database_lifecycle_temp_workspace",),
        ("TempWorkspace", "quota"),
    ),
    SurfaceRequirement(
        "event notification",
        ("DBLC-013Y",),
        ("project/src/engine/internal_api/notification/notification_api.cpp", "project/src/server/event_notification_router.cpp"),
        ("project/tests/database_lifecycle/event_notification_conformance.cpp",),
        ("database_lifecycle_event_notification",),
        ("notification", "subscription"),
    ),
    SurfaceRequirement(
        "encryption protected material",
        ("DBLC-013Z",),
        ("project/src/engine/internal_api/security/protected_material_api.cpp",),
        ("project/tests/database_lifecycle/encryption_key_conformance.cpp",),
        ("database_lifecycle_encryption_key",),
        ("protected_material", "redact"),
    ),
    SurfaceRequirement(
        "resource seed",
        ("DBLC-013AA",),
        ("project/src/core/resources/resource_seed_pack.cpp", "project/src/storage/database/database_lifecycle.cpp"),
        ("project/tests/database_lifecycle/resource_seed_conformance.cpp",),
        ("database_lifecycle_resource_seed",),
        ("resource_seed", "runtime_cache_epoch"),
    ),
    SurfaceRequirement(
        "MGA GC retention",
        ("DBLC-013AB",),
        ("project/src/transaction/mga/transaction_cleanup.cpp",),
        ("project/tests/database_lifecycle/mga_gc_retention_conformance.cpp",),
        ("database_lifecycle_mga_gc_retention",),
        ("cleanup", "retention"),
    ),
    SurfaceRequirement(
        "background jobs",
        ("DBLC-013AC",),
        ("project/src/core/agents/agent_background_jobs.cpp",),
        ("project/tests/database_lifecycle/jobs_scheduler_conformance.cpp",),
        ("database_lifecycle_jobs_scheduler",),
        ("JobScheduler", "quarantine"),
    ),
    SurfaceRequirement(
        "cluster boundary",
        ("DBLC-013AD",),
        (
            "project/src/engine/internal_api/cluster/cluster_insert_route_api.cpp",
            "project/src/engine/internal_api/cluster/remote_participant_insert_api.cpp",
            "project/src/core/index/index_transaction.cpp",
        ),
        ("project/tests/database_lifecycle/cluster_boundary_conformance.cpp",),
        ("database_lifecycle_cluster_boundary",),
        ("CLUSTER_AUTHORITY", "fail"),
    ),
    SurfaceRequirement(
        "security principal",
        ("DBLC-013AE",),
        ("project/src/engine/internal_api/security/security_principal_lifecycle.cpp",),
        ("project/tests/database_lifecycle/security_principal_conformance.cpp",),
        ("database_lifecycle_security_principal",),
        ("SecurityPrincipal", "cache_invalidation_epoch"),
    ),
    SurfaceRequirement(
        "storage allocation",
        ("DBLC-013AF",),
        ("project/src/storage/page/page_allocation_lifecycle.cpp",),
        ("project/tests/database_lifecycle/storage_allocation_conformance.cpp",),
        ("database_lifecycle_storage_allocation",),
        ("PageAllocation", "MGA"),
    ),
    SurfaceRequirement(
        "executable object",
        ("DBLC-013AG",),
        ("project/src/engine/internal_api/extensibility/executable_object_lifecycle.cpp",),
        ("project/tests/database_lifecycle/executable_object_conformance.cpp",),
        ("database_lifecycle_executable_object",),
        ("ExecutableObject", "SBLR"),
    ),
    SurfaceRequirement(
        "sequence generator",
        ("DBLC-013AH",),
        ("project/src/engine/internal_api/lifecycle/sequence_generator_lifecycle.cpp",),
        ("project/tests/database_lifecycle/sequence_generator_conformance.cpp",),
        ("database_lifecycle_sequence_generator",),
        ("Sequence", "reference_mapping"),
    ),
    SurfaceRequirement(
        "supportability evidence",
        ("DBLC-013AI",),
        ("project/src/server/server_observability.cpp", "project/src/engine/internal_api/management/support_bundle_api.cpp"),
        ("project/tests/database_lifecycle/supportability_evidence_conformance.cpp",),
        ("database_lifecycle_supportability_evidence",),
        ("support_bundle", "redact"),
    ),
    SurfaceRequirement(
        "capability profile",
        ("DBLC-013AJ",),
        ("project/src/core/agents/agent_feature_gates.cpp", "project/src/server/parser_package_registry.cpp"),
        ("project/tests/database_lifecycle/capability_profile_conformance.cpp",),
        ("database_lifecycle_capability_profile",),
        ("Capability", "POLICY_EPOCH"),
    ),
    SurfaceRequirement(
        "replication boundary",
        ("DBLC-013AK",),
        ("project/src/engine/internal_api/cluster/replication_api.cpp",),
        ("project/tests/database_lifecycle/replication_boundary_conformance.cpp",),
        ("database_lifecycle_replication_boundary",),
        ("replication", "standalone"),
    ),
    SurfaceRequirement(
        "UDR extension",
        ("DBLC-013L",),
        ("project/src/engine/internal_api/extensibility/udr_api.cpp", "project/src/udr/runtime/sb_udr_runtime.cpp"),
        ("project/tests/database_lifecycle/udr_extension_conformance.cpp",),
        ("database_lifecycle_udr_extension",),
        ("UDR", "policy"),
    ),
    SurfaceRequirement(
        "database engine agent",
        ("DBLC-013H",),
        ("project/src/core/agents/agent_engine_lifecycle.cpp",),
        ("project/tests/database_lifecycle/engine_agent_conformance.cpp",),
        ("database_lifecycle_engine_agent",),
        ("EngineAgent", "authority_boundary"),
    ),
    SurfaceRequirement(
        "cache checkpoint",
        ("DBLC-013I",),
        ("project/src/storage/page/page_cache.cpp",),
        ("project/tests/database_lifecycle/cache_checkpoint_conformance.cpp",),
        ("database_lifecycle_cache_checkpoint",),
        ("checkpoint", "finality"),
    ),
    SurfaceRequirement(
        "configuration policy security",
        ("DBLC-013J", "DBLC-007"),
        ("project/src/server/config_policy_security_lifecycle.cpp", "project/src/engine/internal_api/security/security_model.cpp"),
        ("project/tests/database_lifecycle/config_policy_security_provider_conformance.cpp",),
        ("database_lifecycle_config_policy_security_provider", "database_lifecycle_security"),
        ("policy_epoch", "security"),
    ),
    SurfaceRequirement(
        "backup restore resource workload",
        ("DBLC-013K", "DBLC-013M"),
        (
            "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp",
            "project/src/core/agents/agent_workload_resource_quota.cpp",
        ),
        (
            "project/tests/database_lifecycle/backup_archive_restore_conformance.cpp",
            "project/tests/database_lifecycle/workload_resource_conformance.cpp",
        ),
        ("database_lifecycle_backup_archive_restore", "database_lifecycle_workload_resource"),
        ("backup", "quota"),
    ),
)


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def index_by(rows: Iterable[dict[str, str]], key: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row.get(key, "")
        if value:
            out[value] = row
    return out


def add(findings: list[Finding], severity: str, code: str, location: str, detail: str) -> None:
    findings.append(Finding(severity, code, location, detail))


def split_semicolon(value: str) -> list[str]:
    return [part.strip() for part in value.split(";") if part.strip()]


def is_source_file(path: Path) -> bool:
    if path.suffix.lower() not in SOURCE_SUFFIXES:
        return False
    return not any(part in EXCLUDED_SOURCE_PARTS for part in path.parts)


def path_exists(root: Path, rel: str) -> bool:
    return (root / rel).exists()


def path_text(root: Path, rel: str) -> str:
    return (root / rel).read_text(encoding="utf-8", errors="replace")


def collect_cmake_labels(root: Path) -> set[str]:
    labels: set[str] = set()
    cmake_paths = list((root / "project/tests").rglob("CMakeLists.txt"))
    cmake_paths.extend((root / "project").glob("CMakeLists.txt"))
    label_string = re.compile(r"(?:LABELS|APPEND PROPERTY LABELS)\s+\"([^\"]+)\"", re.MULTILINE)
    bare_label_block = re.compile(r"APPEND PROPERTY LABELS\s+([A-Za-z0-9_\-\s]+)\)", re.MULTILINE)
    add_test = re.compile(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_\-]+)", re.MULTILINE)
    for path in cmake_paths:
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in label_string.finditer(text):
            labels.update(split_semicolon(match.group(1)))
            labels.update(match.group(1).split())
        for match in bare_label_block.finditer(text):
            labels.update(match.group(1).split())
        for match in add_test.finditer(text):
            labels.add(match.group(1))
    return labels


def collect_critical_source_paths(root: Path, tracker_rows: dict[str, dict[str, str]]) -> list[Path]:
    paths: set[Path] = set()
    for surface in SURFACES:
        for rel in surface.source_paths:
            path = root / rel
            if path.is_file() and is_source_file(path):
                paths.add(path)
    for slice_id in RECONCILED_SLICES:
        row = tracker_rows.get(slice_id)
        if not row:
            continue
        for rel in split_semicolon(row.get("outputs", "")):
            if not rel.startswith("project/"):
                continue
            if not any(rel.startswith(source_root.as_posix() + "/") for source_root in SOURCE_ROOTS):
                continue
            path = root / rel
            if path.is_file() and is_source_file(path):
                paths.add(path)
            elif path.is_dir():
                for child in path.rglob("*"):
                    if child.is_file() and is_source_file(child):
                        paths.add(child)
    return sorted(paths)


def collect_authority_scan_paths(root: Path, critical_paths: Iterable[Path]) -> list[Path]:
    paths: set[Path] = set()
    paths.update(critical_paths)
    for source_root in (Path("project/src/parsers"), Path("project/src/udr"), Path("project/drivers/driver"), Path("project/drivers/adaptor")):
        base = root / source_root
        if not base.exists():
            continue
        for child in base.rglob("*"):
            if child.is_file() and is_source_file(child):
                paths.add(child)
    return sorted(paths)


def relpath(root: Path, path: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


MARKER_RE = re.compile(
    r"\b(?:TODO|FIXME|XXX|NotImplemented)\b|"
    r"\bplaceholder\b|"
    r"\bstub\b|"
    r"deferred_to_successor|"
    r"implementation\s+deferred|"
    r"future\s+work",
    re.IGNORECASE,
)

AUTHORITY_SHORTCUT_RE = re.compile(
    r"(\bWAL\b|write-ahead|\bredo\b|\bundo\b|SQLite|sqlite3|PRAGMA|journal_mode|reference).{0,96}"
    r"(recovery|finality|authority|backend|storage|transaction|commit|rollback)|"
    r"(recovery|finality|authority|backend|storage|transaction|commit|rollback).{0,96}"
    r"(\bWAL\b|write-ahead|\bredo\b|\bundo\b|SQLite|sqlite3|PRAGMA|journal_mode|reference)",
    re.IGNORECASE,
)

PARSER_DRIVER_DRIFT_RE = re.compile(
    r"sqlite3_|duckdb_|rocksdb_|leveldb_|"
    r"PRAGMA\s+journal_mode|journal_mode\s*=|"
    r"transaction_inventory\.hpp|local_transaction_store\.hpp|"
    r"engine\s+owns\s+commit|parser\s+owns\s+commit|driver\s+owns\s+commit|"
    r"parser\s+finality|driver\s+finality|reference\s+finality",
    re.IGNORECASE,
)


def allowed_marker(path: Path, line: str) -> bool:
    lowered = line.lower()
    if "placeholder" in lowered and (
        "parameter placeholder" in lowered
        or "placeholders" in lowered
        or "countplaceholders" in lowered
        or "$1" in lowered
        or "prepared" in lowered
    ):
        return True
    if "stub" in lowered and ("/tests/" in path.as_posix() or "test_" in path.name):
        return True
    if "deferred" in lowered and (
        "deferrable" in lowered
        or "deferred_epoch" in lowered
        or "unique_deferred" in lowered
        or "deferred secondary-index" in lowered
        or "job_deferred_for_maintenance" in lowered
        or "action_deferred" in lowered
        or "defer {" in lowered
    ):
        return True
    return False


def allowed_authority_shortcut(path: Path, line: str) -> bool:
    rel = path.as_posix()
    lowered = line.lower()
    if "reference_finality_authority" in lowered and "false" in lowered:
        return True
    if any(
        token in lowered
        for token in (
            "not authority",
            "not_authority",
            "not finality",
            "not_finality",
            "never redo authority",
            "never recovery authority",
            "forbidden",
            "refused",
            "denied",
            "bypass",
            "not_engine",
            "evidence only",
            "classification evidence only",
            "rather than inheriting reference shortcuts",
            "not durable authority",
            "no wal",
            "no_wal",
            "wal_not_authority",
            "wal_or_redo_authority",
            "authoritative_wal",
            "authority:reference",
            "authority:sqlite",
            "reference_shortcut",
            "sqlite_shortcut",
            "zero reference file effects",
        )
    ):
        return True
    if "reference_" in lowered and (
        "mapping" in lowered
        or "_behavior" in lowered
        or "compat" in lowered
        or "profile" in lowered
    ):
        return True
    if "wal" in lowered and (
        "not authority" in lowered
        or "forbidden" in lowered
        or "must not contain" in lowered
        or "not part of" in lowered
        or "avoid wal framing" in lowered
        or "no wal" in lowered
        or "no_wal" in lowered
    ):
        return True
    if "reference" in lowered and (
        "not authority" in lowered
        or "alias only" in lowered
        or "compatibility" in lowered
        or "requires engine policy" in lowered
        or "fails closed" in lowered
        or "rather than inheriting reference shortcuts" in lowered
    ):
        return True
    if "project/drivers/driver/odbc/src/odbc_driver.cpp" in rel and "sqlexecdirect" in lowered:
        return True
    if "project/drivers/driver/odbc/include" in rel and "sqlexecdirect" in lowered:
        return True
    return False


def validate_csv_inputs(root: Path, findings: list[Finding]) -> tuple[
    dict[str, dict[str, str]],
    dict[str, dict[str, str]],
    dict[str, dict[str, str]],
    dict[str, dict[str, str]],
    dict[str, dict[str, str]],
]:
    required = (TRACKER, GATES, QUEUE, AGENT_STATUS, GAP_MATRIX)
    for rel in required:
        path = root / rel
        if not path.is_file():
            add(findings, "fatal", "DBLC013N.MISSING_INPUT", rel.as_posix(), "required reconciliation input is missing")
    if any(f.code == "DBLC013N.MISSING_INPUT" for f in findings):
        empty: dict[str, dict[str, str]] = {}
        return empty, empty, empty, empty, empty
    return (
        index_by(read_csv(root / TRACKER), "slice_id"),
        index_by(read_csv(root / GATES), "slice_id"),
        index_by(read_csv(root / QUEUE), "slice_id"),
        index_by(read_csv(root / AGENT_STATUS), "slice_id"),
        index_by(read_csv(root / GAP_MATRIX), "slice_id"),
    )


def validate_execution_plan_status(
    tracker: dict[str, dict[str, str]],
    gates: dict[str, dict[str, str]],
    queue: dict[str, dict[str, str]],
    agent_status: dict[str, dict[str, str]],
    labels: set[str],
    findings: list[Finding],
) -> None:
    for slice_id in RECONCILED_SLICES:
        tracker_row = tracker.get(slice_id)
        if not tracker_row:
            add(findings, "fatal", "DBLC013N.TRACKER_ROW_MISSING", slice_id, "reconciled slice is absent from TRACKER.csv")
            continue
        if tracker_row.get("status") != "passed":
            add(findings, "fatal", "DBLC013N.TRACKER_NOT_PASSED", slice_id, f"tracker status is {tracker_row.get('status')!r}")
        gate_row = gates.get(slice_id)
        if not gate_row:
            add(findings, "fatal", "DBLC013N.GATE_ROW_MISSING", slice_id, "slice is absent from CTest required gates")
        else:
            for label in split_semicolon(gate_row.get("ctest_labels", "")):
                if label and label not in labels:
                    add(findings, "fatal", "DBLC013N.CTEST_LABEL_MISSING", slice_id, f"CTest label {label!r} is not materialized")
        queue_row = queue.get(slice_id)
        if not queue_row:
            add(findings, "fatal", "DBLC013N.QUEUE_ROW_MISSING", slice_id, "slice is absent from execution queue")
        elif queue_row.get("status") != "validation_passed":
            add(
                findings,
                "fatal",
                "DBLC013N.QUEUE_STATUS_STALE",
                slice_id,
                f"queue status is {queue_row.get('status')!r}, expected validation_passed",
            )
        agent_row = agent_status.get(slice_id)
        if not agent_row:
            add(findings, "warn", "DBLC013N.AGENT_STATUS_ROW_MISSING", slice_id, "slice is absent from agent status rollup")
        elif agent_row.get("status") not in {"validation_passed", "completed_scope_released"}:
            add(
                findings,
                "fatal",
                "DBLC013N.AGENT_STATUS_STALE",
                slice_id,
                f"agent status is {agent_row.get('status')!r}",
            )

    dblc013n_gate = gates.get("DBLC-013N")
    if not dblc013n_gate:
        add(findings, "fatal", "DBLC013N.SELF_GATE_ROW_MISSING", "DBLC-013N", "DBLC-013N gate row is missing")
    else:
        declared = set(split_semicolon(dblc013n_gate.get("ctest_labels", "")))
        declared.update(split_semicolon(dblc013n_gate.get("static_or_audit_gates", "")))
        missing = EXPECTED_DBLC_013N_LABELS - declared
        for label in sorted(missing):
            add(findings, "fatal", "DBLC013N.SELF_GATE_INCOMPLETE", "DBLC-013N", f"DBLC-013N gate row lacks {label}")
        if "database_lifecycle_existing_reconciliation" not in labels:
            add(
                findings,
                "warn",
                "DBLC013N.CMAKE_SNIPPET_REQUIRED",
                "project/tests/database_lifecycle/CMakeLists.txt",
                "database_lifecycle_existing_reconciliation label is not materialized until the returned CMake snippet is applied",
            )


def validate_surface_inventory(root: Path, labels: set[str], findings: list[Finding]) -> None:
    for surface in SURFACES:
        for rel in surface.source_paths:
            if not path_exists(root, rel):
                add(findings, "fatal", "DBLC013N.SURFACE_SOURCE_MISSING", surface.name, rel)
        for rel in surface.test_paths:
            if not path_exists(root, rel):
                add(findings, "fatal", "DBLC013N.SURFACE_TEST_MISSING", surface.name, rel)
        for label in surface.labels:
            if label not in labels:
                add(findings, "fatal", "DBLC013N.SURFACE_LABEL_MISSING", surface.name, label)
        combined = ""
        for rel in surface.source_paths:
            if path_exists(root, rel) and (root / rel).is_file():
                combined += "\n" + path_text(root, rel)
        for token in surface.tokens:
            if token.lower() not in combined.lower():
                add(findings, "fatal", "DBLC013N.SURFACE_TOKEN_MISSING", surface.name, f"{token} absent from source evidence")


def validate_gap_matrix(gap: dict[str, dict[str, str]], findings: list[Finding]) -> None:
    for slice_id in RECONCILED_SLICES:
        row = gap.get(slice_id)
        if not row:
            add(findings, "fatal", "DBLC013N.GAP_ROW_MISSING", slice_id, "slice is absent from implementation gap matrix")
            continue
        if row.get("current_implementation_status") in {"pending", "partial_or_not_started_current_code_evidence"}:
            add(
                findings,
                "warn",
                "DBLC013N.GAP_STATUS_STALE",
                slice_id,
                f"gap matrix status is {row.get('current_implementation_status')!r}; coordinator must update shared gap matrix after DBLC-013N",
            )
        if row.get("test_status") in {"ctest_required", "artifact_validation_required"}:
            add(findings, "warn", "DBLC013N.GAP_TEST_STATUS_STALE", slice_id, f"test_status is {row.get('test_status')!r}")
        if "reconciliation_pending" in row.get("current_implementation_status", ""):
            add(
                findings,
                "warn",
                "DBLC013N.GAP_RECONCILIATION_NOTE",
                slice_id,
                "gap matrix intentionally records reconciliation_pending; DBLC-013N report supersedes this note once coordinator updates shared files",
            )


def scan_markers(root: Path, critical_paths: list[Path], findings: list[Finding]) -> None:
    for path in critical_paths:
        for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
            if MARKER_RE.search(line) and not allowed_marker(path, line):
                add(
                    findings,
                    "fatal",
                    "DBLC013N.LIFECYCLE_MARKER",
                    f"{relpath(root, path)}:{lineno}",
                    line.strip(),
                )


def scan_authority_shortcuts(root: Path, paths: list[Path], findings: list[Finding]) -> None:
    for path in paths:
        rel = relpath(root, path)
        for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
            if AUTHORITY_SHORTCUT_RE.search(line) and not allowed_authority_shortcut(path, line):
                add(findings, "fatal", "DBLC013N.AUTHORITY_SHORTCUT", f"{rel}:{lineno}", line.strip())


def scan_parser_driver_drift(root: Path, findings: list[Finding]) -> None:
    scan_roots = (
        root / "project/src/parsers",
        root / "project/src/udr",
        root / "project/drivers",
    )
    for base in scan_roots:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or not is_source_file(path):
                continue
            rel = relpath(root, path)
            if "/tests/" in rel or "/test/" in rel or "generated/" in rel or "vendor/" in rel:
                continue
            for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
                if PARSER_DRIVER_DRIFT_RE.search(line) and not allowed_authority_shortcut(path, line):
                    add(findings, "fatal", "DBLC013N.PARSER_DRIVER_AUTHORITY_DRIFT", f"{rel}:{lineno}", line.strip())


def write_report(
    root: Path,
    report_path: Path,
    findings: list[Finding],
    critical_paths: list[Path],
    authority_paths: list[Path],
    labels: set[str],
) -> None:
    fatal_count = sum(1 for finding in findings if finding.severity == "fatal")
    warn_count = sum(1 for finding in findings if finding.severity == "warn")
    status = "passed" if fatal_count == 0 else "failed"
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    lines = [
        "# Database Lifecycle Reconciliation Report",
        "",
        f"Generated: `{now}`",
        "Slice: `DBLC-013N`",
        f"Status: `{status}`",
        "",
        "## Scope",
        "",
        "This report reconciles DBLC-013A through DBLC-013AK plus the earlier lifecycle surfaces they depend on: manager, listener, parser, server daemon, IPC, process associations, sessions, filespaces, catalog, index, concurrency, temporary workspace, event notification, encryption, resource seed, MGA GC, jobs, cluster boundary, security principal, storage allocation, executable objects, sequences, supportability, capability, replication, UDR, agent, cache, configuration/security, backup, resource, and workload surfaces.",
        "",
        "## Audit Summary",
        "",
        f"- Fatal findings: `{fatal_count}`",
        f"- Warnings: `{warn_count}`",
        f"- Critical source files scanned for lifecycle markers: `{len(critical_paths)}`",
        f"- Source/driver/parser files scanned for authority shortcuts: `{len(authority_paths)}`",
        f"- Materialized CTest labels observed: `{len(labels)}`",
        "",
        "## Findings",
        "",
    ]
    if not findings:
        lines.append("No findings.")
    else:
        lines.append("| Severity | Code | Location | Detail |")
        lines.append("| --- | --- | --- | --- |")
        for finding in sorted(findings, key=lambda f: (f.severity != "fatal", f.code, f.location)):
            detail = finding.detail.replace("|", "\\|")
            location = finding.location.replace("|", "\\|")
            lines.append(f"| `{finding.severity}` | `{finding.code}` | `{location}` | {detail} |")
    lines.extend(["", "## CMake Integration", ""])
    if "database_lifecycle_existing_reconciliation" in labels:
        lines.extend(
            [
                "`database_lifecycle_existing_reconciliation` is materialized in CTest with the `DBLC_STATIC_NO_LEGACY_LIFECYCLE_DRIFT` static gate label.",
                "",
            ]
        )
    else:
        lines.extend(
            [
                "The worker-owned files are present, but the shared CMake registration has not been applied. Apply this snippet in `project/tests/database_lifecycle/CMakeLists.txt` after `SB_PRIVATE_REPO_ROOT` is defined:",
                "",
                "```cmake",
                "set(SB_DATABASE_LIFECYCLE_RECONCILIATION_AUDIT",
                "  \"${SB_PRIVATE_REPO_ROOT}/project/tools/database_lifecycle/lifecycle_reconciliation_audit.py\"",
                ")",
                "",
                "add_test(",
                "  NAME database_lifecycle_existing_reconciliation",
                "  COMMAND \"${Python3_EXECUTABLE}\" \"${SB_DATABASE_LIFECYCLE_RECONCILIATION_AUDIT}\"",
                "          --repo-root \"${SB_PRIVATE_REPO_ROOT}\"",
                "          --report \"${SB_PRIVATE_REPO_ROOT}/project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/artifacts/DATABASE_LIFECYCLE_RECONCILIATION_REPORT.md\"",
                ")",
                "set_tests_properties(database_lifecycle_existing_reconciliation PROPERTIES",
                "  LABELS \"database_lifecycle_existing_reconciliation;DBLC_STATIC_NO_LEGACY_LIFECYCLE_DRIFT;database_lifecycle;mga_transaction_regression\"",
                ")",
                "```",
                "",
            ]
        )
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_audit(root: Path, report: Path | None) -> int:
    findings: list[Finding] = []
    tracker, gates, queue, agent_status, gap = validate_csv_inputs(root, findings)
    labels = collect_cmake_labels(root)

    if tracker:
        validate_execution_plan_status(tracker, gates, queue, agent_status, labels, findings)
        validate_gap_matrix(gap, findings)
    validate_surface_inventory(root, labels, findings)

    critical_paths = collect_critical_source_paths(root, tracker)
    authority_paths = collect_authority_scan_paths(root, critical_paths)
    scan_markers(root, critical_paths, findings)
    scan_authority_shortcuts(root, authority_paths, findings)
    scan_parser_driver_drift(root, findings)

    report_path = report if report is not None else root / DEFAULT_REPORT
    write_report(root, report_path, findings, critical_paths, authority_paths, labels)

    fatal = [finding for finding in findings if finding.severity == "fatal"]
    if fatal:
        print(f"DBLC-013N reconciliation audit failed with {len(fatal)} fatal finding(s).", file=sys.stderr)
        for finding in fatal[:20]:
            print(f"{finding.code}: {finding.location}: {finding.detail}", file=sys.stderr)
        if len(fatal) > 20:
            print(f"... {len(fatal) - 20} more fatal finding(s); see {report_path}", file=sys.stderr)
        return 1
    print(f"DBLC-013N reconciliation audit passed; report written to {report_path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="DBLC-013N lifecycle reconciliation audit")
    parser.add_argument("--repo-root", default=os.getcwd(), help="ScratchBird" "-Private repository root")
    parser.add_argument("--report", default=None, help="report path; defaults to execution_plan artifact")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    report = Path(args.report).resolve() if args.report else None
    return run_audit(root, report)


if __name__ == "__main__":
    raise SystemExit(main())
