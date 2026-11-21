#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0
"""SEARCH_KEY: SB_NATIVE_SEAM_SYMBOLS (owner Q295, 2026-11-19).

The restart fault-injection test seam (SetNativeRestartSinkWrapperForTest / NativeRestartSinkWrapperForTest) is
gated behind the compile-time guard SB_NATIVE_LOOP_TEST_SEAMS, defined only by the one test target that needs it.
Asserts with nm: the production sb_server_core archive and the production sb_server binary contain neither
symbol; the guarded test binary contains both (so the check is not vacuous).
"""
import argparse
import shutil
import subprocess
import sys

SYMBOLS = ("SetNativeRestartSinkWrapperForTest", "NativeRestartSinkWrapperForTest", "RunWithTransactionSinkOnLoopThreadForTest")


def names(path):
    out = subprocess.run(["nm", "-C", path], capture_output=True, text=True, timeout=300)
    if out.returncode != 0:
        print(f"FAIL nm {path}: {out.stderr[-500:]}")
        sys.exit(1)
    return out.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--production", nargs="+", required=True)
    parser.add_argument("--guarded", required=True)
    args = parser.parse_args()
    if shutil.which("nm") is None:
        print("FAIL nm is required")
        sys.exit(1)
    for path in args.production:
        text = names(path)
        for symbol in SYMBOLS:
            if symbol in text:
                print(f"FAIL production artifact {path} contains {symbol}")
                sys.exit(1)
        print(f"native_seam_symbols: {path}: neither seam symbol present")
    text = names(args.guarded)
    for symbol in SYMBOLS:
        if symbol not in text:
            print(f"FAIL guarded test binary {args.guarded} lacks {symbol}: the check is vacuous")
            sys.exit(1)
    print(f"native_seam_symbols: {args.guarded}: both seam symbols present")
    print("native_seam_symbols: passed")


if __name__ == "__main__":
    main()
