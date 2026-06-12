#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Stage original reference tools and replay first-tranche reference SQL parser cases.

This is a regular CTest gate.  It intentionally keeps reference tools as
test-only artifacts under project/tests while preserving ScratchBird authority:
reference tools and reference SQL fragments provide parser-facing evidence only; they
do not execute storage, transaction finality, recovery, or security authority.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
from dataclasses import dataclass
from typing import Iterable


PUBLIC_REFERENCE_NOT_PACKAGED = "public_repo_reference_not_packaged"
PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED = PUBLIC_REFERENCE_NOT_PACKAGED
COMPATIBILITY_REPLAY_GATE_ID = "compatibility_sql_first_tranche_original_tool_replay_gate"
REFERENCE_REPLAY_INPUT_ID = "reference_sql_first_tranche_original_tool_replay_gate"
EXTERNAL_REFERENCE_SKIP_CODE = 77

MYSQL_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
POSTGRESQL_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
FIREBIRD_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
SQLITE_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
MARIADB_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
DUCKDB_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
CLICKHOUSE_SOURCE_REL = (
    PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
)
TIDB_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
VITESS_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
COCKROACHDB_SOURCE_REL = (
    PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
)
YUGABYTEDB_SOURCE_REL = (
    PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
)
CASSANDRA_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
MONGODB_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
REDIS_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
OPENSEARCH_SQL_PPL_SOURCE_REL = (
    PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
)
OPENSEARCH_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
NEO4J_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
INFLUXDB_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
MILVUS_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
DOLT_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
APACHE_IGNITE_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
TIKV_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
FOUNDATIONDB_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
IMMUDB_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED
XTDB_SOURCE_REL = PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED


class ExternalReferenceFixtureMissing(AssertionError):
    """Raised when local reference fixtures are not installed for this CTest."""


@dataclass(frozen=True)
class ToolSpec:
    dialect: str
    tool_id: str
    source: pathlib.Path
    staged_rel: str
    smoke_command: tuple[str, ...]
    expected_smoke_fragments: tuple[str, ...]
    allowed_return_codes: tuple[int, ...] = (0,)
    copy_tree: bool = False
    extra_tree_source: pathlib.Path | None = None
    extra_tree_name: str = ""


@dataclass(frozen=True)
class ReplayCase:
    dialect: str
    case_id: str
    source_rel: str
    source_fragment: str
    sql: str
    expected_operation_family: str
    expected_statement_kind: str
    expected_disposition: str
    expected_diagnostic_code: str = ""
    validation_tags: tuple[str, ...] = ()
    expected_datatype_families: tuple[str, ...] = ()
    provenance: str = "reference_release_packet_sql"


def run_command(command: list[str],
                *,
                cwd: pathlib.Path,
                timeout_seconds: int,
                env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout_seconds,
        check=False,
    )


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_digest(root: pathlib.Path) -> dict[str, object]:
    file_count = 0
    total_bytes = 0
    digest = hashlib.sha256()
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        data = path.read_bytes()
        file_count += 1
        total_bytes += len(data)
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(data).digest())
    return {
        "file_count": file_count,
        "total_bytes": total_bytes,
        "tree_shape_digest": digest.hexdigest(),
    }


def paths_digest(paths: Iterable[pathlib.Path], root: pathlib.Path) -> dict[str, object]:
    file_count = 0
    total_bytes = 0
    digest = hashlib.sha256()
    files: list[pathlib.Path] = []
    for path in paths:
        if path.is_dir():
            files.extend(sorted(p for p in path.rglob("*") if p.is_file()))
        elif path.is_file():
            files.append(path)
    for path in sorted(files):
        rel = path.relative_to(root).as_posix()
        data = path.read_bytes()
        file_count += 1
        total_bytes += len(data)
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(data).digest())
    return {
        "file_count": file_count,
        "total_bytes": total_bytes,
        "tree_shape_digest": digest.hexdigest(),
    }


def ensure_under(path: pathlib.Path, root: pathlib.Path) -> None:
    resolved_path = path.resolve()
    resolved_root = root.resolve()
    try:
        resolved_path.relative_to(resolved_root)
    except ValueError as exc:
        raise AssertionError(f"path escapes expected root: {resolved_path}") from exc


def ensure_mysql_tools(repo_root: pathlib.Path,
                       build_root: pathlib.Path,
                       timeout_seconds: int) -> tuple[pathlib.Path, pathlib.Path]:
    source_root = repo_root / MYSQL_SOURCE_REL
    build_dir = build_root / "reference" / "mysql-8.4.8-tools"
    mysql = build_dir / "runtime_output_directory" / "mysql"
    mysqltest = build_dir / "runtime_output_directory" / "mysqltest"
    if mysql.exists() and mysqltest.exists():
        return mysql, mysqltest

    configure = [
        "cmake",
        "-S",
        str(source_root),
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        "-DWITHOUT_SERVER=ON",
        "-DWITH_UNIT_TESTS=OFF",
        "-DWITH_SSL=system",
        "-DWITH_ZLIB=system",
        "-DWITH_ICU=system",
        "-DWITH_LZ4=system",
        "-DWITH_ZSTD=system",
        "-DWITH_CURL=system",
        "-DWITH_PROTOBUF=system",
        "-DWITH_TIRPC=bundled",
        "-DFORCE_UNSUPPORTED_COMPILER=ON",
    ]
    result = run_command(configure, cwd=repo_root, timeout_seconds=timeout_seconds)
    if result.returncode != 0:
        raise AssertionError(f"MySQL reference tool configure failed:\n{result.stdout}")

    for target in ("mysqltest", "mysql"):
        result = run_command(
            ["cmake", "--build", str(build_dir), "--target", target, "--parallel", "4"],
            cwd=repo_root,
            timeout_seconds=timeout_seconds,
        )
        if result.returncode != 0:
            raise AssertionError(f"MySQL reference tool build failed for {target}:\n{result.stdout}")

    if not mysql.exists() or not mysqltest.exists():
        raise AssertionError("MySQL reference tool build completed without mysql/mysqltest binaries")
    return mysql, mysqltest


def ensure_firebird_isql(repo_root: pathlib.Path,
                         timeout_seconds: int) -> pathlib.Path:
    reference_root = repo_root / "build" / "reference" / "firebird-5.0.4-release-src"
    if not reference_root.exists():
        source_root = repo_root / FIREBIRD_SOURCE_REL
        if not source_root.is_dir():
            raise AssertionError(f"Firebird reference source packet missing: {source_root}")
        reference_root.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source_root, reference_root)
    manifest = (
        repo_root
        / "project/tests/firebird_parser_worker/fixtures/"
        / "full_firebirdsql_parser_udr_emulation_closure/artifacts/"
        / "FIREBIRD_REFERENCE_TOOL_BUILD_MANIFEST.csv"
    )
    gate = repo_root / "project/tests/firebird_parser_worker/firebird_reference_tool_gate.py"
    result = run_command(
        [
            sys.executable,
            str(gate),
            "--mode",
            "build",
            "--repo-root",
            str(repo_root),
            "--reference-root",
            str(reference_root),
            "--manifest",
            str(manifest),
            "--build-if-missing",
            "--jobs",
            "4",
            "--build-timeout",
            str(timeout_seconds),
        ],
        cwd=repo_root,
        timeout_seconds=timeout_seconds,
    )
    if result.returncode != 0:
        raise AssertionError(f"Firebird reference tool build gate failed:\n{result.stdout}")
    isql = reference_root / "gen" / "Release" / "firebird" / "bin" / "isql"
    if not isql.exists():
        raise AssertionError(f"Firebird isql was not built: {isql}")
    return isql


def ensure_sqlite_cli(repo_root: pathlib.Path,
                      build_root: pathlib.Path,
                      timeout_seconds: int) -> pathlib.Path:
    source_root = repo_root / SQLITE_SOURCE_REL
    build_dir = build_root / "reference" / "sqlite-3.53.0-tools"
    staged_source = build_root / "reference" / "sqlite-3.53.0-source"
    sqlite3 = build_dir / "sqlite3"
    if sqlite3.exists():
        return sqlite3
    if not staged_source.exists():
        shutil.copytree(source_root, staged_source)
    autosetup_find = staged_source / "autosetup" / "autosetup-find-tclsh"
    if autosetup_find.exists():
        autosetup_find.chmod(autosetup_find.stat().st_mode | stat.S_IXUSR)
    build_dir.mkdir(parents=True, exist_ok=True)
    configure = [
        "sh",
        str(staged_source / "configure"),
        "--disable-shared",
        "--enable-static",
    ]
    result = run_command(configure, cwd=build_dir, timeout_seconds=timeout_seconds)
    if result.returncode != 0:
        raise AssertionError(f"SQLite reference tool configure failed:\n{result.stdout}")
    result = run_command(
        ["make", "-j4", "sqlite3"],
        cwd=build_dir,
        timeout_seconds=timeout_seconds,
    )
    if result.returncode != 0:
        raise AssertionError(f"SQLite reference tool build failed:\n{result.stdout}")
    if not sqlite3.exists():
        raise AssertionError(f"SQLite reference tool build completed without sqlite3: {sqlite3}")
    return sqlite3


def find_postgresql_tool(name: str) -> pathlib.Path:
    candidates = {
        "pg_regress": (
            pathlib.Path("/usr/lib/postgresql/18/lib/pgxs/src/test/regress/pg_regress"),
        ),
        "pg_isolation_regress": (
            pathlib.Path("/usr/lib/postgresql/18/lib/pgxs/src/test/isolation/pg_isolation_regress"),
        ),
        "psql": (pathlib.Path("/usr/lib/postgresql/18/bin/psql"),),
        "pgbench": (pathlib.Path("/usr/lib/postgresql/18/bin/pgbench"),),
    }
    for candidate in candidates[name]:
        if candidate.exists():
            return candidate
    raise AssertionError(f"PostgreSQL 18 original tool not found: {name}")


def executable_staged_path(repo_root: pathlib.Path, staged_rel: str) -> pathlib.Path:
    staged = repo_root / staged_rel
    ensure_under(staged, repo_root / "project/tests/reference_regression")
    return staged


def copy_tool(spec: ToolSpec, repo_root: pathlib.Path) -> dict[str, object]:
    staged = executable_staged_path(repo_root, spec.staged_rel)
    staged.parent.mkdir(parents=True, exist_ok=True)
    if not spec.source.exists():
        raise ExternalReferenceFixtureMissing(
            f"{spec.tool_id} external reference tool missing: {spec.staged_rel}"
        )
    same_path = spec.source.resolve() == staged.resolve()
    if spec.copy_tree and not same_path:
        if staged.exists():
            shutil.rmtree(staged)
        shutil.copytree(spec.source, staged)
    elif not spec.copy_tree and not same_path:
        shutil.copy2(spec.source, staged)
    if staged.is_file():
        mode = staged.stat().st_mode
        staged.chmod(mode | stat.S_IXUSR)

    digest_paths = [staged]
    if spec.extra_tree_source is not None:
        if not spec.extra_tree_name:
            raise AssertionError(f"{spec.tool_id} extra tree requires staged name")
        extra_dst = staged.parent / spec.extra_tree_name
        if extra_dst.exists():
            shutil.rmtree(extra_dst)
        shutil.copytree(spec.extra_tree_source, extra_dst)
        digest_paths.append(extra_dst)

    digest = (
        tree_digest(staged)
        if staged.is_dir() and spec.extra_tree_source is None
        else paths_digest(digest_paths, staged.parent)
    )
    return {
        "dialect": spec.dialect,
        "tool_id": spec.tool_id,
        "staged_locator": spec.staged_rel,
        "source_kind": "directory" if spec.source.is_dir() else "file",
        "source_basename": spec.source.name,
        "staged": True,
        **digest,
    }


