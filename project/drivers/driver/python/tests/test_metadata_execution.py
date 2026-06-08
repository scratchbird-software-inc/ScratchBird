# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pytest

from scratchbird import errors
from scratchbird.connection import Connection
from scratchbird.metadata import (
    CATALOGS_QUERY,
    COLUMN_PRIVILEGES_QUERY,
    CONSTRAINTS_QUERY,
    FOREIGN_KEYS_QUERY,
    FUNCTIONS_QUERY,
    PRIMARY_KEYS_QUERY,
    PROCEDURES_QUERY,
    ROUTINES_QUERY,
    filter_rows_for_collection_family,
    TABLE_PRIVILEGES_QUERY,
    TYPE_INFO_QUERY,
    filter_rows_by_restrictions,
    normalize_collection_name,
    resolve_collection_query,
)


@pytest.mark.parametrize(
    ("alias", "expected"),
    [
        ("table", "tables"),
        ("schemas", "schemas"),
        ("catalog", "catalogs"),
        ("primarykeys", "primary_keys"),
        ("foreign_keys", "foreign_keys"),
        ("tablePrivileges", "table_privileges"),
        ("column-privileges", "column_privileges"),
        ("type info", "type_info"),
    ],
)
def test_normalize_collection_name_aliases(alias: str, expected: str):
    assert normalize_collection_name(alias) == expected


@pytest.mark.parametrize(
    ("collection", "query"),
    [
        ("catalogs", CATALOGS_QUERY),
        ("primary_keys", PRIMARY_KEYS_QUERY),
        ("foreignkey", FOREIGN_KEYS_QUERY),
        ("procedures", PROCEDURES_QUERY),
        ("functions", FUNCTIONS_QUERY),
        ("routines", ROUTINES_QUERY),
        ("tableprivileges", TABLE_PRIVILEGES_QUERY),
        ("column_privileges", COLUMN_PRIVILEGES_QUERY),
        ("typeinfo", TYPE_INFO_QUERY),
    ],
)
def test_resolve_collection_query_extended_families(collection: str, query: str):
    assert resolve_collection_query(collection) == query


def test_resolve_collection_query_rejects_unknown_collection():
    with pytest.raises(ValueError, match="not supported"):
        resolve_collection_query("unsupported_collection")


def test_filter_rows_for_collection_family_narrows_view_backed_metadata():
    assert filter_rows_for_collection_family(
        [
            {"CONSTRAINT_NAME": "pk_events", "CONSTRAINT_TYPE": "PRIMARY KEY"},
            {"CONSTRAINT_NAME": "fk_events_users", "CONSTRAINT_TYPE": "FOREIGN KEY"},
        ],
        "primary_keys",
    ) == [{"CONSTRAINT_NAME": "pk_events", "CONSTRAINT_TYPE": "PRIMARY KEY"}]
    assert filter_rows_for_collection_family(
        [
            {"ROUTINE_NAME": "upsert_event", "ROUTINE_TYPE": "PROCEDURE"},
            {"ROUTINE_NAME": "event_count", "ROUTINE_TYPE": "FUNCTION"},
        ],
        "functions",
    ) == [{"ROUTINE_NAME": "event_count", "ROUTINE_TYPE": "FUNCTION"}]


def test_connection_query_metadata_executes_resolved_sql():
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    class DummyCursor:
        pass

    expected_cursor = DummyCursor()

    def fake_execute(sql: str, params=None):
        captured["sql"] = sql
        captured["params"] = params
        return expected_cursor

    conn.execute = fake_execute

    actual = Connection.query_metadata(conn, "constraints")
    assert actual is expected_cursor
    assert captured["sql"] == CONSTRAINTS_QUERY
    assert captured["params"] is None


