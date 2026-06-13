#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run native component tests for manifest-selected driver lanes."""

from __future__ import annotations

import argparse
import csv
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


DBEAVER_COMPONENT_ID = "adaptor:scratchbird-dbeaver-driver"


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def read_manifest(repo_root: Path) -> list[dict[str, str]]:
    path = repo_root / "project" / "drivers" / "DriverPackageManifest.csv"
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def selected_components(
    rows: list[dict[str, str]],
    category: str,
    exclude: set[str],
) -> list[str]:
    components: list[str] = []
    for row in rows:
        component_id = row.get("component_id", "").strip()
        if row.get("category", "").strip() != category:
            continue
        if component_id == DBEAVER_COMPONENT_ID or row.get("name", "").strip() in exclude:
            continue
        components.append(component_id)
    return components


def report_path(repo_root: Path, name: str) -> Path:
    return repo_root / "build" / "reports" / name


def child_env(repo_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(repo_root / "build" / "pycache" / "driver_scripts")
    return env


def run_component(repo_root: Path, component: str, require_all_toolchains: bool) -> dict[str, Any]:
    command = [
        sys.executable,
        "project/drivers/scripts/driver_component_runner.py",
        "--repo-root",
        str(repo_root),
        "--project-root",
        str(repo_root / "project"),
        "--build-root",
        str(repo_root / "build" / "drivers"),
        "--component",
        component,
    ]
    if require_all_toolchains:
        command.append("--require-all-toolchains")
    else:
        command.append("--allow-toolchain-waivers")
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
        "component": component,
        "returncode": result.returncode,
        "status": "pass" if result.returncode == 0 else ("skip" if result.returncode == 77 else "fail"),
        "command": command,
        "output_tail": result.stdout.splitlines()[-80:],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--category", choices=("driver", "adaptor", "tool"), required=True)
    parser.add_argument("--exclude", action="append", default=[])
    parser.add_argument("--require-all-toolchains", action="store_true")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    rows = read_manifest(repo_root)
    components = selected_components(rows, args.category, set(args.exclude))
    results = [
        run_component(repo_root, component, args.require_all_toolchains)
        for component in components
    ]
    failures = [result for result in results if result["status"] != "pass"]
    report = {
        "command": "run_driver_lane_tests.py",
        "category": args.category,
        "status": "pass" if not failures else "fail",
        "summary": {
            "components": len(components),
            "passed": sum(1 for result in results if result["status"] == "pass"),
            "skipped": sum(1 for result in results if result["status"] == "skip"),
            "failed": sum(1 for result in results if result["status"] == "fail"),
        },
        "results": results,
    }
    output = args.output or report_path(repo_root, f"{args.category}_lane_tests.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{args.category}_lane_tests={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
