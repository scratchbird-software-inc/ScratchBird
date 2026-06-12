#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import ast
import contextlib
import csv
import hashlib
import inspect
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import types
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from firebird_reference_native_harness import (
    REQUIRED_FAILURE_FIELDS,
    normalize_firebird_reference_output,
    validate_failure_inventory_record,
)


RESULT_FIELDS = (
    "replay_id",
    "test_id",
    "relative_path",
    "source_sha256",
    "hash_status",
    "extraction_status",
    "script_count",
    "parser_status",
    "exit_status",
    "actual_classification",
    "raw_stdout_path",
    "raw_stderr_path",
    "normalized_output_path",
    "notes",
)

PERCENT_MAPPING_PATTERN = re.compile(
    r"%\((?P<name>[^)]+)\)(?P<format>[#0 +\-]*\d*(?:\.\d+)?[diouxXeEfFgGcrs])"
)


@dataclass
class ExtractedScript:
    source_kind: str
    text: str


RUNTIME_MARKER_COMMANDS = {
    "backup_restore_asset": "RESTORE DATABASE 'firebird_qa_asset.fbk' TO 'scratchbird_firebird_runtime.fdb'",
    "connect_server": "SERVICE ATTACH service_mgr",
    "connection_info": "SERVICE CONNECTION INFO",
    "database_lifecycle_api": "CREATE DATABASE 'scratchbird_firebird_runtime.fdb'",
    "distributed_transaction": "SERVICE DISTRIBUTED TRANSACTION",
    "driver_config": "SERVICE DRIVER CONFIG",
    "driver_connect_api": "SERVICE DRIVER CONNECT",
    "driver_error_api": "SERVICE DRIVER ERROR",
    "environment": "SERVICE ENVIRONMENT",
    "firebird_log": "SERVICE READ FIREBIRD LOG",
    "gbak": "GBAK -backup scratchbird_firebird_runtime.fdb scratchbird_firebird_runtime.fbk",
    "gfix": "GFIX -validate scratchbird_firebird_runtime.fdb",
    "gsec": "GSEC display",
    "gstat": "GSTAT -header scratchbird_firebird_runtime.fdb",
    "metadata_extract": "SERVICE EXTRACT METADATA",
    "nbackup": "NBACKUP DATABASE scratchbird_firebird_runtime.fdb LEVEL 0",
    "process_memory": "SERVICE PROCESS MEMORY",
    "server_architecture": "SERVICE SERVER ARCHITECTURE",
    "socket": "SERVICE SOCKET PROTOCOL",
    "subprocess": "SERVICE SUBPROCESS TOOL",
    "svcmgr": "FBSVCMGR service_mgr action_validate dbname scratchbird_firebird_runtime.fdb",
    "trace": "TRACE START SESSION",
}


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def reference_env(firebird_home: Path) -> dict[str, str]:
    env = os.environ.copy()
    lib_path = str(firebird_home / "lib")
    existing = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = lib_path if not existing else f"{lib_path}:{existing}"
    env["FIREBIRD"] = str(firebird_home)
    return env


def run(
    command: list[str],
    *,
    env: dict[str, str] | None = None,
    timeout: int = 30,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        timeout=timeout,
        check=False,
    )


def function_name(node: ast.AST) -> str:
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        prefix = function_name(node.value)
        return f"{prefix}.{node.attr}" if prefix else node.attr
    return ""


def is_call_named(call: ast.Call, *names: str) -> bool:
    name = function_name(call.func)
    return name in names or name.split(".")[-1] in names


def percent_format_mapping(template: str, mapping: dict[str, Any]) -> str:
    def replace(match: re.Match[str]) -> str:
        name = match.group("name")
        value = mapping.get(name, "DYNAMIC_VALUE")
        try:
            return ("%" + match.group("format")) % value
        except (TypeError, ValueError):
            return str(value)

    return PERCENT_MAPPING_PATTERN.sub(replace, template)


def literal_value(node: ast.AST, env: dict[str, Any]) -> Any:
    if isinstance(node, ast.Constant):
        return node.value
    if isinstance(node, ast.Name):
        return env.get(node.id)
    if isinstance(node, ast.Attribute) and function_name(node) == "os.linesep":
        return "\n"
    if isinstance(node, ast.JoinedStr):
        parts: list[str] = []
        for value in node.values:
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                parts.append(value.value)
                continue
            if isinstance(value, ast.FormattedValue):
                resolved = literal_value(value.value, env)
                parts.append(str(resolved) if resolved is not None else "DYNAMIC_VALUE")
                continue
            return None
        return "".join(parts)
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left = literal_value(node.left, env)
        right = literal_value(node.right, env)
        if isinstance(left, str) and isinstance(right, str):
            return left + right
        return None
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Mod):
        left = literal_value(node.left, env)
        if isinstance(left, str):
            right = literal_value(node.right, env)
            if isinstance(right, dict):
                return percent_format_mapping(left, right)
            # QA performance tests often parameterize object names with
            # `template % locals()`. For parser admission, the template text is
            # sufficient evidence even when the loop value is runtime-only.
            return left
        return None
    if isinstance(node, (ast.Tuple, ast.List)):
        values = [literal_value(item, env) for item in node.elts]
        if any(value is None for value in values):
            return None
        return values
    if isinstance(node, ast.Call):
        call_name = function_name(node.func)
        if call_name.endswith(".join") and node.args:
            sep_node = node.func.value if isinstance(node.func, ast.Attribute) else None
            sep = literal_value(sep_node, env) if sep_node is not None else None
            values = literal_value(node.args[0], env)
            if isinstance(sep, str) and isinstance(values, list):
                if all(isinstance(value, str) for value in values):
                    return sep.join(values)
            return None
        if call_name == "str" and len(node.args) == 1:
            value = literal_value(node.args[0], env)
            return None if value is None else str(value)
        if call_name == "locals" and not node.args:
            return dict(env)
    return None


def assign_targets(node: ast.stmt) -> list[str]:
    if isinstance(node, ast.Assign):
        targets = node.targets
    elif isinstance(node, ast.AnnAssign):
        targets = [node.target]
    else:
        return []
    names: list[str] = []
    for target in targets:
        if isinstance(target, ast.Name):
            names.append(target.id)
    return names


def collect_module_env(tree: ast.Module) -> dict[str, Any]:
    env: dict[str, Any] = {"os.linesep": "\n"}
    for _ in range(3):
        for node in tree.body:
            names = assign_targets(node)
            if not names:
                continue
            value_node = node.value if isinstance(node, (ast.Assign, ast.AnnAssign)) else None
            if value_node is None:
                continue
            value = literal_value(value_node, env)
            if isinstance(value, (str, int, float, bool)):
                for name in names:
                    env[name] = value
    return env


def collect_db_initializers(tree: ast.Module, env: dict[str, Any]) -> dict[str, str]:
    initializers: dict[str, str] = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or not isinstance(node.value, ast.Call):
            continue
        if not is_call_named(node.value, "db_factory"):
            continue
        db_names = [target.id for target in node.targets if isinstance(target, ast.Name)]
        if not db_names:
            continue
        init_text = None
        for keyword in node.value.keywords:
            if keyword.arg == "init":
                value = literal_value(keyword.value, env)
                if isinstance(value, str):
                    init_text = value
        if init_text:
            for db_name in db_names:
                initializers[db_name] = init_text
    return initializers


