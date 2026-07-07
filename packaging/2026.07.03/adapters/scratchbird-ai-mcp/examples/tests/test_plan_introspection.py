# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.plan_introspection import build_plan_response, compute_plan_hash


class PlanIntrospectionTests(unittest.TestCase):
    def test_plan_hash_is_deterministic_for_same_inputs(self) -> None:
        operator_tree_a = {
            "operator_id": "root",
            "operator_type": "Join",
            "children": [
                {"operator_id": "b", "operator_type": "Scan", "children": []},
                {"operator_id": "a", "operator_type": "Scan", "children": []},
            ],
        }
        operator_tree_b = {
            "operator_id": "root",
            "operator_type": "Join",
            "children": [
                {"operator_id": "a", "operator_type": "Scan", "children": []},
                {"operator_id": "b", "operator_type": "Scan", "children": []},
            ],
        }

        hash_a = compute_plan_hash(
            dialect="native",
            normalized_query="SELECT * FROM t",
            operator_tree=operator_tree_a,
            rls_policy_ids=["p2", "p1"],
            predicate_hash="pred123",
            planner_version="v1",
        )
        hash_b = compute_plan_hash(
            dialect="native",
            normalized_query="SELECT * FROM t",
            operator_tree=operator_tree_b,
            rls_policy_ids=["p1", "p2"],
            predicate_hash="pred123",
            planner_version="v1",
        )

        self.assertEqual(hash_a, hash_b)

    def test_build_plan_response_contains_required_fields(self) -> None:
        plan = build_plan_response(
            dialect="native",
            query_text="SELECT 1",
            operator_tree={"operator_id": "root", "operator_type": "Read", "children": []},
            rls_policy_ids=["policy_a"],
            predicate_hash="pred",
            planner_version="v1",
            rls_applied=True,
        )
        self.assertIn("plan_hash", plan)
        self.assertIn("operator_tree", plan)
        self.assertIn("rls_visibility", plan)
        self.assertEqual(plan["rls_visibility"]["policy_ids"], ["policy_a"])
        self.assertEqual(plan["rls_visibility"]["rls_policy_ids"], ["policy_a"])


if __name__ == "__main__":
    unittest.main()
