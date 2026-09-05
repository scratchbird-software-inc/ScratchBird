#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "project" / "tools" / "release" / "implementation_maturity_gate.py"
SPEC = importlib.util.spec_from_file_location("implementation_maturity_gate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class ImplementationMaturityGateTests(unittest.TestCase):
    def test_repository_maturity_surface_is_valid(self):
        errors, counts = gate.validate(REPO_ROOT)
        self.assertEqual([], errors)
        self.assertGreater(sum(counts.values()), 100)
        self.assertGreater(counts["physical_implementation"], 0)

    def test_duplicate_maturity_field_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "matrix.yaml"
            path.write_text(
                "entries:\n"
                "  - sblr_operation: SBLR_TEST\n"
                "    api_operation_id: test.operation\n"
                "    implementation_maturity: routing\n"
                "    implementation_maturity: physical_implementation\n",
                encoding="utf-8",
            )
            _, errors = gate.parse_rows(path, "sblr_operation")
        self.assertTrue(any("duplicate row field implementation_maturity" in error for error in errors))

    def test_maturity_order_is_exact(self):
        self.assertEqual(
            (
                "codec_contract",
                "routing",
                "logical_implementation",
                "physical_implementation",
                "durability_proven",
                "production_qualified",
            ),
            gate.MATURITY_LEVELS,
        )


if __name__ == "__main__":
    unittest.main()
