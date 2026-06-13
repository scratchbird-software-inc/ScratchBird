#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DBLC-013B listener lifecycle conformance static gate."""

from __future__ import annotations

import pathlib
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def main(argv: list[str]) -> int:
    if len(argv) != 1:
        print("usage: database_lifecycle_listener_conformance.py PROJECT_SOURCE_DIR", file=sys.stderr)
        return 2
    project = pathlib.Path(argv[0])

    cmake = read(project / "tests" / "listener" / "CMakeLists.txt")
    runtime = read(project / "src" / "listener" / "listener_runtime.cpp")
    pool = read(project / "src" / "listener" / "parser_pool.cpp")
    restart_smoke = read(project / "tests" / "listener" / "server_restart_killed_listener_smoke.cpp")
    authority_generator = read(project / "tools" / "generate_spec_authority_from_artifacts.py")
    lifecycle_closure = read(
        project / "tests" / "database_lifecycle" / "fixtures" / "full_database_lifecycle_closure" /
        "ACCEPTANCE_GATES.csv"
    )

    required_tests = [
        "sb_listener_runtime_handoff_smoke",
        "sb_listener_management_commands_smoke",
        "sb_listener_management_stop_smoke",
        "sb_listener_management_stop_force_smoke",
        "sb_listener_drain_admission_smoke",
        "sb_listener_parser_pool_exhaustion_smoke",
        "sb_listener_parser_crash_recovery_smoke",
        "sb_listener_kill_after_handoff_smoke",
        "sb_listener_owner_token_collision_smoke",
        "sb_listener_owner_token_stale_corrupt_smoke",
    ]
    for test in required_tests:
        require(test in cmake, f"required listener lifecycle test missing from CMake: {test}")
    require("database_lifecycle_listener" in cmake, "listener lifecycle label must be materialized")
    require("database_lifecycle_listener_conformance" in cmake, "aggregate listener lifecycle conformance test missing")

    require('::setenv("SB_DATABASE_SELECTOR"' in pool, "listener must pass database selector to parser workers")
    require('::setenv("SB_DATABASE_TOKEN"' in pool, "listener must pass parser-consumed database token to parser workers")
    require("kQuarantined" in pool, "parser-pool failure quarantine state must be implemented")
    require("parser_pool_.Stop(true)" in runtime, "listener must support forced parser-pool stop")
    require('WriteLifecycleState("draining")' in runtime, "listener drain must publish lifecycle state")
    require('WriteLifecycleState("running")' in runtime, "listener undrain/reload must publish running lifecycle state")
    require("LISTENER.RELOAD_ACCEPTED" in runtime, "listener reload acknowledgement diagnostic required")
    require("LISTENER.HANDOFF_DRAINING" in pool, "listener handoff must fail closed while draining")
    require(".sb.local_password_auth" in restart_smoke and "kAliceVerifier" in restart_smoke,
            "server-management listener restart test must use engine-owned password-verifier auth")
    require("SB_DATABASE_SELECTOR" in authority_generator and "SB_DATABASE_TOKEN" in authority_generator,
            "public authority generator must define listener database selector/token binding")
    require("database isolation" in authority_generator.lower(),
            "public authority generator must define listener database isolation behavior")
    require("DBLC_P13B_LISTENER_LIFECYCLE_COMPLETE" in lifecycle_closure,
            "public lifecycle fixture must carry listener lifecycle closure proof")

    print("database_lifecycle_listener_conformance=passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except AssertionError as exc:
        print(f"database_lifecycle_listener_conformance=failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
