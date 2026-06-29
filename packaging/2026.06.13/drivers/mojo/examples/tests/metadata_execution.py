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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

import scratchbird


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


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
        self.calls.append(("send", msg_type, payload))

    def _read_resultset(self):
        self.calls.append(("read",))
        return self.result


def test_normalize_metadata_collection_aliases() -> None:
    _require(scratchbird.normalize_metadata_collection_name("catalog") == "catalogs", "catalog alias mismatch")
    _require(scratchbird.normalize_metadata_collection_name("primaryKeys") == "primary_keys", "primary key alias mismatch")
    _require(scratchbird.normalize_metadata_collection_name("foreign-keys") == "foreign_keys", "foreign key alias mismatch")
    _require(
        scratchbird.normalize_metadata_collection_name("table privileges") == "table_privileges",
        "table privileges alias mismatch",
    )
    _require(
        scratchbird.normalize_metadata_collection_name("columnprivilege") == "column_privileges",
        "column privileges alias mismatch",
    )
    _require(scratchbird.normalize_metadata_collection_name("typeinfo") == "type_info", "type info alias mismatch")


def test_resolve_metadata_collection_query_extended_families() -> None:
    _require(
        scratchbird.resolve_metadata_collection_query("catalogs") == scratchbird.METADATA_CATALOGS_QUERY,
        "catalog query mismatch",
    )
    _require(
        scratchbird.resolve_metadata_collection_query("primarykeys") == scratchbird.METADATA_PRIMARY_KEYS_QUERY,
        "primary keys query mismatch",
    )
    _require(
        scratchbird.resolve_metadata_collection_query("foreign_keys") == scratchbird.METADATA_FOREIGN_KEYS_QUERY,
        "foreign keys query mismatch",
    )
    _require(
        scratchbird.resolve_metadata_collection_query("tableprivileges")
        == scratchbird.METADATA_TABLE_PRIVILEGES_QUERY,
        "table privileges query mismatch",
    )
    _require(
        scratchbird.resolve_metadata_collection_query("column_privileges")
        == scratchbird.METADATA_COLUMN_PRIVILEGES_QUERY,
        "column privileges query mismatch",
    )
    _require(
        scratchbird.resolve_metadata_collection_query("type_info") == scratchbird.METADATA_TYPE_INFO_QUERY,
        "type info query mismatch",
    )


def test_normalize_metadata_restriction_aliases() -> None:
    _require(scratchbird.normalize_metadata_restriction_key("TABLE_CAT") == "catalog_name", "catalog alias mismatch")
    _require(
        scratchbird.normalize_metadata_restriction_key("tableCatalog") == "catalog_name",
        "tableCatalog alias mismatch",
    )
    _require(scratchbird.normalize_metadata_restriction_key("TABLE_SCHEM") == "schema_name", "schema alias mismatch")
    _require(
        scratchbird.normalize_metadata_restriction_key("tableSchem") == "schema_name",
        "tableSchem alias mismatch",
    )
    _require(scratchbird.normalize_metadata_restriction_key("column") == "column_name", "column alias mismatch")
    _require(
        scratchbird.normalize_metadata_restriction_key("columnName") == "column_name",
        "columnName alias mismatch",
    )
    _require(scratchbird.normalize_metadata_restriction_key("index") == "index_name", "index alias mismatch")
    _require(
        scratchbird.normalize_metadata_restriction_key("indexName") == "index_name",
        "indexName alias mismatch",
    )
    _require(
        scratchbird.normalize_metadata_restriction_key("constraint") == "constraint_name",
        "constraint alias mismatch",
    )
    _require(
        scratchbird.normalize_metadata_restriction_key("constraintName") == "constraint_name",
        "constraintName alias mismatch",
    )
    _require(scratchbird.normalize_metadata_restriction_key("routine") == "routine_name", "routine alias mismatch")
    _require(
        scratchbird.normalize_metadata_restriction_key("functionName") == "routine_name",
        "functionName alias mismatch",
    )
    _require(scratchbird.normalize_metadata_restriction_key("udt_name") == "type_name", "type alias mismatch")
    _require(
        scratchbird.normalize_metadata_restriction_key("dataTypeName") == "type_name",
        "dataTypeName alias mismatch",
    )
    _require(scratchbird.normalize_metadata_restriction_key("none") == "", "none restriction should normalize empty")
    try:
        scratchbird.normalize_metadata_restriction_key("unsupported_restriction")
        raise RuntimeError("expected unsupported restriction failure")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "0A000", "unsupported restriction should map to 0A000")


