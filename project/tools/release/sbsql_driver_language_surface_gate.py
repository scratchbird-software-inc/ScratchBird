#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the SBsql driver/tool language-resource surface declaration.

This gate is intentionally read-only. It does not claim runtime support; it
prevents drivers, tools, and adaptors from silently diverging on resource
identity, parse fallback order, renderer lossiness, local draft SBLR handling,
or server revalidation authority while runtime integration is still in flight.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import sys
from typing import Any


DRIVER_MANIFEST = "project/drivers/DriverPackageManifest.csv"
SURFACE_MANIFEST = "project/drivers/language/sbsql_language_surface_manifest.json"
PROTOCOL_SCHEMA = "project/drivers/language/sbsql_editor_tool_protocol.schema.json"

EXPECTED_SYNTAX_PROFILE_ORDER = [
    "explicit_syntax_profile",
    "preferred_language_and_dialect",
    "canonical_english_fallback_when_preferred_fails",
    "fail_closed",
]

REQUIRED_COMMON_CONTRACT = {
    "server_revalidates_client_sblr",
    "server_owns_uuid_descriptor_security_authority",
    "server_owns_mga_transaction_finality",
    "standard_english_fallback_preserves_preferred_language",
    "renderer_output_is_canonical_not_source_reconstruction",
    "draft_sblr_is_untrusted_until_server_admission",
    "predictive_text_must_not_infer_hidden_objects",
}

REQUIRED_COMPONENT_FIELDS = {
    "component_id",
    "component_category",
    "resource_identity",
    "syntax_profile_order",
    "local_parse",
    "draft_sblr",
    "completion",
    "diagnostics",
    "canonical_preview",
    "renderer",
    "renderer_lossiness",
    "predictive",
    "standard_english_fallback",
    "offline_cache",
    "redaction_metadata",
    "capability_negotiation",
    "fail_closed_on_mismatch",
    "server_revalidation_authority",
    "authority_boundary",
    "implementation_state",
}

DIRECT_VALUES = {
    "local_parse": "common_resource_pack_required",
    "draft_sblr": "local_draft_allowed_server_revalidated",
    "completion": "common_protocol_required",
    "diagnostics": "canonical_message_vector_keys_required",
    "canonical_preview": "required",
    "renderer": "preferred_language_then_canonical_english",
    "renderer_lossiness": "classified_required",
    "predictive": "resource_bounded_no_hidden_objects",
    "standard_english_fallback": "enabled_only_when_preferred_profile_fails",
    "offline_cache": "signed_hash_epoch_scoped",
    "redaction_metadata": "required_no_query_text_or_hidden_identifiers",
    "capability_negotiation": "exact_resource_identity_required",
}

DELEGATE_VALUES = {
    "local_parse": "delegates_to_common_resource_pack_consumer",
    "draft_sblr": "delegates_to_driver_local_draft_server_revalidated",
    "completion": "delegates_to_common_protocol_consumer",
    "diagnostics": "delegates_to_canonical_message_vector_consumer",
    "canonical_preview": "delegates_to_common_protocol_consumer",
    "renderer": "delegates_to_common_renderer_consumer",
    "renderer_lossiness": "delegates_to_common_renderer_consumer",
    "predictive": "delegates_to_common_protocol_consumer",
    "standard_english_fallback": "delegates_to_common_parser_consumer",
    "offline_cache": "delegates_to_common_resource_cache",
    "redaction_metadata": "delegates_to_common_protocol_consumer",
    "capability_negotiation": "delegates_to_common_negotiation_consumer",
}


def read_json(path: Path) -> Any:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def read_driver_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def allowed_values(schema: dict[str, Any], field: str) -> set[str]:
    values = schema.get("allowed_component_values", {}).get(field, [])
    return {value for value in values if isinstance(value, str)}


