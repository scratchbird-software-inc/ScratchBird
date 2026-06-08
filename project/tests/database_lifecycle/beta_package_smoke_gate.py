#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Install/package smoke gate for core beta artifacts."""

from __future__ import annotations

import argparse
import csv
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import textwrap
import time


def fail(message: str) -> None:
    print(f"beta_package_smoke_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def run(
    command: list[str],
    *,
    cwd: pathlib.Path,
    timeout: int = 120,
    env: dict[str, str] | None = None,
    accepted_returncodes: set[int] | None = None,
) -> str:
    accepted = accepted_returncodes if accepted_returncodes is not None else {0}
    result = subprocess.run(
        command,
        cwd=str(cwd),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )
    if result.returncode not in accepted:
        print(result.stdout)
        fail(f"command_failed:{' '.join(command)}:exit={result.returncode}")
    return result.stdout


def safe_rmtree(path: pathlib.Path, build_root: pathlib.Path) -> None:
    resolved = path.resolve()
    require(
        resolved == build_root.resolve() or build_root.resolve() in resolved.parents,
        f"refusing_to_remove_outside_build:{resolved}",
    )
    if resolved.exists():
        shutil.rmtree(resolved)


def require_file(path: pathlib.Path, label: str) -> None:
    require(path.exists() and path.is_file(), f"missing_{label}:{path}")


def require_any_file(paths: tuple[pathlib.Path, ...], label: str) -> pathlib.Path:
    for path in paths:
        if path.exists() and path.is_file():
            return path
    require(False, f"missing_{label}:{paths[0]}")
    return paths[0]


def require_executable(path: pathlib.Path, label: str) -> None:
    require(path.exists() and path.is_file() and os.access(path, os.X_OK), f"missing_executable_{label}:{path}")


def write_consumer_project(source_dir: pathlib.Path) -> None:
    source_dir.mkdir(parents=True, exist_ok=True)
    (source_dir / "CMakeLists.txt").write_text(
        textwrap.dedent(
            """\
            cmake_minimum_required(VERSION 3.20)
            project(scratchbird_package_smoke_consumer C)
            find_package(ScratchBirdEngine CONFIG REQUIRED)
            add_executable(scratchbird_package_smoke_consumer main.c)
            target_link_libraries(scratchbird_package_smoke_consumer PRIVATE ScratchBird::sb_engine)
            """
        ),
        encoding="utf-8",
    )
    (source_dir / "main.c").write_text(
        textwrap.dedent(
            r"""\
            #include "scratchbird/engine/engine.h"

            #include <stdint.h>
            #include <string.h>

            static sb_engine_uuid_t uuid_with_tail(uint8_t tail) {
              sb_engine_uuid_t uuid;
              memset(&uuid, 0, sizeof(uuid));
              uuid.bytes[0] = 0x01;
              uuid.bytes[6] = 0x70;
              uuid.bytes[15] = tail;
              return uuid;
            }

            int main(void) {
              if (sb_engine_abi_version_packed() != SB_ENGINE_ABI_VERSION_PACKED) {
                return 1;
              }
              const char* build_id = 0;
              uint64_t build_id_size = 0;
              if (sb_engine_abi_build_id(&build_id, &build_id_size) != SB_ENGINE_STATUS_OK ||
                  build_id == 0 || build_id_size == 0) {
                return 2;
              }

              sb_engine_open_params_v1_t open_params;
              memset(&open_params, 0, sizeof(open_params));
              open_params.struct_size = sizeof(open_params);
              open_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
              open_params.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;

              sb_engine_handle_t engine = 0;
              sb_engine_result_t result = 0;
              if (sb_engine_open(&open_params, &engine, &result) != SB_ENGINE_STATUS_OK || engine == 0) {
                return 3;
              }

              sb_engine_session_params_v1_t session_params;
              memset(&session_params, 0, sizeof(session_params));
              session_params.struct_size = sizeof(session_params);
              session_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
              session_params.effective_user_uuid = uuid_with_tail(1);
              session_params.session_uuid = uuid_with_tail(2);
              session_params.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
              session_params.default_language_utf8 = "en";
              session_params.default_language_size = 2;

              sb_engine_session_t session = 0;
              if (sb_engine_session_begin(engine, &session_params, &session, &result) != SB_ENGINE_STATUS_OK ||
                  session == 0) {
                (void)sb_engine_close(engine, 0);
                return 4;
              }

              sb_engine_request_context_v1_t context;
              memset(&context, 0, sizeof(context));
              context.struct_size = sizeof(context);
              context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
              context.effective_user_uuid = session_params.effective_user_uuid;
              context.session_uuid = session_params.session_uuid;
              context.trust_mode = SB_ENGINE_TRUST_EMBEDDED_TRUSTED;
              context.rights_set_ref = 1;
              context.capability_set_ref = 1;

              sb_engine_sblr_dispatch_params_v1_t dispatch;
              memset(&dispatch, 0, sizeof(dispatch));
              dispatch.struct_size = sizeof(dispatch);
              dispatch.abi_version = SB_ENGINE_ABI_VERSION_PACKED;

              if (sb_engine_dispatch_sblr(session, 0, &context, &dispatch, &result) != SB_ENGINE_STATUS_OK ||
                  result == 0) {
                (void)sb_engine_session_end(session, 0, 0);
                (void)sb_engine_close(engine, 0);
                return 5;
              }
              sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
              if (sb_engine_result_class(result, &result_class) != SB_ENGINE_STATUS_OK ||
                  result_class != SB_ENGINE_RESULT_CAPABILITY_REPORT) {
                return 6;
              }
              if (sb_engine_result_release(result) != SB_ENGINE_STATUS_OK) {
                return 7;
              }

              sb_engine_session_end_params_v1_t end_params;
              memset(&end_params, 0, sizeof(end_params));
              end_params.struct_size = sizeof(end_params);
              end_params.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
              end_params.rollback_active_transactions = 1;
              end_params.cancel_open_results = 1;
              if (sb_engine_session_end(session, &end_params, 0) != SB_ENGINE_STATUS_OK) {
                return 8;
              }
              if (sb_engine_close(engine, 0) != SB_ENGINE_STATUS_OK) {
                return 9;
              }
              return 0;
            }
            """
        ),
        encoding="utf-8",
    )


