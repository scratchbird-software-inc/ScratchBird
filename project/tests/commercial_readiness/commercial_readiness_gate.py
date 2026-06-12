#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CRP static/evidence gate for commercial-readiness proof artifacts."""

from __future__ import annotations

import argparse
import csv
import pathlib
import sys


EXECUTION_PLAN = pathlib.Path("docs" "/completed-execution-plans/commercial-readiness-proof-hardening-closure")
ARTIFACTS = EXECUTION_PLAN / "artifacts"

REQUIRED_ARTIFACTS = [
    "COMMERCIAL_CLAIM_BOUNDARY_MATRIX.csv",
    "PRODUCTION_BUILD_GATE_MATRIX.csv",
    "RELEASE_PROVENANCE_MATRIX.csv",
    "SCALE_BENCHMARK_MATRIX.csv",
    "REFERENCE_COMPARISON_COVERAGE_MATRIX.csv",
    "DURABLE_STORAGE_INDEX_PROOF_MATRIX.csv",
    "SECURITY_PROVIDER_INTEGRATION_MATRIX.csv",
    "CLUSTER_PRODUCTION_BLOCK_MATRIX.csv",
    "NOSQL_FUSION_TEST_MATRIX.csv",
    "SUPPORT_BUNDLE_SECURITY_MATRIX.csv",
    "SOAK_FAULT_SECURITY_SUITE_MATRIX.csv",
    "OVERSIZED_SOURCE_REFACTOR_MATRIX.csv",
    "FINAL_COMMERCIAL_READINESS_AUDIT.md",
]


