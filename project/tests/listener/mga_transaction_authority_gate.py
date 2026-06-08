#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Gate ScratchBird MGA transaction authority and cluster-path boundaries."""

from __future__ import annotations

import pathlib
import re
import sys


ROOTS = [
    "src/transaction/mga",
    "src/storage",
    "src/server",
    "src/parsers/native",
    "src/parsers/shared/lowering",
    "tools/sb_transaction_probe",
    "tools/sb_single_node_transaction_probe",
]

SKIP_PARTS = {"build", "__pycache__", "." + "git"}
TEXT_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".py", ".yaml", ".yml", ".md"}

FORBIDDEN_PATTERNS = [
    ("MGA_NO_SQLITE_AUTHORITY", re.compile(r"#\s*include\s*[<\"]sqlite3\.h[>\"]|sqlite3_|ExecSqlite|PRAGMA\s+(foreign_keys|journal_mode|synchronous)|journal_mode\s*=", re.IGNORECASE)),
    ("MGA_NO_LEGACY_CLUSTER_PAGE_BUILD", re.compile(r"BuildClusterTransactionPageBodyPlaceholder|ParseClusterTransactionPageBodyPlaceholder|ClusterTransactionMetadataPlaceholder|cluster_transaction_placeholder")),
    ("MGA_NO_CLUSTER_PROFILE_SWITCH", re.compile(r"cluster_authority_available\s*\?\s*\"private_cluster\"")),
    ("MGA_NO_CLUSTER_PAGE_AUTHORIZATION", re.compile(r"SB-PAGE-MANAGER-CLUSTER-PAGE-AUTHORIZED|cluster_page_authorized")),
]

UNFINISHED_PATTERN = re.compile(r"\b(TODO|FIXME|NotImplemented|not implemented|placeholder|stub)\b", re.IGNORECASE)

REQUIRED_PATTERNS = [
    ("SERVER_REJECTS_CLUSTER_AUTHORITY_FLAG", pathlib.Path("src/server/sblr_admission.cpp"), "SBLR.CLUSTER_MAPPING.UNAVAILABLE"),
    ("DBOPEN_REJECTS_CLUSTER_MAPPING", pathlib.Path("src/storage/database/database_lifecycle.cpp"), "SB-DB-LIFECYCLE-CLUSTER-MAPPING-UNAVAILABLE"),
    ("PAGE_MANAGER_REJECTS_CLUSTER_MAPPING", pathlib.Path("src/storage/page/page_manager.cpp"), "SB-PAGE-MANAGER-CLUSTER-MAPPING-UNAVAILABLE"),
    ("NATIVE_PACKAGE_REJECTS_CLUSTER_MAPPING", pathlib.Path("src/parsers/native/v3/package/native_v3_parser_package.cpp"), "cluster_mapping_unavailable"),
    ("TXN_PROBE_DOES_NOT_TRAVERSE_CLUSTER", pathlib.Path("tools/sb_transaction_probe/main.cpp"), "cluster_path_traversed"),
    ("SNTXN_PROBE_DOES_NOT_TRAVERSE_CLUSTER", pathlib.Path("tools/sb_single_node_transaction_probe/main.cpp"), "cluster_path_traversed"),
]


def iter_files(project_root: pathlib.Path):
    for root in ROOTS:
        base = project_root / root
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_dir() or any(part in SKIP_PARTS for part in path.parts):
                continue
            if path.suffix in TEXT_SUFFIXES or path.name == "CMakeLists.txt":
                yield path


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: mga_transaction_authority_gate.py <project-root>", file=sys.stderr)
        return 2
    project_root = pathlib.Path(argv[1]).resolve()
    findings: list[str] = []

    for path in iter_files(project_root):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        rel = path.relative_to(project_root)
        for line_no, line in enumerate(text.splitlines(), 1):
            for code, pattern in FORBIDDEN_PATTERNS:
                if pattern.search(line):
                    findings.append(f"{rel}:{line_no}: {code}")
            if ("/transaction/mga/" in path.as_posix() or "/storage/" in path.as_posix()) and UNFINISHED_PATTERN.search(line):
                findings.append(f"{rel}:{line_no}: MGA_NO_PLACEHOLDER_OR_STUB")

    for code, rel_path, needle in REQUIRED_PATTERNS:
        path = project_root / rel_path
        if not path.exists():
            findings.append(f"{rel_path}:0: {code}: missing file")
            continue
        if needle not in path.read_text(encoding="utf-8"):
            findings.append(f"{rel_path}:0: {code}: missing {needle}")

    if findings:
        for finding in findings:
            print(finding)
        print(f"mga_transaction_authority_gate=failed findings={len(findings)}", file=sys.stderr)
        return 1
    print("mga_transaction_authority_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
