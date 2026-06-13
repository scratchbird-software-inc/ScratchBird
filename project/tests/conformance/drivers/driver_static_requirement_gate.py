#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Mode-specific static gates for beta driver SBLR/UUID/language/cache slices."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

from validate_shared_beta_driver_corpus import validate


CORPUS_ROOT = Path("project/tests/conformance/drivers/corpora/shared_beta_driver_corpus")
LANGUAGE_SURFACE_GATE = Path("project/tools/release/sbsql_driver_language_surface_gate.py")

MODE_COVERAGE = {
    "sblr_uuid_bundle": {
        "driver_sbsql_command_surface",
        "sblr_uuid_server_revalidation",
        "auth_context_injection",
    },
    "schema_path_resolution": {
        "authorization_filtered_uuid_path_resolution",
    },
    "sblr_uuid_authorization": {
        "sblr_uuid_server_revalidation",
        "cross_user_replay_refusal",
        "role_group_replay_refusal",
        "auth_context_injection",
    },
    "cache_lifecycle": {
        "cache_lifecycle",
        "sblr_uuid_server_revalidation",
    },
    "sbsql_language_resource": {
        "language_surface_en-US",
        "language_surface_en-CA",
        "language_surface_fr-CA",
        "language_surface_fr-FR",
        "language_surface_de-DE",
        "language_surface_it-IT",
        "language_surface_es-ES",
        "language_fallback_standard_en",
    },
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[4]


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def collect_coverage(repo_root: Path) -> set[str]:
    coverage: set[str] = set()
    for path in sorted((repo_root / CORPUS_ROOT).glob("*.json")):
        doc = json.loads(path.read_text(encoding="utf-8"))
        for key in ("cases", "oracles", "budgets"):
            for item in as_list(doc.get(key)):
                if isinstance(item, dict):
                    coverage.update(str(value) for value in as_list(item.get("coverage")))
    for rel in (
        Path("project/tests/conformance/resources/driver_resource_soak_requirements.json"),
        Path("project/tests/security/driver_auth_replay_refusal_corpus.json"),
        Path("project/tests/performance/driver_resource_soak_requirements.json"),
    ):
        doc = json.loads((repo_root / rel).read_text(encoding="utf-8"))
        for key in ("cases", "oracles", "budgets"):
            for item in as_list(doc.get(key)):
                if isinstance(item, dict):
                    coverage.update(str(value) for value in as_list(item.get("coverage")))
    return coverage


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--mode", choices=sorted(MODE_COVERAGE), required=True)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    errors = validate(repo_root)
    try:
        coverage = collect_coverage(repo_root)
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"{args.mode}:coverage_load_failed:{exc}")
        coverage = set()
    missing = MODE_COVERAGE[args.mode] - coverage
    if missing:
        errors.append(f"{args.mode}:missing_coverage:" + ",".join(sorted(missing)))
    if args.mode == "sbsql_language_resource" and not (repo_root / LANGUAGE_SURFACE_GATE).is_file():
        errors.append("sbsql_language_resource:missing_language_surface_gate")
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"driver_static_requirement_gate {args.mode}: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
