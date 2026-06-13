#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Engine adapter helpers for the public ScratchBird benchmark harness."""

from __future__ import annotations

import sys
from pathlib import Path


def ensure_scratchbird_driver() -> None:
    repo_root = Path(__file__).resolve().parents[6]
    driver_path = repo_root / "project" / "drivers" / "driver" / "python" / "src"
    text = str(driver_path)
    if text not in sys.path:
        sys.path.insert(0, text)
