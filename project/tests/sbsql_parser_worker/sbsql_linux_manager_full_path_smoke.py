#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Linux manager/listener/SBSQL/parser/server full-path smoke.

Drives the public CLI through:
  sb_isql -> TCP -> sbmn_manager -> LPREFACE/DBBT listener control ->
  sb_listener -> sbp_sbsql -> SBPS IPC -> sb_server -> engine query.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from live_auth_fixture import DEFAULT_PRINCIPAL_UUID, write_temporary_token_auth_fixture


VERIFIER = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
DBBT_KEY_HEX = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
MANAGER_TOKEN = "manager-route-token-1"
MANAGER_DB = "linux_gold_route"
OWNER_DB_UUID = "019f0000000070008000000000000a11"
TOKEN_HANDLE = "linux-manager-full-path-token-handle"
CONTROL_MAGIC = 0x54434253
CONTROL_VERSION = 1
CONTROL_HEADER = struct.Struct("<IHHHHQQ")
CONTROL_MANAGEMENT_COMMAND = 0x0060
CONTROL_MANAGEMENT_RESPONSE = 0x0061


class SmokeError(RuntimeError):
    pass


def make_work_dir(preferred_root: Path) -> Path:
    roots = (preferred_root, Path(tempfile.gettempdir()) / "sbmfp")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="sbm_", dir=root))
        endpoint_probe = candidate / "sc" / "s.sock"
        listener_probe = candidate / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock")
        manager_probe = candidate / "mc" / "sbmn_manager.control.sock"
        if max(len(str(endpoint_probe)), len(str(listener_probe)), len(str(manager_probe))) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise SmokeError("unable to allocate a short-enough Linux full-path workspace")


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
    raise SmokeError(f"timed out waiting for {path}")


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
    raise SmokeError(f"timed out waiting for TCP port {port}: {last_error}")


def wait_for_single_management_socket(control_dir: Path, timeout: float = 8.0) -> Path:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        matches = sorted(control_dir.glob("*.management.sock"))
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            raise SmokeError(f"ambiguous listener management sockets: {matches}")
        time.sleep(0.05)
    raise SmokeError(f"timed out waiting for listener management socket in {control_dir}")


