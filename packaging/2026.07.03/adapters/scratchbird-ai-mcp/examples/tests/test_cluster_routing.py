# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.cluster_routing import (
    ClusterNode,
    ClusterShard,
    ClusterTopology,
    RoutingDecisionError,
    distributed_vector_search,
    merge_ranked_results,
    route_query_contract,
    route_query,
    topology_from_dict,
)


class ClusterRoutingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.topology = ClusterTopology(
            cluster_epoch=5,
            shards=(
                ClusterShard(
                    shard_id="s1",
                    tenant_ids=("tenant_a",),
                    status="healthy",
                    primary=ClusterNode(node_id="n1", health="healthy"),
                    replicas=(ClusterNode(node_id="n2", health="healthy"),),
                ),
                ClusterShard(
                    shard_id="s2",
                    tenant_ids=("tenant_a",),
                    status="healthy",
                    primary=ClusterNode(node_id="n3", health="offline"),
                    replicas=(ClusterNode(node_id="n4", health="healthy"),),
                ),
            ),
        )

    def test_route_query_uses_primary_then_replica_failover(self) -> None:
        decision = route_query(topology=self.topology, tenant_id="tenant_a")
        by_shard = {route.shard_id: route for route in decision.routes}
        self.assertEqual(by_shard["s1"].node_id, "n1")
        self.assertEqual(by_shard["s1"].route_reason, "primary_healthy")
        self.assertEqual(by_shard["s2"].node_id, "n4")
        self.assertEqual(by_shard["s2"].route_reason, "replica_failover")

    def test_route_query_epoch_mismatch(self) -> None:
        with self.assertRaises(RoutingDecisionError) as ctx:
            route_query(topology=self.topology, tenant_id="tenant_a", cluster_epoch=4)
        self.assertEqual(ctx.exception.error_code, "E_CLUSTER_EPOCH_MISMATCH")

    def test_route_query_unauthorized_tenant(self) -> None:
        with self.assertRaises(RoutingDecisionError) as ctx:
            route_query(topology=self.topology, tenant_id="tenant_b")
        self.assertEqual(ctx.exception.error_code, "E_SHARD_UNAUTHORIZED")

    def test_merge_ranked_results_is_deterministic(self) -> None:
        merged = merge_ranked_results(
            per_shard_results={
                "s2": [
                    {"document_id": "doc_b", "score": 0.9},
                    {"document_id": "doc_a", "score": 0.9},
                ],
                "s1": [
                    {"document_id": "doc_c", "score": 0.9},
                ],
            },
            top_k=3,
        )
        ordered = [(row["score"], row["shard_id"], row["document_id"]) for row in merged]
        self.assertEqual(
            ordered,
            [
                (0.9, "s1", "doc_c"),
                (0.9, "s2", "doc_a"),
                (0.9, "s2", "doc_b"),
            ],
        )

    def test_distributed_vector_search_filters_to_routed_shards(self) -> None:
        result = distributed_vector_search(
            topology=self.topology,
            tenant_id="tenant_a",
            per_shard_results={
                "s1": [{"document_id": "doc1", "score": 0.7}],
                "s2": [{"document_id": "doc2", "score": 0.8}],
                "s999": [{"document_id": "doc_x", "score": 1.0}],
            },
            top_k=5,
        )
        docs = [row["document_id"] for row in result["results"]]
        self.assertNotIn("doc_x", docs)
        self.assertEqual(docs, ["doc2", "doc1"])
        self.assertEqual(result["participating_shards"], ["s1", "s2"])
        self.assertTrue(result["rls_applied"])

    def test_route_query_contract_output(self) -> None:
        decision = route_query(topology=self.topology, tenant_id="tenant_a")
        contract = route_query_contract(decision)
        self.assertEqual(contract["cluster_epoch"], 5)
        self.assertEqual(contract["target_shards"], ["s1", "s2"])
        self.assertEqual(contract["execution_mode"], "multi_shard")
        self.assertTrue(str(contract["route_plan_id"]).startswith("rp_"))

    def test_topology_from_dict_supports_spec_shape(self) -> None:
        topology = topology_from_dict(
            {
                "cluster_id": "cluster-a",
                "cluster_epoch": 9,
                "generated_at": "2026-02-24T12:00:00Z",
                "shards": [
                    {
                        "shard_id": "s01",
                        "tenant_range": ["tenant_a", "tenant_b"],
                        "primary_node": "node-1",
                        "replicas": ["node-2"],
                        "status": "healthy",
                    }
                ],
            }
        )
        self.assertEqual(topology.cluster_id, "cluster-a")
        self.assertEqual(topology.generated_at, "2026-02-24T12:00:00Z")
        self.assertEqual(topology.cluster_epoch, 9)
        self.assertEqual(topology.shards[0].primary.node_id, "node-1")
        self.assertEqual(topology.shards[0].replicas[0].node_id, "node-2")


if __name__ == "__main__":
    unittest.main()
