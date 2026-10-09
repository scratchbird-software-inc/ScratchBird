#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Validate source identities and dispositions, not claims of runtime success."""

from __future__ import annotations

from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re

from exhaustive_generators import AUTH_ROUTE_ROOT_REL, REPLAY_INDEX_REL, SBLR_ROUND_TRIP_ROOT_REL

RELEASE_REL = Path(
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/"
    "SBSQL_SURFACE_RELEASE_DECLARATION.csv"
)
DISPOSITIONS = {"e2e_passed", "exact_refusal_passed", "cluster_provider_route_passed"}


def identity_digest(identities: set[str]) -> str:
    return hashlib.sha256(("\n".join(sorted(identities)) + "\n").encode()).hexdigest()


def index_rows(rows: list[dict[str, str]], name: str) -> dict[str, dict[str, str]]:
    result = {}
    for row in rows:
        identity = row.get("surface_id", "")
        if not identity or identity in result:
            raise ValueError(f"{name}:empty_or_duplicate_identity:{identity}")
        result[identity] = row
    return result


def validate_replay_rows(release_rows, replay_rows, contract) -> list[str]:
    release = index_rows(release_rows, "release")
    replay = index_rows(replay_rows, "replay")
    errors = []
    if set(release) != set(replay):
        errors.append("replay:identity_set_mismatch")
    if identity_digest(set(release)) != contract.get("surface_identity_sha256"):
        errors.append("release:identity_digest_mismatch")
    counts = Counter(row["final_status"] for row in release.values())
    if set(counts) - DISPOSITIONS or dict(counts) != contract.get("disposition_counts"):
        errors.append("release:disposition_counts_mismatch")
    for identity in sorted(set(release) & set(replay)):
        status = release[identity]["final_status"]
        row = replay[identity]
        evidence = row.get("expected_server_result", "").split(";")
        if evidence.count(f"release-evidence={status}") != 1 or sum(
            token.startswith("release-evidence=") for token in evidence
        ) != 1:
            errors.append(f"replay:disposition_mismatch:{identity}")
        routes = row.get("route_set", "").split(";")
        if routes.count("full_route") != int(status == "e2e_passed"):
            errors.append(f"replay:full_route_mismatch:{identity}")
    return errors


def quoted_field(text: str, name: str) -> str:
    # These generated flat fixtures use JSON-quoted scalar fields, not general YAML.
    values = re.findall(r"^" + re.escape(name) + r":\s*(\".*\")$", text, re.M)
    if len(values) != 1:
        raise ValueError(f"fixture:missing_or_duplicate_field:{name}")
    value = json.loads(values[0])
    if not isinstance(value, str):
        raise ValueError(f"fixture:non_string_field:{name}")
    return value


def validate_source_coverage(repo_root: Path, manifest: dict, scalar_ids: set[str],
                             scalar_rows: int) -> list[str]:
    def read_rows(relative):
        with (repo_root / relative).open(newline="", encoding="utf-8") as stream:
            return list(csv.DictReader(stream))

    release_rows = read_rows(RELEASE_REL)
    errors = validate_replay_rows(release_rows, read_rows(REPLAY_INDEX_REL),
                                  manifest["replay_source_contract"])
    release = index_rows(release_rows, "release")
    for root, suffix in ((AUTH_ROUTE_ROOT_REL, ".route.yaml"),
                         (SBLR_ROUND_TRIP_ROOT_REL, ".round_trip.yaml")):
        expected_files = {identity + suffix for identity in release}
        if {path.name for path in (repo_root / root).glob("*.yaml")} != expected_files:
            errors.append(f"fixtures:identity_set_mismatch:{root.name}")
        for identity, row in release.items():
            text = (repo_root / root / (identity + suffix)).read_text(encoding="utf-8")
            if (quoted_field(text, "surface_id") != identity or
                    quoted_field(text, "per_row_final_state") != row["final_status"]):
                errors.append(f"fixtures:identity_or_disposition_mismatch:{root.name}:{identity}")

    query_ids = set()
    for contract in manifest["required_query_fixture_contracts"]:
        identity = contract["surface_id"]
        if identity in query_ids or identity in scalar_ids:
            errors.append(f"query_fixture:duplicate_or_scalar_marker:{identity}")
        query_ids.add(identity)
        row = release.get(identity, {})
        if row.get("final_status") != "e2e_passed":
            errors.append(f"query_fixture:not_full_route:{identity}")
        fixture = contract["fixture_path"]
        if fixture not in row.get("fixture_refs", "").split(";") or not (repo_root / fixture).is_file():
            errors.append(f"query_fixture:missing_executable:{identity}")
        text = (repo_root / AUTH_ROUTE_ROOT_REL / (identity + ".route.yaml")).read_text(encoding="utf-8")
        if quoted_field(text, "per_row_fixture_path") != fixture:
            errors.append(f"query_fixture:wrong_executable:{identity}")
        refs = quoted_field(text, "implementation_refs").split(";")
        if not set(contract["required_implementation_refs"]).issubset(refs):
            errors.append(f"query_fixture:missing_route_contract:{identity}")
    minimums = manifest["scalar_and_query_coverage_minimums"]
    if scalar_rows + len(query_ids) < minimums["fixture_rows"]:
        errors.append("coverage:scalar_and_query_rows_below_minimum")
    if len(scalar_ids | query_ids) < minimums["fixture_surface_ids"]:
        errors.append("coverage:scalar_and_query_identities_below_minimum")
    return errors
