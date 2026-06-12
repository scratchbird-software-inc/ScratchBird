#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import argparse
import csv
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


SMOKE_PROBES = {
    "fb_lock_print": (["-?"], ("Firebird lock print utility",), {0}),
    "fbguard": (["-?"], ("Usage:", "fbguard"), {255}),
    "fbsvcmgr": (["-?"], ("Usage: fbsvcmgr",), {1}),
    "fbtracemgr": (["-?"], ("Firebird Trace Manager",), {1}),
    "gbak": (["-z"], ("gbak version",), {1}),
    "gfix": (["-z"], ("gfix version",), {0}),
    "gpre": (["-z"], ("gpre version",), {1}),
    "gsec": (["-?"], ("gsec utility",), {0}),
    "gsplit": (["-?"], ("Command Line Options Are",), {1}),
    "gstat": (["-z"], ("gstat version",), {1}),
    "isql": (["-z"], ("ISQL Version",), {0}),
    "nbackup": (["-?"], ("Physical Backup Manager",), {1}),
}

TEST_ONLY_NO_SMOKE = {
    "firebird": "server binary is built for reference oracle harness startup, not metadata smoke",
    "fbtrace": "trace plugin is loaded only by reference trace harness cases",
}

REFERENCE_LIB_NAMES = ("libfbclient", "libtommath", "libtomcrypt", "libib_util")


@dataclass(frozen=True)
class ToolRow:
    artifact_id: str
    tool_name: str
    firebird_target: str
    expected_path: Path
    status: str


def is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def run_command(
    command: list[str],
    *,
    cwd: Path,
    env: dict[str, str],
    timeout_seconds: int,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout_seconds,
        check=False,
    )


def read_manifest(manifest: Path, repo_root: Path) -> list[ToolRow]:
    rows: list[ToolRow] = []
    with manifest.open(newline="") as handle:
        for row in csv.DictReader(handle):
            expected = (repo_root / row["expected_test_artifact_path"]).resolve()
            rows.append(
                ToolRow(
                    artifact_id=row["artifact_id"],
                    tool_name=row["tool_name"],
                    firebird_target=row["firebird_target"],
                    expected_path=expected,
                    status=row["status"],
                )
            )
    if not rows:
        raise SystemExit(f"reference tool manifest has no rows: {manifest}")
    return rows


def reference_home(reference_root: Path) -> Path:
    return reference_root / "gen" / "Release" / "firebird"


def target_path(home: Path, target: str) -> Path:
    if target == "fbtrace":
        return home / "plugins" / "libfbtrace.so"
    return home / "bin" / target


def build_if_missing(reference_root: Path, jobs: int, timeout_seconds: int) -> None:
    home = reference_home(reference_root)
    if (home / "bin" / "isql").exists() and (home / "lib" / "libfbclient.so").exists():
        return

    if shutil.which("make") is None:
        raise SystemExit("make is required to compile Firebird reference tools")

    configure_args = [
        f"--prefix={home}",
        "--with-builtin-tommath",
        "--with-builtin-tomcrypt",
    ]
    configure = reference_root / "configure"
    autogen = reference_root / "autogen.sh"
    if configure.exists():
        configure_cmd = [str(configure), *configure_args]
    elif autogen.exists():
        if shutil.which("autoreconf") is None:
            raise SystemExit(
                "autoreconf is required to generate Firebird reference configure script"
            )
        configure_cmd = [str(autogen), *configure_args]
    else:
        raise SystemExit(
            f"reference source configure/autogen script missing: {configure} or {autogen}"
        )
    result = run_command(
        configure_cmd,
        cwd=reference_root,
        env=os.environ.copy(),
        timeout_seconds=timeout_seconds,
    )
    if result.returncode != 0:
        raise SystemExit(f"Firebird reference configure failed:\n{result.stdout}")

    make_cmd = ["make", f"-j{jobs}"]
    result = run_command(
        make_cmd,
        cwd=reference_root,
        env=os.environ.copy(),
        timeout_seconds=timeout_seconds,
    )
    if result.returncode != 0:
        raise SystemExit(f"Firebird reference make failed:\n{result.stdout}")


def reference_env(home: Path, temp_root: Path | None = None) -> dict[str, str]:
    env = os.environ.copy()
    lib_path = str(home / "lib")
    existing_ld = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = lib_path if not existing_ld else f"{lib_path}:{existing_ld}"
    env["FIREBIRD"] = str(home)
    if temp_root is not None:
        env["HOME"] = str(temp_root / "home")
        env["TMPDIR"] = str(temp_root / "tmp")
        Path(env["HOME"]).mkdir(parents=True, exist_ok=True)
        Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    return env


