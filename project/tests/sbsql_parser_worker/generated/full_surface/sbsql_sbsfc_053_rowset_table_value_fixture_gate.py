#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-053 rowset/table-value/set-returning fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_053_ROWSET_TABLE_VALUE_FIXTURES.csv"

TARGETS = {
    "SBSQL-957EA3F617A2": ("rowset", "sb.rowset.rowset"),
    "SBSQL-441883FA4E87": ("rowset_new", "sb.rowset.new"),
    "SBSQL-A3DADE5255A6": ("rowset_new(<row_shape>)", "sb.rowset.new"),
    "SBSQL-4C3F8279098E": ("rowset_append", "sb.rowset.append"),
    "SBSQL-1AFC18FA8618": ("rowset_append(rowset,expr[,expr...])", "sb.rowset.append"),
    "SBSQL-098E28A1F45B": ("rowset_size", "sb.rowset.size"),
    "SBSQL-50C1BBB6018E": ("rowset_size(rowset)", "sb.rowset.size"),
    "SBSQL-054E4DC54266": ("rowset_to_array", "sb.rowset.to_array"),
    "SBSQL-94F61E4D245C": ("rowset_to_array(rowset)", "sb.rowset.to_array"),
    "SBSQL-415E89D3266D": ("table_value", "sb.table_value.value"),
    "SBSQL-425230445B2C": ("table_value_new", "sb.table_value.new"),
    "SBSQL-8467B84B58DF": ("table_value_new(<row_shape>)", "sb.table_value.new"),
    "SBSQL-BB65E97117E9": ("table_value_append", "sb.table_value.append"),
    "SBSQL-24E967F07B8A": ("table_value_append(tv,row)", "sb.table_value.append"),
    "SBSQL-3278282AF7A1": ("setof(T,...,ordinalitybigint)", "sb.setof.generic"),
    "SBSQL-618842668D61": ("setof(keytext,valuetext)", "sb.setof.key_text_value_text"),
    "SBSQL-DC6373538835": ("setof(keytext,valuedocument)", "sb.setof.key_text_value_document"),
    "SBSQL-0D038FF22DA8": ("unnest", "sb.rowset.unnest"),
    "SBSQL-E11E27B45C94": ("unnest(array)", "sb.rowset.unnest"),
    "SBSQL-8A1E3E863769": ("generate_series", "sb.rowset.generate_series"),
    "SBSQL-38EE3D5E1400": ("generate_series(start,stop[,step])", "sb.rowset.generate_series"),
    "SBSQL-01B057BBC0EA": ("element(multiset<T>)", "sb.multiset.element"),
    "SBSQL-4B19CB6607C3": ("fusion(multiset<T>)", "sb.multiset.fusion"),
    "SBSQL-6B9810626A72": ("intersection(multiset<T>)", "sb.multiset.intersection"),
}

REQUIRED_INVALID_FIXTURES = {
    "SBSFC053-rowset-append-missing-input",
    "SBSFC053-generate-series-step-zero",
    "SBSFC053-unnest-scalar-invalid",
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
    registered_ids = set(
        re.findall(r'"(sb\.(?:rowset|table_value|setof|multiset)\.[^"]+)"', seed_text)
    )

    errors: list[str] = []
    positive_by_surface: dict[str, set[str]] = {surface_id: set() for surface_id in TARGETS}
    invalid_seen: set[str] = set()
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
        expected_descriptor = row.get("expected_result_descriptor", "")

        target = TARGETS.get(surface_id)
        if target is None:
            errors.append(f"{fixture_id}: unexpected surface_id {surface_id}")
            continue
        expected_name, expected_function_id = target
        if function_id != expected_function_id or canonical != expected_function_id:
            errors.append(f"{fixture_id}: function/canonical id mismatch for {surface_id}")
        if function_id not in registered_ids:
            errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")

        surface = surfaces.get(surface_id)
        if surface is None:
            errors.append(f"{fixture_id}: surface_id missing from registry")
        else:
            if surface.get("status") != "native_now":
                errors.append(f"{fixture_id}: surface_id is not native_now")
            if surface.get("canonical_name") != expected_name:
                errors.append(
                    f"{fixture_id}: canonical_name mismatch: "
                    f"{surface.get('canonical_name')} != {expected_name}"
                )

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
            if expected_descriptor not in {"json_document", "int64"}:
                errors.append(f"{fixture_id}: unexpected positive descriptor {expected_descriptor}")
        elif case_kind == "invalid_input":
            invalid_seen.add(fixture_id)
            if expected_diag != "SB_DIAG_FUNCTION_INVALID_INPUT":
                errors.append(f"{fixture_id}: invalid_input must declare SB_DIAG_FUNCTION_INVALID_INPUT")
        else:
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")

    for surface_id, fixtures_for_surface in positive_by_surface.items():
        if not fixtures_for_surface:
            errors.append(f"{surface_id}: missing positive fixture")
    missing_invalid = REQUIRED_INVALID_FIXTURES - invalid_seen
    for fixture_id in sorted(missing_invalid):
        errors.append(f"{fixture_id}: missing required invalid-input fixture")

    print(
        "sbsql_sbsfc_053_rowset_table_value_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_053_rowset_table_value_fixture_gate=failed", file=sys.stderr)
        for error in errors[:100]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_053_rowset_table_value_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
