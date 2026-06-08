#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ELER-076 donor catalog seed manifest conformance gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any


FIXTURE_DIR = Path(
    "project/tests/sblr_surface/fixtures/donor_sblr_interface_gap_2026_06_03"
)
NON_DIRECT_MATRIX = "NON_DIRECT_FUNCTION_SURFACE_MATRIX.csv"
NO_EXTERNAL_REFERENCE_RE = re.compile(
    r"https?://|/home/|/Users/|[A-Za-z]:\\\\", re.IGNORECASE
)
ALLOWED_MANIFEST_STATUSES = {
    "actual_private_seed_manifest",
    "profile_derived_private_seed_manifest",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--fixture-root")
    return parser.parse_args()


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def load_csv(path: Path, errors: list[str]) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            return list(csv.DictReader(handle))
    except FileNotFoundError:
        errors.append(f"missing CSV: {path}")
    except csv.Error as exc:
        errors.append(f"invalid CSV {path}: {exc}")
    except OSError as exc:
        errors.append(f"failed reading CSV {path}: {exc}")
    return []


def load_json(path: Path, errors: list[str]) -> dict[str, Any]:
    try:
        with path.open(encoding="utf-8") as handle:
            loaded = json.load(handle)
    except FileNotFoundError:
        errors.append(f"missing JSON manifest: {path}")
        return {}
    except json.JSONDecodeError as exc:
        errors.append(f"invalid JSON manifest {path}: {exc}")
        return {}
    except OSError as exc:
        errors.append(f"failed reading JSON manifest {path}: {exc}")
        return {}
    if not isinstance(loaded, dict):
        errors.append(f"JSON manifest must be an object: {path}")
        return {}
    return loaded


def list_field_has_identifier(rows: Any, *identifier_keys: str) -> bool:
    if not isinstance(rows, list):
        return False
    for row in rows:
        if not isinstance(row, dict):
            return False
        if not any(str(row.get(key, "")).strip() for key in identifier_keys):
            return False
    return bool(rows)


def text_contains_any(value: Any, needles: tuple[str, ...]) -> bool:
    lowered = json.dumps(value, sort_keys=True).lower()
    return any(needle in lowered for needle in needles)


def validate_manifest(path: Path,
                      engine_id: str,
                      expected_recipe_prefix: str,
                      errors: list[str]) -> None:
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        errors.append(f"missing JSON manifest: {path}")
        return
    except OSError as exc:
        errors.append(f"failed reading JSON manifest {path}: {exc}")
        return
    for lineno, line in enumerate(text.splitlines(), start=1):
        if NO_EXTERNAL_REFERENCE_RE.search(line):
            errors.append(f"{path}:{lineno} contains a local absolute path or URL")
    manifest = load_json(path, errors)
    if not manifest:
        return

    context = f"{engine_id} catalog seed manifest"
    require(
        manifest.get("seed_manifest_schema_version") == 1,
        f"{context} schema version must be 1",
        errors,
    )
    require(
        manifest.get("seed_manifest_status") in ALLOWED_MANIFEST_STATUSES,
        f"{context} status is not a recognized private seed-manifest status",
        errors,
    )
    require(
        manifest.get("row_hash_algorithm") == "sha256",
        f"{context} row_hash_algorithm must be sha256",
        errors,
    )
    require(bool(str(manifest.get("seed_manifest_uuid", "")).strip()),
            f"{context} missing seed_manifest_uuid", errors)
    require(bool(str(manifest.get("manifest_hash", "")).strip()),
            f"{context} missing manifest_hash", errors)
    require(bool(str(manifest.get("search_key", "")).strip()),
            f"{context} missing search_key", errors)
    require(
        str(manifest.get("new_database_recipe_id", "")).startswith(expected_recipe_prefix),
        f"{context} new_database_recipe_id must start with {expected_recipe_prefix}",
        errors,
    )
    require(
        str(manifest.get("normalization_profile_ref", "")).startswith("embedded:"),
        f"{context} normalization profile must be embedded",
        errors,
    )
    require(
        str(manifest.get("redaction_profile_ref", "")).startswith("embedded:"),
        f"{context} redaction profile must be embedded",
        errors,
    )

    for field_name in (
        "catalog_object_set",
        "catalog_column_set",
        "default_row_set",
        "scratchbird_mapping_set",
        "rowset_hashes",
        "conformance_gate_set",
    ):
        value = manifest.get(field_name)
        require(
            isinstance(value, (list, dict)) and len(value) > 0,
            f"{context} {field_name} must be non-empty",
            errors,
        )

    require(
        list_field_has_identifier(manifest.get("visibility_rule_set"),
                                  "visibility_rule_id",
                                  "rule_id"),
        f"{context} visibility rules must have stable identifiers",
        errors,
    )
    require(
        list_field_has_identifier(manifest.get("redaction_rule_set"),
                                  "redaction_rule_id",
                                  "rule_id"),
        f"{context} redaction rules must have stable identifiers",
        errors,
    )
    require(
        text_contains_any(manifest.get("redaction_rule_set"), ("raw_secret_forbidden",)),
        f"{context} redaction rules must forbid raw secrets",
        errors,
    )
    require(
        text_contains_any(manifest.get("mutation_rule_set"), ("read_only", "report_only")),
        f"{context} mutation rules must include read-only/report-only catalog seed behavior",
        errors,
    )
    invalid_state = manifest.get("invalid_state_behavior")
    require(isinstance(invalid_state, dict),
            f"{context} invalid_state_behavior must be an object", errors)
    if isinstance(invalid_state, dict):
        for key in ("missing_manifest", "raw_secret", "visibility_undefined"):
            require(bool(str(invalid_state.get(key, "")).strip()),
                    f"{context} missing invalid-state diagnostic {key}", errors)
    require(
        text_contains_any(
            manifest.get("default_security_row_set"),
            ("redacted", "secret_ref", "redacted_not_applicable"),
        ),
        f"{context} default security seed rows must use redacted/secret-ref material",
        errors,
    )


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    fixture_root = Path(args.fixture_root).resolve() if args.fixture_root else (
        repo_root / FIXTURE_DIR
    )
    errors: list[str] = []

    rows = load_csv(fixture_root / NON_DIRECT_MATRIX, errors)
    catalog_rows = [
        row for row in rows
        if row.get("implementation_decision") == "catalog_projection_only"
    ]
    require(len(rows) == 258, "non-direct function row count must be 258", errors)
    require(len(catalog_rows) == 112, "catalog projection row count must be 112", errors)
    require(
        Counter(row.get("sb_normalized_target", "") for row in catalog_rows)
        == Counter({
            "SB.ADMIN.PLUGIN_STATUS": 72,
            "SB.EXTENSION.ITEMS": 28,
            "SB.EXTENSION.LIST": 12,
        }),
        "catalog projection normalized target counts drifted",
        errors,
    )

    manifest_paths: dict[str, Path] = {}
    for row in catalog_rows:
        context = row.get("inventory_id", "unknown")
        for field_name in (
            "engine_id",
            "item_name",
            "capability_family",
            "sb_normalized_target",
            "sb_catalog_projection",
            "catalog_exposure",
            "source_packet",
        ):
            require(bool(row.get(field_name, "").strip()),
                    f"{context} missing {field_name}", errors)

        source_packet = Path(row.get("source_packet", ""))
        require(
            source_packet.parts[:4]
            == ("docs", "contracts", "implementation_inputs", "donor-emulation"),
            f"{context} source_packet must stay under donor-emulation packets",
            errors,
        )
        require(
            source_packet.name == "function_inventory_full.csv",
            f"{context} source_packet must point to function_inventory_full.csv",
            errors,
        )
        require(
            len(source_packet.parts) >= 6 and source_packet.parts[4] == row.get("engine_id"),
            f"{context} source_packet donor does not match engine_id",
            errors,
        )
        manifest_path = repo_root / source_packet.parent / "catalog_seed_manifest_full.json"
        manifest_paths[row["engine_id"]] = manifest_path

    require(len(manifest_paths) == 19,
            "catalog projection donor engine count must be 19", errors)
    for engine_id, manifest_path in sorted(manifest_paths.items()):
        validate_manifest(
            manifest_path,
            engine_id,
            f"{engine_id}_empty_database_beta2_default_recipe_v1",
            errors,
        )

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("sblr_surface_catalog_seed_manifest_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
