#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate normalized replay evidence shape."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


HEX64_RE = re.compile(r"^[0-9a-f]{64}$")
VALID_GATE_STATUSES = {"passed", "failed", "skipped"}
VALID_NORMALIZED_STATUSES = {"passed", "failed", "skipped", "refused", "unsupported"}
VALID_CLASSIFICATIONS = {
    "semantic_probe_passed",
    "semantic_suite_passed",
    "embedded_shell_and_parser_probe_passed",
    "environment_or_protocol_error",
    "mapped_failure",
    "mapped_failures",
    "resource_timeout",
    "public_no_payload_mode",
    "firebird_original_tool_not_present",
    "firebird_original_suite_not_present",
    "firebird_driver_not_present",
    "firebird_qa_plugin_not_present",
    "apache_ignite_original_tool_not_present",
    "cassandra_original_tool_not_present",
    "clickhouse_original_tool_not_present",
    "cockroachdb_original_tool_not_present",
    "dolt_original_tool_not_present",
    "duckdb_original_tool_not_present",
    "foundationdb_original_tool_not_present",
    "immudb_original_tool_not_present",
    "influxdb_original_tool_not_present",
    "mariadb_original_tool_not_present",
    "milvus_original_tool_not_present",
    "mongodb_original_tool_not_present",
    "mysql_original_tool_not_present",
    "mysql_lts_original_tool_not_present",
    "neo4j_original_tool_not_present",
    "opensearch_original_tool_not_present",
    "opensearch_sql_ppl_original_tool_not_present",
    "postgresql_original_tool_not_present",
    "postgresql_original_suite_not_present",
    "pytest_not_present",
    "redis_original_tool_not_present",
    "sqlite_original_tool_not_present",
    "tidb_original_tool_not_present",
    "tikv_original_tool_not_present",
    "vitess_original_tool_not_present",
    "xtdb_original_tool_not_present",
    "yugabytedb_original_tool_not_present",
    "native_original_tool_replay_not_implemented_for_family",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def load_json(path: Path) -> dict[str, Any]:
    require(path.is_file(), f"missing evidence file: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"{path}: invalid JSON: {exc}")
    require(isinstance(payload, dict), f"{path}: payload must be a JSON object")
    return payload


def normalized_from(payload: dict[str, Any]) -> dict[str, Any] | None:
    if isinstance(payload.get("normalized_result"), dict):
        return payload["normalized_result"]
    tool = payload.get("tool")
    if isinstance(tool, dict) and isinstance(tool.get("normalized_result"), dict):
        return tool["normalized_result"]
    return None


def validate(path: Path) -> dict[str, Any]:
    payload = load_json(path)
    gate_status = payload.get("status")
    require(gate_status in VALID_GATE_STATUSES, f"{path}: invalid gate status {gate_status!r}")
    normalized = normalized_from(payload)
    require(normalized is not None, f"{path}: missing normalized_result object")
    status = normalized.get("status")
    classification = normalized.get("classification")
    command_id = normalized.get("command_id")
    output_sha256 = normalized.get("output_sha256")
    require(status in VALID_NORMALIZED_STATUSES, f"{path}: invalid normalized status {status!r}")
    require(
        classification in VALID_CLASSIFICATIONS,
        f"{path}: invalid classification {classification!r}",
    )
    require(isinstance(command_id, str) and command_id, f"{path}: command_id is required")
    require(isinstance(output_sha256, str) and HEX64_RE.match(output_sha256) is not None,
            f"{path}: output_sha256 must be hex sha256")
    canonicalization = normalized.get("canonicalization")
    require(isinstance(canonicalization, dict), f"{path}: canonicalization object is required")
    return {
        "path": str(path),
        "gate_status": gate_status,
        "normalized_status": status,
        "classification": classification,
        "command_id": command_id,
        "output_sha256": output_sha256,
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-file", required=True, type=Path)
    parser.add_argument("--output-file", type=Path)
    args = parser.parse_args(argv)

    result = validate(args.evidence_file)
    if args.output_file:
        args.output_file.parent.mkdir(parents=True, exist_ok=True)
        args.output_file.write_text(
            json.dumps(
                {
                    "gate": "reference_result_normalization_gate",
                    "status": "passed",
                    "validated": result,
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
    print(
        "reference_result_normalization_gate=passed "
        f"status={result['normalized_status']} classification={result['classification']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
