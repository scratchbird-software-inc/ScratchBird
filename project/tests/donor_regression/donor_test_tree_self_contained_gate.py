#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Fail when donor parser test metadata depends on out-of-tree assets."""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import re
import sys


ALLOWED_TEST_PREFIX = "project/tests/"
DONOR_TEST_PREFIX = "project/tests/donor_regression/"
FORBIDDEN_TEXT = (
    "docs/" + "reference",
    "ScratchBird" + "-Private",
    "audit_" + "local_reference:",
    "audit_" + "external_reference:",
    "/" + "home/",
    "\\" + "home\\",
    "project_" + "clones",
)
SKIP_DIRS = {
    ".git",
    "__pycache__",
    "donor_release_acquisition",
    "original_firebird_qa",
}
PATH_COLUMN_RE = re.compile(
    r"(?:^|_)(?:path|paths|locator|root|source|destination|references|required|manifest)(?:_|$)"
)


def is_binary(path: pathlib.Path) -> bool:
    return b"\0" in path.read_bytes()[:8192]


def iter_owned_files(root: pathlib.Path):
    for current_root, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for filename in files:
            path = pathlib.Path(current_root) / filename
            if path.suffix in {".pyc", ".o", ".obj"}:
                continue
            yield path


def split_path_items(value: str) -> list[str]:
    items: list[str] = []
    for chunk in value.replace(",", ";").split(";"):
        item = chunk.strip().strip('"').strip("'")
        if item:
            items.append(item)
    return items


def check_text(rel: str, text: str, failures: list[str]) -> None:
    for token in FORBIDDEN_TEXT:
        if token in text:
            failures.append(f"{rel}: forbidden out-of-tree token {token}")


def check_csv_paths(path: pathlib.Path, rel: str, failures: list[str]) -> None:
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            return
        for line_number, row in enumerate(reader, start=2):
            for column, value in row.items():
                if not value or column is None:
                    continue
                if not PATH_COLUMN_RE.search(column):
                    continue
                for item in split_path_items(value):
                    if item in {"", "forbidden", "no_tool_recorded", "not_applicable"}:
                        continue
                    if item.startswith(("donor.", "scratchbird.", "SB", "normalized_")):
                        continue
                    if item.startswith(("http://", "https://")):
                        failures.append(f"{rel}:{line_number}: URL path in {column}: {item}")
                        continue
                    if item.startswith("/") or item.startswith("\\"):
                        failures.append(f"{rel}:{line_number}: absolute path in {column}: {item}")
                        continue
                    if item.startswith("docs/" + "reference"):
                        blocked = "docs/" + "reference"
                        failures.append(f"{rel}:{line_number}: {blocked} path in {column}: {item}")
                        continue


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo = pathlib.Path(args.repo_root).resolve()
    donor_root = repo / DONOR_TEST_PREFIX
    if not donor_root.is_dir():
        print(f"missing donor test root: {donor_root}", file=sys.stderr)
        return 1

    failures: list[str] = []
    for path in iter_owned_files(donor_root):
        rel = path.relative_to(repo).as_posix()
        if is_binary(path):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        check_text(rel, text, failures)
        if path.suffix == ".csv":
            check_csv_paths(path, rel, failures)

    if failures:
        print("\n".join(failures[:300]))
        if len(failures) > 300:
            print(f"... {len(failures) - 300} more")
        return 1

    print("donor_test_tree_self_contained_gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
