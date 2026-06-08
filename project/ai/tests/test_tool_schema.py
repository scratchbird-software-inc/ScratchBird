# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.tool_schema import (
    get_tool_descriptor,
    get_tool_descriptors,
    normalize_tool_invocation,
    normalize_tool_response,
    ToolContractError,
    error_envelope,
    require_security_context,
    validate_structured_output,
    validate_tool_arguments,
    validate_options,
)


class ToolSchemaTests(unittest.TestCase):
    def test_require_security_context_normalizes_payload(self) -> None:
        normalized = require_security_context(
            {
                "security_context": {
                    "tenant_id": "tenant_a",
                    "actor_id": "actor_a",
                    "roles": ["analyst"],
                    "groups": ["finance"],
                    "grants": ["graph:read"],
                    "session_id": "sess_1",
                    "context_version": 1,
                    "region": "ca-central",
                }
            }
        )
        self.assertEqual(normalized["tenant_id"], "tenant_a")
        self.assertEqual(normalized["actor_id"], "actor_a")
        self.assertEqual(normalized["roles"], ["analyst"])
        self.assertEqual(normalized["groups"], ["finance"])
        self.assertEqual(normalized["grants"], ["graph:read"])
        self.assertEqual(normalized["region"], "ca-central")

    def test_require_security_context_fails_closed(self) -> None:
        with self.assertRaises(ToolContractError) as ctx:
            require_security_context({"security_context": {"tenant_id": "tenant_a"}})
        self.assertEqual(ctx.exception.error_code, "E_POLICY_DENY")

    def test_validate_options_enforces_bounds(self) -> None:
        options = validate_options({"timeout_ms": 1, "memory_mb": 1, "max_rows": 0})
        self.assertEqual(options["timeout_ms"], 100)
        self.assertEqual(options["memory_mb"], 64)
        self.assertEqual(options["max_rows"], 1)

    def test_validate_options_rejects_hard_limit(self) -> None:
        with self.assertRaises(ToolContractError) as ctx:
            validate_options({"max_rows": 10001})
        self.assertEqual(ctx.exception.error_code, "E_LIMIT_EXCEEDED")

    def test_error_envelope_shape(self) -> None:
        env = error_envelope(
            error_code="E_POLICY_DENY",
            message="denied",
            trace_id="tr_123",
            policy_rule_id="RULE-1",
            retryable=False,
        )
        self.assertEqual(env["error_code"], "E_POLICY_DENY")
        self.assertEqual(env["trace_id"], "tr_123")
        self.assertEqual(env["policy_rule_id"], "RULE-1")
        self.assertFalse(env["retryable"])

    def test_get_tool_descriptors_contains_execute_readonly_query(self) -> None:
        tools = {tool["tool_name"]: tool for tool in get_tool_descriptors()}
        self.assertIn("execute_readonly_query", tools)
        self.assertEqual(tools["execute_readonly_query"]["output_mode"], "json_object")
        self.assertIn("get_provider_profiles", tools)
        self.assertIn("export_certification_manifest", tools)
        self.assertIn("get_server_registry", tools)
        self.assertIn("register_remote_server", tools)
        self.assertIn("update_remote_server_lifecycle", tools)
        self.assertIn("report_remote_server_health", tools)
        self.assertIn("resolve_gateway_route", tools)
        self.assertIn("open_remote_session", tools)
        self.assertIn("invoke_remote_tool", tools)
        self.assertIn("poll_remote_operation", tools)
        self.assertIn("replay_audit_bundle", tools)
        self.assertIn("list_audit_bundles", tools)
        self.assertIn("validate_approval_evidence", tools)
        self.assertIn("list_approval_records", tools)
        self.assertIn("revoke_approval_record", tools)
        self.assertIn("create_audit_attestation", tools)
        self.assertIn("verify_audit_attestation", tools)
        self.assertIn("get_runtime_diagnostics", tools)
        self.assertIn("generate_operator_runbook_bundle", tools)
        self.assertIn("create_vector_index", tools)
        self.assertIn("add_generated_embeddings", tools)
        self.assertIn("list_vector_indexes", tools)

    def test_validate_tool_arguments_rejects_unknown_field(self) -> None:
        with self.assertRaises(ToolContractError) as ctx:
            validate_tool_arguments(
                "execute_readonly_query",
                {
                    "dialect": "native",
                    "query_text": "SELECT 1",
                    "security_context": {
                        "tenant_id": "tenant_a",
                        "actor_id": "actor_a",
                    },
                    "unexpected": True,
                },
            )
        self.assertEqual(ctx.exception.error_code, "E_TOOL_INPUT_INVALID")

    def test_validate_add_generated_embeddings_arguments(self) -> None:
        normalized = validate_tool_arguments(
            "add_generated_embeddings",
            {
                "index_id": "idx_generated",
                "dimension": 4,
                "records": [
                    {
                        "vector_id": "doc-1#1",
                        "text": "north overdue invoice",
                        "metadata": {"document_id": "doc-1"},
                    }
                ],
                "provider_config": {
                    "provider_profile_id": "openai_embeddings_v1",
                    "model": "text-embedding-3-small",
                    "api_key": "secret-inline",
                },
                "security_context": {
                    "tenant_id": "tenant_a",
                    "actor_id": "actor_a",
                },
            },
        )
        self.assertEqual(normalized["dimension"], 4)
        self.assertEqual(normalized["provider_config"]["model"], "text-embedding-3-small")

    def test_validate_open_remote_session_arguments(self) -> None:
        normalized = validate_tool_arguments(
            "open_remote_session",
            {
                "request_id": "req_remote_open",
                "interface_profile_id": "mcp_remote_v0",
                "protocol_version": "v0",
                "requested_transport": "https_json_request_response",
                "client_id": "remote-client",
                "client_version": "0.0.1",
                "client_capabilities": {"streaming": False},
                "auth_envelope": {
                    "auth_type": "proxy_principal",
                    "proxy_principal": "svc://agent/frontdoor",
                    "security_context": {
                        "tenant_id": "tenant_a",
                        "actor_id": "actor_a",
                        "groups": ["ops"],
                        "grants": ["graph:read"],
                    },
                },
            },
        )
        self.assertEqual(normalized["protocol_version"], "v0")
        self.assertEqual(normalized["auth_envelope"]["proxy_principal"], "svc://agent/frontdoor")

    def test_validate_governance_arguments(self) -> None:
        normalized = validate_tool_arguments(
            "validate_approval_evidence",
            {
                "approval_evidence": {"approval_token": "approved-token"},
                "security_context": {
                    "tenant_id": "tenant_a",
                    "actor_id": "actor_a",
                },
                "statement_hash": "stmt_hash_1",
            },
        )
        self.assertEqual(normalized["statement_hash"], "stmt_hash_1")
        self.assertEqual(normalized["approval_evidence"]["approval_token"], "approved-token")

    def test_validate_runtime_diagnostics_arguments(self) -> None:
        normalized = validate_tool_arguments(
            "get_runtime_diagnostics",
            {
                "security_context": {
                    "tenant_id": "tenant_admin",
                    "actor_id": "sysarch",
                },
                "max_recent_errors": 25,
            },
        )
        self.assertEqual(normalized["max_recent_errors"], 25)

    def test_validate_operator_bundle_arguments(self) -> None:
        normalized = validate_tool_arguments(
            "generate_operator_runbook_bundle",
            {
                "security_context": {
                    "tenant_id": "tenant_admin",
                    "actor_id": "sysarch",
                },
                "target_profiles": ["manager_proxy"],
            },
        )
        self.assertEqual(normalized["target_profiles"], ["manager_proxy"])

    def test_normalize_openai_tool_call(self) -> None:
        normalized = normalize_tool_invocation(
            payload={
                "id": "call_123",
                "request_id": "req_123",
                "function": {
                    "name": "execute_readonly_query",
                    "arguments": (
                        '{"dialect":"native","query_text":"SELECT 1","security_context":'
                        '{"tenant_id":"tenant_a","actor_id":"actor_a"}}'
                    ),
                },
            },
            interface_profile_id="provider_tool_calling_v0",
            provider_profile_id="openai_tool_calling_v0",
        )
        self.assertEqual(normalized["tool_name"], "execute_readonly_query")
        self.assertEqual(normalized["call_id"], "call_123")
        self.assertEqual(normalized["arguments"]["security_context"]["tenant_id"], "tenant_a")

    def test_normalize_anthropic_tool_use(self) -> None:
        normalized = normalize_tool_invocation(
            payload={
                "id": "call_456",
                "name": "execute_readonly_query",
                "input": {
                    "dialect": "native",
                    "query_text": "SELECT 1",
                    "security_context": {
                        "tenant_id": "tenant_a",
                        "actor_id": "actor_a",
                    },
                    "options": {"max_rows": 1},
                },
            },
            interface_profile_id="provider_tool_calling_v0",
            provider_profile_id="anthropic_tool_use_v0",
        )
        self.assertEqual(normalized["tool_name"], "execute_readonly_query")
        self.assertEqual(normalized["call_id"], "call_456")
        self.assertEqual(normalized["arguments"]["options"]["max_rows"], 1)

    def test_normalize_gemini_function_call(self) -> None:
        normalized = normalize_tool_invocation(
            payload={
                "functionCall": {
                    "id": "call_789",
                    "name": "execute_readonly_query",
                    "args": {
                        "dialect": "native",
                        "query_text": "SELECT 1",
                        "security_context": {
                            "tenant_id": "tenant_a",
                            "actor_id": "actor_a",
                        },
                        "options": {"max_rows": 1},
                    },
                }
            },
            interface_profile_id="provider_tool_calling_v0",
            provider_profile_id="gemini_function_calling_v0",
        )
        self.assertEqual(normalized["tool_name"], "execute_readonly_query")
        self.assertEqual(normalized["call_id"], "call_789")
        self.assertEqual(normalized["arguments"]["options"]["max_rows"], 1)

    def test_validate_structured_output_json_schema(self) -> None:
        validated = validate_structured_output(
            output_mode="json_schema",
            payload={"tools": []},
            output_schema=get_tool_descriptor("get_tool_descriptors")["output_schema"],
        )
        self.assertEqual(validated["validation_status"], "valid")
        self.assertEqual(validated["schema_id"], "tool_descriptor_catalog")

    def test_validate_provider_profile_catalog_output(self) -> None:
        validated = validate_structured_output(
            output_mode="json_schema",
            payload={"profiles": []},
            output_schema=get_tool_descriptor("get_provider_profiles")["output_schema"],
        )
        self.assertEqual(validated["validation_status"], "valid")
        self.assertEqual(validated["schema_id"], "provider_profile_catalog")

    def test_validate_structured_output_invalid_json_object(self) -> None:
        with self.assertRaises(ToolContractError) as ctx:
            validate_structured_output(output_mode="json_object", payload="not-an-object")
        self.assertEqual(ctx.exception.error_code, "E_STRUCTURED_OUTPUT_INVALID")

    def test_normalize_tool_response_wraps_success(self) -> None:
        envelope = normalize_tool_response(
            tool_name="get_tool_descriptors",
            request_id="req_1",
            call_id="call_1",
            interface_profile_id="service_internal_v0",
            trace_id="tr_1",
            result={"tools": []},
        )
        self.assertEqual(envelope["status"], "success")
        self.assertEqual(envelope["structured_output"]["validation_status"], "valid")


if __name__ == "__main__":
    unittest.main()