def smoke_tool(spec: ToolSpec,
               repo_root: pathlib.Path,
               timeout_seconds: int) -> dict[str, object]:
    staged = executable_staged_path(repo_root, spec.staged_rel)
    command = [part.format(tool=str(staged)) for part in spec.smoke_command]
    result = run_command(command, cwd=staged.parent, timeout_seconds=timeout_seconds)
    if result.returncode not in spec.allowed_return_codes:
        raise AssertionError(
            f"{spec.tool_id} smoke returned {result.returncode}, expected "
            f"{spec.allowed_return_codes}:\n{result.stdout}"
        )
    for fragment in spec.expected_smoke_fragments:
        if fragment not in result.stdout:
            raise AssertionError(f"{spec.tool_id} smoke missing {fragment!r}:\n{result.stdout}")
    return {
        "tool_id": spec.tool_id,
        "returncode": result.returncode,
        "output_digest": hashlib.sha256(result.stdout.encode("utf-8")).hexdigest(),
        "output_sample": result.stdout.splitlines()[:2],
    }


def build_tool_specs(repo_root: pathlib.Path,
                     build_root: pathlib.Path,
                     timeout_seconds: int) -> list[ToolSpec]:
    _ = (build_root, timeout_seconds)
    return [
        ToolSpec(
            "firebird",
            "firebird-isql",
            repo_root / "project/tests/reference_regression/firebird/native_tool_harness/tools/isql",
            "project/tests/reference_regression/firebird/native_tool_harness/tools/isql",
            ("{tool}", "-z"),
            ("ISQL Version",),
        ),
        ToolSpec(
            "mysql",
            "mysql-client",
            repo_root / "project/tests/reference_regression/mysql/native_tool_harness/tools/mysql",
            "project/tests/reference_regression/mysql/native_tool_harness/tools/mysql",
            ("{tool}", "--version"),
            ("Ver 8.4.8", "Source distribution"),
        ),
        ToolSpec(
            "mysql",
            "mysqltest",
            repo_root / "project/tests/reference_regression/mysql/native_tool_harness/tools/mysqltest",
            "project/tests/reference_regression/mysql/native_tool_harness/tools/mysqltest",
            ("{tool}", "--version"),
            ("Ver 8.4.8", "Source distribution"),
        ),
        ToolSpec(
            "postgresql",
            "pg-regress",
            repo_root / "project/tests/reference_regression/postgresql/native_tool_harness/tools/pg_regress",
            "project/tests/reference_regression/postgresql/native_tool_harness/tools/pg_regress",
            ("{tool}", "--version"),
            ("pg_regress (PostgreSQL) 18",),
        ),
        ToolSpec(
            "postgresql",
            "pg-isolation-regress",
            repo_root / "project/tests/reference_regression/postgresql/native_tool_harness/tools/pg_isolation_regress",
            "project/tests/reference_regression/postgresql/native_tool_harness/tools/pg_isolation_regress",
            ("{tool}", "--version"),
            ("pg_regress (PostgreSQL) 18",),
        ),
        ToolSpec(
            "postgresql",
            "psql",
            repo_root / "project/tests/reference_regression/postgresql/native_tool_harness/tools/psql",
            "project/tests/reference_regression/postgresql/native_tool_harness/tools/psql",
            ("{tool}", "--version"),
            ("psql (PostgreSQL) 18",),
        ),
        ToolSpec(
            "postgresql",
            "pgbench",
            repo_root / "project/tests/reference_regression/postgresql/native_tool_harness/tools/pgbench",
            "project/tests/reference_regression/postgresql/native_tool_harness/tools/pgbench",
            ("{tool}", "--version"),
            ("pgbench (PostgreSQL) 18",),
        ),
        ToolSpec(
            "sqlite",
            "sqlite3-cli",
            repo_root / "project/tests/reference_regression/sqlite/native_tool_harness/tools/sqlite3",
            "project/tests/reference_regression/sqlite/native_tool_harness/tools/sqlite3",
            ("{tool}", "--version"),
            ("3.53.0",),
        ),
    ]


