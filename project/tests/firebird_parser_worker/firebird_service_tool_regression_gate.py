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
import subprocess
import sys
from pathlib import Path

from firebird_reference_native_harness import normalize_firebird_reference_output


TOOL_PROBES = {
    "gbak": (["-z"], ("gbak version",), {1}),
    "gfix": (["-z"], ("gfix version",), {0}),
    "gstat": (["-z"], ("gstat version",), {1}),
    "nbackup": (["-?"], ("Physical Backup Manager",), {1}),
    "fbsvcmgr": (["-?"], ("Usage: fbsvcmgr",), {1}),
    "fbtracemgr": (["-?"], ("Firebird Trace Manager",), {1}),
    "gsec": (["-?"], ("gsec utility",), {0}),
    "gpre": (["-z"], ("gpre version",), {1}),
    "gsplit": (["-?"], ("Command Line Options Are",), {1}),
    "fb_lock_print": (["-?"], ("Firebird lock print utility",), {0}),
    "fbguard": (["-?"], ("Usage:", "fbguard"), {255}),
}

REQUIRED_PARSER_VARIATIONS = (
    "GBAK -restore",
    "GFIX -mend",
    "GFIX -sweep",
    "GSTAT -data",
    "GSTAT -index",
    "NBACKUP -lock",
    "NBACKUP -unlock",
    "NBACKUP -fixup",
    "FBSVCMGR service_mgr action_backup",
    "FBSVCMGR service_mgr action_restore",
    "FBSVCMGR service_mgr action_validate",
)


def read_manifest(path: Path) -> dict[str, dict[str, str]]:
    with path.open(newline="") as handle:
        return {row["firebird_target"]: row for row in csv.DictReader(handle)}


def reference_env(firebird_home: Path, output_dir: Path) -> dict[str, str]:
    env = os.environ.copy()
    lib_path = str(firebird_home / "lib")
    existing = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = lib_path if not existing else f"{lib_path}:{existing}"
    env["FIREBIRD"] = str(firebird_home)
    env["HOME"] = str(output_dir / "home")
    env["TMPDIR"] = str(output_dir / "tmp")
    Path(env["HOME"]).mkdir(parents=True, exist_ok=True)
    Path(env["TMPDIR"]).mkdir(parents=True, exist_ok=True)
    return env


def run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=30,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--firebird-home", required=True)
    parser.add_argument("--parser-probe", required=True)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    manifest = read_manifest(Path(args.manifest).resolve())
    firebird_home = Path(args.firebird_home).resolve()
    parser_probe = Path(args.parser_probe).resolve()
    repo_root = Path(args.repo_root).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    missing = sorted(target for target in TOOL_PROBES if target not in manifest)
    if missing:
        raise SystemExit(f"reference tool build manifest missing service targets: {missing}")

    parser_result = run([str(parser_probe)], cwd=output_dir)
    parser_raw = output_dir / "firebird_service_tool_parser_probe.raw.txt"
    parser_raw.write_text(parser_result.stdout)
    if parser_result.returncode != 0:
        print(parser_result.stdout, file=sys.stderr)
        return parser_result.returncode
    for fragment in REQUIRED_PARSER_VARIATIONS:
        if fragment not in parser_result.stdout:
            raise SystemExit(f"parser service-tool variation missing: {fragment}")

    report_rows: list[dict[str, str]] = []
    for line in parser_result.stdout.splitlines():
        if " => " not in line:
            continue
        command, operation = line.split(" => ", 1)
        report_rows.append(
            {
                "tool": f"parser:{command.split()[0].lower()}",
                "probe_args": command,
                "raw_output": str(parser_raw),
                "normalized_output": str(parser_raw),
                "scratchbird_runtime_classification": "unsupported_denied_authority_diagnostic",
                "parser_operation_family": operation,
                "ctest_gate": "firebird_service_tool_regression_gate",
            }
        )
    env = reference_env(firebird_home, output_dir)
    for target, (probe_args, fragments, allowed_statuses) in TOOL_PROBES.items():
        tool_path = firebird_home / "bin" / target
        if not tool_path.exists():
            raise SystemExit(f"reference service tool missing: {tool_path}")
        result = run([str(tool_path), *probe_args], cwd=output_dir, env=env)
        raw_path = output_dir / f"{target}.raw.txt"
        normalized_path = output_dir / f"{target}.normalized.txt"
        raw_path.write_text(result.stdout)
        normalized_path.write_text(
            normalize_firebird_reference_output(
                result.stdout,
                repo_root=repo_root,
                temp_root=output_dir,
            )
        )
        if result.returncode not in allowed_statuses:
            print(result.stdout, file=sys.stderr)
            raise SystemExit(f"{target} returned {result.returncode}")
        for fragment in fragments:
            if fragment not in result.stdout:
                raise SystemExit(f"{target} output missing required fragment {fragment!r}")
        report_rows.append(
            {
                "tool": target,
                "probe_args": " ".join(probe_args),
                "raw_output": str(raw_path),
                "normalized_output": str(normalized_path),
                "scratchbird_runtime_classification": "emulated_service_or_authority_diagnostic",
                "parser_operation_family": "reference_tool_metadata_probe",
                "ctest_gate": "firebird_service_tool_regression_gate",
            }
        )

    classification_path = output_dir / "firebird_service_tool_regression_classification.csv"
    with classification_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(report_rows[0].keys()))
        writer.writeheader()
        writer.writerows(report_rows)

    report = output_dir / "FIREBIRD_SERVICE_TOOL_REGRESSION_SEED_REPORT.md"
    report.write_text(
        "# Firebird Service Tool Regression Seed Report\n\n"
        f"Validated {len(report_rows)} reference service and utility tool probes.\n\n"
        "Parser classification probe:\n\n"
        "```\n"
        f"{parser_result.stdout.strip()}\n"
        "```\n"
    )
    print(
        "validated Firebird service-tool regression seed for "
        f"{len(report_rows)} reference tool surfaces"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