def validate_driver_manifest(path: pathlib.Path) -> None:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    ids = {row.get("component_id", "") for row in rows}
    for required in ("driver:python", "driver:cpp", "tool:cli"):
        require(required in ids, f"installed_driver_manifest_missing:{required}")
    for row in rows:
        if row.get("component_id") in {"driver:python", "driver:cpp", "tool:cli"}:
            require(row.get("conformance_profile_ref"), f"driver_manifest_missing_ctest_ref:{row.get('component_id')}")


def compiler_runtime_dir(build_root: pathlib.Path) -> pathlib.Path | None:
    cache = build_root / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_CXX_COMPILER:FILEPATH="):
            compiler = pathlib.Path(line.split("=", 1)[1])
            if compiler.exists():
                return compiler.parent
    return None


def package_runtime_env(prefix: pathlib.Path, build_root: pathlib.Path) -> dict[str, str]:
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = os.pathsep.join(
        [str(prefix / "lib"), env.get("LD_LIBRARY_PATH", "")]
    )
    if os.name == "nt":
        path_parts = [str(prefix / "bin"), str(prefix / "lib")]
        runtime_dir = compiler_runtime_dir(build_root)
        if runtime_dir is not None:
            path_parts.append(str(runtime_dir))
        existing_path = env.get("PATH", "")
        if existing_path:
            path_parts.append(existing_path)
        env["PATH"] = os.pathsep.join(path_parts)
    return env


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    args = parser.parse_args()

    repo_root = pathlib.Path(args.repo_root).resolve()
    build_root = pathlib.Path(args.build_root).resolve()
    gate_root = build_root / "pkg"
    prefix = gate_root / "install"
    work_root = gate_root / "w"
    consumer_source = gate_root / "c"
    consumer_build = gate_root / "cb"

    safe_rmtree(gate_root, build_root)
    gate_root.mkdir(parents=True)

    run(["cmake", "--install", str(build_root), "--prefix", str(prefix)], cwd=repo_root, timeout=300)

    require_any_file(
        (
            prefix / "lib/libSBcore.so",
            prefix / "bin/SBcore.dll",
            prefix / "lib/libSBcore.dll.a",
        ),
        "engine_shared_library",
    )
    require_file(prefix / "include/scratchbird/engine/engine.h", "public_engine_header")
    require_file(prefix / "include/scratchbird/engine/version.h", "public_version_header")
    require_file(prefix / "lib/cmake/ScratchBirdEngine/ScratchBirdEngineConfig.cmake", "cmake_package_config")
    require_file(prefix / "lib/cmake/ScratchBirdEngine/ScratchBirdEngineTargets.cmake", "cmake_package_targets")
    require_file(
        prefix / "share/scratchbird/docs/public_api/CORE_BETA_PUBLIC_API_ABI_MANIFEST.json",
        "public_api_manifest",
    )

    cache_text = (build_root / "CMakeCache.txt").read_text(encoding="utf-8")
    install_non_engine_components = "SB_INSTALL_NON_ENGINE_COMPONENTS:BOOL=ON" in cache_text
    runtime_env = package_runtime_env(prefix, build_root)
    non_engine_artifacts = (
        (prefix / "bin/SBsrv", "server"),
        (prefix / "bin/sb_ipc_tester", "ipc_tester"),
        (prefix / "bin/SBgate", "listener"),
        (prefix / "bin/SBParser", "sbsql_parser_worker"),
        (prefix / "bin/SB_FBSQL_Parser", "firebird_parser_worker"),
        (prefix / "lib/libSBParser_udr.a", "sbsql_parser_udr_archive"),
        (prefix / "share/scratchbird/drivers/DriverPackageManifest.csv", "driver_package_manifest"),
        (prefix / "share/scratchbird/examples/core_beta_qa/manifest.json", "qa_examples_manifest"),
    )

    if install_non_engine_components:
        require_executable(prefix / "bin/SBsrv", "server")
        require_executable(prefix / "bin/sb_ipc_tester", "ipc_tester")
        require_executable(prefix / "bin/SBgate", "listener")
        require_executable(prefix / "bin/SBParser", "sbsql_parser_worker")
        require_executable(prefix / "bin/SB_FBSQL_Parser", "firebird_parser_worker")
        require_file(prefix / "lib/libSBParser_udr.a", "sbsql_parser_udr_archive")
        require_file(prefix / "share/scratchbird/drivers/DriverPackageManifest.csv", "driver_package_manifest")
        require_file(prefix / "share/scratchbird/examples/core_beta_qa/manifest.json", "qa_examples_manifest")

        validate_driver_manifest(prefix / "share/scratchbird/drivers/DriverPackageManifest.csv")
        examples_manifest = json.loads((prefix / "share/scratchbird/examples/core_beta_qa/manifest.json").read_text(encoding="utf-8"))
        require(examples_manifest.get("pack_id") == "scratchbird-core-beta-qa-examples", "installed_examples_manifest_mismatch")

        server_version = run([str(prefix / "bin/SBsrv"), "--version"], cwd=repo_root, env=runtime_env)
        require("SBsrv" in server_version and "standalone-server" in server_version, "server_version_output_mismatch")
        listener_help = run([str(prefix / "bin/SBgate"), "--help"], cwd=repo_root, env=runtime_env)
        require("SBgate" in listener_help and "tls-required" in listener_help, "listener_help_output_mismatch")
        parser_probe = run([str(prefix / "bin/SBParser"), "--probe-worker", "--help"], cwd=repo_root, env=runtime_env)
        require("requires --listener-worker" in parser_probe, "parser_worker_probe_output_mismatch")

        work_root.mkdir(parents=True)
        server_route = run(
            [
                str(prefix / "bin/SBsrv"),
                "--control-dir",
                str(work_root / "control"),
                "--runtime-dir",
                str(work_root / "data"),
                "--database",
                str(work_root / "missing.sbdb"),
                "--lifecycle-command",
                "status",
                "--lifecycle-audit-reason",
                "beta-package-smoke",
            ],
            cwd=repo_root,
            timeout=60,
            env=runtime_env,
            accepted_returncodes={0, 2},
        )
        require("server_startup" in server_route, "installed_server_route_missing_startup_evidence")
        require("SERVER.ENGINE_HOST.DATABASE_NOT_FOUND" in server_route, "installed_server_route_missing_exact_diagnostic")
        require((work_root / "control/sb_server.pid").exists(), "server_route_missing_pid_file")
        require(not any(str(path).startswith(str(repo_root / ".understand-anything")) for path in work_root.rglob("*")), "runtime_touched_understand_anything")

        live_root = work_root / "l"
        live_control = live_root / "c"
        live_runtime = live_root / "data"
        live_database = live_root / "package-smoke.sbdb"
        live_log = live_root / "server.log"
        live_stdout = live_root / "server.out"
        live_root.mkdir(parents=True)
        with live_stdout.open("w", encoding="utf-8") as stdout_handle:
            server = subprocess.Popen(
                [
                    str(prefix / "bin/SBsrv"),
                    "--control-dir",
                    str(live_control),
                    "--runtime-dir",
                    str(live_runtime),
                    "--database",
                    str(live_database),
                    "--create-if-missing",
                    "--no-listeners",
                    "--log",
                    str(live_log),
                ],
                cwd=str(repo_root),
                stdout=stdout_handle,
                stderr=subprocess.STDOUT,
                text=True,
                env=runtime_env,
            )
        try:
            endpoint = live_control / "sb_server.sbps.sock"
            for _ in range(60):
                if endpoint.exists():
                    break
                if server.poll() is not None:
                    print(live_stdout.read_text(encoding="utf-8", errors="replace"))
                    fail(f"installed_server_exited_before_endpoint:exit={server.returncode}")
                time.sleep(0.1)
            require(endpoint.exists(), "installed_server_endpoint_missing")
            hello = run(
                [str(prefix / "bin/sb_ipc_tester"), "--endpoint", str(endpoint), "--scenario", "hello", "--expect", "accept"],
                cwd=repo_root,
                timeout=30,
                env=runtime_env,
            )
            require('"scenario":"hello"' in hello and '"accepted":true' in hello, "installed_ipc_hello_failed")
            status = run(
                [
                    str(prefix / "bin/sb_ipc_tester"),
                    "--endpoint",
                    str(endpoint),
                    "--scenario",
                    "database_status",
                    "--expect",
                    "accept",
                    "--expect-payload-contains",
                    "database_open",
                ],
                cwd=repo_root,
                timeout=30,
                env=runtime_env,
            )
            require('"scenario":"database_status"' in status and '"accepted":true' in status, "installed_ipc_database_status_failed")
            require(live_database.exists(), "installed_server_did_not_create_database")
        finally:
            if server.poll() is None:
                server.send_signal(signal.SIGTERM)
                try:
                    server.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=10)
            require(server.returncode == 0, f"installed_server_shutdown_not_clean:{server.returncode}")
    else:
        for artifact, label in non_engine_artifacts:
            require(not artifact.exists(), f"unexpected_non_engine_artifact:{label}:{artifact}")

    write_consumer_project(consumer_source)
    run(
        ["cmake", "-S", str(consumer_source), "-B", str(consumer_build), f"-DCMAKE_PREFIX_PATH={prefix}"],
        cwd=repo_root,
        timeout=120,
    )
    run(["cmake", "--build", str(consumer_build)], cwd=repo_root, timeout=120)
    run([str(consumer_build / "scratchbird_package_smoke_consumer")], cwd=repo_root, timeout=60, env=runtime_env)

    report = {
        "gate": "CBQ_GATE_BETA_PACKAGE_SMOKE",
        "install_prefix": str(prefix),
        "install_mode": "non_engine_components" if install_non_engine_components else "engine_only",
        "consumer_build": str(consumer_build),
        "server_route": "installed_server_status_diagnostic_and_live_ipc_database_status"
        if install_non_engine_components
        else "not_installed_engine_only_first_public_release",
        "sblr_admission": "installed_public_abi_consumer_capability_report",
        "driver_route": "installed_driver_manifest_python_cpp_tool_cli_conformance_refs"
        if install_non_engine_components
        else "not_installed_engine_only_first_public_release",
        "cluster_claim": "package smoke records installed non-cluster/provider-boundary artifacts only",
    }
    (gate_root / "beta_package_smoke_report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    safe_rmtree(work_root, build_root)
    print(f"beta_package_smoke_install_prefix={prefix}")
    print(f"beta_package_smoke_report={gate_root / 'beta_package_smoke_report.json'}")
    print("beta_package_smoke_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
