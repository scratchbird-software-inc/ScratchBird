# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest
from typing import Any

from scratchbird_ai.adapters.http import make_http_dialect_adapter


class FakeJsonClient:
    def __init__(self) -> None:
        self.calls: list[tuple[str, str, dict[str, Any] | None, dict[str, str] | None]] = []

    def request(
        self,
        *,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
        query: dict[str, str] | None = None,
    ) -> Any:
        self.calls.append((method, path, payload, query))

        if method == "POST" and path.endswith("/compile"):
            return {
                "statement_kind": "read",
                "sblr_hash": "abc123",
                "diagnostics": [],
                "warnings": ["compiled by fake client"],
            }

        if method == "POST" and path.endswith("/execute"):
            return {
                "rows": [{"id": 1}, {"id": 2}],
                "notices": ["executed by fake client"],
            }

        if method == "GET" and path.endswith("/schemas"):
            return {"schemas": ["public", "analytics"]}

        if method == "GET" and path.endswith("/schemas/public/tables"):
            return {"tables": ["customers", "orders"]}

        if method == "GET" and path.endswith("/schemas/public/tables/customers"):
            return {
                "schema": "public",
                "table": "customers",
                "columns": [{"name": "id", "type": "uuid", "nullable": False}],
            }

        raise AssertionError(f"Unexpected fake client call: {method} {path}")


class HttpAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.client = FakeJsonClient()
        self.adapter = make_http_dialect_adapter(dialect="native", client=self.client)

    def test_compile_query(self) -> None:
        result = self.adapter.compiler.compile_query("SELECT 1", {})
        self.assertEqual(result.statement_kind, "read")
        self.assertEqual(result.sblr_hash, "abc123")

    def test_execute_compiled(self) -> None:
        result = self.adapter.executor.execute_compiled(
            compile_artifact_id="cmp_x",
            query_text="SELECT 1",
            options={},
        )
        self.assertEqual(len(result.rows), 2)

    def test_metadata_tools(self) -> None:
        schemas = self.adapter.metadata.list_schemas()
        tables = self.adapter.metadata.list_tables("public")
        desc = self.adapter.metadata.describe_table("public", "customers")

        self.assertIn("public", schemas)
        self.assertIn("customers", tables)
        self.assertEqual(desc["table"], "customers")


if __name__ == "__main__":
    unittest.main()
