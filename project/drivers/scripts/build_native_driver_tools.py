#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Stage runnable native-driver tools under the build output tree.

The conformance matrix needs one stable executable entry point per driver. Some
lanes produce native binaries, while VM or interpreter lanes are represented by
build-tree launcher executables. The launcher is still the contract boundary:
every lane is invoked with the same input/output/error/diagnostic arguments and
the server remains the final SBLR/UUID/MGA authority.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shlex
import shutil
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


MANIFEST_REL = Path("project/drivers/DriverPackageManifest.csv")
TOOL_MATRIX_REL = Path("project/tests/conformance/drivers/native_tool_matrix.json")
DEFAULT_MANIFEST_REL = Path("build/reports/driver_native_tool_binaries.json")
RELEASE_BUCKETS = {"release_candidate", "release_supported", "supported"}
CORE_DRIVER_ORDER = [
    "cpp",
    "dart",
    "dotnet",
    "elixir",
    "go",
    "jdbc",
    "mojo",
    "node",
    "odbc",
    "pascal",
    "php",
    "python",
    "r",
    "ruby",
    "rust",
    "swift",
]
NATIVE_BINARY_KINDS = {
    "cmake_native_binary",
    "go_native_binary",
    "rust_native_binary",
    "pascal_native_binary",
    "swift_native_binary",
    "dart_native_binary",
    "mojo_native_binary",
}


@dataclass(frozen=True)
class DriverTool:
    driver: str
    executable_name: str
    command: list[str]
    cwd: Path
    runtime_tools: list[str]
    kind: str


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[3]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def release_drivers(repo_root: Path) -> list[str]:
    rows = read_csv(repo_root / MANIFEST_REL)
    release_names = {
        str(row.get("name", "")).strip()
        for row in rows
        if row.get("category") == "driver"
        and row.get("release_bucket") in RELEASE_BUCKETS
        and row.get("driver_status") != "planned_not_implemented"
    }
    return [driver for driver in CORE_DRIVER_ORDER if driver in release_names]


def tool_matrix_drivers(repo_root: Path) -> set[str]:
    matrix = load_json(repo_root / TOOL_MATRIX_REL)
    drivers: set[str] = set()
    for item in matrix.get("driver_tools", []):
        if isinstance(item, dict) and item.get("driver"):
            drivers.add(str(item["driver"]))
    return drivers


def executable_name(driver: str) -> str:
    if driver == "r":
        return "sb_isql_r"
    return f"sb_isql_{driver}"


def platform_bin_root(build_root: Path) -> Path:
    return build_root / "output" / "linux" / "bin"


