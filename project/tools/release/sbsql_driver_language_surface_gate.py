#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the SBsql driver/tool language-resource surface declaration.

This gate is intentionally read-only. It requires runtime-integrated driver,
tool, and adaptor declarations and prevents them from silently diverging on
resource identity, parse fallback order, renderer lossiness, local draft SBLR
handling, cache scoping, or server revalidation authority.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


DRIVER_MANIFEST = "project/drivers/DriverPackageManifest.csv"
SURFACE_MANIFEST = "project/drivers/language/sbsql_language_surface_manifest.json"
PROTOCOL_SCHEMA = "project/drivers/language/sbsql_editor_tool_protocol.schema.json"
LANGUAGE_RESOURCE_CONTRACT_HPP = (
    "project/src/parsers/sbsql_worker/resources/language_resource_contract.hpp"
)
LANGUAGE_RESOURCE_CONTRACT_CPP = (
    "project/src/parsers/sbsql_worker/resources/language_resource_contract.cpp"
)
LANGUAGE_RESOURCE_RENDERING_CPP = "project/src/parsers/sbsql_worker/rendering/rendering.cpp"
LANGUAGE_RESOURCE_CONFORMANCE_TEST = (
    "project/tests/sbsql_parser_worker/sbsql_language_resource_contract_conformance.cpp"
)
SBSQL_PARSER_WORKER_CMAKE = "project/tests/sbsql_parser_worker/CMakeLists.txt"

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

REQUIRED_COMMON_METADATA_FIELDS = {
    "resource_identity",
    "resource_hash",
    "resource_pack_path",
    "resource_pack_manifest_sha256",
    "resource_pack_common_resource_hash",
    "supported_exact_profiles",
    "support_state",
    "fallback_diagnostics",
    "rendering_diagnostics",
    "renderer_lossiness_classes",
    "deterministic_validation",
}

EXPECTED_COMMON_METADATA_SUPPORT_STATE = "release_supported"
EXPECTED_RESOURCE_PACK_PATH = (
    "project/resources/seed-packs/initial-resource-pack/resources/i18n/"
    "sbsql-language-resource-pack"
)
EXPECTED_EXACT_PROFILES = ["en-US", "fr-FR", "de-DE", "it-IT", "es-ES"]

EXPECTED_FALLBACK_DIAGNOSTICS = [
    "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
    "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH",
]

EXPECTED_RENDERING_DIAGNOSTICS = [
    "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED",
    "SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN",
    "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE",
]

EXPECTED_RENDERER_LOSSINESS_CLASSES = [
    "lossless_canonical",
    "canonical_equivalent",
    "preferred_language_partial",
    "canonical_english_fallback",
    "not_renderable",
]

EXPECTED_PRIVACY_FAILURE_DIAGNOSTICS = [
    "SBSQL.LANG_RESOURCE.MISSING",
    "SBSQL.LANG_RESOURCE.UNSIGNED",
    "SBSQL.LANG_RESOURCE.REVOKED",
    "SBSQL.LANG_RESOURCE.EXPIRED",
    "SBSQL.LANG_RESOURCE.INCOMPATIBLE",
    "SBSQL.LANG_RESOURCE.UNSUPPORTED_CHANNEL",
    "SBSQL.LANG_RESOURCE.AMBIGUOUS_FALLBACK",
    "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE",
    "SBSQL.LANG_RESOURCE.TOPOLOGY_DIALECT_UNICODE_UNSUPPORTED",
    "SBSQL.LANG_RESOURCE.PREDICTIVE_RESOURCE_REFUSED",
    "SBSQL.LANG_RESOURCE.LOCAL_DRAFT_SBLR_REFUSED",
]

EXPECTED_PRIVACY_REDACTION_FIELDS = [
    "diagnostic_contract",
    "failure_kind",
    "disclosure_state",
    "private_input_state",
    "resource_identity_state",
    "profile_identity_state",
    "input_text_state",
    "identifier_evidence_state",
    "source_location_state",
    "local_sblr_state",
    "telemetry_redaction",
    "support_bundle_redaction",
    "server_revalidation_required",
]

