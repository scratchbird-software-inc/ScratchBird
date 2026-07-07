# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.adapters.base import AdapterCompileResult
from scratchbird_ai.service import build_default_service
from scratchbird_ai.tool_schema import ToolContractError


class RepairAwareCompiler:
    def compile_query(self, query_text: str, context: dict) -> AdapterCompileResult:
        _ = context
        normalized = query_text.strip()
        if normalized.startswith("```") or normalized.lower().startswith("sql:"):
            raise ToolContractError(
                error_code="E_COMPILE_FAILED",
                message="recoverable wrapped query",
                policy_rule_id="COMPILE-REPAIR-001",
            )
        return AdapterCompileResult(
            statement_kind="read",
            sblr_hash="hash-select-1",
            diagnostics=[],
            warnings=[],
        )


class CompileRepairTests(unittest.TestCase):
    def setUp(self) -> None:
        self.service = build_default_service()
        self.service.adapters["native"].compiler = RepairAwareCompiler()
        self.security_context = {
            "tenant_id": "tenant_a",
            "actor_id": "actor_a",
            "roles": ["analyst"],
            "session_id": "sess_1",
            "context_version": 1,
        }

    def test_compile_query_repairs_markdown_fence(self) -> None:
        compiled = self.service.compile_query(
            dialect="native",
            query_text="```sql\nSELECT 1\n```",
            context={"security_context": self.security_context},
        )
        self.assertIn("compile repair applied: strip_markdown_fence", compiled.warnings)

        executed = self.service.execute_compiled(
            compile_artifact_id=compiled.compile_artifact_id,
            options={"max_rows": 1},
            mode="ai_analysis",
        )
        self.assertEqual(executed.rows[0]["query_echo"], "SELECT 1")

    def test_run_query_returns_cost_attribution(self) -> None:
        response = self.service.run_query(
            request_id="req_cost_attr",
            dialect="native",
            query_text="SELECT 1",
            mode="ai_analysis",
            options={"max_rows": 10, "timeout_ms": 5000, "memory_mb": 256},
            context={"security_context": self.security_context},
        )
        self.assertIsNotNone(response.cost_attribution)
        assert response.cost_attribution is not None
        self.assertGreaterEqual(response.cost_attribution["cost_units"], 1)
        self.assertIn("cost_units=", response.notices[-1])


if __name__ == "__main__":
    unittest.main()
