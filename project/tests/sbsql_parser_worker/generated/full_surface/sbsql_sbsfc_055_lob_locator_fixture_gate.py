#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-055 LOB/locator fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_055_LOB_LOCATOR_FIXTURES.csv"

TARGETS = {
    "SBSQL-15EB156297E9": ("lob_locator_to_binary", "sb.lob.locator_to_binary"),
    "SBSQL-176685E96193": ("locator_validity(locator)", "sb.locator.validity"),
    "SBSQL-1EE6C3D7F2EE": ("lob_write", "sb.lob.write"),
    "SBSQL-2E2F4913A42F": ("lob_close(locator)", "sb.lob.close"),
    "SBSQL-317A464A74B3": ("lob_size", "sb.lob.size"),
    "SBSQL-41A19A07C09B": ("lob_open(locator,mode)", "sb.lob.open"),
    "SBSQL-4B3B3D4FB26A": ("lob_append", "sb.lob.append"),
    "SBSQL-531A760C8C66": ("current_row_locator", "sb.locator.current_row"),
    "SBSQL-7B6B59743B35": ("locator_validity", "sb.locator.validity"),
    "SBSQL-7CF4F4150D85": ("lob_truncate", "sb.lob.truncate"),
    "SBSQL-891992A5F310": ("lob_open", "sb.lob.open"),
    "SBSQL-96DE0F2265B6": ("lob_locator_to_binary(locator)", "sb.lob.locator_to_binary"),
    "SBSQL-9C16F6BF7072": ("lob_truncate(locator,length)", "sb.lob.truncate"),
    "SBSQL-A9177A9A947C": ("lob_write(locator,offset,data)", "sb.lob.write"),
    "SBSQL-C2F659F4DFC0": ("lob_read", "sb.lob.read"),
    "SBSQL-C62D69D167C8": ("lob_size(locator)", "sb.lob.size"),
    "SBSQL-D2E48C4160ED": ("lob_create(class,[media])", "sb.lob.create"),
    "SBSQL-D5E167A4984B": ("lob_append(locator,data)", "sb.lob.append"),
    "SBSQL-D89EF3B31969": ("lob_create", "sb.lob.create"),
    "SBSQL-DA9087A02218": ("lob_locator_to_text(locator)", "sb.lob.locator_to_text"),
    "SBSQL-DF89DE098501": ("lob_close", "sb.lob.close"),
    "SBSQL-E7CBEEE4AAC6": ("lob_locator_to_text", "sb.lob.locator_to_text"),
    "SBSQL-F2A363288372": ("locator", "sb.locator.locator"),
    "SBSQL-FC06D12DAC16": ("lob_read(locator,offset,length)", "sb.lob.read"),
}

REQUIRED_INVALID_FIXTURES = {
    "SBSFC055-lob-read-missing",
    "SBSFC055-lob-open-malformed",
    "SBSFC055-lob-write-bad-offset",
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
    registered_ids = set(re.findall(r'"(sb\.(?:lob|locator)\.[^"]+)"', seed_text))

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

        case_kind = row.get("case_kind", "")
        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()
        surface_id = row.get("surface_id", "")
        function_id = row.get("function_id", "")
        canonical = row.get("canonical_builtin_id", "")

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

        target = TARGETS.get(surface_id)
        if target is None:
            errors.append(f"{fixture_id}: unexpected surface_id {surface_id}")
            continue
        expected_name, expected_function_id = target
        if function_id != expected_function_id or canonical != expected_function_id:
            errors.append(f"{fixture_id}: function/canonical id mismatch for {surface_id}")
        if function_id not in registered_ids:
            errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")
        if expected_diag:
            errors.append(f"{fixture_id}: positive fixture must not declare diagnostic")
        if row.get("expected_result_descriptor", "") not in {"json_document", "character", "int64", "boolean", "binary"}:
            errors.append(f"{fixture_id}: unexpected positive descriptor {row.get('expected_result_descriptor', '')}")

        surface = surfaces.get(surface_id)
        if surface is None:
            errors.append(f"{fixture_id}: surface_id missing from registry")
        else:
            if surface.get("status") != "native_now":
                errors.append(f"{fixture_id}: surface_id is not native_now")
            if surface.get("canonical_name") != expected_name:
                errors.append(
                    f"{fixture_id}: canonical_name mismatch: "
                    f"{surface.get('canonical_name')} != {expected_name}"
                )
            if surface.get("sblr_operation_family") != "sblr.expression.runtime.v3":
                errors.append(f"{fixture_id}: unexpected SBLR family {surface.get('sblr_operation_family')}")
        positive_by_surface[surface_id].add(fixture_id)

    for surface_id, fixtures_for_surface in positive_by_surface.items():
        if not fixtures_for_surface:
            errors.append(f"{surface_id}: missing positive fixture")
    for fixture_id in sorted(REQUIRED_INVALID_FIXTURES - invalid_seen):
        errors.append(f"{fixture_id}: missing required invalid-input fixture")

    print(
        "sbsql_sbsfc_055_lob_locator_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_055_lob_locator_fixture_gate=failed", file=sys.stderr)
        for error in errors[:100]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_055_lob_locator_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