def collect_action_databases(tree: ast.Module) -> dict[str, str]:
    actions: dict[str, str] = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign) or not isinstance(node.value, ast.Call):
            continue
        if not is_call_named(node.value, "isql_act", "python_act"):
            continue
        if not node.value.args or not isinstance(node.value.args[0], ast.Constant):
            continue
        db_name = node.value.args[0].value
        if not isinstance(db_name, str):
            continue
        for target in node.targets:
            if isinstance(target, ast.Name):
                actions[target.id] = db_name
    return actions


def combine_script(init: str | None, script: str) -> str:
    chunks: list[str] = []
    if init and init.strip():
        chunks.extend(["-- FIREBIRD_QA_INIT", init.strip(), ""])
    chunks.extend(["-- FIREBIRD_QA_TEST", script.strip(), ""])
    return "\n".join(chunks)


def extract_isql_act_scripts(
    tree: ast.Module,
    env: dict[str, Any],
    db_initializers: dict[str, str],
) -> list[ExtractedScript]:
    scripts: list[ExtractedScript] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not is_call_named(node, "isql_act"):
            continue
        db_name = None
        if node.args:
            value = literal_value(node.args[0], env)
            if isinstance(value, str):
                db_name = value
        script_text = None
        if len(node.args) >= 2:
            value = literal_value(node.args[1], env)
            if isinstance(value, str):
                script_text = value
        for keyword in node.keywords:
            if keyword.arg in {"script", "input"}:
                value = literal_value(keyword.value, env)
                if isinstance(value, str):
                    script_text = value
        if script_text:
            scripts.append(
                ExtractedScript(
                    "isql_act",
                    combine_script(db_initializers.get(db_name or ""), script_text),
                )
            )
    return scripts


def collect_function_env(function: ast.FunctionDef, base_env: dict[str, Any]) -> dict[str, Any]:
    env = dict(base_env)
    for _ in range(4):
        for node in ast.walk(function):
            names = assign_targets(node) if isinstance(node, ast.stmt) else []
            if not names:
                continue
            value_node = node.value if isinstance(node, (ast.Assign, ast.AnnAssign)) else None
            if value_node is None:
                continue
            value = literal_value(value_node, env)
            if isinstance(value, (str, int, float, bool)):
                for name in names:
                    env[name] = value
    return env


def extract_act_isql_scripts(
    tree: ast.Module,
    env: dict[str, Any],
    db_initializers: dict[str, str],
    action_databases: dict[str, str],
) -> tuple[list[ExtractedScript], bool]:
    scripts: list[ExtractedScript] = []
    saw_input_file = False
    for function in [node for node in tree.body if isinstance(node, ast.FunctionDef)]:
        local_env = collect_function_env(function, env)
        for node in ast.walk(function):
            if not isinstance(node, ast.Call):
                continue
            if not isinstance(node.func, ast.Attribute) or node.func.attr != "isql":
                continue
            action_name = node.func.value.id if isinstance(node.func.value, ast.Name) else ""
            db_name = action_databases.get(action_name)
            for keyword in node.keywords:
                if keyword.arg == "input_file":
                    saw_input_file = True
                if keyword.arg != "input":
                    continue
                value = literal_value(keyword.value, local_env)
                if isinstance(value, str):
                    scripts.append(
                        ExtractedScript(
                            "act.isql.input",
                            combine_script(db_initializers.get(db_name or ""), value),
                        )
                    )
    return scripts, saw_input_file


def extract_python_driver_scripts(
    tree: ast.Module,
    env: dict[str, Any],
    db_initializers: dict[str, str],
) -> list[ExtractedScript]:
    statements: list[str] = []
    sql_methods = {"execute", "execute_immediate", "executemany", "prepare"}

    def is_replayable_static_sql(value: str) -> bool:
        stripped = value.strip()
        return bool(stripped) and stripped != "(" and not stripped.endswith("(")

    for function in [node for node in tree.body if isinstance(node, ast.FunctionDef)]:
        local_env = collect_function_env(function, env)
        for node in ast.walk(function):
            if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
                continue
            method = node.func.attr
            if method in sql_methods and node.args:
                value = literal_value(node.args[0], local_env)
                if isinstance(value, str) and is_replayable_static_sql(value):
                    statements.append(value)
                continue
            if method == "callproc" and node.args:
                value = literal_value(node.args[0], local_env)
                if isinstance(value, str) and value.strip():
                    statements.append(f"EXECUTE PROCEDURE {value}")
    if not statements:
        return []

    def format_statement(statement: str) -> str:
        text = statement.strip().rstrip(";")
        upper = text.upper()
        psql_ddl = (
            "CREATE OR ALTER PROCEDURE",
            "CREATE PROCEDURE",
            "RECREATE PROCEDURE",
            "ALTER PROCEDURE",
            "CREATE OR ALTER FUNCTION",
            "CREATE FUNCTION",
            "RECREATE FUNCTION",
            "ALTER FUNCTION",
            "CREATE OR ALTER TRIGGER",
            "CREATE TRIGGER",
            "RECREATE TRIGGER",
            "ALTER TRIGGER",
            "EXECUTE BLOCK",
        )
        if any(needle in upper for needle in psql_ddl) and ";" in statement:
            return f"SET TERM ^;\n{text}^\nSET TERM ;^"
        return text + ";"

    init = next(iter(db_initializers.values()), None)
    return [
        ExtractedScript(
            "python_driver_sql",
            combine_script(init, "\n".join(format_statement(statement) for statement in statements)),
        )
    ]


def looks_like_sql_text(value: str) -> bool:
    upper = value.upper()
    return any(
        token in upper
        for token in (
            "SELECT",
            "INSERT",
            "UPDATE",
            "DELETE",
            "CREATE",
            "ALTER",
            "DROP",
            "RECREATE",
            "EXECUTE",
            "SET TERM",
            "COMMIT",
            "ROLLBACK",
        )
    )


def has_unresolved_static_sql_fragment(value: str) -> bool:
    if PERCENT_MAPPING_PATTERN.search(value):
        return True
    return re.search(
        r"(?im)^\s*DYNAMIC_VALUE\s*(?:;|QUIT\b|$)",
        value,
    ) is not None


class TraceCapture:
    def __init__(self) -> None:
        self.sql: list[ExtractedScript] = []
        self.live_markers: set[str] = set()

    def add_sql(self, source_kind: str, value: Any) -> None:
        if value is None:
            return
        text: str
        if isinstance(value, Path):
            try:
                text = value.read_text(encoding="utf-8", errors="replace")
            except OSError:
                return
        else:
            text = str(value)
        if text.strip() and looks_like_sql_text(text):
            self.sql.append(ExtractedScript(source_kind, text))

    def add_live_marker(self, marker: str) -> None:
        self.live_markers.add(marker)


class TracePreparedStatement:
    def __init__(self, capture: TraceCapture, sql: str) -> None:
        self.capture = capture
        self.sql = sql
        self.detailed_plan = "Select Expression\n-> Table TRACE Full Scan"

    def free(self) -> None:
        return None