def replay_cases() -> list[ReplayCase]:
    return [
        ReplayCase(
            "firebird",
            "FIREBIRD-EMPDDL-DOMAIN",
            f"{FIREBIRD_SOURCE_REL}/examples/empbuild/empddl.sql",
            "CREATE DOMAIN firstname     AS VARCHAR(15);",
            "CREATE DOMAIN firstname AS VARCHAR(15)",
            "firebird.datatype.domain.create",
            "CREATE_DOMAIN",
            "",
        ),
        ReplayCase(
            "firebird",
            "FIREBIRD-EMPDDL-TABLE",
            f"{FIREBIRD_SOURCE_REL}/examples/empbuild/empddl.sql",
            "CREATE TABLE country",
            "CREATE TABLE country (country COUNTRYNAME NOT NULL PRIMARY KEY, currency VARCHAR(10) NOT NULL)",
            "firebird.ddl.create",
            "CREATE_TABLE",
            "",
        ),
        ReplayCase(
            "firebird",
            "FIREBIRD-EMPDML-INSERT",
            f"{FIREBIRD_SOURCE_REL}/examples/empbuild/empdml.sql",
            "INSERT INTO country (country, currency) VALUES ('USA',         'Dollar');",
            "INSERT INTO country (country, currency) VALUES ('USA', 'Dollar')",
            "firebird.dml.insert",
            "INSERT",
            "",
        ),
        ReplayCase(
            "firebird",
            "FIREBIRD-EMPDML-UPDATE",
            f"{FIREBIRD_SOURCE_REL}/examples/empbuild/empdml.sql",
            "UPDATE customer SET on_hold = '*' WHERE cust_no = 1002;",
            "UPDATE customer SET on_hold = '*' WHERE cust_no = 1002",
            "firebird.dml.update",
            "UPDATE",
            "",
        ),
        ReplayCase(
            "firebird",
            "FIREBIRD-PARSER-PSQL-EXECUTE-BLOCK",
            "project/tests/firebird_parser_worker/firebird_parser_pipeline_probe.cpp",
            "EXECUTE BLOCK AS BEGIN SUSPEND; END",
            "EXECUTE BLOCK AS BEGIN SUSPEND; END",
            "firebird.psql.execute_block",
            "EXECUTE_BLOCK",
            "",
            provenance="tracked_firebird_parser_regression",
        ),
        ReplayCase(
            "firebird",
            "FIREBIRD-PARSER-DDL-DATATYPE-TABLE",
            "project/tests/firebird_parser_worker/firebird_parser_pipeline_probe.cpp",
            "CREATE TABLE customer (id INTEGER NOT NULL, name VARCHAR(40))",
            "CREATE TABLE customer (id INTEGER NOT NULL, name VARCHAR(40))",
            "firebird.ddl.create",
            "CREATE_TABLE",
            "",
            validation_tags=("datatype",),
            expected_datatype_families=("numeric", "text"),
            provenance="tracked_firebird_parser_regression",
        ),
        ReplayCase(
            "firebird",
            "FIREBIRD-PARSER-PSQL-FUNCTION",
            "project/tests/firebird_parser_worker/firebird_parser_pipeline_probe.cpp",
            "CREATE FUNCTION calc_total RETURNS INTEGER AS BEGIN RETURN 1; END",
            "CREATE FUNCTION calc_total RETURNS INTEGER AS BEGIN RETURN 1; END",
            "firebird.ddl.create.function",
            "CREATE_FUNCTION",
            "",
            validation_tags=("procedural",),
            provenance="tracked_firebird_parser_regression",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-INSERT-BASIC",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/insert.test",
            "insert into t1 values (1);",
            "insert into t1 values (1)",
            "mysql.dml.insert",
            "INSERT",
            "admitted_sblr",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-INSERT-ON-DUPLICATE",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/insert.test",
            "insert into t1 values (2, 2) on duplicate key update data= data + 10;",
            "insert into t1 values (2, 2) on duplicate key update data= data + 10",
            "mysql.dml.insert",
            "INSERT",
            "admitted_sblr",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-SHOW-TABLES",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/show_variables.test",
            "SHOW TABLES WHERE 0;",
            "SHOW TABLES WHERE 0",
            "mysql.catalog_overlay.show",
            "SHOW",
            "catalog_projection",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-LOAD-DATA-FILE-REFUSAL",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/loaddata.test",
            "load data infile '../../std_data/loaddata1.dat' ignore into table t1 fields terminated by ',';",
            "load data infile '../../std_data/loaddata1.dat' ignore into table t1 fields terminated by ','",
            "mysql.bulk_io.load_data_infile",
            "LOAD_DATA",
            "policy_refusal_fail_closed",
            "MYSQL.AUTHORITY.FILE_IO_DENIED",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-LOAD-FILE-REFUSAL",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/loaddata_special.test",
            'select load_file("/proc/uptime");',
            'select load_file("/proc/uptime")',
            "mysql.bulk_io.load_file",
            "SELECT",
            "policy_refusal_fail_closed",
            "MYSQL.AUTHORITY.FILE_IO_DENIED",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-MAINTENANCE-ANALYZE",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/func_rand.test",
            "ANALYZE TABLE t;",
            "ANALYZE TABLE t",
            "mysql.maintenance.analyze_table",
            "ANALYZE",
            "unsupported_refusal_fail_closed",
            "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-MAINTENANCE-OPTIMIZE",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/alter_table_myisam.test",
            "OPTIMIZE TABLE t1;",
            "OPTIMIZE TABLE t1",
            "mysql.maintenance.optimize_table",
            "OPTIMIZE",
            "unsupported_refusal_fail_closed",
            "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-MAINTENANCE-CHECK",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/myisampack.test",
            "CHECK TABLE t1;",
            "CHECK TABLE t1",
            "mysql.maintenance.check_table",
            "CHECK",
            "unsupported_refusal_fail_closed",
            "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-MAINTENANCE-REPAIR",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/myisampack.test",
            "repair table t1;",
            "repair table t1",
            "mysql.maintenance.repair_table",
            "REPAIR",
            "unsupported_refusal_fail_closed",
            "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-MAINTENANCE-FLUSH",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/flush_block_commit.test",
            "FLUSH TABLES WITH READ LOCK;",
            "FLUSH TABLES WITH READ LOCK",
            "mysql.maintenance.flush",
            "FLUSH",
            "unsupported_refusal_fail_closed",
            "MYSQL.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-BINLOG-RESET-REPLICATION-UDR",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/no_binlog_gtid_next_partially_failed_stmts_error.test",
            "RESET BINARY LOGS AND GTIDS;",
            "RESET BINARY LOGS AND GTIDS",
            "mysql.replication.reset_binary_logs",
            "RESET",
            "parser_support_udr",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-XA-REFUSAL",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/xa_gtid.test",
            "XA START 'xa1';",
            "XA START 'xa1'",
            "mysql.transaction.xa",
            "XA",
            "unsupported_refusal_fail_closed",
            "MYSQL.AUTHORITY.XA_DENIED",
        ),
        ReplayCase(
            "mysql",
            "MYSQL-TEST-ROUTINE-PROCEDURE",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/alter_table_myisam.test",
            "CREATE PROCEDURE sp1() INSERT INTO t2(d) VALUES(10);",
            "CREATE PROCEDURE sp1() INSERT INTO t2(d) VALUES(10)",
            "mysql.routine.procedure.create",
            "CREATE_PROCEDURE",
            "parser_support_udr",
            validation_tags=("procedural",),
        ),
        ReplayCase(
            "mysql",
            "MYSQL-TEST-DATATYPE-HEAVY-DDL",
            f"{MYSQL_SOURCE_REL}/mysql-test/t/insert_select.test",
            "CREATE TABLE t1 ( USID INTEGER UNSIGNED, ServerID TINYINT UNSIGNED, State ENUM ('unknown', 'Access-Granted', 'Session-Active', 'Session-Closed' ) NOT NULL DEFAULT 'unknown'",
            (
                "CREATE TABLE t1 ( USID INTEGER UNSIGNED, "
                "ServerID TINYINT UNSIGNED, "
                "State ENUM ('unknown', 'Access-Granted', 'Session-Active', "
                "'Session-Closed' ) NOT NULL DEFAULT 'unknown', "
                "SessionID CHAR(32), User CHAR(32) NOT NULL DEFAULT '<UNKNOWN>', "
                "AccessRequestTime DATETIME, AcctStartTime DATETIME, "
                "LastModification TIMESTAMP NOT NULL)"
            ),
            "mysql.ddl.create",
            "CREATE_TABLE",
            "admitted_sblr",
            validation_tags=("datatype",),
            expected_datatype_families=(
                "numeric",
                "text",
                "temporal",
                "enum_set",
            ),
        ),
        ReplayCase(
            "mysql",
            "MYSQL-TEST-CHARSET-COLLATION-DATATYPE-DDL",
            f"{MYSQL_SOURCE_REL}/mysql-test/include/ctype_utf8mb4.inc",
            "eval create table t1 (id integer, a varchar(100) character set utf8mb4 collate utf8mb4_unicode_ci) engine $engine;",
            (
                "CREATE TABLE t1 (id integer, a varchar(100) "
                "character set utf8mb4 collate utf8mb4_unicode_ci)"
            ),
            "mysql.ddl.create",
            "CREATE_TABLE",
            "admitted_sblr",
            validation_tags=("datatype",),
            expected_datatype_families=(
                "numeric",
                "text",
                "charset_collation_sensitive_text",
            ),
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-COPY-STDIN",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/copy.sql",
            "copy copytest2(test) from stdin;",
            "copy copytest2(test) from stdin",
            "postgresql.logical_stream.copy_from_stdin",
            "COPY",
            "parser_support_udr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-COPY-TO-PSQL-FILE-REFUSAL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/copy.sql",
            "copy copytest to :'filename' csv;",
            "copy copytest to :'filename' csv",
            "postgresql.bulk_io.copy_to_file",
            "COPY",
            "policy_refusal_fail_closed",
            "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-COPY-FROM-PSQL-FILE-REFUSAL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/copy.sql",
            "copy copytest2 from :'filename' csv;",
            "copy copytest2 from :'filename' csv",
            "postgresql.bulk_io.copy_from_file",
            "COPY",
            "policy_refusal_fail_closed",
            "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-VACUUM-PARALLEL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/vacuum.sql",
            "VACUUM (PARALLEL 0) pvactst; -- disable parallel vacuum",
            "VACUUM (PARALLEL 0) pvactst",
            "postgresql.maintenance.vacuum",
            "VACUUM",
            "unsupported_refusal_fail_closed",
            "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-CREATE-FOREIGN-TABLE-CONNECTOR-REFUSAL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/copy.sql",
            "create foreign table copytest_foreign_table (a int) server copytest_server;",
            "create foreign table copytest_foreign_table (a int) server copytest_server",
            "postgresql.connector.foreign_table.create",
            "CREATE_FOREIGN",
            "parser_support_udr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-TYPE-CAST-SELECT",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/copy.sql",
            "select tableoid::regclass,count(*),sum(a) from parted_copytest",
            "select tableoid::regclass,count(*),sum(a) from parted_copytest",
            "postgresql.query.select",
            "SELECT",
            "admitted_sblr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-PREPARE",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/prepare.sql",
            "PREPARE q1 AS SELECT 1 AS a;",
            "PREPARE q1 AS SELECT 1 AS a",
            "postgresql.prepared.prepare",
            "PREPARE",
            "admitted_sblr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-EXECUTE",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/prepare.sql",
            "EXECUTE q1;",
            "EXECUTE q1",
            "postgresql.prepared.execute",
            "EXECUTE",
            "admitted_sblr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-DEALLOCATE",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/prepare.sql",
            "DEALLOCATE q1;",
            "DEALLOCATE q1",
            "postgresql.prepared.deallocate",
            "DEALLOCATE",
            "admitted_sblr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-CURSOR-MOVE",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/xmlmap.sql",
            "MOVE BACKWARD ALL IN xc;",
            "MOVE BACKWARD ALL IN xc",
            "postgresql.cursor.move",
            "MOVE",
            "admitted_sblr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-CURSOR-CLOSE",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/tablesample.sql",
            "CLOSE tablesample_cur;",
            "CLOSE tablesample_cur",
            "postgresql.cursor.close",
            "CLOSE",
            "admitted_sblr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-LOCK-TABLE",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/create_am.sql",
            "LOCK TABLE fast_emp4000;",
            "LOCK TABLE fast_emp4000",
            "postgresql.locking.lock_table",
            "LOCK",
            "admitted_sblr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-DISCARD",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/sequence.sql",
            "DISCARD SEQUENCES;",
            "DISCARD SEQUENCES",
            "postgresql.session.discard",
            "DISCARD",
            "admitted_sblr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-CHECKPOINT-REFUSAL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/stats.sql",
            "CHECKPOINT;",
            "CHECKPOINT",
            "postgresql.system.checkpoint",
            "CHECKPOINT",
            "unsupported_refusal_fail_closed",
            "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-SECURITY-LABEL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/security_label.sql",
            "SECURITY LABEL ON TABLE seclabel_tbl1 IS 'classified';",
            "SECURITY LABEL ON TABLE seclabel_tbl1 IS 'classified'",
            "postgresql.security.security_label",
            "SECURITY",
            "parser_support_udr",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-REFRESH-MATERIALIZED-VIEW-REFUSAL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/privileges.sql",
            "REFRESH MATERIALIZED VIEW sro_mv;",
            "REFRESH MATERIALIZED VIEW sro_mv",
            "postgresql.maintenance.refresh_materialized_view",
            "REFRESH",
            "unsupported_refusal_fail_closed",
            "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-ALTER-TABLESPACE-REFUSAL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/tablespace.sql",
            "ALTER TABLESPACE regress_tblspace SET (random_page_cost = 1.0, seq_page_cost = 1.1);",
            "ALTER TABLESPACE regress_tblspace SET (random_page_cost = 1.0, seq_page_cost = 1.1)",
            "postgresql.storage.tablespace.alter",
            "ALTER_TABLESPACE",
            "policy_refusal_fail_closed",
            "POSTGRESQL.AUTHORITY.TABLESPACE_DENIED",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-DROP-TABLESPACE-REFUSAL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/tablespace.sql",
            "DROP TABLESPACE regress_tblspacewith;",
            "DROP TABLESPACE regress_tblspacewith",
            "postgresql.storage.tablespace.drop",
            "DROP_TABLESPACE",
            "policy_refusal_fail_closed",
            "POSTGRESQL.AUTHORITY.TABLESPACE_DENIED",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-PREPARE-TRANSACTION-REFUSAL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/temp.sql",
            "prepare transaction 'twophase_func';",
            "prepare transaction 'twophase_func'",
            "postgresql.transaction.prepare_transaction",
            "PREPARE",
            "unsupported_refusal_fail_closed",
            "POSTGRESQL.AUTHORITY.PREPARE_TRANSACTION_DENIED",
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-REGRESS-PROCEDURE",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/object_address.sql",
            "CREATE PROCEDURE addr_nsp.proc(int4) LANGUAGE SQL AS $$ $$;",
            "CREATE PROCEDURE addr_nsp.proc(int4) LANGUAGE SQL AS $$ $$",
            "postgresql.routine.procedure.create",
            "CREATE_PROCEDURE",
            "parser_support_udr",
            validation_tags=("procedural",),
        ),
        ReplayCase(
            "postgresql",
            "POSTGRESQL-REGRESS-DATATYPE-HEAVY-DDL",
            f"{POSTGRESQL_SOURCE_REL}/src/test/regress/sql/brin_multi.sql",
            "CREATE TABLE brintest_multi (",
            (
                "CREATE TABLE brintest_multi ("
                "int8col bigint, int2col smallint, int4col integer, "
                "float4col real, float8col double precision, "
                "macaddrcol macaddr, macaddr8col macaddr8, "
                "inetcol inet, cidrcol cidr, datecol date, "
                "timecol time without time zone, "
                "timestampcol timestamp without time zone, "
                "timestamptzcol timestamp with time zone, "
                "intervalcol interval, numericcol numeric, uuidcol uuid)"
            ),
            "postgresql.ddl.create",
            "CREATE_TABLE",
            "admitted_sblr",
            validation_tags=("datatype",),
            expected_datatype_families=(
                "numeric",
                "floating",
                "temporal",
                "uuid",
                "network",
                "exact_decimal",
            ),
        ),
        ReplayCase(
            "sqlite",
            "SQLITE-JSON-SELECT",
            f"{SQLITE_SOURCE_REL}/test/json108.test",
            "SELECT count(*) FROM t1 WHERE json(j0)==json(json_pretty(j0,NULL));",
            "SELECT count(*) FROM t1 WHERE json(j0)==json(json_pretty(j0,NULL))",
            "sqlite.query.select",
            "SELECT",
            "admitted_sblr",
        ),
        ReplayCase(
            "sqlite",
            "SQLITE-CREATE-TEMP-TABLE",
            f"{SQLITE_SOURCE_REL}/test/json108.test",
            "CREATE TEMP TABLE t1(j0,j5);",
            "CREATE TEMP TABLE t1(j0,j5)",
            "sqlite.ddl.create_temp",
            "CREATE_TEMP",
            "admitted_sblr",
        ),
        ReplayCase(
            "sqlite",
            "SQLITE-PRAGMA-UDR",
            f"{SQLITE_SOURCE_REL}/test/veryquick.test",
            "quick.test",
            "PRAGMA table_info(t)",
            "sqlite.pragma.generic",
            "PRAGMA",
            "parser_support_udr",
            provenance="reference_release_packet_regression_launcher",
        ),
        ReplayCase(
            "mariadb",
            "MARIADB-SEQUENCE-CREATE",
            f"{MARIADB_SOURCE_REL}/mysql-test/suite/rpl/t/sequence.test",
            "CREATE SEQUENCE s1 cache=10;",
            "CREATE SEQUENCE s1 cache=10",
            "mariadb.sequence.create",
            "CREATE_SEQUENCE",
            "admitted_sblr",
        ),
        ReplayCase(
            "mariadb",
            "MARIADB-INSERT-RETURNING",
            f"{MARIADB_SOURCE_REL}/mysql-test/main/insert_returning.test",
            "INSERT INTO t1 (id1, val1) VALUES (2, 'b') RETURNING *;",
            "INSERT INTO t1 (id1, val1) VALUES (2, 'b') RETURNING *",
            "mariadb.dml.returning",
            "INSERT",
            "admitted_sblr",
        ),
        ReplayCase(
            "mariadb",
            "MARIADB-HANDLER-UDR",
            f"{MARIADB_SOURCE_REL}/mysql-test/suite/storage_engine/handler.test",
            "HANDLER t1 OPEN AS h1;",
            "HANDLER t1 OPEN AS h1",
            "mariadb.handler.cursor",
            "HANDLER",
            "parser_support_udr",
        ),
        ReplayCase(
            "mariadb",
            "MARIADB-LOAD-DATA-REFUSAL",
            f"{MARIADB_SOURCE_REL}/mysql-test/main/loaddata.test",
            "load data infile '../../std_data/loaddata1.dat' ignore into table t1 fields terminated by ',';",
            "load data infile '../../std_data/loaddata1.dat' ignore into table t1 fields terminated by ','",
            "mariadb.bulk_io.load_data_infile",
            "LOAD_DATA",
            "policy_refusal_fail_closed",
            "MARIADB.AUTHORITY.FILE_IO_DENIED",
        ),
        ReplayCase(
            "mariadb",
            "MARIADB-INSTALL-SONAME-REFUSAL",
            f"{MARIADB_SOURCE_REL}/mysql-test/main/plugin.test",
            "INSTALL SONAME 'ha_example';",
            "INSTALL SONAME 'ha_example'",
            "mariadb.plugin.install_soname",
            "INSTALL",
            "policy_refusal_fail_closed",
            "MARIADB.AUTHORITY.PLUGIN_DENIED",
        ),
        ReplayCase(
            "duckdb",
            "DUCKDB-CREATE-SECRET-UDR",
            f"{DUCKDB_SOURCE_REL}/test/sql/secrets/create_secret_expression.test",
            "CREATE SECRET a (TYPE http);",
            "CREATE SECRET a",
            "duckdb.security.create_secret",
            "CREATE_SECRET",
            "parser_support_udr",
        ),
        ReplayCase(
            "duckdb",
            "DUCKDB-COPY-FILE-REFUSAL",
            f"{DUCKDB_SOURCE_REL}/test/sql/copy/row_groups_per_file.test",
            "COPY bigdata TO '__TEST_DIR__/row_groups_per_file1' (FORMAT PARQUET, ROW_GROUP_SIZE 2000, ROW_GROUPS_PER_FILE 1)",
            "COPY bigdata TO '__TEST_DIR__/row_groups_per_file1' (FORMAT PARQUET, ROW_GROUP_SIZE 2000, ROW_GROUPS_PER_FILE 1)",
            "duckdb.bulk_io.file_to",
            "COPY",
            "policy_refusal_fail_closed",
            "DUCKDB.AUTHORITY.FILE_IO_DENIED",
        ),
        ReplayCase(
            "duckdb",
            "DUCKDB-PIVOT",
            f"{DUCKDB_SOURCE_REL}/test/sql/pivot/test_pivot.test",
            "PIVOT",
            "PIVOT monthly_sales ON month USING sum(amount)",
            "duckdb.query.pivot",
            "PIVOT",
            "admitted_sblr",
        ),
        ReplayCase(
            "clickhouse",
            "CLICKHOUSE-COUNT-SELECT",
            f"{CLICKHOUSE_SOURCE_REL}/tests/queries/0_stateless/00001_count_hits.sql",
            "SELECT count() FROM test.hits",
            "SELECT count() FROM test.hits",
            "clickhouse.query.select",
            "SELECT",
            "admitted_sblr",
        ),
        ReplayCase(
            "clickhouse",
            "CLICKHOUSE-CREATE-DICTIONARY-CONNECTOR-REFUSAL",
            f"{CLICKHOUSE_SOURCE_REL}/tests/queries/0_stateless/01018_ddl_dictionaries_create.sql",
            "CREATE DICTIONARY db_01018.dict1",
            "CREATE DICTIONARY db_01018.dict1",
            "clickhouse.connector.dictionary.create",
            "CREATE_DICTIONARY",
            "policy_refusal_fail_closed",
            "CLICKHOUSE.EMULATION.CONNECTOR_ROUTE",
        ),
        ReplayCase(
            "clickhouse",
            "CLICKHOUSE-OPTIMIZE-REFUSAL",
            f"{CLICKHOUSE_SOURCE_REL}/tests/queries/0_stateless/01006_ttl_with_default_2.sql",
            "OPTIMIZE TABLE ttl_with_default FINAL;",
            "OPTIMIZE TABLE ttl_with_default FINAL",
            "clickhouse.maintenance.optimize",
            "OPTIMIZE",
            "unsupported_refusal_fail_closed",
            "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "clickhouse",
            "CLICKHOUSE-S3-ETL-UDR",
            f"{CLICKHOUSE_SOURCE_REL}/tests/queries/0_stateless/02752_forbidden_headers.sql",
            "SELECT * FROM s3('http://localhost:8123/123/4', LineAsString, headers('exact_header' = 'value'));",
            "SELECT * FROM s3('http://localhost:8123/123/4', LineAsString, headers('exact_header' = 'value'))",
            "clickhouse.external_io.s3_function",
            "SELECT",
            "parser_support_udr",
        ),
        ReplayCase(
            "tidb",
            "TIDB-BUILTIN-VERSION",
            f"{TIDB_SOURCE_REL}/README.md",
            "TiDB is compatible with MySQL 8.0",
            "select tidb_version()",
            "tidb.builtin.version",
            "SELECT",
            "catalog_projection",
        ),
        ReplayCase(
            "tidb",
            "TIDB-SPLIT-TABLE-CLUSTER-ROUTE",
            f"{TIDB_SOURCE_REL}/pkg/parser/parser_test.go",
            "split table t1 index idx1 by ('a'),('b'),('c')",
            "split table t between (0) and (5000) regions 5",
            "tidb.placement.split_table",
            "SPLIT",
            "admitted_sblr",
        ),
        ReplayCase(
            "tidb",
            "TIDB-ADMIN-CHECKSUM-REFUSAL",
            f"{TIDB_SOURCE_REL}/pkg/disttask/importinto/task_executor_testkit_test.go",
            'res := tk.MustQuery("admin checksum table t").Rows()',
            "admin checksum table t",
            "tidb.admin.checksum_table",
            "ADMIN",
            "policy_refusal_fail_closed",
            "TIDB.AUTHORITY.ADMIN_DENIED",
        ),
        ReplayCase(
            "vitess",
            "VITESS-KEYSPACE-SELECT",
            f"{VITESS_SOURCE_REL}/examples/demo/schema/product/product_schema.sql",
            "keyspace_id varbinary(10)",
            "select keyspace_id from corder_keyspace_idx",
            "vitess.query.select",
            "SELECT",
            "admitted_sblr",
        ),
        ReplayCase(
            "vitess",
            "VITESS-MOVE-TABLES-VREPLICATION-UDR",
            f"{VITESS_SOURCE_REL}/examples/local/202_move_tables.sh",
            'vtctldclient MoveTables --workflow commerce2customer --target-keyspace customer create --source-keyspace commerce --tables "customer,corder"',
            "move tables commerce.customer",
            "vitess.vreplication.move_tables",
            "MOVE",
            "parser_support_udr",
        ),
        ReplayCase(
            "vitess",
            "VITESS-REPARENT-CLUSTER-ROUTE",
            f"{VITESS_SOURCE_REL}/examples/backups/start_cluster.sh",
            'vtctldclient PlannedReparentShard commerce/0 --new-primary "zone1-100"',
            "vtctl reparent shard commerce/0",
            "vitess.topology.reparent",
            "VTCTL",
            "admitted_sblr",
        ),
        ReplayCase(
            "cockroachdb",
            "COCKROACHDB-CRDB-INTERNAL-CATALOG",
            f"{COCKROACHDB_SOURCE_REL}/docs/tech-notes/sql.md",
            'The SQL layer is responsible for providing the "SQL API"',
            "select crdb_internal.node_id()",
            "cockroachdb.catalog_overlay.crdb_internal",
            "SELECT",
            "catalog_projection",
        ),
        ReplayCase(
            "cockroachdb",
            "COCKROACHDB-CHANGEFEED-UDR",
            f"{COCKROACHDB_SOURCE_REL}/pkg/cmd/roachtest/operations/changefeeds/utils.go",
            'fmt.Sprintf("CREATE CHANGEFEED FOR TABLE %s.%s INTO',
            "create changefeed for table accounts into 'kafka://broker'",
            "cockroachdb.changefeed.create",
            "CREATE_CHANGEFEED",
            "parser_support_udr",
        ),
        ReplayCase(
            "cockroachdb",
            "COCKROACHDB-BACKUP-REFUSAL",
            f"{COCKROACHDB_SOURCE_REL}/pkg/backup/schedule_pts_chaining_test.go",
            "BACKUP DATABASE db INTO 'nodelocal://1/%s'",
            "backup database d to 'nodelocal://1/a'",
            "cockroachdb.backup.backup",
            "BACKUP",
            "policy_refusal_fail_closed",
            "COCKROACHDB.AUTHORITY.BACKUP_DENIED",
        ),
        ReplayCase(
            "yugabytedb",
            "YUGABYTEDB-SERVER-REGION-CATALOG",
            f"{YUGABYTEDB_SOURCE_REL}/sample/schema.sql",
            "CREATE TABLE users(",
            "select yb_server_region()",
            "yugabytedb.catalog_overlay.server_region",
            "SELECT",
            "catalog_projection",
        ),
        ReplayCase(
            "yugabytedb",
            "YUGABYTEDB-SPLIT-INTO-UDR",
            f"{YUGABYTEDB_SOURCE_REL}/python/yugabyte/test_test_descriptor.py",
            "tests-tools/yb-backup-cross-feature-test:::YBBackupTest.TestYSQLManualTabletSplit",
            "create table t (id int primary key) split into 3 tablets",
            "yugabytedb.tablet.split_into",
            "CREATE_TABLE",
            "parser_support_udr",
        ),
        ReplayCase(
            "yugabytedb",
            "YUGABYTEDB-BACKUP-REFUSAL",
            f"{YUGABYTEDB_SOURCE_REL}/architecture/design/distributed-backup-and-restore.md",
            "An entire database would get backed up.",
            "backup database d to '/tmp/d'",
            "yugabytedb.backup.backup",
            "BACKUP",
            "policy_refusal_fail_closed",
            "YUGABYTEDB.AUTHORITY.BACKUP_DENIED",
        ),
        ReplayCase(
            "cassandra",
            "CASSANDRA-CQL-SELECT-JSON",
            f"{CASSANDRA_SOURCE_REL}/src/antlr/Parser.g",
            "selectStatement returns [SelectStatement.RawStatement expr]",
            "select json * from ks.t",
            "cassandra.query.select_json",
            "SELECT",
            "admitted_sblr",
        ),
        ReplayCase(
            "cassandra",
            "CASSANDRA-CQL-CREATE-KEYSPACE-UDR",
            f"{CASSANDRA_SOURCE_REL}/src/antlr/Parser.g",
            "createKeyspaceStatement returns [CreateKeyspaceStatement.Raw stmt]",
            "create keyspace ks with replication = {'class':'SimpleStrategy'}",
            "cassandra.keyspace.create",
            "CREATE_KEYSPACE",
            "parser_support_udr",
        ),
        ReplayCase(
            "cassandra",
            "CASSANDRA-REPAIR-REFUSAL",
            f"{CASSANDRA_SOURCE_REL}/test/unit/org/apache/cassandra/tools/nodetool/SetAutoRepairConfigTest.java",
            "public void testRepairSchedulingDisabled()",
            "repair ks",
            "cassandra.admin.repair",
            "REPAIR",
            "unsupported_refusal_fail_closed",
            "CASSANDRA.AUTHORITY.UNSUPPORTED_DENIED",
        ),
        ReplayCase(
            "mongodb",
            "MONGODB-FIND-DOCUMENT",
            f"{MONGODB_SOURCE_REL}/src/mongo/db/query/find_command.idl",
            "command_name: find",
            "find users { status: 'A' }",
            "mongodb.query.find",
            "FIND",
            "admitted_sblr",
        ),
        ReplayCase(
            "mongodb",
            "MONGODB-AGGREGATE-OUT-UDR",
            f"{MONGODB_SOURCE_REL}/src/mongo/db/pipeline/aggregate_command.idl",
            "command_name: aggregate",
            "aggregate orders [ {$match:{status:'A'}}, {$out:'archived'} ]",
            "mongodb.aggregate.out",
            "AGGREGATE",
            "parser_support_udr",
        ),
        ReplayCase(
            "mongodb",
            "MONGODB-SHARDING-CLUSTER-ROUTE",
            f"{MONGODB_SOURCE_REL}/src/mongo/shell/utils_sh.js",
            "sh.shardCollection = function(fullName, key, unique, options)",
            "sh.shardCollection('db.c',{_id:1})",
            "mongodb.sharding.shard_collection",
            "SH",
            "admitted_sblr",
        ),
        ReplayCase(
            "redis",
            "REDIS-SET-KV",
            f"{REDIS_SOURCE_REL}/src/commands/set.json",
            "\"SET\"",
            "set account:1 active",
            "redis.kv.set",
            "SET",
            "admitted_sblr",
        ),
        ReplayCase(
            "redis",
            "REDIS-EVAL-UDR",
            f"{REDIS_SOURCE_REL}/src/commands/eval.json",
            "\"EVAL\"",
            "eval \"return redis.call('get', KEYS[1])\" 1 account:1",
            "redis.script.eval",
            "EVAL",
            "parser_support_udr",
        ),
        ReplayCase(
            "redis",
            "REDIS-CLUSTER-ROUTE",
            f"{REDIS_SOURCE_REL}/src/commands/cluster.json",
            "\"CLUSTER\"",
            "cluster nodes",
            "redis.cluster.command",
            "CLUSTER",
            "admitted_sblr",
        ),
        ReplayCase(
            "opensearch_sql_ppl",
            "OPENSEARCH-SQL-SELECT",
            f"{OPENSEARCH_SQL_PPL_SOURCE_REL}/language-grammar/src/main/antlr4/OpenSearchSQLParser.g4",
            "SELECT selectSpec? selectElements",
            "select count(*) from accounts",
            "opensearch_sql_ppl.sql.select",
            "SELECT",
            "admitted_sblr",
        ),
        ReplayCase(
            "opensearch_sql_ppl",
            "OPENSEARCH-PPL-STATS",
            f"{OPENSEARCH_SQL_PPL_SOURCE_REL}/language-grammar/src/main/antlr4/OpenSearchPPLParser.g4",
            "statsCommand",
            "source=accounts | stats count() by state",
            "opensearch_sql_ppl.ppl.stats",
            "SOURCE",
            "admitted_sblr",
        ),
        ReplayCase(
            "opensearch_sql_ppl",
            "OPENSEARCH-SQL-PPL-PLUGIN-POST",
            f"{OPENSEARCH_SQL_PPL_SOURCE_REL}/plugin/src/main/java/org/opensearch/sql/plugin/rest/RestPPLQueryAction.java",
            'QUERY_API_ENDPOINT = "/_plugins/_ppl"',
            'POST /_plugins/_ppl {"query":"source=accounts | stats count() by state"}',
            "opensearch_sql_ppl.rest.post",
            "POST",
            "parser_support_udr",
        ),
        ReplayCase(
            "opensearch_sql_ppl",
            "OPENSEARCH-SQL-PPL-CLUSTER-ROUTE",
            f"{OPENSEARCH_SQL_PPL_SOURCE_REL}/language-grammar/src/main/antlr4/OpenSearchPPLParser.g4",
            "pplStatement",
            "GET /_cluster/health",
            "opensearch_sql_ppl.cluster.get",
            "GET",
            "admitted_sblr",
        ),
        ReplayCase(
            "opensearch",
            "OPENSEARCH-REST-SEARCH",
            f"{OPENSEARCH_SOURCE_REL}/rest-api-spec/src/main/resources/rest-api-spec/api/search.json",
            '"path":"/{index}/_search"',
            'POST /accounts/_search {"query":{"match_all":{}}}',
            "opensearch.search.query",
            "POST",
            "admitted_sblr",
        ),
        ReplayCase(
            "opensearch",
            "OPENSEARCH-REST-CREATE-INDEX",
            f"{OPENSEARCH_SOURCE_REL}/rest-api-spec/src/main/resources/rest-api-spec/api/indices.create.json",
            '"indices.create":{',
            'PUT /accounts {"settings":{}}',
            "opensearch.index.create",
            "PUT",
            "admitted_sblr",
        ),
        ReplayCase(
            "opensearch",
            "OPENSEARCH-REST-CLUSTER-ROUTE",
            f"{OPENSEARCH_SOURCE_REL}/rest-api-spec/src/main/resources/rest-api-spec/api/cluster.health.json",
            '"path":"/_cluster/health"',
            "GET /_cluster/health",
            "opensearch.admin.cluster_health",
            "GET",
            "admitted_sblr",
        ),
        ReplayCase(
            "neo4j",
            "NEO4J-CYPHER-MATCH",
            f"{NEO4J_SOURCE_REL}/community/cypher/front-end/parser/v5/parser/src/main/antlr4/org/neo4j/cypher/internal/parser/v5/Cypher5Parser.g4",
            "matchClause",
            "match (n:Account) return n",
            "neo4j.query.match",
            "MATCH",
            "admitted_sblr",
        ),
        ReplayCase(
            "neo4j",
            "NEO4J-CYPHER-CREATE-CONSTRAINT-UDR",
            f"{NEO4J_SOURCE_REL}/community/cypher/front-end/parser/v5/parser/src/main/antlr4/org/neo4j/cypher/internal/parser/v5/Cypher5Parser.g4",
            "createClause",
            "create constraint account_id if not exists for (a:Account) require a.id is unique",
            "neo4j.schema.constraint.create",
            "CREATE_CONSTRAINT",
            "parser_support_udr",
        ),
        ReplayCase(
            "neo4j",
            "NEO4J-CYPHER-SHOW-SERVERS-CLUSTER-ROUTE",
            f"{NEO4J_SOURCE_REL}/community/cypher/front-end/parser/v5/parser/src/main/antlr4/org/neo4j/cypher/internal/parser/v5/Cypher5Parser.g4",
            "SHOW (",
            "show servers",
            "neo4j.admin.show_servers",
            "SHOW",
            "admitted_sblr",
        ),
        ReplayCase(
            "influxdb",
            "INFLUXDB-INFLUXQL-SELECT",
            f"{INFLUXDB_SOURCE_REL}/influxdb3_commands/src/query.rs",
            "enum QueryLanguage",
            "select mean(value) from cpu",
            "influxdb.query.select",
            "SELECT",
            "admitted_sblr",
        ),
        ReplayCase(
            "influxdb",
            "INFLUXDB-LINE-PROTOCOL-UDR",
            f"{INFLUXDB_SOURCE_REL}/influxdb3_load_generator/src/line_protocol_generator.rs",
            "pub fn create_generators(",
            "cpu,host=a value=1",
            "influxdb.write.line_protocol",
            "CPU",
            "parser_support_udr",
        ),
        ReplayCase(
            "influxdb",
            "INFLUXDB-RETENTION-UDR",
            f"{INFLUXDB_SOURCE_REL}/influxdb3/tests/cli/db_retention.rs",
            "test_create_db_with_retention_period",
            "create retention policy rp on metrics duration 7d replication 1",
            "influxdb.retention_policy.create",
            "CREATE_RETENTION",
            "parser_support_udr",
        ),
        ReplayCase(
            "influxdb",
            "INFLUXDB-CLUSTER-ROUTE",
            f"{INFLUXDB_SOURCE_REL}/influxdb3_commands/src/query.rs",
            "query_host_url",
            "show servers",
            "influxdb.admin.show_servers",
            "SHOW",
            "admitted_sblr",
        ),
        ReplayCase(
            "milvus",
            "MILVUS-CREATE-COLLECTION",
            f"{MILVUS_SOURCE_REL}/pkg/proto/root_coord.proto",
            "rpc CreateCollection(milvus.CreateCollectionRequest) returns (common.Status){}",
            "create collection accounts id int64 vector float_vector dim 128",
            "milvus.collection.create",
            "CREATE_COLLECTION",
            "admitted_sblr",
        ),
        ReplayCase(
            "milvus",
            "MILVUS-SEARCH",
            f"{MILVUS_SOURCE_REL}/pkg/proto/query_coord.proto",
            "rpc Search(SearchRequest) returns (internal.SearchResults) {",
            "search collection accounts vector [0.1,0.2] topk 10",
            "milvus.query.search",
            "SEARCH",
            "admitted_sblr",
        ),
        ReplayCase(
            "milvus",
            "MILVUS-CREATE-INDEX-UDR",
            f"{MILVUS_SOURCE_REL}/pkg/proto/index_coord.proto",
            "rpc CreateIndex(CreateIndexRequest) returns (common.Status){}",
            "create index accounts vector hnsw",
            "milvus.index.create",
            "CREATE_INDEX",
            "parser_support_udr",
        ),
        ReplayCase(
            "milvus",
            "MILVUS-TRANSFER-REPLICA-CLUSTER-ROUTE",
            f"{MILVUS_SOURCE_REL}/internal/coordinator/mix_coord.go",
            "func (s *mixCoordImpl) TransferReplica(ctx context.Context, req *querypb.TransferReplicaRequest) (*commonpb.Status, error) {",
            "transfer_replica collection accounts",
            "milvus.admin.transfer_replica",
            "TRANSFER_REPLICA",
            "admitted_sblr",
        ),
        ReplayCase(
            "dolt",
            "DOLT-SQL-BRANCH-UDR",
            f"{DOLT_SOURCE_REL}/integration-tests/bats/sql-branch.bats",
            "CALL DOLT_BRANCH('new-branch')",
            "call dolt_branch('new-branch')",
            "dolt.version.branch",
            "CALL",
            "parser_support_udr",
        ),
        ReplayCase(
            "dolt",
            "DOLT-SQL-COMMIT-DIFF-CATALOG",
            f"{DOLT_SOURCE_REL}/integration-tests/bats/sql-commit-diff.bats",
            "select from_pk, to_pk, diff_type from dolt_commit_diff_test",
            "select * from dolt_diff",
            "dolt.version.diff",
            "SELECT",
            "catalog_projection",
        ),
        ReplayCase(
            "dolt",
            "DOLT-REMOTE-PUSH-UDR",
            f"{DOLT_SOURCE_REL}/integration-tests/bats/remotes-push-pull.bats",
            "dolt push origin main",
            "push origin main",
            "dolt.remote.push",
            "PUSH",
            "parser_support_udr",
        ),
        ReplayCase(
            "apache_ignite",
            "APACHE-IGNITE-SQL-SELECT",
            f"{APACHE_IGNITE_SOURCE_REL}/docs/_docs/SQL/sql-introduction.adoc",
            "As a SQL database, Ignite supports all DML commands including SELECT, UPDATE, INSERT, and DELETE queries",
            "select * from City",
            "apache_ignite.query.select",
            "SELECT",
            "admitted_sblr",
        ),
        ReplayCase(
            "apache_ignite",
            "APACHE-IGNITE-CACHE-CREATE-UDR",
            f"{APACHE_IGNITE_SOURCE_REL}/docs/_docs/key-value-api/basic-cache-operations.adoc",
            "You can also create a cache dynamically:",
            "create cache CityCache",
            "apache_ignite.cache.create",
            "CREATE_CACHE",
            "parser_support_udr",
        ),
        ReplayCase(
            "apache_ignite",
            "APACHE-IGNITE-SCAN-UDR",
            f"{APACHE_IGNITE_SOURCE_REL}/docs/_docs/key-value-api/using-cache-queries.adoc",
            "A scan query is a simple search query used to retrieve data from a cache in a distributed manner.",
            "scan CityCache",
            "apache_ignite.cache.scan",
            "SCAN",
            "parser_support_udr",
        ),
        ReplayCase(
            "apache_ignite",
            "APACHE-IGNITE-CONTROL-CLUSTER-RESERVED",
            f"{APACHE_IGNITE_SOURCE_REL}/bin/control.sh",
            "Grid cluster control.",
            "control.sh --baseline",
            "apache_ignite.admin.control_script",
            "CONTROL",
            "policy_refusal_fail_closed",
            "APACHE_IGNITE.AUTHORITY.CLUSTER_CONTROL_RESERVED",
        ),
        ReplayCase(
            "tikv",
            "TIKV-RAW-GET",
            f"{TIKV_SOURCE_REL}/CHANGELOG.md",
            "Optimize read performance using `ReadPool` and increase the `raw_get/get/batch_get` by 30%",
            "RAW_GET account:1",
            "tikv.raw.get",
            "RAW_GET",
            "admitted_sblr",
        ),
        ReplayCase(
            "tikv",
            "TIKV-IMPORT-SST-UDR",
            f"{TIKV_SOURCE_REL}/CHANGELOG.md",
            "Support the `ImportSST` API to import SST files [experimental]",
            "IMPORT_SST path",
            "tikv.import_sst",
            "IMPORT_SST",
            "parser_support_udr",
        ),
        ReplayCase(
            "tikv",
            "TIKV-TXN-PREWRITE-UDR",
            f"{TIKV_SOURCE_REL}/CHANGELOG.md",
            "Make the prewrite requests as idempotent as possible to reduce the chance of undetermined errors",
            "TXN_PREWRITE account:1 active",
            "tikv.txn.prewrite",
            "TXN_PREWRITE",
            "parser_support_udr",
        ),
        ReplayCase(
            "tikv",
            "TIKV-SPLIT-REGION-CLUSTER-ROUTE",
            f"{TIKV_SOURCE_REL}/CHANGELOG.md",
            "Enable the load-based `split region` operation",
            "SPLIT_REGION 1",
            "tikv.admin.split_region",
            "SPLIT_REGION",
            "admitted_sblr",
        ),
        ReplayCase(
            "foundationdb",
            "FOUNDATIONDB-GET-RANGE",
            f"{FOUNDATIONDB_SOURCE_REL}/bindings/bindingtester/tests/api.py",
            '"GET_RANGE",',
            "GET_RANGE accounts begin end",
            "foundationdb.kv.get_range",
            "GET_RANGE",
            "admitted_sblr",
        ),
        ReplayCase(
            "foundationdb",
            "FOUNDATIONDB-DIRECTORY-CREATE-UDR",
            f"{FOUNDATIONDB_SOURCE_REL}/bindings/bindingtester/tests/directory.py",
            "directory_mutations = ['DIRECTORY_CREATE_OR_OPEN', 'DIRECTORY_CREATE', 'DIRECTORY_MOVE'",
            "DIRECTORY_CREATE app users",
            "foundationdb.directory.create",
            "DIRECTORY_CREATE",
            "parser_support_udr",
        ),
        ReplayCase(
            "foundationdb",
            "FOUNDATIONDB-ATOMIC-OP-UDR",
            f"{FOUNDATIONDB_SOURCE_REL}/bindings/bindingtester/tests/api.py",
            '"ATOMIC_OP",',
            "ATOMIC_OP ADD counter 1",
            "foundationdb.kv.atomic_op",
            "ATOMIC_OP",
            "parser_support_udr",
        ),
        ReplayCase(
            "foundationdb",
            "FOUNDATIONDB-CONFIGURE-CLUSTER-ROUTE",
            f"{FOUNDATIONDB_SOURCE_REL}/fdbcli/ConfigureCommand.actor.cpp",
            '"configure [new|tss]"',
            "CONFIGURE new single memory",
            "foundationdb.admin.configure",
            "CONFIGURE",
            "admitted_sblr",
        ),
        ReplayCase(
            "immudb",
            "IMMUDB-SQL-UPSERT",
            f"{IMMUDB_SOURCE_REL}/embedded/sql/parser_test.go",
            "UPSERT INTO table1(id, time, title, active, compressed, payload, note) VALUES",
            "UPSERT INTO accounts(id, name) VALUES (1, 'Ada')",
            "immudb.dml.upsert",
            "UPSERT",
            "admitted_sblr",
        ),
        ReplayCase(
            "immudb",
            "IMMUDB-SHOW-DATABASES",
            f"{IMMUDB_SOURCE_REL}/embedded/sql/sql_grammar.y",
            "SHOW DATABASES",
            "SHOW DATABASES",
            "immudb.catalog.show_databases",
            "SHOW",
            "catalog_projection",
        ),
        ReplayCase(
            "immudb",
            "IMMUDB-VERIFIED-GET-UDR",
            "public_input_snapshot",
            "Verifiable RPC proofs are cryptographically verified against the local state root",
            "VERIFIED_GET account:1",
            "immudb.kv.verified_get",
            "VERIFIED_GET",
            "parser_support_udr",
            provenance="source_verified_implementation_packet",
        ),
        ReplayCase(
            "immudb",
            "IMMUDB-DUMP-FILE-AUTHORITY-REFUSAL",
            "public_input_snapshot",
            "Language families: `SQL subset;key-value verified commands`",
            "DUMP database to '/tmp/x'",
            "immudb.backup.dump",
            "DUMP",
            "policy_refusal_fail_closed",
            "IMMUDB.AUTHORITY.BACKUP_DENIED",
            provenance="source_verified_implementation_packet",
        ),
        ReplayCase(
            "xtdb",
            "XTDB-API-Q",
            f"{XTDB_SOURCE_REL}/api/src/main/clojure/xtdb/api.clj",
            "(defn q",
            "XTDB_Q [:find ?e :where [?e :name \"Ada\"]]",
            "xtdb.datalog.query",
            "XTDB_Q",
            "parser_support_udr",
        ),
        ReplayCase(
            "xtdb",
            "XTDB-API-SUBMIT-TX",
            f"{XTDB_SOURCE_REL}/api/src/main/clojure/xtdb/api.clj",
            "(defn submit-tx",
            "XTDB_SUBMIT_TX [{:xt/id :account/1 :name \"Ada\"}]",
            "xtdb.entity.submit_tx",
            "XTDB_SUBMIT_TX",
            "parser_support_udr",
        ),
        ReplayCase(
            "xtdb",
            "XTDB-BITEMPORAL-VALID-TIME",
            f"{XTDB_SOURCE_REL}/api/src/main/clojure/xtdb/tx_ops.clj",
            "valid-from valid-to",
            "select * from docs for valid_time as of now",
            "xtdb.time.valid_time",
            "SELECT",
            "parser_support_udr",
        ),
        ReplayCase(
            "xtdb",
            "XTDB-MODULE-CATALOG",
            "public_input_snapshot",
            "modules configuration",
            "XTDB_MODULES",
            "xtdb.catalog.modules",
            "XTDB_MODULES",
            "catalog_projection",
            provenance="source_verified_implementation_packet",
        ),
        ReplayCase(
            "xtdb",
            "XTDB-CLUSTER-CONTROL-ROUTE",
            f"{XTDB_SOURCE_REL}/core/src/main/clojure/xtdb/node.clj",
            "(defn start-node",
            "CLUSTER status",
            "xtdb.cluster.control",
            "CLUSTER",
            "admitted_sblr",
        ),
    ]


