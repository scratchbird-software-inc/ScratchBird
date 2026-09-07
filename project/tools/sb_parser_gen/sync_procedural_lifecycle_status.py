#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Synchronize reviewed procedure-lifecycle implementation status.

This narrowly updates the public implementation backlog after the bounded
CREATE PROCEDURE/CALL/typed-NULL profile has passed its authenticated public
route and independent restart evidence.  It does not alter canonical language
identity, source status, ownership, batching, or any adjacent procedural row.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from procedural_lifecycle_generated_evidence import (
    PROCEDURAL_LIFECYCLE_BY_SURFACE_ID,
    validate_authoritative_runtime_inputs,
)


BACKLOG = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/"
    "artifacts/SURFACE_IMPLEMENTATION_BACKLOG.csv"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=".")
    args = parser.parse_args()
    root = Path(args.repo_root)
    validate_authoritative_runtime_inputs(root)
    path = root / BACKLOG
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        fieldnames = reader.fieldnames
        rows = list(reader)
    if not fieldnames or "status" not in fieldnames:
        raise ValueError("SURFACE_IMPLEMENTATION_BACKLOG schema drift")

    selected: set[str] = set()
    changed = 0
    for row in rows:
        surface_id = row.get("surface_id", "")
        evidence = PROCEDURAL_LIFECYCLE_BY_SURFACE_ID.get(surface_id)
        if evidence is None:
            continue
        if surface_id in selected:
            raise ValueError(f"duplicate procedural backlog row {surface_id}")
        selected.add(surface_id)
        if row.get("canonical_name") != evidence.canonical_name:
            raise ValueError(f"procedural backlog identity drift {surface_id}")
        if row.get("source_status") != "native_now":
            raise ValueError(f"procedural backlog source-status drift {surface_id}")
        if row.get("status") != "e2e_passed":
            row["status"] = "e2e_passed"
            changed += 1

    expected = set(PROCEDURAL_LIFECYCLE_BY_SURFACE_ID)
    if selected != expected:
        raise ValueError(
            f"procedural backlog coverage drift missing={sorted(expected - selected)}"
        )
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(
        "procedural_lifecycle_status= synchronized "
        f"rows={len(selected)} changed={changed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
