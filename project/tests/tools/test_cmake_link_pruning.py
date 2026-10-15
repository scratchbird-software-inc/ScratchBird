# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Real compiler/linker controls: pruning, constructors, exports and --wrap."""
from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "cmake/LinkPruning.cmake"


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux ELF link-profile controls")
class LinkPruningTest(unittest.TestCase):
    def run_ok(self, args, **kwargs):
        result = subprocess.run(args, capture_output=True, text=True, timeout=120, **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_native_linkage_and_negative_profiles(self):
        for tool in ("cmake", "ctest", "nm", "readelf"):
            self.assertIsNotNone(shutil.which(tool))
        with tempfile.TemporaryDirectory(prefix="sb-link-layout-") as temporary:
            root = Path(temporary)
            (root / "nested").mkdir()
            shutil.copyfile(POLICY, root / "LinkPruning.cmake")
            (root / "nested/impl.c").write_text('''
#include <unistd.h>
int live_c(void) { return 11; }
int wrapped_native_call(void) { return getpid(); }
int unreachable_c_entry(void) { return 7; }
''')
            (root / "nested/impl.cpp").write_text('''
extern "C" int live_c();
static int (*registered)() = nullptr;
static int callback() { return live_c() + 4; }
struct Register { Register() { registered = &callback; } };
static Register startup;
extern "C" int live_cpp() { return registered ? registered() : -1; }
extern "C" int unreachable_cpp_entry() { return 42; }
''')
            (root / "export.cpp").write_text('''
extern "C" int runtime_export() { return 31; }
''')
            (root / "c_main.c").write_text('''
int live_c(void);
int main(void) { return live_c() == 11 ? 0 : 1; }
''')
            (root / "main.cpp").write_text('''
#include <dlfcn.h>
extern "C" int live_cpp();
extern "C" int wrapped_native_call();
static int interceptions = 0;
extern "C" int __wrap_getpid() { ++interceptions; return 123; }
int main(int argc, char** argv) {
  if (argc != 2 || live_cpp() != 15 || wrapped_native_call() != 123 || interceptions != 1) return 1;
  void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!library) return 2;
  auto call = reinterpret_cast<int(*)()>(dlsym(library, "runtime_export"));
  const int result = call && call() == 31 ? 0 : 3;
  if (dlclose(library)) return 4;
  return result;
}
''')
            (root / "nested/CMakeLists.txt").write_text('''
add_library(implementation STATIC impl.c impl.cpp)
add_library(c_only STATIC impl.c)
add_library(header_only INTERFACE)
add_custom_target(source_inventory SOURCES impl.cpp)
''')
            (root / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.25)
project(LinkLayout C CXX)
enable_testing()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(nested)
add_library(exports SHARED export.cpp)
function(late_target)
  add_executable(probe main.cpp)
  target_link_libraries(probe PRIVATE implementation ${CMAKE_DL_LIBS})
  target_link_options(probe PRIVATE "LINKER:--wrap=getpid")
  add_test(NAME native COMMAND probe $<TARGET_FILE:exports>)
  add_executable(c_probe c_main.c)
  target_link_libraries(c_probe PRIVATE c_only)
  add_test(NAME native_c COMMAND c_probe)
endfunction()
cmake_language(DEFER CALL late_target)
include("${CMAKE_CURRENT_SOURCE_DIR}/LinkPruning.cmake")
cmake_language(DEFER CALL sb_enable_link_pruning "${CMAKE_CURRENT_SOURCE_DIR}")
# Repeated enrollment must not duplicate options.
cmake_language(DEFER CALL sb_enable_link_pruning "${CMAKE_CURRENT_SOURCE_DIR}")
''')
            sizes = {}
            for profile, enabled in (("Release", True), ("RelWithDebInfo", True),
                                     ("MinSizeRel", True), ("Debug", True),
                                     ("Release", False), ("Release", None)):
                with self.subTest(profile=profile, enabled=enabled):
                    build = root / (profile + str(enabled))
                    configure = ["cmake", "-S", str(root), "-B", str(build),
                                 "-DCMAKE_BUILD_TYPE=" + profile,
                                 "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
                    if enabled is not None:
                        configure.append("-DSB_ENABLE_LINK_PRUNING=" + ("ON" if enabled else "OFF"))
                    self.run_ok(configure)
                    self.run_ok(["cmake", "--build", str(build), "--parallel", "2"])
                    self.run_ok(["ctest", "--test-dir", str(build), "--no-tests=error",
                                 "--output-on-failure"])
                    # Fresh processes must independently run the real initializer
                    # and interposition. No persistent test-worker/shared state.
                    self.run_ok([str(build / "probe"), str(build / "libexports.so")])
                    active = enabled is not False and profile != "Debug"
                    symbols = self.run_ok(["nm", str(build / "probe")])
                    for symbol in ("unreachable_c_entry", "unreachable_cpp_entry"):
                        self.assertEqual(symbol in symbols, not active)
                    c_symbols = self.run_ok(["nm", str(build / "c_probe")])
                    self.assertEqual("unreachable_c_entry" in c_symbols, not active)
                    exports = self.run_ok(["nm", "-D", str(build / "libexports.so")])
                    self.assertIn("runtime_export", exports)
                    commands = json.loads((build / "compile_commands.json").read_text())
                    for item in commands:
                        for flag in ("-ffunction-sections", "-fdata-sections"):
                            expected = int(active and Path(item["file"]).name not in ("main.cpp", "c_main.c"))
                            self.assertEqual(item["command"].split().count(flag), expected)
                    sections = self.run_ok(["readelf", "-SW", str(build / "probe")])
                    self.assertIn(".symtab", sections)  # Diagnosis is not stripped away.
                    sizes[profile, enabled] = (build / "probe").stat().st_size
            self.assertLess(sizes["Release", True], sizes["Release", False])


if __name__ == "__main__":
    unittest.main()
