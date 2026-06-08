#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for the agent heartbeat and recovery controls validate path.

Delegates to `project/tools/sb_parser_gen/sb_execution_plan_agent_controls.py
validate` over a regression-owned fixture snapshot of AGENT_STATUS.csv,
AGENT_WRITE_SCOPE_REGISTER.csv, and TRACKER.csv:

  * every AGENT_STATUS row has required columns populated and a parseable
    last_heartbeat_utc timestamp (when present);
  * no two active agents claim the same declared primary write scope
    (write-scope lock enforcement);
  * active agents' owned write scopes are recognized as declared primary
    scopes or paths under a declared scope (no rogue scopes);
  * TRACKER.csv state transitions are sane (every `completed` row has
    outputs and acceptance text; every row's status is in the allowed
    state machine).

Heartbeat freshness is not enforced here because CTest is a one-shot
validation; agents enforce freshness through the `detect-stalls` CLI
subcommand during live work runs.

Architecture invariant compliance: read-only structural validation; no
transaction model touched; no engine, parser worker, server, listener,
storage, or MGA file modified; no WAL surface introduced. MGA copy-on-write
remains the sole transaction recovery model.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--fixture-root", "--execution_plan-root", dest="execution_plan_root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    execution_plan_root = Path(args.execution_plan_root).resolve()

    cli = root / "project/tools/sb_parser_gen/sb_execution_plan_agent_controls.py"
    if not cli.is_file():
        print(f"agent_controls cli missing: {cli}", file=sys.stderr)
        return 1

    result = subprocess.run(
        [
            sys.executable,
            str(cli),
            "--repo-root",
            str(root),
            "--execution_plan-root",
            str(execution_plan_root),
            "validate",
        ],
        check=False,
    )
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