@pytest.mark.parametrize(
    ("collection_name", "restrictions", "expected_sql", "rows", "expected_row"),
    [
        (
            "procedures",
            {"schema": "users", "procedure": "upsert_event"},
            PROCEDURES_QUERY,
            [
                {"ROUTINE_SCHEMA": "users", "ROUTINE_NAME": "upsert_event", "ROUTINE_TYPE": "PROCEDURE"},
                {"ROUTINE_SCHEMA": "sys", "ROUTINE_NAME": "other_proc", "ROUTINE_TYPE": "PROCEDURE"},
            ],
            [{"ROUTINE_SCHEMA": "users", "ROUTINE_NAME": "upsert_event", "ROUTINE_TYPE": "PROCEDURE"}],
        ),
        (
            "functions",
            {"schema": "users", "function": "event_count"},
            FUNCTIONS_QUERY,
            [
                {"ROUTINE_SCHEMA": "users", "ROUTINE_NAME": "event_count", "ROUTINE_TYPE": "FUNCTION"},
                {"ROUTINE_SCHEMA": "sys", "ROUTINE_NAME": "other_fn", "ROUTINE_TYPE": "FUNCTION"},
            ],
            [{"ROUTINE_SCHEMA": "users", "ROUTINE_NAME": "event_count", "ROUTINE_TYPE": "FUNCTION"}],
        ),
        (
            "routines",
            {"schema": "users", "routine": "event_count"},
            ROUTINES_QUERY,
            [
                {"schema_name": "users", "routine_name": "event_count"},
                {"schema_name": "sys", "routine_name": "other_routine"},
            ],
            [{"schema_name": "users", "routine_name": "event_count"}],
        ),
    ],
)
def test_connection_query_metadata_executes_routine_queries_with_restrictions(
    collection_name: str,
    restrictions: dict,
    expected_sql: str,
    rows: list,
    expected_row: list,
):
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    class DummyCursor:
        description = []
        statusmessage = "SELECT"
        lastrowid = None

        def fetchall(self):
            return list(rows)

    def fake_execute(sql: str, params=None):
        captured["sql"] = sql
        captured["params"] = params
        return DummyCursor()

    conn.execute = fake_execute

    actual = Connection.query_metadata(conn, collection_name, restrictions=restrictions)
    assert captured == {"sql": expected_sql, "params": None}
    assert actual.fetchall() == expected_row


@pytest.mark.parametrize(
    ("collection_name", "rows", "expected_rows"),
    [
        (
            "primary_keys",
            [
                {"CONSTRAINT_NAME": "pk_events", "CONSTRAINT_TYPE": "PRIMARY KEY"},
                {"CONSTRAINT_NAME": "fk_events_users", "CONSTRAINT_TYPE": "FOREIGN KEY"},
            ],
            [{"CONSTRAINT_NAME": "pk_events", "CONSTRAINT_TYPE": "PRIMARY KEY"}],
        ),
        (
            "functions",
            [
                {"ROUTINE_NAME": "upsert_event", "ROUTINE_TYPE": "PROCEDURE"},
                {"ROUTINE_NAME": "event_count", "ROUTINE_TYPE": "FUNCTION"},
            ],
            [{"ROUTINE_NAME": "event_count", "ROUTINE_TYPE": "FUNCTION"}],
        ),
    ],
)
def test_connection_query_metadata_applies_client_side_family_filters(
    collection_name: str,
    rows: list,
    expected_rows: list,
):
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    class DummyCursor:
        description = []
        statusmessage = "SELECT"
        lastrowid = None

        def fetchall(self):
            return list(rows)

    def fake_execute(sql: str, params=None):
        captured["sql"] = sql
        captured["params"] = params
        return DummyCursor()

    conn.execute = fake_execute

    actual = Connection.query_metadata(conn, collection_name)
    assert captured["params"] is None
    assert actual.fetchall() == expected_rows


def test_connection_get_schema_drains_cursor_rows():
    conn = Connection.__new__(Connection)
    conn._closed = False

    rows = [{"catalog_name": "sys"}, {"catalog_name": "users"}]

    class DummyCursor:
        def fetchall(self):
            return rows

    def fake_execute(sql: str, params=None):
        assert sql == CATALOGS_QUERY
        return DummyCursor()

    conn.execute = fake_execute

    actual_rows = Connection.get_schema(conn, "catalog")
    assert actual_rows == rows


