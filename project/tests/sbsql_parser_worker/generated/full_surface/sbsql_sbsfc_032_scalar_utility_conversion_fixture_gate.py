#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-032 scalar utility/conversion fixture gate."""

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
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_032_SCALAR_UTILITY_CONVERSION_FIXTURES.csv"

TARGETS = {
    "sb.scalar.atan2d": {"SBSQL-23E118659719"},
    "sb.scalar.collation_for": {"SBSQL-6DFCF52729AD", "SBSQL-D989C17E1878"},
    "sb.scalar.descriptor_of": {"SBSQL-99C52D5953AD", "SBSQL-7A5C9806BEF5"},
    "sb.scalar.pg_typeof": {"SBSQL-CC379798CF3D", "SBSQL-3E3527E30D5F"},
    "sb.scalar.safe_cast": {"SBSQL-D6FBF57E26FC", "SBSQL-6A962F180717"},
    "sb.scalar.try_cast": {"SBSQL-78EE8FA84A8F", "SBSQL-77A5EAFF0CD5"},
    "sb.scalar.similar_to_escape": {"SBSQL-BC1A67FBC111", "SBSQL-254D0D3E1F58"},
    "sb.scalar.value_state": {"SBSQL-161D7B3339E9", "SBSQL-A442EACDA177"},
}

REQUIRED_FIXTURES = {
    "SBSFC032-atan2d-quadrant",
    "SBSFC032-collation-for-bare-invalid",
    "SBSFC032-collation-for-text",
    "SBSFC032-descriptor-of-bare-invalid",
    "SBSFC032-descriptor-of-expr",
    "SBSFC032-pg-typeof-bare-unknown",
    "SBSFC032-pg-typeof-expr",
    "SBSFC032-safe-cast-bare-invalid",
    "SBSFC032-safe-cast-expr-as-type",
    "SBSFC032-try-cast-bare-invalid",
    "SBSFC032-try-cast-expr-as-type-null-on-failure",
    "SBSFC032-similar-to-escape-bare-invalid",
    "SBSFC032-similar-to-escape-text",
    "SBSFC032-value-state-bare-invalid",
    "SBSFC032-value-state-any",
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
    registered_ids = set(re.findall(r'"(sb\.scalar\.(?:atan2d|collation_for|descriptor_of|pg_typeof|safe_cast|try_cast|similar_to_escape|value_state))"', seed_text))

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
            errors.append(f"{fixture_id}: positive case must not declare expected_result_diagnostic_code")

    missing_fixtures = REQUIRED_FIXTURES - seen_fixture_ids
    for fixture_id in sorted(missing_fixtures):
        errors.append(f"missing required fixture {fixture_id}")
    for canonical, surface_ids in sorted(TARGETS.items()):
        missing = surface_ids - seen_surface_ids[canonical]
        if missing:
            errors.append(f"{canonical}: missing required surface fixture ids {', '.join(sorted(missing))}")

    print(
        "sbsql_sbsfc_032_scalar_utility_conversion_fixture_gate "
        f"fixtures={len(fixtures)} canonicals={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_032_scalar_utility_conversion_fixture_gate=failed", file=sys.stderr)
        for error in errors[:40]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_032_scalar_utility_conversion_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
