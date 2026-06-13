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
