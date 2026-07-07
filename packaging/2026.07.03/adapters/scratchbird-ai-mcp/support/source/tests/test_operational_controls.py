# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.operational_controls import OperationalControlEngine
from scratchbird_ai.tool_schema import ToolContractError


class OperationalControlTests(unittest.TestCase):
    def test_allows_within_window_and_reports_cost(self) -> None:
        engine = OperationalControlEngine(
            window_sec=60,
            max_requests_per_window=10,
            max_mutations_per_window=2,
            max_cost_units_per_window=100,
        )
        cost = engine.enforce(
            tenant_id="tenant_a",
            actor_id="actor_a",
            is_mutation=False,
            options={"max_rows": 10, "timeout_ms": 5000, "memory_mb": 256},
        )
        self.assertGreaterEqual(cost.cost_units, 1)
        self.assertEqual(cost.window_requests, 1)

    def test_request_rate_limit_fails_closed(self) -> None:
        engine = OperationalControlEngine(
            window_sec=60,
            max_requests_per_window=1,
            max_mutations_per_window=1,
            max_cost_units_per_window=100,
        )
        engine.enforce(
            tenant_id="tenant_a",
            actor_id="actor_a",
            is_mutation=False,
            options={"max_rows": 10},
        )
        with self.assertRaises(ToolContractError) as ctx:
            engine.enforce(
                tenant_id="tenant_a",
                actor_id="actor_a",
                is_mutation=False,
                options={"max_rows": 10},
            )
        self.assertEqual(ctx.exception.error_code, "E_LIMIT_EXCEEDED")

    def test_mutation_rate_limit_fails_closed(self) -> None:
        engine = OperationalControlEngine(
            window_sec=60,
            max_requests_per_window=10,
            max_mutations_per_window=1,
            max_cost_units_per_window=100,
        )
        engine.enforce(
            tenant_id="tenant_a",
            actor_id="actor_a",
            is_mutation=True,
            options={"max_rows": 10},
        )
        with self.assertRaises(ToolContractError) as ctx:
            engine.enforce(
                tenant_id="tenant_a",
                actor_id="actor_a",
                is_mutation=True,
                options={"max_rows": 10},
            )
        self.assertEqual(ctx.exception.error_code, "E_LIMIT_EXCEEDED")

    def test_cost_limit_fails_closed(self) -> None:
        engine = OperationalControlEngine(
            window_sec=60,
            max_requests_per_window=10,
            max_mutations_per_window=10,
            max_cost_units_per_window=5,
        )
        with self.assertRaises(ToolContractError) as ctx:
            engine.enforce(
                tenant_id="tenant_a",
                actor_id="actor_a",
                is_mutation=True,
                options={"max_rows": 500, "timeout_ms": 30000, "memory_mb": 2048},
            )
        self.assertEqual(ctx.exception.error_code, "E_LIMIT_EXCEEDED")


if __name__ == "__main__":
    unittest.main()
