#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate claimed interface profiles for release-candidate promotion."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parents[1]
SRC_DIR = ROOT_DIR / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

from scratchbird_ai.interface_profiles import get_interface_profiles  # noqa: E402


DEFAULT_SPEC = "docs/releases/EARLY_BETA_CONFORMANCE_GATES.md"
DEFAULT_OUTPUT = "artifacts/release_candidate/claim_validation.json"
LIVE_NATIVE_DIR = "artifacts/live_native_conformance"
LIVE_NATIVE_SUMMARY = f"{LIVE_NATIVE_DIR}/summary.json"
LIVE_NATIVE_ENVIRONMENT = f"{LIVE_NATIVE_DIR}/environment_manifest.json"
LIVE_NATIVE_JUNIT = f"{LIVE_NATIVE_DIR}/test_report.junit.xml"
FRAMEWORK_PARITY_JSON = "artifacts/ai_conformance/12/framework_parity.json"
FRAMEWORK_PARITY_JUNIT = "artifacts/ai_conformance/12/test_report.junit.xml"
PROVIDER_PARITY_JSON = "artifacts/ai_conformance/13/provider_parity.json"
PROVIDER_PARITY_JUNIT = "artifacts/ai_conformance/13/test_report.junit.xml"
AUDIT_REPLAY_JSON = "artifacts/ai_conformance/09/audit_replay_report.json"
ATTESTATION_JSON = "artifacts/ai_conformance/09/attestation_report.json"
RELEASE_MATRIX_JSON = "artifacts/ai_conformance/11/matrix_status.json"
RELEASE_ENVIRONMENT_JSON = "artifacts/ai_conformance/11/environment_manifest.json"

PROFILE_MIN_LEVEL = {
    "service_internal_v0": "live_native",
    "mcp_local_v0": "live_native",
    "mcp_remote_v0": "live_native",
    "langchain_v0": "framework_parity",
    "llamaindex_v0": "framework_parity",
    "semantic_kernel_v0": "framework_parity",
    "provider_tool_calling_v0": "provider_parity",
    "streaming_async_v0": "live_native",
    "retrieval_ingest_v0": "live_native",
    "governance_certification_v0": "release_candidate",
}


@dataclass(slots=True)
class ClaimResult:
    profile_id: str
    required_level: str | None
    passed: bool
    checks: list[str]
    errors: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "profile_id": self.profile_id,
            "required_level": self.required_level,
            "passed": self.passed,
            "checks": self.checks,
            "errors": self.errors,
        }


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


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
        return "uncommitted"
    return result.stdout.strip() or "uncommitted"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".")
    parser.add_argument(
        "--artifact-root",
        default=None,
        help=(
            "Directory containing generated artifact families. When supplied, "
            "relative paths beginning with artifacts/ are resolved below this root."
        ),
    )
    parser.add_argument("--spec", default=DEFAULT_SPEC)
    parser.add_argument(
        "--claim-profile",
        action="append",
        default=[],
        help="Claimed interface profile id. May be supplied multiple times. Defaults to every implemented profile.",
    )
    parser.add_argument("--output-json", default=DEFAULT_OUTPUT)
    parser.add_argument(
        "--release-time-utc",
        default=None,
        help="Optional fixed UTC release-evaluation time passed to evidence-gate validation.",
    )
    return parser.parse_args()


def _resolve_artifact_path(repo_root: Path, artifact_root: Path | None, rel_path: str) -> Path:
    candidate = Path(rel_path)
    if candidate.is_absolute():
        return candidate
    if artifact_root is not None:
        parts = candidate.parts
        if parts and parts[0] == "artifacts":
            return artifact_root.joinpath(*parts[1:])
    return repo_root / candidate


