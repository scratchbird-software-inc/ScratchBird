#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True


def load_gate(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("listener_parser_neutrality_gate", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ListenerParserNeutralityGateTests(unittest.TestCase):
    gate = None

    def fixture(self, root: pathlib.Path) -> pathlib.Path:
        project = root / "project"
        listener = project / "src" / "listener"
        listener.mkdir(parents=True)
        (listener / "CMakeLists.txt").write_text(
            "add_executable(sb_listener main.cpp)\n", encoding="utf-8"
        )
        (listener / "main.cpp").write_text(
            "int main() { return 0; }\n", encoding="utf-8"
        )
        (project / "src" / "parsers" / "compatibility" / "firebird").mkdir(
            parents=True
        )
        return project

    def rules(self, project: pathlib.Path) -> set[str]:
        return {row.rule for row in self.gate.audit_project(project)}

    def test_generic_listener_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            self.assertEqual(set(), self.rules(project))

    def test_parse_statement_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "main.cpp").write_text(
                "void route() { ParseStatement(\"select 1\"); }\n", encoding="utf-8"
            )
            self.assertIn("listener_parser_entrypoint", self.rules(project))

    def test_grammar_artifact_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            cmake = project / "src" / "listener" / "CMakeLists.txt"
            cmake.write_text(cmake.read_text(encoding="utf-8") + "set(GRAMMAR sql.g4)\n")
            self.assertIn("listener_grammar_artifact", self.rules(project))

    def test_family_specific_auth_branch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "main.cpp").write_text(
                'if (protocol_family == "firebird") { authenticate_password(); }\n',
                encoding="utf-8",
            )
            self.assertIn("family_specific_auth_branch", self.rules(project))

    def test_compiled_protocol_family_literal_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "main.cpp").write_text(
                'const char* family = "firebird.wire.v13";\n', encoding="utf-8"
            )
            self.assertIn("compiled_protocol_family_literal", self.rules(project))

    def test_compiled_parser_executable_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "main.cpp").write_text(
                'const char* parser = "/opt/scratchbird/sbp_firebird";\n',
                encoding="utf-8",
            )
            self.assertIn("compiled_parser_executable", self.rules(project))

    def test_compiled_port_default_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "main.cpp").write_text(
                "struct Config { unsigned short port{3050}; };\n",
                encoding="utf-8",
            )
            self.assertIn("compiled_listener_port_default", self.rules(project))

    def test_compiled_endpoint_string_default_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "main.cpp").write_text(
                'struct Config { const char* bind_address{"127.0.0.1"}; };\n',
                encoding="utf-8",
            )
            self.assertIn("compiled_endpoint_string_default", self.rules(project))

    def test_compiled_profile_registry_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "main.cpp").write_text(
                "void ApplyProfileDefaults();\n", encoding="utf-8"
            )
            self.assertIn("compiled_parser_profile_registry", self.rules(project))

    def test_family_specific_auth_alias_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "main.cpp").write_text(
                'const char* value = "SB_REFERENCE_FIREBIRD_PASSWORD";\n',
                encoding="utf-8",
            )
            self.assertIn("family_specific_auth_alias", self.rules(project))

    def test_ambient_parser_child_environment_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "parser_pool.cpp").write_text(
                "void spawn() { GetEnvironmentStringsA(); }\n",
                encoding="utf-8",
            )
            rules = self.rules(project)
            self.assertIn("ambient_parser_child_environment", rules)
            self.assertIn("closed_parser_child_exec_missing", rules)

    def test_setenv_parser_child_launch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            (project / "src" / "listener" / "parser_pool.cpp").write_text(
                'void spawn() { ::setenv("ARBITRARY_SECRET", "x", 1); '\
                '::execve("/parser", nullptr, nullptr); }\n',
                encoding="utf-8",
            )
            self.assertIn("ambient_parser_child_environment", self.rules(project))

    def test_second_listener_executable_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            project = self.fixture(pathlib.Path(temporary))
            cmake = project / "src" / "listener" / "CMakeLists.txt"
            cmake.write_text(
                cmake.read_text(encoding="utf-8")
                + "add_executable(sb_listener_firebird firebird.cpp)\n",
                encoding="utf-8",
            )
            self.assertIn("per_family_listener_executable", self.rules(project))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gate-script", required=True, type=pathlib.Path)
    args = parser.parse_args()
    ListenerParserNeutralityGateTests.gate = load_gate(args.gate_script.resolve())
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(
        ListenerParserNeutralityGateTests
    )
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
