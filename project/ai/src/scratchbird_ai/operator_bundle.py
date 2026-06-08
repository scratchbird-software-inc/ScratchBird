# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Operator diagnostics and runbook bundle generation."""

from __future__ import annotations

import json
import platform
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .approval_store import ApprovalLedger
from .scratchbird_core_surface import build_scratchbird_core_surface_packet
from .settings import RuntimeSettings
from .structured_logging import StructuredEventLogger


_RUNTIME_MODE_SETUP = {
    "listener_direct": {
        "bridge_server_setup": "listener-only",
        "transport_mode": "inet_listener",
        "front_door_mode": "direct",
        "required_env": [],
    },
    "manager_proxy": {
        "bridge_server_setup": "managed",
        "transport_mode": "managed",
        "front_door_mode": "manager_proxy",
        "required_env": ["SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN"],
    },
    "local_ipc": {
        "bridge_server_setup": "ipc-only",
        "transport_mode": "local_ipc",
        "front_door_mode": "direct",
        "required_env": [
            "SCRATCHBIRD_AI_BRIDGE_IPC_METHOD",
            "SCRATCHBIRD_AI_BRIDGE_IPC_PATH",
        ],
    },
    "embedded_local_only": {
        "bridge_server_setup": "embedded",
        "transport_mode": "embedded",
        "front_door_mode": "direct",
        "required_env": [],
    },
}


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _configured_attestation_modes(settings: RuntimeSettings) -> list[str]:
    modes = {str(settings.audit_attestation_mode).strip() or "hmac_sha256", "external_reference"}
    if settings.audit_attestation_secret:
        modes.add("hmac_sha256")
    if settings.audit_attestation_delegated_secret:
        modes.add("third_party_hmac_sha256")
    return sorted(modes)


def _runtime_mode_rows(certification_manifest: dict[str, Any]) -> list[dict[str, Any]]:
    packet = certification_manifest.get("scratchbird_core_surface_packet", {})
    truth_packet = packet.get("runtime_mode_truth_packet", {})
    rows = truth_packet.get("admitted_modes", [])
    return [dict(row) for row in rows] if isinstance(rows, list) else []


def build_runtime_diagnostics(
    *,
    settings: RuntimeSettings,
    certification_manifest: dict[str, Any],
    event_logger: StructuredEventLogger,
    approval_ledger: ApprovalLedger,
    max_recent_errors: int = 10,
) -> dict[str, Any]:
    event_summary = event_logger.summarize(max_recent_errors=max_recent_errors)
    approval_summary = approval_ledger.summary()
    runtime_modes = _runtime_mode_rows(certification_manifest) or build_scratchbird_core_surface_packet()[
        "runtime_mode_truth_packet"
    ]["admitted_modes"]
    return {
        "generated_at_utc": _utc_now(),
        "runtime_configuration": {
            "adapter_mode": settings.adapter_mode,
            "http_base_url": settings.http_base_url,
            "http_dialects": list(settings.http_dialects),
            "structured_event_log_path": settings.structured_event_log_path,
            "approval_ledger_path": settings.approval_ledger_path,
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
            "configured_attestation_modes": _configured_attestation_modes(settings),
        },
        "runtime_modes": runtime_modes,
        "event_summary": event_summary,
        "approval_summary": approval_summary,
        "certification_manifest": certification_manifest,
    }