def _load_json(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError(f"{path}: expected JSON object")
    return payload


def _require_pass_artifact(path: Path) -> tuple[dict[str, Any] | None, list[str]]:
    errors: list[str] = []
    if not path.exists():
        return None, [f"missing artifact {path}"]
    try:
        payload = _load_json(path)
    except Exception as exc:
        return None, [f"invalid JSON artifact {path}: {exc}"]
    if payload.get("status") != "PASS":
        errors.append(f"artifact {path} must have status=PASS")
    if payload.get("failed_checks") not in ([], None):
        if payload.get("failed_checks") != []:
            errors.append(f"artifact {path} has failed_checks={payload.get('failed_checks')}")
    git_commit = payload.get("git_commit")
    if not isinstance(git_commit, str) or not git_commit:
        errors.append(f"artifact {path} missing git_commit")
    return payload, errors


def _require_junit(path: Path) -> list[str]:
    if not path.exists():
        return [f"missing artifact {path}"]
    try:
        tree = ET.parse(path)
    except Exception as exc:
        return [f"invalid JUnit XML {path}: {exc}"]
    if not tree.findall(".//testcase"):
        return [f"JUnit XML {path} must contain at least one testcase"]
    return []


def _as_string_list(value: Any) -> list[str]:
    if isinstance(value, list):
        return [str(item) for item in value if str(item).strip()]
    if isinstance(value, str) and value.strip():
        return [value.strip()]
    return []


def _run_evidence_gate_validator(
    repo_root: Path,
    artifact_root: Path | None,
    spec: str,
    *,
    release_time_utc: str | None = None,
) -> tuple[bool, str]:
    env = os.environ.copy()
    existing = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = str(SRC_DIR) if not existing else str(SRC_DIR) + os.pathsep + existing
    tool = ROOT_DIR / "tools" / "validate_evidence_gates.py"
    command = [sys.executable, str(tool), "--repo-root", str(repo_root), "--spec", spec]
    if artifact_root is not None:
        command.extend(["--artifact-root", str(artifact_root)])
    if release_time_utc:
        command.extend(["--release-time-utc", release_time_utc])
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    output = (result.stdout + result.stderr).strip()
    return result.returncode == 0, output


def _implemented_profiles() -> dict[str, dict[str, Any]]:
    return {profile["profile_id"]: profile for profile in get_interface_profiles()}


def _validate_live_native_claim(repo_root: Path, artifact_root: Path | None, profile_id: str) -> ClaimResult:
    checks: list[str] = []
    errors: list[str] = []
    summary_path = _resolve_artifact_path(repo_root, artifact_root, LIVE_NATIVE_SUMMARY)
    manifest_path = _resolve_artifact_path(repo_root, artifact_root, LIVE_NATIVE_ENVIRONMENT)
    junit_path = _resolve_artifact_path(repo_root, artifact_root, LIVE_NATIVE_JUNIT)

    summary, summary_errors = _require_pass_artifact(summary_path)
    manifest, manifest_errors = _require_pass_artifact(manifest_path)
    errors.extend(summary_errors)
    errors.extend(manifest_errors)
    errors.extend(_require_junit(junit_path))

    if summary is not None:
        checks.append("live_native_summary_present")
        if summary.get("certification_level") != "live_native":
            errors.append("live-native summary must declare certification_level=live_native")
        covered_profiles = _as_string_list(summary.get("covered_interface_profiles"))
        if not covered_profiles:
            covered_profiles = _as_string_list(summary.get("interface_profile_id"))
        if profile_id not in covered_profiles:
            errors.append(
                f"live-native summary does not cover {profile_id}: covered={covered_profiles}"
            )
        profile_results = summary.get("profile_results")
        if not isinstance(profile_results, list):
            errors.append("live-native summary missing profile_results")
        else:
            matching = [
                row for row in profile_results
                if isinstance(row, dict) and row.get("profile_id") == profile_id
            ]
            if not matching:
                errors.append(f"live-native summary missing profile result for {profile_id}")
            elif matching[0].get("passed") is not True:
                errors.append(f"live-native profile result for {profile_id} did not pass")
    if manifest is not None:
        checks.append("live_native_environment_manifest_present")
        certification_manifest = manifest.get("certification_manifest")
        if not isinstance(certification_manifest, dict):
            errors.append("live-native environment manifest missing certification_manifest")
            certification_manifest = {}
        metadata = certification_manifest.get("live_run_metadata")
        if not isinstance(metadata, dict):
            errors.append("live-native environment manifest missing live_run_metadata")
        else:
            if metadata.get("certification_level") != "live_native":
                errors.append("live-native environment manifest must declare certification_level=live_native")
            covered_profiles = _as_string_list(metadata.get("covered_interface_profiles"))
            if not covered_profiles:
                covered_profiles = _as_string_list(metadata.get("interface_profile_id"))
            if profile_id not in covered_profiles:
                errors.append(
                    f"live-native environment manifest does not cover {profile_id}: covered={covered_profiles}"
                )
    if summary is not None and manifest is not None:
        if summary.get("git_commit") != manifest.get("git_commit"):
            errors.append("live-native artifacts must share the same git_commit")
        else:
            checks.append("live_native_git_commit_match")
    return ClaimResult(
        profile_id=profile_id,
        required_level="live_native",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def _validate_framework_claim(repo_root: Path, artifact_root: Path | None, profile_id: str) -> ClaimResult:
    checks: list[str] = []
    errors: list[str] = []
    payload, payload_errors = _require_pass_artifact(
        _resolve_artifact_path(repo_root, artifact_root, FRAMEWORK_PARITY_JSON)
    )
    errors.extend(payload_errors)
    errors.extend(
        _require_junit(_resolve_artifact_path(repo_root, artifact_root, FRAMEWORK_PARITY_JUNIT))
    )
    if payload is not None:
        checks.append("framework_parity_artifact_present")
        covered = payload.get("interface_profiles") or payload.get("profiles") or []
        if profile_id not in covered:
            errors.append(f"framework parity artifact does not cover {profile_id}")
        else:
            checks.append("framework_profile_covered")
    return ClaimResult(
        profile_id=profile_id,
        required_level="framework_parity",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def _validate_provider_claim(repo_root: Path, artifact_root: Path | None, profile_id: str) -> ClaimResult:
    checks: list[str] = []
    errors: list[str] = []
    payload, payload_errors = _require_pass_artifact(
        _resolve_artifact_path(repo_root, artifact_root, PROVIDER_PARITY_JSON)
    )
    errors.extend(payload_errors)
    errors.extend(
        _require_junit(_resolve_artifact_path(repo_root, artifact_root, PROVIDER_PARITY_JUNIT))
    )
    if payload is not None:
        checks.append("provider_parity_artifact_present")
        declared = payload.get("interface_profile_id")
        if declared not in (None, profile_id):
            errors.append(
                f"provider parity artifact interface_profile_id mismatch: expected {profile_id}, got {declared}"
            )
        child_profiles = payload.get("provider_payload_profiles") or payload.get("profiles") or []
        if not isinstance(child_profiles, list) or len(child_profiles) < 1:
            errors.append("provider parity artifact missing covered provider payload profiles")
        else:
            checks.append("provider_payload_profiles_present")
    return ClaimResult(
        profile_id=profile_id,
        required_level="provider_parity",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def _validate_governance_release_candidate_claim(repo_root: Path, artifact_root: Path | None, profile_id: str) -> ClaimResult:
    checks: list[str] = []
    errors: list[str] = []
    audit_replay, audit_errors = _require_pass_artifact(
        _resolve_artifact_path(repo_root, artifact_root, AUDIT_REPLAY_JSON)
    )
    attestation, attestation_errors = _require_pass_artifact(
        _resolve_artifact_path(repo_root, artifact_root, ATTESTATION_JSON)
    )
    matrix_status, matrix_errors = _require_pass_artifact(
        _resolve_artifact_path(repo_root, artifact_root, RELEASE_MATRIX_JSON)
    )
    environment_manifest, environment_errors = _require_pass_artifact(
        _resolve_artifact_path(repo_root, artifact_root, RELEASE_ENVIRONMENT_JSON)
    )
    errors.extend(audit_errors)
    errors.extend(attestation_errors)
    errors.extend(matrix_errors)
    errors.extend(environment_errors)

    if audit_replay is not None:
        checks.append("audit_replay_present")
    if attestation is not None:
        checks.append("attestation_present")
    if matrix_status is not None:
        checks.append("release_matrix_present")
    if environment_manifest is not None:
        checks.append("release_environment_present")
        certification_manifest = environment_manifest.get("certification_manifest")
        if not isinstance(certification_manifest, dict):
            errors.append("release environment manifest missing certification_manifest")
        else:
            compatibility_manifest = certification_manifest.get("compatibility_manifest")
            if not isinstance(compatibility_manifest, dict):
                errors.append("release environment manifest missing compatibility_manifest")
            else:
                profiles = compatibility_manifest.get("interface_profiles")
                if not isinstance(profiles, list):
                    errors.append("release environment manifest missing interface_profiles")
                else:
                    matching = [
                        row for row in profiles
                        if isinstance(row, dict) and row.get("component") == profile_id
                    ]
                    if not matching:
                        errors.append(f"release environment manifest does not list {profile_id}")
                    elif matching[0].get("support_state") != "supported":
                        errors.append(f"release environment manifest does not mark {profile_id} supported")
                    else:
                        checks.append("governance_profile_supported_in_manifest")

    git_commits = {
        payload.get("git_commit")
        for payload in (audit_replay, attestation, matrix_status, environment_manifest)
        if isinstance(payload, dict)
    }
    if len(git_commits) > 1:
        errors.append("governance release-candidate artifacts must share the same git_commit")
    elif git_commits:
        checks.append("governance_git_commit_match")

    return ClaimResult(
        profile_id=profile_id,
        required_level="release_candidate",
        passed=not errors,
        checks=checks,
        errors=errors,
    )


def validate_claimed_profiles(
    repo_root: Path,
    artifact_root: Path | None,
    claimed_profiles: list[str],
) -> tuple[list[ClaimResult], list[str]]:
    profiles = _implemented_profiles()
    errors: list[str] = []
    results: list[ClaimResult] = []

    for profile_id in claimed_profiles:
        descriptor = profiles.get(profile_id)
        if descriptor is None:
            results.append(
                ClaimResult(
                    profile_id=profile_id,
                    required_level=None,
                    passed=False,
                    checks=[],
                    errors=[f"unknown interface profile {profile_id}"],
                )
            )
            continue
        if descriptor.get("state") != "implemented":
            results.append(
                ClaimResult(
                    profile_id=profile_id,
                    required_level=PROFILE_MIN_LEVEL.get(profile_id),
                    passed=False,
                    checks=[],
                    errors=[f"interface profile {profile_id} is not implemented (state={descriptor.get('state')})"],
                )
            )
            continue

        required_level = PROFILE_MIN_LEVEL.get(profile_id)
        if required_level is None:
            results.append(
                ClaimResult(
                    profile_id=profile_id,
                    required_level=None,
                    passed=False,
                    checks=[],
                    errors=[f"no minimum certification level registered for {profile_id}"],
                )
            )
            continue

        if required_level == "live_native":
            results.append(_validate_live_native_claim(repo_root, artifact_root, profile_id))
        elif required_level == "framework_parity":
            results.append(_validate_framework_claim(repo_root, artifact_root, profile_id))
        elif required_level == "provider_parity":
            results.append(_validate_provider_claim(repo_root, artifact_root, profile_id))
        elif required_level == "release_candidate":
            results.append(
                _validate_governance_release_candidate_claim(repo_root, artifact_root, profile_id)
            )
        else:
            results.append(
                ClaimResult(
                    profile_id=profile_id,
                    required_level=required_level,
                    passed=False,
                    checks=[],
                    errors=[
                        f"no automated artifact validator is defined yet for required_level={required_level}"
                    ],
                )
            )

    for result in results:
        errors.extend(result.errors)
    return results, errors


def build_report(
    *,
    repo_root: Path,
    artifact_root: Path | None,
    claimed_profiles: list[str],
    spec: str,
    release_time_utc: str | None = None,
) -> dict[str, Any]:
    release_gate_ok, release_gate_output = _run_evidence_gate_validator(
        repo_root,
        artifact_root,
        spec,
        release_time_utc=release_time_utc,
    )
    claim_results, claim_errors = validate_claimed_profiles(repo_root, artifact_root, claimed_profiles)
    all_errors = [] if release_gate_ok else [f"release gate validation failed: {release_gate_output}"]
    all_errors.extend(claim_errors)
    check_count = 1 + len(claim_results)
    passed_checks = int(release_gate_ok) + sum(1 for result in claim_results if result.passed)
    return {
        "generated_at_utc": _utc_now(),
        "git_commit": _git_commit(repo_root),
        "status": "PASS" if not all_errors else "FAIL",
        "check_count": check_count,
        "passed_checks": passed_checks,
        "release_gate_validator": {
            "passed": release_gate_ok,
            "output": release_gate_output,
        },
        "claimed_profiles": [result.to_dict() for result in claim_results],
        "failed_checks": all_errors,
    }


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root).resolve() if args.artifact_root else None
    output_path = _resolve_artifact_path(repo_root, artifact_root, args.output_json)

    if args.claim_profile:
        claimed_profiles = args.claim_profile
    else:
        claimed_profiles = [
            profile["profile_id"]
            for profile in get_interface_profiles()
            if profile.get("state") == "implemented"
        ]

    report = build_report(
        repo_root=repo_root,
        artifact_root=artifact_root,
        claimed_profiles=claimed_profiles,
        spec=args.spec,
        release_time_utc=args.release_time_utc,
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if report["status"] != "PASS":
        for item in report["failed_checks"]:
            print(f"ERROR: {item}", file=sys.stderr)
        return 1

    print(
        "OK: release-candidate claims valid "
        f"(profiles={len(claimed_profiles)}, git_commit={report['git_commit']})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