def assert_source_fragment(repo_root: pathlib.Path, case: ReplayCase) -> dict[str, object]:
    if (
        case.source_rel.startswith(PUBLIC_REFERENCE_REFERENCE_NOT_PACKAGED)
        or case.source_rel == "public_input_snapshot"
    ):
        return {
            "reference_material_publication": "not_packaged_in_public_repo",
            "reference_locator": case.source_rel,
            "reference_fragment_digest": hashlib.sha256(
                case.source_fragment.encode("utf-8")
            ).hexdigest(),
            "sql_input_digest": hashlib.sha256(case.sql.encode("utf-8")).hexdigest(),
            "provenance": case.provenance,
        }
    source = repo_root / case.source_rel
    if not source.is_file():
        raise AssertionError(f"{case.case_id} source file missing: {case.source_rel}")
    text = source.read_text(encoding="utf-8", errors="replace")
    if case.source_fragment not in text:
        raise AssertionError(
            f"{case.case_id} source fragment missing from {case.source_rel}: "
            f"{case.source_fragment!r}"
        )
    return {
        "source": case.source_rel,
        "fragment_digest": hashlib.sha256(case.source_fragment.encode("utf-8")).hexdigest(),
        "source_digest": sha256_file(source),
        "provenance": case.provenance,
    }


