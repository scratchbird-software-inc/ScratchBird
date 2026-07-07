# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pathlib
import struct
import sys
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

import scratchbird


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


class TxnHarness:
    def __init__(self, txn_id: int):
        self._txn_id = txn_id
        self._runtime_txn_active = txn_id != 0
        self._explicit_transaction = txn_id != 0
        self._savepoint_counter = 0
        self._savepoints = []
        self.sent = []
        self.drained = 0

    def _send_message(self, msg_type: int, payload: bytes, flags: int = 0, force_zero: bool = False):
        self.sent.append((msg_type, payload, flags, force_zero))

    def _drain_until_ready(self):
        self.drained += 1


class TxnHarnessNoSavepoints:
    def __init__(self, txn_id: int):
        self._txn_id = txn_id
        self._runtime_txn_active = txn_id != 0
        self._explicit_transaction = txn_id != 0
        self.sent = []
        self.drained = 0

    def _send_message(self, msg_type: int, payload: bytes, flags: int = 0, force_zero: bool = False):
        self.sent.append((msg_type, payload, flags, force_zero))

    def _drain_until_ready(self):
        self.drained += 1


class QueryHarness:
    def __init__(self):
        self.calls = []
        self.result = scratchbird.ScratchBirdResult([[1]], [], 1)

    def _begin_operation(self, name: str, sql: str):
        self.calls.append(("begin", name, sql))
        return None

    def _end_operation(self, span, success: bool):
        self.calls.append(("end", success))

    def _extended_query(self, sql: str, params):
        self.calls.append(("extended", sql, list(params)))
        return self.result

    def _send_message(self, msg_type: int, payload: bytes, flags: int = 0, force_zero: bool = False):
        self.calls.append(("send", msg_type))

    def _read_resultset(self):
        self.calls.append(("read",))
        return self.result


def _shim_cfg() -> scratchbird.ScratchBirdConfig:
    return scratchbird.ScratchBirdConfig("scratchbird://user:pass@localhost:3092/testdb?sslmode=require")


def test_begin_maps_kwargs_to_payload_flags() -> None:
    conn = TxnHarness(0)
    conn._savepoints = ["stale"]
    scratchbird.ScratchBirdConnection.begin(
        conn,
        isolation_level=2,
        access_mode=1,
        deferrable=True,
        wait=False,
        timeout_ms=75,
        autocommit_mode=1,
    )

    _require(len(conn.sent) == 1, "begin should send exactly one message")
    msg_type, payload, _, _ = conn.sent[0]
    _require(msg_type == scratchbird.MessageType.TXN_BEGIN, "begin should send TXN_BEGIN")

    flags, conflict, autocommit_mode, isolation, access_mode, deferrable, wait_mode, timeout_ms = struct.unpack(
        "<HBBBBBBI", payload
    )
    _require((flags & scratchbird.TXN_FLAG_HAS_ISOLATION) != 0, "missing isolation flag")
    _require((flags & scratchbird.TXN_FLAG_HAS_ACCESS) != 0, "missing access flag")
    _require((flags & scratchbird.TXN_FLAG_HAS_DEFERRABLE) != 0, "missing deferrable flag")
    _require((flags & scratchbird.TXN_FLAG_HAS_WAIT) != 0, "missing wait flag")
    _require((flags & scratchbird.TXN_FLAG_HAS_TIMEOUT) != 0, "missing timeout flag")
    _require((flags & scratchbird.TXN_FLAG_HAS_AUTOCOMMIT) != 0, "missing autocommit flag")
    _require(conflict == 0, "unexpected conflict_action")
    _require(autocommit_mode == 1, "unexpected autocommit_mode")
    _require(isolation == 2, "unexpected isolation_level")
    _require(access_mode == 1, "unexpected access_mode")
    _require(deferrable == 1, "unexpected deferrable value")
    _require(wait_mode == 0, "unexpected wait_mode")
    _require(timeout_ms == 75, "unexpected timeout_ms")
    _require(conn.drained == 1, "begin should drain once")
    _require(conn._txn_id == 1, "begin should mark transaction active")
    _require(conn._runtime_txn_active is True, "begin should mark runtime boundary active")
    _require(conn._explicit_transaction is True, "begin should mark explicit transaction active")
    _require(conn._savepoints == [], "begin should reset savepoints")
    begin_options = getattr(conn, "_txn_begin_options", {})
    _require(begin_options.get("isolation_level") == 2, "begin should persist normalized isolation_level")
    _require(begin_options.get("access_mode") == 1, "begin should persist normalized access_mode")
    _require(begin_options.get("deferrable") == 1, "begin should persist normalized deferrable")
    _require(begin_options.get("wait_mode") == 0, "begin should persist normalized wait_mode")
    _require(begin_options.get("timeout_ms") == 75, "begin should persist normalized timeout_ms")
    _require(begin_options.get("autocommit_mode") == 1, "begin should persist normalized autocommit_mode")


