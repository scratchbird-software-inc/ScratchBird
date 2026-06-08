# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "src"))

import scratchbird


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


class QueryErrorHarness:
    def __init__(self, fail_on: str):
        self.fail_on = fail_on
        self.calls = []

    def _begin_operation(self, name: str, sql: str):
        self.calls.append(("begin", name, sql))
        return "span"

    def _end_operation(self, span, success: bool):
        self.calls.append(("end", span, success))

    def _extended_query(self, sql: str, params):
        self.calls.append(("extended", sql, list(params)))
        if self.fail_on == "extended":
            raise scratchbird.ScratchBirdError(
                "extended path failure",
                "XX000",
                detail="extended detail",
                hint="extended hint",
            )
        return scratchbird.ScratchBirdResult([[1]], [], 1)

    def _send_message(self, msg_type: int, payload: bytes, flags: int = 0, force_zero: bool = False):
        self.calls.append(("send", msg_type))

    def _read_resultset(self):
        self.calls.append(("read",))
        if self.fail_on == "simple":
            raise scratchbird.ScratchBirdError(
                "simple path failure",
                "XX001",
                detail="simple detail",
                hint="simple hint",
            )
        if self.fail_on == "truncated":
            raise RuntimeError("row data truncated")
        return scratchbird.ScratchBirdResult([[1]], [], 1)


def test_scratchbird_error_fields() -> None:
    err = scratchbird.ScratchBirdError(
        "field test",
        "22000",
        detail="bad data",
        hint="check input",
    )
    _require(err.sqlstate == "22000", "sqlstate field mismatch")
    _require(err.detail == "bad data", "detail field mismatch")
    _require(err.hint == "check input", "hint field mismatch")


def test_simple_query_error_propagates_detail_and_hint() -> None:
    conn = QueryErrorHarness("simple")
    try:
        scratchbird.ScratchBirdConnection.query(conn, "SELECT 1", None)
        raise RuntimeError("expected simple-path query failure")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "XX001", "simple-path sqlstate mismatch")
        _require(exc.detail == "simple detail", "simple-path detail mismatch")
        _require(exc.hint == "simple hint", "simple-path hint mismatch")
    _require(("end", "span", False) in conn.calls, "simple-path failure should mark operation unsuccessful")


def test_extended_query_error_propagates_detail_and_hint() -> None:
    conn = QueryErrorHarness("extended")
    try:
        scratchbird.ScratchBirdConnection.query(conn, "SELECT $1::INTEGER", [1])
        raise RuntimeError("expected extended-path query failure")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "XX000", "extended-path sqlstate mismatch")
        _require(exc.detail == "extended detail", "extended-path detail mismatch")
        _require(exc.hint == "extended hint", "extended-path hint mismatch")
    _require(("end", "span", False) in conn.calls, "extended-path failure should mark operation unsuccessful")


def test_auth_guard_exposes_sqlstate() -> None:
    cfg = scratchbird.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&sb_test_auth_fail=true"
    )
    try:
        scratchbird.connect(cfg)
        raise RuntimeError("expected auth guard failure")
    except scratchbird.ScratchBirdError as exc:
        _require(exc.sqlstate == "28P01", "auth guard sqlstate mismatch")
        _require(str(exc) == "authentication failed", "auth guard message mismatch")


def test_retry_scope_helper_classifies_statement_and_reconnect_boundaries() -> None:
    _require(scratchbird.retry_scope_for_sqlstate("40001") == "statement", "40001 retry scope mismatch")
    _require(scratchbird.retry_scope_for_sqlstate("40P01") == "statement", "40P01 retry scope mismatch")
    _require(scratchbird.retry_scope_for_sqlstate("08006") == "reconnect", "08006 retry scope mismatch")
    _require(scratchbird.retry_scope_for_sqlstate("57014") == "none", "57014 retry scope mismatch")
    _require(scratchbird.retry_scope_for_sqlstate(None) == "none", "nil retry scope mismatch")
    _require(scratchbird.is_retryable_sqlstate("40001"), "40001 should be retryable")
    _require(not scratchbird.is_retryable_sqlstate("57014"), "57014 should not be auto-retryable")


def test_canonical_isolation_helper_documents_public_alias_mapping() -> None:
    _require(
        scratchbird.canonical_isolation_label(scratchbird.ISOLATION_READ_UNCOMMITTED) == "READ COMMITTED",
        "read uncommitted alias mismatch",
    )
    _require(
        scratchbird.canonical_isolation_label(scratchbird.ISOLATION_READ_COMMITTED) == "READ COMMITTED",
        "read committed alias mismatch",
    )
    _require(
        scratchbird.canonical_isolation_label(scratchbird.ISOLATION_REPEATABLE_READ) == "SNAPSHOT",
        "repeatable read alias mismatch",
    )
    _require(
        scratchbird.canonical_isolation_label(scratchbird.ISOLATION_SERIALIZABLE) == "SNAPSHOT TABLE STABILITY",
        "serializable alias mismatch",
    )


def test_simple_query_truncation_failure_surfaces() -> None:
    conn = QueryErrorHarness("truncated")
    try:
        scratchbird.ScratchBirdConnection.query(conn, "SELECT 1", None)
        raise RuntimeError("expected truncation failure")
    except RuntimeError as exc:
        _require("row data truncated" in str(exc), "truncation error message mismatch")
    _require(("end", "span", False) in conn.calls, "truncation failure should mark operation unsuccessful")


def main() -> None:
    test_scratchbird_error_fields()
    test_simple_query_error_propagates_detail_and_hint()
    test_extended_query_error_propagates_detail_and_hint()
    test_auth_guard_exposes_sqlstate()
    test_retry_scope_helper_classifies_statement_and_reconnect_boundaries()
    test_canonical_isolation_helper_documents_public_alias_mapping()
    test_simple_query_truncation_failure_surfaces()
    print("Mojo error propagation tests OK")


if __name__ == "__main__":
    main()
