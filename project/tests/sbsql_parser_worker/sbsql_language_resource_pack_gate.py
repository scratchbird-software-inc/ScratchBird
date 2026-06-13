#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest gate for the generated SBsql language resource pack."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


VERIFIER = "project/tools/release/verify_sbsql_language_resource_pack.py"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    verifier = repo_root / VERIFIER
    if not verifier.is_file():
        print(f"sbsql_language_resource_pack_gate=failed missing verifier: {verifier}", file=sys.stderr)
        return 2

    result = subprocess.run(
        [
            sys.executable,
            str(verifier),
            "--repo-root",
            str(repo_root),
            "--check-generated",
            "--check-corruption",
        ],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        print(f"sbsql_language_resource_pack_gate=failed exit={result.returncode}", file=sys.stderr)
        return result.returncode
    print("sbsql_language_resource_pack_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