def command_for_driver(repo_root: Path, build_root: Path, driver: str) -> DriverTool:
    exe = executable_name(driver)
    if driver == "cpp":
        return DriverTool(
            driver,
            exe,
            [str(build_root / "drivers" / "driver" / "cpp" / "sb_isql_cpp")],
            repo_root,
            [],
            "cmake_native_binary",
        )
    if driver == "odbc":
        return DriverTool(
            driver,
            exe,
            [str(build_root / "drivers" / "driver" / "odbc" / "sb_isql_odbc")],
            repo_root,
            [],
            "cmake_native_binary",
        )
    if driver == "jdbc":
        cp = repo_root / "project" / "drivers" / "driver" / "jdbc" / "build" / "classes" / "java" / "main"
        return DriverTool(
            driver,
            exe,
            ["java", "-cp", str(cp), "com.scratchbird.jdbc.tools.SBIsqlJdbc"],
            repo_root,
            ["java"],
            "jvm_launcher",
        )
    if driver == "dotnet":
        dll = build_root / "drivers" / "driver" / "dotnet" / "publish" / "SBIsqlDotNet.dll"
        return DriverTool(driver, exe, ["dotnet", str(dll)], repo_root, ["dotnet"], "dotnet_launcher")
    if driver == "python":
        return DriverTool(
            driver,
            exe,
            [sys.executable, str(repo_root / "project" / "drivers" / "driver" / "python" / "tools" / "sb_isql_python.py")],
            repo_root,
            [Path(sys.executable).name],
            "python_launcher",
        )
    if driver == "go":
        return DriverTool(
            driver,
            exe,
            [str(build_root / "drivers" / "driver" / "go" / "bin" / "sb_isql_go")],
            repo_root,
            [],
            "go_native_binary",
        )
    if driver == "rust":
        return DriverTool(
            driver,
            exe,
            [str(build_root / "drivers" / "driver" / "rust" / "bin" / "sb_isql_rust")],
            repo_root,
            [],
            "rust_native_binary",
        )
    if driver == "node":
        return DriverTool(
            driver,
            exe,
            ["node", str(repo_root / "project" / "drivers" / "driver" / "node" / "dist" / "tools" / "sb-isql-node.js")],
            repo_root,
            ["node"],
            "node_launcher",
        )
    if driver == "php":
        return DriverTool(
            driver,
            exe,
            ["php", str(repo_root / "project" / "drivers" / "driver" / "php" / "tools" / "sb_isql_php.php")],
            repo_root,
            ["php"],
            "php_launcher",
        )
    if driver == "r":
        return DriverTool(
            driver,
            exe,
            ["Rscript", str(repo_root / "project" / "drivers" / "driver" / "r" / "tools" / "sb_isql_r.R")],
            repo_root,
            ["Rscript"],
            "r_launcher",
        )
    if driver == "ruby":
        return DriverTool(
            driver,
            exe,
            ["ruby", str(repo_root / "project" / "drivers" / "driver" / "ruby" / "tools" / "sb_isql_ruby.rb")],
            repo_root,
            ["ruby"],
            "ruby_launcher",
        )
    if driver == "pascal":
        return DriverTool(
            driver,
            exe,
            [str(build_root / "drivers" / "driver" / "pascal" / "bin" / "sb_isql_pascal")],
            repo_root,
            [],
            "pascal_native_binary",
        )
    if driver == "swift":
        return DriverTool(
            driver,
            exe,
            [str(build_root / "drivers" / "driver" / "swift" / ".build" / "release" / "SBIsqlSwift")],
            repo_root,
            [],
            "swift_native_binary",
        )
    if driver == "dart":
        return DriverTool(
            driver,
            exe,
            [str(build_root / "drivers" / "driver" / "dart" / "bin" / "sb_isql_dart")],
            repo_root,
            [],
            "dart_native_binary",
        )
    if driver == "elixir":
        return DriverTool(
            driver,
            exe,
            ["elixir", str(repo_root / "project" / "drivers" / "driver" / "elixir" / "tools" / "sb_isql_elixir.exs")],
            repo_root,
            ["elixir"],
            "elixir_launcher",
        )
    if driver == "mojo":
        return DriverTool(
            driver,
            exe,
            [str(build_root / "drivers" / "driver" / "mojo" / "bin" / "sb_isql_mojo")],
            repo_root,
            [],
            "mojo_native_binary",
        )
    raise KeyError(f"no driver tool contract for {driver}")


