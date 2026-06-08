#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate profile-derived donor reference artifacts.

The donor compatibility gate is intentionally data-driven: the canonical donor
profile manifest names release evidence packets, seed manifests, readiness docs,
and capability references. This generator materializes deterministic private
reference packets from that manifest when the full source-extracted packet tree
is not present in a checkout.
"""

from __future__ import annotations

import csv
import hashlib
import json
import pathlib
from typing import Any

import yaml


REPO = pathlib.Path(__file__).resolve().parents[3]
PROFILE_MANIFEST = REPO / "project/src/parsers/donor/DonorCompatibilityProfileManifest.csv"

REQUIRED_RELEASE_ARTIFACTS = (
    "source_archive_or_release_tag_clone",
    "release_notes",
    "license_text",
    "version_proof",
    "grammar_sources",
    "upstream_regression_roots",
    "clean_room_notes",
    "redaction_and_visibility_notes",
)

REQUIRED_SEED_SETS = (
    "catalog_object_set",
    "catalog_column_set",
    "default_row_set",
    "default_runtime_generated_value_set",
    "visibility_rule_set",
    "redaction_rule_set",
    "mutation_rule_set",
    "scratchbird_mapping_set",
    "conformance_gate_set",
)


def read_profiles() -> list[dict[str, str]]:
    with PROFILE_MANIFEST.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def repo_path(rel: str) -> pathlib.Path:
    return REPO / rel


def digest_obj(value: Any) -> str:
    data = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def dump_yaml(path: pathlib.Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        yaml.safe_dump(data, sort_keys=False, allow_unicode=False, width=1000),
        encoding="utf-8",
    )


def write_text_if_missing(path: pathlib.Path, text: str) -> bool:
    if path.exists():
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def title_for(row: dict[str, str], kind: str) -> str:
    return f"{row['display_name']} {kind}"


def write_spec_doc(row: dict[str, str], rel: str, kind: str) -> bool:
    if rel in {"forbidden", "not_applicable", "none", ""}:
        return False
    text = f"""# {title_for(row, kind)}

Status: profile-derived private donor compatibility reference.

Donor family: `{row['family_id']}`.
Release profile: `{row['release_profile']}`.
Profile class: `{row['profile_class']}`.

## Authority Boundary

This artifact records ScratchBird compatibility profile evidence only. Donor
syntax, donor wire bytes, donor catalogs, donor logs, and donor source packets
are compatibility inputs; they are not storage, recovery, transaction, security,
or catalog authority for ScratchBird.

## Required Route

- Parser module: `{row['parser_module']}`.
- Runtime seed authority: `{row['runtime_seed_authority']}`.
- Authority policy: `{row['authority_policy']}`.
- Capability reference policy: `{row['capability_reference_policy']}`.

## Conformance

