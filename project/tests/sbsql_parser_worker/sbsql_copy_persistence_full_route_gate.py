#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Full-route COPY persistence gate for TLS and non-TLS transports.

This gate uses fixed source-controlled COPY fixtures and validates that COPY
rows become durable only after engine-owned MGA commit. Rollback rows must not
be visible immediately or after restart. The non-TLS lane drives sb_isql, while
the TLS lane drives the SBWP listener/parser/server route directly so TLS is a
required route proof rather than a manifest-only claim.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from sbsql_sbwp_tls_engine_auth_route_smoke import (
    FEATURE_BULK_REJECTS,
    FEATURE_STREAMING,
    MSG_COMMAND_COMPLETE,
    MSG_COPY_DATA,
    MSG_COPY_DONE,
    MSG_COPY_IN_RESPONSE,
    MSG_DATA_ROW,
    MSG_ERROR,
    MSG_QUERY,
    MSG_READY,
    MSG_ROW_DESCRIPTION,
    MSG_TERMINATE,
    MSG_TXN_COMMIT,
    MSG_TXN_ROLLBACK,
    RouteError,
    authenticate,
    connect_tls,
    decode_data_row_values,
    decode_ready,
    expect_frame,
    expect_ready_after_command,
    generate_server_cert,
    query_payload,
    recv_frame,
    send_frame,
)
from cdp_database_lifecycle_support import PUBLIC_TEST_PASSWORD


USER = "benchmark_user"
DATABASE_NAME = "default"
# The approved example seeder accepts this fixed fixture password and writes
# the matching local-password authority record used by both sb_isql and SBWP.
VERIFIER = PUBLIC_TEST_PASSWORD
PERSIST_TABLE_PLAIN = "copy_plain_persist"
ROLLBACK_TABLE_PLAIN = "copy_plain_rollback"
PERSIST_TABLE_TLS = "copy_tls_persist"
ROLLBACK_TABLE_TLS = "copy_tls_rollback"
DISCONNECT_TABLE_TLS = "copy_tls_disconnect"


class CopyPersistenceError(RuntimeError):
    pass


@dataclass
class StartedRoute:
    name: str
    database: Path
    port: int
    server: subprocess.Popen[bytes]
    listener: subprocess.Popen[bytes]
    root: Path
    traces: dict[str, Path]


@dataclass
class IsqlResult:
    case: str
    returncode: int
    stdout: str
    stderr: str


def make_work_dir(preferred_root: Path) -> Path:
    roots = (preferred_root, Path(tempfile.gettempdir()) / "sbcp")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="cp_", dir=root))
        endpoint_probe = candidate / "plain" / "restart" / "sc" / "s.sock"
        listener_probe = candidate / "plain" / "restart" / "lc" / ("sbsql_" + ("0" * 32) + ".management.sock")
        if max(len(str(endpoint_probe)), len(str(listener_probe))) < 100:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise CopyPersistenceError("unable to allocate a short-enough COPY persistence workspace")


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
    raise CopyPersistenceError(f"timed out waiting for {path}")


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
    raise CopyPersistenceError(f"timed out waiting for listener port {port}: {last_error}")


def stop_process(proc: subprocess.Popen[bytes] | None) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=4)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=4)


