#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static gate for the public parser/SBLR operation matrix closure."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def parse_matrix(path: Path) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    current: dict[str, str] | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        start = re.match(r"\s*-\s+sblr_operation:\s*(\S+)", line)
        if start:
            if current:
                entries.append(current)
            current = {"sblr_operation": start.group(1)}
            continue
        field = re.match(r"\s+([a-zA-Z0-9_]+):\s*(.+?)\s*$", line)
        if field and current is not None:
            current[field.group(1)] = field.group(2).strip('"')
    if current:
        entries.append(current)
    return entries


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root)
    matrix_path = root / "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"
    dispatch_path = root / "project/src/engine/sblr/sblr_dispatch.cpp"
    lowering_path = root / "project/src/parsers/sbsql_worker/lowering/lowering.cpp"
    udr_path = root / "project/src/udr/sbu_sbsql_parser_support/sbu_sbsql_parser_support.cpp"
    for path in (matrix_path, dispatch_path, lowering_path, udr_path):
        if not path.is_file():
            fail(f"required file missing: {path}")

    dispatch = dispatch_path.read_text(encoding="utf-8")
    lowering = lowering_path.read_text(encoding="utf-8")
    udr = udr_path.read_text(encoding="utf-8")
    entries = parse_matrix(matrix_path)
    if len(entries) < 100:
        fail(f"SBLR operation matrix unexpectedly small: {len(entries)}")

    missing: list[str] = []
    implemented = 0
    for entry in entries:
        operation = entry.get("sblr_operation", "")
        api_operation = entry.get("api_operation_id", "")
        ready = entry.get("executor_readiness_status", "")
        status = entry.get("current_implementation_status", "")
        if ready != "mapped_ready" and status != "behavior_implemented":
            continue
        implemented += 1
        if operation not in dispatch:
            missing.append(f"{api_operation} missing opcode {operation} in sblr_dispatch.cpp")
        if api_operation and api_operation not in dispatch:
            missing.append(f"{api_operation} missing dispatch branch")
        if entry.get("scope_status", "").startswith("cluster") and "cluster_authority_unavailable" not in dispatch:
            missing.append(f"{api_operation} missing cluster fail-closed dispatch evidence")
    if missing:
        fail("\n".join(missing[:80]))
    if implemented < 100:
        fail(f"implemented/mapped operation coverage unexpectedly small: {implemented}")

    required_lowering_evidence = [
        "SBLRExecutionEnvelope.v3",
        "source_payload_embedded",
        "parser_executes_sql",
        "SBSQL.SBLR.SQL_TEXT_EMBEDDED",
        "authority.parser.no_storage_or_finality",
    ]
    for token in required_lowering_evidence:
        if token not in lowering:
            fail(f"lowering evidence missing {token}")

    required_udr_evidence = [
        "sbu_sbsql_parse_to_sblr",
        "sbu_sbsql_describe_statement",
        "UDR.SBSQL.CONTEXT_MISSING",
        "engine_context=trusted",
    ]
    for token in required_udr_evidence:
        if token not in udr:
            fail(f"trusted UDR bridge evidence missing {token}")

    print(
        "sbsql_sblr_operation_matrix_static_gate=passed "
        f"entries={len(entries)} mapped={implemented}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
