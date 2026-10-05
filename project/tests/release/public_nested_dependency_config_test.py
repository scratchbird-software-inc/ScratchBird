#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Unit coverage for nested dependency forwarding; not backend qualification."""

from __future__ import annotations

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[2] / "tools" / "release"
sys.path.insert(0, str(TOOLS))
from public_nested_dependency_config import dependency_configure_args


class DependencyForwardingTest(unittest.TestCase):
    def setUp(self) -> None:
        self.owned = tempfile.TemporaryDirectory(prefix="sb-nested-dependencies-")
        self.addCleanup(self.owned.cleanup)
        self.cache = Path(self.owned.name) / "CMakeCache.txt"

    def forward(self, contents: str) -> list[str]:
        self.cache.write_text(contents, encoding="utf-8")
        return dependency_configure_args(self.cache)

    def test_standalone_discovery_is_not_overridden(self) -> None:
        self.assertEqual(dependency_configure_args(None), [])

    def test_explicit_missing_cache_is_an_error(self) -> None:
        with self.assertRaises(FileNotFoundError):
            dependency_configure_args(self.cache)

    def test_only_dependency_locations_are_forwarded(self) -> None:
        actual = self.forward(
            "# parent configuration\n"
            "SBL_NUMERIC_MPFR_LIBRARY:FILEPATH=/opt/backend/lib/libmpfr.so\n"
            "CMAKE_PREFIX_PATH:UNINITIALIZED=/opt/deps one;/opt/deps two\n"
            "CMAKE_LIBRARY_PATH:STRING=/opt/lib one;/opt/lib two\n"
            "CMAKE_INCLUDE_PATH:PATH=/opt/include\n"
            "SBL_NUMERIC_MPFR_INCLUDE_DIR:PATH=/opt/MPFR=4.2/include\n"
            "SBL_NUMERIC_GMP_INCLUDE_DIR:PATH=/opt/gmp/include\n"
            "SBL_NUMERIC_GMP_LIBRARY:FILEPATH=/opt/gmp/lib/libgmp.so\n"
            "SBL_NUMERIC_BOOST_INCLUDE_DIR:PATH=/opt/boost/include\n"
            "CMAKE_BUILD_TYPE:STRING=Debug\n"
            "CMAKE_CXX_FLAGS:STRING=-DSUPPRESS_CHECKS=1\n"
            "SB_BUILD_TESTS:BOOL=OFF\n"
            "SBL_NUMERIC_REFERENCE_LINKS:INTERNAL=1\n"
            "SBL_NUMERIC_REFERENCE_TLS:INTERNAL=1\n"
        )
        self.assertEqual(actual, [
            "-DCMAKE_PREFIX_PATH=/opt/deps one;/opt/deps two",
            "-DCMAKE_LIBRARY_PATH=/opt/lib one;/opt/lib two",
            "-DCMAKE_INCLUDE_PATH=/opt/include",
            "-DSBL_NUMERIC_MPFR_INCLUDE_DIR=/opt/MPFR=4.2/include",
            "-DSBL_NUMERIC_MPFR_LIBRARY=/opt/backend/lib/libmpfr.so",
            "-DSBL_NUMERIC_GMP_INCLUDE_DIR=/opt/gmp/include",
            "-DSBL_NUMERIC_GMP_LIBRARY=/opt/gmp/lib/libgmp.so",
            "-DSBL_NUMERIC_BOOST_INCLUDE_DIR=/opt/boost/include",
        ])

    def test_unresolved_or_malformed_inputs_are_refused(self) -> None:
        for content in (
            "SBL_NUMERIC_MPFR_INCLUDE_DIR:PATH=SBL_NUMERIC_MPFR_INCLUDE_DIR-NOTFOUND\n",
            "SBL_NUMERIC_MPFR_LIBRARY:FILEPATH=NOTFOUND\n",
            "SBL_NUMERIC_GMP_LIBRARY:FILEPATH=\n",
            "SBL_NUMERIC_GMP_INCLUDE_DIR:BOOL=ON\n",
            "SBL_NUMERIC_GMP_INCLUDE_DIR:PATH\n",
            "SBL_NUMERIC_GMP_INCLUDE_DIR=/opt/include\n",
            "CMAKE_PREFIX_PATH:STRING=one\0two\n",
            "CMAKE_PREFIX_PATH:PATH=/first\nCMAKE_PREFIX_PATH:STRING=/second\n",
            "CMAKE_PREFIX_PATH:PATH=\nCMAKE_PREFIX_PATH:PATH=/second\n",
        ):
            with self.subTest(content=content), self.assertRaises(ValueError):
                self.forward(content)

    def test_empty_search_hints_and_comments_are_not_arguments(self) -> None:
        self.assertEqual(self.forward(
            "// SBL_NUMERIC_MPFR_LIBRARY:FILEPATH=not a setting\n"
            "CMAKE_PREFIX_PATH:STRING=\n"
            "CMAKE_LIBRARY_PATH:PATH=\n"
            "OTHER_OPTION:STRING=-DSBL_NUMERIC_REFERENCE_TLS=1\n"
        ), [])

    def test_path_spelling_is_preserved(self) -> None:
        value = r"C:\SDK with spaces\mpfr\lib\mpfr.lib"
        self.assertEqual(
            self.forward("SBL_NUMERIC_MPFR_LIBRARY:FILEPATH=" + value + "\n"),
            ["-DSBL_NUMERIC_MPFR_LIBRARY=" + value],
        )


    def test_second_cmake_boundary_preserves_exact_argv(self) -> None:
        cmake = os.environ.get("SB_TEST_CMAKE_COMMAND") or shutil.which("cmake")
        self.assertIsNotNone(cmake, "CMake is required; this check must not be skipped")
        expected = self.forward(
            "CMAKE_PREFIX_PATH:STRING=/first root;/second=root\n"
            "CMAKE_LIBRARY_PATH:STRING=/lib one;/lib two\n"
            "CMAKE_INCLUDE_PATH:STRING=/include one;/include two\n"
            "SBL_NUMERIC_MPFR_INCLUDE_DIR:PATH=/mpfr include\n"
            "SBL_NUMERIC_MPFR_LIBRARY:FILEPATH=C:\\SDK space\\mpfr.lib\n"
            "SBL_NUMERIC_GMP_INCLUDE_DIR:PATH=/gmp include\n"
            "SBL_NUMERIC_GMP_LIBRARY:FILEPATH=/lib/gmp.so\n"
            "SBL_NUMERIC_BOOST_INCLUDE_DIR:PATH=/boost include\n"
        )
        helper = TOOLS.parents[1] / "cmake" / "NestedDependencyConfigureArgs.cmake"
        script = Path(self.owned.name) / "argv.cmake"
        script.write_text(
            f"include([==[{helper.as_posix()}]==])\n"
            "set(forwarded --existing)\n"
            "sb_append_nested_dependency_configure_args(forwarded)\n"
            f"execute_process(COMMAND [==[{Path(sys.executable).as_posix()}]==] "
            "-c [==[import json,sys; print(json.dumps(sys.argv[1:]))]==] "
            "${forwarded} RESULT_VARIABLE result)\n"
            "if(NOT result EQUAL 0)\nmessage(FATAL_ERROR \"argv child failed\")\nendif()\n",
            encoding="utf-8")
        result = subprocess.run(
            [cmake, *expected, "-DSBL_NUMERIC_REFERENCE_TLS:INTERNAL=1",
             "-DSB_BUILD_TESTS:BOOL=OFF", "-DCMAKE_CXX_FLAGS:STRING=-DSUPPRESS_CHECKS=1",
             "-P", str(script)], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout), ["--existing", *expected])


if __name__ == "__main__":
    unittest.main()
