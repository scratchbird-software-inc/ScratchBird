#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-059 expression runtime function cleanup fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_059_EXPRESSION_RUNTIME_FUNCTION_CLEANUP_FIXTURES.csv"
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
SURFACE_REGISTRY_NAME = "STRICT_ROW_COVERAGE_LEDGER.csv"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"

TARGETS = {
    "SBSQL-EBE863C39BD6": "sb.temporal.month_name",
    "SBSQL-ED8C540CF5B1": "sb.temporal.date_sub",
    "SBSQL-EF179A79677B": "sb.temporal.epoch",
    "SBSQL-F0999F1E0637": "sb.scalar.gen_id",
    "SBSQL-F6C685816805": "sb.temporal.day_name",
    "SBSQL-FB4C56854614": "sb.temporal.date_sub",
    "SBSQL-FEC27B990B29": "sb.scalar.position_regex",
}

ALLOWED_CASE_KINDS = {"positive"}
ALLOWED_DESCRIPTORS = {"character", "date", "int64", "timestamp"}


def load_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root
    errors: list[str] = []

    fixture_rows = load_csv(root / FIXTURES)
    surface_rows = {row["surface_id"]: row for row in load_csv(artifact_root / SURFACE_REGISTRY_NAME)}
    seed_text = (root / SEED_REGISTRY).read_text(encoding="utf-8")
    seeded_functions = set(re.findall(r'"(sb\.[^"]+)"', seed_text))

    seen: dict[str, dict[str, str]] = {}
    for row in fixture_rows:
        surface_id = row["surface_id"]
        if surface_id in seen:
            errors.append(f"{surface_id}: duplicate SBSFC-059 fixture row")
        seen[surface_id] = row
        expected_function = TARGETS.get(surface_id)
        if expected_function is None:
            errors.append(f"{surface_id}: unexpected SBSFC-059 fixture row")
            continue
        if row["function_id"] != expected_function or row["canonical_builtin_id"] != expected_function:
            errors.append(f"{surface_id}: function id mismatch")
        if expected_function not in seeded_functions:
            errors.append(f"{surface_id}: function id missing from seed registry")
        if row["case_kind"] not in ALLOWED_CASE_KINDS:
            errors.append(f"{surface_id}: unsupported case kind {row['case_kind']}")
        if row["expected_result_descriptor"] not in ALLOWED_DESCRIPTORS:
            errors.append(f"{surface_id}: unsupported descriptor {row['expected_result_descriptor']}")
        try:
            arguments = json.loads(row["arguments_json"])
        except json.JSONDecodeError as exc:
            errors.append(f"{surface_id}: arguments_json does not parse: {exc}")
            arguments = None
        if not isinstance(arguments, list):
            errors.append(f"{surface_id}: arguments_json must be a list")
        surface = surface_rows.get(surface_id)
        if surface is None:
            errors.append(f"{surface_id}: missing strict ledger row")
            continue
        if surface["status"] != "native_now":
            errors.append(f"{surface_id}: surface status is not native_now")
        if surface["cluster_scope"] != "noncluster_or_profile_scoped":
            errors.append(f"{surface_id}: cluster scope is not noncluster/profile scoped")
        if surface["surface_kind"] != "function":
            errors.append(f"{surface_id}: surface kind is not function")
        if surface["sblr_operation_family"] != "sblr.expression.runtime.v3":
            errors.append(f"{surface_id}: SBLR operation family is not expression runtime")

    missing = sorted(set(TARGETS) - set(seen))
    for surface_id in missing:
        errors.append(f"{surface_id}: missing SBSFC-059 fixture")

    if len(fixture_rows) != len(TARGETS):
        errors.append(f"expected {len(TARGETS)} fixture rows, found {len(fixture_rows)}")

    if errors:
        print(
            "sbsql_sbsfc_059_expression_runtime_function_cleanup_fixture_gate found "
            f"{len(errors)} issue(s):",
            file=sys.stderr,
        )
        for error in errors:
            print(f" - {error}", file=sys.stderr)
        print("sbsql_sbsfc_059_expression_runtime_function_cleanup_fixture_gate=failed", file=sys.stderr)
        return 1

    print("sbsql_sbsfc_059_expression_runtime_function_cleanup_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
