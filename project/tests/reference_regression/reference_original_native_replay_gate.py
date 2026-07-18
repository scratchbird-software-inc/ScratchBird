#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Opt-in native original-tool replay gate.

Public CTest runs must not require acquired upstream payloads or locally built
reference tools. This gate therefore skips by default and runs only when the
operator selects a local or release replay mode through SB_REFERENCE_REPLAY_MODE.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


SKIP_RETURN_CODE = 77
VALID_RUN_MODES = {"local-optional", "release-mandatory", "single-family"}
DEFAULT_MODE = "public-no-payload"
MYSQL_WIRE_FAMILIES = {"dolt", "mariadb", "mysql", "mysql_lts", "tidb", "vitess"}
POSTGRESQL_WIRE_FAMILIES = {"cockroachdb", "postgresql", "xtdb", "yugabytedb"}
HTTP_REST_FAMILIES = {"opensearch", "opensearch_sql_ppl"}
APACHE_IGNITE_JDBC_FAMILIES = {"apache_ignite"}
INFLUXDB_HTTP_FAMILIES = {"influxdb"}
MONGODB_WIRE_FAMILIES = {"mongodb"}
REDIS_WIRE_FAMILIES = {"redis"}
CASSANDRA_WIRE_FAMILIES = {"cassandra"}
CLICKHOUSE_WIRE_FAMILIES = {"clickhouse"}
NEO4J_WIRE_FAMILIES = {"neo4j"}
MILVUS_GRPC_FAMILIES = {"milvus"}
IMMUDB_GRPC_FAMILIES = {"immudb"}
TIKV_GRPC_FAMILIES = {"tikv"}
FOUNDATIONDB_FLOW_FAMILIES = {"foundationdb"}
EMBEDDED_SHELL_FAMILIES = {"duckdb", "sqlite"}


def utc_timestamp() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def write_evidence(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8", errors="replace")).hexdigest()


def normalized_skip(command_id: str, reason: str) -> dict[str, Any]:
    return {
        "schema_version": "scratchbird_reference_replay_normalized_result_v1",
        "status": "skipped",
        "classification": reason,
        "command_id": command_id,
        "output_sha256": sha256_text(reason),
        "diagnostic": reason,
        "canonicalization": {
            "whitespace": "not_applicable_skip",
            "ordering": "not_applicable_skip",
            "nondeterministic_values": "none",
        },
    }


def candidate_firebird_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_FIREBIRD_ISQL")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root
            / "build/reference/firebird-5.0.4-release-src/gen/Release/firebird/bin/isql",
            repo_root / "project/tests/reference_regression/firebird/native_tool_harness/tools/isql",
        ]
    )
    return candidates


def resolve_firebird_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_firebird_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_postgresql_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_POSTGRESQL_PSQL")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/postgresql/native_tool_harness/tools/psql",
            Path("/usr/bin/psql"),
        ]
    )
    return candidates


def resolve_postgresql_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_postgresql_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_mysql_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_MYSQL_CLIENT")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/mysql/native_tool_harness/tools/mysql",
            Path("/usr/bin/mysql"),
        ]
    )
    return candidates


def resolve_mysql_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_mysql_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_curl_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_CURL")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/http/native_tool_harness/tools/curl",
            Path("/usr/bin/curl"),
        ]
    )
    return candidates


def resolve_curl_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_curl_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_apache_ignite_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_APACHE_IGNITE_SQLLINE")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root
            / "project/tests/reference_regression/apache_ignite/native_tool_harness/tools/sqlline.sh",
            repo_root
            / "project/tests/reference_regression/reference_release_acquisition/apache_ignite/2.17.0/regression/tools/apache-ignite-2.17.0-bin/bin/sqlline.sh",
        ]
    )
    return candidates


def resolve_apache_ignite_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_apache_ignite_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_mongodb_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_MONGODB_SHELL")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/mongodb/native_tool_harness/tools/mongosh",
            Path("/usr/bin/mongosh"),
        ]
    )
    return candidates


def resolve_mongodb_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_mongodb_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_redis_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_REDIS_CLI")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/redis/native_tool_harness/tools/redis-cli",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/redis/8.6.2/regression/tools/redis-8.6.2/src/redis-cli",
            Path("/usr/bin/redis-cli"),
        ]
    )
    return candidates


