# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import select
import struct

import pytest

from scratchbird import errors
from scratchbird.connection import (
    Connection,
    ResultStream,
    canonical_isolation_label,
    canonical_read_committed_mode_label,
)
from scratchbird.cursor import Cursor
from scratchbird.protocol import (
    ISOLATION_READ_COMMITTED,
    ISOLATION_READ_UNCOMMITTED,
    ISOLATION_REPEATABLE_READ,
    ISOLATION_SERIALIZABLE,
    READ_COMMITTED_MODE_DEFAULT,
    READ_COMMITTED_MODE_READ_CONSISTENCY,
    MessageType,
    MSG_FLAG_URGENT,
    TXN_FLAG_HAS_ACCESS,
    TXN_FLAG_HAS_AUTOCOMMIT,
    TXN_FLAG_HAS_DEFERRABLE,
    TXN_FLAG_HAS_ISOLATION,
    TXN_FLAG_HAS_READ_COMMITTED_MODE,
    TXN_FLAG_HAS_TIMEOUT,
    TXN_FLAG_HAS_WAIT,
    build_cancel_payload,
    build_txn_rollback_payload,
    build_txn_savepoint_payload,
)


def _new_connection(txn_id: int = 0, active: bool = False) -> Connection:
    conn = Connection.__new__(Connection)
    conn._closed = False
    conn._txn_id = txn_id
    conn._runtime_txn_active = active or txn_id != 0
    conn._portal_resume_pending = False
    conn._authed = False
    conn._autocommit = True
    conn._connected = True
    conn._parameters = {}
    conn._skip_schema_apply_once = False
    conn._prefetched_message = None
    conn._last_notice = None
    conn._last_query_progress = None
    conn._batched_insert_statement_cache = False
    conn._prepared_exec_counter = 0
    conn._prepared_exec_cache = {}
    conn._circuit_breaker = None
    conn._telemetry = None
    conn._keepalive_tracker = None
    conn._keepalive = None
    conn._socket = None
    conn._cancel_socket_timeout = None
    conn._session_schema = None
    conn._config = type(
        "Cfg",
        (),
        {
            "schema": None,
            "binary_transfer": True,
            "dormant_id": None,
            "dormant_reattach_token": None,
        },
    )()
    conn._cursors = []
    return conn


class _Header:
    def __init__(self, msg_type: int):
        self.msg_type = msg_type


class _ResultConnection:
    def __init__(self, messages):
        self._messages = list(messages)
        self._txn_id = 0
        self._runtime_txn_active = False
        self._portal_resume_pending = False
        self._prefetched_message = None
        self._last_notice = None
        self._last_query_progress = None
        self.sent_messages = []

    def _recv_message(self):
        if self._prefetched_message is not None:
            message = self._prefetched_message
            self._prefetched_message = None
            return message
        if not self._messages:
            raise AssertionError("unexpected _recv_message call")
        return self._messages.pop(0)

    def _handle_async(self, _header, _payload):
        return False

    def _stash_prefetched_message(self, header, payload):
        self._prefetched_message = (header, payload)

    def _raise_protocol_error(self, payload):
        Connection._raise_protocol_error(self, payload)

    def _raise_protocol_error_and_sync(self, payload):
        while self._messages:
            header, ready_payload = self._recv_message()
            if header.msg_type != MessageType.READY:
                continue
            status, txn_id, _ = struct.unpack("<B3xQQ", ready_payload)
            self._apply_runtime_ready_state(status, txn_id)
            self._portal_resume_pending = False
            break
        Connection._raise_protocol_error(self, payload)

    def _send_message(self, msg_type, payload):
        self.sent_messages.append((msg_type, payload))

    def _allow_portal_resume(self):
        self._portal_resume_pending = True

    def _resume_suspended_portal(self, page_size):
        if not self._portal_resume_pending:
            raise errors.OperationalError("[55000] portal resume requires explicit suspended state")
        self._portal_resume_pending = False
        self._send_message(MessageType.EXECUTE, struct.pack("<I", 0) + struct.pack("<I", page_size))

    def _apply_runtime_ready_state(self, status, txn_id):
        if int(status) != 0:
            self._txn_id = int(txn_id)
            self._runtime_txn_active = True
        else:
            self._txn_id = 0
            self._runtime_txn_active = False


def _row_description_payload(name: str, type_oid: int = 25) -> bytes:
    name_bytes = name.encode("utf-8")
    payload = bytearray()
    payload += struct.pack("<H", 1)
    payload += b"\x00\x00"
    payload += struct.pack("<I", len(name_bytes))
    payload += name_bytes
    payload += struct.pack("<I", 0)  # table oid
    payload += struct.pack("<H", 1)  # column index
    payload += struct.pack("<I", type_oid)
    payload += struct.pack("<h", 0)  # type size
    payload += struct.pack("<i", 0)  # type modifier
    payload += struct.pack("<B", 0)  # format text
    payload += struct.pack("<B", 1)  # nullable
    payload += b"\x00\x00"
    return bytes(payload)


def _notice_payload(**fields: str) -> bytes:
    payload = bytearray()
    for key, value in fields.items():
        payload += key.encode("utf-8")
        payload += value.encode("utf-8")
        payload += b"\x00"
    payload += b"\x00"
    return bytes(payload)


def _data_row_payload(value: str) -> bytes:
    value_bytes = value.encode("utf-8")
    payload = bytearray()
    payload += struct.pack("<H", 1)  # one column
    payload += struct.pack("<H", 1)  # null bitmap bytes
    payload += b"\x00"  # not null
    payload += struct.pack("<i", len(value_bytes))
    payload += value_bytes
    return bytes(payload)


def _error_payload(**fields: str) -> bytes:
    out = bytearray()
    for tag, value in fields.items():
        out += tag.encode("ascii")
        out += value.encode("utf-8")
        out += b"\x00"
    out += b"\x00"
    return bytes(out)


def _txn_status_payload(status: str, txn_id: int) -> bytes:
    payload = bytearray()
    payload.append(ord(status))
    payload += b"\x01\x00\x00"
    payload += struct.pack("<Q", txn_id)
    payload += struct.pack("<Q", 0)
    payload += struct.pack("<Q", 0)
    payload += struct.pack("<I", 0)
    return bytes(payload)


def _parse_sql_from_query_payload(payload: bytes) -> str:
    raw = payload[12:]
    terminator = raw.find(b"\x00")
    if terminator >= 0:
        raw = raw[:terminator]
    return raw.decode("utf-8")


class _FakeStream:
    def __init__(self, rows, rowcount: int, lastrowid, command: str | None = None):
        self._rows = list(rows)
        self.rowcount = rowcount
        self.lastrowid = lastrowid
        self.command = command
        self.completion_count = 0
        self._completed = False
        self.columns = []

    def read_row(self):
        if self._rows:
            return self._rows.pop(0)
        if not self._completed:
            self._completed = True
            self.completion_count += 1
        return None


