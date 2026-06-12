#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-013P static gate for admin/CLI lifecycle routing."""

from __future__ import annotations

import argparse
import pathlib
import sys


REQUIRED_OPERATIONS = {
    "health": ("health_database", "SHOW SERVER LIFECYCLE"),
    "status": ("status_database", "SHOW SERVER LIFECYCLE"),
    "create": ("create_database", "ADMIN LIFECYCLE CREATE DATABASE"),
    "open": ("open_database", "ADMIN LIFECYCLE OPEN DATABASE"),
    "attach": ("attach_database", "ADMIN LIFECYCLE ATTACH DATABASE"),
    "detach": ("detach_database", "ADMIN LIFECYCLE DETACH DATABASE"),
    "inspect": ("inspect_database", "INSPECT DATABASE"),
    "verify": ("verify_database", "VERIFY DATABASE"),
    "repair": ("repair_database", "ADMIN LIFECYCLE REPAIR DATABASE"),
    "shutdown": ("shutdown_database", "SHUTDOWN DATABASE"),
    "shutdown-force": ("shutdown_database_force", "SHUTDOWN DATABASE FORCE"),
    "drop": ("drop_database", "DROP DATABASE"),
}

ROUTE_FILES = [
    "project/src/server/cli.hpp",
    "project/src/server/cli.cpp",
    "project/src/server/manager_control.cpp",
    "project/src/server/maintenance_coordinator.cpp",
    "project/src/server/ipc_server.cpp",
    "project/drivers/tool/cli/sb_admin.cpp",
    "project/drivers/tool/cli/res_lifecycle_parity.cpp",
    "project/tests/database_lifecycle/admin_cli_conformance.cpp",
]

FORBIDDEN_SHORTCUTS = [
    "sqlite3",
    "PRAGMA",
    "journal_mode",
    "reference shortcut",
    "placeholder lifecycle",
]


def read(repo: pathlib.Path, rel: str) -> str:
    path = repo / rel
    if not path.exists():
        raise AssertionError(f"missing required file: {rel}")
    return path.read_text(encoding="utf-8")


def require_token(text: str, token: str, label: str) -> None:
    if token not in text:
        raise AssertionError(f"{label}: missing token {token!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=".")
    args = parser.parse_args()
    repo = pathlib.Path(args.repo_root).resolve()

    contents = {rel: read(repo, rel) for rel in ROUTE_FILES}
    combined = "\n".join(contents.values())

    for operation, (management_key, client_statement) in REQUIRED_OPERATIONS.items():
        require_token(combined, operation, "operation coverage")
        require_token(combined, management_key, "management key coverage")
        require_token(combined, client_statement, "client statement coverage")

    manager = contents["project/src/server/manager_control.cpp"]
    require_token(manager, "EngineAuthorizeManagement", "engine authorization route")
    require_token(manager, "authorization_authority", "structured auth evidence")
    require_token(manager, "RecordServerAuditEvent", "audit evidence route")
    require_token(manager, "DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE", "required static marker")
    require_token(manager, "PARSER_SERVER_IPC.SESSION_NOT_BOUND", "missing-session fail closed")
    require_token(manager, "SECURITY.ACCESS_DENIED", "insufficient-rights fail closed")
    require_token(combined, "ENGINE.SHUTDOWN_INPUT_INVALID", "force shutdown refusal diagnostic")
    require_token(combined, "ENGINE.DBLC_DROP_UNSAFE", "drop refusal diagnostic")

    ipc = contents["project/src/server/ipc_server.cpp"]
    require_token(ipc, "MessageType::kManagementRequest", "SBPS management route")
    require_token(ipc, "HandleServerManagementRequest", "SBPS management dispatch")

    cli = contents["project/drivers/tool/cli/sb_admin.cpp"]
    require_token(cli, "connectToDatabase", "authenticated client path")
    require_token(cli, "executeSQL", "client-visible route")
    require_token(cli, "lifecycleCommand", "admin lifecycle command")

    for shortcut in FORBIDDEN_SHORTCUTS:
        if shortcut in combined:
            raise AssertionError(f"forbidden lifecycle shortcut token present: {shortcut}")

    print("DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE=passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"DBLC_STATIC_ADMIN_AUTH_AUDIT_ROUTE=failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
