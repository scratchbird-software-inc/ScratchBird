#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""MMCH-043 static source gate for secure temp workspace platform providers."""

from __future__ import annotations

import argparse
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root)
    source = root / "project/src/core/memory/temp_workspace_lifecycle.cpp"
    header = root / "project/src/core/memory/temp_workspace_lifecycle.hpp"
    cmake = root / "project/src/core/memory/CMakeLists.txt"
    text = source.read_text(encoding="utf-8")
    header_text = header.read_text(encoding="utf-8")
    cmake_text = cmake.read_text(encoding="utf-8")

    required_tokens = {
        "MMCH_TEMP_WORKSPACE_CROSS_PLATFORM": header_text + text,
        "BCryptGenRandom": text,
        "bcrypt": cmake_text,
        "CreateFileW": text,
        "FILE_ATTRIBUTE_REPARSE_POINT": text,
        "SetFileInformationByHandle": text,
        "arc4random_buf": text,
        "getrandom": text,
        "/dev/urandom": text,
        "openat": text,
        "O_NOFOLLOW": text,
        "posix_fallocate": text,
        "CurrentTempWorkspacePlatformSecurityCapabilities": header_text + text,
    }
    for token, haystack in required_tokens.items():
        require(token in haystack, f"MMCH-043 missing cross-platform token: {token}")

    require("platform_secure_tempfile_semantics_unavailable" not in text,
            "MMCH-043 still has unsupported-platform secure tempfile fallback")
    require("secure random source is not wired for this platform" not in text,
            "MMCH-043 still has unwired secure-random fallback")

    print("MMCH-043 source gate passed")


if __name__ == "__main__":
    main()
