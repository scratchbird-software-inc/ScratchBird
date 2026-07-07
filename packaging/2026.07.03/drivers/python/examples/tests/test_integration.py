# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import json
import os
import threading
import time

import pytest

import scratchbird


def test_basic_query_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        cur = conn.cursor()
        cur.execute("SELECT 1")
        row = cur.fetchone()
        assert row == (1,)
    finally:
        conn.close()


def test_prepare_bind_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        cur = conn.cursor()
        cur.execute("SELECT ?::INTEGER", (42,))
        row = cur.fetchone()
        assert row == (42,)
    finally:
        conn.close()


def test_types_fixture_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        cur = conn.cursor()
        cur.execute("SELECT * FROM type_coverage")
        row = cur.fetchone()
        assert row is not None
        assert len(row) > 0
    finally:
        conn.close()


def test_basic_type_roundtrip_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        cur = conn.cursor()
        cur.execute(
            "SELECT ?::INTEGER, ?::DOUBLE, ?::VARCHAR, ?::BOOLEAN",
            (42, 3.5, "scratchbird", True),
        )
        row = cur.fetchone()
        assert row == (42, 3.5, "scratchbird", True)
    finally:
        conn.close()


def test_json_type_roundtrip_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        cur = conn.cursor()
        cur.execute(
            "SELECT ?::JSON, ?::JSONB",
            ({"a": 1}, scratchbird.Jsonb(raw=b'{"b":2}')),
        )
        row = cur.fetchone()
        assert row is not None
        assert json.loads(row[0]) == {"a": 1}
        assert isinstance(row[1], scratchbird.Jsonb)
        assert json.loads(row[1].raw.decode("utf-8")) == {"b": 2}
    finally:
        conn.close()


def test_cancel_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    cancel_sql = os.environ.get("SCRATCHBIRD_TEST_CANCEL_SQL")
    if not cancel_sql:
        pytest.skip("SCRATCHBIRD_TEST_CANCEL_SQL not set")
    conn = scratchbird.connect(dsn)
    error = []
    done = threading.Event()

    def run_query():
        try:
            cur = conn.cursor()
            cur.execute(cancel_sql)
            cur.fetchall()
        except Exception as exc:  # noqa: BLE001
            error.append(exc)
        finally:
            done.set()

    thread = threading.Thread(target=run_query)
    thread.start()
    time.sleep(0.2)
    if not thread.is_alive():
        conn.close()
        pytest.skip("SCRATCHBIRD_TEST_CANCEL_SQL completed before cancel window")
    conn.cancel()
    thread.join(timeout=5)
    if thread.is_alive():
        conn.close()
        thread.join(timeout=5)
    else:
        conn.close()
    assert done.is_set(), "expected query to terminate after cancel/close fallback"
    assert error or not thread.is_alive()


def test_query_multi_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        result_sets = conn.query_multi("SELECT 1; SELECT 2")
        assert len(result_sets) == 2
        assert result_sets[0]["rows"] == [(1,)]
        assert result_sets[1]["rows"] == [(2,)]
    finally:
        conn.close()


def test_query_multi_summary_shape_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        result_sets = conn.query_multi("SELECT 1; SELECT 2")
        assert len(result_sets) == 2
        for result_set in result_sets:
            assert set(result_set) == {"rows", "rowCount", "fields", "command", "lastId"}
            assert isinstance(result_set["rows"], list)
            assert isinstance(result_set["fields"], list)
    finally:
        conn.close()


def test_execute_multi_alias_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        result_sets = conn.execute_multi("SELECT 3; SELECT 4")
        assert len(result_sets) == 2
        assert result_sets[0]["rows"] == [(3,)]
        assert result_sets[1]["rows"] == [(4,)]
    finally:
        conn.close()


def test_execute_batch_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        batch = conn.execute_batch("SELECT ?::INTEGER", [(11,), (22,)])
        assert batch["totalRowCount"] >= 0
        assert [item["index"] for item in batch["items"]] == [0, 1]
        assert len(batch["items"]) == 2
    finally:
        conn.close()


def test_execute_batch_summary_shape_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        batch = conn.execute_batch("SELECT ?::INTEGER", [(11,), (22,)])
        assert set(batch) == {"items", "totalRowCount"}
        assert len(batch["items"]) == 2
        for expected_index, item in enumerate(batch["items"]):
            assert set(item) == {"index", "rowCount", "fields", "command", "lastId"}
            assert item["index"] == expected_index
            assert isinstance(item["fields"], list)
    finally:
        conn.close()


def test_query_batch_alias_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        batch = conn.query_batch("SELECT ?::INTEGER", [(7,), (8,)])
        assert set(batch) == {"items", "totalRowCount"}
        assert len(batch["items"]) == 2
        assert [item["index"] for item in batch["items"]] == [0, 1]
    finally:
        conn.close()


def test_execute_with_generated_keys_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        keys = conn.execute_with_generated_keys("SELECT 1")
        rows = keys.fetchall()
        assert len(rows) == 1
        assert isinstance(rows[0][0], int)
    finally:
        conn.close()


def test_cursor_get_generated_keys_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        cur = conn.cursor()
        cur.execute("SELECT 1")
        cur.fetchall()
        keys = cur.get_generated_keys()
        rows = keys.fetchall()
        assert len(rows) == 1
        assert isinstance(rows[0][0], int)
    finally:
        conn.close()


