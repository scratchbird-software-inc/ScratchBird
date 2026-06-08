#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CDP-049 config defaults and rollback policy evidence gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "cdp.config_defaults_rollback_gate.v1"
EXPECTED_DISABLED = "DML.NATIVE_BULK_INGEST.DISABLED"
TARGET = "cdp049_copy_survives_native_disabled"
VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
FORBIDDEN_EXECUTION_PLAN_PATH = "docs" + "/execution-plans"

SHOW_MANAGEMENT_FIELDS = (
    "read_only_mode",
    "catalog_generation_id",
    "security_epoch",
    "resource_epoch",
    "performance_optimization_surface",
    "optimization_profile",
    "copy_append_batching_enabled",
    "plan_cache_enabled",
    "descriptor_metadata_cache_enabled",
    "statistics_epoch",
    "agent_worker_status",
    "resource_governor_state",
    "parser_finality_authority",
    "donor_finality_authority",
)

REQUIRED_MANIFEST_FIELDS = (
    "candidate",
    "compile_time_flag",
    "runtime_config_name",
    "development_default",
    "benchmark_clean_default",
    "beta_packaged_default",
    "disabled_behavior",
    "disabled_refusal_message_vector",
    "support_bundle_show_field",
    "standalone_regression_test_evidence",
)


class GateError(RuntimeError):
    pass


@dataclass
class Route:
    name: str
    root: Path
    database: Path
    args: list[str]
    processes: list[subprocess.Popen[bytes]] = field(default_factory=list)


@dataclass
class RunResult:
    route: str
    case: str
    returncode: int
    stdout: str
    stderr: str
    stdout_path: Path
    stderr_path: Path

    @property
    def diagnostic_code(self) -> str:
        text = "\n".join(part for part in (self.stderr, self.stdout) if part)
        match = re.search(r"\(([A-Za-z0-9_.:-]+)\)", text)
        if match:
            return match.group(1)
        match = re.search(r"(DML\.NATIVE_BULK_INGEST\.DISABLED)", text)
        if match:
            return match.group(1)
        match = re.search(r"Error:\s*(.*)", text)
        return match.group(1).strip() if match else text.strip()


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def make_work_dir(preferred_root: Path) -> Path:
    for root in (preferred_root, Path(tempfile.gettempdir()) / "cdp049"):
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="c_", dir=root))
        probes = (
            candidate / "ipc" / "sc" / "s.sock",
            candidate / "inet" / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock"),
            candidate / "embedded" / "e.sbdb",
        )
        if max(len(str(path)) for path in probes) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise GateError("unable to allocate a short-enough CDP-049 workspace")


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(path: Path, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise GateError(f"timed out waiting for {path}")


def wait_for_tcp(port: int, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError as exc:
            last_error = exc
            time.sleep(0.05)
    raise GateError(f"timed out waiting for listener port {port}: {last_error}")


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=4)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=4)


def write_auth_file(database: Path) -> None:
    Path(str(database) + ".sb.local_password_auth").write_text(
        f"alice\tlocal_password\t{VERIFIER}\n", encoding="utf-8"
    )


def auth_evidence() -> str:
    return f"scheme=local_password_v1;principal=alice;verifier={VERIFIER}"


def start_embedded(args: argparse.Namespace, work: Path) -> Route:
    root = work / "embedded"
    root.mkdir(parents=True, exist_ok=True)
    database = root / "e.sbdb"
    return Route(
        name="embedded",
        root=root,
        database=database,
        args=[args.sb_isql, str(database), "--mode=embedded", "--sslmode=disable"],
    )


def start_local_ipc(args: argparse.Namespace, work: Path) -> Route:
    root = work / "ipc"
    root.mkdir(parents=True, exist_ok=True)
    database = root / "l.sbdb"
    control = root / "sc"
    endpoint = control / "s.sock"
    write_auth_file(database)
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
            "--control-dir",
            str(control),
            "--runtime-dir",
            str(root / "sr"),
            "--database",
            str(database),
            "--sbps-endpoint",
            str(endpoint),
        ],
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
    )
    wait_for_path(endpoint)
    return Route(
        name="local-ipc",
        root=root,
        database=database,
        args=[
            args.sb_isql,
            str(database),
            "--mode=local-ipc",
            "--ipc-method=unix",
            f"--ipc-path={endpoint}",
            "--sslmode=disable",
            "-U",
            "alice",
            "-P",
            auth_evidence(),
        ],
        processes=[server],
    )