def test_begin_sends_wire_restart_for_active_session_boundary(monkeypatch):
    conn = _new_connection(txn_id=9)
    calls = []

    monkeypatch.setattr(conn, "_send_message", lambda *args, **kwargs: calls.append(("send", args, kwargs)))
    monkeypatch.setattr(conn, "_drain_until_ready", lambda: calls.append(("drain", (), {})))

    conn.begin()

    assert calls[0][0] == "send"
    assert calls[0][1][0] == MessageType.TXN_BEGIN
    assert calls[1] == ("drain", (), {})


@pytest.mark.parametrize(
    ("isolation_level", "expected"),
    [
        (ISOLATION_READ_UNCOMMITTED, "READ COMMITTED"),
        (ISOLATION_READ_COMMITTED, "READ COMMITTED"),
        (ISOLATION_REPEATABLE_READ, "SNAPSHOT"),
        (ISOLATION_SERIALIZABLE, "SNAPSHOT TABLE STABILITY"),
        (99, "UNKNOWN(99)"),
    ],
)
def test_canonical_isolation_label_documents_public_alias_mapping(isolation_level, expected):
    assert canonical_isolation_label(isolation_level) == expected


@pytest.mark.parametrize(
    ("read_committed_mode", "expected"),
    [
        (READ_COMMITTED_MODE_DEFAULT, "READ COMMITTED"),
        (READ_COMMITTED_MODE_READ_CONSISTENCY, "READ COMMITTED READ CONSISTENCY"),
        (99, "UNKNOWN(99)"),
    ],
)
def test_canonical_read_committed_mode_label_documents_public_selector(read_committed_mode, expected):
    assert canonical_read_committed_mode_label(read_committed_mode) == expected


def test_transaction_active_false_when_no_ready_activity_and_zero_txn_id():
    conn = _new_connection(txn_id=0)
    conn._authed = True
    assert conn._transaction_active() is False


def test_transaction_active_can_follow_ready_state_with_zero_txn_id():
    conn = _new_connection(txn_id=0, active=True)
    conn._authed = True
    assert conn._transaction_active() is True


def test_transaction_active_true_when_server_txn_id_present():
    conn = _new_connection(txn_id=9)
    conn._authed = True
    assert conn._transaction_active() is True


def test_supports_dormant_reattach_is_explicitly_true():
    conn = _new_connection()
    assert conn.supports_dormant_reattach() is True


def test_detach_to_dormant_returns_engine_issued_identifiers(monkeypatch):
    conn = _new_connection(txn_id=9, active=True)
    sent = []

    def _send_message(msg_type, payload, flags=0, force_zero=False):
        sent.append((msg_type, payload, flags, force_zero))

    def _drain_until_ready():
        conn._parameters["dormant_id"] = "00112233-4455-6677-8899-aabbccddeeff"
        conn._parameters["dormant_reattach_token"] = "ffeeddcc-bbaa-9988-7766-554433221100"

    monkeypatch.setattr(conn, "_send_message", _send_message)
    monkeypatch.setattr(conn, "_drain_until_ready", _drain_until_ready)

    dormant_id, reattach_token = conn.detach_to_dormant()

    assert sent == [(MessageType.ATTACH_DETACH, b"", 0, False)]
    assert dormant_id == "00112233-4455-6677-8899-aabbccddeeff"
    assert reattach_token == "ffeeddcc-bbaa-9988-7766-554433221100"


def test_detach_to_dormant_rejects_missing_identifiers(monkeypatch):
    conn = _new_connection(txn_id=9, active=True)

    monkeypatch.setattr(conn, "_send_message", lambda *args, **kwargs: None)
    monkeypatch.setattr(conn, "_drain_until_ready", lambda: None)

    with pytest.raises(errors.OperationalError, match="expected dormant detach identifiers"):
        conn.detach_to_dormant()


def test_reattach_dormant_reconnects_with_explicit_startup_params(monkeypatch):
    conn = _new_connection()
    events = []

    monkeypatch.setattr(
        conn,
        "_disconnect_socket_for_reconnect",
        lambda: events.append(("disconnect", None)),
    )

    def _connect():
        events.append(
            (
                "connect",
                (
                    conn._config.dormant_id,
                    conn._config.dormant_reattach_token,
                    conn._skip_schema_apply_once,
                ),
            )
        )

    monkeypatch.setattr(conn, "_connect", _connect)

    conn.reattach_dormant(
        "00112233-4455-6677-8899-aabbccddeeff",
        "ffeeddcc-bbaa-9988-7766-554433221100",
    )

    assert events == [
        ("disconnect", None),
        (
            "connect",
            (
                "00112233-4455-6677-8899-aabbccddeeff",
                "ffeeddcc-bbaa-9988-7766-554433221100",
                True,
            ),
        ),
    ]
    assert conn._config.dormant_id is None
    assert conn._config.dormant_reattach_token is None
    assert conn._skip_schema_apply_once is False


def test_handle_async_txn_status_marks_active_boundary_with_reported_txn_id():
    conn = _new_connection(txn_id=0)

    handled = conn._handle_async(_Header(MessageType.TXN_STATUS), _txn_status_payload("T", 77))

    assert handled is True
    assert conn._transaction_active() is True
    assert conn._txn_id == 77


def test_handle_async_txn_status_can_clear_active_boundary():
    conn = _new_connection(txn_id=11)

    handled = conn._handle_async(_Header(MessageType.TXN_STATUS), _txn_status_payload("I", 0))

    assert handled is True
    assert conn._transaction_active() is False
    assert conn._txn_id == 0


def test_handle_async_notice_tracks_last_notice_fields():
    conn = _new_connection()

    handled = conn._handle_async(
        _Header(MessageType.NOTICE),
        _notice_payload(S="NOTICE", M="index build running", D="detail"),
    )

    assert handled is True
    assert conn._last_notice == {"S": "NOTICE", "M": "index build running", "D": "detail"}


def test_handle_async_query_progress_tracks_latest_counters():
    conn = _new_connection()

    handled = conn._handle_async(
        _Header(MessageType.QUERY_PROGRESS),
        struct.pack("<QQ", 123, 456),
    )

    assert handled is True
    assert conn._last_query_progress == (123, 456)


def test_cancel_sends_urgent_cancel_message(monkeypatch):
    conn = _new_connection()
    sent = {}

    def _capture(msg_type, payload, flags=0, force_zero=False):
        sent["msg_type"] = msg_type
        sent["payload"] = payload
        sent["flags"] = flags
        sent["force_zero"] = force_zero

    monkeypatch.setattr(conn, "_send_message", _capture)

    conn.cancel()

    assert sent["msg_type"] == MessageType.CANCEL
    assert sent["payload"] == build_cancel_payload(0, 0)
    assert sent["flags"] == MSG_FLAG_URGENT
    assert sent["force_zero"] is False


