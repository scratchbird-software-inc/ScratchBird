#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import csv
import sys
from pathlib import Path


EXPECTED_PROFILES = {
    "firebird_2_5",
    "firebird_3_x",
    "firebird_4_x",
    "firebird_5_0_4",
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix", required=True)
    args = parser.parse_args()

    matrix = Path(args.matrix).resolve()
    with matrix.open(newline="") as handle:
        rows = list(csv.DictReader(handle))

    errors: list[str] = []
    profile_ids = {row["profile_id"] for row in rows}
    if profile_ids != EXPECTED_PROFILES:
        errors.append(f"profile id set mismatch: {sorted(profile_ids)}")
    for row in rows:
        for field in (
            "source_profile",
            "release_status",
            "surface_family",
            "implementation_requirement",
            "emulation_requirement",
            "diagnostic_requirement",
            "ctest_gate",
            "status",
        ):
            if not row.get(field):
                errors.append(f"{row.get('profile_id', '<missing>')} missing {field}")
        if row.get("ctest_gate") != "firebird_version_profile_gate":
            errors.append(f"{row.get('profile_id')} has wrong ctest_gate")
        if row.get("status") != "completed":
            errors.append(f"{row.get('profile_id')} status must be completed")
        if "zero_unassigned_rows" not in row.get("emulation_requirement", ""):
            errors.append(f"{row.get('profile_id')} missing zero-unassigned requirement")

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(f"validated Firebird version profile matrix with {len(rows)} profiles")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
