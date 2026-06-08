#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate cross-platform listener owner/lifecycle artifact source contracts, including Windows x64."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    print(f"engine_listener_owner_lifecycle_platform_source_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        fail(f"read_failed:{path}:{exc}")


def compile_windows_object(compiler: str, project_root: Path, source: Path, output: Path) -> None:
    command = [
        compiler,
        "-std=c++23",
        "-DWIN32_LEAN_AND_MEAN",
        f"-I{project_root / 'src/listener'}",
        f"-I{project_root / 'src/manager/protocol'}",
        f"-I{project_root / 'src'}",
        "-c",
        str(source),
        "-o",
        str(output),
    ]
    result = subprocess.run(command, cwd=project_root.parent, text=True, capture_output=True)
    if result.returncode != 0:
        fail(
            "windows_object_compile_failed:"
            + source.name
            + ":"
            + (result.stderr.strip() or result.stdout.strip())
        )


def compile_windows_x64_probe(compiler: str, source: Path, output: Path) -> None:
    source.write_text(
        """#ifndef _WIN64
#error "ScratchBird Windows release support is x64-only"
#endif
#include <cstddef>
static_assert(sizeof(void*) == 8, "ScratchBird Windows release support requires 64-bit pointers");
int scratchbird_windows_x64_probe = 0;
""",
        encoding="utf-8",
    )
    command = [
        compiler,
        "-std=c++23",
        "-DWIN32_LEAN_AND_MEAN",
        "-DNOMINMAX",
        "-c",
        str(source),
        "-o",
        str(output),
    ]
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode != 0:
        fail("windows_x64_probe_compile_failed:" + (result.stderr.strip() or result.stdout.strip()))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    source = project_root / "src/listener/listener_socket_identity.cpp"
    text = read(source)

    for token in (
        "::flock(fd, LOCK_EX | LOCK_NB)",
        "::fsync(fd)",
        "::fsync(dir_fd)",
        "::kill(static_cast<pid_t>(pid), 0)",
        "clean_shutdown_required=true",
        "tamper_evidence=signature_sha256_128",
    ):
        require(token in text, f"posix_owner_lifecycle_token_missing:{token}")

    for token in (
        "::CreateFileA(lock_path.string().c_str()",
        "::LockFileEx(handle",
        "::UnlockFileEx(handle_",
        "const BOOL flushed = ::FlushFileBuffers(file);",
        "const BOOL closed = ::CloseHandle(file);",
        "::FlushFileBuffers(dir)",
        "::MoveFileExA(tmp.c_str()",
        "MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH",
        "::GetCurrentProcessId()",
        "::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION",
        "::WaitForSingleObject(process, 0)",
        "ERROR_ACCESS_DENIED",
    ):
        require(token in text, f"windows_owner_lifecycle_token_missing:{token}")

    for forbidden in (
        "lock.fd_ = 0",
        "<< \"pid=0\\n\"",
        "(void)identity;\n  (void)owner_pid;",
        "(void)path;\n  (void)error_message;",
    ):
        require(forbidden not in text, f"windows_owner_lifecycle_stub_present:{forbidden}")

    compiler = shutil.which("x86_64-w64-mingw32-g++")
    require(compiler is not None, "mingw_windows_x64_cross_compiler_missing")
    with tempfile.TemporaryDirectory(prefix="sb_listener_owner_lifecycle_windows_") as tmp:
        tmp_path = Path(tmp)
        compile_windows_x64_probe(compiler, tmp_path / "windows_x64_probe.cpp", tmp_path / "windows_x64_probe.o")
        compile_windows_object(compiler, project_root, source, tmp_path / "listener_socket_identity.o")

    print("engine_listener_owner_lifecycle_platform_source_gate=passed")


if __name__ == "__main__":
    main()