def test_commit_noop_without_active_transaction(monkeypatch):
    conn = _new_connection(txn_id=0)
    calls = []
    monkeypatch.setattr(conn, "_send_message", lambda *args, **kwargs: calls.append(("send", args, kwargs)))
    monkeypatch.setattr(conn, "_drain_until_ready", lambda: calls.append(("drain", (), {})))
    conn.commit()
    assert calls == []


def test_rollback_noop_without_active_transaction(monkeypatch):
    conn = _new_connection(txn_id=0)
    calls = []
    monkeypatch.setattr(conn, "_send_message", lambda *args, **kwargs: calls.append(("send", args, kwargs)))
    monkeypatch.setattr(conn, "_drain_until_ready", lambda: calls.append(("drain", (), {})))
    conn.rollback()
    assert calls == []


def test_drain_immediate_reopen_boundary_stashes_non_ready_message(monkeypatch):
    conn = _new_connection()
    sock = object()
    conn._socket = sock
    row_description = (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("value"))
    monkeypatch.setattr(select, "select", lambda read, write, exc, timeout: ([sock], [], []))
    monkeypatch.setattr(conn, "_recv_message", lambda: row_description)

    conn._drain_immediate_reopen_boundary()

    assert conn._prefetched_message == row_description


def test_is_valid_returns_false_when_closed(monkeypatch):
    conn = _new_connection()
    conn._closed = True
    monkeypatch.setattr(conn, "ping", lambda: pytest.fail("ping should not be called"))
    assert conn.is_valid() is False


def test_is_valid_returns_true_on_successful_ping(monkeypatch):
    conn = _new_connection()
    monkeypatch.setattr(conn, "ping", lambda: None)
    assert conn.is_valid() is True


def test_is_valid_returns_false_on_ping_error(monkeypatch):
    conn = _new_connection()

    def _fail_ping():
        raise errors.OperationalError("ping failed")

    monkeypatch.setattr(conn, "ping", _fail_ping)
    assert conn.is_valid() is False


def test_is_valid_rejects_negative_timeout():
    conn = _new_connection()
    with pytest.raises(errors.ProgrammingError, match="timeout_ms must be >= 0"):
        conn.is_valid(-1)


def test_is_valid_applies_and_restores_socket_timeout(monkeypatch):
    conn = _new_connection()
    observed = {}

    class _Socket:
        def __init__(self):
            self.timeout = None

        def gettimeout(self):
            return self.timeout

        def settimeout(self, value):
            self.timeout = value

    conn._socket = _Socket()

    def _ping():
        observed["during_ping_timeout"] = conn._socket.gettimeout()

    monkeypatch.setattr(conn, "ping", _ping)

    assert conn.is_valid(250) is True
    assert observed["during_ping_timeout"] == 0.25
    assert conn._socket.gettimeout() is None


def test_is_valid_restores_timeout_when_ping_fails(monkeypatch):
    conn = _new_connection()

    class _Socket:
        def __init__(self):
            self.timeout = 1.5

        def gettimeout(self):
            return self.timeout

        def settimeout(self, value):
            self.timeout = value

    conn._socket = _Socket()

    def _fail_ping():
        raise errors.OperationalError("down")

    monkeypatch.setattr(conn, "ping", _fail_ping)

    assert conn.is_valid(500) is False
    assert conn._socket.gettimeout() == 1.5


def test_autocommit_true_commits_active_transaction_before_switch(monkeypatch):
    conn = _new_connection(txn_id=13)
    conn._autocommit = False
    calls = []

    def _fake_commit():
        calls.append(("commit",))
        conn._txn_id = 0
        conn._runtime_txn_active = False

    monkeypatch.setattr(conn, "commit", _fake_commit)

    conn.autocommit = True

    assert calls == [("commit",)]
    assert conn.autocommit is True


def test_autocommit_true_skips_commit_without_active_transaction(monkeypatch):
    conn = _new_connection(txn_id=0)
    conn._autocommit = False
    monkeypatch.setattr(conn, "commit", lambda: pytest.fail("commit should not be called"))

    conn.autocommit = True

    assert conn.autocommit is True


def test_autocommit_setter_noops_when_value_unchanged(monkeypatch):
    conn = _new_connection(txn_id=17)
    conn._autocommit = True
    monkeypatch.setattr(conn, "commit", lambda: pytest.fail("commit should not be called"))

    conn.autocommit = True

    assert conn.autocommit is True


def test_autocommit_false_does_not_inject_wire_option_or_begin(monkeypatch):
    conn = _new_connection(txn_id=0)
    conn._autocommit = True
    monkeypatch.setattr(conn, "set_option", lambda *_args, **_kwargs: pytest.fail("set_option should not be called"))
    monkeypatch.setattr(conn, "begin", lambda **_kwargs: pytest.fail("begin should not be called"))

    conn.autocommit = False

    assert conn.autocommit is False


def test_autocommit_false_leaves_active_session_boundary_untouched(monkeypatch):
    conn = _new_connection(txn_id=0, active=True)
    conn._autocommit = True
    monkeypatch.setattr(conn, "set_option", lambda *_args, **_kwargs: pytest.fail("set_option should not be called"))
    monkeypatch.setattr(conn, "begin", lambda **_kwargs: pytest.fail("begin should not be called"))

    conn.autocommit = False

    assert conn.autocommit is False


def test_set_session_schema_executes_schema_statement(monkeypatch):
    conn = _new_connection()
    sent = {}
    monkeypatch.setattr(conn, "_execute_command", lambda sql: sent.setdefault("sql", sql))

    conn.set_session_schema(" analytics ")

    assert conn.get_session_schema() == "analytics"
    assert conn._config.schema == "analytics"
    assert sent["sql"] == 'SET SCHEMA "analytics"'


def test_set_session_schema_public_alias_maps_to_users_public(monkeypatch):
    conn = _new_connection()
    sent = {}
    monkeypatch.setattr(conn, "_execute_command", lambda sql: sent.setdefault("sql", sql))

    conn.set_session_schema(" public ")

    assert conn.get_session_schema() == "users.public"
    assert conn._config.schema == "users.public"
    assert sent["sql"] == 'SET SCHEMA "users.public"'


def test_send_simple_query_respects_binary_transfer_toggle():
    conn = _new_connection()
    sent = []
    conn._send_message = lambda msg_type, payload, flags=0, force_zero=False: sent.append((msg_type, payload, flags, force_zero))

    conn._config.binary_transfer = False
    conn._send_simple_query("SELECT 1")
    flags_no_binary = struct.unpack_from("<I", sent[-1][1], 0)[0]
    assert (flags_no_binary & 0x04) == 0

    conn._config.binary_transfer = True
    conn._send_simple_query("SELECT 1")
    flags_binary = struct.unpack_from("<I", sent[-1][1], 0)[0]
    assert (flags_binary & 0x04) == 0x04


