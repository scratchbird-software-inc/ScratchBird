#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Stage a bounded configured-build handoff for split CI signal jobs.

The public release build tree contains every test executable and all compiler
objects, so passing the tree wholesale between GitHub Actions jobs is both
unnecessarily large and operationally fragile.  This tool derives the release
test payload from CTest's configured inventory and stages only:

* CTest and install metadata;
* files named by selected test commands;
* runtime libraries, configuration, and resources;
* the allowlisted native executables needed by package verification.

The staged tree retains its repository-relative path.  Archiving the contents
of ``--output-root`` and extracting at the repository root therefore restores
the configured test directory at exactly the path embedded by CMake/CTest.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Iterable


NATIVE_EXECUTABLES = (
    "SBsrv",
    "SBgate",
    "SBmgr",
    "SBParser",
    "SBsql",
    "SBadm",
    "SBbak",
    "SBsec",
    "SBdoc",
    "SBcop",
)

PLATFORM_EXECUTABLE_SUFFIX = {
    "linux": "",
    "macos": "",
    "windows": ".exe",
}

PLATFORM_RUNTIME_LIBRARY_SUFFIXES = {
    "linux": (".so",),
    "macos": (".dylib", ".so"),
    "windows": (".dll",),
}

METADATA_NAMES = frozenset(
    {
        "CMakeCache.txt",
        "CTestCustom.cmake",
        "CTestTestfile.cmake",
        "DartConfiguration.tcl",
        "cmake_install.cmake",
    }
)


class HandoffError(RuntimeError):
    """The configured-build handoff could not be staged safely."""


def fail(message: str) -> None:
    raise HandoffError(message)


def compile_patterns(values: list[str], description: str) -> list[re.Pattern[str]]:
    patterns: list[re.Pattern[str]] = []
    for value in values:
        try:
            patterns.append(re.compile(value))
        except re.error as exc:
            fail(f"invalid_{description}:{value}:{exc}")
    return patterns


def matches(patterns: Iterable[re.Pattern[str]], values: Iterable[str]) -> bool:
    return any(pattern.search(value) for pattern in patterns for value in values)


def test_labels(test: dict[str, object]) -> tuple[str, ...]:
    for prop in test.get("properties", []):
        if not isinstance(prop, dict) or prop.get("name") != "LABELS":
            continue
        value = prop.get("value", [])
        if isinstance(value, list):
            return tuple(str(item) for item in value)
        if isinstance(value, str):
            return tuple(part for part in value.split(";") if part)
    return ()


def load_inventory(test_dir: Path) -> list[dict[str, object]]:
    completed = subprocess.run(
        ["ctest", "--test-dir", str(test_dir), "-N", "--show-only=json-v1"],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout)
        fail(f"ctest_inventory_failed:{completed.returncode}")
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        fail(f"ctest_inventory_invalid_json:{exc}")
    tests = payload.get("tests", [])
    if not isinstance(tests, list) or not tests:
        fail("ctest_inventory_empty")
    return [test for test in tests if isinstance(test, dict)]


def repository_relative(path: Path, repository_root: Path, context: str) -> Path:
    try:
        relative = path.relative_to(repository_root)
    except ValueError:
        fail(f"path_outside_repository:{context}:{path}")
    if not relative.parts or any(part in {"", ".", ".."} for part in relative.parts):
        fail(f"unsafe_repository_relative_path:{context}:{relative}")
    return relative


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists() or destination.is_symlink():
        return
    if source.is_symlink():
        target = os.readlink(source)
        if os.path.isabs(target):
            fail(f"absolute_symlink_forbidden:{source}:{target}")
        destination.symlink_to(target)
        return
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def copy_tree(source: Path, destination: Path) -> None:
    if not source.is_dir() or source.is_symlink():
        fail(f"runtime_tree_missing_or_unsafe:{source}")
    for candidate in sorted(source.rglob("*")):
        relative = candidate.relative_to(source)
        target = destination / relative
        if candidate.is_dir() and not candidate.is_symlink():
            target.mkdir(parents=True, exist_ok=True)
        elif candidate.is_file() or candidate.is_symlink():
            copy_file(candidate, target)


def command_paths(
    tests: Iterable[dict[str, object]], test_dir: Path
) -> tuple[set[Path], set[Path]]:
    files: set[Path] = set()
    directories: set[Path] = set()
    for test in tests:
        command = test.get("command", [])
        if isinstance(command, list):
            for raw in command:
                if not isinstance(raw, str):
                    continue
                candidate = Path(raw)
                if not candidate.is_absolute():
                    continue
                try:
                    relative = candidate.relative_to(test_dir)
                except ValueError:
                    continue
                if candidate == test_dir:
                    continue
                if candidate.is_file() or candidate.is_symlink():
                    files.add(relative)
                elif candidate.is_dir():
                    directories.add(relative)
        for prop in test.get("properties", []):
            if not isinstance(prop, dict) or prop.get("name") != "WORKING_DIRECTORY":
                continue
            raw = prop.get("value")
            if not isinstance(raw, str):
                continue
            candidate = Path(raw)
            try:
                relative = candidate.relative_to(test_dir)
            except ValueError:
                continue
            if candidate != test_dir:
                directories.add(relative)
    return files, directories


def native_binary_names(platform: str) -> tuple[str, ...]:
    suffix = PLATFORM_EXECUTABLE_SUFFIX[platform]
    names = [f"{name}{suffix}" for name in NATIVE_EXECUTABLES]
    if platform == "macos":
        names.append("SBlaunch")
    return tuple(names)