def parse_json_lines(output: str) -> list[dict[str, object]]:
    docs: list[dict[str, object]] = []
    for line in output.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            docs.append(json.loads(line))
        except json.JSONDecodeError as exc:
            raise AssertionError(f"parser emitted invalid JSON line: {line}") from exc
    if not docs:
        raise AssertionError(f"parser emitted no JSON documents:\n{output}")
    return docs


def find_envelope(docs: Iterable[dict[str, object]]) -> dict[str, object]:
    for doc in docs:
        if doc.get("envelope") == "SBLRExecutionEnvelope.v3":
            return doc
    raise AssertionError("parser output did not contain SBLRExecutionEnvelope.v3")


def diagnostic_codes(docs: Iterable[dict[str, object]]) -> set[str]:
    codes: set[str] = set()
    for doc in docs:
        diagnostics = doc.get("diagnostics")
        if not isinstance(diagnostics, list):
            continue
        for diagnostic in diagnostics:
            if isinstance(diagnostic, dict) and isinstance(diagnostic.get("code"), str):
                codes.add(diagnostic["code"])
    return codes


def datatype_families_digest(families: tuple[str, ...]) -> str:
    payload = json.dumps(list(families), separators=(",", ":"))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def validation_metadata(case: ReplayCase) -> dict[str, object]:
    metadata: dict[str, object] = {
        "validation_tags": list(case.validation_tags),
        "expected_datatype_families": list(case.expected_datatype_families),
    }
    if "datatype" in case.validation_tags:
        metadata.update({
            "asserted_datatype_family_count": len(case.expected_datatype_families),
            "asserted_datatype_families_digest": datatype_families_digest(
                case.expected_datatype_families
            ),
        })
    if "procedural" in case.validation_tags:
        metadata.update({
            "procedural_source_retention_asserted": True,
            "procedural_functional_encoding_asserted": True,
        })
    return metadata