def test_resolve_metadata_collection_query_restricted() -> None:
    _require(
        scratchbird.resolve_metadata_collection_query_restricted("table", "name", "orders")
        == "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 AND table_name = 'orders' ORDER BY table_name",
        "restricted table query mismatch",
    )
    _require(
        "table_name LIKE 'ord%'"
        in scratchbird.resolve_metadata_collection_query_restricted("table", "table", "ord%"),
        "table wildcard restriction should use LIKE predicate",
    )
    _require(
        "table_name LIKE 'ord\\%' ESCAPE '\\'"
        in scratchbird.resolve_metadata_collection_query_restricted("table", "table", r"ord\%"),
        "escaped wildcard restriction should preserve ESCAPE semantics",
    )
    _require(
        "schema_name = 'acme''schema'"
        in scratchbird.resolve_metadata_collection_query_restricted("schema", "schema", "acme'schema"),
        "restricted schema query should escape SQL literals",
    )
    _require(
        "schema_name IS NULL"
        in scratchbird.resolve_metadata_collection_query_restricted("schema", "schema", "null"),
        "null restriction value should map to IS NULL predicate",
    )
    _require(
        "table_id IN (SELECT t.table_id FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE s.schema_name = 'public')"
        in scratchbird.resolve_metadata_collection_query_restricted("columns", "schema", "public"),
        "columns schema restriction should map through table-schema subquery",
    )
    _require(
        "schema_id IN (SELECT schema_id FROM sys.schemas WHERE schema_name = 'public')"
        in scratchbird.resolve_metadata_collection_query_restricted("tables", "catalog", "public"),
        "catalog restriction should normalize through schema predicate",
    )
    _require(
        "s.schema_name LIKE 'pub%'"
        in scratchbird.resolve_metadata_collection_query_restricted("columns", "schema", "pub%"),
        "columns schema wildcard restriction should use LIKE predicate",
    )
    _require(
        "s.schema_name LIKE 'pub\\_%' ESCAPE '\\'"
        in scratchbird.resolve_metadata_collection_query_restricted("columns", "schema", r"pub\_%"),
        "columns escaped wildcard restriction should preserve ESCAPE semantics",
    )
    _require(
        "index_id IN (SELECT i.index_id FROM sys.indexes i JOIN sys.tables t ON t.table_id = i.table_id WHERE t.table_name = 'orders')"
        in scratchbird.resolve_metadata_collection_query_restricted("index_columns", "table", "orders"),
        "index_columns table restriction should map through index-table subquery",
    )
    _require(
        "index_id IN (SELECT index_id FROM sys.indexes WHERE index_name LIKE 'idx_orders' ESCAPE '\\')"
        in scratchbird.resolve_metadata_collection_query_restricted("index_columns", "index", "idx_orders"),
        "index_columns index restriction should map through index-name subquery",
    )
    _require(
        "constraint_name LIKE 'orders_pk' ESCAPE '\\'"
        in scratchbird.resolve_metadata_collection_query_restricted("constraints", "constraint", "orders_pk"),
        "constraint restriction should target constraint_name",
    )
    _require(
        "routine_name LIKE 'orders_upsert' ESCAPE '\\'"
        in scratchbird.resolve_metadata_collection_query_restricted("routines", "routine", "orders_upsert"),
        "routine restriction should target routine_name",
    )
    _require(
        "data_type_name = 'INTEGER'"
        in scratchbird.resolve_metadata_collection_query_restricted("columns", "type", "INTEGER"),
        "type restriction should target data_type_name",
    )
    _require(
        scratchbird.resolve_metadata_collection_query_restricted("tables", "table_name", "")
        == scratchbird.METADATA_TABLES_QUERY,
        "empty restriction value should not mutate query",
    )
    try:
        scratchbird.resolve_metadata_collection_query_restricted("tables", "column", "id")
        raise RuntimeError("expected unsupported collection restriction failure")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "0A000", "unsupported collection restriction should map to 0A000")
    try:
        scratchbird.resolve_metadata_collection_query_restricted("tables", "index", "idx_orders")
        raise RuntimeError("expected unsupported index restriction for tables")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "0A000", "unsupported index restriction should map to 0A000")


