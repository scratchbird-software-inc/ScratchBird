#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate BSD peer-owner listener management authentication source contract."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


def fail(message: str) -> None:
    print(f"engine_listener_bsd_peer_owner_management_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"source_read_failed:{path}:{exc}")


def extract_verify_function(text: str) -> str:
    match = re.search(r"bool\s+VerifyPeerOwnerEvidence\s*\([^)]*\)\s*\{", text, re.DOTALL)
    require(match is not None, "verify_peer_owner_function_missing")
    depth = 1
    index = match.end()
    while index < len(text) and depth:
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
        index += 1
    require(depth == 0, "verify_peer_owner_function_unclosed")
    return text[match.end(): index - 1]


def extract_bsd_branch(function_body: str) -> str:
    marker = "#elif !defined(_WIN32) && (defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__))"
    start = function_body.find(marker)
    require(start != -1, "bsd_peer_owner_branch_missing")
    next_branch = function_body.find("#else", start)
    require(next_branch != -1, "bsd_peer_owner_branch_not_fail_closed")
    return function_body[start:next_branch]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True, type=Path)
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    source_path = project_root / "src" / "listener" / "listener_runtime.cpp"
    source = read_text(source_path)
    function_body = extract_verify_function(source)
    bsd_branch = extract_bsd_branch(function_body)

    for token in (
        "getpeereid(peer_fd, &peer_uid, &peer_gid)",
        "const uid_t owner_uid = ::getuid();",
        "peer_uid != owner_uid",
        "LISTENER.MANAGEMENT.PEER_OWNER_UNAVAILABLE",
        "LISTENER.MANAGEMENT.PEER_OWNER_INVALID",
        "\"BSD Unix peer credentials were unavailable for management authentication\"",
        "{\"peer_uid\", std::to_string(static_cast<unsigned long long>(peer_uid))}",
        "{\"owner_uid\", std::to_string(static_cast<unsigned long long>(owner_uid))}",
        "{\"peer_gid\", std::to_string(static_cast<unsigned long long>(peer_gid))}",
    ):
        require(token in bsd_branch, f"bsd_peer_owner_token_missing:{token}")

    require("return true;" in bsd_branch, "bsd_peer_owner_success_path_missing")
    require(bsd_branch.count("return false;") >= 3, "bsd_peer_owner_fail_closed_paths_missing")
    require("peer-owner management authentication is unavailable on this platform" in function_body,
            "unsupported_platform_fail_closed_fallback_missing")
    require("SO_PEERCRED" in function_body, "linux_peer_owner_path_missing")

    print("engine_listener_bsd_peer_owner_management_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