def test_cursor_nextset_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        cur = conn.cursor()
        cur.execute("SELECT 1; SELECT 2")
        assert cur.fetchone() == (1,)
        assert cur.fetchone() is None
        assert cur.nextset() is True
        assert cur.fetchone() == (2,)
        assert cur.fetchone() is None
        assert cur.nextset() is None
    finally:
        conn.close()


def test_connection_call_callable_escape_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        cur = conn.call("{? = call abs(?)}", [9])
        assert cur.fetchone() == (9,)
    finally:
        conn.close()


def test_session_schema_runtime_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        conn.set_session_schema("public")
        assert conn.get_session_schema() == "users.public"
        conn.set_session_schema(None)
        assert conn.get_session_schema() is None
    finally:
        conn.close()


def test_connection_ping_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        conn.ping()
    finally:
        conn.close()


def test_connection_is_valid_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        assert conn.is_valid() is True
        assert conn.is_valid(250) is True
    finally:
        conn.close()
    assert conn.is_valid() is False


def test_transaction_begin_commit_rollback_cycle_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        assert conn._transaction_active() is True
        assert conn._txn_id > 0

        conn.commit()
        assert conn._transaction_active() is True
        assert conn._txn_id > 0
        conn.begin()
        assert conn._transaction_active() is True
        assert conn._txn_id > 0
        cur = conn.cursor()
        cur.execute("SELECT 1")
        assert cur.fetchone() == (1,)
        conn.rollback()
        assert conn._transaction_active() is True
        assert conn._txn_id > 0
        conn.begin()
        assert conn._transaction_active() is True
        assert conn._txn_id > 0
        cur = conn.cursor()
        cur.execute("SELECT 2")
        assert cur.fetchone() == (2,)
        conn.commit()
        assert conn._transaction_active() is True
        assert conn._txn_id > 0

        conn.commit()
        conn.rollback()
    finally:
        conn.close()


def test_transaction_begin_restarts_current_boundary_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        first_txn = conn._txn_id
        assert first_txn > 0
        conn.begin()
        second_txn = conn._txn_id
        assert second_txn > 0
        assert conn._transaction_active() is True
    finally:
        conn.close()


def test_autocommit_mode_transition_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        assert conn.autocommit is True
        conn.autocommit = False
        assert conn.autocommit is False
        assert conn._transaction_active() is True
        assert conn._txn_id > 0
        cur = conn.cursor()
        cur.execute("SELECT 1")
        assert cur.fetchone() == (1,)
        assert conn._transaction_active() is True
        assert conn._txn_id > 0
        conn.autocommit = True
        assert conn.autocommit is True
    finally:
        conn.close()


def test_transaction_savepoint_lifecycle_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        conn.autocommit = False
        assert conn._transaction_active() is True
        assert conn._txn_id > 0
        cur = conn.cursor()
        cur.execute("SELECT 1")
        assert cur.fetchone() == (1,)
        conn.savepoint("sp1")
        conn.rollback_to_savepoint("sp1")
        conn.release_savepoint("sp1")
        conn.commit()
        assert conn._transaction_active() is True
        assert conn._txn_id > 0
    finally:
        conn.close()


def test_metadata_wrappers_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        assert isinstance(conn.schemas(), list)
        assert isinstance(conn.tables(), list)
        assert isinstance(conn.columns(), list)
        assert isinstance(conn.indexes(), list)
        assert isinstance(conn.index_columns(), list)
        assert isinstance(conn.constraints(), list)
        assert isinstance(conn.catalogs(), list)
        assert isinstance(conn.primary_keys(), list)
        assert isinstance(conn.foreign_keys(), list)
        assert isinstance(conn.procedures(), list)
        assert isinstance(conn.functions(), list)
        assert isinstance(conn.routines(), list)
        assert isinstance(conn.table_privileges(), list)
        assert isinstance(conn.column_privileges(), list)
        assert isinstance(conn.type_info(), list)
    finally:
        conn.close()


def test_metadata_restriction_wrappers_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        missing_name = "__scratchbird_metadata_missing_object__"
        assert conn.tables(table=missing_name) == []
        assert conn.columns(table=missing_name) == []
        assert conn.indexes(table=missing_name) == []
        assert conn.index_columns(table=missing_name) == []
        assert conn.constraints(table=missing_name) == []
        assert conn.primary_keys(table=missing_name) == []
        assert conn.foreign_keys(table=missing_name) == []
        assert conn.table_privileges(table=missing_name) == []
        assert conn.column_privileges(table=missing_name) == []
    finally:
        conn.close()


def test_metadata_restriction_wildcard_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        all_tables = conn.tables()
        wildcard_tables = conn.tables(table="%")
        assert len(wildcard_tables) == len(all_tables)
    finally:
        conn.close()


def test_metadata_ddl_editor_schema_payload_integration():
    dsn = os.environ.get("SCRATCHBIRD_TEST_DSN")
    if not dsn:
        pytest.skip("SCRATCHBIRD_TEST_DSN not set")
    conn = scratchbird.connect(dsn)
    try:
        payload = conn.ddl_editor_schema_payload(schema_pattern="%")
        assert set(payload) == {"schemaPattern", "expandSchemaParents", "schemaPaths", "schemaTree"}
        assert payload["schemaPattern"] == "%"
        assert isinstance(payload["schemaPaths"], list)
        assert isinstance(payload["schemaTree"], list)
    finally:
        conn.close()
