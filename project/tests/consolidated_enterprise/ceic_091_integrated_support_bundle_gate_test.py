#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""CTest wrapper for CEIC-091 integrated support-bundle validation.

SEARCH_KEY: CEIC_091_INTEGRATED_SUPPORT_BUNDLE_GATE_TEST
"""

from __future__ import annotations

import argparse
import copy
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
from collections.abc import Callable
from typing import Any


def run(command: list[str], *, expect_success: bool) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if expect_success and result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected success: {' '.join(command)}")
    if not expect_success and result.returncode == 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise AssertionError(f"expected failure: {' '.join(command)}")
    return result


def expect_failure_contains(command: list[str], text: str) -> None:
    result = run(command, expect_success=False)
    output = result.stdout + result.stderr
    if text not in output:
        raise AssertionError(f"expected failure output to contain {text!r}, got: {output}")


def load_tool(tool_path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("ceic_integrated_support_bundle", tool_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load tool module: {tool_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_manifest(path: pathlib.Path, model: dict[str, Any]) -> None:
    path.write_text(json.dumps(model, indent=2, sort_keys=True), encoding="utf-8")


def first_row(model: dict[str, Any], subsystem: str) -> dict[str, Any]:
    for section in model["sections"]:
        if section["subsystem"] == subsystem:
            return section["rows"][0]
    raise AssertionError(f"missing subsystem: {subsystem}")


def first_section(model: dict[str, Any], subsystem: str) -> dict[str, Any]:
    for section in model["sections"]:
        if section["subsystem"] == subsystem:
            return section
    raise AssertionError(f"missing subsystem: {subsystem}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    tool = repo_root / "project/tools/ceic_integrated_support_bundle.py"
    command = [sys.executable, str(tool), "--repo-root", str(repo_root)]

    positive = run(command, expect_success=True)
    module = load_tool(tool)
    base_model = module.default_model(repo_root)

    with tempfile.TemporaryDirectory(prefix="ceic_091_bundle_") as temp_text:
        temp_dir = pathlib.Path(temp_text)
        cases: list[tuple[str, str, Callable[[dict[str, Any]], None]]] = [
            (
                "missing_subsystem",
                "missing_subsystem",
                lambda model: model["sections"].pop(2),
            ),
            (
                "unbounded",
                "unbounded",
                lambda model: first_section(model, "memory").update({"bounded": False, "row_limit": 0}),
            ),
            (
                "streaming_required",
                "streaming_required",
                lambda model: first_section(model, "index").update({"streaming_or_chunked": False}),
            ),
            (
                "protected_material",
                "protected_material",
                lambda model: first_row(model, "memory").update(
                    {
                        "value": "plaintext secret token",
                        "redacted": False,
                        "protected_material_excluded": False,
                    }
                ),
            ),
            (
                "placeholder_tamperless",
                "placeholder_tamperless",
                lambda model: first_row(model, "optimizer").update({"tamper_digest": "sha256:0000000000000000"}),
            ),
            (
                "retention_redaction_metadata",
                "retention_redaction_metadata",
                lambda model: first_row(model, "agent").update({"retention_class": ""}),
            ),
            (
                "sidecar_only",
                "sidecar_only",
                lambda model: first_section(model, "agent").update({"sidecar_only_evidence": True}),
            ),
            (
                "unsafe_authority",
                "unsafe_authority",
                lambda model: first_row(model, "index")["authority_flags"].update(
                    {"transaction_finality_authority": True}
                ),
            ),
            (
                "local_cluster",
                "local_cluster_claim",
                lambda model: first_section(model, "optimizer").update({"local_cluster_production_claim": True}),
            ),
            (
                "fixture_test_only",
                "fixture_test_only",
                lambda model: first_section(model, "memory").update({"fixture_or_test_only_evidence": True}),
            ),
            (
                "successor_overclaim",
                "successor_overclaim",
                lambda model: model.update({"ceic_092_route_chain_claimed": True}),
            ),
        ]

        for name, expected, mutate in cases:
            model = copy.deepcopy(base_model)
            mutate(model)
            path = temp_dir / f"{name}.json"
            write_manifest(path, model)
            expect_failure_contains(command + ["--manifest", str(path), "--skip-execution_plan-control"], expected)

    print("ceic_091_integrated_support_bundle_gate_test=pass")
    print(positive.stdout.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
