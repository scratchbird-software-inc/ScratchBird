#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-046 session-admin scalar fixture gate."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_046_SESSION_ADMIN_FIXTURES.csv"

TARGETS = {
    "sb.scalar.set_config": {"SBSQL-293E7F73D57E"},
    "sb.scalar.set_config_name_value_is_local": {"SBSQL-5F6C7C137D61"},
    "sb.scalar.pid": {"SBSQL-68FAFF2DC5B5"},
    "sb.scalar.temp_buffers": {"SBSQL-8AEA24D52907"},
    "sb.scalar.pg_cancel_backend": {"SBSQL-A9980035E562"},
    "sb.scalar.pg_cancel_backend_pid": {"SBSQL-448BCEA87184"},
    "sb.scalar.pg_terminate_backend": {"SBSQL-33BC7C3DE12B"},
    "sb.scalar.pg_terminate_backend_pid": {"SBSQL-B81698E45C53"},
}

REQUIRED_FIXTURES = {
    "SBSFC046-set-config-marker",
    "SBSFC046-set-config-apply-timezone",
    "SBSFC046-pid-positive",
    "SBSFC046-temp-buffers-default",
    "SBSFC046-temp-buffers-applied",
    "SBSFC046-pg-cancel-backend-marker",
    "SBSFC046-pg-cancel-backend-unknown",
    "SBSFC046-pg-cancel-backend-current",
    "SBSFC046-pg-terminate-backend-marker",
    "SBSFC046-pg-terminate-backend-unknown",
    "SBSFC046-pg-terminate-backend-self-blocked",
    "SBSFC046-pg-terminate-backend-controlled",
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
    registered_ids = set(re.findall(r'"(sb\.scalar\.(?:set_config|set_config_name_value_is_local|pid|temp_buffers|pg_cancel_backend|pg_cancel_backend_pid|pg_terminate_backend|pg_terminate_backend_pid))"', seed_text))

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
        if case_kind != "positive":
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")
        if expected_diag:
            errors.append(f"{fixture_id}: SBSFC-046 fixtures must be successful non-refusal cases")

        note = (row.get("notes", "") or "").lower()
        if not ("sblr" in note and ("session config" in note or "backend control" in note or "backend process" in note)):
            errors.append(f"{fixture_id}: note must cite SBLR session config or backend control authority")
        for forbidden in ("parser sql", "donor", "wal", "sqlite", "cluster"):
            if (forbidden in note and
                    f"without {forbidden}" not in note and
                    f"no {forbidden}" not in note and
                    "without parser sql donor wal sqlite or cluster authority" not in note):
                errors.append(f"{fixture_id}: note cites {forbidden} without explicit non-authority wording")

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
    print("sbsql_sbsfc_046_session_admin_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
