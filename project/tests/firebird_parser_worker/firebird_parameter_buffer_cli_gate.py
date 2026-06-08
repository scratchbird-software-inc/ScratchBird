#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import json
import subprocess
import sys
from pathlib import Path


def run_case(binary: Path, kind: str, payload_hex: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), "--decode-parameter-buffer", kind, payload_hex],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def expect_ok(binary: Path, kind: str, payload_hex: str, required: tuple[str, ...]) -> bool:
    result = run_case(binary, kind, payload_hex)
    if result.returncode != 0:
        print(f"{kind} decode failed unexpectedly", file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        return False
    decoded = json.loads(result.stdout)
    if not decoded.get("ok"):
        print(f"{kind} decode did not report ok=true: {result.stdout}", file=sys.stderr)
        return False
    rendered = result.stdout
    missing = [token for token in required if token not in rendered]
    if missing:
        print(f"{kind} decode missing tokens: {missing}", file=sys.stderr)
        print(rendered, file=sys.stderr)
        return False
    return True


def expect_fail(binary: Path, kind: str, payload_hex: str, diagnostic_code: str) -> bool:
    result = run_case(binary, kind, payload_hex)
    if result.returncode == 0:
        print(f"{kind} bad payload was accepted", file=sys.stderr)
        print(result.stdout, file=sys.stderr)
        return False
    decoded = json.loads(result.stderr)
    if decoded.get("diagnostic_code") != diagnostic_code:
        print(f"{kind} diagnostic mismatch: {result.stderr}", file=sys.stderr)
        return False
    if decoded.get("runtime_policy") != "fail_closed":
        print(f"{kind} failure did not report fail_closed: {result.stderr}", file=sys.stderr)
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--parser-worker", required=True)
    args = parser.parse_args()
    binary = Path(args.parser_worker).resolve()
    if not binary.exists():
        print(f"parser worker does not exist: {binary}", file=sys.stderr)
        return 1

    cases = [
        expect_ok(
            binary,
            "DPB",
            "01:1c:06:53:59:53:44:42:41:30:04:55:54:46:38",
            ("isc_dpb_user_name", "isc_dpb_lc_ctype", "descriptor_only_no_engine_authority"),
        ),
        expect_ok(
            binary,
            "TPB",
            "03-0f-11-06-08",
            ("isc_tpb_read_committed", "isc_tpb_rec_version", "isc_tpb_wait", "isc_tpb_read"),
        ),
        expect_ok(
            binary,
            "SPB",
            "02_0b_6a_08_65_6d_70_6c_6f_79_65_65",
            ("isc_action_svc_db_stats", "isc_spb_dbname", "emulated_service_or_authority_diagnostic"),
        ),
        expect_ok(
            binary,
            "BLR",
            "05:02:ff:4c",
            ("blr_begin", "saw_message", "descriptor_only_no_engine_authority"),
        ),
        expect_ok(
            binary,
            "MESSAGE_BLR",
            "05:02:04:00:02:00:08:00:07:00:ff:4c",
            ("blr_message", "message_slot_count", "blr_long", "null_indicator"),
        ),
        expect_ok(
            binary,
            "SQLDA",
            "01:00:58:53:51:4c:44:41:20:20:22:00:00:00:02:00:02:00:"
            "f0:01:00:00:00:00:04:00:c5:01:00:00:00:00:0a:00",
            ("SQL_LONG", "SQL_TEXT", "nullable", "descriptor_only_no_engine_authority"),
        ),
        expect_fail(binary, "DPB", "01:1c:04:53", "FIREBIRD.WIRE.CLUMPLET_LENGTH_INVALID"),
        expect_fail(binary, "TPB", "03ff", "FIREBIRD.WIRE.UNKNOWN_TAG"),
        expect_fail(
            binary,
            "MESSAGE_BLR",
            "05:02:04:00:02:00:08:4c",
            "FIREBIRD.WIRE.BLR_TRUNCATED",
        ),
        expect_fail(binary, "SQLDA", "01:00", "FIREBIRD.WIRE.SQLDA_TRUNCATED"),
        expect_fail(binary, "DPB", "1", "FIREBIRD.WIRE.HEX_ODD_LENGTH"),
        expect_fail(binary, "DPB", "zz", "FIREBIRD.WIRE.HEX_INVALID"),
    ]
    if not all(cases):
        return 1

    print("validated sbp_firebird parameter-buffer decode CLI")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
