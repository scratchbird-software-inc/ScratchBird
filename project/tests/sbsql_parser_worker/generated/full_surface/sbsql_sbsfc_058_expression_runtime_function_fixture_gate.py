#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-058 expression runtime function fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_058_EXPRESSION_RUNTIME_FUNCTION_FIXTURES.csv"
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
SURFACE_REGISTRY_NAME = "STRICT_ROW_COVERAGE_LEDGER.csv"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"

TARGETS = {
    "SBSQL-0FF0127F4755": "sb.scalar.position_regex",
    "SBSQL-190A9409E3F9": "sb.temporal.age",
    "SBSQL-1B190CAA8EB4": "sb.temporal.epoch",
    "SBSQL-20441CF0D96A": "sb.temporal.date_add",
    "SBSQL-270091D8A5BB": "sb.scalar.currval",
    "SBSQL-2B4C5FFFF451": "sb.temporal.timezone",
    "SBSQL-2D1538908FB4": "sb.temporal.age",
    "SBSQL-3053BF29742E": "sb.scalar.regexp_split_to_table",
    "SBSQL-308BCDB4E875": "sb.temporal.day_name",
    "SBSQL-352D7A25CBF2": "sb.temporal.age_in_years",
    "SBSQL-3D0D5DEFD6B5": "sb.temporal.next_day",
    "SBSQL-3DF9A31D7101": "sb.temporal.date_bin",
    "SBSQL-4064D6205441": "sb.scalar.gen_id",
    "SBSQL-5159B04F9783": "sb.scalar.currval",
    "SBSQL-54956088D143": "sb.scalar.nextval",
    "SBSQL-5ECBB4B91523": "sb.temporal.date_bin",
    "SBSQL-5F642BC24E5F": "sb.scalar.occurrences_regex",
    "SBSQL-6421F1CDC60B": "sb.temporal.timezone",
    "SBSQL-6A44A39395D3": "sb.temporal.age_in_days",
    "SBSQL-76BE7E8C82E9": "sb.scalar.translate_regex",
    "SBSQL-7C1E9B3A101C": "sb.scalar.to_char",
    "SBSQL-7DE0B158322A": "sb.scalar.regexp_matches",
    "SBSQL-819EAB680D05": "sb.temporal.from_unixtime",
    "SBSQL-822B9F2AD5C5": "sb.scalar.lastval",
    "SBSQL-83DF52C71DB1": "sb.temporal.date_add",
    "SBSQL-8457926FEA71": "sb.scalar.to_date",
    "SBSQL-88E2E2EB0657": "sb.scalar.setval",
    "SBSQL-89C364139695": "sb.temporal.age_in_months",
    "SBSQL-924B70AB5641": "sb.temporal.month_name",
    "SBSQL-94D34F1E05AF": "sb.temporal.date_diff",
    "SBSQL-96A61FEA2B25": "sb.scalar.setval",
    "SBSQL-9AFD6269AD11": "sb.scalar.to_number",
    "SBSQL-9B20B713B248": "sb.scalar.to_timestamp",
    "SBSQL-9E51B994A03C": "sb.scalar.occurrences_regex",
    "SBSQL-A1FB79D234A0": "sb.temporal.months_between",
    "SBSQL-A48CE29C1EF8": "sb.temporal.next_day",
    "SBSQL-A66313AFAD59": "sb.temporal.months_between",
    "SBSQL-AB367C935012": "sb.scalar.substring_regex",
    "SBSQL-B18EB4D81617": "sb.temporal.date_diff",
    "SBSQL-B4E283C07390": "sb.scalar.to_date",
    "SBSQL-B684AF9349FE": "sb.scalar.regexp_split_to_table",
    "SBSQL-BF05630BC377": "sb.temporal.make_interval",
    "SBSQL-C4FAF4EDAF96": "sb.scalar.nextval",
    "SBSQL-CDD43912803F": "sb.scalar.to_timestamp",
    "SBSQL-D13340E9CF67": "sb.scalar.regexp_matches",
    "SBSQL-D14DD562957B": "sb.scalar.translate_regex",
    "SBSQL-D9E42211F7B8": "sb.scalar.to_char",
    "SBSQL-E4A673B0FADE": "sb.temporal.make_interval",
    "SBSQL-E6D846B78614": "sb.temporal.from_unixtime",
    "SBSQL-EA11A0912D91": "sb.scalar.substring_regex",
}

ALLOWED_CASE_KINDS = {"positive", "null_context"}
ALLOWED_DESCRIPTORS = {"array", "character", "date", "int64", "interval", "real64", "timestamp", "timestamp_tz"}


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
            errors.append(f"{surface_id}: duplicate SBSFC-058 fixture row")
        seen[surface_id] = row
        expected_function = TARGETS.get(surface_id)
        if expected_function is None:
            errors.append(f"{surface_id}: unexpected SBSFC-058 fixture row")
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
        errors.append(f"{surface_id}: missing SBSFC-058 fixture")

    if len(fixture_rows) != len(TARGETS):
        errors.append(f"expected {len(TARGETS)} fixture rows, found {len(fixture_rows)}")

    if errors:
        print(
            "sbsql_sbsfc_058_expression_runtime_function_fixture_gate found "
            f"{len(errors)} issue(s):",
            file=sys.stderr,
        )
        for error in errors:
            print(f" - {error}", file=sys.stderr)
        print("sbsql_sbsfc_058_expression_runtime_function_fixture_gate=failed", file=sys.stderr)
        return 1

    print("sbsql_sbsfc_058_expression_runtime_function_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
