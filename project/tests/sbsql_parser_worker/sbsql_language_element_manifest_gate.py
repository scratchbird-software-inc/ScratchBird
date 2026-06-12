#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the generated SBsql language element manifest."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
CSV_NAME = "SBSQL_LANGUAGE_ELEMENT_MANIFEST.csv"
JSON_NAME = "SBSQL_LANGUAGE_ELEMENT_MANIFEST.json"
GENERATOR = "project/tools/sb_parser_gen/generate_sbsql_language_element_manifest.py"

REQUIRED_COLUMNS = [
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

REQUIRED_SECTIONS = {
    "keywords",
    "phrase_topology_slots",
    "surfaces",
    "dialect_resource_metadata",
    "predictive_states",
    "renderers",
    "compatibility_ids",
    "diagnostics_messages",
    "release_channels_governance_provenance",
}

REQUIRED_CHANNELS = {
    "experimental": ("load_allowed", "no_support_commitment"),
    "preview": ("load_allowed_after_native_review", "no_release_support_commitment"),
    "beta": ("load_allowed_after_native_review", "limited_support_no_release_claim"),
    "release_supported": ("load_allowed", "release_support_claim_allowed"),
    "deprecated": ("load_allowed_with_deprecation_warning", "release_support_until_removal"),
    "revoked": ("refused", "no_support_claim"),
    "removed": ("refused", "no_support_claim"),
}

NETWORK_MODULE_NAMES = (
    "".join(("url", "lib")),
    "http",
    "".join(("so", "cket")),
    "ssl",
    "".join(("ft", "plib")),
    "".join(("sm", "tplib")),
    "".join(("telnet", "lib")),
    "".join(("imap", "lib")),
    "".join(("pop", "lib")),
    "".join(("nntp", "lib")),
    "".join(("xml", "rpc")),
    "".join(("web", "browser")),
    "".join(("re", "quests")),
    "httpx",
    "aiohttp",
    "".join(("url", "lib3")),
    "pycurl",
    "paramiko",
    "asyncssh",
    "botocore",
    "boto3",
    "google.cloud",
    "google.api_core",
)

NETWORK_MODULE_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    + "|".join(re.escape(name) for name in NETWORK_MODULE_NAMES)
    + r")(?:[\.\s]|$)",
    re.MULTILINE,
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"manifest CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != REQUIRED_COLUMNS:
            fail(f"manifest CSV columns drifted: {reader.fieldnames}")
        return list(reader)


def md5_of(path: Path) -> str:
    digest = hashlib.md5()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def validate_generator_no_network(generator: Path) -> list[str]:
    if not generator.is_file():
        return [f"language element manifest generator missing: {generator}"]
    text = generator.read_text(encoding="utf-8", errors="replace")
    findings: list[str] = []
    for match in NETWORK_MODULE_RE.finditer(text):
        line_num = text[: match.start()].count("\n") + 1
        findings.append(f"{generator.name}:{line_num} forbidden_network_import={match.group(1)!r}")
    return findings


def validate_deterministic_regeneration(root: Path, artifact_root: Path) -> list[str]:
    errors: list[str] = []
    generator = root / GENERATOR
    errors.extend(validate_generator_no_network(generator))
    if errors:
        return errors

    with tempfile.TemporaryDirectory(prefix="sbsql_language_element_manifest_") as tmp:
        temp_artifact_root = Path(tmp) / "artifacts"
        shutil.copytree(artifact_root, temp_artifact_root)
        before = {
            CSV_NAME: md5_of(temp_artifact_root / CSV_NAME),
            JSON_NAME: md5_of(temp_artifact_root / JSON_NAME),
        }
        result = subprocess.run(
            [
                sys.executable,
                str(generator),
                "--repo-root",
                str(root),
                "--artifact-root",
                str(temp_artifact_root),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            return [
                "language element manifest deterministic regeneration failed "
                f"exit={result.returncode} stderr={result.stderr.strip()[:240]}"
            ]
        after = {
            CSV_NAME: md5_of(temp_artifact_root / CSV_NAME),
            JSON_NAME: md5_of(temp_artifact_root / JSON_NAME),
        }
        for name in (CSV_NAME, JSON_NAME):
            if before[name] != after[name]:
                errors.append(
                    f"{name} non-deterministic regeneration before_md5={before[name]} after_md5={after[name]}"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    rows = read_csv(artifact_root / CSV_NAME)
    json_path = artifact_root / JSON_NAME
    if not json_path.is_file():
        fail(f"manifest JSON missing: {json_path}")
    summary = json.loads(json_path.read_text(encoding="utf-8"))

    errors: list[str] = []
    errors.extend(validate_deterministic_regeneration(root, artifact_root))
    if summary.get("schema_version") != "sbsql.language_element_manifest.v1":
        errors.append("schema_version mismatch")
    if summary.get("counts", {}).get("surfaces") != len(rows):
        errors.append("JSON surface count disagrees with CSV row count")
    if len(summary.get("elements", [])) != len(rows):
        errors.append("JSON elements are not complete")
    if not REQUIRED_SECTIONS.issubset(set(summary.get("required_sections", []))):
        errors.append("JSON required_sections does not declare every required SML-010 class")

    for channel, (admission, support) in REQUIRED_CHANNELS.items():
        policy = summary.get("release_channel_policy", {}).get(channel)
        if not policy:
            errors.append(f"release_channel_policy missing {channel}")
            continue
        if policy.get("admission") != admission:
            errors.append(f"{channel} admission drifted")
        if policy.get("support") != support:
            errors.append(f"{channel} support behavior drifted")
        if not policy.get("diagnostic"):
            errors.append(f"{channel} diagnostic missing")

    unique_fields = (
        "surface_id",
        "topology_slot_id",
        "predictive_state_id",
        "renderer_id",
        "compatibility_id",
        "message_id",
    )
    for field in unique_fields:
        values = [row[field] for row in rows]
        if len(values) != len(set(values)):
            errors.append(f"duplicate {field}")

    keyword_count = 0
    for index, row in enumerate(rows):
        missing = [
            column
            for column in REQUIRED_COLUMNS
            if column != "keyword_text" and not row.get(column)
        ]
        if missing:
            errors.append(f"row {index} {row.get('surface_id', '<missing>')} missing {missing[:4]}")
        if row["keyword_text"]:
            keyword_count += 1
        if row["release_channel"] != "release_supported":
            errors.append(f"row {row['surface_id']} release_channel={row['release_channel']}")
        if row["support_state"] != "release_supported":
            errors.append(f"row {row['surface_id']} support_state={row['support_state']}")
        if row["exact_tag"] != summary.get("exact_tag"):
            errors.append(f"row {row['surface_id']} exact_tag drifted")
        if row["profile_uuid"] != summary.get("profile_uuid"):
            errors.append(f"row {row['surface_id']} profile_uuid drifted")
        if row["common_resource_hash"] != summary.get("common_resource_hash"):
            errors.append(f"row {row['surface_id']} common_resource_hash drifted")

    if keyword_count == 0:
        errors.append("manifest contains no keyword declarations")
    if summary.get("counts", {}).get("keywords") != keyword_count:
        errors.append("keyword count disagrees between CSV and JSON")

    print(
        "sbsql_language_element_manifest_gate "
        f"rows={len(rows)} keywords={keyword_count} errors={len(errors)}"
    )
    if errors:
        print("sbsql_language_element_manifest_gate=failed", file=sys.stderr)
        for error in errors[:25]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_language_element_manifest_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
