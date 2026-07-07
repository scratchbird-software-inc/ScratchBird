# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import os
import tempfile
import unittest

from scratchbird_ai.retrieval import InMemoryRetrievalStore, RetrievalError


class RetrievalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.store = InMemoryRetrievalStore()
        self.security_context = {
            "tenant_id": "tenant_a",
            "actor_id": "actor_a",
            "roles": ["analyst"],
            "session_id": "sess_1",
            "context_version": 1,
        }
        self.store.add_embeddings(
            index_id="idx_docs",
            dimension=3,
            records=[
                {
                    "vector_id": "doc-1#1",
                    "embedding": [0.1, 0.2, 0.3],
                    "metadata": {
                        "document_id": "doc-1",
                        "text": "north overdue invoice",
                        "status": "OVERDUE",
                        "priority": 10,
                    },
                },
                {
                    "vector_id": "doc-2#1",
                    "embedding": [0.3, 0.1, 0.0],
                    "metadata": {
                        "document_id": "doc-2",
                        "text": "south paid invoice",
                        "status": "PAID",
                        "priority": 3,
                    },
                },
            ],
            security_context=self.security_context,
        )

    def test_vector_search_returns_deterministic_order(self) -> None:
        result = self.store.vector_search(
            index_id="idx_docs",
            query_embedding=[0.1, 0.2, 0.3],
            top_k=2,
            filters={},
            include_vectors=False,
            security_context=self.security_context,
        )
        ids = [row["vector_id"] for row in result["results"]]
        self.assertEqual(ids, ["doc-1#1", "doc-2#1"])
        self.assertTrue(result["rls_applied"])

    def test_vector_search_supports_structured_filter_operators(self) -> None:
        result = self.store.vector_search(
            index_id="idx_docs",
            query_embedding=[0.1, 0.2, 0.3],
            top_k=2,
            filters={
                "priority": {"gte": 5},
                "status": {"in": ["OVERDUE", "OPEN"]},
            },
            include_vectors=False,
            security_context=self.security_context,
        )
        self.assertEqual([row["vector_id"] for row in result["results"]], ["doc-1#1"])
        self.assertEqual(result["filter_summary"]["clause_count"], 2)

    def test_hybrid_search_is_supported_offline_for_metadata_filter(self) -> None:
        result = self.store.hybrid_search(
            dialect="native",
            query_text="overdue invoice north",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id="idx_docs",
            sql_filter={"metadata": {"document_id": "doc-1"}},
            weights={"vector": 0.6, "lexical": 0.3, "structured": 0.1},
            top_k=5,
            security_context=self.security_context,
        )
        self.assertEqual(len(result["results"]), 1)
        self.assertEqual(result["results"][0]["document_id"], "doc-1")

    def test_hybrid_search_rejects_where_filter_without_engine_pushdown(self) -> None:
        with self.assertRaises(RetrievalError) as ctx:
            self.store.hybrid_search(
                dialect="native",
                query_text="invoice",
                query_embedding=[0.1, 0.2, 0.3],
                vector_index_id="idx_docs",
                sql_filter={"where": "status = 'OVERDUE'"},
                weights=None,
                top_k=5,
                security_context=self.security_context,
            )
        self.assertEqual(ctx.exception.error_code, "E_FILTER_PUSHDOWN_UNAVAILABLE")

    def test_add_embeddings_blocks_cross_tenant_records(self) -> None:
        with self.assertRaises(RetrievalError) as ctx:
            self.store.add_embeddings(
                index_id="idx_docs",
                dimension=3,
                records=[
                    {
                        "vector_id": "doc-x#1",
                        "embedding": [0.0, 0.1, 0.2],
                        "metadata": {"tenant_id": "tenant_b"},
                    }
                ],
                security_context=self.security_context,
            )
        self.assertEqual(ctx.exception.error_code, "E_POLICY_DENY")

    def test_index_lifecycle_operations_track_state_and_visibility(self) -> None:
        created = self.store.create_index(
            index_id="idx_lifecycle",
            dimension=4,
            security_context=self.security_context,
        )
        self.assertEqual(created["index"]["state"], "provisioning")

        listed = self.store.list_indexes(security_context=self.security_context)
        listed_ids = {row["index_id"] for row in listed["indexes"]}
        self.assertIn("idx_lifecycle", listed_ids)

        reindexed = self.store.reindex_index(
            index_id="idx_lifecycle",
            security_context=self.security_context,
        )
        self.assertEqual(reindexed["previous_state"], "provisioning")
        self.assertEqual(reindexed["index"]["state"], "provisioning")

        deleted = self.store.delete_index(
            index_id="idx_lifecycle",
            security_context=self.security_context,
        )
        self.assertEqual(deleted["index"]["state"], "deleted")

        visible = self.store.list_indexes(security_context=self.security_context)
        visible_ids = {row["index_id"] for row in visible["indexes"]}
        self.assertNotIn("idx_lifecycle", visible_ids)

        described = self.store.describe_index(
            index_id="idx_lifecycle",
            security_context=self.security_context,
        )
        self.assertEqual(described["index"]["state"], "deleted")

        with self.assertRaises(RetrievalError) as ctx:
            self.store.vector_search(
                index_id="idx_lifecycle",
                query_embedding=[0.1, 0.2, 0.3, 0.4],
                top_k=1,
                filters=None,
                include_vectors=False,
                security_context=self.security_context,
            )
        self.assertEqual(ctx.exception.error_code, "E_INDEX_NOT_FOUND")

    def test_catalog_persists_indexes_and_records(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            catalog_path = os.path.join(tmpdir, "retrieval_catalog.json")
            store = InMemoryRetrievalStore(catalog_path=catalog_path)
            store.add_embeddings(
                index_id="idx_persist",
                dimension=3,
                records=[
                    {
                        "vector_id": "doc-persist#1",
                        "embedding": [0.1, 0.2, 0.3],
                        "metadata": {"document_id": "doc-persist", "text": "persisted row"},
                    }
                ],
                security_context=self.security_context,
            )

            reloaded = InMemoryRetrievalStore(catalog_path=catalog_path)
            described = reloaded.describe_index(
                index_id="idx_persist",
                security_context=self.security_context,
            )
            self.assertEqual(described["index"]["state"], "ready")
            self.assertEqual(described["index"]["record_count"], 1)

            result = reloaded.vector_search(
                index_id="idx_persist",
                query_embedding=[0.1, 0.2, 0.3],
                top_k=1,
                filters=None,
                include_vectors=True,
                security_context=self.security_context,
            )
            self.assertEqual(result["results"][0]["vector_id"], "doc-persist#1")
            self.assertEqual(result["results"][0]["embedding"], [0.1, 0.2, 0.3])

    def test_add_generated_embeddings_redacts_provider_secret_reference(self) -> None:
        os.environ["SCRATCHBIRD_AI_TEST_EMBED_KEY"] = "secret-value"
        try:
            result = self.store.add_generated_embeddings(
                index_id="idx_provider",
                dimension=4,
                records=[
                    {
                        "vector_id": "doc-provider#1",
                        "text": "north overdue invoice",
                        "metadata": {"document_id": "doc-provider"},
                    }
                ],
                provider_config={
                    "provider_profile_id": "openai_embeddings_v1",
                    "model": "text-embedding-3-small",
                    "api_key_env_var": "SCRATCHBIRD_AI_TEST_EMBED_KEY",
                },
                security_context=self.security_context,
            )
        finally:
            os.environ.pop("SCRATCHBIRD_AI_TEST_EMBED_KEY", None)

        self.assertEqual(result["profile_id"], "provider_generated_embeddings_v0")
        self.assertEqual(result["provider_profile_id"], "openai_embeddings_v1")
        self.assertEqual(result["provider_ref"], "env:SCRATCHBIRD_AI_TEST_EMBED_KEY")

        described = self.store.describe_index(
            index_id="idx_provider",
            security_context=self.security_context,
        )
        self.assertEqual(described["index"]["provider_ref"], "env:SCRATCHBIRD_AI_TEST_EMBED_KEY")
        self.assertEqual(described["index"]["state"], "ready")

    def test_embedding_profile_mismatch_is_rejected(self) -> None:
        os.environ["SCRATCHBIRD_AI_TEST_EMBED_KEY"] = "secret-value"
        try:
            self.store.add_generated_embeddings(
                index_id="idx_profile_mismatch",
                dimension=4,
                records=[
                    {
                        "vector_id": "doc-generated#1",
                        "text": "provider generated text",
                        "metadata": {"document_id": "doc-generated"},
                    }
                ],
                provider_config={
                    "provider_profile_id": "openai_embeddings_v1",
                    "model": "text-embedding-3-small",
                    "api_key_env_var": "SCRATCHBIRD_AI_TEST_EMBED_KEY",
                },
                security_context=self.security_context,
            )
        finally:
            os.environ.pop("SCRATCHBIRD_AI_TEST_EMBED_KEY", None)

        with self.assertRaises(RetrievalError) as ctx:
            self.store.add_embeddings(
                index_id="idx_profile_mismatch",
                dimension=4,
                records=[
                    {
                        "vector_id": "doc-client#1",
                        "embedding": [0.1, 0.2, 0.3, 0.4],
                        "metadata": {"document_id": "doc-client"},
                    }
                ],
                security_context=self.security_context,
            )
        self.assertEqual(ctx.exception.error_code, "E_COMPATIBILITY_MISMATCH")

    def test_engine_managed_profile_allows_safe_where_pushdown(self) -> None:
        store = InMemoryRetrievalStore(
            supported_profiles={"engine_managed_retrieval_v0"},
            backend_kind="engine_managed_contract_scaffold",
            allow_where_pushdown=True,
        )
        store.create_index(
            index_id="idx_managed",
            dimension=3,
            security_context=self.security_context,
            profile_id="engine_managed_retrieval_v0",
        )
        store.add_embeddings(
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

        result = store.hybrid_search(
            dialect="native",
            query_text="overdue invoice north",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id="idx_managed",
            sql_filter={"where": "status = 'OVERDUE'"},
            weights={"vector": 0.6, "lexical": 0.3, "structured": 0.1},
            top_k=5,
            security_context=self.security_context,
        )
        self.assertEqual(result["results"][0]["document_id"], "doc-managed")

    def test_engine_managed_where_pushdown_supports_richer_predicates(self) -> None:
        store = InMemoryRetrievalStore(
            supported_profiles={"engine_managed_retrieval_v0"},
            backend_kind="engine_managed_contract_scaffold",
            allow_where_pushdown=True,
        )
        store.create_index(
            index_id="idx_managed_rich",
            dimension=3,
            security_context=self.security_context,
            profile_id="engine_managed_retrieval_v0",
        )
        store.add_embeddings(
            index_id="idx_managed_rich",
            dimension=3,
            records=[
                {
                    "vector_id": "doc-managed#1",
                    "embedding": [0.1, 0.2, 0.3],
                    "metadata": {
                        "document_id": "doc-managed",
                        "status": "OVERDUE",
                        "priority": 10,
                        "text": "north overdue invoice",
                    },
                },
                {
                    "vector_id": "doc-managed#2",
                    "embedding": [0.3, 0.1, 0.0],
                    "metadata": {
                        "document_id": "doc-managed-2",
                        "status": "PAID",
                        "priority": 2,
                        "text": "south paid invoice",
                    },
                },
            ],
            security_context=self.security_context,
        )

        result = store.hybrid_search(
            dialect="native",
            query_text="north overdue invoice",
            query_embedding=[0.1, 0.2, 0.3],
            vector_index_id="idx_managed_rich",
            sql_filter={
                "where": (
                    "priority >= 5 AND status IN ('OVERDUE', 'OPEN') "
                    "AND text ILIKE 'north%'"
                )
            },
            weights={"vector": 0.6, "lexical": 0.3, "structured": 0.1},
            top_k=5,
            security_context=self.security_context,
        )
        self.assertEqual(result["results"][0]["document_id"], "doc-managed")
        self.assertEqual(result["query_plan"]["planner_mode"], "engine_pushdown")
        self.assertEqual(result["filter_summary"]["filter_mode"], "planner_safe_where")


if __name__ == "__main__":
    unittest.main()
