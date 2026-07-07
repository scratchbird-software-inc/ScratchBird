# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.framework_adapters import (
    LangChainAdapter,
    LlamaIndexAdapter,
    SemanticKernelAdapter,
)
from scratchbird_ai.service import build_default_service


class FrameworkAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.service = build_default_service()
        self.security_context = {
            "tenant_id": "tenant_a",
            "actor_id": "actor_a",
            "roles": ["analyst"],
            "session_id": "sess_1",
            "context_version": 1,
        }

    def test_langchain_toolkit_and_query_flow(self) -> None:
        adapter = LangChainAdapter(self.service)

        tool_names = {tool.name for tool in adapter.get_toolkit()}
        self.assertIn("execute_readonly_query", tool_names)
        self.assertIn("hybrid_search", tool_names)

        response = adapter.run_query(
            dialect="native",
            query_text="SELECT 1",
            security_context=self.security_context,
            options={"max_rows": 1},
            request_id="req_langchain_query",
        )
        self.assertEqual(response["status"], "success")
        self.assertEqual(response["interface_profile_id"], "langchain_v0")
        self.assertEqual(response["result"]["row_count"], 1)

    def test_langchain_mutation_denial_without_approval(self) -> None:
        adapter = LangChainAdapter(self.service)

        response = adapter.run_mutation(
            dialect="native",
            query_text="UPDATE widgets SET qty = 2",
            security_context=self.security_context,
            request_id="req_langchain_mutation",
        )
        self.assertEqual(response["status"], "error")
        self.assertEqual(response["error"]["error_code"], "E_APPROVAL_INVALID")

    def test_llamaindex_query_explain_and_retrieval(self) -> None:
        adapter = LlamaIndexAdapter(self.service)
        self.service.add_embeddings(
            index_id="idx_framework_docs",
            dimension=3,
            records=[
                {
                    "vector_id": "doc-1#1",
                    "embedding": [0.1, 0.2, 0.3],
                    "metadata": {
                        "document_id": "doc-1",
                        "text": "north overdue invoice",
                    },
                }
            ],
            security_context=self.security_context,
        )

        query = adapter.query(
            dialect="native",
            query_text="SELECT 1",
            security_context=self.security_context,
            options={"max_rows": 1},
            request_id="req_llama_query",
        )
        self.assertEqual(query["status"], "success")
        self.assertEqual(query["interface_profile_id"], "llamaindex_v0")
        self.assertEqual(query["result"]["row_count"], 1)

        explain = adapter.explain(
            dialect="native",
            query_text="SELECT 1",
            security_context=self.security_context,
            request_id="req_llama_explain",
        )
        self.assertEqual(explain["status"], "success")
        self.assertIn("plan_hash", explain["result"])

        vector = adapter.vector_retrieve(
            index_id="idx_framework_docs",
            query_embedding=[0.1, 0.2, 0.3],
            top_k=5,
            security_context=self.security_context,
            request_id="req_llama_vector",
        )
        self.assertEqual(vector["status"], "success")
        self.assertEqual(
            vector["result"]["results"][0]["metadata"]["document_id"],
            "doc-1",
        )

        hybrid = adapter.hybrid_retrieve(
            dialect="native",
            query_text="north overdue invoice",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id="idx_framework_docs",
            top_k=5,
            security_context=self.security_context,
            sql_filter={"metadata": {"document_id": "doc-1"}},
            request_id="req_llama_hybrid",
        )
        self.assertEqual(hybrid["status"], "success")
        self.assertEqual(hybrid["result"]["results"][0]["document_id"], "doc-1")

    def test_semantic_kernel_plugin_manifest_and_function_invoke(self) -> None:
        adapter = SemanticKernelAdapter(self.service)

        functions = adapter.get_plugin_functions()
        function_names = {item.function_name for item in functions}
        plugin_names = {item.plugin_name for item in functions}
        self.assertEqual(plugin_names, {"scratchbird"})
        self.assertIn("execute_readonly_query", function_names)

        response = adapter.invoke_function(
            function_name="execute_readonly_query",
            arguments={
                "dialect": "native",
                "query_text": "SELECT 1",
                "options": {"max_rows": 1},
            },
            security_context=self.security_context,
            request_id="req_semantic_kernel_query",
        )
        self.assertEqual(response["status"], "success")
        self.assertEqual(response["interface_profile_id"], "semantic_kernel_v0")
        self.assertEqual(response["result"]["row_count"], 1)


if __name__ == "__main__":
    unittest.main()
