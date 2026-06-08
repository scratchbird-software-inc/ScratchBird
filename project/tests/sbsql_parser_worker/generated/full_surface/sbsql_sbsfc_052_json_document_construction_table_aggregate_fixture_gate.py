#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-052 JSON/document construction table aggregate fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_052_JSON_DOCUMENT_CONSTRUCTION_TABLE_AGGREGATE_FIXTURES.csv"

TARGETS = {
    "SBSQL-E4C08DADB61A": "sb.json.table",
    "SBSQL-433AC9801679": "sb.json.table",
    "SBSQL-22963E18DC40": "sb.json.table",
    "SBSQL-2866302407B6": "sb.json.array_to_json",
    "SBSQL-579AE2ED91B2": "sb.json.array_to_json",
    "SBSQL-4DBBCD45F15C": "sb.json.object_text_array",
    "SBSQL-5F35CBE51FA4": "sb.json.jsonb_agg",
    "SBSQL-F9F64D586108": "sb.json.jsonb_agg",
    "SBSQL-EA3286F7FED5": "sb.json.row_to_json",
    "SBSQL-1E99FF5633C4": "sb.json.row_to_json",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    surfaces = {row["surface_id"]: row for row in read_csv(root / SURFACE_REGISTRY)}
    fixtures = read_csv(root / FIXTURES)
    seed_text = (root / SEED_REGISTRY).read_text(encoding="utf-8")
    registered_ids = set(re.findall(r'"(sb\.json\.[^"]+)"', seed_text))

    errors: list[str] = []
    positive_by_surface: dict[str, set[str]] = {surface_id: set() for surface_id in TARGETS}
    invalid_seen = False
    fixture_ids: set[str] = set()

    for row in fixtures:
        fixture_id = row.get("fixture_id", "")
        if not fixture_id:
            errors.append("fixture row missing fixture_id")
            continue
        if fixture_id in fixture_ids:
            errors.append(f"{fixture_id}: duplicate fixture_id")
        fixture_ids.add(fixture_id)

        surface_id = row.get("surface_id", "")
        function_id = row.get("function_id", "")
        canonical = row.get("canonical_builtin_id", "")
        case_kind = row.get("case_kind", "")
        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()

        if surface_id not in TARGETS:
            errors.append(f"{fixture_id}: unexpected surface_id {surface_id}")
            continue
        if function_id != TARGETS[surface_id] or canonical != TARGETS[surface_id]:
            errors.append(f"{fixture_id}: function/canonical id mismatch for {surface_id}")
        if function_id not in registered_ids:
            errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")

        surface = surfaces.get(surface_id)
        if surface is None:
            errors.append(f"{fixture_id}: surface_id missing from registry")
        elif surface.get("status") != "native_now":
            errors.append(f"{fixture_id}: surface_id is not native_now")

        try:
            parsed_args = json.loads(row.get("arguments_json", ""))
            if not isinstance(parsed_args, list):
                errors.append(f"{fixture_id}: arguments_json must be a JSON list")
        except json.JSONDecodeError as exc:
            errors.append(f"{fixture_id}: invalid arguments_json: {exc}")

        if case_kind == "positive":
            positive_by_surface[surface_id].add(fixture_id)
            if expected_diag:
                errors.append(f"{fixture_id}: positive fixture must not declare diagnostic")
        elif case_kind == "invalid_input":
            invalid_seen = expected_diag == "SB_DIAG_FUNCTION_INVALID_INPUT" or invalid_seen
            if expected_diag != "SB_DIAG_FUNCTION_INVALID_INPUT":
                errors.append(f"{fixture_id}: invalid_input must declare SB_DIAG_FUNCTION_INVALID_INPUT")
        else:
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")

    for surface_id, fixtures_for_surface in positive_by_surface.items():
        if not fixtures_for_surface:
            errors.append(f"{surface_id}: missing positive fixture")
    if not invalid_seen:
        errors.append("missing invalid-input diagnostic fixture")

    print(
        "sbsql_sbsfc_052_json_document_construction_table_aggregate_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_052_json_document_construction_table_aggregate_fixture_gate=failed", file=sys.stderr)
        for error in errors[:80]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_052_json_document_construction_table_aggregate_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
