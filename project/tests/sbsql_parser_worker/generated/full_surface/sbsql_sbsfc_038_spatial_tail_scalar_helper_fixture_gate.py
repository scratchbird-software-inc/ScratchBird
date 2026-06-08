#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-038 spatial tail scalar helper fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_038_SPATIAL_TAIL_SCALAR_HELPER_FIXTURES.csv"

TARGETS = {
    "SBSQL-9689873CEFCA": "sb.scalar.st_setsrid_geometry_srid",
    "SBSQL-A01836D957A0": "sb.scalar.st_dwithin",
    "SBSQL-A0BCD0E4C3DC": "sb.scalar.st_m",
    "SBSQL-A57555BEE95E": "sb.scalar.st_overlaps",
    "SBSQL-A5BDCC976DD0": "sb.scalar.st_difference",
    "SBSQL-A5D10A16CCFA": "sb.scalar.st_z",
    "SBSQL-A8D99D74565F": "sb.scalar.st_area_geometry",
    "SBSQL-AD4F92702329": "sb.scalar.st_asmvtgeom",
    "SBSQL-AEFECB9626BB": "sb.scalar.st_difference",
    "SBSQL-B1718AA4E4B6": "sb.scalar.st_length",
    "SBSQL-B26EC3DF7AFB": "sb.scalar.geom_union_geometry",
    "SBSQL-B288AFD4ECE5": "sb.scalar.geom_collect",
    "SBSQL-B5825D1638CA": "sb.scalar.st_makepoint",
    "SBSQL-BA4115A6DBA5": "sb.scalar.st_equals_g1_g2",
    "SBSQL-BD9DD4BBECA7": "sb.scalar.st_intersection",
    "SBSQL-C03FDC7E09D0": "sb.scalar.st_centroid",
    "SBSQL-C44E7F61A475": "sb.scalar.st_geometrytype_geometry",
    "SBSQL-C557FC25C1DF": "sb.scalar.st_geomfromgeojson_text",
    "SBSQL-C5B5E28021D3": "sb.scalar.st_makeline",
    "SBSQL-C6D14CCCA2D1": "sb.scalar.st_geomfromtext",
    "SBSQL-CBD9B6358B34": "sb.scalar.geom_extent_geometry",
    "SBSQL-CBE14326BD0B": "sb.scalar.st_symdifference",
    "SBSQL-CF31B52FAA1F": "sb.scalar.st_asgeojson_geometry_maxdecimaldigits",
    "SBSQL-CFE56EE1BAC3": "sb.scalar.st_dwithin",
    "SBSQL-D3C5EA9765BE": "sb.scalar.st_touches_g1_g2",
    "SBSQL-D5BEA7309046": "sb.scalar.st_transform_geometry_target_srid",
    "SBSQL-DB22C5B8D6E6": "sb.scalar.st_covers",
    "SBSQL-E211ACCD957F": "sb.scalar.st_srid_geometry",
    "SBSQL-E43632706687": "sb.scalar.st_disjoint",
    "SBSQL-E4EB3BEDAA0A": "sb.scalar.st_convexhull",
    "SBSQL-E73C186D5991": "sb.scalar.st_length",
    "SBSQL-E8E12B064114": "sb.scalar.st_convexhull",
    "SBSQL-F053EEAC95CD": "sb.scalar.st_npoints",
    "SBSQL-F1B58755A174": "sb.scalar.st_makeline",
    "SBSQL-F21F901FC2AF": "sb.scalar.st_makepolygon_linestring_holesarray",
    "SBSQL-F3C89846D91C": "sb.scalar.st_geomfromtext",
    "SBSQL-F4AE1FA62237": "sb.scalar.st_within_g1_g2",
    "SBSQL-F763191B3241": "sb.scalar.st_symdifference",
    "SBSQL-F7D5231CA0E4": "sb.scalar.st_covers",
    "SBSQL-F8050BCAF06D": "sb.scalar.st_union",
    "SBSQL-F985930BDD2F": "sb.scalar.st_geogfromtext",
    "SBSQL-FB46F964CAA5": "sb.scalar.st_union",
    "SBSQL-FF57FEDF9747": "sb.scalar.st_asmvtgeom",
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
    registered_ids = set(re.findall(r'"(sb\.scalar\.(?:st|geom)_[^"]+)"', seed_text))

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
        "sbsql_sbsfc_038_spatial_tail_scalar_helper_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_038_spatial_tail_scalar_helper_fixture_gate=failed", file=sys.stderr)
        for error in errors[:60]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_038_spatial_tail_scalar_helper_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
