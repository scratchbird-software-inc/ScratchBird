# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Mock adapter implementations for P0 scaffolding and tests."""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any

from .base import (
    AdapterCompileResult,
    AdapterExecuteResult,
    CompilerAdapter,
    DialectAdapter,
    ExecutorAdapter,
    MetadataAdapter,
)


class MockCompilerAdapter(CompilerAdapter):
    def compile_query(self, query_text: str, context: dict[str, Any]) -> AdapterCompileResult:
        _ = context
        normalized = query_text.strip().lower()
        statement_kind = "read"
        if normalized.startswith(
            (
                "insert",
                "update",
                "delete",
                "merge",
                "create",
                "drop",
                "alter",
                "truncate",
                "grant",
                "revoke",
                "set",
                "call",
                "execute",
            )
        ):
            statement_kind = "mutation"
        elif normalized.startswith(("select", "with", "show", "describe", "desc", "explain", "match")):
            statement_kind = "read"

        sblr_hash = hashlib.sha256(query_text.encode("utf-8")).hexdigest()
        return AdapterCompileResult(
            statement_kind=statement_kind,
            sblr_hash=sblr_hash,
            diagnostics=[],
            warnings=["mock compiler in use"],
        )


class MockExecutorAdapter(ExecutorAdapter):
    def execute_compiled(
        self,
        *,
        compile_artifact_id: str,
        query_text: str,
        options: dict[str, Any],
    ) -> AdapterExecuteResult:
        limit = int(options.get("limit", 10))
        rows = [
            {
                "compile_artifact_id": compile_artifact_id,
                "row_number": i + 1,
                "query_echo": query_text[:120],
            }
            for i in range(max(0, min(limit, 200)))
        ]
        return AdapterExecuteResult(rows=rows, notices=["mock executor in use"])


class MockMetadataAdapter(MetadataAdapter):
    def list_schemas(self, database: str | None = None) -> list[str]:
        _ = database
        return ["public", "analytics"]

    def list_tables(self, schema: str) -> list[str]:
        return {
            "public": ["customers", "orders", "products"],
            "analytics": ["daily_metrics", "monthly_revenue"],
        }.get(schema, [])

    def describe_table(self, schema: str, table: str) -> dict[str, Any]:
        return {
            "schema": schema,
            "table": table,
            "columns": [
                {"name": "id", "type": "uuid", "nullable": False},
                {"name": "name", "type": "text", "nullable": True},
            ],
        }


@dataclass(slots=True)
class MockDialectAdapter(DialectAdapter):
    dialect: str
    compiler: CompilerAdapter
    executor: ExecutorAdapter
    metadata: MetadataAdapter


def make_mock_dialect_adapter(dialect: str) -> MockDialectAdapter:
    return MockDialectAdapter(
        dialect=dialect,
        compiler=MockCompilerAdapter(),
        executor=MockExecutorAdapter(),
        metadata=MockMetadataAdapter(),
    )
