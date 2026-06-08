#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-027 string/encoding helper scalar fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import defaultdict
from pathlib import Path


GENERATED_REGISTRY = "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.cpp"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
LOWERING = "project/src/parsers/sbsql_worker/lowering/lowering.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_027_STRING_ENCODING_HELPER_FIXTURES.csv"
AUTHORITY_REF = "SBSFC-027-string-encoding-helper-scalar"

TARGETS = {
    "sb.scalar.concat_ws": {
        "surfaces": {"SBSQL-A48C58E244D4", "SBSQL-5829DF9E679F"},
        "descriptor": "character",
    },
    "sb.scalar.split_part": {
        "surfaces": {"SBSQL-6DDFC8D53E41", "SBSQL-CB145A6D7A51"},
        "descriptor": "character",
    },
    "sb.scalar.string_to_array": {
        "surfaces": {"SBSQL-9F13AA94F8D4", "SBSQL-A540BC40955E"},
        "descriptor": "array",
    },
    "sb.scalar.convert_to": {
        "surfaces": {"SBSQL-796EC3F8D4C3", "SBSQL-64177C9CCDD6"},
        "descriptor": "binary",
    },
    "sb.scalar.convert_from": {
        "surfaces": {"SBSQL-67DBE514663F", "SBSQL-67FF765FE27A"},
        "descriptor": "character",
    },
}

REQUIRED_COLUMNS = [
    "fixture_id",
    "surface_id",
    "function_id",
    "canonical_builtin_id",
    "case_kind",
    "arguments_json",
    "expected_result_descriptor",
    "oracle_authority_ref",
    "notes",
]

REFUSAL_CASES = {"arity_error", "domain_error", "type_error"}


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

    fixtures = read_csv(root / FIXTURES)
    seed_text = (root / SEED_REGISTRY).read_text(encoding="utf-8")
    lowering_text = (root / LOWERING).read_text(encoding="utf-8")
    generated_registry_text = (root / GENERATED_REGISTRY).read_text(encoding="utf-8")

    registered_ids = set(
        re.findall(r'"(sb\.scalar\.(?:concat_ws|split_part|string_to_array|convert_to|convert_from))"', seed_text)
    )

    errors: list[str] = []
    seen_fixture_ids: set[str] = set()
    seen_surface_ids: dict[str, set[str]] = defaultdict(set)
    positive_by_builtin: dict[str, int] = defaultdict(int)
    null_by_builtin: dict[str, int] = defaultdict(int)
    refusal_by_builtin: dict[str, int] = defaultdict(int)

    for canonical in TARGETS:
        if canonical not in registered_ids:
            errors.append(f"{canonical}: canonical id not registered in function_seed_registry")
        if canonical not in lowering_text:
            errors.append(f"{canonical}: canonical id not present in lowering.cpp")
        for surface_id in TARGETS[canonical]["surfaces"]:
            if surface_id not in generated_registry_text:
                errors.append(f"{canonical}: surface_id {surface_id} missing from generated registry source")

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
        expected_value = (row.get("expected_result_value", "") or "").strip()
        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()
        expected_descriptor = (row.get("expected_result_descriptor", "") or "").strip()
        authority_ref = (row.get("oracle_authority_ref", "") or "").strip()

        if canonical not in TARGETS:
            errors.append(f"{fixture_id}: unexpected canonical_builtin_id {canonical}")
        else:
            target = TARGETS[canonical]
            if surface_id not in target["surfaces"]:
                errors.append(f"{fixture_id}: surface_id {surface_id} is not declared for {canonical}")
            else:
                seen_surface_ids[canonical].add(surface_id)
            if expected_descriptor != target["descriptor"]:
                errors.append(
                    f"{fixture_id}: expected_result_descriptor {expected_descriptor} "
                    f"does not match {target['descriptor']}"
                )

        if function_id != canonical:
            errors.append(f"{fixture_id}: function_id {function_id} does not match canonical_builtin_id {canonical}")
        if authority_ref != AUTHORITY_REF:
            errors.append(f"{fixture_id}: oracle_authority_ref must be {AUTHORITY_REF}")

        try:
            parsed_args = json.loads(row.get("arguments_json", ""))
            if not isinstance(parsed_args, list):
                errors.append(f"{fixture_id}: arguments_json must be a JSON list")
        except json.JSONDecodeError as exc:
            errors.append(f"{fixture_id}: arguments_json is not valid JSON: {exc}")

        if case_kind.startswith("null"):
            if expected_value or expected_diag:
                errors.append(f"{fixture_id}: null case must not declare expected value or diagnostic")
            null_by_builtin[canonical] += 1
        elif case_kind in REFUSAL_CASES:
            if expected_diag != "SB_DIAG_FUNCTION_INVALID_INPUT":
                errors.append(f"{fixture_id}: refusal case must declare SB_DIAG_FUNCTION_INVALID_INPUT")
            if expected_value:
                errors.append(f"{fixture_id}: refusal case must not declare expected_result_value")
            refusal_by_builtin[canonical] += 1
        else:
            if not expected_value:
                errors.append(f"{fixture_id}: positive case must declare expected_result_value")
            if expected_diag:
                errors.append(f"{fixture_id}: positive case must not declare expected_diagnostic_code")
            positive_by_builtin[canonical] += 1

    for canonical, target in sorted(TARGETS.items()):
        if positive_by_builtin[canonical] == 0:
            errors.append(f"{canonical}: no positive fixture")
        if null_by_builtin[canonical] == 0:
            errors.append(f"{canonical}: no null_strict fixture")
        if refusal_by_builtin[canonical] == 0:
            errors.append(f"{canonical}: no refusal fixture")
        missing = target["surfaces"] - seen_surface_ids[canonical]
        if missing:
            errors.append(f"{canonical}: missing required surface fixture ids {', '.join(sorted(missing))}")

    print(
        "sbsql_sbsfc_027_string_encoding_helper_fixture_gate "
        f"fixtures={len(fixtures)} canonicals={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_027_string_encoding_helper_fixture_gate=failed", file=sys.stderr)
        for error in errors[:40]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_027_string_encoding_helper_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
