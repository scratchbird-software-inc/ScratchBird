#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Driver live-server fixture contract and lifecycle command anchor."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
from typing import Any


TOOLS_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS_ROOT / "release"))

from driver_release_common import (  # noqa: E402
    add_common_args,
    default_report_path,
    fail,
    is_closing_status,
    load_workplan_csv,
    report_status,
    resolve_repo_root,
    resolve_workplan_root,
    status_value,
    unique_index,
    write_report,
)


REPORT_NAME = "driver_test_server_fixture.json"
START_ENV = "SB_DRIVER_TEST_SERVER_START_COMMAND"
FIXTURE_MANIFEST = Path(
    "project/tests/conformance/drivers/fixtures/live_driver_test_server/manifest.json"
)


def state_path(state_dir: Path) -> Path:
    return state_dir / "driver_test_server_fixture.state.json"


def write_state(state_dir: Path, data: dict[str, Any]) -> None:
    state_dir.mkdir(parents=True, exist_ok=True)
    state_path(state_dir).write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_state(state_dir: Path) -> dict[str, Any]:
    path = state_path(state_dir)
    if not path.is_file():
        return {"state": "stopped"}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"state": "corrupt"}
    return data if isinstance(data, dict) else {"state": "corrupt"}


def read_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"invalid_json:{path}:{exc}") from exc
    if not isinstance(data, dict):
        raise ValueError(f"json_root_not_object:{path}")
    return data


def driver_test_server_start(args: argparse.Namespace) -> int:
    state_dir = args.state_dir.resolve()
    command = args.command or os.environ.get(START_ENV, "")
    if args.dry_run:
        write_state(
            state_dir,
            {
                "state": "dry_run_started",
                "endpoint": args.endpoint,
                "database_path": str(args.database_path) if args.database_path else None,
            },
        )
        print("driver_test_server_start=dry_run")
        return 0
    if not command:
        return fail(f"server_start_command_required:set_{START_ENV}_or_pass_--command")
    completed = subprocess.run(shlex.split(command), text=True, check=False)
    if completed.returncode != 0:
        return completed.returncode
    write_state(
        state_dir,
        {
            "state": "started",
            "endpoint": args.endpoint,
            "database_path": str(args.database_path) if args.database_path else None,
        },
    )
    print("driver_test_server_start=started")
    return 0


def driver_test_server_stop(args: argparse.Namespace) -> int:
    state_dir = args.state_dir.resolve()
    state = read_state(state_dir)
    if state.get("state") == "stopped":
        print("driver_test_server_stop=already_stopped")
        return 0
    write_state(state_dir, {"state": "stopped"})
    print("driver_test_server_stop=stopped")
    return 0


def driver_test_server_reset(args: argparse.Namespace) -> int:
    state_dir = args.state_dir.resolve()
    write_state(
        state_dir,
        {
            "state": "reset",
            "endpoint": args.endpoint,
            "database_path": str(args.database_path) if args.database_path else None,
            "fixture_seed": "contract_only_until_live_server_start_is_configured",
        },
    )
    print("driver_test_server_reset=reset")
    return 0


def driver_test_server_status(args: argparse.Namespace) -> int:
    state = read_state(args.state_dir.resolve())
    print(json.dumps(state, indent=2, sort_keys=True))
    return 0 if state.get("state") != "corrupt" else 1


