#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Interpreter selection must agree between admission and child execution."""
import contextlib
import io
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import driver_component_runner as runner


class InterpreterSelectionTest(unittest.TestCase):
    def test_selected_python_not_sdk_path(self):
        with mock.patch.object(runner.sys, "executable", "/selected python/python"), \
                mock.patch.object(runner.shutil, "which", return_value="/sdk/python3") as lookup:
            self.assertEqual(
                runner.resolve_argv(["python3", "-m", "pytest"], {"PATH": "/sdk"}),
                ["/selected python/python", "-m", "pytest"])
            lookup.assert_not_called()

    def test_explicit_interpreter_preserved(self):
        command = ["/explicit/python", "script.py"]
        self.assertEqual(runner.resolve_argv(command, {}), command)

    def test_other_tool_uses_supplied_path(self):
        with mock.patch.object(runner.shutil, "which", return_value="/tools/cmake") as lookup:
            self.assertEqual(runner.resolve_argv(["cmake", "--version"], {"PATH": "/tools"}),
                             ["/tools/cmake", "--version"])
            lookup.assert_called_once_with("cmake", path="/tools")

    def test_empty_command_unchanged(self):
        self.assertEqual(runner.resolve_argv([], {}), [])

    def test_python_already_admitted_without_path_alias(self):
        context = SimpleNamespace(component="driver:python", require_all_toolchains=True,
                                  allow_toolchain_waivers=False)
        with mock.patch.object(runner.shutil, "which", return_value=None):
            self.assertEqual(runner.check_toolchains(context), 0)

    def test_missing_non_python_tool_still_fails(self):
        context = SimpleNamespace(component="driver:go", require_all_toolchains=True,
                                  allow_toolchain_waivers=False)
        with mock.patch.object(runner.shutil, "which", return_value=None), \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(runner.check_toolchains(context), 1)

    def test_child_log_header_precedes_output(self):
        with tempfile.TemporaryDirectory() as directory:
            context = runner.Context(Path(directory), Path(directory), Path(directory),
                                     "driver:python", False, True)
            def child(argv, **kwargs):
                os.write(kwargs["stdout"].fileno(), b"child-output\n")
                return SimpleNamespace(returncode=0)
            with mock.patch.object(runner.subprocess, "run", side_effect=child), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(runner.run_command(context, "ordering", ["python3", "-V"],
                                                   cwd=Path(directory)), 0)
            log = (context.logs_root / "ordering.log").read_text()
            self.assertLess(log.index("cwd="), log.index("child-output"))

    def test_nested_dependencies_do_not_import_cached_admission(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "CMakeCache.txt"
            cache.write_text("SBL_NUMERIC_MPFR_INCLUDE_DIR:PATH=/deps with spaces/include\n"
                             "SBL_NUMERIC_REFERENCE_LINKS:INTERNAL=1\n"
                             "SB_BUILD_TESTS:BOOL=OFF\n")
            context = runner.Context(Path(directory), Path(directory), Path(directory),
                                     "tool:cli", False, True, cache)
            with mock.patch.object(runner, "run_command", return_value=0) as command:
                self.assertEqual(runner.run_cmake_component(context, Path(directory)), 0)
            arguments = command.call_args_list[0].args[2]
            self.assertIn("-DSBL_NUMERIC_MPFR_INCLUDE_DIR=/deps with spaces/include", arguments)
            self.assertFalse(any("REFERENCE_LINKS" in arg or "SB_BUILD_TESTS" in arg
                                 for arg in arguments))
            self.assertEqual(command.call_count, 3)

    def test_unreadable_dependency_cache_runs_no_child(self):
        with tempfile.TemporaryDirectory() as directory:
            context = runner.Context(Path(directory), Path(directory), Path(directory),
                                     "tool:cli", False, True, Path(directory) / "missing")
            with mock.patch.object(runner, "run_command") as command, \
                    contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(runner.run_cmake_component(context, Path(directory)), 1)
            command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