def start_route(
    args: argparse.Namespace,
    root: Path,
    database: Path,
    *,
    tls_required: bool,
    cert: Path | None = None,
    key: Path | None = None,
    extra_env: dict[str, str] | None = None,
) -> StartedRoute:
    server_control = root / "sc"
    server_runtime = root / "sr"
    listener_control = root / "lc"
    listener_runtime = root / "lr"
    endpoint = server_control / "s.sock"
    port = find_free_port()
    root.mkdir(parents=True, exist_ok=True)
    trace_root = root / "trace"
    trace_root.mkdir(parents=True, exist_ok=True)
    traces = {
        "sbps": trace_root / "sbps_client.tsv",
        "dispatch": trace_root / "sblr_dispatch.tsv",
        "server": trace_root / "server_execute.tsv",
        "worker": trace_root / "sbsql_worker.jsonl",
    }
    route_env = os.environ.copy()
    route_env.update(
        {
            "SCRATCHBIRD_SBPS_CLIENT_PHASE_TRACE_FILE": str(traces["sbps"]),
            "SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE": str(traces["dispatch"]),
            "SCRATCHBIRD_SERVER_EXECUTE_PHASE_TRACE_FILE": str(traces["server"]),
            "SCRATCHBIRD_SBSQL_WORKER_PHASE_TRACE_FILE": str(traces["worker"]),
        }
    )
    if extra_env:
        route_env.update(extra_env)
    if not database.exists():
        if not args.example_db_seeder:
            raise CopyPersistenceError("missing database and no approved example database seeder")
        subprocess.check_call(
            [args.example_db_seeder, str(database), USER, PUBLIC_TEST_PASSWORD],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--control-dir",
            str(server_control),
            "--runtime-dir",
            str(server_runtime),
            "--database",
            str(database),
            "--sbps-endpoint",
            str(endpoint),
        ],
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
        env=route_env,
    )
    try:
        wait_for_path(endpoint)
        listener_args = [
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
            f"--tls-required={'true' if tls_required else 'false'}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
            "--dbbt-key-source=test_builtin",
            "--allow-test-dbbt-builtin=true",
        ]
        if tls_required:
            if cert is None or key is None:
                raise CopyPersistenceError("TLS route requires cert and key")
            listener_args.extend([f"--tls-cert-file={cert}", f"--tls-key-file={key}"])
        listener = subprocess.Popen(
            listener_args,
            stdout=(root / "listener.out").open("wb"),
            stderr=(root / "listener.err").open("wb"),
            env=route_env,
        )
        wait_for_tcp(port)
        return StartedRoute(
            name="tls" if tls_required else "plain",
            database=database,
            port=port,
            server=server,
            listener=listener,
            root=root,
            traces=traces,
        )
    except Exception:
        stop_process(server)
        raise


def stop_route(route: StartedRoute | None) -> None:
    if route is None:
        return
    stop_process(route.listener)
    stop_process(route.server)


def quote_sql_path(path: Path) -> str:
    return str(path).replace("'", "''")


def isql_args(args: argparse.Namespace, route: StartedRoute) -> list[str]:
    return [
        args.sb_isql,
        str(route.database),
        "--host=127.0.0.1",
        f"--port={route.port}",
        "--sslmode=disable",
        "-U",
        USER,
        "-P",
        PUBLIC_TEST_PASSWORD,
    ]


def run_isql(
    args: argparse.Namespace,
    route: StartedRoute,
    case: str,
    sql: str,
    timeout: int = 35,
) -> IsqlResult:
    case_dir = route.root / "isql" / case
    case_dir.mkdir(parents=True, exist_ok=True)
    script = case_dir / "script.sbsql"
    script.write_text(sql, encoding="utf-8")
    out_path = case_dir / "stdout.log"
    err_path = case_dir / "stderr.log"
    completed = subprocess.run(
        isql_args(args, route) + ["-q", "-A", "-t", "-b", "-f", str(script)],
        stdout=out_path.open("wb"),
        stderr=err_path.open("wb"),
        check=False,
        timeout=timeout,
    )
    return IsqlResult(
        case=case,
        returncode=completed.returncode,
        stdout=out_path.read_text(encoding="utf-8", errors="replace").strip(),
        stderr=err_path.read_text(encoding="utf-8", errors="replace").strip(),
    )


def require_isql_success(result: IsqlResult) -> None:
    if result.returncode != 0 or result.stderr:
        raise CopyPersistenceError(
            f"sb_isql {result.case} failed: rc={result.returncode} "
            f"stdout={result.stdout!r} stderr={result.stderr!r}"
        )


