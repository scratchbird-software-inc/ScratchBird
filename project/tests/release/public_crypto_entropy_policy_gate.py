#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static gate for public crypto, entropy, and protected-material policy."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_CRYPTO_ENTROPY_GATE

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

POLICY_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "central_crypto_policy_anchor",
        "surface": "central_policy",
        "path": "src/engine/internal_api/security/security_crypto_policy.hpp",
        "tokens": (
            "PUBLIC_CRYPTO_POLICY",
            "SecurityConstantTimeEqual",
            "SecuritySha256Hex",
            "SecurityHmacSha256Hex",
            "signature_ready_ed25519",
            "Weak checksums cannot",
        ),
    },
    {
        "row_id": "approved_openssl_primitives",
        "surface": "hash_hmac",
        "path": "src/core/hash/hash_digest.cpp",
        "tokens": (
            "<openssl/evp.h>",
            "<openssl/hmac.h>",
            "EVP_sha256()",
            "EVP_Digest",
            "HMAC(EVP_sha256()",
            "ComputeHmacSha256Digest",
        ),
    },
    {
        "row_id": "production_entropy_identity_source",
        "surface": "entropy",
        "path": "src/core/uuid/uuid.cpp",
        "tokens": (
            "std::random_device random",
            "GenerateEngineIdentityV7",
            "GenerateCompatibilityUnixTimeV7",
        ),
    },
    {
        "row_id": "password_verifier_sha256",
        "surface": "password",
        "path": "src/engine/internal_api/security/authentication_api.cpp",
        "tokens": (
            "local-password-verifier:v1:sha256:",
            "SecuritySha256Hex(verifier)",
            "SecurityConstantTimeEqual",
            "credential_verifier_mismatch",
        ),
    },
    {
        "row_id": "temporary_token_hmac",
        "surface": "token",
        "path": "src/engine/internal_api/security/authentication_api.cpp",
        "tokens": (
            "security_database_temporary_token",
            "security-temporary-token:v1:hmac-sha256:",
            "SecurityHmacSha256Hex(token_handle, payload)",
            "security_database_temporary_token_revoked",
        ),
    },
    {
        "row_id": "protected_material_hmac_handles",
        "surface": "protected_material",
        "path": "src/engine/internal_api/security/protected_material_api.cpp",
        "tokens": (
            "protected-material-handle:v1:hmac-sha256:",
            "protected-material-release:v1:hmac-sha256:",
            "protected-material-audit:v1:sha256:",
            "SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED",
        ),
    },
    {
        "row_id": "security_lifecycle_sha256_fingerprint",
        "surface": "password",
        "path": "src/engine/internal_api/security/security_principal_lifecycle.cpp",
        "tokens": (
            "StableToken(",
            "SecuritySha256Hex(payload)",
            "protected_material_redacted",
        ),
    },
    {
        "row_id": "cluster_evidence_signature_ready",
        "surface": "signature_ready",
        "path": "src/engine/internal_api/security/security_crypto_policy.cpp",
        "tokens": (
            "signature-ready-ed25519",
            "SB_CLUSTER_EVIDENCE_INTEGRITY.HMAC_KEY_REQUIRED",
            "SB_CLUSTER_EVIDENCE_INTEGRITY.DIGEST_MISMATCH",
            "provider_authority_claim_allowed",
        ),
    },
    {
        "row_id": "durable_crypto_public_gate",
        "surface": "public_test",
        "path": "tests/release/public_security_durable_crypto_hardening_gate.cpp",
        "tokens": (
            "SHA-256 known answer mismatch",
            "HMAC-SHA-256 known answer mismatch",
            "security_database_temporary_token_not_found",
            "security_database_temporary_token_revoked",
            "credential-fingerprint:v1:sha256:",
        ),
    },
    {
        "row_id": "protected_material_public_gate",
        "surface": "public_test",
        "path": "tests/release/public_security_provider_contract_protected_material_gate.cpp",
        "tokens": (
            "protected-material-handle:v1:hmac-sha256:",
            "protected-material-release:v1:hmac-sha256:",
            "SECURITY.KEY.UNAVAILABLE",
            "SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED",
            "plaintext_material_returned",
        ),
    },
    {
        "row_id": "cluster_crypto_public_gate",
        "surface": "public_test",
        "path": "tests/release/public_cluster_catalog_crypto_evidence_gate.cpp",
        "tokens": (
            "hmac-sha256",
            "signature-ready-ed25519",
            "weak_security",
            "security accepted HMAC evidence without key material",
            "DIGEST_MISMATCH",
        ),
    },
    {
        "row_id": "cross_platform_openssl_linkage",
        "surface": "platform",
        "path": "src/engine/internal_api/CMakeLists.txt",
        "tokens": (
            "find_package(OpenSSL REQUIRED)",
            "OpenSSL::Crypto",
        ),
    },
    {
        "row_id": "release_security_gates_wired",
        "surface": "public_test",
        "path": "tests/release/CMakeLists.txt",
        "tokens": (
            "public_security_durable_crypto_hardening_gate",
            "public_security_provider_contract_protected_material_gate",
            "public_cluster_catalog_crypto_evidence_gate",
            "OpenSSL::Crypto",
        ),
    },
)

