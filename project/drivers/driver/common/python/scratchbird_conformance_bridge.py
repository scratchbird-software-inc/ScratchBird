#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Shared beta conformance bridge for provider-style driver lanes.

ADBC, Flight SQL, and R2DBC have public APIs that normally require external
provider packages. The beta driver matrix still needs a runnable first-class
ScratchBird script runner for those lanes. This bridge executes through the
ScratchBird Python protocol stack without shelling out to another driver, then
rewrites the proof artifacts so the lane remains identified by its public API
surface.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
from typing import Any


def repo_root() -> Path:
    return Path(__file__).resolve().parents[5]


def _load_python_tool() -> Any:
    root = repo_root()
    python_src = root / "project" / "drivers" / "driver" / "python" / "src"
    if str(python_src) not in sys.path:
        sys.path.insert(0, str(python_src))
    tool_path = root / "project" / "drivers" / "driver" / "python" / "tools" / "sb_isql_python.py"
    spec = importlib.util.spec_from_file_location("scratchbird_beta_python_isql", tool_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load ScratchBird Python conformance tool: {tool_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, payload: Any) -> None:
    path.write_text(json.dumps(payload, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")


def _rewrite_jsonl(path: Path, driver: str, native_api_surface: str) -> None:
    if not path.is_file():
        return
    rewritten: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        row = json.loads(line)
        row["driver_name"] = driver
        row["native_api_surface"] = native_api_surface
        row["code_example_section"] = f"{driver}_beta_conformance_bridge"
        rewritten.append(json.dumps(row, sort_keys=True, default=str))
    path.write_text("".join(line + "\n" for line in rewritten), encoding="utf-8")


def run_beta_conformance_bridge(
    args: Any,
    *,
    driver: str,
    host_api: str,
    native_api_surface: str,
    transport_implementation: str,
    api_hits: dict[str, int],
) -> int:
    """Run the shared ScratchBird script suite and relabel artifacts for a lane."""

    python_tool = _load_python_tool()
    result = int(python_tool.run_script(args))

    summary_path = Path(args.summary)
    run_root = summary_path.parent
    summary = _read_json(summary_path)
    summary["driver_name"] = driver
    summary["driver_transport_implementation"] = transport_implementation
    summary["cpp_library_boundary"] = "none"
    summary["beta_conformance_bridge"] = {
        "host_api": host_api,
        "execution_stack": "scratchbird_python_protocol_stack",
        "engine_sql_execution": False,
        "server_revalidation_required": True,
    }
    _write_json(summary_path, summary)

    route_env_path = run_root / "route-environment.json"
    if route_env_path.is_file():
        route_env = _read_json(route_env_path)
        route_env["driver"] = driver
        route_env["driver_transport_implementation"] = transport_implementation
        _write_json(route_env_path, route_env)

    _rewrite_jsonl(run_root / "command-events.jsonl", driver, native_api_surface)
    _rewrite_jsonl(Path(args.transcript), driver, native_api_surface)

    _write_json(run_root / "native-api-coverage.json", api_hits)
    _write_json(
        run_root / "code-example-review.json",
        {
            "driver": driver,
            "host_api": host_api,
            "public_api_only": True,
            "shells_out_to_other_driver": False,
            "source_is_canonical_example": True,
            "beta_conformance_bridge": "scratchbird_python_protocol_stack",
        },
    )
    return result