def isql_data_lines(result: IsqlResult) -> list[str]:
    require_isql_success(result)
    lines: list[str] = []
    for line in result.stdout.splitlines():
        text = line.strip()
        if not text:
            continue
        if text.startswith("COPY ") or text.startswith("Transaction "):
            continue
        lines.append(text)
    return lines


def assert_export_matches(path: Path, expected: Path) -> None:
    if not path.is_file():
        raise CopyPersistenceError(f"COPY export did not create {path}")
    actual_text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    expected_text = expected.read_text(encoding="utf-8").replace("\r\n", "\n")
    actual_lines = [line for line in actual_text.splitlines() if line]
    expected_lines = [line for line in expected_text.splitlines() if line]
    if (
        len(actual_lines) != len(expected_lines)
        or not actual_lines
        or actual_lines[0] != expected_lines[0]
        or sorted(actual_lines[1:]) != sorted(expected_lines[1:])
    ):
        raise CopyPersistenceError(
            f"COPY export mismatch for {path}: actual={actual_text!r} expected={expected_text!r}"
        )


def run_plain_copy_lane(args: argparse.Namespace, work: Path, fixtures: Path) -> None:
    root = work / "plain"
    database = root / "copy_plain.sbdb"
    route: StartedRoute | None = None
    copy_rows = fixtures / "copy_persist.rows"
    rollback_rows = fixtures / "copy_rollback.rows"
    expected_csv = fixtures / "copy_persist_expected.csv"
    try:
        route = start_route(args, root, database, tls_required=False)
        setup_sql = "\n".join(
            [
                f"CREATE TABLE {PERSIST_TABLE_PLAIN} (id int);",
                f"\\copy {PERSIST_TABLE_PLAIN} FROM '{quote_sql_path(copy_rows)}'",
                "COMMIT;",
                f"CREATE TABLE {ROLLBACK_TABLE_PLAIN} (id int);",
                "COMMIT;",
                f"\\copy {ROLLBACK_TABLE_PLAIN} FROM '{quote_sql_path(rollback_rows)}'",
                "ROLLBACK;",
                "",
            ]
        )
        require_isql_success(run_isql(args, route, "load_commit_and_rollback", setup_sql))
        verify = run_isql(
            args,
            route,
            "verify_immediate",
            "\n".join(
                [
                    f"SELECT COUNT(*) FROM {PERSIST_TABLE_PLAIN};",
                    f"SELECT id FROM {PERSIST_TABLE_PLAIN} ORDER BY id ASC;",
                    f"SELECT COUNT(*) FROM {ROLLBACK_TABLE_PLAIN};",
                    "",
                ]
            ),
        )
        if isql_data_lines(verify) != ["3", "101", "102", "103", "0"]:
            raise CopyPersistenceError(f"plain immediate verification mismatch: {verify.stdout!r}")
        export_path = root / "plain_export.csv"
        require_isql_success(
            run_isql(
                args,
                route,
                "export_immediate",
                f"\\copy {PERSIST_TABLE_PLAIN} TO '{quote_sql_path(export_path)}'\n",
            )
        )
        assert_export_matches(export_path, expected_csv)
        stop_route(route)
        route = None

        route = start_route(args, root / "restart", database, tls_required=False)
        restarted = run_isql(
            args,
            route,
            "verify_after_restart",
            "\n".join(
                [
                    f"SELECT COUNT(*) FROM {PERSIST_TABLE_PLAIN};",
                    f"SELECT id FROM {PERSIST_TABLE_PLAIN} ORDER BY id ASC;",
                    f"SELECT COUNT(*) FROM {ROLLBACK_TABLE_PLAIN};",
                    "",
                ]
            ),
        )
        if isql_data_lines(restarted) != ["3", "101", "102", "103", "0"]:
            raise CopyPersistenceError(f"plain restart verification mismatch: {restarted.stdout!r}")
        restart_export = root / "plain_export_after_restart.csv"
        require_isql_success(
            run_isql(
                args,
                route,
                "export_after_restart",
                f"\\copy {PERSIST_TABLE_PLAIN} TO '{quote_sql_path(restart_export)}'\n",
            )
        )
        assert_export_matches(restart_export, expected_csv)
    finally:
        stop_route(route)


