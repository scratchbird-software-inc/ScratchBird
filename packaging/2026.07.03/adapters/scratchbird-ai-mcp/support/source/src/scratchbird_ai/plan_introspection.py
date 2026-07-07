# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Deterministic plan introspection helpers."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .deterministic import canonical_json, sha256_hex


@dataclass(slots=True, frozen=True)
class PlanNode:
    operator_id: str
    operator_type: str
    children: tuple["PlanNode", ...]
    relation: str | None = None
    index: str | None = None
    predicate_summary: str | None = None
    estimated_rows: float | None = None
    estimated_cost: float | None = None

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "operator_id": self.operator_id,
            "operator_type": self.operator_type,
            "children": [child.to_dict() for child in self.children],
        }
        if self.relation is not None:
            result["relation"] = self.relation
        if self.index is not None:
            result["index"] = self.index
        if self.predicate_summary is not None:
            result["predicate_summary"] = self.predicate_summary
        if self.estimated_rows is not None:
            result["estimated_rows"] = round(self.estimated_rows, 6)
        if self.estimated_cost is not None:
            result["estimated_cost"] = round(self.estimated_cost, 6)
        return result


def _normalize_node(node: dict[str, Any]) -> dict[str, Any]:
    children_raw = node.get("children") or []
    if not isinstance(children_raw, list):
        children_raw = []

    normalized_children = [
        _normalize_node(child) for child in children_raw if isinstance(child, dict)
    ]

    # Deterministic child ordering fallback by operator_id/operator_type if caller order is unstable.
    normalized_children = sorted(
        normalized_children,
        key=lambda child: (
            str(child.get("operator_id", "")),
            str(child.get("operator_type", "")),
        ),
    )

    normalized: dict[str, Any] = {
        "operator_id": str(node.get("operator_id", "")),
        "operator_type": str(node.get("operator_type", "")),
        "children": normalized_children,
    }

    for optional_key in ("relation", "index", "predicate_summary"):
        if optional_key in node and node[optional_key] is not None:
            normalized[optional_key] = str(node[optional_key])

    for numeric_key in ("estimated_rows", "estimated_cost"):
        if numeric_key in node and node[numeric_key] is not None:
            try:
                normalized[numeric_key] = round(float(node[numeric_key]), 6)
            except (TypeError, ValueError):
                normalized[numeric_key] = 0.0

    return normalized


def normalize_operator_tree(operator_tree: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(operator_tree, dict):
        return {"operator_id": "", "operator_type": "unknown", "children": []}
    return _normalize_node(operator_tree)


def compute_plan_hash(
    *,
    dialect: str,
    normalized_query: str,
    operator_tree: dict[str, Any],
    rls_policy_ids: list[str],
    predicate_hash: str,
    planner_version: str,
) -> str:
    hash_input = {
        "dialect": dialect,
        "normalized_query": normalized_query,
        "normalized_operator_tree": normalize_operator_tree(operator_tree),
        "rls_policy_ids": sorted(str(item) for item in rls_policy_ids),
        "predicate_hash": predicate_hash,
        "planner_version": planner_version,
    }
    return sha256_hex(canonical_json(hash_input))


def build_plan_response(
    *,
    dialect: str,
    query_text: str,
    operator_tree: dict[str, Any],
    rls_policy_ids: list[str] | None = None,
    predicate_hash: str = "",
    planner_version: str = "v1",
    rls_applied: bool = True,
) -> dict[str, Any]:
    normalized_tree = normalize_operator_tree(operator_tree)
    normalized_query = " ".join(query_text.split())
    policy_ids = sorted(str(item) for item in (rls_policy_ids or []))
    plan_hash = compute_plan_hash(
        dialect=dialect,
        normalized_query=normalized_query,
        operator_tree=normalized_tree,
        rls_policy_ids=policy_ids,
        predicate_hash=predicate_hash,
        planner_version=planner_version,
    )
    return {
        "dialect": dialect,
        "normalized_query": normalized_query,
        "operator_tree": normalized_tree,
        "rls_visibility": {
            "applied": bool(rls_applied),
            "policy_ids": policy_ids,
            "predicate_hash": predicate_hash,
            # Legacy aliases retained during migration.
            "rls_applied": bool(rls_applied),
            "rls_policy_ids": policy_ids,
        },
        "predicate_hash": predicate_hash,
        "planner_version": planner_version,
        "plan_version": "1.0",
        "estimated_cost": {
            "cpu": 0.0,
            "io": 0.0,
            "rows": 0,
        },
        "plan_hash": plan_hash,
    }
