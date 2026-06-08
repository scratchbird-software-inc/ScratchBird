# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scratchbird_ai.policy import PolicyDeniedError
from scratchbird_ai.router import RoutingError
from scratchbird_ai.service import build_default_service
from scratchbird_ai.settings import RuntimeSettings
from scratchbird_ai.tool_schema import ToolContractError


class ServiceTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp_dir.cleanup)
        self.service = build_default_service(
            settings=RuntimeSettings(
                approval_ledger_path=str(Path(self._tmp_dir.name) / "approval-ledger.json"),
                structured_event_log_path=str(Path(self._tmp_dir.name) / "structured-events.jsonl"),
                operator_bundle_output_dir=str(Path(self._tmp_dir.name) / "operator-bundle"),
                audit_attestation_secret="test-attestation-secret",
                audit_attestation_delegated_secret="test-delegated-secret",
                audit_attestation_delegated_attestor_id="external-attestor",
                audit_attestation_external_reference_base_url="https://approvals.example.com",
                operator_target_profiles=("listener_direct", "manager_proxy"),
            )
        )
        self.security_context = {
            "tenant_id": "tenant_a",
            "actor_id": "actor_a",
            "roles": ["analyst"],
            "session_id": "sess_1",
            "context_version": 1,
        }
        self.admin_context = {
            "tenant_id": "tenant_admin",
            "actor_id": "sysarch_actor",
            "roles": ["sysarch"],
            "grants": ["admin:manage", "mcp:registry:write"],
            "session_id": "sess_admin",
            "context_version": 1,
        }

    def test_run_query_read_only(self) -> None:
        resp = self.service.run_query(
            request_id="req_test_1",
            dialect="native",
            query_text="SELECT 1",
            mode="read_only",
            options={"limit": 2},
            context={
                "security_context": {
                    "tenant_id": "tenant_a",
                    "actor_id": "actor_a",
                    "roles": ["analyst"],
                    "session_id": "sess_1",
                    "context_version": 1,
                }
            },
        )
        self.assertEqual(resp.request_id, "req_test_1")
        self.assertEqual(resp.row_count, 2)
        self.assertEqual(len(resp.result_rows), 2)

    def test_run_query_mutation_denied_without_approval(self) -> None:
        with self.assertRaises(PolicyDeniedError):
            self.service.run_query(
                request_id="req_test_2",
                dialect="native",
                query_text="UPDATE users SET name = 'x'",
                mode="read_only",
                context={
                    "security_context": {
                        "tenant_id": "tenant_a",
                        "actor_id": "actor_a",
                        "roles": ["analyst"],
                        "session_id": "sess_1",
                        "context_version": 1,
                    }
                },
            )

    def test_run_query_denies_missing_security_context(self) -> None:
        with self.assertRaises(PolicyDeniedError) as ctx:
            self.service.run_query(
                request_id="req_missing_ctx",
                dialect="native",
                query_text="SELECT 1",
                mode="ai_analysis",
                context={},
            )
        self.assertEqual(ctx.exception.error_code, "E_POLICY_DENY")
        bundle = self.service.latest_audit_bundle()
        self.assertIsNotNone(bundle)
        assert bundle is not None
        self.assertEqual(bundle["request_id"], "req_missing_ctx")
        self.assertEqual(bundle["policy_decision"], "deny")

    def test_hybrid_mode_wires_http_for_native_dialect(self) -> None:
        settings = RuntimeSettings(
            adapter_mode="hybrid",
            http_base_url="http://127.0.0.1:3095",
            http_dialects=("native",),
        )
        service = build_default_service(settings=settings)

        native_adapter_name = service.adapters["native"].__class__.__name__

        self.assertEqual(native_adapter_name, "ScratchBirdHttpDialectAdapter")
        self.assertEqual(set(service.adapters.keys()), {"native"})

    def test_run_query_rejects_non_native_dialect(self) -> None:
        with self.assertRaises(RoutingError) as ctx:
            self.service.run_query(
                request_id="req_test_non_native",
                dialect="postgresql",
                query_text="SELECT 1",
                mode="read_only",
                context={
                    "security_context": {
                        "tenant_id": "tenant_a",
                        "actor_id": "actor_a",
                        "roles": [],
                        "session_id": "sess_1",
                        "context_version": 1,
                    }
                },
            )
        self.assertIn("supports ScratchBird-native", str(ctx.exception))

    def test_get_capabilities_publishes_interface_profile_inventory(self) -> None:
        capabilities = self.service.get_capabilities()
        self.assertEqual(capabilities["tool_descriptor_version"], "1.0")
        self.assertEqual(capabilities["compatibility_version"], "2026-03-07")
        self.assertTrue(capabilities["supports"]["compatibility_negotiation"])
        self.assertTrue(capabilities["supports"]["registry_governance"])
        self.assertTrue(capabilities["supports"]["gateway_routing"])
        self.assertTrue(capabilities["supports"]["remote_server_lifecycle"])
        self.assertTrue(capabilities["server_id"].startswith("srv_"))
        self.assertTrue(capabilities["capability_manifest_id"].startswith("mf_"))
        self.assertEqual(capabilities["capability_manifest_version"], "2026-04-20")
        self.assertEqual(capabilities["publication_state"], "published")
        self.assertEqual(capabilities["lifecycle_state"], "enabled")
        self.assertEqual(capabilities["health_state"], "healthy")
        self.assertEqual(capabilities["routing_target"], "local_service")
        self.assertEqual(capabilities["registry_summary"]["local_server_id"], capabilities["server_id"])
        self.assertEqual(
            capabilities["supports"]["structured_output_modes"],
            ["none", "json_object", "json_schema"],
        )
        self.assertIn("get_tool_descriptors", capabilities["supports"]["canonical_tools"])
        self.assertIn("get_provider_profiles", capabilities["supports"]["canonical_tools"])
        self.assertIn("get_compatibility_manifest", capabilities["supports"]["canonical_tools"])
        self.assertIn("export_certification_manifest", capabilities["supports"]["canonical_tools"])
        self.assertIn("negotiate_compatibility", capabilities["supports"]["canonical_tools"])
        self.assertIn("get_server_registry", capabilities["supports"]["canonical_tools"])
        self.assertIn("register_remote_server", capabilities["supports"]["canonical_tools"])
        self.assertIn("update_remote_server_lifecycle", capabilities["supports"]["canonical_tools"])
        self.assertIn("report_remote_server_health", capabilities["supports"]["canonical_tools"])
        self.assertIn("resolve_gateway_route", capabilities["supports"]["canonical_tools"])
        self.assertIn("open_remote_session", capabilities["supports"]["canonical_tools"])
        self.assertIn("invoke_remote_tool", capabilities["supports"]["canonical_tools"])
        self.assertIn("poll_remote_operation", capabilities["supports"]["canonical_tools"])
        self.assertIn("cancel_remote_operation", capabilities["supports"]["canonical_tools"])
        self.assertIn("create_vector_index", capabilities["supports"]["canonical_tools"])
        self.assertIn("add_generated_embeddings", capabilities["supports"]["canonical_tools"])
        self.assertIn("list_vector_indexes", capabilities["supports"]["canonical_tools"])
        self.assertIn("replay_audit_bundle", capabilities["supports"]["canonical_tools"])
        self.assertIn("list_audit_bundles", capabilities["supports"]["canonical_tools"])
        self.assertIn("validate_approval_evidence", capabilities["supports"]["canonical_tools"])
        self.assertIn("list_approval_records", capabilities["supports"]["canonical_tools"])
        self.assertIn("revoke_approval_record", capabilities["supports"]["canonical_tools"])
        self.assertIn("create_audit_attestation", capabilities["supports"]["canonical_tools"])
        self.assertIn("verify_audit_attestation", capabilities["supports"]["canonical_tools"])
        self.assertIn("get_runtime_diagnostics", capabilities["supports"]["canonical_tools"])
        self.assertIn(
            "generate_operator_runbook_bundle",
            capabilities["supports"]["canonical_tools"],
        )
        self.assertTrue(capabilities["supports"]["retrieval_catalog"])
        self.assertTrue(capabilities["supports"]["provider_generated_embeddings"])
        self.assertTrue(capabilities["supports"]["engine_managed_retrieval"])
        self.assertEqual(
            capabilities["supports"]["engine_managed_retrieval_state"],
            "bounded_current_tree_contract",
        )
        self.assertEqual(
            capabilities["supports"]["engine_managed_retrieval_binding_state"],
            "local_contract_scaffold",
        )
        self.assertEqual(
            capabilities["supports"]["engine_managed_retrieval_live_validation_state"],
            "current",
        )
        self.assertEqual(
            capabilities["supports"]["scratchbird_core_release_ceiling"],
            "bounded_current_tree",
        )
        self.assertEqual(
            capabilities["supports"]["scratchbird_runtime_modes"],
            [
                "listener_direct",
                "manager_proxy",
                "local_ipc",
                "embedded_local_only",
            ],
        )
        self.assertTrue(capabilities["supports"]["compile_repair"])
        self.assertTrue(capabilities["supports"]["durable_approval_evidence"])
        self.assertTrue(capabilities["supports"]["approval_record_workflow"])
        self.assertTrue(capabilities["supports"]["operational_controls"])
        self.assertTrue(capabilities["supports"]["cost_attribution"])
        self.assertTrue(capabilities["supports"]["graph_ops"])
        self.assertTrue(capabilities["supports"]["bridge_runtime"])
        self.assertTrue(capabilities["supports"]["server_policy_bound_authorization"])
        self.assertTrue(capabilities["supports"]["structured_runtime_logging"])
        self.assertTrue(capabilities["supports"]["operator_runbook_packaging"])
        self.assertTrue(capabilities["supports"]["audit_attestation"])
        self.assertIn(
            "third_party_hmac_sha256",
            capabilities["supports"]["audit_attestation_modes"],
        )
        self.assertEqual(
            capabilities["supports"]["operator_target_profiles"],
            ["listener_direct", "manager_proxy"],
        )
        self.assertEqual(
            capabilities["supports"]["remote_transports"],
            [
                "https_json_request_response",
                "https_sse_server_stream",
                "websocket_bidirectional",
            ],
        )
        self.assertIn("proxy_principal", capabilities["supports"]["remote_auth_types"])
        self.assertEqual(
            capabilities["supports"]["authorization_model"],
            "server_policy_grants_groups",
        )
        family_rows = {row["tool_family"]: row for row in capabilities["tool_families"]}
        self.assertIn("registry_governance", family_rows)
        self.assertEqual(family_rows["registry_governance"]["visibility_scope"], "admin_only")
        self.assertIn("operator_governance", family_rows)
        self.assertEqual(family_rows["operator_governance"]["visibility_scope"], "admin_only")
        self.assertIn("graph_ops", family_rows)
        self.assertIn("bridge_runtime", family_rows)
        self.assertEqual(family_rows["branch_ops"]["publication_state"], "hidden")
        self.assertIn("groups", capabilities["supports"]["security_context_fields"])
        self.assertIn("grants", capabilities["supports"]["security_context_fields"])
        self.assertEqual(
            capabilities["scratchbird_core_surface_packet"]["packet_version"],
            "2026-04-18",
        )
        self.assertEqual(
            capabilities["scratchbird_core_surface_packet"][
                "retrieval_metadata_discovery_packet"
            ]["required_semantics"],
            ["queryability_state", "metrics_confidence_class"],
        )
        profiles = {
            profile["profile_id"]: profile for profile in capabilities["interface_profiles"]
        }
        provider_profiles = {
            profile["profile_id"]: profile for profile in capabilities["provider_profiles"]
        }

        self.assertEqual(profiles["service_internal_v0"]["state"], "implemented")
        self.assertEqual(profiles["service_internal_v0"]["transport"], "in_process")
        self.assertEqual(profiles["mcp_local_v0"]["state"], "implemented")
        self.assertEqual(profiles["mcp_remote_v0"]["state"], "implemented")
        self.assertEqual(profiles["provider_tool_calling_v0"]["state"], "implemented")
        self.assertEqual(profiles["langchain_v0"]["state"], "implemented")
        self.assertEqual(profiles["llamaindex_v0"]["state"], "implemented")
        self.assertEqual(profiles["semantic_kernel_v0"]["state"], "implemented")
        self.assertEqual(profiles["streaming_async_v0"]["state"], "implemented")
        self.assertEqual(profiles["retrieval_ingest_v0"]["state"], "implemented")
        self.assertEqual(profiles["governance_certification_v0"]["state"], "implemented")
        self.assertIn(
            "execute_readonly_query",
            profiles["mcp_local_v0"]["operation_set"],
        )
        self.assertIn(
            "open_remote_session",
            profiles["mcp_remote_v0"]["operation_set"],
        )
        self.assertIn(
            "poll_remote_operation",
            profiles["streaming_async_v0"]["operation_set"],
        )
        self.assertIn(
            "add_embeddings",
            profiles["service_internal_v0"]["operation_set"],
        )
        self.assertIn(
            "validate_approval_evidence",
            profiles["governance_certification_v0"]["operation_set"],
        )
        self.assertIn(
            "generate_operator_runbook_bundle",
            profiles["governance_certification_v0"]["operation_set"],
        )
        self.assertIn(
            "get_runtime_diagnostics",
            profiles["mcp_local_v0"]["operation_set"],
        )
        self.assertIn(
            "create_audit_attestation",
            profiles["mcp_remote_v0"]["operation_set"],
        )
        self.assertIn("get_server_registry", profiles["service_internal_v0"]["operation_set"])
        self.assertIn("resolve_gateway_route", profiles["mcp_remote_v0"]["operation_set"])
        self.assertIn(
            "register_remote_server",
            profiles["governance_certification_v0"]["operation_set"],
        )
        self.assertEqual(provider_profiles["openai_tool_calling_v0"]["state"], "implemented")
        self.assertEqual(provider_profiles["anthropic_tool_use_v0"]["state"], "implemented")
        self.assertEqual(provider_profiles["gemini_function_calling_v0"]["state"], "implemented")

    def test_get_tool_descriptors_returns_catalog(self) -> None:
        catalog = self.service.get_tool_descriptors()
        names = {tool["tool_name"] for tool in catalog["tools"]}
        self.assertIn("execute_readonly_query", names)
        self.assertIn("get_tool_descriptors", names)
        self.assertIn("get_provider_profiles", names)
        self.assertIn("export_certification_manifest", names)
        self.assertIn("get_server_registry", names)
        self.assertIn("register_remote_server", names)
        self.assertIn("update_remote_server_lifecycle", names)
        self.assertIn("report_remote_server_health", names)
        self.assertIn("resolve_gateway_route", names)
        self.assertIn("open_remote_session", names)
        self.assertIn("invoke_remote_tool", names)
        self.assertIn("poll_remote_operation", names)
        self.assertIn("cancel_remote_operation", names)
        self.assertIn("create_vector_index", names)
        self.assertIn("add_generated_embeddings", names)
        self.assertIn("list_vector_indexes", names)
        self.assertIn("replay_audit_bundle", names)
        self.assertIn("list_audit_bundles", names)
        self.assertIn("validate_approval_evidence", names)
        self.assertIn("list_approval_records", names)
        self.assertIn("revoke_approval_record", names)
        self.assertIn("create_audit_attestation", names)
        self.assertIn("verify_audit_attestation", names)
        self.assertIn("get_runtime_diagnostics", names)
        self.assertIn("generate_operator_runbook_bundle", names)

    def test_get_provider_profiles_returns_catalog(self) -> None:
        catalog = self.service.get_provider_profiles()
        profiles = {profile["profile_id"]: profile for profile in catalog["profiles"]}
        self.assertEqual(profiles["openai_tool_calling_v0"]["state"], "implemented")
        self.assertFalse(profiles["openai_tool_calling_v0"]["streaming_support"])

    def test_server_registry_tracks_remote_lifecycle_and_gateway_resolution(self) -> None:
        registered = self.service.register_remote_server(
            server_label="remote-lab",
            interface_profile_ids=["mcp_remote_v0"],
            tool_families=["remote_session", "metadata"],
            routing_target="remote_server",
            security_context=self.admin_context,
            metadata={"region": "lab-a"},
        )
        remote_server_id = registered["entry"]["server_id"]

        self.assertTrue(remote_server_id.startswith("srv_"))
        self.assertEqual(registered["entry"]["routing_target"], "remote_server")
        self.assertEqual(registered["entry"]["health_state"], "unknown")

        health = self.service.report_remote_server_health(
            server_id=remote_server_id,
            health_state="healthy",
            security_context=self.admin_context,
            summary="reachable",
            metrics={"latency_ms": 8},
        )
        self.assertEqual(health["entry"]["health_state"], "healthy")

        routed = self.service.resolve_gateway_route(
            tool_name="open_remote_session",
            interface_profile_id="mcp_remote_v0",
            security_context=self.admin_context,
            preferred_server_id=remote_server_id,
        )
        self.assertEqual(routed["status"], "inventory_only")
        self.assertFalse(routed["proxy_execution_supported"])

        drained = self.service.update_remote_server_lifecycle(
            server_id=remote_server_id,
            action="drain",
            security_context=self.admin_context,
            reason="maintenance",
        )
        self.assertEqual(drained["entry"]["lifecycle_state"], "draining")

        routed_after_drain = self.service.resolve_gateway_route(
            tool_name="open_remote_session",
            interface_profile_id="mcp_remote_v0",
            security_context=self.admin_context,
            preferred_server_id=remote_server_id,
        )
        self.assertEqual(routed_after_drain["status"], "draining")

        catalog = self.service.get_server_registry(
            security_context=self.admin_context,
            include_hidden=True,
        )
        server_rows = {row["server_id"]: row for row in catalog["entries"]}
        self.assertIn(remote_server_id, server_rows)
        self.assertEqual(server_rows[remote_server_id]["lifecycle_state"], "draining")
        self.assertEqual(server_rows[remote_server_id]["health_state"], "healthy")

    def test_governance_workflow_supports_attestation_runbook_and_runtime_diagnostics(self) -> None:
        response = self.service.run_query(
            request_id="req_governance_1",
            dialect="native",
            query_text="SELECT 1",
            mode="ai_analysis",
            context={"security_context": dict(self.security_context)},
        )
        self.assertEqual(response.row_count, 200)

        bundles = self.service.list_audit_bundles(
            security_context=dict(self.security_context),
            limit=10,
        )
        self.assertEqual(bundles["bundle_count"], 1)
        bundle = bundles["bundles"][0]

        approval = self.service.validate_approval_evidence(
            approval_evidence={
                "approval_token": "approved-token",
                "approved_by": "operator_a",
            },
            security_context=dict(self.security_context),
            statement_hash="stmt_hash_1",
        )
        self.assertEqual(approval["validation_status"], "valid")

        listed_approvals = self.service.list_approval_records(
            security_context=dict(self.security_context),
        )
        self.assertEqual(listed_approvals["record_count"], 1)

        revoked = self.service.revoke_approval_record(
            approval_id=approval["approval_id"],
            reason="qa_revocation",
            security_context=dict(self.admin_context),
        )
        self.assertEqual(revoked["record"]["revoked_by"], self.admin_context["actor_id"])

        attested = self.service.create_audit_attestation(
            security_context=dict(self.admin_context),
            bundle=bundle,
        )
        self.assertEqual(attested["attestation"]["attestation_mode"], "hmac_sha256")

        verified = self.service.verify_audit_attestation(
            security_context=dict(self.security_context),
            bundle=bundle,
            attestation=attested["attestation"],
        )
        self.assertTrue(verified["verified"])

        diagnostics = self.service.get_runtime_diagnostics(
            security_context=dict(self.admin_context),
            max_recent_errors=5,
        )
        self.assertGreaterEqual(diagnostics["event_summary"]["total_events"], 1)
        self.assertEqual(diagnostics["approval_summary"]["total_records"], 1)

        runbook = self.service.generate_operator_runbook_bundle(
            security_context=dict(self.admin_context),
        )
        self.assertEqual(runbook["status"], "PASS")
        self.assertTrue(Path(runbook["files"]["summary"]).is_file())
        self.assertTrue(Path(runbook["files"]["runtime_diagnostics"]).is_file())
        self.assertTrue(Path(runbook["files"]["dashboard_manifest"]).is_file())
        self.assertIn("listener_direct", runbook["packages"])
        self.assertTrue(
            Path(self._tmp_dir.name, "structured-events.jsonl").read_text(encoding="utf-8")
        )

    def test_service_supports_third_party_attestation_and_target_package_selection(self) -> None:
        self.service.execute_readonly_query(
            request_id="req_third_party_attestation",
            dialect="native",
            query_text="SELECT 1",
            security_context=dict(self.security_context),
            options={"max_rows": 1},
        )
        bundle = self.service.latest_audit_bundle()
        assert bundle is not None

        attested = self.service.create_audit_attestation(
            security_context=dict(self.admin_context),
            bundle=bundle,
            attestation_mode="third_party_hmac_sha256",
            key_id="approval-key-1",
            metadata={"approval_reference": "APR-1001"},
        )
        self.assertEqual(attested["attestation"]["attestation_mode"], "third_party_hmac_sha256")
        self.assertEqual(attested["attestation"]["attestor_id"], "external-attestor")
        self.assertTrue(attested["attestation"]["external_reference"].startswith("https://approvals.example.com/"))

        verified = self.service.verify_audit_attestation(
            security_context=dict(self.security_context),
            bundle=bundle,
            attestation=attested["attestation"],
        )
        self.assertTrue(verified["verified"])

        runbook = self.service.generate_operator_runbook_bundle(
            security_context=dict(self.admin_context),
            target_profiles=["manager_proxy"],
        )
        self.assertEqual(list(runbook["packages"].keys()), ["manager_proxy"])

    def test_runtime_diagnostics_require_admin_context(self) -> None:
        with self.assertRaises(ToolContractError) as ctx:
            self.service.get_runtime_diagnostics(security_context=dict(self.security_context))
        self.assertEqual(ctx.exception.error_code, "E_POLICY_DENY")

    def test_register_remote_server_requires_admin_authority(self) -> None:
        regular_context = {
            "tenant_id": "tenant_a",
            "actor_id": "actor_a",
            "roles": ["analyst"],
            "session_id": "sess_regular",
            "context_version": 1,
        }
        with self.assertRaises(ToolContractError) as ctx:
            self.service.register_remote_server(
                server_label="remote-lab",
                interface_profile_ids=["mcp_remote_v0"],
                tool_families=["remote_session"],
                routing_target="remote_server",
                security_context=regular_context,
            )
        self.assertEqual(ctx.exception.error_code, "E_POLICY_DENY")

    def test_invoke_provider_tool_openai_profile(self) -> None:
        response = self.service.invoke_provider_tool(
            provider_profile_id="openai_tool_calling_v0",
            payload={
                "request_id": "req_provider_openai",
                "id": "call_provider_openai",
                "function": {
                    "name": "execute_readonly_query",
                    "arguments": (
                        '{"dialect":"native","query_text":"SELECT 1","security_context":'
                        '{"tenant_id":"tenant_a","actor_id":"actor_a"},"options":{"max_rows":1}}'
                    ),
                },
            },
        )
        self.assertEqual(response["status"], "success")
        self.assertEqual(response["provider_profile_id"], "openai_tool_calling_v0")
        self.assertEqual(response["result"]["row_count"], 1)

    def test_invoke_provider_tool_rejects_unimplemented_profile(self) -> None:
        response = self.service.invoke_provider_tool(
            provider_profile_id="unknown_profile_v0",
            payload={
                "request_id": "req_provider_anthropic",
                "id": "call_provider_anthropic",
                "name": "execute_readonly_query",
                "input": {
                    "dialect": "native",
                    "query_text": "SELECT 1",
                    "security_context": {"tenant_id": "tenant_a", "actor_id": "actor_a"},
                },
            },
        )
        self.assertEqual(response["status"], "error")
        self.assertEqual(response["error"]["error_code"], "E_PROVIDER_CONTRACT_UNSUPPORTED")

    def test_invoke_provider_tool_anthropic_profile(self) -> None:
        response = self.service.invoke_provider_tool(
            provider_profile_id="anthropic_tool_use_v0",
            payload={
                "request_id": "req_provider_anthropic",
                "id": "call_provider_anthropic",
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
        )
        self.assertEqual(response["status"], "success")
        self.assertEqual(response["provider_profile_id"], "anthropic_tool_use_v0")
        self.assertEqual(response["result"]["row_count"], 1)

    def test_invoke_provider_tool_gemini_profile(self) -> None:
        response = self.service.invoke_provider_tool(
            provider_profile_id="gemini_function_calling_v0",
            payload={
                "request_id": "req_provider_gemini",
                "functionCall": {
                    "id": "call_provider_gemini",
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
                },
            },
        )
        self.assertEqual(response["status"], "success")
        self.assertEqual(response["provider_profile_id"], "gemini_function_calling_v0")
        self.assertEqual(response["result"]["row_count"], 1)

    def test_remote_session_invocation_binds_session_security_context(self) -> None:
        service = build_default_service(
            settings=RuntimeSettings(remote_mcp_auth_token="remote-secret")
        )
        opened = service.open_remote_session(
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
                },
                "security_context_hint": {
                    "tenant_id": "tenant_remote",
                    "actor_id": "actor_remote",
                    "roles": ["remote_reader"],
                    "groups": ["ops"],
                    "grants": ["graph:read"],
                    "session_id": "sess_remote",
                    "context_version": 1,
                },
            }
        )
        self.assertTrue(opened["server_id"].startswith("srv_"))
        self.assertTrue(opened["capability_manifest_id"].startswith("mf_"))

        response = service.invoke_remote_tool(
            session_id=opened["session_id"],
            request_id="req_remote_query",
            method="execute_readonly_query",
            params={
                "dialect": "native",
                "query_text": "SELECT 1",
                "security_context": {
                    "tenant_id": "tenant_override",
                    "actor_id": "actor_override",
                },
                "context": {
                    "security_context": {
                        "tenant_id": "tenant_override_ctx",
                        "actor_id": "actor_override_ctx",
                    }
                },
                "client_capabilities": {"requested_transport": "websocket_bidirectional"},
                "options": {"max_rows": 1},
            },
        )

        self.assertEqual(response["status"], "success")
        self.assertEqual(response["result"]["row_count"], 1)
        bundle = service.latest_audit_bundle()
        self.assertIsNotNone(bundle)
        assert bundle is not None
        self.assertEqual(bundle["tenant_id"], "tenant_remote")
        self.assertEqual(bundle["actor_id"], "actor_remote")

    def test_remote_session_accepts_preauthenticated_context_without_local_token(self) -> None:
        service = build_default_service()
        opened = service.open_remote_session(
            {
                "request_id": "req_remote_preauth_open",
                "interface_profile_id": "mcp_remote_v0",
                "protocol_version": "v0",
                "requested_transport": "websocket_bidirectional",
                "client_id": "remote-client",
                "client_version": "0.0.1",
                "auth_envelope": {
                    "auth_type": "preauthenticated_context",
                    "security_context": {
                        "tenant_id": "tenant_remote",
                        "actor_id": "actor_remote",
                        "groups": ["ops"],
                        "grants": ["graph:read"],
                    },
                },
            }
        )
        self.assertEqual(opened["negotiated_transport"], "websocket_bidirectional")

    def test_remote_session_stream_request_fails_closed(self) -> None:
        service = build_default_service(
            settings=RuntimeSettings(remote_mcp_auth_token="remote-secret")
        )
        opened = service.open_remote_session(
            {
                "client_id": "remote-client",
                "client_version": "0.0.1",
                "auth_envelope": {
                    "auth_type": "bearer",
                    "token": "remote-secret",
                    "security_context": {
                        "tenant_id": "tenant_remote",
                        "actor_id": "actor_remote",
                    },
                },
            }
        )

        response = service.invoke_remote_tool(
            session_id=opened["session_id"],
            request_id="req_remote_stream",
            method="execute_readonly_query",
            params={"dialect": "native", "query_text": "SELECT 1"},
            stream_requested=True,
        )

        self.assertEqual(response["status"], "error")
        self.assertEqual(response["error"]["error_code"], "E_STREAM_NOT_SUPPORTED")

    def test_remote_streaming_lifecycle_exposes_events_and_continuation(self) -> None:
        service = build_default_service(
            settings=RuntimeSettings(remote_mcp_auth_token="remote-secret")
        )
        opened = service.open_remote_session(
            {
                "request_id": "req_remote_stream_open",
                "requested_transport": "https_sse_server_stream",
                "client_id": "remote-client",
                "client_version": "0.0.1",
                "auth_envelope": {
                    "auth_type": "oauth2_access_token",
                    "access_token": "remote-secret",
                    "security_context": {
                        "tenant_id": "tenant_remote",
                        "actor_id": "actor_remote",
                        "roles": ["remote_reader"],
                    },
                },
            }
        )

        response = service.invoke_remote_tool(
            session_id=opened["session_id"],
            request_id="req_remote_stream_exec",
            method="execute_readonly_query",
            params={
                "dialect": "native",
                "query_text": "SELECT 1",
                "options": {"max_rows": 1},
            },
            stream_requested=True,
        )

        self.assertEqual(response["status"], "success")
        self.assertEqual(response["operation_state"], "completed")
        self.assertTrue(str(response["operation_id"]).startswith("op_"))
        self.assertIsNone(response["result"])
        self.assertEqual(response["stream_channel"], f"stream:{response['operation_id']}")
        self.assertIsNotNone(response["continuation_token"])

        events = service.poll_remote_operation(
            session_id=opened["session_id"],
            operation_id=response["operation_id"],
        )
        self.assertEqual(events["operation_state"], "completed")
        self.assertTrue(events["terminal"])
        self.assertEqual(
            [event["event_type"] for event in events["events"]],
            ["accepted", "progress", "completed"],
        )
        self.assertEqual(events["events"][-1]["payload"]["result"]["row_count"], 1)

        empty_poll = service.poll_remote_operation(
            session_id=opened["session_id"],
            operation_id=response["operation_id"],
            continuation_token=events["continuation_token"],
        )
        self.assertEqual(empty_poll["events"], [])
        self.assertTrue(empty_poll["terminal"])

    def test_cancel_remote_operation_reports_already_terminal(self) -> None:
        service = build_default_service(
            settings=RuntimeSettings(remote_mcp_auth_token="remote-secret")
        )
        opened = service.open_remote_session(
            {
                "requested_transport": "https_sse_server_stream",
                "client_id": "remote-client",
                "client_version": "0.0.1",
                "auth_envelope": {
                    "auth_type": "bearer",
                    "token": "remote-secret",
                    "security_context": {
                        "tenant_id": "tenant_remote",
                        "actor_id": "actor_remote",
                    },
                },
            }
        )

        response = service.invoke_remote_tool(
            session_id=opened["session_id"],
            request_id="req_remote_stream_cancel",
            method="execute_readonly_query",
            params={"dialect": "native", "query_text": "SELECT 1"},
            stream_requested=True,
        )

        cancelled = service.cancel_remote_operation(
            session_id=opened["session_id"],
            operation_id=response["operation_id"],
            request_id="req_cancel_terminal",
            reason="user_cancelled",
        )
        self.assertEqual(cancelled["status"], "already_terminal")
        self.assertEqual(cancelled["operation_state"], "completed")

    def test_remote_session_close_invalidates_future_invocation(self) -> None:
        service = build_default_service(
            settings=RuntimeSettings(remote_mcp_auth_token="remote-secret")
        )
        opened = service.open_remote_session(
            {
                "client_id": "remote-client",
                "client_version": "0.0.1",
                "auth_envelope": {
                    "auth_type": "bearer",
                    "token": "remote-secret",
                    "security_context": {
                        "tenant_id": "tenant_remote",
                        "actor_id": "actor_remote",
                    },
                },
            }
        )

        closed = service.close_remote_session(
            session_id=opened["session_id"],
            request_id="req_remote_close",
        )
        self.assertEqual(closed["status"], "closed")

        response = service.invoke_remote_tool(
            session_id=opened["session_id"],
            request_id="req_remote_after_close",
            method="execute_readonly_query",
            params={"dialect": "native", "query_text": "SELECT 1"},
        )
        self.assertEqual(response["status"], "error")
        self.assertEqual(response["error"]["error_code"], "E_SESSION_REQUIRED")

    def test_validate_approval_evidence_returns_durable_record(self) -> None:
        validation = self.service.validate_approval_evidence(
            approval_evidence={"approval_token": '{"tenant_id":"tenant_a","actor_id":"actor_a"}'},
            security_context=self.security_context,
            statement_hash="hash_stmt_1",
        )
        self.assertEqual(validation["validation_status"], "valid")
        self.assertTrue(validation["approval_id"].startswith("appr_"))
        self.assertEqual(validation["tenant_id"], "tenant_a")
        self.assertEqual(validation["actor_id"], "actor_a")
        self.assertEqual(validation["statement_hash"], "hash_stmt_1")
        self.assertEqual(validation["use_count"], 1)

    def test_replay_audit_bundle_reports_match(self) -> None:
        response = self.service.run_query(
            request_id="req_replay_bundle",
            dialect="native",
            query_text="SELECT 1",
            mode="ai_analysis",
            context={"security_context": self.security_context},
        )
        bundle = self.service.latest_audit_bundle()
        self.assertIsNotNone(bundle)
        assert bundle is not None

        replay = self.service.replay_audit_bundle(
            bundle=bundle,
            security_context=self.security_context,
            expected_policy_decision="allow",
            expected_policy_rule_id="MODE-ALLOW-READ-001",
            expected_plan_hash=bundle["plan_hash"],
        )
        self.assertEqual(replay["request_id"], response.request_id)
        self.assertTrue(replay["matches"])
        self.assertEqual(replay["outcome"], "REPLAY_MATCH")

    def test_run_query_emits_allow_audit_bundle(self) -> None:
        response = self.service.run_query(
            request_id="req_audit_allow",
            dialect="native",
            query_text="SELECT 1",
            mode="ai_analysis",
            context={
                "security_context": {
                    "tenant_id": "tenant_a",
                    "actor_id": "actor_a",
                    "roles": ["analyst"],
                    "session_id": "sess_1",
                    "context_version": 1,
                }
            },
        )
        bundle = self.service.latest_audit_bundle()
        self.assertIsNotNone(bundle)
        assert bundle is not None
        self.assertEqual(bundle["request_id"], response.request_id)
        self.assertEqual(bundle["policy_decision"], "allow")
        self.assertEqual(bundle["tenant_id"], "tenant_a")

    def test_run_query_emits_deny_audit_bundle(self) -> None:
        with self.assertRaises(PolicyDeniedError):
            self.service.run_query(
                request_id="req_audit_deny",
                dialect="native",
                query_text="UPDATE t SET c=1",
                mode="ai_analysis",
                context={
                    "security_context": {
                        "tenant_id": "tenant_a",
                        "actor_id": "actor_a",
                        "roles": ["analyst"],
                        "session_id": "sess_1",
                        "context_version": 1,
                    }
                },
            )
        bundle = self.service.latest_audit_bundle()
        self.assertIsNotNone(bundle)
        assert bundle is not None
        self.assertEqual(bundle["request_id"], "req_audit_deny")
        self.assertEqual(bundle["policy_decision"], "deny")
        self.assertEqual(bundle["error_code"], "E_POLICY_DENY")

    def test_execute_readonly_query_canonical_tool(self) -> None:
        response = self.service.execute_readonly_query(
            request_id="req_tool_read",
            dialect="native",
            query_text="SELECT 1",
            security_context={
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "roles": ["analyst"],
                "session_id": "sess_1",
                "context_version": 1,
            },
            options={"max_rows": 1},
        )
        self.assertEqual(response["row_count"], 1)
        self.assertTrue(response["compile_artifact_id"].startswith("cmp_"))

    def test_vector_and_hybrid_search_engine_free(self) -> None:
        self.service.add_embeddings(
            index_id="idx_docs",
            dimension=3,
            records=[
                {
                    "vector_id": "doc-1#1",
                    "embedding": [0.1, 0.2, 0.3],
                    "metadata": {"document_id": "doc-1", "text": "north overdue invoice"},
                }
            ],
            security_context={
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "roles": ["analyst"],
                "session_id": "sess_1",
                "context_version": 1,
            },
        )
        vector = self.service.vector_search(
            index_id="idx_docs",
            query_embedding=[0.1, 0.2, 0.3],
            top_k=5,
            security_context={
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "roles": ["analyst"],
                "session_id": "sess_1",
                "context_version": 1,
            },
        )
        self.assertEqual(vector["results"][0]["metadata"]["document_id"], "doc-1")

        hybrid = self.service.hybrid_search(
            dialect="native",
            query_text="overdue north invoice",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id="idx_docs",
            top_k=5,
            security_context={
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "roles": ["analyst"],
                "session_id": "sess_1",
                "context_version": 1,
            },
            sql_filter={"metadata": {"document_id": "doc-1"}},
        )
        self.assertEqual(hybrid["results"][0]["document_id"], "doc-1")
        self.assertEqual(hybrid["query_plan"]["planner_mode"], "local_structured_filter")

    def test_retrieval_catalog_and_generated_ingest_service_surface(self) -> None:
        created = self.service.create_vector_index(
            index_id="idx_catalog",
            dimension=4,
            security_context=self.security_context,
        )
        self.assertEqual(created["index"]["state"], "provisioning")

        generated = self.service.add_generated_embeddings(
            index_id="idx_generated",
            dimension=4,
            records=[
                {
                    "vector_id": "doc-generated#1",
                    "text": "north overdue invoice",
                    "metadata": {"document_id": "doc-generated"},
                }
            ],
            provider_config={
                "provider_profile_id": "openai_embeddings_v1",
                "model": "text-embedding-3-small",
                "api_key": "secret-inline",
            },
            security_context=self.security_context,
        )
        self.assertEqual(generated["provider_ref"], "inline:redacted")
        self.assertEqual(generated["index"]["state"], "ready")

        listed = self.service.list_vector_indexes(security_context=self.security_context)
        listed_ids = {row["index_id"] for row in listed["indexes"]}
        self.assertIn("idx_catalog", listed_ids)
        self.assertIn("idx_generated", listed_ids)

        described = self.service.describe_vector_index(
            index_id="idx_generated",
            security_context=self.security_context,
        )
        self.assertEqual(described["index"]["profile_id"], "provider_generated_embeddings_v0")
        self.assertEqual(described["index"]["provider_ref"], "inline:redacted")

        reindexed = self.service.reindex_vector_index(
            index_id="idx_generated",
            security_context=self.security_context,
        )
        self.assertEqual(reindexed["index"]["state"], "ready")

        deleted = self.service.delete_vector_index(
            index_id="idx_catalog",
            security_context=self.security_context,
        )
        self.assertEqual(deleted["index"]["state"], "deleted")

    def test_engine_managed_retrieval_profile_allows_where_pushdown(self) -> None:
        created = self.service.create_vector_index(
            index_id="idx_managed",
            dimension=3,
            security_context=self.security_context,
            profile_id="engine_managed_retrieval_v0",
        )
        self.assertEqual(created["index"]["profile_id"], "engine_managed_retrieval_v0")
        self.assertEqual(created["index"]["backend_kind"], "engine_managed_contract_scaffold")

        self.service.add_embeddings(
            index_id="idx_managed",
            dimension=3,
            records=[
                {
                    "vector_id": "doc-managed#1",
                    "embedding": [0.1, 0.2, 0.3],
                    "metadata": {
                        "document_id": "doc-managed",
                        "status": "OVERDUE",
                        "text": "north overdue invoice",
                    },
                }
            ],
            security_context=self.security_context,
        )

        result = self.service.hybrid_search(
            dialect="native",
            query_text="overdue north invoice",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id="idx_managed",
            top_k=5,
            security_context=self.security_context,
            sql_filter={"where": "status = 'OVERDUE'"},
        )
        self.assertEqual(result["results"][0]["document_id"], "doc-managed")
        self.assertEqual(result["query_plan"]["planner_mode"], "engine_pushdown")

    def test_compile_artifact_id_is_deterministic(self) -> None:
        context = {
            "security_context": {
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "roles": ["analyst"],
                "session_id": "sess_1",
                "context_version": 1,
            }
        }
        first = self.service.compile_query(
            dialect="native",
            query_text="SELECT 1",
            context=context,
        )
        second = self.service.compile_query(
            dialect="native",
            query_text="SELECT   1",
            context=context,
        )
        self.assertEqual(first.compile_artifact_id, second.compile_artifact_id)

    def test_execution_artifact_id_changes_with_attempt_index(self) -> None:
        context = {
            "security_context": {
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "roles": ["analyst"],
                "session_id": "sess_1",
                "context_version": 1,
            }
        }
        compiled = self.service.compile_query(
            dialect="native",
            query_text="SELECT 1",
            context=context,
        )
        first = self.service.execute_compiled(
            compile_artifact_id=compiled.compile_artifact_id,
            options={"max_rows": 1},
            mode="ai_analysis",
        )
        second = self.service.execute_compiled(
            compile_artifact_id=compiled.compile_artifact_id,
            options={"max_rows": 1},
            mode="ai_analysis",
        )
        self.assertNotEqual(first.execution_artifact_id, second.execution_artifact_id)


if __name__ == "__main__":
    unittest.main()
