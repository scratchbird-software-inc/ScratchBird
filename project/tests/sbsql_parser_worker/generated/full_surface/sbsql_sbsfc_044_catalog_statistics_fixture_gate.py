#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-044 catalog-statistics scalar fixture gate."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_044_CATALOG_STATISTICS_FIXTURES.csv"

TARGETS = {
    "sb.scalar.relation_row_estimate": {"SBSQL-92A5BAB00707", "SBSQL-545ECE804256"},
    "sb.scalar.table_size": {"SBSQL-84CF6043F00F", "SBSQL-C167CB99BFAD"},
}

REQUIRED_FIXTURES = {
    "SBSFC044-relation-row-estimate-catalog",
    "SBSFC044-relation-row-estimate-table",
    "SBSFC044-relation-row-estimate-null",
    "SBSFC044-relation-row-estimate-unknown",
    "SBSFC044-table-size-catalog",
    "SBSFC044-table-size-table-default",
    "SBSFC044-table-size-table-no-indexes",
    "SBSFC044-table-size-null-table",
    "SBSFC044-table-size-null-include-indexes",
    "SBSFC044-table-size-unknown",
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
        re.findall(r'"(sb\.scalar\.(?:relation_row_estimate|table_size))"', seed_text)
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
            errors.append(f"{fixture_id}: function_id {function_id} must match canonical {canonical}")
        if case_kind not in {"positive", "null_propagation"}:
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")
        if expected_diag:
            errors.append(f"{fixture_id}: SBSFC-044 fixtures must be successful non-refusal cases")

        note = (row.get("notes", "") or "").lower()
        if "mga" not in note or ("sidecar" not in note and "relation" not in note):
            errors.append(f"{fixture_id}: note must cite MGA relation sidecar authority")
        if any(forbidden in note for forbidden in ("parser sql", "donor", "wal", "sqlite")):
            if "without parser sql" not in note and "no parser sql" not in note:
                errors.append(f"{fixture_id}: note cites forbidden authority without refusal wording")

    missing = REQUIRED_FIXTURES - seen_fixture_ids
    if missing:
        errors.append(f"missing required fixture ids: {sorted(missing)}")
    extra = seen_fixture_ids - REQUIRED_FIXTURES
    if extra:
        errors.append(f"unexpected fixture ids: {sorted(extra)}")
    for canonical, surface_ids in TARGETS.items():
        if not surface_ids.issubset(seen_surface_ids[canonical]):
            errors.append(f"{canonical}: missing fixture coverage for {sorted(surface_ids - seen_surface_ids[canonical])}")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("sbsql_sbsfc_044_catalog_statistics_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