def require_object(parent: dict[str, object],
                   key: str,
                   case_id: str) -> dict[str, object]:
    value = parent.get(key)
    if not isinstance(value, dict):
        raise AssertionError(f"{case_id} missing {key} object")
    return value


def assert_field(doc: dict[str, object],
                 key: str,
                 expected: object,
                 case_id: str) -> None:
    actual = doc.get(key)
    if actual != expected:
        raise AssertionError(f"{case_id} {key} {actual!r} != {expected!r}")


def assert_false(doc: dict[str, object], key: str, case_id: str) -> None:
    assert_field(doc, key, False, case_id)


def assert_expected_datatype_families(profile: dict[str, object],
                                      case: ReplayCase) -> None:
    if not case.expected_datatype_families:
        raise AssertionError(
            f"{case.case_id} datatype case missing expected family metadata"
        )
    detected = profile.get("detected_families")
    if not isinstance(detected, str):
        raise AssertionError(f"{case.case_id} missing detected_families string")
    detected_families = {family for family in detected.split(",") if family}
    for family in case.expected_datatype_families:
        assert_field(profile, family, True, case.case_id)
        if family not in detected_families:
            raise AssertionError(
                f"{case.case_id} detected_families missing {family!r}: "
                f"{detected!r}"
            )


def assert_not_source_text(envelope: dict[str, object],
                           case: ReplayCase) -> None:
    serialized = json.dumps(envelope, sort_keys=True)
    if case.sql in serialized:
        raise AssertionError(f"{case.case_id} leaked full SQL text in evidence")
    if case.source_fragment in serialized:
        raise AssertionError(f"{case.case_id} leaked source fragment in evidence")


