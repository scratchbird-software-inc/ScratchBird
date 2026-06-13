#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import pathlib
import sys


EXPECTED_CLI_TARGETS = {
    "sb_isql": "SBsql",
    "sb_admin": "SBadm",
    "sb_backup": "SBbak",
    "sb_security": "SBsec",
    "sb_verify": "SBdoc",
    "sbdriver_conformance": "SBcop",
}


def fail(message: str) -> int:
    print(f"cli_public_branding_smoke: FAIL: {message}", file=sys.stderr)
    return 1


def parse_manifest(path: pathlib.Path) -> dict[str, dict[str, str]]:
    rows = path.read_text(encoding="utf-8").splitlines()
    if not rows:
        return {}
    header = rows[0].split("|")
    parsed: dict[str, dict[str, str]] = {}
    for line in rows[1:]:
        if not line.strip():
            continue
        values = line.split("|")
        if len(values) != len(header):
            raise ValueError(f"manifest row has {len(values)} fields, expected {len(header)}")
        row = dict(zip(header, values))
        parsed[row["target"]] = row
    return parsed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    args = parser.parse_args()

    try:
        rows = parse_manifest(args.manifest)
    except (OSError, ValueError) as exc:
        return fail(str(exc))

    failures: list[str] = []
    for target, short_brand in EXPECTED_CLI_TARGETS.items():
        row = rows.get(target)
        if row is None:
            failures.append(f"{target}: missing from CLI branding manifest")
            continue
        if row.get("short_brand") != short_brand:
            failures.append(f"{target}: short_brand={row.get('short_brand')!r}, expected={short_brand!r}")
        if row.get("actual_output_name") != short_brand:
            failures.append(
                f"{target}: actual_output_name={row.get('actual_output_name')!r}, expected={short_brand!r}"
            )
        if row.get("scope") != "cli":
            failures.append(f"{target}: scope={row.get('scope')!r}, expected='cli'")

    unexpected = sorted(set(rows) - set(EXPECTED_CLI_TARGETS))
    if unexpected:
        failures.append("unexpected standalone CLI branding targets: " + ", ".join(unexpected))

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1

    print("cli_public_branding_smoke: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
