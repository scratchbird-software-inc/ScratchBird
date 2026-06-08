#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-037 XML/multimodel scalar helper fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_037_XML_MULTIMODEL_SCALAR_HELPER_FIXTURES.csv"

TARGETS = {
    "SBSQL-0C16676374C8": "sb.xml.forest",
    "SBSQL-6C89436D2254": "sb.xml.forest",
    "SBSQL-2BBA1DA50B23": "sb.xml.cast",
    "SBSQL-0C8A8486F751": "sb.xml.cast",
    "SBSQL-104DD993AED4": "sb.xml.exists",
    "SBSQL-EEA4907830CB": "sb.xml.exists",
    "SBSQL-1FD7CBD0921F": "sb.xml.attributes",
    "SBSQL-E2022718464C": "sb.xml.attributes",
    "SBSQL-934D2E7C0508": "sb.xml.concat",
    "SBSQL-2B38A69D425B": "sb.xml.concat",
    "SBSQL-4F494D9A6610": "sb.xml.comment",
    "SBSQL-7881C81BBBE8": "sb.xml.comment",
    "SBSQL-DC75730A32EA": "sb.xml.pi",
    "SBSQL-51E09D00A979": "sb.xml.pi",
    "SBSQL-52CC2FA7719D": "sb.xml.root",
    "SBSQL-A31D3F4A9E77": "sb.xml.root",
    "SBSQL-54EBF8EDE58A": "sb.xml.element",
    "SBSQL-5702FA6BF536": "sb.xml.agg",
    "SBSQL-94785A48EF57": "sb.xml.agg",
    "SBSQL-F0C5F1661298": "sb.xml.table",
    "SBSQL-796CAD6CD56E": "sb.xml.table",
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
    registered_ids = set(re.findall(r'"(sb\.xml\.[^"]+)"', seed_text))

    errors: list[str] = []
    positive_by_surface: dict[str, set[str]] = {surface_id: set() for surface_id in TARGETS}
    invalid_seen = False
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

        if surface_id not in TARGETS:
            errors.append(f"{fixture_id}: unexpected surface_id {surface_id}")
            continue
        if function_id != TARGETS[surface_id] or canonical != TARGETS[surface_id]:
            errors.append(f"{fixture_id}: function/canonical id mismatch for {surface_id}")
        if function_id not in registered_ids:
            errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")

        surface = surfaces.get(surface_id)
        if surface is None:
            errors.append(f"{fixture_id}: surface_id missing from registry")
        elif surface.get("status") != "native_now":
            errors.append(f"{fixture_id}: surface_id is not native_now")

        try:
            parsed_args = json.loads(row.get("arguments_json", ""))
            if not isinstance(parsed_args, list):
                errors.append(f"{fixture_id}: arguments_json must be a JSON list")
        except json.JSONDecodeError as exc:
            errors.append(f"{fixture_id}: invalid arguments_json: {exc}")

        if case_kind == "positive":
            positive_by_surface[surface_id].add(fixture_id)
            if expected_diag:
                errors.append(f"{fixture_id}: positive fixture must not declare diagnostic")
        elif case_kind == "invalid_input":
            invalid_seen = expected_diag == "SB_DIAG_FUNCTION_INVALID_INPUT" or invalid_seen
            if expected_diag != "SB_DIAG_FUNCTION_INVALID_INPUT":
                errors.append(f"{fixture_id}: invalid_input must declare SB_DIAG_FUNCTION_INVALID_INPUT")
        else:
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")

    for surface_id, fixture_ids_for_surface in positive_by_surface.items():
        if not fixture_ids_for_surface:
            errors.append(f"{surface_id}: missing positive fixture")
    if not invalid_seen:
        errors.append("missing invalid-input diagnostic fixture")

    print(
        "sbsql_sbsfc_037_xml_multimodel_scalar_helper_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_037_xml_multimodel_scalar_helper_fixture_gate=failed", file=sys.stderr)
        for error in errors[:60]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_037_xml_multimodel_scalar_helper_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
