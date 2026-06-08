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


class ValidateEvidenceGatesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[1]
        self.tool = self.repo_root / "tools" / "validate_evidence_gates.py"

    def _run(self, temp_root: Path, release_time: str = "2026-02-24T18:00:00Z") -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(self.tool),
                "--repo-root",
                str(temp_root),
                "--spec",
                "test_spec.md",
                "--release-time-utc",
                release_time,
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_passes_with_valid_json_and_consistent_commit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifacts = root / "artifacts" / "ai_conformance" / "01"
            artifacts.mkdir(parents=True, exist_ok=True)

            _write_spec(
                root / "test_spec.md",
                [
                    "| `EVID-01` | `01` | `x` | `x` | `x` | `artifacts/ai_conformance/01/summary.json` |",
                ],
            )
            (artifacts / "summary.json").write_text(
                json.dumps(
                    {
                        "generated_at_utc": "2026-02-24T17:59:00Z",
                        "git_commit": "abc123",
                        "status": "PASS",
                        "check_count": 3,
                        "passed_checks": 3,
                        "failed_checks": [],
                    }
                ),
                encoding="utf-8",
            )

            result = self._run(root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("OK: evidence gates valid", result.stdout)

    def test_fails_when_commits_differ(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            a01 = root / "artifacts" / "ai_conformance" / "01"
            a02 = root / "artifacts" / "ai_conformance" / "02"
            a01.mkdir(parents=True, exist_ok=True)
            a02.mkdir(parents=True, exist_ok=True)

            _write_spec(
                root / "test_spec.md",
                [
                    "| `EVID-01` | `01` | `x` | `x` | `x` | `artifacts/ai_conformance/01/summary.json` |",
                    "| `EVID-02` | `02` | `x` | `x` | `x` | `artifacts/ai_conformance/02/adapter_parity.json` |",
                ],
            )

            (a01 / "summary.json").write_text(
                json.dumps(
                    {
                        "generated_at_utc": "2026-02-24T17:59:00Z",
                        "git_commit": "abc111",
                        "status": "PASS",
                        "check_count": 1,
                        "passed_checks": 1,
                        "failed_checks": [],
                    }
                ),
                encoding="utf-8",
            )
            (a02 / "adapter_parity.json").write_text(
                json.dumps(
                    {
                        "generated_at_utc": "2026-02-24T17:59:00Z",
                        "git_commit": "abc222",
                        "status": "PASS",
                        "check_count": 1,
                        "passed_checks": 1,
                        "failed_checks": [],
                    }
                ),
                encoding="utf-8",
            )

            result = self._run(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("same git_commit", result.stderr)

    def test_fails_for_stale_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifacts = root / "artifacts" / "ai_conformance" / "01"
            artifacts.mkdir(parents=True, exist_ok=True)

            _write_spec(
                root / "test_spec.md",
                [
                    "| `EVID-01` | `01` | `x` | `x` | `x` | `artifacts/ai_conformance/01/summary.json` |",
                ],
            )
            (artifacts / "summary.json").write_text(
                json.dumps(
                    {
                        "generated_at_utc": "2026-01-01T00:00:00Z",
                        "git_commit": "abc123",
                        "status": "PASS",
                        "check_count": 1,
                        "passed_checks": 1,
                        "failed_checks": [],
                    }
                ),
                encoding="utf-8",
            )

            result = self._run(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("stale artifact", result.stderr)


if __name__ == "__main__":
    unittest.main()
