#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-034 text/trigram/bit-string scalar fixture gate."""

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
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_034_TEXT_TRIGRAM_BIT_STRING_FIXTURES.csv"

TARGETS = {
    "sb.scalar.bit_string_position": {"SBSQL-D7C03289F1AA"},
    "sb.scalar.bit_string_substring": {"SBSQL-4CE572F88F79"},
    "sb.scalar.show_trgm": {"SBSQL-38CE72403078", "SBSQL-A2A770275F65"},
    "sb.scalar.pg_trgm": {"SBSQL-F01F3112C706"},
}

REQUIRED_FIXTURES = {
    "SBSFC034-bit-string-position",
    "SBSFC034-bit-string-substring",
    "SBSFC034-show-trgm-bare-invalid",
    "SBSFC034-show-trgm-text",
    "SBSFC034-show-trgm-null",
    "SBSFC034-pg-trgm-capability",
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
            r'"(sb\.scalar\.(?:bit_string_position|bit_string_substring|show_trgm|pg_trgm))"',
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

        if case_kind not in {"positive", "invalid_input", "null_strict"}:
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")
        if case_kind == "invalid_input" and expected_diag != "SB_DIAG_FUNCTION_INVALID_INPUT":
            errors.append(f"{fixture_id}: invalid_input case must declare SB_DIAG_FUNCTION_INVALID_INPUT")
        if case_kind in {"positive", "null_strict"} and expected_diag:
            errors.append(f"{fixture_id}: {case_kind} case must not declare expected_diagnostic_code")

    missing_fixtures = REQUIRED_FIXTURES - seen_fixture_ids
    for fixture_id in sorted(missing_fixtures):
        errors.append(f"missing required fixture {fixture_id}")
    for canonical, surface_ids in sorted(TARGETS.items()):
        missing = surface_ids - seen_surface_ids[canonical]
        if missing:
            errors.append(f"{canonical}: missing required surface fixture ids {', '.join(sorted(missing))}")

    print(
        "sbsql_sbsfc_034_text_trigram_bit_string_fixture_gate "
        f"fixtures={len(fixtures)} canonicals={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_034_text_trigram_bit_string_fixture_gate=failed", file=sys.stderr)
        for error in errors[:40]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_034_text_trigram_bit_string_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
