# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pytest

from scratchbird.sql import normalize_callable_query, normalize_callable_sql, normalize_query


def test_normalize_positional():
    sql = "SELECT ?"
    rewritten, params = normalize_query(sql, (1,))
    assert rewritten == "SELECT $1"
    assert params == [1]


def test_normalize_named():
    sql = "SELECT :id, @name"
    rewritten, params = normalize_query(sql, {"id": 1, "name": "Ada"})
    assert rewritten == "SELECT $1, $2"
    assert params == [1, "Ada"]


def test_normalize_empty_named_mapping_is_noop():
    sql = "SELECT 1"
    rewritten, params = normalize_query(sql, {})
    assert rewritten == "SELECT 1"
    assert params == []


def test_normalize_named_preserves_cast_syntax():
    sql = "SELECT :id::INTEGER"
    rewritten, params = normalize_query(sql, {"id": 1})
    assert rewritten == "SELECT $1::INTEGER"
    assert params == [1]


def test_normalize_callable_sql_procedure_and_function_escape_syntax():
    assert normalize_callable_sql("{call demo.proc(?, :name)}") == "call demo.proc(?, :name)"
    assert normalize_callable_sql("{? = call demo.fn(:id)}") == "select demo.fn(:id) as return_value"


def test_normalize_callable_query_rewrites_escape_and_parameters():
    sql, params = normalize_callable_query("{call demo.proc(?, ?)}", [1, 2])
    assert sql == "call demo.proc($1, $2)"
    assert params == [1, 2]

    sql, params = normalize_callable_query("{? = call demo.fn(:id)}", {"id": 7})
    assert sql == "select demo.fn($1) as return_value"
    assert params == [7]


def test_normalize_callable_sql_rejects_invalid_escape_call_syntax():
    with pytest.raises(ValueError, match="invalid JDBC escape call syntax"):
        normalize_callable_sql("{call ()}")
