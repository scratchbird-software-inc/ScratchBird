#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Engine-owned database bootstrap support for live CDP route gates."""

from __future__ import annotations

import subprocess
from pathlib import Path


PUBLIC_TEST_PASSWORD = "ScratchBird-E2E-2026!"


class CdpDatabaseLifecycleError(RuntimeError):
    pass


def seed_database(
    *,
    database_seed: str,
    resource_seed_pack_root: str,
    database: Path,
    evidence_root: Path,
    fixture_label: str,
) -> Path:
    """Create a complete database through the production lifecycle authority."""

    database.parent.mkdir(parents=True, exist_ok=True)
    evidence_root.mkdir(parents=True, exist_ok=True)
    manifest = evidence_root / f"{fixture_label}.database-seed-manifest.json"
    stdout_path = evidence_root / f"{fixture_label}.database-seed.out"
    stderr_path = evidence_root / f"{fixture_label}.database-seed.err"
    completed = subprocess.run(
        [
            database_seed,
            "--output",
            str(database),
            "--manifest",
            str(manifest),
            "--resource-seed-pack-root",
            resource_seed_pack_root,
            "--overwrite",
        ],
        stdout=stdout_path.open("wb"),
        stderr=stderr_path.open("wb"),
        check=False,
        timeout=35,
    )
    if completed.returncode != 0:
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace").strip()
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace").strip()
        raise CdpDatabaseLifecycleError(
            f"{fixture_label} database seed failed rc={completed.returncode} "
            f"stdout={stdout!r} stderr={stderr!r}"
        )
    if not database.is_file() or not manifest.is_file():
        raise CdpDatabaseLifecycleError(
            f"{fixture_label} database seed omitted database or manifest evidence"
        )
    prohibited_sidecars = (
        Path(str(database) + ".sb.local_password_auth"),
        Path(str(database) + ".sb.security_principal_events"),
    )
    present_sidecars = [str(path) for path in prohibited_sidecars if path.exists()]
    if present_sidecars:
        raise CdpDatabaseLifecycleError(
            f"{fixture_label} database seed created forbidden security sidecars: "
            + ", ".join(present_sidecars)
        )
    return manifest
