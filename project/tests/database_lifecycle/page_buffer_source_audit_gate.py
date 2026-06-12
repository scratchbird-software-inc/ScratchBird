#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""MMCH-051 source audit for page-buffer allocation ownership.

This is a production-claim gate, not a parser/client/reference authority source.
It rejects raw page-buffer heap allocation in storage paths and verifies that
the approved MemoryManager-backed page-frame APIs are present.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


SEARCH_KEY = "MMCH_NO_RAW_PAGE_BUFFER_AUDIT"

RAW_ALLOC_RE = re.compile(
    r"\b(?:std::)?(?:malloc|calloc|realloc|aligned_alloc|posix_memalign|_aligned_malloc)\s*\("
)
RAW_NEW_RE = re.compile(
    r"\bnew\s+(?:std::)?(?:byte|char|unsigned\s+char|std::uint8_t|uint8_t)\s*\["
)
PAGE_CONTEXT_RE = re.compile(
    r"page|Page|PAGE|buffer|Buffer|BUFFER|page_size|pageSize|PageBuffer"
)

ALLOWED_RAW_ALLOC_FILES = {
    pathlib.Path("project/src/core/memory/memory.cpp"),
}

REQUIRED_ANCHORS = {
    pathlib.Path("project/src/storage/page/page_manager.cpp"): (
        "AllocateManagedPageBuffer",
        "AllocateScopedPageBuffer",
    ),
    pathlib.Path("project/src/storage/page/page_cache.cpp"): (
        "MMCH_PAGE_CACHE_FRAME_OWNERSHIP",
        "AllocateScopedPageBuffer",
    ),
    pathlib.Path("project/src/core/memory/memory.hpp"): (
        "ScopedPageBuffer",
        "PageBufferRequest",
    ),
}


def relative(path: pathlib.Path, root: pathlib.Path) -> pathlib.Path:
    return path.resolve().relative_to(root.resolve())


def source_files(root: pathlib.Path) -> list[pathlib.Path]:
    scan_roots = [
        root / "project/src/storage",
        root / "project/src/engine/internal_api/dml",
        root / "project/src/core/index",
    ]
    files: list[pathlib.Path] = []
    for scan_root in scan_roots:
        if not scan_root.exists():
            continue
        files.extend(
            path
            for path in scan_root.rglob("*")
            if path.suffix in {".cpp", ".hpp", ".cc", ".h"}
        )
    return sorted(files)


def has_page_context(lines: list[str], index: int) -> bool:
    start = max(0, index - 2)
    end = min(len(lines), index + 3)
    return any(PAGE_CONTEXT_RE.search(lines[i]) for i in range(start, end))


def audit_raw_allocations(root: pathlib.Path) -> list[str]:
    findings: list[str] = []
    for path in source_files(root):
        rel = relative(path, root)
        if rel in ALLOWED_RAW_ALLOC_FILES:
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            findings.append(f"{rel}: binary or non-UTF8 source cannot be audited")
            continue
        for index, line in enumerate(lines):
            if RAW_ALLOC_RE.search(line):
                findings.append(
                    f"{rel}:{index + 1}: raw allocator call is not allowed for storage page-buffer paths"
                )
                continue
            if RAW_NEW_RE.search(line) and has_page_context(lines, index):
                findings.append(
                    f"{rel}:{index + 1}: raw byte array allocation near page-buffer context is not allowed"
                )
    return findings


def audit_required_anchors(root: pathlib.Path) -> list[str]:
    findings: list[str] = []
    for rel, tokens in REQUIRED_ANCHORS.items():
        path = root / rel
        if not path.exists():
            findings.append(f"{rel}: required approved allocation anchor file missing")
            continue
        text = path.read_text(encoding="utf-8")
        for token in tokens:
            if token not in text:
                findings.append(f"{rel}: required anchor token missing: {token}")
    return findings


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = pathlib.Path(args.repo_root)

    findings = []
    findings.extend(audit_required_anchors(root))
    findings.extend(audit_raw_allocations(root))

    print(
        f"{SEARCH_KEY}: authority_note=source_audit_evidence_only;"
        "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
    )
    if findings:
        for finding in findings:
            print(f"MMCH-051 finding: {finding}", file=sys.stderr)
        return 1
    print("MMCH-051 passed: no raw page-buffer allocations outside approved APIs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
