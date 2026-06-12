#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Enterprise completion truth gate for first-tranche compatibility SQL parsers.

This gate is deliberately stricter than a file-presence manifest check and
deliberately separate from the final release claim.  Normal CTest execution
regenerates a blocker inventory and fails if FirebirdSQL, MySQL, or PostgreSQL
parser work is represented as complete while row-level execution_plan or project-test
evidence still contains pending, blocked, seed, or generated-only states.

Use --strict-release for the final release-candidate run.  In strict mode any
open blocker fails the gate.
"""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from typing import Iterable


DOCS_ROOT = "docs"
EXECUTION_PLAN_ROOT = DOCS_ROOT + "/" + "execution-plans"
COMPLETED_EXECUTION_PLAN_ROOT = DOCS_ROOT + "/" + "completed-execution-plans"
PROCEDURAL_DATATYPE_EXECUTION_PLAN = (
    EXECUTION_PLAN_ROOT + "/" + "compatibility-parser-procedural-datatype-enterprise-closure"
)


DIALECTS = {
    "firebird": {
        "display": "FirebirdSQL",
        "native_tool_manifest": "project/tests/reference_regression/firebird/native_tool_harness/native_tool_harness_manifest.csv",
        "execution-plans": (
            EXECUTION_PLAN_ROOT + "/compatibility-parser-firebird-implementation-readiness",
            EXECUTION_PLAN_ROOT + "/full-firebirdsql-parser-udr-emulation-implementation-closure",
        ),
        "completed_execution-plans": (
            COMPLETED_EXECUTION_PLAN_ROOT + "/compatibility-parser-firebird-implementation-readiness",
            COMPLETED_EXECUTION_PLAN_ROOT + "/full-firebirdsql-parser-udr-emulation-implementation-closure",
        ),
        "project_tests": (
            "project/tests/reference_regression/firebird",
            "project/tests/firebird_parser_worker/fixtures/full_firebirdsql_parser_udr_emulation_closure",
        ),
    },
    "mysql": {
        "display": "MySQL",
        "native_tool_manifest": "project/tests/reference_regression/mysql/native_tool_harness/native_tool_harness_manifest.csv",
        "execution-plans": (
            EXECUTION_PLAN_ROOT + "/compatibility-parser-mysql-implementation-readiness",
        ),
        "completed_execution-plans": (
            COMPLETED_EXECUTION_PLAN_ROOT + "/compatibility-parser-mysql-implementation-readiness",
        ),
        "project_tests": (
            "project/tests/reference_regression/mysql",
        ),
    },
    "postgresql": {
        "display": "PostgreSQL",
        "native_tool_manifest": "project/tests/reference_regression/postgresql/native_tool_harness/native_tool_harness_manifest.csv",
        "execution-plans": (
            EXECUTION_PLAN_ROOT + "/compatibility-parser-postgresql-implementation-readiness",
        ),
        "completed_execution-plans": (
            COMPLETED_EXECUTION_PLAN_ROOT + "/compatibility-parser-postgresql-implementation-readiness",
        ),
        "project_tests": (
            "project/tests/reference_regression/postgresql",
        ),
    },
}

STATUS_COLUMNS = {
    "status",
    "source_status",
    "source_review_status",
    "official_resource_status",
    "implementation_status",
    "completion_status",
    "gate_status",
    "proof_status",
}

BLOCKING_STATUS_TOKENS = (
    "pending",
    "blocked",
    "blocking",
    "ready_blocked",
    "ready_for_preimplementation",
    "generated_pending",
    "manifest_generated",
    "seed",
    "assumed",
    "active",
    "no_go",
    "not_started",
    "in_progress",
)

GLOBAL_ENTERPRISE_BLOCKER_TOKENS = (
    "active",
    "pending",
    "not_proven",
    "not_enterprise",
    "proof_pending",
    "route_only",
    "descriptor_only",
    "partial_descriptor_coverage",
    "current_route_or_descriptor_only",
)

NON_BLOCKING_STATUSES = {
    "available",
    "verified_present",
    "source_reviewed_from_recorded_compatibility_release_packet",
    "official_resource_reviewed_or_recorded_for_reacquisition",
    "completed",
    "complete",
    "passed",
    "passing",
    "accepted",
    "closed",
}

COMPLETION_STATUSES = {
    "completed",
    "complete",
    "passed",
    "passing",
    "accepted",
    "closed",
}

CSV_FALSE_COMPLETION_STATUSES = {
    "enterprise_completion_proof_passed",
    "gold_completion_proof_passed",
    "enterprise_release_ready",
    "gold_release_ready",
}

CSV_CONTEXTUAL_FALSE_COMPLETION_STATUSES = {
    "completed",
    "complete",
    "passed",
    "passing",
    "accepted",
    "closed",
    "variance_decision_proof_passed",
}

CSV_ENTERPRISE_CLAIM_PHRASES = (
    "enterprise parser operation",
    "enterprise operation",
    "enterprise completion proof",
    "enterprise release",
    "gold release",
    "observable equivalence",
    "observable compatibility equivalence",
    "semantic defaults observable equivalence",
    "procedural datatype",
    "procedural/API encoding",
    "exact datatypes",
    "compatibility-native proof behavior",
)

NATIVE_REPLAY_COMPLETION_STATUSES = {
    "native_replay_passed",
    "original_compatibility_replay_passed",
    "original_compatibility_tool_replay_passed",
    "passed",
}

COMPLETION_CLAIM_PATTERNS = (
    re.compile(r"\benterprise[-_ ]release[-_ ]ready\s*[:=]\s*true\b", re.IGNORECASE),
    re.compile(r"\bgold[-_ ]standard[-_ ]ready\s*[:=]\s*true\b", re.IGNORECASE),
    re.compile(r"\bstatus\s*:\s*(completed|complete|passed|accepted)\b", re.IGNORECASE),
    re.compile(r"\bgo\s*:\s*(yes|approved|release)\b", re.IGNORECASE),
)
EXTERNAL_REFERENCE_SKIP_CODE = 77


@dataclass(frozen=True)
class Blocker:
    dialect: str
    source: str
    row_id: str
    column: str
    status: str

    def as_json(self) -> dict[str, str]:
        return {
            "dialect": self.dialect,
            "source": self.source,
            "row_id": self.row_id,
            "column": self.column,
            "status": self.status,
        }


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise AssertionError(f"{path}: missing CSV header")
        rows: list[dict[str, str]] = []
        for row in reader:
            row.pop(None, None)
            rows.append({key: value for key, value in row.items()})
    return rows


def is_status_column(column: str) -> bool:
    lower = column.lower()
    return lower in STATUS_COLUMNS or lower.endswith("_status")


def normalize_status(value: str) -> str:
    return value.strip().lower()


def is_blocking_status(value: str) -> bool:
    status = normalize_status(value)
    if not status or status in NON_BLOCKING_STATUSES:
        return False
    return any(token in status for token in BLOCKING_STATUS_TOKENS)


def row_identifier(row: dict[str, str], fallback: int) -> str:
    for column in (
        "gate_id",
        "tracker_id",
        "task_id",
        "proof_id",
        "row_id",
        "spec_id",
        "audit_id",
        "variance_id",
        "lane_id",
        "item_id",
        "manifest_id",
        "evidence_id",
        "test_id",
        "requirement_id",
        "surface_id",
        "operation_id",
        "dependency_id",
        "agent_id",
        "family_id",
        "compatibility_id",
        "name",
    ):
        value = row.get(column, "").strip()
        if value:
            return value
    return f"row-{fallback}"


def iter_csv_files(repo_root: pathlib.Path, roots: Iterable[str]) -> Iterable[pathlib.Path]:
    for rel in roots:
        root = repo_root / rel
        if root.is_file() and root.suffix.lower() == ".csv":
            yield root
        elif root.is_dir():
            yield from sorted(root.rglob("*.csv"))


def collect_blockers(repo_root: pathlib.Path,
                     dialect: str,
                     roots: Iterable[str]) -> list[Blocker]:
    blockers: list[Blocker] = []
    for path in iter_csv_files(repo_root, roots):
        rel_path = path.relative_to(repo_root).as_posix()
        for index, row in enumerate(read_csv(path), start=1):
            row_id = row_identifier(row, index)
            for column, value in row.items():
                if is_status_column(column) and is_blocking_status(value):
                    blockers.append(
                        Blocker(
                            dialect=dialect,
                            source=rel_path,
                            row_id=row_id,
                            column=column,
                            status=value.strip(),
                        )
                    )
    return blockers


def is_global_enterprise_blocker_status(value: str) -> bool:
    status = normalize_status(value)
    if not status or status in NON_BLOCKING_STATUSES:
        return False
    return any(token in status for token in GLOBAL_ENTERPRISE_BLOCKER_TOKENS)


def collect_global_enterprise_blockers(repo_root: pathlib.Path) -> list[Blocker]:
    blockers: list[Blocker] = []
    root = repo_root / PROCEDURAL_DATATYPE_EXECUTION_PLAN
    if not root.is_dir():
        blockers.append(
            Blocker(
                dialect="first_tranche_global",
                source=PROCEDURAL_DATATYPE_EXECUTION_PLAN,
                row_id="compatibility-parser-procedural-datatype-enterprise-closure",
                column="execution_plan",
                status="missing_global_enterprise_blocker_execution_plan",
            )
        )
        return blockers

    for path in sorted(root.glob("*.csv")):
        rel_path = path.relative_to(repo_root).as_posix()
        for index, row in enumerate(read_csv(path), start=1):
            row_id = row_identifier(row, index)
            for column, value in row.items():
                if (
                    is_status_column(column)
                    and is_global_enterprise_blocker_status(value)
                ):
                    blockers.append(
                        Blocker(
                            dialect="first_tranche_global",
                            source=rel_path,
                            row_id=row_id,
                            column=column,
                            status=value.strip(),
                        )
                    )
    return blockers


def collect_runtime_replay_blockers(
    first_tranche_replay: dict[str, object],
) -> list[Blocker]:
    if not first_tranche_replay.get("required"):
        return []
    if first_tranche_replay.get("enterprise_release_ready_from_runtime_evidence") is True:
        return []

    raw_counts = first_tranche_replay.get("replay_counts_by_dialect", {})
    counts = raw_counts if isinstance(raw_counts, dict) else {}
    total = int(first_tranche_replay.get("runtime_enterprise_blocker_count", 0) or 0)
    if total <= 0:
        total = sum(int(value) for value in counts.values()) if counts else 1

    blockers: list[Blocker] = []
    if counts:
        for dialect, count in sorted(counts.items()):
            if int(count) <= 0:
                continue
            blockers.append(
                Blocker(
                    dialect="first_tranche_global",
                    source=str(
                        first_tranche_replay.get(
                            "evidence_file",
                            "first_tranche_replay",
                        )
                    ),
                    row_id=f"{dialect}_runtime_enterprise_readiness",
                    column="enterprise_release_ready_from_runtime_evidence",
                    status="not_enterprise_ready",
                )
            )
    else:
        blockers.append(
            Blocker(
                dialect="first_tranche_global",
                source=str(
                    first_tranche_replay.get("evidence_file", "first_tranche_replay")
                ),
                row_id="runtime_enterprise_readiness",
                column="runtime_enterprise_blocker_count",
                status=f"not_enterprise_ready:{total}",
            )
        )
    return blockers


def collect_status_counts(repo_root: pathlib.Path,
                          roots: Iterable[str]) -> dict[str, int]:
    counts: Counter[str] = Counter()
    for path in iter_csv_files(repo_root, roots):
        for row in read_csv(path):
            for column, value in row.items():
                if is_status_column(column) and value.strip():
                    counts[f"{column}={value.strip()}"] += 1
    return dict(sorted(counts.items()))


def collect_native_tool_replay(repo_root: pathlib.Path,
                               dialect: str,
                               manifest_rel: str) -> dict[str, object]:
    path = repo_root / manifest_rel
    if not path.is_file():
        return {
            "dialect": dialect,
            "manifest": manifest_rel,
            "original_compatibility_tools_required": True,
            "manifest_present": False,
            "replay_completed": False,
            "reason": "missing_native_tool_manifest",
        }

    rows = read_csv(path)
    status_counts: Counter[str] = Counter()
    missing_tool_locators: list[str] = []
    missing_endpoint_contracts: list[str] = []
    missing_required_outputs: list[str] = []
    incomplete_replay_rows: list[str] = []

    for index, row in enumerate(rows, start=1):
        row_id = row_identifier(row, index)
        status = normalize_status(row.get("status", ""))
        status_counts[status] += 1
        if status not in NATIVE_REPLAY_COMPLETION_STATUSES:
            incomplete_replay_rows.append(row_id)
        locator = row.get("tool_locator", "").strip()
        if locator.startswith("docs/") and not (repo_root / locator).exists():
            missing_tool_locators.append(f"{row_id}:{locator}")
        if not row.get("required_endpoint_env", "").strip():
            missing_endpoint_contracts.append(row_id)
        required_output = row.get("required_output", "").strip()
        if not required_output:
            missing_required_outputs.append(row_id)

    replay_completed = (
        bool(rows)
        and not incomplete_replay_rows
        and not missing_tool_locators
        and not missing_endpoint_contracts
        and not missing_required_outputs
    )
    return {
        "dialect": dialect,
        "manifest": manifest_rel,
        "original_compatibility_tools_required": True,
        "manifest_present": True,
        "tool_row_count": len(rows),
        "replay_completed": replay_completed,
        "status_counts": dict(sorted(status_counts.items())),
        "incomplete_replay_rows": incomplete_replay_rows[:25],
        "missing_tool_locators": missing_tool_locators[:25],
        "missing_endpoint_contract_rows": missing_endpoint_contracts[:25],
        "missing_required_output_rows": missing_required_outputs[:25],
    }


def collect_first_tranche_replay_evidence(path: pathlib.Path | None) -> dict[str, object]:
    if path is None:
        return {
            "required": False,
            "present": False,
            "passed": False,
            "reason": "not_requested",
        }
    if not path.is_file():
        raise AssertionError(f"first-tranche original-tool replay evidence missing: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("gate") != "compatibility_sql_first_tranche_original_tool_replay_gate":
        raise AssertionError(f"unexpected first-tranche replay gate evidence: {path}")
    if data.get("regular_ctest_gate") is not True:
        raise AssertionError("first-tranche replay evidence must come from a regular CTest gate")
    if data.get("compatibility_tools_are_storage_authority") is not False:
        raise AssertionError("first-tranche replay evidence gives compatibility tools storage authority")
    if data.get("compatibility_tools_are_transaction_authority") is not False:
        raise AssertionError("first-tranche replay evidence gives compatibility tools transaction authority")
    if data.get("external_reference_fixture_status") == "missing":
        return {
            "required": True,
            "present": True,
            "passed": False,
            "reason": str(data.get("skip_reason", "external_reference_fixture_missing")),
            "evidence_file": str(path),
            "external_reference_fixture_required": True,
            "external_reference_fixture_status": "missing",
            "missing_external_tool_count": data.get("missing_external_tool_count", 0),
            "missing_external_tools": data.get("missing_external_tools", []),
            "replay_case_count": 0,
            "replay_counts_by_dialect": {},
            "staged_tool_count": 0,
            "tool_staging_root": data.get("tool_staging_root", ""),
            "authority_rule": data.get("parser_authority_rule", ""),
            "enterprise_release_ready_from_runtime_evidence": False,
            "runtime_enterprise_blocker_count": data.get(
                "runtime_enterprise_blocker_count", 1
            ),
            "runtime_enterprise_blocker_reason": data.get(
                "runtime_enterprise_blocker_reason",
                "external reference fixture missing",
            ),
        }
    counts = data.get("replay_counts_by_dialect", {})
    if not isinstance(counts, dict):
        raise AssertionError("first-tranche replay evidence has malformed dialect counts")
    missing = [dialect for dialect in DIALECTS if int(counts.get(dialect, 0)) <= 0]
    if missing:
        raise AssertionError(
            "first-tranche replay evidence missing dialect cases: " + ", ".join(missing)
        )
    staged_tools = data.get("staged_tools", [])
    if not isinstance(staged_tools, list) or len(staged_tools) < 8:
        raise AssertionError("first-tranche replay evidence missing staged original tools")
    return {
        "required": True,
        "present": True,
        "passed": True,
        "evidence_file": str(path),
        "replay_case_count": data.get("replay_case_count", 0),
        "replay_counts_by_dialect": counts,
        "staged_tool_count": len(staged_tools),
        "tool_staging_root": data.get("tool_staging_root", ""),
        "authority_rule": data.get("parser_authority_rule", ""),
        "enterprise_release_ready_from_runtime_evidence": data.get(
            "enterprise_release_ready_from_runtime_evidence", False
        ),
        "runtime_enterprise_blocker_count": data.get(
            "runtime_enterprise_blocker_count", data.get("replay_case_count", 0)
        ),
        "runtime_enterprise_blocker_reason": data.get(
            "runtime_enterprise_blocker_reason",
            "runtime replay evidence does not claim enterprise release readiness",
        ),
    }


def text_claims_completion(path: pathlib.Path) -> bool:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return False
    return any(pattern.search(text) for pattern in COMPLETION_CLAIM_PATTERNS)


def row_text(row: dict[str, str]) -> str:
    values: list[str] = []
    for column, value in row.items():
        lower_column = column.lower()
        if "path" in lower_column or "evidence" in lower_column:
            continue
        if value.strip():
            values.append(value.strip())
    return " ".join(values)


def row_context_claims_enterprise_completion(row: dict[str, str]) -> bool:
    text = row_text(row).lower()
    text = re.sub(r"[_/.-]+", " ", text)
    return any(
        re.sub(r"[_/.-]+", " ", phrase.lower()) in text
        for phrase in CSV_ENTERPRISE_CLAIM_PHRASES
    )


def csv_row_claims_completion(row: dict[str, str]) -> tuple[str, str] | None:
    for column, value in row.items():
        if not is_status_column(column):
            continue
        status = normalize_status(value)
        if not status:
            continue
        if status in CSV_FALSE_COMPLETION_STATUSES:
            return column, value.strip()
        if (
            status in CSV_CONTEXTUAL_FALSE_COMPLETION_STATUSES
            and row_context_claims_enterprise_completion(row)
        ):
            return column, value.strip()
    return None


def collect_csv_completion_claims(repo_root: pathlib.Path,
                                  roots: Iterable[str]) -> list[str]:
    claims: list[str] = []
    for path in iter_csv_files(repo_root, roots):
        rel_path = path.relative_to(repo_root).as_posix()
        for index, row in enumerate(read_csv(path), start=1):
            claim = csv_row_claims_completion(row)
            if claim is None:
                continue
            column, status = claim
            claims.append(
                f"{rel_path}:{row_identifier(row, index)}:{column}={status}"
            )
    return claims


def collect_completion_claims(repo_root: pathlib.Path,
                              dialect: str,
                              spec: dict[str, tuple[str, ...]]) -> list[str]:
    claims: list[str] = []
    for rel in spec["completed_execution-plans"]:
        path = repo_root / rel
        if path.exists():
            claims.append(rel)
    claims.extend(collect_csv_completion_claims(repo_root, spec["execution-plans"]))
    for rel in spec["execution-plans"]:
        root = repo_root / rel
        if not root.exists():
            continue
        for path in sorted(root.rglob("*")):
            if path.is_dir():
                continue
            rel_path = path.relative_to(repo_root).as_posix()
            if path.suffix.lower() in {".md", ".txt", ".json"}:
                if text_claims_completion(path):
                    claims.append(rel_path)
    return claims


def validate_required_roots(repo_root: pathlib.Path) -> None:
    missing: list[str] = []
    for spec in DIALECTS.values():
        for rel in (*spec["execution-plans"], *spec["project_tests"]):
            if not (repo_root / rel).exists():
                missing.append(rel)
    if missing:
        raise AssertionError("missing required parser completion evidence roots: " +
                             ", ".join(missing))


def build_evidence(repo_root: pathlib.Path,
                   strict_release: bool,
                   first_tranche_replay_evidence: pathlib.Path | None) -> dict[str, object]:
    validate_required_roots(repo_root)
    first_tranche_replay = collect_first_tranche_replay_evidence(
        first_tranche_replay_evidence
    )
    global_blockers = collect_global_enterprise_blockers(repo_root)
    runtime_replay_blockers = collect_runtime_replay_blockers(first_tranche_replay)
    dialect_evidence: list[dict[str, object]] = []
    all_blockers: list[Blocker] = []
    completion_claims: dict[str, list[str]] = {}

    for dialect, spec in DIALECTS.items():
        roots = (*spec["execution-plans"], *spec["project_tests"])
        blockers = collect_blockers(repo_root, dialect, roots)
        claims = collect_completion_claims(repo_root, dialect, spec)
        native_tool_replay = collect_native_tool_replay(
            repo_root, dialect, spec["native_tool_manifest"]
        )
        all_blockers.extend(blockers)
        completion_claims[dialect] = claims

        source_counts: dict[str, int] = defaultdict(int)
        status_counts = collect_status_counts(repo_root, roots)
        for blocker in blockers:
            source_counts[blocker.source] += 1

        dialect_evidence.append(
            {
                "dialect": dialect,
                "display": spec["display"],
                "enterprise_release_ready": len(blockers) == 0,
                "blocker_count": len(blockers),
                "completion_claims": claims,
                "blockers_by_source": dict(sorted(source_counts.items())),
                "status_counts": status_counts,
                "original_compatibility_tool_replay": native_tool_replay,
                "sample_blockers": [blocker.as_json() for blocker in blockers[:25]],
            }
        )

    false_claims = [
        {"dialect": dialect, "claims": claims}
        for dialect, claims in completion_claims.items()
        if claims and any(blocker.dialect == dialect for blocker in all_blockers)
    ]
    native_replay_incomplete = [
        {
            "dialect": item["dialect"],
            "manifest": item["original_compatibility_tool_replay"]["manifest"],
            "incomplete_replay_rows": item["original_compatibility_tool_replay"].get(
                "incomplete_replay_rows", []
            ),
            "missing_tool_locators": item["original_compatibility_tool_replay"].get(
                "missing_tool_locators", []
            ),
        }
        for item in dialect_evidence
        if not item["original_compatibility_tool_replay"]["replay_completed"]
    ]
    all_blockers.extend(global_blockers)
    all_blockers.extend(runtime_replay_blockers)

    return {
        "gate": "compatibility_sql_parser_enterprise_completion_truth_gate",
        "strict_release": strict_release,
        "enterprise_release_ready": len(all_blockers) == 0,
        "file_presence_is_completion": False,
        "generated_manifest_is_completion": False,
        "original_compatibility_tool_replay_required": True,
        "first_tranche_original_tool_replay": first_tranche_replay,
        "global_enterprise_blocker_count": len(global_blockers),
        "runtime_replay_enterprise_blocker_count": len(runtime_replay_blockers),
        "sample_global_enterprise_blockers": [
            blocker.as_json() for blocker in global_blockers[:25]
        ],
        "sample_runtime_replay_enterprise_blockers": [
            blocker.as_json() for blocker in runtime_replay_blockers[:25]
        ],
        "execution_plan_move_requires_zero_blockers": True,
        "project_test_evidence_required": True,
        "blocker_count": len(all_blockers),
        "false_completion_claims": false_claims,
        "native_replay_incomplete": native_replay_incomplete,
        "dialects": dialect_evidence,
    }


def write_evidence(path: pathlib.Path, evidence: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def write_private_packet_skip_evidence(path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "gate": "compatibility_sql_parser_enterprise_completion_truth_gate",
        "status": "skipped",
        "skip_reason": "external_public_execution_plan_packet_not_installed",
        "required_packet": "public_execution_plan",
        "workplan_storage": "private workplan repository",
        "public_repo_policy": "workplans_are_not_tracked_in_scratchbird_public_repo",
        "enterprise_release_ready": False,
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n",
                    encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--evidence-file", required=True, type=pathlib.Path)
    parser.add_argument("--first-tranche-replay-evidence", type=pathlib.Path)
    parser.add_argument("--strict-release", action="store_true")
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    if not (repo_root / "public_execution_plan").is_dir():
        write_private_packet_skip_evidence(args.evidence_file)
        print("compatibility_sql_parser_enterprise_completion_truth_gate=skipped external_public_execution_plan_packet_not_installed")
        return EXTERNAL_REFERENCE_SKIP_CODE
    replay_evidence = (
        args.first_tranche_replay_evidence.resolve()
        if args.first_tranche_replay_evidence is not None
        else None
    )
    evidence = build_evidence(repo_root, args.strict_release, replay_evidence)
    write_evidence(args.evidence_file, evidence)

    false_claims = evidence["false_completion_claims"]
    if false_claims:
        raise AssertionError(
            "parser completion is claimed while blockers remain: " +
            json.dumps(false_claims, sort_keys=True)
        )
    if args.strict_release and evidence["blocker_count"] != 0:
        raise AssertionError(
            f"strict release mode found {evidence['blocker_count']} open blockers"
        )
    if args.strict_release and evidence["native_replay_incomplete"]:
        raise AssertionError(
            "strict release mode requires original compatibility-tool replay completion: " +
            json.dumps(evidence["native_replay_incomplete"], sort_keys=True)
        )

    print(
        "compatibility_sql_parser_enterprise_completion_truth_gate="
        f"{'release_ready' if evidence['enterprise_release_ready'] else 'no_false_completion_claim'} "
        f"blockers={evidence['blocker_count']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except AssertionError as exc:
        print(f"compatibility_sql_parser_enterprise_completion_truth_gate: {exc}",
              file=sys.stderr)
        raise SystemExit(1)