def test_begin_expands_payload_for_read_committed_mode() -> None:
    conn = TxnHarness(0)
    scratchbird.ScratchBirdConnection.begin(
        conn,
        timeout_ms=25,
        read_committed_mode=scratchbird.READ_COMMITTED_MODE_READ_CONSISTENCY,
    )

    _require(len(conn.sent) == 1, "begin should send exactly one message")
    msg_type, payload, _, _ = conn.sent[0]
    _require(msg_type == scratchbird.MessageType.TXN_BEGIN, "begin should send TXN_BEGIN")
    _require(len(payload) == 16, "read_committed_mode should expand begin payload")

    flags, conflict, autocommit_mode, isolation, access_mode, deferrable, wait_mode, timeout_ms, read_committed_mode = struct.unpack(
        "<HBBBBBBIB3x", payload
    )
    _require((flags & scratchbird.TXN_FLAG_HAS_ISOLATION) != 0, "missing isolation flag")
    _require((flags & scratchbird.TXN_FLAG_HAS_TIMEOUT) != 0, "missing timeout flag")
    _require(
        (flags & scratchbird.TXN_FLAG_HAS_READ_COMMITTED_MODE) != 0,
        "missing read committed mode flag",
    )
    _require(conflict == 0, "unexpected conflict_action")
    _require(autocommit_mode == 0, "unexpected autocommit_mode")
    _require(isolation == scratchbird.ISOLATION_READ_COMMITTED, "unexpected isolation_level")
    _require(access_mode == 0, "unexpected access_mode")
    _require(deferrable == 0, "unexpected deferrable value")
    _require(wait_mode == 0, "unexpected wait_mode")
    _require(timeout_ms == 25, "unexpected timeout_ms")
    _require(
        read_committed_mode == scratchbird.READ_COMMITTED_MODE_READ_CONSISTENCY,
        "unexpected read_committed_mode",
    )
    _require(conn.drained == 1, "begin should drain once")
    begin_options = getattr(conn, "_txn_begin_options", {})
    _require(begin_options.get("read_committed_mode") == scratchbird.READ_COMMITTED_MODE_READ_CONSISTENCY,
             "begin should persist normalized read_committed_mode")


def test_begin_rejects_read_committed_mode_with_snapshot_alias() -> None:
    conn = TxnHarness(0)
    try:
        scratchbird.ScratchBirdConnection.begin(
            conn,
            isolation_level=scratchbird.ISOLATION_SERIALIZABLE,
            read_committed_mode=scratchbird.READ_COMMITTED_MODE_READ_CONSISTENCY,
        )
        raise RuntimeError("expected invalid read_committed_mode isolation rejection")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "0A000", "invalid read_committed_mode should raise 0A000")
        _require("READ COMMITTED isolation alias" in str(exc), "rejection should explain allowed isolation")
    _require(len(conn.sent) == 0, "invalid begin should not send wire messages")
    _require(conn.drained == 0, "invalid begin should not drain")


def test_canonical_read_committed_mode_label_documents_public_selector() -> None:
    _require(
        scratchbird.canonical_read_committed_mode_label(scratchbird.READ_COMMITTED_MODE_DEFAULT)
        == "READ COMMITTED",
        "default read committed mode label mismatch",
    )
    _require(
        scratchbird.canonical_read_committed_mode_label(scratchbird.READ_COMMITTED_MODE_READ_CONSISTENCY)
        == "READ COMMITTED READ CONSISTENCY",
        "read consistency label mismatch",
    )
    _require(
        scratchbird.canonical_read_committed_mode_label(scratchbird.READ_COMMITTED_MODE_RECORD_VERSION)
        == "READ COMMITTED RECORD VERSION",
        "record version label mismatch",
    )
    _require(
        scratchbird.canonical_read_committed_mode_label(scratchbird.READ_COMMITTED_MODE_NO_RECORD_VERSION)
        == "READ COMMITTED NO RECORD VERSION",
        "no record version label mismatch",
    )
    _require(
        scratchbird.canonical_read_committed_mode_label(99) == "UNKNOWN(99)",
        "unknown mode label mismatch",
    )


def test_begin_rejects_nested_transaction() -> None:
    conn = TxnHarness(42)
    try:
        scratchbird.ScratchBirdConnection.begin(conn)
        raise RuntimeError("expected nested transaction begin rejection")
    except scratchbird.ScratchBirdError as exc:
        _require("transaction already active" in str(exc), "nested begin should raise clear message")
        _require(exc.sqlstate == "25001", "nested begin should map to 25001")
    _require(len(conn.sent) == 0, "nested begin should not send wire messages")
    _require(conn.drained == 0, "nested begin should not drain")


def test_begin_adopts_fresh_boundary_and_rejects_non_default_adoption() -> None:
    conn = TxnHarness(0)
    conn._runtime_txn_active = True
    conn._explicit_transaction = False
    scratchbird.ScratchBirdConnection.begin(conn)
    _require(len(conn.sent) == 0, "fresh-boundary adoption should not send wire begin")
    _require(conn._explicit_transaction is True, "fresh-boundary adoption should mark explicit transaction active")
    _require(conn._runtime_txn_active is True, "fresh-boundary adoption should preserve runtime boundary")
    _require(conn._txn_id == 0, "fresh-boundary adoption should preserve zero txn id")

    rejected = TxnHarness(0)
    rejected._runtime_txn_active = True
    rejected._explicit_transaction = False
    try:
        scratchbird.ScratchBirdConnection.begin(
            rejected,
            read_committed_mode=scratchbird.READ_COMMITTED_MODE_READ_CONSISTENCY,
        )
        raise RuntimeError("expected non-default fresh-boundary adoption rejection")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "0A000", "non-default fresh-boundary adoption should raise 0A000")
    _require(len(rejected.sent) == 0, "rejected fresh-boundary adoption should not send wire begin")


