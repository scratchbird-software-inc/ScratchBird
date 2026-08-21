#!/usr/bin/env python3
"""Validate synthetic SBsql command rows against the Core exact-refusal contract."""

import csv
import pathlib
import sys


def load(path):
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gate <command-audit.csv> <core-zero-grey.csv>", file=sys.stderr)
        return 2
    audit = load(pathlib.Path(sys.argv[1]))
    core = {row["surface_id"]: row for row in load(pathlib.Path(sys.argv[2]))}
    checked = 0
    for row in audit:
        if not row["surface_id"].startswith("SBSQL-CMD-"):
            continue
        checked += 1
        contract = core.get(row["surface_id"])
        if contract is None:
            raise SystemExit(f"missing Core command contract: {row['surface_id']}")
        result_shape = row["result_authority"].split("#shape=", 1)[-1]
        expected = {
            "canonical_name": row["canonical_name"],
            "root_route": row["root_route"],
            "result_shape": result_shape,
        }
        for field, value in expected.items():
            if contract.get(field) != value:
                raise SystemExit(
                    f"{row['surface_id']} {field}: expected {value!r}, "
                    f"got {contract.get(field)!r}"
                )
    if checked == 0:
        raise SystemExit("no synthetic SBsql command rows were checked")
    print(f"SBSQL command zero-grey audit passed: synthetic_rows={checked}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