def execute_query(
    sock: ssl.SSLSocket,
    sequence: int,
    attachment: bytes,
    txn_id: int,
    sql: str,
    *,
    require_rows: bool,
) -> tuple[int, list[list[bytes | None]], int]:
    send_frame(sock, MSG_QUERY, sequence, query_payload(sql), attachment=attachment, txn_id=txn_id)
    sequence += 1
    rows: list[list[bytes | None]] = []
    saw_row_description = False
    saw_complete = False
    ready_txn = txn_id
    while True:
        msg_type, payload, _, frame_txn = recv_frame(sock)
        if msg_type == MSG_ROW_DESCRIPTION:
            saw_row_description = True
        elif msg_type == MSG_DATA_ROW:
            rows.append(decode_data_row_values(payload))
        elif msg_type == MSG_COMMAND_COMPLETE:
            saw_complete = True
        elif msg_type == MSG_READY:
            status, ready_txn = decode_ready(payload)
            if status == 0 or ready_txn == 0 or frame_txn == 0:
                raise RouteError(f"{sql} did not leave an active MGA transaction")
            break
        elif msg_type == MSG_ERROR:
            raise RouteError(f"{sql} failed with ERROR payload {payload!r}")
        else:
            raise RouteError(f"{sql} returned unexpected frame 0x{msg_type:02x} payload={payload!r}")
    if not saw_complete:
        raise RouteError(f"{sql} did not emit COMMAND_COMPLETE")
    if require_rows and (not saw_row_description or not rows):
        raise RouteError(f"{sql} did not emit row result evidence")
    return sequence, rows, ready_txn


def expect_ready_after_bulk_import(
    sock: ssl.SSLSocket,
    expected_rows: int,
) -> tuple[bytes, int]:
    """Require the exact command-only opcode-775 completion contract."""

    saw_complete = False
    expected_tag = f"COPY {expected_rows}".encode("ascii")
    while True:
        msg_type, payload, _, txn_id = recv_frame(sock)
        if msg_type in (MSG_ROW_DESCRIPTION, MSG_DATA_ROW):
            raise RouteError(
                "opcode-775 COPY fabricated a SQL rowset instead of returning "
                "its canonical bulk mutation completion"
            )
        if msg_type == MSG_COMMAND_COMPLETE:
            if saw_complete:
                raise RouteError("opcode-775 COPY emitted duplicate COMMAND_COMPLETE frames")
            if len(payload) < 21 or payload[:4] != b"\x01\x00\x00\x00":
                raise RouteError(f"COPY completion carrier header is invalid: {payload!r}")
            affected_rows = struct.unpack_from("<Q", payload, 4)[0]
            reserved = struct.unpack_from("<Q", payload, 12)[0]
            if reserved != 0 or payload[-1] != 0 or b"\x00" in payload[20:-1]:
                raise RouteError(f"COPY completion carrier shape is invalid: {payload!r}")
            tag = payload[20:-1]
            if affected_rows != expected_rows or tag != expected_tag:
                raise RouteError(
                    "COPY completion did not preserve the exact affected-row result: "
                    f"rows={affected_rows} tag={tag!r} expected={expected_tag!r}"
                )
            saw_complete = True
            continue
        if msg_type == MSG_READY:
            if not saw_complete:
                raise RouteError("opcode-775 COPY reached READY without COMMAND_COMPLETE")
            return payload, txn_id
        if msg_type == MSG_ERROR:
            raise RouteError(f"COPY failed with ERROR payload {payload!r}")
        raise RouteError(f"unexpected COPY frame 0x{msg_type:02x} payload={payload!r}")


