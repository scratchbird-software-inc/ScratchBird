# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Cluster-aware routing and deterministic result merge utilities."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .deterministic import deterministic_id

ALLOWED_SHARD_STATUS = {"healthy", "degraded", "offline"}

ROUTING_ERROR_CODES = {
    "epoch_mismatch": "E_CLUSTER_EPOCH_MISMATCH",
    "route_unavailable": "E_ROUTE_UNAVAILABLE",
    "shard_unauthorized": "E_SHARD_UNAUTHORIZED",
}


class RoutingDecisionError(RuntimeError):
    def __init__(self, *, error_code: str, message: str) -> None:
        super().__init__(message)
        self.error_code = error_code
        self.message = message


@dataclass(slots=True, frozen=True)
class ClusterNode:
    node_id: str
    health: str = "healthy"


@dataclass(slots=True, frozen=True)
class ClusterShard:
    shard_id: str
    tenant_ids: tuple[str, ...]
    status: str
    primary: ClusterNode
    replicas: tuple[ClusterNode, ...] = ()

    def is_healthy(self) -> bool:
        return self.status == "healthy"


@dataclass(slots=True, frozen=True)
class ClusterTopology:
    cluster_epoch: int
    shards: tuple[ClusterShard, ...]
    cluster_id: str = ""
    generated_at: str = ""


@dataclass(slots=True, frozen=True)
class ShardRoute:
    shard_id: str
    node_id: str
    route_reason: str


@dataclass(slots=True, frozen=True)
class RouteDecision:
    trace_id: str
    cluster_epoch: int
    tenant_id: str
    routes: tuple[ShardRoute, ...]
    partial: bool
    dropped_shards: tuple[str, ...]


def topology_from_dict(payload: dict[str, Any]) -> ClusterTopology:
    if not isinstance(payload, dict):
        raise RoutingDecisionError(
            error_code=ROUTING_ERROR_CODES["route_unavailable"],
            message="topology payload must be an object",
        )

    epoch_raw = payload.get("cluster_epoch", 0)
    try:
        cluster_epoch = int(epoch_raw)
    except (TypeError, ValueError):
        raise RoutingDecisionError(
            error_code=ROUTING_ERROR_CODES["route_unavailable"],
            message="cluster_epoch must be integer",
        ) from None

    shards_raw = payload.get("shards", [])
    if not isinstance(shards_raw, list):
        raise RoutingDecisionError(
            error_code=ROUTING_ERROR_CODES["route_unavailable"],
            message="shards must be an array",
        )

    shards: list[ClusterShard] = []
    for item in shards_raw:
        if not isinstance(item, dict):
            continue
        primary_raw = item.get("primary", {}) if isinstance(item.get("primary"), dict) else {}
        if not primary_raw and isinstance(item.get("primary_node"), str):
            primary_raw = {"node_id": str(item.get("primary_node"))}

        replicas_raw = item.get("replicas", [])
        if not replicas_raw and isinstance(item.get("replica_nodes"), list):
            replicas_raw = item.get("replica_nodes", [])
        replicas: list[ClusterNode] = []
        if isinstance(replicas_raw, list):
            for rep in replicas_raw:
                if isinstance(rep, str):
                    replicas.append(ClusterNode(node_id=rep, health="healthy"))
                    continue
                if isinstance(rep, dict):
                    replicas.append(
                        ClusterNode(
                            node_id=str(rep.get("node_id", "")),
                            health=str(rep.get("health", "offline")),
                        )
                    )

        tenants_raw = item.get("tenant_ids", [])
        if not tenants_raw and isinstance(item.get("tenant_range"), list):
            tenants_raw = item.get("tenant_range", [])
        tenant_ids = (
            tuple(str(v) for v in tenants_raw)
            if isinstance(tenants_raw, list)
            else tuple()
        )
        shards.append(
            ClusterShard(
                shard_id=str(item.get("shard_id", "")),
                tenant_ids=tenant_ids,
                status=str(item.get("status", "offline")),
                primary=ClusterNode(
                    node_id=str(primary_raw.get("node_id", "")),
                    health=str(primary_raw.get("health", "offline")),
                ),
                replicas=tuple(replicas),
            )
        )

    return ClusterTopology(
        cluster_epoch=cluster_epoch,
        shards=tuple(shards),
        cluster_id=str(payload.get("cluster_id", "")),
        generated_at=str(payload.get("generated_at", "")),
    )


def _validate_topology(topology: ClusterTopology) -> None:
    if topology.cluster_epoch < 0:
        raise RoutingDecisionError(
            error_code=ROUTING_ERROR_CODES["route_unavailable"],
            message="Invalid cluster epoch",
        )
    for shard in topology.shards:
        if shard.status not in ALLOWED_SHARD_STATUS:
            raise RoutingDecisionError(
                error_code=ROUTING_ERROR_CODES["route_unavailable"],
                message=f"Invalid shard status: {shard.status}",
            )


def _candidate_shards(topology: ClusterTopology, tenant_id: str) -> list[ClusterShard]:
    return [
        shard
        for shard in topology.shards
        if tenant_id in shard.tenant_ids
    ]


