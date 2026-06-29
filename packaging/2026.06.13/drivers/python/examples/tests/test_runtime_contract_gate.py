# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import datetime as dt
import socket
import struct
import threading

import scratchbird
from scratchbird.protocol import (
    FEATURE_COMPRESSION,
    FEATURE_STREAMING,
    HEADER_SIZE,
    MessageHeader,
    MessageType,
    decode_header,
    encode_message,
)
from scratchbird.types import FORMAT_TEXT, OID_BYTEA, OID_INT4, OID_NUMERIC, OID_TEXT, OID_TIMESTAMPTZ


def _ready_payload(txn_id: int) -> bytes:
    payload = bytearray(20)
    payload[0] = 0 if txn_id == 0 else 1
    struct.pack_into("<Q", payload, 4, txn_id)
    struct.pack_into("<Q", payload, 12, 0)
    return bytes(payload)


def _auth_ok_payload() -> bytes:
    return (b"\x22" * 16) + struct.pack("<I", 0)


def _startup_features(payload: bytes) -> int:
    if len(payload) >= 16 and struct.unpack_from("<I", payload, 4)[0] == 0:
        return struct.unpack_from("<Q", payload, 8)[0]
    return struct.unpack_from("<Q", payload, 4)[0]


def _command_complete_payload(rows: int = 0, last_id: int = 0, tag: str = "SELECT") -> bytes:
    return struct.pack("<B3xQQ", 0, rows, last_id) + tag.encode("utf-8") + b"\x00"


def _row_description_payload(columns: list[tuple[str, int]]) -> bytes:
    payload = bytearray()
    payload += struct.pack("<H", len(columns))
    payload += b"\x00\x00"
    for index, (name, type_oid) in enumerate(columns, start=1):
        encoded = name.encode("utf-8")
        payload += struct.pack("<I", len(encoded))
        payload += encoded
        payload += struct.pack("<I", 0)  # table oid
        payload += struct.pack("<H", index)
        payload += struct.pack("<I", type_oid)
        payload += struct.pack("<h", 0)
        payload += struct.pack("<i", 0)
        payload += struct.pack("<B", FORMAT_TEXT)
        payload += struct.pack("<B", 1)
        payload += b"\x00\x00"
    return bytes(payload)


