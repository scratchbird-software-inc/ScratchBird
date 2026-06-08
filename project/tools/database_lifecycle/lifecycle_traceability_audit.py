#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-013R spec-to-test traceability audit.

The audit reconciles lifecycle execution_plan rows, gate artifacts, state-model
vocabulary, refusal diagnostics, operation families, route surfaces, and the
current CTest/test inventory. It fails only for traceability defects: missing
rows, missing gate coverage, missing diagnostic/test mapping, or unmapped
lifecycle vocabulary.
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
ACCEPTANCE_GATES = EXECUTION_PLAN / "ACCEPTANCE_GATES.csv"
SPEC_MATRIX = EXECUTION_PLAN / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv"
VALIDATION_PLAN = EXECUTION_PLAN / "VALIDATION_PLAN.md"
CTEST_GATES = ARTIFACTS / "DATABASE_LIFECYCLE_CTEST_REQUIRED_GATES.csv"
GAP_MATRIX = ARTIFACTS / "DATABASE_LIFECYCLE_IMPLEMENTATION_GAP_MATRIX.csv"
STATE_MODEL = ARTIFACTS / "DATABASE_LIFECYCLE_STATE_MODEL_DRAFT.md"
NO_DEFER_CONTRACT = ARTIFACTS / "NO_DEFER_LIFECYCLE_CONTRACT.md"
VALIDATION_COMMANDS = ARTIFACTS / "database_lifecycle_validation_commands.md"
AGENT_STATUS = ARTIFACTS / "DATABASE_LIFECYCLE_AGENT_STATUS.csv"
WRITE_SCOPE = ARTIFACTS / "DATABASE_LIFECYCLE_AGENT_WRITE_SCOPE_REGISTER.csv"
EXECUTION_QUEUE = ARTIFACTS / "DATABASE_LIFECYCLE_SLICE_EXECUTION_QUEUE.csv"
ENGINE_LIFECYCLE_PACKET = Path("public_input_snapshot")
DIAGNOSTIC_CODES = Path("public_contract_snapshot")
DIAGNOSTIC_SHAPES = Path("public_contract_snapshot")
DEFAULT_REPORT = ARTIFACTS / "DATABASE_LIFECYCLE_TRACEABILITY_REPORT.md"

TRACEABILITY_SLICE = "DBLC-013R"
TRACEABILITY_GATE = "DBLC_P13R_TRACEABILITY_COMPLETE"
TRACEABILITY_CTEST = "database_lifecycle_traceability"
TRACEABILITY_STATIC = "DBLC_STATIC_TRACEABILITY_COVERAGE"

OWNED_PATHS = (
    Path("project/tools/database_lifecycle/lifecycle_traceability_audit.py"),
    Path("project/tests/database_lifecycle/traceability_conformance.cpp"),
    Path("project/tests/database_lifecycle/traceability_static.py"),
    DEFAULT_REPORT,
)

STATE_RE = re.compile(
    r"`((?:database|filespace|ownership|recovery|route|process|session|transaction|runtime|policy|catalog|resource|mode|diagnostic)\.[a-z0-9_]+)`"
)
OPERATION_RE = re.compile(
    r"`((?:lifecycle|policy|storage|process|runtime|security|catalog|resources|mga|cluster|observability|replication)\.[a-z0-9_]+(?:\.[a-z0-9_]+)*)`"
)
REFUSAL_RE = re.compile(r"`(refusal\.[a-z0-9_]+)`")
DIAGNOSTIC_RE = re.compile(r"\b([A-Z][A-Z0-9_]*(?:\.[A-Z0-9_]+)+)\b")

SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".hpp", ".hh", ".h", ".py", ".md", ".yaml", ".csv"}


@dataclass(frozen=True)
class Finding:
    severity: str
    code: str
    location: str
    detail: str


@dataclass(frozen=True)
class TraceRecord:
    kind: str
    item: str
    gates: tuple[str, ...]
    source: str


def split_semicolon(value: str) -> list[str]:
    return [part.strip() for part in value.split(";") if part.strip()]


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def read_csv_rows(path: Path) -> list[dict[str, str]]:
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


def required_inputs(root: Path) -> tuple[Path, ...]:
    return (
        root / TRACKER,
        root / ACCEPTANCE_GATES,
        root / SPEC_MATRIX,
        root / VALIDATION_PLAN,
        root / CTEST_GATES,
        root / GAP_MATRIX,
        root / STATE_MODEL,
        root / NO_DEFER_CONTRACT,
        root / VALIDATION_COMMANDS,
        root / AGENT_STATUS,
        root / WRITE_SCOPE,
        root / EXECUTION_QUEUE,
        root / ENGINE_LIFECYCLE_PACKET,
        root / DIAGNOSTIC_CODES,
        root / DIAGNOSTIC_SHAPES,
    )