def test_begin_rejects_invalid_integer_kwargs() -> None:
    conn = TxnHarness(0)
    try:
        scratchbird.ScratchBirdConnection.begin(conn, isolation_level="bad")
        raise RuntimeError("expected invalid begin option rejection")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "22023", "invalid begin option should raise 22023")
    _require(len(conn.sent) == 0, "invalid begin should not send wire messages")
    _require(conn.drained == 0, "invalid begin should not drain")

    try:
        scratchbird.ScratchBirdConnection.begin(conn, wait_mode="bad")
        raise RuntimeError("expected invalid wait_mode rejection")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "22023", "invalid wait_mode should raise 22023")
    _require(len(conn.sent) == 0, "invalid wait_mode should not send wire messages")
    _require(conn.drained == 0, "invalid wait_mode should not drain")


def test_commit_and_rollback_noop_when_no_active_txn() -> None:
    conn = TxnHarness(0)
    scratchbird.ScratchBirdConnection.commit(conn)
    scratchbird.ScratchBirdConnection.rollback(conn)
    _require(len(conn.sent) == 0, "inactive txn should not send commit/rollback")
    _require(conn.drained == 0, "inactive txn should not drain")


def test_commit_and_rollback_send_when_active_txn() -> None:
    commit_conn = TxnHarness(42)
    commit_conn._savepoints = ["sp1"]
    commit_conn._txn_begin_options = {"isolation_level": 2}
    scratchbird.ScratchBirdConnection.commit(commit_conn)
    _require(len(commit_conn.sent) == 1, "active txn should send commit")
    _require(commit_conn.sent[0][0] == scratchbird.MessageType.TXN_COMMIT, "commit should send TXN_COMMIT")
    _require(commit_conn.drained == 1, "active commit should drain once")
    _require(commit_conn._txn_id == 0, "commit should preserve zero txn id on the fresh boundary")
    _require(commit_conn._runtime_txn_active is True, "commit should preserve runtime boundary activity")
    _require(commit_conn._explicit_transaction is False, "commit should clear explicit transaction state")
    _require(commit_conn._savepoints == [], "commit should clear savepoints")
    _require(getattr(commit_conn, "_txn_begin_options", None) == {}, "commit should clear begin options")

    rollback_conn = TxnHarness(42)
    rollback_conn._savepoints = ["sp1"]
    rollback_conn._txn_begin_options = {"isolation_level": 2}
    scratchbird.ScratchBirdConnection.rollback(rollback_conn)
    _require(len(rollback_conn.sent) == 1, "active txn should send rollback")
    _require(
        rollback_conn.sent[0][0] == scratchbird.MessageType.TXN_ROLLBACK,
        "rollback should send TXN_ROLLBACK",
    )
    _require(rollback_conn.drained == 1, "active rollback should drain once")
    _require(rollback_conn._txn_id == 0, "rollback should preserve zero txn id on the fresh boundary")
    _require(rollback_conn._runtime_txn_active is True, "rollback should preserve runtime boundary activity")
    _require(rollback_conn._explicit_transaction is False, "rollback should clear explicit transaction state")
    _require(rollback_conn._savepoints == [], "rollback should clear savepoints")
    _require(getattr(rollback_conn, "_txn_begin_options", None) == {}, "rollback should clear begin options")


def _decode_savepoint_payload(payload: bytes) -> str:
    length = struct.unpack("<I", payload[:4])[0]
    return payload[4 : 4 + length].decode("utf-8")


def test_savepoint_messages_and_payloads() -> None:
    conn = TxnHarness(42)
    generated = scratchbird.ScratchBirdConnection.set_savepoint(conn)
    named = scratchbird.ScratchBirdConnection.set_savepoint(conn, "named_sp")
    scratchbird.ScratchBirdConnection.release_savepoint(conn, "named_sp")
    scratchbird.ScratchBirdConnection.rollback_to_savepoint(conn, generated)

    _require(generated == "sp_1", "generated savepoint name mismatch")
    _require(named == "named_sp", "named savepoint mismatch")
    _require(conn.drained == 4, "savepoint operations should drain once per message")
    _require(conn.sent[0][0] == scratchbird.MessageType.TXN_SAVEPOINT, "expected TXN_SAVEPOINT message")
    _require(conn.sent[1][0] == scratchbird.MessageType.TXN_SAVEPOINT, "expected second TXN_SAVEPOINT message")
    _require(conn.sent[2][0] == scratchbird.MessageType.TXN_RELEASE, "expected TXN_RELEASE message")
    _require(conn.sent[3][0] == scratchbird.MessageType.TXN_ROLLBACK_TO, "expected TXN_ROLLBACK_TO message")
    _require(_decode_savepoint_payload(conn.sent[0][1]) == "sp_1", "generated payload mismatch")
    _require(_decode_savepoint_payload(conn.sent[1][1]) == "named_sp", "named payload mismatch")
    _require(_decode_savepoint_payload(conn.sent[2][1]) == "named_sp", "release payload mismatch")
    _require(_decode_savepoint_payload(conn.sent[3][1]) == "sp_1", "rollback_to payload mismatch")