def test_resolve_metadata_collection_query_restricted_multi() -> None:
    sql = scratchbird.resolve_metadata_collection_query_restricted_multi(
        "tables",
        {"schema": "public", "table": "ord%"},
    )
    _require(
        "schema_id IN (SELECT schema_id FROM sys.schemas WHERE schema_name = 'public')" in sql,
        "multi restriction SQL should include schema predicate",
    )
    _require(
        "table_name LIKE 'ord%'" in sql,
        "multi restriction SQL should include wildcard table predicate",
    )
    _require(
        "table_name LIKE 'ord\\_%' ESCAPE '\\'"
        in scratchbird.resolve_metadata_collection_query_restricted_multi(
            "tables",
            {"table": r"ord\_%"},
        ),
        "multi restriction SQL should keep escaped wildcard semantics",
    )
    _require(
        "schema_id IN (SELECT schema_id FROM sys.schemas WHERE schema_name IS NULL)"
        in scratchbird.resolve_metadata_collection_query_restricted_multi(
            "tables",
            {"schema": "null"},
        ),
        "multi restriction SQL should support IS NULL schema predicates",
    )
    _require(
        "data_type_name = 'INTEGER'"
        in scratchbird.resolve_metadata_collection_query_restricted_multi(
            "columns",
            {"catalog": "public", "table": "orders", "type_name": "INTEGER"},
        ),
        "multi restriction SQL should support type-name predicates",
    )
    sql_duplicate_aliases = scratchbird.resolve_metadata_collection_query_restricted_multi(
        "tables",
        {"table": "orders", "tableName": "customers"},
    )
    _require(
        "table_name = 'customers'" in sql_duplicate_aliases,
        "multi restriction SQL should keep last duplicate-alias table predicate",
    )
    _require(
        "table_name = 'orders'" not in sql_duplicate_aliases,
        "multi restriction SQL should drop overridden duplicate-alias table predicate",
    )
    sql_duplicate_empty = scratchbird.resolve_metadata_collection_query_restricted_multi(
        "tables",
        {"table": "orders", "table_name": ""},
    )
    _require(
        "table_name = 'orders'" not in sql_duplicate_empty,
        "multi restriction SQL should drop table predicate when last duplicate-alias value is empty",
    )
    _require(
        sql_duplicate_empty == scratchbird.METADATA_TABLES_QUERY,
        "multi restriction SQL should remain base query when last duplicate-alias value is empty",
    )
    _require(
        "schema_id IN (SELECT schema_id FROM sys.schemas WHERE schema_name = 'public')"
        in scratchbird.resolve_metadata_collection_query_restricted_multi(
            "tables",
            {"tableSchem": "public", "tableName": "orders"},
        ),
        "multi restriction SQL should support camel/collapsed schema aliases",
    )
    _require(
        "table_name = 'orders'"
        in scratchbird.resolve_metadata_collection_query_restricted_multi(
            "tables",
            {"tableSchem": "public", "tableName": "orders"},
        ),
        "multi restriction SQL should support camel/collapsed table aliases",
    )
    try:
        scratchbird.resolve_metadata_collection_query_restricted_multi(
            "tables",
            ["not", "a", "mapping"],
        )
        raise RuntimeError("expected metadata restriction mapping failure")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "22023", "non-mapping restrictions should map to 22023")