def quote_command(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def write_launcher(path: Path, tool: DriverTool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    body = [
        "#!/usr/bin/env bash",
        "set -euo pipefail",
        f"cd {shlex.quote(str(tool.cwd))}",
    ]
    if tool.kind == "python_launcher":
        python_src = tool.cwd / "project" / "drivers" / "driver" / "python" / "src"
        body.append(
            "export PYTHONPATH="
            + shlex.quote(str(python_src))
            + '${PYTHONPATH:+":$PYTHONPATH"}'
        )
    body.extend(
        [
            f"exec {quote_command(tool.command)} \"$@\"",
            "",
        ]
    )
    path.write_text("\n".join(body), encoding="utf-8")
    current = path.stat().st_mode
    path.chmod(current | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def is_executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def copy_if_executable(src: Path, dst: Path) -> bool:
    if not is_executable(src):
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.resolve() != dst.resolve():
        shutil.copy2(src, dst)
        current = dst.stat().st_mode
        dst.chmod(current | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return True


def run_command(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> dict[str, Any]:
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    return {
        "command": command,
        "cwd": str(cwd),
        "returncode": result.returncode,
        "output_tail": result.stdout.splitlines()[-80:],
    }


def build_compiled_tool(repo_root: Path, build_root: Path, driver: str) -> dict[str, Any] | None:
    driver_root = repo_root / "project" / "drivers" / "driver" / driver
    if driver in {"cpp", "odbc"} and shutil.which("cmake"):
        return run_command(["cmake", "--build", str(build_root), "--target", f"sb_isql_{driver}", "--parallel", "2"], repo_root)
    if driver == "go" and shutil.which("go"):
        out = build_root / "drivers" / "driver" / "go" / "bin" / "sb_isql_go"
        out.parent.mkdir(parents=True, exist_ok=True)
        return run_command(["go", "build", "-o", str(out), "./cmd/sb-isql-go"], driver_root)
    if driver == "rust" and shutil.which("cargo"):
        out_dir = build_root / "drivers" / "driver" / "rust"
        env = os.environ.copy()
        env["CARGO_TARGET_DIR"] = str(out_dir / "target")
        result = run_command(["cargo", "build", "--quiet", "--bin", "sb_isql_rust"], driver_root, env=env)
        produced = out_dir / "target" / "debug" / "sb_isql_rust"
        final = out_dir / "bin" / "sb_isql_rust"
        if result["returncode"] == 0 and produced.exists():
            final.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(produced, final)
            final.chmod(final.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        return result
    if driver == "pascal" and shutil.which("fpc"):
        out_dir = build_root / "drivers" / "driver" / "pascal" / "bin"
        unit_dir = build_root / "drivers" / "driver" / "pascal" / "units"
        out_dir.mkdir(parents=True, exist_ok=True)
        unit_dir.mkdir(parents=True, exist_ok=True)
        return run_command(
            [
                "fpc",
                f"-Fu{driver_root / 'src'}",
                f"-FE{out_dir}",
                f"-FU{unit_dir}",
                str(driver_root / "tools" / "sb_isql_pascal.pas"),
            ],
            repo_root,
        )
    if driver == "dart" and shutil.which("dart"):
        out = build_root / "drivers" / "driver" / "dart" / "bin" / "sb_isql_dart"
        out.parent.mkdir(parents=True, exist_ok=True)
        return run_command(["dart", "compile", "exe", "bin/sb_isql_dart.dart", "-o", str(out)], driver_root)
    if driver == "swift" and shutil.which("swift"):
        build_path = build_root / "drivers" / "driver" / "swift" / ".build"
        return run_command(["swift", "build", "-c", "release", "--build-path", str(build_path), "--product", "SBIsqlSwift"], driver_root)
    if driver == "dotnet" and shutil.which("dotnet"):
        out = build_root / "drivers" / "driver" / "dotnet" / "publish"
        out.mkdir(parents=True, exist_ok=True)
        return run_command(
            [
                "dotnet",
                "publish",
                str(driver_root / "tools" / "SBIsqlDotNet" / "SBIsqlDotNet.csproj"),
                "-c",
                "Release",
                "-o",
                str(out),
            ],
            repo_root,
        )
    if driver == "mojo" and shutil.which("mojo"):
        bridge_build = run_command(
            ["cmake", "--build", str(build_root), "--target", "scratchbird_mojo_client_bridge", "--parallel", "2"],
            repo_root,
        )
        if bridge_build["returncode"] != 0:
            return bridge_build
        bridge_dir = build_root / "output" / "linux" / "lib"
        bridge = bridge_dir / "libscratchbird_mojo_client_bridge.so"
        out = build_root / "drivers" / "driver" / "mojo" / "bin" / "sb_isql_mojo"
        out.parent.mkdir(parents=True, exist_ok=True)
        return run_command(
            [
                "mojo",
                "build",
                "-I",
                str(driver_root / "src"),
                "-I",
                str(driver_root / "src" / "scratchbird"),
                "-Xlinker",
                str(bridge),
                "-Xlinker",
                "-rpath",
                "-Xlinker",
                str(bridge_dir),
                "-o",
                str(out),
                str(driver_root / "tools" / "sb_isql_mojo.mojo"),
            ],
            repo_root,
        )
    if driver == "node" and shutil.which("npm"):
        return run_command(["npm", "run", "build"], driver_root)
    if driver == "jdbc" and (driver_root / "gradlew").exists():
        return run_command(["bash", str(driver_root / "gradlew"), "classes"], driver_root)
    return None


def stage_tool(
    repo_root: Path,
    build_root: Path,
    bin_root: Path,
    driver: str,
    *,
    build_compiled: bool,
    strict_runtimes: bool,
) -> dict[str, Any]:
    tool = command_for_driver(repo_root, build_root, driver)
    output_path = bin_root / tool.executable_name
    build_result = None
    if build_compiled:
        build_result = build_compiled_tool(repo_root, build_root, driver)

    copied_native_binary = False
    if tool.kind in NATIVE_BINARY_KINDS:
        copied = False
        first = Path(tool.command[0])
        if first.is_absolute():
            copied = copy_if_executable(first, output_path)
        if not copied:
            if tool.kind == "cmake_native_binary" and is_executable(output_path):
                copied = True
            else:
                write_launcher(output_path, tool)
        copied_native_binary = copied
    else:
        write_launcher(output_path, tool)

    missing_runtimes = [runtime for runtime in tool.runtime_tools if shutil.which(runtime) is None]
    status = "pass"
    if build_result is not None and build_result["returncode"] != 0:
        status = "fail"
    if build_compiled and tool.kind in NATIVE_BINARY_KINDS and not copied_native_binary:
        status = "fail"
    if strict_runtimes and missing_runtimes:
        status = "fail"

    return {
        "driver": driver,
        "status": status,
        "executable": str(output_path),
        "executable_name": tool.executable_name,
        "kind": tool.kind,
        "command": tool.command,
        "cwd": str(tool.cwd),
        "runtime_tools": tool.runtime_tools,
        "missing_runtime_tools": missing_runtimes,
        "copied_native_binary": copied_native_binary,
        "build_result": build_result,
        "build_compiled_requested": build_compiled,
        "strict_runtimes": strict_runtimes,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--build-root", type=Path, default=repo_root_from_script() / "build")
    parser.add_argument("--bin-root", type=Path)
    parser.add_argument("--manifest-out", type=Path)
    parser.add_argument("--driver", action="append")
    parser.add_argument("--build-compiled", action="store_true")
    parser.add_argument("--strict-runtimes", action="store_true")
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    build_root = args.build_root.resolve()
    bin_root = (args.bin_root or platform_bin_root(build_root)).resolve()
    manifest_out = (args.manifest_out or repo_root / DEFAULT_MANIFEST_REL).resolve()

    manifest_drivers = release_drivers(repo_root)
    matrix_drivers = tool_matrix_drivers(repo_root)
    drivers = [driver for driver in manifest_drivers if driver in matrix_drivers]
    if args.driver:
        requested = set(args.driver)
        unknown = sorted(requested - set(drivers))
        if unknown:
            raise SystemExit(f"unknown or non-core driver(s): {', '.join(unknown)}")
        drivers = [driver for driver in drivers if driver in requested]

    results = [
        stage_tool(
            repo_root,
            build_root,
            bin_root,
            driver,
            build_compiled=args.build_compiled,
            strict_runtimes=args.strict_runtimes,
        )
        for driver in drivers
    ]
    missing = sorted(set(CORE_DRIVER_ORDER) - {result["driver"] for result in results})
    failures = [result for result in results if result["status"] != "pass"]
    report = {
        "command": "build_native_driver_tools.py",
        "status": "pass" if not failures else "fail",
        "repo_root": str(repo_root),
        "build_root": str(build_root),
        "bin_root": str(bin_root),
        "driver_count": len(results),
        "missing_core_drivers": missing,
        "build_compiled": args.build_compiled,
        "strict_runtimes": args.strict_runtimes,
        "results": results,
    }
    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_out.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"driver_native_tool_binaries={report['status']} "
        f"drivers={len(results)} bin_root={bin_root}"
    )
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
