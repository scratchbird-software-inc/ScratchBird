#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-056 native surface scalar/descriptor fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_056_NATIVE_SURFACE_FIXTURES.csv"

TARGETS = {
    "SBSQL-DF502F8DF4FA": "sb.scalar.accept",
    "SBSQL-8CBB8186C7CC": "sb.scalar.close",
    "SBSQL-755DD39EA853": "sb.scalar.future_version",
    "SBSQL-B30BB888C751": "sb.scalar.gap",
    "SBSQL-CD2216F125FB": "sb.scalar.immutable",
    "SBSQL-14EDC2636B45": "sb.scalar.match_recognize",
    "SBSQL-C4027F6E6C8A": "sb.scalar.open",
    "SBSQL-67B876B5339F": "sb.scalar.reserved",
    "SBSQL-4AF1FA4C5BBC": "sb.scalar.sbsql_syntax_future_version",
    "SBSQL-4975481A1AB7": "sb.scalar.sbsql_syntax_reserved",
    "SBSQL-8893D25F387F": "sb.scalar.stable",
    "SBSQL-ABD89A468ECA": "sb.scalar.treat",
    "SBSQL-504CEBDC6FE1": "sb.scalar.treat_typed",
    "SBSQL-2E11730BB92B": "sb.scalar.volatile",
    "SBSQL-12CD234538AF": "sb.scalar.accept_sql2016_timeseries",
    "SBSQL-A23C7082573D": "sb.aggregate.any_value",
    "SBSQL-76EC89319569": "sb.aggregate.any_value_expr",
    "SBSQL-6C877B4376DE": "sb.scalar.at_time_zone",
    "SBSQL-E17CFDACCB8E": "sb.scalar.bit_string",
    "SBSQL-4E24AE0D0EDE": "sb.scalar.bulk_exceptions",
    "SBSQL-D03ED69E33B7": "sb.aggregate.collect",
    "SBSQL-A1B94C83C5F1": "sb.aggregate.collect_expr",
    "SBSQL-5550BDA0A76C": "sb.scalar.domain_stack",
    "SBSQL-1906412209C9": "sb.scalar.domain_stack_value",
    "SBSQL-8F66D89149F5": "sb.scalar.donor_only",
    "SBSQL-F785EAF383DE": "sb.scalar.donor_rewrite",
    "SBSQL-FD0DF4067008": "sb.multiset.element",
    "SBSQL-1A8470FC95E7": "sb.expr.match_recognize.v1",
    "SBSQL-9F6F909938A0": "sb.multiset.fusion",
    "SBSQL-DB32CA47B7B5": "sb.type.integer",
    "SBSQL-9C90F3645C34": "sb.multiset.intersection",
    "SBSQL-AC8794BE30FE": "sb.scalar.native_future",
    "SBSQL-3F50B9923297": "sb.scalar.native_now",
    "SBSQL-83969495B383": "sb.scalar.nvl",
    "SBSQL-75F997655797": "sb.scalar.private_only",
    "SBSQL-F94025D79003": "sb.scalar.tabular",
    "SBSQL-FB4A06130103": "sb.scalar.void",
}

REQUIRED_INVALID_FIXTURES = {
    "SBSFC056-bit-string-invalid",
    "SBSFC056-nvl-missing",
    "SBSFC056-integer-invalid",
}

ALLOWED_DESCRIPTORS = {
    "character",
    "json_document",
    "boolean",
    "timestamp_tz",
    "bit_string",
    "type_descriptor",
    "varchar",
    "void",
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
    registered_ids = set(re.findall(r'"(sb\.[^"]+)"', seed_text))

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

        try:
            parsed_args = json.loads(row.get("arguments_json", ""))
            if not isinstance(parsed_args, list):
                errors.append(f"{fixture_id}: arguments_json must be a JSON list")
        except json.JSONDecodeError as exc:
            errors.append(f"{fixture_id}: invalid arguments_json: {exc}")

        if case_kind == "invalid_input":
            invalid_seen.add(fixture_id)
            if expected_diag != "SB_DIAG_FUNCTION_INVALID_INPUT":
                errors.append(f"{fixture_id}: invalid_input must declare SB_DIAG_FUNCTION_INVALID_INPUT")
            if function_id not in registered_ids:
                errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")
            continue

        if case_kind != "positive":
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")
            continue

        expected_function_id = TARGETS.get(surface_id)
        if expected_function_id is None:
            errors.append(f"{fixture_id}: unexpected surface_id {surface_id}")
            continue
        if function_id != expected_function_id or canonical != expected_function_id:
            errors.append(f"{fixture_id}: function/canonical id mismatch for {surface_id}")
        if function_id not in registered_ids:
            errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")
        if expected_diag:
            errors.append(f"{fixture_id}: positive fixture must not declare diagnostic")
        if row.get("expected_result_descriptor", "") not in ALLOWED_DESCRIPTORS:
            errors.append(f"{fixture_id}: unexpected positive descriptor {row.get('expected_result_descriptor', '')}")

        surface = surfaces.get(surface_id)
        if surface is None:
            errors.append(f"{fixture_id}: surface_id missing from registry")
        else:
            if surface.get("status") != "native_now":
                errors.append(f"{fixture_id}: surface_id is not native_now")
            if surface.get("sblr_operation_family") != "sblr.expression.runtime.v3":
                errors.append(f"{fixture_id}: unexpected SBLR family {surface.get('sblr_operation_family')}")
        positive_by_surface[surface_id].add(fixture_id)

    for surface_id, fixtures_for_surface in positive_by_surface.items():
        if not fixtures_for_surface:
            errors.append(f"{surface_id}: missing positive fixture")
    for fixture_id in sorted(REQUIRED_INVALID_FIXTURES - invalid_seen):
        errors.append(f"{fixture_id}: missing required invalid-input fixture")

    print(
        "sbsql_sbsfc_056_native_surface_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_056_native_surface_fixture_gate=failed", file=sys.stderr)
        for error in errors[:100]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_056_native_surface_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
