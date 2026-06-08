#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate cross-platform FileDevice native I/O source contracts, including Windows x64."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    print(f"engine_listener_storage_io_platform_source_gate=fail:{message}", file=sys.stderr)
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
        "-DNOMINMAX",
        f"-I{project_root / 'src/storage/disk'}",
        f"-I{project_root / 'src/storage/database'}",
        f"-I{project_root / 'src/storage/filespace'}",
        f"-I{project_root / 'src/storage/page'}",
        f"-I{project_root / 'src/core/platform'}",
        f"-I{project_root / 'src/core/uuid'}",
        f"-I{project_root / 'src/core/memory'}",
        f"-I{project_root / 'src/core/metrics'}",
        f"-I{project_root / 'src/core/agents'}",
        f"-I{project_root / 'src/core/resources'}",
        f"-I{project_root / 'src/core/catalog'}",
        f"-I{project_root / 'src/core/datatypes'}",
        f"-I{project_root / 'src/transaction/mga'}",
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
    source = project_root / "src/storage/disk/disk_device.cpp"
    write_path_source = project_root / "src/storage/database/write_path_batching.cpp"
    header = project_root / "src/storage/disk/disk_device.hpp"
    source_text = read(source)
    write_path_text = read(write_path_source)
    header_text = read(header)

    for token in (
        "void* file_handle_ = nullptr;",
        "int file_fd_ = -1;",
    ):
        require(token in header_text, f"native_handle_member_missing:{token}")
    require("<fstream>" not in header_text, "fstream_member_header_dependency_present")

    for token in (
        "::pread(fd",
        "::pwrite(fd",
        "::fsync(fd)",
        "::fstat(fd, &st)",
        "::open(path.c_str(), flags, 0600)",
        "::flock(prospective_owner_lock_fd, lock_mode)",
        "O_CLOEXEC",
        "O_DIRECTORY",
    ):
        require(token in source_text, f"posix_native_io_token_missing:{token}")

    for token in (
        "::CreateFileA(path.c_str()",
        "::ReadFile(handle",
        "::WriteFile(handle",
        "OVERLAPPED overlapped = OffsetToOverlapped",
        "::FlushFileBuffers(handle)",
        "::GetFileSizeEx",
        "::CloseHandle(static_cast<HANDLE>(file_handle_))",
    ):
        require(token in source_text, f"windows_native_io_token_missing:{token}")

    for token in (
        "CheckFileDeviceExtent(offset, bytes)",
        "SB-STORAGE-DISK-CLOSE-SYNC-FAILED",
        "DurableSyncHandle(static_cast<HANDLE>(file_handle_)",
        "DurableSyncFd(file_fd_",
        "NativeReadAt(file_handle_",
        "NativeWriteAt(file_handle_",
        "NativeReadAt(file_fd_",
        "NativeWriteAt(file_fd_",
    ):
        require(token in source_text, f"native_file_device_contract_missing:{token}")

    for forbidden in (
        "std::make_unique<std::fstream>",
        "stream_->seekg",
        "stream_->seekp",
        "stream_->read",
        "stream_->write",
        "stream_->flush",
        "stream_->close",
        "std::filesystem::file_size(path_",
    ):
        require(forbidden not in source_text, f"stream_based_file_device_path_present:{forbidden}")

    for token in (
        "#ifdef _WIN32",
        "::CreateFileA(file_path.c_str()",
        "::WriteFile(file",
        "::FlushFileBuffers(file)",
        "::ReadFile(file",
        "::CloseHandle(file)",
        "#else\n#include <fcntl.h>\n#include <unistd.h>",
        "::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600)",
        "::fsync(fd)",
        "::read(read_fd",
        "kWritePathBatchingProofPageSize = 8192",
        "entry.page_size = kWritePathBatchingProofPageSize",
        "mem::DefaultLocalEngineMemoryPolicy()",
        'memory_policy.policy_name = "write_path_batching_page_cache"',
        "memory_policy.page_buffer_pool_limit_bytes = policy.max_resident_bytes",
        "mem::MemoryManager page_cache_memory(memory_policy)",
        "page::BindPageCacheMemoryManager(&cache, &page_cache_memory)",
        "BuildTransactionInventoryPageBody(body,\n                                              kWritePathBatchingProofPageSize)",
    ):
        require(token in write_path_text, f"write_path_batching_platform_token_missing:{token}")
    for forbidden in (
        "#include <fcntl.h>\n#include <unistd.h>\n\n#include <algorithm>",
        "Windows write batching marker is unavailable",
        "entry.page_size = 4096",
        "BuildTransactionInventoryPageBody(body, 4096)",
        "page::PageCacheLedger cache;\n  page::PageCacheLifecycleInput lifecycle;",
    ):
        require(forbidden not in write_path_text,
                f"write_path_batching_platform_stub_present:{forbidden}")

    compiler = shutil.which("x86_64-w64-mingw32-g++")
    require(compiler is not None, "mingw_windows_x64_cross_compiler_missing")
    with tempfile.TemporaryDirectory(prefix="sb_storage_disk_windows_") as tmp:
        tmp_path = Path(tmp)
        compile_windows_x64_probe(compiler, tmp_path / "windows_x64_probe.cpp", tmp_path / "windows_x64_probe.o")
        compile_windows_object(compiler, project_root, source, tmp_path / "disk_device.o")
        compile_windows_object(
            compiler,
            project_root,
            write_path_source,
            tmp_path / "write_path_batching.o",
        )

    print("engine_listener_storage_io_platform_source_gate=passed")


if __name__ == "__main__":
    main()
