#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import sys
from pathlib import Path

from firebird_donor_native_harness import normalize_firebird_donor_output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    donor = "\n".join(
        [
            f"RAW_PATH={repo_root}/build/donor/firebird/case.log",
            "timestamp=2026-05-08 19:00:00.001",
            "SQL_RESULT=42",
            "SQLSTATE=00000",
            "AFFECTED_ROWS=1",
        ]
    )
    scratchbird = "\n".join(
        [
            f"RAW_PATH={output_dir}/scratchbird/case.log",
            "timestamp=2026-05-08 20:00:00.999",
            "SQL_RESULT=42",
            "SQLSTATE=00000",
            "AFFECTED_ROWS=1",
        ]
    )
    donor_normalized = normalize_firebird_donor_output(
        donor,
        repo_root=repo_root,
        temp_root=output_dir,
    )
    scratchbird_normalized = normalize_firebird_donor_output(
        scratchbird,
        repo_root=repo_root,
        temp_root=output_dir,
    )
    (output_dir / "donor.normalized.txt").write_text(donor_normalized)
    (output_dir / "scratchbird.normalized.txt").write_text(scratchbird_normalized)
    if donor_normalized != scratchbird_normalized:
        print("normalized compatible outputs did not match", file=sys.stderr)
        print(donor_normalized, file=sys.stderr)
        print(scratchbird_normalized, file=sys.stderr)
        return 1

    changed_result = scratchbird.replace("SQL_RESULT=42", "SQL_RESULT=43")
    changed_normalized = normalize_firebird_donor_output(
        changed_result,
        repo_root=repo_root,
        temp_root=output_dir,
    )
    if donor_normalized == changed_normalized:
        print("diff oracle masked SQL_RESULT mismatch", file=sys.stderr)
        return 1

    print("validated Firebird donor-native diff oracle seed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
