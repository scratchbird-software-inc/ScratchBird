#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-014 fuzzy/phonetic fixture gate.

Static gate only: validates fixture structure, canonical surface IDs, fixed
target builtin IDs, and seed-registry registration. It does not promote status
artifacts or execute parser/engine paths.
"""

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
BUILTIN_REGISTRY = "public_contract_snapshot"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_014_FUZZY_TEXT_FIXTURES.csv"

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

TARGETS = {
    "sb.scalar.soundex": {"SBSQL-268AC6558734", "SBSQL-A55C406B74CC"},
    "sb.scalar.metaphone": {"SBSQL-0752129C568E", "SBSQL-FBF867953952"},
    "sb.scalar.dmetaphone": {"SBSQL-8E7688A340AA", "SBSQL-B64F6DF63A9B"},
    "sb.scalar.dmetaphone_alt": {"SBSQL-86176123AE39", "SBSQL-A742FEA707AC"},
    "sb.scalar.levenshtein": {"SBSQL-3FFEC260208E", "SBSQL-9D13C2A55E82"},
    "sb.scalar.levenshtein_le": {"SBSQL-DFBD84A09436", "SBSQL-798A9C6439FC"},
    "sb.scalar.damerau_levenshtein": {"SBSQL-B42684BD946D", "SBSQL-ACBB5242C20F"},
    "sb.scalar.jaro_similarity": {"SBSQL-C1EAABBFE490", "SBSQL-58D9AE253602"},
    "sb.scalar.jaro_winkler_similarity": {"SBSQL-88211FEE0FE4", "SBSQL-6053F750107B"},
    "sb.scalar.similarity": {"SBSQL-0546AEB18B4D", "SBSQL-6BF92553FF34"},
    "sb.scalar.word_similarity": {"SBSQL-253B19650892", "SBSQL-24DA1B54B0CB"},
    "sb.regex.match": {"SBSQL-5B98209F441E"},
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
    builtin_text = (root / BUILTIN_REGISTRY).read_text(encoding="utf-8")
    registered_ids = set(re.findall(r'"(sb\.(?:scalar|regex)\.[\w.]+)"', seed_text))

    errors: list[str] = []
    seen_ids: set[str] = set()
    seen_surface_ids_by_builtin: dict[str, set[str]] = defaultdict(set)
    positive_by_builtin: dict[str, int] = defaultdict(int)
    null_by_builtin: dict[str, int] = defaultdict(int)
    refusal_by_builtin: dict[str, int] = defaultdict(int)

    for row in fixtures:
        fixture_id = row.get("fixture_id", "")
        if not fixture_id:
            errors.append("fixture row missing fixture_id")
            continue
        if fixture_id in seen_ids:
            errors.append(f"{fixture_id}: duplicate fixture_id")
        seen_ids.add(fixture_id)

        for column in REQUIRED_COLUMNS:
            if not (row.get(column, "") or "").strip():
                errors.append(f"{fixture_id}: required column {column} is empty")

        canonical = row.get("canonical_builtin_id", "")
        function_id = row.get("function_id", "")
        surface_id = row.get("surface_id", "")
        case_kind = row.get("case_kind", "")
        expected_value = (row.get("expected_result_value", "") or "").strip()
        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()

        if canonical not in TARGETS:
            errors.append(f"{fixture_id}: unexpected canonical_builtin_id {canonical}")
        elif surface_id not in TARGETS[canonical]:
            errors.append(f"{fixture_id}: surface_id {surface_id} is not declared for {canonical}")
        else:
            seen_surface_ids_by_builtin[canonical].add(surface_id)

        if surface_id and surface_id not in surfaces:
            errors.append(f"{fixture_id}: surface_id {surface_id} not in canonical surface registry")
        if canonical and canonical not in registered_ids:
            errors.append(f"{fixture_id}: canonical_builtin_id {canonical} not registered in function_seed_registry")
        if canonical and f"builtin_id: {canonical}" not in builtin_text:
            errors.append(f"{fixture_id}: canonical_builtin_id {canonical} missing from builtin-expression-registry")
        if function_id != canonical:
            errors.append(f"{fixture_id}: function_id {function_id} does not match canonical_builtin_id {canonical}")

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
        elif case_kind in {"arity_error", "domain_error", "type_error", "overflow"}:
            if not expected_diag:
                errors.append(f"{fixture_id}: refusal case must declare expected_diagnostic_code")
            if expected_value:
                errors.append(f"{fixture_id}: refusal case must not declare expected_result_value")
            refusal_by_builtin[canonical] += 1
        else:
            if not expected_value:
                errors.append(f"{fixture_id}: positive case must declare expected_result_value")
            if expected_diag:
                errors.append(f"{fixture_id}: positive case must not declare expected_diagnostic_code")
            positive_by_builtin[canonical] += 1

    for canonical in sorted(TARGETS):
        if positive_by_builtin[canonical] == 0:
            errors.append(f"{canonical}: no positive fixture")
        if null_by_builtin[canonical] == 0:
            errors.append(f"{canonical}: no null_strict fixture")
        missing_surface_ids = TARGETS[canonical] - seen_surface_ids_by_builtin[canonical]
        if missing_surface_ids:
            errors.append(
                f"{canonical}: missing required surface fixture ids {', '.join(sorted(missing_surface_ids))}"
            )

    print(
        "sbsql_sbsfc_014_fuzzy_text_fixture_gate "
        f"fixtures={len(fixtures)} canonicals={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_014_fuzzy_text_fixture_gate=failed", file=sys.stderr)
        for error in errors[:30]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_014_fuzzy_text_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
