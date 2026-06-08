# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pathlib
import sys
from typing import Any, Mapping, Optional

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

import scratchbird


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


class _FakeCursor:
    def __init__(self, rows: list[tuple[Any, ...]], rowcount: int = -1, description: Optional[list[Any]] = None):
        self._rows = list(rows)
        self._index = 0
        self.rowcount = rowcount
        self.description = description or []

    def fetchall(self):
        if self._index >= len(self._rows):
            return []
        out = self._rows[self._index :]
        self._index = len(self._rows)
        return out

    def fetchone(self):
        if self._index >= len(self._rows):
            return None
        row = self._rows[self._index]
        self._index += 1
        return row

    def close(self) -> None:
        return None


class _FakeWireConnection:
    def __init__(self, dsn: str):
        self.dsn = dsn
        self.closed = False
        self._txn_id = 0
        self._runtime_txn_active = True
        self.cancel_calls = 0
        self.begin_calls = 0
        self.commit_calls = 0
        self.rollback_calls = 0
        self.savepoints: list[str] = []
        self.last_metadata_restrictions: Optional[Mapping[str, Any]] = None

    def execute(self, sql: str, params=None):
        statement = str(sql).strip().lower()
        bound = list(params or [])
        if "broken_wire" in statement:
            raise RuntimeError("[08006] truncated wire frame")
        integer_literal = scratchbird._select_integer_literal(statement)
        if integer_literal is not None:
            return _FakeCursor([(integer_literal,)], rowcount=1)
        if statement == "select $1::integer, $2::integer":
            return _FakeCursor([(int(bound[0]), int(bound[1]))], rowcount=1)
        if statement.startswith("select id from basic_table"):
            return _FakeCursor([(1,), (2,), (3,)], rowcount=3)
        if "type_coverage" in statement:
            return _FakeCursor([("ok",)], rowcount=1)
        return _FakeCursor([], rowcount=0)

    def begin(self, **kwargs):
        _ = kwargs
        self.begin_calls += 1
        self._txn_id = 1
        self._runtime_txn_active = True

    def commit(self):
        self.commit_calls += 1
        self._txn_id = 0
        self._runtime_txn_active = True

    def rollback(self):
        self.rollback_calls += 1
        self._txn_id = 0
        self._runtime_txn_active = True

    def savepoint(self, name: str):
        self.savepoints.append(name)

    def release_savepoint(self, name: str):
        if name in self.savepoints:
            self.savepoints.remove(name)

    def rollback_to_savepoint(self, name: str):
        if name in self.savepoints:
            index = self.savepoints.index(name)
            self.savepoints = self.savepoints[: index + 1]

    def query_metadata(self, collection_name: str = "tables", restrictions: Optional[Mapping[str, Any]] = None):
        self.last_metadata_restrictions = restrictions
        if collection_name == "schemas":
            rows = [("users.alice.dev",), ("users.bob.dev",), ("sys",), (None,)]
            if restrictions and str(restrictions.get("schema", "")).strip() == "users.%":
                rows = [("users.alice.dev",), ("users.bob.dev",)]
            return _FakeCursor(rows, rowcount=len(rows), description=[("schema_name",)])
        return _FakeCursor([(1,)], rowcount=1)

    def ddl_editor_schema_payload(self, schema_pattern=None, expand_schema_parents=False):
        return {
            "schemaPattern": schema_pattern,
            "expandSchemaParents": bool(expand_schema_parents),
            "schemaPaths": ["users", "users.alice", "users.alice.dev"],
            "schemaTree": [{"name": "users", "path": "users", "children": []}],
        }

    def cancel(self):
        self.cancel_calls += 1

    def ping(self):
        return None

    def close(self):
        self.closed = True


class _FakePythonDriver:
    def __init__(self):
        self.connections: list[_FakeWireConnection] = []

    def connect(self, dsn=None, **kwargs):
        resolved_dsn = str(dsn or kwargs.get("dsn", "") or "")
        conn = _FakeWireConnection(resolved_dsn)
        self.connections.append(conn)
        return conn


def _with_fake_driver():
    fake = _FakePythonDriver()
    previous = scratchbird._PYTHON_DRIVER_MODULE
    scratchbird._PYTHON_DRIVER_MODULE = fake
    return fake, previous


def _restore_fake_driver(previous) -> None:
    scratchbird._PYTHON_DRIVER_MODULE = previous


def test_connect_uses_python_wire_transport() -> None:
    fake, previous = _with_fake_driver()
    try:
        cfg = scratchbird.ScratchBirdConfig(
            "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&sb_wire_transport=python"
        )
        conn = scratchbird.connect(cfg)
        _require(getattr(conn, "_wire_mode", False), "wire transport connection should set _wire_mode")
        _require(len(fake.connections) == 1, "wire connect should allocate fake connection")
        result = conn.query("SELECT 1")
        _require(result.rows == [[1]], "wire query should coerce cursor rows to list payload")
        conn.rollback()
        result = conn.query("SELECT 2")
        _require(result.rows == [[2]], "wire rollback boundary should not suppress the next scalar query")
        conn.close()
    finally:
        _restore_fake_driver(previous)


