#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-031 txid surface fixture gate."""

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
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_031_TXID_SURFACE_FIXTURES.csv"

TARGETS = {
    "sb.session.transaction_id": {"SBSQL-8A70982AB170"},
    "sb.session.txid_status": {"SBSQL-F239E35B53FE", "SBSQL-2D1AB3761105"},
    "sb.scalar.mga_snapshot_id": {"SBSQL-6990C01E5452"},
    "sb.session.pg_xact_status": {"SBSQL-B475E4E3EEA6"},
    "sb.session.savepoint_active": {"SBSQL-64CEC9E89DD9", "SBSQL-854048B3A018"},
}

REQUIRED_FIXTURES = {
    "SBSFC031-txid-current-context",
    "SBSFC031-txid-current-no-context-null",
    "SBSFC031-txid-status-current-in-progress",
    "SBSFC031-txid-status-no-context-null",
    "SBSFC031-txid-status-bigint-in-progress",
    "SBSFC031-txid-status-bigint-unknown",
    "SBSFC031-txid-status-bigint-null",
    "SBSFC031-mga-snapshot-id-context",
    "SBSFC031-mga-snapshot-id-no-context-null",
    "SBSFC031-pg-xact-status-current-in-progress",
    "SBSFC031-pg-xact-status-no-context-null",
    "SBSFC031-pg-xact-status-bigint-in-progress",
    "SBSFC031-pg-xact-status-bigint-unknown",
    "SBSFC031-pg-xact-status-bigint-null",
    "SBSFC031-savepoint-active-any-false",
    "SBSFC031-savepoint-active-any-true",
    "SBSFC031-savepoint-active-name-true",
    "SBSFC031-savepoint-active-name-missing-false",
    "SBSFC031-savepoint-active-name-false-after-release",
    "SBSFC031-savepoint-active-name-null",
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
    registered_ids = set(re.findall(
        r'"((?:sb\.session\.(?:transaction_id|txid_status|pg_xact_status|savepoint_active))|(?:sb\.scalar\.mga_snapshot_id))"',
        seed_text,
    ))

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
        expected_value = (row.get("expected_result_value", "") or "").strip()
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

        if row.get("case_kind") != "positive":
            errors.append(f"{fixture_id}: SBSFC-031 only publishes deterministic positive/null cases")
        if not expected_value:
            errors.append(f"{fixture_id}: expected_result_value is required")
        if expected_diag:
            errors.append(f"{fixture_id}: positive case must not declare expected_result_diagnostic_code")

    missing_fixtures = REQUIRED_FIXTURES - seen_fixture_ids
    for fixture_id in sorted(missing_fixtures):
        errors.append(f"missing required fixture {fixture_id}")
    for canonical, surface_ids in sorted(TARGETS.items()):
        missing = surface_ids - seen_surface_ids[canonical]
        if missing:
            errors.append(f"{canonical}: missing required surface fixture ids {', '.join(sorted(missing))}")

    print(
        "sbsql_sbsfc_031_txid_surface_fixture_gate "
        f"fixtures={len(fixtures)} canonicals={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_031_txid_surface_fixture_gate=failed", file=sys.stderr)
        for error in errors[:30]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_031_txid_surface_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