def collect_cmake_labels(root: Path) -> set[str]:
    labels: set[str] = set()
    cmake_paths = list((root / "project/tests").rglob("CMakeLists.txt"))
    cmake_paths.extend((root / "project").glob("CMakeLists.txt"))
    quoted_labels = re.compile(r"(?:LABELS|APPEND PROPERTY LABELS)\s+\"([^\"]+)\"", re.MULTILINE)
    add_test = re.compile(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_\-]+)", re.MULTILINE)
    for path in cmake_paths:
        text = read_text(path)
        for match in quoted_labels.finditer(text):
            labels.update(part for part in re.split(r"[;\s]+", match.group(1)) if part)
        for match in add_test.finditer(text):
            labels.add(match.group(1))
    return labels


def collect_test_files(root: Path) -> set[str]:
    tests: set[str] = set()
    base = root / "project/tests"
    if not base.exists():
        return tests
    for path in base.rglob("*"):
        if path.is_file() and path.suffix in SOURCE_SUFFIXES:
            tests.add(path.relative_to(root).as_posix())
            tests.add(path.stem)
    return tests


def md_cells(line: str) -> list[str]:
    stripped = line.strip()
    if not stripped.startswith("|") or not stripped.endswith("|"):
        return []
    cells = [cell.strip() for cell in stripped.strip("|").split("|")]
    if cells and all(set(cell) <= {"-", " "} for cell in cells):
        return []
    return cells


def extract_trace_vocabulary(root: Path) -> tuple[set[str], set[tuple[str, str]], set[str], set[str], set[str], set[str]]:
    state_text = read_text(root / STATE_MODEL)
    no_defer_text = read_text(root / NO_DEFER_CONTRACT)
    engine_text = read_text(root / ENGINE_LIFECYCLE_PACKET)
    registry_text = read_text(root / DIAGNOSTIC_CODES)

    combined = "\n".join((state_text, no_defer_text))
    states = set(STATE_RE.findall(combined))
    operations = set(OPERATION_RE.findall(combined))
    refusals = set(REFUSAL_RE.findall(combined))

    transitions: set[tuple[str, str]] = set()
    for line in state_text.splitlines():
        cells = md_cells(line)
        if len(cells) < 4:
            continue
        source_match = re.fullmatch(r"`([^`]+)`", cells[0])
        if not source_match:
            continue
        source = source_match.group(1)
        if source not in states:
            continue
        next_cell = cells[3] if len(cells) >= 4 else ""
        for target in STATE_RE.findall(next_cell):
            if target != source:
                transitions.add((source, target))

    routes = {state for state in states if state.startswith("route.")}
    routes.update(operation for operation in operations if "route" in operation or operation.startswith("process."))

    diagnostics: set[str] = set()
    for code in DIAGNOSTIC_RE.findall(engine_text):
        if lifecycle_diagnostic_prefix(code):
            diagnostics.add(code)
    for code in DIAGNOSTIC_RE.findall(registry_text):
        if lifecycle_diagnostic_prefix(code):
            diagnostics.add(code)

    return states, transitions, operations, refusals, routes, diagnostics


def lifecycle_diagnostic_prefix(code: str) -> bool:
    prefixes = (
        "ENGINE.",
        "SECURITY.",
        "MGA.",
        "CACHE.",
        "IPC.",
        "PROCESS.",
        "ROUTE.",
        "POLICY.",
        "FORMAT.",
        "TRACE.",
        "BUILDABILITY.",
        "CONFIG.",
        "LISTENER.",
        "PARSER_SERVER_IPC.",
        "CONTROL.",
        "CLEANUP.",
        "LIMBO.",
        "RESOURCE.",
        "TEMP_WORKSPACE.",
        "WORKLOAD_RESOURCE.",
    )
    return code.startswith(prefixes)


def gate_pool(ctest_rows: Iterable[dict[str, str]], acceptance_rows: Iterable[dict[str, str]]) -> set[str]:
    labels: set[str] = set()
    for row in ctest_rows:
        labels.update(split_semicolon(row.get("ctest_labels", "")))
        labels.update(split_semicolon(row.get("static_or_audit_gates", "")))
        for command in split_semicolon(row.get("required_commands", "")):
            if ":" in command:
                labels.add(command.split(":", 1)[1])
    for row in acceptance_rows:
        if row.get("gate_id"):
            labels.add(row["gate_id"])
        labels.update(split_semicolon(row.get("evidence", "")))
    return labels