def resolve_redis_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_redis_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_cassandra_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_CASSANDRA_CQLSH")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/cassandra/native_tool_harness/tools/cqlsh.py",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/cassandra/5.0.8/regression/tools/cassandra-5.0.8/bin/cqlsh.py",
            Path("/usr/bin/cqlsh"),
        ]
    )
    return candidates


def resolve_cassandra_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_cassandra_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_sqlite_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_SQLITE_SHELL")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/sqlite/native_tool_harness/tools/sqlite3",
            Path("/usr/bin/sqlite3"),
        ]
    )
    return candidates


def resolve_sqlite_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_sqlite_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_duckdb_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_DUCKDB_CLI")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/duckdb/native_tool_harness/tools/duckdb",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/duckdb/1.5.2/regression/tools/duckdb-v1.5.2-cli/duckdb",
            Path("/usr/bin/duckdb"),
        ]
    )
    return candidates


def resolve_duckdb_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_duckdb_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_influxdb_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_INFLUXDB3_CLI")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/influxdb/native_tool_harness/tools/influxdb3",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/influxdb/3.9.0/regression/tools/influxdb-3.9.0/target/release/influxdb3",
            Path("/usr/bin/influxdb3"),
        ]
    )
    return candidates


def resolve_influxdb_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_influxdb_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_clickhouse_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_CLICKHOUSE_CLIENT")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/clickhouse/native_tool_harness/tools/clickhouse",
            repo_root / "project/tests/reference_regression/clickhouse/native_tool_harness/tools/clickhouse-client",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/clickhouse/25.12.10.7-stable/regression/tools/clickhouse-25.12.10.7/usr/bin/clickhouse",
            Path("/usr/bin/clickhouse"),
            Path("/usr/bin/clickhouse-client"),
        ]
    )
    return candidates


def resolve_clickhouse_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_clickhouse_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_neo4j_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_NEO4J_CYPHER_SHELL")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/neo4j/native_tool_harness/tools/cypher-shell",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/neo4j/2026.06.0/regression/tools/cypher-shell-2026.06.0/bin/cypher-shell",
            Path("/usr/bin/cypher-shell"),
        ]
    )
    return candidates


def resolve_neo4j_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_neo4j_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_milvus_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_MILVUS_PYTHON")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root
            / "project/tests/reference_regression/milvus/native_tool_harness/tools/python",
            repo_root
            / "project/tests/reference_regression/reference_release_acquisition/milvus/2.6.5/regression/tools/pymilvus-2.6.3-venv/bin/python",
            Path("/usr/bin/python3"),
        ]
    )
    return candidates


def resolve_milvus_tool(repo_root: Path) -> Path | None:
    probe = "from pymilvus.grpc_gen import milvus_pb2, milvus_pb2_grpc"
    for candidate in candidate_milvus_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if not candidate.is_file() or not os.access(candidate, os.X_OK):
            continue
        proc = subprocess.run(
            [str(candidate), "-c", probe],
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10,
            check=False,
        )
        if proc.returncode == 0:
            return candidate
    return None


def candidate_immudb_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_IMMUDB_CLIENT")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/immudb/native_tool_harness/tools/immuclient",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/immudb/1.11.0/regression/tools/immuclient",
        ]
    )
    return candidates


def resolve_immudb_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_immudb_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_tikv_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_TIKV_CTL")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/tikv/native_tool_harness/tools/tikv-ctl",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/tikv/8.5.6/regression/tools/tikv-ctl",
        ]
    )
    return candidates


def resolve_tikv_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_tikv_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def candidate_foundationdb_tools(repo_root: Path) -> list[Path]:
    candidates: list[Path] = []
    env_tool = os.environ.get("SB_REFERENCE_FOUNDATIONDB_FDBCLI")
    if env_tool:
        candidates.append(Path(env_tool))
    candidates.extend(
        [
            repo_root / "project/tests/reference_regression/foundationdb/native_tool_harness/tools/fdbcli",
            repo_root / "project/tests/reference_regression/reference_release_acquisition/foundationdb/7.3.77/regression/tools/fdbcli",
            Path("/usr/bin/fdbcli"),
        ]
    )
    return candidates