def test_send_extended_query_uses_text_result_format_when_binary_transfer_disabled():
    conn = _new_connection()
    sent = []
    conn._send_message = lambda msg_type, payload, flags=0, force_zero=False: sent.append((msg_type, payload, flags, force_zero))
    conn._describe_statement = lambda _name: 1

    conn._config.binary_transfer = False
    conn._send_extended_query("SELECT $1", [1], max_rows=0)
    bind_payload = next(payload for msg_type, payload, _flags, _force_zero in sent if msg_type == MessageType.BIND)
    assert bind_payload.endswith(b"\x00\x00")

    sent.clear()
    conn._config.binary_transfer = True
    conn._send_extended_query("SELECT $1", [1], max_rows=0)
    bind_payload = next(payload for msg_type, payload, _flags, _force_zero in sent if msg_type == MessageType.BIND)
    assert bind_payload.endswith(b"\x01\x00\x01\x00")


def test_send_cached_extended_query_reuses_prepared_statement_for_identical_sql_shape():
    conn = _new_connection()
    sent = []
    describe_calls = []
    conn._send_message = lambda msg_type, payload, flags=0, force_zero=False: sent.append((msg_type, payload, flags, force_zero))
    conn._describe_statement = lambda name: describe_calls.append(name) or 2

    conn._send_cached_extended_query("INSERT INTO t VALUES ($1, $2)", [1, 2], max_rows=0)
    conn._send_cached_extended_query("INSERT INTO t VALUES ($1, $2)", [3, 4], max_rows=0)

    parse_messages = [msg for msg in sent if msg[0] == MessageType.PARSE]
    bind_messages = [msg for msg in sent if msg[0] == MessageType.BIND]
    execute_messages = [msg for msg in sent if msg[0] == MessageType.EXECUTE]

    assert len(parse_messages) == 1
    assert len(bind_messages) == 2
    assert len(execute_messages) == 2
    assert describe_calls == ["__py_exec_1"]


def test_set_session_schema_none_resets_to_users_public(monkeypatch):
    conn = _new_connection()
    conn._session_schema = "analytics"
    conn._config.schema = "analytics"
    sent = {}
    monkeypatch.setattr(conn, "_execute_command", lambda sql: sent.setdefault("sql", sql))

    conn.set_session_schema(None)

    assert conn.get_session_schema() is None
    assert conn._config.schema is None
    assert sent["sql"] == 'SET SCHEMA "users.public"'


def test_set_session_schema_noops_when_unchanged(monkeypatch):
    conn = _new_connection()
    conn._session_schema = "analytics"
    conn._config.schema = "analytics"
    monkeypatch.setattr(conn, "_execute_command", lambda _sql: pytest.fail("execute should not be called"))

    conn.set_session_schema(" analytics ")

    assert conn.get_session_schema() == "analytics"


def test_set_session_schema_rejects_non_string_values():
    conn = _new_connection()
    with pytest.raises(errors.ProgrammingError, match="schema must be a string or None"):
        conn.set_session_schema(42)


def test_savepoint_requires_active_transaction():
    conn = _new_connection(txn_id=0)
    with pytest.raises(errors.ProgrammingError, match="active transaction"):
        conn.savepoint("sp1")


def test_savepoint_requires_non_empty_name():
    conn = _new_connection(txn_id=7)
    with pytest.raises(errors.ProgrammingError, match="savepoint name is required"):
        conn.savepoint("   ")


def test_savepoint_normalizes_name_and_sends_payload(monkeypatch):
    conn = _new_connection(txn_id=7)
    sent = {}
    monkeypatch.setattr(
        conn,
        "_send_message",
        lambda msg_type, payload: sent.update({"msg_type": msg_type, "payload": payload}),
    )
    monkeypatch.setattr(conn, "_drain_until_ready", lambda: sent.update({"drained": True}))
    conn.savepoint("  sp1  ")
    assert sent["msg_type"] == MessageType.TXN_SAVEPOINT
    assert sent["payload"] == build_txn_savepoint_payload("sp1")
    assert sent["drained"] is True


def test_begin_sets_expected_flags(monkeypatch):
    conn = _new_connection(txn_id=0)
    sent = {}
    monkeypatch.setattr(
        conn,
        "_send_message",
        lambda msg_type, payload: sent.update({"msg_type": msg_type, "payload": payload}),
    )
    monkeypatch.setattr(conn, "_drain_until_ready", lambda: sent.update({"drained": True}))

    conn.begin(
        isolation_level=2,
        access_mode=1,
        deferrable=True,
        wait=False,
        timeout_ms=75,
        autocommit_mode=1,
    )

    assert sent["msg_type"] == MessageType.TXN_BEGIN
    flags = struct.unpack_from("<H", sent["payload"], 0)[0]
    assert flags & TXN_FLAG_HAS_ISOLATION
    assert flags & TXN_FLAG_HAS_ACCESS
    assert flags & TXN_FLAG_HAS_DEFERRABLE
    assert flags & TXN_FLAG_HAS_WAIT
    assert flags & TXN_FLAG_HAS_TIMEOUT
    assert flags & TXN_FLAG_HAS_AUTOCOMMIT
    assert sent["drained"] is True


def test_begin_encodes_read_committed_mode_and_expands_payload(monkeypatch):
    conn = _new_connection(txn_id=0)
    sent = {}
    monkeypatch.setattr(
        conn,
        "_send_message",
        lambda msg_type, payload: sent.update({"msg_type": msg_type, "payload": payload}),
    )
    monkeypatch.setattr(conn, "_drain_until_ready", lambda: sent.update({"drained": True}))

    conn.begin(read_committed_mode=READ_COMMITTED_MODE_READ_CONSISTENCY, timeout_ms=25)

    assert sent["msg_type"] == MessageType.TXN_BEGIN
    assert len(sent["payload"]) == 16
    flags = struct.unpack_from("<H", sent["payload"], 0)[0]
    assert flags & TXN_FLAG_HAS_ISOLATION
    assert flags & TXN_FLAG_HAS_READ_COMMITTED_MODE
    assert flags & TXN_FLAG_HAS_TIMEOUT
    assert sent["payload"][4] == ISOLATION_READ_COMMITTED
    assert struct.unpack_from("<I", sent["payload"], 8)[0] == 25
    assert sent["payload"][12] == READ_COMMITTED_MODE_READ_CONSISTENCY
    assert sent["drained"] is True


def test_begin_rejects_read_committed_mode_with_snapshot_alias():
    conn = _new_connection(txn_id=0)
    with pytest.raises(errors.NotSupportedError, match="READ COMMITTED isolation alias"):
        conn.begin(
            isolation_level=ISOLATION_REPEATABLE_READ,
            read_committed_mode=READ_COMMITTED_MODE_READ_CONSISTENCY,
        )


