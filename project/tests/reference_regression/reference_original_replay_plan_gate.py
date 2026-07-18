#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the original-tool reference replay plan.

The plan is public test metadata only: it names parser lanes, acquisition source
ids, local-only payload locations, route serial groups, and CTest modes. It
does not contain acquired upstream regression payloads or private workplan data.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import re
import sys
from collections import defaultdict


REFERENCE_ROOT = pathlib.Path("project/tests/reference_regression")
PROFILE_MANIFEST = pathlib.Path("project/src/parsers/compatibility/CompatibilityProfileManifest.csv")
ACQUISITION_SOURCES = REFERENCE_ROOT / "reference_regression_acquisition_sources.csv"
REPLAY_PLAN = REFERENCE_ROOT / "reference_original_replay_plan.csv"

REQUIRED_PROFILE_CLASSES = {"compatibility_emulation", "release_profile_variant"}
REQUIRED_CTEST_MODES = {
    "public_no_payload",
    "local_optional_replay",
    "release_mandatory_replay",
    "single_family_replay",
}
VALID_MODES = {"public-no-payload", "local-optional", "release-mandatory", "single-family"}
FORBIDDEN_TEXT = ("future", "defer", "deferred", "stub", "todo", "tbd", "fixme")
ROUTE_RE = re.compile(r"^localhost:(\d+)$")
NON_NETWORK_ROUTES = {
    "sqlite-shell-route",
    "duckdb-shell-route",
    "suite-metadata-route",
}

