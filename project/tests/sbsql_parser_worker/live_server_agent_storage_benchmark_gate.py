#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""PFAR-019 live server/listener/parser/agent/storage integration gate."""

from __future__ import annotations

import argparse
import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any

import live_auth_fixture


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
ALICE_PRINCIPAL_UUID = live_auth_fixture.DEFAULT_PRINCIPAL_UUID
SYSDBA_PRINCIPAL_UUID = "019f0a11-ce00-7000-8000-000000000002"
PRINCIPAL_UUIDS = {
    "alice": ALICE_PRINCIPAL_UUID,
    "sysdba": SYSDBA_PRINCIPAL_UUID,
}


class GateError(RuntimeError):
    pass


def make_work_dir(preferred_root: Path) -> Path:
    roots = (preferred_root, Path(tempfile.gettempdir()) / "sb_live_agent_storage")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="pf19_", dir=root))
        endpoint_probe = candidate / "sc1" / "s.sock"
        listener_probe = candidate / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock")
        if max(len(str(endpoint_probe)), len(str(listener_probe))) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise GateError("unable to allocate a short-enough live gate workspace")


def find_free_port() -> int:
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


def wait_for_exit(proc: subprocess.Popen[bytes], timeout: float = 8.0) -> None:
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raise GateError(f"process did not exit within {timeout:.0f}s") from exc


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def dump_logs(work: Path) -> None:
    for path in sorted(work.glob("*.out")) + sorted(work.glob("*.err")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.name} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def require_uuid(value: str, field_name: str) -> None:
    require(value, f"{field_name}_missing")
    parsed = uuid.UUID(value)
    require(str(parsed) == value.lower(), f"{field_name}_not_canonical_uuid:{value}")
    forbidden = ("agent.", "policy.", "scope.", "agent:", "policy:", "scope:", "agent-")
    require(not value.startswith(forbidden), f"{field_name}_label_prefixed:{value}")


def parse_ipc_json(completed: subprocess.CompletedProcess[bytes], scenario: str) -> dict[str, Any]:
    stdout_text = completed.stdout.decode("utf-8", errors="replace").strip()
    text = next((line.strip() for line in reversed(stdout_text.splitlines()) if line.strip()), "")
    require(completed.returncode == 0, f"{scenario}_ipc_tester_failed:{completed.returncode}:{text}")
    try:
        outer = json.loads(text)
    except json.JSONDecodeError as exc:
        payload_marker = '"payload":"'
        if '"expectation_match":true' not in text or payload_marker not in text:
            raise GateError(f"{scenario}_ipc_tester_non_json:{stdout_text}") from exc
        payload_start = text.find(payload_marker) + len(payload_marker)
        payload_end = text.rfind('"}}')
        require(payload_end > payload_start, f"{scenario}_ipc_tester_payload_bounds_invalid")
        payload = text[payload_start:payload_end]
        return {
            "expectation_match": True,
            "payload": payload.replace('\\\\"', '"').replace('\\\\n', '\n'),
        }
    info = outer.get("ipc_tester", {})
    require(info.get("expectation_match") is True, f"{scenario}_expectation_mismatch:{text}")
    return info


def run_ipc(args: argparse.Namespace,
            endpoint: Path,
            scenario: str,
            log_prefix: str,
            *extra: str) -> dict[str, Any]:
    out_path = args.work / f"{log_prefix}_{scenario}.out"
    err_path = args.work / f"{log_prefix}_{scenario}.err"
    extra_args = list(extra)
    if scenario.startswith("management_") and "--principal-uuid" not in extra_args:
        principal = "alice"
        if "--principal" in extra_args:
            principal_index = extra_args.index("--principal")
            if principal_index + 1 < len(extra_args):
                principal = extra_args[principal_index + 1]
        principal_uuid = PRINCIPAL_UUIDS.get(principal)
        if principal_uuid:
            extra_args.extend(["--principal-uuid", principal_uuid])
    command = [
        args.ipc_tester,
        "--endpoint",
        str(endpoint),
        "--scenario",
        scenario,
        "--expect",
        "accept",
        *extra_args,
    ]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    out_path.write_bytes(completed.stdout)
    err_path.write_bytes(completed.stderr)
    if completed.returncode != 0:
        raise GateError(
            f"{scenario}_failed:{completed.returncode}:"
            f"{completed.stderr.decode('utf-8', errors='replace')}"
        )
    return parse_ipc_json(completed, scenario)