def test_prepared_transaction_helpers_emit_canonical_control_sql(monkeypatch):
    conn = _new_connection()
    sql = []
    monkeypatch.setattr(conn, "_execute_command", lambda statement: sql.append(statement))

    conn.prepare_transaction("gid-1")
    conn.commit_prepared("gid-1")
    conn.rollback_prepared("gid'2")

    assert sql == [
        "PREPARE TRANSACTION 'gid-1'",
        "COMMIT PREPARED 'gid-1'",
        "ROLLBACK PREPARED 'gid''2'",
    ]


def test_prepared_transaction_helpers_reject_empty_gid():
    conn = _new_connection()
    with pytest.raises(errors.ProgrammingError, match="42601"):
        conn.prepare_transaction("   ")


def test_dormant_helpers_require_engine_issued_token_and_explicit_uuid_inputs():
    conn = _new_connection()

    assert conn.supports_prepared_transactions() is True
    assert conn.supports_dormant_reattach() is True

    with pytest.raises(errors.InterfaceError, match="no active socket"):
        conn.detach_to_dormant()
    with pytest.raises(errors.ProgrammingError, match="42601"):
        conn.reattach_dormant("not-a-uuid", "token-1")
    with pytest.raises(errors.ProgrammingError, match="42601"):
        conn.reattach_dormant("00112233-4455-6677-8899-aabbccddeeff", None)


def test_resume_suspended_portal_requires_explicit_pending_state():
    conn = _new_connection()

    with pytest.raises(errors.OperationalError, match="55000"):
        conn._resume_suspended_portal(2)


def test_result_stream_resumes_only_after_explicit_suspended_state():
    ready_payload = struct.pack("<B3xQQ", 1, 77, 0)
    conn = _ResultConnection(
        [
            (_Header(MessageType.PORTAL_SUSPENDED), b""),
            (_Header(MessageType.DATA_ROW), _data_row_payload("1")),
            (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 0) + b"SELECT 1\x00"),
            (_Header(MessageType.READY), ready_payload),
        ]
    )
    stream = ResultStream(conn, page_size=2)

    assert stream.read_row() == (1,)
    assert conn.sent_messages[0][0] == MessageType.EXECUTE
    assert struct.unpack_from("<I", conn.sent_messages[0][1], 4)[0] == 2
    assert stream.read_row() is None
    assert conn._txn_id == 77


def test_result_stream_ignores_stray_reopen_ready_before_next_query_frame():
    reopen_ready_payload = struct.pack("<B3xQQ", 1, 77, 0)
    final_ready_payload = struct.pack("<B3xQQ", 1, 77, 0)
    conn = _ResultConnection(
        [
            (_Header(MessageType.TXN_STATUS), _txn_status_payload("T", 77)),
            (_Header(MessageType.READY), reopen_ready_payload),
            (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("value")),
            (_Header(MessageType.DATA_ROW), _data_row_payload("2")),
            (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 0) + b"SELECT 1\x00"),
            (_Header(MessageType.READY), final_ready_payload),
        ]
    )
    stream = ResultStream(conn, page_size=0)

    assert stream.read_row() == ("2",)
    assert stream.read_row() is None
    assert conn._runtime_txn_active is True
    assert conn._txn_id == 77


def test_native_sql_rewrites_parameters():
    conn = _new_connection()
    assert conn.native_sql("SELECT ?::INTEGER", [42]) == "SELECT $1::INTEGER"
    assert conn.native_sql("SELECT :v::INTEGER", {"v": 7}) == "SELECT $1::INTEGER"


def test_native_sql_maps_normalization_errors():
    conn = _new_connection()
    with pytest.raises(errors.ProgrammingError, match="not enough parameters"):
        conn.native_sql("SELECT ?::INTEGER", [])


def test_native_callable_sql_rewrites_escape_calls():
    conn = _new_connection()
    assert conn.native_callable_sql("{call demo.proc(?, ?)}", [1, 2]) == "call demo.proc($1, $2)"
    assert conn.native_callable_sql("{? = call demo.fn(:id)}", {"id": 7}) == "select demo.fn($1) as return_value"


def test_native_callable_sql_maps_callable_syntax_errors():
    conn = _new_connection()
    with pytest.raises(errors.ProgrammingError, match="invalid JDBC escape call syntax"):
        conn.native_callable_sql("{call ()}")


def test_copy_in_binary_normalizes_text_stream_to_fixed_shape_native_rowset(monkeypatch):
    conn = _new_connection(txn_id=77, active=True)
    messages = [
        (_Header(MessageType.COPY_IN_RESPONSE), bytes([1]) + struct.pack("<I", 1024 * 1024)),
        (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 2, 0) + b"COPY 2\x00"),
        (_Header(MessageType.READY), struct.pack("<B3xQQ", 1, 77, 0)),
    ]
    sent = []

    monkeypatch.setattr(conn, "_send_simple_query", lambda sql: sent.append(("query", sql)))
    monkeypatch.setattr(conn, "_send_message", lambda msg_type, payload: sent.append((msg_type, payload)))
    monkeypatch.setattr(conn, "_recv_message", lambda: messages.pop(0))

    rows = conn.copy_in(
        "COPY users.public.driver_copy FROM STDIN",
        b"id,payload,active,amount\n1,alpha,true,4.5\n2,beta,false,5.75\n",
    )

    assert rows == 2
    copy_payloads = [payload for msg_type, payload in sent if msg_type == MessageType.COPY_DATA]
    assert len(copy_payloads) == 1
    payload = copy_payloads[0]
    assert payload[:4] == b"SBNR"
    assert struct.unpack_from("<H", payload, 4)[0] == 2
    assert struct.unpack_from("<Q", payload, 8)[0] == 2
    assert struct.unpack_from("<I", payload, 16)[0] == 4
    assert payload[20:24] == bytes([4, 1, 3, 6])
    assert b"id=" not in payload
    assert b"payload=" not in payload


def test_copy_in_binary_refuses_mid_stream_shape_change(monkeypatch):
    conn = _new_connection(txn_id=77, active=True)
    messages = [
        (_Header(MessageType.COPY_IN_RESPONSE), bytes([1]) + struct.pack("<I", 1024 * 1024)),
    ]
    sent = []

    monkeypatch.setattr(conn, "_send_simple_query", lambda sql: sent.append(("query", sql)))
    monkeypatch.setattr(conn, "_send_message", lambda msg_type, payload: sent.append((msg_type, payload)))
    monkeypatch.setattr(conn, "_recv_message", lambda: messages.pop(0))

    with pytest.raises(errors.ProgrammingError, match="row shape mismatch|changed row shape"):
        conn.copy_in(
            "COPY users.public.driver_copy FROM STDIN",
            b"id,payload\n1,alpha\n2,beta,extra\n",
        )
    assert not any(msg_type == MessageType.COPY_DATA for msg_type, _ in sent)


