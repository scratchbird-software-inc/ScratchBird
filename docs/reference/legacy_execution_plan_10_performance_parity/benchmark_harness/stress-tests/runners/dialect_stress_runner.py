#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Dialect stress runner entry point for public ScratchBird benchmark gates."""

from __future__ import annotations

from pathlib import Path


def current_python_driver_source(repo_root: Path) -> Path:
    return repo_root / "project" / "drivers" / "driver" / "python" / "src"


if __name__ == "__main__":
    print(current_python_driver_source(Path.cwd()))
