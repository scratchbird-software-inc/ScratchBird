#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate generated, AI-assisted, copied-reference, and compatibility-derived provenance policy."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_GENERATED_AI_COMPAT_PROVENANCE
# PUBLIC_PROVENANCE_POLICY_GATE

PROVENANCE_CLASSES: tuple[dict[str, Any], ...] = (
    {
        "class_id": "generated_checked_in_code",
        "release_status": "allowed_with_public_generator_and_drift_gate",
        "required_evidence": (
            "generator_path",
            "public_input_manifest",
            "output_sha256",
            "regeneration_required",
            "drift_detection_required",
        ),
        "authority_limit": "generated output is checked-in source evidence only",
    },
    {
        "class_id": "generated_fixture_data",
        "release_status": "allowed_with_deterministic_artifact_manifest",
        "required_evidence": (
            "artifact_path",
            "sha256",
            "size_bytes",
            "category",
            "source_inputs",
        ),
        "authority_limit": "fixture data cannot become engine runtime authority",
    },
    {
        "class_id": "ai_assisted_code",
        "release_status": "allowed_only_after_human_review_and_public_provenance",
        "required_evidence": (
            "human_review_required",
            "public_tree_inputs_only",
            "no_private_prompt_required",
            "no_model_output_authority",
        ),
        "authority_limit": "AI assistance is not source, transaction, security, or recovery authority",
    },
    {
        "class_id": "copied_reference_code",
        "release_status": "not_accepted_without_license_and_origin_evidence",
        "required_evidence": (
            "source_license_record",
            "copy_scope_record",
            "reviewer_approval",
            "release_owner_acceptance",
        ),
        "authority_limit": "copied reference code cannot enter public release without explicit evidence",
    },
    {
        "class_id": "reference_derived_behavior_docs",
        "release_status": "allowed_as_behavior_mapping_or_refusal_evidence_only",
        "required_evidence": (
            "reference_surface",
            "exact_mapping_or_refusal_policy",
            "no_copied_reference_code",
            "no_reference_runtime_authority",
        ),
        "authority_limit": "compatibility behavior never owns engine execution or finality",
    },
)

EVIDENCE_CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "generated_source_provenance_gate",
        "path": "project/tools/release/public_generated_source_provenance.py",
        "tokens": (
            "PUBLIC_GENERATED_SOURCE_PROVENANCE",
            "regeneration_required",
            "checked_in_authority_status",
            "drift_detection_required",
            "public_generated_provenance_gate=passed",
        ),
    },
    {
        "surface": "deterministic_generated_manifest",
        "path": "project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv",
        "tokens": (
            "artifact_path",
            "sha256",
            "size_bytes",
            "category",
            "source_inputs",
            "test_generated_reference_alias",
        ),
    },
    {
        "surface": "reference_alias_fixture_policy",
        "path": "project/tests/sbsql_parser_worker/generated/reference_alias/REFERENCE_ALIAS_RENDERING_FIXTURES.csv",
        "tokens": (
            "alias_kind",
            "fixture_root",
            "command_tag_policy",
            "warning_error_policy",
            "reference_catalog_projection_or_exact_refusal",
        ),
    },
    {
        "surface": "reference_alias_conformance",
        "path": "project/tests/sbsql_parser_worker/generated/reference_alias/sbsql_reference_alias_rendering_conformance.cpp",
        "tokens": (
            "reference_surface",
            "map_to_native_behavior_or_exact_canonical_refusal",
            "lacks compatibility rendering",
        ),
    },
    {
        "surface": "release_attestation_generated_provenance",
        "path": "project/tools/release/public_release_attestation_gate.py",
        "tokens": (
            "generated_source_provenance",
            "deterministic_generated_manifest",
            "public_tree_inputs_only",
            "release_attestation_is_evidence_only",
        ),
    },
    {
        "surface": "secret_scan_public_package_guard",
        "path": "project/tools/release/public_secret_credential_scan.py",
        "tokens": (
            "PUBLIC_SECRET_CREDENTIAL_SCAN",
            "private_keys_rejected",
            "public_release_inputs_only",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "PUBLIC_PROVENANCE_POLICY_GATE",
            "public_generated_ai_reference_provenance_gate",
            "public_generated_provenance_gate",
            "public_release_attestation_gate",
            "PCR-GATE-154",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_generated_ai_reference_provenance_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_text(repo_root: Path, relative_path: str) -> str:
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{relative_path}")
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{relative_path}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{relative_path}:{exc}")


def validate_policy_class(row: dict[str, Any]) -> dict[str, Any]:
    class_id = row["class_id"]
    required = row["required_evidence"]
    require(isinstance(class_id, str) and class_id, "policy_class_invalid")
    require(isinstance(row["release_status"], str) and row["release_status"], f"status_invalid:{class_id}")
    require(isinstance(required, tuple) and required, f"required_evidence_invalid:{class_id}")
    require(isinstance(row["authority_limit"], str) and row["authority_limit"], f"authority_limit_invalid:{class_id}")
    return {
        "surface": class_id,
        "path": "provenance_policy",
        "token_count": len(required),
        "source_sha256": sha256_text(json.dumps(row, sort_keys=True)),
        "token_digest_sha256": sha256_text("\n".join(required) + "\n"),
        "status": "pass",
        "release_status": row["release_status"],
        "authority_limit": row["authority_limit"],
    }


def validate_evidence_check(repo_root: Path, check: dict[str, Any]) -> dict[str, Any]:
    surface = check["surface"]
    path_text = check["path"]
    tokens = check["tokens"]
    require(isinstance(surface, str) and surface, "surface_invalid")
    require(isinstance(path_text, str) and path_text, f"path_invalid:{surface}")
    require(isinstance(tokens, tuple) and tokens, f"tokens_invalid:{surface}")
    text = read_text(repo_root, path_text)
    token_digests: list[str] = []
    for token in tokens:
        require(isinstance(token, str) and token, f"token_invalid:{surface}")
        if token not in text:
            fail(f"token_missing:{surface}:{path_text}:{token}")
        token_digests.append(sha256_text(token))
    return {
        "surface": surface,
        "path": path_text,
        "token_count": len(tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
        "status": "pass",
        "release_status": "",
        "authority_limit": "",
    }


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "surface",
                "path",
                "token_count",
                "source_sha256",
                "token_digest_sha256",
                "status",
                "release_status",
                "authority_limit",
            ],
        )
        writer.writeheader()
        writer.writerows(records)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    records = [validate_policy_class(row) for row in PROVENANCE_CLASSES]
    records.extend(validate_evidence_check(repo_root, check) for check in EVIDENCE_CHECKS)
    matrix_text = "\n".join(
        f"{record['surface']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_PROVENANCE_POLICY_GATE",
        "marker": "PUBLIC_GENERATED_AI_COMPAT_PROVENANCE",
        "status": "pass",
        "checks": records,
        "check_count": len(records),
        "matrix_sha256": sha256_text(matrix_text),
        "policy": {
            "generated_code_requires_public_generator": True,
            "ai_assisted_code_requires_human_review": True,
            "copied_reference_code_requires_explicit_license_origin_evidence": True,
            "reference_derived_behavior_is_evidence_only": True,
            "legal_approval_required_elsewhere": True,
        },
        "authority": {
            "provenance_policy_is_engine_authority": False,
            "reference_runtime_authority": False,
            "ai_model_output_authority": False,
            "release_gate_is_legal_approval": False,
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_generated_ai_reference_provenance_gate=passed "
        f"checks={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