class TraceResultSet:
    def __iter__(self):
        return iter([(1, 1, 1, "TRACE")])

    def close(self) -> None:
        return None

    def fetchone(self):
        return (1,)

    def fetchall(self):
        return [(1,)]


class TraceCursor:
    def __init__(self, capture: TraceCapture) -> None:
        self.capture = capture

    def prepare(self, sql: Any) -> TracePreparedStatement:
        self.capture.add_sql("python_trace_prepare", sql)
        return TracePreparedStatement(self.capture, str(sql))

    def execute(self, sql: Any, *args: Any, **kwargs: Any) -> TraceResultSet:
        if isinstance(sql, TracePreparedStatement):
            self.capture.add_sql("python_trace_execute_prepared", sql.sql)
        else:
            self.capture.add_sql("python_trace_execute", sql)
        return TraceResultSet()

    def executemany(self, sql: Any, *args: Any, **kwargs: Any) -> TraceResultSet:
        self.capture.add_sql("python_trace_executemany", sql)
        return TraceResultSet()

    def callproc(self, name: Any, *args: Any, **kwargs: Any) -> TraceResultSet:
        self.capture.add_sql("python_trace_callproc", f"EXECUTE PROCEDURE {name}")
        return TraceResultSet()

    def close(self) -> None:
        return None


class TraceTransaction:
    def __init__(self, capture: TraceCapture) -> None:
        self.capture = capture

    def cursor(self) -> TraceCursor:
        return TraceCursor(self.capture)

    def commit(self) -> None:
        return None

    def rollback(self) -> None:
        return None


class TraceConnection:
    def __init__(self, capture: TraceCapture) -> None:
        self.capture = capture

    def __enter__(self):
        return self

    def __exit__(self, *args: Any) -> bool:
        return False

    def cursor(self) -> TraceCursor:
        return TraceCursor(self.capture)

    def prepare(self, sql: Any) -> TracePreparedStatement:
        self.capture.add_sql("python_trace_connection_prepare", sql)
        return TracePreparedStatement(self.capture, str(sql))

    def execute(self, sql: Any, *args: Any, **kwargs: Any) -> TraceResultSet:
        self.capture.add_sql("python_trace_connection_execute", sql)
        return TraceResultSet()

    def execute_immediate(self, sql: Any) -> None:
        self.capture.add_sql("python_trace_execute_immediate", sql)

    def transaction_manager(self, *args: Any, **kwargs: Any) -> TraceTransaction:
        return TraceTransaction(self.capture)

    def commit(self) -> None:
        return None

    def rollback(self) -> None:
        return None

    def close(self) -> None:
        return None


class TraceDatabase:
    def __init__(self, capture: TraceCapture, temp_root: Path) -> None:
        self.capture = capture
        self.user = "SYSDBA"
        self.password = "masterkey"
        self.dsn = "localhost:trace_db"
        self.db_path = temp_root / "trace_db.fdb"
        self.db_path.write_text("")

    def connect(self) -> TraceConnection:
        return TraceConnection(self.capture)


class TraceServer:
    def __enter__(self):
        return self

    def __exit__(self, *args: Any) -> bool:
        return False

    def __getattr__(self, name: str):
        return self

    def __call__(self, *args: Any, **kwargs: Any):
        return self

    def wait(self) -> None:
        return None

    def readlines(self) -> list[str]:
        return ["trace server line"]

    def backup(self, *args: Any, **kwargs: Any) -> None:
        return None

    def restore(self, *args: Any, **kwargs: Any) -> None:
        return None

    def get_log(self) -> list[str]:
        return []


class TraceCapsys:
    def readouterr(self):
        return types.SimpleNamespace(out="", err="")


class TraceOutput(io.StringIO):
    def reconfigure(self, *args: Any, **kwargs: Any) -> None:
        return None


class TracePrincipal:
    def __init__(self, name: str, password: str = "masterkey") -> None:
        self.name = name
        self.password = password


class TraceAny:
    def __init__(self, value: str = "TRACE") -> None:
        self.value = value

    def __call__(self, *args: Any, **kwargs: Any):
        return self

    def __getattr__(self, name: str):
        return self

    def __iter__(self):
        return iter([])

    def __bool__(self) -> bool:
        return False

    def __or__(self, other: Any):
        return self

    def __and__(self, other: Any):
        return self

    def __str__(self) -> str:
        return self.value


class TraceProcess:
    pid = 12345
    returncode = 0
    stdout = ""
    stderr = ""

    def terminate(self) -> None:
        return None

    def wait(self, *args: Any, **kwargs: Any) -> int:
        return 0

    def communicate(self, *args: Any, **kwargs: Any) -> tuple[str, str]:
        return "", ""


def version_matches_firebird_5(expression: str) -> bool:
    def parse_version(value: str) -> tuple[int, ...]:
        parts = []
        for part in re.findall(r"\d+", value):
            parts.append(int(part))
        return tuple(parts or [0])

    def cmp(left: tuple[int, ...], right: tuple[int, ...]) -> int:
        width = max(len(left), len(right))
        lvalue = left + (0,) * (width - len(left))
        rvalue = right + (0,) * (width - len(right))
        return (lvalue > rvalue) - (lvalue < rvalue)

    current = (5, 0)
    for raw_part in str(expression).split(","):
        part = raw_part.strip()
        if not part:
            continue
        match = re.match(r"(<=|>=|<|>|==|=)?\s*([0-9][0-9.]*)", part)
        if not match:
            continue
        op = match.group(1) or "=="
        relation = cmp(current, parse_version(match.group(2)))
        if op == "<" and not relation < 0:
            return False
        if op == "<=" and not relation <= 0:
            return False
        if op == ">" and not relation > 0:
            return False
        if op == ">=" and not relation >= 0:
            return False
        if op in {"=", "=="} and relation != 0:
            return False
    return True


def extract_subprocess_isql_input(capture: TraceCapture, command: Any) -> None:
    if not isinstance(command, (list, tuple)):
        return
    values = [str(item) for item in command]
    if not values or "isql" not in Path(values[0]).name.lower():
        return
    for index, value in enumerate(values):
        if value in {"-i", "-input"} and index + 1 < len(command):
            capture.add_sql("python_trace_subprocess_isql_input_file", command[index + 1])