REQUIRED_SURFACES = {
    "central_policy",
    "entropy",
    "hash_hmac",
    "password",
    "platform",
    "protected_material",
    "public_test",
    "signature_ready",
    "token",
}

BANNED_POLICY_TOKENS = (
    "std::hash<",
    "fnv",
    "crc32",
    "weak_checksum",
)


def fail(message: str) -> None:
    print(f"public_crypto_entropy_policy_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_project_file(project_root: Path, relative: str) -> str:
    reject_private_reference(relative, "policy_path")
    path = project_root / relative
    if not path.is_file():
        fail(f"missing_file:{relative}")
    return path.read_text(encoding="utf-8")


def validate_rows(project_root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    seen: set[str] = set()
    surfaces: set[str] = set()
    for row in POLICY_ROWS:
        row_id = row["row_id"]
        if row_id in seen:
            fail(f"duplicate_row:{row_id}")
        seen.add(row_id)
        reject_private_reference(row_id, "row_id")
        surface = row["surface"]
        surfaces.add(surface)
        text = read_project_file(project_root, row["path"])
        missing = [token for token in row["tokens"] if token not in text]
        if missing:
            fail(f"missing_tokens:{row['path']}:{','.join(missing)}")
        rows.append(
            {
                "row_id": row_id,
                "surface": surface,
                "path": row["path"],
                "token_count": len(row["tokens"]),
                "source_sha256": sha256_text(text),
                "status": "present",
            }
        )
    missing_surfaces = sorted(REQUIRED_SURFACES - surfaces)
    if missing_surfaces:
        fail("missing_surfaces:" + ",".join(missing_surfaces))

    policy_text = read_project_file(
        project_root,
        "src/engine/internal_api/security/security_crypto_policy.cpp",
    )
    for banned in BANNED_POLICY_TOKENS:
        if banned in policy_text:
            fail(f"banned_crypto_policy_token:{banned}")
    return rows


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    project_root = args.project_root.resolve()
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")
    rows = validate_rows(project_root)
    evidence: dict[str, Any] = {
        "schema_version": 1,
        "gate": "PCR-GATE-122",
        "marker": "PUBLIC_CRYPTO_ENTROPY_GATE",
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "approved_hash": "sha256",
            "approved_mac": "hmac-sha256",
            "signature_ready_metadata": "ed25519_envelope_ready",
            "protected_material_plaintext_returned": False,
            "weak_checksums_authority": False,
            "release_proof_is_evidence_only": True,
        },
        "row_count": len(rows),
        "rows": rows,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(f"public_crypto_entropy_policy_rows={evidence['row_count']}")
    print(f"public_crypto_entropy_policy_sha256={evidence['evidence_sha256']}")
    print("public_crypto_entropy_policy_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
