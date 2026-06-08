# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import http.server
import os
import runpy
import sys
import types
import unittest
import warnings
from unittest.mock import patch

import scratchbird_ai.mcp_server as MODULE
import scratchbird_ai.service as SERVICE_MODULE


class _FakeResult:
    def __init__(self, payload: dict) -> None:
        self._payload = dict(payload)

    def to_dict(self) -> dict:
        return dict(self._payload)


class _FakeService:
    def __init__(self, *, fail_name: str | None = None) -> None:
        self.calls: list[tuple[str, tuple, dict]] = []
        self.fail_name = fail_name

    def __getattr__(self, name: str):
        def handler(*args, **kwargs):
            self.calls.append((name, args, kwargs))
            if self.fail_name == name:
                raise ValueError(f"{name} exploded")
            if name == "get_capabilities":
                return {"service": "scratchbird-ai"}
            if name == "get_tool_descriptors":
                return {"tools": []}
            if name == "get_provider_profiles":
                return {"profiles": []}
            if name == "get_compatibility_manifest":
                return {"compatibility": True}
            if name == "export_certification_manifest":
                return {"certification": True}
            if name == "negotiate_compatibility":
                return {"decision": "allow"}
            if name == "get_server_registry":
                return {"entries": [], "registry_schema_version": "2026-04-20"}
            if name == "register_remote_server":
                return {"entry": {"server_id": "srv_remote"}}
            if name == "update_remote_server_lifecycle":
                return {"entry": {"server_id": kwargs["server_id"], "lifecycle_state": kwargs["action"]}}
            if name == "report_remote_server_health":
                return {"entry": {"server_id": kwargs["server_id"], "health_state": kwargs["health_state"]}}
            if name == "resolve_gateway_route":
                return {"status": "allowed", "server_id": kwargs.get("preferred_server_id") or "srv_local"}
            if name == "open_remote_session":
                request = args[0]
                return {
                    "request_id": request["request_id"],
                    "session_id": "sess_remote",
                    "server_id": "srv_local",
                    "capability_manifest_id": "mf_local",
                    "negotiated_transport": request["requested_transport"],
                }
            if name == "invoke_remote_tool":
                return {
                    "session_id": kwargs["session_id"],
                    "request_id": kwargs["request_id"],
                    "status": "success",
                    "result": {"tool": kwargs["method"]},
                    "error": None,
                    "operation_id": None,
                    "operation_state": "completed",
                }
            if name == "close_remote_session":
                return {"session_id": kwargs["session_id"], "status": "closed"}
            if name == "poll_remote_operation":
                return {
                    "session_id": kwargs["session_id"],
                    "operation_id": kwargs["operation_id"],
                    "operation_state": "completed",
                    "events": [{"event_type": "completed"}],
                    "error": None,
                }
            if name == "cancel_remote_operation":
                return {
                    "session_id": kwargs["session_id"],
                    "operation_id": kwargs["operation_id"],
                    "status": "already_completed",
                    "operation_state": "completed",
                }
            if name == "list_dialects":
                return ["native"]
            if name == "list_schemas":
                return ["users.public"]
            if name == "list_tables":
                return ["customers"]
            if name == "describe_table":
                return {
                    "dialect": kwargs["dialect"],
                    "schema": kwargs["schema"],
                    "table": kwargs["table"],
                    "columns": [{"name": "customer_id", "type": "INT32", "nullable": False}],
                }
            if name in {"compile_query", "execute_compiled", "run_query"}:
                return _FakeResult({"tool": name})
            if name in {"execute_readonly_query", "execute_mutation"}:
                return {"tool": name}
            if name == "introspect_plan":
                return {"tool": name, "plan": []}
            if name == "create_vector_index":
                return {"index_id": kwargs["index_id"], "status": "created"}
            if name == "list_vector_indexes":
                return [{"index_id": "idx_customers"}]
            if name == "describe_vector_index":
                return {"index_id": kwargs["index_id"], "dimension": 2}
            if name == "add_embeddings":
                return {"added": len(kwargs["records"])}
            if name == "add_generated_embeddings":
                return {"added": len(kwargs["records"]), "provider": kwargs["provider_config"]["provider"]}
            if name == "delete_embeddings":
                return {"deleted": len(kwargs["vector_ids"])}
            if name == "reindex_vector_index":
                return {"index_id": kwargs["index_id"], "status": "reindexed"}
            if name == "delete_vector_index":
                return {"index_id": kwargs["index_id"], "status": "deleted"}
            if name == "vector_search":
                return {"matches": [], "top_k": kwargs["top_k"]}
            if name == "hybrid_search":
                return {"matches": [], "dialect": kwargs["dialect"]}
            if name == "replay_audit_bundle":
                return {"matches": True, "outcome": "REPLAY_MATCH"}
            if name == "list_audit_bundles":
                return {"bundle_count": 1, "bundles": [{"bundle_hash": "hash_1"}]}
            if name == "validate_approval_evidence":
                return {"validation_status": "validated", "approval_id": "appr_1"}
            if name == "list_approval_records":
                return {"record_count": 1, "records": [{"approval_id": "appr_1"}]}
            if name == "revoke_approval_record":
                return {"record": {"approval_id": kwargs["approval_id"], "revoked_by": "sysarch"}}
            if name == "create_audit_attestation":
                return {
                    "bundle_hash": "hash_1",
                    "attestation": {"attestation_mode": "hmac_sha256"},
                }
            if name == "verify_audit_attestation":
                return {"verified": True, "outcome": "ATTESTATION_VERIFIED"}
            if name == "get_runtime_diagnostics":
                return {"event_summary": {"total_events": 1}, "approval_summary": {"total_records": 1}}
            if name == "generate_operator_runbook_bundle":
                return {"status": "PASS", "files": {"summary": "/tmp/summary.json"}}
            raise AssertionError(f"Unexpected fake service call: {name}")

        return handler


