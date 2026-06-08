#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public support and release lifecycle policy evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_LIFECYCLE_POLICY_GATE
# PUBLIC_SUPPORT_RELEASE_LIFECYCLE

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "support_release_lifecycle_policy",
        "path": "project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md",
        "tokens": (
            "PUBLIC_SUPPORT_RELEASE_LIFECYCLE",
            "public_release_evidence_only",
            "Supported Platforms",
            "File-Format And Upgrade Promises",
            "Security Patch Window",
            "Preview Status",
            "Unsupported Features",
            "First-Release Support Boundaries",
            "supported first proof lane",
            "target platform pending native CI/runtime proof",
            "target platform pending native runner proof",
            "before support is claimed",
            "legal/IP closure artifacts that are still waiting for lawyer approval",
        ),
    },
    {
        "surface": "platform_support_matrix",
        "path": "docs/build_requirements/README.md",
        "tokens": (
            "Supported Platform Matrix",
            "Linux x86_64, Ubuntu 24.04 LTS",
            "Fully proven first target",
            "Windows x64, Windows 11 or Windows Server 2022/2025",
            "Target platform pending native CI/runtime proof",
            "FreeBSD x86_64, FreeBSD 14.x",
            "Target platform pending native runner proof",
            "macOS",
            "No support claim",
        ),
    },
    {
        "surface": "admin_runbook_boundary",
        "path": "project/docs/admin/PUBLIC_ADMIN_RUNBOOKS.md",
        "tokens": (
            "PUBLIC_ADMIN_RUNBOOKS",
            "RUNBOOK_UNSUPPORTED_FEATURES",
            "RUNBOOK_UPGRADE",
            "RUNBOOK_CONFIG_DEFAULTS",
            "public_release_evidence_only",
            "MGA transaction inventory remains finality authority",
        ),
    },
    {
        "surface": "api_abi_compatibility_policy",
        "path": "project/docs/public_api/CORE_BETA_PUBLIC_COMPATIBILITY_POLICY.md",
        "tokens": (
            "PUBLIC_API_COMPATIBILITY_POLICY",
            "Semantic Versioning",
            "Patch versions must not add, remove, or reorder public ABI symbols",
            "File format",
            "Policy pack schema",
            "Removal Gate",
        ),
    },
    {
        "surface": "api_abi_freeze_inventory",
        "path": "project/docs/public_api/CORE_BETA_PUBLIC_API_ABI.md",
        "tokens": (
            "Core Beta Public API and ABI Freeze",
            "Version: `1.0.0`",
            "Packaged Public Headers",
            "C ABI Symbols",
            "MGA transaction inventory remains finality authority",
            "Cluster-positive behavior is outside core",
        ),
    },
    {
        "surface": "upgrade_migration_evidence",
        "path": "project/tests/release/public_upgrade_migration_gate.cpp",
        "tokens": (
            "PUBLIC_UPGRADE_MIGRATION_GATE",
            "downgrade_requested",
            "rollback_before_commit",
            "diagnostic_code",
            "policy_pack_manifest",
        ),
    },
    {
        "surface": "unsupported_feature_evidence",
        "path": "project/tools/release/public_unsupported_feature_matrix.py",
        "tokens": (
            "PUBLIC_UNSUPPORTED_FEATURE_MATRIX",
            "external_provider_required",
            "compile_time_disabled",
            "policy_blocked",
            "runtime_executable=False",
            "authority_claim=False",
        ),
    },
    {
        "surface": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "PUBLIC_LIFECYCLE_POLICY_GATE",
            "public_lifecycle_policy_gate",
            "public_admin_runbook_gate",
            "public_api_abi_compat_gate",
            "public_unsupported_feature_gate",
            "public_upgrade_migration_gate",
            "PCR-GATE-152",
        ),
    },
)

CONTENT_PRIVATE_REFERENCE_SCAN_PATHS = {
    "project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md",
}

PLATFORM_SUPPORT_STATUS_DOCS = {
    "project/docs/release/PUBLIC_SUPPORT_RELEASE_LIFECYCLE.md",
    "docs/build_requirements/README.md",
}

FORBIDDEN_PLATFORM_SUPPORT_OVERCLAIMS = (
    "Supported target with CI/runtime proof",
    "Supported only after runner proof",
    "supported target with CI/runtime proof",
    "supported only after runner proof",
    "All supported platforms must provide",
    "Every supported platform must prove",
)


def fail(message: str) -> None:
    print(f"public_lifecycle_policy_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def read_text(repo_root: Path, relative_path: str) -> str:
    reject_private_reference(relative_path, "source_path")
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{relative_path}")
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{relative_path}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{relative_path}:{exc}")
    if relative_path in CONTENT_PRIVATE_REFERENCE_SCAN_PATHS:
        for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
            if fragment in text:
                fail(f"private_reference_recorded:{relative_path}:{fragment}")
    if relative_path in PLATFORM_SUPPORT_STATUS_DOCS:
        for token in FORBIDDEN_PLATFORM_SUPPORT_OVERCLAIMS:
            if token in text:
                fail(f"platform_support_overclaim:{relative_path}:{token}")
    return text


def validate_check(repo_root: Path, check: dict[str, Any]) -> dict[str, Any]:
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
    records = [validate_check(repo_root, check) for check in CHECKS]
    matrix_text = "\n".join(
        f"{record['surface']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_LIFECYCLE_POLICY_GATE",
        "marker": "PUBLIC_SUPPORT_RELEASE_LIFECYCLE",
        "status": "pass",
        "checks": records,
        "check_count": len(records),
        "matrix_sha256": sha256_text(matrix_text),
        "policy": {
            "release_stage": "core_beta_preview",
            "public_release_evidence_only": True,
            "supported_platforms_are_bounded": True,
            "file_format_upgrade_promises_are_gate_backed": True,
            "security_patch_window_defined": True,
            "unsupported_features_fail_closed": True,
            "legal_ip_final_approval_required_elsewhere": True,
        },
        "authority": {
            "release_lifecycle_policy_is_engine_authority": False,
            "release_lifecycle_policy_is_transaction_authority": False,
            "mga_transaction_inventory_remains_finality_authority": True,
            "cluster_positive_production_claim": False,
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_lifecycle_policy_gate=passed "
        f"checks={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