def stage(args: argparse.Namespace) -> dict[str, object]:
    repository_root = Path.cwd().resolve()
    test_dir = args.test_dir.resolve()
    if not test_dir.is_dir():
        fail(f"configured_test_directory_missing:{test_dir}")
    test_dir_relative = repository_relative(test_dir, repository_root, "test_dir")
    output_root = args.output_root.resolve()
    if output_root.exists() or output_root.is_symlink():
        fail(f"output_root_already_exists:{output_root}")
    output_root.mkdir(parents=True)
    staged_test_dir = output_root / test_dir_relative

    include_patterns = compile_patterns(args.include_label, "include_label")
    exclude_patterns = compile_patterns(args.exclude_label, "exclude_label")
    if not include_patterns:
        fail("include_label_required")
    inventory = load_inventory(test_dir)
    selected = []
    for test in inventory:
        labels = test_labels(test)
        if not matches(include_patterns, labels):
            continue
        if matches(exclude_patterns, labels):
            continue
        selected.append(test)
    if not selected:
        fail("selected_test_inventory_empty")

    selected_files, selected_directories = command_paths(selected, test_dir)
    for candidate in test_dir.rglob("*"):
        if candidate.is_file() and candidate.name in METADATA_NAMES:
            selected_files.add(candidate.relative_to(test_dir))
    for candidate in test_dir.iterdir():
        if candidate.is_file() and candidate.suffix.lower() in {
            ".cmake",
            ".json",
            ".tsv",
            ".txt",
        }:
            selected_files.add(candidate.relative_to(test_dir))
    generated_root = test_dir / "generated"
    if generated_root.is_dir():
        copy_tree(generated_root, staged_test_dir / "generated")
    # Release metadata gates read the compiler identity from CMake's platform
    # files (the cache does not reliably contain internal compiler keys).
    cmake_files_root = test_dir / "CMakeFiles"
    if cmake_files_root.is_dir():
        copy_tree(cmake_files_root, staged_test_dir / "CMakeFiles")
    # The public install-consumer gate deliberately rebuilds the public engine
    # target before installing it.  Preserve the production object graph while
    # continuing to omit the far larger collection of test object trees.
    for production_build_root in ("src", "libraries"):
        candidate = test_dir / production_build_root
        if candidate.is_dir():
            copy_tree(candidate, staged_test_dir / production_build_root)
    # A small number of install-consumer gates intentionally exercise the
    # configured build tool through ``cmake --build`` before installing.
    for build_control in ("Makefile", "build.ninja", ".ninja_deps", ".ninja_log"):
        candidate = test_dir / build_control
        if candidate.is_file():
            selected_files.add(candidate.relative_to(test_dir))

    platform_root = test_dir / "output" / args.platform
    if not platform_root.is_dir():
        fail(f"platform_output_missing:{platform_root}")
    for directory_name in ("etc", "lib", "share"):
        source = platform_root / directory_name
        if source.is_dir():
            copy_tree(source, staged_test_dir / "output" / args.platform / directory_name)
    manifest = platform_root / "STANDALONE_OUTPUT_MANIFEST.json"
    if manifest.is_file():
        selected_files.add(manifest.relative_to(test_dir))

    bin_root = platform_root / "bin"
    for name in native_binary_names(args.platform):
        candidate = bin_root / name
        if not candidate.is_file():
            fail(f"native_binary_missing:{candidate}")
        selected_files.add(candidate.relative_to(test_dir))
    suffixes = PLATFORM_RUNTIME_LIBRARY_SUFFIXES[args.platform]
    for candidate in bin_root.iterdir():
        lower_name = candidate.name.lower()
        if candidate.is_file() and any(suffix in lower_name for suffix in suffixes):
            selected_files.add(candidate.relative_to(test_dir))

    for relative in sorted(selected_directories):
        (staged_test_dir / relative).mkdir(parents=True, exist_ok=True)
    for relative in sorted(selected_files):
        source = test_dir / relative
        if not source.is_file() and not source.is_symlink():
            fail(f"selected_handoff_file_missing:{relative}")
        copy_file(source, staged_test_dir / relative)

    staged_files = sorted(
        candidate.relative_to(output_root).as_posix()
        for candidate in output_root.rglob("*")
        if candidate.is_file() or candidate.is_symlink()
    )
    payload = {
        "schema_id": "scratchbird.ci_configured_build_handoff.v1",
        "platform": args.platform,
        "configured_test_directory": test_dir_relative.as_posix(),
        "inventory_test_count": len(inventory),
        "selected_test_count": len(selected),
        "include_label": args.include_label,
        "exclude_label": args.exclude_label,
        "file_count": len(staged_files),
        "files": staged_files,
    }
    (output_root / "CI_BUILD_HANDOFF_MANIFEST.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=sorted(PLATFORM_EXECUTABLE_SUFFIX), required=True)
    parser.add_argument("--include-label", action="append", default=[])
    parser.add_argument("--exclude-label", action="append", default=[])
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        payload = stage(args)
    except HandoffError as exc:
        print(f"stage_ci_build_handoff=fail:{exc}", file=sys.stderr)
        return 1
    print(
        "stage_ci_build_handoff=passed "
        f"platform={payload['platform']} tests={payload['selected_test_count']} "
        f"files={payload['file_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
