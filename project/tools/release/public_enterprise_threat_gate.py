#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the public enterprise threat-model and abuse-case suite."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any


# PUBLIC_ENTERPRISE_THREAT_GATE

REQUIRED_CATEGORIES = {
    "auth_bypass",
    "catalog_corruption",
    "privilege_escalation",
    "parser_boundary_abuse",
    "parser_worker_compromise",
    "management_socket_abuse",
    "handoff_race",
    "malformed_database_open",
    "replay_attack",
    "sblr_tampering",
    "uuid_spoofing",
    "policy_rollback",
    "support_bundle_leakage",
    "cluster_boundary_bypass",
    "recovery_authority_spoofing",
    "backup_archive_misuse",
    "downgrade_attack",
}

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/" + "dcalford",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

DIAGNOSTIC_RE = re.compile(r"^[A-Z0-9][A-Z0-9_.:-]*[A-Z0-9]$")


def fail(message: str) -> None:
    print(f"public_enterprise_threat_gate=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def reject_private_references_recursive(value: Any, context: str) -> None:
    if isinstance(value, str):
        reject_private_reference(value, context)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            reject_private_references_recursive(item, f"{context}[{index}]")
    elif isinstance(value, dict):
        for key, item in value.items():
            reject_private_reference(str(key), f"{context}.key")
            reject_private_references_recursive(item, f"{context}.{key}")


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def read_text(path: Path, repo_root: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{path.relative_to(repo_root).as_posix()}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{path.relative_to(repo_root).as_posix()}:{exc}")


def load_suite(path: Path) -> dict[str, Any]:
    try:
        suite = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"suite_json_invalid:{exc}")
    if not isinstance(suite, dict):
        fail("suite_root_not_object")
    reject_private_references_recursive(suite, "suite")
    return suite


def validate_suite_header(suite: dict[str, Any]) -> None:
    require(suite.get("schema_version") == 1, "suite_schema_version_invalid")
    require(suite.get("marker") == "ENTERPRISE_THREAT_ABUSE_SUITE", "suite_marker_missing")
    require(suite.get("authority") == "public_release_evidence_only", "suite_authority_invalid")
    policy = suite.get("policy")
    require(isinstance(policy, dict), "suite_policy_missing")
    require(policy.get("expected_result") == "fail_closed", "suite_policy_not_fail_closed")
    require(policy.get("stable_diagnostics_required") is True, "suite_requires_stable_diagnostics_false")
    require(policy.get("engine_authority_required") is True, "suite_engine_authority_missing")
    require(policy.get("parser_sql_text_authority") is False, "suite_parser_authority_drift")
    require(policy.get("uuid_identity_authority") == "engine_uuid_v7", "suite_uuid_authority_drift")
    require(policy.get("mga_recovery_authority") is True, "suite_mga_authority_drift")
    require(policy.get("cluster_public_execution") is False, "suite_cluster_execution_drift")
    require(policy.get("support_bundle_secret_leakage") is False, "suite_support_bundle_leakage_drift")


def source_record(repo_root: Path, evidence: dict[str, Any], case_id: str) -> dict[str, Any]:
    path_text = evidence.get("path")
    tokens = evidence.get("tokens")
    require(isinstance(path_text, str) and path_text, f"evidence_path_invalid:{case_id}")
    require(isinstance(tokens, list) and tokens, f"evidence_tokens_empty:{case_id}:{path_text}")
    reject_private_reference(path_text, f"evidence_path:{case_id}")
    path = repo_root / path_text
    require(path.is_file(), f"evidence_file_missing:{case_id}:{path_text}")
    text = read_text(path, repo_root)
    token_digests: list[str] = []
    for token in tokens:
        require(isinstance(token, str) and token, f"evidence_token_invalid:{case_id}:{path_text}")
        reject_private_reference(token, f"evidence_token:{case_id}:{path_text}")
        if token not in text:
            fail(f"evidence_token_missing:{case_id}:{path_text}:{token}")
        token_digests.append(sha256_text(token))
    return {
        "path": path_text,
        "token_count": len(tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
    }


def validate_case(repo_root: Path, case: dict[str, Any]) -> dict[str, Any]:
    case_id = case.get("case_id")
    category = case.get("category")
    require(isinstance(case_id, str) and case_id, "case_id_invalid")
    require(isinstance(category, str) and category, f"case_category_invalid:{case_id}")
    require(category in REQUIRED_CATEGORIES, f"case_category_unknown:{case_id}:{category}")
    require(case.get("expected_result") == "fail_closed", f"case_not_fail_closed:{case_id}")

    diagnostics = case.get("diagnostic_codes")
    require(isinstance(diagnostics, list) and diagnostics, f"case_diagnostics_empty:{case_id}")
    diagnostic_set: set[str] = set()
    for diagnostic in diagnostics:
        require(isinstance(diagnostic, str) and diagnostic, f"diagnostic_invalid:{case_id}")
        reject_private_reference(diagnostic, f"diagnostic:{case_id}")
        require(DIAGNOSTIC_RE.match(diagnostic) is not None, f"diagnostic_unstable_shape:{case_id}:{diagnostic}")
        diagnostic_set.add(diagnostic)
    require(len(diagnostic_set) == len(diagnostics), f"diagnostic_duplicate:{case_id}")

    evidence = case.get("evidence")
    require(isinstance(evidence, list) and evidence, f"case_evidence_empty:{case_id}")
    records = [source_record(repo_root, item, case_id) for item in evidence]
    return {
        "case_id": case_id,
        "category": category,
        "expected_result": "fail_closed",
        "diagnostic_count": len(diagnostics),
        "source_count": len(records),
        "source_token_count": sum(record["token_count"] for record in records),
        "diagnostics": sorted(diagnostic_set),
        "sources": records,
    }


def validate_cases(repo_root: Path, suite: dict[str, Any]) -> list[dict[str, Any]]:
    cases = suite.get("cases")
    require(isinstance(cases, list) and cases, "suite_cases_empty")
    seen_case_ids: set[str] = set()
    records: list[dict[str, Any]] = []
    for case in cases:
        require(isinstance(case, dict), "case_not_object")
        record = validate_case(repo_root, case)
        case_id = record["case_id"]
        require(case_id not in seen_case_ids, f"case_duplicate:{case_id}")
        seen_case_ids.add(case_id)
        records.append(record)

    categories = {record["category"] for record in records}
    missing = sorted(REQUIRED_CATEGORIES - categories)
    extra = sorted(categories - REQUIRED_CATEGORIES)
    require(not missing, "required_categories_missing:" + ",".join(missing))
    require(not extra, "unexpected_categories:" + ",".join(extra))
    return records


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "case_id",
                "category",
                "expected_result",
                "diagnostic_count",
                "source_count",
                "source_token_count",
            ],
        )
        writer.writeheader()
        for record in records:
            writer.writerow(
                {
                    "case_id": record["case_id"],
                    "category": record["category"],
                    "expected_result": record["expected_result"],
                    "diagnostic_count": record["diagnostic_count"],
                    "source_count": record["source_count"],
                    "source_token_count": record["source_token_count"],
                }
            )


