#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parent


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    contract = json.loads((ROOT / "package_contract.json").read_text(encoding="utf-8"))

    require(contract["component_id"] == "tool:cli", "component_id mismatch")
    require(contract["category"] == "tool", "category mismatch")
    require(contract["status"] == "beta_2", "status mismatch")
    require(contract["release_scope"] == "in_scope_required", "release_scope mismatch")
    require(contract["server_revalidation_required"] is True, "server revalidation must be required")
    require(contract["transaction_authority"] == "mga_engine", "transaction authority mismatch")
    require(contract["delegation_posture"]["mode"] == "explicit_session", "delegation mode mismatch")
    require(contract["delegation_posture"]["target_component"] == "driver:cpp", "target component mismatch")
    require(contract["artifact_verification"]["package_type"] == "cmake_install_tree", "package type mismatch")
    require(contract["dbeaver_exclusion"]["this_artifact_includes_dbeaver"] is False, "DBeaver must be excluded")
    require(len(contract["best_in_class_deltas"]) >= 2, "best-in-class deltas missing")

    for relative in contract["package_files"]:
        require("dbeaver" not in relative, f"unexpected DBeaver package file {relative}")
        require((ROOT / relative).is_file(), f"missing package file {relative}")

    print("cli_package_contract_smoke: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"cli_package_contract_smoke: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