def test_connection_query_metadata_maps_unsupported_collection_to_not_supported():
    conn = Connection.__new__(Connection)
    conn._closed = False
    conn.execute = lambda *_args, **_kwargs: None

    with pytest.raises(errors.NotSupportedError, match="not supported"):
        Connection.query_metadata(conn, "nope")


def test_connection_get_schema_forwards_restrictions():
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    class DummyCursor:
        def fetchall(self):
            return [("users",)]

    def fake_query_metadata(collection_name="tables", restrictions=None):
        captured["collection_name"] = collection_name
        captured["restrictions"] = restrictions
        return DummyCursor()

    conn.query_metadata = fake_query_metadata

    rows = Connection.get_schema(conn, "schemas", restrictions={"schema": "users"})
    assert rows == [("users",)]
    assert captured == {"collection_name": "schemas", "restrictions": {"schema": "users"}}


def test_connection_schemas_wrapper_forwards_catalog_restriction():
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    def fake_get_schema(collection_name="tables", restrictions=None):
        captured["collection_name"] = collection_name
        captured["restrictions"] = restrictions
        return [("users",)]

    conn.get_schema = fake_get_schema

    rows = Connection.schemas(conn, catalog="main")
    assert rows == [("users",)]
    assert captured == {"collection_name": "schemas", "restrictions": {"catalog": "main"}}


def test_connection_tables_wrapper_forwards_restrictions():
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    def fake_get_schema(collection_name="tables", restrictions=None):
        captured["collection_name"] = collection_name
        captured["restrictions"] = restrictions
        return [("events",)]

    conn.get_schema = fake_get_schema

    rows = Connection.tables(conn, schema="users", table="events", table_type="BASE TABLE")
    assert rows == [("events",)]
    assert captured == {
        "collection_name": "tables",
        "restrictions": {"schema": "users", "table": "events", "type": "BASE TABLE"},
    }


def test_connection_columns_wrapper_forwards_restrictions():
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    def fake_get_schema(collection_name="tables", restrictions=None):
        captured["collection_name"] = collection_name
        captured["restrictions"] = restrictions
        return [("column",)]

    conn.get_schema = fake_get_schema

    rows = Connection.columns(conn, schema="users", table="events", column="event_id", column_type="INTEGER")
    assert rows == [("column",)]
    assert captured == {
        "collection_name": "columns",
        "restrictions": {"schema": "users", "table": "events", "column": "event_id", "type": "INTEGER"},
    }


def test_connection_indexes_wrapper_handles_missing_restrictions():
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    def fake_get_schema(collection_name="tables", restrictions=None):
        captured["collection_name"] = collection_name
        captured["restrictions"] = restrictions
        return [("idx",)]

    conn.get_schema = fake_get_schema

    rows = Connection.indexes(conn)
    assert rows == [("idx",)]
    assert captured == {"collection_name": "indexes", "restrictions": None}


def test_connection_ddl_editor_schema_payload_uses_config_parent_expansion_and_forwards_pattern():
    conn = Connection.__new__(Connection)
    conn._closed = False
    conn._config = type("Cfg", (), {"metadata_expand_schema_parents": True})()
    captured = {}

    class DummyCursor:
        def __init__(self):
            self.description = [("schema_name", None, None, None, None, None, True)]

        def fetchall(self):
            return [("users.alice.dev",), ("sys",)]

    def fake_query_metadata(collection_name="tables", restrictions=None):
        captured["collection_name"] = collection_name
        captured["restrictions"] = restrictions
        return DummyCursor()

    conn.query_metadata = fake_query_metadata

    payload = Connection.ddl_editor_schema_payload(conn, schema_pattern="users.%")
    assert captured == {"collection_name": "schemas", "restrictions": {"schema": "users.%"}}
    assert payload["expandSchemaParents"] is True
    assert payload["schemaPaths"] == ["users", "users.alice", "users.alice.dev"]


