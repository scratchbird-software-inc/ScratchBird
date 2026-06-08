# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import os
import unittest

from scratchbird_ai.settings import RuntimeSettings, load_runtime_settings


class SettingsTests(unittest.TestCase):
    def test_runtime_settings_mode_normalization(self) -> None:
        settings = RuntimeSettings(adapter_mode="HYBRID")
        self.assertEqual(settings.normalized_mode(), "hybrid")

    def test_runtime_settings_invalid_mode_falls_back_to_mock(self) -> None:
        settings = RuntimeSettings(adapter_mode="invalid")
        self.assertEqual(settings.normalized_mode(), "mock")

    def test_load_runtime_settings_from_env(self) -> None:
        env = {
            "SCRATCHBIRD_AI_ADAPTER_MODE": "hybrid",
            "SCRATCHBIRD_AI_HTTP_BASE_URL": "http://localhost:9999",
            "SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC": "3.5",
            "SCRATCHBIRD_AI_HTTP_DIALECTS": "native",
            "SCRATCHBIRD_AI_RETRIEVAL_CATALOG_PATH": "/tmp/scratchbird-ai-retrieval.json",
            "SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH": "/tmp/scratchbird-ai-events.jsonl",
            "SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR": "/tmp/scratchbird-ai-runbook",
            "SCRATCHBIRD_AI_OPERATOR_TARGET_PROFILES": "listener_direct,manager_proxy",
            "SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE": "external_reference",
            "SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET": "attestation-secret",
            "SCRATCHBIRD_AI_AUDIT_ATTESTATION_ATTESTOR_ID": "qa-attestor",
            "SCRATCHBIRD_AI_AUDIT_DELEGATED_ATTESTATION_SECRET": "delegated-secret",
            "SCRATCHBIRD_AI_AUDIT_DELEGATED_ATTESTATION_ATTESTOR_ID": "external-attestor",
            "SCRATCHBIRD_AI_AUDIT_EXTERNAL_REFERENCE_BASE_URL": "https://approvals.example.com",
            "SCRATCHBIRD_AI_REMOTE_MCP_AUTH_TOKEN": "secret-token",
            "SCRATCHBIRD_AI_REMOTE_MCP_SESSION_TTL_SEC": "1200",
            "SCRATCHBIRD_AI_REMOTE_MCP_HEARTBEAT_INTERVAL_SEC": "45",
            "SCRATCHBIRD_AI_REMOTE_MCP_PROTOCOL_VERSIONS": "v0,v1-preview",
            "SCRATCHBIRD_AI_REMOTE_MCP_SUPPORTED_AUTH_TYPES": (
                "bearer,proxy_principal,preauthenticated_context"
            ),
            "SCRATCHBIRD_AI_REMOTE_MCP_SUPPORTED_TRANSPORTS": (
                "https_json_request_response,https_sse_server_stream,websocket_bidirectional"
            ),
        }

        backup = {key: os.environ.get(key) for key in env}
        try:
            os.environ.update(env)
            settings = load_runtime_settings()
        finally:
            for key, value in backup.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value

        self.assertEqual(settings.normalized_mode(), "hybrid")
        self.assertEqual(settings.http_base_url, "http://localhost:9999")
        self.assertEqual(settings.http_timeout_sec, 3.5)
        self.assertEqual(settings.http_dialects, ("native",))
        self.assertEqual(settings.retrieval_catalog_path, "/tmp/scratchbird-ai-retrieval.json")
        self.assertEqual(settings.structured_event_log_path, "/tmp/scratchbird-ai-events.jsonl")
        self.assertEqual(settings.operator_bundle_output_dir, "/tmp/scratchbird-ai-runbook")
        self.assertEqual(settings.operator_target_profiles, ("listener_direct", "manager_proxy"))
        self.assertEqual(settings.audit_attestation_mode, "external_reference")
        self.assertEqual(settings.audit_attestation_secret, "attestation-secret")
        self.assertEqual(settings.audit_attestation_attestor_id, "qa-attestor")
        self.assertEqual(settings.audit_attestation_delegated_secret, "delegated-secret")
        self.assertEqual(
            settings.audit_attestation_delegated_attestor_id,
            "external-attestor",
        )
        self.assertEqual(
            settings.audit_attestation_external_reference_base_url,
            "https://approvals.example.com",
        )
        self.assertEqual(settings.remote_mcp_auth_token, "secret-token")
        self.assertEqual(settings.remote_mcp_session_ttl_sec, 1200)
        self.assertEqual(settings.remote_mcp_heartbeat_interval_sec, 45)
        self.assertEqual(settings.remote_mcp_protocol_versions, ("v0", "v1-preview"))
        self.assertEqual(
            settings.remote_mcp_supported_auth_types,
            ("bearer", "proxy_principal", "preauthenticated_context"),
        )
        self.assertEqual(
            settings.remote_mcp_supported_transports,
            (
                "https_json_request_response",
                "https_sse_server_stream",
                "websocket_bidirectional",
            ),
        )


if __name__ == "__main__":
    unittest.main()
