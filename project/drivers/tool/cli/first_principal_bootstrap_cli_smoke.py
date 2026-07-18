#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Exercise the shared SBsql/SBsec bootstrap entry point without OS mutation.

The positive first-principal path requires root/Administrator authority and is
therefore exercised by installed-artifact privilege tests.  This smoke proves
that each public CLI recognizes the same closed grammar *before* it can open a
connection, prompt for an ordinary login, or create a database file.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


class SmokeError(RuntimeError):
    pass


def run_command(command: list[str], work: Path, name: str) -> subprocess.CompletedProcess[bytes]:
    completed = subprocess.run(
        command,
        stdout=(work / f"{name}.out").open("wb"),
        stderr=(work / f"{name}.err").open("wb"),
        check=False,
    )
    return completed


def combined_output(work: Path, name: str) -> str:
    return "\n".join(
        (work / f"{name}.{suffix}").read_text(
            encoding="utf-8", errors="replace"
        )
        for suffix in ("out", "err")
    )


def assert_no_bootstrap_artifacts(database: Path) -> None:
    forbidden = (
        database,
        Path(f"{database}.sb.local_password_auth"),
        Path(f"{database}.sb.security_principal_events"),
    )
    found = [str(path) for path in forbidden if path.exists()]
    if found:
        raise SmokeError(f"rejected bootstrap left state behind: {', '.join(found)}")


def exercise_tool(tool: str, work: Path) -> None:
    tool_name = Path(tool).name
    base = [
        tool,
        "bootstrap",
        "qa_admin",
        str(work / f"{tool_name}.sbdb"),
        "--mode=embedded",
        f"--platform-profile={work / 'SBbootstrap.profile'}",
        f"--resource-seed-pack-root={work / 'resource-pack'}",
        f"--policy-seed-pack-root={work / 'policy-pack'}",
    ]

    help_result = run_command([tool, "bootstrap", "--help"], work, f"{tool_name}-help")
    if help_result.returncode != 0 or "first-principal" not in combined_output(
        work, f"{tool_name}-help"
    ).lower():
        raise SmokeError(f"{tool_name} did not expose the shared bootstrap help")

    password_database = Path(base[3])
    password_result = run_command(
        [*base, "--password=must-not-reach-engine"],
        work,
        f"{tool_name}-password",
    )
    password_output = combined_output(work, f"{tool_name}-password")
    if password_result.returncode == 0 or "BOOTSTRAP.OPTION_NOT_ALLOWED" not in password_output:
        raise SmokeError(f"{tool_name} accepted a bootstrap password in argv")
    assert_no_bootstrap_artifacts(password_database)

    route_database = work / f"{tool_name}-route.sbdb"
    route_result = run_command(
        [
            tool,
            "bootstrap",
            "qa_admin",
            str(route_database),
            "--mode=embedded",
            f"--platform-profile={work / 'SBbootstrap.profile'}",
            f"--resource-seed-pack-root={work / 'resource-pack'}",
            f"--policy-seed-pack-root={work / 'policy-pack'}",
            "--manager-auth-token=must-not-reach-manager",
        ],
        work,
        f"{tool_name}-route",
    )
    route_output = combined_output(work, f"{tool_name}-route")
    if route_result.returncode == 0 or "BOOTSTRAP.OPTION_NOT_ALLOWED" not in route_output:
        raise SmokeError(f"{tool_name} accepted a manager route during bootstrap")
    assert_no_bootstrap_artifacts(route_database)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--sb-security", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    root = Path(args.work_dir)
    root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="sb_first_principal_bootstrap_", dir=root))
    succeeded = False
    try:
        exercise_tool(args.sb_isql, work)
        exercise_tool(args.sb_security, work)
        succeeded = True
        print("cli_first_principal_bootstrap_cli_smoke=passed")
        return 0
    except Exception as exc:  # noqa: BLE001 - preserve precise CTest evidence.
        print(f"cli_first_principal_bootstrap_cli_smoke=failed work={work}: {exc}", file=sys.stderr)
        for path in sorted(work.glob("*")):
            if path.is_file():
                print(f"--- {path.name} ---", file=sys.stderr)
                print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)
        return 1
    finally:
        if succeeded:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
