#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ORH-901 source-backed successor runtime readiness validator."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


REQUIRED_SOURCE_TOKENS = {
    "project/tests/performance/optimizer_runtime_hot_path_orh_004_gate.cpp": {
        "hash_dml_write_requires_explicit_hash_route=true",
        "KEYED_HASH_REQUIRED",
        "hash DML update route did not expose keyed write support",
    },
    "project/tests/performance/optimizer_runtime_hot_path_orh_120_gate.cpp": {
        "benchmark_clean_index_runtime_closure",
        "document_path_index_runtime_proven=true",
        "document_path_index_runtime_strict_route",
    },
    "project/tests/performance/optimizer_runtime_hot_path_orh_121_gate.cpp": {
        "allow_minimal_resource_bootstrap:true",
        "CreateLifecycleSchemaAndTable",
    },
    "project/tests/performance/CMakeLists.txt": {
        "optimizer_runtime_hot_path_orh_004_gate",
        "optimizer_runtime_hot_path_orh_120_gate",
        "optimizer_runtime_hot_path_orh_121_gate",
        "optimizer_runtime_hot_path_orh_901_gate",
    },
}

FORBIDDEN_SOURCE_TOKENS = {
    "optimizer_runtime_hot_path_orh_901_gate.py": {
        "completed-" "execution-plans",
        "execution-" "plans/optimizer-runtime-hot-path",
        "FINAL_AUDIT" ".md",
        "TRACKER" ".csv",
    },
}


def fail(message: str) -> None:
    print(f"ORH-901 gate failure: {message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read(repo_root: Path, rel_path: str) -> str:
    path = repo_root / rel_path
    require(path.exists(), f"required source file missing: {rel_path}")
    return path.read_text(encoding="utf-8", errors="replace")


def validate(repo_root: Path) -> None:
    for rel_path, tokens in REQUIRED_SOURCE_TOKENS.items():
        text = read(repo_root, rel_path)
        for token in tokens:
            require(token in text, f"{rel_path} missing token: {token}")

    for rel_path, tokens in FORBIDDEN_SOURCE_TOKENS.items():
        text = read(repo_root, "project/tests/performance/" + rel_path)
        for token in tokens:
            require(token not in text, f"{rel_path} contains stale public-plan dependency: {token}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    args = parser.parse_args()
    validate(args.repo_root.resolve())
    print("ORH-901 source-backed runtime readiness gate passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