def test_connection_call_executes_normalized_callable_sql(monkeypatch):
    conn = _new_connection()
    calls = []
    stream = _FakeStream(rows=[], rowcount=0, lastrowid=None)

    def _fake_execute_query(sql, params, max_rows=0):
        calls.append((sql, list(params), max_rows))
        return stream

    monkeypatch.setattr(conn, "_execute_query", _fake_execute_query)

    cur = conn.call("{call demo.proc(?, ?)}", [11, 22])

    assert isinstance(cur, Cursor)
    assert calls == [("call demo.proc($1, $2)", [11, 22], 0)]


def test_execute_query_selects_simple_or_extended_path(monkeypatch):
    conn = _new_connection()
    calls = []
    monkeypatch.setattr(conn, "_begin_operation", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(conn, "_end_operation", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(conn, "_send_simple_query", lambda sql, max_rows=0: calls.append(("simple", sql, max_rows)))
    monkeypatch.setattr(
        conn,
        "_send_extended_query",
        lambda sql, params, max_rows=0: calls.append(("extended", sql, list(params), max_rows)),
    )

    conn._execute_query("SELECT 1")
    conn._execute_query("SELECT ?::INTEGER", [5], max_rows=10)

    assert calls == [
        ("simple", "SELECT 1", 0),
        ("extended", "SELECT $1::INTEGER", [5], 10),
    ]


def test_execute_query_maps_normalization_errors():
    conn = _new_connection()
    with pytest.raises(errors.ProgrammingError, match="not enough parameters"):
        conn._execute_query("SELECT ?::INTEGER", [])


def test_cursor_executemany_requires_seq_of_params():
    cursor = Cursor(object())
    with pytest.raises(errors.ProgrammingError, match="seq_of_params is required"):
        cursor.executemany("SELECT 1", None)


def test_result_stream_propagates_command_complete_last_id():
    command_complete_payload = struct.pack("<B3xQQ", 1, 3, 91) + b"INSERT 0 3\x00"
    ready_payload = struct.pack("<B3xQQ", 1, 77, 0)
    conn = _ResultConnection(
        [
            (_Header(MessageType.COMMAND_COMPLETE), command_complete_payload),
            (_Header(MessageType.READY), ready_payload),
        ]
    )
    stream = ResultStream(conn, page_size=0)

    assert stream.read_row() is None
    assert stream.rowcount == 3
    assert stream.lastrowid == 91
    assert stream.command == "INSERT 0 3"
    assert conn._txn_id == 77


def test_result_stream_treats_notice_before_ready_as_async_not_next_result_set():
    command_complete_payload = struct.pack("<B3xQQ", 1, 3, 91) + b"INSERT 0 3\x00"
    ready_payload = struct.pack("<B3xQQ", 1, 77, 0)
    stream_conn = _ResultConnection(
        [
            (_Header(MessageType.COMMAND_COMPLETE), command_complete_payload),
            (_Header(MessageType.NOTICE), _notice_payload(S="NOTICE", M="index build running")),
            (_Header(MessageType.READY), ready_payload),
        ]
    )
    stream_conn._handle_async = lambda header, payload: Connection._handle_async(stream_conn, header, payload)

    stream = ResultStream(stream_conn, page_size=0)

    assert stream.read_row() is None
    assert stream.has_next_result_set() is False
    assert stream_conn._last_notice == {"S": "NOTICE", "M": "index build running"}


def test_result_stream_exposes_next_result_set_boundaries():
    ready_payload = struct.pack("<B3xQQ", 1, 81, 0)
    conn = _ResultConnection(
        [
            (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("first_value")),
            (_Header(MessageType.DATA_ROW), _data_row_payload("1")),
            (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 7) + b"SELECT 1\x00"),
            (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("second_value")),
            (_Header(MessageType.DATA_ROW), _data_row_payload("2")),
            (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 8) + b"SELECT 1\x00"),
            (_Header(MessageType.READY), ready_payload),
        ]
    )
    stream = ResultStream(conn, page_size=0)

    assert stream.read_row() == ("1",)
    assert stream.read_row() is None
    assert stream.rowcount == 1
    assert stream.lastrowid == 7
    assert stream.command == "SELECT 1"
    assert stream.has_next_result_set() is True
    assert stream.next_result_set() is True

    assert stream.read_row() == ("2",)
    assert stream.read_row() is None
    assert stream.rowcount == 1
    assert stream.lastrowid == 8
    assert stream.command == "SELECT 1"
    assert stream.has_next_result_set() is False
    assert stream.next_result_set() is False
    assert conn._txn_id == 81


def test_cursor_nextset_advances_between_result_sets(monkeypatch):
    conn = _new_connection()
    stream = ResultStream(
        _ResultConnection(
            [
                (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("first_value")),
                (_Header(MessageType.DATA_ROW), _data_row_payload("1")),
                (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 7) + b"SELECT 1\x00"),
                (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("second_value")),
                (_Header(MessageType.DATA_ROW), _data_row_payload("2")),
                (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 8) + b"SELECT 1\x00"),
                (_Header(MessageType.READY), struct.pack("<B3xQQ", 1, 90, 0)),
            ]
        ),
        page_size=0,
    )
    monkeypatch.setattr(conn, "_execute_query", lambda sql, *_args, **_kwargs: stream)

    cursor = Cursor(conn)
    cursor.execute("SELECT 1; SELECT 2")

    assert cursor.fetchone() == ("1",)
    assert cursor.fetchone() is None
    assert cursor.rowcount == 1
    assert cursor.lastrowid == 7
    assert cursor.statusmessage == "SELECT 1"

    assert cursor.nextset() is True
    assert cursor.rowcount == -1
    assert cursor.lastrowid is None
    assert cursor.fetchone() == ("2",)
    assert cursor.fetchone() is None
    assert cursor.rowcount == 1
    assert cursor.lastrowid == 8
    assert cursor.statusmessage == "SELECT 1"
    assert cursor.nextset() is None


def test_cursor_execute_propagates_lastrowid_on_stream_completion(monkeypatch):
    conn = _new_connection()
    stream = _FakeStream(rows=[(1,)], rowcount=1, lastrowid=42, command="INSERT 0 1")
    monkeypatch.setattr(conn, "_execute_query", lambda *_args, **_kwargs: stream)

    cursor = Cursor(conn)
    cursor.execute("INSERT INTO t VALUES (?) RETURNING id", [1])

    assert cursor.fetchone() == (1,)
    assert cursor.lastrowid is None
    assert cursor.fetchone() is None
    assert cursor.rowcount == 1
    assert cursor.lastrowid == 42
    assert cursor.statusmessage == "INSERT 0 1"
    keys = cursor.get_generated_keys()
    assert keys.rowcount == 1
    assert keys.description[0][0] == "GENERATED_KEY"
    assert keys.fetchone() == (42,)
    assert keys.fetchone() is None


