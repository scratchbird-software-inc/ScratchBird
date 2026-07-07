# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import threading
import unittest
from typing import Any

from scratchbird_ai.http_bridge import (
    BridgeCompileResult,
    BridgeExecuteResult,
    BridgeSettings,
    ScratchBirdBridgeApp,
    build_http_server,
)
from scratchbird_ai.service import build_default_service
from scratchbird_ai.settings import RuntimeSettings


class FakeBridgeBackend:
    def compile_query(
        self,
        *,
        dialect: str,
        query_text: str,
        context: dict[str, Any],
    ) -> BridgeCompileResult:
        del context
        if query_text.strip().lower().startswith("select"):
            statement_kind = "read"
        else:
            statement_kind = "mutation"
        return BridgeCompileResult(
            statement_kind=statement_kind,
            sblr_hash=f"{dialect}-hash",
            diagnostics=[],
            warnings=[],
        )

    def execute_query(
        self,
        *,
        dialect: str,
        query_text: str,
        options: dict[str, Any],
        compile_artifact_id: str,
    ) -> BridgeExecuteResult:
        return BridgeExecuteResult(
            rows=[
                {
                    "dialect": dialect,
                    "query_text": query_text,
                    "compile_artifact_id": compile_artifact_id,
                    "options": options,
                }
            ],
            notices=["fake-bridge-executed"],
        )

    def list_schemas(self, *, dialect: str, database: str | None = None) -> list[str]:
        del dialect, database
        return ["public", "analytics"]

    def list_tables(self, *, dialect: str, schema: str) -> list[str]:
        del dialect
        if schema == "public":
            return ["customers", "orders"]
        return []

    def describe_table(self, *, dialect: str, schema: str, table: str) -> dict[str, Any]:
        return {
            "dialect": dialect,
            "schema": schema,
            "table": table,
            "columns": [{"name": "id", "type": "uuid", "nullable": False}],
        }


class HttpServiceIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.bridge_settings = BridgeSettings(
            host="127.0.0.1",
            port=0,
            api_token="bridge-token",
            enabled_dialects=("native",),
            default_dsn="scratchbird://unused",
        )
        app = ScratchBirdBridgeApp(
            settings=self.bridge_settings,
            backend=FakeBridgeBackend(),
        )
        self.server = build_http_server(app=app)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

        host, port = self.server.server_address
        base_url = f"http://{host}:{port}"
        runtime_settings = RuntimeSettings(
            adapter_mode="http",
            http_base_url=base_url,
            http_timeout_sec=3.0,
            http_api_token="bridge-token",
            http_dialects=("native",),
        )
        self.service = build_default_service(settings=runtime_settings)

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)

    def test_run_query_over_http_adapter(self) -> None:
        response = self.service.run_query(
            request_id="req_http_integration_1",
            dialect="native",
            query_text="SELECT 1",
            mode="read_only",
            options={"limit": 1},
            context={
                "security_context": {
                    "tenant_id": "tenant_http",
                    "actor_id": "actor_http",
                    "roles": ["analyst"],
                    "session_id": "sess_http",
                    "context_version": 1,
                }
            },
        )

        self.assertEqual(response.request_id, "req_http_integration_1")
        self.assertEqual(len(response.result_rows), 1)
        self.assertEqual(response.result_rows[0]["dialect"], "native")
        self.assertIn("fake-bridge-executed", response.notices)

    def test_metadata_roundtrip_over_http_adapter(self) -> None:
        schemas = self.service.list_schemas(dialect="native")
        tables = self.service.list_tables(dialect="native", schema="public")
        desc = self.service.describe_table(
            dialect="native",
            schema="public",
            table="customers",
        )

        self.assertIn("public", schemas)
        self.assertIn("customers", tables)
        self.assertEqual(desc["table"], "customers")


if __name__ == "__main__":
    unittest.main()