def _choose_target(shard: ClusterShard) -> tuple[str, str] | None:
    if shard.status == "offline":
        return None
    if shard.primary.health == "healthy":
        return shard.primary.node_id, "primary_healthy"
    for replica in shard.replicas:
        if replica.health == "healthy":
            return replica.node_id, "replica_failover"
    return None


def route_query(
    *,
    topology: ClusterTopology,
    tenant_id: str,
    cluster_epoch: int | None = None,
    allow_partial: bool = False,
) -> RouteDecision:
    _validate_topology(topology)

    if cluster_epoch is not None and cluster_epoch != topology.cluster_epoch:
        raise RoutingDecisionError(
            error_code=ROUTING_ERROR_CODES["epoch_mismatch"],
            message=(
                f"cluster epoch mismatch: requested={cluster_epoch}, "
                f"current={topology.cluster_epoch}"
            ),
        )

    candidate_shards = sorted(
        _candidate_shards(topology, tenant_id),
        key=lambda shard: shard.shard_id,
    )
    if not candidate_shards:
        raise RoutingDecisionError(
            error_code=ROUTING_ERROR_CODES["shard_unauthorized"],
            message=f"tenant '{tenant_id}' has no authorized shards",
        )

    routes: list[ShardRoute] = []
    dropped: list[str] = []
    for shard in candidate_shards:
        target = _choose_target(shard)
        if target is None:
            if allow_partial:
                dropped.append(shard.shard_id)
                continue
            raise RoutingDecisionError(
                error_code=ROUTING_ERROR_CODES["route_unavailable"],
                message=f"no healthy node available for shard '{shard.shard_id}'",
            )
        node_id, reason = target
        routes.append(
            ShardRoute(
                shard_id=shard.shard_id,
                node_id=node_id,
                route_reason=reason,
            )
        )

    if not routes:
        raise RoutingDecisionError(
            error_code=ROUTING_ERROR_CODES["route_unavailable"],
            message="no routes available after health filtering",
        )

    trace_id = deterministic_id(
        "tr",
        {
            "tenant_id": tenant_id,
            "cluster_epoch": topology.cluster_epoch,
            "route_shards": [route.shard_id for route in routes],
            "allow_partial": allow_partial,
        },
    )
    return RouteDecision(
        trace_id=trace_id,
        cluster_epoch=topology.cluster_epoch,
        tenant_id=tenant_id,
        routes=tuple(routes),
        partial=bool(dropped),
        dropped_shards=tuple(sorted(dropped)),
    )


def route_query_contract(decision: RouteDecision) -> dict[str, Any]:
    shard_ids = [route.shard_id for route in decision.routes]
    route_plan_id = deterministic_id(
        "rp",
        {
            "cluster_epoch": decision.cluster_epoch,
            "tenant_id": decision.tenant_id,
            "target_shards": shard_ids,
            "partial": decision.partial,
        },
    )
    execution_mode = "single_shard" if len(shard_ids) == 1 else "multi_shard"
    return {
        "cluster_epoch": decision.cluster_epoch,
        "route_plan_id": route_plan_id,
        "target_shards": shard_ids,
        "execution_mode": execution_mode,
        "trace_id": decision.trace_id,
    }


def merge_ranked_results(
    *,
    per_shard_results: dict[str, list[dict[str, Any]]],
    top_k: int,
) -> list[dict[str, Any]]:
    merged: list[dict[str, Any]] = []
    for shard_id, rows in per_shard_results.items():
        for row in rows:
            score_raw = row.get("score", 0.0)
            try:
                score = float(score_raw)
            except (TypeError, ValueError):
                score = 0.0
            merged.append(
                {
                    **row,
                    "score": round(score, 6),
                    "shard_id": str(row.get("shard_id") or shard_id),
                    "document_id": str(row.get("document_id", "")),
                }
            )

    merged.sort(
        key=lambda row: (
            -row["score"],
            row["shard_id"],
            row["document_id"],
        )
    )
    return merged[: max(0, top_k)]


def distributed_vector_search(
    *,
    topology: ClusterTopology,
    tenant_id: str,
    per_shard_results: dict[str, list[dict[str, Any]]],
    top_k: int,
    cluster_epoch: int | None = None,
    allow_partial: bool = False,
) -> dict[str, Any]:
    decision = route_query(
        topology=topology,
        tenant_id=tenant_id,
        cluster_epoch=cluster_epoch,
        allow_partial=allow_partial,
    )
    allowed_shards = {route.shard_id for route in decision.routes}
    filtered_results = {
        shard_id: rows
        for shard_id, rows in per_shard_results.items()
        if shard_id in allowed_shards
    }
    results = merge_ranked_results(per_shard_results=filtered_results, top_k=top_k)
    return {
        "trace_id": decision.trace_id,
        "cluster_epoch": decision.cluster_epoch,
        "participating_shards": [route.shard_id for route in decision.routes],
        "routes": [
            {"shard_id": route.shard_id, "node_id": route.node_id, "reason": route.route_reason}
            for route in decision.routes
        ],
        "partial": decision.partial,
        "dropped_shards": list(decision.dropped_shards),
        "results": results,
        "rls_applied": True,
    }
