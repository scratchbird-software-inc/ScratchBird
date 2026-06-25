#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBsql SBWP/TLS route smoke test.

This intentionally drives the network front door rather than the legacy text
test wire:

  SBWP/TLS client -> sb_listener -> pool-allocated sbp_sbsql -> SBPS ->
  sb_server -> engine authentication/authorization/transaction APIs
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
import threading
import time
from pathlib import Path

from live_auth_fixture import DEFAULT_PRINCIPAL_UUID, write_local_password_auth_fixture


MSG_STARTUP = 0x01
MSG_AUTH_RESPONSE = 0x02
MSG_QUERY = 0x03
MSG_TERMINATE = 0x0C
MSG_COPY_DATA = 0x0D
MSG_COPY_DONE = 0x0E
MSG_TXN_BEGIN = 0x15
MSG_TXN_COMMIT = 0x16
MSG_TXN_ROLLBACK = 0x17

MSG_AUTH_REQUEST = 0x40
MSG_AUTH_OK = 0x41
MSG_READY = 0x43
MSG_ROW_DESCRIPTION = 0x44
MSG_DATA_ROW = 0x45
MSG_COMMAND_COMPLETE = 0x46
MSG_ERROR = 0x48
MSG_PARAMETER_STATUS = 0x4F
MSG_COPY_IN_RESPONSE = 0x51
MSG_SERVER_INFO = 0x61

BENCHMARK_VERIFIER = b"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
WRONG_VERIFIER = b"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
SBWP_VERSION_P1 = 0x0101
FEATURE_STREAMING = 1 << 1
FEATURE_BINARY_COPY = 1 << 8
FEATURE_BULK_REJECTS = 1 << 17
FEATURE_UNKNOWN_REQUIRED_PROBE = 1 << 62


class RouteError(RuntimeError):
    pass


def trace_txn(label: str, **values: int) -> None:
    if not os.environ.get("SBSQL_SBWP_TXN_TRACE"):
        return
    rendered = " ".join(f"{key}={value}" for key, value in values.items())
    print(f"[txn-trace] {label} {rendered}", file=sys.stderr)