def gates_for_item(kind: str, item: str) -> tuple[str, ...]:
    text = item.lower()
    gates: set[str] = set()

    def has(*tokens: str) -> bool:
        return any(token in text for token in tokens)

    if kind == "invalid_transition_or_refusal":
        gates.update({TRACEABILITY_CTEST, TRACEABILITY_STATIC})

    if text.startswith("process."):
        gates.update(
            {
                "database_lifecycle_process_association",
                "database_lifecycle_server_daemon",
                "database_lifecycle_manager",
                "database_lifecycle_listener",
                "database_lifecycle_parser",
                "database_lifecycle_ipc",
            }
        )

    if text.startswith("route."):
        gates.update(
            {
                "database_lifecycle_server_route",
                "database_lifecycle_ipc",
                "database_lifecycle_parser_route",
                "database_lifecycle_session_request_cursor",
                "database_lifecycle_security",
            }
        )

    if kind == "state":
        namespace = text.split(".", 1)[0]
        namespace_gates = {
            "database": {"database_lifecycle_storage", "database_lifecycle_admin_cli"},
            "filespace": {"database_lifecycle_filespace", "database_lifecycle_storage"},
            "ownership": {"database_lifecycle_storage", "database_lifecycle_server_daemon"},
            "recovery": {"database_lifecycle_storage", "database_lifecycle_upgrade_migration"},
            "route": {"database_lifecycle_server_route", "database_lifecycle_ipc", "database_lifecycle_parser_route"},
            "process": {"database_lifecycle_process_association", "database_lifecycle_server_daemon"},
            "session": {"database_lifecycle_session_request_cursor", "database_lifecycle_security"},
            "transaction": {"database_lifecycle_storage", "mga_transaction_regression"},
            "runtime": {"database_lifecycle_engine_agent", "database_lifecycle_supportability_evidence"},
            "policy": {"database_lifecycle_default_policy_catalog", "database_lifecycle_config_policy_security_provider"},
            "catalog": {"database_lifecycle_catalog_object", "database_lifecycle_sys_information_projection"},
            "resource": {"database_lifecycle_resource_seed", "database_lifecycle_workload_resource"},
            "mode": {"database_lifecycle_admin_cli", "database_lifecycle_storage"},
            "diagnostic": {"database_lifecycle_supportability_evidence", TRACEABILITY_STATIC},
        }
        gates.update(namespace_gates.get(namespace, set()))

    if has("traceability"):
        gates.update({TRACEABILITY_CTEST, TRACEABILITY_STATIC, TRACEABILITY_GATE})
    if has("reconciliation"):
        gates.update({"database_lifecycle_existing_reconciliation", "DBLC_STATIC_NO_LEGACY_LIFECYCLE_DRIFT"})
    if has("regression", "exhaustive"):
        gates.update({"database_lifecycle_exhaustive", "DBLC_STATIC_REGRESSION_REPORT_ARTIFACT"})
    if has("hardening", "fault"):
        gates.update({"database_lifecycle_fault_injection", "DBLC_STATIC_AUTHORITY_DRIFT_GATES"})
    if has("release", "declaration"):
        gates.update({"database_lifecycle_release", "DBLC_STATIC_FINAL_ZERO_OPEN_AUDIT"})
    if has("policy"):
        gates.update({"database_lifecycle_default_policy_catalog", "database_lifecycle_config_policy_security_provider"})
    if has("diagnostic", "message_vector", "message-vector"):
        gates.update({"database_lifecycle_supportability_evidence", TRACEABILITY_STATIC})
    if has("durable", "evidence"):
        gates.update({"database_lifecycle_storage", "database_lifecycle_supportability_evidence"})
    if has("filespace"):
        gates.update({"database_lifecycle_filespace", "database_lifecycle_storage"})
    if has("create", "tx1", "bootstrap"):
        gates.update({"database_lifecycle_storage", "database_lifecycle_default_policy_catalog"})
    if has("first_open", "tx2", "activation"):
        gates.update({"database_lifecycle_storage"})
    if has("open", "recovery", "migration", "format", "version", "epoch"):
        gates.update({"database_lifecycle_storage", "database_lifecycle_protocol_versioning", "database_lifecycle_upgrade_migration"})
    if has("attach", "auth", "security", "principal", "privilege"):
        gates.update({"database_lifecycle_security", "database_lifecycle_security_principal"})
    if has("detach", "session", "cursor", "request"):
        gates.update({"database_lifecycle_session_request_cursor", "database_lifecycle_server_route"})
    if has("transaction", "mga", "rollback", "commit", "cleanup", "retention"):
        gates.update({"database_lifecycle_storage", "mga_transaction_regression"})
    if has("maintenance", "restricted", "inspect", "verify", "repair", "fence"):
        gates.update({"database_lifecycle_admin_cli", "database_lifecycle_storage"})
    if has("shutdown", "ack", "force"):
        gates.update({"database_lifecycle_shutdown_notification", "database_lifecycle_process_association", "database_lifecycle_server_route"})
    if has("drop"):
        gates.update({"database_lifecycle_storage", "database_lifecycle_filespace", "database_lifecycle_admin_cli"})
    if has("owner", "ownership"):
        gates.update({"database_lifecycle_storage", "database_lifecycle_server_daemon", "database_lifecycle_process_association"})
    if has("association", "cross_database"):
        gates.update({"database_lifecycle_process_association", "database_lifecycle_server_daemon", "database_lifecycle_server_route"})
    if has("state_transition"):
        gates.update({"database_lifecycle_engine_api", TRACEABILITY_STATIC})
    if has("route"):
        gates.update({"database_lifecycle_server_route", "database_lifecycle_ipc", "database_lifecycle_parser_route"})
    if has("manager"):
        gates.add("database_lifecycle_manager")
    if has("listener"):
        gates.add("database_lifecycle_listener")
    if has("parser"):
        gates.update({"database_lifecycle_parser", "database_lifecycle_parser_route"})
    if has("server_daemon", "daemon", "service"):
        gates.add("database_lifecycle_server_daemon")
    if has("ipc", "endpoint", "frame", "backpressure"):
        gates.add("database_lifecycle_ipc")
    if has("agent"):
        gates.add("database_lifecycle_engine_agent")
    if has("cache", "checkpoint"):
        gates.add("database_lifecycle_cache_checkpoint")
    if has("backup", "archive", "restore", "shadow", "snapshot", "hold"):
        gates.add("database_lifecycle_backup_archive_restore")
    if has("udr", "extension"):
        gates.add("database_lifecycle_udr_extension")
    if has("workload", "quota", "resource_quota"):
        gates.add("database_lifecycle_workload_resource")
    if has("catalog", "schema", "object", "ddl"):
        gates.add("database_lifecycle_catalog_object")
    if has("sys_information", "projection"):
        gates.add("database_lifecycle_sys_information_projection")
    if has("index", "optimizer", "statistics", "plan"):
        gates.add("database_lifecycle_index_statistics_plan")
    if has("lock", "wait", "deadlock", "latch"):
        gates.add("database_lifecycle_lock_wait_deadlock")
    if has("temp", "spill", "workspace"):
        gates.add("database_lifecycle_temp_workspace")
    if has("event", "notification", "listen", "notify", "subscription"):
        gates.add("database_lifecycle_event_notification")
    if has("encryption", "key", "protected"):
        gates.add("database_lifecycle_encryption_key")
    if has("charset", "collation", "timezone", "locale", "seed_i18n", "resource_seed"):
        gates.add("database_lifecycle_resource_seed")
    if has("job", "scheduler", "background"):
        gates.add("database_lifecycle_jobs_scheduler")
    if has("cluster", "replication", "changefeed", "cdc", "live_ingest"):
        gates.update({"database_lifecycle_cluster_boundary", "database_lifecycle_replication_boundary"})
    if has("allocation", "freespace", "pagemap", "page"):
        gates.add("database_lifecycle_storage_allocation")
    if has("executable", "routine", "procedure", "trigger", "function"):
        gates.add("database_lifecycle_executable_object")
    if has("sequence", "generator", "identity"):
        gates.add("database_lifecycle_sequence_generator")
    if has("supportability", "audit", "operational"):
        gates.add("database_lifecycle_supportability_evidence")
    if has("capability", "feature", "edition"):
        gates.add("database_lifecycle_capability_profile")
    if has("threat", "abuse"):
        gates.add("database_lifecycle_threat_model")
    if has("admin", "cli", "command"):
        gates.add("database_lifecycle_admin_cli")
    if has("packaging", "runtime"):
        gates.add("database_lifecycle_packaging_service")
    if has("donor"):
        gates.add("database_lifecycle_donor_mapping")

    if kind == "diagnostic":
        prefix = item.split(".", 1)[0]
        diagnostic_prefix_gates = {
            "ENGINE": {"database_lifecycle_storage", "database_lifecycle_admin_cli"},
            "SECURITY": {"database_lifecycle_security"},
            "MGA": {"database_lifecycle_storage", "mga_transaction_regression"},
            "CACHE": {"database_lifecycle_cache_checkpoint"},
            "IPC": {"database_lifecycle_ipc"},
            "PROCESS": {"database_lifecycle_process_association"},
            "ROUTE": {"database_lifecycle_server_route"},
            "POLICY": {"database_lifecycle_default_policy_catalog", "database_lifecycle_config_policy_security_provider"},
            "FORMAT": {"database_lifecycle_protocol_versioning", "database_lifecycle_upgrade_migration"},
            "TRACE": {TRACEABILITY_STATIC},
            "BUILDABILITY": {TRACEABILITY_STATIC},
            "CONFIG": {"database_lifecycle_config_policy_security_provider"},
            "LISTENER": {"database_lifecycle_listener"},
            "PARSER_SERVER_IPC": {"database_lifecycle_ipc", "database_lifecycle_parser"},
            "CONTROL": {"database_lifecycle_ipc"},
            "CLEANUP": {"database_lifecycle_mga_gc_retention"},
            "LIMBO": {"database_lifecycle_mga_gc_retention"},
            "RESOURCE": {"database_lifecycle_resource_seed"},
            "TEMP_WORKSPACE": {"database_lifecycle_temp_workspace"},
            "WORKLOAD_RESOURCE": {"database_lifecycle_workload_resource"},
        }
        gates.update(diagnostic_prefix_gates.get(prefix, set()))

    return tuple(sorted(gates))


