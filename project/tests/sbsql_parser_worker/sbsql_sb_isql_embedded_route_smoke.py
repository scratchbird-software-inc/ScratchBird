#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Embedded sb_isql route smoke.

Drives the public CLI through:
  sb_isql -> in-process SBsql parser bridge -> embedded engine dispatcher.

No server, listener, parser worker process, or authentication database is launched
for this route. The embedded session is the local sysarch session for only this
single database file.
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


def make_work_dir(preferred_root: Path) -> Path:
    roots = (preferred_root, Path(tempfile.gettempdir()) / "sb_isql_embedded")
    for root in roots:
        root.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="sbi_", dir=root))
        if len(str(candidate / "embedded.sbdb")) < 160:
            return candidate
        shutil.rmtree(candidate, ignore_errors=True)
    raise SmokeError("unable to allocate an embedded workspace")


def dump_logs(work: Path) -> None:
    for name in ("sb_isql.out", "sb_isql.err"):
        path = work / name
        if path.exists():
            print(f"--- {name} ---", file=sys.stderr)
            print(path.read_text(encoding="utf-8", errors="replace"), file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sb-isql", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()

    work = make_work_dir(Path(args.work_dir))
    try:
        database = work / "embedded.sbdb"
        completed = subprocess.run(
            [
                args.sb_isql,
                str(database),
                "--mode=embedded",
                "--sslmode=disable",
                "-q",
                "-A",
                "-t",
                "-c",
                "SELECT 1",
            ],
            stdout=(work / "sb_isql.out").open("wb"),
            stderr=(work / "sb_isql.err").open("wb"),
            check=False,
        )
        if completed.returncode != 0:
            raise SmokeError(f"sb_isql exited {completed.returncode}")
        output = (work / "sb_isql.out").read_text(encoding="utf-8", errors="replace").strip()
        if output != "1":
            raise SmokeError(f"sb_isql SELECT 1 returned {output!r}")
        if not database.exists():
            raise SmokeError("embedded route did not create/open the database file")
        print(f"sbsql_sb_isql_embedded_route_smoke=passed work={work}")
        return 0
    except Exception as exc:  # noqa: BLE001 - ctest should receive the concrete failure.
        print(f"sbsql_sb_isql_embedded_route_smoke=failed work={work}: {exc}", file=sys.stderr)
        dump_logs(work)
        return 1
    finally:
        if not any((work / name).exists() and (work / name).stat().st_size for name in ("sb_isql.err",)):
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