def build_evidence(repo_root: Path, suite_path: Path, csv_output: Path) -> dict[str, Any]:
    reject_private_reference(suite_path.relative_to(repo_root).as_posix(), "suite_path")
    suite = load_suite(suite_path)
    validate_suite_header(suite)
    records = validate_cases(repo_root, suite)
    write_csv(csv_output, records)
    summary_text = json.dumps(records, sort_keys=True, separators=(",", ":"))
    return {
        "schema_version": 1,
        "marker": "PUBLIC_ENTERPRISE_THREAT_GATE",
        "gate": "PCR-GATE-134",
        "status": "pass",
        "suite": {
            "path": suite_path.relative_to(repo_root).as_posix(),
            "suite_id": suite["suite_id"],
            "case_count": len(records),
            "category_count": len({record["category"] for record in records}),
            "source_reference_count": sum(record["source_count"] for record in records),
            "source_token_count": sum(record["source_token_count"] for record in records),
            "stable_diagnostic_count": sum(record["diagnostic_count"] for record in records),
        },
        "policy": {
            "public_tree_inputs_only": True,
            "fail_closed_required": True,
            "stable_diagnostics_required": True,
            "engine_authority_required": True,
            "parser_sql_text_authority": False,
            "uuid_identity_authority": "engine_uuid_v7",
            "mga_recovery_authority": True,
            "cluster_public_execution": False,
            "support_bundle_secret_leakage": False,
        },
        "coverage": records,
        "coverage_sha256": sha256_text(summary_text),
        "csv_output": csv_output.relative_to(repo_root).as_posix()
        if repo_root in csv_output.resolve().parents or csv_output.resolve() == repo_root
        else csv_output.name,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--suite", type=Path, default=None)
    parser.add_argument("--csv-output", type=Path, required=True)
    parser.add_argument("--evidence-output", type=Path, required=True)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    project_root = args.project_root.resolve()
    require(repo_root.is_dir(), "repo_root_missing")
    require(project_root.is_dir(), "project_root_missing")

    suite_path = args.suite.resolve() if args.suite else project_root / "tests" / "security" / "enterprise_threat_abuse_suite.json"
    evidence = build_evidence(repo_root, suite_path, args.csv_output.resolve())
    args.evidence_output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.evidence_output.resolve().write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        "public_enterprise_threat_gate=passed "
        f"cases={evidence['suite']['case_count']} "
        f"categories={evidence['suite']['category_count']} "
        f"sha256={evidence['coverage_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
