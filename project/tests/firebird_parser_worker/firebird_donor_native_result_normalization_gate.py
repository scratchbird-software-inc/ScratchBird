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
import tempfile
from pathlib import Path

from firebird_donor_native_harness import (
    changed_protected_lines,
    normalize_firebird_donor_output,
)


POLICY_MUST_CONTAIN = (
    "Absolute paths",
    "Temporary directory names",
    "Process identifiers",
    "Connection identifiers",
    "Transaction identifiers",
    "Page identifiers",
    "Attachment identifiers",
    "Timestamps and durations",
    "Hostnames and loopback addresses",
    "Object UUIDs",
    "Warning ordering",
    "SQL result values",
    "Column labels",
    "Type names",
    "Precision, scale, nullability, charset, collation, and descriptor metadata",
    "Status-vector symbolic names",
    "SQLCODE and SQLSTATE",
    "Affected row counts",
    "Command tags and service action results",
    "firebird_donor_native_result_normalization_gate",
)


def validate_policy(policy_path: Path) -> list[str]:
    text = policy_path.read_text()
    return [token for token in POLICY_MUST_CONTAIN if token not in text]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--policy", required=True)
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    policy_path = Path(args.policy).resolve()
    repo_root = Path(args.repo_root).resolve()
    missing = validate_policy(policy_path)
    if missing:
        for token in missing:
            print(f"normalization policy missing required token: {token}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="sb_firebird_norm_") as temp:
        temp_root = Path(temp).resolve()
        external_path = "/" + "var/tmp/firebird/qa/out.log"
        home_path = "/" + "home/buildbot/work/firebird/qa/out.log"
        windows_path = "C:" + "\\Users\\buildbot\\AppData\\Local\\Temp\\firebird\\qa\\out.log"
        sample = "\n".join(
            [
                f"RAW_PATH={repo_root}/build/donor/firebird-5.0.4-release-src/out.log",
                f"TEMP_PATH={temp_root}/case_017/out.log",
                f"UNTAGGED_REPO_PATH {repo_root}/build/donor/firebird-5.0.4-release-src/out.log",
                f"UNTAGGED_TEMP_PATH {temp_root}/case_017/out.log",
                f"UNTAGGED_EXTERNAL_PATH {external_path}",
                f"UNTAGGED_HOME_PATH {home_path}",
                f"UNTAGGED_WINDOWS_PATH {windows_path}",
                "TEMP_DIR_NAME sb_firebird_norm_abc123",
                "pid=42111",
                "process id: 42112",
                "connection id=31415",
                "transaction id=27182",
                "page id=65535",
                "attachment id=144",
                "timestamp=2026-05-08 19:37:57.123",
                "duration=42.75 ms",
                "host=scratchbird-host",
                "address=127.0.0.1",
                "object uuid=550e8400-e29b-41d4-a716-446655440000",
                "SQL_RESULT=550e8400-e29b-41d4-a716-446655440000",
                "SQL_RESULT=42111",
                "  sql_result=/var/tmp/this/value/must/not/change",
                "COLUMN_LABEL=pid",
                "TYPE_NAME=VARCHAR(42)",
                "DESCRIPTOR=precision=18 scale=2 nullable=false charset=UTF8 collation=UNICODE",
                "SQL_DESCRIPTOR=precision=18 scale=2 nullable=false charset=UTF8 collation=UNICODE",
                "PRECISION=18",
                "SCALE=2",
                "NULLABLE=false",
                "CHARSET=UTF8",
                "COLLATION=UNICODE",
                "STATUS_VECTOR=isc_sqlerr isc_token_err",
                "SQLCODE=-104",
                "SQLSTATE=42000",
                "AFFECTED_ROWS=3",
                "COMMAND_TAG=INSERT",
                "SERVICE_RESULT=backup_completed",
                "WARNING_CHAIN_EQUIVALENT_BEGIN",
                "warning: zeta",
                "warning: alpha",
                "WARNING_CHAIN_EQUIVALENT_END",
                "warning: outside-order-zeta",
                "warning: outside-order-alpha",
            ]
        )
        normalized = normalize_firebird_donor_output(
            sample,
            repo_root=repo_root,
            temp_root=temp_root,
            hostname="scratchbird-host",
        )

    expected_tokens = (
        "RAW_PATH=<PATH>",
        "TEMP_PATH=<PATH>",
        "UNTAGGED_REPO_PATH <PATH>",
        "UNTAGGED_TEMP_PATH <PATH>",
        "UNTAGGED_EXTERNAL_PATH <PATH>",
        "UNTAGGED_HOME_PATH <PATH>",
        "UNTAGGED_WINDOWS_PATH <PATH>",
        "TEMP_DIR_NAME <TEMP_DIR>",
        "pid=<PID>",
        "process id:<PID>",
        "connection id=<CONNECTION_ID>",
        "transaction id=<TRANSACTION_ID>",
        "page id=<PAGE_ID>",
        "attachment id=<ATTACHMENT_ID>",
        "timestamp=<TIMESTAMP>",
        "duration=<DURATION>",
        "host=<HOSTNAME>",
        "address=<LOOPBACK>",
        "object uuid=<OBJECT_UUID>",
    )
    for token in expected_tokens:
        if token not in normalized:
            print(f"normalization output missing expected token: {token}", file=sys.stderr)
            print(normalized, file=sys.stderr)
            return 1

    protected_changes = changed_protected_lines(sample, normalized)
    if protected_changes:
        print(
            "protected SQL-visible lines changed during normalization: "
            + "; ".join(protected_changes),
            file=sys.stderr,
        )
        return 1

    normalized_lines = normalized.splitlines()
    if normalized_lines.index("warning: alpha") > normalized_lines.index("warning: zeta"):
        print("equivalent warning block was not ordered deterministically", file=sys.stderr)
        return 1
    if normalized_lines.index("warning: outside-order-zeta") > normalized_lines.index(
        "warning: outside-order-alpha"
    ):
        print("warning order outside equivalent block was changed", file=sys.stderr)
        return 1

    print("validated Firebird donor-native result normalization policy and seed normalizer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
