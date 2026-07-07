# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.compatibility import build_compatibility_manifest
from scratchbird_ai.environment_manifest import build_certification_manifest
from scratchbird_ai.settings import RuntimeSettings


class EnvironmentManifestTests(unittest.TestCase):
    def test_build_certification_manifest_redacts_secret_values(self) -> None:
        settings = RuntimeSettings(
            adapter_mode="hybrid",
            http_base_url="http://127.0.0.1:3095",
            http_api_token="secret-token",
            retrieval_catalog_path="/tmp/retrieval.json",
            structured_event_log_path="/tmp/runtime-events.jsonl",
            operator_bundle_output_dir="/tmp/operator-runbook",
            operator_target_profiles=("listener_direct", "manager_proxy"),
            audit_attestation_mode="hmac_sha256",
            audit_attestation_secret="attestation-secret",
            audit_attestation_attestor_id="qa-attestor",
            audit_attestation_delegated_secret="delegated-secret",
            audit_attestation_delegated_attestor_id="external-attestor",
            audit_attestation_external_reference_base_url="https://approvals.example.com",
            remote_mcp_auth_token="remote-secret",
            remote_mcp_supported_auth_types=("bearer", "proxy_principal"),
            remote_mcp_supported_transports=("https_json_request_response",),
        )
        compatibility_manifest = build_compatibility_manifest(
            adapter_mode="hybrid",
            matrix_version="test-matrix",
            runtime_settings=settings,
        )

        manifest = build_certification_manifest(
            settings=settings,
            adapter_mode="hybrid",
            matrix_version="test-matrix",
            compatibility_manifest=compatibility_manifest,
        )

        self.assertEqual(manifest["adapter_mode"], "hybrid")
        self.assertEqual(manifest["runtime_configuration"]["http_base_url"], "http://127.0.0.1:3095")
        self.assertTrue(manifest["runtime_configuration"]["http_api_token_present"])
        self.assertTrue(manifest["runtime_configuration"]["remote_mcp_auth_token_present"])
        self.assertEqual(
            manifest["runtime_configuration"]["structured_event_log_path"],
            "/tmp/runtime-events.jsonl",
        )
        self.assertEqual(
            manifest["runtime_configuration"]["operator_bundle_output_dir"],
            "/tmp/operator-runbook",
        )
        self.assertEqual(
            manifest["runtime_configuration"]["operator_target_profiles"],
            ["listener_direct", "manager_proxy"],
        )
        self.assertEqual(
            manifest["runtime_configuration"]["audit_attestation_mode"],
            "hmac_sha256",
        )
        self.assertTrue(
            manifest["runtime_configuration"]["audit_attestation_secret_present"]
        )
        self.assertTrue(
            manifest["runtime_configuration"]["audit_attestation_delegated_secret_present"]
        )
        self.assertEqual(
            manifest["runtime_configuration"]["audit_attestation_attestor_id"],
            "qa-attestor",
        )
        self.assertEqual(
            manifest["runtime_configuration"]["audit_attestation_delegated_attestor_id"],
            "external-attestor",
        )
        self.assertEqual(
            manifest["runtime_configuration"]["audit_attestation_external_reference_base_url"],
            "https://approvals.example.com",
        )
        self.assertEqual(
            manifest["runtime_configuration"]["remote_mcp_supported_auth_types"],
            ["bearer", "proxy_principal"],
        )
        self.assertEqual(manifest["runtime_configuration"]["http_retry_attempts"], 1)
        self.assertNotIn("secret-token", str(manifest))
        self.assertNotIn("remote-secret", str(manifest))
        self.assertNotIn("attestation-secret", str(manifest))
        self.assertTrue(manifest["supported_interface_profiles"])
        self.assertTrue(manifest["supported_provider_profiles"])
        self.assertEqual(
            manifest["scratchbird_core_surface_packet"]["packet_version"],
            "2026-04-18",
        )
        self.assertEqual(
            manifest["scratchbird_core_surface_packet"]["runtime_mode_truth_packet"][
                "admitted_modes"
            ][2]["mode_id"],
            "local_ipc",
        )


if __name__ == "__main__":
    unittest.main()