def test_connection_ddl_editor_schema_payload_allows_expansion_override():
    conn = Connection.__new__(Connection)
    conn._closed = False
    conn._config = type("Cfg", (), {"metadata_expand_schema_parents": True})()

    class DummyCursor:
        description = [("schema_name", None, None, None, None, None, True)]

        def fetchall(self):
            return [("users.alice.dev",)]

    conn.query_metadata = lambda *_args, **_kwargs: DummyCursor()

    payload = Connection.ddl_editor_schema_payload(conn, expand_schema_parents=False)
    assert payload["expandSchemaParents"] is False
    assert payload["schemaPaths"] == ["users.alice.dev"]


@pytest.mark.parametrize(
    ("method_name", "kwargs", "expected_collection", "expected_restrictions"),
    [
        (
            "index_columns",
            {"schema": "users", "table": "events", "index": "idx_events", "column": "event_id"},
            "index_columns",
            {"schema": "users", "table": "events", "index": "idx_events", "column": "event_id"},
        ),
        (
            "constraints",
            {"schema": "users", "table": "events", "constraint": "events_pk"},
            "constraints",
            {"schema": "users", "table": "events", "constraint": "events_pk"},
        ),
        ("catalogs", {}, "catalogs", None),
        ("catalogs", {"catalog": "main"}, "catalogs", {"catalog": "main"}),
        (
            "primary_keys",
            {"catalog": "main", "schema": "users", "table": "events", "constraint": "events_pk"},
            "primary_keys",
            {"catalog": "main", "schema": "users", "table": "events", "constraint": "events_pk"},
        ),
        (
            "foreign_keys",
            {"schema": "users", "table": "events"},
            "foreign_keys",
            {"schema": "users", "table": "events"},
        ),
        (
            "procedures",
            {"schema": "users", "procedure": "upsert_event"},
            "procedures",
            {"schema": "users", "procedure": "upsert_event"},
        ),
        (
            "functions",
            {"catalog": "main", "schema": "users", "function": "event_count"},
            "functions",
            {"catalog": "main", "schema": "users", "function": "event_count"},
        ),
        (
            "routines",
            {"schema": "users", "routine": "event_count"},
            "routines",
            {"schema": "users", "routine": "event_count"},
        ),
        (
            "table_privileges",
            {"schema": "users", "table": "events"},
            "table_privileges",
            {"schema": "users", "table": "events"},
        ),
        (
            "column_privileges",
            {"schema": "users", "table": "events", "column": "event_id"},
            "column_privileges",
            {"schema": "users", "table": "events", "column": "event_id"},
        ),
        ("type_info", {}, "type_info", None),
        ("type_info", {"type_name": "INTEGER"}, "type_info", {"type": "INTEGER"}),
    ],
)
def test_connection_metadata_wrappers_forward_expected_restrictions(
    method_name: str,
    kwargs: dict,
    expected_collection: str,
    expected_restrictions: dict | None,
):
    conn = Connection.__new__(Connection)
    conn._closed = False
    captured = {}

    def fake_get_schema(collection_name="tables", restrictions=None):
        captured["collection_name"] = collection_name
        captured["restrictions"] = restrictions
        return [("ok",)]

    conn.get_schema = fake_get_schema

    rows = getattr(Connection, method_name)(conn, **kwargs)
    assert rows == [("ok",)]
    assert captured == {"collection_name": expected_collection, "restrictions": expected_restrictions}


def test_filter_rows_by_restrictions_filters_mapping_rows_with_aliases():
    rows = [
        {"schema_name": "sys", "table_name": "events"},
        {"schema_name": "users", "table_name": "events"},
        {"schema_name": "users", "table_name": "profiles"},
    ]

    filtered = filter_rows_by_restrictions(
        rows,
        {"schema": "users", "table": "events"},
        collection_name="tables",
    )
    assert filtered == [{"schema_name": "users", "table_name": "events"}]