def assert_procedural_evidence(envelope: dict[str, object],
                               parser_evidence: dict[str, object],
                               case: ReplayCase) -> None:
    assert_not_source_text(envelope, case)
    firebird_case = case.dialect == "firebird"
    retention = require_object(
        parser_evidence,
        "procedural_body_source_retention_evidence",
        case.case_id,
    )
    functional = require_object(
        parser_evidence,
        "procedural_functional_encoding_source_span_uuid_binding_evidence",
        case.case_id,
    )
    assert_field(retention, "evidence_contract",
                 "compatibility_procedural_body_source_retention.v1", case.case_id)
    assert_field(retention, "source_retention_state",
                 "catalog_reference_audit_material", case.case_id)
    assert_field(retention, "source_retention_metadata_source",
                 "parser_derived_token_offsets", case.case_id)
    assert_field(retention, "parser_derived_source_range_metadata", True,
                 case.case_id)
    assert_false(retention, "source_text_included", case.case_id)
    assert_false(retention, "raw_sql_body_embedded_in_sblr_envelope",
                 case.case_id)
    assert_field(retention, "body_text_redacted_from_parser_evidence", True,
                 case.case_id)
    assert_field(retention, "execution_authority", "scratchbird_engine_sblr",
                 case.case_id)
    for key in (
        "parser_transaction_authority",
        "parser_storage_authority",
        "parser_execution_authority",
        "parser_runtime_authority",
        "compatibility_sql_executed",
    ):
        assert_false(retention, key, case.case_id)
    assert_field(retention, "parser_bound_sblr_body_instruction_stream",
                 firebird_case, case.case_id)
    assert_field(retention, "uuid_dependency_bindings_bound",
                 firebird_case, case.case_id)
    assert_field(
        retention,
        "body_lowering_status",
        "parser_bound_sblr_instruction_stream_encoded"
        if firebird_case else "lowering_pending",
        case.case_id,
    )
    assert_field(
        retention,
        "compiled_sblr_status",
        "parser_bound_instruction_stream_present_runtime_compile_pending"
        if firebird_case else "pending",
        case.case_id,
    )
    for key in (
        "runtime_executable_status",
        "runtime_storage_status",
        "catalog_persistence_status",
        "catalog_reopen_runtime_proof_status",
    ):
        assert_field(retention, key, "pending", case.case_id)
    assert_field(retention, "enterprise_readiness", "not_enterprise_ready",
                 case.case_id)

    assert_field(functional, "evidence_contract",
                 "compatibility_procedural_functional_encoding_source_span_uuid_binding.v1",
                 case.case_id)
    assert_field(functional, "source_text_redacted_from_parser_evidence", True,
                 case.case_id)
    assert_false(functional, "sblr_evidence_includes_source_text",
                 case.case_id)
    assert_false(functional, "body_text_included", case.case_id)
    assert_field(functional, "parser_bound_sblr_body_instruction_stream",
                 firebird_case, case.case_id)
    assert_field(functional, "uuid_dependency_bindings_bound",
                 firebird_case, case.case_id)
    assert_field(
        functional,
        "executable_sblr_lowering_status",
        "parser_bound_sblr_instruction_stream_encoded"
        if firebird_case else "pending",
        case.case_id,
    )
    assert_field(
        functional,
        "jit_readiness_status",
        "parser_bound_sblr_requires_runtime_codegen_proof"
        if firebird_case else "pending",
        case.case_id,
    )
    assert_field(
        functional,
        "aot_readiness_status",
        "parser_bound_sblr_requires_runtime_codegen_proof"
        if firebird_case else "pending",
        case.case_id,
    )
    if firebird_case:
        firebird_encoding = require_object(
            parser_evidence,
            "firebird_psql_functional_encoding_evidence",
            case.case_id,
        )
        assert_field(firebird_encoding, "evidence_contract",
                     "firebird_psql_functional_encoding.v1", case.case_id)
        assert_field(firebird_encoding, "functional_encoding_status",
                     "firebird_psql_parser_bound_sblr_encoded", case.case_id)
        assert_field(firebird_encoding, "runtime_equivalence_status",
                     "pending_compatibility_native_psql_replay", case.case_id)
    for key in (
        "parser_uuid_authority",
        "parser_dependency_authority",
        "parser_storage_authority",
        "parser_transaction_finality_authority",
        "parser_source_execution_authority",
        "compatibility_sql_executed",
        "original_source_executed",
        "catalog_source_reference_execute_allowed",
    ):
        assert_false(functional, key, case.case_id)
    assert_field(functional, "enterprise_readiness", "not_enterprise_ready",
                 case.case_id)


def assert_datatype_evidence(envelope: dict[str, object],
                             parser_evidence: dict[str, object],
                             case: ReplayCase) -> None:
    descriptor = require_object(
        parser_evidence,
        "datatype_descriptor_evidence",
        case.case_id,
    )
    profile = require_object(
        parser_evidence,
        "datatype_profile_evidence",
        case.case_id,
    )
    assert_field(descriptor, "evidence_contract",
                 "compatibility_datatype_descriptor_evidence.v1", case.case_id)
    assert_field(descriptor, "descriptor_resolution", "uuid_required",
                 case.case_id)
    if descriptor.get("datatype_reference_count", 0) <= 0:
        raise AssertionError(f"{case.case_id} missing datatype references")
    assert_field(descriptor, "datatype_surface_matched", True, case.case_id)
    assert_field(descriptor, "catalog_descriptor_required", True,
                 case.case_id)
    assert_field(descriptor, "wire_literal_cast_comparison_required", True,
                 case.case_id)
    assert_false(descriptor, "generic_text_fallback_allowed", case.case_id)
    for key in (
        "parser_storage_authority",
        "parser_transaction_authority",
        "compatibility_sql_executed",
    ):
        assert_false(descriptor, key, case.case_id)
    assert_field(descriptor, "exactness_status",
                 "descriptor_surface_recorded_exactness_proof_pending",
                 case.case_id)
    assert_field(descriptor, "enterprise_readiness", "not_enterprise_ready",
                 case.case_id)

    assert_field(profile, "evidence_contract",
                 "compatibility_datatype_profile_family_detection.v1", case.case_id)
    assert_field(profile, "dialect", case.dialect, case.case_id)
    if profile.get("detected_family_count", 0) <= 0:
        raise AssertionError(f"{case.case_id} detected no datatype families")
    assert_expected_datatype_families(profile, case)
    assert_field(profile, "descriptor_authority", "scratchbird_engine_catalog",
                 case.case_id)
    assert_false(profile, "source_text_included", case.case_id)
    assert_false(profile, "generic_text_fallback_allowed", case.case_id)
    for key in (
        "parser_storage_authority",
        "parser_transaction_authority",
        "compatibility_sql_executed",
    ):
        assert_false(profile, key, case.case_id)
    assert_field(profile, "runtime_equivalence_status",
                 "pending_compatibility_native_exactness_replay", case.case_id)
    assert_field(profile, "enterprise_readiness", "not_enterprise_ready",
                 case.case_id)

    readiness = require_object(envelope, "enterprise_readiness_evidence",
                               case.case_id)
    assert_field(readiness, "datatype_exactness_status",
                 "surface_cataloged_exactness_proof_pending", case.case_id)
    assert_field(readiness, "completion_claim", "not_enterprise_ready",
                 case.case_id)
    assert_field(readiness, "enterprise_implemented_proven", False,
                 case.case_id)