REQUIRED_COLUMNS = {
    "family_id",
    "display_name",
    "profile_class",
    "release_profile",
    "source_ids",
    "harness_root",
    "parser_module",
    "default_route",
    "serial_group",
    "runner_tools",
    "local_acquisition_policy",
    "ctest_modes",
    "status",
    "notes",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    require(path.is_file(), f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        require(reader.fieldnames is not None, f"{path}: missing header")
        missing = sorted(REQUIRED_COLUMNS - set(reader.fieldnames)) if path.name == REPLAY_PLAN.name else []
        require(not missing, f"{path}: missing columns {missing}")
        rows = [dict(row) for row in reader]
    require(rows, f"{path}: no rows")
    return rows


def split_semicolon(value: str) -> list[str]:
    return [item.strip() for item in value.split(";") if item.strip()]


def load_required_profiles(repo_root: pathlib.Path) -> dict[str, dict[str, str]]:
    rows = read_csv(repo_root / PROFILE_MANIFEST)
    required = {
        row["family_id"]: row
        for row in rows
        if row["profile_class"] in REQUIRED_PROFILE_CLASSES
    }
    require(required, "compatibility profile manifest has no replay-required rows")
    return required


def load_acquisition_sources(repo_root: pathlib.Path) -> dict[str, dict[str, str]]:
    rows = read_csv(repo_root / ACQUISITION_SOURCES)
    by_source: dict[str, dict[str, str]] = {}
    for row in rows:
        source_id = row["source_id"]
        require(source_id not in by_source, f"duplicate acquisition source_id: {source_id}")
        by_source[source_id] = row
    return by_source


def validate_route(row: dict[str, str]) -> list[str]:
    routes = split_semicolon(row["default_route"])
    require(routes, f"{row['family_id']}: default_route is empty")
    normalized: list[str] = []
    for route in routes:
        match = ROUTE_RE.match(route)
        if match:
            port = int(match.group(1))
            require(1 <= port <= 65535, f"{row['family_id']}: invalid port {port}")
            normalized.append(route)
            continue
        require(route in NON_NETWORK_ROUTES,
                f"{row['family_id']}: unsupported default_route {route!r}")
        normalized.append(route)
    return normalized


def local_payload_status(repo_root: pathlib.Path,
                         acquisition_rows: list[dict[str, str]]) -> tuple[list[str], list[str]]:
    present: list[str] = []
    missing: list[str] = []
    for source in acquisition_rows:
        root = (
            repo_root
            / REFERENCE_ROOT
            / "reference_release_acquisition"
            / source["acquisition_subdir"]
            / "regression"
            / "acquired"
            / source["source_id"]
        )
        if root.exists():
            present.append(source["source_id"])
        else:
            missing.append(source["source_id"])
    return present, missing


def validate_plan(repo_root: pathlib.Path,
                  mode: str,
                  selected_family: str | None) -> dict[str, object]:
    require(mode in VALID_MODES, f"unsupported mode: {mode}")
    required_profiles = load_required_profiles(repo_root)
    acquisition_by_source = load_acquisition_sources(repo_root)
    rows = read_csv(repo_root / REPLAY_PLAN)
    by_family: dict[str, dict[str, str]] = {}
    route_groups: dict[str, set[str]] = defaultdict(set)
    missing_payloads: dict[str, list[str]] = {}
    present_payloads: dict[str, list[str]] = {}

    for row_number, row in enumerate(rows, start=2):
        context = f"{REPLAY_PLAN}:{row_number}"
        family_id = row["family_id"]
        require(family_id not in by_family, f"{context}: duplicate family_id {family_id}")
        by_family[family_id] = row

        lowered = " ".join(row.values()).lower()
        for token in FORBIDDEN_TEXT:
            require(token not in lowered, f"{context}: forbidden planning token {token!r}")

        require(row["profile_class"] in REQUIRED_PROFILE_CLASSES,
                f"{context}: profile_class is not replay-required")
        require(row["status"] == "ready_for_local_replay",
                f"{context}: status must be ready_for_local_replay")
        require(row["local_acquisition_policy"] == "local_payload_untracked",
                f"{context}: local acquisition policy drift")
        require(set(split_semicolon(row["ctest_modes"])) == REQUIRED_CTEST_MODES,
                f"{context}: CTest modes do not match required modes")
        require(row["serial_group"], f"{context}: serial_group is empty")
        require(row["runner_tools"], f"{context}: runner_tools is empty")

        profile = required_profiles.get(family_id)
        require(profile is not None, f"{context}: family_id missing from compatibility manifest")
        require(profile["release_profile"] == row["release_profile"],
                f"{context}: release_profile does not match compatibility manifest")
        require(profile["profile_class"] == row["profile_class"],
                f"{context}: profile_class does not match compatibility manifest")

        harness_root = repo_root / row["harness_root"]
        parser_module = repo_root / row["parser_module"]
        require(harness_root.is_dir(), f"{context}: missing harness root {harness_root}")
        require(parser_module.is_dir(), f"{context}: missing parser module {parser_module}")
        harness_manifest = harness_root / "native_tool_harness" / "native_tool_harness_manifest.csv"
        require(harness_manifest.is_file(), f"{context}: missing native harness manifest")

        acquisition_rows: list[dict[str, str]] = []
        for source_id in split_semicolon(row["source_ids"]):
            source = acquisition_by_source.get(source_id)
            require(source is not None, f"{context}: no acquisition source_id {source_id}")
            acquisition_rows.append(source)

        for route in validate_route(row):
            route_groups[route].add(row["serial_group"])

        present, missing = local_payload_status(repo_root, acquisition_rows)
        present_payloads[family_id] = present
        missing_payloads[family_id] = missing

    expected = set(required_profiles)
    actual = set(by_family)
    require(actual == expected,
            f"replay plan family set drift: missing={sorted(expected - actual)} extra={sorted(actual - expected)}")

    for route, groups in route_groups.items():
        if route in NON_NETWORK_ROUTES:
            continue
        if sum(1 for row in rows if route in split_semicolon(row["default_route"])) > 1:
            require(len(groups) == 1, f"{route}: multiple serial groups declared for same port: {sorted(groups)}")

    if selected_family:
        require(selected_family in by_family, f"selected family is not in replay plan: {selected_family}")
        families_to_check = [selected_family]
    else:
        families_to_check = sorted(by_family)

    if mode == "release-mandatory":
        missing = {
            family: missing_payloads[family]
            for family in families_to_check
            if missing_payloads[family]
        }
        require(not missing, f"release replay payloads missing: {missing}")
    elif mode == "single-family":
        require(selected_family is not None, "single-family mode requires --family")
        require(not missing_payloads[selected_family],
                f"{selected_family}: local replay payloads missing: {missing_payloads[selected_family]}")

    digest = hashlib.sha256(
        json.dumps(rows, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return {
        "gate": "reference_original_replay_plan_gate",
        "mode": mode,
        "family_count": len(by_family),
        "plan_digest": digest,
        "families": sorted(by_family),
        "present_payloads": present_payloads,
        "missing_payloads": missing_payloads,
        "authority_policy": "reference_tools_feed_parser_routes_only_engine_mga_security_storage_authority",
    }


def write_evidence(path: pathlib.Path, payload: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument(
        "--mode",
        choices=sorted(VALID_MODES),
        default="public-no-payload",
    )
    parser.add_argument("--family")
    parser.add_argument("--evidence-file", type=pathlib.Path)
    args = parser.parse_args(argv)

    payload = validate_plan(args.repo_root.resolve(), args.mode, args.family)
    if args.evidence_file:
        write_evidence(args.evidence_file, payload)
    present = sum(1 for values in payload["present_payloads"].values() if values)
    missing = sum(1 for values in payload["missing_payloads"].values() if values)
    print(
        "reference_original_replay_plan_gate=passed "
        f"mode={args.mode} "
        f"families={payload['family_count']} "
        f"payload_present_families={present} "
        f"payload_missing_families={missing}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