def commit_txn(sock: ssl.SSLSocket, sequence: int, attachment: bytes, txn_id: int, label: str) -> tuple[int, int]:
    send_frame(sock, MSG_TXN_COMMIT, sequence, b"\x00\x00\x00\x00", attachment=attachment, txn_id=txn_id)
    sequence += 1
    ready_payload, frame_txn = expect_ready_after_command(sock, label)
    status, ready_txn = decode_ready(ready_payload)
    if status == 0 or ready_txn == 0 or frame_txn == 0:
        raise RouteError(f"{label} did not publish active replacement transaction")
    if ready_txn == txn_id:
        raise RouteError(f"{label} did not advance to a replacement transaction")
    return sequence, ready_txn


def rollback_txn(sock: ssl.SSLSocket, sequence: int, attachment: bytes, txn_id: int, label: str) -> tuple[int, int]:
    send_frame(sock, MSG_TXN_ROLLBACK, sequence, attachment=attachment, txn_id=txn_id)
    sequence += 1
    ready_payload, frame_txn = expect_ready_after_command(sock, label)
    status, ready_txn = decode_ready(ready_payload)
    if status == 0 or ready_txn == 0 or frame_txn == 0:
        raise RouteError(f"{label} did not publish active replacement transaction")
    if ready_txn == txn_id:
        raise RouteError(f"{label} did not advance to a replacement transaction")
    return sequence, ready_txn


def copy_from_fixture(
    sock: ssl.SSLSocket,
    sequence: int,
    attachment: bytes,
    txn_id: int,
    table: str,
    fixture: Path,
    expected_rows: int,
    *,
    split_chunks: bool = False,
) -> tuple[int, int]:
    send_frame(
        sock,
        MSG_QUERY,
        sequence,
        query_payload(f"COPY {table} FROM STDIN"),
        attachment=attachment,
        txn_id=txn_id,
    )
    sequence += 1
    copy_in_payload, _, _ = expect_frame(sock, MSG_COPY_IN_RESPONSE)
    if len(copy_in_payload) != 5 or copy_in_payload[0] != 2:
        raise RouteError(
            "COPY_IN_RESPONSE did not advertise opcode-775 "
            f"canonical_csv_default_v1: {copy_in_payload!r}"
        )
    copy_window = struct.unpack_from("<I", copy_in_payload, 1)[0]
    if copy_window == 0 or copy_window > 8_388_608:
        raise RouteError(
            "COPY_IN_RESPONSE advertised an invalid opcode-775 chunk window: "
            f"{copy_window}"
        )
    payload = fixture.read_bytes()
    chunks = [payload]
    if split_chunks:
        split = payload.find(b"\n") + 1
        if split <= 0 or split >= len(payload):
            raise RouteError("multi-chunk COPY fixture has no safe record boundary")
        chunks = [payload[:split], payload[split:]]
    for chunk in chunks:
        if not chunk or len(chunk) > copy_window:
            raise RouteError("COPY fixture chunk violates the advertised window")
        send_frame(
            sock,
            MSG_COPY_DATA,
            sequence,
            chunk,
            attachment=attachment,
            txn_id=txn_id,
        )
        sequence += 1
    send_frame(sock, MSG_COPY_DONE, sequence, b"", attachment=attachment, txn_id=txn_id)
    sequence += 1
    ready_payload, frame_txn = expect_ready_after_bulk_import(sock, expected_rows)
    status, ready_txn = decode_ready(ready_payload)
    if status == 0 or ready_txn == 0 or frame_txn == 0:
        raise RouteError("COPY completion did not leave active MGA transaction")
    if ready_txn != txn_id:
        raise RouteError(f"COPY completion changed explicit transaction from {txn_id} to {ready_txn}")
    return sequence, ready_txn


