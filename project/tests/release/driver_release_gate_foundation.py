#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fast foundation tests for beta driver release gate anchors."""

from __future__ import annotations

import csv
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[3]


MANIFEST_FIELDS = [
    "component_id",
    "category",
    "name",
    "driver_package_uuid",
    "driver_family",
    "driver_status",
    "api_surface_set",
    "ingress_mode_set",
    "wire_protocol_set",
    "dsn_key_set",
    "auth_method_set",
    "tls_profile_set",
    "type_mapping_profile",
    "diagnostic_mapping_profile",
    "metadata_profile",
    "thread_safety_class",
    "pooling_capability",
    "release_bucket",
    "conformance_profile_ref",
    "source_path",
]


def write_csv(path: Path, rows: list[dict[str, str]], fields: list[str] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = fields or list(rows[0])
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def manifest_row(component_id: str, category: str, name: str, family: str, source_path: str) -> dict[str, str]:
    return {
        "component_id": component_id,
        "category": category,
        "name": name,
        "driver_package_uuid": f"019e12a0-test-{name}",
        "driver_family": family,
        "driver_status": "beta_2",
        "api_surface_set": "language_binding",
        "ingress_mode_set": "direct_listener;manager_proxy",
        "wire_protocol_set": "sbwp_v1_1",
        "dsn_key_set": "database;host;port;user;auth_method",
        "auth_method_set": "engine_local_password;scram_ready",
        "tls_profile_set": "scratchbird_tls_1_3_floor",
        "type_mapping_profile": "sbsql_core",
        "diagnostic_mapping_profile": "native_sqlstate",
        "metadata_profile": "sys_information_recursive",
        "thread_safety_class": "thread_safe",
        "pooling_capability": "connection_pool",
        "release_bucket": "release_candidate",
        "conformance_profile_ref": f"{category}_{name}_gate",
        "source_path": source_path,
    }


class DriverReleaseGateFoundationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.workplan = self.root / "workplan"
        (self.repo / "project" / "drivers").mkdir(parents=True)
        (self.repo / "project" / "drivers" / "driver" / "python").mkdir(parents=True)
        (self.repo / "project" / "drivers" / "adaptor" / "scratchbird-dbeaver-driver").mkdir(parents=True)
        write_csv(
            self.repo / "project" / "drivers" / "DriverPackageManifest.csv",
            [
                manifest_row(
                    "driver:python",
                    "driver",
                    "python",
                    "python",
                    "project/drivers/driver/python",
                ),
                manifest_row(
                    "adaptor:scratchbird-dbeaver-driver",
                    "adaptor",
                    "scratchbird-dbeaver-driver",
                    "dbeaver",
                    "project/drivers/adaptor/scratchbird-dbeaver-driver",
                ),
            ],
            MANIFEST_FIELDS,
        )
        write_csv(
            self.workplan / "LANE_COMPLETION_MATRIX.csv",
            [
                {
                    "component_id": "driver:python",
                    "category": "driver",
                    "name": "python",
                    "source_path": "project/drivers/driver/python",
                    "workplan_path": "private-controller/drivers/python",
                    "release_scope": "in_scope_required",
                    "completion_policy": "no_deferral_full_implementation_required",
                    "required_shared_gates": "BETA-DTA-GATE-003;BETA-DTA-GATE-010",
                    "status": "implemented_and_proven",
                },
                {
                    "component_id": "adaptor:scratchbird-dbeaver-driver",
                    "category": "adaptor",
                    "name": "scratchbird-dbeaver-driver",
                    "source_path": "project/drivers/adaptor/scratchbird-dbeaver-driver",
                    "workplan_path": "private-controller/drivers/scratchbird-dbeaver-driver",
                    "release_scope": "separate_controller_explicit_user_exception",
                    "completion_policy": "not_part_of_this_beta_controller",
                    "required_shared_gates": "BETA-DTA-GATE-001",
                    "status": "separate_controller",
                },
            ],
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def run_gate(self, relative_script: str, *args: str) -> subprocess.CompletedProcess[str]:
        output = self.root / "report.json"
        command = [
            sys.executable,
            str(REPO_ROOT / relative_script),
            "--repo-root",
            str(self.repo),
            "--workplan-root",
            str(self.workplan),
            "--output",
            str(output),
            *args,
        ]
        return subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)

    def read_report(self) -> dict[str, object]:
        return json.loads((self.root / "report.json").read_text(encoding="utf-8"))

    def test_inventory_gate_accepts_manifest_driven_dbeaver_exclusion(self) -> None:
        result = self.run_gate("project/tools/release/verify_driver_lane_inventory.py")
        self.assertEqual(result.returncode, 0, result.stderr)
        report = self.read_report()
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["summary"]["in_scope_components"], 1)

    def test_no_deferral_gate_rejects_not_started_rows(self) -> None:
        write_csv(
            self.workplan / "TRACKER.csv",
            [{"slice_id": "BETA-DTA-001", "status": "not_started"}],
        )
        write_csv(
            self.workplan / "ACCEPTANCE_GATES.csv",
            [{"gate_id": "BETA-DTA-GATE-001", "status": "not_started"}],
        )
        result = self.run_gate("project/tools/release/driver_beta_no_deferral_gate.py")
        self.assertNotEqual(result.returncode, 0)
        report = self.read_report()
        self.assertEqual(report["status"], "fail")
        self.assertTrue(any("not_started" in issue for issue in report["issues"]))

    def test_release_verifier_rejects_dbeaver_output(self) -> None:
        output_root = self.root / "out"
        (output_root / "drivers" / "python").mkdir(parents=True)
        (output_root / "adapters" / "scratchbird-dbeaver-driver").mkdir(parents=True)
        write_csv(
            self.workplan / "RELEASE_ARTIFACT_MANIFEST_MATRIX.csv",
            [
                {
                    "artifact_id": "ART-001",
                    "component_id": "driver:python",
                    "artifact_type": "driver_package",
                    "expected_public_output": "build/output/drivers/python",
                    "required_metadata": "hash SBOM license version source_commit",
                    "install_proof": "package install plus conformance",
                    "release_scope": "in_scope_required",
                    "status": "implemented_and_proven",
                },
                {
                    "artifact_id": "ART-002",
                    "component_id": "adaptor:scratchbird-dbeaver-driver",
                    "artifact_type": "adapter_package",
                    "expected_public_output": "none",
                    "required_metadata": "excluded_by_controller",
                    "install_proof": "no install proof in this controller",
                    "release_scope": "separate_controller",
                    "status": "excluded",
                },
            ],
        )
        result = self.run_gate(
            "project/tools/release/verify_driver_beta_release.py",
            str(output_root),
        )
        self.assertNotEqual(result.returncode, 0)
        report = self.read_report()
        self.assertTrue(any("dbeaver_output_present" in issue for issue in report["issues"]))

    def test_toolchain_probe_uses_matrix_lane_tokens(self) -> None:
        write_csv(
            self.workplan / "PLATFORM_TOOLCHAIN_MATRIX.csv",
            [
                {
                    "toolchain_id": "TOOLCHAIN-001",
                    "lanes": "python",
                    "required_runtime": "Python venv pip build",
                    "minimum_version": "release_profile_current",
                    "package_manager": "pip;python_build",
                    "proof_command_anchor": "BETA-DTA-CMD-024",
                    "status": "not_started",
                }
            ],
        )
        result = self.run_gate(
            "project/tools/release/driver_toolchain_probe.py",
            "--allow-missing",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = self.read_report()
        self.assertEqual(report["status"], "pass")

    def test_driver_test_server_contract_only_verify(self) -> None:
        write_csv(
            self.workplan / "LIVE_SERVER_TEST_FIXTURE_MATRIX.csv",
            [
                {
                    "fixture_id": "FIX-001",
                    "area": "startup",
                    "status": "not_started",
                    "public_anchor": "project/tools/driver_test_server#driver_test_server_start",
                    "required_fixture": "server starts",
                    "acceptance": "starts",
                },
                {
                    "fixture_id": "FIX-002",
                    "area": "shutdown",
                    "status": "not_started",
                    "public_anchor": "project/tools/driver_test_server#driver_test_server_stop",
                    "required_fixture": "server stops",
                    "acceptance": "stops",
                },
                {
                    "fixture_id": "FIX-003",
                    "area": "reset",
                    "status": "not_started",
                    "public_anchor": "project/tools/driver_test_server#driver_test_server_reset",
                    "required_fixture": "fixture resets",
                    "acceptance": "resets",
                },
            ],
        )
        result = self.run_gate(
            "project/tools/driver_test_server/driver_test_server_fixture.py",
            "verify",
            "--contract-only",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        report = self.read_report()
        self.assertEqual(report["status"], "pass")


if __name__ == "__main__":
    unittest.main()