def validate_required_files(root: Path, findings: list[Finding]) -> bool:
    ok = True
    for path in required_inputs(root):
        if not path.is_file():
            add(findings, "fatal", "DBLC013R.MISSING_INPUT", path.relative_to(root).as_posix(), "required traceability input is missing")
            ok = False
    return ok


def validate_execution_plan_rows(
    tracker_rows: list[dict[str, str]],
    acceptance_rows: list[dict[str, str]],
    spec_rows: list[dict[str, str]],
    ctest_rows: list[dict[str, str]],
    gap_rows: list[dict[str, str]],
    actual_labels: set[str],
    findings: list[Finding],
) -> None:
    tracker = index_by(tracker_rows, "slice_id")
    ctest = index_by(ctest_rows, "slice_id")
    gap = index_by(gap_rows, "slice_id")
    acceptance = index_by(acceptance_rows, "gate_id")
    declared = gate_pool(ctest_rows, acceptance_rows)

    for row in tracker_rows:
        slice_id = row.get("slice_id", "")
        if not slice_id:
            add(findings, "fatal", "DBLC013R.TRACKER_ID_MISSING", TRACKER.as_posix(), "tracker row lacks slice_id")
            continue
        gate_row = ctest.get(slice_id)
        if not gate_row:
            add(findings, "fatal", "DBLC013R.GATE_ROW_MISSING", slice_id, "tracker row has no CTest/static gate row")
            continue
        has_gate = bool(
            split_semicolon(gate_row.get("ctest_labels", ""))
            or split_semicolon(gate_row.get("static_or_audit_gates", ""))
            or split_semicolon(gate_row.get("required_commands", ""))
        )
        if not has_gate:
            add(findings, "fatal", "DBLC013R.GATE_COVERAGE_MISSING", slice_id, "gate row has no CTest/static/audit command")
        gap_row = gap.get(slice_id)
        if not gap_row:
            add(findings, "fatal", "DBLC013R.GAP_ROW_MISSING", slice_id, "implementation gap row is missing")
        else:
            for column in ("test_status", "diagnostic_status", "closure_gate", "next_required_action"):
                value = gap_row.get(column, "").strip()
                if not value:
                    add(findings, "fatal", "DBLC013R.GAP_MAPPING_MISSING", slice_id, f"gap matrix lacks {column}")
            closure_gate = gap_row.get("closure_gate", "").strip()
            if closure_gate and closure_gate not in acceptance:
                add(findings, "fatal", "DBLC013R.ACCEPTANCE_GATE_MISSING", slice_id, f"{closure_gate} is not in ACCEPTANCE_GATES.csv")

        if row.get("status") == "passed":
            for label in split_semicolon(gate_row.get("ctest_labels", "")):
                if label not in actual_labels:
                    add(findings, "fatal", "DBLC013R.PASSED_CTEST_LABEL_NOT_MATERIALIZED", slice_id, f"{label} is not present in current CTest files")

    for row in ctest_rows:
        slice_id = row.get("slice_id", "")
        if slice_id and slice_id not in tracker:
            add(findings, "fatal", "DBLC013R.CTEST_ROW_ORPHANED", slice_id, "gate row has no tracker row")
    for row in gap_rows:
        slice_id = row.get("slice_id", "")
        if slice_id and slice_id not in tracker:
            add(findings, "fatal", "DBLC013R.GAP_ROW_ORPHANED", slice_id, "gap matrix row has no tracker row")

    for row in spec_rows:
        key = row.get("source_search_key", "")
        if not key:
            add(findings, "fatal", "DBLC013R.SPEC_TRACE_KEY_MISSING", SPEC_MATRIX.as_posix(), "spec trace row lacks source_search_key")
        for column in ("canonical_spec_or_matrix", "implementation_target", "validation_gate"):
            if not row.get(column, "").strip():
                add(findings, "fatal", "DBLC013R.SPEC_TRACE_FIELD_MISSING", key or SPEC_MATRIX.as_posix(), f"spec trace row lacks {column}")
        for gate in split_semicolon(row.get("validation_gate", "")):
            if gate not in declared:
                add(findings, "fatal", "DBLC013R.SPEC_TRACE_GATE_UNKNOWN", key, f"{gate} is not declared by acceptance or CTest gate artifacts")


