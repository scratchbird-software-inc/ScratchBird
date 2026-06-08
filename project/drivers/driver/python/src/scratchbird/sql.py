# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SQL parameter normalization helpers."""

from __future__ import annotations

import re
from typing import Any, Dict, List, Tuple


def normalize_query(sql: str, params) -> Tuple[str, List[Any]]:
    if params is None:
        return sql, []
    if isinstance(params, dict):
        if not params:
            return sql, []
        if not _has_named_params(sql):
            raise ValueError("named parameters provided but query has no placeholders")
        return _rewrite_named(sql, params)
    values = list(params)
    if "?" in sql:
        return _rewrite_positional(sql, values)
    return sql, values


def normalize_callable_query(sql: str, params) -> Tuple[str, List[Any]]:
    callable_sql = normalize_callable_sql(sql)
    return normalize_query(callable_sql, params)


def split_top_level_statements(sql: str) -> List[str]:
    statements: List[str] = []
    current: List[str] = []
    in_single = False
    in_double = False
    for ch in sql:
        if ch == "'" and not in_double:
            in_single = not in_single
            current.append(ch)
            continue
        if ch == '"' and not in_single:
            in_double = not in_double
            current.append(ch)
            continue
        if not in_single and not in_double and ch == ";":
            statement = "".join(current).strip()
            if statement:
                statements.append(statement)
            current = []
            continue
        current.append(ch)
    statement = "".join(current).strip()
    if statement:
        statements.append(statement)
    return statements


def normalize_callable_sql(sql: str) -> str:
    trimmed = sql.strip()
    if not (trimmed.startswith("{") and trimmed.endswith("}")):
        return sql
    inner = trimmed[1:-1].strip()
    if not inner:
        return sql

    function_match = re.match(r"^\?\s*=\s*call\s+([\s\S]+)$", inner, flags=re.IGNORECASE)
    if function_match:
        invocation = _parse_callable_invocation(function_match.group(1).strip())
        if invocation is None:
            raise ValueError("invalid JDBC escape call syntax")
        routine, args, has_parens = invocation
        call_args = args if has_parens else ""
        return f"select {routine}({call_args}) as return_value"

    procedure_match = re.match(r"^call\s+([\s\S]+)$", inner, flags=re.IGNORECASE)
    if procedure_match:
        invocation = _parse_callable_invocation(procedure_match.group(1).strip())
        if invocation is None:
            raise ValueError("invalid JDBC escape call syntax")
        routine, args, has_parens = invocation
        if has_parens:
            return f"call {routine}({args})"
        return f"call {routine}"

    return sql


def _has_named_params(sql: str) -> bool:
    i = 0
    in_string = False
    while i < len(sql) - 1:
        ch = sql[i]
        if ch == "'":
            in_string = not in_string
            i += 1
            continue
        if in_string:
            i += 1
            continue
        if ch == ":" and sql[i + 1] == ":":
            i += 2
            continue
        if ch in (":", "@") and sql[i + 1].isidentifier():
            return True
        i += 1
    return False


def _rewrite_named(sql: str, params: Dict[str, Any]) -> Tuple[str, List[Any]]:
    result = []
    ordered: List[Any] = []
    i = 0
    in_string = False
    while i < len(sql):
        ch = sql[i]
        if ch == "'" and i + 1 < len(sql):
            in_string = not in_string
            result.append(ch)
            i += 1
            continue
        if not in_string and ch == ":" and i + 1 < len(sql) and sql[i + 1] == ":":
            result.append("::")
            i += 2
            continue
        if not in_string and ch in (":", "@") and i + 1 < len(sql) and sql[i + 1].isidentifier():
            j = i + 1
            while j < len(sql) and (sql[j].isalnum() or sql[j] == "_"):
                j += 1
            key = sql[i + 1 : j]
            if key not in params:
                raise ValueError(f"missing named parameter: {key}")
            ordered.append(params[key])
            result.append(f"${len(ordered)}")
            i = j
            continue
        result.append(ch)
        i += 1
    return "".join(result), ordered


def _rewrite_positional(sql: str, values: List[Any]) -> Tuple[str, List[Any]]:
    result = []
    ordered: List[Any] = []
    i = 0
    in_string = False
    idx = 0
    while i < len(sql):
        ch = sql[i]
        if ch == "'" and i + 1 < len(sql):
            in_string = not in_string
            result.append(ch)
            i += 1
            continue
        if not in_string and ch == "?":
            if idx >= len(values):
                raise ValueError("not enough parameters")
            ordered.append(values[idx])
            idx += 1
            result.append(f"${len(ordered)}")
            i += 1
            continue
        result.append(ch)
        i += 1
    if idx < len(values):
        raise ValueError("too many parameters")
    return "".join(result), ordered


def _parse_callable_invocation(text: str):
    open_paren = text.find("(")
    if open_paren < 0:
        routine = text.strip()
        if not routine:
            return None
        return routine, "", False

    in_single = False
    in_double = False
    depth = 0
    close_paren = -1
    for idx in range(open_paren, len(text)):
        ch = text[idx]
        if ch == "'" and not in_double:
            in_single = not in_single
            continue
        if ch == '"' and not in_single:
            in_double = not in_double
            continue
        if in_single or in_double:
            continue
        if ch == "(":
            depth += 1
            continue
        if ch == ")":
            depth -= 1
            if depth == 0:
                close_paren = idx
                break
    if close_paren < 0:
        return None
    routine = text[:open_paren].strip()
    if not routine:
        return None
    trailing = text[close_paren + 1 :].strip()
    if trailing:
        return None
    args = text[open_paren + 1 : close_paren].strip()
    return routine, args, True