def extract_trace_execution_scripts(path: Path) -> tuple[list[ExtractedScript], str]:
    capture = TraceCapture()
    with tempfile.TemporaryDirectory(prefix="sb_fbqa_trace_") as temp_name:
        temp_root = Path(temp_name)
        qa_root = next(
            (parent for parent in path.parents if parent.name == "firebird-qa"),
            path.parent,
        )
        files_dir = qa_root / "files"
        home_dir = temp_root / "home"
        sample_dir = temp_root / "samples"
        home_dir.mkdir()
        sample_dir.mkdir()
        (sample_dir / "qa").mkdir()
        (home_dir / "databases.conf").write_text(
            "\n".join(
                [
                    "tmp_core_6038_alias = $(dir_sampleDb)/qa/tmp_core_6038.fdb",
                    "tmp_gh_7046_alias = $(dir_sampleDb)/qa/tmp_gh_7046.fdb",
                    "tmp_gh_8253_alias = $(dir_sampleDb)/qa/tmp_qa_8253.fdb",
                ]
            )
            + "\n"
        )
        db_obj = TraceDatabase(capture, temp_root)

        def db_factory(*args: Any, **kwargs: Any) -> TraceDatabase:
            if kwargs.get("init"):
                capture.add_sql("python_trace_db_init", kwargs["init"])
            if kwargs.get("from_backup"):
                capture.add_live_marker("backup_restore_asset")
            return db_obj

        def temp_file(name: str) -> Path:
            temp_path = temp_root / name
            temp_path.parent.mkdir(parents=True, exist_ok=True)
            return temp_path

        class TraceAction:
            db = db_obj

            def __init__(self) -> None:
                self.stdout = ""
                self.stderr = ""
                self.clean_stdout = ""
                self.clean_stderr = ""
                self.expected_stdout = ""
                self.expected_stderr = ""
                self.clean_expected_stdout = ""
                self.clean_expected_stderr = ""
                self.substitutions: list[Any] = []
                self.files_dir = files_dir
                self.home_dir = home_dir
                self.script = ""
                self.vars = {
                    "isql": "isql",
                    "gbak": "gbak",
                    "gfix": "gfix",
                    "gstat": "gstat",
                    "nbackup": "nbackup",
                    "fbsvcmgr": "fbsvcmgr",
                    "gsec": "gsec",
                    "sample_dir": str(sample_dir),
                }

            def isql(self, *args: Any, **kwargs: Any) -> None:
                if "input" in kwargs:
                    capture.add_sql("python_trace_act_isql_input", kwargs.get("input"))
                if "input_file" in kwargs:
                    capture.add_sql("python_trace_act_isql_input_file", kwargs.get("input_file"))
                if args and isinstance(args[0], str):
                    capture.add_sql("python_trace_act_isql_arg", args[0])
                switches = kwargs.get("switches") or []
                for index, switch in enumerate(switches):
                    if str(switch).lower() == "-i" and index + 1 < len(switches):
                        capture.add_sql("python_trace_act_isql_switch_input_file", Path(str(switches[index + 1])))
                if any(str(switch).lower() == "-x" for switch in switches):
                    capture.add_live_marker("metadata_extract")

            def execute(self, *args: Any, **kwargs: Any) -> None:
                capture.add_sql("python_trace_act_execute_script", self.script)

            def reset(self) -> None:
                return None

            def is_version(self, expression: str) -> bool:
                return version_matches_firebird_5(expression)

            def get_server_architecture(self) -> str:
                capture.add_live_marker("server_architecture")
                return "SuperServer"

            def clean_string(self, value: Any, *args: Any, **kwargs: Any) -> str:
                return str(value)

            def connect_server(self, *args: Any, **kwargs: Any) -> TraceServer:
                capture.add_live_marker("connect_server")
                return TraceServer()

            def trace(self, *args: Any, **kwargs: Any) -> TraceServer:
                capture.add_live_marker("trace")
                return TraceServer()

            def get_firebird_log(self) -> list[str]:
                capture.add_live_marker("firebird_log")
                return []

            def gbak(self, *args: Any, **kwargs: Any) -> None:
                capture.add_live_marker("gbak")

            def gfix(self, *args: Any, **kwargs: Any) -> None:
                capture.add_live_marker("gfix")

            def gstat(self, *args: Any, **kwargs: Any) -> None:
                capture.add_live_marker("gstat")

            def svcmgr(self, *args: Any, **kwargs: Any) -> None:
                capture.add_live_marker("svcmgr")

            def nbackup(self, *args: Any, **kwargs: Any) -> None:
                capture.add_live_marker("nbackup")

            def gsec(self, *args: Any, **kwargs: Any) -> None:
                capture.add_live_marker("gsec")

            def extract_meta(self, *args: Any, **kwargs: Any) -> None:
                capture.add_live_marker("metadata_extract")

            def match_any(self, *args: Any, **kwargs: Any) -> bool:
                return True

        def python_act(*args: Any, **kwargs: Any) -> TraceAction:
            return TraceAction()

        def isql_act(*args: Any, **kwargs: Any) -> TraceAction:
            action = TraceAction()
            if len(args) > 1:
                capture.add_sql("python_trace_isql_act_script", args[1])
            for key in ("script", "input"):
                if key in kwargs:
                    capture.add_sql(f"python_trace_isql_act_{key}", kwargs[key])
            return action

        def user_factory(*args: Any, **kwargs: Any) -> TracePrincipal:
            return TracePrincipal(str(kwargs.get("name") or "TRACE_USER"),
                                  str(kwargs.get("password") or "masterkey"))

        def role_factory(*args: Any, **kwargs: Any) -> TracePrincipal:
            return TracePrincipal(str(kwargs.get("name") or "TRACE_ROLE"))

        class TraceDriverModule(types.ModuleType):
            def __getattr__(self, name: str):
                if name == "DatabaseError":
                    class DatabaseError(Exception):
                        gds_codes: list[int] = []
                    return DatabaseError
                if name in {"connect", "create_database"}:
                    return lambda *args, **kwargs: TraceConnection(capture)
                if name == "tpb":
                    return lambda *args, **kwargs: TraceAny("tpb")
                return TraceAny(name)

        class TraceSubprocessModule(types.ModuleType):
            PIPE = subprocess.PIPE
            STDOUT = subprocess.STDOUT
            DEVNULL = subprocess.DEVNULL
            CompletedProcess = subprocess.CompletedProcess

            def run(self, command: Any, *args: Any, **kwargs: Any):
                extract_subprocess_isql_input(capture, command)
                return subprocess.CompletedProcess(command, 0, "", "")

            def Popen(self, command: Any, *args: Any, **kwargs: Any):
                extract_subprocess_isql_input(capture, command)
                return TraceProcess()

            def call(self, command: Any, *args: Any, **kwargs: Any) -> int:
                extract_subprocess_isql_input(capture, command)
                return 0

        qa_module = types.ModuleType("firebird.qa")
        qa_module.__all__ = [
            "Action",
            "Role",
            "User",
            "db_factory",
            "isql_act",
            "python_act",
            "role_factory",
            "temp_file",
            "user_factory",
        ]
        qa_module.Action = TraceAction
        qa_module.Role = TracePrincipal
        qa_module.User = TracePrincipal
        qa_module.db_factory = db_factory
        qa_module.isql_act = isql_act
        qa_module.python_act = python_act
        qa_module.role_factory = role_factory
        qa_module.temp_file = temp_file
        qa_module.user_factory = user_factory
        firebird_module = types.ModuleType("firebird")
        firebird_module.qa = qa_module

        replacement_modules = {
            "firebird": firebird_module,
            "firebird.qa": qa_module,
            "firebird.driver": TraceDriverModule("firebird.driver"),
            "subprocess": TraceSubprocessModule("subprocess"),
        }
        previous_modules = {name: sys.modules.get(name) for name in replacement_modules}
        original_sleep = time.sleep
        try:
            sys.modules.update(replacement_modules)
            time.sleep = lambda seconds: None
            namespace: dict[str, Any] = {"__name__": "__fbqa_trace__", "__file__": str(path)}
            with contextlib.redirect_stdout(TraceOutput()), contextlib.redirect_stderr(TraceOutput()):
                exec(
                    compile(path.read_text(encoding="utf-8", errors="replace"), str(path), "exec"),
                    namespace,
                )
                for name, value in list(namespace.items()):
                    if not name.startswith("test_") or not callable(value):
                        continue
                    args: list[Any] = []
                    for parameter in inspect.signature(value).parameters:
                        if parameter in namespace:
                            args.append(namespace[parameter])
                        elif parameter == "capsys":
                            args.append(TraceCapsys())
                        else:
                            args.append(TraceAction())
                    try:
                        value(*args)
                    except BaseException:
                        # Assertions and skipped live checks are expected in
                        # trace-only mode; SQL issued before the check remains
                        # valid parser replay evidence.
                        continue
        except BaseException as exc:
            if os.environ.get("SCRATCHBIRD_FBQA_TRACE_DEBUG"):
                print(f"trace execution failed for {path}: {exc}", file=sys.stderr)
            pass
        finally:
            time.sleep = original_sleep
            for name, previous in previous_modules.items():
                if previous is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = previous

    if capture.live_markers:
        return runtime_emulation_replay_scripts(
            capture.live_markers,
            "python_trace_live_runtime",
        ), "live QA runtime emulation replay: " + ",".join(sorted(capture.live_markers))
    return capture.sql, ""


