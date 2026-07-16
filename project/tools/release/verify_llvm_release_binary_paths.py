#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fail if release binaries retain configured LLVM build-machine paths."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def contains(path: Path, needle: bytes) -> bool:
    overlap = max(len(needle) - 1, 0)
    previous = b""
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            data = previous + chunk
            if needle in data:
                return True
            previous = data[-overlap:] if overlap else b""
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, action="append", required=True)
    parser.add_argument(
        "--forbidden-fragment", action="append", default=[], required=True
    )
    args = parser.parse_args()
    fragments = [value for value in args.forbidden_fragment if value]
    if not fragments:
        print("verify_llvm_release_binary_paths=fail:no_forbidden_fragments", file=sys.stderr)
        return 1
    for binary in args.binary:
        if not binary.is_file() or binary.stat().st_size <= 0:
            print(
                f"verify_llvm_release_binary_paths=fail:binary_missing:{binary}",
                file=sys.stderr,
            )
            return 1
        for fragment in fragments:
            if contains(binary, fragment.encode("utf-8")):
                print(
                    "verify_llvm_release_binary_paths=fail:"
                    f"build_path_leak:{binary.name}:{fragment}",
                    file=sys.stderr,
                )
                return 1
    print("verify_llvm_release_binary_paths=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
