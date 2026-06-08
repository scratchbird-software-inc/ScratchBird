#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-036 spatial geometry scalar helper fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_036_SPATIAL_GEOMETRY_SCALAR_HELPER_FIXTURES.csv"

TARGETS = {
    "SBSQL-007BD17BDF55": "sb.scalar.st_x",
    "SBSQL-01C6BF2303B1": "sb.scalar.st_makepoint",
    "SBSQL-064CC33574E2": "sb.scalar.st_crosses_g1_g2",
    "SBSQL-14816C2A7E33": "sb.scalar.st_simplify",
    "SBSQL-14CD6B2AA8E3": "sb.scalar.st_geometrytype_geometry",
    "SBSQL-16705FD6AD8C": "sb.scalar.st_geogfromtext",
    "SBSQL-1817177FD841": "sb.scalar.st_contains_g1_g2",
    "SBSQL-19B4EE69BD6A": "sb.scalar.geom_extent_geometry",
    "SBSQL-19C49EFCE56D": "sb.scalar.st_y",
    "SBSQL-20DB96AF4D98": "sb.scalar.st_crosses",
    "SBSQL-2ED82920C391": "sb.scalar.st_disjoint",
    "SBSQL-34B32A5FF887": "sb.scalar.st_transform_geometry_target_srid",
    "SBSQL-37FDE5D4CA38": "sb.scalar.st_numpoints_geometry",
    "SBSQL-39EC9401DD7F": "sb.scalar.st_asbinary",
    "SBSQL-3B96D65453D5": "sb.scalar.st_simplify_geometry_tolerance",
    "SBSQL-3E576350E9B0": "sb.scalar.st_assvg_geometry",
    "SBSQL-3F84A2CEBD71": "sb.scalar.geom_union_geometry",
    "SBSQL-4016AFBC31B8": "sb.scalar.st_perimeter_geometry",
    "SBSQL-48C47B22CD64": "sb.scalar.st_envelope",
    "SBSQL-4976BE206EC9": "sb.scalar.st_distance",
    "SBSQL-53EF6CC1B84B": "sb.scalar.st_x_point",
    "SBSQL-549915041FC5": "sb.scalar.st_distance_g1_g2",
    "SBSQL-56C21F337176": "sb.scalar.st_envelope_geometry",
    "SBSQL-577953487165": "sb.scalar.st_astext",
    "SBSQL-581DB27EE2F3": "sb.scalar.st_astext_geometry",
    "SBSQL-5C008F9218F5": "sb.scalar.st_asbinary_geometry",
    "SBSQL-610EB642822F": "sb.scalar.st_touches_g1_g2",
    "SBSQL-63555A174F42": "sb.scalar.st_setsrid_geometry_srid",
    "SBSQL-65C7DA8D048B": "sb.scalar.st_perimeter",
    "SBSQL-6B3AC153575D": "sb.scalar.st_assvg",
    "SBSQL-6CC46392ADAC": "sb.scalar.st_buffer_geometry_distance",
    "SBSQL-6E042C3D9DA7": "sb.scalar.st_npoints",
    "SBSQL-6EE712BB2CA2": "sb.scalar.st_numpoints",
    "SBSQL-71E8B706D3F5": "sb.scalar.st_overlaps",
    "SBSQL-7387E0B53393": "sb.scalar.st_intersects",
    "SBSQL-774E359ADAF4": "sb.scalar.st_geomfromwkb_wkb_srid",
    "SBSQL-779284739DB2": "sb.scalar.st_equals_g1_g2",
    "SBSQL-78846923611D": "sb.scalar.st_geomfromwkb",
    "SBSQL-7B1A7ED9A65B": "sb.scalar.st_makepolygon_linestring_holesarray",
    "SBSQL-7C81986B79D9": "sb.scalar.st_srid_geometry",
    "SBSQL-7E7F908D3782": "sb.scalar.st_geomfromgeojson_text",
    "SBSQL-7F1AA7BC1C1B": "sb.scalar.st_centroid",
    "SBSQL-81134D15580F": "sb.scalar.st_contains",
    "SBSQL-8126547CB199": "sb.scalar.st_intersection",
    "SBSQL-82098A2E3A54": "sb.scalar.st_area_geometry",
    "SBSQL-83D324A5BD04": "sb.scalar.st_within_g1_g2",
    "SBSQL-8A2191CBD1FF": "sb.scalar.st_buffer",
    "SBSQL-918FBB8B8F9A": "sb.scalar.geom_collect",
    "SBSQL-9589706D80E8": "sb.scalar.st_asgeojson_geometry_maxdecimaldigits",
    "SBSQL-9639CB5F0B9A": "sb.scalar.st_intersects_g1_g2",
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
        "sbsql_sbsfc_036_spatial_geometry_scalar_helper_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_036_spatial_geometry_scalar_helper_fixture_gate=failed", file=sys.stderr)
        for error in errors[:60]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_036_spatial_geometry_scalar_helper_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