def test_cursor_execute_sets_description_before_first_fetch(monkeypatch):
    ready_payload = struct.pack("<B3xQQ", 1, 77, 0)
    stream_conn = _ResultConnection(
        [
            (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("value_col")),
            (_Header(MessageType.DATA_ROW), _data_row_payload("1")),
            (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 0) + b"SELECT 1\x00"),
            (_Header(MessageType.READY), ready_payload),
        ]
    )
    stream = ResultStream(stream_conn, page_size=0)
    conn = _new_connection()
    monkeypatch.setattr(conn, "_execute_query", lambda *_args, **_kwargs: stream)

    cursor = Cursor(conn)
    cursor.execute("SELECT 1")

    assert cursor.description is not None
    assert cursor.description[0][0] == "value_col"
    assert cursor.fetchone() == ("1",)


def test_cursor_execute_discards_unread_single_row_stream_before_next_execute(monkeypatch):
    ready_payload = struct.pack("<B3xQQ", 1, 77, 0)
    shared_conn = _ResultConnection(
        [
            (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("first_value")),
            (_Header(MessageType.DATA_ROW), _data_row_payload("1")),
            (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 0) + b"SELECT 1\x00"),
            (_Header(MessageType.READY), ready_payload),
            (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("second_value")),
            (_Header(MessageType.DATA_ROW), _data_row_payload("2")),
            (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 0) + b"SELECT 1\x00"),
            (_Header(MessageType.READY), ready_payload),
        ]
    )
    conn = _new_connection()
    monkeypatch.setattr(conn, "_execute_query", lambda *_args, **_kwargs: ResultStream(shared_conn, page_size=0))

    cursor = Cursor(conn)
    cursor.execute("SELECT 1")

    assert cursor.fetchone() == ("1",)

    cursor.execute("SELECT 2")

    assert cursor.description is not None
    assert cursor.description[0][0] == "second_value"
    assert cursor.fetchone() == ("2",)
    assert cursor.fetchone() is None


def test_cursor_execute_error_then_rollback_keeps_connection_usable(monkeypatch):
    ready_active_payload = struct.pack("<B3xQQ", 1, 44, 0)
    ready_idle_payload = struct.pack("<B3xQQ", 0, 0, 0)
    messages = [
        (
            _Header(MessageType.ERROR),
            _error_payload(
                S="ERROR",
                C="42P01",
                M="DROP TABLE resolve failed for definitely_missing_ddl_probe: Object not found",
            ),
        ),
        (_Header(MessageType.READY), ready_active_payload),
        (_Header(MessageType.READY), ready_idle_payload),
        (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("value_col")),
        (_Header(MessageType.DATA_ROW), _data_row_payload("1")),
        (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 0) + b"SELECT 1\x00"),
        (_Header(MessageType.READY), ready_idle_payload),
    ]

    conn = _new_connection(txn_id=44, active=True)
    sent = []

    def _recv_message():
        if not messages:
            raise AssertionError("unexpected _recv_message call")
        return messages.pop(0)

    monkeypatch.setattr(conn, "_recv_message", _recv_message)
    monkeypatch.setattr(conn, "_handle_async", lambda *_args, **_kwargs: False)
    monkeypatch.setattr(conn, "_send_simple_query", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(
        conn,
        "_send_message",
        lambda msg_type, payload, flags=0, force_zero=False: sent.append((msg_type, payload)),
    )

    cursor = Cursor(conn)

    with pytest.raises(errors.ProgrammingError, match="42P01"):
        cursor.execute("DROP TABLE definitely_missing_ddl_probe")

    conn.rollback()
    cursor.execute("SELECT 1")

    assert cursor.fetchone() == ("1",)
    assert cursor.fetchone() is None
    assert cursor.description is not None
    assert cursor.description[0][0] == "value_col"
    assert sent == [(MessageType.TXN_ROLLBACK, build_txn_rollback_payload(0))]


def test_cursor_execute_synthesizes_description_without_row_description(monkeypatch):
    ready_payload = struct.pack("<B3xQQ", 1, 88, 0)
    stream_conn = _ResultConnection(
        [
            (_Header(MessageType.DATA_ROW), _data_row_payload("1")),
            (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 0) + b"SELECT 1\x00"),
            (_Header(MessageType.READY), ready_payload),
        ]
    )
    stream = ResultStream(stream_conn, page_size=0)
    conn = _new_connection()
    monkeypatch.setattr(conn, "_execute_query", lambda *_args, **_kwargs: stream)

    cursor = Cursor(conn)
    cursor.execute("SELECT 1")

    assert cursor.description is not None
    assert cursor.description[0][0] == "column1"
    assert cursor.fetchone() is not None


def test_cursor_callproc_executes_callable_sql(monkeypatch):
    conn = _new_connection()
    calls = []
    stream = _FakeStream(rows=[], rowcount=0, lastrowid=None)

    def _fake_execute_query(sql, params, max_rows=0):
        calls.append((sql, list(params) if params is not None else None, max_rows))
        return stream

    monkeypatch.setattr(conn, "_execute_query", _fake_execute_query)
    cursor = Cursor(conn)

    returned = cursor.callproc("demo.proc", [5, 6])

    assert returned == [5, 6]
    assert calls == [("call demo.proc($1, $2)", [5, 6], 0)]


def test_cursor_callproc_rejects_empty_procedure_name():
    cursor = Cursor(_new_connection())
    with pytest.raises(errors.ProgrammingError, match="procname is required"):
        cursor.callproc("   ", [1])


def test_cursor_executemany_sets_final_lastrowid_and_total_rowcount(monkeypatch):
    conn = _new_connection()
    calls = []

    def _fake_execute_query(sql, params, max_rows=0):
        calls.append((sql, tuple(params), max_rows))
        return _FakeStream(rows=[], rowcount=2, lastrowid=11, command="INSERT 0 2")

    monkeypatch.setattr(conn, "_execute_query", _fake_execute_query)

    cursor = Cursor(conn)
    cursor.executemany("INSERT INTO t VALUES (?)", [(1,), (2,)])

    assert calls == [("INSERT INTO t VALUES (?), (?)", (1, 2), 0)]
    assert cursor.rowcount == 2
    assert cursor.lastrowid == 11
    assert cursor.statusmessage == "INSERT 0 2"
    assert cursor.get_generated_keys().fetchall() == [(11,)]


