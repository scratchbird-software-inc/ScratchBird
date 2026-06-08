#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate authored SBsql SBLR binary round-trip fixtures against the matrix."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from collections import Counter
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
MATRIX_NAME = "SBLR_BINARY_ROUND_TRIP_MATRIX.csv"
FIXTURE_DIR = "project/tests/sbsql_parser_worker/generated/full_surface/sblr_binary_round_trip"
FIXTURE_KIND = "sblr_binary_round_trip"
ALLOWED_STATUSES = {"pending_authoring", "fixture_authored", "e2e_passed"}
REQUIRED_KEYS = [
    "fixture_kind",
    "fixture_status",
    "surface_id",
    "canonical_name",
    "surface_kind",
    "status",
    "cluster_scope",
    "sblr_operation_family",
    "oracle_authority_status",
    "expected_canonical_function_or_api_operation_id",
    "parse_phase_expectation",
    "bind_phase_expectation",
    "lower_phase_expectation",
    "binary_serialize_phase_expectation",
    "verify_phase_expectation",
    "binary_deserialize_phase_expectation",
    "dispatch_phase_expectation",
    "execute_phase_expectation",
    "render_phase_expectation",
    "canonical_container_magic",
    "canonical_container_header_size_bytes",
    "byte_identical_round_trip_required",
    "crc32c_check_required",
    "engine_anchored_uuids_required",
    "forbidden_authority_sources",
    "execution_authority_model",
    "per_row_final_state",
    "per_row_ctest_label",
    "per_row_fixture_path",
    "implementation_refs",
    "diagnostic_proof",
    "result_proof",
]


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def parse_fixture(path: Path) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.lstrip().startswith("#") or line.startswith((" ", "-")):
            continue
        if ":" not in line:
            continue
        key, raw_value = line.split(":", 1)
        value = raw_value.strip()
        if value.startswith('"') and value.endswith('"'):
            value = json.loads(value)
        fields[key.strip()] = value
    return fields


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    rows = read_csv(artifact_root / MATRIX_NAME)
    matrix_by_path = {row["fixture_path"]: row for row in rows}
    errors: list[str] = []
    counts: Counter[str] = Counter()

    for row in rows:
        status = row.get("fixture_status", "")
        surface_id = row.get("surface_id", "")
        fixture_path = root / row["fixture_path"]
        counts[status] += 1
        if status not in ALLOWED_STATUSES:
            errors.append(f"{surface_id} invalid fixture_status={status}")
            continue
        if status == "pending_authoring":
            if fixture_path.exists():
                errors.append(f"{surface_id} fixture file exists but matrix still says pending_authoring: {row['fixture_path']}")
            continue
        if not fixture_path.is_file():
            errors.append(f"{surface_id} matrix says {status} but fixture is missing: {row['fixture_path']}")
            continue
        fields = parse_fixture(fixture_path)
        for key in REQUIRED_KEYS:
            if not fields.get(key, ""):
                errors.append(f"{surface_id} fixture missing {key}")
        expected_pairs = {
            "fixture_kind": FIXTURE_KIND,
            "fixture_status": status,
            "surface_id": surface_id,
            "canonical_name": row["canonical_name"],
            "surface_kind": row["surface_kind"],
            "status": row["status"],
            "cluster_scope": row["cluster_scope"],
            "sblr_operation_family": row["sblr_operation_family"],
            "oracle_authority_status": row["oracle_authority_status"],
            "expected_canonical_function_or_api_operation_id": row["expected_canonical_function_or_api_operation_id"],
            "canonical_container_magic": row["canonical_container_magic"],
            "canonical_container_header_size_bytes": row["canonical_container_header_size_bytes"],
            "byte_identical_round_trip_required": row["byte_identical_round_trip_required"],
            "crc32c_check_required": row["crc32c_check_required"],
            "engine_anchored_uuids_required": row["engine_anchored_uuids_required"],
        }
        for key, expected in expected_pairs.items():
            if fields.get(key, "") != expected:
                errors.append(f"{surface_id} fixture {key} drift: expected={expected} observed={fields.get(key, '')}")
        forbidden = fields.get("forbidden_authority_sources", "")
        if "sql_text" not in forbidden or "operation_family_only_routing" not in forbidden:
            errors.append(f"{surface_id} fixture lost forbidden authority source coverage")
        authority = fields.get("execution_authority_model", "")
        if "no_wal_authority" not in authority or "sblr_envelope_with_uuid_and_descriptor_authority_only" not in authority:
            errors.append(f"{surface_id} fixture lost SBLR/MGA authority model")
        if "sbsql_parser_worker" not in fields.get("per_row_ctest_label", ""):
            errors.append(f"{surface_id} fixture missing parser-worker CTest evidence")

    fixture_root = root / FIXTURE_DIR
    if fixture_root.exists():
        for path in fixture_root.glob("*.round_trip.yaml"):
            rel = path.relative_to(root).as_posix()
            if rel not in matrix_by_path:
                errors.append(f"orphan SBLR round-trip fixture: {rel}")

    authored = counts["fixture_authored"] + counts["e2e_passed"]
    if authored == 0:
        errors.append("no SBLR binary round-trip fixtures have been authored")

    if errors:
        for error in errors[:50]:
            print(error, file=sys.stderr)
        if len(errors) > 50:
            print(f"... {len(errors) - 50} additional errors", file=sys.stderr)
        print("sbsql_sblr_binary_round_trip_fixture_gate=failed", file=sys.stderr)
        return 1

    print(
        "sbsql_sblr_binary_round_trip_fixture_gate=passed "
        f"rows={len(rows)} pending_authoring={counts['pending_authoring']} "
        f"fixture_authored={counts['fixture_authored']} e2e_passed={counts['e2e_passed']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