def _data_row_payload(values: list[object]) -> bytes:
    payload = bytearray()
    count = len(values)
    payload += struct.pack("<H", count)
    null_bytes = max(1, (count + 7) // 8)
    payload += struct.pack("<H", null_bytes)
    null_bitmap = bytearray(null_bytes)
    payload += null_bitmap
    for value in values:
        if value is None:
            payload += struct.pack("<i", -1)
            continue
        if isinstance(value, bytes):
            data = value
        else:
            data = str(value).encode("utf-8")
        payload += struct.pack("<i", len(data))
        payload += data
    return bytes(payload)


class _RuntimeGateServer:
    def __init__(self) -> None:
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self.port = self._listener.getsockname()[1]
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._stop_event = threading.Event()
        self._ready_event = threading.Event()
        self._error: Exception | None = None
        self.startup_features = 0
        self.query_flags: list[int] = []

    def start(self) -> None:
        self._thread.start()
        self._ready_event.wait(timeout=3)
        if self._error:
            raise self._error

    def close(self) -> None:
        self._stop_event.set()
        try:
            self._listener.close()
        except OSError:
            pass
        self._thread.join(timeout=3)
        if self._error:
            raise self._error

    def _run(self) -> None:
        try:
            self._ready_event.set()
            conn, _addr = self._listener.accept()
            with conn:
                self._serve_connection(conn)
        except Exception as exc:  # noqa: BLE001
            self._error = exc

    def _serve_connection(self, conn: socket.socket) -> None:
        sequence = 0
        txn_id = 0
        attachment = b"\x22" * 16

        startup_header, startup_payload = self._recv_message(conn)
        if startup_header.msg_type != MessageType.STARTUP:
            raise AssertionError("expected STARTUP message")
        self.startup_features = _startup_features(startup_payload)

        sequence = self._send_message(conn, sequence, attachment, txn_id, MessageType.AUTH_OK, _auth_ok_payload())
        sequence = self._send_message(conn, sequence, attachment, txn_id, MessageType.READY, _ready_payload(txn_id))

        while not self._stop_event.is_set():
            try:
                header, payload = self._recv_message(conn)
            except EOFError:
                return

            if header.msg_type == MessageType.QUERY:
                flags = struct.unpack_from("<I", payload, 0)[0]
                self.query_flags.append(flags)
                sql = payload[12:].split(b"\x00", 1)[0].decode("utf-8", errors="replace").strip()
                sequence = self._handle_query(conn, sequence, attachment, txn_id, sql)
                sequence = self._send_message(conn, sequence, attachment, txn_id, MessageType.READY, _ready_payload(txn_id))
                continue

            if header.msg_type == MessageType.TXN_BEGIN:
                txn_id = 77
                sequence = self._send_message(conn, sequence, attachment, txn_id, MessageType.READY, _ready_payload(txn_id))
                continue
            if header.msg_type in (MessageType.TXN_SAVEPOINT, MessageType.TXN_RELEASE, MessageType.TXN_ROLLBACK_TO):
                sequence = self._send_message(conn, sequence, attachment, txn_id, MessageType.READY, _ready_payload(txn_id))
                continue
            if header.msg_type in (MessageType.TXN_COMMIT, MessageType.TXN_ROLLBACK):
                txn_id = 0
                sequence = self._send_message(conn, sequence, attachment, txn_id, MessageType.READY, _ready_payload(txn_id))
                continue
            if header.msg_type == MessageType.SET_OPTION:
                sequence = self._send_message(conn, sequence, attachment, txn_id, MessageType.READY, _ready_payload(txn_id))
                continue
            if header.msg_type == MessageType.PING:
                sequence = self._send_message(conn, sequence, attachment, txn_id, MessageType.PONG, b"")
                continue
            if header.msg_type == MessageType.TERMINATE:
                return

    def _handle_query(
        self,
        conn: socket.socket,
        sequence: int,
        attachment: bytes,
        txn_id: int,
        sql: str,
    ) -> int:
        if sql == "SELECT 1; SELECT 2":
            sequence = self._send_result_set(
                conn,
                sequence,
                attachment,
                txn_id,
                columns=[("first_value", OID_INT4)],
                rows=[[1]],
                tag="SELECT",
            )
            return self._send_result_set(
                conn,
                sequence,
                attachment,
                txn_id,
                columns=[("second_value", OID_INT4)],
                rows=[[2]],
                tag="SELECT",
            )
        if "FROM sys.tables" in sql:
            return self._send_result_set(
                conn,
                sequence,
                attachment,
                txn_id,
                columns=[
                    ("table_id", OID_INT4),
                    ("schema_id", OID_INT4),
                    ("table_name", OID_TEXT),
                    ("table_type", OID_TEXT),
                    ("owner_id", OID_INT4),
                ],
                rows=[[1, 7, "events", "TABLE", 11]],
                tag="SELECT",
            )
        if sql == "SELECT runtime_type_probe":
            return self._send_result_set(
                conn,
                sequence,
                attachment,
                txn_id,
                columns=[("tsz", OID_TIMESTAMPTZ), ("amount", OID_NUMERIC), ("raw", OID_BYTEA)],
                rows=[["2026-03-01 12:34:56+00", "12.34", "\\x6162"]],
                tag="SELECT",
            )
        return self._send_result_set(
            conn,
            sequence,
            attachment,
            txn_id,
            columns=[("value", OID_INT4)],
            rows=[[1]],
            tag="SELECT",
        )

    def _send_result_set(
        self,
        conn: socket.socket,
        sequence: int,
        attachment: bytes,
        txn_id: int,
        columns: list[tuple[str, int]],
        rows: list[list[object]],
        tag: str,
    ) -> int:
        sequence = self._send_message(
            conn,
            sequence,
            attachment,
            txn_id,
            MessageType.ROW_DESCRIPTION,
            _row_description_payload(columns),
        )
        for row in rows:
            sequence = self._send_message(
                conn,
                sequence,
                attachment,
                txn_id,
                MessageType.DATA_ROW,
                _data_row_payload(row),
            )
        return self._send_message(
            conn,
            sequence,
            attachment,
            txn_id,
            MessageType.COMMAND_COMPLETE,
            _command_complete_payload(rows=len(rows), last_id=0, tag=tag),
        )

    @staticmethod
    def _read_exact(conn: socket.socket, n: int) -> bytes:
        out = bytearray()
        while len(out) < n:
            chunk = conn.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return bytes(out)

    def _recv_message(self, conn: socket.socket):
        header_raw = self._read_exact(conn, HEADER_SIZE)
        header = decode_header(header_raw)
        payload = self._read_exact(conn, header.length) if header.length else b""
        return header, payload

    @staticmethod
    def _send_message(
        conn: socket.socket,
        sequence: int,
        attachment: bytes,
        txn_id: int,
        msg_type: int,
        payload: bytes,
    ) -> int:
        header = MessageHeader(
            msg_type=msg_type,
            flags=0,
            length=len(payload),
            sequence=sequence,
            attachment_id=attachment,
            txn_id=txn_id,
        )
        conn.sendall(encode_message(header, payload))
        return (sequence + 1) & 0xFFFFFFFF


def _runtime_dsn(port: int) -> str:
    return (
        f"scratchbird://alice:secret@127.0.0.1:{port}/runtime_db"
        "?sslmode=disable&protocol=jdbc&binarytransfer=off&compression=zstd"
    )


def test_runtime_gate_txn_exec_without_env():
    server = _RuntimeGateServer()
    server.start()
    conn = scratchbird.connect(_runtime_dsn(server.port))
    try:
        conn.begin()
        conn.savepoint("sp1")
        conn.rollback_to_savepoint("sp1")
        conn.release_savepoint("sp1")
        conn.commit()

        results = conn.query_multi("SELECT 1; SELECT 2")
        assert [set(result.keys()) for result in results] == [
            {"rows", "rowCount", "fields", "command", "lastId"},
            {"rows", "rowCount", "fields", "command", "lastId"},
        ]
        assert results[0]["rows"] == [(1,)]
        assert results[1]["rows"] == [(2,)]
    finally:
        conn.close()
        server.close()

    assert server.startup_features & FEATURE_COMPRESSION
    assert (server.startup_features & FEATURE_STREAMING) == 0
    assert all((flags & 0x04) == 0 for flags in server.query_flags)


def test_runtime_gate_metadata_without_env():
    server = _RuntimeGateServer()
    server.start()
    conn = scratchbird.connect(_runtime_dsn(server.port))
    try:
        tables = conn.tables(table="events")
        assert len(tables) == 1
        assert tables[0][2] == "events"
        assert tables[0][3] == "TABLE"
    finally:
        conn.close()
        server.close()


def test_runtime_gate_type_decode_without_env():
    server = _RuntimeGateServer()
    server.start()
    conn = scratchbird.connect(_runtime_dsn(server.port))
    try:
        cur = conn.execute("SELECT runtime_type_probe")
        row = cur.fetchone()
        assert isinstance(row[0], dt.datetime)
        assert row[0].tzinfo is not None
        assert str(row[1]) == "12.34"
        assert row[2] == b"ab"
    finally:
        conn.close()
        server.close()
