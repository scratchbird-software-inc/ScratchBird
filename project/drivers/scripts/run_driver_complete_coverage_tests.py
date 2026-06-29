#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run the driver complete-coverage proof suite.

This wrapper keeps the command-registry gate stable while delegating to the
public conformance validators that own each proof surface. It never rewrites
evidence to pass; it records command output and fails on any validator failure.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any


REPORT_NAME = "driver_complete_coverage_tests.json"
STATIC_MODES = (
    "sblr_uuid_bundle",
    "schema_path_resolution",
    "sblr_uuid_authorization",
    "cache_lifecycle",
    "sbsql_language_resource",
)


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def child_env(repo_root: Path) -> dict[str, str]:
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(repo_root / "build" / "pycache" / "driver_complete_coverage")
    return env


def run_command(repo_root: Path, command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
    try:
        result = subprocess.run(
            command,
            cwd=repo_root,
            env=child_env(repo_root),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=timeout,
        )
        return {
            "command": command,
            "returncode": result.returncode,
            "status": "pass" if result.returncode == 0 else "fail",
            "output_tail": result.stdout.splitlines()[-120:],
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "command": command,
            "returncode": None,
            "status": "fail",
            "timeout_seconds": timeout,
            "output_tail": (exc.stdout or "").splitlines()[-120:] if isinstance(exc.stdout, str) else [],
            "error": "timeout",
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--all-drivers", action="store_true", required=True)
    parser.add_argument("--no-skip", action="store_true")
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--execute-matrix",
        action="store_true",
        help="Run live native full-surface matrix instead of plan/static validation only.",
    )
    parser.add_argument(
        "--allow-plan-only",
        action="store_true",
        help="Allow plan-only matrix output for local preflight. Release no-skip gates must not use this.",
    )
    parser.add_argument("--matrix-extra-arg", action="append", default=[])
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    commands: list[tuple[str, list[str], int | None]] = [
        (
            "workplan_coverage",
            [
                sys.executable,
                "project/tests/conformance/drivers/driver_native_workplan_coverage_gate.py",
                "--repo-root",
                str(repo_root),
            ],
            600,
        ),
        (
            "full_surface_script_suite",
            [
                sys.executable,
                "project/tests/conformance/drivers/full_surface_scripts/validate_full_surface_script_suite.py",
                "--output-root",
                str(repo_root / "build" / "reports" / "driver_complete_coverage_full_surface_validation"),
            ],
            900,
        ),
    ]
    for mode in STATIC_MODES:
        commands.append(
            (
                f"static_requirement_{mode}",
                [
                    sys.executable,
                    "project/tests/conformance/drivers/driver_static_requirement_gate.py",
                    "--repo-root",
                    str(repo_root),
                    "--mode",
                    mode,
                ],
                600,
            )
        )

    matrix_command = [
        sys.executable,
        "project/drivers/scripts/run_driver_native_full_surface_matrix.py",
        "--repo-root",
        str(repo_root),
        "--output",
        str(repo_root / "build" / "reports" / "driver_complete_coverage_native_matrix.json"),
    ]
    if not args.execute_matrix:
        matrix_command.append("--plan-only")
    matrix_command.extend(args.matrix_extra_arg)
    commands.append(("native_full_surface_matrix", matrix_command, None if args.execute_matrix else 900))

    results = [
        {"name": name, **run_command(repo_root, command, timeout=timeout)}
        for name, command, timeout in commands
    ]
    if not args.execute_matrix and not args.allow_plan_only:
        results.append(
            {
                "name": "native_full_surface_matrix_release_requirement",
                "command": matrix_command,
                "returncode": 1,
                "status": "fail",
                "output_tail": [
                    "release complete-coverage gate requires --execute-matrix live evidence",
                    "plan-only output is a preflight artifact, not release proof",
                ],
            }
        )
    failures = [result for result in results if result["status"] != "pass"]
    if not failures and not args.execute_matrix and args.allow_plan_only:
        status = "preflight_pass"
    else:
        status = "pass" if not failures else "fail"
    report = {
        "command": "run_driver_complete_coverage_tests.py",
        "gate_id": "BETA-DTA-GATE-034",
        "status": status,
        "summary": {
            "all_drivers": args.all_drivers,
            "no_skip": args.no_skip,
            "execute_matrix": args.execute_matrix,
            "commands": len(results),
            "passed": len(results) - len(failures),
            "failed": len(failures),
        },
        "results": results,
    }
    output = args.output or repo_root / "build" / "reports" / REPORT_NAME
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"driver_complete_coverage_tests={report['status']}")
    return 0 if report["status"] in {"pass", "preflight_pass"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
