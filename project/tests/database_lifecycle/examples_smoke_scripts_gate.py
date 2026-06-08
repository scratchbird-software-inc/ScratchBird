#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate and run the core beta QA examples without execution_plan artifacts."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys


REQUIRED_MARKERS = {
    "admin_lifecycle_smoke": "admin_lifecycle_smoke=passed",
    "embedded_public_abi_smoke": "embedded_public_abi_smoke=passed",
    "driver_route_smoke": "driver_route_smoke=passed",
}


def fail(message: str) -> None:
    print(f"examples_smoke_scripts_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def run_command(command: list[str], *, cwd: pathlib.Path, timeout: int = 300) -> str:
    result = subprocess.run(
        command,
        cwd=str(cwd),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )
    if result.returncode != 0:
        print(result.stdout)
        fail(f"command_failed:{' '.join(command)}:exit={result.returncode}")
    return result.stdout


def require_registered_tests(build_root: pathlib.Path, test_names: list[str]) -> None:
    pattern = "|".join(test_names)
    output = run_command(
        ["ctest", "--test-dir", str(build_root), "-N", "-R", pattern],
        cwd=build_root,
        timeout=120,
    )
    missing = [name for name in test_names if name not in output]
    if missing:
        fail("missing_ctest_registration:" + ",".join(missing))


def validate_static_files(repo_root: pathlib.Path, manifest: dict[str, object]) -> None:
    examples_root = repo_root / "project" / "examples" / "core_beta_qa"
    readme = examples_root / "README.md"
    if not readme.exists():
        fail("missing_readme")
    readme_text = readme.read_text(encoding="utf-8")
    for phrase in (
        "SBLR/internal API execution",
        "SQL text is not runtime authority",
        "admin_lifecycle_smoke.sh",
        "embedded_public_abi_smoke.sh",
        "driver_route_smoke.sh",
    ):
        if phrase not in readme_text:
            fail(f"readme_missing:{phrase}")

    if manifest.get("schema_version") != 1:
        fail("unexpected_manifest_schema")
    boundary = str(manifest.get("engine_execution_boundary", ""))
    if "SQL text" not in boundary or "not runtime authority" not in boundary:
        fail("manifest_boundary_missing")

    scripts = manifest.get("scripts")
    if not isinstance(scripts, list) or len(scripts) != 3:
        fail("manifest_scripts_shape")

    for script in scripts:
        if not isinstance(script, dict):
            fail("manifest_script_not_object")
        script_id = str(script.get("id", ""))
        script_path = repo_root / str(script.get("path", ""))
        if script_id not in REQUIRED_MARKERS:
            fail(f"unknown_script_id:{script_id}")
        if not script_path.exists():
            fail(f"missing_script:{script_path}")
        text = script_path.read_text(encoding="utf-8")
        for required in ("set -euo pipefail", "trap cleanup EXIT", "/tmp"):
            if required not in text:
                fail(f"script_missing:{script_id}:{required}")
        if "docs" "/execution-plans" in text:
            fail(f"script_depends_on_execution_plan:{script_id}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()
    manifest_path = repo_root / "project" / "examples" / "core_beta_qa" / "manifest.json"
    if not manifest_path.exists():
        fail("missing_manifest")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    validate_static_files(repo_root, manifest)
    required_tests = manifest.get("required_ctest_evidence")
    if not isinstance(required_tests, list) or not all(isinstance(item, str) for item in required_tests):
        fail("manifest_required_ctest_shape")
    require_registered_tests(build_root, list(required_tests))

    env = os.environ.copy()
    env["SCRATCHBIRD_CORE_BETA_EXAMPLES_GATE"] = "1"
    for script in manifest["scripts"]:
        script_id = str(script["id"])
        script_path = repo_root / str(script["path"])
        output = subprocess.run(
            ["bash", str(script_path), str(repo_root), str(build_root)],
            cwd=str(repo_root),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=300,
            check=False,
        )
        if output.returncode != 0:
            print(output.stdout)
            fail(f"script_failed:{script_id}:exit={output.returncode}")
        if REQUIRED_MARKERS[script_id] not in output.stdout:
            print(output.stdout)
            fail(f"script_marker_missing:{script_id}")
        print(output.stdout.strip())

    print("examples_smoke_scripts_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
