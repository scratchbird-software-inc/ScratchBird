#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import subprocess
import sys
from pathlib import Path


POLICY_TOKENS = (
    "read-only behavior evidence",
    "original ScratchBird code",
    "Copy Firebird implementation code",
    "Link ScratchBird runtime products against Firebird libraries",
    "firebird_clean_room_provenance_gate",
)

AUDIT_TOKENS = (
    "Status: seeded",
    "Source evidence files consulted",
    "Behavior facts extracted",
    "Generated matrix rows created",
    "ScratchBird implementation files touched",
    "Static-copy checks",
    "Runtime link dependency checks",
    "Final clean-room conclusion",
)

FORBIDDEN_SOURCE_TOKENS = (
    "The contents of this file are subject to the InterBase Public License",
    "Initial Developer",
    "Firebird Project",
    "Mozilla Public License",
    "#include <ibase.h>",
    "#include \"ibase.h\"",
    "#include <firebird/",
    "#include \"firebird/",
    "libfbclient",
    "libtommath",
    "libtomcrypt",
)

FORBIDDEN_LINK_TOKENS = (
    "libfbclient",
    "libtommath",
    "libtomcrypt",
    "firebird-5.0.4-release-src",
    "donor/firebird",
)


def require_tokens(path: Path, tokens: tuple[str, ...]) -> list[str]:
    text = path.read_text(errors="replace").lower()
    return [token for token in tokens if token.lower() not in text]


def check_implementation_sources(root: Path) -> list[str]:
    errors: list[str] = []
    if not root.exists():
        return [f"implementation source root missing: {root}"]
    for path in root.rglob("*"):
        if path.suffix not in {".cpp", ".hpp", ".h", ".cc", ".cxx"}:
            continue
        text = path.read_text(errors="replace")
        for token in FORBIDDEN_SOURCE_TOKENS:
            if token in text:
                errors.append(f"forbidden donor provenance token {token!r} in {path}")
    return errors


def check_binary(binary: Path) -> list[str]:
    if not binary.exists():
        return [f"binary missing: {binary}"]
    result = subprocess.run(
        ["ldd", str(binary)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output = result.stdout.lower()
    if result.returncode != 0 and "not a dynamic executable" not in output:
        return [f"ldd failed for {binary}: {result.stdout}"]
    return [
        f"forbidden donor runtime dependency {token} in {binary}"
        for token in FORBIDDEN_LINK_TOKENS
        if token in output
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", required=True)
    parser.add_argument("--audit", required=True)
    parser.add_argument("--source-root", action="append", required=True)
    parser.add_argument("--binary", nargs="+", required=True)
    args = parser.parse_args()

    errors: list[str] = []
    for token in require_tokens(Path(args.policy), POLICY_TOKENS):
        errors.append(f"clean-room policy missing token: {token}")
    for token in require_tokens(Path(args.audit), AUDIT_TOKENS):
        errors.append(f"clean-room audit missing token: {token}")
    for root in args.source_root:
        errors.extend(check_implementation_sources(Path(root)))
    for binary in args.binary:
        errors.extend(check_binary(Path(binary)))

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("validated Firebird clean-room provenance gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
