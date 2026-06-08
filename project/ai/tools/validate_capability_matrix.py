#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate ScratchBird AI capability matrix JSON against required contract.

This script performs deterministic validation without external dependencies.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

VERSION_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}\.[0-9]+$")
DATE_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
ALLOWED_STATUS = {"unavailable", "experimental", "partial", "baseline", "full"}
ALLOWED_DIALECTS = {
    "native",
}
REQUIRED_CAP_FIELDS = {
    "status",
    "read_select",
    "write_dml",
    "ddl",
    "transactions",
    "prepare_bind",
    "metadata_introspection",
    "vector_ops",
    "graph_ops",
    "last_verified_at",
    "compat_version",
}


def fail(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def expect_type(name: str, value: Any, expected: type) -> None:
    if not isinstance(value, expected):
        fail(f"{name} must be {expected.__name__}, got {type(value).__name__}")


def validate_capability(dialect: str, cap: dict[str, Any]) -> None:
    extra = set(cap.keys()) - REQUIRED_CAP_FIELDS
    missing = REQUIRED_CAP_FIELDS - set(cap.keys())
    if missing:
        fail(f"dialect '{dialect}' missing required fields: {sorted(missing)}")
    if extra:
        fail(f"dialect '{dialect}' has unknown fields: {sorted(extra)}")

    if cap["status"] not in ALLOWED_STATUS:
        fail(f"dialect '{dialect}' has invalid status: {cap['status']}")

    for field in [
        "read_select",
        "write_dml",
        "ddl",
        "transactions",
        "prepare_bind",
        "metadata_introspection",
        "vector_ops",
        "graph_ops",
    ]:
        if not isinstance(cap[field], bool):
            fail(f"dialect '{dialect}' field '{field}' must be boolean")

    if not isinstance(cap["compat_version"], str) or not cap["compat_version"].strip():
        fail(f"dialect '{dialect}' field 'compat_version' must be non-empty string")

    if not isinstance(cap["last_verified_at"], str) or not DATE_RE.match(cap["last_verified_at"]):
        fail(
            f"dialect '{dialect}' field 'last_verified_at' must match YYYY-MM-DD"
        )


def validate_matrix(data: dict[str, Any]) -> None:
    expect_type("root", data, dict)

    expected_root = {"version", "dialects"}
    extra = set(data.keys()) - expected_root
    missing = expected_root - set(data.keys())
    if missing:
        fail(f"root missing required fields: {sorted(missing)}")
    if extra:
        fail(f"root has unknown fields: {sorted(extra)}")

    version = data["version"]
    if not isinstance(version, str) or not VERSION_RE.match(version):
        fail("version must match YYYY-MM-DD.N")

    dialects = data["dialects"]
    expect_type("dialects", dialects, dict)
    if not dialects:
        fail("dialects must contain at least one entry")

    for dialect, cap in dialects.items():
        if dialect not in ALLOWED_DIALECTS:
            fail(f"unknown dialect key: {dialect}")
        expect_type(f"dialects.{dialect}", cap, dict)
        validate_capability(dialect, cap)


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate capability matrix JSON")
    parser.add_argument(
        "--matrix",
        default="capability/capability-matrix.v0.json",
        help="Path to capability matrix JSON",
    )
    args = parser.parse_args()

    path = Path(args.matrix)
    if not path.exists():
        fail(f"matrix file not found: {path}")

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"invalid JSON: {exc}")

    validate_matrix(data)
    print(f"OK: capability matrix is valid ({path})")


if __name__ == "__main__":
    main()