class _FakeFastMCP:
    last_instance: "_FakeFastMCP | None" = None

    def __init__(self, name: str, json_response: bool = False) -> None:
        self.name = name
        self.json_response = json_response
        self.tools: dict[str, object] = {}
        self.ran = False
        _FakeFastMCP.last_instance = self

    def tool(self):
        def decorator(fn):
            self.tools[fn.__name__] = fn
            return fn

        return decorator

    def run(self) -> None:
        self.ran = True


class McpServerTests(unittest.TestCase):
    maxDiff = None

    def _create_server(self, service: _FakeService | None = None) -> _FakeFastMCP:
        fake_service = service or _FakeService()
        with patch.object(MODULE, "FastMCP", _FakeFastMCP):
            server = MODULE.create_server(service=fake_service)
        self.assertIsInstance(server, _FakeFastMCP)
        return server

    def test_create_server_requires_runtime(self) -> None:
        with patch.object(MODULE, "FastMCP", None):
            with self.assertRaisesRegex(RuntimeError, "MCP runtime not installed"):
                MODULE.create_server(service=_FakeService())

    def test_create_server_registers_complete_tool_surface(self) -> None:
        server = self._create_server()
        self.assertEqual(
            set(server.tools),
            {
                "get_capabilities",
                "get_tool_descriptors",
                "get_provider_profiles",
                "get_compatibility_manifest",
                "export_certification_manifest",
                "negotiate_compatibility",
                "get_server_registry",
                "register_remote_server",
                "update_remote_server_lifecycle",
                "report_remote_server_health",
                "resolve_gateway_route",
                "open_remote_session",
                "invoke_remote_tool",
                "close_remote_session",
                "poll_remote_operation",
                "cancel_remote_operation",
                "list_dialects",
                "list_schemas",
                "list_tables",
                "describe_table",
                "compile_query",
                "execute_compiled",
                "execute_readonly_query",
                "execute_mutation",
                "run_query",
                "run_mutation",
                "explain_query",
                "create_vector_index",
                "list_vector_indexes",
                "describe_vector_index",
                "add_embeddings",
                "add_generated_embeddings",
                "delete_embeddings",
                "reindex_vector_index",
                "delete_vector_index",
                "vector_search",
                "hybrid_search",
                "replay_audit_bundle",
                "list_audit_bundles",
                "validate_approval_evidence",
                "list_approval_records",
                "revoke_approval_record",
                "create_audit_attestation",
                "verify_audit_attestation",
                "get_runtime_diagnostics",
                "generate_operator_runbook_bundle",
            },
        )

    def test_registered_tools_invoke_service_wrappers(self) -> None:
        service = _FakeService()
        server = self._create_server(service)
        tools = server.tools
        security_context = {
            "tenant_id": "tenant_1",
            "actor_id": "actor_1",
            "roles": ["analyst"],
            "session_id": "sess_1",
            "context_version": 1,
        }
        records = [{"vector_id": "v1", "embedding": [0.1, 0.2], "metadata": {"tenant_id": "tenant_1"}}]

        self.assertEqual(tools["get_capabilities"]()["service"], "scratchbird-ai")
        self.assertEqual(tools["get_tool_descriptors"]()["tools"], [])
        self.assertEqual(tools["get_provider_profiles"]()["profiles"], [])
        self.assertTrue(tools["get_compatibility_manifest"]()["compatibility"])
        self.assertTrue(tools["export_certification_manifest"]()["certification"])
        self.assertEqual(tools["negotiate_compatibility"]({"client": "codex"})["decision"], "allow")
        self.assertEqual(tools["get_server_registry"]()["registry_schema_version"], "2026-04-20")
        self.assertEqual(
            tools["register_remote_server"](
                "remote-a",
                ["mcp_remote_v0"],
                ["remote_session"],
                "remote_server",
                security_context,
            )["entry"]["server_id"],
            "srv_remote",
        )
        self.assertEqual(
            tools["update_remote_server_lifecycle"](
                "srv_remote",
                "drain",
                security_context,
            )["entry"]["lifecycle_state"],
            "drain",
        )
        self.assertEqual(
            tools["report_remote_server_health"](
                "srv_remote",
                "healthy",
                security_context,
            )["entry"]["health_state"],
            "healthy",
        )
        self.assertEqual(
            tools["resolve_gateway_route"]("execute_readonly_query")["status"],
            "allowed",
        )
        self.assertEqual(
            tools["open_remote_session"](
                {"auth_type": "bearer", "token": "secret"},
                request_id="req_remote_1",
                client_id="codex",
                client_version="0.1.0",
            ),
            {
                "request_id": "req_remote_1",
                "session_id": "sess_remote",
                "server_id": "srv_local",
                "capability_manifest_id": "mf_local",
                "negotiated_transport": "https_json_request_response",
            },
        )
        self.assertEqual(
            tools["invoke_remote_tool"](
                "sess_remote",
                "req_remote_invoke",
                "execute_readonly_query",
                {"dialect": "native", "query_text": "SELECT 1"},
            )["status"],
            "success",
        )
        self.assertEqual(
            tools["poll_remote_operation"]("sess_remote", "op_1")["operation_state"],
            "completed",
        )
        self.assertEqual(
            tools["cancel_remote_operation"]("sess_remote", "op_1", "req_cancel", "done")["status"],
            "already_completed",
        )
        self.assertEqual(tools["close_remote_session"]("sess_remote")["status"], "closed")
        self.assertEqual(tools["list_dialects"]()["dialects"], ["native"])
        self.assertEqual(tools["list_schemas"]("native", "main")["schemas"], ["users.public"])
        self.assertEqual(
            tools["list_tables"]("native", "users.public")["tables"],
            ["customers"],
        )
        self.assertEqual(
            tools["describe_table"]("native", "users.public", "customers")["table"],
            "customers",
        )
        self.assertEqual(
            tools["compile_query"]("native", "SELECT 1", {"trace": True})["tool"],
            "compile_query",
        )
        self.assertEqual(
            tools["execute_compiled"]("cmp_1", {"limit": 1}, "ai_analysis", "")["tool"],
            "execute_compiled",
        )
        self.assertEqual(
            tools["execute_readonly_query"](
                "native",
                "SELECT 1",
                security_context,
                {"limit": 1},
                {"timeout_ms": 5},
            )["tool"],
            "execute_readonly_query",
        )
        self.assertEqual(
            tools["execute_mutation"](
                "native",
                "DELETE FROM customers",
                security_context,
                {"approval_token": "approved"},
                {"limit": 1},
                {"timeout_ms": 5},
            )["tool"],
            "execute_mutation",
        )
        self.assertEqual(
            tools["run_query"](
                "native",
                "SELECT 1",
                "ai_analysis",
                {"limit": 1},
                {"security_context": security_context},
                "",
            )["tool"],
            "run_query",
        )
        self.assertEqual(
            tools["run_mutation"](
                "native",
                "DELETE FROM customers",
                "approved",
                {"limit": 1},
                {"security_context": security_context},
            )["tool"],
            "execute_mutation",
        )
        self.assertEqual(
            tools["explain_query"]("native", "SELECT 1", security_context, {"trace": True})["tool"],
            "introspect_plan",
        )
        self.assertEqual(
            tools["create_vector_index"]("idx_customers", 2, security_context, "client_supplied_embeddings_v0")[
                "status"
            ],
            "created",
        )
        self.assertEqual(
            tools["list_vector_indexes"](security_context, False)[0]["index_id"],
            "idx_customers",
        )
        self.assertEqual(
            tools["describe_vector_index"]("idx_customers", security_context)["index_id"],
            "idx_customers",
        )
        self.assertEqual(
            tools["add_embeddings"]("idx_customers", 2, records, security_context)["added"],
            1,
        )
        self.assertEqual(
            tools["add_generated_embeddings"](
                "idx_customers",
                2,
                records,
                {"provider": "openai", "api_key": "secret"},
                security_context,
            )["provider"],
            "openai",
        )
        self.assertEqual(
            tools["delete_embeddings"]("idx_customers", ["v1"], security_context)["deleted"],
            1,
        )
        self.assertEqual(
            tools["reindex_vector_index"]("idx_customers", security_context)["status"],
            "reindexed",
        )
        self.assertEqual(
            tools["delete_vector_index"]("idx_customers", security_context)["status"],
            "deleted",
        )
        self.assertEqual(
            tools["vector_search"]("idx_customers", [0.1, 0.2], 5, security_context, {"tenant_id": "tenant_1"}, False)[
                "top_k"
            ],
            5,
        )
        self.assertEqual(
            tools["hybrid_search"](
                "native",
                "SELECT * FROM customers",
                [0.1, 0.2],
                "idx_customers",
                5,
                security_context,
                {"metadata": {"tenant_id": "tenant_1"}},
                {"vector": 0.7, "lexical": 0.3},
                {"limit": 5},
            )["dialect"],
            "native",
        )
        self.assertTrue(tools["replay_audit_bundle"]({"bundle_hash": "hash_1"})["matches"])
        self.assertEqual(
            tools["list_audit_bundles"](security_context)["bundle_count"],
            1,
        )
        self.assertEqual(
            tools["validate_approval_evidence"](
                {"approval_token": "approved-token"},
                security_context,
                "stmt_hash_1",
            )["validation_status"],
            "validated",
        )
        self.assertEqual(
            tools["list_approval_records"](security_context)["record_count"],
            1,
        )
        self.assertEqual(
            tools["revoke_approval_record"]("appr_1", "cleanup", security_context)["record"][
                "approval_id"
            ],
            "appr_1",
        )
        self.assertEqual(
            tools["create_audit_attestation"](security_context, {"bundle_hash": "hash_1"})[
                "attestation"
            ]["attestation_mode"],
            "hmac_sha256",
        )
        self.assertTrue(
            tools["verify_audit_attestation"](
                security_context,
                {"bundle_hash": "hash_1"},
                {"attestation_mode": "hmac_sha256"},
            )["verified"]
        )
        self.assertEqual(
            tools["get_runtime_diagnostics"](security_context)["approval_summary"][
                "total_records"
            ],
            1,
        )
        self.assertEqual(
            tools["generate_operator_runbook_bundle"](security_context)["status"],
            "PASS",
        )

        called_names = [name for name, _, _ in service.calls]
        self.assertIn("execute_mutation", called_names)
        self.assertIn("introspect_plan", called_names)
        self.assertIn("open_remote_session", called_names)
        self.assertIn("validate_approval_evidence", called_names)
        self.assertIn("generate_operator_runbook_bundle", called_names)

    def test_tool_wrapper_maps_service_exceptions(self) -> None:
        service = _FakeService(fail_name="list_schemas")
        server = self._create_server(service)

        response = server.tools["list_schemas"]("native", "main")

        self.assertEqual(response["error_code"], "E_INVALID_ARGUMENT")
        self.assertIn("list_schemas exploded", response["message"])

    def test_main_runs_registered_server(self) -> None:
        fake_server = _FakeFastMCP("scratchbird-ai", json_response=True)
        with patch.object(MODULE, "create_server", return_value=fake_server):
            MODULE.main()
        self.assertTrue(fake_server.ran)

    def test_module_entrypoint_runs_main_for_mcp_server(self) -> None:
        fake_service = _FakeService()
        fake_mcp_pkg = types.ModuleType("mcp")
        fake_mcp_server_pkg = types.ModuleType("mcp.server")
        fake_mcp_fastmcp_pkg = types.ModuleType("mcp.server.fastmcp")
        fake_mcp_fastmcp_pkg.FastMCP = _FakeFastMCP

        with patch.object(SERVICE_MODULE, "build_default_service", return_value=fake_service):
            with patch.dict(
                sys.modules,
                {
                    "mcp": fake_mcp_pkg,
                    "mcp.server": fake_mcp_server_pkg,
                    "mcp.server.fastmcp": fake_mcp_fastmcp_pkg,
                },
                clear=False,
            ):
                _FakeFastMCP.last_instance = None
                with warnings.catch_warnings():
                    warnings.simplefilter("ignore", RuntimeWarning)
                    runpy.run_module("scratchbird_ai.mcp_server", run_name="__main__")

        self.assertIsNotNone(_FakeFastMCP.last_instance)
        self.assertTrue(_FakeFastMCP.last_instance.ran)

    def test_package_entrypoint_runs_main(self) -> None:
        fake_server = _FakeFastMCP("scratchbird-ai", json_response=True)
        with patch.object(MODULE, "create_server", return_value=fake_server):
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", RuntimeWarning)
                runpy.run_module("scratchbird_ai", run_name="__main__")

        self.assertTrue(fake_server.ran)

    def test_module_entrypoint_runs_main_for_http_bridge(self) -> None:
        served: list[tuple[str, int]] = []

        fake_scratchbird = types.ModuleType("scratchbird")
        fake_scratchbird.connect = lambda **kwargs: None
        fake_scratchbird.protocol = types.SimpleNamespace()

        def fake_serve_forever(server, *args, **kwargs) -> None:
            served.append(server.server_address)
            server.server_close()

        env = {
            "SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN": "scratchbird://user:pass@127.0.0.1:3092/main",
            "SCRATCHBIRD_AI_BRIDGE_PORT": "0",
            "SCRATCHBIRD_AI_BRIDGE_API_TOKEN": "bridge-token",
        }
        with patch.dict(sys.modules, {"scratchbird": fake_scratchbird}, clear=False):
            with patch.object(http.server.ThreadingHTTPServer, "serve_forever", fake_serve_forever):
                with patch.dict(os.environ, env, clear=False):
                    with warnings.catch_warnings():
                        warnings.simplefilter("ignore", RuntimeWarning)
                        runpy.run_module("scratchbird_ai.http_bridge", run_name="__main__")

        self.assertTrue(served)


if __name__ == "__main__":
    unittest.main()