def verify_fixture_contract(
    repo_root: Path,
    workplan_root: Path,
    contract_only: bool,
    latest_json: Path | None,
    commands_file: Path | None,
) -> dict[str, Any]:
    rows = load_workplan_csv(workplan_root, "LIVE_SERVER_TEST_FIXTURE_MATRIX.csv")
    by_id, issues = unique_index(rows, "fixture_id", "LIVE_SERVER_TEST_FIXTURE_MATRIX")
    areas = {row.get("area", "").strip() for row in by_id.values()}
    for area in ("startup", "shutdown", "reset"):
        if area not in areas:
            issues.append(f"fixture_matrix:missing_lifecycle_area:{area}")
    for fixture_id, row in sorted(by_id.items()):
        for field in ("area", "public_anchor", "required_fixture", "acceptance"):
            if not row.get(field, "").strip():
                issues.append(f"fixture_matrix:{fixture_id}:missing_{field}")
        if not contract_only and not is_closing_status(status_value(row)):
            issues.append(
                f"fixture_matrix:{fixture_id}:non_closing_status:{status_value(row) or 'empty'}"
            )

    source_text = Path(__file__).read_text(encoding="utf-8")
    for anchor in (
        "driver_test_server_start",
        "driver_test_server_stop",
        "driver_test_server_reset",
        "driver_test_server_status",
    ):
        if f"def {anchor}" not in source_text:
            issues.append(f"fixture_source:missing_anchor:{anchor}")

    manifest_path = repo_root / FIXTURE_MANIFEST
    if not manifest_path.is_file():
        issues.append(f"fixture_manifest:missing:{FIXTURE_MANIFEST.as_posix()}")
        manifest: dict[str, Any] = {}
    else:
        manifest = read_json(manifest_path)
        validate_fixture_manifest(manifest, issues)

    runtime_checked = False
    if latest_json is not None:
        latest = read_json(latest_json)
        validate_latest_runtime(latest, issues)
        runtime_checked = True
    if commands_file is not None:
        validate_commands_file(commands_file, issues)
        runtime_checked = True
    if not contract_only and not runtime_checked and not os.environ.get(START_ENV):
        issues.append(
            f"fixture_runtime:server_start_command_missing:{START_ENV}_or_latest_json"
        )
    return {
        "command": "driver_test_server_fixture.py verify",
        "gate_id": "BETA-DTA-GATE-025",
        "status": report_status(issues),
        "summary": {
            "fixture_rows": len(rows),
            "areas": sorted(areas),
            "contract_only": contract_only,
            "manifest_path": FIXTURE_MANIFEST.as_posix(),
            "latest_runtime_checked": latest_json is not None,
            "commands_file_checked": commands_file is not None,
        },
        "issues": issues,
    }


def validate_fixture_manifest(manifest: dict[str, Any], issues: list[str]) -> None:
    if manifest.get("schema_version") != "scratchbird_live_driver_test_server_fixture_v1":
        issues.append("fixture_manifest:invalid_schema_version")
    if "server revalidates" not in manifest.get("authority_boundary", "").lower():
        issues.append("fixture_manifest:missing_server_revalidation_boundary")
    required_sections = (
        "startup",
        "shutdown",
        "reset",
        "users",
        "roles_groups",
        "schema_objects",
        "transactions",
        "language_resources",
        "routes",
        "policy_epochs",
        "large_data",
        "diagnostics",
    )
    for section in required_sections:
        if section not in manifest:
            issues.append(f"fixture_manifest:missing_section:{section}")
    users = manifest.get("users", [])
    if not isinstance(users, list):
        issues.append("fixture_manifest:users_not_list")
        users = []
    user_names = {user.get("name") for user in users if isinstance(user, dict)}
    for name in ("alice", "bob", "carol", "admin"):
        if name not in user_names:
            issues.append(f"fixture_manifest:missing_user:{name}")
    alice = next(
        (user for user in users if isinstance(user, dict) and user.get("name") == "alice"),
        {},
    )
    alice_roles = set(alice.get("roles", [])) if isinstance(alice, dict) else set()
    alice_rights = set(alice.get("rights", [])) if isinstance(alice, dict) else set()
    if "sysarch" not in alice_roles:
        issues.append("fixture_manifest:alice_not_sysarch_role")
    for right in (
        "CONNECT",
        "SELECT",
        "CREATE",
        "ALTER",
        "DROP",
        "SEC_IDENTITY_ADMIN",
        "SEC_MEMBERSHIP_ADMIN",
        "SEC_GRANT_ADMIN",
        "POLICY_ADMIN",
        "OBS_MANAGEMENT_CONTROL",
        "OBS_MANAGEMENT_INSPECT",
        "OBS_CONFIG_INSPECT",
        "OBS_CONFIG_CONTROL",
        "OBS_METRICS_READ_ALL",
        "OBS_RUNTIME_ALL",
        "OBS_INDEX_PROFILE_READ",
        "MGA_TRANSACTION_INSPECT",
        "UDR_MANAGE",
        "UDR_INSPECT",
        "BACKUP_CREATE",
        "BACKUP_RESTORE",
        "MANAGER_ADMISSION_ADMIN",
    ):
        if right not in alice_rights:
            issues.append(f"fixture_manifest:alice_missing_sysarch_right:{right}")
    language_profiles = set(manifest.get("language_resources", {}).get("profiles", []))
    for profile in ("en-US", "en-CA", "fr-CA", "fr-FR", "de-DE", "it-IT", "es-ES"):
        if profile not in language_profiles:
            issues.append(f"fixture_manifest:missing_language_profile:{profile}")
    routes = manifest.get("routes", {})
    for route in ("local_ipc", "tcp_listener", "manager_proxy"):
        if routes.get(route) is not True:
            issues.append(f"fixture_manifest:missing_route:{route}")
    tx = manifest.get("transactions", {})
    if tx.get("authority") != "server_mga_transaction_inventory":
        issues.append("fixture_manifest:transaction_authority_not_server_mga")
    diagnostics = manifest.get("diagnostics", {})
    for code in (
        "SB_DRIVER_REPLAY_CONTEXT_MISMATCH",
        "SB_AUTH_OBJECT_NOT_VISIBLE",
        "SB_DRIVER_CACHE_STALE",
    ):
        if code not in diagnostics.get("expected_codes", []):
            issues.append(f"fixture_manifest:missing_diagnostic:{code}")


