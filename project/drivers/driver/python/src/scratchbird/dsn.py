# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DSN parsing helpers for ScratchBird Python driver."""

from __future__ import annotations

import re
import urllib.parse


def normalize_native_protocol(value: str | None) -> str:
    normalized = (value or "").strip().lower()
    # The Python lane currently speaks the native wire protocol only, but accepts
    # common protocol/dialect hints used by JDBC/ODBC tooling and DSN builders.
    if normalized in (
        "",
        "native",
        "scratchbird",
        "scratchbird-native",
        "scratchbird_native",
        "sbwp",
        "postgres",
        "postgresql",
        "pg",
        "jdbc",
        "odbc",
        "sql",
        "mysql",
        "mariadb",
        "sqlite",
        "duckdb",
        "firebird",
    ):
        return "native"
    return "native"


def normalize_front_door_mode(value: str | None) -> str:
    normalized = (value or "").strip().lower()
    if normalized in ("", "direct"):
        return "direct"
    if normalized in ("manager_proxy", "manager-proxy", "managed"):
        return "manager_proxy"
    raise ValueError("front_door_mode must be direct or manager_proxy.")


def normalize_ssl_mode(value: str | None) -> str:
    normalized = (value or "").strip().lower()
    if normalized in ("", "require"):
        return "require"
    if normalized in ("verify-ca", "verifyca"):
        return "verify-ca"
    if normalized in ("verify-full", "verifyfull"):
        return "verify-full"
    if normalized in ("disable", "off", "false", "0", "no"):
        return "disable"
    if normalized in ("on", "true", "1", "yes"):
        return "require"
    if normalized in ("allow", "prefer"):
        # JDBC-compatible lenient modes still negotiate TLS in this lane.
        return "require"
    raise ValueError("sslmode must be disable, require, verify-ca, or verify-full.")


def normalize_compression_mode(value: str | None) -> str:
    normalized = (value or "").strip().lower()
    if normalized in ("", "off", "none", "false", "0", "no"):
        return "off"
    if normalized in ("zstd", "on", "true", "1", "yes"):
        return "zstd"
    raise ValueError("compression must be off or zstd.")


def parse_dsn(dsn: str | None) -> dict:
    if not dsn:
        return {}

    if "://" in dsn:
        return _parse_uri(dsn)
    return _parse_kv(dsn)


def _parse_uri(dsn: str) -> dict:
    parsed = urllib.parse.urlparse(dsn)
    if parsed.scheme not in ("scratchbird", "sb"):
        raise ValueError(f"Unsupported DSN scheme: {parsed.scheme}")

    params = {}
    if parsed.hostname:
        params["host"] = parsed.hostname
    if parsed.port:
        params["port"] = parsed.port
    if parsed.username:
        params["user"] = urllib.parse.unquote(parsed.username)
    if parsed.password:
        params["password"] = urllib.parse.unquote(parsed.password)
    if parsed.path and parsed.path != "/":
        params["database"] = parsed.path.lstrip("/")

    query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
    for key, values in query.items():
        if values:
            params[key] = values[-1]
    return params


def _parse_kv(dsn: str) -> dict:
    params = {}
    tokens = re.split(r"[;\s]+", dsn.strip())
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        params[key.strip()] = value.strip()
    return params