def start_inet(args: argparse.Namespace, work: Path) -> Route:
    root = work / "inet"
    root.mkdir(parents=True, exist_ok=True)
    database = root / "i.sbdb"
    server_control = root / "sc"
    endpoint = server_control / "s.sock"
    port = free_port()
    write_auth_file(database)
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--create-if-missing",
            "--control-dir",
            str(server_control),
            "--runtime-dir",
            str(root / "sr"),
            "--database",
            str(database),
            "--sbps-endpoint",
            str(endpoint),
        ],
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
    )
    wait_for_path(endpoint)
    listener = subprocess.Popen(
        [
            args.listener,
            "--foreground",
            "--protocol-family=sbsql",
            "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1",
            f"--database-selector=dev_bootstrap_path:{database}",
            f"--server-endpoint=unix:{endpoint}",
            f"--parser-executable={args.parser_worker}",
            f"--control-dir={root / 'lc'}",
            f"--runtime-dir={root / 'lr'}",
            "--bind-address=127.0.0.1",
            f"--port={port}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
        ],
        stdout=(root / "listener.out").open("wb"),
        stderr=(root / "listener.err").open("wb"),
    )
    wait_for_tcp(port)
    return Route(
        name="inet",
        root=root,
        database=database,
        args=[
            args.sb_isql,
            str(database),
            "--host=127.0.0.1",
            f"--port={port}",
            "--sslmode=disable",
            "-U",
            "alice",
            "-P",
            auth_evidence(),
        ],
        processes=[server, listener],
    )


def run_sql(route: Route, case: str, sql: str, timeout: int = 35) -> RunResult:
    case_dir = route.root / case
    case_dir.mkdir(parents=True, exist_ok=True)
    script = case_dir / "script.sql"
    script.write_text(sql, encoding="utf-8")
    out_path = case_dir / "sb_isql.out"
    err_path = case_dir / "sb_isql.err"
    completed = subprocess.run(
        route.args + ["-q", "-A", "-t", "-b", "-f", str(script)],
        stdout=out_path.open("wb"),
        stderr=err_path.open("wb"),
        check=False,
        timeout=timeout,
    )
    return RunResult(
        route=route.name,
        case=case,
        returncode=completed.returncode,
        stdout=out_path.read_text(encoding="utf-8", errors="replace").strip(),
        stderr=err_path.read_text(encoding="utf-8", errors="replace").strip(),
        stdout_path=out_path,
        stderr_path=err_path,
    )


def data_lines(stdout: str) -> list[str]:
    lines: list[str] = []
    for raw in stdout.splitlines():
        line = raw.strip()
        if not line or line.startswith("Rows affected:") or line.startswith("Stopping due to error"):
            continue
        if line.startswith("NATIVE_BULK_INGEST ") or line.startswith("COPY "):
            continue
        lines.append(line)
    return lines


def parse_show_management(stdout: str) -> dict[str, str]:
    rows = data_lines(stdout)
    candidates: list[dict[str, str]] = []
    for row in rows:
        values = row.split("|")
        if len(values) >= 38 and values[4] == "scratchbird.performance_optimization_surface.v1":
            fields = {
                "read_only_mode": values[0],
                "catalog_generation_id": values[1],
                "security_epoch": values[2],
                "resource_epoch": values[3],
                "performance_optimization_surface": values[4],
                "optimization_profile": values[5],
                "copy_append_batching_enabled": values[6],
                "plan_cache_enabled": values[7],
                "descriptor_metadata_cache_enabled": values[8],
                "statistics_epoch": values[9],
                "agent_worker_status": values[17],
                "resource_governor_state": values[26],
                "parser_finality_authority": values[35],
                "donor_finality_authority": values[36],
            }
            candidates.append(fields)
    require(len(candidates) == 1, f"SHOW MANAGEMENT performance surface row drifted: {rows!r}")
    fields = candidates[0]
    expected = {
        "performance_optimization_surface": "scratchbird.performance_optimization_surface.v1",
        "optimization_profile": "runtime_default",
        "copy_append_batching_enabled": "true",
        "plan_cache_enabled": "true",
        "descriptor_metadata_cache_enabled": "true",
        "parser_finality_authority": "false",
        "donor_finality_authority": "false",
    }
    for key, value in expected.items():
        require(fields.get(key) == value, f"SHOW MANAGEMENT {key} drifted: {fields.get(key)!r}")
    require(fields["statistics_epoch"].isdigit(), "SHOW MANAGEMENT statistics_epoch was not numeric")
    require(bool(fields["agent_worker_status"]), "SHOW MANAGEMENT agent_worker_status was empty")
    require(bool(fields["resource_governor_state"]), "SHOW MANAGEMENT resource_governor_state was empty")
    return fields