def decode_payload_json(info: dict[str, Any], scenario: str) -> dict[str, Any]:
    payload = info.get("payload", "")
    require(isinstance(payload, str) and payload, f"{scenario}_payload_missing")
    try:
        return json.loads(payload)
    except json.JSONDecodeError as exc:
        raise GateError(f"{scenario}_payload_not_json:{payload}") from exc


def first_database(status: dict[str, Any]) -> dict[str, Any]:
    databases = status.get("engine_host", {}).get("databases", [])
    require(isinstance(databases, list) and databases, "database_status_missing_database")
    database = databases[0]
    require(isinstance(database, dict), "database_status_database_not_object")
    return database


def assert_agent_runtime(database: dict[str, Any], *, expect_created: bool, expected_uuid: str | None) -> str:
    require(database.get("database_open") is True, "database_not_open")
    require(database.get("database_created") is expect_created, "database_created_flag_mismatch")
    database_uuid = str(database.get("database_uuid", ""))
    require_uuid(database_uuid, "database_uuid")
    if expected_uuid is not None:
        require(database_uuid == expected_uuid, "database_uuid_changed_across_recovery")

    require(database.get("database_engine_agent_state") in {"active", "restricted", "draining"},
            f"unexpected_agent_state:{database.get('database_engine_agent_state')}")
    require(int(database.get("database_engine_agent_health_generation", 0)) >= 1,
            "agent_health_generation_missing")
    require(database.get("database_engine_agent_ordinary_admission_allowed") is True,
            "ordinary_agent_admission_not_allowed")

    health = database.get("database_engine_agent_health", {}).get("database_engine_agent", {})
    require(health.get("authority_boundary_valid") is True, "agent_authority_boundary_invalid")
    require(health.get("cluster_paths_failed_closed") is True, "noncluster_cluster_paths_not_failed_closed")
    selected = health.get("selected_agents", [])
    require("page_allocation_manager" in selected, "page_allocation_manager_not_selected")
    require("filespace_capacity_manager" in selected, "filespace_capacity_manager_not_selected")
    return database_uuid


def start_server(args: argparse.Namespace,
                 database: Path,
                 control_dir: Path,
                 runtime_dir: Path,
                 endpoint: Path,
                 log_prefix: str,
                 create_if_missing: bool) -> subprocess.Popen[bytes]:
    command = [
        args.server,
        "--foreground",
        "--no-listeners",
        "--control-dir",
        str(control_dir),
        "--runtime-dir",
        str(runtime_dir),
        "--database",
        str(database),
        "--sbps-endpoint",
        str(endpoint),
    ]
    if create_if_missing:
        command.insert(2, "--create-if-missing")
    proc = subprocess.Popen(
        command,
        stdout=(args.work / f"{log_prefix}_server.out").open("wb"),
        stderr=(args.work / f"{log_prefix}_server.err").open("wb"),
    )
    wait_for_path(endpoint)
    return proc


def stop_server_via_ipc(args: argparse.Namespace, endpoint: Path, proc: subprocess.Popen[bytes], log_prefix: str) -> None:
    run_ipc(args, endpoint, "management_stop_server", log_prefix, "--principal", "sysdba")
    wait_for_exit(proc)


def write_live_auth_fixture(database: Path) -> None:
    live_auth_fixture.write_local_password_auth_fixture(
        database,
        "alice",
        VERIFIER,
        ALICE_PRINCIPAL_UUID,
        append=False,
    )
    live_auth_fixture.write_local_password_auth_fixture(
        database,
        "sysdba",
        VERIFIER,
        SYSDBA_PRINCIPAL_UUID,
        append=True,
    )


