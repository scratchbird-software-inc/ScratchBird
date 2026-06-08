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
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import anyio
    from mcp import ClientSession
    from mcp.client.stdio import StdioServerParameters, stdio_client
except ImportError:  # pragma: no cover - optional runtime dependency
    anyio = None
    ClientSession = None
    StdioServerParameters = None
    stdio_client = None


REPO_ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = REPO_ROOT / "src"


def _prepend_pythonpath(value: str) -> str:
    if not value:
        return str(SRC_DIR)
    return str(SRC_DIR) + os.pathsep + value


@unittest.skipUnless(anyio is not None, "mcp runtime not installed")
class McpRuntimeIntegrationTests(unittest.TestCase):
    def test_stdio_runtime_supports_mock_tool_calls(self) -> None:
        async def exercise() -> None:
            with tempfile.TemporaryDirectory() as tmp_dir:
                env = os.environ.copy()
                env["PYTHONPATH"] = _prepend_pythonpath(env.get("PYTHONPATH", ""))
                env["SCRATCHBIRD_AI_ADAPTER_MODE"] = "mock"
                env["SCRATCHBIRD_AI_REMOTE_MCP_AUTH_TOKEN"] = "secret-token"
                env["SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH"] = str(
                    Path(tmp_dir) / "approval-ledger.json"
                )

                server = StdioServerParameters(
                    command=sys.executable,
                    args=["-m", "scratchbird_ai.mcp_server"],
                    env=env,
                    cwd=str(REPO_ROOT),
                )

                async with stdio_client(server) as (read_stream, write_stream):
                    async with ClientSession(read_stream, write_stream) as session:
                        initialized = await session.initialize()
                        self.assertEqual(initialized.serverInfo.name, "scratchbird-ai")

                        tools = await session.list_tools()
                        tool_names = {tool.name for tool in tools.tools}
                        self.assertIn("get_capabilities", tool_names)
                        self.assertIn("compile_query", tool_names)
                        self.assertIn("run_query", tool_names)
                        self.assertIn("open_remote_session", tool_names)
                        self.assertIn("validate_approval_evidence", tool_names)

                        capabilities = await session.call_tool("get_capabilities", {})
                        self.assertFalse(capabilities.isError)
                        capability_payload = json.loads(capabilities.content[0].text)
                        self.assertEqual(capability_payload["service"], "scratchbird-ai")
                        self.assertEqual(capability_payload["adapter_mode"], "mock")

                        compiled = await session.call_tool(
                            "compile_query",
                            {
                                "dialect": "native",
                                "query_text": "SELECT 1",
                                "context": {},
                            },
                        )
                        self.assertFalse(compiled.isError)
                        compile_payload = json.loads(compiled.content[0].text)
                        self.assertEqual(compile_payload["dialect"], "native")
                        self.assertEqual(compile_payload["statement_kind"], "read")
                        self.assertIn("compile_artifact_id", compile_payload)

                        remote_open = await session.call_tool(
                            "open_remote_session",
                            {
                                "auth_envelope": {
                                    "auth_type": "bearer",
                                    "token": "secret-token",
                                    "security_context": {
                                        "tenant_id": "tenant_a",
                                        "actor_id": "actor_a",
                                        "roles": ["analyst"],
                                        "session_id": "sess_1",
                                        "context_version": 1,
                                    },
                                },
                                "request_id": "req_runtime_remote_open",
                                "client_id": "runtime-test",
                                "client_version": "0.1.0",
                            },
                        )
                        self.assertFalse(remote_open.isError)
                        remote_open_payload = json.loads(remote_open.content[0].text)
                        session_id = remote_open_payload["session_id"]

                        remote_query = await session.call_tool(
                            "invoke_remote_tool",
                            {
                                "session_id": session_id,
                                "request_id": "req_runtime_remote_invoke",
                                "method": "execute_readonly_query",
                                "params": {
                                    "dialect": "native",
                                    "query_text": "SELECT 1",
                                    "options": {"max_rows": 1},
                                },
                            },
                        )
                        self.assertFalse(remote_query.isError)
                        remote_query_payload = json.loads(remote_query.content[0].text)
                        self.assertEqual(remote_query_payload["status"], "success")

                        approval = await session.call_tool(
                            "validate_approval_evidence",
                            {
                                "approval_evidence": {"approval_token": "approved-token"},
                                "security_context": {
                                    "tenant_id": "tenant_a",
                                    "actor_id": "actor_a",
                                },
                                "statement_hash": "stmt_hash_runtime",
                            },
                        )
                        self.assertFalse(approval.isError)
                        approval_payload = json.loads(approval.content[0].text)
                        self.assertIn(
                            approval_payload["validation_status"],
                            {"valid", "validated"},
                        )
                        self.assertIn("approval_id", approval_payload)

        anyio.run(exercise)


if __name__ == "__main__":
    unittest.main()
