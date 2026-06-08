#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest gates for the public P1 protected-material/cloud-ops closure."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


FIXTURE_ROOT = Path("project/tests/cloud_ops/fixtures/public_p1_cloud_foundation")


def read(path: Path) -> str:
    if not path.exists():
        raise AssertionError(f"missing required file: {path}")
    return path.read_text(encoding="utf-8")


def require_contains(path: Path, needles: list[str]) -> list[str]:
    text = read(path)
    return [f"{path}: missing {needle!r}" for needle in needles if needle not in text]


def require_regex(path: Path, patterns: list[str]) -> list[str]:
    text = read(path)
    return [f"{path}: missing pattern {pattern!r}" for pattern in patterns if re.search(pattern, text, re.MULTILINE) is None]


def require_no_secret_leak(path: Path) -> list[str]:
    text = read(path)
    errors: list[str] = []
    forbidden = [
        "password =",
        "secret =",
        "static_secret =",
    ]
    for token in forbidden:
        if token in text:
            errors.append(f"{path}: possible unredacted secret marker {token!r}")
    return errors


def target_manifest(repo: Path) -> list[dict[str, str]]:
    path = repo / FIXTURE_ROOT / "artifacts/TARGET_EVIDENCE_MANIFEST.csv"
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def gate_protected_material(repo: Path) -> list[str]:
    errors: list[str] = []
    errors += require_contains(repo / "public_contract_snapshot", [
        "Search key: `SB_SPEC_PROTECTED_MATERIAL_CATALOG_CLOSURE`",
        "protected_material_identity",
        "protected_material_version_identity",
        "retention_policy_uuid",
        "release_policy_uuid",
        "purge_policy_uuid",
        "sys.information",
    ])
    errors += require_contains(repo / "public_contract_snapshot", [
        "protected_material",
        "protected_material_version",
        "retention_policy_uuid",
    ])
    errors += require_contains(repo / "public_contract_snapshot", [
        "sys.information.scratchbird_protected_material",
        "sys.information.scratchbird_protected_material_versions",
        "no raw UUIDs or protected values",
    ])
    errors += require_contains(repo / "project/src/core/catalog/catalog_records.hpp", [
        "protected_material",
        "protected_material_version",
    ])
    errors += require_contains(repo / "project/src/core/catalog/catalog_records.cpp", [
        "protected_material",
        "protected_material_version",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/security/protected_material_api.hpp", [
        "EngineCreateProtectedMaterial",
        "EngineAddProtectedMaterialVersion",
        "EngineResolveProtectedMaterial",
        "EngineReleaseProtectedMaterial",
        "EnginePurgeProtectedMaterialVersion",
        "plaintext_material_returned",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/security/protected_material_api.cpp", [
        "SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED",
        "SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED",
        "protected_material_catalog_create",
        "protected_material_version_add",
    ])
    errors += require_contains(repo / "project/tests/cloud_ops/protected_material_catalog_conformance.cpp", [
        "EngineCreateProtectedMaterial",
        "EngineResolveProtectedMaterial",
        "EnginePurgeProtectedMaterialVersion",
        "SECURITY.PROTECTED_MATERIAL.PLAINTEXT_REFUSED",
    ])
    errors += require_no_secret_leak(repo / "project/src/engine/internal_api/security/protected_material_api.cpp")
    return errors


def gate_cloud_provider(repo: Path) -> list[str]:
    errors: list[str] = []
    errors += require_contains(repo / "public_contract_snapshot", [
        "Search key: `SB_SPEC_CLOUD_PROVIDER_CAPABILITY_REGISTRY`",
        "Search key: `SB_SPEC_CLOUD_DEPLOYMENT_PROFILE_SELECTOR_VALIDATION`",
        "CloudProviderCapabilityProfile",
        "provider_profile_uuid",
        "local_emulator",
        "cluster-only",
        "sys.information",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/cloud/cloud_provider_capability.hpp", [
        "CloudProviderCapabilityProfile",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/cloud/cloud_deployment_profile.hpp", [
        "CloudDeploymentProfile",
        "ValidateCloudDeploymentProfile",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/cloud/cloud_provider_capability.cpp", [
        "SB-CLOUD-CAPABILITY-UNSUPPORTED",
        "SB-CLOUD-CLUSTER-FIELD-REFUSED",
        "local_emulator",
    ])
    errors += require_contains(repo / "project/tests/cloud_ops/cloud_provider_capability_registry_test.cpp", [
        "ValidateCloudDeploymentProfile",
        "kCloudProviderDiagnosticClusterFieldRefused",
    ])
    return errors