def generate_server_cert(openssl: str, work: Path) -> tuple[Path, Path]:
    cert = work / "server.crt"
    key = work / "server.key"
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
    return cert, key


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_for_path(path: Path, timeout: float = 6.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return True
        time.sleep(0.05)
    return False


def connect_tls(port: int, timeout: float = 6.0) -> ssl.SSLSocket:
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
            return ctx.wrap_socket(raw, server_hostname="localhost")
        except Exception as exc:  # noqa: BLE001 - report the last transport error.
            last_error = exc
            time.sleep(0.05)
    raise RouteError(f"TLS connect failed: {last_error}")


def run_plaintext_required_refusal(port: int, timeout: float = 6.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0) as raw:
                raw.sendall(b"SBWP\x01\x01\x01\x00")
                raw.settimeout(3.0)
                try:
                    data = raw.recv(1)
                except (ConnectionResetError, BrokenPipeError):
                    return
                if data:
                    raise RouteError(f"plaintext was not refused while TLS is required: {data!r}")
                return
        except (ConnectionRefusedError, TimeoutError, OSError) as exc:
            last_error = exc
            time.sleep(0.05)
    raise RouteError(f"plaintext refusal probe could not connect: {last_error}")


def run_unknown_required_feature_refusal(port: int) -> None:
    with connect_tls(port) as sock:
        send_frame(
            sock,
            MSG_STARTUP,
            0,
            p1_startup_payload("benchmark_user", "default", FEATURE_UNKNOWN_REQUIRED_PROBE),
        )
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type != MSG_ERROR:
            raise RouteError(f"unknown required feature was not refused, got 0x{msg_type:02x}")
        if (
            b"SBWP.FEATURE.REQUIRED_UNSUPPORTED" not in payload
            and b"SBWP.FEATURE.UNKNOWN_REQUIRED" not in payload
        ):
            raise RouteError(f"unknown feature refusal did not carry feature diagnostic: {payload!r}")


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
    params = {
        "database": database,
        "user": user,
        "client_flags": "256",
    }
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
        return (
            lpstr(key)
            + bytes([0x01, 0x00])
            + struct.pack("<I", len(encoded))
            + encoded
        )

    fields = [
        ("database", database),
        ("user", user),
    ]
    payload = bytearray()
    payload += struct.pack("<HHIQQQ", SBWP_VERSION_P1, SBWP_VERSION_P1, 0, client_features, client_features, 0)
    payload += b"\x11" * 16
    payload += b"\x00" * 16
    payload += b"\x00" * 16
    payload += struct.pack("<I", len(fields))
    for key, value in fields:
        payload += key_value(key, value)
    payload += struct.pack("<I", 0)
    return bytes(payload)


def query_payload(sql: str) -> bytes:
    return struct.pack("<III", 0, 0, 0) + sql.encode("utf-8") + b"\x00"


def native_rowset_v2_payload(
    columns: tuple[str, ...],
    column_types: tuple[int, ...],
    rows: tuple[tuple[int | float | bool | bytes | str | None, ...], ...],
) -> bytes:
    if not rows:
        raise RouteError("native rowset proof requires at least one row")
    if len(columns) != len(column_types):
        raise RouteError("native rowset column/type shape mismatch")
    column_count = len(columns)
    null_bitmap_bytes = (column_count + 7) // 8
    payload = bytearray(b"SBNR")
    payload += struct.pack("<HHQI", 2, 0, len(rows), column_count)
    payload += bytes(column_types)
    for column in columns:
        encoded = column.encode("utf-8")
        payload += struct.pack("<I", len(encoded)) + encoded
    for row in rows:
        if len(row) != column_count:
            raise RouteError("native rowset row shape mismatch")
        null_bitmap = bytearray(null_bitmap_bytes)
        values = bytearray()
        for index, value in enumerate(row):
            if value is None:
                null_bitmap[index // 8] |= 1 << (index % 8)
                continue
            if column_types[index] == 2:
                values += struct.pack("<q", int(value))
            elif column_types[index] == 3:
                if isinstance(value, str):
                    values.append(1 if value.strip().lower() in {"true", "1"} else 0)
                else:
                    values.append(1 if bool(value) else 0)
            elif column_types[index] == 4:
                values += struct.pack("<i", int(value))
            elif column_types[index] == 5:
                values += struct.pack("<Q", int(value))
            elif column_types[index] == 6:
                values += struct.pack("<d", float(value))
            elif column_types[index] == 7:
                raw = bytes(value)
                values += struct.pack("<I", len(raw)) + raw
            elif column_types[index] == 1:
                encoded = str(value).encode("utf-8")
                values += struct.pack("<I", len(encoded)) + encoded
            else:
                raise RouteError(f"unsupported native rowset type {column_types[index]}")
        payload += null_bitmap + values
    return bytes(payload)


def decode_ready(payload: bytes) -> tuple[int, int]:
    if len(payload) >= 76:
        txn_id = struct.unpack_from("<Q", payload, 48)[0]
        state = payload[56]
        return (0 if state == ord("I") else 1), txn_id
    if len(payload) < 20:
        raise RouteError("READY payload too short")
    return payload[0], struct.unpack_from("<Q", payload, 4)[0]


def expect_frame(sock: ssl.SSLSocket, expected: int) -> tuple[bytes, bytes, int]:
    msg_type, payload, attachment, txn_id = recv_frame(sock)
    if msg_type != expected:
        raise RouteError(
            f"expected frame 0x{expected:02x}, got 0x{msg_type:02x}, payload={payload!r}"
        )
    return payload, attachment, txn_id


def expect_ready_after_command(sock: ssl.SSLSocket, label: str = "transaction command") -> tuple[bytes, int]:
    saw_complete = False
    while True:
        msg_type, payload, _, txn_id = recv_frame(sock)
        if msg_type == MSG_COMMAND_COMPLETE:
            saw_complete = True
            continue
        if msg_type == MSG_READY:
            if not saw_complete:
                raise RouteError(f"{label} did not emit COMMAND_COMPLETE before READY")
            return payload, txn_id
        if msg_type == MSG_ERROR:
            raise RouteError(f"{label} failed with ERROR payload {payload!r}")
        raise RouteError(f"unexpected {label} frame 0x{msg_type:02x} payload={payload!r}")


def expect_error_then_ready(sock: ssl.SSLSocket, expected_detail: bytes) -> tuple[bytes, int]:
    msg_type, payload, _, _ = recv_frame(sock)
    if msg_type != MSG_ERROR:
        raise RouteError(f"expected ERROR frame, got 0x{msg_type:02x} payload={payload!r}")
    if expected_detail not in payload:
        raise RouteError(f"ERROR frame did not contain {expected_detail!r}: {payload!r}")
    ready_payload, _, txn_id = expect_frame(sock, MSG_READY)
    return ready_payload, txn_id


def expect_ready_after_copy(sock: ssl.SSLSocket) -> tuple[bytes, int]:
    saw_row_description = False
    saw_data_row = False
    saw_complete = False
    while True:
        msg_type, payload, _, txn_id = recv_frame(sock)
        if msg_type == MSG_ROW_DESCRIPTION:
            saw_row_description = True
            continue
        if msg_type == MSG_DATA_ROW:
            saw_data_row = True
            continue
        if msg_type == MSG_COMMAND_COMPLETE:
            if b"COPY" not in payload:
                raise RouteError(f"COPY completion did not carry COPY tag: {payload!r}")
            saw_complete = True
            continue
        if msg_type == MSG_READY:
            if not (saw_row_description and saw_data_row and saw_complete):
                raise RouteError("COPY did not return engine import rows plus command completion")
            return payload, txn_id
        if msg_type == MSG_ERROR:
            raise RouteError(f"COPY failed with ERROR payload {payload!r}")
        raise RouteError(f"unexpected COPY frame 0x{msg_type:02x} payload={payload!r}")


def decode_data_row_values(payload: bytes) -> list[bytes | None]:
    if len(payload) < 4:
        raise RouteError(f"DATA_ROW payload too short: {payload!r}")
    field_count = struct.unpack_from("<H", payload, 0)[0]
    null_bytes = struct.unpack_from("<H", payload, 2)[0]
    offset = 4
    if offset + null_bytes > len(payload):
        raise RouteError(f"DATA_ROW null bitmap exceeded payload: {payload!r}")
    null_bitmap = payload[offset : offset + null_bytes]
    offset += null_bytes
    values: list[bytes | None] = []
    for index in range(field_count):
        is_null = bool(null_bitmap[index // 8] & (1 << (index % 8))) if null_bytes else False
        if is_null:
            values.append(None)
            continue
        if offset + 4 > len(payload):
            raise RouteError(f"DATA_ROW value length exceeded payload: {payload!r}")
        value_size = struct.unpack_from("<i", payload, offset)[0]
        offset += 4
        if value_size < 0 or offset + value_size > len(payload):
            raise RouteError(f"DATA_ROW value exceeded payload: {payload!r}")
        values.append(payload[offset : offset + value_size])
        offset += value_size
    if offset != len(payload):
        raise RouteError(f"DATA_ROW payload had trailing bytes: {payload!r}")
    return values


def row_values_match(values: list[bytes | None], expected: tuple[bytes | None, ...]) -> bool:
    return len(values) == len(expected) and all(actual == wanted for actual, wanted in zip(values, expected))


def execute_row_query(
    sock: ssl.SSLSocket,
    sequence: int,
    attachment: bytes,
    txn_id: int,
    sql: str,
    expected_rows: tuple[tuple[bytes | None, ...], ...] | None = None,
    require_data_row: bool = True,
    require_row_description: bool = True,
) -> int:
    send_frame(sock, MSG_QUERY, sequence, query_payload(sql), attachment=attachment, txn_id=txn_id)
    sequence += 1
    saw_row_description = False
    saw_data_row = False
    saw_complete = False
    decoded_rows: list[list[bytes | None]] = []
    while True:
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type == MSG_ROW_DESCRIPTION:
            saw_row_description = True
        elif msg_type == MSG_DATA_ROW:
            saw_data_row = True
            if expected_rows is not None:
                decoded_rows.append(decode_data_row_values(payload))
        elif msg_type == MSG_COMMAND_COMPLETE:
            saw_complete = True
        elif msg_type == MSG_READY:
            break
        elif msg_type == MSG_ERROR:
            raise RouteError(f"{sql} failed with ERROR payload {payload!r}")
    if not saw_complete:
        raise RouteError(f"{sql} did not traverse SBWP/SBsql/SBPS/server/engine as a row result")
    if require_row_description and not saw_row_description:
        raise RouteError(f"{sql} did not traverse SBWP/SBsql/SBPS/server/engine as a row result")
    if require_data_row and not saw_data_row:
        raise RouteError(f"{sql} did not traverse SBWP/SBsql/SBPS/server/engine as a row result")
    if expected_rows is not None:
        if not any(row_values_match(row, expected) for row in decoded_rows for expected in expected_rows):
            raise RouteError(f"{sql} did not return an expected row: {decoded_rows!r}")
    return sequence


def execute_command(
    sock: ssl.SSLSocket,
    sequence: int,
    attachment: bytes,
    txn_id: int,
    sql: str,
) -> int:
    send_frame(sock, MSG_QUERY, sequence, query_payload(sql), attachment=attachment, txn_id=txn_id)
    sequence += 1
    saw_complete = False
    while True:
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type in (MSG_ROW_DESCRIPTION, MSG_DATA_ROW):
            continue
        if msg_type == MSG_COMMAND_COMPLETE:
            saw_complete = True
            continue
        if msg_type == MSG_READY:
            if not saw_complete:
                raise RouteError(f"{sql} did not complete before READY")
            return sequence
        if msg_type == MSG_ERROR:
            raise RouteError(f"{sql} failed with ERROR payload {payload!r}")
        raise RouteError(f"unexpected command frame 0x{msg_type:02x} payload={payload!r}")


def local_password_evidence(user: str, verifier: bytes, *extra_fields: bytes) -> bytes:
    evidence = (
        b"scheme=local_password_v1;principal="
        + user.encode("utf-8")
        + b";principal_uuid="
        + DEFAULT_PRINCIPAL_UUID.encode("ascii")
        + b";storage_authority=mga_security_principal_lifecycle"
        + b";authorization_tags=right:CONNECT"
        + b";verifier="
        + verifier
    )
    for field in extra_fields:
        evidence += b";" + field
    return evidence


def authenticate(sock: ssl.SSLSocket, evidence: bytes, *, p1_features: int = 0) -> tuple[bytes, int, int]:
    sequence = 0
    payload = (
        p1_startup_payload("benchmark_user", "default", p1_features)
        if p1_features
        else startup_payload("benchmark_user", "default")
    )
    send_frame(sock, MSG_STARTUP, sequence, payload)
    sequence += 1
    auth_payload, _, _ = expect_frame(sock, MSG_AUTH_REQUEST)
    if not auth_payload or auth_payload[0] != 1:
        raise RouteError("expected PASSWORD auth request")
    send_frame(sock, MSG_AUTH_RESPONSE, sequence, evidence)
    auth_ok, attachment, _ = expect_frame(sock, MSG_AUTH_OK)
    if len(auth_ok) < 20:
        raise RouteError("AUTH_OK payload too short")
    attachment = auth_ok[:16] or attachment
    while True:
        msg_type, ready_payload, _, txn_id = recv_frame(sock)
        if msg_type == MSG_READY:
            break
        if msg_type in (MSG_SERVER_INFO, MSG_PARAMETER_STATUS):
            continue
        raise RouteError(f"expected READY after AUTH_OK, got 0x{msg_type:02x}, payload={ready_payload!r}")
    status, ready_txn = decode_ready(ready_payload)
    trace_txn("authenticate", status=status, ready_txn=ready_txn, frame_txn=txn_id)
    if status == 0 or ready_txn == 0 or txn_id == 0:
        raise RouteError("fresh authenticated session did not publish an active MGA transaction")
    return attachment, sequence + 1, ready_txn


def run_positive_route(port: int, copy_fixture_seeded: bool) -> None:
    with connect_tls(port) as sock:
        attachment, sequence, txn_id = authenticate(
            sock,
            local_password_evidence("benchmark_user", BENCHMARK_VERIFIER),
            p1_features=FEATURE_STREAMING | FEATURE_BULK_REJECTS,
        )
        sequence = execute_row_query(
            sock,
            sequence,
            attachment,
            txn_id,
            "SHOW CLUSTER PROVIDER",
            expected_rows=(
                (
                    b"scratchbird.cluster.no_cluster_provider",
                    b"no_cluster",
                    b"1.0.0",
                    b"not_enabled",
                    b"false",
                    b"1",
                    b"sb.cluster_catalog.public_source.v1",
                    b"1",
                    b"1",
                    b"sha256:cd1bce3b9693404108dbb321725402eefdc7b6d98a424b8db5b0c05512c8ab29",
                    b"failed_closed",
                    b"false",
                ),
            ),
        )

        send_frame(sock, MSG_TXN_BEGIN, sequence, attachment=attachment)
        sequence += 1
        ready_payload, frame_txn = expect_ready_after_command(sock, "TXN_BEGIN")
        status, txn_id = decode_ready(ready_payload)
        if status == 0 or txn_id == 0 or frame_txn == 0:
            raise RouteError("engine transaction begin did not return an active MGA transaction")

        sequence = execute_row_query(sock, sequence, attachment, txn_id, "SELECT 1")
        sequence = execute_row_query(
            sock,
            sequence,
            attachment,
            txn_id,
            "SELECT pb.* FROM sys.configuration.policy_bindings AS pb",
            require_data_row=False,
            require_row_description=False,
        )
        for system_table in (
            "sys.catalog.column_descriptor",
            "sys.catalog.index_definitions",
            "sys.catalog.object_comments",
            "sys.catalog.object_dependencies",
            "sys.catalog.object_identity",
            "sys.catalog.object_name_entries",
            "sys.catalog.object_name_vectors",
            "sys.catalog.object_versions",
            "sys.catalog.synonym",
            "sys.constraint_dependency",
            "sys.constraint_descriptor",
            "sys.constraint_subject",
            "sys.constraint_support_structure",
            "sys.key_descriptor",
        ):
            sequence = execute_row_query(
                sock,
                sequence,
                attachment,
                txn_id,
                f"SELECT * FROM {system_table}",
                require_data_row=False,
                require_row_description=False,
            )
        sequence = execute_row_query(sock, sequence, attachment, txn_id, "VALUES (1, 'two'), (3, NULL)")
        if copy_fixture_seeded:
            sequence = execute_row_query(
                sock,
                sequence,
                attachment,
                txn_id,
                "SELECT * FROM users.public.sbsfc021_stream_table",
            )

        send_frame(sock, MSG_TXN_COMMIT, sequence, b"\x00\x00\x00\x00", attachment=attachment, txn_id=txn_id)
        sequence += 1
        ready_payload, frame_txn = expect_ready_after_command(sock, "TXN_COMMIT")
        status, ready_txn = decode_ready(ready_payload)
        if status == 0 or ready_txn == 0 or frame_txn == 0:
            raise RouteError("engine transaction commit did not return an active replacement transaction")
        if ready_txn == txn_id:
            raise RouteError("engine transaction commit did not advance to a replacement transaction")
        txn_id = ready_txn

        if copy_fixture_seeded:
            send_frame(sock, MSG_TXN_BEGIN, sequence, attachment=attachment)
            sequence += 1
            ready_payload, frame_txn = expect_ready_after_command(sock, "TXN_BEGIN before COPY")
            status, txn_id = decode_ready(ready_payload)
            if status == 0 or txn_id == 0 or frame_txn == 0:
                raise RouteError("engine transaction begin before COPY did not return active state")

            copy_sql = "COPY users.public.sbsfc021_stream_table FROM STDIN"
            send_frame(sock, MSG_QUERY, sequence, query_payload(copy_sql), attachment=attachment, txn_id=txn_id)
            sequence += 1
            copy_in_payload, _, _ = expect_frame(sock, MSG_COPY_IN_RESPONSE)
            if len(copy_in_payload) != 5:
                raise RouteError("COPY_IN_RESPONSE payload was malformed")
            if copy_in_payload[0] != 0:
                raise RouteError(f"COPY_IN_RESPONSE did not advertise canonical row-field text profile: {copy_in_payload!r}")
            if struct.unpack_from("<I", copy_in_payload, 1)[0] == 0:
                raise RouteError("COPY_IN_RESPONSE advertised a zero-byte copy window")
            copy_payload = b"id=9;payload=sbwp-copy-valid\nid=10;payload=sbwp-copy-second\n"
            send_frame(sock, MSG_COPY_DATA, sequence, copy_payload, attachment=attachment, txn_id=txn_id)
            sequence += 1
            send_frame(sock, MSG_COPY_DONE, sequence, b"", attachment=attachment, txn_id=txn_id)
            sequence += 1
            ready_payload, frame_txn = expect_ready_after_copy(sock)
            status, ready_txn = decode_ready(ready_payload)
            if status == 0 or ready_txn == 0 or frame_txn == 0:
                raise RouteError("COPY completion did not leave the engine transaction active")
            if ready_txn != txn_id:
                raise RouteError(
                    f"COPY completion changed explicit transaction from {txn_id} to {ready_txn}"
                )
            txn_id = ready_txn

            send_frame(sock, MSG_TXN_COMMIT, sequence, b"\x00\x00\x00\x00", attachment=attachment, txn_id=txn_id)
            sequence += 1
            ready_payload, frame_txn = expect_ready_after_command(sock, "TXN_COMMIT after COPY")
            status, ready_txn = decode_ready(ready_payload)
            if status == 0 or ready_txn == 0 or frame_txn == 0:
                raise RouteError("engine transaction commit after COPY did not return an active replacement transaction")
            if ready_txn == txn_id:
                raise RouteError("engine transaction commit after COPY did not advance to a replacement transaction")
            txn_id = ready_txn

        send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)


def run_binary_copy_positive_route(port: int, copy_fixture_seeded: bool) -> None:
    if not copy_fixture_seeded:
        return
    with connect_tls(port) as sock:
        attachment, sequence, txn_id = authenticate(
            sock,
            local_password_evidence("benchmark_user", BENCHMARK_VERIFIER),
            p1_features=FEATURE_STREAMING | FEATURE_BINARY_COPY | FEATURE_BULK_REJECTS,
        )
        send_frame(sock, MSG_TXN_BEGIN, sequence, attachment=attachment)
        sequence += 1
        ready_payload, frame_txn = expect_ready_after_command(sock, "TXN_BEGIN before binary COPY")
        status, txn_id = decode_ready(ready_payload)
        if status == 0 or txn_id == 0 or frame_txn == 0:
            raise RouteError("engine transaction begin before binary COPY did not return active state")

        copy_sql = "COPY users.public.sbsfc021_stream_table FROM STDIN"
        send_frame(sock, MSG_QUERY, sequence, query_payload(copy_sql), attachment=attachment, txn_id=txn_id)
        sequence += 1
        copy_in_payload, _, _ = expect_frame(sock, MSG_COPY_IN_RESPONSE)
        if len(copy_in_payload) != 5:
            raise RouteError("binary COPY_IN_RESPONSE payload was malformed")
        if copy_in_payload[0] != 1:
            raise RouteError(f"COPY_IN_RESPONSE did not advertise native binary rowset profile: {copy_in_payload!r}")
        if struct.unpack_from("<I", copy_in_payload, 1)[0] == 0:
            raise RouteError("binary COPY_IN_RESPONSE advertised a zero-byte copy window")

        copy_payload = native_rowset_v2_payload(
            ("id", "payload"),
            (2, 1),
            (
                (11, "sbwp-copy-binary-native"),
                (12, "sbwp-copy-binary-second"),
            ),
        )
        send_frame(sock, MSG_COPY_DATA, sequence, copy_payload, attachment=attachment, txn_id=txn_id)
        sequence += 1
        send_frame(sock, MSG_COPY_DONE, sequence, b"", attachment=attachment, txn_id=txn_id)
        sequence += 1
        ready_payload, frame_txn = expect_ready_after_copy(sock)
        status, ready_txn = decode_ready(ready_payload)
        if status == 0 or ready_txn == 0 or frame_txn == 0:
            raise RouteError("binary COPY completion did not leave the engine transaction active")
        if ready_txn != txn_id:
            raise RouteError(
                f"binary COPY completion changed explicit transaction from {txn_id} to {ready_txn}"
            )
        txn_id = ready_txn

        send_frame(sock, MSG_TXN_COMMIT, sequence, b"\x00\x00\x00\x00", attachment=attachment, txn_id=txn_id)
        sequence += 1
        ready_payload, frame_txn = expect_ready_after_command(sock, "TXN_COMMIT after binary COPY")
        status, ready_txn = decode_ready(ready_payload)
        if status == 0 or ready_txn == 0 or frame_txn == 0:
            raise RouteError("engine transaction commit after binary COPY did not return active replacement")

        sequence = execute_row_query(
            sock,
            sequence,
            attachment,
            ready_txn,
            "SELECT * FROM users.public.sbsfc021_stream_table",
            expected_rows=((b"11", b"sbwp-copy-binary-native"),),
        )
        send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)


def run_nested_schema_parent_route(port: int) -> None:
    with connect_tls(port) as sock:
        attachment, sequence, txn_id = authenticate(
            sock,
            local_password_evidence("benchmark_user", BENCHMARK_VERIFIER),
        )
        sequence = execute_command(
            sock,
            sequence,
            attachment,
            txn_id,
            "CREATE SCHEMA users.public.route_nested_regression",
        )
        sequence = execute_command(
            sock,
            sequence,
            attachment,
            txn_id,
            "CREATE TABLE users.public.route_nested_regression.route_nested_table "
            "(id bigint, payload text)",
        )
        sequence = execute_command(
            sock,
            sequence,
            attachment,
            txn_id,
            "INSERT INTO users.public.route_nested_regression.route_nested_table "
            "(id, payload) VALUES (1, 'nested-route'), (2, 'schema-parent')",
        )
        sequence = execute_row_query(
            sock,
            sequence,
            attachment,
            txn_id,
            "SELECT * FROM users.public.route_nested_regression.route_nested_table",
            expected_rows=((b"1", b"nested-route"),),
        )
        send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)


def run_light_authenticated_route(port: int) -> None:
    with connect_tls(port) as sock:
        attachment, sequence, txn_id = authenticate(
            sock,
            local_password_evidence("benchmark_user", BENCHMARK_VERIFIER),
        )
        sequence = execute_row_query(sock, sequence, attachment, txn_id, "SELECT 1")
        send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)


