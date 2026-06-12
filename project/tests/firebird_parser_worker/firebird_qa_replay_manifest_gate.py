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
from pathlib import Path


EXPECTED_CANDIDATE_COUNT = 1949
EXPECTED_FAMILY_COUNT = 6
EXPECTED_ASSET_COUNT = 171
EXPECTED_GATE = "firebird_original_regression_replay_gate"
EXPECTED_REPLAY_STATUS = "original_reference_replay_passed"
EXPECTED_ASSET_STATUS = "candidate_hashed_for_replay"


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-index", required=True)
    parser.add_argument("--replay-manifest", required=True)
    parser.add_argument("--family-manifest", required=True)
    parser.add_argument("--asset-manifest", required=True)
    args = parser.parse_args()

    index_path = Path(args.candidate_index)
    replay_path = Path(args.replay_manifest)
    family_path = Path(args.family_manifest)
    asset_path = Path(args.asset_manifest)

    index_rows = read_rows(index_path)
    replay_rows = read_rows(replay_path)
    family_rows = read_rows(family_path)
    asset_rows = read_rows(asset_path)

    if len(index_rows) != EXPECTED_CANDIDATE_COUNT:
        raise SystemExit(f"expected {EXPECTED_CANDIDATE_COUNT} candidate rows, found {len(index_rows)}")
    if len(replay_rows) != len(index_rows):
        raise SystemExit(f"expected {len(index_rows)} replay rows, found {len(replay_rows)}")
    if len(family_rows) != EXPECTED_FAMILY_COUNT:
        raise SystemExit(f"expected {EXPECTED_FAMILY_COUNT} family rows, found {len(family_rows)}")
    if len(asset_rows) != EXPECTED_ASSET_COUNT:
        raise SystemExit(f"expected {EXPECTED_ASSET_COUNT} asset rows, found {len(asset_rows)}")

    index_by_test_id = {row["test_id"]: row for row in index_rows}
    if len(index_by_test_id) != len(index_rows):
        raise SystemExit("candidate index contains duplicate test_id rows")

    for replay in replay_rows:
        test_id = replay["test_id"]
        indexed = index_by_test_id.get(test_id)
        if indexed is None:
            raise SystemExit(f"replay row references unknown test_id {test_id}")
        for field in ("relative_path", "sha256", "ctest_gate"):
            if replay[field] != indexed[field]:
                raise SystemExit(f"replay row {test_id} mismatches candidate index field {field}")
        if replay["origin_classification"] != indexed["classification"]:
            raise SystemExit(f"replay row {test_id} mismatches candidate classification")
        if replay["replay_status"] != EXPECTED_REPLAY_STATUS:
            raise SystemExit(f"replay row {test_id} has invalid replay_status {replay['replay_status']}")
        if replay["ctest_gate"] != EXPECTED_GATE:
            raise SystemExit(f"replay row {test_id} has invalid ctest_gate {replay['ctest_gate']}")
        if replay["source_index"] != index_path.name:
            raise SystemExit(f"replay row {test_id} does not identify source index {index_path.name}")

    family_total = 0
    for family in family_rows:
        family_total += int(family["candidate_test_count"])
        if family["classification"] != "candidate_original_firebird_qa_test_family":
            raise SystemExit(f"family {family['family_id']} has invalid classification")
        if family["replay_status"] != EXPECTED_REPLAY_STATUS:
            raise SystemExit(f"family {family['family_id']} has invalid replay_status")
        if family["ctest_gate"] != EXPECTED_GATE:
            raise SystemExit(f"family {family['family_id']} has invalid ctest_gate")
    if family_total != len(index_rows):
        raise SystemExit(f"family manifest totals {family_total}, expected {len(index_rows)}")

    for asset in asset_rows:
        if asset["status"] != EXPECTED_ASSET_STATUS:
            raise SystemExit(f"asset {asset['asset_id']} has invalid status")
        if asset["ctest_gate"] != EXPECTED_GATE:
            raise SystemExit(f"asset {asset['asset_id']} has invalid ctest_gate")
        if not asset["sha256"]:
            raise SystemExit(f"asset {asset['asset_id']} is missing sha256")

    print(
        "validated Firebird QA replay evidence: "
        f"{len(replay_rows)} replay rows, {len(family_rows)} families, {len(asset_rows)} assets"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