def validate_latest_runtime(latest: dict[str, Any], issues: list[str]) -> None:
    if latest.get("schema_version") != "scratchbird_driver_test_server_v1":
        issues.append("fixture_runtime:invalid_latest_schema")
    server = latest.get("server", {})
    listener = latest.get("listener", {})
    if not server.get("pid"):
        issues.append("fixture_runtime:missing_server_pid")
    if not listener.get("pid"):
        issues.append("fixture_runtime:missing_listener_pid")
    if listener.get("available") is not True:
        issues.append("fixture_runtime:listener_not_available")
    verification = latest.get("verification", {})
    local_ipc = verification.get("local_ipc", {})
    inet = verification.get("inet", {})
    if local_ipc.get("ok") is not True or local_ipc.get("returncode") != 0:
        issues.append("fixture_runtime:local_ipc_not_verified")
    inet_result = inet.get("result", {})
    if inet.get("ok") is not True and inet_result.get("returncode") != 0:
        issues.append("fixture_runtime:inet_not_verified")


def validate_commands_file(path: Path, issues: list[str]) -> None:
    text = path.read_text(encoding="utf-8")
    for token in (
        "start_server()",
        "start_listener()",
        "stop()",
        "force_stop()",
        "verify_ipc()",
        "verify_inet()",
    ):
        if token not in text:
            issues.append(f"fixture_commands:missing_token:{token}")


def driver_test_server_verify(args: argparse.Namespace) -> int:
    repo_root = resolve_repo_root(args.repo_root)
    workplan_root = resolve_workplan_root(repo_root, args.workplan_root)
    output = args.output or default_report_path(repo_root, REPORT_NAME)
    try:
        latest_json = args.latest_json.expanduser().resolve() if args.latest_json else None
        commands_file = args.commands_file.expanduser().resolve() if args.commands_file else None
        report = verify_fixture_contract(
            repo_root,
            workplan_root,
            args.contract_only,
            latest_json,
            commands_file,
        )
    except (OSError, ValueError) as exc:
        return fail(str(exc))
    write_report(output, report)
    print(f"driver_test_server_fixture={report['status']}")
    return 0 if report["status"] == "pass" else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    add_common_args(parser, Path(__file__))
    subparsers = parser.add_subparsers(dest="mode", required=True)

    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--contract-only", action="store_true")
    verify_parser.add_argument("--latest-json", type=Path)
    verify_parser.add_argument("--commands-file", type=Path)

    for name in ("start", "reset"):
        subparser = subparsers.add_parser(name)
        subparser.add_argument("--state-dir", type=Path, default=Path("build/driver_test_server"))
        subparser.add_argument("--endpoint", default="127.0.0.1:0")
        subparser.add_argument("--database-path", type=Path)
        if name == "start":
            subparser.add_argument("--command", default="")
            subparser.add_argument("--dry-run", action="store_true")

    stop_parser = subparsers.add_parser("stop")
    stop_parser.add_argument("--state-dir", type=Path, default=Path("build/driver_test_server"))

    status_parser = subparsers.add_parser("status")
    status_parser.add_argument("--state-dir", type=Path, default=Path("build/driver_test_server"))

    args = parser.parse_args()
    if args.mode == "verify":
        return driver_test_server_verify(args)
    if args.mode == "start":
        return driver_test_server_start(args)
    if args.mode == "stop":
        return driver_test_server_stop(args)
    if args.mode == "reset":
        return driver_test_server_reset(args)
    if args.mode == "status":
        return driver_test_server_status(args)
    return fail(f"unknown_mode:{args.mode}")


if __name__ == "__main__":
    raise SystemExit(main())
