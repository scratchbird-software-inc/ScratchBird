#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-013B developer handoff implementation map gate."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys


ARTIFACTS = Path(__file__).resolve().parent
MAP_PATH = ARTIFACTS / "DEVELOPER_HANDOFF_IMPLEMENTATION_MAP.csv"
SURFACE_BACKLOG = ARTIFACTS / "SURFACE_IMPLEMENTATION_BACKLOG.csv"
ENGINE_BACKLOG = ARTIFACTS / "ENGINE_GAP_IMPLEMENTATION_BACKLOG.csv"
MEMBERSHIP = ARTIFACTS / "BATCH_ROW_MEMBERSHIP.csv"


REQUIRED_COLUMNS = {
    "surface_family",
    "surface_count",
    "registry_source",
    "generated_registry_symbols",
    "parser_files",
    "udr_files",
    "sblr_files",
    "server_files",
    "engine_files",
    "diagnostic_rendering_files",
    "test_files",
    "owning_batches",
    "ctest_labels",
    "owner_slice",
    "generated_files",
    "handwritten_files",
    "status",
    "notes",
}

PATH_COLUMNS = {
    "parser_files",
    "udr_files",
    "sblr_files",
    "server_files",
    "engine_files",
    "diagnostic_rendering_files",
    "test_files",
    "generated_files",
    "handwritten_files",
}


class Gate:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("sbsql_developer_handoff_implementation_map_gate: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("sbsql_developer_handoff_implementation_map_gate: passed")
        return 0


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def split_list(value: str) -> list[str]:
    return [item.strip() for item in value.split(";") if item.strip()]


def audit_map(repo: Path, gate: Gate) -> None:
    handoff = read_csv(MAP_PATH)
    surfaces = read_csv(SURFACE_BACKLOG)
    engine_gaps = read_csv(ENGINE_BACKLOG)
    membership = read_csv(MEMBERSHIP)

    gate.require(handoff, "developer handoff map is header-only")
    gate.require(REQUIRED_COLUMNS.issubset(handoff[0].keys()), "developer handoff map is missing required columns")
    families = sorted({row["family"] for row in surfaces})
    mapped_families = sorted(row["surface_family"] for row in handoff)
    gate.require(mapped_families == families, "developer handoff map does not exactly cover surface families")
    gate.require(sum(int(row["surface_count"]) for row in handoff) == len(surfaces),
                 "developer handoff map surface counts do not sum to surface backlog")

    counts: dict[str, int] = {}
    for row in surfaces:
        counts[row["family"]] = counts.get(row["family"], 0) + 1

    membership_families = {row["family"] for row in membership}
    gate.require(membership_families == set(families), "batch membership families do not match surface backlog")
    gate.require(len(engine_gaps) == 932, "engine gap backlog count changed from FSPE baseline")

    for row in handoff:
        family = row["surface_family"]
        gate.require(row["status"] == "complete", f"{family} row is not complete")
        gate.require(int(row["surface_count"]) == counts[family],
                     f"{family} surface count does not match backlog")
        for column in REQUIRED_COLUMNS - {"surface_count"}:
            gate.require(bool(row.get(column, "").strip()), f"{family} has blank {column}")
        gate.require(row["owner_slice"] == "FSPE-013B", f"{family} owner_slice is not FSPE-013B")
        gate.require("SbsqlSurfaceRegistry" in row["generated_registry_symbols"],
                     f"{family} missing generated registry symbol")
        gate.require("generated" in row["generated_files"],
                     f"{family} does not identify generated files")
        gate.require("project/src/" in row["handwritten_files"],
                     f"{family} does not identify handwritten source files")

        for column in PATH_COLUMNS:
            for item in split_list(row[column]):
                path = repo / item
                gate.require(path.exists(), f"{family} {column} path does not exist: {item}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    gate = Gate()
    audit_map(args.repo_root.resolve(), gate)
    return gate.finish()


if __name__ == "__main__":
    raise SystemExit(main())
