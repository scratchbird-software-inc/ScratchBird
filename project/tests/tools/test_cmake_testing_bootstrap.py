# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Exercise actual root CMake initialization, not runtime feature conformance."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = 'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PublicBranding.cmake")'


class TestingBootstrapTest(unittest.TestCase):
    def test_first_configure_sets_option_before_conditional_test_enrollment(self):
        source = (ROOT / "CMakeLists.txt").read_text()
        self.assertEqual(source.count(BOUNDARY), 1)
        prefix = source.split(BOUNDARY, 1)[0]
        cmake = shutil.which("cmake")
        self.assertIsNotNone(cmake)
        for setting, expected in [(None, "ON"), ("ON", "ON"), ("OFF", "OFF")]:
            with self.subTest(setting=setting), tempfile.TemporaryDirectory(prefix="sbct-") as tmp:
                root = Path(tmp)
                (root / "CMakeLists.txt").write_text(prefix + '''
if(NOT DEFINED BUILD_TESTING)
  message(FATAL_ERROR "BUILD_TESTING was not initialized before test enrollment")
endif()
if(BUILD_TESTING)
  file(WRITE "${CMAKE_BINARY_DIR}/bootstrap-state.txt" "ON")
else()
  file(WRITE "${CMAKE_BINARY_DIR}/bootstrap-state.txt" "OFF")
endif()
''')
                command = [cmake, "-S", str(root), "-B", str(root / "build")]
                if setting is not None:
                    command.append("-DBUILD_TESTING=" + setting)
                env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
                for configure_number in (1, 2):
                    result = subprocess.run(command, capture_output=True, text=True,
                                            timeout=60, env=env)
                    self.assertEqual(result.returncode, 0,
                                     f"configure {configure_number}: {result.stdout}{result.stderr}")
                    self.assertEqual((root / "build/bootstrap-state.txt").read_text(), expected)


if __name__ == "__main__":
    unittest.main()