def resolve_foundationdb_tool(repo_root: Path) -> Path | None:
    for candidate in candidate_foundationdb_tools(repo_root):
        candidate = candidate.expanduser()
        if not candidate.is_absolute():
            candidate = repo_root / candidate
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def firebird_library_env(tool: Path, env: dict[str, str]) -> dict[str, str]:
    lib_dir = os.environ.get("SB_REFERENCE_FIREBIRD_LIB_DIR")
    if lib_dir:
        env["LD_LIBRARY_PATH"] = (
            lib_dir
            if not env.get("LD_LIBRARY_PATH")
            else lib_dir + os.pathsep + env["LD_LIBRARY_PATH"]
        )
        return env

    built_lib_dir = tool.parents[1] / "lib" if len(tool.parents) > 1 else None
    if built_lib_dir is not None and (built_lib_dir / "libfbclient.so.2").exists():
        env["LD_LIBRARY_PATH"] = (
            str(built_lib_dir)
            if not env.get("LD_LIBRARY_PATH")
            else str(built_lib_dir) + os.pathsep + env["LD_LIBRARY_PATH"]
        )
    return env


def skip(args: argparse.Namespace, reason: str) -> int:
    payload = {
        "gate": "reference_original_native_replay_gate",
        "schema_version": "scratchbird_reference_original_native_replay_gate_v1",
        "status": "skipped",
        "timestamp_utc": utc_timestamp(),
        "family": args.family,
        "reason": reason,
        "run_mode": args.mode,
        "normalized_result": normalized_skip(f"{args.family}.original_tool_replay", reason),
        "authority_policy": "public_ctest_does_not_require_acquired_upstream_payloads",
    }
    write_evidence(args.evidence_file, payload)
    print(
        "reference_original_native_replay_gate=skipped "
        f"family={args.family} mode={args.mode} reason={reason}"
    )
    return SKIP_RETURN_CODE


def fail(args: argparse.Namespace, reason: str) -> int:
    payload = {
        "gate": "reference_original_native_replay_gate",
        "schema_version": "scratchbird_reference_original_native_replay_gate_v1",
        "status": "failed",
        "timestamp_utc": utc_timestamp(),
        "family": args.family,
        "reason": reason,
        "run_mode": args.mode,
        "normalized_result": {
            "schema_version": "scratchbird_reference_replay_normalized_result_v1",
            "status": "failed",
            "classification": reason,
            "command_id": f"{args.family}.original_tool_replay",
            "output_sha256": sha256_text(reason),
            "diagnostic": reason,
            "canonicalization": {
                "whitespace": "not_applicable_failure",
                "ordering": "not_applicable_failure",
                "nondeterministic_values": "none",
            },
        },
        "authority_policy": "release_mandatory_replay_requires_executable_original_tool_runner",
    }
    write_evidence(args.evidence_file, payload)
    print(
        "reference_original_native_replay_gate=failed "
        f"family={args.family} mode={args.mode} reason={reason}",
        file=sys.stderr,
    )
    return 1


