#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Regression coverage for the non-publishable driver metadata snapshot."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
STAGER = REPOSITORY_ROOT / "project" / "tools" / "release" / "stage_driver_beta_artifacts.py"


class DriverMetadataSnapshotTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory(
            prefix="scratchbird_driver_metadata_snapshot_"
        )
        self.root = Path(self.temporary_directory.name) / "repository"
        manifest = self.root / "project" / "drivers" / "DriverPackageManifest.csv"
        manifest.parent.mkdir(parents=True)
        rows = [
            {
                "component_id": "driver:python",
                "category": "driver",
                "name": "python",
                "driver_family": "python",
                "source_path": "project/drivers/driver/python",
                "conformance_profile_ref": "fixture",
                "wire_protocol_set": "sbwp",
                "auth_method_set": "password",
                "tls_profile_set": "tls",
            },
            {
                "component_id": "adaptor:scratchbird-dbeaver-driver",
                "category": "adaptor",
                "name": "scratchbird-dbeaver-driver",
                "driver_family": "dbeaver",
                "source_path": "project/drivers/adaptor/scratchbird-dbeaver-driver",
                "conformance_profile_ref": "fixture",
                "wire_protocol_set": "sbwp",
                "auth_method_set": "password",
                "tls_profile_set": "tls",
            },
            {
                "component_id": "ADAPTOR:DBEAVER",
                "category": "adaptor",
                "name": "not-a-driver-name",
                "driver_family": "DBeaver",
                "source_path": "project/drivers/adaptor/alternate-name",
                "conformance_profile_ref": "fixture",
                "wire_protocol_set": "sbwp",
                "auth_method_set": "password",
                "tls_profile_set": "tls",
            },
            {
                "component_id": "tool:cli",
                "category": "tool",
                "name": "cli",
                "driver_family": "native",
                "source_path": "project/drivers/tool/cli",
                "conformance_profile_ref": "fixture",
                "wire_protocol_set": "sbwp",
                "auth_method_set": "password",
                "tls_profile_set": "tls",
            },
        ]
        with manifest.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        python_source = self.root / "project" / "drivers" / "driver" / "python"
        python_source.mkdir(parents=True)
        (python_source / "driver.py").write_text("print('fixture')\n", encoding="utf-8")
        tool_source = self.root / "project" / "drivers" / "tool" / "cli"
        tool_source.mkdir(parents=True)
        (tool_source / "main.py").write_text("print('fixture')\n", encoding="utf-8")

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def run_stager(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(STAGER),
                "--repo-root",
                str(self.root),
                *arguments,
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_explicit_metadata_only_acknowledgement_is_required(self) -> None:
        output_root = self.root / "snapshot"
        report = self.root / "report.json"
        result = self.run_stager(
            "--output-root",
            str(output_root),
            "--report",
            str(report),
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--metadata-only is required", result.stderr)
        self.assertFalse(output_root.exists())
        self.assertFalse(report.exists())

    def test_release_output_root_is_rejected(self) -> None:
        report = self.root / "report.json"
        result = self.run_stager(
            "--metadata-only",
            "--output-root",
            "build/output",
            "--report",
            str(report),
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("reserved for verified release payloads", result.stderr)
        self.assertFalse(report.exists())

    def test_snapshot_is_explicitly_non_publishable_and_not_release_shaped(self) -> None:
        output_root = self.root / "snapshot"
        report_path = self.root / "report.json"
        result = self.run_stager(
            "--metadata-only",
            "--output-root",
            str(output_root),
            "--report",
            str(report_path),
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("metadata_snapshot", result.stdout)
        snapshot = json.loads((output_root / "METADATA_SNAPSHOT.json").read_text(encoding="utf-8"))
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertFalse(snapshot["release_eligible"])
        self.assertTrue(snapshot["must_not_publish"])
        self.assertTrue(snapshot["dbeaver_excluded"])
        self.assertEqual(
            [entry["component_id"] for entry in snapshot["components"]],
            ["driver:python", "tool:cli"],
        )
        self.assertEqual(report["result"], "metadata_snapshot")
        self.assertFalse(report["release_eligible"])
        self.assertTrue(report["must_not_publish"])
        self.assertFalse((output_root / "RELEASE_MANIFEST.json").exists())
        self.assertFalse(any(path.name == "artifact_manifest.json" for path in output_root.rglob("*")))
        self.assertFalse(any(path.name == "SHA256SUMS" for path in output_root.rglob("*")))
        self.assertTrue((output_root / "SOURCE_METADATA_SHA256SUMS").is_file())
        self.assertTrue(
            (output_root / "components" / "driver" / "python" / "source_metadata.json").is_file()
        )


if __name__ == "__main__":
    unittest.main()
