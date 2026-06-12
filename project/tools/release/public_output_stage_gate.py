#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the staged standalone public output tree."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


REQUIRED_DIRS = (
    "bin",
    "lib",
    "etc/scratchbird",
    "share/scratchbird/resources",
    "share/scratchbird/resources/seed-packs/initial-resource-pack/resources/charsets",
    "share/scratchbird/resources/seed-packs/initial-resource-pack/resources/collations",
    "share/scratchbird/resources/seed-packs/initial-resource-pack/resources/timezones",
    "share/scratchbird/resources/policy-packs/default-local-password",
    "share/scratchbird/docs/public_api",
    "share/scratchbird/docs/release",
    "share/scratchbird/examples/core_beta_qa",
)

REQUIRED_PARSER_EXECUTABLES = (
    "SBParser",
)

REQUIRED_PARSER_UDR_LIBRARIES = tuple(
    f"{parser_name}_udr" for parser_name in REQUIRED_PARSER_EXECUTABLES
)

REQUIRED_PARSER_CONFIGS = tuple(
    f"etc/scratchbird/{parser_name}.conf"
    for parser_name in REQUIRED_PARSER_EXECUTABLES
)

REQUIRED_FILES = (
    "STANDALONE_OUTPUT_MANIFEST.json",
    "etc/scratchbird/SBsrv.conf",
    "etc/scratchbird/SBgate.conf",
    "etc/scratchbird/SBmgr.conf",
    *REQUIRED_PARSER_CONFIGS,
    "share/scratchbird/resources/seed-packs/initial-resource-pack/RESOURCE_SEED_MANIFEST.csv",
    "share/scratchbird/resources/seed-packs/initial-resource-pack/resources/charsets/charsets.json",
    "share/scratchbird/resources/seed-packs/initial-resource-pack/resources/collations/collations.json",
    "share/scratchbird/resources/seed-packs/initial-resource-pack/resources/timezones/version",
    "share/scratchbird/resources/policy-packs/default-local-password/POLICY_PACK_MANIFEST.json",
    "share/scratchbird/resources/policy-packs/default-local-password/policies/security_providers.json",
    "share/scratchbird/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json",
    "share/scratchbird/docs/release/PUBLIC_SUPPORT_MAINTENANCE_POLICY.md",
    "share/scratchbird/examples/core_beta_qa/manifest.json",
)

REQUIRED_EXECUTABLES = (
    "SBsql",
    "SBadm",
    "SBbak",
    "SBsec",
    "SBdoc",
    "SBcop",
    "SBsrv",
    "SBgate",
    *REQUIRED_PARSER_EXECUTABLES,
)

def static_library_candidates(library_name: str) -> dict[str, tuple[str, ...]]:
    return {
        "linux": (f"lib/lib{library_name}.a",),
        "bsd": (f"lib/lib{library_name}.a",),
        "windows": (f"lib/{library_name}.lib", f"lib/lib{library_name}.a"),
    }


REQUIRED_LIBRARY_CANDIDATES = (
    (
        "engine_shared_library",
        {
            "linux": ("lib/libSBcore.so",),
            "bsd": ("lib/libSBcore.so",),
            "windows": ("bin/SBcore.dll", "lib/SBcore.dll"),
        },
    ),
    (
        "engine_static_library",
        {
            "linux": ("lib/libSBcore_static.a",),
            "bsd": ("lib/libSBcore_static.a",),
            "windows": ("lib/SBcore_static.lib", "lib/libSBcore_static.a"),
        },
    ),
    *(
        (f"{library_name}_library", static_library_candidates(library_name))
        for library_name in REQUIRED_PARSER_UDR_LIBRARIES
    ),
)


def fail(message: str) -> int:
    print(f"public_output_stage_gate: FAIL: {message}", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    args = parser.parse_args()

    root = args.artifact_root.resolve()
    failures: list[str] = []
    if args.platform not in {"linux", "windows", "bsd"}:
        failures.append(f"unsupported_platform:{args.platform}")
    if root.name != args.platform:
        failures.append(f"artifact_root_platform_mismatch:{root}")
    if root.parent.name != "output":
        failures.append(f"artifact_root_not_under_output:{root}")

    for rel in REQUIRED_DIRS:
        path = root / rel
        if not path.is_dir():
            failures.append(f"missing_directory:{rel}")
    for rel in REQUIRED_FILES:
        path = root / rel
        if not path.is_file():
            failures.append(f"missing_file:{rel}")

    executable_suffix = ".exe" if args.platform == "windows" else ""
    for name in REQUIRED_EXECUTABLES:
        rel = f"bin/{name}{executable_suffix}"
        path = root / rel
        if not path.is_file():
            failures.append(f"missing_executable:{rel}")
        elif path.stat().st_size <= 0:
            failures.append(f"empty_executable:{rel}")

    for label, candidates_by_platform in REQUIRED_LIBRARY_CANDIDATES:
        candidates = candidates_by_platform.get(args.platform, ())
        if not any((root / rel).is_file() and (root / rel).stat().st_size > 0 for rel in candidates):
            failures.append(f"missing_library:{label}:{','.join(candidates)}")

    manifest_path = root / "STANDALONE_OUTPUT_MANIFEST.json"
    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as exc:
            failures.append(f"manifest_invalid_json:{exc}")
        else:
            if manifest.get("platform") != args.platform:
                failures.append("manifest_platform_mismatch")
            if manifest.get("short_brand") != "SBcde":
                failures.append("manifest_short_brand_mismatch")
            if manifest.get("product") != "ScratchBird Convergent Data Engine":
                failures.append("manifest_product_mismatch")

    if failures:
        print("\n".join(failures[:200]), file=sys.stderr)
        if len(failures) > 200:
            print(f"... {len(failures) - 200} more", file=sys.stderr)
        return 1
    print("public_output_stage_gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
