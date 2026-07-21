#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Quarantined legacy beta-driver verifier; it has no publish authority.

The former beta release route cannot prove a completed component's build,
install, live ScratchBird connection, independent review, and immutable
admission record.  It is intentionally retained only to emit a deterministic
fail-closed report for callers that still invoke its historical command line.
Completed driver, adaptor, and MCP release publication must use the separate
completed-component nightly controller when that controller exists.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from driver_release_common import (
    add_common_args,
    default_report_path,
    fail,
    legacy_driver_release_quarantine_report,
    resolve_repo_root,
    resolve_workplan_root,
    write_report,
)


REPORT_NAME = "driver_beta_release_verify.json"
GATE_ID = "BETA-DTA-GATE-019"


def build_report(repo_root: Path, workplan_root: Path, output_root: Path) -> dict[str, Any]:
    """Return the permanent fail-closed result for the retired beta route."""

    del repo_root, workplan_root
    return legacy_driver_release_quarantine_report(
        "verify_driver_beta_release.py",
        GATE_ID,
        output_root,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    parser.add_argument("output_root", nargs="?", type=Path, default=Path("build/output"))
    args = parser.parse_args()
    repo_root = resolve_repo_root(args.repo_root)
    workplan_root = resolve_workplan_root(repo_root, args.workplan_root)
    output_root = args.output_root
    if not output_root.is_absolute():
        output_root = repo_root / output_root
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        report = build_report(repo_root, workplan_root, output_root.resolve())
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_beta_release_verify={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
