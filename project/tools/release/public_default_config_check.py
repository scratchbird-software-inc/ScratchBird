#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public secure default configuration and profile wiring."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_DEFAULT_CONFIG_CHECK

REQUIRED_TOKENS: dict[str, tuple[str, ...]] = {
    "project/CMakeLists.txt": (
        '_SCRATCHBIRD_ENABLE_DEBUG_LOGS_DEFAULT OFF',
        'CMAKE_BUILD_TYPE STREQUAL "Debug"',
        'SCRATCHBIRD_ENABLE_HOTPATH_TRACE OFF',
        'SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE OFF',
        'SCRATCHBIRD_ENABLE_PREPARED_TRACE OFF',
        'SB_NONCLUSTER_ENGINE_PROFILE "release-complete"',
        'release-complete bootstrap emergency',
        'option(SB_ENABLE_CLUSTER_PROVIDER "Build the engine with a compile-time cluster provider boundary" OFF)',
        'option(SB_CLUSTER_PROVIDER_STUB "Link the public cluster stub provider instead of the no-cluster provider" OFF)',
        'SB_COMMERCIAL_ALLOW_FIXTURE_AUTH_IN_PRODUCTION',
        'SB_COMMERCIAL_ALLOW_STUB_PROVIDERS_IN_PRODUCTION',
        'SB_COMMERCIAL_ALLOW_NO_CLUSTER_PRODUCTION_CLAIMS',
        'SB_COMMERCIAL_CLUSTER_PRODUCTION_CLAIMS',
        'PUBLIC_PRODUCTION_FEATURE_AUDIT',
    ),
    "project/src/server/config.hpp": (
        'std::string security_authority_mode = "database_local"',
        'std::string security_provider_family = "local_password"',
        'bool security_default_policy_installed = true',
        'bool database_auto_create = false',
        'std::string database_open_mode = "normal"',
        'std::string database_daemon_scope = "shared"',
        'bool embedded_direct_mode = false',
        'bool listener_native_enabled = false',
        'std::string listener_native_bind_host = "127.0.0.1"',
        'bool listener_native_tls_required = true',
        'std::string memory_policy_name = "default_local_server_memory_cache_v1"',
        "memory_min_startup_available_bytes",
        "memory_adaptive_page_cache_enabled",
        "memory_index_read_cache_enabled",
        "memory_trim_heap_on_disconnect",
        'memory_failure_mode =',
        'AllocationFailureMode::return_error',
        'bool memory_zero_memory_on_release = true',
    ),
    "project/src/server/config.cpp": (
        "PUBLIC_DEFAULT_CONFIG_CHECK",
        "PublicStartupAuthProviderFamilyAllowed",
        "CanonicalAuthProviderFamily",
        "AuthProviderFamilySupportsAuthn",
        "CONFIG.VALUE_INVALID_ENUM",
        "ValidateServerMemoryPolicy",
        "ApplyDefaultMemoryPolicyFromPolicyPack",
        "config->allow_current_directory = false",
        'config->log_file = "/var/log/scratchbird/sb_server.log"',
    ),
    "project/src/server/lifecycle.cpp": (
        "HasPrivatePermissions",
        "ApplyPrivatePermissions",
        "0700",
        "0600",
        "SERVER.RUNTIME.DIRECTORY_VALIDATION_FAILED",
        "SERVER.RUNTIME.PID_FILE_INVALID",
    ),
    "project/tests/release/CMakeLists.txt": (
        "PUBLIC_DEFAULT_CONFIG_CHECK",
        "PUBLIC_SECURE_DEFAULTS_GATE",
        "public_default_config_check",
        "public_secure_defaults_gate",
        "PCR-GATE-123",
        "PCR-123",
    ),
    "project/tests/release/public_secure_defaults_gate.cpp": (
        "PUBLIC_SECURE_DEFAULTS_GATE",
        "fixture_auth",
        "unsupported_provider",
        "SERVER.RUNTIME.DIRECTORY_VALIDATION_FAILED",
        "SERVER.RUNTIME.PID_FILE_INVALID",
        "default_local_server_memory_cache_v1",
    ),
}

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)


def fail(message: str) -> None:
    print(f"public_default_config_check=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def require_file(repo_root: Path, relative_path: str) -> str:
    path = repo_root / relative_path
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{relative_path}")
    return path.read_text(encoding="utf-8")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    if project_root != repo_root / "project":
        fail("project_root_must_be_repo_project")

    checks: list[dict[str, Any]] = []
    for path_text, tokens in REQUIRED_TOKENS.items():
        reject_private_reference(path_text, "check_path")
        text = require_file(repo_root, path_text)
        for token in tokens:
            require_contains(text, token, path_text)
        checks.append(
            {
                "path": path_text,
                "token_count": len(tokens),
                "sha256": sha256_text(text),
                "status": "secure_default_tokens_present",
            }
        )

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "policy": {
            "fixture_auth_default": "disabled",
            "debug_instrumentation_default": "disabled_outside_debug_builds",
            "test_provider_defaults": "rejected_or_absent",
            "runtime_artifact_permissions": "owner_only",
            "cluster_stub_execution_default": "disabled",
        "implicit_memory_policy": "default_policy_pack_resolved_before_startup",
            "public_tree_inputs_only": True,
        },
        "checks": checks,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_default_config_check_output={output.name}")
    print(f"public_default_config_check_sha256={evidence['evidence_sha256']}")
    print("public_default_config_check=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
