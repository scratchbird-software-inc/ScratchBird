#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-017 no-placeholder lifecycle gate."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


SCAN_ROOTS = (
    "project/src/storage/database",
    "project/src/server",
    "project/src/engine/internal_api/lifecycle",
    "project/src/transaction/mga",
    "project/src/parsers/donor/firebird",
    "project/src/parsers/sbsql_worker/statement",
    "project/src/parsers/sbsql_worker/lowering",
    "project/src/parsers/sbsql_worker/runtime",
)

TEXT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}

FORBIDDEN = (
    re.compile(r"\bTODO\b", re.IGNORECASE),
    re.compile(r"\bFIXME\b", re.IGNORECASE),
    re.compile(r"\bNotImplemented\b", re.IGNORECASE),
    re.compile(r"\bnot implemented\b", re.IGNORECASE),
    re.compile(r"\bstub\b", re.IGNORECASE),
    re.compile(r"\bplaceholder\b", re.IGNORECASE),
    re.compile(r"\bfuture work\b", re.IGNORECASE),
    re.compile(r"\bdeferred\b", re.IGNORECASE),
)

REQUIRED_TOKENS = {
    "project/src/server/listener_orchestrator.cpp": (
        "LISTENER.PLATFORM_UNAVAILABLE",
        "unavailable on this platform",
    ),
    "project/tests/database_lifecycle/fixtures/full_database_lifecycle_closure/"
    "artifacts/DATABASE_LIFECYCLE_HARDENING_REPORT.md": (
        "DBLC_STATIC_NO_LIFECYCLE_PLACEHOLDERS",
        "accepted lifecycle code paths",
    ),
}


def allowed_lifecycle_term(rel: str, line: str, token: str) -> bool:
    if token.lower() == "stub" and "cluster.provider.stub.v1" in line:
        return True
    if token.lower() == "deferred" and 'ContainsAdjacent(words, "INITIALLY", "DEFERRED")' in line:
        return True
    return False


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        raise AssertionError(f"missing required file: {path}") from None


def iter_sources(repo_root: Path):
    for rel_root in SCAN_ROOTS:
        root = repo_root / rel_root
        if not root.exists():
            raise AssertionError(f"missing scan root: {rel_root}")
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in TEXT_SUFFIXES:
                yield path


def assert_required_tokens(repo_root: Path) -> None:
    for rel, tokens in REQUIRED_TOKENS.items():
        text = read(repo_root / rel)
        for token in tokens:
            if token not in text:
                raise AssertionError(f"{rel}: missing required token {token!r}")


def assert_no_placeholders(repo_root: Path) -> None:
    findings: list[str] = []
    for path in iter_sources(repo_root):
        text = read(path)
        for line_no, line in enumerate(text.splitlines(), 1):
            for pattern in FORBIDDEN:
                match = pattern.search(line)
                if match:
                    rel = path.relative_to(repo_root).as_posix()
                    if allowed_lifecycle_term(rel, line, match.group(0)):
                        continue
                    findings.append(f"{rel}:{line_no}: {match.group(0)!r}")
    if findings:
        raise AssertionError("placeholder language remains: " + "; ".join(findings[:40]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()

    try:
        assert_required_tokens(repo_root)
        assert_no_placeholders(repo_root)
    except AssertionError as exc:
        print(f"DBLC_STATIC_NO_LIFECYCLE_PLACEHOLDERS=failed: {exc}", file=sys.stderr)
        return 1

    print("DBLC_STATIC_NO_LIFECYCLE_PLACEHOLDERS=passed")
    print("DBLC_P17_HARDENED=no-placeholder-evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
