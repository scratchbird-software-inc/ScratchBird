#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import os
import platform
import shutil
import subprocess
from pathlib import Path


FORBIDDEN_LINK_TOKENS = (
    "libfbclient",
    "libtommath",
    "libtomcrypt",
    "firebird-5.0.4-release-src",
    "reference/firebird",
)

FORBIDDEN_SOURCE_TOKENS = (
    "#include <ibase.h>",
    "#include \"ibase.h\"",
    "#include <firebird/",
    "#include \"firebird/",
    "libfbclient",
    "tommath",
    "tomcrypt",
    "fbclient",
)


def dependency_scan_command() -> list[str]:
    if platform.system() == "Darwin":
        path = shutil.which("otool")
        if path:
            return [path, "-L"]
        raise SystemExit("runtime isolation dependency scanner missing: otool")
    if os.name == "nt":
        for tool in ("objdump", "llvm-objdump"):
            path = shutil.which(tool)
            if path:
                return [path, "-p"]
        raise SystemExit("runtime isolation dependency scanner missing: objdump")
    ldd = shutil.which("ldd")
    if ldd:
        return [ldd]
    for tool in ("objdump", "llvm-objdump"):
        path = shutil.which(tool)
        if path:
            return [path, "-p"]
    raise SystemExit("runtime isolation dependency scanner missing: ldd or objdump")


def check_binary(binary: Path) -> None:
    if not binary.exists():
        raise SystemExit(f"runtime isolation binary missing: {binary}")
    if not binary.is_file():
        raise SystemExit(f"runtime isolation path is not a file: {binary}")

    command = [*dependency_scan_command(), str(binary)]
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output = result.stdout
    if result.returncode != 0 and "not a dynamic executable" not in output:
        raise SystemExit(f"dependency scan failed for {binary}: {output}")
    lowered = output.lower()
    for token in FORBIDDEN_LINK_TOKENS:
        if token in lowered:
            raise SystemExit(f"forbidden reference/runtime dependency {token} in {binary}: {output}")


def check_source_tree(root: Path) -> None:
    if not root.exists():
        raise SystemExit(f"runtime isolation source root missing: {root}")
    for path in root.rglob("*"):
        if path.suffix not in (".cpp", ".hpp", ".h", ".cc", ".cxx"):
            continue
        text = path.read_text(errors="replace")
        lowered = text.lower()
        for token in FORBIDDEN_SOURCE_TOKENS:
            if token in lowered:
                raise SystemExit(f"forbidden reference runtime token {token} in {path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", action="append", required=True)
    parser.add_argument("--binary", nargs="+", required=True)
    args = parser.parse_args()

    for source_root in args.source_root:
        check_source_tree(Path(source_root))
    for binary in args.binary:
        check_binary(Path(binary))

    print(
        "validated Firebird runtime isolation for "
        f"{len(args.source_root)} source roots and {len(args.binary)} binaries"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