def check_tool_paths(rows: list[ToolRow], repo_root: Path, reference_root: Path) -> None:
    expected_root = (repo_root / "build" / "reference").resolve()
    reference_root = reference_root.resolve()
    if not is_relative_to(reference_root, expected_root):
        raise SystemExit(f"reference root must stay under {expected_root}: {reference_root}")
    home = reference_home(reference_root).resolve()
    if not home.exists():
        raise SystemExit(f"reference Firebird home missing: {home}")

    for row in rows:
        actual = target_path(home, row.firebird_target).resolve()
        if row.expected_path != actual:
            raise SystemExit(
                f"{row.artifact_id} manifest path mismatch: "
                f"{row.expected_path} != {actual}"
            )
        if not actual.exists():
            raise SystemExit(f"{row.artifact_id} reference artifact missing: {actual}")
        if row.firebird_target != "fbtrace" and not os.access(actual, os.X_OK):
            raise SystemExit(f"{row.artifact_id} reference binary is not executable: {actual}")
        if row.status not in {"built_probe_passed", "built_no_smoke_required"}:
            raise SystemExit(
                f"{row.artifact_id} status must record built evidence, found {row.status}"
            )


def check_reference_link_resolution(rows: list[ToolRow], home: Path, reference_root: Path) -> None:
    env = reference_env(home)
    reference_root = reference_root.resolve()
    for row in rows:
        path = target_path(home, row.firebird_target)
        result = run_command(["ldd", str(path)], cwd=home, env=env, timeout_seconds=15)
        output = result.stdout
        if result.returncode != 0 and "not a dynamic executable" not in output:
            raise SystemExit(f"ldd failed for {path}:\n{output}")
        for line in output.splitlines():
            lowered = line.lower()
            if not any(name in lowered for name in REFERENCE_LIB_NAMES):
                continue
            if "not found" in lowered:
                raise SystemExit(f"unresolved reference library dependency in {path}:\n{output}")
            if "=>" not in line:
                continue
            resolved = line.split("=>", 1)[1].split("(", 1)[0].strip()
            if not resolved:
                continue
            resolved_path = Path(resolved).resolve()
            if not is_relative_to(resolved_path, reference_root):
                raise SystemExit(
                    f"reference tool dependency escaped reference build root for {path}: "
                    f"{resolved_path}"
                )


def smoke_tool(row: ToolRow, home: Path, temp_root: Path, timeout_seconds: int) -> None:
    target = row.firebird_target
    if target in TEST_ONLY_NO_SMOKE:
        return
    probe = SMOKE_PROBES.get(target)
    if probe is None:
        raise SystemExit(f"no reference smoke probe declared for {row.artifact_id}:{target}")
    args, required_fragments, allowed_statuses = probe
    path = target_path(home, target)
    before = sorted(path.relative_to(temp_root) for path in temp_root.rglob("*"))
    result = run_command(
        [str(path), *args],
        cwd=temp_root,
        env=reference_env(home, temp_root),
        timeout_seconds=timeout_seconds,
    )
    output = result.stdout
    if result.returncode not in allowed_statuses:
        raise SystemExit(
            f"{row.artifact_id} smoke returned {result.returncode}, "
            f"expected {sorted(allowed_statuses)}:\n{output}"
        )
    for fragment in required_fragments:
        if fragment not in output:
            raise SystemExit(f"{row.artifact_id} smoke missing {fragment!r}:\n{output}")
    after = sorted(path.relative_to(temp_root) for path in temp_root.rglob("*"))
    allowed_roots = {"home", "tmp"}
    created = {path for path in after if path not in before}
    if any(path.parts[0] not in allowed_roots for path in created):
        raise SystemExit(f"{row.artifact_id} wrote outside sandbox home/tmp: {sorted(created)}")


def run_sandbox_smoke(rows: list[ToolRow], home: Path, timeout_seconds: int) -> None:
    with tempfile.TemporaryDirectory(prefix="sb_firebird_reference_tool_") as raw_temp:
        temp_root = Path(raw_temp).resolve()
        for row in rows:
            smoke_tool(row, home, temp_root, timeout_seconds)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("build", "sandbox"), required=True)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--reference-root", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--build-if-missing", action="store_true")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--build-timeout", type=int, default=1800)
    parser.add_argument("--smoke-timeout", type=int, default=10)
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    reference_root = Path(args.reference_root).resolve()
    manifest = Path(args.manifest).resolve()

    if args.build_if_missing:
        build_if_missing(reference_root, args.jobs, args.build_timeout)

    rows = read_manifest(manifest, repo_root)
    check_tool_paths(rows, repo_root, reference_root)
    home = reference_home(reference_root).resolve()
    check_reference_link_resolution(rows, home, reference_root)
    if args.mode == "sandbox":
        run_sandbox_smoke(rows, home, args.smoke_timeout)

    print(
        f"validated Firebird reference tool {args.mode} gate for "
        f"{len(rows)} manifest rows at {home}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.TimeoutExpired as exc:
        print(f"reference tool command timed out: {exc.cmd}", file=sys.stderr)
        raise SystemExit(1)
