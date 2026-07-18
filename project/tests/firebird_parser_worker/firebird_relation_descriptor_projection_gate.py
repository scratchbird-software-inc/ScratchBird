#!/usr/bin/env python3
"""Guard the standalone Firebird SBPS V3 persisted-descriptor consumer."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    require(start >= 0, f"missing function signature: {signature}")
    opening = source.find("{", start)
    require(opening >= 0, f"missing function body: {signature}")
    depth = 0
    for offset in range(opening, len(source)):
        char = source[offset]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening : offset + 1]
    raise AssertionError(f"unterminated function body: {signature}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker-source", required=True)
    parser.add_argument("--execution-header", required=True)
    parser.add_argument("--execution-source", required=True)
    args = parser.parse_args()

    worker = Path(args.worker_source).read_text(encoding="utf-8")
    execution_header = Path(args.execution_header).read_text(encoding="utf-8")
    execution = Path(args.execution_source).read_text(encoding="utf-8")

    require(
        "ResolveRelationDescriptorPublicOnTransaction(" in execution_header,
        "Firebird execution session does not expose an exact persisted-descriptor API",
    )
    constructor = function_body(
        execution, "FirebirdExecutionSession::FirebirdExecutionSession("
    )
    require(
        "config_.require_relation_descriptor_projection_v3 = true;" in constructor,
        "Firebird execution session does not require SBPS V3",
    )
    authenticate = function_body(
        execution, "FirebirdExecutionSession::AuthenticateCredentials("
    )
    require(
        "session_.relation_descriptor_projection_v3_negotiated" in authenticate,
        "Firebird attach does not fail closed when SBPS V3 is absent",
    )
    resolver = function_body(
        execution,
        "FirebirdExecutionSession::ResolveRelationDescriptorPublicOnTransaction(",
    )
    require(
        "transaction.present()" in resolver
        and "client_.ResolveRelationDescriptorPublicOnTransaction(" in resolver
        and 'presented_name, quoted, "table", config_, transaction' in resolver,
        "Firebird persisted-descriptor API is not routed with the exact selector",
    )

    require(
        worker.count("config.require_relation_descriptor_projection_v3 = true;") == 1,
        "Firebird worker client config must opt into SBPS V3 exactly once",
    )
    column_struct = worker[
        worker.index("struct FirebirdColumnDescriptor") : worker.index(
            "struct FirebirdTableDescriptor"
        )
    ]
    for field in (
        "canonical_type_name",
        "charset_uuid",
        "collation_uuid",
        "character_length",
        "nullable",
    ):
        require(field in column_struct, f"Firebird column metadata lost {field}")

    projection = function_body(
        worker, "FirebirdColumnFromPersistedRelationProjection("
    )
    for assignment in (
        "column.canonical_type_name = projected.canonical_type_name;",
        "column.charset_uuid = projected.charset_uuid;",
        "column.collation_uuid = projected.collation_uuid;",
        "column.character_length = projected.character_length;",
        "column.nullable = projected.nullable;",
    ):
        require(assignment in projection, f"missing exact V3 projection: {assignment}")

    fresh_resolver = function_body(
        worker, "ResolveFirebirdPersistedRelationByName("
    )
    require(
        "ResolveRelationDescriptorPublicOnTransaction(" in fresh_resolver,
        "fresh relation resolution does not consume the execution-session V3 API",
    )
    require(
        "state.tables" not in fresh_resolver
        and "tables[" not in fresh_resolver
        and ".tables.emplace" not in fresh_resolver,
        "fresh persisted-descriptor resolution depends on or mutates state.tables",
    )

    describe = function_body(worker, "DescribeFirebirdSelect(")
    require(
        "const FirebirdTableDescriptor* persisted_relation = nullptr" in worker[
            worker.index("DescribeFirebirdSelect(") : worker.index(
                "DescribeFirebirdSelect("
            )
            + 400
        ]
        and "*persisted_relation" in describe
        and ": FirebirdTableForQuery(state, sql)" in describe,
        "SELECT description does not prefer the transient persisted descriptor",
    )
    prepare_marker = worker.find(
        "std::optional<FirebirdTableDescriptor> persisted_relation;"
    )
    describe_call = worker.find(
        "DescribeFirebirdSelect(state, sql_text, request.sql_dialect,",
        prepare_marker,
    )
    require(
        prepare_marker >= 0
        and describe_call > prepare_marker
        and "FirebirdExecutionTransactionSelector(" in worker[
            prepare_marker : describe_call
        ]
        and "ResolveFirebirdPersistedRelationForSelect(" in worker[
            prepare_marker : describe_call
        ],
        "prepared SQLDA does not resolve V3 metadata on its exact transaction",
    )
    prepare_slice = worker[prepare_marker : describe_call]
    require(
        "FIREBIRD.RELATION_DESCRIPTOR.REQUIRED" in prepare_slice
        and "ClearFirebirdStatementPreparationDescriptor(statement);" in prepare_slice
        and "continue;" in prepare_slice,
        "physical SELECT silently falls back after a missing/incomplete V3 descriptor",
    )

    catalog = function_body(worker, "FirebirdCatalogRows(")
    require(
        "persisted_relation" in catalog
        and "visible_physical_relations.push_back(persisted_relation);" in catalog,
        "filtered Firebird catalog projection cannot consume a transient descriptor",
    )
    require(
        "state.tables[" not in catalog and "state.tables.emplace" not in catalog,
        "catalog V3 projection persists engine metadata into the parser overlay",
    )

    print("firebird_relation_descriptor_projection_gate: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"firebird_relation_descriptor_projection_gate: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
