#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Compiled placement gate only, not canonical rendering/runtime conformance."""
import argparse
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--engine-archive", type=Path, required=True)
    parser.add_argument("--renderer-archive", type=Path, required=True)
    parser.add_argument("--nm", required=True)
    args = parser.parse_args()
    failures = []
    engine = args.source_root / "src/engine/internal_api"
    for name in ("diagnostic_rendering.hpp", "diagnostic_rendering.cpp"):
        if (engine / "diagnostics" / name).exists():
            failures.append("renderer remains in engine source tree: " + name)
    cmake = (engine / "CMakeLists.txt").read_text()
    for token in ("diagnostics/diagnostic_rendering.cpp", "sb_server_legacy_diagnostic_rendering"):
        if token in cmake:
            failures.append("engine target retains rendering membership/dependency")
    symbols = {}
    for kind, archive in (("engine", args.engine_archive), ("renderer", args.renderer_archive)):
        run = subprocess.run([args.nm, "-C", "--defined-only", str(archive)],
                             capture_output=True, text=True, check=False)
        if run.returncode:
            failures.append(kind + " symbol inspection failed")
        symbols[kind] = run.stdout
    for name in ("RenderEngineApiResultForParserPackage", "ValidateLegacyRenderedProjectionStructure"):
        if name + "(" in symbols["engine"]:
            failures.append("engine archive defines parser rendering: " + name)
        if "scratchbird::server::legacy_rendering::" + name + "(" not in symbols["renderer"]:
            failures.append("server archive lacks implementation: " + name)
        if "scratchbird::engine::internal_api::" + name + "(" in symbols["renderer"]:
            failures.append("server archive retains engine rendering namespace: " + name)
    for failure in failures:
        print("FAIL " + failure)
    if failures:
        return 1
    print("PASS actual_archive_placement=2 renderer_entrypoints=2 runtime_acceptance=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