def wait_for_file_contains(path: Path, needle: str, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and needle in path.read_text(encoding="utf-8", errors="replace"):
            return
        time.sleep(0.05)
    raise SmokeError(f"{path} did not contain {needle!r}")


def wait_for_json_metric(path: Path, metric_name: str, minimum: int, timeout: float = 5.0) -> int:
    deadline = time.monotonic() + timeout
    last_value = 0
    while time.monotonic() < deadline:
        if path.exists():
            data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
            for metric in data.get("metrics", []):
                if metric.get("name") == metric_name:
                    try:
                        last_value = int(metric.get("value", 0))
                    except (TypeError, ValueError):
                        last_value = 0
                    if last_value >= minimum:
                        return last_value
        time.sleep(0.05)
    raise SmokeError(f"{metric_name} stayed below {minimum}; last={last_value}")


def encode_control_frame(opcode: int, request_id: int, payload: bytes) -> bytes:
    return CONTROL_HEADER.pack(
        CONTROL_MAGIC,
        CONTROL_VERSION,
        opcode,
        0,
        0,
        request_id,
        len(payload),
    ) + payload


def read_exact(sock: socket.socket, byte_count: int) -> bytes:
    chunks: list[bytes] = []
    remaining = byte_count
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise SmokeError(f"listener management response ended with {remaining} bytes unread")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_listener_status(management_socket: Path) -> dict[str, object]:
    request_id = time.time_ns() & 0xFFFFFFFFFFFFFFFF
    frame = encode_control_frame(CONTROL_MANAGEMENT_COMMAND, request_id, b"STATUS")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(3.0)
        sock.connect(str(management_socket))
        sock.sendall(frame)
        header = read_exact(sock, CONTROL_HEADER.size)
        magic, version, opcode, _flags, reserved, response_id, payload_len = CONTROL_HEADER.unpack(header)
        if magic != CONTROL_MAGIC or version != CONTROL_VERSION or reserved != 0:
            raise SmokeError("listener management response control header was invalid")
        if opcode != CONTROL_MANAGEMENT_RESPONSE:
            raise SmokeError(f"listener management returned opcode {opcode:#x}")
        if response_id != request_id:
            raise SmokeError(f"listener management response id mismatch {response_id} != {request_id}")
        if payload_len == 0 or payload_len > 65536:
            raise SmokeError(f"listener management response payload length invalid: {payload_len}")
        payload = read_exact(sock, int(payload_len))
    status = payload[0]
    body = payload[1:].decode("utf-8", errors="replace")
    if status != 0:
        raise SmokeError(f"listener STATUS refused: {body}")
    loaded = json.loads(body)
    if not isinstance(loaded, dict) or loaded.get("ok") is not True:
        raise SmokeError(f"listener STATUS body was not an ok object: {body}")
    status_body = loaded.get("status")
    if not isinstance(status_body, dict):
        raise SmokeError(f"listener STATUS did not include status object: {body}")
    return status_body


def listener_counter(status: dict[str, object], name: str) -> int:
    metrics = status.get("metrics")
    if not isinstance(metrics, dict):
        return 0
    counters = metrics.get("counters")
    if not isinstance(counters, dict):
        return 0
    try:
        return int(counters.get(name, 0))
    except (TypeError, ValueError):
        return 0


def wait_for_listener_metric(management_socket: Path,
                             metric_name: str,
                             minimum: int,
                             timeout: float = 5.0) -> tuple[dict[str, object], int]:
    deadline = time.monotonic() + timeout
    last_status: dict[str, object] = {}
    last_value = 0
    while time.monotonic() < deadline:
        last_status = read_listener_status(management_socket)
        last_value = listener_counter(last_status, metric_name)
        if last_value >= minimum:
            return last_status, last_value
        time.sleep(0.05)
    raise SmokeError(f"listener metric {metric_name} stayed below {minimum}; last={last_value}")


def require_listener_field_minimum(status: dict[str, object], field: str, minimum: int) -> int:
    try:
        value = int(status.get(field, 0))
    except (TypeError, ValueError):
        value = 0
    if value < minimum:
        raise SmokeError(f"listener status {field} stayed below {minimum}; actual={value}")
    return value


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def require_running(proc: subprocess.Popen[bytes] | None, name: str, work: Path) -> None:
    if proc is not None and proc.poll() is None:
        return
    code = proc.poll() if proc is not None else None
    raise SmokeError(f"{name} exited before readiness, rc={code}, work={work}")


def dump_logs(work: Path) -> None:
    for name in (
        "server.out",
        "server.err",
        "listener.out",
        "listener.err",
        "manager.out",
        "manager.err",
        "sb_isql.out",
        "sb_isql.err",
    ):
        path = work / name
        if path.exists():
            print(f"--- {name} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)


def write_private(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    os.chmod(path, 0o600)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--manager", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--evidence-file", required=True)
    args = parser.parse_args()

    work = make_work_dir(Path(args.work_dir))
    server: subprocess.Popen[bytes] | None = None
    listener: subprocess.Popen[bytes] | None = None
    manager: subprocess.Popen[bytes] | None = None
    success = False

    try:
        database = work / "managed_full_path.sbdb"
        server_control = work / "sc"
        server_runtime = work / "sr"
        listener_control = work / "lc"
        listener_runtime = work / "lr"
        manager_control = work / "mc"
        manager_runtime = work / "mr"
        endpoint = server_control / "s.sock"
        listener_port = find_free_port()
        manager_port = find_free_port()
        token_store = work / "manager_tokens.tsv"
        keyring = work / "dbbt.keyring"

        write_temporary_token_auth_fixture(
            database,
            "alice",
            MANAGER_TOKEN,
            TOKEN_HANDLE,
            DEFAULT_PRINCIPAL_UUID,
        )
        write_private(
            token_store,
            f"{MANAGER_TOKEN}\talice\t0\tactive\t"
            "database.connect,manager.status,listener.status\t"
            f"{DEFAULT_PRINCIPAL_UUID}\t{TOKEN_HANDLE}\tmga_security_principal_lifecycle\n",
        )
        write_private(
            keyring,
            "format=SBMN_DBBT_KEYRING_V1\n"
            "active_key_id=active\n"
            f"active_key_hex={DBBT_KEY_HEX}\n",
        )

        server = subprocess.Popen(
            [
                args.server,
                "--foreground",
                "--no-listeners",
                "--create-if-missing",
                "--control-dir",
                str(server_control),
                "--runtime-dir",
                str(server_runtime),
                "--database",
                str(database),
                "--sbps-endpoint",
                str(endpoint),
            ],
            stdout=(work / "server.out").open("wb"),
            stderr=(work / "server.err").open("wb"),
        )
        wait_for_path(endpoint)
        require_running(server, "sb_server", work)

        listener_env = os.environ.copy()
        listener_env["SCRATCHBIRD_LISTENER_DBBT_KEY_HEX"] = DBBT_KEY_HEX
        listener = subprocess.Popen(
            [
                args.listener,
                "--foreground",
                "--protocol-family=sbsql",
                "--listener-profile=default",
                "--controller-type=sbmn_manager",
                "--controller-uuid=linux-manager-full-path",
                "--managed-by-manager=true",
                "--bundle-contract-id=bundle.default@1",
                f"--database-selector=dev_bootstrap_path:{database}",
                f"--server-endpoint=unix:{endpoint}",
                f"--parser-executable={args.parser_worker}",
                f"--control-dir={listener_control}",
                f"--runtime-dir={listener_runtime}",
                "--bind-address=127.0.0.1",
                f"--port={listener_port}",
                "--warm-pool-min=1",
                "--warm-pool-max=2",
            ],
            stdout=(work / "listener.out").open("wb"),
            stderr=(work / "listener.err").open("wb"),
            env=listener_env,
        )
        wait_for_tcp(listener_port)
        management_socket = wait_for_single_management_socket(listener_control)
        require_running(listener, "sb_listener", work)

        manager = subprocess.Popen(
            [
                args.manager,
                "--foreground",
                "--runtime-dir",
                str(manager_runtime),
                "--control-dir",
                str(manager_control),
                "--bind",
                "127.0.0.1",
                "--port",
                str(manager_port),
                "--native-bind",
                "127.0.0.1",
                "--native-port",
                str(listener_port),
                "--owner-db-path",
                str(database),
                "--owner-db",
                MANAGER_DB,
                "--owner-db-uuid",
                OWNER_DB_UUID,
                "--listener-id",
                "1",
                "--listener-control-dir",
                str(listener_control),
                "--security-token-store",
                str(token_store),
                "--dbbt-keyring",
                str(keyring),
                "--release-profile",
                "test",
            ],
            stdout=(work / "manager.out").open("wb"),
            stderr=(work / "manager.err").open("wb"),
        )
        wait_for_tcp(manager_port)
        require_running(manager, "sbmn_manager", work)

        evidence = f"scheme=local_password_v1;principal=alice;verifier={VERIFIER}"
        completed = subprocess.run(
            [
                args.sb_isql,
                str(database),
                "--mode=managed",
                "--front-door-mode=manager_proxy",
                "--host=127.0.0.1",
                f"--port={manager_port}",
                "--manager-user=alice",
                f"--manager-db={MANAGER_DB}",
                f"--manager-auth-token={MANAGER_TOKEN}",
                "--manager-auth-fast-path=true",
                "--manager-profile=SBsql",
                "--manager-intent=SBsql",
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
            stdout=(work / "sb_isql.out").open("wb"),
            stderr=(work / "sb_isql.err").open("wb"),
            check=False,
        )
        if completed.returncode != 0:
            raise SmokeError(f"sb_isql exited {completed.returncode}")
        output = (work / "sb_isql.out").read_text(encoding="utf-8", errors="replace").strip()
        if output != "1":
            raise SmokeError(f"sb_isql SELECT 1 through manager returned {output!r}")

        audit = manager_control / "sbmn_manager.audit.jsonl"
        metrics = manager_control / "sbmn_manager.metrics.json"
        server_audit = server_control / "sb_server.audit.jsonl"
        wait_for_file_contains(audit, "MANAGER_PROXY_ADMISSION_DECISION")
        wait_for_file_contains(audit, "MANAGER_AUTH_DECISION")
        wait_for_file_contains(audit, "MANAGER_DB_CONNECT_DECISION")
        wait_for_file_contains(
            server_audit,
            '"event_type":"server.auth_handoff","actor_class":"server","outcome":"accepted"',
        )
        wait_for_file_contains(
            server_audit,
            '"event_type":"server.attach_database","actor_class":"server","outcome":"accepted"',
        )
        wait_for_file_contains(
            server_audit,
            '"event_type":"server.sblr.execute","actor_class":"server","outcome":"completed"',
        )
        client_bytes = wait_for_json_metric(metrics, "sb_manager_proxy_bytes_total", 1)
        listener_requests = wait_for_json_metric(metrics, "sb_manager_listener_control_requests_total", 1)
        listener_status, listener_network_handoffs = wait_for_listener_metric(
            management_socket,
            "sys.metrics.listener.network.handoff_complete_total",
            1,
        )
        _listener_status, listener_pool_handoffs = wait_for_listener_metric(
            management_socket,
            "sys.metrics.listener.handoff_complete_total",
            1,
        )
        _listener_status, listener_claims = wait_for_listener_metric(
            management_socket,
            "sys.metrics.listener.handoff_binding.claim_consumed_total",
            1,
        )
        listener_handoff_total = require_listener_field_minimum(
            listener_status,
            "handoff_complete_total",
            1,
        )
        pool = listener_status.get("pool")
        if not isinstance(pool, dict) or pool.get("running") is not True:
            raise SmokeError("listener STATUS did not report a running parser pool")
        if listener_status.get("protocol_family") != "sbsql":
            raise SmokeError(f"listener STATUS protocol family drifted: {listener_status.get('protocol_family')!r}")

        evidence_path = Path(args.evidence_file)
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(
            json.dumps(
                {
                    "gate": "sbsql_linux_manager_listener_parser_ipc_server_full_path",
                    "platform": sys.platform,
                    "route": [
                        "sb_isql_client",
                        "tcp_network",
                        "sbmn_manager_proxy",
                        "mcp_token_auth_fast_path",
                        "mcp_db_connect_lpreface",
                        "sb_listener_management_lpreface_validate",
                        "lpreface_handoff_claim",
                        "sb_listener_accept",
                        "sbp_sbsql_parser_worker",
                        "sbps_parser_server_ipc",
                        "sb_server",
                        "engine_select_execution",
                    ],
                    "client_result": output,
                    "manager_audit_proxy_admission": True,
                    "manager_audit_auth": True,
                    "manager_audit_db_connect_lpreface": True,
                    "server_audit_auth_handoff_accepted": True,
                    "server_audit_attach_database_accepted": True,
                    "server_audit_sblr_execute_completed": True,
                    "manager_proxy_bytes_total_min": client_bytes,
                    "manager_listener_control_requests_total_min": listener_requests,
                    "listener_status_handoff_complete_total_min": listener_handoff_total,
                    "listener_metric_network_handoff_complete_total_min": listener_network_handoffs,
                    "listener_metric_parser_pool_handoff_complete_total_min": listener_pool_handoffs,
                    "listener_metric_lpreface_claim_consumed_total_min": listener_claims,
                    "listener_status_parser_pool_running": True,
                    "listener_status_protocol_family": "sbsql",
                    "listener_management_socket_discovered": management_socket.name,
                    "temporary_workspace_cleaned_on_success": True,
                    "skipped": False,
                    "simulated": False,
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )

        print(
            "sbsql_linux_manager_full_path_smoke=passed "
            f"evidence={evidence_path} route=client->manager->listener->sbsql->ipc->server"
        )
        success = True
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest should receive the exact failure.
        print(f"sbsql_linux_manager_full_path_smoke=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1
    finally:
        stop_process(manager)
        stop_process(listener)
        stop_process(server)
        if success:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
