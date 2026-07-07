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


def _assert_connect_guard(dsn: str, expected_message_fragment: str) -> None:
    cfg = scratchbird.ScratchBirdConfig(dsn)
    try:
        scratchbird.connect(cfg)
        raise RuntimeError("expected connect guard to reject DSN")
    except Exception as exc:
        _require(
            expected_message_fragment in str(exc),
            f"expected guard message containing '{expected_message_fragment}', got '{exc}'",
        )


def _assert_connect_guard_sqlstate(dsn: str, expected_sqlstate: str) -> None:
    cfg = scratchbird.ScratchBirdConfig(dsn)
    try:
        scratchbird.connect(cfg)
        raise RuntimeError("expected connect guard to reject DSN")
    except Exception as exc:
        actual = str(getattr(exc, "sqlstate", "") or "")
        _require(
            actual == expected_sqlstate,
            f"expected sqlstate '{expected_sqlstate}', got '{actual}'",
        )


def _assert_connect_ok(dsn: str) -> None:
    cfg = scratchbird.ScratchBirdConfig(dsn)
    conn = scratchbird.connect(cfg)
    conn.close()


def main() -> None:
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=disable",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?ssl=disable",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?binary_transfer=false",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?binarytransfer=false",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?compression=zstd",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?compression=none",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?compression=gzip",
        "0A000",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?front_door_mode=invalid",
        "0A000",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?protocol=scratchbird-native",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?parser=scratchbird_native",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?dialect=postgres",
        "0A000",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://:pass@localhost:3092/testdb?sslmode=require",
        "28000",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/?sslmode=require",
        "28000",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@/testdb?sslmode=require",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?host=",
        "28000",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?note=%ZZ",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@[::1:3092/testdb?sslmode=require",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?front_door_mode=managerproxy",
        "08001",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?front_door_mode=direct&connection_mode=managerproxy",
        "08001",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?front_door_mode=managerproxy&manager_auth_token=token",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?front_door_mode=managerproxy&mcp_auth_token=token",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?frontdoormode=managerproxy&ingress_mode=direct",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?frontdoormode=direct&ingress_mode=managerproxy",
        "08001",
    )
    _assert_connect_ok(
        "scratchbird://localhost:3092/testdb?sslmode=require&user=u1&username=u2&pguser=u3&password=p1&passwd=p2&pgpassword=p3&host=h1&hostname=h2&servername=h3&pghost=h4&database=db1&dbname=db2&databaseName=db3&pgdatabase=db4&port=4100&portNumber=4101&pgport=4102",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://localhost:3092/testdb?sslmode=require&user=u1&pguser=",
        "28000",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://localhost:3092/testdb?sslmode=require&host=h1&pghost=",
        "28000",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://localhost:3092/testdb?sslmode=require&database=db1&pgdatabase=",
        "28000",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&front_door_mode=managerproxy&manager_auth_token=token_a&mcp_auth_token=",
        "08001",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?min_pool_size=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?max_pool_size=0",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?min_pool_size=5&max_pool_size=2",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?default_row_fetch_size=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?default_row_fetch_size=64&fetchSize=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?default_row_fetch_size=-1",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?connection_lifetime=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?connection_lifetime=30&poolingconnectionlifetime=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?poolingconnectionlifetime=-1",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?manager_client_flags=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?manager_client_flags=1&mcp_client_flags=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?connect_client_flags=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?client_flags=-1",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?auth_method_id=invalid.namespace",
        "28000",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?connect_client_flags=257&auth_method_id=scratchbird.auth.proxy_principal_assertion",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?mcp_client_flags=-1",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?port=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?port=4100&portNumber=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?port=0",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?pgport=70000",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?connect_timeout=bad",
        "22023",
    )
    _assert_connect_ok(
        "scratchbird://user:pass@localhost:3092/testdb?connect_timeout=5&connecttimeout=6&socket_timeout=7&sockettimeout=8&login_timeout=9&logintimeout=10&acquire_timeout=11&poolingacquiretimeout=12",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?connect_timeout=5&connecttimeout=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?sockettimeout=-1",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?pooling_acquire_timeout=-1",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?prepare_threshold=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?prepare_threshold=5&preparethreshold=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?cb_failure_threshold=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?cb_recovery_timeout_ms=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?cb_success_threshold=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?cb_half_open_max_requests=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?keepalive_max_idle_before_check_ms=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?pipeline_max_in_flight=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?pipeline_auto_flush_threshold=bad",
        "22023",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?leak_threshold_ms=bad",
        "22023",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sb_test_auth_fail=true",
        "authentication failed",
    )
    _assert_connect_guard_sqlstate(
        "scratchbird://user:pass@localhost:3092/testdb?sb_test_auth_fail=true",
        "28P01",
    )
    print("Mojo connection guard tests OK")


if __name__ == "__main__":
    main()