def validate_traceability_self(root: Path, findings: list[Finding], actual_labels: set[str], test_files: set[str]) -> None:
    acceptance = index_by(read_csv_rows(root / ACCEPTANCE_GATES), "gate_id")
    ctest = index_by(read_csv_rows(root / CTEST_GATES), "slice_id")
    spec = index_by(read_csv_rows(root / SPEC_MATRIX), "source_search_key")
    agent = read_text(root / AGENT_STATUS)
    write_scope = read_text(root / WRITE_SCOPE)
    queue = read_text(root / EXECUTION_QUEUE)
    validation_plan = read_text(root / VALIDATION_PLAN)
    commands = read_text(root / VALIDATION_COMMANDS)

    if TRACEABILITY_GATE not in acceptance:
        add(findings, "fatal", "DBLC013R.SELF_ACCEPTANCE_MISSING", TRACEABILITY_GATE, "required P13R acceptance gate is absent")
    gate_row = ctest.get(TRACEABILITY_SLICE)
    if not gate_row:
        add(findings, "fatal", "DBLC013R.SELF_CTEST_ROW_MISSING", TRACEABILITY_SLICE, "P13R CTest gate row is absent")
    else:
        declared_ctest = set(split_semicolon(gate_row.get("ctest_labels", "")))
        declared_static = set(split_semicolon(gate_row.get("static_or_audit_gates", "")))
        if TRACEABILITY_CTEST not in declared_ctest:
            add(findings, "fatal", "DBLC013R.SELF_CTEST_LABEL_MISSING", TRACEABILITY_SLICE, TRACEABILITY_CTEST)
        if TRACEABILITY_STATIC not in declared_static:
            add(findings, "fatal", "DBLC013R.SELF_STATIC_GATE_MISSING", TRACEABILITY_SLICE, TRACEABILITY_STATIC)

    spec_row = spec.get("SPEC_TO_TEST_TRACEABILITY_GENERATOR")
    if not spec_row or TRACEABILITY_GATE not in spec_row.get("validation_gate", ""):
        add(findings, "fatal", "DBLC013R.SPEC_MATRIX_SELF_ROW_MISSING", "SPEC_TO_TEST_TRACEABILITY_GENERATOR", "P13R spec trace row is absent or lacks P13R gate")

    for token, location in (
        (TRACEABILITY_CTEST, VALIDATION_PLAN.as_posix()),
        (TRACEABILITY_STATIC, VALIDATION_COMMANDS.as_posix()),
        (TRACEABILITY_GATE, ACCEPTANCE_GATES.as_posix()),
    ):
        if token not in validation_plan and token not in commands and token not in read_text(root / ACCEPTANCE_GATES):
            add(findings, "fatal", "DBLC013R.SELF_GATE_TOKEN_MISSING", location, token)

    for path in OWNED_PATHS:
        if not (root / path).exists():
            add(findings, "fatal", "DBLC013R.OWNED_OUTPUT_MISSING", path.as_posix(), "worker-owned traceability output is missing")
        if path.as_posix() not in write_scope:
            add(findings, "fatal", "DBLC013R.WRITE_SCOPE_NOT_REGISTERED", path.as_posix(), "owned path is absent from write-scope register")

    for text, location in ((agent, AGENT_STATUS.as_posix()), (queue, EXECUTION_QUEUE.as_posix())):
        if TRACEABILITY_SLICE not in text or TRACEABILITY_GATE not in text:
            add(findings, "fatal", "DBLC013R.AGENT_TRACE_MISSING", location, "P13R slice/gate is absent from orchestration artifacts")

    direct_gate_files = {
        "traceability_conformance": "project/tests/database_lifecycle/traceability_conformance.cpp",
        "traceability_static": "project/tests/database_lifecycle/traceability_static.py",
    }
    for stem, rel in direct_gate_files.items():
        if stem not in test_files:
            add(findings, "fatal", "DBLC013R.DIRECT_TEST_FILE_MISSING", rel, "direct traceability gate file is absent from test inventory")

    if TRACEABILITY_CTEST not in actual_labels:
        add(
            findings,
            "warn",
            "DBLC013R.CMAKE_REGISTRATION_PENDING",
            "project/tests/database_lifecycle/CMakeLists.txt",
            "traceability tests are direct-runnable; shared CMake registration remains coordinator-owned",
        )