def test_cursor_executemany_caps_batched_insert_by_total_placeholder_count(monkeypatch):
    conn = _new_connection()
    calls = []

    def _fake_execute_query(sql, params, max_rows=0):
        calls.append((sql, tuple(params), max_rows))
        batch_rows = max(1, len(params) // 4)
        return _FakeStream(rows=[], rowcount=batch_rows, lastrowid=batch_rows, command=f"INSERT 0 {batch_rows}")

    monkeypatch.setattr(conn, "_execute_query", _fake_execute_query)

    cursor = Cursor(conn)
    cursor._DEFAULT_EXECUTEMANY_BATCH_ROWS = 4096
    rows = [(i, f"status-{i}", i % 10, f"payload-{i}") for i in range(2000)]

    cursor.executemany("INSERT INTO t VALUES (?, ?, ?, ?)", rows)

    assert len(calls) == 2
    assert len(calls[0][1]) == 6144
    assert len(calls[1][1]) == 1856
    assert cursor.rowcount == 2000
    assert cursor.lastrowid == 464
    assert cursor.statusmessage == "INSERT 0 464"


def test_cursor_executemany_caps_batched_insert_by_sql_text_size(monkeypatch):
    conn = _new_connection()
    calls = []

    def _fake_execute_query(sql, params, max_rows=0):
        calls.append((sql, tuple(params), max_rows))
        batch_rows = max(1, len(params))
        return _FakeStream(rows=[], rowcount=batch_rows, lastrowid=batch_rows, command=f"INSERT 0 {batch_rows}")

    monkeypatch.setattr(conn, "_execute_query", _fake_execute_query)

    cursor = Cursor(conn)
    cursor._DEFAULT_EXECUTEMANY_BATCH_ROWS = 4096
    cursor._MAX_EXECUTEMANY_BATCH_PARAMS = 65535
    cursor._MAX_EXECUTEMANY_BATCH_SQL_BYTES = 80
    rows = [(i,) for i in range(24)]

    cursor.executemany("INSERT INTO t VALUES (?)", rows)

    assert len(calls) > 1
    assert all(len(call[0]) <= 80 for call in calls)


def test_connection_execute_batch_returns_summary(monkeypatch):
    conn = _new_connection()
    calls = []

    class _FakeCursor:
        def __init__(self, rowcount, lastrowid, command):
            self.rowcount = rowcount
            self.lastrowid = lastrowid
            self.statusmessage = command
            self.description = [("id", 23, None, None, None, None, True)]

        def fetchall(self):
            return []

    cursors = [
        _FakeCursor(1, 10, "INSERT 0 1"),
        _FakeCursor(2, 11, "INSERT 0 2"),
    ]

    def _fake_execute(sql, params=None):
        calls.append((sql, tuple(params) if params is not None else None))
        return cursors[len(calls) - 1]

    monkeypatch.setattr(conn, "execute", _fake_execute)

    result = conn.execute_batch("INSERT INTO t VALUES (?)", [(1,), (2,)])

    assert calls == [
        ("INSERT INTO t VALUES (?)", (1,)),
        ("INSERT INTO t VALUES (?)", (2,)),
    ]
    assert result["totalRowCount"] == 3
    assert result["items"][0]["index"] == 0
    assert result["items"][0]["rowCount"] == 1
    assert result["items"][0]["lastId"] == 10
    assert result["items"][0]["command"] == "INSERT 0 1"
    assert result["items"][1]["index"] == 1
    assert result["items"][1]["rowCount"] == 2
    assert result["items"][1]["lastId"] == 11
    assert result["items"][1]["command"] == "INSERT 0 2"


def test_connection_execute_batch_validates_inputs():
    conn = _new_connection()
    with pytest.raises(errors.ProgrammingError, match="sql is required"):
        conn.execute_batch(None, [])
    with pytest.raises(errors.ProgrammingError, match="batch_params is required"):
        conn.execute_batch("INSERT INTO t VALUES (?)", None)


def test_connection_query_batch_aliases_execute_batch(monkeypatch):
    conn = _new_connection()
    monkeypatch.setattr(conn, "execute_batch", lambda sql, params: {"sql": sql, "params": params})
    result = conn.query_batch("SELECT 1", [()])
    assert result == {"sql": "SELECT 1", "params": [()]}


def test_connection_query_multi_returns_result_set_summaries(monkeypatch):
    conn = _new_connection()
    stream = ResultStream(
        _ResultConnection(
            [
                (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("first_value")),
                (_Header(MessageType.DATA_ROW), _data_row_payload("1")),
                (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 7) + b"SELECT 1\x00"),
                (_Header(MessageType.ROW_DESCRIPTION), _row_description_payload("second_value")),
                (_Header(MessageType.DATA_ROW), _data_row_payload("2")),
                (_Header(MessageType.COMMAND_COMPLETE), struct.pack("<B3xQQ", 1, 1, 8) + b"SELECT 1\x00"),
                (_Header(MessageType.READY), struct.pack("<B3xQQ", 1, 91, 0)),
            ]
        ),
        page_size=0,
    )
    monkeypatch.setattr(conn, "_execute_query", lambda sql, *_args, **_kwargs: stream)

    result_sets = conn.query_multi("SELECT 1; SELECT 2")

    assert len(result_sets) == 2
    assert result_sets[0]["rows"] == [("1",)]
    assert result_sets[0]["rowCount"] == 1
    assert result_sets[0]["command"] == "SELECT 1"
    assert result_sets[0]["lastId"] == 7
    assert result_sets[1]["rows"] == [("2",)]
    assert result_sets[1]["rowCount"] == 1
    assert result_sets[1]["command"] == "SELECT 1"
    assert result_sets[1]["lastId"] == 8


def test_connection_execute_multi_aliases_query_multi(monkeypatch):
    conn = _new_connection()
    monkeypatch.setattr(conn, "query_multi", lambda sql, params=None: {"sql": sql, "params": params})
    result = conn.execute_multi("SELECT 1", [1])
    assert result == {"sql": "SELECT 1", "params": [1]}


def test_generated_keys_accumulate_across_result_sets(monkeypatch):
    conn = _new_connection()
    monkeypatch.setattr(
        conn,
        "_execute_multi_statement_query",
        lambda *_args, **_kwargs: [
            {
                "rows": [("1",)],
                "rowCount": 1,
                "fields": [("first_value", 25, None, None, None, None, True)],
                "command": "SELECT 1",
                "lastId": 7,
            },
            {
                "rows": [("2",)],
                "rowCount": 1,
                "fields": [("second_value", 25, None, None, None, None, True)],
                "command": "SELECT 1",
                "lastId": 8,
            },
        ],
    )

    cursor = Cursor(conn)
    cursor.execute("SELECT 1; SELECT 2")
    assert cursor.fetchone() == ("1",)
    assert cursor.fetchone() is None
    assert cursor.nextset() is True
    assert cursor.fetchone() == ("2",)
    assert cursor.fetchone() is None
    assert cursor.get_generated_keys().fetchall() == [(7,), (8,)]


def test_connection_execute_with_generated_keys(monkeypatch):
    conn = _new_connection()
    stream = _FakeStream(rows=[], rowcount=1, lastrowid=55, command="INSERT 0 1")
    monkeypatch.setattr(conn, "_execute_query", lambda *_args, **_kwargs: stream)

    keys = conn.execute_with_generated_keys("INSERT INTO t VALUES (?)", [1])
    assert keys.fetchall() == [(55,)]
