#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run all in-scope native lane tests through the shared component runner."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


CATEGORIES = ("driver", "adaptor", "tool")


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def child_env(repo_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(repo_root / "build" / "pycache" / "driver_scripts")
    return env


def run_category(repo_root: Path, category: str, require_all_toolchains: bool) -> dict[str, Any]:
    command = [
        sys.executable,
        "project/drivers/scripts/run_driver_lane_tests.py",
        "--repo-root",
        str(repo_root),
        "--category",
        category,
    ]
    if category == "adaptor":
        command.extend(["--exclude", "scratchbird-dbeaver-driver"])
    if require_all_toolchains:
        command.append("--require-all-toolchains")
    result = subprocess.run(
        command,
        cwd=repo_root,
        env=child_env(repo_root),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    return {
        "category": category,
        "returncode": result.returncode,
        "status": "pass" if result.returncode == 0 else "fail",
        "command": command,
        "output_tail": result.stdout.splitlines()[-80:],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--all-in-scope", action="store_true", required=True)
    parser.add_argument("--require-all-toolchains", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    results = [
        run_category(repo_root, category, args.require_all_toolchains)
        for category in CATEGORIES
    ]
    failures = [result for result in results if result["status"] != "pass"]
    report = {
        "command": "run_driver_native_tests.py",
        "status": "pass" if not failures else "fail",
        "summary": {
            "categories": len(CATEGORIES),
            "passed": sum(1 for result in results if result["status"] == "pass"),
            "failed": len(failures),
        },
        "results": results,
    }
    output = args.output or repo_root / "build" / "reports" / "driver_native_tests.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"driver_native_tests={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