def validate_vocabulary_mapping(
    vocabulary: tuple[set[str], set[tuple[str, str]], set[str], set[str], set[str], set[str]],
    declared_gates: set[str],
    findings: list[Finding],
) -> list[TraceRecord]:
    states, transitions, operations, refusals, routes, diagnostics = vocabulary
    records: list[TraceRecord] = []

    def record(kind: str, item: str, source: str) -> None:
        gates = gates_for_item(kind, item)
        if not gates:
            add(findings, "fatal", "DBLC013R.VOCABULARY_UNMAPPED", item, f"{kind} has no generated gate mapping")
            return
        missing = [gate for gate in gates if gate not in declared_gates and not gate.startswith("project/")]
        if missing:
            add(findings, "fatal", "DBLC013R.VOCABULARY_GATE_UNKNOWN", item, f"{kind} maps to undeclared gates: {', '.join(missing)}")
            return
        records.append(TraceRecord(kind, item, gates, source))

    for state in sorted(states):
        record("state", state, STATE_MODEL.as_posix())
    for source, target in sorted(transitions):
        transition = f"{source}->{target}"
        gates = tuple(sorted(set(gates_for_item("state", source)).union(gates_for_item("state", target)).union({TRACEABILITY_STATIC})))
        missing = [gate for gate in gates if gate not in declared_gates]
        if missing:
            add(findings, "fatal", "DBLC013R.TRANSITION_GATE_UNKNOWN", transition, f"transition maps to undeclared gates: {', '.join(missing)}")
        else:
            records.append(TraceRecord("transition", transition, gates, STATE_MODEL.as_posix()))
    for operation in sorted(operations):
        record("operation_family", operation, STATE_MODEL.as_posix())
    for refusal in sorted(refusals):
        record("invalid_transition_or_refusal", refusal, NO_DEFER_CONTRACT.as_posix())
    for route in sorted(routes):
        record("route", route, STATE_MODEL.as_posix())
    for diagnostic in sorted(diagnostics):
        record("diagnostic", diagnostic, f"{ENGINE_LIFECYCLE_PACKET.as_posix()};{DIAGNOSTIC_CODES.as_posix()}")

    return records