def test_wire_prepare_stream_and_lifecycle() -> None:
    fake, previous = _with_fake_driver()
    try:
        cfg = scratchbird.ScratchBirdConfig(
            "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&sb_wire_transport=python"
        )
        conn = scratchbird.connect(cfg)
        stmt = conn.prepare("SELECT $1::INTEGER, $2::INTEGER")
        prepared = stmt.execute(["5", "7"])
        _require(prepared.rows == [[5, 7]], "wire prepared execute payload mismatch")

        conn.begin()
        try:
            conn.begin()
            raise RuntimeError("expected nested wire begin on explicit transaction to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "25001", "nested wire begin should reject already-active explicit transaction")
        savepoint = conn.set_savepoint()
        _require(savepoint == "sp_1", "wire savepoint auto-name mismatch")
        conn.release_savepoint(savepoint)
        conn.commit()

        stream = conn.stream("SELECT id FROM basic_table ORDER BY id", None, 1)
        _require(stream.__next__() == [1], "wire stream first row mismatch")
        conn.cancel()
        try:
            stream.__next__()
            raise RuntimeError("expected cancelled wire stream to raise")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "57014", "wire stream cancel should surface 57014")
        finally:
            stream.close()

        metadata = conn.query_metadata_restricted_multi("schemas", {"schema": "users.%"})
        _require(metadata.rowcount == 2, "wire metadata restriction rowcount mismatch")
        payload = conn.ddl_editor_schema_payload(schema_pattern="users.%", expand_schema_parents=True)
        _require(payload.get("schemaPattern") == "users.%", "wire payload should retain schema pattern")
        _require(payload.get("expandSchemaParents") is True, "wire payload should retain expand flag")

        before = conn.lifecycle_snapshot()
        _ = conn.query("SELECT 1")
        after = conn.lifecycle_snapshot()
        _require(int(after.get("query_count", 0)) >= int(before.get("query_count", 0)), "query_count should advance")
        conn.close()
    finally:
        _restore_fake_driver(previous)


def test_wire_transport_maps_sqlstate_from_errors() -> None:
    fake, previous = _with_fake_driver()
    try:
        cfg = scratchbird.ScratchBirdConfig(
            "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&sb_wire_transport=python"
        )
        conn = scratchbird.connect(cfg)
        try:
            conn.query("SELECT broken_wire")
            raise RuntimeError("expected wire query failure")
        except scratchbird.ScratchBirdError as exc:
            _require(exc.sqlstate == "08006", "wire truncation/decode failures should preserve SQLSTATE")
        finally:
            conn.close()
        _require(len(fake.connections) == 1, "wire test should only create one fake connection")
    finally:
        _restore_fake_driver(previous)


def test_wire_reconnect_clears_abandoned_transaction_state() -> None:
    fake, previous = _with_fake_driver()
    try:
        cfg = scratchbird.ScratchBirdConfig(
            "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&sb_wire_transport=python"
        )
        conn = scratchbird.connect(cfg)
        _require(len(fake.connections) == 1, "initial wire connect should create one fake connection")

        conn._savepoints = ["stale_sp"]
        conn._txn_begin_options = {"isolation_level": 2, "access_mode": 1}
        conn._cancel_requested = True
        conn._mark_reconnect_required()

        _require(conn._needs_reconnect is True, "reconnect flag should be raised")
        _require(conn._savepoints == [], "reconnect-required path should clear tracked savepoints")
        _require(conn._txn_begin_options == {}, "reconnect-required path should clear begin options")
        _require(conn._cancel_requested is False, "reconnect-required path should clear cancel state")

        result = conn.query("SELECT 1")
        _require(result.rows == [[1]], "reconnected wire query should succeed on the fresh session")
        _require(len(fake.connections) == 2, "reconnect should allocate a fresh fake wire connection")
        _require(fake.connections[0].closed is True, "stale fake wire connection should be closed before reconnect")
        _require(fake.connections[1].begin_calls == 0, "reconnect should not replay explicit begin state implicitly")
        _require(conn._needs_reconnect is False, "successful reconnect should clear reconnect-required flag")
        _require(conn._savepoints == [], "fresh wire session should keep cleared savepoint state")
        _require(conn._txn_begin_options == {}, "fresh wire session should keep cleared begin options")
        _require(conn._txn_id == 0, "fresh wire session should preserve zero txn id on the fresh boundary")
        _require(conn._runtime_txn_active is True, "fresh wire session should preserve runtime boundary activity")
        conn.close()
    finally:
        _restore_fake_driver(previous)


def main() -> None:
    test_connect_uses_python_wire_transport()
    test_wire_prepare_stream_and_lifecycle()
    test_wire_transport_maps_sqlstate_from_errors()
    test_wire_reconnect_clears_abandoned_transaction_state()
    print("wire_transport_bridge: OK")


if __name__ == "__main__":
    main()