def run_listener_route(args: argparse.Namespace, database: Path, endpoint: Path) -> None:
    listener_control = args.work / "lc"
    listener_runtime = args.work / "lr"
    port = find_free_port()
    listener: subprocess.Popen[bytes] | None = None
    try:
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
                f"--control-dir={listener_control}",
                f"--runtime-dir={listener_runtime}",
                "--bind-address=127.0.0.1",
                f"--port={port}",
                "--warm-pool-min=1",
                "--warm-pool-max=2",
            ],
            stdout=(args.work / "listener.out").open("wb"),
            stderr=(args.work / "listener.err").open("wb"),
        )
        wait_for_tcp(port)
        evidence = f"scheme=local_password_v1;principal=alice;verifier={VERIFIER}"
        completed = subprocess.run(
            [
                args.sb_isql,
                str(database),
                "--host=127.0.0.1",
                f"--port={port}",
                "--sslmode=disable",
                "-U",
                "alice",
                "-P",
                evidence,
                "-q",
                "-A",
                "-t",
                "-c",
                "SELECT 1",
            ],
            stdout=(args.work / "sb_isql.out").open("wb"),
            stderr=(args.work / "sb_isql.err").open("wb"),
            check=False,
        )
        require(completed.returncode == 0, f"sb_isql_failed:{completed.returncode}")
        output = (args.work / "sb_isql.out").read_text(encoding="utf-8", errors="replace").strip()
        require(output == "1", f"sb_isql_select_unexpected:{output!r}")
    finally:
        stop_process(listener)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--ipc-tester", required=True)
    parser.add_argument("--work-dir", required=True)
    parsed = parser.parse_args()
    parsed.work = make_work_dir(Path(parsed.work_dir))

    server1: subprocess.Popen[bytes] | None = None
    server2: subprocess.Popen[bytes] | None = None
    try:
        database = parsed.work / "live.sbdb"
        write_live_auth_fixture(database)

        endpoint1 = parsed.work / "sc1" / "s.sock"
        server1 = start_server(
            parsed, database, parsed.work / "sc1", parsed.work / "sr1", endpoint1, "first", True
        )
        status1 = decode_payload_json(run_ipc(parsed, endpoint1, "database_status", "first"), "database_status")
        database_uuid = assert_agent_runtime(first_database(status1), expect_created=True, expected_uuid=None)

        run_listener_route(parsed, database, endpoint1)

        metrics = run_ipc(
            parsed,
            endpoint1,
            "management_show_metrics",
            "first",
            "--expect-payload-contains",
            "sys.metrics.server.management.request_total",
        )
        require("sys.metrics.server.management.request_total" in str(metrics.get("payload", "")),
                "management_metrics_missing_server_request_total")

        support = run_ipc(
            parsed,
            endpoint1,
            "management_export_support_bundle",
            "first",
            "--principal",
            "sysdba",
            "--expect-payload-contains",
            "support_bundle",
        )
        require("support_bundle" in str(support.get("payload", "")), "support_bundle_payload_missing")

        stop_server_via_ipc(parsed, endpoint1, server1, "first")
        server1 = None

        endpoint2 = parsed.work / "sc2" / "s.sock"
        server2 = start_server(
            parsed, database, parsed.work / "sc2", parsed.work / "sr2", endpoint2, "second", False
        )
        status2 = decode_payload_json(run_ipc(parsed, endpoint2, "database_status", "second"), "database_status")
        recovered = first_database(status2)
        assert_agent_runtime(recovered, expect_created=False, expected_uuid=database_uuid)
        require(str(recovered.get("startup_recovery_classification", "")),
                "startup_recovery_classification_missing")
        stop_server_via_ipc(parsed, endpoint2, server2, "second")
        server2 = None

        print(f"live_server_agent_storage_benchmark_gate=passed work={parsed.work}")
        shutil.rmtree(parsed.work, ignore_errors=True)
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest needs the concrete failure and logs.
        print(f"live_server_agent_storage_benchmark_gate=failed work={parsed.work}: {exc}", file=sys.stderr)
        dump_logs(parsed.work)
        return 1
    finally:
        stop_process(server2)
        stop_process(server1)


if __name__ == "__main__":
    raise SystemExit(main())
