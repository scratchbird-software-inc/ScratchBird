#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Command-boundary checks; real process ownership is qualified separately."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import build_native_driver_tools as builder


class BuildLifetimeContract(unittest.TestCase):
    def test_dotnet_disables_reusable_nodes_and_compiler_servers(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            with mock.patch.object(builder.shutil, "which", return_value="dotnet"), \
                    mock.patch.object(builder, "stage_driver_source", return_value=root / "stage"), \
                    mock.patch.object(builder, "run_command", return_value={"returncode": 19}) as run:
                result = builder.build_compiled_tool(root, root / "build", "dotnet")
            command = run.call_args.args[0]
            self.assertEqual(command[:2], ["dotnet", "publish"])
            for option in ("--disable-build-servers", "/nr:false", "-p:UseSharedCompilation=false"):
                self.assertIn(option, command)
            self.assertNotIn("--no-build", command)
            self.assertEqual(result["returncode"], 19)

    def test_gradle_is_single_build_even_on_failure(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            wrapper = root / "project/drivers/driver/jdbc/gradlew"
            wrapper.parent.mkdir(parents=True)
            wrapper.touch()
            with mock.patch.object(builder, "stage_driver_source", return_value=root / "stage"), \
                    mock.patch.object(builder, "run_command", return_value={"returncode": 23}) as run:
                result = builder.build_compiled_tool(root, root / "build", "jdbc")
            self.assertEqual(run.call_args.args[0],
                             ["bash", str(root / "stage/gradlew"), "--no-daemon", "classes"])
            self.assertEqual(result["returncode"], 23)

    def test_command_result_retains_failure_output_and_timing(self):
        result = builder.run_command(
            [sys.executable, "-c", "print('deliberate failure'); raise SystemExit(17)"], Path.cwd())
        self.assertEqual(result["returncode"], 17)
        self.assertEqual(result["output_tail"], ["deliberate failure"])
        self.assertGreaterEqual(result["elapsed_seconds"], 0)

    def test_launch_failure_retains_diagnostic_and_timing(self):
        with tempfile.TemporaryDirectory() as name:
            result = builder.run_command([str(Path(name) / "absent-tool")], Path(name), dict(os.environ))
        self.assertEqual(result["returncode"], 127)
        self.assertTrue(result["output_tail"][0].startswith("launch_failed:"))
        self.assertGreaterEqual(result["elapsed_seconds"], 0)


if __name__ == "__main__":
    unittest.main()