def test_query_metadata_routes_through_query_path() -> None:
    conn = QueryHarness()
    result = scratchbird.ScratchBirdConnection.query_metadata(conn, "primarykeys")
    _require(result is conn.result, "query_metadata should return harness result")
    _require(
        any(call[0] == "send" and call[1] == scratchbird.MessageType.QUERY for call in conn.calls),
        "query path should send QUERY",
    )
    _require(("read",) in conn.calls, "query path should read resultset")

    sent_payload = None
    for call in conn.calls:
        if call[0] == "send" and call[1] == scratchbird.MessageType.QUERY:
            sent_payload = call[2]
            break
    _require(sent_payload is not None, "query_metadata should send QUERY payload")
    _require(
        sent_payload.decode("utf-8") == scratchbird.METADATA_PRIMARY_KEYS_QUERY,
        "query_metadata should route primary key query SQL",
    )


def test_query_metadata_restricted_routes_through_query_path() -> None:
    conn = QueryHarness()
    result = scratchbird.ScratchBirdConnection.query_metadata_restricted(conn, "table", "name", "orders")
    _require(result is conn.result, "query_metadata_restricted should return harness result")
    sent_payload = None
    for call in conn.calls:
        if call[0] == "send" and call[1] == scratchbird.MessageType.QUERY:
            sent_payload = call[2]
            break
    _require(sent_payload is not None, "query_metadata_restricted should send QUERY payload")
    _require(
        sent_payload.decode("utf-8")
        == "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 AND table_name = 'orders' ORDER BY table_name",
        "query_metadata_restricted should route restricted query SQL",
    )


def test_query_metadata_restricted_multi_routes_through_query_path() -> None:
    conn = QueryHarness()
    result = scratchbird.ScratchBirdConnection.query_metadata_restricted_multi(
        conn,
        "tables",
        {"schema": "public", "table": "orders"},
    )
    _require(result is conn.result, "query_metadata_restricted_multi should return harness result")
    sent_payload = None
    for call in conn.calls:
        if call[0] == "send" and call[1] == scratchbird.MessageType.QUERY:
            sent_payload = call[2]
            break
    _require(sent_payload is not None, "query_metadata_restricted_multi should send QUERY payload")
    sent_sql = sent_payload.decode("utf-8")
    _require(
        "schema_id IN (SELECT schema_id FROM sys.schemas WHERE schema_name = 'public')" in sent_sql,
        "query_metadata_restricted_multi should include schema predicate",
    )
    _require(
        "table_name = 'orders'" in sent_sql,
        "query_metadata_restricted_multi should include table predicate",
    )


def test_get_schema_returns_result_rows() -> None:
    conn = QueryHarness()
    conn.result = scratchbird.ScratchBirdResult([[7], [9]], [], 2)
    rows = scratchbird.ScratchBirdConnection.get_schema(conn, "catalog")
    _require(rows == [[7], [9]], "get_schema should return result rows")


def test_query_metadata_rows_returns_rowcount() -> None:
    conn = QueryHarness()
    conn.result = scratchbird.ScratchBirdResult([[1], [2], [3]], [], 3)
    rowcount = scratchbird.ScratchBirdConnection.query_metadata_rows(conn, "table")
    _require(rowcount == 3, "query_metadata_rows should return result rowcount")


def test_query_metadata_rows_restricted_returns_rowcount() -> None:
    conn = QueryHarness()
    conn.result = scratchbird.ScratchBirdResult([[1], [2], [3], [4]], [], 4)
    rowcount = scratchbird.ScratchBirdConnection.query_metadata_rows_restricted(
        conn,
        "routines",
        "schema",
        "public",
    )
    _require(rowcount == 4, "query_metadata_rows_restricted should return result rowcount")


def test_query_metadata_rows_restricted_multi_returns_rowcount() -> None:
    conn = QueryHarness()
    conn.result = scratchbird.ScratchBirdResult([[1], [2], [3], [4], [5]], [], 5)
    rowcount = scratchbird.ScratchBirdConnection.query_metadata_rows_restricted_multi(
        conn,
        "tables",
        {"schema": "public", "table": "orders"},
    )
    _require(rowcount == 5, "query_metadata_rows_restricted_multi should return result rowcount")


