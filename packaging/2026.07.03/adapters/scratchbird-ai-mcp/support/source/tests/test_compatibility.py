# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.service import build_default_service
from scratchbird_ai.settings import RuntimeSettings


class CompatibilityTests(unittest.TestCase):
    def setUp(self) -> None:
        self.service = build_default_service()

    def test_get_compatibility_manifest_is_machine_readable(self) -> None:
        manifest = self.service.get_compatibility_manifest()
        self.assertEqual(manifest["manifest_version"], "2026-03-07")
        self.assertEqual(manifest["release_version"], "0.1.0")
        self.assertTrue(manifest["interface_profiles"])
        self.assertTrue(manifest["transport_support"])
        self.assertEqual(
            manifest["scratchbird_core_surface_packet"]["packet_version"],
            "2026-04-18",
        )
        self.assertEqual(
            manifest["scratchbird_runtime_modes"][1]["component"],
            "manager_proxy",
        )
        self.assertEqual(
            manifest["scratchbird_retrieval_metadata_discovery"]["catalog_namespace"],
            "opensearch_meta",
        )
        transports = {row["component"]: row for row in manifest["transport_support"]}
        self.assertEqual(transports["https_json_request_response"]["support_state"], "supported")
        self.assertEqual(transports["https_sse_server_stream"]["support_state"], "supported")
        self.assertEqual(transports["websocket_bidirectional"]["support_state"], "supported")
        auth_support = {row["component"]: row for row in manifest["remote_session_auth_support"]}
        self.assertEqual(auth_support["proxy_principal"]["support_state"], "supported")
        control_support = {
            row["component"]: row for row in manifest["native_control_surface_support"]
        }
        self.assertEqual(control_support["graph_ops"]["support_state"], "supported")

    def test_export_certification_manifest_is_machine_readable(self) -> None:
        manifest = self.service.export_certification_manifest()
        self.assertEqual(manifest["manifest_version"], "2026-03-07")
        self.assertEqual(manifest["release_version"], "0.1.0")
        self.assertTrue(manifest["environment_descriptor"])
        self.assertTrue(manifest["runtime_configuration"])
        self.assertTrue(manifest["compatibility_manifest"])
        self.assertEqual(
            manifest["scratchbird_core_surface_packet"]["verification_state"][
                "ai_repo_live_validation_state"
            ],
            "current",
        )

    def test_negotiate_compatibility_supports_local_service_profile(self) -> None:
        response = self.service.negotiate_compatibility(
            {
                "request_id": "req_compat_ok",
                "interface_profile_id": "service_internal_v0",
                "requested_profile_version": "v0",
                "requested_transport": "in_process",
                "client_component_versions": {"scratchbird_ai": "0.1.0"},
            }
        )
        self.assertEqual(response["negotiation_status"], "supported")
        self.assertIsNone(response["error"])

    def test_negotiate_compatibility_supports_remote_profile(self) -> None:
        response = self.service.negotiate_compatibility(
            {
                "request_id": "req_compat_blocked",
                "interface_profile_id": "mcp_remote_v0",
                "requested_profile_version": "v0",
                "requested_transport": "websocket_bidirectional",
            }
        )
        self.assertEqual(response["negotiation_status"], "supported")
        self.assertIsNone(response["error"])
        self.assertEqual(response["resolved_transport"], "websocket_bidirectional")

    def test_negotiate_compatibility_blocks_unsupported_server_version(self) -> None:
        service = build_default_service(
            settings=RuntimeSettings(
                supported_server_versions=("sb-native-1",),
                supported_parser_compiler_versions=("pc-1",),
                supported_driver_runtime_versions=("builtin",),
            )
        )
        response = service.negotiate_compatibility(
            {
                "request_id": "req_compat_server_version",
                "interface_profile_id": "service_internal_v0",
                "requested_profile_version": "v0",
                "requested_transport": "in_process",
                "server_component_versions": {
                    "scratchbird_server": "sb-native-2",
                },
            }
        )
        self.assertEqual(response["negotiation_status"], "blocked")
        self.assertEqual(response["error"]["error_code"], "E_SERVER_RUNTIME_UNSUPPORTED")

    def test_negotiate_compatibility_blocks_unsupported_parser_compiler_version(self) -> None:
        service = build_default_service(
            settings=RuntimeSettings(
                supported_server_versions=("sb-native-1",),
                supported_parser_compiler_versions=("pc-1",),
                supported_driver_runtime_versions=("builtin",),
            )
        )
        response = service.negotiate_compatibility(
            {
                "request_id": "req_compat_parser_version",
                "interface_profile_id": "service_internal_v0",
                "requested_profile_version": "v0",
                "requested_transport": "in_process",
                "server_component_versions": {
                    "native_parser_compiler": "pc-2",
                },
            }
        )
        self.assertEqual(response["negotiation_status"], "blocked")
        self.assertEqual(response["error"]["error_code"], "E_PARSER_COMPILER_UNSUPPORTED")

    def test_negotiate_compatibility_blocks_unsupported_driver_runtime_version(self) -> None:
        service = build_default_service(
            settings=RuntimeSettings(
                supported_server_versions=("sb-native-1",),
                supported_parser_compiler_versions=("pc-1",),
                supported_driver_runtime_versions=("builtin",),
            )
        )
        response = service.negotiate_compatibility(
            {
                "request_id": "req_compat_driver_runtime",
                "interface_profile_id": "mcp_local_v0",
                "requested_profile_version": "v0",
                "requested_transport": "stdio_jsonrpc",
                "driver_runtime_versions": {
                    "mcp_local_runtime": "nonbuiltin",
                },
            }
        )
        self.assertEqual(response["negotiation_status"], "blocked")
        self.assertEqual(response["error"]["error_code"], "E_DRIVER_RUNTIME_UNSUPPORTED")

    def test_compile_query_accepts_supported_remote_client_request(self) -> None:
        compiled = self.service.compile_query(
            dialect="native",
            query_text="SELECT 1",
            context={
                "security_context": {
                    "tenant_id": "tenant_a",
                    "actor_id": "actor_a",
                    "roles": ["analyst"],
                    "session_id": "sess_1",
                    "context_version": 1,
                },
                "client_capabilities": {
                    "interface_profile_id": "mcp_remote_v0",
                    "requested_profile_version": "v0",
                    "requested_transport": "https_json_request_response",
                },
            },
        )
        self.assertEqual(compiled.statement_kind, "read")
        self.assertTrue(compiled.compile_artifact_id.startswith("cmp_"))


if __name__ == "__main__":
    unittest.main()