def copy_data_then_disconnect(
    sock: ssl.SSLSocket,
    sequence: int,
    attachment: bytes,
    txn_id: int,
    table: str,
    payload: bytes,
) -> None:
    """Leave one durably ACKed chunk without seal or terminal execution."""

    send_frame(
        sock,
        MSG_QUERY,
        sequence,
        query_payload(f"COPY {table} FROM STDIN"),
        attachment=attachment,
        txn_id=txn_id,
    )
    copy_in_payload, _, _ = expect_frame(sock, MSG_COPY_IN_RESPONSE)
    if len(copy_in_payload) != 5 or copy_in_payload[0] != 2:
        raise RouteError("disconnect COPY did not negotiate canonical_csv_default_v1")
    copy_window = struct.unpack_from("<I", copy_in_payload, 1)[0]
    if not payload or len(payload) > copy_window or copy_window > 8_388_608:
        raise RouteError("disconnect COPY payload violates the opcode-775 window")
    send_frame(
        sock,
        MSG_COPY_DATA,
        sequence + 1,
        payload,
        attachment=attachment,
        txn_id=txn_id,
    )
    time.sleep(0.2)
    # Closing the authenticated channel before CopyDone is the public-route
    # disconnect boundary. The server owns stream cleanup and transaction
    # rollback; the parser cannot synthesize a seal or opcode-775 execution.
    sock.shutdown(socket.SHUT_RDWR)


def require_bulk_route_traces(route: StartedRoute) -> None:
    for name, path in route.traces.items():
        if not path.is_file() or path.stat().st_size == 0:
            raise CopyPersistenceError(f"missing {name} bulk-route trace {path}")

    sbps_rows: list[tuple[int, int]] = []
    for line in route.traces["sbps"].read_text(
        encoding="utf-8", errors="strict"
    ).splitlines():
        message = re.search(r"(?:^|\t)message_type=(\d+)(?:\t|$)", line)
        schema = re.search(r"(?:^|\t)schema_id=(\d+)(?:\t|$)", line)
        if message and schema:
            sbps_rows.append((int(message.group(1)), int(schema.group(1))))

    bulk_pairs = {
        (706, 7719),  # immutable syntax bind
        (106, 7101),  # BIRQ -> BIRD coordination
        (702, 7715),  # opaque durable chunk append
        (704, 7717),  # durable seal
    }
    observed_bulk = [pair for pair in sbps_rows if pair in bulk_pairs]
    expected_bulk = [
        (706, 7719),
        (106, 7101),
        (702, 7715),
        (702, 7715),
        (704, 7717),
        (706, 7719),
        (106, 7101),
        (702, 7715),
        (704, 7717),
        (706, 7719),
        (106, 7101),
        (702, 7715),
    ]
    if observed_bulk != expected_bulk:
        raise CopyPersistenceError(
            "authenticated COPY did not preserve exact bind/coordinate/"
            f"chunk/seal ordering: {observed_bulk!r}"
        )

    dispatch = route.traces["dispatch"].read_text(
        encoding="utf-8", errors="strict"
    )
    exact_executor = (
        "layer=bulk_import_stream_executor\t"
        "executor_id=engine.op.bulk_import_stream\t"
        "opcode=SBLR_BULK_IMPORT_STREAM\t"
        "opcode_code=775\t"
    )
    if dispatch.count(exact_executor) != 2:
        raise CopyPersistenceError(
            "completed COPY routes did not execute opcode 775 exactly twice"
        )
    for forbidden in (
        "opcode_code=789",
        "opcode_code=793",
        "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
        "SBLR_DML_PLAN_IMPORT_ROWS",
        "bulk.import",
        "dml.plan_import_rows",
    ):
        if forbidden in dispatch:
            raise CopyPersistenceError(
                f"retired COPY execution identity reached dispatch: {forbidden}"
            )

    server_trace = route.traces["server"].read_text(
        encoding="utf-8", errors="strict"
    )
    terminal_publication = (
        "layer=execute_result_publication\t"
        "operation=engine.op.bulk_import_stream\t"
    )
    terminal_completion = (
        "layer=handle_execute_sblr_total\t"
        "operation=engine.op.bulk_import_stream\t"
    )
    if (
        server_trace.count(terminal_publication) != 2
        or server_trace.count(terminal_completion) != 2
    ):
        raise CopyPersistenceError(
            "server execution trace did not observe exactly two terminal "
            "stream publications and completions"
        )

    worker_events = [
        json.loads(line)
        for line in route.traces["worker"].read_text(
            encoding="utf-8", errors="strict"
        ).splitlines()
        if line.strip()
    ]
    append_events = [
        event
        for event in worker_events
        if event.get("event") == "copy_data"
        and event.get("phase") == "durable_opcode775_append"
        and event.get("detail") == "opaque_payload_acknowledged"
    ]
    terminal_events = [
        event
        for event in worker_events
        if event.get("event") == "copy_done"
        and event.get("phase") == "durable_opcode775_terminal"
        and event.get("detail") == "exact_birs_published"
    ]
    if len(append_events) != 4 or len(terminal_events) != 2:
        raise CopyPersistenceError(
            "worker did not preserve four opaque chunk ACKs, two seals/results, "
            f"and one unsealed disconnect: append={len(append_events)} "
            f"terminal={len(terminal_events)}"
        )


