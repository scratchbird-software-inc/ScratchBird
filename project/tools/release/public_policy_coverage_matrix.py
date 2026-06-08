#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate and validate the public default policy coverage matrix for PCR-130."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


UUID_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)

REQUIRED_AREAS = {
    "security_provider_selection": {
        "mode": "local_password_only",
        "coverage_class": "default_enabled",
        "consumer_group": "security",
        "default_behavior": "local password provider is the only enabled authentication provider",
    },
    "standard_roles_groups_grants": {
        "mode": "uuid_catalog_seed",
        "coverage_class": "default_enabled",
        "consumer_group": "security",
        "default_behavior": "standard roles groups memberships and grants are UUID catalog seeds",
    },
    "default_security_posture": {
        "mode": "deny_by_default",
        "coverage_class": "default_enabled",
        "consumer_group": "security",
        "default_behavior": "authorization starts from deny by default and explicit grants",
    },
    "memory_resource_governance": {
        "mode": "configured_policy_required",
        "coverage_class": "policy_required",
        "consumer_group": "memory_resources",
        "default_behavior": "memory and resource governors require durable policy selection",
    },
    "storage_filespace_page_policy": {
        "mode": "local_durable_fail_closed",
        "coverage_class": "default_enabled",
        "consumer_group": "storage",
        "default_behavior": "local filespace and page policies use durable fail-closed defaults",
    },
    "transaction_mga_cleanup_archive_backup_forward": {
        "mode": "mga_inventory_authority",
        "coverage_class": "engine_authority",
        "consumer_group": "transactions",
        "default_behavior": "MGA inventory remains finality cleanup archive and backup-forward authority",
    },
    "optimizer_statistics_feedback": {
        "mode": "catalog_backed_or_diagnostic_only",
        "coverage_class": "evidence_only_until_catalog_backed",
        "consumer_group": "optimizer",
        "default_behavior": "optimizer feedback is catalog backed or diagnostic evidence only",
    },
    "index_maintenance": {
        "mode": "provider_admission_required",
        "coverage_class": "policy_required",
        "consumer_group": "indexes",
        "default_behavior": "index maintenance requires provider admission and durable policy context",
    },
    "agent_policy": {
        "mode": "evidence_not_authority",
        "coverage_class": "evidence_only",
        "consumer_group": "agents",
        "default_behavior": "agent recommendations are evidence and do not become engine authority",
    },
    "diagnostics": {
        "mode": "stable_redacted_diagnostics",
        "coverage_class": "default_enabled",
        "consumer_group": "diagnostics",
        "default_behavior": "diagnostics are stable and redacted by default",
    },
    "observability": {
        "mode": "redacted_evidence_only",
        "coverage_class": "evidence_only",
        "consumer_group": "observability",
        "default_behavior": "observability records are redacted evidence only",
    },
    "unsupported_feature_behavior": {
        "mode": "deterministic_fail_closed",
        "coverage_class": "unsupported_fail_closed",
        "consumer_group": "feature_admission",
        "default_behavior": "unsupported features produce deterministic fail-closed diagnostics",
    },
    "cluster_boundary": {
        "mode": "external_provider_required",
        "coverage_class": "external_provider_boundary",
        "consumer_group": "cluster",
        "default_behavior": "cluster production behavior requires an external cluster provider",
    },
    "release_default_configuration": {
        "mode": "secure_defaults",
        "coverage_class": "default_enabled",
        "consumer_group": "release",
        "default_behavior": "first public release defaults are secure and local-only",
    },
}

POLICY_BLOCKED_AREAS = {
    "unsupported_feature_behavior",
    "cluster_boundary",
}

MATRIX_COLUMNS = [
    "area",
    "profile_uuid",
    "mode",
    "coverage_class",
    "consumer_group",
    "default_behavior",
    "post_create_filesystem_authority",
    "engine_authority_claim",
]


def fail(message: str) -> None:
    raise SystemExit(message)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def load_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"{path}: invalid JSON: {exc}") from exc


def require_uuid(value: str, field: str) -> None:
    require(bool(UUID_RE.fullmatch(value)), f"{field}: invalid UUID {value!r}")