def _build_summary(diagnostics: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    event_summary = diagnostics["event_summary"]
    approval_summary = diagnostics["approval_summary"]
    total_events = int(event_summary.get("total_events", 0) or 0)
    error_rate_pct = float(event_summary.get("error_rate_pct", 0.0) or 0.0)
    p95_duration_ms = float(event_summary.get("p95_duration_ms", 0.0) or 0.0)
    slo_report = {
        "generated_at_utc": diagnostics["generated_at_utc"],
        "service_level_objectives": {
            "request_error_rate_target_pct": 1.0,
            "request_p95_duration_target_ms": 500.0,
        },
        "current_window": {
            "total_events": total_events,
            "error_rate_pct": error_rate_pct,
            "p95_duration_ms": p95_duration_ms,
            "targets_met": {
                "request_error_rate": error_rate_pct <= 1.0,
                "request_p95_duration": p95_duration_ms <= 500.0 if total_events else True,
            },
        },
    }
    summary = {
        "generated_at_utc": diagnostics["generated_at_utc"],
        "status": "PASS"
        if all(slo_report["current_window"]["targets_met"].values())
        else "WARN",
        "log_event_count": total_events,
        "approval_record_count": approval_summary["total_records"],
        "revoked_approval_count": approval_summary["revoked_records"],
        "target_profile_count": len(diagnostics["runtime_configuration"]["operator_target_profiles"]),
    }
    return summary, slo_report


def _dashboard_manifest(
    *,
    diagnostics: dict[str, Any],
    summary: dict[str, Any],
    slo_report: dict[str, Any],
    target_profiles: list[str],
) -> dict[str, Any]:
    return {
        "generated_at_utc": diagnostics["generated_at_utc"],
        "status": summary["status"],
        "cards": [
            {
                "card_id": "events_total",
                "label": "Events",
                "value": diagnostics["event_summary"]["total_events"],
            },
            {
                "card_id": "error_rate_pct",
                "label": "Error Rate %",
                "value": slo_report["current_window"]["error_rate_pct"],
            },
            {
                "card_id": "p95_duration_ms",
                "label": "P95 Duration ms",
                "value": slo_report["current_window"]["p95_duration_ms"],
            },
            {
                "card_id": "approvals_total",
                "label": "Approvals",
                "value": diagnostics["approval_summary"]["total_records"],
            },
        ],
        "target_profiles": list(target_profiles),
        "configured_attestation_modes": list(
            diagnostics["runtime_configuration"]["configured_attestation_modes"]
        ),
    }


def _target_runbook_text(
    *,
    target_profile: str,
    runtime_mode: dict[str, Any],
    setup: dict[str, Any],
    diagnostics: dict[str, Any],
    summary: dict[str, Any],
) -> str:
    required_conditions = runtime_mode.get("required_conditions", [])
    required_env = setup.get("required_env", [])
    lines = [
        f"# ScratchBird AI Operator Runbook: {target_profile}",
        "",
        f"- Generated: {diagnostics['generated_at_utc']}",
        f"- Runtime mode: {runtime_mode.get('mode_id', target_profile)}",
        f"- Transport family: {runtime_mode.get('transport_family', 'unknown')}",
        f"- Bundle status: {summary['status']}",
        "",
        "## Connection Setup",
        "",
        f"- `server_setup`: `{setup['bridge_server_setup']}`",
        f"- `transport_mode`: `{setup['transport_mode']}`",
        f"- `front_door_mode`: `{setup['front_door_mode']}`",
        "",
        "## Preconditions",
        "",
    ]
    if required_conditions:
        for item in required_conditions:
            lines.append(f"- `{item}`")
    else:
        lines.append("- No additional runtime prerequisites declared.")
    lines.extend(
        [
            "",
            "## Required Environment",
            "",
        ]
    )
    if required_env:
        for item in required_env:
            lines.append(f"- `{item}`")
    else:
        lines.append("- No additional environment variables required beyond the base bridge/runtime settings.")
    lines.extend(
        [
            "",
            "## Validation Checklist",
            "",
            "- Run the live native conformance harness for this runtime mode.",
            "- Confirm `summary.json` reports `PASS` or an expected bounded warning only.",
            "- Review `dashboard.json` for event, error-rate, and approval counts.",
            "- Review `recent_errors.json` and investigate any recurring policy or transport failures.",
        ]
    )
    return "\n".join(lines) + "\n"


def _package_target_profiles(
    *,
    output_path: Path,
    diagnostics: dict[str, Any],
    summary: dict[str, Any],
    slo_report: dict[str, Any],
    target_profiles: list[str],
) -> tuple[dict[str, Any], dict[str, Any]]:
    runtime_modes = {
        str(row.get("mode_id", "")).strip(): dict(row)
        for row in diagnostics.get("runtime_modes", [])
        if str(row.get("mode_id", "")).strip()
    }
    packages: dict[str, Any] = {}
    target_root = output_path / "targets"
    target_root.mkdir(parents=True, exist_ok=True)
    for target_profile in target_profiles:
        runtime_mode = runtime_modes.get(
            target_profile,
            {
                "mode_id": target_profile,
                "transport_family": "unknown",
                "support_state": "implemented",
                "required_conditions": [],
            },
        )
        setup = _RUNTIME_MODE_SETUP.get(
            target_profile,
            {
                "bridge_server_setup": "listener-only",
                "transport_mode": "inet_listener",
                "front_door_mode": "direct",
                "required_env": [],
            },
        )
        target_dir = target_root / target_profile
        target_dir.mkdir(parents=True, exist_ok=True)
        dashboard = {
            "generated_at_utc": diagnostics["generated_at_utc"],
            "target_profile": target_profile,
            "runtime_mode": runtime_mode,
            "connection_setup": setup,
            "status": summary["status"],
            "metrics": {
                "total_events": diagnostics["event_summary"]["total_events"],
                "error_rate_pct": slo_report["current_window"]["error_rate_pct"],
                "p95_duration_ms": slo_report["current_window"]["p95_duration_ms"],
                "approval_records": diagnostics["approval_summary"]["total_records"],
            },
        }
        target_manifest = {
            "generated_at_utc": diagnostics["generated_at_utc"],
            "target_profile": target_profile,
            "platform_family": platform.system().lower(),
            "runtime_mode": runtime_mode,
            "connection_setup": setup,
            "configured_attestation_modes": diagnostics["runtime_configuration"][
                "configured_attestation_modes"
            ],
            "operator_bundle_status": summary["status"],
        }
        runbook_text = _target_runbook_text(
            target_profile=target_profile,
            runtime_mode=runtime_mode,
            setup=setup,
            diagnostics=diagnostics,
            summary=summary,
        )
        files = {
            "target_manifest": target_dir / "target_manifest.json",
            "dashboard": target_dir / "dashboard.json",
            "runbook": target_dir / "runbook.md",
        }
        files["target_manifest"].write_text(
            json.dumps(target_manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        files["dashboard"].write_text(
            json.dumps(dashboard, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        files["runbook"].write_text(runbook_text, encoding="utf-8")
        packages[target_profile] = {
            "runtime_mode": runtime_mode,
            "connection_setup": setup,
            "files": {name: str(path) for name, path in files.items()},
        }

    package_manifest = {
        "generated_at_utc": diagnostics["generated_at_utc"],
        "status": summary["status"],
        "target_profiles": list(target_profiles),
        "packages": packages,
    }
    return packages, package_manifest


def _general_runbook_text(
    *,
    diagnostics: dict[str, Any],
    summary: dict[str, Any],
    target_profiles: list[str],
) -> str:
    lines = [
        "# ScratchBird AI Operator Bundle",
        "",
        f"- Generated: {diagnostics['generated_at_utc']}",
        f"- Status: {summary['status']}",
        f"- Target profiles: {', '.join(target_profiles)}",
        "",
        "## Primary Actions",
        "",
        "- Review `summary.json` and `dashboard_manifest.json` first.",
        "- Review target-specific `targets/<profile>/runbook.md` for environment details.",
        "- Review `runtime_diagnostics.json` for certification manifest and runtime configuration.",
        "- Review `recent_errors.json` for active errors before promoting a release.",
        "",
        "## Attestation Modes",
        "",
    ]
    for mode in diagnostics["runtime_configuration"]["configured_attestation_modes"]:
        lines.append(f"- `{mode}`")
    return "\n".join(lines) + "\n"


def generate_operator_runbook_bundle(
    *,
    output_dir: str | Path,
    settings: RuntimeSettings,
    certification_manifest: dict[str, Any],
    event_logger: StructuredEventLogger,
    approval_ledger: ApprovalLedger,
    max_recent_errors: int = 10,
    target_profiles: list[str] | None = None,
) -> dict[str, Any]:
    diagnostics = build_runtime_diagnostics(
        settings=settings,
        certification_manifest=certification_manifest,
        event_logger=event_logger,
        approval_ledger=approval_ledger,
        max_recent_errors=max_recent_errors,
    )
    output_path = Path(output_dir).expanduser()
    output_path.mkdir(parents=True, exist_ok=True)

    summary, slo_report = _build_summary(diagnostics)
    selected_targets = [
        str(item).strip()
        for item in (target_profiles or list(settings.operator_target_profiles))
        if str(item).strip()
    ]
    if not selected_targets:
        selected_targets = list(settings.operator_target_profiles)

    dashboard_manifest = _dashboard_manifest(
        diagnostics=diagnostics,
        summary=summary,
        slo_report=slo_report,
        target_profiles=selected_targets,
    )
    packages, package_manifest = _package_target_profiles(
        output_path=output_path,
        diagnostics=diagnostics,
        summary=summary,
        slo_report=slo_report,
        target_profiles=selected_targets,
    )
    general_runbook = _general_runbook_text(
        diagnostics=diagnostics,
        summary=summary,
        target_profiles=selected_targets,
    )

    files = {
        "summary": output_path / "summary.json",
        "runtime_diagnostics": output_path / "runtime_diagnostics.json",
        "slo_report": output_path / "slo_report.json",
        "recent_errors": output_path / "recent_errors.json",
        "approval_summary": output_path / "approval_summary.json",
        "certification_manifest": output_path / "certification_manifest.json",
        "dashboard_manifest": output_path / "dashboard_manifest.json",
        "package_manifest": output_path / "package_manifest.json",
        "runbook": output_path / "runbook.md",
    }
    files["summary"].write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    files["runtime_diagnostics"].write_text(
        json.dumps(diagnostics, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    files["slo_report"].write_text(
        json.dumps(slo_report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    files["recent_errors"].write_text(
        json.dumps(diagnostics["event_summary"]["recent_errors"], indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    files["approval_summary"].write_text(
        json.dumps(diagnostics["approval_summary"], indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    files["certification_manifest"].write_text(
        json.dumps(certification_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    files["dashboard_manifest"].write_text(
        json.dumps(dashboard_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    files["package_manifest"].write_text(
        json.dumps(package_manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    files["runbook"].write_text(general_runbook, encoding="utf-8")

    return {
        "output_dir": str(output_path),
        "status": summary["status"],
        "files": {name: str(path) for name, path in files.items()},
        "summary": summary,
        "slo_report": slo_report,
        "packages": packages,
        "dashboard_manifest": dashboard_manifest,
        "package_manifest": package_manifest,
    }
