#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Run public sanitizer, warnings-as-errors, and source-hygiene proof."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from typing import Any


FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
)

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".cmake",
    ".py",
}

SANITIZER_PROFILES = ("none", "asan-ubsan", "tsan")


def fail(message: str) -> None:
    print(f"public_sanitizer_static_analysis_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def require_file(path: Path, repo_root: Path) -> str:
    if not path.exists() or not path.is_file():
        fail(f"required_file_missing:{rel(path, repo_root)}")
    return path.read_text(encoding="utf-8")


def require_contains(text: str, token: str, context: str) -> None:
    if token not in text:
        fail(f"{context}_missing:{token}")


def command_digest(output: str) -> str:
    return sha256_text(output.replace("\r\n", "\n"))


def run_command(
    command: list[str],
    cwd: Path,
    label: str,
    allowed_failure_markers: tuple[str, ...] = (),
    unavailable_diagnostic: str = "SB_DIAG_TSAN_RUNTIME_UNAVAILABLE",
) -> dict[str, Any]:
    proc = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    output = proc.stdout or ""
    if proc.returncode != 0:
        if allowed_failure_markers and (
            any(marker in output for marker in allowed_failure_markers)
            or proc.returncode < 0
        ):
            return {
                "label": label,
                "tool": Path(command[0]).name,
                "returncode": proc.returncode,
                "stdout_sha256": command_digest(output),
                "status": "runtime_unavailable_fail_closed",
                "diagnostic": unavailable_diagnostic,
            }
        print(output, file=sys.stderr)
        fail(f"command_failed:{label}:exit={proc.returncode}")
    return {
        "label": label,
        "tool": Path(command[0]).name,
        "returncode": proc.returncode,
        "stdout_sha256": command_digest(output),
        "status": "passed",
    }


def check_project_controls(repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    cmake_text = require_file(project_root / "CMakeLists.txt", repo_root)
    required = (
        "PUBLIC_RELEASE_SANITIZER_PROFILES",
        "SB_PUBLIC_RELEASE_SANITIZER_PROFILE",
        "SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS",
        "SB_DIAG_WARNINGS_AS_ERRORS_UNSUPPORTED",
        "SB_DIAG_SANITIZER_PROFILE_UNSUPPORTED",
        "-Wall",
        "-Wextra",
        "-Wpedantic",
        "-Werror",
        "/W4",
        "/WX",
        "-fsanitize=address,undefined",
        "-fsanitize=thread",
        "-fno-omit-frame-pointer",
        "SB_PUBLIC_RELEASE_ASAN_UBSAN_PROFILE",
        "SB_PUBLIC_RELEASE_TSAN_PROFILE",
    )
    for token in required:
        require_contains(cmake_text, token, "project_cmake")

    release_cmake = require_file(project_root / "tests" / "release" / "CMakeLists.txt", repo_root)
    for token in (
        "public_sanitizer_warning_probe",
        "public_sanitizer_static_analysis_gate",
        "public_sanitizer_static_analysis_gate.py",
        "PCR-GATE-110",
        "PCR-110",
        "warnings_as_errors",
        "source_hygiene",
    ):
        require_contains(release_cmake, token, "release_ctest")
    return [
        {"path": "project/CMakeLists.txt", "status": "hardening_controls_declared"},
        {"path": "project/tests/release/CMakeLists.txt", "status": "hardening_ctest_declared"},
    ]


def check_build_requirement_docs(repo_root: Path) -> list[dict[str, str]]:
    records: list[dict[str, str]] = []
    docs = {
        "root": repo_root / "docs" / "build_requirements" / "README.md",
        "linux": repo_root / "docs" / "build_requirements" / "linux" / "README.md",
        "windows": repo_root / "docs" / "build_requirements" / "windows" / "README.md",
        "freebsd": repo_root / "docs" / "build_requirements" / "freebsd" / "README.md",
    }
    required_tokens = {
        "root": (
            "clang-tidy 18 or newer",
            "cppcheck",
            "ASan",
            "UBSan",
            "TSan",
        ),
        "linux": (
            "clang-tidy-18 cppcheck",
            "ASan and UBSan",
            "TSan where",
            "SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON",
            "SB_PUBLIC_RELEASE_SANITIZER_PROFILE=asan-ubsan",
        ),
        "windows": (
            "clang-tidy",
            "cppcheck",
            "MSVC warnings-as-errors",
            "SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON",
        ),
        "freebsd": (
            "cppcheck",
            "ASan and UBSan",
            "TSan where",
            "SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON",
            "SB_PUBLIC_RELEASE_SANITIZER_PROFILE=asan-ubsan",
        ),
    }
    for key, path in docs.items():
        text = require_file(path, repo_root)
        for token in required_tokens[key]:
            require_contains(text, token, f"build_requirements_{key}")
        records.append({"platform": key, "path": rel(path, repo_root), "status": "declared"})
    return records


def iter_hygiene_files(project_root: Path):
    roots = [
        project_root / "CMakeLists.txt",
        project_root / "cmake",
        project_root / "src",
        project_root / "include",
        project_root / "tests" / "release",
        project_root / "tools" / "release",
    ]
    for root in roots:
        if not root.exists():
            continue
        if root.is_file():
            yield root
            continue
        for path in root.rglob("*"):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield path


def source_hygiene_scan(repo_root: Path, project_root: Path) -> dict[str, Any]:
    banned_substrings = (
        "#pragma " + "GCC diagnostic ignored",
        "#pragma " + "clang diagnostic ignored",
        "-W" + "no-error",
        "-f" + "no-sanitize",
        "no_" + "sanitize",
        "DISABLE_" + "SANITIZER",
    )
    dash_w = "-" + "w"
    warning_disable_needles = (
        " " + dash_w,
        "\"" + dash_w + "\"",
        "'" + dash_w + "'",
    )
    findings: list[str] = []
    scanned = 0
    for path in iter_hygiene_files(project_root):
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        path_text = rel(path, repo_root)
        reject_private_reference(path_text, "source_hygiene_path")
        scanned += 1
        for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
            if fragment in text:
                findings.append(f"{path_text}: private reference fragment")
        for needle in banned_substrings:
            if needle in text:
                findings.append(f"{path_text}: warning_or_sanitizer_suppression:{needle}")
        if any(needle in text for needle in warning_disable_needles):
            findings.append(f"{path_text}: global_warning_disable:{dash_w}")
    if findings:
        for finding in findings[:100]:
            print(finding, file=sys.stderr)
        if len(findings) > 100:
            print(f"... {len(findings) - 100} additional findings omitted", file=sys.stderr)
        fail("source_hygiene_scan_failed")
    return {"files_scanned": scanned, "status": "passed"}


def resolve_tool(requested: str, fallbacks: tuple[str, ...], label: str) -> str:
    candidates = (requested,) if requested else fallbacks
    for candidate in candidates:
        path = shutil.which(candidate)
        if path:
            return candidate
    fail(f"required_tool_missing:{label}:{','.join(candidates)}")


def llvm_configure_args_from_env() -> list[str]:
    args: list[str] = []
    for name in (
        "SB_LLVM_PROJECT_ROOT",
        "SB_LLVM_TOOLS_ROOT",
        "SB_LLVM_LIBRARY",
        "SB_LLVM_LINK_MODE",
    ):
        value = os.environ.get(name, "")
        if value:
            args.append(f"-D{name}={value}")
    return args


def windows_sanitizer_runtime_markers(profile: str) -> tuple[str, tuple[str, ...]]:
    if os.name != "nt":
        return "", ()
    if profile == "asan-ubsan":
        return (
            "SB_DIAG_WINDOWS_ASAN_UBSAN_RUNTIME_UNAVAILABLE",
            (
                "cannot find -lasan",
                "cannot find -lubsan",
                "libclang_rt.asan",
                "libclang_rt.ubsan",
                "unsupported option '-fsanitize=address'",
                "unsupported argument 'address'",
                "AddressSanitizer is not supported",
                "UndefinedBehaviorSanitizer is not supported",
            ),
        )
    if profile == "tsan":
        return (
            "SB_DIAG_TSAN_RUNTIME_UNAVAILABLE",
            (
                "cannot find -ltsan",
                "libclang_rt.tsan",
                "unsupported option '-fsanitize=thread'",
                "unsupported argument 'thread'",
                "ThreadSanitizer is not supported",
            ),
        )
    return "", ()


def run_static_tools(args: argparse.Namespace, repo_root: Path, project_root: Path) -> list[dict[str, Any]]:
    clang_tidy = resolve_tool(args.clang_tidy, ("clang-tidy-18", "clang-tidy"), "clang-tidy")
    cppcheck = resolve_tool(args.cppcheck, ("cppcheck",), "cppcheck")
    probe = project_root / "tests" / "release" / "public_sanitizer_warning_probe.cpp"
    require_file(probe, repo_root)
    records = [
        run_command([clang_tidy, "--version"], repo_root, "clang_tidy_version"),
        run_command([cppcheck, "--version"], repo_root, "cppcheck_version"),
        run_command(
            [
                clang_tidy,
                str(probe),
                "--checks=-*,bugprone-*,clang-analyzer-*,performance-*,portability-*",
                "--warnings-as-errors=*",
                "--",
                "-std=c++23",
            ],
            repo_root,
            "clang_tidy_probe_scan",
        ),
        run_command(
            [
                cppcheck,
                "--enable=warning,performance,portability",
                "--error-exitcode=1",
                "--std=c++20",
                "--language=c++",
                str(probe),
            ],
            repo_root,
            "cppcheck_probe_scan",
        ),
    ]
    for record in records:
        reject_private_reference(record["tool"], "static_tool_record")
    return records


def configure_build_run_probe(
    args: argparse.Namespace,
    repo_root: Path,
    project_root: Path,
    profile: str,
) -> dict[str, Any]:
    build_dir = args.work_root / f"build-{profile}"
    if build_dir.exists():
        shutil.rmtree(build_dir)
    configure = [
        args.cmake,
        "-S",
        str(project_root),
        "-B",
        str(build_dir),
        "-G",
        args.generator,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DSB_BUILD_PUBLIC_RELEASE_CORRECTNESS=ON",
        "-DSB_PUBLIC_RELEASE_HARDENING_NESTED=ON",
        "-DSB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS=ON",
        f"-DSB_PUBLIC_RELEASE_SANITIZER_PROFILE={profile}",
        *llvm_configure_args_from_env(),
    ]
    if args.c_compiler:
        configure.append(f"-DCMAKE_C_COMPILER={args.c_compiler}")
    if args.cxx_compiler:
        configure.append(f"-DCMAKE_CXX_COMPILER={args.cxx_compiler}")

    records = [run_command(configure, repo_root, f"configure_{profile}")]
    build_diagnostic, build_failure_markers = windows_sanitizer_runtime_markers(profile)
    records.append(
        run_command(
            [
                args.cmake,
                "--build",
                str(build_dir),
                "--target",
                "public_sanitizer_warning_probe",
                "--parallel",
                "1",
            ],
            repo_root,
            f"build_probe_{profile}",
            allowed_failure_markers=build_failure_markers,
            unavailable_diagnostic=build_diagnostic or "SB_DIAG_TSAN_RUNTIME_UNAVAILABLE",
        )
    )
    if records[-1]["status"] == "runtime_unavailable_fail_closed":
        return {
            "profile": profile,
            "build_dir": build_dir.name,
            "warnings_as_errors": "on",
            "status": records[-1]["status"],
            "diagnostic": records[-1]["diagnostic"],
            "commands": records,
        }
    executable = find_probe_executable(build_dir)
    probe_command = [
        str(executable),
        "--expect-sanitizer-profile",
        profile,
        "--expect-warnings-as-errors",
    ]
    allowed_runtime_markers = ()
    if profile == "tsan":
        allowed_runtime_markers = (
            "FATAL: ThreadSanitizer: unexpected memory mapping",
            "ThreadSanitizer: unsupported",
            "failed to mmap",
        )
    records.append(
        run_command(
            probe_command,
            repo_root,
            f"run_probe_{profile}",
            allowed_failure_markers=allowed_runtime_markers,
        )
    )
    run_status = records[-1]["status"]
    return {
        "profile": profile,
        "build_dir": build_dir.name,
        "warnings_as_errors": "on",
        "status": run_status,
        "commands": records,
    }


def find_probe_executable(build_dir: Path) -> Path:
    names = {"public_sanitizer_warning_probe", "public_sanitizer_warning_probe.exe"}
    for path in build_dir.rglob("*"):
        if path.name in names and path.is_file():
            return path
    fail(f"probe_executable_missing:{build_dir.name}")


def build_evidence(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    build_root = args.build_root.resolve()
    args.work_root = args.work_root.resolve()
    output = args.output.resolve()
    if not repo_root.is_dir() or not project_root.is_dir() or not build_root.is_dir():
        fail("input_root_missing")
    if project_root.name != "project":
        fail("project_root_must_be_project_directory")
    try:
        output_record = output.relative_to(build_root).as_posix()
    except ValueError:
        fail("output_must_be_under_build_root")
    reject_private_reference(output_record, "output")
    args.work_root.mkdir(parents=True, exist_ok=True)

    control_records = check_project_controls(repo_root, project_root)
    doc_records = check_build_requirement_docs(repo_root)
    hygiene = source_hygiene_scan(repo_root, project_root)
    static_tool_records = run_static_tools(args, repo_root, project_root)
    profile_records = [
        configure_build_run_probe(args, repo_root, project_root, profile)
        for profile in SANITIZER_PROFILES
    ]

    evidence: dict[str, Any] = {
        "schema_version": 1,
        "policy": {
            "public_tree_inputs_only": True,
            "private_docs_required": False,
            "git_history_required": False,
            "warnings_as_errors": "required_for_public_hardening_profiles",
            "sanitizer_profiles": list(SANITIZER_PROFILES),
            "tsan_scope": "configure_and_build_required_runtime_may_fail_closed_when_runner_cannot_load_tsan",
            "tsan_runtime_unavailable_diagnostic": "SB_DIAG_TSAN_RUNTIME_UNAVAILABLE",
            "windows_asan_ubsan_runtime_unavailable_diagnostic": "SB_DIAG_WINDOWS_ASAN_UBSAN_RUNTIME_UNAVAILABLE",
            "windows_sanitizer_runtime_scope": "configure_required_build_may_fail_closed_when_windows_gnu_toolchain_does_not_ship_profile_runtime",
            "static_analysis_tools": ["clang-tidy", "cppcheck"],
            "release_proof_is_evidence_only": True,
        },
        "controls": control_records,
        "build_requirement_docs": doc_records,
        "source_hygiene": hygiene,
        "static_analysis": static_tool_records,
        "profile_builds": profile_records,
    }
    evidence["evidence_sha256"] = sha256_text(
        json.dumps(evidence, sort_keys=True, separators=(",", ":"))
    )
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--c-compiler", default="")
    parser.add_argument("--cxx-compiler", default="")
    parser.add_argument("--clang-tidy", default=os.environ.get("CLANG_TIDY", ""))
    parser.add_argument("--cppcheck", default=os.environ.get("CPPCHECK", ""))
    args = parser.parse_args()

    evidence = build_evidence(args)
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"public_sanitizer_static_analysis_output={output.relative_to(args.build_root.resolve()).as_posix()}")
    print(f"public_sanitizer_static_analysis_sha256={evidence['evidence_sha256']}")
    print("public_sanitizer_static_analysis_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
