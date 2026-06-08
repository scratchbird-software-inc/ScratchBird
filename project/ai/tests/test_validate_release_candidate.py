# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


def _write_spec(path: Path, rows: list[str]) -> None:
    lines = [
        "# Test Spec",
        "",
        "## 3. Evidence IDs and Gates",
        "",
        "| Evidence ID | Spec | Baseline | Parity | Exceed | Artifacts |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    lines.extend(rows)
    lines.extend(["", "## 4. Next Section", ""])
    path.write_text("\n".join(lines), encoding="utf-8")


def _write_pass_json(path: Path, git_commit: str = "abc123", extra: dict | None = None) -> None:
    payload = {
        "generated_at_utc": "2026-03-07T23:59:00Z",
        "git_commit": git_commit,
        "status": "PASS",
        "check_count": 1,
        "passed_checks": 1,
        "failed_checks": [],
    }
    if extra:
        payload.update(extra)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload), encoding="utf-8")


def _write_junit(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "<?xml version='1.0' encoding='utf-8'?><testsuites><testsuite name='x' tests='1' failures='0' errors='0' skipped='0'><testcase classname='x' name='y' time='0.0'/></testsuite></testsuites>",
        encoding="utf-8",
    )


def _write_live_native_artifacts(
    root: Path,
    *,
    covered_profiles: list[str],
    git_commit: str = "abc123",
) -> None:
    live_root = root / "artifacts" / "live_native_conformance"
    _write_pass_json(
        live_root / "summary.json",
        git_commit=git_commit,
        extra={
            "certification_level": "live_native",
            "interface_profile_id": covered_profiles[0],
            "covered_interface_profiles": covered_profiles,
            "profile_results": [
                {"profile_id": profile_id, "passed": True, "checks": ["probe"], "errors": []}
                for profile_id in covered_profiles
            ],
        },
    )
    _write_pass_json(
        live_root / "environment_manifest.json",
        git_commit=git_commit,
        extra={
            "certification_manifest": {
                "live_run_metadata": {
                    "certification_level": "live_native",
                    "interface_profile_id": covered_profiles[0],
                    "covered_interface_profiles": covered_profiles,
                }
            }
        },
    )
    _write_junit(live_root / "test_report.junit.xml")


def _write_governance_release_artifacts(root: Path, git_commit: str = "abc123") -> None:
    _write_pass_json(root / "artifacts" / "ai_conformance" / "09" / "audit_replay_report.json", git_commit)
    _write_pass_json(root / "artifacts" / "ai_conformance" / "09" / "attestation_report.json", git_commit)
    _write_pass_json(root / "artifacts" / "ai_conformance" / "11" / "matrix_status.json", git_commit)
    _write_pass_json(
        root / "artifacts" / "ai_conformance" / "11" / "environment_manifest.json",
        git_commit,
        extra={
            "certification_manifest": {
                "compatibility_manifest": {
                    "interface_profiles": [
                        {
                            "component": "governance_certification_v0",
                            "support_state": "supported",
                        }
                    ]
                }
            }
        },
    )


class ValidateReleaseCandidateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[1]
        self.tool = self.repo_root / "tools" / "validate_release_candidate.py"

    def _run(self, temp_root: Path, *claims: str) -> subprocess.CompletedProcess[str]:
        args = [
            sys.executable,
            str(self.tool),
            "--repo-root",
            str(temp_root),
            "--spec",
            "test_spec.md",
            "--output-json",
            "claim_report.json",
            "--release-time-utc",
            "2026-03-08T00:00:00Z",
        ]
        for claim in claims:
            args.extend(["--claim-profile", claim])
        return subprocess.run(args, check=False, capture_output=True, text=True)

    def test_provider_claim_passes_with_valid_parity_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_spec(
                root / "test_spec.md",
                [
                    "| `EVID-01` | `01` | `x` | `x` | `x` | `artifacts/ai_conformance/01/summary.json` |",
                ],
            )
            _write_pass_json(root / "artifacts" / "ai_conformance" / "01" / "summary.json")
            _write_pass_json(
                root / "artifacts" / "ai_conformance" / "13" / "provider_parity.json",
                extra={
                    "profiles": [
                        "openai_tool_calling_v0",
                        "anthropic_tool_use_v0",
                        "gemini_function_calling_v0",
                    ]
                },
            )
            _write_junit(root / "artifacts" / "ai_conformance" / "13" / "test_report.junit.xml")

            result = self._run(root, "provider_tool_calling_v0")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("OK: release-candidate claims valid", result.stdout)
            report = json.loads((root / "claim_report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "PASS")
            self.assertEqual(report["claimed_profiles"][0]["profile_id"], "provider_tool_calling_v0")
            self.assertTrue(report["claimed_profiles"][0]["passed"])

    def test_service_internal_claim_fails_without_live_native_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_spec(
                root / "test_spec.md",
                [
                    "| `EVID-01` | `01` | `x` | `x` | `x` | `artifacts/ai_conformance/01/summary.json` |",
                ],
            )
            _write_pass_json(root / "artifacts" / "ai_conformance" / "01" / "summary.json")

            result = self._run(root, "service_internal_v0")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing artifact", result.stderr)
            report = json.loads((root / "claim_report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "FAIL")
            self.assertFalse(report["claimed_profiles"][0]["passed"])

    def test_remote_live_native_claim_passes_when_covered(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_spec(
                root / "test_spec.md",
                [
                    "| `EVID-01` | `01` | `x` | `x` | `x` | `artifacts/ai_conformance/01/summary.json` |",
                ],
            )
            _write_pass_json(root / "artifacts" / "ai_conformance" / "01" / "summary.json")
            _write_live_native_artifacts(
                root,
                covered_profiles=["service_internal_v0", "mcp_remote_v0", "streaming_async_v0"],
            )

            result = self._run(root, "mcp_remote_v0")
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((root / "claim_report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "PASS")
            self.assertTrue(report["claimed_profiles"][0]["passed"])

    def test_governance_release_candidate_claim_passes_with_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_spec(
                root / "test_spec.md",
                [
                    "| `EVID-01` | `01` | `x` | `x` | `x` | `artifacts/ai_conformance/01/summary.json` |",
                    "| `EVID-09` | `09` | `x` | `x` | `x` | `artifacts/ai_conformance/09/audit_replay_report.json`, `artifacts/ai_conformance/09/attestation_report.json` |",
                    "| `EVID-11` | `11` | `x` | `x` | `x` | `artifacts/ai_conformance/11/matrix_status.json`, `artifacts/ai_conformance/11/environment_manifest.json` |",
                ],
            )
            _write_pass_json(root / "artifacts" / "ai_conformance" / "01" / "summary.json")
            _write_governance_release_artifacts(root)

            result = self._run(root, "governance_certification_v0")
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads((root / "claim_report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "PASS")
            self.assertTrue(report["claimed_profiles"][0]["passed"])

    def test_unknown_profile_claim_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write_spec(
                root / "test_spec.md",
                [
                    "| `EVID-01` | `01` | `x` | `x` | `x` | `artifacts/ai_conformance/01/summary.json` |",
                ],
            )
            _write_pass_json(root / "artifacts" / "ai_conformance" / "01" / "summary.json")

            result = self._run(root, "unknown_profile_v0")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unknown interface profile", result.stderr)
            report = json.loads((root / "claim_report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["status"], "FAIL")
            self.assertIn("unknown interface profile", report["claimed_profiles"][0]["errors"][0])


if __name__ == "__main__":
    unittest.main()
