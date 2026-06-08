#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-045 privilege predicate scalar fixture gate."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_045_PRIVILEGE_PREDICATE_FIXTURES.csv"

TARGETS = {
    "sb.scalar.has_table_privilege": {"SBSQL-12D9D5393953", "SBSQL-809276DC4FE0"},
    "sb.scalar.has_column_privilege": {"SBSQL-2B2F87582D90", "SBSQL-BC5D89E8C4B8"},
    "sb.scalar.has_function_privilege": {"SBSQL-590198041265", "SBSQL-A88A53AD9C2D"},
    "sb.scalar.has_schema_privilege": {"SBSQL-F49AEC91691D", "SBSQL-DB663563FC1F"},
}

REQUIRED_FIXTURES = {
    "SBSFC045-has-table-privilege-current-owner",
    "SBSFC045-has-table-privilege-optional-user",
    "SBSFC045-has-table-privilege-null",
    "SBSFC045-has-table-privilege-unknown",
    "SBSFC045-has-column-privilege-current-owner",
    "SBSFC045-has-column-privilege-optional-user",
    "SBSFC045-has-column-privilege-null",
    "SBSFC045-has-column-privilege-unknown-column",
    "SBSFC045-has-function-privilege-current-owner",
    "SBSFC045-has-function-privilege-optional-user",
    "SBSFC045-has-function-privilege-null",
    "SBSFC045-has-function-privilege-unknown",
    "SBSFC045-has-schema-privilege-current-owner",
    "SBSFC045-has-schema-privilege-optional-user",
    "SBSFC045-has-schema-privilege-null",
    "SBSFC045-has-schema-privilege-unknown",
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
        re.findall(r'"(sb\.scalar\.has_(?:table|column|function|schema)_privilege)"', seed_text)
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
        expected_descriptor = row.get("expected_result_descriptor", "")

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
        if case_kind not in {"positive", "null_propagation", "negative_false"}:
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")
        if expected_diag:
            errors.append(f"{fixture_id}: SBSFC-045 fixtures must be successful non-refusal cases")
        if expected_descriptor != "boolean":
            errors.append(f"{fixture_id}: expected_result_descriptor must be boolean")

        note = (row.get("notes", "") or "").lower()
        if "security context" not in note:
            errors.append(f"{fixture_id}: note must cite security context authority")
        if not any(token in note for token in ("mga relation", "builtin metadata", "schema context")):
            errors.append(f"{fixture_id}: note must cite engine catalog metadata authority")
        if any(forbidden in note for forbidden in ("parser sql", "donor", "wal", "sqlite")):
            if "without parser sql donor wal sqlite" not in note:
                errors.append(f"{fixture_id}: note cites forbidden authority without refusal wording")
        if "cluster" in note and "without parser sql donor wal sqlite or cluster authority" not in note:
            errors.append(f"{fixture_id}: note cites cluster without fail-closed wording")

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
    print("sbsql_sbsfc_045_privilege_predicate_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