def test_filter_rows_by_restrictions_supports_jdbc_wildcard_patterns():
    rows = [
        {"schema_name": "users", "table_name": "events"},
        {"schema_name": "users.dev", "table_name": "events"},
        {"schema_name": "sys", "table_name": "events"},
        {"schema_name": "users", "table_name": "logs"},
    ]

    filtered = filter_rows_by_restrictions(
        rows,
        {"schema": "us%", "table": "eve_ts"},
        collection_name="tables",
    )
    assert filtered == [
        {"schema_name": "users", "table_name": "events"},
        {"schema_name": "users.dev", "table_name": "events"},
    ]


def test_filter_rows_by_restrictions_supports_escaped_wildcards():
    rows = [
        {"table_name": "ev%nts"},
        {"table_name": "events"},
    ]

    filtered = filter_rows_by_restrictions(
        rows,
        {"table": r"ev\%nts"},
        collection_name="tables",
    )
    assert filtered == [{"table_name": "ev%nts"}]


def test_filter_rows_by_restrictions_applies_wildcards_to_tuple_rows():
    rows = [
        ("users", "events"),
        ("sys", "events"),
        ("users", "logs"),
    ]
    column_names = ["schema_name", "table_name"]

    filtered = filter_rows_by_restrictions(
        rows,
        {"schema": "us%", "table": "eve%"},
        collection_name="tables",
        column_names=column_names,
    )
    assert filtered == [("users", "events")]


def test_connection_query_metadata_with_restrictions_filters_tuple_rows_from_description():
    conn = Connection.__new__(Connection)
    conn._closed = False

    rows = [
        ("sys", "events", "PRIMARY KEY"),
        ("users", "events", "PRIMARY KEY"),
        ("users", "profiles", "PRIMARY KEY"),
    ]
    description = [
        ("schema_name", None, None, None, None, None, True),
        ("table_name", None, None, None, None, None, True),
        ("constraint_type", None, None, None, None, None, True),
    ]

    class DummyCursor:
        def __init__(self):
            self.description = description
            self.statusmessage = "SELECT"
            self.lastrowid = None

        def fetchall(self):
            return list(rows)

    def fake_execute(sql: str, params=None):
        assert sql == PRIMARY_KEYS_QUERY
        assert params is None
        return DummyCursor()

    conn.execute = fake_execute

    actual = Connection.query_metadata(conn, "primary_keys", restrictions={"schema": "users"})
    assert actual.description == description
    assert actual.fetchall() == [
        ("users", "events", "PRIMARY KEY"),
        ("users", "profiles", "PRIMARY KEY"),
    ]


def test_connection_query_metadata_with_restrictions_supports_null_and_ignores_unknown_keys():
    conn = Connection.__new__(Connection)
    conn._closed = False

    rows = [{"table_name": "events", "owner_id": None}, {"table_name": "events", "owner_id": 7}]

    class DummyCursor:
        description = [("table_name", None, None, None, None, None, True), ("owner_id", None, None, None, None, None, True)]
        statusmessage = "SELECT"
        lastrowid = None

        def fetchall(self):
            return list(rows)

    conn.execute = lambda *_args, **_kwargs: DummyCursor()

    actual = Connection.query_metadata(
        conn,
        "tables",
        restrictions={"owner_id": "null", "missing_filter": "ignored"},
    )
    assert actual.fetchall() == [{"table_name": "events", "owner_id": None}]


def test_connection_query_metadata_with_restrictions_rejects_non_mapping():
    conn = Connection.__new__(Connection)
    conn._closed = False

    class DummyCursor:
        description = []
        statusmessage = "SELECT"
        lastrowid = None

        def fetchall(self):
            return []

    conn.execute = lambda *_args, **_kwargs: DummyCursor()

    with pytest.raises(errors.ProgrammingError, match="mapping"):
        Connection.query_metadata(conn, "tables", restrictions=["not", "a", "mapping"])
