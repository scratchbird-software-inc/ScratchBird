#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate beta driver wire transcript oracle fixtures."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


ORACLE_REL = Path("project/tests/conformance/drivers/corpora/shared_beta_driver_corpus/wire_transcript_oracles.json")
REPORT_REL = Path("build/reports/driver_wire_transcript_oracle.json")
FORBIDDEN_FRAMES = {
    "DriverCommitFinality",
    "DriverRollbackFinality",
    "ParserSqlExecutionAuthority",
}
REQUIRED_COVERAGE = {
    "wire_transcript_oracle",
    "sblr_uuid_server_revalidation",
    "auth_context_injection",
    "cross_user_replay_refusal",
    "role_group_replay_refusal",
    "authorization_filtered_uuid_path_resolution",
}


def repo_root_from_script() -> Path:
    return Path(__file__).resolve().parents[4]


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def validate_oracles(doc: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if doc.get("corpus_id") != "wire_transcript_oracles":
        errors.append("wire_transcript_oracles:corpus_id_mismatch")
    boundary = str(doc.get("default_expected_boundary", ""))
    if "server-owned admission/revalidation" not in boundary:
        errors.append("wire_transcript_oracles:missing_server_owned_boundary")
    if "driver- or parser-owned transaction finality" not in boundary:
        errors.append("wire_transcript_oracles:missing_no_driver_parser_finality_boundary")

    coverage: set[str] = set()
    oracle_ids: set[str] = set()
    for oracle in as_list(doc.get("oracles")):
        if not isinstance(oracle, dict):
            errors.append("wire_transcript_oracles:oracle_not_object")
            continue
        case_id = str(oracle.get("case_id", ""))
        if not case_id.startswith("BDRV-WIRE-"):
            errors.append(f"{case_id or '<missing>'}:invalid_case_id")
        if case_id in oracle_ids:
            errors.append(f"{case_id}:duplicate_case_id")
        oracle_ids.add(case_id)
        if oracle.get("server_revalidation") != "required":
            errors.append(f"{case_id}:server_revalidation_not_required")
        coverage.update(str(item) for item in as_list(oracle.get("coverage")))
        forbidden = set(str(item) for item in as_list(oracle.get("forbidden_frames")))
        if oracle.get("category") == "wire_transcript_oracle" and not FORBIDDEN_FRAMES <= forbidden:
            errors.append(f"{case_id}:missing_forbidden_finality_frames")
        steps = [step for step in as_list(oracle.get("steps")) if isinstance(step, dict)]
        if not steps:
            errors.append(f"{case_id}:missing_steps")
        if not any(step.get("direction") == "server_to_client" for step in steps):
            errors.append(f"{case_id}:missing_server_to_client_step")
        for step in steps:
            frame = str(step.get("frame", ""))
            if frame in FORBIDDEN_FRAMES:
                errors.append(f"{case_id}:forbidden_frame_present:{frame}")
            assertions = step.get("assertions", {})
            if isinstance(assertions, dict) and assertions.get("driver_finality_authority") is True:
                errors.append(f"{case_id}:driver_finality_authority_true")

    missing = REQUIRED_COVERAGE - coverage
    if missing:
        errors.append("wire_transcript_oracles:missing_coverage:" + ",".join(sorted(missing)))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=repo_root_from_script())
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    repo_root = args.repo_root.resolve()
    errors: list[str] = []
    try:
        doc = load_json(repo_root / ORACLE_REL)
        errors.extend(validate_oracles(doc))
    except (OSError, json.JSONDecodeError) as exc:
        errors.append(f"wire_transcript_oracles:load_failed:{exc}")
    report = {
        "command": "driver_wire_transcript_oracle.py",
        "status": "fail" if errors else "pass",
        "oracle_path": str(ORACLE_REL),
        "issues": errors,
    }
    output = args.output or repo_root / REPORT_REL
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"driver_wire_transcript_oracle={report['status']}")
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
