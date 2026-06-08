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
import hashlib
import sys
from collections import Counter
from pathlib import Path


REQUIRED_CATALOG_HASH_FAMILIES = (
    "RDB$ catalog inventory rowset hash",
    "MON$ computed catalog surface hash",
    "builtin behavior inventory hash",
)


def read_dict_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def stable_hash(rows: list[dict[str, str]], fields: tuple[str, ...]) -> str:
    digest = hashlib.sha256()
    for row in sorted(rows, key=lambda item: tuple(item[field] for field in fields)):
        digest.update("|".join(row[field] for field in fields).encode("utf-8"))
        digest.update(b"\n")
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog-inventory", required=True)
    parser.add_argument("--builtin-inventory", required=True)
    parser.add_argument("--extraction-status", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    catalog_path = Path(args.catalog_inventory).resolve()
    builtin_path = Path(args.builtin_inventory).resolve()
    status_path = Path(args.extraction_status).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    catalog_rows = read_dict_rows(catalog_path)
    builtin_rows = read_dict_rows(builtin_path)
    if not catalog_rows:
        raise SystemExit("catalog inventory has no rows")
    if not builtin_rows:
        raise SystemExit("builtin inventory has no rows")

    status_text = status_path.read_text()
    missing = [
        family for family in REQUIRED_CATALOG_HASH_FAMILIES
        if family not in status_text
    ]
    if missing:
        for family in missing:
            print(f"extraction status missing hash family: {family}", file=sys.stderr)
        return 1

    rdb_rows = [row for row in catalog_rows if row["emulation_source"] == "RDB$"]
    mon_rows = [row for row in catalog_rows if row["object_name"].startswith("MON$")]
    by_kind = Counter(row["object_kind"] for row in catalog_rows)
    by_behavior = Counter(row["behavior_class"] for row in builtin_rows)

    if not rdb_rows:
        raise SystemExit("catalog inventory has no RDB$ rows")
    if not mon_rows:
        raise SystemExit("catalog inventory has no MON$ rows")

    output_rows = [
        {
            "rowset_family": "RDB$ catalog inventory rowset hash",
            "row_count": str(len(rdb_rows)),
            "sha256": stable_hash(
                rdb_rows,
                ("object_name", "object_kind", "seed_status", "emulation_source"),
            ),
            "source": str(catalog_path),
        },
        {
            "rowset_family": "MON$ computed catalog surface hash",
            "row_count": str(len(mon_rows)),
            "sha256": stable_hash(
                mon_rows,
                ("object_name", "object_kind", "seed_status", "emulation_source"),
            ),
            "source": str(catalog_path),
        },
        {
            "rowset_family": "builtin behavior inventory hash",
            "row_count": str(len(builtin_rows)),
            "sha256": stable_hash(
                builtin_rows,
                ("behavior_name", "behavior_class", "mapping_status"),
            ),
            "source": str(builtin_path),
        },
    ]

    report = output_dir / "firebird_catalog_rowset_hashes.csv"
    with report.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=("rowset_family", "row_count", "sha256", "source"),
        )
        writer.writeheader()
        writer.writerows(output_rows)

    summary = output_dir / "FIREBIRD_CATALOG_ROWSET_HASH_REPORT.md"
    summary.write_text(
        "# Firebird Catalog Rowset Hash Report\n\n"
        f"RDB$ rows: {len(rdb_rows)}\n\n"
        f"MON$ rows: {len(mon_rows)}\n\n"
        f"Catalog object kinds: {dict(sorted(by_kind.items()))}\n\n"
        f"Builtin behavior classes: {dict(sorted(by_behavior.items()))}\n"
    )
    print(
        "validated Firebird catalog and builtin rowset hashes: "
        f"{len(rdb_rows)} RDB$ rows, {len(mon_rows)} MON$ rows, "
        f"{len(builtin_rows)} builtin rows"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
