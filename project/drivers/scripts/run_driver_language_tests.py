#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run driver-facing language resource and fallback conformance gates."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def child_env(repo_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(repo_root / "build" / "pycache" / "driver_scripts")
    return env


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--all-in-scope", action="store_true", required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    commands = [
        [
            sys.executable,
            "project/tests/conformance/drivers/validate_shared_beta_driver_corpus.py",
            "--repo-root",
            str(repo_root),
        ],
        [
            sys.executable,
            "project/tools/release/sbsql_driver_language_surface_gate.py",
            "--repo-root",
            str(repo_root),
        ],
    ]
    results: list[dict[str, Any]] = []
    for command in commands:
        result = subprocess.run(
            command,
            cwd=repo_root,
            env=child_env(repo_root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        results.append(
            {
                "command": command,
                "returncode": result.returncode,
                "status": "pass" if result.returncode == 0 else "fail",
                "output_tail": result.stdout.splitlines()[-120:],
            }
        )
    failures = [result for result in results if result["status"] != "pass"]
    report = {
        "command": "run_driver_language_tests.py",
        "status": "pass" if not failures else "fail",
        "summary": {"checks": len(results), "failed": len(failures)},
        "results": results,
    }
    output = args.output or repo_root / "build" / "reports" / "driver_language_tests.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"driver_language_tests={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
