# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.capability_matrix import load_capability_matrix
from scratchbird_ai.router import DialectRouter, RoutingError


class DialectRouterTests(unittest.TestCase):
    def setUp(self) -> None:
        self.router = DialectRouter(matrix=load_capability_matrix())

    def test_available_dialects_contains_native(self) -> None:
        self.assertEqual(self.router.available_dialects(), ["native"])

    def test_require_capability_allows_supported_dialect(self) -> None:
        self.router.require_capability("native", "read_select")

    def test_require_capability_allows_native_graph_ops(self) -> None:
        self.router.require_capability("native", "graph_ops")

    def test_require_capability_rejects_non_native_dialect_with_policy_message(self) -> None:
        with self.assertRaises(RoutingError) as ctx:
            self.router.require_capability("postgresql", "read_select")
        self.assertIn("supports ScratchBird-native", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
