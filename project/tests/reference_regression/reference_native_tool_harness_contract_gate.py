#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate reference parser native-tool and client-replay contracts.

The public repo tracks harness manifests and acquisition instructions, not the
downloaded upstream payloads or locally built reference tools. This gate proves
that every public compatibility parser lane has a deterministic replay contract
that either points at a declared local native-tool slot or records the required
client-replay output bundle for lanes without a compiled reference tool.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import re
import sys


REFERENCE_ROOT = pathlib.Path("project/tests/reference_regression")

EXPECTED_REFERENCE_ROOTS = (
    ("apache_ignite", "apache_ignite"),
    ("cassandra", "cassandra"),
    ("clickhouse", "clickhouse"),
    ("cockroachdb", "cockroachdb"),
    ("dolt", "dolt"),
    ("duckdb", "duckdb"),
    ("firebird", "firebird"),
    ("foundationdb", "foundationdb"),
    ("immudb", "immudb"),
    ("influxdb", "influxdb"),
    ("mariadb", "mariadb"),
    ("milvus", "milvus"),
    ("mongodb", "mongodb"),
    ("mysql", "mysql"),
    ("neo4j", "neo4j"),
    ("opensearch/rest_dsl", "opensearch/rest_dsl"),
    ("opensearch_sql_ppl", "opensearch_sql_ppl"),
    ("postgresql", "postgresql"),
    ("redis", "redis"),
    ("sqlite", "sqlite"),
    ("tidb", "tidb"),
    ("tikv", "tikv"),
    ("vitess", "vitess"),
    ("xtdb", "xtdb"),
    ("yugabytedb", "yugabytedb"),
)

REQUIRED_COLUMNS = {
    "reference_id",
    "harness_id",
    "native_tool_family",
    "tool_locator",
    "tool_exists",
    "tool_kind",
    "tool_file_count",
    "tool_total_bytes",
    "tool_tree_shape_digest",
    "required_endpoint_env",
    "required_output",
    "parser_authority_rule",
    "status",
}

REQUIRED_ENDPOINT_ENV = {
    "SCRATCHBIRD_REFERENCE_ENDPOINT",
    "SCRATCHBIRD_REFERENCE_AUTH_PACKET",
    "SCRATCHBIRD_REFERENCE_RESULT_DIR",
}

REQUIRED_OUTPUTS = {
    "normalized_native_replay_results.json",
    "native_replay_support_bundle.json",
}

NO_COMPILED_TOOL_LOCATORS = {"no_tool_recorded", "no_compiled_tool_packaged"}
HEX64_RE = re.compile(r"^[0-9a-f]{64}$")


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    require(path.is_file(), f"missing CSV: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        require(reader.fieldnames is not None, f"{path}: missing CSV header")
        missing = sorted(REQUIRED_COLUMNS - set(reader.fieldnames))
        require(not missing, f"{path}: missing columns {missing}")
        rows = [dict(row) for row in reader]
    require(rows, f"{path}: no rows")
    return rows


def split_semicolon(value: str) -> set[str]:
    return {item.strip() for item in value.split(";") if item.strip()}


def validate_manifest(repo_root: pathlib.Path,
                      reference_id: str,
                      reference_root: str) -> dict[str, object]:
    manifest_rel = (
        REFERENCE_ROOT
        / reference_root
        / "native_tool_harness"
        / "native_tool_harness_manifest.csv"
    )
    rows = read_csv(repo_root / manifest_rel)
    tool_rows = 0
    client_replay_rows = 0
    declared_locators: list[str] = []

    for row_number, row in enumerate(rows, start=2):
        context = f"{manifest_rel}:{row_number}"
        require(row["reference_id"] == reference_id,
                f"{context}: reference_id mismatch: {row['reference_id']}")
        require(row["harness_id"].startswith(reference_id.upper().replace("/", "_")),
                f"{context}: harness_id does not start with reference id")
        require(row["status"],
                f"{context}: status is empty")
        require(split_semicolon(row["required_endpoint_env"]) == REQUIRED_ENDPOINT_ENV,
                f"{context}: required endpoint environment contract drift")
        require(split_semicolon(row["required_output"]) == REQUIRED_OUTPUTS,
                f"{context}: required output contract drift")

        if row["tool_exists"] == "yes":
            tool_rows += 1
            locator = row["tool_locator"]
            allowed_prefix = (
                REFERENCE_ROOT
                / reference_root
                / "native_tool_harness"
                / "tools"
            ).as_posix() + "/"
            require(locator.startswith(allowed_prefix),
                    f"{context}: native tool locator escapes harness tools dir")
            require(row["tool_kind"] in {"file", "directory"},
                    f"{context}: invalid tool_kind for existing tool")
            require(int(row["tool_file_count"]) > 0,
                    f"{context}: existing tool has no file count")
            require(int(row["tool_total_bytes"]) > 0,
                    f"{context}: existing tool has no byte count")
            require(HEX64_RE.match(row["tool_tree_shape_digest"]) is not None,
                    f"{context}: invalid tool digest")
            require("engine retains" in row["parser_authority_rule"].lower(),
                    f"{context}: existing-tool row lacks engine authority statement")
            declared_locators.append(locator)
        elif row["tool_exists"] == "no":
            client_replay_rows += 1
            require(row["tool_locator"] in NO_COMPILED_TOOL_LOCATORS,
                    f"{context}: missing-tool locator is not explicit")
            require(row["tool_kind"] == "not_applicable",
                    f"{context}: missing-tool row has concrete tool_kind")
            require(row["tool_file_count"] == "0",
                    f"{context}: missing-tool row has file count")
            require(row["tool_total_bytes"] == "0",
                    f"{context}: missing-tool row has byte count")
            require(row["tool_tree_shape_digest"] == "not_applicable",
                    f"{context}: missing-tool row has digest")
            require("client replay harness required" in row["parser_authority_rule"].lower(),
                    f"{context}: missing-tool row lacks client replay contract")
        else:
            fail(f"{context}: tool_exists must be yes or no")

    require(tool_rows or client_replay_rows,
            f"{manifest_rel}: manifest has no replay route")
    return {
        "reference_id": reference_id,
        "reference_root": reference_root,
        "manifest": manifest_rel.as_posix(),
        "native_tool_rows": tool_rows,
        "client_replay_rows": client_replay_rows,
        "declared_native_tool_locators": declared_locators,
    }


def write_evidence(path: pathlib.Path, results: list[dict[str, object]]) -> None:
    digest = hashlib.sha256(
        json.dumps(results, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    payload = {
        "gate": "reference_native_tool_harness_contract_gate",
        "reference_count": len(results),
        "native_tool_row_count": sum(int(item["native_tool_rows"]) for item in results),
        "client_replay_row_count": sum(int(item["client_replay_rows"]) for item in results),
        "manifest_contract_digest": digest,
        "authority_policy": "reference_tools_drive_parser_endpoint_only_engine_mga_security_storage_authority",
        "references": results,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-file", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    results = [
        validate_manifest(repo_root, reference_id, reference_root)
        for reference_id, reference_root in EXPECTED_REFERENCE_ROOTS
    ]
    write_evidence(args.evidence_file, results)
    print(
        "reference_native_tool_harness_contract_gate=passed "
        f"references={len(results)} "
        f"native_tool_rows={sum(int(item['native_tool_rows']) for item in results)} "
        f"client_replay_rows={sum(int(item['client_replay_rows']) for item in results)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
