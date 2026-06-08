# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Environment and certification manifest helpers for release packaging."""

from __future__ import annotations

import platform
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .compatibility import SERVICE_RELEASE_VERSION
from .interface_profiles import INTERFACE_COMPATIBILITY_VERSION, get_interface_profiles
from .provider_profiles import get_provider_profiles
from .scratchbird_core_surface import build_scratchbird_core_surface_packet
from .settings import RuntimeSettings


CERTIFICATION_MANIFEST_VERSION = INTERFACE_COMPATIBILITY_VERSION
RELEASE_TRACK = "early_beta"


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _git_commit(repo_root: Path) -> str:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=repo_root,
            check=True,
            capture_output=True,
            text=True,
        )
    except Exception:
        return "unknown"
    return result.stdout.strip() or "unknown"


def build_certification_manifest(
    *,
    settings: RuntimeSettings,
    adapter_mode: str,
    matrix_version: str,
    compatibility_manifest: dict[str, Any],
) -> dict[str, Any]:
    repo_root = _repo_root()
    interface_profiles = get_interface_profiles()
    provider_profiles = get_provider_profiles()
    supported_interfaces = [
        profile["profile_id"] for profile in interface_profiles if profile["state"] == "implemented"
    ]
    supported_providers = [
        profile["profile_id"] for profile in provider_profiles if profile["state"] == "implemented"
    ]
    scratchbird_core_surface = build_scratchbird_core_surface_packet()

    return {
        "manifest_version": CERTIFICATION_MANIFEST_VERSION,
        "generated_at_utc": _utc_now(),
        "git_commit": _git_commit(repo_root),
        "release_version": SERVICE_RELEASE_VERSION,
        "release_track": RELEASE_TRACK,
        "compatibility_version": INTERFACE_COMPATIBILITY_VERSION,
        "adapter_mode": adapter_mode,
        "matrix_version": matrix_version,
        "environment_descriptor": {
            "python_version": platform.python_version(),
            "python_implementation": platform.python_implementation(),
            "platform": platform.platform(),
            "system": platform.system(),
            "machine": platform.machine(),
        },
        "runtime_configuration": {
            "http_base_url": settings.http_base_url,
            "http_timeout_sec": settings.http_timeout_sec,
            "http_retry_attempts": settings.http_retry_attempts,
            "http_retry_backoff_ms": settings.http_retry_backoff_ms,
            "http_circuit_breaker_failure_threshold": settings.http_circuit_breaker_failure_threshold,
            "http_circuit_breaker_cooldown_sec": settings.http_circuit_breaker_cooldown_sec,
            "http_dialects": list(settings.http_dialects),
            "http_api_token_present": bool(settings.http_api_token),
            "retrieval_catalog_path": settings.retrieval_catalog_path,
            "approval_ledger_path": settings.approval_ledger_path,
            "structured_event_log_path": settings.structured_event_log_path,
            "operator_bundle_output_dir": settings.operator_bundle_output_dir,
            "operator_target_profiles": list(settings.operator_target_profiles),
            "audit_attestation_mode": settings.audit_attestation_mode,
            "audit_attestation_secret_present": bool(settings.audit_attestation_secret),
            "audit_attestation_attestor_id": settings.audit_attestation_attestor_id,
            "audit_attestation_delegated_secret_present": bool(
                settings.audit_attestation_delegated_secret
            ),
            "audit_attestation_delegated_attestor_id": (
                settings.audit_attestation_delegated_attestor_id
            ),
            "audit_attestation_external_reference_base_url": (
                settings.audit_attestation_external_reference_base_url
            ),
            "compile_repair_max_attempts": settings.compile_repair_max_attempts,
            "operation_window_sec": settings.operation_window_sec,
            "max_requests_per_window": settings.max_requests_per_window,
            "max_mutations_per_window": settings.max_mutations_per_window,
            "max_cost_units_per_window": settings.max_cost_units_per_window,
            "supported_server_versions": list(settings.supported_server_versions),
            "supported_parser_compiler_versions": list(
                settings.supported_parser_compiler_versions
            ),
            "supported_driver_runtime_versions": list(
                settings.supported_driver_runtime_versions
            ),
            "remote_mcp_auth_token_present": bool(settings.remote_mcp_auth_token),
            "remote_mcp_supported_auth_types": list(settings.remote_mcp_supported_auth_types),
            "remote_mcp_session_ttl_sec": settings.remote_mcp_session_ttl_sec,
            "remote_mcp_heartbeat_interval_sec": settings.remote_mcp_heartbeat_interval_sec,
            "remote_mcp_protocol_versions": list(settings.remote_mcp_protocol_versions),
            "remote_mcp_supported_transports": list(settings.remote_mcp_supported_transports),
        },
        "live_target_descriptor": {
            "http_bridge_enabled": adapter_mode in {"http", "hybrid"},
            "http_bridge_runtime_supported": True,
            "remote_mcp_supported": True,
            "http_bridge_base_url": settings.http_base_url,
            "remote_mcp_preview_enabled": bool(settings.remote_mcp_supported_transports),
        },
        "supported_interface_profiles": supported_interfaces,
        "supported_provider_profiles": supported_providers,
        "interface_profiles": interface_profiles,
        "provider_profiles": provider_profiles,
        "scratchbird_core_surface_packet": scratchbird_core_surface,
        "compatibility_manifest": compatibility_manifest,
    }
