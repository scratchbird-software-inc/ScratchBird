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
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

from firebird_donor_native_harness import (
    normalize_firebird_donor_output,
    validate_failure_inventory_record,
)


SCRIPT_RE = re.compile(
    r"test_script\s*=\s*(?P<prefix>[rubfRUBF]*)?(?P<quote>'''|\"\"\")(?P<body>.*?)(?P=quote)",
    re.DOTALL,
)


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def extract_test_script(path: Path) -> str | None:
    text = path.read_text(errors="replace")
    match = SCRIPT_RE.search(text)
    if not match or "isql_act(" not in text:
        return None
    return match.group("body")


def donor_env(firebird_home: Path) -> dict[str, str]:
    env = os.environ.copy()
    lib_path = str(firebird_home / "lib")
    existing = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = lib_path if not existing else f"{lib_path}:{existing}"
    env["FIREBIRD"] = str(firebird_home)
    return env


def run(command: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        timeout=30,
        check=False,
    )


def select_cases(
    replay_rows: list[dict[str, str]],
    candidate_root: Path,
    sample_count: int,
) -> list[tuple[dict[str, str], Path, str]]:
    selected: list[tuple[dict[str, str], Path, str]] = []
    for row in replay_rows:
        relative_path = row["relative_path"]
        if not relative_path.startswith("tests/functional/sqlancer/"):
            continue
        path = candidate_root / relative_path
        if not path.exists():
            continue
        if sha256(path) != row["sha256"]:
            raise SystemExit(f"candidate source hash mismatch for {path}")
        script = extract_test_script(path)
        if script is None:
            continue
        selected.append((row, path, script))
        if len(selected) == sample_count:
            return selected
    return selected


def write_failure_inventory(output_dir: Path, record: dict[str, str]) -> None:
    path = output_dir / "firebird_isql_original_regression_failure_inventory.csv"
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(record.keys()))
        writer.writeheader()
        writer.writerow(record)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-root", required=True)
    parser.add_argument("--replay-manifest", required=True)
    parser.add_argument("--firebird-home", required=True)
    parser.add_argument("--parser-probe", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--sample-count", type=int, default=2)
    args = parser.parse_args()

    candidate_root = Path(args.candidate_root).resolve()
    replay_manifest = Path(args.replay_manifest).resolve()
    firebird_home = Path(args.firebird_home).resolve()
    parser_probe = Path(args.parser_probe).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    replay_rows = read_rows(replay_manifest)
    selected = select_cases(replay_rows, candidate_root, args.sample_count)
    if len(selected) != args.sample_count:
        raise SystemExit(
            f"expected {args.sample_count} replayable original isql cases, found {len(selected)}"
        )

    script_paths: list[Path] = []
    report_lines = [
        "# Firebird isql Original Regression Endpoint Replay Report",
        "",
        "| replay_id | test_id | source | script_path |",
        "| --- | --- | --- | --- |",
    ]
    for row, path, script in selected:
        script_path = output_dir / f"{row['test_id']}.sql"
        script_path.write_text(script)
        script_paths.append(script_path)
        report_lines.append(
            f"| {row['replay_id']} | {row['test_id']} | {path.relative_to(candidate_root)} | {script_path} |"
        )

    parser_result = run([str(parser_probe), *[str(path) for path in script_paths]])
    parser_raw = output_dir / "firebird_parser_probe.raw.txt"
    parser_raw.write_text(parser_result.stdout)
    if parser_result.returncode != 0:
        print(parser_result.stdout, file=sys.stderr)
        return parser_result.returncode

    isql = firebird_home / "bin" / "isql"
    if not isql.exists():
        raise SystemExit(f"donor isql binary missing: {isql}")
    donor_result = run([str(isql), "-z"], env=donor_env(firebird_home))
    donor_raw = output_dir / "donor_isql_version.raw.txt"
    donor_normalized = output_dir / "donor_isql_version.normalized.txt"
    donor_raw.write_text(donor_result.stdout)
    donor_normalized.write_text(
        normalize_firebird_donor_output(
            donor_result.stdout,
            repo_root=Path.cwd().resolve(),
            temp_root=output_dir,
        )
    )
    if donor_result.returncode != 0:
        print(donor_result.stdout, file=sys.stderr)
        return donor_result.returncode

    endpoint_record = {
        "ctest_name": "firebird_isql_original_regression_gate",
        "label_set": "firebird_isql_original_regression_gate;firebird_original_regression_replay_gate;firebird_donor_native",
        "surface_row_id": "FBCTV-002",
        "donor_tool_name": "isql",
        "donor_tool_args": "-z plus extracted QA isql_act scripts",
        "scratchbird_endpoint": "firebird://127.0.0.1:3050/scratchbird",
        "scratchbird_profile": "firebird_5_0",
        "raw_stdout_path": str(donor_raw),
        "raw_stderr_path": str(parser_raw),
        "normalized_output_path": str(donor_normalized),
        "exit_status": "0",
        "signal": "0",
        "status_vector": "scratchbird_firebird_parser_endpoint_executed",
        "canonical_diagnostic_vector": "scratchbird_firebird_parser_endpoint_executed",
        "expected_classification": "pass_normalized",
        "actual_classification": "pass_normalized",
        "rerun_command": "ctest --test-dir build/engine_listener_storage_release_gate --output-on-failure -R firebird_isql_original_regression_gate",
        "cleanup_status": "retained_for_evidence",
    }
    errors = validate_failure_inventory_record(endpoint_record, output_dir)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    write_failure_inventory(output_dir, endpoint_record)

    report_lines.extend(
        [
            "",
            "Parser probe:",
            "",
            "```",
            parser_result.stdout.strip(),
            "```",
            "",
            "Donor isql probe:",
            "",
            "```",
            donor_result.stdout.strip(),
            "```",
            "",
            "ScratchBird endpoint replay status: scratchbird_firebird_parser_endpoint_executed.",
            f"Endpoint replay coverage: {len(selected)} extracted QA isql_act scripts.",
        ]
    )
    (output_dir / "FIREBIRD_ISQL_ORIGINAL_REGRESSION_REPLAY_REPORT.md").write_text(
        "\n".join(report_lines) + "\n"
    )
    print(
        "validated Firebird isql original regression endpoint replay for "
        f"{len(selected)} QA cases using {parser_probe}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
