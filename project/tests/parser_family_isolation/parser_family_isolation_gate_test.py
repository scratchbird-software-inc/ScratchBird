#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
import unittest


def load_gate(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("parser_family_isolation_gate", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load gate: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ParserFamilyIsolationFixtureTests(unittest.TestCase):
    gate = None
    fixture_root = pathlib.Path()

    def scan(self, fixture: str):
        root = self.fixture_root / fixture
        registry = self.gate.OwnershipRegistry.load(root, root / "ownership.json")
        return self.gate.scan_repository(registry)

    def test_positive_neutral_component_is_shareable(self) -> None:
        result = self.scan("positive_neutral_sharing")
        self.assertEqual([], [violation.to_json() for violation in result.violations])
        self.assertEqual({"alpha", "beta"}, set(result.family_files))
        self.assertGreater(result.neutral_files, 0)

    def test_cross_family_dependency_forms_are_rejected(self) -> None:
        root = self.fixture_root / "negative_cross_family"
        expected = set(json.loads((root / "expected_violation_kinds.json").read_text()))
        result = self.scan("negative_cross_family")
        actual = {violation.kind for violation in result.violations}
        self.assertTrue(expected.issubset(actual), f"missing={sorted(expected - actual)} actual={sorted(actual)}")
        self.assertTrue(
            all(violation.diagnostic.startswith("PARSER.") for violation in result.violations)
        )
        self.assertTrue(
            any(
                violation.kind == "neutral_family_symbol"
                and violation.evidence == "scratchbird::parser::beta"
                for violation in result.violations
            ),
            "neutral family namespace declaration was not rejected",
        )
        neutral_bad = root / "project/src/parsers/compatibility/common/neutral_bad.cpp"
        alias_line = next(
            index
            for index, line in enumerate(neutral_bad.read_text().splitlines(), start=1)
            if "neutral_beta_alias" in line
        )
        self.assertTrue(
            any(
                violation.kind == "neutral_family_symbol"
                and violation.path.endswith("neutral_bad.cpp")
                and violation.line == alias_line
                for violation in result.violations
            ),
            "neutral namespace alias target was not rejected",
        )
        self.assertTrue(
            any(
                violation.kind == "neutral_family_semantic_selector"
                and violation.foreign_owner == "beta"
                for violation in result.violations
            ),
            "neutral family semantic selector was not rejected",
        )
        self.assertTrue(
            any(
                violation.kind == "neutral_family_semantic_policy"
                and violation.evidence == "beta.transaction_semantic_policy"
                for violation in result.violations
            ),
            "neutral family semantic policy was not rejected",
        )
        self.assertTrue(
            any(
                violation.kind == "neutral_sql_parser_surface"
                and violation.evidence == "ParseStatement"
                for violation in result.violations
            ),
            "neutral SQL parser entry point was not rejected",
        )
        self.assertTrue(
            any(
                violation.kind == "neutral_parser_semantic_renderer"
                and violation.evidence == "RenderSemanticEvidenceJson"
                for violation in result.violations
            ),
            "neutral parser semantic/evidence renderer was not rejected",
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate-script", required=True, type=pathlib.Path)
    parser.add_argument("--fixture-root", required=True, type=pathlib.Path)
    args = parser.parse_args()
    ParserFamilyIsolationFixtureTests.gate = load_gate(args.gate_script.resolve())
    ParserFamilyIsolationFixtureTests.fixture_root = args.fixture_root.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ParserFamilyIsolationFixtureTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