def quote_path(path: Path) -> str:
    require("'" not in str(path), f"path cannot contain a quote: {path}")
    return f"'{path}'"


def route_live_evidence(route: Route) -> dict[str, Any]:
    rows = route.root / "copy.rows"
    rows.write_text("id=1\nid=2\n", encoding="utf-8")
    show = run_sql(route, "show_management", "SHOW MANAGEMENT;\n")
    require(show.returncode == 0 and not show.stderr, f"{route.name} SHOW MANAGEMENT failed: {show.stderr!r}")
    fields = parse_show_management(show.stdout)

    ddl = run_sql(route, "create_target", f"CREATE TABLE {TARGET} (id int);\n")
    require(ddl.returncode == 0 and not ddl.stderr, f"{route.name} target DDL failed: {ddl.stderr!r}")

    native_disabled = run_sql(
        route,
        "native_disabled",
        f"\\native_bulk_ingest {TARGET} FROM {quote_path(rows)} DISABLED\n",
    )
    require(native_disabled.returncode != 0, f"{route.name} disabled native ingest was accepted")
    require(
        native_disabled.diagnostic_code == EXPECTED_DISABLED,
        f"{route.name} disabled native diagnostic drifted: {native_disabled.diagnostic_code!r}",
    )

    copy_after_refusal = run_sql(
        route,
        "copy_after_native_disabled",
        "\n".join([
            f"\\copy {TARGET} FROM {quote_path(rows)}",
            f"SELECT COUNT(*) FROM {TARGET};",
            "",
        ]),
    )
    require(
        copy_after_refusal.returncode == 0 and
        "COPY " in copy_after_refusal.stdout and
        data_lines(copy_after_refusal.stdout) == ["2"] and
        not copy_after_refusal.stderr,
        f"{route.name} COPY baseline failed after disabled native refusal: "
        f"rc={copy_after_refusal.returncode} stdout={copy_after_refusal.stdout!r} stderr={copy_after_refusal.stderr!r}",
    )

    return {
        "route": route.name,
        "show_management_fields": fields,
        "show_management_stdout_path": str(show.stdout_path),
        "native_disabled": {
            "status": "refused",
            "message_vector": [EXPECTED_DISABLED],
            "stderr_path": str(native_disabled.stderr_path),
        },
        "copy_after_native_disabled": {
            "status": "accepted",
            "rows_returned": 1,
            "row_count": 2,
            "stdout_path": str(copy_after_refusal.stdout_path),
            "result_hash": sha256("copy_after_native_disabled:accepted:2"),
        },
    }