EXPECTED_DETERMINISTIC_VALIDATION = {
    "stable_utf8_json": True,
    "sort_keys_for_hash": True,
    "no_wall_clock_fields": True,
}

EXPECTED_IMPLEMENTATION_STATE = "runtime_integrated_with_tests"

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


def common_metadata_hash(metadata: dict[str, Any]) -> str:
    hashed_metadata = dict(metadata)
    hashed_metadata.pop("resource_hash", None)
    payload = json.dumps(
        hashed_metadata,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return f"sha256:{hashlib.sha256(payload).hexdigest()}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def validate_common_resource_pack_metadata(
    repo_root: Path,
    metadata: Any,
    resource_identity: str,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(metadata, dict):
        return ["common_resource_pack_metadata must be an object"]

    missing = sorted(REQUIRED_COMMON_METADATA_FIELDS - set(metadata))
    unknown = sorted(set(metadata) - REQUIRED_COMMON_METADATA_FIELDS)
    if missing:
        errors.append(f"common_resource_pack_metadata missing fields: {missing}")
    if unknown:
        errors.append(f"common_resource_pack_metadata unknown fields: {unknown}")
    if missing:
        return errors

    if metadata.get("resource_identity") != resource_identity:
        errors.append("common_resource_pack_metadata.resource_identity must match manifest resource_identity")
    if metadata.get("support_state") != EXPECTED_COMMON_METADATA_SUPPORT_STATE:
        errors.append(
            "common_resource_pack_metadata.support_state must be "
            f"{EXPECTED_COMMON_METADATA_SUPPORT_STATE!r}"
        )
    if metadata.get("resource_pack_path") != EXPECTED_RESOURCE_PACK_PATH:
        errors.append("common_resource_pack_metadata.resource_pack_path must point to the public seed-pack language resource pack")
    if metadata.get("supported_exact_profiles") != EXPECTED_EXACT_PROFILES:
        errors.append("common_resource_pack_metadata.supported_exact_profiles must list the exact initial language profiles")
    pack_manifest = repo_root / EXPECTED_RESOURCE_PACK_PATH / "manifest.sblrp.json"
    if not pack_manifest.is_file():
        errors.append(f"language resource pack manifest missing: {pack_manifest}")
    else:
        observed_manifest_hash = sha256_file(pack_manifest)
        if metadata.get("resource_pack_manifest_sha256") != observed_manifest_hash:
            errors.append(
                "common_resource_pack_metadata.resource_pack_manifest_sha256 mismatch: "
                f"expected {observed_manifest_hash}"
            )
        try:
            pack_manifest_payload = read_json(pack_manifest)
            if metadata.get("resource_pack_common_resource_hash") != pack_manifest_payload.get("common_resource_hash"):
                errors.append("common_resource_pack_metadata.resource_pack_common_resource_hash does not match pack manifest")
        except (OSError, json.JSONDecodeError) as exc:
            errors.append(f"language resource pack manifest unreadable: {exc}")
    if metadata.get("fallback_diagnostics") != EXPECTED_FALLBACK_DIAGNOSTICS:
        errors.append("common_resource_pack_metadata.fallback_diagnostics must be deterministic and complete")
    if metadata.get("rendering_diagnostics") != EXPECTED_RENDERING_DIAGNOSTICS:
        errors.append("common_resource_pack_metadata.rendering_diagnostics must be deterministic and complete")
    if metadata.get("renderer_lossiness_classes") != EXPECTED_RENDERER_LOSSINESS_CLASSES:
        errors.append("common_resource_pack_metadata.renderer_lossiness_classes must be deterministic and complete")
    if metadata.get("deterministic_validation") != EXPECTED_DETERMINISTIC_VALIDATION:
        errors.append("common_resource_pack_metadata.deterministic_validation must require stable hash inputs")

    resource_hash = metadata.get("resource_hash")
    if not isinstance(resource_hash, str) or not resource_hash.startswith("sha256:"):
        errors.append("common_resource_pack_metadata.resource_hash must use sha256")
    else:
        expected_hash = common_metadata_hash(metadata)
        if resource_hash != expected_hash:
            errors.append(
                "common_resource_pack_metadata.resource_hash mismatch: "
                f"expected {expected_hash}"
            )
    return errors


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
    if component.get("implementation_state") != EXPECTED_IMPLEMENTATION_STATE:
        errors.append(
            f"{component_id}: implementation_state must be "
            f"{EXPECTED_IMPLEMENTATION_STATE!r}"
        )
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


def validate_language_resource_privacy_gate(repo_root: Path) -> list[str]:
    errors: list[str] = []
    paths = {
        LANGUAGE_RESOURCE_CONTRACT_HPP: repo_root / LANGUAGE_RESOURCE_CONTRACT_HPP,
        LANGUAGE_RESOURCE_CONTRACT_CPP: repo_root / LANGUAGE_RESOURCE_CONTRACT_CPP,
        LANGUAGE_RESOURCE_RENDERING_CPP: repo_root / LANGUAGE_RESOURCE_RENDERING_CPP,
        LANGUAGE_RESOURCE_CONFORMANCE_TEST: repo_root / LANGUAGE_RESOURCE_CONFORMANCE_TEST,
        SBSQL_PARSER_WORKER_CMAKE: repo_root / SBSQL_PARSER_WORKER_CMAKE,
    }
    missing = [rel for rel, path in paths.items() if not path.is_file()]
    if missing:
        return [f"missing language-resource privacy gate file: {rel}" for rel in sorted(missing)]

    try:
        texts = {rel: path.read_text(encoding="utf-8") for rel, path in paths.items()}
    except OSError as exc:
        return [f"failed reading language-resource privacy gate source: {exc}"]

    contract_text = (
        texts[LANGUAGE_RESOURCE_CONTRACT_HPP] + "\n" +
        texts[LANGUAGE_RESOURCE_CONTRACT_CPP]
    )
    test_text = texts[LANGUAGE_RESOURCE_CONFORMANCE_TEST]
    rendering_text = texts[LANGUAGE_RESOURCE_RENDERING_CPP]
    cmake_text = texts[SBSQL_PARSER_WORKER_CMAKE]

    for code in EXPECTED_PRIVACY_FAILURE_DIAGNOSTICS:
        if code not in contract_text:
            errors.append(f"language-resource privacy diagnostic missing from contract: {code}")
        if code not in test_text:
            errors.append(f"language-resource privacy diagnostic missing from executable test: {code}")

    for field in EXPECTED_PRIVACY_REDACTION_FIELDS:
        if field not in contract_text:
            errors.append(f"language-resource redaction field missing from contract: {field}")
        if field not in test_text:
            errors.append(f"language-resource redaction field missing from executable test: {field}")

    if "IsPublicDiagnosticFieldAllowed" not in rendering_text:
        errors.append("message-vector rendering must apply public diagnostic field filtering")
    if "RenderMessageVectorSet" not in test_text or "MessageVectorToJson" not in test_text:
        errors.append("language-resource privacy test must exercise text and JSON diagnostic renderers")
    for label in ("SML-091", "SML-093"):
        if label not in cmake_text:
            errors.append(f"sbsql parser worker CMake labels must include {label}")
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
    if schema.get("syntax_profile_order") != EXPECTED_SYNTAX_PROFILE_ORDER:
        errors.append("protocol schema syntax_profile_order mismatch")
    if schema.get("renderer_lossiness_classes") != EXPECTED_RENDERER_LOSSINESS_CLASSES:
        errors.append("protocol schema renderer_lossiness_classes mismatch")
    if schema.get("fallback_diagnostics") != EXPECTED_FALLBACK_DIAGNOSTICS:
        errors.append("protocol schema fallback_diagnostics mismatch")
    if schema.get("rendering_diagnostics") != EXPECTED_RENDERING_DIAGNOSTICS:
        errors.append("protocol schema rendering_diagnostics mismatch")
    if allowed_values(schema, "implementation_state") != {EXPECTED_IMPLEMENTATION_STATE}:
        errors.append(
            "protocol schema implementation_state values must require "
            f"{EXPECTED_IMPLEMENTATION_STATE!r}"
        )
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
    errors.extend(
        validate_common_resource_pack_metadata(
            repo_root,
            surface.get("common_resource_pack_metadata"),
            str(resource_identity),
        )
    )

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

    errors.extend(validate_language_resource_privacy_gate(repo_root))

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
