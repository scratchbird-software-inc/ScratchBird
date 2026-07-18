#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run original-tool replay lanes against one canonical ScratchBird database."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Callable

import reference_original_native_replay_gate as native
import reference_original_tool_smoke as smoke


SKIP_RETURN_CODE = 77
DEFAULT_MODE = "public-no-payload"
VALID_RUN_MODES = {"local-optional", "release-mandatory", "single-family"}

FAMILIES = (
    "firebird",
    "apache_ignite",
    "cassandra",
    "clickhouse",
    "cockroachdb",
    "dolt",
    "duckdb",
    "foundationdb",
    "immudb",
    "influxdb",
    "mariadb",
    "milvus",
    "mongodb",
    "mysql",
    "mysql_lts",
    "neo4j",
    "opensearch",
    "opensearch_sql_ppl",
    "postgresql",
    "redis",
    "sqlite",
    "tidb",
    "tikv",
    "vitess",
    "xtdb",
    "yugabytedb",
)

PARSER_BINARY_FAMILY = {
    "mysql_lts": "mysql",
}

TOOL_RESOLVERS: dict[str, Callable[[Path], Path | None]] = {
    "firebird": native.resolve_firebird_tool,
    "apache_ignite": native.resolve_apache_ignite_tool,
    "cassandra": native.resolve_cassandra_tool,
    "clickhouse": native.resolve_clickhouse_tool,
    "cockroachdb": native.resolve_postgresql_tool,
    "dolt": native.resolve_mysql_tool,
    "duckdb": native.resolve_duckdb_tool,
    "foundationdb": native.resolve_foundationdb_tool,
    "immudb": native.resolve_immudb_tool,
    "influxdb": native.resolve_influxdb_tool,
    "mariadb": native.resolve_mysql_tool,
    "milvus": native.resolve_milvus_tool,
    "mongodb": native.resolve_mongodb_tool,
    "mysql": native.resolve_mysql_tool,
    "mysql_lts": native.resolve_mysql_tool,
    "neo4j": native.resolve_neo4j_tool,
    "opensearch": native.resolve_curl_tool,
    "opensearch_sql_ppl": native.resolve_curl_tool,
    "postgresql": native.resolve_postgresql_tool,
    "redis": native.resolve_redis_tool,
    "sqlite": native.resolve_sqlite_tool,
    "tidb": native.resolve_mysql_tool,
    "tikv": native.resolve_tikv_tool,
    "vitess": native.resolve_mysql_tool,
    "xtdb": native.resolve_postgresql_tool,
    "yugabytedb": native.resolve_postgresql_tool,
}


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def write_evidence(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parser_binary(parser_bin_root: Path, family: str) -> Path:
    parser_family = PARSER_BINARY_FAMILY.get(family, family)
    binary = parser_bin_root / f"sbp_{parser_family}"
    if os.name == "nt":
        exe = binary.with_name(binary.name + ".exe")
        if exe.exists():
            return exe
    return binary


def normalized_skip(command_id: str, reason: str) -> dict[str, Any]:
    return native.normalized_skip(command_id, reason)


def embedded_probe(family_args: SimpleNamespace, family_work: Path) -> dict[str, Any]:
    return smoke.embedded_shell_probe(family_args, family_work)


def listener_probe(family: str,
                   family_args: SimpleNamespace,
                   family_work: Path,
                   repo_root: Path,
                   listener_info: dict[str, Any]) -> dict[str, Any]:
    port = int(listener_info["port"])
    if family == "firebird":
        return smoke.firebird_probe(family_args, family_work, port)
    if family in smoke.MYSQL_WIRE_FAMILIES:
        return smoke.mysql_probe(family_args, family_work, port)
    if family in smoke.POSTGRESQL_WIRE_FAMILIES:
        return smoke.postgresql_probe(family_args, family_work, port)
    if family in smoke.HTTP_REST_FAMILIES:
        return smoke.http_rest_probe(family_args, family_work, port)
    if family in smoke.APACHE_IGNITE_JDBC_FAMILIES:
        return smoke.apache_ignite_probe(family_args, family_work, port)
    if family in smoke.INFLUXDB_HTTP_FAMILIES:
        return smoke.influxdb_probe(family_args, family_work, port)
    if family in smoke.CLICKHOUSE_WIRE_FAMILIES:
        return smoke.clickhouse_probe(family_args, family_work, port)
    if family in smoke.NEO4J_WIRE_FAMILIES:
        return smoke.neo4j_probe(family_args, family_work, port)
    if family in smoke.MILVUS_GRPC_FAMILIES:
        return smoke.milvus_probe(family_args, family_work, port)
    if family in smoke.IMMUDB_GRPC_FAMILIES:
        return smoke.immudb_probe(family_args, family_work, port)
    if family in smoke.TIKV_GRPC_FAMILIES:
        return smoke.tikv_probe(family_args, family_work, port)
    if family in smoke.FOUNDATIONDB_FLOW_FAMILIES:
        return smoke.foundationdb_probe(family_args, family_work, port, listener_info)
    if family in smoke.MONGODB_WIRE_FAMILIES:
        return smoke.mongodb_probe(family_args, family_work, port)
    if family in smoke.REDIS_WIRE_FAMILIES:
        return smoke.redis_probe(family_args, family_work, port)
    if family in smoke.CASSANDRA_WIRE_FAMILIES:
        return smoke.cassandra_probe(family_args, family_work, port, repo_root)
    raise RuntimeError(f"canonical replay has no probe for family={family}")


def run_family(args: argparse.Namespace,
               repo_root: Path,
               work: Path,
               server_info: dict[str, Any],
               family: str) -> dict[str, Any]:
    family_work = work / "families" / family
    family_work.mkdir(parents=True, exist_ok=True)

    resolver = TOOL_RESOLVERS[family]
    tool = resolver(repo_root)
    if tool is None:
        reason = f"{family}_original_tool_not_present"
        return {
            "family": family,
            "status": "failed" if args.mode == "release-mandatory" else "skipped",
            "reason": reason,
            "normalized_result": normalized_skip(f"{family}.canonical_database_replay", reason),
        }

    family_args = SimpleNamespace(
        server=args.server,
        listener=args.listener,
        parser_worker=str(parser_binary(args.parser_bin_root, family)),
        family=family,
        tool=str(tool),
        tool_timeout=args.tool_timeout,
        port=0,
    )

    if family in smoke.EMBEDDED_SHELL_FAMILIES:
        tool_info = embedded_probe(family_args, family_work)
        return {
            "family": family,
            "status": "passed",
            "tool": tool_info,
            "server_route_used": False,
        }

    listener = None
    old_env = dict(os.environ)
    try:
        if family == "firebird":
            os.environ.clear()
            os.environ.update(native.firebird_library_env(tool, dict(old_env)))
        listener, listener_info = smoke.start_listener(family_args, family_work, server_info)
        tool_info = listener_probe(family, family_args, family_work, repo_root, listener_info)
        return {
            "family": family,
            "status": "passed",
            "listener": listener_info,
            "tool": tool_info,
            "server_route_used": True,
        }
    except Exception as exc:  # noqa: BLE001 - preserve per-family evidence.
        return {
            "family": family,
            "status": "failed",
            "reason": str(exc),
            "normalized_result": smoke.normalized_result(
                "failed",
                "environment_or_protocol_error",
                f"{family}.canonical_database_replay",
                str(exc),
                str(exc),
            ),
        }
    finally:
        smoke.stop_process(listener)
        os.environ.clear()
        os.environ.update(old_env)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-bin-root", required=True, type=Path)
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    parser.add_argument("--tool-timeout", type=int, default=20)
    args = parser.parse_args(argv)

    args.mode = os.environ.get("SB_REFERENCE_REPLAY_MODE", DEFAULT_MODE)
    repo_root = args.repo_root.resolve()
    args.parser_bin_root = args.parser_bin_root.resolve()

    if args.mode == DEFAULT_MODE:
        payload = {
            "schema_version": "scratchbird_reference_original_canonical_database_replay_gate_v1",
            "gate": "reference_original_canonical_database_replay_gate",
            "status": "skipped",
            "timestamp_utc": utc_timestamp(),
            "run_mode": args.mode,
            "reason": "public_no_payload_mode",
            "normalized_result": normalized_skip(
                "reference_original_canonical_database_replay",
                "public_no_payload_mode",
            ),
        }
        write_evidence(args.evidence_file, payload)
        print("reference_original_canonical_database_replay_gate=skipped reason=public_no_payload_mode")
        return SKIP_RETURN_CODE
    if args.mode not in VALID_RUN_MODES:
        print(f"unsupported SB_REFERENCE_REPLAY_MODE={args.mode}", file=sys.stderr)
        return 1

    work = smoke.make_work_dir(args.work_root)
    server = None
    try:
        server, server_info = smoke.start_server(args, work)
        results = [
            run_family(args, repo_root, work, server_info, family)
            for family in FAMILIES
        ]
    finally:
        smoke.stop_process(server)

    failed = [row for row in results if row["status"] != "passed"]
    payload = {
        "schema_version": "scratchbird_reference_original_canonical_database_replay_gate_v1",
        "gate": "reference_original_canonical_database_replay_gate",
        "status": "failed" if failed else "passed",
        "timestamp_utc": utc_timestamp(),
        "run_mode": args.mode,
        "work_dir": str(work),
        "server": server_info if "server_info" in locals() else {},
        "family_count": len(results),
        "passed_count": len(results) - len(failed),
        "failed_count": len(failed),
        "families": results,
        "authority_policy": "one_scratchbird_database_reused_by_original_tool_parser_listener_replay_engine_remains_sblr_uuid_mga_authority",
    }
    write_evidence(args.evidence_file, payload)
    if failed:
        print(
            "reference_original_canonical_database_replay_gate=failed "
            f"failed={','.join(row['family'] for row in failed)} evidence={args.evidence_file}",
            file=sys.stderr,
        )
        return 1
    print(
        "reference_original_canonical_database_replay_gate=passed "
        f"families={len(results)} evidence={args.evidence_file}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
