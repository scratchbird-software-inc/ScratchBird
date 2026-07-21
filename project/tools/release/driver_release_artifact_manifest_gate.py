#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Quarantined legacy driver-artifact gate; it has no publish authority.

The old beta artifact manifest asserted metadata over a broad output tree but
did not establish that each component was complete, independently admitted,
or installable against a live ScratchBird route.  It is retained only so
historical automation receives an explicit, deterministic failure.  A future
completed-component nightly controller is the sole permitted driver/adaptor/
MCP publisher.
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


REPORT_NAME = "driver_release_artifact_manifest.json"
GATE_ID = "BETA-DTA-GATE-028"


def build_report(repo_root: Path, workplan_root: Path, output_root: Path) -> dict[str, Any]:
    """Return the permanent fail-closed result for the retired beta route."""

    del repo_root, workplan_root
    return legacy_driver_release_quarantine_report(
        "driver_release_artifact_manifest_gate.py",
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
    output_root = args.output_root if args.output_root.is_absolute() else repo_root / args.output_root
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        report = build_report(repo_root, workplan_root, output_root.resolve())
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_release_artifact_manifest={report['status']}")
    return 0 if report["status"] == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