def validate_component(
    component: dict[str, Any],
    row: dict[str, str],
    schema: dict[str, Any],
    resource_identity: str,
) -> list[str]:
    errors: list[str] = []
    component_id = str(component.get("component_id", "<missing>"))
    missing = sorted(REQUIRED_COMPONENT_FIELDS - set(component))
    if missing:
        errors.append(f"{component_id}: missing component fields: {missing}")
        return errors

    if component.get("component_category") != row.get("category"):
        errors.append(f"{component_id}: category does not match DriverPackageManifest.csv")
    if component.get("component_family") != row.get("driver_family"):
        errors.append(f"{component_id}: component_family does not match driver_family")
    if component.get("resource_identity") != resource_identity:
        errors.append(f"{component_id}: resource_identity does not match manifest root")
    if component.get("syntax_profile_order") != EXPECTED_SYNTAX_PROFILE_ORDER:
        errors.append(f"{component_id}: syntax_profile_order must be deterministic and include canonical English fallback then fail_closed")
    if component.get("fail_closed_on_mismatch") is not True:
        errors.append(f"{component_id}: fail_closed_on_mismatch must be true")
    if component.get("server_revalidation_authority") != "required":
        errors.append(f"{component_id}: server_revalidation_authority must be required")
    if component.get("authority_boundary") != "client_resources_are_untrusted_until_server_revalidation":
        errors.append(f"{component_id}: authority_boundary must preserve server authority")
    if component.get("implementation_state") not in allowed_values(schema, "implementation_state"):
        errors.append(f"{component_id}: invalid implementation_state")

    category = row.get("category")
    expected_values = DIRECT_VALUES if category in {"driver", "tool"} else DELEGATE_VALUES
    for field, expected in expected_values.items():
        if component.get(field) != expected:
            errors.append(f"{component_id}: {field} must be {expected!r}")
        if component.get(field) not in allowed_values(schema, field):
            errors.append(f"{component_id}: {field} is not admitted by protocol schema")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    schema_path = repo_root / PROTOCOL_SCHEMA
    surface_path = repo_root / SURFACE_MANIFEST
    driver_path = repo_root / DRIVER_MANIFEST
    errors: list[str] = []

    for rel_path, path in (
        (PROTOCOL_SCHEMA, schema_path),
        (SURFACE_MANIFEST, surface_path),
        (DRIVER_MANIFEST, driver_path),
    ):
        if not path.is_file():
            errors.append(f"missing required file: {rel_path}")
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 2

    try:
        schema = read_json(schema_path)
        surface = read_json(surface_path)
        driver_rows = read_driver_rows(driver_path)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"sbsql_driver_language_surface_gate=failed: {exc}", file=sys.stderr)
        return 2

    if schema.get("schema_version") != "sbsql.editor_tool_protocol.schema.v1":
        errors.append("protocol schema version mismatch")
    if schema.get("protocol_version") != "sbsql.editor_tool.v1":
        errors.append("protocol version mismatch")
    if surface.get("schema_version") != "sbsql.driver_language_surface_manifest.v1":
        errors.append("surface manifest schema version mismatch")
    if surface.get("protocol_schema") != PROTOCOL_SCHEMA:
        errors.append("surface manifest protocol_schema path mismatch")
    if surface.get("driver_package_manifest") != DRIVER_MANIFEST:
        errors.append("surface manifest driver_package_manifest path mismatch")

    common_contract = surface.get("common_resource_contract", {})
    for key in sorted(REQUIRED_COMMON_CONTRACT):
        if common_contract.get(key) is not True:
            errors.append(f"common_resource_contract.{key} must be true")

    resource_identity = surface.get("resource_identity")
    if resource_identity != "sbsql.common_resource_pack.v1":
        errors.append("resource_identity must be sbsql.common_resource_pack.v1")

    driver_ids = {row.get("component_id", "") for row in driver_rows}
    components = surface.get("components")
    if not isinstance(components, list):
        errors.append("components must be a list")
        components = []
    component_by_id: dict[str, dict[str, Any]] = {}
    for component in components:
        if not isinstance(component, dict):
            errors.append("component row is not an object")
            continue
        component_id = str(component.get("component_id", ""))
        if component_id in component_by_id:
            errors.append(f"duplicate component declaration: {component_id}")
        component_by_id[component_id] = component

    component_ids = set(component_by_id)
    missing = sorted(driver_ids - component_ids)
    unknown = sorted(component_ids - driver_ids)
    if missing:
        errors.append(f"missing language surface declarations for DriverPackageManifest.csv rows: {missing}")
    if unknown:
        errors.append(f"language surface declarations not present in DriverPackageManifest.csv: {unknown}")

    rows_by_id = {row.get("component_id", ""): row for row in driver_rows}
    for component_id in sorted(driver_ids & component_ids):
        errors.extend(validate_component(component_by_id[component_id], rows_by_id[component_id], schema, str(resource_identity)))

    if errors:
        print(f"sbsql_driver_language_surface_gate=failed: errors={len(errors)}", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print(
        "sbsql_driver_language_surface_gate=passed:"
        f" components={len(component_ids)} resource_identity={resource_identity}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
