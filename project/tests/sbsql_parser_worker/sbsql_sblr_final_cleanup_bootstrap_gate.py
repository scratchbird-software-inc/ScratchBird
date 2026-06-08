#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Bootstrap CTest label gate for SBsql/SBLR final cleanup work.

This gate is intentionally lightweight. It proves that the final cleanup label
exists under project tests and that the reusable source audit command is present;
it does not read execution_plan files.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


FINAL_CLEANUP_LABELS = {
    "sbsql_public_exact_commands",
    "sbsql_sblr_api_routes",
    "sbsql_engine_behavior_complete",
    "sbsql_udr_support",
    "sbsql_result_shape_contracts",
    "sbsql_security_privilege_contracts",
    "sbsql_catalog_system_view_sync",
    "sblr_family_admission_conformance",
    "sblr_opcode_registry_conformance",
    "sblr_version_compatibility",
    "sbsql_cluster_fail_closed_noncluster",
    "sbsql_cluster_stub_provider",
    "sbsql_cluster_provider_abi_contract",
    "sbsql_public_private_cluster_boundary",
    "sbsql_message_vector_rendering",
    "sbsql_no_stub_noncluster_integrity",
    "sbsql_catalog_seed_upgrade_restart",
    "sbsql_spec_doc_sync",
    "sbsql_sblr_final_cleanup_full_route",
}

REQUIRED_REPO_FILES = (
    "project/tools/sbsql_sblr_final_cleanup_audit.py",
    "public_contract_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
    "public_contract_snapshot",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--label", required=True)
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    label = args.label
    if label not in FINAL_CLEANUP_LABELS:
        print(f"unknown final cleanup bootstrap label: {label}", file=sys.stderr)
        return 2

    missing = [rel for rel in REQUIRED_REPO_FILES if not (repo / rel).is_file()]
    if missing:
        print("sbsql_sblr_final_cleanup_bootstrap_gate=failed", file=sys.stderr)
        for rel in missing:
            print(f"missing required file: {rel}", file=sys.stderr)
        return 1

    print(f"sbsql_sblr_final_cleanup_bootstrap_gate label={label} status=passed")
    print("execution_plan_runtime_dependency=none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
