# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.execution_mode import (
    ApprovalEvidence,
    ExecutionModeError,
    evaluate_execution_mode,
    normalize_mode,
    validate_transition,
)


class ExecutionModeTests(unittest.TestCase):
    def test_alias_modes_normalize(self) -> None:
        self.assertEqual(normalize_mode("read_only"), "ai_analysis")
        self.assertEqual(
            normalize_mode("mutation_with_approval", approval_present=False),
            "ai_mutation_pending_approval",
        )
        self.assertEqual(
            normalize_mode("mutation_with_approval", approval_present=True),
            "ai_mutation_approved",
        )

    def test_mutation_denied_in_analysis_mode(self) -> None:
        evaluation, approval, options = evaluate_execution_mode(
            mode="ai_analysis",
            statement_kind="mutation",
            approval=None,
            options={"max_rows": 10},
        )
        self.assertFalse(evaluation.allowed)
        self.assertIsNone(approval)
        self.assertEqual(options["max_rows"], 10)
        self.assertEqual(evaluation.error_code, "E_POLICY_DENY")

    def test_mutation_approved_requires_token(self) -> None:
        with self.assertRaises(ExecutionModeError) as ctx:
            evaluate_execution_mode(
                mode="ai_mutation_approved",
                statement_kind="mutation",
                approval=ApprovalEvidence(approval_token=None),
            )
        self.assertEqual(ctx.exception.error_code, "E_APPROVAL_INVALID")

    def test_mutation_approved_with_token_allows(self) -> None:
        evaluation, approval, _ = evaluate_execution_mode(
            mode="ai_mutation_approved",
            statement_kind="mutation",
            approval=ApprovalEvidence(approval_token="approved-token"),
        )
        self.assertTrue(evaluation.allowed)
        self.assertIsNotNone(approval)
        assert approval is not None
        self.assertTrue(approval.approval_id.startswith("appr_"))

    def test_unknown_statement_is_treated_as_mutation(self) -> None:
        evaluation, _, _ = evaluate_execution_mode(
            mode="ai_analysis",
            statement_kind="unknown",
            approval=None,
            options={"timeout_ms": 20},
        )
        self.assertFalse(evaluation.allowed)
        self.assertEqual(evaluation.error_code, "E_POLICY_DENY")

    def test_limit_exceeded_raises(self) -> None:
        with self.assertRaises(ExecutionModeError) as ctx:
            evaluate_execution_mode(
                mode="ai_analysis",
                statement_kind="read",
                approval=None,
                options={"timeout_ms": 50_000},
            )
        self.assertEqual(ctx.exception.error_code, "E_LIMIT_EXCEEDED")

    def test_timeout_and_memory_lower_bounds_normalize(self) -> None:
        _, _, options = evaluate_execution_mode(
            mode="ai_analysis",
            statement_kind="read",
            approval=None,
            options={"timeout_ms": 1, "memory_mb": 1},
        )
        self.assertEqual(options["timeout_ms"], 100)
        self.assertEqual(options["memory_mb"], 64)

    def test_transition_requires_approval_for_pending_to_approved(self) -> None:
        self.assertFalse(
            validate_transition(
                from_mode="ai_mutation_pending_approval",
                to_mode="ai_mutation_approved",
                approval=ApprovalEvidence(approval_token=None),
            )
        )
        self.assertTrue(
            validate_transition(
                from_mode="ai_mutation_pending_approval",
                to_mode="ai_mutation_approved",
                approval=ApprovalEvidence(approval_token="approved-token"),
            )
        )


if __name__ == "__main__":
    unittest.main()