def run_replay_case(repo_root: pathlib.Path,
                    parser_path: pathlib.Path,
                    case: ReplayCase,
                    timeout_seconds: int) -> dict[str, object]:
    source_evidence = assert_source_fragment(repo_root, case)
    result = run_command(
        [str(parser_path), case.sql],
        cwd=repo_root,
        timeout_seconds=timeout_seconds,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"{case.case_id} parser returned {result.returncode}:\n{result.stdout}"
        )
    docs = parse_json_lines(result.stdout)
    envelope = find_envelope(docs)
    operation_family = envelope.get("operation_family")
    if operation_family != case.expected_operation_family:
        raise AssertionError(
            f"{case.case_id} operation_family {operation_family!r} != "
            f"{case.expected_operation_family!r}"
        )
    parser_evidence = envelope.get("parser_evidence")
    if not isinstance(parser_evidence, dict):
        raise AssertionError(f"{case.case_id} missing parser_evidence object")
    if parser_evidence.get("statement_kind") != case.expected_statement_kind:
        raise AssertionError(
            f"{case.case_id} statement_kind {parser_evidence.get('statement_kind')!r} != "
            f"{case.expected_statement_kind!r}"
        )
    if parser_evidence.get("source_text_redacted") is not True:
        raise AssertionError(f"{case.case_id} parser evidence did not redact source text")
    if parser_evidence.get("parser_transaction_finality_authority") is not False:
        raise AssertionError(f"{case.case_id} parser claimed transaction finality authority")
    if parser_evidence.get("parser_storage_authority") is not False:
        raise AssertionError(f"{case.case_id} parser claimed storage authority")
    if envelope.get("reference_engine_sql_executed") is not False:
        raise AssertionError(f"{case.case_id} claimed reference SQL execution")
    if envelope.get("sql_text_included") is not False:
        raise AssertionError(f"{case.case_id} included SQL text in envelope")

    readiness = envelope.get("enterprise_readiness_evidence")
    if isinstance(readiness, dict):
        if readiness.get("completion_claim") != "not_enterprise_ready":
            raise AssertionError(f"{case.case_id} claimed enterprise readiness")
        if readiness.get("enterprise_implemented_proven") is not False:
            raise AssertionError(f"{case.case_id} claimed enterprise implementation proof")

    if "procedural" in case.validation_tags:
        assert_procedural_evidence(envelope, parser_evidence, case)
    if "datatype" in case.validation_tags:
        assert_datatype_evidence(envelope, parser_evidence, case)

    disposition = envelope.get("mapping_disposition")
    if case.expected_disposition and disposition != case.expected_disposition:
        raise AssertionError(
            f"{case.case_id} disposition {disposition!r} != "
            f"{case.expected_disposition!r}"
        )
    if case.expected_diagnostic_code:
        codes = diagnostic_codes(docs)
        if case.expected_diagnostic_code not in codes:
            raise AssertionError(
                f"{case.case_id} missing diagnostic {case.expected_diagnostic_code}: "
                f"{sorted(codes)}"
            )
        if envelope.get("fail_closed_refusal") is not True:
            raise AssertionError(f"{case.case_id} diagnostic case did not fail closed")

    return {
        "case_id": case.case_id,
        "dialect": case.dialect,
        "operation_family": operation_family,
        "statement_kind": parser_evidence.get("statement_kind"),
        "mapping_disposition": disposition,
        "expected_diagnostic_code": case.expected_diagnostic_code,
        "output_digest": hashlib.sha256(result.stdout.encode("utf-8")).hexdigest(),
        **validation_metadata(case),
        **source_evidence,
    }


def write_evidence(path: pathlib.Path, evidence: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--build-root", required=True, type=pathlib.Path)
    parser.add_argument("--firebird-parser", required=True, type=pathlib.Path)
    parser.add_argument("--mysql-parser", required=True, type=pathlib.Path)
    parser.add_argument("--postgresql-parser", required=True, type=pathlib.Path)
    parser.add_argument("--sqlite-parser", required=True, type=pathlib.Path)
    parser.add_argument("--mariadb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--duckdb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--clickhouse-parser", required=True, type=pathlib.Path)
    parser.add_argument("--tidb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--vitess-parser", required=True, type=pathlib.Path)
    parser.add_argument("--cockroachdb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--yugabytedb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--cassandra-parser", required=True, type=pathlib.Path)
    parser.add_argument("--mongodb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--redis-parser", required=True, type=pathlib.Path)
    parser.add_argument("--opensearch-sql-ppl-parser", required=True, type=pathlib.Path)
    parser.add_argument("--opensearch-parser", required=True, type=pathlib.Path)
    parser.add_argument("--neo4j-parser", required=True, type=pathlib.Path)
    parser.add_argument("--influxdb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--milvus-parser", required=True, type=pathlib.Path)
    parser.add_argument("--dolt-parser", required=True, type=pathlib.Path)
    parser.add_argument("--apache-ignite-parser", required=True, type=pathlib.Path)
    parser.add_argument("--tikv-parser", required=True, type=pathlib.Path)
    parser.add_argument("--foundationdb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--immudb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--xtdb-parser", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-file", required=True, type=pathlib.Path)
    parser.add_argument("--tool-timeout-seconds", type=int, default=1800)
    parser.add_argument("--parser-timeout-seconds", type=int, default=20)
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    build_root = args.build_root.resolve()
    parser_paths = {
        "firebird": args.firebird_parser.resolve(),
        "mysql": args.mysql_parser.resolve(),
        "postgresql": args.postgresql_parser.resolve(),
        "sqlite": args.sqlite_parser.resolve(),
        "mariadb": args.mariadb_parser.resolve(),
        "duckdb": args.duckdb_parser.resolve(),
        "clickhouse": args.clickhouse_parser.resolve(),
        "tidb": args.tidb_parser.resolve(),
        "vitess": args.vitess_parser.resolve(),
        "cockroachdb": args.cockroachdb_parser.resolve(),
        "yugabytedb": args.yugabytedb_parser.resolve(),
        "cassandra": args.cassandra_parser.resolve(),
        "mongodb": args.mongodb_parser.resolve(),
        "redis": args.redis_parser.resolve(),
        "opensearch_sql_ppl": args.opensearch_sql_ppl_parser.resolve(),
        "opensearch": args.opensearch_parser.resolve(),
        "neo4j": args.neo4j_parser.resolve(),
        "influxdb": args.influxdb_parser.resolve(),
        "milvus": args.milvus_parser.resolve(),
        "dolt": args.dolt_parser.resolve(),
        "apache_ignite": args.apache_ignite_parser.resolve(),
        "tikv": args.tikv_parser.resolve(),
        "foundationdb": args.foundationdb_parser.resolve(),
        "immudb": args.immudb_parser.resolve(),
        "xtdb": args.xtdb_parser.resolve(),
    }
    for dialect, path in parser_paths.items():
        if not path.exists():
            raise AssertionError(f"{dialect} parser binary missing: {path}")

    tool_specs = build_tool_specs(repo_root, build_root, args.tool_timeout_seconds)
    missing_tools = [
        {
            "dialect": spec.dialect,
            "tool_id": spec.tool_id,
            "staged_locator": spec.staged_rel,
            "required_local_path": spec.staged_rel,
        }
        for spec in tool_specs
        if not spec.source.exists()
    ]
    if missing_tools:
        evidence = {
            "gate": COMPATIBILITY_REPLAY_GATE_ID,
            "reference_input_gate": REFERENCE_REPLAY_INPUT_ID,
            "regular_ctest_gate": True,
            "external_reference_fixture_required": True,
            "external_reference_fixture_status": "missing",
            "skip_reason": "external_reference_tools_not_installed",
            "tool_staging_root": "project/tests/reference_regression/*/native_tool_harness/tools",
            "compatibility_tools_are_storage_authority": False,
            "compatibility_tools_are_transaction_authority": False,
            "reference_tools_are_storage_authority": False,
            "reference_tools_are_transaction_authority": False,
            "parser_authority_rule": (
                "native reference tools and reference regression fragments drive parser "
                "endpoints only; engine MGA, security, recovery, and storage remain "
                "ScratchBird authority"
            ),
            "missing_external_tool_count": len(missing_tools),
            "missing_external_tools": missing_tools,
            "staged_tools": [],
            "tool_smokes": [],
            "replay_case_count": 0,
            "replay_counts_by_dialect": {},
            "enterprise_release_ready_from_runtime_evidence": False,
            "runtime_enterprise_blocker_count": len(replay_cases()),
            "runtime_enterprise_blocker_reason": (
                "external reference tools are intentionally not tracked in the public "
                "repo; install local fixtures to execute this CTest replay"
            ),
            "replay_results": [],
        }
        write_evidence(args.evidence_file, evidence)
        print(
            f"{COMPATIBILITY_REPLAY_GATE_ID}=skipped "
            f"missing_external_tools={len(missing_tools)}"
        )
        return EXTERNAL_REFERENCE_SKIP_CODE

    staged_tools = [copy_tool(spec, repo_root) for spec in tool_specs]
    tool_smokes = [
        smoke_tool(spec, repo_root, args.parser_timeout_seconds)
        for spec in tool_specs
    ]
    replay_results = [
        run_replay_case(
            repo_root,
            parser_paths[case.dialect],
            case,
            args.parser_timeout_seconds,
        )
        for case in replay_cases()
    ]

    counts: dict[str, int] = {}
    for result in replay_results:
        counts[result["dialect"]] = counts.get(result["dialect"], 0) + 1

    evidence = {
        "gate": COMPATIBILITY_REPLAY_GATE_ID,
        "reference_input_gate": REFERENCE_REPLAY_INPUT_ID,
        "regular_ctest_gate": True,
        "external_reference_fixture_required": True,
        "external_reference_fixture_status": "present",
        "tool_staging_root": "project/tests/reference_regression/*/native_tool_harness/tools",
        "compatibility_tools_are_storage_authority": False,
        "compatibility_tools_are_transaction_authority": False,
        "reference_tools_are_storage_authority": False,
        "reference_tools_are_transaction_authority": False,
        "parser_authority_rule": (
            "native reference tools and reference regression fragments drive parser "
            "endpoints only; engine MGA, security, recovery, and storage remain "
            "ScratchBird authority"
        ),
        "staged_tools": staged_tools,
        "tool_smokes": tool_smokes,
        "replay_case_count": len(replay_results),
        "replay_counts_by_dialect": dict(sorted(counts.items())),
        "enterprise_release_ready_from_runtime_evidence": False,
        "runtime_enterprise_blocker_count": len(replay_results),
        "runtime_enterprise_blocker_reason": (
            "parser envelopes intentionally assert not_enterprise_ready until "
            "procedural bodies, exact datatypes, reference semantic defaults, and "
            "reference-native runtime equivalence are fully implemented and proven"
        ),
        "replay_results": replay_results,
    }
    write_evidence(args.evidence_file, evidence)
    print(
        f"{COMPATIBILITY_REPLAY_GATE_ID}=passed "
        f"tools={len(staged_tools)} replay_cases={len(replay_results)}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except ExternalReferenceFixtureMissing as exc:
        print(f"{COMPATIBILITY_REPLAY_GATE_ID}: {exc}",
              file=sys.stderr)
        raise SystemExit(EXTERNAL_REFERENCE_SKIP_CODE)
    except (AssertionError, subprocess.TimeoutExpired) as exc:
        print(f"{COMPATIBILITY_REPLAY_GATE_ID}: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
