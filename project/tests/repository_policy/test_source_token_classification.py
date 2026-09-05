#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = REPO_ROOT / "project/tools/release/github_actions_static_gate.py"
SPEC = importlib.util.spec_from_file_location("github_actions_static_gate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)


class SourceTokenClassificationTests(unittest.TestCase):
    def test_source_token_checks_are_classified_and_non_behavioral(self):
        gate.check_source_token_classification(REPO_ROOT)


if __name__ == "__main__":
    unittest.main()