def authenticate_tls(port: int) -> tuple[ssl.SSLSocket, bytes, int, int]:
    sock = connect_tls(port)
    attachment, sequence, txn_id = authenticate(
        sock,
        tls_password_evidence(),
        p1_features=FEATURE_STREAMING | FEATURE_BULK_REJECTS,
    )
    return sock, attachment, sequence, txn_id


def tls_password_evidence() -> bytes:
    # Seeded databases authenticate the bootstrap credential through the
    # durable PBKDF2 fingerprint path.  Structured evidence is intentionally
    # rejected there because it would carry a password secret.
    return VERIFIER.encode("ascii")


def byte_rows_to_text(rows: list[list[bytes | None]]) -> list[list[str | None]]:
    rendered: list[list[str | None]] = []
    for row in rows:
        rendered.append([None if value is None else value.decode("utf-8") for value in row])
    return rendered


def assert_tls_rows(rows: list[list[bytes | None]], expected: list[list[str]]) -> None:
    actual = byte_rows_to_text(rows)
    if actual != expected:
        raise CopyPersistenceError(f"TLS row verification mismatch: actual={actual!r} expected={expected!r}")


def verify_tls_tables(port: int) -> None:
    sock, attachment, sequence, txn_id = authenticate_tls(port)
    try:
        sequence, rows, txn_id = execute_query(
            sock,
            sequence,
            attachment,
            txn_id,
            f"SELECT COUNT(*) FROM {PERSIST_TABLE_TLS}",
            require_rows=True,
        )
        assert_tls_rows(rows, [["3"]])
        sequence, rows, txn_id = execute_query(
            sock,
            sequence,
            attachment,
            txn_id,
            f"SELECT id FROM {PERSIST_TABLE_TLS} ORDER BY id ASC",
            require_rows=True,
        )
        assert_tls_rows(rows, [["101"], ["102"], ["103"]])
        sequence, rows, txn_id = execute_query(
            sock,
            sequence,
            attachment,
            txn_id,
            f"SELECT COUNT(*) FROM {ROLLBACK_TABLE_TLS}",
            require_rows=True,
        )
        assert_tls_rows(rows, [["0"]])
        sequence, rows, txn_id = execute_query(
            sock,
            sequence,
            attachment,
            txn_id,
            f"SELECT COUNT(*) FROM {DISCONNECT_TABLE_TLS}",
            require_rows=True,
        )
        assert_tls_rows(rows, [["1"]])
        sequence, rows, txn_id = execute_query(
            sock,
            sequence,
            attachment,
            txn_id,
            f"SELECT id FROM {DISCONNECT_TABLE_TLS} ORDER BY id ASC",
            require_rows=True,
        )
        assert_tls_rows(rows, [["900"]])
        send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)
    finally:
        sock.close()