def run_concurrent_tls_routes(port: int, count: int) -> None:
    errors: list[str] = []
    lock = threading.Lock()

    def worker(index: int) -> None:
        try:
            run_light_authenticated_route(port)
        except Exception as exc:  # noqa: BLE001 - aggregate all route evidence.
            with lock:
                errors.append(f"client {index}: {exc}")

    threads = [threading.Thread(target=worker, args=(index,)) for index in range(count)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if errors:
        raise RouteError("concurrent TLS route failed: " + "; ".join(errors))


def run_copy_protocol_negative_routes(port: int, copy_fixture_seeded: bool) -> None:
    with connect_tls(port) as sock:
        attachment, sequence, txn_id = authenticate(
            sock,
            local_password_evidence("benchmark_user", BENCHMARK_VERIFIER),
        )
        send_frame(sock, MSG_COPY_DATA, sequence, b"id=100;payload=not-negotiated\n", attachment=attachment)
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type != MSG_ERROR:
            raise RouteError(f"unnegotiated CopyData did not fail closed, got 0x{msg_type:02x}")
        if b"SBWP.FEATURE.NOT_NEGOTIATED" not in payload:
            raise RouteError(f"unnegotiated CopyData did not carry feature diagnostic: {payload!r}")

    if not copy_fixture_seeded:
        return

    with connect_tls(port) as sock:
        attachment, sequence, txn_id = authenticate(
            sock,
            local_password_evidence("benchmark_user", BENCHMARK_VERIFIER),
            p1_features=FEATURE_STREAMING,
        )
        send_frame(sock, MSG_TXN_BEGIN, sequence, attachment=attachment)
        sequence += 1
        ready_payload, frame_txn = expect_ready_after_command(sock, "negative COPY TXN_BEGIN")
        status, txn_id = decode_ready(ready_payload)
        trace_txn("negative-copy-begin", status=status, ready_txn=txn_id, frame_txn=frame_txn)
        if status == 0 or txn_id == 0 or frame_txn == 0:
            raise RouteError("negative COPY route did not obtain an active transaction")

        send_frame(
            sock,
            MSG_QUERY,
            sequence,
            query_payload("COPY users.public.sbsfc021_stream_table FROM STDIN"),
            attachment=attachment,
            txn_id=txn_id,
        )
        sequence += 1
        expect_frame(sock, MSG_COPY_IN_RESPONSE)
        send_frame(sock, MSG_COPY_DATA, sequence, b"malformed-row-without-equals\n", attachment=attachment, txn_id=txn_id)
        sequence += 1
        ready_payload, frame_txn = expect_error_then_ready(sock, b"SBSQL.COPY.DATA_ROW_INVALID")
        status, ready_txn = decode_ready(ready_payload)
        trace_txn(
            "negative-copy-error-ready",
            status=status,
            ready_txn=ready_txn,
            frame_txn=frame_txn,
            client_txn=txn_id,
        )
        if status == 0 or ready_txn == 0 or frame_txn == 0:
            raise RouteError("malformed COPY recovery did not preserve active transaction state")
        if ready_txn != txn_id:
            raise RouteError(
                f"malformed COPY recovery changed explicit transaction from {txn_id} to {ready_txn}"
            )
        send_frame(sock, MSG_TXN_ROLLBACK, sequence, attachment=attachment, txn_id=txn_id)
        sequence += 1
        ready_payload, frame_txn = expect_ready_after_command(sock, "negative COPY TXN_ROLLBACK")
        status, ready_txn = decode_ready(ready_payload)
        if status == 0 or ready_txn == 0 or frame_txn == 0:
            raise RouteError("rollback recovery did not publish an active replacement transaction")
        send_frame(sock, MSG_TERMINATE, sequence + 1, attachment=attachment)


def run_negative_auth_route(port: int) -> None:
    with connect_tls(port) as sock:
        sequence = 0
        send_frame(sock, MSG_STARTUP, sequence, startup_payload("benchmark_user", "default"))
        sequence += 1
        expect_frame(sock, MSG_AUTH_REQUEST)
        send_frame(sock, MSG_AUTH_RESPONSE, sequence, b"")
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type != MSG_ERROR:
            raise RouteError(f"empty credential evidence was not engine-rejected, got 0x{msg_type:02x}")
        if b"SECURITY.AUTHENTICATION" not in payload and b"credential" not in payload:
            raise RouteError(f"auth rejection did not carry engine security detail: {payload!r}")


def run_invalid_evidence_auth_route(port: int) -> None:
    with connect_tls(port) as sock:
        sequence = 0
        send_frame(sock, MSG_STARTUP, sequence, startup_payload("benchmark_user", "default"))
        sequence += 1
        expect_frame(sock, MSG_AUTH_REQUEST)
        send_frame(sock, MSG_AUTH_RESPONSE, sequence, local_password_evidence("benchmark_user", WRONG_VERIFIER))
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type != MSG_ERROR:
            raise RouteError(f"invalid credential evidence was not engine-rejected, got 0x{msg_type:02x}")
        if b"SECURITY.AUTHENTICATION" not in payload and b"credential" not in payload:
            raise RouteError(f"invalid auth rejection did not carry engine security detail: {payload!r}")


def run_tls_transport_denial_route(port: int, tls_field: bytes, expected: bytes) -> None:
    with connect_tls(port) as sock:
        sequence = 0
        send_frame(sock, MSG_STARTUP, sequence, startup_payload("benchmark_user", "default"))
        sequence += 1
        expect_frame(sock, MSG_AUTH_REQUEST)
        send_frame(
            sock,
            MSG_AUTH_RESPONSE,
            sequence,
            local_password_evidence("benchmark_user", BENCHMARK_VERIFIER, tls_field),
        )
        msg_type, payload, _, _ = recv_frame(sock)
        if msg_type != MSG_ERROR:
            raise RouteError(f"TLS transport denial did not return ERROR, got 0x{msg_type:02x}")
        if expected not in payload:
            raise RouteError(f"TLS transport denial did not carry {expected!r}: {payload!r}")


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


def preserve_failure_database(database: Path) -> None:
    destination = os.environ.get("SBSQL_SBWP_PRESERVE_FAILURE_DATABASE")
    if not destination:
        return
    out = Path(destination)
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(database, out)
    for suffix in (
        ".sb.owner.lock",
        ".sb.route.owner.lock",
        ".sb.txn_publish",
        ".sb.txn_publish.tmp",
    ):
        sidecar = Path(str(database) + suffix)
        if sidecar.exists():
            shutil.copy2(sidecar, Path(str(out) + suffix))
    print(f"preserved_failure_database={out}", file=sys.stderr)


def write_local_password_auth_store(database: Path) -> None:
    write_local_password_auth_fixture(
        database,
        "benchmark_user",
        BENCHMARK_VERIFIER.decode("ascii"),
        DEFAULT_PRINCIPAL_UUID,
    )


def seed_example_database(seeder: str | None, database: Path) -> None:
    if seeder:
        subprocess.check_call(
            [
                seeder,
                str(database),
                "benchmark_user",
                BENCHMARK_VERIFIER.decode("ascii"),
            ]
        )
        return
    write_local_password_auth_store(database)


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
    with tempfile.TemporaryDirectory(prefix="s1_", dir=args.work_dir) as tmp, tempfile.TemporaryDirectory(
        prefix="s1r_", dir="/tmp"
    ) as runtime_tmp:
      work = Path(tmp)
      runtime = Path(runtime_tmp)
      cert, key = generate_server_cert(args.openssl, work)

      server_control = runtime / "c"
      endpoint = server_control / "s"
      database = work / "d"
      seed_example_database(args.example_db_seeder, database)
      port = find_free_port()
      server = None
      listener = None
      try:
          server = subprocess.Popen(
              [
                  args.server,
                  "--foreground",
                  "--no-listeners",
                  "--create-if-missing",
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
                  f"--tls-cert-file={cert}",
                  f"--tls-key-file={key}",
                  "--warm-pool-min=1",
                  "--warm-pool-max=2",
                  "--dbbt-key-source=test_builtin",
                  "--allow-test-dbbt-builtin=true",
              ],
              stdout=(work / "listener.out").open("wb"),
              stderr=(work / "listener.err").open("wb"),
          )

          run_plaintext_required_refusal(port)
          run_unknown_required_feature_refusal(port)
          run_positive_route(port, args.example_db_seeder is not None)
          run_binary_copy_positive_route(port, args.example_db_seeder is not None)
          run_nested_schema_parent_route(port)
          run_concurrent_tls_routes(port, 4)
          run_copy_protocol_negative_routes(port, args.example_db_seeder is not None)
          run_negative_auth_route(port)
          run_invalid_evidence_auth_route(port)
          run_tls_transport_denial_route(
              port,
              b"tls_client_cert_status=wrong_ca",
              b"SECURITY.AUTHENTICATION.TLS_CLIENT_CA_INVALID",
          )
          run_tls_transport_denial_route(
              port,
              b"tls_client_cert_status=expired",
              b"SECURITY.AUTHENTICATION.TLS_CLIENT_CERT_EXPIRED",
          )
          run_tls_transport_denial_route(
              port,
              b"tls_channel_binding_status=mismatch",
              b"SECURITY.AUTHENTICATION.TLS_CHANNEL_BINDING_MISMATCH",
          )
          print(f"sbsql_sbwp_tls_engine_auth_route_smoke=passed work={work}")
          return 0
      except Exception as exc:  # noqa: BLE001 - emit all route evidence.
          print(f"sbsql_sbwp_tls_engine_auth_route_smoke=failed: {exc}", file=sys.stderr)
          preserve_failure_database(database)
          dump_logs(work)
          return 1
      finally:
          stop_process(listener)
          stop_process(server)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    raise SystemExit(main())
