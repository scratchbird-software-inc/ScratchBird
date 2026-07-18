#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


def load_gate(path: pathlib.Path):
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location(
        "parser_family_runtime_isolation_gate", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load gate: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class RuntimeIsolationGateTests(unittest.TestCase):
    gate = None
    ownership_map: pathlib.Path

    def test_runtime_prefix_accepts_exact_owned_worker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            prefix = pathlib.Path(temporary)
            worker = prefix / "bin/sbp_firebird"
            worker.parent.mkdir(parents=True)
            worker.write_text("fixture", encoding="utf-8")
            self.assertEqual([], self.gate.runtime_prefix_gaps(prefix, "firebird"))

    def test_runtime_prefix_rejects_foreign_sibling_worker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            prefix = pathlib.Path(temporary)
            bin_dir = prefix / "bin"
            bin_dir.mkdir(parents=True)
            (bin_dir / "sbp_firebird").write_text("owned", encoding="utf-8")
            (bin_dir / "sbp_postgresql").write_text("foreign", encoding="utf-8")
            gaps = self.gate.runtime_prefix_gaps(prefix, "firebird")
            self.assertTrue(
                any("foreign_parser_executables=sbp_postgresql" in gap for gap in gaps),
                gaps,
            )

    def test_runtime_prefix_rejects_sbsql_worker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            prefix = pathlib.Path(temporary)
            bin_dir = prefix / "bin"
            bin_dir.mkdir(parents=True)
            (bin_dir / "sbp_firebird").write_text("owned", encoding="utf-8")
            (bin_dir / "SBParser").write_text("foreign", encoding="utf-8")
            gaps = self.gate.runtime_prefix_gaps(prefix, "firebird")
            self.assertTrue(
                any("foreign_parser_executables=SBParser" in gap for gap in gaps), gaps
            )

    def test_search_path_rejects_foreign_worker(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            (directory / "sbp_mysql").write_text("foreign", encoding="utf-8")
            gaps = self.gate.search_path_parser_gaps(str(directory), "firebird")
            self.assertEqual(1, len(gaps), gaps)
            self.assertIn("foreign_search_path_parser_executable", gaps[0])

    def test_trace_extracts_only_successful_execve(self) -> None:
        trace = (
            'execve("/isolated/bin/sbp_firebird", ["sbp_firebird"], 0x1) = 0\n'
            'execve("/missing/sbp_mysql", ["sbp_mysql"], 0x1) = -1 ENOENT\n'
            'execve("/usr/bin/isql", ["isql"], 0x1) = 0\n'
        )
        self.assertEqual(
            ["/isolated/bin/sbp_firebird", "/usr/bin/isql"],
            self.gate.successful_execve_paths(trace),
        )

    def test_foreign_parser_exec_is_rejected(self) -> None:
        execs = [
            "/isolated/bin/sbp_firebird",
            "/fallback/bin/sbp_postgresql",
            "/fallback/bin/SBParser",
        ]
        self.assertEqual(
            ["/fallback/bin/SBParser", "/fallback/bin/sbp_postgresql"],
            self.gate.unexpected_parser_execs(execs, "firebird"),
        )

    def test_registry_scan_rejects_foreign_trace_artifacts(self) -> None:
        payload = self.gate.identity.strict_json_loads(
            self.ownership_map.read_text(encoding="utf-8")
        )
        self.assertIsInstance(payload, dict)
        registry = self.gate.identity.Registry(payload)
        findings = self.gate.identity.scan_text(
            registry,
            "firebird",
            "runtime_trace",
            'execve("/isolated/bin/sbp_postgresql", ["sbp_postgresql"], 0x1) = 0\n'
            'connect(5, {sun_path="/run/scratchbird/parser_sbsql.sock"}, 42) = 0\n',
        )
        self.assertEqual({"postgresql", "sbsql"}, {row.foreign_family for row in findings})

    def test_registry_scan_accepts_owned_worker_and_neutral_route(self) -> None:
        payload = json.loads(self.ownership_map.read_text(encoding="utf-8"))
        registry = self.gate.identity.Registry(payload)
        findings = self.gate.identity.scan_text(
            registry,
            "firebird",
            "runtime_trace",
            'execve("/isolated/bin/sbp_firebird", ["sbp_firebird"], 0x1) = 0\n'
            'execve("/coherent/bin/SBgate", ["SBgate"], 0x1) = 0\n'
            'connect(5, {sun_path="/tmp/runtime/server/sbps.sock"}, 42) = 0\n',
        )
        self.assertEqual([], findings)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate-script", required=True, type=pathlib.Path)
    parser.add_argument("--ownership-map", required=True, type=pathlib.Path)
    args = parser.parse_args()
    RuntimeIsolationGateTests.gate = load_gate(args.gate_script.resolve())
    RuntimeIsolationGateTests.ownership_map = args.ownership_map.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RuntimeIsolationGateTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
