#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]
MODULE_PATH = (
    REPO_ROOT / "project" / "tools" / "ci" / "configure_critical_review_ruleset.py"
)
SPEC = importlib.util.spec_from_file_location("configure_critical_review_ruleset", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
ruleset = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ruleset)


class CriticalReviewRulesetTests(unittest.TestCase):
    def setUp(self):
        self.desired = json.loads(
            (REPO_ROOT / ".github" / "rulesets" / "main-critical-path.json").read_text(
                encoding="utf-8"
            )
        )

    def live(self):
        return {
            "name": self.desired["name"],
            "enforcement": "active",
            "rules": copy.deepcopy(self.desired["rules"]),
        }

    def test_versioned_ruleset_has_all_required_controls(self):
        self.assertEqual("disabled", self.desired["enforcement"])
        self.assertEqual([], ruleset.validate_live_ruleset(self.live(), self.desired))

    def test_disabled_ruleset_is_rejected(self):
        live = self.live()
        live["enforcement"] = "disabled"
        failures = ruleset.validate_live_ruleset(live, self.desired)
        self.assertIn("ruleset enforcement is not active", failures)

    def test_missing_review_status_is_rejected(self):
        live = self.live()
        status_rule = next(
            rule for rule in live["rules"] if rule["type"] == "required_status_checks"
        )
        status_rule["parameters"]["required_status_checks"] = []
        failures = ruleset.validate_live_ruleset(live, self.desired)
        self.assertTrue(any("required status contexts missing" in value for value in failures))

    def test_status_from_unpinned_integration_is_rejected(self):
        live = self.live()
        status_rule = next(
            rule for rule in live["rules"] if rule["type"] == "required_status_checks"
        )
        status_rule["parameters"]["required_status_checks"][0].pop("integration_id")
        failures = ruleset.validate_live_ruleset(live, self.desired)
        self.assertTrue(any("wrong integration" in value for value in failures))


if __name__ == "__main__":
    unittest.main()
