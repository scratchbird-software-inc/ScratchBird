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
import re
import sys
from pathlib import Path


SHA256 = re.compile(r"^[0-9a-f]{64}$")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--regression-root", required=True)
    args = parser.parse_args()

    regression_root = Path(args.regression_root)
    candidate_doc = regression_root / "FIREBIRD_QA_CANDIDATE.md"
    source_pointers = regression_root / "SOURCE_POINTERS.md"
    test_index = regression_root / "FIREBIRD_QA_CANDIDATE_TEST_INDEX.csv"

    errors: list[str] = []
    for path in (candidate_doc, source_pointers, test_index):
      if not path.exists():
          errors.append(f"missing required Firebird QA evidence file: {path}")

    if not errors:
        doc_text = candidate_doc.read_text()
        pointers_text = source_pointers.read_text()
        for needle in (
            "84d137de5cfdc59ecf392b22db15f4d014a5a150",
            "Python test files: `1949`",
            "FIREBIRD_QA_CANDIDATE_TEST_INDEX.csv",
        ):
            if needle not in doc_text and needle not in pointers_text:
                errors.append(f"candidate evidence missing {needle}")

    rows = []
    if test_index.exists():
        with test_index.open(newline="") as handle:
            reader = csv.DictReader(handle)
            rows = list(reader)
        required = {"test_id", "relative_path", "sha256", "classification", "status", "ctest_gate"}
        if set(reader.fieldnames or []) != required:
            errors.append(f"unexpected Firebird QA test index columns: {reader.fieldnames}")
        if len(rows) != 1949:
            errors.append(f"expected 1949 Firebird QA tests, found {len(rows)}")
        seen_ids: set[str] = set()
        for row in rows:
            if row["test_id"] in seen_ids:
                errors.append(f"duplicate test_id {row['test_id']}")
                break
            seen_ids.add(row["test_id"])
            if not row["relative_path"].startswith("tests/") or not row["relative_path"].endswith("_test.py"):
                errors.append(f"invalid test path {row['relative_path']}")
                break
            if not SHA256.match(row["sha256"]):
                errors.append(f"invalid sha256 for {row['relative_path']}")
                break
            if row["ctest_gate"] != "firebird_original_regression_replay_gate":
                errors.append(f"invalid CTest gate for {row['relative_path']}")
                break

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(f"validated Firebird QA candidate index: {len(rows)} tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