def run_tls_copy_lane(args: argparse.Namespace, work: Path, fixtures: Path) -> None:
    root = work / "tls"
    database = root / "copy_tls.sbdb"
    root.mkdir(parents=True, exist_ok=True)
    cert, key = generate_server_cert(args.openssl, root)
    route: StartedRoute | None = None
    try:
        route = start_route(args, root, database, tls_required=True, cert=cert, key=key)
        sock, attachment, sequence, txn_id = authenticate_tls(route.port)
        try:
            sequence, _, txn_id = execute_query(
                sock,
                sequence,
                attachment,
                txn_id,
                f"CREATE TABLE {PERSIST_TABLE_TLS} (id int)",
                require_rows=False,
            )
            sequence, txn_id = commit_txn(sock, sequence, attachment, txn_id, "TLS CREATE persist COMMIT")
            sequence, txn_id = copy_from_fixture(
                sock,
                sequence,
                attachment,
                txn_id,
                PERSIST_TABLE_TLS,
                fixtures / "copy_persist.rows",
                3,
                split_chunks=True,
            )
            sequence, txn_id = commit_txn(sock, sequence, attachment, txn_id, "TLS COPY persist COMMIT")
            sequence, _, txn_id = execute_query(
                sock,
                sequence,
                attachment,
                txn_id,
                f"CREATE TABLE {ROLLBACK_TABLE_TLS} (id int)",
                require_rows=False,
            )
            sequence, txn_id = commit_txn(sock, sequence, attachment, txn_id, "TLS CREATE rollback COMMIT")
            sequence, txn_id = copy_from_fixture(
                sock,
                sequence,
                attachment,
                txn_id,
                ROLLBACK_TABLE_TLS,
                fixtures / "copy_rollback.rows",
                2,
            )
            sequence, txn_id = rollback_txn(sock, sequence, attachment, txn_id, "TLS COPY rollback ROLLBACK")
            sequence, _, txn_id = execute_query(
                sock,
                sequence,
                attachment,
                txn_id,
                f"CREATE TABLE {DISCONNECT_TABLE_TLS} (id int)",
                require_rows=False,
            )
            sequence, txn_id = commit_txn(
                sock, sequence, attachment, txn_id,
                "TLS CREATE disconnect COMMIT",
            )
            sequence, _, txn_id = execute_query(
                sock,
                sequence,
                attachment,
                txn_id,
                f"INSERT INTO {DISCONNECT_TABLE_TLS} (id) VALUES (900)",
                require_rows=False,
            )
            sequence, txn_id = commit_txn(
                sock, sequence, attachment, txn_id,
                "TLS seed disconnect oracle COMMIT",
            )
            copy_data_then_disconnect(
                sock,
                sequence,
                attachment,
                txn_id,
                DISCONNECT_TABLE_TLS,
                b"901\n",
            )
        finally:
            sock.close()

        verify_tls_tables(route.port)
        require_bulk_route_traces(route)
        stop_route(route)
        route = None

        route = start_route(args, root / "restart", database, tls_required=True, cert=cert, key=key)
        verify_tls_tables(route.port)
    finally:
        stop_route(route)


def dump_logs(work: Path) -> None:
    for path in (
        sorted(work.rglob("*.out"))
        + sorted(work.rglob("*.err"))
        + sorted(work.rglob("*.log"))
        + sorted(work.rglob("*.tsv"))
        + sorted(work.rglob("*.jsonl"))
    ):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--example-db-seeder", required=True)
    parser.add_argument("--fixture-root", required=True)
    parser.add_argument("--openssl", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args(argv[1:])

    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    work = make_work_dir(Path(args.work_dir))
    try:
        fixtures = Path(args.fixture_root)
        for name in ("copy_persist.rows", "copy_rollback.rows", "copy_persist_expected.csv"):
            if not (fixtures / name).is_file():
                raise CopyPersistenceError(f"missing fixture {fixtures / name}")
        run_plain_copy_lane(args, work, fixtures)
        run_tls_copy_lane(args, work, fixtures)
        print(f"sbsql_copy_persistence_full_route_gate=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001 - CTest needs concrete route evidence.
        print(f"sbsql_copy_persistence_full_route_gate=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