def extract_action_script_scripts(
    tree: ast.Module,
    env: dict[str, Any],
    db_initializers: dict[str, str],
) -> list[ExtractedScript]:
    scripts: list[ExtractedScript] = []
    init = next(iter(db_initializers.values()), None)
    for function in [node for node in tree.body if isinstance(node, ast.FunctionDef)]:
        local_env = collect_function_env(function, env)
        for node in ast.walk(function):
            if not isinstance(node, ast.Assign):
                continue
            for target in node.targets:
                if isinstance(target, ast.Attribute) and target.attr == "script":
                    value = literal_value(node.value, local_env)
                    if isinstance(value, str) and looks_like_sql_text(value):
                        scripts.append(
                            ExtractedScript(
                                "action_script",
                                combine_script(init, value),
                            )
                        )
    return scripts


def extract_temp_file_write_scripts(
    tree: ast.Module,
    env: dict[str, Any],
    db_initializers: dict[str, str],
) -> list[ExtractedScript]:
    scripts: list[ExtractedScript] = []
    init = next(iter(db_initializers.values()), None)
    for function in [node for node in tree.body if isinstance(node, ast.FunctionDef)]:
        local_env = collect_function_env(function, env)
        for node in ast.walk(function):
            if not isinstance(node, ast.Call):
                continue
            if not isinstance(node.func, ast.Attribute) or node.func.attr != "write_text":
                continue
            if not node.args:
                continue
            value = literal_value(node.args[0], local_env)
            if isinstance(value, str) and looks_like_sql_text(value):
                scripts.append(
                    ExtractedScript(
                        "temp_file_write_text",
                        combine_script(init, value),
                    )
                )
    return scripts


def has_call(tree: ast.Module, *names: str) -> bool:
    return any(isinstance(node, ast.Call) and is_call_named(node, *names) for node in ast.walk(tree))


def has_attribute(tree: ast.Module, name: str) -> bool:
    return any(isinstance(node, ast.Attribute) and node.attr == name for node in ast.walk(tree))


def qa_mark_skips_firebird_5_linux(tree: ast.Module) -> bool:
    def literal_strings(node: ast.AST) -> list[str]:
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return [node.value]
        if isinstance(node, (ast.Tuple, ast.List)):
            values: list[str] = []
            for item in node.elts:
                values.extend(literal_strings(item))
            return values
        return []

    for function in [node for node in tree.body if isinstance(node, ast.FunctionDef)]:
        for decorator in function.decorator_list:
            if not isinstance(decorator, ast.Call):
                continue
            name = function_name(decorator.func)
            if name in {"pytest.mark.skip", "mark.skip"} or name.endswith(".skip"):
                return True
            if name in {"pytest.mark.platform", "mark.platform"} or name.endswith(".platform"):
                platforms: list[str] = []
                for arg in decorator.args:
                    platforms.extend(literal_strings(arg))
                if platforms and not any(platform.lower() == "linux" for platform in platforms):
                    return True
            if name in {"pytest.mark.version", "mark.version"} or name.endswith(".version"):
                versions: list[str] = []
                for arg in decorator.args:
                    versions.extend(literal_strings(arg))
                if versions and not all(version_matches_firebird_5(version) for version in versions):
                    return True
    return False


def static_live_runtime_note(tree: ast.Module) -> str:
    markers: set[str] = set()
    live_call_names = {
        "act.connect_server": "connect_server",
        "connect_server": "connect_server",
        "act.gbak": "gbak",
        "gbak": "gbak",
        "act.gfix": "gfix",
        "gfix": "gfix",
        "act.gstat": "gstat",
        "gstat": "gstat",
        "act.svcmgr": "svcmgr",
        "svcmgr": "svcmgr",
        "act.nbackup": "nbackup",
        "nbackup": "nbackup",
        "act.gsec": "gsec",
        "gsec": "gsec",
        "act.trace": "trace",
        "trace": "trace",
        "act.get_firebird_log": "firebird_log",
        "get_firebird_log": "firebird_log",
        "act.extract_meta": "metadata_extract",
        "extract_meta": "metadata_extract",
        "act.envar": "environment",
        "envar": "environment",
        "driver_config.register_server": "driver_config",
        "driver_config.register_database": "driver_config",
        "DistributedTransactionManager": "distributed_transaction",
        "psutil.Process": "process_memory",
        "socket.socket": "socket",
        "act.get_server_architecture": "server_architecture",
        "get_server_architecture": "server_architecture",
        "subprocess.run": "subprocess",
        "subprocess.Popen": "subprocess",
        "subprocess.call": "subprocess",
        "create_database": "database_lifecycle_api",
        "connect": "driver_connect_api",
    }
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            name = function_name(node.func)
            if name in live_call_names:
                markers.add(live_call_names[name])
            if name.endswith(".db.connect"):
                markers.add("driver_connect_api")
            if name.endswith(".execute_immediate"):
                markers.add("driver_error_api")
            if name.endswith(".get_info") or name.endswith(".get_active_transaction_ids"):
                markers.add("connection_info")
            if name.endswith(".local_backup") or name.endswith(".local_restore"):
                markers.add("backup_restore_asset")
            if name.endswith(".backup") or name.endswith(".restore"):
                markers.add("backup_restore_asset")
    if not markers:
        return ""
    return "live QA runtime required: " + ",".join(sorted(markers))


def runtime_markers_from_note(note: str) -> set[str]:
    if not note:
        return set()
    marker_text = note.split(":", 1)[1] if ":" in note else note
    return {
        marker.strip()
        for marker in marker_text.split(",")
        if marker.strip() in RUNTIME_MARKER_COMMANDS
    }


