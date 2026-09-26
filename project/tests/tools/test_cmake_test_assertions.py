# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Compile and execute real CMake assertion policy, including its negative control."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "cmake/TestAssertions.cmake"
ENROLLMENT = 'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestAssertions.cmake")'


class TestAssertionPolicy(unittest.TestCase):
    def run_command(self, command):
        return subprocess.run(command, capture_output=True, text=True, timeout=120)

    def require_success(self, command):
        result = self.run_command(command)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_release_assertions_and_production_isolation(self):
        cmake = shutil.which("cmake")
        ctest = shutil.which("ctest")
        self.assertIsNotNone(cmake)
        self.assertIsNotNone(ctest)
        root_source = (ROOT / "CMakeLists.txt").read_text()
        self.assertEqual(root_source.count(ENROLLMENT), 1)
        enrollment = ENROLLMENT + root_source.split(ENROLLMENT, 1)[1]
        with tempfile.TemporaryDirectory(prefix="sbassert-") as tmp:
            root = Path(tmp)
            (root / "tests/nested").mkdir(parents=True)
            (root / "tests_extra").mkdir()
            (root / "cmake").mkdir()
            shutil.copyfile(POLICY, root / "cmake/TestAssertions.cmake")
            (root / "tests/nested/check.cpp").write_text('''
#include <cassert>
int check() { int evaluated = 0; assert(++evaluated == 1); return evaluated != 1; }
''')
            (root / "tests/main.cpp").write_text('''
#include <cassert>
int check();
int main() { int evaluated = 0; assert(++evaluated == 1); return evaluated != 1 || check(); }
''')
            # A sibling whose name starts with "tests" is still production.
            (root / "tests_extra/production.cpp").write_text('''
#ifndef NDEBUG
#error Test assertion policy must not alter production Release flags
#endif
int main() { return 0; }
''')
            (root / "tests/nested/CMakeLists.txt").write_text('''
add_library(test_support STATIC check.cpp)
add_library(test_headers INTERFACE)
target_sources(test_headers INTERFACE check.cpp)
add_custom_target(test_source_inventory SOURCES check.cpp)
function(register_from_parent)
  add_executable(function_registered tests/main.cpp)
  target_link_libraries(function_registered PRIVATE test_support)
  add_test(NAME function_registered COMMAND function_registered)
endfunction()
''')
            (root / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.25)
project(AssertionPolicy CXX)
enable_testing()
add_subdirectory(tests/nested)
cmake_language(DEFER CALL register_from_parent)
add_executable(root_registered tests/main.cpp)
target_link_libraries(root_registered PRIVATE test_support)
add_test(NAME root_registered COMMAND root_registered)
add_executable(production tests_extra/production.cpp)
add_test(NAME production COMMAND production)
if(ENFORCE_ASSERTIONS)
''' + enrollment + '''
endif()
''')
            for profile, enabled in [("Release", False), ("Release", True),
                                     ("RelWithDebInfo", True), ("MinSizeRel", True)]:
                with self.subTest(profile=profile, enabled=enabled):
                    build = root / f"build-{profile}-{enabled}"
                    self.require_success([cmake, "-S", str(root), "-B", str(build),
                                          "-DCMAKE_BUILD_TYPE=" + profile,
                                          "-DENFORCE_ASSERTIONS=" + ("ON" if enabled else "OFF")])
                    self.require_success([cmake, "--build", str(build), "--config", profile,
                                          "--parallel", "2"])
                    result = self.run_command([ctest, "--test-dir", str(build), "-C", profile,
                                               "--output-on-failure"])
                    output = result.stdout + result.stderr
                    if enabled:
                        self.assertEqual(result.returncode, 0, output)
                        self.assertIn("100% tests passed", output)
                    else:
                        self.assertNotEqual(result.returncode, 0, output)
                        self.assertIn("2 tests failed out of 3", output)


if __name__ == "__main__":
    unittest.main()