def test_savepoint_tracking_initializes_when_missing() -> None:
    conn = TxnHarnessNoSavepoints(1)
    generated = scratchbird.ScratchBirdConnection.set_savepoint(conn)
    _require(generated == "sp_1", "generated savepoint should initialize from missing state")
    _require(getattr(conn, "_savepoints", None) == ["sp_1"], "set_savepoint should initialize tracking list")

    scratchbird.ScratchBirdConnection.release_savepoint(conn, "sp_1")
    _require(getattr(conn, "_savepoints", None) == [], "release should remove tracked savepoint")

    try:
        scratchbird.ScratchBirdConnection.release_savepoint(conn, "sp_1")
        raise RuntimeError("expected release of missing savepoint to raise")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "3B001", "missing release should raise 3B001")

    try:
        scratchbird.ScratchBirdConnection.rollback_to_savepoint(conn, "sp_1")
        raise RuntimeError("expected rollback_to missing savepoint to raise")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "3B001", "missing rollback_to should raise 3B001")


def test_savepoint_guards() -> None:
    inactive = TxnHarness(0)
    try:
        scratchbird.ScratchBirdConnection.set_savepoint(inactive)
        raise RuntimeError("expected inactive savepoint guard")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "25000", "inactive savepoint should raise 25000")

    active = TxnHarness(42)
    try:
        scratchbird.ScratchBirdConnection.release_savepoint(active, "")
        raise RuntimeError("expected empty release savepoint guard")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "HY000", "empty release savepoint should raise HY000")

    try:
        scratchbird.ScratchBirdConnection.release_savepoint(active, "missing")
        raise RuntimeError("expected missing release savepoint guard")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "3B001", "missing release savepoint should raise 3B001")

    try:
        scratchbird.ScratchBirdConnection.rollback_to_savepoint(active, "missing")
        raise RuntimeError("expected missing rollback savepoint guard")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "3B001", "missing rollback savepoint should raise 3B001")


def test_query_none_params_uses_simple_path() -> None:
    conn = QueryHarness()
    result = scratchbird.ScratchBirdConnection.query(conn, "SELECT 1", None)
    _require(result is conn.result, "query should return harness result")
    _require(("extended", "SELECT 1", []) not in conn.calls, "None params should not use extended path")
    _require(("send", scratchbird.MessageType.QUERY) in conn.calls, "simple query should send QUERY")
    _require(("read",) in conn.calls, "simple query should read resultset")


def test_query_empty_params_uses_extended_path() -> None:
    conn = QueryHarness()
    result = scratchbird.ScratchBirdConnection.query(conn, "SELECT 1", [])
    _require(result is conn.result, "query should return harness result")
    _require(("extended", "SELECT 1", []) in conn.calls, "empty params should use extended path")
    _require(("send", scratchbird.MessageType.QUERY) not in conn.calls, "extended path should not send simple QUERY")
    _require(("read",) not in conn.calls, "extended path should not call simple _read_resultset directly")


