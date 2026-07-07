#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate AI MCP first-class release integration.

This gate is intentionally deterministic and does not require a live server.
It proves that release promotion cannot rely on mock, optional, unconfigured,
or locally-authoritative SQL execution paths.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from scratchbird_ai.language_resources import LanguageResourcePack  # noqa: E402
from scratchbird_ai.remote_sessions import RemoteSessionManager  # noqa: E402
from scratchbird_ai.service import build_default_service  # noqa: E402
from scratchbird_ai.settings import RuntimeSettings  # noqa: E402
from scratchbird_ai.sblr_artifacts import TEST_ONLY_MOCK, validate_prepared_artifact  # noqa: E402
from scratchbird_ai.tool_schema import get_tool_descriptors  # noqa: E402


COMPONENT_ID = "adaptor:scratchbird-ai-mcp"
REQUIRED_TOOLS = {
    "compile_query",
    "execute_compiled",
    "get_sbsql_language_resource_manifest",
    "list_sbsql_language_profiles",
    "get_sbsql_predictive_grammar",
    "get_metadata_resolution_contract",
    "generate_ai_mcp_support_bundle",
}


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def load_json(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return payload


def check_manifest(public_root: Path, errors: list[str]) -> None:
    manifest = public_root / "project" / "drivers" / "DriverPackageManifest.csv"
    rows = {row.get("component_id", ""): row for row in read_csv(manifest)}
    row = rows.get(COMPONENT_ID)
    if row is None:
        errors.append(f"{COMPONENT_ID} missing from DriverPackageManifest.csv")
        return
    expected = {
        "category": "adaptor",
        "name": "scratchbird-ai-mcp",
        "driver_family": "ai_mcp",
        "driver_status": "beta_2",
        "release_bucket": "release_candidate",
        "conformance_profile_ref": "ai_mcp_final_zero_drift_audit",
        "source_path": "project/ai",
    }
    for key, value in expected.items():
        if row.get(key) != value:
            errors.append(f"DriverPackageManifest {COMPONENT_ID} {key} must be {value}")
    for key, token in {
        "wire_protocol_set": "sbwp_v1_1",
        "auth_method_set": "engine_local_password",
        "tls_profile_set": "scratchbird_tls_1_3_floor",
        "metadata_profile": "sys_information_recursive",
    }.items():
        if token not in str(row.get(key, "")):
            errors.append(f"DriverPackageManifest {COMPONENT_ID} {key} missing {token}")


def check_package_contract(public_root: Path, errors: list[str]) -> None:
    contract_path = public_root / "project" / "ai" / "package_contract.json"
    if not contract_path.is_file():
        errors.append("AI MCP package_contract.json missing")
        return
    contract = load_json(contract_path)
    for key, value in {
        "component_id": COMPONENT_ID,
        "category": "adaptor",
        "name": "scratchbird-ai-mcp",
        "driver_family": "ai_mcp",
        "status": "beta_2",
        "wire_protocol": "sbwp_v1_1",
        "auth_authority": "engine",
        "transaction_authority": "mga_engine",
        "server_revalidation_required": True,
    }.items():
        if contract.get(key) != value:
            errors.append(f"package_contract {key} must be {value!r}")
    for field in ("route_requirements", "conformance", "release_readiness", "package_files"):
        if not contract.get(field):
            errors.append(f"package_contract missing {field}")
    for rel in contract.get("package_files", []):
        if not (public_root / "project" / "ai" / str(rel)).is_file():
            errors.append(f"package_contract package_files missing {rel}")


def check_dependency_policy(public_root: Path, errors: list[str]) -> None:
    policy_path = public_root / "project" / "ai" / "dependency_policy.json"
    if not policy_path.is_file():
        errors.append("AI MCP dependency_policy.json missing")
        return
    policy = load_json(policy_path)
    if policy.get("component_id") != COMPONENT_ID:
        errors.append("dependency_policy component_id mismatch")
    mcp_release = (
        policy.get("runtime_dependency_groups", {})
        .get("mcp_release", [])
    )
    if not any(row.get("package") == "mcp" and row.get("release_required") is True for row in mcp_release if isinstance(row, dict)):
        errors.append("dependency_policy must mark mcp runtime as release_required")


def check_tool_surface(errors: list[str]) -> None:
    names = {row["tool_name"] for row in get_tool_descriptors()}
    missing = sorted(REQUIRED_TOOLS - names)
    if missing:
        errors.append(f"AI MCP tool catalog missing: {missing}")


def check_language_pack(errors: list[str]) -> None:
    try:
        pack = LanguageResourcePack.load(verify_hashes=False)
        authority = pack.manifest.get("authority", {})
        if authority.get("local_sblr_uuid_streams_are_untrusted") is not True:
            errors.append("language pack does not mark local SBLR/UUID streams untrusted")
        if authority.get("server_revalidates_sblr_uuid_descriptor_authorization_policy_and_mga") is not True:
            errors.append("language pack does not require server SBLR/UUID revalidation")
        expected_tags = {"en-US", "en-CA", "fr-FR", "fr-CA", "de-DE", "it-IT", "es-ES"}
        missing = sorted(expected_tags - set(pack.profile_tags()))
        if missing:
            errors.append(f"language pack missing profiles: {missing}")
    except Exception as exc:
        errors.append(f"language pack validation failed: {exc}")


def check_compile_artifact(errors: list[str]) -> None:
    service = build_default_service(
        settings=RuntimeSettings(
            approval_ledger_path=None,
            structured_event_log_path=None,
            operator_bundle_output_dir=None,
        )
    )
    compiled = service.compile_query(
        dialect="native",
        query_text="SELECT 1",
        context={
            "security_context": {
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "session_id": "sess_a",
                "context_version": 1,
            }
        },
    )
    artifact = compiled.prepared_artifact
    artifact_errors = validate_prepared_artifact(artifact)
    if artifact_errors:
        errors.append(f"prepared artifact invalid: {artifact_errors}")
    if artifact.get("local_sql_execution_authority") is not False:
        errors.append("prepared artifact grants local SQL execution authority")
    if compiled.server_revalidation_state != TEST_ONLY_MOCK:
        errors.append("default mock compile must be explicitly classified as test_only_mock")

    release_service = build_default_service(
        settings=RuntimeSettings(
            require_server_revalidated_artifacts=True,
            approval_ledger_path=None,
            structured_event_log_path=None,
            operator_bundle_output_dir=None,
        )
    )
    release_compiled = release_service.compile_query(
        dialect="native",
        query_text="SELECT 1",
        context={
            "security_context": {
                "tenant_id": "tenant_a",
                "actor_id": "actor_a",
                "session_id": "sess_a",
                "context_version": 1,
            }
        },
    )
    try:
        release_service.execute_compiled(
            compile_artifact_id=release_compiled.compile_artifact_id,
            mode="ai_analysis",
        )
    except Exception as exc:
        if "server-revalidated" not in str(exc):
            errors.append(f"release execution refused with unexpected error: {exc}")
    else:
        errors.append("release execution accepted non-server-revalidated mock artifact")


def check_remote_auth_policy(errors: list[str]) -> None:
    manager = RemoteSessionManager(
        auth_token=None,
        supported_auth_types=("preauthenticated_context",),
        allow_preauthenticated_context=False,
    )
    try:
        manager.open_session(
            {
                "auth_envelope": {
                    "auth_type": "preauthenticated_context",
                    "security_context": {"tenant_id": "t", "actor_id": "a"},
                }
            },
            capability_advertisement={
                "service": "scratchbird-ai",
                "server_id": "srv",
                "capability_manifest_id": "mf",
            },
        )
    except Exception as exc:
        if "preauthenticated_context requires explicit runtime policy admission" not in str(exc):
            errors.append(f"preauth refusal used unexpected error: {exc}")
    else:
        errors.append("preauthenticated_context accepted without explicit policy admission")


def check_release_gate_split(public_root: Path, errors: list[str]) -> None:
    runner = (public_root / "project" / "ai" / "scripts" / "ai_component_runner.py").read_text(
        encoding="utf-8"
    )
    if "--require-mcp-runtime" not in runner:
        errors.append("ai_component_runner missing --require-mcp-runtime release flag")
    if "--require-live-native" not in runner:
        errors.append("ai_component_runner missing --require-live-native release flag")
    cmake = (public_root / "project" / "ai" / "CMakeLists.txt").read_text(encoding="utf-8")
    for token in (
        "SB_AI_REQUIRE_MCP_RUNTIME",
        "SB_AI_REQUIRE_LIVE_NATIVE",
        "ai_mcp_runtime_gate",
        "ai_mcp_live_native_required_gate",
        "--require-mcp-runtime",
        "--require-live-native",
    ):
        if token not in cmake:
            errors.append(f"CMakeLists missing release-required AI MCP token {token}")
    for gate in (
        "ai_mcp_manifest_gate",
        "ai_mcp_sblr_uuid_artifact_gate",
        "ai_mcp_language_resource_gate",
        "ai_mcp_release_integration_gate",
        "ai_mcp_final_zero_drift_audit",
    ):
        if gate not in cmake:
            errors.append(f"CMakeLists missing {gate}")
    presets = (public_root / "project" / "CMakePresets.json").read_text(encoding="utf-8")
    for token in (
        "ai-mcp-release-proof-linux",
        "SB_AI_REQUIRE_MCP_RUNTIME",
        "SB_AI_REQUIRE_LIVE_NATIVE",
    ):
        if token not in presets:
            errors.append(f"CMakePresets missing AI MCP release proof token {token}")


def check_packaging(public_root: Path, errors: list[str]) -> None:
    package_root = public_root / "packaging" / "2026.07.03" / "adapters" / "scratchbird-ai-mcp"
    required_files = (
        "package_manifest.json",
        "SBOM.json",
        "SHA256SUMS",
        "support/README.md",
        "support/package_contract.json",
        "support/source/dependency_policy.json",
        "support/source/package_contract.json",
        "support/source/tools/validate_ai_mcp_release_integration.py",
        "support/support_bundle_manifest.json",
        "examples/README.md",
        "proofs/proof_summary.json",
        "resources/sbsql-language-resource-pack/manifest.sblrp.json",
        "legal/LICENSE.txt",
    )
    for rel in required_files:
        if not (package_root / rel).is_file():
            errors.append(f"AI MCP packaging missing {rel}")
    try:
        manifest = load_json(package_root / "package_manifest.json")
    except (OSError, ValueError, json.JSONDecodeError):
        errors.append("AI MCP package_manifest.json is missing or invalid")
        return
    if manifest.get("component_id") != COMPONENT_ID:
        errors.append("AI MCP package_manifest component_id mismatch")
    if manifest.get("category") != "adaptor":
        errors.append("AI MCP package_manifest category must be adaptor")
    if manifest.get("source_path") != "project/ai":
        errors.append("AI MCP package_manifest source_path must be project/ai")


def check_capability_matrix(public_root: Path, errors: list[str]) -> None:
    matrix = load_json(public_root / "project" / "ai" / "capability" / "capability-matrix.v0.json")
    native = matrix.get("dialects", {}).get("native", {})
    for key in (
        "prepared_sblr_uuid_artifacts",
        "server_revalidation",
        "shared_language_resources",
        "predictive_prompting",
        "authorized_metadata_resolution",
        "resource_budgets",
        "cluster_boundary_fail_closed",
    ):
        if native.get(key) is not True:
            errors.append(f"capability matrix native.{key} must be true")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=str(ROOT_DIR))
    parser.add_argument("--output-json", default="")
    args = parser.parse_args()
    ai_root = Path(args.repo_root).resolve()
    public_root = ai_root.parents[1] if ai_root.name == "ai" else ai_root

    errors: list[str] = []
    check_manifest(public_root, errors)
    check_package_contract(public_root, errors)
    check_dependency_policy(public_root, errors)
    check_tool_surface(errors)
    check_language_pack(errors)
    check_compile_artifact(errors)
    check_remote_auth_policy(errors)
    check_release_gate_split(public_root, errors)
    check_packaging(public_root, errors)
    check_capability_matrix(public_root, errors)

    payload = {
        "schema_id": "scratchbird.ai_mcp_release_integration_gate.v1",
        "status": "PASS" if not errors else "FAIL",
        "component_id": COMPONENT_ID,
        "failed_checks": errors,
        "check_count": 10,
    }
    if args.output_json:
        target = Path(args.output_json)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("OK: AI MCP release integration gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
