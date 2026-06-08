#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-033 catalog/descriptor/diagnostic scalar fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_033_CATALOG_DESCRIPTOR_DIAGNOSTIC_FIXTURES.csv"

TARGETS = {
    "sb.scalar.catalog_object_owner": {"SBSQL-06BFD87D6529", "SBSQL-D26A0353E396"},
    "sb.scalar.catalog_object_uuid": {"SBSQL-1C8329808D20", "SBSQL-400622501328"},
    "sb.scalar.catalog_object_name": {"SBSQL-99BB305208AD", "SBSQL-61072961FEDB"},
    "sb.scalar.catalog_object_class": {"SBSQL-E1CB2F1D2656", "SBSQL-A9F113392815"},
    "sb.scalar.descriptor_snapshot_id": {"SBSQL-352707AC1CAE"},
    "sb.scalar.execution_type_descriptor": {"SBSQL-BE412B3728C3"},
    "sb.scalar.column_descriptor": {"SBSQL-62343F602D38", "SBSQL-67431DA0E42F"},
    "sb.scalar.index_descriptor": {"SBSQL-95EA30EEFDEB", "SBSQL-A209C2B5CDDD"},
    "sb.scalar.diagnostic_field": {"SBSQL-780AF496F174", "SBSQL-6A13011127CF"},
    "sb.scalar.diagnostic_count": {"SBSQL-78B6ABBE922C"},
    "sb.scalar.gdscode": {"SBSQL-0D860B4A13B7"},
    "sb.scalar.last_error_position": {"SBSQL-E00EAE7EDC3C"},
    "sb.scalar.error_class": {"SBSQL-E36B1B028CC2"},
}

REQUIRED_FIXTURES = {
    "SBSFC033-catalog-object-owner-bare",
    "SBSFC033-catalog-object-owner-uuid",
    "SBSFC033-catalog-object-uuid-bare",
    "SBSFC033-catalog-object-uuid-name-class",
    "SBSFC033-catalog-object-name-bare",
    "SBSFC033-catalog-object-name-uuid",
    "SBSFC033-catalog-object-class-bare",
    "SBSFC033-catalog-object-class-uuid",
    "SBSFC033-descriptor-snapshot-id",
    "SBSFC033-execution-type-descriptor",
    "SBSFC033-column-descriptor-table-column",
    "SBSFC033-column-descriptor-bare",
    "SBSFC033-index-descriptor-bare",
    "SBSFC033-index-descriptor-unknown",
    "SBSFC033-diagnostic-field-bare",
    "SBSFC033-diagnostic-field-name",
    "SBSFC033-diagnostic-count",
    "SBSFC033-gdscode",
    "SBSFC033-last-error-position",
    "SBSFC033-error-class",
}

REQUIRED_COLUMNS = [
    "fixture_id",
    "surface_id",
    "function_id",
    "canonical_builtin_id",
    "case_kind",
    "arguments_json",
    "expected_result_value",
    "expected_result_descriptor",
    "oracle_authority_ref",
    "notes",
]


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
        re.findall(
            r'"(sb\.scalar\.(?:catalog_object_owner|catalog_object_uuid|catalog_object_name|'
            r'catalog_object_class|descriptor_snapshot_id|execution_type_descriptor|'
            r'column_descriptor|index_descriptor|diagnostic_field|diagnostic_count|'
            r'gdscode|last_error_position|error_class))"',
            seed_text,
        )
    )

    errors: list[str] = []
    seen_fixture_ids: set[str] = set()
    seen_surface_ids: dict[str, set[str]] = defaultdict(set)

    for row in fixtures:
        fixture_id = row.get("fixture_id", "")
        if not fixture_id:
            errors.append("fixture row missing fixture_id")
            continue
        if fixture_id in seen_fixture_ids:
            errors.append(f"{fixture_id}: duplicate fixture_id")
        seen_fixture_ids.add(fixture_id)

        for column in REQUIRED_COLUMNS:
            if not (row.get(column, "") or "").strip():
                errors.append(f"{fixture_id}: required column {column} is empty")

        canonical = row.get("canonical_builtin_id", "")
        function_id = row.get("function_id", "")
        surface_id = row.get("surface_id", "")
        case_kind = row.get("case_kind", "")
        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()

        if canonical not in TARGETS:
            errors.append(f"{fixture_id}: unexpected canonical_builtin_id {canonical}")
        elif surface_id not in TARGETS[canonical]:
            errors.append(f"{fixture_id}: surface_id {surface_id} is not declared for {canonical}")
        else:
            seen_surface_ids[canonical].add(surface_id)

        surface = surfaces.get(surface_id)
        if surface is None:
            errors.append(f"{fixture_id}: surface_id {surface_id} not in canonical surface registry")
        elif surface.get("status") != "native_now":
            errors.append(f"{fixture_id}: surface_id {surface_id} is not native_now")
        if canonical and canonical not in registered_ids:
            errors.append(f"{fixture_id}: canonical_builtin_id {canonical} not registered in function_seed_registry")
        if function_id != canonical:
            errors.append(f"{fixture_id}: function_id {function_id} does not match canonical_builtin_id {canonical}")

        try:
            parsed_args = json.loads(row.get("arguments_json", ""))
            if not isinstance(parsed_args, list):
                errors.append(f"{fixture_id}: arguments_json must be a JSON list")
        except json.JSONDecodeError as exc:
            errors.append(f"{fixture_id}: arguments_json is not valid JSON: {exc}")

        if case_kind not in {"positive", "invalid_input"}:
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")
        if case_kind == "invalid_input" and expected_diag != "SB_DIAG_FUNCTION_INVALID_INPUT":
            errors.append(f"{fixture_id}: invalid_input case must declare SB_DIAG_FUNCTION_INVALID_INPUT")
        if case_kind == "positive" and expected_diag:
            errors.append(f"{fixture_id}: positive case must not declare expected_diagnostic_code")

    missing_fixtures = REQUIRED_FIXTURES - seen_fixture_ids
    for fixture_id in sorted(missing_fixtures):
        errors.append(f"missing required fixture {fixture_id}")
    for canonical, surface_ids in sorted(TARGETS.items()):
        missing = surface_ids - seen_surface_ids[canonical]
        if missing:
            errors.append(f"{canonical}: missing required surface fixture ids {', '.join(sorted(missing))}")

    print(
        "sbsql_sbsfc_033_catalog_descriptor_diagnostic_fixture_gate "
        f"fixtures={len(fixtures)} canonicals={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_033_catalog_descriptor_diagnostic_fixture_gate=failed", file=sys.stderr)
        for error in errors[:40]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_033_catalog_descriptor_diagnostic_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