def fail(message: str) -> None:
    print(f"commercial_readiness_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def read_text(repo: pathlib.Path, rel: str) -> str:
    path = repo / rel
    if not path.exists():
        fail(f"missing required file: {rel}")
    if path.is_dir():
        fail(f"expected file but found directory: {rel}")
    return path.read_text(encoding="utf-8")


def require_path(repo: pathlib.Path, rel: str) -> None:
    path = repo / rel
    require(path.exists(), f"missing required path: {rel}")


def read_csv(repo: pathlib.Path, rel: str) -> list[dict[str, str]]:
    path = repo / rel
    if not path.exists():
        fail(f"missing required csv: {rel}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def require_rows_complete(rows: list[dict[str, str]], rel: str) -> None:
    for row in rows:
        status = row.get("status", "")
        ident = next(iter(row.values()), rel)
        require(status == "completed", f"{rel} row {ident!r} status is {status!r}")


def require_artifacts(repo: pathlib.Path) -> None:
    for rel_name in REQUIRED_ARTIFACTS:
        path = repo / ARTIFACTS / rel_name
        require(path.exists(), f"missing CRP artifact: {path}")


def gate_manifest(repo: pathlib.Path) -> None:
    require_artifacts(repo)
    require_rows_complete(read_csv(repo, str(EXECUTION_PLAN / "TRACKER.csv")), "TRACKER.csv")
    require_rows_complete(read_csv(repo, str(EXECUTION_PLAN / "ACCEPTANCE_GATES.csv")), "ACCEPTANCE_GATES.csv")
    require_rows_complete(
        read_csv(repo, str(EXECUTION_PLAN / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv")),
        "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv",
    )
    claim_rows = read_csv(repo, str(ARTIFACTS / "COMMERCIAL_CLAIM_BOUNDARY_MATRIX.csv"))
    required_states = {
        "implemented",
        "compiled",
        "route_consumed",
        "scale_proven",
        "production_claim_status",
    }
    require(claim_rows, "commercial claim matrix must not be empty")
    for row in claim_rows:
        missing = [name for name in required_states if row.get(name, "") == ""]
        require(not missing, f"commercial claim row {row} missing {missing}")


def gate_release(repo: pathlib.Path) -> None:
    rows = read_csv(repo, str(ARTIFACTS / "RELEASE_PROVENANCE_MATRIX.csv"))
    require_rows_complete(rows, "RELEASE_PROVENANCE_MATRIX.csv")
    items = {row["item"] for row in rows}
    for item in [
        "release_profile",
        "package_targets",
        "artifact_signing",
        "sbom",
        "dependency_provenance",
        "release_ledger",
    ]:
        require(item in items, f"release provenance matrix missing {item}")

    cmake = read_text(repo, "project/CMakeLists.txt")
    cmake_gate = read_text(repo, "project/cmake/CommercialReadinessProductionBuildGate.cmake")
    for needle in [
        "commercial_readiness_production_build_cmake_gate",
        "install(TARGETS sb_engine_shared",
        "install(EXPORT ScratchBirdEngineTargets",
    ]:
        require(needle in cmake, f"release gate missing CMake anchor {needle}")
    require(
        "CRP-PRODUCTION-BUILD-SAFETY-GATE" in cmake_gate,
        "release gate missing CRP production build CMake anchor",
    )
    generator = read_text(repo, "project/tools/release_provenance/generate_release_evidence.py")
    for needle in ["sbom_components", "license_inventory", "checksums", "source_provenance"]:
        require(needle in generator, f"release provenance generator missing {needle}")

    build_rows = read_csv(repo, str(ARTIFACTS / "PRODUCTION_BUILD_GATE_MATRIX.csv"))
    require_rows_complete(build_rows, "PRODUCTION_BUILD_GATE_MATRIX.csv")


def gate_benchmark(repo: pathlib.Path) -> None:
    rows = read_csv(repo, str(ARTIFACTS / "SCALE_BENCHMARK_MATRIX.csv"))
    require_rows_complete(rows, "SCALE_BENCHMARK_MATRIX.csv")
    tiers = {row["scale_tier"] for row in rows}
    for tier in ["10k", "100k", "1m", "gb"]:
        require(tier in tiers, f"scale benchmark matrix missing {tier} tier")
    cache_phases = {row["cache_phase"] for row in rows}
    require({"cold", "warm"}.issubset(cache_phases), "benchmark matrix must include cold and warm phases")
    for row in rows:
        for field in ["p50_ms", "p95_ms", "p99_ms", "route_label", "reference_comparison_set"]:
            require(row.get(field), f"benchmark row {row.get('lane_id')} missing {field}")
    references = read_csv(repo, str(ARTIFACTS / "REFERENCE_COMPARISON_COVERAGE_MATRIX.csv"))
    require_rows_complete(references, "REFERENCE_COMPARISON_COVERAGE_MATRIX.csv")
    require(len(references) >= 24, "reference comparison matrix must cover at least 24 reference/reference engines")
    read_text(repo, "project/tools/benchmark_reproducibility.py")
    cmake = read_text(repo, "project/CMakeLists.txt")
    require("benchmark_clean_cmake_instrumentation_flags_gate" in cmake, "benchmark clean CMake gate missing")


def gate_durability(repo: pathlib.Path) -> None:
    rows = read_csv(repo, str(ARTIFACTS / "DURABLE_STORAGE_INDEX_PROOF_MATRIX.csv"))
    require_rows_complete(rows, "DURABLE_STORAGE_INDEX_PROOF_MATRIX.csv")
    families = {row["family"] for row in rows}
    required = {
        "btree",
        "unique_btree",
        "hash",
        "brin_zone",
        "bloom",
        "columnar_zone",
        "document_path",
        "text_inverted_gin_ngram",
        "spatial_gist_spgist",
        "vector_exact_hnsw_ivf",
        "graph",
    }
    require(required.issubset(families), f"durability matrix missing {sorted(required - families)}")
    for rel in [
        "project/src/core/index/index_backup_restore.cpp",
        "project/src/core/index/index_fault_injection_matrix.cpp",
        "project/src/core/index/index_validation_repair_tooling.cpp",
        "project/tests/database_lifecycle/backup_archive_restore_conformance.cpp",
        "project/tests/performance/index_fault_injection_crash_matrix_gate.cpp",
        "project/tests/performance/index_hash_split_merge_compaction_gate.cpp",
    ]:
        read_text(repo, rel)


def gate_cluster(repo: pathlib.Path) -> None:
    rows = read_csv(repo, str(ARTIFACTS / "CLUSTER_PRODUCTION_BLOCK_MATRIX.csv"))
    require_rows_complete(rows, "CLUSTER_PRODUCTION_BLOCK_MATRIX.csv")
    cases = {row["case_id"] for row in rows}
    for case_id in ["no_cluster", "public_stub", "external_provider"]:
        require(case_id in cases, f"cluster block matrix missing {case_id}")
    cmake = read_text(repo, "project/CMakeLists.txt")
    for needle in [
        "SB_CLUSTER_PROVIDER_STUB requires SB_ENABLE_CLUSTER_PROVIDER=ON",
        "Choose either SB_CLUSTER_PROVIDER_EXTERNAL_LIBRARY or SB_CLUSTER_PROVIDER_STUB",
        "SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS",
    ]:
        require(needle in cmake, f"cluster production block missing {needle}")


def gate_security_provider(repo: pathlib.Path) -> None:
    rows = read_csv(repo, str(ARTIFACTS / "SECURITY_PROVIDER_INTEGRATION_MATRIX.csv"))
    require_rows_complete(rows, "SECURITY_PROVIDER_INTEGRATION_MATRIX.csv")
    providers = {row["provider"] for row in rows}
    for provider in [
        "ldap_ad",
        "kerberos",
        "pam",
        "radius",
        "oidc",
        "saml",
        "webauthn_fido2",
        "mtls_revocation",
        "workload_identity",
    ]:
        require(provider in providers, f"security provider matrix missing {provider}")
    for row in rows:
        require(row.get("positive_gate"), f"security provider {row.get('provider')} missing positive gate")
        require(row.get("negative_gate"), f"security provider {row.get('provider')} missing negative gate")
        require(row.get("production_claim_status"), f"security provider {row.get('provider')} missing claim status")
    for rel in [
        "project/tools/sb_auth_plugin_ldap_ad_probe",
        "project/tools/sb_auth_plugin_kerberos_pac_probe",
        "project/tools/sb_auth_plugin_oidc_jwt_probe",
        "project/tools/sb_auth_plugin_radius_probe",
        "project/tools/sb_auth_plugin_saml_probe",
        "project/tools/sb_auth_plugin_webauthn_mfa_probe",
        "project/tools/sb_auth_plugin_workload_identity_probe",
        "project/tools/sb_auth_plugin_certificate_mtls_probe",
    ]:
        require_path(repo, rel)


def gate_nosql_fusion(repo: pathlib.Path) -> None:
    rows = read_csv(repo, str(ARTIFACTS / "NOSQL_FUSION_TEST_MATRIX.csv"))
    require_rows_complete(rows, "NOSQL_FUSION_TEST_MATRIX.csv")
    families = {row["family"] for row in rows}
    for family in ["document", "vector", "search", "graph"]:
        require(family in families, f"NoSQL fusion matrix missing {family}")
    for row in rows:
        for field in ["route_label", "result_equivalence_gate", "mga_security_recheck"]:
            require(row.get(field), f"NoSQL fusion row {row.get('family')} missing {field}")
    for rel in [
        "project/src/engine/internal_api/nosql/document_api.cpp",
        "project/src/engine/internal_api/nosql/vector_api.cpp",
        "project/src/engine/internal_api/nosql/search_api.cpp",
        "project/src/engine/internal_api/nosql/graph_api.cpp",
        "project/src/engine/optimizer/specialized_planner.cpp",
    ]:
        read_text(repo, rel)


def gate_support_bundle(repo: pathlib.Path) -> None:
    rows = read_csv(repo, str(ARTIFACTS / "SUPPORT_BUNDLE_SECURITY_MATRIX.csv"))
    require_rows_complete(rows, "SUPPORT_BUNDLE_SECURITY_MATRIX.csv")
    cases = {row["case_id"] for row in rows}
    for case_id in ["redaction", "tamper_chain", "disclosure_policy"]:
        require(case_id in cases, f"support bundle matrix missing {case_id}")
    for rel in [
        "project/src/core/memory/memory_support_bundle.cpp",
        "project/src/server/server_observability.cpp",
        "project/tests/database_lifecycle/memory_support_bundle_evidence_gate.cpp",
        "project/tests/database_lifecycle/dpc_management_observability_support_bundle_gate.cpp",
        "project/tests/database_lifecycle/agent_evidence_audit_redaction_retention_gate.cpp",
    ]:
        read_text(repo, rel)


def gate_stress_refactor(repo: pathlib.Path) -> None:
    stress = read_csv(repo, str(ARTIFACTS / "SOAK_FAULT_SECURITY_SUITE_MATRIX.csv"))
    require_rows_complete(stress, "SOAK_FAULT_SECURITY_SUITE_MATRIX.csv")
    profiles = {row["suite_id"] for row in stress}
    for suite_id in ["soak_24h", "soak_48h", "soak_72h", "high_concurrency", "crash_fault", "security_negative"]:
        require(suite_id in profiles, f"stress suite matrix missing {suite_id}")
    refactors = read_csv(repo, str(ARTIFACTS / "OVERSIZED_SOURCE_REFACTOR_MATRIX.csv"))
    require_rows_complete(refactors, "OVERSIZED_SOURCE_REFACTOR_MATRIX.csv")
    for row in refactors:
        for field in ["source_path", "search_key_anchor", "equivalence_gate", "refactor_status"]:
            require(row.get(field), f"refactor row {row.get('source_path')} missing {field}")


def gate_final(repo: pathlib.Path) -> None:
    gate_manifest(repo)
    final = read_text(repo, str(ARTIFACTS / "FINAL_COMMERCIAL_READINESS_AUDIT.md"))
    for needle in [
        "CRP-FINAL-COMMERCIAL-READINESS-AUDIT",
        "Status: completed",
        "commercial readiness proof controls are implemented",
        "cluster production claims remain blocked",
    ]:
        require(needle in final, f"final audit missing {needle}")


MODES = {
    "manifest": gate_manifest,
    "release": gate_release,
    "benchmark": gate_benchmark,
    "durability": gate_durability,
    "cluster": gate_cluster,
    "security_provider": gate_security_provider,
    "nosql_fusion": gate_nosql_fusion,
    "support_bundle": gate_support_bundle,
    "stress_refactor": gate_stress_refactor,
    "final": gate_final,
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--mode", required=True, choices=sorted(MODES))
    args = parser.parse_args()

    repo = pathlib.Path(args.repo_root).resolve()
    MODES[args.mode](repo)
    print(f"commercial_readiness_gate=passed mode={args.mode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