def test_connection_ddl_editor_schema_payload_applies_schema_pattern() -> None:
    conn = QueryHarness()
    conn.result = scratchbird.ScratchBirdResult(
        [
            {"schema_name": "users.alice.dev"},
            {"schema_name": "users.bob.dev"},
        ],
        [],
        2,
    )
    payload = scratchbird.ScratchBirdConnection.ddl_editor_schema_payload(
        conn,
        schema_pattern="users.%",
        expand_schema_parents=True,
    )
    _require(payload["schemaPattern"] == "users.%", "ddl payload schemaPattern mismatch")
    _require(payload["expandSchemaParents"] is True, "ddl payload expandSchemaParents mismatch")
    _require(payload["schemaPaths"] == ["users", "users.alice", "users.alice.dev", "users.bob", "users.bob.dev"], "ddl payload schemaPaths mismatch")
    sent_payload = None
    for call in conn.calls:
        if call[0] == "send" and call[1] == scratchbird.MessageType.QUERY:
            sent_payload = call[2]
            break
    _require(sent_payload is not None, "ddl payload path should route through metadata query")
    sent_sql = sent_payload.decode("utf-8")
    _require(
        "schema_name LIKE 'users.%' ESCAPE '\\'" in sent_sql,
        "ddl payload path should apply schema wildcard restriction",
    )


def test_shim_schema_metadata_supports_is_null_predicate() -> None:
    cfg = scratchbird.ScratchBirdConfig("scratchbird://user:pass@localhost:3092/testdb?sslmode=require")
    conn = scratchbird.connect(cfg)
    result = conn.query_metadata_restricted("schemas", "schema", "null")
    _require(result.rowcount == 1, "null schema restriction should return deterministic null schema row")
    _require(result.rows == [{"schema_name": None}], "null schema restriction should return null schema payload")


def test_sql_like_match_supports_escape_and_case_insensitive_semantics() -> None:
    _require(
        scratchbird._sql_like_match("users.alice.dev", "users.%"),
        "sql like helper should match wildcard patterns",
    )
    _require(
        scratchbird._sql_like_match("Users.Alice.Dev", "users.%"),
        "sql like helper should match case-insensitively",
    )
    _require(
        scratchbird._sql_like_match("ord%ers", r"ord\%ers"),
        "sql like helper should respect escaped percent wildcards",
    )
    _require(
        scratchbird._sql_like_match("ord_ers", r"ord\_ers"),
        "sql like helper should respect escaped underscore wildcards",
    )
    _require(
        not scratchbird._sql_like_match("orders", r"ord\%ers"),
        "escaped wildcard should not match non-literal characters",
    )


def test_query_metadata_rejects_unsupported_collection() -> None:
    conn = QueryHarness()
    try:
        scratchbird.ScratchBirdConnection.query_metadata(conn, "unsupported_collection")
        raise RuntimeError("expected unsupported collection failure")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "0A000", "unsupported collection should map to 0A000")


def main() -> None:
    test_normalize_metadata_collection_aliases()
    test_resolve_metadata_collection_query_extended_families()
    test_normalize_metadata_restriction_aliases()
    test_resolve_metadata_collection_query_restricted()
    test_resolve_metadata_collection_query_restricted_multi()
    test_query_metadata_routes_through_query_path()
    test_query_metadata_restricted_routes_through_query_path()
    test_query_metadata_restricted_multi_routes_through_query_path()
    test_get_schema_returns_result_rows()
    test_query_metadata_rows_returns_rowcount()
    test_query_metadata_rows_restricted_returns_rowcount()
    test_query_metadata_rows_restricted_multi_returns_rowcount()
    test_connection_ddl_editor_schema_payload_applies_schema_pattern()
    test_shim_schema_metadata_supports_is_null_predicate()
    test_sql_like_match_supports_escape_and_case_insensitive_semantics()
    test_query_metadata_rejects_unsupported_collection()
    print("Mojo metadata execution tests OK")


if __name__ == "__main__":
    main()
