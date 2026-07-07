# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.policy import PolicyDeniedError, PolicyEngine


class PolicyEngineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.engine = PolicyEngine()

    def test_read_only_blocks_mutation(self) -> None:
        with self.assertRaises(PolicyDeniedError):
            self.engine.enforce(mode="read_only", is_mutation=True, approval_token=None)

    def test_mutation_mode_requires_approval(self) -> None:
        with self.assertRaises(PolicyDeniedError):
            self.engine.enforce(
                mode="mutation_with_approval",
                is_mutation=True,
                approval_token=None,
            )

    def test_mutation_mode_with_approval_allows(self) -> None:
        self.engine.enforce(
            mode="mutation_with_approval",
            is_mutation=True,
            approval_token="approved-token",
        )

    def test_canonical_analysis_mode_blocks_mutation(self) -> None:
        with self.assertRaises(PolicyDeniedError) as ctx:
            self.engine.enforce(
                mode="ai_analysis",
                is_mutation=True,
                approval_token=None,
            )
        self.assertIn("Mutations are denied", str(ctx.exception))

    def test_canonical_approved_mode_allows_mutation(self) -> None:
        decision = self.engine.enforce(
            mode="ai_mutation_approved",
            is_mutation=True,
            approval_token="approved-token",
        )
        self.assertTrue(decision.allowed)
        self.assertEqual(decision.canonical_mode, "ai_mutation_approved")


if __name__ == "__main__":
    unittest.main()