def source_and_cmake_evidence(repo_root: Path) -> dict[str, dict[str, Any]]:
    tests = {
        "cdp_copy_append_batching_gate": "project/tests/database_lifecycle/cdp_copy_append_batching_gate.cpp",
        "cdp_page_filespace_demand_gate": "project/tests/database_lifecycle/cdp_page_filespace_demand_gate.cpp",
        "cdp_plan_cache_live_gate": "project/tests/database_lifecycle/cdp_plan_cache_live_gate.cpp",
        "cdp_uuid_descriptor_cache_gate": "project/tests/database_lifecycle/cdp_uuid_descriptor_cache_gate.cpp",
        "cdp_statistics_access_path_gate": "project/tests/database_lifecycle/cdp_statistics_access_path_gate.cpp",
        "cdp_join_costing_gate": "project/tests/database_lifecycle/cdp_join_costing_gate.cpp",
        "cdp030_agent_worker_capacity_gate": "project/tests/agents/cdp030_agent_worker_capacity_gate.cpp",
        "server_agent_thread_runtime_gate": "project/tests/sbsql_parser_worker/server_agent_thread_runtime_gate.py",
        "cdp031_agent_resource_governor_gate": "project/tests/agents/cdp031_agent_resource_governor_gate.cpp",
        "cdp_native_bulk_ingest_api_gate": "project/tests/database_lifecycle/cdp_native_bulk_ingest_api_gate.cpp",
        "cdp_native_bulk_ingest_cli_gate": "project/tests/sbsql_parser_worker/cdp_native_bulk_ingest_cli_gate.py",
        "cdp_security_api_abi_gate": "project/tests/sbsql_parser_worker/cdp_security_api_abi_gate.py",
    }
    required_tokens = {
        "cdp_copy_append_batching_gate": ("copy_append_batching=disabled", "singleton baseline"),
        "cdp_page_filespace_demand_gate": ("disabled_request.enabled = false", "foreground fallback"),
        "cdp_plan_cache_live_gate": ("optimizer_plan_cache:disabled", "disabled plan cache fallback"),
        "cdp_uuid_descriptor_cache_gate": ("descriptor_cache:disabled", "disabled descriptor cache fallback"),
        "cdp_statistics_access_path_gate": ("optimizer_statistics:disabled", "disabled statistics fallback"),
        "cdp_join_costing_gate": ("optimizer_join_costing:disabled", "disabled join costing"),
        "cdp030_agent_worker_capacity_gate": ("max_background_worker_slots = 1", "foreground"),
        "server_agent_thread_runtime_gate": ("agent_worker", "foreground"),
        "cdp031_agent_resource_governor_gate": ("hard limit", "soft limit"),
        "cdp_native_bulk_ingest_api_gate": ("native_bulk_ingest_enabled:false", EXPECTED_DISABLED),
        "cdp_native_bulk_ingest_cli_gate": ("DISABLED", EXPECTED_DISABLED),
        "cdp_security_api_abi_gate": ("native_disabled", EXPECTED_DISABLED),
    }
    cmake_files = [
        repo_root / "project/tests/database_lifecycle/CMakeLists.txt",
        repo_root / "project/tests/agents/CMakeLists.txt",
        repo_root / "project/tests/sbsql_parser_worker/CMakeLists.txt",
    ]
    cmake_text = "\n".join(read_text(path) for path in cmake_files)
    evidence: dict[str, dict[str, Any]] = {}
    for test_name, relative_path in tests.items():
        source = repo_root / relative_path
        require(source.exists(), f"referenced standalone source missing: {relative_path}")
        source_text = read_text(source)
        require(source_text.strip(), f"{test_name} source unreadable")
        tokens = required_tokens[test_name]
        missing_tokens = [token for token in tokens if token not in source_text]
        require(
            not missing_tokens,
            f"{test_name} missing required disabled/rollback evidence tokens: {missing_tokens}",
        )
        registration_patterns = (
            f"NAME {test_name}",
            f"add_executable({test_name}",
            f"sb_agent_runtime_cpp_gate(\n  {test_name}",
        )
        require(
            any(pattern in cmake_text for pattern in registration_patterns),
            f"{test_name} missing CMake source/test registration",
        )
        evidence[test_name] = {
            "source_path": relative_path,
            "cmake_registration": "present",
            "source_registration": "present",
            "matched_evidence_tokens": list(tokens),
        }
    return evidence