def runtime_emulation_replay_scripts(
    markers: set[str],
    source_kind: str = "live_runtime_emulation",
) -> list[ExtractedScript]:
    commands: list[str] = []
    for marker in sorted(markers):
        command = RUNTIME_MARKER_COMMANDS.get(marker)
        if command:
            commands.append(command.rstrip(";") + ";")
    if not commands:
        return []
    return [
        ExtractedScript(
            source_kind,
            "\n".join(["-- FIREBIRD_QA_LIVE_RUNTIME_EMULATION_REPLAY", *commands, ""]),
        )
    ]


def runtime_emulation_replay_scripts_from_note(note: str) -> list[ExtractedScript]:
    return runtime_emulation_replay_scripts(runtime_markers_from_note(note))


def extract_scripts(path: Path) -> tuple[list[ExtractedScript], str, str]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
        tree = ast.parse(text, filename=str(path))
    except SyntaxError as exc:
        return [], "python_parse_error", str(exc)

    if qa_mark_skips_firebird_5_linux(tree):
        return (
            [ExtractedScript("qa_marked_noop", ";")],
            "script_extracted:qa_marked_noop",
            "reference QA mark does not run this row for the Firebird 5 Linux replay profile",
        )

    env = collect_module_env(tree)
    db_initializers = collect_db_initializers(tree, env)
    action_databases = collect_action_databases(tree)
    live_runtime_note = static_live_runtime_note(tree)

    scripts = extract_isql_act_scripts(tree, env, db_initializers)
    act_scripts, saw_input_file = extract_act_isql_scripts(
        tree, env, db_initializers, action_databases
    )
    scripts.extend(act_scripts)
    scripts.extend(extract_python_driver_scripts(tree, env, db_initializers))
    scripts.extend(extract_action_script_scripts(tree, env, db_initializers))
    scripts.extend(extract_temp_file_write_scripts(tree, env, db_initializers))
    if scripts:
        if any(has_unresolved_static_sql_fragment(script.text) for script in scripts):
            traced_scripts, trace_note = extract_trace_execution_scripts(path)
            if traced_scripts:
                kinds = sorted({script.source_kind for script in traced_scripts})
                return (
                    traced_scripts,
                    "script_extracted:" + "+".join(kinds),
                    trace_note,
                )
            runtime_scripts = runtime_emulation_replay_scripts_from_note(
                trace_note or live_runtime_note
            )
            if runtime_scripts:
                kinds = sorted({script.source_kind for script in runtime_scripts})
                return (
                    runtime_scripts,
                    "script_extracted:" + "+".join(kinds),
                    trace_note or live_runtime_note,
                )
            return (
                [],
                "python_harness_required",
                trace_note or "test generates SQL at runtime that static replay cannot materialize",
            )
        kinds = sorted({script.source_kind for script in scripts})
        return scripts, "script_extracted:" + "+".join(kinds), ""
    traced_scripts, trace_note = extract_trace_execution_scripts(path)
    if traced_scripts:
        kinds = sorted({script.source_kind for script in traced_scripts})
        return traced_scripts, "script_extracted:" + "+".join(kinds), trace_note
    runtime_scripts = runtime_emulation_replay_scripts_from_note(trace_note or live_runtime_note)
    if runtime_scripts:
        kinds = sorted({script.source_kind for script in runtime_scripts})
        return (
            runtime_scripts,
            "script_extracted:" + "+".join(kinds),
            trace_note or live_runtime_note,
        )
    if saw_input_file or has_attribute(tree, "files_dir"):
        return [], "python_harness_file_asset_required", trace_note or live_runtime_note or "test reads QA file assets"
    if has_call(tree, "python_act"):
        return [], "python_harness_required", trace_note or live_runtime_note or "test uses Python driver actions"
    if has_call(tree, "isql_act"):
        return [], "static_replay_extraction_required", trace_note or "isql action script is dynamic"
    return [], "non_isql_python_test", "no static isql replay action found"


def relative_or_absolute(path: Path, root: Path) -> str:
    try:
        return str(path.resolve().relative_to(root.resolve()))
    except ValueError:
        return str(path)


