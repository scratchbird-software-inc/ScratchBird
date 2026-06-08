#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-014A source-size and maintainability gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


SOURCE_ROOTS = (
    "project/src/parsers/sbsql_worker",
    "project/src/udr/sbu_sbsql_parser_support",
    "project/src/server",
    "project/src/wire/parser_server_ipc",
    "project/src/engine",
)

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}
HANDWRITTEN_LIMIT_BYTES = 128 * 1024
GENERATED_LIMIT_BYTES = 3 * 1024 * 1024

EXCEPTIONS = {
    "project/src/engine/functions/registry/function_seed_registry.cpp": {
        "max_bytes": 2_500_000,
        "reason": "deterministic canonical function and name seed data from FSPE-013A",
    },
}


class Gate:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("sbsql_source_size_maintainability_gate: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("sbsql_source_size_maintainability_gate: passed")
        return 0


def source_files(repo: Path) -> list[Path]:
    files: list[Path] = []
    for root in SOURCE_ROOTS:
        base = repo / root
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                files.append(path)
    return sorted(files)


def rel(path: Path, repo: Path) -> str:
    return path.relative_to(repo).as_posix()


def is_generated(path_text: str) -> bool:
    return "/generated/" in path_text or path_text.endswith(".generated.cpp") or path_text.endswith(".generated.hpp")


def audit(repo: Path, gate: Gate) -> None:
    files = source_files(repo)
    gate.require(files, "no implementation source files found")
    seen_exceptions: set[str] = set()

    for path in files:
        path_text = rel(path, repo)
        size = path.stat().st_size
        if path_text in EXCEPTIONS:
            seen_exceptions.add(path_text)
            gate.require(size <= EXCEPTIONS[path_text]["max_bytes"],
                         f"{path_text} exceeds exception limit {EXCEPTIONS[path_text]['max_bytes']} bytes")
            continue
        if is_generated(path_text):
            gate.require(size <= GENERATED_LIMIT_BYTES,
                         f"{path_text} exceeds generated limit {GENERATED_LIMIT_BYTES} bytes")
            continue
        gate.require(size <= HANDWRITTEN_LIMIT_BYTES,
                     f"{path_text} exceeds handwritten limit {HANDWRITTEN_LIMIT_BYTES} bytes")

    for path_text in EXCEPTIONS:
        gate.require(path_text in seen_exceptions, f"declared source-size exception missing: {path_text}")

    largest = sorted(((path.stat().st_size, rel(path, repo)) for path in files), reverse=True)[:10]
    print("largest_sources:")
    for size, path_text in largest:
        marker = " exception" if path_text in EXCEPTIONS else " generated" if is_generated(path_text) else ""
        print(f"  {size} {path_text}{marker}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    gate = Gate()
    audit(args.repo_root.resolve(), gate)
    return gate.finish()


if __name__ == "__main__":
    raise SystemExit(main())
