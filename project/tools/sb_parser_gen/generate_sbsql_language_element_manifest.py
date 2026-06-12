#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate the SBsql language element manifest.

The manifest is intentionally derived from the already published SBsql
surface-to-SBLR release artifacts. It does not infer new language behavior;
it repackages the release rows into the resource classes needed by the
multilingual language-resource contract: keywords, phrase/topology slots,
surfaces, predictive states, renderers, compatibility IDs, diagnostics,
release-channel governance, and provenance.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
RELEASE_DECLARATION = "SBSQL_SURFACE_RELEASE_DECLARATION.csv"
STRICT_LEDGER = "STRICT_ROW_COVERAGE_LEDGER.csv"
OUTPUT_CSV = "SBSQL_LANGUAGE_ELEMENT_MANIFEST.csv"
OUTPUT_JSON = "SBSQL_LANGUAGE_ELEMENT_MANIFEST.json"

PROFILE_UUID = "sbsql.builtin.recovery.en"
EXACT_TAG = "en"
DIALECT_PROFILE_UUID = "sbsql.v3"
TOPOLOGY_PROFILE_UUID = "topology.sbsql.canonical.v1"
COMPATIBILITY_IDENTITY = "sbsql.resource.compat.v1"
RELEASE_CHANNEL = "release_supported"
SUPPORT_STATE = "release_supported"
GOVERNANCE_EVIDENCE_ID = "SML-010.language_element_manifest.generator"
NATIVE_REVIEW_EVIDENCE_ID = "SML-010.native_reviewed_resource_manifest"
SUPPORT_OWNER_ID = "scratchbird.release.sbsql"
TRACE_ORACLE_ID = "SML-010.surface_release_declaration"
PROVENANCE_ID = "sbsql.language_element_manifest.provenance.v1"

COLUMNS = [
    "surface_id",
    "canonical_name",
    "element_kind",
    "keyword_text",
    "keyword_class",
    "phrase_id",
    "topology_slot_id",
    "topology_role",
    "surface_kind",
    "family",
    "sblr_operation_family",
    "exact_tag",
    "profile_uuid",
    "dialect_profile_uuid",
    "topology_profile_uuid",
    "common_resource_hash",
    "predictive_state_id",
    "predictive_state",
    "renderer_id",
    "renderer_lossiness",
    "compatibility_id",
    "diagnostic_code",
    "message_id",
    "release_channel",
    "support_state",
    "governance_evidence_id",
    "provenance_id",
    "release_status",
]