def test_shim_prepare_execute_and_mismatch() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        stmt = conn.prepare("SELECT $1::INTEGER, $2::INTEGER")
        result = stmt.execute([5, 7])
        _require(result.rows == [[5, 7]], "prepared execute should decode ordered params")
        try:
            stmt.execute([5])
            raise RuntimeError("expected prepared mismatch to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "07001", "prepared mismatch should raise 07001")
        try:
            stmt.execute(["bad", "7"])
            raise RuntimeError("expected prepared integer coercion guard to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "22023", "prepared integer coercion should raise 22023")
        stmt.close()
        stmt.close()
        try:
            stmt.execute([5, 7])
            raise RuntimeError("expected closed statement to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "HY010", "closed statement should raise HY010")

        stmt_conn_closed = conn.prepare("SELECT $1::INTEGER, $2::INTEGER")
        conn.close()
        try:
            stmt_conn_closed.execute([5, 7])
            raise RuntimeError("expected statement execute on closed connection to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "08003", "statement execute on closed connection should raise 08003")
    finally:
        conn.close()


def test_shim_ping_and_txn_lifecycle() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        _require(conn.ping(), "ping should succeed on open shim connection")
        conn.commit()
        conn.rollback()
        conn.begin()
        try:
            conn.begin()
            raise RuntimeError("expected nested begin on explicit shim transaction to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "25001", "nested begin on explicit shim transaction should raise 25001")
    finally:
        conn.close()


def test_shim_ping_and_txn_lifecycle_repeated_control() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        conn.commit()
        conn.rollback()
        conn.begin()
        try:
            conn.begin()
            raise RuntimeError("expected repeated begin to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "25001", "repeated begin should raise 25001")
        conn.commit()
    finally:
        conn.close()


def test_shim_begin_validates_kwargs_and_prepare_guard() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        try:
            conn.begin(isolation_level="bad")
            raise RuntimeError("expected invalid isolation_level rejection")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "22023", "invalid shim begin should raise 22023")
        _require(getattr(conn, "_txn_id", 0) == 0, "invalid begin should preserve zero txn id on the fresh boundary")
        _require(getattr(conn, "_runtime_txn_active", False) is True, "invalid begin should preserve runtime boundary")

        conn.commit()
        _require(getattr(conn, "_txn_id", 0) == 0, "commit should preserve zero txn id on the fresh boundary")
        _require(getattr(conn, "_runtime_txn_active", False) is True, "commit should preserve runtime boundary")

        conn.close()
        try:
            conn.prepare("SELECT 1")
            raise RuntimeError("expected prepare on closed connection to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "08003", "prepare on closed connection should raise 08003")
    finally:
        conn.close()


def test_static_prepared_and_dormant_capability_surfaces() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        _require(
            scratchbird.ScratchBirdConnection.supports_prepared_transactions(),
            "prepared transaction capability should stay explicit",
        )
        _require(
            not scratchbird.ScratchBirdConnection.supports_dormant_reattach(),
            "dormant reattach should stay explicit and false",
        )
        _require(
            not scratchbird.ScratchBirdConnection.supports_portal_resume(),
            "standalone portal resume should stay explicitly unsupported",
        )
        _require(
            scratchbird.ScratchBirdConnection.build_prepared_transaction_sql(
                "PREPARE TRANSACTION",
                "gid-1",
            )
            == "PREPARE TRANSACTION 'gid-1'",
            "prepare transaction SQL mismatch",
        )
        _require(
            scratchbird.ScratchBirdConnection.build_prepared_transaction_sql(
                "COMMIT PREPARED",
                "gid-1",
            )
            == "COMMIT PREPARED 'gid-1'",
            "commit prepared SQL mismatch",
        )
        _require(
            scratchbird.ScratchBirdConnection.build_prepared_transaction_sql(
                "ROLLBACK PREPARED",
                "gid'2",
            )
            == "ROLLBACK PREPARED 'gid''2'",
            "rollback prepared SQL quoting mismatch",
        )
        try:
            scratchbird.ScratchBirdConnection.build_prepared_transaction_sql(
                "PREPARE TRANSACTION",
                "   ",
            )
            raise RuntimeError("expected blank global transaction id to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "42601", "blank global transaction id should raise 42601")
        try:
            scratchbird.ScratchBirdConnection.prepare_transaction(conn, "   ")
            raise RuntimeError("expected prepared transaction guard to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "42601", "prepared transaction guard should raise 42601")
        try:
            scratchbird.ScratchBirdConnection.detach_to_dormant(conn)
            raise RuntimeError("expected dormant detach to fail closed")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "0A000", "dormant detach should raise 0A000")
        try:
            scratchbird.ScratchBirdConnection.reattach_dormant(conn, "dormant-1", "token")
            raise RuntimeError("expected dormant reattach to fail closed")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "0A000", "dormant reattach should raise 0A000")
    finally:
        conn.close()


def test_shim_savepoint_lifecycle() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        sp_auto = conn.set_savepoint()
        _require(sp_auto == "sp_1", "generated savepoint name mismatch")
        _require(conn.set_savepoint("named_sp") == "named_sp", "named savepoint mismatch")
        conn.set_savepoint("tail_sp")
        conn.rollback_to_savepoint("named_sp")

        try:
            conn.release_savepoint("tail_sp")
            raise RuntimeError("expected released-by-rollback savepoint to be missing")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "3B001", "rolled-back savepoint should raise 3B001")

        conn.release_savepoint("named_sp")
        conn.commit()
        try:
            conn.release_savepoint("named_sp")
            raise RuntimeError("expected release of cleared savepoint to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "3B001", "cleared savepoint should raise 3B001")
    finally:
        conn.close()


def test_stream_fetch_boundaries() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        stream = conn.stream("SELECT id FROM basic_table ORDER BY id", None, 1)
        _require(stream.__next__() == [1], "stream first row mismatch")
        _require(stream.__next__() == [2], "stream second row mismatch")
        stream.close()
        try:
            stream.__next__()
            raise RuntimeError("closed stream should raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "HY010", "closed stream should raise HY010")
    finally:
        conn.close()


def test_cancel_stream_returns_57014() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        stream = conn.stream(
            "SELECT a.id FROM basic_table a, basic_table b, basic_table c, basic_table d, basic_table e",
            None,
            1,
        )
        _ = stream.__next__()
        conn.cancel()
        try:
            stream.__next__()
            raise RuntimeError("expected cancelled stream to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "57014", "cancelled stream should raise sqlstate 57014")
    finally:
        conn.close()


def test_post_cancel_stream_recovery() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        stream = conn.stream(
            "SELECT a.id FROM basic_table a, basic_table b, basic_table c, basic_table d, basic_table e",
            None,
            1,
        )
        _ = stream.__next__()
        conn.cancel()
        try:
            stream.__next__()
            raise RuntimeError("expected cancelled stream to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "57014", "cancelled stream should raise sqlstate 57014")

        post_cancel = conn.stream("SELECT id FROM basic_table ORDER BY id", None, 1)
        _require(post_cancel.__next__() == [1], "post-cancel stream should recover")
    finally:
        conn.close()


def test_close_is_idempotent_for_connection_and_stream() -> None:
    conn = scratchbird.connect(_shim_cfg())
    stream = conn.stream("SELECT id FROM basic_table ORDER BY id", None, 1)
    _ = stream.__next__()
    stream.close()
    stream.close()
    try:
        stream.__next__()
        raise RuntimeError("closed stream should raise")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "HY010", "closed stream should raise HY010")
    conn.close()
    conn.close()


def test_shim_closed_connection_guards() -> None:
    conn = scratchbird.connect(_shim_cfg())
    active_stream = conn.stream("SELECT id FROM basic_table ORDER BY id", None, 1)
    _ = active_stream.__next__()
    conn.close()

    try:
        active_stream.__next__()
        raise RuntimeError("active stream on closed connection should raise")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "08003", "active stream on closed connection should raise 08003")

    def _expect_08003(fn) -> None:
        try:
            fn()
            raise RuntimeError("expected operation on closed connection to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "08003", "closed connection operation should raise 08003")

    _expect_08003(lambda: conn.query("SELECT 1"))
    _expect_08003(lambda: conn.begin())
    _expect_08003(lambda: conn.commit())
    _expect_08003(lambda: conn.rollback())
    _expect_08003(lambda: conn.cancel())
    _expect_08003(lambda: conn.query_metadata("tables"))
    _expect_08003(lambda: conn.query_metadata("unsupported_collection"))
    _expect_08003(lambda: conn.query_metadata_restricted("tables", "unsupported_restriction", "public"))
    _expect_08003(lambda: conn.query_metadata_restricted_multi("tables", object()))
    _expect_08003(lambda: conn.stream("SELECT id FROM basic_table ORDER BY id", None, 1))


def test_static_closed_connection_guards() -> None:
    def _expect_08003(fn) -> None:
        try:
            fn()
            raise RuntimeError("expected closed connection operation to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "08003", "closed operation should raise 08003")

    closed_tx = TxnHarness(0)
    closed_tx._closed = True
    closed_tx._savepoints = ["sp_1"]
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.begin(closed_tx))
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.commit(closed_tx))
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.rollback(closed_tx))

    closed_tx._txn_id = 1
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.set_savepoint(closed_tx, "sp_2"))
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.release_savepoint(closed_tx, "sp_1"))
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.rollback_to_savepoint(closed_tx, "sp_1"))

    closed_query = QueryHarness()
    closed_query._closed = True
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.query(closed_query, "SELECT 1", None))
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.query_metadata(closed_query, "tables"))
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.query_metadata(closed_query, "unsupported_collection"))
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.query_metadata_rows(closed_query, "tables"))
    _expect_08003(
        lambda: scratchbird.ScratchBirdConnection.query_metadata_restricted(
            closed_query,
            "tables",
            "schema",
            "public",
        )
    )
    _expect_08003(
        lambda: scratchbird.ScratchBirdConnection.query_metadata_restricted(
            closed_query,
            "tables",
            "unsupported_restriction",
            "public",
        )
    )
    _expect_08003(
        lambda: scratchbird.ScratchBirdConnection.query_metadata_restricted_multi(
            closed_query,
            "tables",
            {"schema": "public"},
        )
    )
    _expect_08003(
        lambda: scratchbird.ScratchBirdConnection.query_metadata_restricted_multi(
            closed_query,
            "tables",
            object(),
        )
    )
    _expect_08003(
        lambda: scratchbird.ScratchBirdConnection.query_metadata_rows_restricted(
            closed_query,
            "tables",
            "schema",
            "public",
        )
    )
    _expect_08003(
        lambda: scratchbird.ScratchBirdConnection.query_metadata_rows_restricted_multi(
            closed_query,
            "tables",
            {"schema": "public"},
        )
    )
    _expect_08003(lambda: scratchbird.ScratchBirdConnection.get_schema(closed_query, "schemas"))
    _expect_08003(
        lambda: scratchbird.ScratchBirdConnection.ddl_editor_schema_payload(
            closed_query,
            "%",
            False,
        )
    )
    _expect_08003(
        lambda: scratchbird.ScratchBirdConnection.ddl_editor_schema_payload(
            closed_query,
            "public",
            False,
        )
    )


