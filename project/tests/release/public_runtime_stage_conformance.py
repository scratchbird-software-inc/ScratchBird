#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
"""Exercise actual CMake runtime staging without building unrelated products."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--module", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="sb-runtime-stage-") as directory:
        root = Path(directory)
        source = root / "source"
        build = root / "build"
        source.mkdir()
        resources = source / "resources"
        resources.mkdir()
        policy = resources / "policy.json"
        policy.write_text('{"generation":1}\n')
        templates = source / "config" / "templates"
        templates.mkdir(parents=True)
        for name in ("SBsrv.conf", "SBgate.conf", "SBmgr.conf", "SBParser.conf", "SBbootstrap.profile"):
            (templates / name).write_text("generation=1\n")
        (source / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.20)
project(runtime_stage_probe NONE)
include("${BRANDING_MODULE}")
set(SB_PUBLIC_ARTIFACT_ROOT "${CMAKE_BINARY_DIR}/artifact")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/product"
  COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/product")
add_custom_target(probe_product DEPENDS "${CMAKE_BINARY_DIR}/product")
add_custom_target(unrelated_product COMMAND "${CMAKE_COMMAND}" -E false)
sb_public_brand_target(probe_product probe Probe standalone_public)
sb_public_brand_target(unrelated_product unrelated Unrelated standalone_public)
sb_public_configure_output_stage("${CMAKE_SOURCE_DIR}" "${TEST_PYTHON}")
''')
        subprocess.run([args.cmake, "-S", str(source), "-B", str(build),
                        f"-DBRANDING_MODULE={args.module.resolve()}",
                        f"-DTEST_PYTHON={sys.executable}"], check=True)
        command = [args.cmake, "--build", str(build), "--target", "probe_product", "--parallel", "2"]
        subprocess.run(command, check=True)
        installed_policy = build / "artifact/share/scratchbird/resources/policy.json"
        installed_config = build / "artifact/etc/scratchbird/SBsrv.conf"
        assert installed_policy.read_bytes() == policy.read_bytes()
        product_time = (build / "product").stat().st_mtime_ns
        policy.write_text('{"generation":2,"openssl_budget_bytes":4194304}\n')
        (templates / "SBsrv.conf").write_text("generation=2\n")
        subprocess.run(command, check=True)
        assert installed_policy.read_bytes() == policy.read_bytes(), "stale targeted-build policy"
        assert installed_config.read_bytes() == (templates / "SBsrv.conf").read_bytes()
        assert (build / "product").stat().st_mtime_ns == product_time, "unnecessary product rebuild"
        assert not (build / "artifact/share/scratchbird/docs").exists(), "full output stage was invoked"
    assert not root.exists(), "temporary stage fixture was not removed"
    print("public_runtime_stage_conformance=PASS")


if __name__ == "__main__":
    main()