def validate_report_claim(records: list[TraceRecord], findings: list[Finding]) -> None:
    required_kinds = {
        "state",
        "transition",
        "operation_family",
        "invalid_transition_or_refusal",
        "route",
        "diagnostic",
    }
    present = {record.kind for record in records}
    for kind in sorted(required_kinds - present):
        add(findings, "fatal", "DBLC013R.TRACE_KIND_MISSING", kind, "required traceability category has no generated records")


def ctest_snippet() -> list[str]:
    return [
        "set(SB_DATABASE_LIFECYCLE_TRACEABILITY_AUDIT",
        "  \"${SB_PRIVATE_REPO_ROOT}/project/tools/database_lifecycle/lifecycle_traceability_audit.py\"",
        ")",
        "",
        "add_executable(database_lifecycle_traceability_conformance",
        "  traceability_conformance.cpp",
        ")",
        "target_compile_features(database_lifecycle_traceability_conformance PRIVATE cxx_std_23)",
        "target_compile_definitions(",
        "  database_lifecycle_traceability_conformance",
        "  PRIVATE",
        "    SB_DBLC_TRACEABILITY_AUDIT=\"${SB_DATABASE_LIFECYCLE_TRACEABILITY_AUDIT}\"",
        "    SB_DBLC_REPO_ROOT=\"${SB_PRIVATE_REPO_ROOT}\"",
        "    SB_DBLC_PYTHON_EXECUTABLE=\"${Python3_EXECUTABLE}\"",
        ")",
        "",
        "add_test(",
        "  NAME database_lifecycle_traceability_conformance",
        "  COMMAND database_lifecycle_traceability_conformance",
        ")",
        "set_tests_properties(database_lifecycle_traceability_conformance PROPERTIES",
        f"  LABELS \"{TRACEABILITY_CTEST};{TRACEABILITY_GATE};database_lifecycle\"",
        ")",
        "",
        "add_test(",
        "  NAME database_lifecycle_traceability_static",
        "  COMMAND \"${Python3_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/traceability_static.py\"",
        "          --repo-root \"${SB_PRIVATE_REPO_ROOT}\"",
        ")",
        "set_tests_properties(database_lifecycle_traceability_static PROPERTIES",
        f"  LABELS \"{TRACEABILITY_CTEST};{TRACEABILITY_STATIC};{TRACEABILITY_GATE};database_lifecycle\"",
        ")",
    ]


