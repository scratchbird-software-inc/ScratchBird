#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""MMCH-110 cross-platform memory behavior source gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    root = Path(args.repo_root)
    memory_root = root / "project/src/core/memory"
    memory_cpp = read(memory_root / "memory.cpp")
    policy_cpp = read(memory_root / "memory_policy_config.cpp")
    locality_cpp = read(memory_root / "memory_locality_policy.cpp")
    temp_cpp = read(memory_root / "temp_workspace_lifecycle.cpp")

    combined = "\n".join([memory_cpp, policy_cpp, locality_cpp, temp_cpp])
    for token in ("_WIN32", "__linux__", "__APPLE__", "__FreeBSD__",
                  "__NetBSD__", "__OpenBSD__"):
        require(token in combined, f"missing cross-platform branch {token}")
    for token in ("BCryptGenRandom", "arc4random_buf", "getrandom",
                  "CreateFileW", "O_EXCL", "O_NOFOLLOW", "posix_fallocate",
                  "SetFileInformationByHandle"):
        require(token in combined, f"missing cross-platform memory/temp primitive {token}")
    require("CurrentTempWorkspacePlatformSecurityCapabilities" in temp_cpp,
            "temp workspace platform capability evidence missing")
    require("CurrentMemoryLocalityPlatformCapabilities" in locality_cpp,
            "memory locality platform capability evidence missing")
    require("MMCH_TEMP_WORKSPACE_CROSS_PLATFORM" in temp_cpp,
            "temp workspace cross-platform anchor missing")
    require("MMCH_NUMA_HUGEPAGE_LOCALITY_POLICY" in locality_cpp,
            "locality/hugepage policy anchor missing")

    print("MMCH_CROSS_PLATFORM_MEMORY_BEHAVIOR: PASS")
    print("platform_branches=linux,windows,macos,bsd")
    print("temp_workspace_secure_primitives=present")
    print("memory_locality_policy=present")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"MMCH_CROSS_PLATFORM_MEMORY_BEHAVIOR: FAIL: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
