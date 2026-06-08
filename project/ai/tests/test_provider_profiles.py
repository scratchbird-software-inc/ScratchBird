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


class ProviderProfileTests(unittest.TestCase):
    def setUp(self) -> None:
        self.service = build_default_service()

    def test_provider_profile_catalog_marks_all_child_profiles_implemented(self) -> None:
        catalog = self.service.get_provider_profiles()
        profiles = {profile["profile_id"]: profile for profile in catalog["profiles"]}
        self.assertEqual(profiles["openai_tool_calling_v0"]["state"], "implemented")
        self.assertEqual(profiles["anthropic_tool_use_v0"]["state"], "implemented")
        self.assertEqual(profiles["gemini_function_calling_v0"]["state"], "implemented")

    def test_openai_provider_profile_executes_read_query(self) -> None:
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
        self.assertEqual(response["result"]["row_count"], 1)

    def test_anthropic_provider_profile_executes_read_query(self) -> None:
        response = self.service.invoke_provider_tool(
            provider_profile_id="anthropic_tool_use_v0",
            payload={
                "request_id": "req_provider_anthropic",
                "id": "call_provider_anthropic",
                "name": "execute_readonly_query",
                "input": {
                    "dialect": "native",
                    "query_text": "SELECT 1",
                    "security_context": {"tenant_id": "tenant_a", "actor_id": "actor_a"},
                    "options": {"max_rows": 1},
                },
            },
        )
        self.assertEqual(response["status"], "success")
        self.assertEqual(response["result"]["row_count"], 1)

    def test_gemini_provider_profile_executes_read_query(self) -> None:
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
                        "security_context": {"tenant_id": "tenant_a", "actor_id": "actor_a"},
                        "options": {"max_rows": 1},
                    },
                },
            },
        )
        self.assertEqual(response["status"], "success")
        self.assertEqual(response["result"]["row_count"], 1)

    def test_openai_provider_profile_can_ingest_generated_embeddings(self) -> None:
        response = self.service.invoke_provider_tool(
            provider_profile_id="openai_tool_calling_v0",
            payload={
                "request_id": "req_provider_openai_generated",
                "id": "call_provider_openai_generated",
                "function": {
                    "name": "add_generated_embeddings",
                    "arguments": (
                        '{"index_id":"idx_provider_generated","dimension":4,"records":['
                        '{"vector_id":"doc-1#1","text":"north overdue invoice",'
                        '"metadata":{"document_id":"doc-1"}}],"provider_config":'
                        '{"provider_profile_id":"openai_embeddings_v1","model":"text-embedding-3-small",'
                        '"api_key":"secret-inline"},"security_context":'
                        '{"tenant_id":"tenant_a","actor_id":"actor_a"}}'
                    ),
                },
            },
        )
        self.assertEqual(response["status"], "success")
        self.assertEqual(response["result"]["provider_ref"], "inline:redacted")
        self.assertEqual(response["result"]["index"]["profile_id"], "provider_generated_embeddings_v0")


if __name__ == "__main__":
    unittest.main()