def test_static_metadata_rowcount_fallbacks() -> None:
    conn = QueryHarness()

    conn.result = SimpleNamespace(rowcount="bad", rows=[[1], [2], [3]])
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "tables") == 3,
        "query_metadata_rows should fall back to len(rows) when rowcount is invalid",
    )

    conn.result = SimpleNamespace(rowcount=True, rows=[[1], [2]])
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "tables") == 2,
        "query_metadata_rows should treat boolean rowcount as invalid and fall back to len(rows)",
    )

    conn.result = SimpleNamespace(rowcount=-1, rows=[[1], [2], [3], [4]])
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "tables") == 4,
        "query_metadata_rows should treat negative rowcount as invalid and fall back to len(rows)",
    )

    conn.result = SimpleNamespace(rowcount=None, rows=[[1], [2]])
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows_restricted(
            conn,
            "tables",
            "schema",
            "public",
        )
        == 2,
        "query_metadata_rows_restricted should fall back to len(rows) when rowcount missing",
    )

    conn.result = SimpleNamespace(rowcount=None, rows=(["a"], ["b"], ["c"], ["d"]))
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows_restricted_multi(
            conn,
            "tables",
            {"schema": "public"},
        )
        == 4,
        "query_metadata_rows_restricted_multi should support tuple row fallback",
    )

    conn.result = SimpleNamespace(rowcount=None)
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "tables") == 0,
        "query_metadata_rows should return 0 when rows are missing",
    )

    conn.result = SimpleNamespace(rowcount=None, rows=object())
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "tables") == 0,
        "query_metadata_rows should return 0 when rows are unsized",
    )

    conn.result = SimpleNamespace(rowcount=None, rows={"schema_name": "users"})
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "tables") == 0,
        "query_metadata_rows should return 0 when rows are mappings",
    )

    conn.result = SimpleNamespace(rowcount=None, rows="not rows")
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "tables") == 0,
        "query_metadata_rows should return 0 when rows are text",
    )

    conn.result = SimpleNamespace(rowcount=None, rows={("users",), ("public",)})
    _require(
        scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "tables") == 0,
        "query_metadata_rows should return 0 when rows are unsupported iterables",
    )

    conn.result = SimpleNamespace(rowcount=None, rows=(["x"], ["y"]))
    _require(
        scratchbird.ScratchBirdConnection.get_schema(conn, "schemas") == [["x"], ["y"]],
        "get_schema should convert tuple rows to list",
    )

    conn.result = SimpleNamespace(rowcount=None, rows=object())
    _require(
        scratchbird.ScratchBirdConnection.get_schema(conn, "schemas") == [],
        "get_schema should return [] when rows are unsized",
    )

    conn.result = SimpleNamespace(rowcount=None, rows={"schema_name": "users"})
    _require(
        scratchbird.ScratchBirdConnection.get_schema(conn, "schemas") == [],
        "get_schema should return [] when rows are mappings",
    )

    conn.result = SimpleNamespace(rowcount=None, rows="not rows")
    _require(
        scratchbird.ScratchBirdConnection.get_schema(conn, "schemas") == [],
        "get_schema should return [] when rows are text",
    )

    conn.result = SimpleNamespace(rowcount=None, rows=(row for row in [["x"], ["y"]]))
    _require(
        scratchbird.ScratchBirdConnection.get_schema(conn, "schemas") == [],
        "get_schema should return [] when rows are unsupported iterables",
    )


