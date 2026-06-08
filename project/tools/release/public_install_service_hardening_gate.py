#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public install/service hardening evidence anchors."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_INSTALL_SERVICE_HARDENING
# PUBLIC_INSTALL_SERVICE_GATE

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

CHECKS: tuple[dict[str, Any], ...] = (
    {
        "surface": "engine_only_install_boundary",
        "path": "project/CMakeLists.txt",
        "required_tokens": (
            "PUBLIC_TARGET_EXPORTS",
            "option(SB_INSTALL_NON_ENGINE_COMPONENTS",
            "Install non-engine listener parser manager driver UDR example and auxiliary artifacts",
            "install(TARGETS sb_engine_shared",
            "EXPORT ScratchBirdEngineTargets",
            "install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/scratchbird/engine",
            "install(EXPORT ScratchBirdEngineTargets",
            "if(SB_INSTALL_NON_ENGINE_COMPONENTS)",
            "COMPONENT non_engine_runtime",
        ),
    },
    {
        "surface": "public_install_consumer_exclusion",
        "path": "project/tools/release/public_install_consumer_gate.py",
        "required_tokens": (
            "REQUIRED_INSTALL_FILES",
            "FORBIDDEN_ENGINE_ONLY_ARTIFACTS",
            "ScratchBird::sb_engine",
            "forbidden_non_engine_artifacts_absent",
            "--install",
        ),
    },
    {
        "surface": "manager_optional_non_engine_install",
        "path": "project/tools/release/public_non_engine_install_gate.py",
        "required_tokens": (
            "public_non_engine_install_gate",
            "SB_INSTALL_NON_ENGINE_COMPONENTS=ON",
            "SB_BUILD_SBMN_MANAGER=ON",
            "non_engine_runtime",
            "bin/sbmn_manager",
            "engine_component_artifacts_absent",
        ),
    },
    {
        "surface": "service_secure_defaults",
        "path": "project/tests/release/public_secure_defaults_gate.cpp",
        "required_tokens": (
            "PUBLIC_SECURE_DEFAULTS_GATE",
            "SecureServiceConfig",
            "mode = service",
            "/var/log/scratchbird/sb_server.log",
            "TestServiceProfileResolution",
            "server_production_default",
        ),
    },
    {
        "surface": "runtime_permission_fail_closed",
        "path": "project/tests/release/public_secure_defaults_gate.cpp",
        "required_tokens": (
            "TestRuntimeArtifactsArePrivate",
            "0700",
            "0600",
            "world-writable control dir",
            "world-writable pid file",
            "SERVER.RUNTIME.DIRECTORY_VALIDATION_FAILED",
            "SERVER.RUNTIME.PID_FILE_INVALID",
        ),
    },
    {
        "surface": "packaged_service_default_paths",
        "path": "project/tests/database_lifecycle/packaging_service_conformance.cpp",
        "required_tokens": (
            "DBLC_P13Q_PACKAGING_SERVICE_COMPLETE",
            "TestPackagedServiceDefaultsDeriveRuntimeAndLogPaths",
            "allow_current_directory",
            "/run/scratchbird",
            "/var/lib/scratchbird",
            "/var/log/scratchbird/sb_server.log",
        ),
    },
    {
        "surface": "packaged_service_startup_ready",
        "path": "project/tests/database_lifecycle/packaging_service_conformance.cpp",
        "required_tokens": (
            "TestStartupArtifactsAreScopedPrivateAndServiceReady",
            "ValidateServerRuntimeArtifacts",
            "EvaluateServerDaemonLifecycle",
            "service_ready",
            "endpoint_descriptors_valid",
            "mkdtemp",
        ),
    },
    {
        "surface": "scoped_uninstall_cleanup",
        "path": "project/tests/database_lifecycle/packaging_service_conformance.cpp",
        "required_tokens": (
            "CleanupServerRuntimeArtifacts",
            "ServerRuntimeCleanupOperation::kUninstall",
            "CleanupManagerRuntimeArtifacts",
            "ManagerRuntimeCleanupOperation::kUninstall",
            "cleanup removed unrelated",
            "RequirePrivateDir",
            "RequirePrivateFile",
        ),
    },
    {
        "surface": "shutdown_clean_and_refusal",
        "path": "project/tests/database_lifecycle/shutdown_conformance.cpp",
        "required_tokens": (
            "TestGracefulShutdownCommitsCleanFinalTransaction",
            "shutdown_database",
            "shutdown_clean",
            "ENGINE.SHUTDOWN_ACK_TIMEOUT",
            "ENGINE.SHUTDOWN_DRAIN_TIMEOUT",
        ),
    },
    {
        "surface": "public_single_node_identity_support",
        "path": "project/cloud/kubernetes/manifests/public-single-node/sample-stack.yaml",
        "required_tokens": (
            "securityRef: workload-identity-local",
            "identityMode: workload_identity",
            "secretless: true",
            "tlsProfileRef: workload-identity-local",
            "publicSingleNodeOnly: true",
            "ScratchBirdSupportBundle",
            "includeTenantData: false",
            "redactionPolicyRef: public-redacted",
        ),
    },
    {
        "surface": "shutdown_fixture_positive",
        "path": "project/cloud/kubernetes/fixtures/public-single-node/database-shutdown.yaml",
        "required_tokens": (
            "lifecycleIntent: shutdown",
            "maintenanceWindow:",
            "redactionPolicyRef: public-redacted",
            "securityRef: workload-identity-local",
        ),
    },
    {
        "surface": "shutdown_fixture_negative",
        "path": "project/cloud/kubernetes/fixtures/public-single-node/database-shutdown-refused.yaml",
        "required_tokens": (
            "lifecycleIntent: shutdown",
            "negative public single-node shutdown missing maintenance window",
            "redactionPolicyRef: public-redacted",
            "securityRef: workload-identity-local",
        ),
        "forbidden_tokens": (
            "maintenanceWindow:",
        ),
    },
    {
        "surface": "manager_runtime_paths_and_support",
        "path": "project/src/manager/node/manager_runtime.cpp",
        "required_tokens": (
            "/etc/scratchbird/sbmn_manager.conf",
            "/run/scratchbird",
            "DefaultManagerControlDir",
            "support-bundles",
            "sbmn_manager.log",
            "ValidateManagerRuntimeArtifacts",
            "CleanupManagerRuntimeArtifacts",
            "manager.control.max_clients",
            "manager.control.max_payload_bytes",
            "MANAGER.CONTROL_MAX_CLIENTS",
            "MANAGER.RELEASE_PROFILE_FORBIDS_LOCAL_TOKEN_STORE",
            "ReleaseProfileAllowsLocalTokenStore",
            "PrivateRegularFile",
            "AddEnterpriseSecretPolicyDiagnostics",
            "MANAGER.SECRET_FILE_UNSAFE",
            "MANAGER.DBBT_KEYRING_FILE_UNSAFE",
            "MANAGER.MCP_SECRET_RIGHTS_REQUIRED",
            "MANAGER.RELEASE_PROFILE_FORBIDS_WILDCARD_SECRET_RIGHT",
            "manager.auth.mcp_secret_rights",
            "McpSecretGrantedRights",
            "ManagerSupportBundleLabelValid",
            "ManagerSupportBundleRedactionProfileValid",
            "MANAGER.SUPPORT_BUNDLE_ARG_INVALID",
            "ManagerConfigReferenceValid",
            "MANAGER.CONFIG_REF_FORBIDDEN",
            "ConfigReferenceResponseValue",
            "ManagerCommandArgsValid",
            "MANAGER.COMMAND_ARGS_INVALID",
            "ManagerIdempotencyKeyValid",
            "MANAGER.IDEMPOTENCY_KEY_INVALID",
            "manager.status",
            "StatusJson",
            "MetricsSnapshotJson",
        ),
    },
    {
        "surface": "manager_enterprise_token_store_profile_fence",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "TestEnterpriseLocalTokenStoreRefused",
            "MANAGER.RELEASE_PROFILE_FORBIDS_LOCAL_TOKEN_STORE",
            "enterprise local token store refusal must not create owner token",
        ),
    },
    {
        "surface": "manager_enterprise_secret_file_safety_proof",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "TestEnterpriseSecretFilePermissionsRefused",
            "TestEnterpriseDbbtKeyringPermissionsRefused",
            "TestEnterpriseMcpSecretRightsRequired",
            "TestEnterpriseMcpSecretWildcardRightsRefused",
            "TestEnterpriseMcpSecretExplicitRightsAuthorization",
            "MANAGER.SECRET_FILE_UNSAFE",
            "MANAGER.DBBT_KEYRING_FILE_UNSAFE",
            "MANAGER.MCP_SECRET_RIGHTS_REQUIRED",
            "MANAGER.RELEASE_PROFILE_FORBIDS_WILDCARD_SECRET_RIGHT",
            "enterprise explicit-rights MCP secret must not inherit support export authority",
        ),
    },
    {
        "surface": "manager_support_bundle_argument_fence",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "ManagerCommandPayloadWithArgs",
            "support bundle must reject newline-injected scope before generation",
            "support bundle invalid redaction profile diagnostic required",
            "support bundle invalid redaction profile must not create bundle root",
            "MANAGER.SUPPORT_BUNDLE_ARG_INVALID",
        ),
    },
    {
        "surface": "manager_config_ref_argument_fence",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "manager.validate_config must reject arbitrary config_ref paths",
            "manager.reload_config must reject relative config_ref paths",
            "manager.validate_config response must redact accepted config_ref",
            "MANAGER.CONFIG_REF_FORBIDDEN",
            "[path-redacted]",
        ),
    },
    {
        "surface": "manager_command_argument_allowlist",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "support bundle must reject duplicate scope arguments",
            "manager.status must reject unknown arguments",
            "listener.stop must reject invalid force argument values",
            "MANAGER.COMMAND_ARGS_INVALID",
            "duplicate_argument",
            "unknown_argument",
            "invalid_value",
        ),
    },
    {
        "surface": "manager_idempotency_key_fence",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "support bundle must reject invalid idempotency key text",
            "support bundle invalid idempotency must not create bundle root",
            "explicit shutdown idempotency key with spaces must fail closed",
            "MANAGER.IDEMPOTENCY_KEY_INVALID",
            "idempotency_key",
            "invalid_character",
        ),
    },
    {
        "surface": "manager_status_command_authorization",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "manager.status",
            "limited management token must allow manager.status",
            "structured manager status json",
        ),
    },
    {
        "surface": "manager_management_pressure_metrics",
        "path": "project/src/manager/node/manager_runtime_snapshot.cpp",
        "required_tokens": (
            "sb_manager_management_clients_active",
            "sb_manager_management_clients_rejected_total",
            "sb_manager_management_requests_total",
            "sb_manager_audit_events_total",
            "sb_manager_audit_bytes",
            "sb_manager_audit_write_failures_total",
            "sb_manager_metrics_publish_failures_total",
            "record_checksum",
            "audit_sequence",
            "config_ref",
            "[path-redacted]",
        ),
    },
    {
        "surface": "manager_durable_audit_metrics_writes",
        "path": "project/src/manager/node/manager_runtime.cpp",
        "required_tokens": (
            "WriteAtomicPrivateText",
            "AppendDurablePrivateText",
            "audit_write_failures_",
            "metrics_publish_failures_",
            "MANAGER.AUDIT_WRITE_FAILED",
        ),
    },
    {
        "surface": "manager_audit_metrics_failure_proof",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "TestAuditMetricsFailureEvidence",
            "metrics_publish_failures",
            "audit_write_failures",
            "MANAGER.AUDIT_WRITE_FAILED",
        ),
    },
    {
        "surface": "manager_service_mode_boundary",
        "path": "project/tests/manager/runtime_integration_tests.cpp",
        "required_tokens": (
            "TestServiceValidateConfigHasNoDaemonSideEffects",
            "config.service = true",
            "config.validate_config = true",
            "MANAGER.SERVICE_MODE_UNSUPPORTED",
            "service validate-config must remove transient owner token",
            "state=stopped",
        ),
    },
    {
        "surface": "manager_platform_service_handoff_policy",
        "path": "project/src/manager/node/manager_runtime.cpp",
        "required_tokens": (
            "DaemonizeService",
            "::fork()",
            "::setsid()",
            "::umask(0027)",
            "::chdir(\"/\")",
            "\"/dev/null\"",
            "Windows service-control handoff is not implemented in this build.",
            "MANAGER.SERVICE_MODE_UNSUPPORTED",
            "config_.service && !config_.validate_config",
        ),
    },
    {
        "surface": "static_cleanup_forbidden_patterns",
        "path": "project/tests/database_lifecycle/packaging_service_static.py",
        "required_tokens": (
            "DBLC_P13Q_PACKAGING_SERVICE_COMPLETE",
            "DBLC_STATIC_RUNTIME_CLEANUP",
            "FORBIDDEN_PATTERNS",
            "std::filesystem::remove_all(config.control_dir",
            "std::filesystem::remove_all(config.data_dir",
            "WAL finality",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_install_service_hardening_gate=fail:{message}", file=sys.stderr)
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


def read_source(path: Path, repo_root: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{path.relative_to(repo_root).as_posix()}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{path.relative_to(repo_root).as_posix()}:{exc}")


def validate_check(repo_root: Path, check: dict[str, Any]) -> dict[str, Any]:
    surface = check.get("surface")
    path_text = check.get("path")
    required_tokens = check.get("required_tokens")
    forbidden_tokens = check.get("forbidden_tokens", ())

    require(isinstance(surface, str) and surface, "surface_invalid")
    require(isinstance(path_text, str) and path_text, f"path_invalid:{surface}")
    reject_private_reference(path_text, f"path:{surface}")
    require(isinstance(required_tokens, tuple) and required_tokens, f"required_tokens_empty:{surface}")
    require(isinstance(forbidden_tokens, tuple), f"forbidden_tokens_invalid:{surface}")

    path = repo_root / path_text
    require(path.is_file(), f"source_missing:{surface}:{path_text}")
    text = read_source(path, repo_root)

    token_digests: list[str] = []
    for token in required_tokens:
        require(isinstance(token, str) and token, f"token_invalid:{surface}:{path_text}")
        if token not in text:
            fail(f"token_missing:{surface}:{path_text}:{token}")
        token_digests.append(sha256_text(token))

    for token in forbidden_tokens:
        require(isinstance(token, str) and token, f"forbidden_token_invalid:{surface}:{path_text}")
        if token in text:
            fail(f"forbidden_token_present:{surface}:{path_text}:{token}")

    return {
        "surface": surface,
        "path": path_text,
        "required_token_count": len(required_tokens),
        "forbidden_token_count": len(forbidden_tokens),
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
                "required_token_count",
                "forbidden_token_count",
                "source_sha256",
                "token_digest_sha256",
                "status",
            ],
        )
        writer.writeheader()
        writer.writerows(records)


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    records = [validate_check(repo_root, check) for check in CHECKS]
    surfaces = {record["surface"] for record in records}
    require(len(surfaces) == len(records), "surface_duplicate")

    write_csv(args.csv_output, records)
    csv_text = args.csv_output.read_text(encoding="utf-8")
    evidence = {
        "schema": "scratchbird.public.install_service_hardening_gate.v1",
        "marker": "PUBLIC_INSTALL_SERVICE_HARDENING",
        "gate_marker": "PUBLIC_INSTALL_SERVICE_GATE",
        "gate": "PCR-GATE-139",
        "status": "pass",
        "check_count": len(records),
        "surface_count": len(surfaces),
        "source_reference_count": len({record["path"] for record in records}),
        "source_token_count": sum(record["required_token_count"] for record in records),
        "forbidden_token_count": sum(record["forbidden_token_count"] for record in records),
        "csv_sha256": sha256_text(csv_text),
        "authority": "public_release_evidence_only",
        "policy": {
            "engine_only_install_boundary": True,
            "non_engine_artifact_exclusion": True,
            "service_defaults_hardened": True,
            "no_world_writable_runtime_paths": True,
            "startup_shutdown_covered": True,
            "scoped_uninstall_cleanup": True,
            "support_bundle_redaction": True,
            "cluster_public_production_claims": False,
            "mga_shutdown_authority_preserved": True,
        },
        "records": records,
    }
    write_evidence(args.evidence_output, evidence)
    print(
        "public_install_service_hardening_gate=passed "
        f"checks={len(records)} "
        f"csv_sha256={evidence['csv_sha256']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
