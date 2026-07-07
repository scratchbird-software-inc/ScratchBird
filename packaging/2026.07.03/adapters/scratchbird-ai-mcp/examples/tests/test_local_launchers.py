# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
RUN_LOCAL_STACK = REPO_ROOT / "tools" / "run_local_stack.sh"


def _write_fake_python(path: Path) -> None:
    path.write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env bash
            set -euo pipefail

            log_file="${FAKE_PYTHON_LOG:?}"

            if [[ "${1:-}" == "-c" && "${2:-}" == *"mcp.server.fastmcp"* ]]; then
              echo "preflight" >> "${log_file}"
              if [[ "${FAKE_PYTHON_MCP_AVAILABLE:-0}" == "1" ]]; then
                exit 0
              fi
              exit 1
            fi

            if [[ "${1:-}" == "-m" && "${2:-}" == "scratchbird_ai.http_bridge" ]]; then
              echo "http_bridge" >> "${log_file}"
              exit 0
            fi

            if [[ "${1:-}" == "-m" && "${2:-}" == "scratchbird_ai.mcp_server" ]]; then
              echo "mcp_server" >> "${log_file}"
              exit 0
            fi

            echo "unexpected:$*" >> "${log_file}"
            exit 0
            """
        ),
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class LocalLauncherTests(unittest.TestCase):
    def _run_stack(self, *, mcp_available: bool) -> tuple[subprocess.CompletedProcess[str], list[str], bool]:
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            log_path = tmp_path / "fake-python.log"
            bridge_log = tmp_path / "bridge.log"
            fake_python = tmp_path / "python3"
            _write_fake_python(fake_python)

            env = os.environ.copy()
            env["PATH"] = str(tmp_path) + os.pathsep + env.get("PATH", "")
            env["FAKE_PYTHON_LOG"] = str(log_path)
            env["FAKE_PYTHON_MCP_AVAILABLE"] = "1" if mcp_available else "0"
            env["SCRATCHBIRD_AI_BRIDGE_LOG"] = str(bridge_log)

            result = subprocess.run(
                ["bash", str(RUN_LOCAL_STACK)],
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
                env=env,
            )
            lines = log_path.read_text(encoding="utf-8").splitlines() if log_path.exists() else []
            return result, lines, bridge_log.exists()

    def test_run_local_stack_fails_before_starting_bridge_when_runtime_missing(self) -> None:
        result, lines, bridge_started = self._run_stack(mcp_available=False)

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(lines, ["preflight"])
        self.assertFalse(bridge_started)
        self.assertIn("MCP runtime is not installed", result.stderr)

    def test_run_local_stack_starts_bridge_and_server_after_preflight(self) -> None:
        result, lines, bridge_started = self._run_stack(mcp_available=True)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(lines[0], "preflight")
        self.assertIn("http_bridge", lines)
        self.assertIn("mcp_server", lines)
        self.assertTrue(bridge_started)
        self.assertIn("Starting ScratchBird AI HTTP bridge", result.stdout)
        self.assertIn("Starting ScratchBird AI MCP server", result.stdout)


if __name__ == "__main__":
    unittest.main()