def candidate_manifest(regression: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    def test_refs(*names: str) -> list[dict[str, Any]]:
        return [{"ctest_name": name, **regression[name]} for name in names]

    manifest = [
        {
            "candidate": "COPY batching",
            "compile_time_flag": "none",
            "runtime_config_name": "scratchbird.dml.copy_append_batching.enabled",
            "development_default": True,
            "benchmark_clean_default": True,
            "beta_packaged_default": True,
            "disabled_behavior": "falls back to singleton COPY append path while COPY remains available",
            "disabled_refusal_message_vector": [],
            "support_bundle_show_field": "copy_append_batching_enabled",
            "standalone_regression_test_evidence": test_refs("cdp_copy_append_batching_gate"),
        },
        {
            "candidate": "page/filespace DML demand hints",
            "compile_time_flag": "none",
            "runtime_config_name": "scratchbird.dml.page_filespace_preallocation.enabled",
            "development_default": True,
            "benchmark_clean_default": True,
            "beta_packaged_default": True,
            "disabled_behavior": "uses on-demand page and filespace growth without preallocation hints",
            "disabled_refusal_message_vector": [],
            "support_bundle_show_field": "page_filespace_preallocation_enabled",
            "standalone_regression_test_evidence": test_refs("cdp_page_filespace_demand_gate"),
        },
        {
            "candidate": "plan cache",
            "compile_time_flag": "none",
            "runtime_config_name": "scratchbird.optimizer.plan_cache.enabled",
            "development_default": True,
            "benchmark_clean_default": True,
            "beta_packaged_default": True,
            "disabled_behavior": "plans are rebuilt per statement; execution remains engine-owned",
            "disabled_refusal_message_vector": [],
            "support_bundle_show_field": "plan_cache_enabled",
            "standalone_regression_test_evidence": test_refs("cdp_plan_cache_live_gate"),
        },
        {
            "candidate": "UUID descriptor metadata cache",
            "compile_time_flag": "none",
            "runtime_config_name": "scratchbird.catalog.descriptor_metadata_cache.enabled",
            "development_default": True,
            "benchmark_clean_default": True,
            "beta_packaged_default": True,
            "disabled_behavior": "descriptor metadata resolves through catalog authority without cache hits",
            "disabled_refusal_message_vector": [],
            "support_bundle_show_field": "descriptor_metadata_cache_enabled",
            "standalone_regression_test_evidence": test_refs("cdp_uuid_descriptor_cache_gate"),
        },
        {
            "candidate": "statistics access path",
            "compile_time_flag": "none",
            "runtime_config_name": "scratchbird.optimizer.statistics.enabled",
            "development_default": True,
            "benchmark_clean_default": True,
            "beta_packaged_default": True,
            "disabled_behavior": "optimizer uses fail-safe access path without stale statistics authority",
            "disabled_refusal_message_vector": [],
            "support_bundle_show_field": "statistics_epoch",
            "standalone_regression_test_evidence": test_refs("cdp_statistics_access_path_gate"),
        },
        {
            "candidate": "join costing",
            "compile_time_flag": "none",
            "runtime_config_name": "scratchbird.optimizer.join_costing.enabled",
            "development_default": True,
            "benchmark_clean_default": True,
            "beta_packaged_default": True,
            "disabled_behavior": "falls back to deterministic non-costed join selection",
            "disabled_refusal_message_vector": [],
            "support_bundle_show_field": "selected_join_algorithm",
            "standalone_regression_test_evidence": test_refs("cdp_join_costing_gate"),
        },
        {
            "candidate": "agent worker parallelism",
            "compile_time_flag": "none",
            "runtime_config_name": "scratchbird.agents.worker_parallelism.enabled",
            "development_default": True,
            "benchmark_clean_default": True,
            "beta_packaged_default": True,
            "disabled_behavior": "agent work executes through bounded single-worker runtime",
            "disabled_refusal_message_vector": [],
            "support_bundle_show_field": "agent_worker_status",
            "standalone_regression_test_evidence": test_refs(
                "cdp030_agent_worker_capacity_gate", "server_agent_thread_runtime_gate"
            ),
        },
        {
            "candidate": "resource governor",
            "compile_time_flag": "none",
            "runtime_config_name": "scratchbird.resource_governor.enabled",
            "development_default": True,
            "benchmark_clean_default": True,
            "beta_packaged_default": True,
            "disabled_behavior": "quota grants are bypassed while admission and MGA finality remain authoritative",
            "disabled_refusal_message_vector": [],
            "support_bundle_show_field": "resource_governor_state",
            "standalone_regression_test_evidence": test_refs("cdp031_agent_resource_governor_gate"),
        },
        {
            "candidate": "native bulk ingest",
            "compile_time_flag": "none",
            "runtime_config_name": "native_bulk_ingest_enabled",
            "development_default": True,
            "benchmark_clean_default": False,
            "beta_packaged_default": False,
            "disabled_behavior": "refuses native bulk ingest and leaves public COPY route available",
            "disabled_refusal_message_vector": [EXPECTED_DISABLED],
            "support_bundle_show_field": "native_ingest_enabled",
            "standalone_regression_test_evidence": test_refs(
                "cdp_native_bulk_ingest_api_gate",
                "cdp_native_bulk_ingest_cli_gate",
                "cdp_security_api_abi_gate",
            ),
        },
    ]
    validate_manifest(manifest)
    return manifest


def validate_manifest(manifest: list[dict[str, Any]]) -> None:
    expected_candidates = {
        "COPY batching",
        "page/filespace DML demand hints",
        "plan cache",
        "UUID descriptor metadata cache",
        "statistics access path",
        "join costing",
        "agent worker parallelism",
        "resource governor",
        "native bulk ingest",
    }
    require({entry.get("candidate") for entry in manifest} == expected_candidates, "candidate manifest set drifted")
    for entry in manifest:
        missing = [field for field in REQUIRED_MANIFEST_FIELDS if field not in entry]
        require(not missing, f"{entry.get('candidate')} missing manifest fields: {missing}")
        require(entry["compile_time_flag"] or entry["compile_time_flag"] == "none", f"{entry['candidate']} empty flag")
        require(entry["runtime_config_name"], f"{entry['candidate']} empty runtime_config_name")
        require(entry["disabled_behavior"], f"{entry['candidate']} empty disabled_behavior")
        require(entry["support_bundle_show_field"], f"{entry['candidate']} empty support/show field")
        require(entry["standalone_regression_test_evidence"], f"{entry['candidate']} missing regression evidence")
        if entry["candidate"] == "native bulk ingest":
            require(
                entry["disabled_refusal_message_vector"] == [EXPECTED_DISABLED],
                "native bulk ingest exact disabled vector drifted",
            )


def forbid_execution_plan_paths(value: Any, trail: str = "payload") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            forbid_execution_plan_paths(child, f"{trail}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            forbid_execution_plan_paths(child, f"{trail}[{index}]")
    elif isinstance(value, str):
        require(FORBIDDEN_EXECUTION_PLAN_PATH not in value, f"JSON payload contains execution_plan path at {trail}")


def run_gate(args: argparse.Namespace, work: Path) -> dict[str, Any]:
    repo_root = Path(args.repo_root).resolve()
    regression = source_and_cmake_evidence(repo_root)
    manifest = candidate_manifest(regression)
    routes = [start_embedded(args, work), start_local_ipc(args, work), start_inet(args, work)]
    live: list[dict[str, Any]] = []
    try:
        for route in routes:
            live.append(route_live_evidence(route))
    finally:
        for route in reversed(routes):
            for proc in reversed(route.processes):
                stop_process(proc)

    show_hashes = {sha256(json.dumps(record["show_management_fields"], sort_keys=True)) for record in live}
    require(len(show_hashes) == 1, f"SHOW MANAGEMENT defaults differ by route: {sorted(show_hashes)}")
    vectors = sorted({code for record in live for code in record["native_disabled"]["message_vector"]})
    require(vectors == [EXPECTED_DISABLED], f"disabled refusal vector drifted: {vectors}")

    payload = {
        "schema_version": SCHEMA_VERSION,
        "timestamp_utc": utc_timestamp(),
        "routes": [route.name for route in routes],
        "candidate_manifest": manifest,
        "live_route_evidence": live,
        "source_cmake_regression_evidence": regression,
        "default_show_management_hash": next(iter(show_hashes)),
        "exact_refusal_vectors": vectors,
        "authority_boundary": {
            "parser_finality_authority": False,
            "donor_finality_authority": False,
            "finality_visibility_authority": "engine_mga",
            "donor_engine_execution": False,
            "sqlite_wal_shortcut": False,
        },
        "runtime_dependency_policy": {
            "docs_execution-plans_runtime_reads": False,
            "inspected_roots": ["project/tests", "project/src"],
        },
    }
    output = work / "evidence" / "cdp-config-defaults-rollback-gate.json"
    payload["output_json"] = str(output)
    forbid_execution_plan_paths(payload)
    write_json(output, payload)
    forbid_execution_plan_paths(json.loads(output.read_text(encoding="utf-8")))
    return payload


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args(argv[1:])

    work = make_work_dir(Path(args.work_dir))
    try:
        payload = run_gate(args, work)
        print(f"cdp_config_defaults_rollback_gate=passed output={payload['output_json']}")
        print("cdp_config_defaults_rollback_vectors=" + ",".join(payload["exact_refusal_vectors"]))
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest should receive the concrete failure.
        print(f"cdp_config_defaults_rollback_gate=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