def load_profiles(pack_root: Path) -> list[dict[str, str]]:
    doc = load_json(pack_root / "policies/policy_profiles.json")
    require(isinstance(doc, dict), "policy_profiles.json must be an object")
    require(doc.get("schema_version") == 1, "policy profile schema version must be 1")
    require(doc.get("policy_generation") == 1, "policy profile generation must be 1")
    profiles = doc.get("profiles")
    require(isinstance(profiles, list), "policy profiles array is required")
    result: list[dict[str, str]] = []
    for profile in profiles:
        require(isinstance(profile, dict), "policy profile entries must be objects")
        result.append({str(key): str(value) for key, value in profile.items()})
    return result


def validate_manifest_boundary(pack_root: Path) -> None:
    manifest = load_json(pack_root / "POLICY_PACK_MANIFEST.json")
    require(isinstance(manifest, dict), "manifest must be an object")
    require(manifest.get("policy_pack_id") == "default-local-password",
            "coverage matrix is only for the default local-password pack")
    require(manifest.get("create_time_only") is True,
            "policy pack must be create-time only")
    require(manifest.get("post_create_filesystem_authority") is False,
            "policy pack must not be post-create filesystem authority")
    provider_policy = manifest.get("default_provider_policy")
    require(isinstance(provider_policy, dict), "default provider policy is required")
    require(provider_policy.get("mode") == "local-password-only",
            "default provider policy must remain local-password-only")


def build_matrix_rows(pack_root: Path) -> list[dict[str, str]]:
    validate_manifest_boundary(pack_root)
    profiles = load_profiles(pack_root)
    seen_areas: set[str] = set()
    seen_uuids: set[str] = set()
    indexed: dict[str, dict[str, str]] = {}
    for profile in profiles:
        area = profile.get("area", "")
        mode = profile.get("mode", "")
        profile_uuid = profile.get("profile_uuid", "")
        require(area, "policy profile area is required")
        require(mode, f"{area}: policy profile mode is required")
        require_uuid(profile_uuid, f"{area}.profile_uuid")
        require(area not in seen_areas, f"duplicate policy profile area: {area}")
        require(profile_uuid not in seen_uuids, f"duplicate policy profile UUID: {profile_uuid}")
        seen_areas.add(area)
        seen_uuids.add(profile_uuid)
        indexed[area] = profile

    required = set(REQUIRED_AREAS)
    missing = required - seen_areas
    extra = seen_areas - required
    require(not missing, "missing required policy areas: " + ",".join(sorted(missing)))
    require(not extra, "unexpected policy areas without public coverage contract: " + ",".join(sorted(extra)))

    rows: list[dict[str, str]] = []
    for area in sorted(REQUIRED_AREAS):
        contract = REQUIRED_AREAS[area]
        profile = indexed[area]
        mode = profile["mode"]
        require(mode == contract["mode"],
                f"{area}: mode {mode!r} does not match required {contract['mode']!r}")
        if area in POLICY_BLOCKED_AREAS:
            require(contract["coverage_class"] in {
                "unsupported_fail_closed",
                "external_provider_boundary",
            }, f"{area}: policy-blocked area lacks explicit fail-closed coverage class")
        rows.append({
            "area": area,
            "profile_uuid": profile["profile_uuid"],
            "mode": mode,
            "coverage_class": contract["coverage_class"],
            "consumer_group": contract["consumer_group"],
            "default_behavior": contract["default_behavior"],
            "post_create_filesystem_authority": "false",
            "engine_authority_claim": "durable_catalog_after_create",
        })
    return rows


def write_matrix(path: Path, rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=MATRIX_COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--output", help="optional generated CSV matrix path")
    args = parser.parse_args(argv)
    project_root = Path(args.project_root).resolve()
    pack_root = project_root / "resources/policy-packs/default-local-password"
    require(pack_root.is_dir(), f"default policy pack directory missing: {pack_root}")
    rows = build_matrix_rows(pack_root)
    if args.output:
        write_matrix(Path(args.output), rows)
    print(f"public_policy_coverage_matrix=passed rows={len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
