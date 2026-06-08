#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-014 vector fixture gate.

Validates row-level fixture evidence for the scalar vector surface rows.
This is a static evidence gate only; runtime behavior is covered by
`sbsql_sbsfc_014_vector_runtime_conformance`.
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
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_014_VECTOR_BUILTIN_FIXTURES.csv"

REQUIRED_COLUMNS = [
    "fixture_id",
    "surface_id",
    "function_id",
    "canonical_builtin_id",
    "case_kind",
    "arguments_json",
    "expected_result_value",
    "expected_result_descriptor",
    "expected_diagnostic_code",
    "oracle_authority_ref",
    "notes",
]

TARGETS = {
    "sb.vector.vector": {"SBSQL-CD9843963529", "SBSQL-DBCE1186E9CE"},
    "sb.vector.vector_dims": {"SBSQL-4115F7B3F459", "SBSQL-C82CD2448FC0"},
    "sb.vector.vector_norm": {"SBSQL-5BA020DF9977", "SBSQL-FEB5761FAF29"},
    "sb.vector.vector_sum": {"SBSQL-09ABD3B9C0D4", "SBSQL-6EBD4C7CB915"},
    "sb.vector.vector_avg": {"SBSQL-6A0DD02E0445", "SBSQL-BFD7D5BD1995"},
    "sb.vector.l2_distance": {"SBSQL-9D2789A81562", "SBSQL-0A79694FE93C"},
    "sb.vector.cosine_distance": {"SBSQL-B706AC22E3F0", "SBSQL-A9D992B92872"},
    "sb.vector.inner_product": {"SBSQL-B04FAE2CB645", "SBSQL-BD65B1E34D96"},
    "sb.vector.negative_inner_product": {"SBSQL-F57618A8B0BB", "SBSQL-5E97D501D992"},
    "sb.vector.hamming_distance": {"SBSQL-0B74FC4EAF28", "SBSQL-4711C0478840"},
    "sb.vector.vector_l2_normalize": {"SBSQL-5E85E51AD1BB", "SBSQL-F5CEBB882D6C"},
    "sb.vector.subvector": {"SBSQL-29F6F6A31027", "SBSQL-609AA171BBEC"},
    "sb.vector.vector_cast_int8": {"SBSQL-3BC5399D0F15", "SBSQL-0D938B14D724"},
    "sb.vector.vector_cast_float16": {"SBSQL-94114FAB3F1C", "SBSQL-5EAE338151D1"},
}

REFUSAL_KINDS = {"arity_error", "domain_error", "type_error", "overflow"}


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
    registered_ids = set(re.findall(r'"(sb\.vector\.[\w.]+)"', seed_text))

    errors: list[str] = []
    seen_fixture_ids: set[str] = set()
    seen_surface_ids_by_builtin: dict[str, set[str]] = defaultdict(set)
    positive_by_builtin: dict[str, int] = defaultdict(int)
    null_by_builtin: dict[str, int] = defaultdict(int)
    refusal_by_builtin: dict[str, int] = defaultdict(int)

    for row in fixtures:
        fixture_id = row.get("fixture_id", "")
        if not fixture_id:
            errors.append("fixture row missing fixture_id")
            continue
        if fixture_id in seen_fixture_ids:
            errors.append(f"{fixture_id}: duplicate fixture_id")
        seen_fixture_ids.add(fixture_id)

        for column in REQUIRED_COLUMNS:
            if column not in row:
                errors.append(f"{fixture_id}: required column {column} is missing")
                continue
            if column not in {"expected_result_value", "expected_diagnostic_code"} and not (row.get(column, "") or "").strip():
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
        elif case_kind in REFUSAL_KINDS:
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

    expected_surface_ids = set().union(*TARGETS.values())
    observed_surface_ids = {row.get("surface_id", "") for row in fixtures if row.get("surface_id", "")}
    extra_surface_ids = observed_surface_ids - expected_surface_ids
    if extra_surface_ids:
        errors.append(f"unexpected vector fixture surface ids {', '.join(sorted(extra_surface_ids))}")

    print(
        "sbsql_sbsfc_014_vector_fixture_gate "
        f"fixtures={len(fixtures)} canonicals={len(TARGETS)} refusals={sum(refusal_by_builtin.values())} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_014_vector_fixture_gate=failed", file=sys.stderr)
        for error in errors[:40]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_014_vector_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