REQUIRED_CHANNEL_POLICY = {
    "experimental": {
        "admission": "load_allowed",
        "support": "no_support_commitment",
        "diagnostic": "SBSQL.LANG_RESOURCE.EXPERIMENTAL_UNSUPPORTED",
    },
    "preview": {
        "admission": "load_allowed_after_native_review",
        "support": "no_release_support_commitment",
        "diagnostic": "SBSQL.LANG_RESOURCE.PREVIEW_LIMITED_SUPPORT",
    },
    "beta": {
        "admission": "load_allowed_after_native_review",
        "support": "limited_support_no_release_claim",
        "diagnostic": "SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT",
    },
    "release_supported": {
        "admission": "load_allowed",
        "support": "release_support_claim_allowed",
        "diagnostic": "SBSQL.LANG_RESOURCE.RELEASE_SUPPORTED",
    },
    "deprecated": {
        "admission": "load_allowed_with_deprecation_warning",
        "support": "release_support_until_removal",
        "diagnostic": "SBSQL.LANG_RESOURCE.DEPRECATED",
    },
    "revoked": {
        "admission": "refused",
        "support": "no_support_claim",
        "diagnostic": "SBSQL.LANG_RESOURCE.REVOKED",
    },
    "removed": {
        "admission": "refused",
        "support": "no_support_claim",
        "diagnostic": "SBSQL.LANG_RESOURCE.REMOVED",
    },
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def stable_hash(*parts: str, length: int = 16) -> str:
    material = "\x1f".join(parts).encode("utf-8")
    return hashlib.sha256(material).hexdigest()[:length]


def stable_id(prefix: str, *parts: str) -> str:
    return f"{prefix}.{stable_hash(*parts)}"


def safe_token(value: str) -> str:
    token = re.sub(r"[^A-Za-z0-9]+", "_", value.strip()).strip("_").lower()
    return token or "anonymous"


def keyword_text(canonical_name: str) -> str:
    stripped = canonical_name.strip()
    if re.fullmatch(r"[A-Z][A-Z0-9_]*", stripped):
        return stripped
    return ""


def element_kind(surface_kind: str, canonical_name: str) -> str:
    if keyword_text(canonical_name):
        return "keyword"
    if surface_kind == "grammar_production":
        return "phrase_topology_slot"
    if surface_kind in {"function", "operator", "variable"}:
        return f"surface_{surface_kind}"
    return f"surface_{safe_token(surface_kind)}"


def topology_role(row: dict[str, str]) -> str:
    if row.get("surface_kind") == "grammar_production":
        return safe_token(row["canonical_name"])
    family = safe_token(row.get("family", "general"))
    kind = safe_token(row.get("surface_kind", "surface"))
    return f"{family}_{kind}"


def diagnostic_code(row: dict[str, str]) -> str:
    final_status = row.get("final_status", "")
    if final_status == "cluster_provider_route_passed":
        return "SBSQL.LANG_ELEMENT.CLUSTER_PROVIDER_GATED"
    if final_status == "exact_refusal_passed":
        return "SBSQL.LANG_ELEMENT.EXACT_REFUSAL"
    return "SBSQL.LANG_ELEMENT.RELEASE_SUPPORTED"


def predictive_state(row: dict[str, str]) -> str:
    final_status = row.get("final_status", "")
    if final_status == "cluster_provider_route_passed":
        return "provider_gated_completion"
    if final_status == "exact_refusal_passed":
        return "refusal_completion"
    return "admitted_completion"


def build_rows(release_rows: list[dict[str, str]],
               ledger_by_surface: dict[str, dict[str, str]],
               common_resource_hash: str) -> list[dict[str, str]]:
    output: list[dict[str, str]] = []
    for row in sorted(release_rows, key=lambda item: item["surface_id"]):
        surface_id = row["surface_id"]
        ledger = ledger_by_surface.get(surface_id)
        if ledger is None:
            fail(f"{surface_id} missing STRICT_ROW_COVERAGE_LEDGER row")
        role = topology_role(row)
        keyword = keyword_text(row["canonical_name"])
        compat_id = stable_id("compat", surface_id, row["canonical_name"])
        diag_code = diagnostic_code(row)
        output.append(
            {
                "surface_id": surface_id,
                "canonical_name": row["canonical_name"],
                "element_kind": element_kind(row["surface_kind"], row["canonical_name"]),
                "keyword_text": keyword,
                "keyword_class": "reserved" if keyword else "not_keyword",
                "phrase_id": stable_id("phrase", surface_id, role),
                "topology_slot_id": stable_id("slot", surface_id, role),
                "topology_role": role,
                "surface_kind": row["surface_kind"],
                "family": row["family"],
                "sblr_operation_family": ledger.get("sblr_operation_family", ""),
                "exact_tag": EXACT_TAG,
                "profile_uuid": PROFILE_UUID,
                "dialect_profile_uuid": DIALECT_PROFILE_UUID,
                "topology_profile_uuid": TOPOLOGY_PROFILE_UUID,
                "common_resource_hash": common_resource_hash,
                "predictive_state_id": stable_id("predictive", surface_id, row["canonical_name"]),
                "predictive_state": predictive_state(row),
                "renderer_id": stable_id("renderer", surface_id, PROFILE_UUID),
                "renderer_lossiness": "canonical_equivalent",
                "compatibility_id": compat_id,
                "diagnostic_code": diag_code,
                "message_id": stable_id("message", diag_code, surface_id),
                "release_channel": RELEASE_CHANNEL,
                "support_state": SUPPORT_STATE,
                "governance_evidence_id": GOVERNANCE_EVIDENCE_ID,
                "provenance_id": PROVENANCE_ID,
                "release_status": row["release_status"],
            }
        )
    return output


def validate_manifest(rows: list[dict[str, str]], summary: dict[str, object]) -> list[str]:
    errors: list[str] = []
    if not rows:
        errors.append("language element manifest has no rows")
        return errors
    expected_columns = set(COLUMNS)
    for index, row in enumerate(rows):
        missing = [col for col in COLUMNS if not row.get(col) and col not in {"keyword_text"}]
        if missing:
            errors.append(f"row {index} {row.get('surface_id', '<missing>')} missing {missing[:4]}")
        if set(row) != expected_columns:
            errors.append(f"row {index} has unexpected columns")
    surface_ids = [row["surface_id"] for row in rows]
    if len(surface_ids) != len(set(surface_ids)):
        errors.append("duplicate surface_id in language element manifest")
    for field in (
        "topology_slot_id",
        "predictive_state_id",
        "renderer_id",
        "compatibility_id",
        "message_id",
    ):
        values = [row[field] for row in rows]
        if len(values) != len(set(values)):
            errors.append(f"duplicate {field} in language element manifest")
    if not any(row["keyword_text"] for row in rows):
        errors.append("language element manifest contains no keyword declarations")
    policy = summary.get("release_channel_policy", {})
    for channel in REQUIRED_CHANNEL_POLICY:
        if channel not in policy:
            errors.append(f"release channel policy missing {channel}")
    required_sections = set(summary.get("required_sections", []))
    for section in (
        "keywords",
        "phrase_topology_slots",
        "surfaces",
        "dialect_resource_metadata",
        "predictive_states",
        "renderers",
        "compatibility_ids",
        "diagnostics_messages",
        "release_channels_governance_provenance",
    ):
        if section not in required_sections:
            errors.append(f"required section missing from summary: {section}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--output-root")
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root
    output_root = Path(args.output_root) if args.output_root else artifact_root
    if not output_root.is_absolute():
        output_root = root / output_root

    release_rows = read_csv(artifact_root / RELEASE_DECLARATION)
    ledger_rows = read_csv(artifact_root / STRICT_LEDGER)
    ledger_by_surface = {row["surface_id"]: row for row in ledger_rows}
    if len(ledger_by_surface) != len(ledger_rows):
        fail("STRICT_ROW_COVERAGE_LEDGER contains duplicate surface_id values")

    common_hash = stable_id("common.sbsql.language_elements", RELEASE_DECLARATION, STRICT_LEDGER)
    rows = build_rows(release_rows, ledger_by_surface, common_hash)
    counts = Counter(row["element_kind"] for row in rows)
    release_counts = Counter(row["release_status"] for row in rows)
    diagnostic_counts = Counter(row["diagnostic_code"] for row in rows)

    summary = {
        "schema_version": "sbsql.language_element_manifest.v1",
        "manifest_uuid": stable_id("manifest", str(len(rows)), common_hash),
        "profile_uuid": PROFILE_UUID,
        "exact_tag": EXACT_TAG,
        "dialect_profile_uuid": DIALECT_PROFILE_UUID,
        "topology_profile_uuid": TOPOLOGY_PROFILE_UUID,
        "common_resource_hash": common_hash,
        "compatibility_identity": COMPATIBILITY_IDENTITY,
        "resource_manifest": {
            "canonical_surface_registry_hash": stable_id("surface.registry", str(len(rows))),
            "sblr_registry_hash": stable_id("sblr.registry", str(len(rows))),
            "predictive_grammar_hash": stable_id("predictive.registry", str(len(rows))),
            "renderer_registry_hash": stable_id("renderer.registry", str(len(rows))),
            "diagnostic_pack_hash": stable_id("diagnostic.registry", str(len(rows))),
            "release_channel": RELEASE_CHANNEL,
            "support_state": SUPPORT_STATE,
            "governance_evidence_id": GOVERNANCE_EVIDENCE_ID,
            "native_review_evidence_id": NATIVE_REVIEW_EVIDENCE_ID,
            "support_owner_id": SUPPORT_OWNER_ID,
            "trace_oracle_id": TRACE_ORACLE_ID,
        },
        "provenance": [
            {
                "provenance_id": PROVENANCE_ID,
                "source_name": "ScratchBird SBsql surface release declaration",
                "source_version": "1",
                "license_id": "MPL-2.0",
                "transformation_id": "generate_sbsql_language_element_manifest.py",
                "sbom_component_id": "sbom.sbsql.language_element_manifest",
                "third_party_notice_id": "notice.scratchbird.builtin",
                "redistribution_allowed": True,
            }
        ],
        "generated_from": [
            f"artifacts/{RELEASE_DECLARATION}",
            f"artifacts/{STRICT_LEDGER}",
        ],
        "required_sections": [
            "keywords",
            "phrase_topology_slots",
            "surfaces",
            "dialect_resource_metadata",
            "predictive_states",
            "renderers",
            "compatibility_ids",
            "diagnostics_messages",
            "release_channels_governance_provenance",
        ],
        "counts": {
            "surfaces": len(rows),
            "keywords": sum(1 for row in rows if row["keyword_text"]),
            "phrase_topology_slots": len({row["topology_slot_id"] for row in rows}),
            "predictive_states": len({row["predictive_state_id"] for row in rows}),
            "renderers": len({row["renderer_id"] for row in rows}),
            "compatibility_ids": len({row["compatibility_id"] for row in rows}),
            "diagnostics_messages": len({row["message_id"] for row in rows}),
        },
        "element_kind_counts": dict(sorted(counts.items())),
        "release_status_counts": dict(sorted(release_counts.items())),
        "diagnostic_counts": dict(sorted(diagnostic_counts.items())),
        "release_channel_policy": REQUIRED_CHANNEL_POLICY,
        "elements": rows,
    }

    errors = validate_manifest(rows, summary)
    if errors:
        for error in errors[:25]:
            print(error, file=sys.stderr)
        return 1

    if not args.validate_only:
        output_root.mkdir(parents=True, exist_ok=True)
        with (output_root / OUTPUT_CSV).open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=COLUMNS, lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
        (output_root / OUTPUT_JSON).write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )

    print(
        "sbsql_language_element_manifest=generated "
        f"rows={len(rows)} keywords={summary['counts']['keywords']} "
        f"release_channels={len(REQUIRED_CHANNEL_POLICY)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