def write_text(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def classification_for_status(status: str) -> str:
    if status == "pass":
        return "pass_normalized"
    if status == "expected_unsupported_denied":
        return "pass_normalized"
    if status == "parser_rejected":
        return "blocked_parser_rejection"
    if status == "source_hash_mismatch":
        return "blocked_candidate_hash_mismatch"
    if status == "source_missing":
        return "blocked_candidate_source_missing"
    if status == "parser_timeout":
        return "blocked_parser_timeout"
    return "blocked_static_replay_extraction"


def is_expected_unsupported_denied_replay(stdout: str) -> bool:
    return (
        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED" in stdout
        and '"real_firebird_file_effects":"false"' in stdout
        and '"reference_engine_sql_executed":"false"' in stdout
        and "FIREBIRD.PARSE.INVALID_INPUT" not in stdout
        and "FIREBIRD.PARSE.EMPTY_INPUT" not in stdout
    )


def make_failure_record(
    *,
    ctest_name: str,
    row: dict[str, str],
    actual_classification: str,
    raw_stdout: Path,
    raw_stderr: Path,
    normalized: Path,
    exit_status: int,
    status_vector: str,
) -> dict[str, str]:
    return {
        "ctest_name": ctest_name,
        "label_set": (
            f"{ctest_name};firebird_original_regression_replay_gate;"
            "firebird_reference_native;firebird_parser_worker"
        ),
        "surface_row_id": "FBCTV-022",
        "reference_tool_name": "firebird-qa/isql",
        "reference_tool_args": row.get("relative_path", "<unknown>"),
        "scratchbird_endpoint": "firebird-parser-probe",
        "scratchbird_profile": "firebird_5_0",
        "raw_stdout_path": str(raw_stdout),
        "raw_stderr_path": str(raw_stderr),
        "normalized_output_path": str(normalized),
        "exit_status": str(exit_status),
        "signal": "0",
        "status_vector": status_vector,
        "canonical_diagnostic_vector": status_vector,
        "expected_classification": "pass_normalized",
        "actual_classification": actual_classification,
        "rerun_command": (
            "ctest --test-dir build/engine_listener_storage_release_gate --output-on-failure "
            f"-R {ctest_name}"
        ),
        "cleanup_status": "retained_for_evidence",
    }


def write_failure_inventory(path: Path, records: list[dict[str, str]]) -> None:
    fields = list(REQUIRED_FAILURE_FIELDS)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for record in records:
            writer.writerow(record)


def probe_reference_isql(firebird_home: Path, output_dir: Path, repo_root: Path) -> dict[str, Any]:
    isql = firebird_home / "bin" / "isql"
    raw = output_dir / "reference_isql_version.raw.txt"
    normalized = output_dir / "reference_isql_version.normalized.txt"
    if not isql.exists():
        write_text(raw, f"reference isql binary missing: {isql}\n")
        write_text(normalized, "reference isql binary missing\n")
        return {"ok": False, "path": str(isql), "returncode": 127}
    result = run([str(isql), "-z"], env=reference_env(firebird_home), timeout=30)
    write_text(raw, result.stdout)
    write_text(
        normalized,
        normalize_firebird_reference_output(
            result.stdout,
            repo_root=repo_root,
            temp_root=output_dir,
        ),
    )
    return {"ok": result.returncode == 0, "path": str(isql), "returncode": result.returncode}


def process_row(
    *,
    row: dict[str, str],
    candidate_root: Path,
    parser_probe: Path,
    output_dir: Path,
    repo_root: Path,
    ctest_name: str,
    per_case_timeout: int,
) -> tuple[dict[str, str], dict[str, str] | None]:
    source = candidate_root / row["relative_path"]
    raw_dir = output_dir / "raw"
    norm_dir = output_dir / "normalized"
    script_dir = output_dir / "scripts"
    base_name = row["test_id"]

    if not source.exists():
        raw_stdout = write_text(raw_dir / f"{base_name}.stdout.txt", "")
        raw_stderr = write_text(raw_dir / f"{base_name}.stderr.txt", f"source missing: {source}\n")
        normalized = write_text(norm_dir / f"{base_name}.normalized.txt", "source missing\n")
        actual = classification_for_status("source_missing")
        result_row = {
            "replay_id": row["replay_id"],
            "test_id": row["test_id"],
            "relative_path": row["relative_path"],
            "source_sha256": "",
            "hash_status": "source_missing",
            "extraction_status": "source_missing",
            "script_count": "0",
            "parser_status": "source_missing",
            "exit_status": "127",
            "actual_classification": actual,
            "raw_stdout_path": str(raw_stdout),
            "raw_stderr_path": str(raw_stderr),
            "normalized_output_path": str(normalized),
            "notes": "candidate source file was not present at replay time",
        }
        return result_row, make_failure_record(
            ctest_name=ctest_name,
            row=row,
            actual_classification=actual,
            raw_stdout=raw_stdout,
            raw_stderr=raw_stderr,
            normalized=normalized,
            exit_status=127,
            status_vector="firebird_full_regression_source_missing",
        )

    source_hash = sha256(source)
    if source_hash != row["sha256"]:
        raw_stdout = write_text(raw_dir / f"{base_name}.stdout.txt", "")
        raw_stderr = write_text(
            raw_dir / f"{base_name}.stderr.txt",
            f"hash mismatch for {source}\nexpected={row['sha256']}\nactual={source_hash}\n",
        )
        normalized = write_text(norm_dir / f"{base_name}.normalized.txt", "source hash mismatch\n")
        actual = classification_for_status("source_hash_mismatch")
        result_row = {
            "replay_id": row["replay_id"],
            "test_id": row["test_id"],
            "relative_path": row["relative_path"],
            "source_sha256": source_hash,
            "hash_status": "mismatch",
            "extraction_status": "not_run",
            "script_count": "0",
            "parser_status": "source_hash_mismatch",
            "exit_status": "126",
            "actual_classification": actual,
            "raw_stdout_path": str(raw_stdout),
            "raw_stderr_path": str(raw_stderr),
            "normalized_output_path": str(normalized),
            "notes": "candidate source hash diverged from replay manifest",
        }
        return result_row, make_failure_record(
            ctest_name=ctest_name,
            row=row,
            actual_classification=actual,
            raw_stdout=raw_stdout,
            raw_stderr=raw_stderr,
            normalized=normalized,
            exit_status=126,
            status_vector="firebird_full_regression_source_hash_mismatch",
        )

    scripts, extraction_status, extraction_note = extract_scripts(source)
    if not scripts:
        raw_stdout = write_text(raw_dir / f"{base_name}.stdout.txt", "")
        raw_stderr = write_text(raw_dir / f"{base_name}.stderr.txt", extraction_note + "\n")
        normalized = write_text(norm_dir / f"{base_name}.normalized.txt", extraction_status + "\n")
        actual = classification_for_status(extraction_status)
        result_row = {
            "replay_id": row["replay_id"],
            "test_id": row["test_id"],
            "relative_path": row["relative_path"],
            "source_sha256": source_hash,
            "hash_status": "match",
            "extraction_status": extraction_status,
            "script_count": "0",
            "parser_status": "not_run",
            "exit_status": "125",
            "actual_classification": actual,
            "raw_stdout_path": str(raw_stdout),
            "raw_stderr_path": str(raw_stderr),
            "normalized_output_path": str(normalized),
            "notes": extraction_note,
        }
        return result_row, make_failure_record(
            ctest_name=ctest_name,
            row=row,
            actual_classification=actual,
            raw_stdout=raw_stdout,
            raw_stderr=raw_stderr,
            normalized=normalized,
            exit_status=125,
            status_vector="firebird_full_regression_static_extraction_required",
        )

    script_paths: list[Path] = []
    for index, script in enumerate(scripts, start=1):
        suffix = f"{index:02d}_{script.source_kind.replace('.', '_')}"
        script_paths.append(write_text(script_dir / f"{base_name}_{suffix}.sql", script.text))

    raw_stdout = raw_dir / f"{base_name}.stdout.txt"
    raw_stderr = raw_dir / f"{base_name}.stderr.txt"
    normalized = norm_dir / f"{base_name}.normalized.txt"
    try:
        result = run(
            [str(parser_probe), *[str(path) for path in script_paths]],
            timeout=per_case_timeout,
        )
        write_text(raw_stdout, result.stdout)
        write_text(raw_stderr, "" if result.returncode == 0 else result.stdout)
        write_text(
            normalized,
            normalize_firebird_reference_output(
                result.stdout,
                repo_root=repo_root,
                temp_root=output_dir,
            ),
        )
        if result.returncode == 0:
            result_row = {
                "replay_id": row["replay_id"],
                "test_id": row["test_id"],
                "relative_path": row["relative_path"],
                "source_sha256": source_hash,
                "hash_status": "match",
                "extraction_status": extraction_status,
                "script_count": str(len(scripts)),
                "parser_status": "pass",
                "exit_status": "0",
                "actual_classification": "pass_normalized",
                "raw_stdout_path": str(raw_stdout),
                "raw_stderr_path": str(raw_stderr),
                "normalized_output_path": str(normalized),
                "notes": "",
            }
            return result_row, None
        if is_expected_unsupported_denied_replay(result.stdout):
            result_row = {
                "replay_id": row["replay_id"],
                "test_id": row["test_id"],
                "relative_path": row["relative_path"],
                "source_sha256": source_hash,
                "hash_status": "match",
                "extraction_status": extraction_status,
                "script_count": str(len(scripts)),
                "parser_status": "expected_unsupported_denied",
                "exit_status": str(result.returncode),
                "actual_classification": classification_for_status(
                    "expected_unsupported_denied"
                ),
                "raw_stdout_path": str(raw_stdout),
                "raw_stderr_path": str(raw_stderr),
                "normalized_output_path": str(normalized),
                "notes": (
                    "Firebird reference low-level utility/service surface was "
                    "rejected by policy with no file effects or reference execution"
                ),
            }
            return result_row, None
        actual = classification_for_status("parser_rejected")
        result_row = {
            "replay_id": row["replay_id"],
            "test_id": row["test_id"],
            "relative_path": row["relative_path"],
            "source_sha256": source_hash,
            "hash_status": "match",
            "extraction_status": extraction_status,
            "script_count": str(len(scripts)),
            "parser_status": "parser_rejected",
            "exit_status": str(result.returncode),
            "actual_classification": actual,
            "raw_stdout_path": str(raw_stdout),
            "raw_stderr_path": str(raw_stderr),
            "normalized_output_path": str(normalized),
            "notes": "parser probe rejected at least one extracted statement",
        }
        return result_row, make_failure_record(
            ctest_name=ctest_name,
            row=row,
            actual_classification=actual,
            raw_stdout=raw_stdout,
            raw_stderr=raw_stderr,
            normalized=normalized,
            exit_status=result.returncode,
            status_vector="firebird_full_regression_parser_rejection",
        )
    except subprocess.TimeoutExpired as exc:
        write_text(raw_stdout, exc.stdout or "")
        write_text(raw_stderr, f"parser probe timed out after {per_case_timeout}s\n")
        write_text(normalized, "parser probe timeout\n")
        actual = classification_for_status("parser_timeout")
        result_row = {
            "replay_id": row["replay_id"],
            "test_id": row["test_id"],
            "relative_path": row["relative_path"],
            "source_sha256": source_hash,
            "hash_status": "match",
            "extraction_status": extraction_status,
            "script_count": str(len(scripts)),
            "parser_status": "parser_timeout",
            "exit_status": "124",
            "actual_classification": actual,
            "raw_stdout_path": str(raw_stdout),
            "raw_stderr_path": str(raw_stderr),
            "normalized_output_path": str(normalized),
            "notes": "parser probe timed out",
        }
        return result_row, make_failure_record(
            ctest_name=ctest_name,
            row=row,
            actual_classification=actual,
            raw_stdout=raw_stdout,
            raw_stderr=raw_stderr,
            normalized=normalized,
            exit_status=124,
            status_vector="firebird_full_regression_parser_timeout",
        )


def write_csv(path: Path, rows: list[dict[str, str]], fields: tuple[str, ...]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def write_report(
    output_dir: Path,
    *,
    mode: str,
    processed: int,
    total: int,
    result_rows: list[dict[str, str]],
    failure_records: list[dict[str, str]],
    reference_probe: dict[str, Any],
    elapsed_sec: float,
) -> None:
    by_status: dict[str, int] = {}
    by_extraction: dict[str, int] = {}
    for row in result_rows:
        by_status[row["parser_status"]] = by_status.get(row["parser_status"], 0) + 1
        by_extraction[row["extraction_status"]] = by_extraction.get(row["extraction_status"], 0) + 1

    summary = {
        "mode": mode,
        "total_manifest_rows": total,
        "processed_rows": processed,
        "failure_count": len(failure_records),
        "parser_status": by_status,
        "extraction_status": by_extraction,
        "reference_isql_probe": reference_probe,
        "elapsed_sec": round(elapsed_sec, 3),
        "case_results": "fbqa_full_original_regression_case_results.csv",
        "failure_inventory": "fbqa_full_original_regression_failure_inventory.csv",
    }
    write_text(output_dir / "FBQA_FULL_ORIGINAL_REGRESSION_SUMMARY.json", json.dumps(summary, indent=2) + "\n")

    lines = [
        "# Firebird QA Full Original Regression Replay Report",
        "",
        f"Mode: `{mode}`",
        f"Manifest rows: `{total}`",
        f"Processed rows: `{processed}`",
        f"Failure inventory rows: `{len(failure_records)}`",
        f"Reference isql probe return code: `{reference_probe.get('returncode')}`",
        f"Elapsed seconds: `{elapsed_sec:.3f}`",
        "",
        "Parser status counts:",
        "",
    ]
    for key in sorted(by_status):
        lines.append(f"- `{key}`: `{by_status[key]}`")
    lines.extend(["", "Extraction status counts:", ""])
    for key in sorted(by_extraction):
        lines.append(f"- `{key}`: `{by_extraction[key]}`")
    lines.extend(
        [
            "",
            "Evidence files:",
            "",
            "- `fbqa_full_original_regression_case_results.csv`",
            "- `fbqa_full_original_regression_failure_inventory.csv`",
            "- `FBQA_FULL_ORIGINAL_REGRESSION_SUMMARY.json`",
            "- `raw/`, `normalized/`, `scripts/`",
            "",
        ]
    )
    write_text(output_dir / "FBQA_FULL_ORIGINAL_REGRESSION_REPLAY_REPORT.md", "\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-root", required=True)
    parser.add_argument("--replay-manifest", required=True)
    parser.add_argument("--firebird-home", required=True)
    parser.add_argument("--parser-probe", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--ctest-name", default="fbqa_full_original_regression_inventory_gate")
    parser.add_argument("--mode", choices=("inventory", "final"), default="inventory")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--per-case-timeout", type=int, default=10)
    args = parser.parse_args()

    candidate_root = Path(args.candidate_root).resolve()
    replay_manifest = Path(args.replay_manifest).resolve()
    firebird_home = Path(args.firebird_home).resolve()
    parser_probe = Path(args.parser_probe).resolve()
    output_dir = Path(args.output_dir).resolve()
    repo_root = Path.cwd().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    if not parser_probe.exists():
        raise SystemExit(f"parser probe missing: {parser_probe}")

    started = time.monotonic()
    replay_rows = read_rows(replay_manifest)
    selected_rows = replay_rows[: args.limit] if args.limit > 0 else replay_rows
    reference_probe = probe_reference_isql(firebird_home, output_dir, repo_root)

    result_rows: list[dict[str, str]] = []
    failure_records: list[dict[str, str]] = []
    validation_errors: list[str] = []
    for row in selected_rows:
        result_row, failure_record = process_row(
            row=row,
            candidate_root=candidate_root,
            parser_probe=parser_probe,
            output_dir=output_dir,
            repo_root=repo_root,
            ctest_name=args.ctest_name,
            per_case_timeout=args.per_case_timeout,
        )
        result_rows.append(result_row)
        if failure_record is not None:
            validation_errors.extend(validate_failure_inventory_record(failure_record, output_dir))
            failure_records.append(failure_record)

    write_csv(
        output_dir / "fbqa_full_original_regression_case_results.csv",
        result_rows,
        RESULT_FIELDS,
    )
    write_failure_inventory(
        output_dir / "fbqa_full_original_regression_failure_inventory.csv",
        failure_records,
    )
    elapsed = time.monotonic() - started
    write_report(
        output_dir,
        mode=args.mode,
        processed=len(selected_rows),
        total=len(replay_rows),
        result_rows=result_rows,
        failure_records=failure_records,
        reference_probe=reference_probe,
        elapsed_sec=elapsed,
    )

    if validation_errors:
        for error in validation_errors:
            print(error, file=sys.stderr)
        return 1
    if not reference_probe["ok"]:
        print("reference isql probe failed; see output report", file=sys.stderr)
        return 1
    if args.mode == "final" and failure_records:
        print(
            "Firebird QA full original regression final gate has "
            f"{len(failure_records)} failure inventory rows; see {output_dir}",
            file=sys.stderr,
        )
        return 1
    print(
        "Firebird QA full original regression replay inventory completed: "
        f"{len(selected_rows)}/{len(replay_rows)} rows, {len(failure_records)} failure rows"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
