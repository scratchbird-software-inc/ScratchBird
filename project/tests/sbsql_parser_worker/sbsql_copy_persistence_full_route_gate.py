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
import os
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

from live_auth_fixture import (
    DEFAULT_PRINCIPAL_UUID,
    local_password_evidence as cli_password_evidence,
    write_local_password_auth_fixture,
)
from sbsql_sbwp_tls_engine_auth_route_smoke import (
    BENCHMARK_VERIFIER,
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
    expect_ready_after_copy,
    generate_server_cert,
    query_payload,
    recv_frame,
    send_frame,
)


USER = "benchmark_user"
DATABASE_NAME = "default"
VERIFIER = BENCHMARK_VERIFIER.decode("ascii")
PERSIST_TABLE_PLAIN = "copy_plain_persist"
ROLLBACK_TABLE_PLAIN = "copy_plain_rollback"
PERSIST_TABLE_TLS = "copy_tls_persist"
ROLLBACK_TABLE_TLS = "copy_tls_rollback"


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


def write_auth_store(database: Path) -> None:
    write_local_password_auth_fixture(
        database,
        USER,
        VERIFIER,
        DEFAULT_PRINCIPAL_UUID,
        "right:CONNECT",
    )


def start_route(
    args: argparse.Namespace,
    root: Path,
    database: Path,
    *,
    tls_required: bool,
    cert: Path | None = None,
    key: Path | None = None,
) -> StartedRoute:
    server_control = root / "sc"
    server_runtime = root / "sr"
    listener_control = root / "lc"
    listener_runtime = root / "lr"
    endpoint = server_control / "s.sock"
    port = find_free_port()
    root.mkdir(parents=True, exist_ok=True)
    write_auth_store(database)

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
        stdout=(root / "server.out").open("wb"),
        stderr=(root / "server.err").open("wb"),
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
        )
        wait_for_tcp(port)
        return StartedRoute(
            name="tls" if tls_required else "plain",
            database=database,
            port=port,
            server=server,
            listener=listener,
            root=root,
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
    evidence = cli_password_evidence(USER, VERIFIER)
    return [
        args.sb_isql,
        str(route.database),
        "--host=127.0.0.1",
        f"--port={route.port}",
        "--sslmode=disable",
        "-U",
        USER,
        "-P",
        evidence,
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
    if actual_text != expected_text:
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
    if len(copy_in_payload) != 5 or copy_in_payload[0] != 0:
        raise RouteError(f"COPY_IN_RESPONSE did not advertise canonical row-field text: {copy_in_payload!r}")
    if struct.unpack_from("<I", copy_in_payload, 1)[0] == 0:
        raise RouteError("COPY_IN_RESPONSE advertised a zero-byte COPY window")
    send_frame(
        sock,
        MSG_COPY_DATA,
        sequence,
        fixture.read_bytes(),
        attachment=attachment,
        txn_id=txn_id,
    )
    sequence += 1
    send_frame(sock, MSG_COPY_DONE, sequence, b"", attachment=attachment, txn_id=txn_id)
    sequence += 1
    ready_payload, frame_txn = expect_ready_after_copy(sock)
    status, ready_txn = decode_ready(ready_payload)
    if status == 0 or ready_txn == 0 or frame_txn == 0:
        raise RouteError("COPY completion did not leave active MGA transaction")
    if ready_txn != txn_id:
        raise RouteError(f"COPY completion changed explicit transaction from {txn_id} to {ready_txn}")
    return sequence, ready_txn


def authenticate_tls(port: int) -> tuple[ssl.SSLSocket, bytes, int, int]:
    sock = connect_tls(port)
    attachment, sequence, txn_id = authenticate(
        sock,
        tls_password_evidence(),
        p1_features=FEATURE_STREAMING | FEATURE_BULK_REJECTS,
    )
    return sock, attachment, sequence, txn_id


def tls_password_evidence() -> bytes:
    return (
        b"scheme=local_password_v1;principal="
        + USER.encode("utf-8")
        + b";principal_uuid="
        + DEFAULT_PRINCIPAL_UUID.encode("ascii")
        + b";storage_authority=mga_security_principal_lifecycle"
        + b";authorization_tags=right:CONNECT"
        + b";verifier="
        + BENCHMARK_VERIFIER
    )


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
            )
            sequence, txn_id = rollback_txn(sock, sequence, attachment, txn_id, "TLS COPY rollback ROLLBACK")
            send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)
        finally:
            sock.close()

        verify_tls_tables(route.port)
        stop_route(route)
        route = None

        route = start_route(args, root / "restart", database, tls_required=True, cert=cert, key=key)
        verify_tls_tables(route.port)
    finally:
        stop_route(route)


def dump_logs(work: Path) -> None:
    for path in sorted(work.rglob("*.out")) + sorted(work.rglob("*.err")) + sorted(work.rglob("*.log")):
        if path.exists() and path.stat().st_size:
            print(f"--- {path.relative_to(work)} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--sb-isql", required=True)
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