def gate_identity_kms(repo: Path) -> list[str]:
    errors: list[str] = []
    errors += require_contains(repo / "public_contract_snapshot", [
        "Search key: `SB_SPEC_CLOUD_IDENTITY_KMS_SECRETLESS_AUTHENTICATION`",
        "Search key: `SB_SPEC_CLOUD_STATIC_SECRET_REFUSAL_POLICY`",
        "workload_identity",
        "oidc_federation",
        "managed_identity",
        "iam_role",
        "service_account_token",
        "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/cloud/cloud_identity_kms.hpp", [
        "CanonicalCloudIdentityMode",
        "CloudKmsEnvelopeMetadata",
        "ValidateCloudIdentityKmsPolicy",
        "plaintext_material_returned",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/cloud/cloud_identity_kms.cpp", [
        "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN",
        "SB_DIAG_CLOUD_KMS_PROFILE_INVALID",
        "protected_material_uuid",
        '"plaintext_material_returned", "false"',
    ])
    errors += require_contains(repo / "project/tests/cloud_ops/cloud_identity_kms_policy_test.cpp", [
        "ValidateCloudIdentityKmsPolicyApi",
        "SB_DIAG_CLOUD_STATIC_SECRET_FORBIDDEN",
        "SB_DIAG_CLOUD_PLAINTEXT_MATERIAL_FORBIDDEN",
    ])
    errors += require_no_secret_leak(repo / "project/src/engine/internal_api/cloud/cloud_identity_kms.cpp")
    return errors


def gate_kubernetes_operator(repo: Path) -> list[str]:
    errors: list[str] = []
    errors += require_contains(repo / "public_contract_snapshot", [
        "Search key: `SB_SPEC_KUBERNETES_OPERATOR_CRDS_LIFECYCLE_AUTOMATION`",
        "Search key: `SB_SPEC_K8S_PUBLIC_SINGLE_NODE_DRY_RUN_AND_IDEMPOTENCY`",
        "ScratchBirdDatabase",
        "ScratchBirdFilespace",
        "ScratchBirdCloudProviderProfile",
        "dry-run",
        "cluster-only",
    ])
    errors += require_contains(repo / "project/cloud/kubernetes/crds/public-single-node-crds.yaml", [
        "ScratchBirdDatabase",
        "ScratchBirdFilespace",
        "ScratchBirdCloudProviderProfile",
        "additionalProperties: false",
    ])
    errors += require_contains(repo / "project/cloud/kubernetes/contracts/public-single-node-operator-contract.yaml", [
        "forbiddenPrivateClusterFields",
        "SB-K8S-CLUSTER-FIELD-REFUSED",
        "SB-K8S-SHUTDOWN-REFUSED",
    ])
    errors += require_contains(repo / "project/cloud/kubernetes/tools/operator_gate.py", [
        "clusterFieldRefused",
        "idempotent",
    ])
    errors += require_contains(repo / "project/tests/cloud_ops/kubernetes_operator_lifecycle_gate.py", [
        "SB-K8S-CLUSTER-FIELD-REFUSED",
        "reconcile output is not deterministic",
    ])
    return errors


def gate_edge_cache(repo: Path) -> list[str]:
    errors: list[str] = []
    errors += require_contains(repo / "public_contract_snapshot", [
        "Search key: `SB_SPEC_EDGE_CACHE_CDN_INTEGRATION_INVALIDATION`",
        "Search key: `SB_SPEC_EDGE_INVALIDATION_AFTER_COMMIT_FINALITY_RULE`",
        "cache tag",
        "after ScratchBird has durable MGA commit evidence",
        "MGA commit",
        "redaction-safe",
        "signed stream",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/cloud/edge_cache_cdn.hpp", [
        "EngineEdgeCacheTagRegistration",
        "EdgeInvalidationRecord",
        "QueueEdgeCacheInvalidationAfterCommit",
        "payload_redacted",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/cloud/edge_cache_cdn.cpp", [
        "SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED",
        "external_effect_only",
    ])
    errors += require_contains(repo / "project/src/engine/internal_api/cloud/external_effect_outbox.cpp", [
        "SB-EDGE-OUTBOX-BACKPRESSURE",
    ])
    errors += require_contains(repo / "project/tests/cloud_ops/edge_cache_cdn_invalidation_conformance.cpp", [
        "QueueEdgeCacheInvalidationAfterCommit",
        "SB-EDGE-PRECOMMIT-INVALIDATION-REFUSED",
        "SB-EDGE-OUTBOX-BACKPRESSURE",
    ])
    errors += require_no_secret_leak(repo / "project/src/engine/internal_api/cloud/edge_cache_cdn.cpp")
    return errors


def gate_management_sblr_abi(repo: Path) -> list[str]:
    errors: list[str] = []
    errors += require_contains(repo / FIXTURE_ROOT / "artifacts/MANAGEMENT_SURFACE_AND_SBLR_OPERATION_MATRIX.md", [
        "protected material",
        "cloud provider",
        "cloud identity",
        "Kubernetes",
        "edge cache",
    ])
    errors += require_contains(repo / FIXTURE_ROOT / "artifacts/PUBLIC_ABI_AND_PACKAGE_MANIFEST_MATRIX.md", [
        "protected_material_catalog_gate",
        "cloud_provider_capability_registry_gate",
        "kubernetes_operator_lifecycle_gate",
    ])
    return errors


def gate_operational_hardening(repo: Path) -> list[str]:
    errors: list[str] = []
    for artifact in [
        "PERSISTED_FORMAT_AND_MIGRATION_POLICY.md",
        "EXTERNAL_EFFECT_OUTBOX_AND_IDEMPOTENCY_POLICY.md",
        "RESOURCE_LIMITS_AND_BACKPRESSURE_POLICY.md",
        "CONFORMANCE_MANIFEST_AND_RELEASE_GATE_RECORDS.md",
        "ADMIN_DOCS_AND_SAMPLE_FLOW.md",
    ]:
        errors += require_regex(repo / FIXTURE_ROOT / f"artifacts/{artifact}", [
            r"(?i)protected",
            r"(?i)cloud|kubernetes|edge|kms",
        ])
    errors += require_contains(repo / "public_contract_snapshot", [
        "protected_material_catalog_gate",
        "cloud_provider_capability_registry_gate",
        "cloud_identity_kms_secretless_gate",
        "kubernetes_operator_lifecycle_gate",
        "edge_cache_cdn_invalidation_gate",
        "public_p1_cloud_foundation_full_route_gate",
    ])
    errors += require_contains(repo / "public_contract_snapshot", [
        "SB-PUBLIC-GAP-0008",
        "SB-PUBLIC-GAP-0168",
        "SB-PUBLIC-GAP-0169",
        "SB-PUBLIC-GAP-0170",
        "SB-PUBLIC-GAP-0173",
        "public_p1_cloud_foundation",
    ])
    return errors


def gate_full_route(repo: Path) -> list[str]:
    errors: list[str] = []
    errors += gate_protected_material(repo)
    errors += gate_cloud_provider(repo)
    errors += gate_identity_kms(repo)
    errors += gate_kubernetes_operator(repo)
    errors += gate_edge_cache(repo)
    errors += gate_management_sblr_abi(repo)
    errors += gate_operational_hardening(repo)
    manifest = target_manifest(repo)
    for row in manifest:
        if row.get("status") != "implemented_in_full":
            errors.append(f"{row.get('gap_id')}: target evidence status is {row.get('status')!r}")
    declaration = repo / FIXTURE_ROOT / "artifacts/PUBLIC_P1_CLOUD_FOUNDATION_RELEASE_DECLARATION.json"
    data = json.loads(read(declaration))
    if data.get("public_open_entries_after_closure") != 24:
        errors.append("release declaration does not record public_open_entries_after_closure=24")
    return errors


MODES = {
    "protected-material": gate_protected_material,
    "cloud-provider": gate_cloud_provider,
    "identity-kms": gate_identity_kms,
    "kubernetes-operator": gate_kubernetes_operator,
    "edge-cache": gate_edge_cache,
    "management-sblr-abi": gate_management_sblr_abi,
    "operational-hardening": gate_operational_hardening,
    "full-route": gate_full_route,
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--mode", choices=sorted(MODES), required=True)
    args = parser.parse_args(argv)

    try:
        errors = MODES[args.mode](args.repo_root)
    except AssertionError as exc:
        errors = [str(exc)]

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
