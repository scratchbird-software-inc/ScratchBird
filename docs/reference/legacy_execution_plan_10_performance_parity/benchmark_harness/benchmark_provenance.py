#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Public ScratchBird benchmark harness provenance helpers."""

from __future__ import annotations

from pathlib import Path


def scratchbird_repo_root_candidates(harness_root: Path) -> list[Path]:
    repo_root = harness_root.resolve().parents[3]
    return [repo_root]


def scratchbird_server_binary_candidates(harness_root: Path) -> list[Path]:
    repo_root = harness_root.resolve().parents[3]
    return [repo_root / "build" / "src" / "server" / "sb_server"]


def scan_scratchbird_runtime_storage_policy(runtime_root: Path) -> dict[str, object]:
    """Reject embedded runtime storage artifacts in benchmark comparison inputs."""

    forbidden_suffixes = (
        ".native-sbwp.sqlite-wal",
        ".native-sbwp.sqlite-shm",
        ".sqlite-wal",
        ".sqlite-shm",
    )
    forbidden_names = {
        "sb_runtime.sqlite-wal",
        "sb_runtime.sqlite-shm",
    }
    runtime_root = runtime_root.resolve()
    forbidden: list[str] = []
    if runtime_root.exists():
        for path in runtime_root.rglob("*"):
            if not path.is_file():
                continue
            name = path.name
            if name in forbidden_names or any(name.endswith(suffix) for suffix in forbidden_suffixes):
                forbidden.append(str(path.relative_to(runtime_root)))

    if forbidden:
        return {
            "status": "failed",
            "comparison_eligible": False,
            "reason": "ScratchBird embedded storage/log artifacts are not valid benchmark comparison evidence",
            "forbidden_count": len(forbidden),
            "forbidden_artifacts": sorted(forbidden),
        }

    return {
        "status": "passed",
        "comparison_eligible": True,
        "reason": "no embedded ScratchBird runtime storage/log artifacts detected",
        "forbidden_count": 0,
        "forbidden_artifacts": [],
    }