def run_firebird(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_firebird_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, "firebird_original_tool_not_present")
        return skip(args, "firebird_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    env = firebird_library_env(tool, dict(os.environ))
    proc = subprocess.run(cmd, cwd=repo_root, env=env, check=False)
    return int(proc.returncode)


def run_postgresql(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_postgresql_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_mysql(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_mysql_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_http_rest(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_curl_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_apache_ignite(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_apache_ignite_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, "apache_ignite_original_tool_not_present")
        return skip(args, "apache_ignite_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_influxdb(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_influxdb_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_clickhouse(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_clickhouse_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_neo4j(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_neo4j_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_milvus(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_milvus_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_immudb(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_immudb_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, "immudb_original_tool_not_present")
        return skip(args, "immudb_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_tikv(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_tikv_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, "tikv_original_tool_not_present")
        return skip(args, "tikv_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_foundationdb(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_foundationdb_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, "foundationdb_original_tool_not_present")
        return skip(args, "foundationdb_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_mongodb(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_mongodb_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_redis(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_redis_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_cassandra(args: argparse.Namespace, repo_root: Path) -> int:
    tool = resolve_cassandra_tool(repo_root)
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, f"{args.family}_original_tool_not_present")
        return skip(args, f"{args.family}_original_tool_not_present")

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def run_embedded_shell(args: argparse.Namespace, repo_root: Path) -> int:
    if args.family == "sqlite":
        tool = resolve_sqlite_tool(repo_root)
        missing_reason = "sqlite_original_tool_not_present"
    elif args.family == "duckdb":
        tool = resolve_duckdb_tool(repo_root)
        missing_reason = "duckdb_original_tool_not_present"
    else:
        tool = None
        missing_reason = f"{args.family}_original_tool_not_present"
    if tool is None:
        if args.mode == "release-mandatory":
            return fail(args, missing_reason)
        return skip(args, missing_reason)

    smoke = repo_root / "project/tests/reference_regression/reference_original_tool_smoke.py"
    cmd = [
        sys.executable,
        str(smoke),
        "--repo-root",
        str(repo_root),
        "--server",
        args.server,
        "--listener",
        args.listener,
        "--parser-worker",
        args.parser_worker,
        "--family",
        args.family,
        "--tool",
        str(tool),
        "--work-root",
        str(args.work_root),
        "--evidence-file",
        str(args.evidence_file),
        "--tool-timeout",
        str(args.tool_timeout),
    ]
    proc = subprocess.run(cmd, cwd=repo_root, env=dict(os.environ), check=False)
    return int(proc.returncode)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--server", required=True)
    parser.add_argument("--listener", required=True)
    parser.add_argument("--parser-worker", required=True)
    parser.add_argument("--family", default="firebird")
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--evidence-file", required=True, type=Path)
    parser.add_argument("--tool-timeout", type=int, default=20)
    args = parser.parse_args(argv)

    args.mode = os.environ.get("SB_REFERENCE_REPLAY_MODE", DEFAULT_MODE)
    repo_root = args.repo_root.resolve()

    if args.mode == DEFAULT_MODE:
        return skip(args, "public_no_payload_mode")
    if args.mode not in VALID_RUN_MODES:
        print(f"unsupported SB_REFERENCE_REPLAY_MODE={args.mode}", file=sys.stderr)
        return 1
    if args.family == "firebird":
        return run_firebird(args, repo_root)
    if args.family in MYSQL_WIRE_FAMILIES:
        return run_mysql(args, repo_root)
    if args.family in POSTGRESQL_WIRE_FAMILIES:
        return run_postgresql(args, repo_root)
    if args.family in HTTP_REST_FAMILIES:
        return run_http_rest(args, repo_root)
    if args.family in APACHE_IGNITE_JDBC_FAMILIES:
        return run_apache_ignite(args, repo_root)
    if args.family in INFLUXDB_HTTP_FAMILIES:
        return run_influxdb(args, repo_root)
    if args.family in CLICKHOUSE_WIRE_FAMILIES:
        return run_clickhouse(args, repo_root)
    if args.family in NEO4J_WIRE_FAMILIES:
        return run_neo4j(args, repo_root)
    if args.family in MILVUS_GRPC_FAMILIES:
        return run_milvus(args, repo_root)
    if args.family in IMMUDB_GRPC_FAMILIES:
        return run_immudb(args, repo_root)
    if args.family in TIKV_GRPC_FAMILIES:
        return run_tikv(args, repo_root)
    if args.family in FOUNDATIONDB_FLOW_FAMILIES:
        return run_foundationdb(args, repo_root)
    if args.family in MONGODB_WIRE_FAMILIES:
        return run_mongodb(args, repo_root)
    if args.family in REDIS_WIRE_FAMILIES:
        return run_redis(args, repo_root)
    if args.family in CASSANDRA_WIRE_FAMILIES:
        return run_cassandra(args, repo_root)
    if args.family in EMBEDDED_SHELL_FAMILIES:
        return run_embedded_shell(args, repo_root)
    if args.family != "firebird":
        if args.mode == "release-mandatory":
            return fail(args, "native_original_tool_replay_not_implemented_for_family")
        return skip(args, "native_original_tool_replay_not_implemented_for_family")
    return run_firebird(args, repo_root)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