def test_instance_metadata_rowcount_fallbacks() -> None:
    conn = scratchbird.connect(_shim_cfg())
    try:
        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount="bad", rows=[[1], [2], [3]])
        _require(
            conn.query_metadata_rows("tables") == 3,
            "instance query_metadata_rows should fall back to len(rows) when rowcount is invalid",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=False, rows=[[1], [2]])
        _require(
            conn.query_metadata_rows("tables") == 2,
            "instance query_metadata_rows should treat boolean rowcount as invalid and fall back to len(rows)",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=-1, rows=[[1], [2], [3], [4]])
        _require(
            conn.query_metadata_rows("tables") == 4,
            "instance query_metadata_rows should treat negative rowcount as invalid and fall back to len(rows)",
        )

        conn.query_metadata_restricted = (
            lambda collection_name=None, restriction_key=None, restriction_value=None: SimpleNamespace(
                rowcount=None,
                rows=[[1], [2]],
            )
        )
        _require(
            conn.query_metadata_rows_restricted("tables", "schema", "public") == 2,
            "instance query_metadata_rows_restricted should fall back to len(rows) when rowcount missing",
        )

        conn.query_metadata_restricted_multi = (
            lambda collection_name=None, restrictions=None: SimpleNamespace(
                rowcount=None,
                rows=(["a"], ["b"], ["c"]),
            )
        )
        _require(
            conn.query_metadata_rows_restricted_multi("tables", {"schema": "public"}) == 3,
            "instance query_metadata_rows_restricted_multi should support tuple row fallback",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=None, rows=object())
        _require(
            conn.query_metadata_rows("tables") == 0,
            "instance query_metadata_rows should return 0 when rows are unsized",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=None, rows={"schema_name": "users"})
        _require(
            conn.query_metadata_rows("tables") == 0,
            "instance query_metadata_rows should return 0 when rows are mappings",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=None, rows="not rows")
        _require(
            conn.query_metadata_rows("tables") == 0,
            "instance query_metadata_rows should return 0 when rows are text",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(
            rowcount=None,
            rows={("users",), ("public",)},
        )
        _require(
            conn.query_metadata_rows("tables") == 0,
            "instance query_metadata_rows should return 0 when rows are unsupported iterables",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=None, rows=(["x"], ["y"]))
        _require(
            conn.get_schema("schemas") == [["x"], ["y"]],
            "instance get_schema should convert tuple rows to list",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=None, rows=object())
        _require(
            conn.get_schema("schemas") == [],
            "instance get_schema should return [] when rows are unsized",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=None, rows={"schema_name": "users"})
        _require(
            conn.get_schema("schemas") == [],
            "instance get_schema should return [] when rows are mappings",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(rowcount=None, rows="not rows")
        _require(
            conn.get_schema("schemas") == [],
            "instance get_schema should return [] when rows are text",
        )

        conn.query_metadata = lambda collection_name=None: SimpleNamespace(
            rowcount=None,
            rows=(row for row in [["x"], ["y"]]),
        )
        _require(
            conn.get_schema("schemas") == [],
            "instance get_schema should return [] when rows are unsupported iterables",
        )
    finally:
        conn.close()


def test_ddl_editor_payload_rows_fallbacks() -> None:
    static_conn = QueryHarness()
    static_conn.result = SimpleNamespace(
        rowcount=None,
        rows=(
            {"schema_name": "users.alice.dev"},
            {"schema_name": "users.bob.dev"},
        ),
    )
    payload = scratchbird.ScratchBirdConnection.ddl_editor_schema_payload(
        static_conn,
        "users.%",
        True,
    )
    _require(
        payload["schemaPaths"] == ["users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"],
        "static ddl payload should normalize tuple rows",
    )
    _require(payload["schemaPattern"] == "users.%", "static ddl payload schemaPattern mismatch")
    _require(payload["expandSchemaParents"] is True, "static ddl payload expandSchemaParents mismatch")

    static_conn.result = SimpleNamespace(rowcount=None, rows=object())
    payload_unsized = scratchbird.ScratchBirdConnection.ddl_editor_schema_payload(
        static_conn,
        "users.%",
        False,
    )
    _require(payload_unsized["schemaPaths"] == [], "static ddl payload should fallback to empty rows for unsized payload")

    static_conn.result = SimpleNamespace(rowcount=None, rows={"schema_name": "users.alice.dev"})
    payload_mapping = scratchbird.ScratchBirdConnection.ddl_editor_schema_payload(
        static_conn,
        "users.%",
        False,
    )
    _require(payload_mapping["schemaPaths"] == [], "static ddl payload should fallback to empty rows for mapping payload")

    static_conn.result = SimpleNamespace(rowcount=None, rows="users.alice.dev")
    payload_text = scratchbird.ScratchBirdConnection.ddl_editor_schema_payload(
        static_conn,
        "users.%",
        False,
    )
    _require(payload_text["schemaPaths"] == [], "static ddl payload should fallback to empty rows for text payload")

    static_conn.result = SimpleNamespace(rowcount=None, rows={("users.alice.dev",), ("users.bob.dev",)})
    payload_iterable = scratchbird.ScratchBirdConnection.ddl_editor_schema_payload(
        static_conn,
        "users.%",
        False,
    )
    _require(
        payload_iterable["schemaPaths"] == [],
        "static ddl payload should fallback to empty rows for unsupported iterables",
    )

    instance_conn = scratchbird.connect(_shim_cfg())
    try:
        instance_conn.query_metadata_restricted_multi = (
            lambda collection_name=None, restrictions=None: SimpleNamespace(
                rowcount=None,
                rows=(
                    {"schema_name": "users.alice.dev"},
                    {"schema_name": "users.bob.dev"},
                ),
            )
        )
        instance_payload = instance_conn.ddl_editor_schema_payload("users.%", True)
        _require(
            instance_payload["schemaPaths"]
            == ["users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"],
            "instance ddl payload should normalize tuple rows",
        )

        instance_conn.query_metadata_restricted_multi = (
            lambda collection_name=None, restrictions=None: SimpleNamespace(
                rowcount=None,
                rows=object(),
            )
        )
        instance_payload_unsized = instance_conn.ddl_editor_schema_payload("users.%", False)
        _require(
            instance_payload_unsized["schemaPaths"] == [],
            "instance ddl payload should fallback to empty rows for unsized payload",
        )

        instance_conn.query_metadata_restricted_multi = (
            lambda collection_name=None, restrictions=None: SimpleNamespace(
                rowcount=None,
                rows={"schema_name": "users.alice.dev"},
            )
        )
        instance_payload_mapping = instance_conn.ddl_editor_schema_payload("users.%", False)
        _require(
            instance_payload_mapping["schemaPaths"] == [],
            "instance ddl payload should fallback to empty rows for mapping payload",
        )

        instance_conn.query_metadata_restricted_multi = (
            lambda collection_name=None, restrictions=None: SimpleNamespace(
                rowcount=None,
                rows="users.alice.dev",
            )
        )
        instance_payload_text = instance_conn.ddl_editor_schema_payload("users.%", False)
        _require(
            instance_payload_text["schemaPaths"] == [],
            "instance ddl payload should fallback to empty rows for text payload",
        )

        instance_conn.query_metadata_restricted_multi = (
            lambda collection_name=None, restrictions=None: SimpleNamespace(
                rowcount=None,
                rows=(row for row in [["users.alice.dev"], ["users.bob.dev"]]),
            )
        )
        instance_payload_iterable = instance_conn.ddl_editor_schema_payload("users.%", False)
        _require(
            instance_payload_iterable["schemaPaths"] == [],
            "instance ddl payload should fallback to empty rows for unsupported iterables",
        )
    finally:
        instance_conn.close()


def main() -> None:
    test_begin_maps_kwargs_to_payload_flags()
    test_begin_rejects_nested_transaction()
    test_begin_rejects_invalid_integer_kwargs()
    test_commit_and_rollback_noop_when_no_active_txn()
    test_commit_and_rollback_send_when_active_txn()
    test_savepoint_messages_and_payloads()
    test_savepoint_tracking_initializes_when_missing()
    test_savepoint_guards()
    test_query_none_params_uses_simple_path()
    test_query_empty_params_uses_extended_path()
    test_shim_prepare_execute_and_mismatch()
    test_shim_ping_and_txn_lifecycle()
    test_shim_begin_validates_kwargs_and_prepare_guard()
    test_shim_savepoint_lifecycle()
    test_stream_fetch_boundaries()
    test_cancel_stream_returns_57014()
    test_post_cancel_stream_recovery()
    test_close_is_idempotent_for_connection_and_stream()
    test_shim_closed_connection_guards()
    test_static_closed_connection_guards()
    test_static_metadata_rowcount_fallbacks()
    test_instance_metadata_rowcount_fallbacks()
    test_ddl_editor_payload_rows_fallbacks()
    print("Mojo TXN/EXEC parity tests OK")


if __name__ == "__main__":
    main()