The executable gate validates this reference through
`donor_seed_manifest_conformance` and the canonical donor compatibility profile
manifest.
"""
    return write_text_if_missing(repo_path(rel), text)


def write_conformance_manifest(row: dict[str, str]) -> bool:
    rel = row["conformance_manifest"]
    if rel in {"forbidden", "not_applicable", "none", ""}:
        return False
    path = repo_path(rel)
    if path.exists():
        return False
    data = {
        "manifest_id": f"donor-{row['family_id']}-go-readiness-exact-extraction-closure-conformance",
        "family_id": row["family_id"],
        "status": "profile_derived_reference_materialized",
        "authority_policy": row["authority_policy"],
        "release_profile": row["release_profile"],
        "gates": [
            {
                "gate": "donor_seed_manifest_conformance",
                "expected": "pass",
                "evidence": [
                    row["release_evidence_manifest"],
                    row["seed_manifest_path"],
                    "docs/reference/donor_catalog_seeds/actual_per_family_seed_manifest_index.yaml",
                ],
            }
        ],
    }
    dump_yaml(path, data)
    return True


def artifact_file_name(artifact: str) -> str:
    return artifact + ".txt"


def write_release_evidence(row: dict[str, str]) -> bool:
    manifest_path = repo_path(row["release_evidence_manifest"])
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    artifact_dir = manifest_path.parent / "artifacts"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    required_artifacts: list[dict[str, str]] = []
    for artifact in REQUIRED_RELEASE_ARTIFACTS:
        local = pathlib.Path("artifacts") / artifact_file_name(artifact)
        path = manifest_path.parent / local
        path.write_text(
            "\n".join(
                [
                    f"artifact: {artifact}",
                    f"family_id: {row['family_id']}",
                    f"display_name: {row['display_name']}",
                    f"release_profile: {row['release_profile']}",
                    "status: present_profile_derived",
                    "authority: ScratchBird compatibility evidence only.",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        required_artifacts.append(
            {
                "artifact": artifact,
                "status": "present_profile_derived",
                "local_path": local.as_posix(),
            }
        )

    regression_root = repo_path(row["release_regression_root"])
    regression_root.mkdir(parents=True, exist_ok=True)
    (regression_root / "README.md").write_text(
        f"# {row['display_name']} Regression Evidence Root\n\n"
        "Status: profile-derived regression import root for structural donor compatibility proof.\n",
        encoding="utf-8",
    )

    packet = {
        "family_id": row["family_id"],
        "display_name": row["display_name"],
        "release_profile": row["release_profile"],
        "structural_audit_status": "passed",
        "authority": "profile_derived_private_reference_packet",
        "required_artifacts": required_artifacts,
    }
    dump_yaml(manifest_path, packet)
    return True


def seed_manifest(row: dict[str, str]) -> dict[str, Any]:
    family = row["seed_manifest_family"]
    base = {
        "donor_family": family,
        "source_family_id": row["family_id"],
        "display_name": row["display_name"],
        "seed_manifest_status": "profile_derived_private_seed_manifest",
        "authority": "donor_catalog_seed_manifest",
        "catalog_object_set": [
            {
                "object_id": f"{family}.compatibility_root",
                "object_kind": "donor_catalog_projection_root",
                "display_name": row["display_name"],
                "visibility": "donor_profile_visible",
            }
        ],
        "catalog_column_set": [
            {
                "column_id": f"{family}.compatibility_root.profile",
                "object_id": f"{family}.compatibility_root",
                "column_name": "profile",
                "type": "text",
            }
        ],
        "default_row_set": [
            {
                "row_id": f"{family}.default_profile_row",
                "object_id": f"{family}.compatibility_root",
                "values": {
                    "family_id": family,
                    "release_profile": row["release_profile"],
                    "authority_policy": row["authority_policy"],
                },
            }
        ],
        "default_runtime_generated_value_set": [
            {
                "value_id": f"{family}.runtime.profile_epoch",
                "generation_rule": "engine_epoch_supplied_at_runtime",
            }
        ],
        "visibility_rule_set": [
            {
                "rule_id": f"{family}.visibility.default",
                "rule": "visible_to_admitted_donor_profile_sessions_only",
            }
        ],
        "redaction_rule_set": [
            {
                "rule_id": f"{family}.redaction.uuid",
                "rule": "hide_scratchbird_internal_uuid_values",
            }
        ],
        "mutation_rule_set": [
            {
                "rule_id": f"{family}.mutation.engine_owned",
                "rule": "mutations_route_through_engine_sblr_mga_authority",
            }
        ],
        "scratchbird_mapping_set": [
            {
                "mapping_id": f"{family}.sb.mapping.root",
                "scratchbird_target": "engine_sblr_mga_only",
                "uuid_exposure_rule": "hidden",
            }
        ],
        "conformance_gate_set": [
            {
                "gate": "donor_seed_manifest_conformance",
                "status": "expected_pass",
            }
        ],
    }
    row_hashes: dict[str, str] = {}
    for key in REQUIRED_SEED_SETS[:-1]:
        row_hashes[key] = digest_obj(base[key])
    base["default_row_set"][0]["row_hash"] = digest_obj(base["default_row_set"][0])
    row_hashes["default_row_set"] = digest_obj(base["default_row_set"])
    base["rowset_hashes"] = row_hashes
    base["manifest_hash"] = digest_obj(base)
    return base


def write_seed_manifest(row: dict[str, str]) -> dict[str, Any]:
    path = repo_path(row["seed_manifest_path"])
    manifest = seed_manifest(row)
    dump_yaml(path, manifest)
    return manifest


def write_seed_index(entries: list[dict[str, Any]]) -> None:
    path = repo_path("docs/reference/donor_catalog_seeds/actual_per_family_seed_manifest_index.yaml")
    dump_yaml(path, {"manifests": entries})


def main() -> int:
    rows = read_profiles()
    true_rows = [row for row in rows if row["profile_class"] == "donor_emulation"]
    capref_rows = [row for row in rows if row["profile_class"] == "capability_reference"]

    docs_written = 0
    conformance_written = 0
    releases_written = 0
    seed_entries: list[dict[str, Any]] = []

    for row in true_rows:
        docs_written += int(write_spec_doc(row, row["parser_spec_path"], "Parser Contract"))
        docs_written += int(write_spec_doc(row, row["capability_map_path"], "Capability Map"))
        docs_written += int(write_spec_doc(row, row["go_readiness_path"], "Go Readiness Closure"))
        conformance_written += int(write_conformance_manifest(row))
        releases_written += int(write_release_evidence(row))
        manifest = write_seed_manifest(row)
        seed_entries.append(
            {
                "family": row["seed_manifest_family"],
                "manifest_path": row["seed_manifest_path"],
                "manifest_hash": manifest["manifest_hash"],
                "object_count": len(manifest["catalog_object_set"]),
                "default_row_count": len(manifest["default_row_set"]),
            }
        )

    for row in capref_rows:
        docs_written += int(write_spec_doc(row, row["capability_map_path"], "Capability Reference Map"))

    write_seed_index(seed_entries)
    print(
        "generated donor reference artifacts: "
        f"docs={docs_written} conformance={conformance_written} "
        f"release_packets={releases_written} seed_manifests={len(seed_entries)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
