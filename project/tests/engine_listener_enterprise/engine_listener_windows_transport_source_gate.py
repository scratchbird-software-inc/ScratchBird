#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate Windows x64 listener transport source and cross-compile proof."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    print(f"engine_listener_windows_transport_source_gate=fail:{message}", file=sys.stderr)
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
        f"-I{project_root / 'src/parsers/sbsql_worker'}",
        f"-I{project_root / 'src/manager/protocol'}",
        f"-I{project_root / 'src/manager/node'}",
        f"-I{project_root / 'src/server'}",
        f"-I{project_root / 'src/core/memory'}",
        f"-I{project_root / 'src/core/platform'}",
        f"-I{project_root / 'src/core/uuid'}",
        f"-I{project_root / 'src/core/agents'}",
        f"-I{project_root / 'src/core/metrics'}",
        f"-I{project_root / 'src/wire/parser_server_ipc'}",
        f"-I{project_root / 'include'}",
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
    listener_runtime = project_root / "src/listener/listener_runtime.cpp"
    parser_pool = project_root / "src/listener/parser_pool.cpp"
    control_plane_cpp = project_root / "src/listener/control_plane.cpp"
    control_plane_hpp = project_root / "src/listener/control_plane.hpp"
    sbsql_parser_runtime = project_root / "src/parsers/sbsql_worker/runtime/parser_runtime.cpp"
    sbsql_test_wire = project_root / "src/parsers/sbsql_worker/wire/sbsql_test_wire.cpp"
    sbsql_worker_cmake = project_root / "src/parsers/sbsql_worker/CMakeLists.txt"
    manager_runtime_cpp = project_root / "src/manager/node/manager_runtime.cpp"
    manager_listener_control = project_root / "src/manager/node/manager_listener_control.cpp"
    manager_cmake = project_root / "src/manager/node/CMakeLists.txt"
    server_listener_orchestrator = project_root / "src/server/listener_orchestrator.cpp"
    server_cmake = project_root / "src/server/CMakeLists.txt"
    listener_cmake = project_root / "src/listener/CMakeLists.txt"
    platform_matrix = project_root / "cmake/SUPPORTED_PLATFORM_TOOLCHAIN_MATRIX.md"
    windows_requirements = project_root.parent / "docs/build_requirements/windows/README.md"

    runtime = read(listener_runtime)
    pool = read(parser_pool)
    control_cpp = read(control_plane_cpp)
    control_hpp = read(control_plane_hpp)
    parser_runtime = read(sbsql_parser_runtime)
    parser_wire = read(sbsql_test_wire)
    parser_cmake = read(sbsql_worker_cmake)
    manager_runtime = read(manager_runtime_cpp)
    manager_control = read(manager_listener_control)
    manager_node_cmake = read(manager_cmake)
    server_orchestrator = read(server_listener_orchestrator)
    server_core_cmake = read(server_cmake)
    cmake = read(listener_cmake)
    matrix = read(platform_matrix)
    requirements = read(windows_requirements)

    for token in (
        'target_id: "windows-x86_64-msvc-core"',
        'architecture: "x86_64"',
        "win32-first-release-out-of-scope",
        "windows_x64_only_no_win32_release_target",
        "Windows 32-bit",
        "Win32",
        "x86 Windows",
    ):
        require(token in matrix, f"windows_x64_matrix_boundary_missing:{token}")
    require('target_id: "windows-x86-' not in matrix,
            "windows_32bit_target_declared")
    for token in (
        "Win32 is not a supported release target",
        "x64 tools, x64 dependencies, x64 Python",
        "Windows x86, i386, i686",
    ):
        require(token in requirements, f"windows_x64_requirements_boundary_missing:{token}")
    for token in (
        "config_.service && !config_.validate_config",
        "MANAGER.SERVICE_MODE_UNSUPPORTED",
        "Windows service-control handoff is not implemented in this build.",
        "DaemonizeService",
        "::fork()",
        "::setsid()",
    ):
        require(token in manager_runtime, f"windows_manager_service_boundary_missing:{token}")

    require("LISTENER.RUNTIME.WINDOWS_SOCKET_NOT_ATTACHED" not in runtime,
            "windows_network_bind_still_unavailable")
    require("LISTENER.MANAGEMENT_SOCKET_UNAVAILABLE" not in runtime,
            "windows_management_socket_still_unavailable")
    for token in (
        "EnsureWinsockInitialized",
        "WSAStartup(MAKEWORD(2, 2)",
        "#include <winsock2.h>",
        "#include <ws2tcpip.h>",
        "#include <afunix.h>",
        "::socket(AF_UNIX, SOCK_STREAM, 0)",
        "::getaddrinfo(host, service.c_str(), &hints, &resolved)",
        "IPV6_V6ONLY",
        "::accept(static_cast<SOCKET>(listen_fd)",
        "::accept(static_cast<SOCKET>(management_fd)",
        "ParserHandoffBinding handoff_binding = TakePendingHandoffBinding(evidence);",
    ):
        require(token in runtime, f"windows_runtime_token_missing:{token}")

    require("LISTENER.HANDOFF_PLATFORM_UNAVAILABLE" not in pool,
            "windows_parser_handoff_still_platform_refused")
    require("windows_handoff_unavailable" not in pool,
            "windows_parser_handoff_reason_still_unavailable")
    require("windows parser process launch is not attached" not in pool,
            "windows_parser_launch_still_platform_stub")
    for token in (
        "SB_LISTENER_CONTROL_SOCKET",
        "SB_LISTENER_CONTROL_TRANSPORT",
        "windows-afunix-v1",
        "BindWindowsControlListener",
        "AcceptWindowsControlSocket",
        "::CreateProcessA(nullptr",
        "::WaitForSingleObject",
        "::TerminateProcess",
        "::WSADuplicateSocketA",
        "EncodeWindowsSocketHandoffPayload",
        "kControlFlagWindowsSocketInfo",
        "LISTENER.HANDOFF_WINDOWS_DUPLICATE_SOCKET_FAILED",
    ):
        require(token in pool, f"windows_parser_pool_handoff_token_missing:{token}")
    require("SendControlFrame(std::intptr_t fd" in control_hpp,
            "windows_control_frame_header_missing")
    require("kControlFlagWindowsSocketInfo" in control_hpp,
            "windows_socket_handoff_flag_missing")
    require("WindowsSocketHandoffPayload" in control_hpp,
            "windows_socket_handoff_payload_type_missing")
    for token in (
        "EncodeWindowsSocketHandoffPayload",
        "DecodeWindowsSocketHandoffPayload",
        "LISTENER.WINDOWS_HANDOFF_BAD_MAGIC",
        "LISTENER.WINDOWS_HANDOFF_LENGTH_INVALID",
    ):
        require(token in control_cpp, f"windows_control_handoff_codec_missing:{token}")
    require("fd_to_transfer >= 0) return false;" in control_cpp,
            "windows_raw_handle_transfer_refusal_missing")
    require("ReadControlFrame(std::intptr_t fd" in control_cpp,
            "windows_control_frame_read_missing")
    require("target_link_libraries(sbl_listener_control_plane PUBLIC ws2_32)" in cmake,
            "windows_control_plane_ws2_32_link_missing")
    require("target_link_libraries(sbl_listener_runtime PUBLIC ws2_32)" in cmake,
            "windows_runtime_ws2_32_link_missing")
    require("sbp_sbsql listener-worker mode is not attached on Windows." not in parser_runtime,
            "sbsql_worker_still_refuses_windows_listener_mode")
    for token in (
        "ConnectListenerControlSocket",
        "SB_LISTENER_CONTROL_SOCKET",
        "DecodeWindowsSocketHandoffPayload",
        "RehydrateWindowsClientSocket",
        "WSASocketA(FROM_PROTOCOL_INFO",
        "CloseSocket(&config.listener_control_fd)",
    ):
        require(token in parser_runtime, f"sbsql_windows_worker_token_missing:{token}")
    for token in (
        "::send(static_cast<SOCKET>(fd)",
        "::recv(static_cast<SOCKET>(fd)",
        "FD_SET(static_cast<SOCKET>(fd)",
        "int SbsqlTestWireSession::ServeFd(std::intptr_t fd)",
    ):
        require(token in parser_wire, f"sbsql_windows_wire_token_missing:{token}")
    require("target_link_libraries(sbl_sbsql_parser_worker_core PUBLIC ws2_32)" in parser_cmake,
            "sbsql_worker_ws2_32_link_missing")
    require("MANAGER.LISTENER_PLATFORM_UNAVAILABLE" not in manager_control,
            "windows_manager_listener_control_still_unavailable")
    require("Listener management sockets are not attached on Windows." not in manager_control,
            "windows_manager_listener_control_still_platform_stub")
    for token in (
        "EnsureWinsockInitialized",
        "WSAStartup(MAKEWORD(2, 2)",
        "NormalizeManagementSocketPath",
        "::socket(AF_UNIX, SOCK_STREAM, 0)",
        "scratchbird::listener::SendControlFrame(fd, frame)",
        "scratchbird::listener::ReadControlFrame(fd, &decoded, &received_fd, timeout_ms)",
        "scratchbird::listener::kListenerManagementAuthHmacSha256",
        "scratchbird::listener::SignListenerManagementEnvelopeHmacSha256",
        "MANAGER.LISTENER_MANAGEMENT_HMAC_KEY_MISSING",
    ):
        require(token in manager_control, f"windows_manager_control_token_missing:{token}")
    require("scratchbird::listener::kListenerManagementAuthPeerOwner\n#endif" in manager_control,
            "windows_manager_control_unix_peer_owner_branch_missing")
    require("target_link_libraries(sbl_manager_node_runtime PUBLIC ws2_32)" in manager_node_cmake,
            "windows_manager_ws2_32_link_missing")
    require("Server-managed listener control is unavailable on this platform." not in server_orchestrator,
            "windows_server_listener_control_still_platform_stub")
    require("Server-managed listener launch is unavailable on this platform." not in server_orchestrator,
            "windows_server_listener_launch_still_platform_stub")
    require("LISTENER.PLATFORM_UNAVAILABLE" not in server_orchestrator,
            "windows_server_listener_lifecycle_still_platform_unavailable")
    for token in (
        "EnsureWinsockInitialized",
        "WSAStartup(MAKEWORD(2, 2)",
        "NormalizeListenerManagementSocketPath",
        "::socket(AF_UNIX, SOCK_STREAM, 0)",
        "scratchbird::listener::SendControlFrame(fd, frame)",
        "scratchbird::listener::ReadControlFrame(fd, &decoded, &received_fd, timeout_ms)",
        "scratchbird::listener::kListenerManagementAuthHmacSha256",
        "scratchbird::listener::SignListenerManagementEnvelopeHmacSha256",
        "ServerManagedListenerDbbtKey",
        "LISTENER.MANAGEMENT_SOCKET_STACK_UNAVAILABLE",
        "QuoteWindowsCommandLineArgument",
        "BuildWindowsCommandLine",
        "OpenListenerProcessHandle",
        "::OpenProcess",
        "::CreateProcessA(nullptr",
        "::WaitForSingleObject",
        "TerminateListenerProcess",
        "::TerminateProcess",
        "LISTENER.START_TIMEOUT",
    ):
        require(token in server_orchestrator, f"windows_server_control_token_missing:{token}")
    require("target_link_libraries(sb_server_core PUBLIC ws2_32)" in server_core_cmake,
            "windows_server_ws2_32_link_missing")

    compiler = shutil.which("x86_64-w64-mingw32-g++")
    require(compiler is not None, "mingw_windows_x64_cross_compiler_missing")
    with tempfile.TemporaryDirectory(prefix="sb_listener_windows_transport_") as tmp:
        tmp_path = Path(tmp)
        compile_windows_x64_probe(compiler, tmp_path / "windows_x64_probe.cpp", tmp_path / "windows_x64_probe.o")
        compile_windows_object(compiler, project_root, control_plane_cpp, tmp_path / "control_plane.o")
        compile_windows_object(compiler, project_root, listener_runtime, tmp_path / "listener_runtime.o")
        compile_windows_object(compiler, project_root, parser_pool, tmp_path / "parser_pool.o")
        compile_windows_object(compiler, project_root, sbsql_parser_runtime, tmp_path / "sbsql_parser_runtime.o")
        compile_windows_object(compiler, project_root, sbsql_test_wire, tmp_path / "sbsql_test_wire.o")
        compile_windows_object(compiler, project_root, manager_listener_control, tmp_path / "manager_listener_control.o")
        compile_windows_object(compiler, project_root, server_listener_orchestrator, tmp_path / "server_listener_orchestrator.o")

    print("engine_listener_windows_transport_source_gate=passed")


if __name__ == "__main__":
    main()