def write_report(
    root: Path,
    report_path: Path,
    findings: list[Finding],
    records: list[TraceRecord],
    actual_labels: set[str],
    test_files: set[str],
) -> None:
    fatal_count = sum(1 for finding in findings if finding.severity == "fatal")
    warn_count = sum(1 for finding in findings if finding.severity == "warn")
    status = "passed" if fatal_count == 0 else "failed"
    open_gaps = 0 if fatal_count == 0 else fatal_count
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    by_kind: dict[str, int] = {}
    for record in records:
        by_kind[record.kind] = by_kind.get(record.kind, 0) + 1

    lines = [
        "# Database Lifecycle Traceability Report",
        "",
        f"Generated: `{now}`",
        f"Slice: `{TRACEABILITY_SLICE}`",
        f"Acceptance gate: `{TRACEABILITY_GATE}`",
        f"Status: `{status}`",
        f"Open traceability gaps: `{open_gaps}`",
        "",
        "## Scope",
        "",
        "This report is generated from the database lifecycle tracker, acceptance gates, implementation gap matrix, required CTest/static gate matrix, validation plan, validation command artifact, state model, no-defer contract, lifecycle implementation packet diagnostics, diagnostic registries, orchestration artifacts, and current test/CMake inventory.",
        "",
        "## Coverage Summary",
        "",
        f"- Execution_Plan CTest labels observed in current CMake/test files: `{len(actual_labels)}`",
        f"- Test source inventory entries observed: `{len(test_files)}`",
        f"- Generated trace records: `{len(records)}`",
        f"- Fatal findings: `{fatal_count}`",
        f"- Warnings: `{warn_count}`",
        "",
        "| Trace category | Records |",
        "| --- | ---: |",
    ]
    for kind in sorted(by_kind):
        lines.append(f"| `{kind}` | `{by_kind[kind]}` |")
    if not by_kind:
        lines.append("| `none` | `0` |")

    lines.extend(["", "## Required Gates", ""])
    lines.extend(
        [
            f"- `{TRACEABILITY_GATE}`: acceptance gate declared in execution_plan artifacts.",
            f"- `{TRACEABILITY_CTEST}`: generated traceability CTest label.",
            f"- `{TRACEABILITY_STATIC}`: static traceability coverage gate.",
        ]
    )

    lines.extend(["", "## Findings", ""])
    if not findings:
        lines.append("No findings. Zero open traceability gaps remain for this slice.")
    else:
        lines.extend(["| Severity | Code | Location | Detail |", "| --- | --- | --- | --- |"])
        for finding in sorted(findings, key=lambda item: (item.severity != "fatal", item.code, item.location)):
            location = finding.location.replace("|", "\\|")
            detail = finding.detail.replace("|", "\\|")
            lines.append(f"| `{finding.severity}` | `{finding.code}` | `{location}` | {detail} |")

    lines.extend(["", "## Trace Samples", ""])
    if records:
        lines.extend(["| Kind | Item | Gates |", "| --- | --- | --- |"])
        for record in records[:80]:
            gates = ", ".join(f"`{gate}`" for gate in record.gates)
            lines.append(f"| `{record.kind}` | `{record.item}` | {gates} |")
        if len(records) > 80:
            lines.append(f"| `summary` | `{len(records) - 80} additional generated records` | `covered` |")
    else:
        lines.append("No trace records were generated.")

    lines.extend(["", "## CMake Integration", ""])
    if TRACEABILITY_CTEST in actual_labels and TRACEABILITY_STATIC in actual_labels:
        lines.append("The traceability CTest/static labels are materialized in the current CMake inventory.")
    else:
        lines.extend(
            [
                "Shared CMake registration is coordinator-owned. Apply this snippet in `project/tests/database_lifecycle/CMakeLists.txt` after `SB_PRIVATE_REPO_ROOT` and `Python3_EXECUTABLE` are available:",
                "",
                "```cmake",
                *ctest_snippet(),
                "```",
            ]
        )

    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_audit(root: Path, report: Path | None = None) -> int:
    findings: list[Finding] = []
    if not validate_required_files(root, findings):
        report_path = report if report is not None else root / DEFAULT_REPORT
        write_report(root, report_path, findings, [], set(), set())
        return 1

    tracker_rows = read_csv_rows(root / TRACKER)
    acceptance_rows = read_csv_rows(root / ACCEPTANCE_GATES)
    spec_rows = read_csv_rows(root / SPEC_MATRIX)
    ctest_rows = read_csv_rows(root / CTEST_GATES)
    gap_rows = read_csv_rows(root / GAP_MATRIX)
    actual_labels = collect_cmake_labels(root)
    test_files = collect_test_files(root)

    validate_execution_plan_rows(
        tracker_rows,
        acceptance_rows,
        spec_rows,
        ctest_rows,
        gap_rows,
        actual_labels,
        findings,
    )
    validate_traceability_self(root, findings, actual_labels, test_files)

    declared_gates = gate_pool(ctest_rows, acceptance_rows)
    vocabulary = extract_trace_vocabulary(root)
    records = validate_vocabulary_mapping(vocabulary, declared_gates, findings)
    validate_report_claim(records, findings)

    report_path = report if report is not None else root / DEFAULT_REPORT
    write_report(root, report_path, findings, records, actual_labels, test_files)

    fatal = [finding for finding in findings if finding.severity == "fatal"]
    if fatal:
        print(f"DBLC-013R traceability audit failed with {len(fatal)} fatal finding(s).", file=sys.stderr)
        for finding in fatal[:20]:
            print(f"{finding.code}: {finding.location}: {finding.detail}", file=sys.stderr)
        if len(fatal) > 20:
            print(f"... {len(fatal) - 20} more fatal finding(s); see {report_path}", file=sys.stderr)
        return 1

    print(f"DBLC-013R traceability audit passed; report written to {report_path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="DBLC-013R lifecycle traceability audit")
    parser.add_argument("--repo-root", default=os.getcwd(), help="ScratchBird" "-Private repository root")
    parser.add_argument("--report", default=None, help="report path; defaults to the DBLC execution_plan artifact")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    report = Path(args.report).resolve() if args.report else None
    return run_audit(root, report)


if __name__ == "__main__":
    raise SystemExit(main())
