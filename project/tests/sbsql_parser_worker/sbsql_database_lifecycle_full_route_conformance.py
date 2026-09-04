#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-013 client SBWP/TLS to engine lifecycle route conformance."""

from __future__ import annotations

import argparse
import os
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from live_auth_fixture import DEFAULT_PRINCIPAL_UUID
from live_auth_fixture import write_local_password_auth_fixture


MSG_STARTUP = 0x01
MSG_AUTH_RESPONSE = 0x02
MSG_QUERY = 0x03
MSG_TERMINATE = 0x0C

MSG_AUTH_REQUEST = 0x40
MSG_AUTH_OK = 0x41
MSG_READY = 0x43
MSG_ROW_DESCRIPTION = 0x44
MSG_DATA_ROW = 0x45
MSG_COMMAND_COMPLETE = 0x46
MSG_ERROR = 0x48
MSG_PARAMETER_STATUS = 0x4F
MSG_SERVER_INFO = 0x61

SBWP_VERSION_P1 = 0x0101
FEATURE_STREAMING = 1 << 1
FEATURE_BULK_REJECTS = 1 << 17

BENCHMARK_PASSWORD = b"ScratchBird-E2E-2026!"


class RouteError(RuntimeError):
    pass


def generate_server_cert(openssl: str, work: Path) -> None:
    key = work / "server.key"
    cert = work / "server.crt"
    subprocess.check_call(
        [
            openssl,
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-sha256",
            "-days",
            "30",
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost,IP:127.0.0.1",
            "-keyout",
            str(key),
            "-out",
            str(cert),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    key.chmod(0o600)


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(path: Path, timeout: float = 8.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.05)
    return False


def connect_tls(port: int, timeout: float = 8.0) -> ssl.SSLSocket:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            raw = socket.create_connection(("127.0.0.1", port), timeout=1.0)
            ctx = ssl.create_default_context()
            ctx.minimum_version = ssl.TLSVersion.TLSv1_3
            ctx.maximum_version = ssl.TLSVersion.TLSv1_3
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            tls_socket = ctx.wrap_socket(raw, server_hostname="localhost")
            # Bound TCP connection attempts independently from authenticated
            # SBWP work. Password verification, durable authorization, attach,
            # and initial transaction publication are a finite engine route but
            # are not required to complete within the one-second connect bound.
            tls_socket.settimeout(max(timeout, 30.0))
            return tls_socket
        except Exception as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(0.05)
    raise RouteError(f"TLS connect failed: {last_error}")


def recvall(sock: ssl.SSLSocket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise RouteError("connection closed while reading SBWP frame")
        chunks.extend(chunk)
    return bytes(chunks)


def send_frame(
    sock: ssl.SSLSocket,
    msg_type: int,
    sequence: int,
    payload: bytes = b"",
    attachment: bytes = b"\x00" * 16,
    txn_id: int = 0,
) -> None:
    header = bytearray()
    header += b"SBWP"
    header += bytes([1, 1, msg_type, 0])
    header += struct.pack("<I", len(payload))
    header += struct.pack("<I", sequence)
    header += attachment
    header += struct.pack("<Q", txn_id)
    sock.sendall(bytes(header) + payload)


def recv_frame(sock: ssl.SSLSocket) -> tuple[int, bytes, bytes, int]:
    header = recvall(sock, 40)
    if header[0:4] != b"SBWP":
        raise RouteError(f"expected SBWP frame, got {header[:24]!r}")
    length = struct.unpack_from("<I", header, 8)[0]
    attachment = header[16:32]
    txn_id = struct.unpack_from("<Q", header, 32)[0]
    payload = recvall(sock, length) if length else b""
    return header[6], payload, attachment, txn_id


def startup_payload(user: str, database: str) -> bytes:
    params = {"database": database, "user": user, "client_flags": "256"}
    payload = bytearray([1, 1, 0, 0])
    payload += struct.pack("<Q", 0)
    for key, value in params.items():
        payload += key.encode("utf-8") + b"\x00"
        payload += value.encode("utf-8") + b"\x00"
    payload += b"\x00"
    return bytes(payload)


def p1_startup_payload(user: str, database: str, client_features: int) -> bytes:
    def lpstr(value: str) -> bytes:
        encoded = value.encode("utf-8")
        return struct.pack("<I", len(encoded)) + encoded

    def key_value(key: str, value: str) -> bytes:
        encoded = value.encode("utf-8")
        return lpstr(key) + bytes([0x01, 0x00]) + struct.pack("<I", len(encoded)) + encoded

    fields = (("database", database), ("user", user))
    payload = bytearray()
    payload += struct.pack(
        "<HHIQQQ",
        SBWP_VERSION_P1,
        SBWP_VERSION_P1,
        0,
        client_features,
        client_features,
        0,
    )
    payload += b"\x11" * 16
    payload += b"\x00" * 16
    payload += b"\x00" * 16
    payload += struct.pack("<I", len(fields))
    for key, value in fields:
        payload += key_value(key, value)
    payload += struct.pack("<I", 0)
    return bytes(payload)


def decode_ready(payload: bytes) -> tuple[int, int]:
    if len(payload) >= 76:
        transaction_id = struct.unpack_from("<Q", payload, 48)[0]
        state = payload[56]
        return (0 if state == ord("I") else 1), transaction_id
    if len(payload) < 20:
        raise RouteError("READY payload too short")
    return payload[0], struct.unpack_from("<Q", payload, 4)[0]


def query_payload(sql: str) -> bytes:
    return struct.pack("<III", 0, 0, 0) + sql.encode("utf-8") + b"\x00"


def expect_frame(sock: ssl.SSLSocket, expected: int) -> tuple[bytes, bytes, int]:
    msg_type, payload, attachment, txn_id = recv_frame(sock)
    if msg_type != expected:
        raise RouteError(f"expected frame 0x{expected:02x}, got 0x{msg_type:02x}, payload={payload!r}")
    return payload, attachment, txn_id


def authenticate(sock: ssl.SSLSocket, user: str, password: bytes) -> tuple[bytes, int, int]:
    sequence = 0
    send_frame(
        sock,
        MSG_STARTUP,
        sequence,
        p1_startup_payload(
            user, "default", FEATURE_STREAMING | FEATURE_BULK_REJECTS
        ),
    )
    sequence += 1
    auth_payload, _, _ = expect_frame(sock, MSG_AUTH_REQUEST)
    if not auth_payload or auth_payload[0] != 1:
        raise RouteError("expected PASSWORD auth request")
    # Only the credential crosses the public boundary. Principal UUID, roles,
    # grants, and authorization tags are resolved from durable engine state.
    send_frame(sock, MSG_AUTH_RESPONSE, sequence, password)
    auth_ok, attachment, _ = expect_frame(sock, MSG_AUTH_OK)
    if len(auth_ok) < 20:
        raise RouteError("AUTH_OK payload too short")
    attachment = auth_ok[:16] or attachment
    while True:
        msg_type, ready_payload, _, frame_transaction_id = recv_frame(sock)
        if msg_type == MSG_READY:
            break
        if msg_type in (MSG_SERVER_INFO, MSG_PARAMETER_STATUS):
            continue
        raise RouteError(
            f"expected READY after AUTH_OK, got 0x{msg_type:02x}, payload={ready_payload!r}"
        )
    status, transaction_id = decode_ready(ready_payload)
    if status == 0 or transaction_id == 0 or frame_transaction_id == 0:
        raise RouteError("fresh authenticated session did not publish an active MGA transaction")
    return attachment, sequence + 1, transaction_id


def query_success(
    sock: ssl.SSLSocket,
    attachment: bytes,
    transaction_id: int,
    sequence: int,
    sql: str,
) -> int:
    send_frame(
        sock,
        MSG_QUERY,
        sequence,
        query_payload(sql),
        attachment=attachment,
        txn_id=transaction_id,
    )
    saw_row = False
    saw_complete = False
    while True:
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type == MSG_ROW_DESCRIPTION:
            continue
        if msg_type == MSG_DATA_ROW:
            saw_row = True
            continue
        if msg_type == MSG_COMMAND_COMPLETE:
            saw_complete = True
            continue
        if msg_type == MSG_READY:
            if not saw_complete:
                raise RouteError(f"{sql} did not return COMMAND_COMPLETE")
            if not saw_row:
                raise RouteError(f"{sql} did not return management route evidence row")
            return sequence + 1
        if msg_type == MSG_ERROR:
            raise RouteError(f"{sql} failed with ERROR payload {payload!r}")
        raise RouteError(f"{sql} returned unexpected frame 0x{msg_type:02x}")


def query_error(
    sock: ssl.SSLSocket,
    attachment: bytes,
    transaction_id: int,
    sequence: int,
    sql: str,
    expected: bytes,
) -> int:
    send_frame(
        sock,
        MSG_QUERY,
        sequence,
        query_payload(sql),
        attachment=attachment,
        txn_id=transaction_id,
    )
    saw_expected = False
    while True:
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type == MSG_ERROR:
            saw_expected = expected in payload
            continue
        if msg_type == MSG_READY:
            if not saw_expected:
                raise RouteError(f"{sql} did not return expected diagnostic {expected!r}")
            return sequence + 1
        raise RouteError(f"{sql} expected ERROR/READY, got frame 0x{msg_type:02x}")


def query_shutdown(
    sock: ssl.SSLSocket,
    attachment: bytes,
    transaction_id: int,
    sequence: int,
    sql: str,
) -> int:
    send_frame(
        sock,
        MSG_QUERY,
        sequence,
        query_payload(sql),
        attachment=attachment,
        txn_id=transaction_id,
    )
    saw_complete = False
    try:
        while True:
            msg_type, payload, _, _ = recv_frame(sock)
            if msg_type == MSG_COMMAND_COMPLETE:
                saw_complete = True
                continue
            if msg_type == MSG_READY:
                return sequence + 1
            if msg_type == MSG_ERROR:
                raise RouteError(f"{sql} failed with ERROR payload {payload!r}")
    except RouteError as exc:
        if saw_complete and "connection closed" in str(exc):
            return sequence + 1
        raise


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
    for name in ("server.out", "server.err", "listener.out", "listener.err"):
        path = work / name
        if path.exists():
            print(f"--- {name} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)


def seed_database(seeder: str | None, database: Path) -> None:
    if seeder:
        subprocess.check_call([seeder, str(database), "benchmark_user", BENCHMARK_PASSWORD.decode("ascii")])
        return
    write_local_password_auth_fixture(
        database,
        "benchmark_user",
        BENCHMARK_PASSWORD.decode("ascii"),
        principal_uuid=DEFAULT_PRINCIPAL_UUID,
        authorization_tags="right:CONNECT,right:OBS_MANAGEMENT_CONTROL",
    )


def launch_stack(
    args: argparse.Namespace,
    work: Path,
    runtime: Path,
    database: Path,
    port: int,
) -> tuple[subprocess.Popen[bytes], subprocess.Popen[bytes]]:
    server_control = runtime / "c"
    endpoint = server_control / "s"
    server = subprocess.Popen(
        [
            args.server,
            "--foreground",
            "--no-listeners",
            "--control-dir",
            str(server_control),
            "--runtime-dir",
            str(runtime / "r"),
            "--database",
            str(database),
            "--sbps-endpoint",
            str(endpoint),
        ],
        stdout=(work / "server.out").open("wb"),
        stderr=(work / "server.err").open("wb"),
    )
    if not wait_for_path(endpoint):
        stop_process(server)
        raise RouteError("sb_server did not create the SBPS endpoint")

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
            f"--control-dir={runtime / 'lc'}",
            f"--runtime-dir={runtime / 'lr'}",
            "--bind-address=127.0.0.1",
            f"--port={port}",
            "--tls-required=true",
            f"--tls-cert-file={work / 'server.crt'}",
            f"--tls-key-file={work / 'server.key'}",
            "--warm-pool-min=1",
            "--warm-pool-max=2",
            "--dbbt-key-source=test_builtin",
            "--allow-test-dbbt-builtin=true",
        ],
        stdout=(work / "listener.out").open("wb"),
        stderr=(work / "listener.err").open("wb"),
    )
    return server, listener


def run_scenario(args: argparse.Namespace, command: str) -> None:
    with tempfile.TemporaryDirectory(prefix="s2_", dir=args.work_dir) as tmp, tempfile.TemporaryDirectory(
        prefix="s2r_", dir="/tmp"
    ) as runtime_tmp:
        work = Path(tmp)
        runtime = Path(runtime_tmp)
        generate_server_cert(args.openssl, work)
        database = work / "d"
        seed_database(args.example_db_seeder, database)
        port = find_free_port()
        server = None
        listener = None
        try:
            server, listener = launch_stack(args, work, runtime, database, port)
            with connect_tls(port) as sock:
                attachment, sequence, transaction_id = authenticate(
                    sock, "benchmark_user", BENCHMARK_PASSWORD
                )
                sequence = query_success(
                    sock, attachment, transaction_id, sequence, "SHOW SERVER LIFECYCLE"
                )
                sequence = query_success(
                    sock, attachment, transaction_id, sequence, "VERIFY DATABASE"
                )
                if command == "drop_refusal":
                    query_error(
                        sock,
                        attachment,
                        transaction_id,
                        sequence,
                        "DROP DATABASE LOGICAL",
                        b"ENGINE.DBLC_DROP_UNSAFE",
                    )
                else:
                    query_shutdown(sock, attachment, transaction_id, sequence, command)
                    try:
                        send_frame(
                            sock,
                            MSG_TERMINATE,
                            sequence + 1,
                            attachment=attachment,
                            txn_id=transaction_id,
                        )
                    except OSError:
                        pass
        except Exception:
            dump_logs(work)
            raise
        finally:
            stop_process(listener)
            stop_process(server)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--example-db-seeder")
    parser.add_argument("--openssl", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    Path(args.work_dir).mkdir(parents=True, exist_ok=True)
    run_scenario(args, "drop_refusal")
    run_scenario(args, "SHUTDOWN DATABASE")
    run_scenario(args, "SHUTDOWN DATABASE FORCE")
    print("database_lifecycle_full_route_conformance=passed")
    return 0


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"database_lifecycle_full_route_conformance=failed: {exc}", file=sys.stderr)
        raise
