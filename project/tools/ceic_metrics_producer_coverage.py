#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate CEIC-090 cross-subsystem metrics producer coverage.

SEARCH_KEY: CEIC_090_METRICS_PRODUCER_COVERAGE_GATE
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Iterable


EXECUTION_PLAN = pathlib.Path(
    "project/tests/release_evidence/consolidated_enterprise_public_evidence"
)
DEFAULT_MATRIX = EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv"
CMAKE_GATE = pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt")

REQUIRED_COLUMNS = (
    "metric_family",
    "subsystem",
    "required_producer_path",
    "operation_path",
    "support_bundle_path",
    "validation_gate",
    "status",
)

COMPLETE_STATUS_PREFIXES = (
    "complete",
    "producer_present",
    "schema_complete",
)
PENDING_STATUS_TOKENS = ("pending", "planned", "todo", "future", "blocked")
DESCRIPTOR_ONLY_RE = re.compile(r"\bdescriptor[-_ ]only\b")
STATIC_ONLY_RE = re.compile(r"\bstatic[-_ ]only\b")
PLACEHOLDER_RE = re.compile(r"\bplaceholder\b|sha256:0{8,}|^0{32,}$")
SYNTHETIC_RE = re.compile(r"\bsynthetic\b|\bsimulated production\b|\bfake production\b")
STALE_RE = re.compile(r"\bstale\b|\bout[-_ ]of[-_ ]date\b")
UNSAFE_AUTHORITY_RE = re.compile(
    r"unsafe[-_ ]authority|authority\s*=\s*true|authority_claimed\s*=\s*true|"
    r"grants?\s+\w*\s*authority|becomes?\s+\w*\s*authority"
)
LOCAL_CLUSTER_RE = re.compile(r"\blocal[-_ ]cluster\b|\blocal cluster\b")
SUCCESSOR_RE = re.compile(r"CEIC[-_]?(09[1-5])", re.IGNORECASE)

SAFE_CLUSTER_WORDS = ("fail", "refus", "block", "external-provider-only", "external provider only")
SAFE_SUCCESSOR_WORDS = (
    "pending",
    "remain",
    "not",
    "no ",
    "without",
    "false",
    "blocked",
    "refus",
    "unclaimed",
    "separate",
    "gate",
)

REQUIRED_ARTIFACTS = (
    "CEIC-ART-011",  # CEIC-024 memory readiness manifest
    "CEIC-ART-012",  # CEIC-030/042 index readiness manifest
    "CEIC-ART-014",  # CEIC-085 agent readiness manifest
    "CEIC-ART-018",  # CEIC-090 matrix
    "CEIC-ART-022",
    "CEIC-ART-026",
    "CEIC-ART-027",
    "CEIC-ART-028",
    "CEIC-ART-029",
    "CEIC-ART-030",
    "CEIC-ART-031",
    "CEIC-ART-032",
    "CEIC-ART-033",
    "CEIC-ART-034",
    "CEIC-ART-035",
    "CEIC-ART-036",
    "CEIC-ART-037",
    "CEIC-ART-038",
    "CEIC-ART-039",
    "CEIC-ART-040",
    "CEIC-ART-041",
    "CEIC-ART-042",
    "CEIC-ART-043",
    "CEIC-ART-044",
    "CEIC-ART-055",  # CEIC-040 index operation metrics
    "CEIC-ART-057",  # CEIC-042 index drift gate
    "CEIC-ART-062",  # CEIC-054 optimizer observability
    "CEIC-ART-065",  # CEIC-057 driver-visible explain
    "CEIC-ART-067",  # CEIC-059 optimizer memory feedback
    "CEIC-ART-070",  # CEIC-062 optimizer manifest evidence
    "CEIC-ART-075",  # CEIC-074 agent metric quorum
    "CEIC-ART-084",  # CEIC-083 agent memory-pressure metrics
    "CEIC-ART-086",  # CEIC-085 agent manifest evidence
    "CEIC-ART-087",  # CEIC-090 evidence
)


@dataclass(frozen=True)
class Diagnostic:
    code: str
    metric_family: str
    message: str

    def render(self) -> str:
        subject = self.metric_family or "matrix"
        return f"{subject}:{self.code}:{self.message}"


def normalize(value: str) -> str:
    return " ".join((value or "").strip().lower().split())


def normalize_status(value: str) -> str:
    return normalize(value).replace(" ", "_").replace("-", "_")


def split_refs(value: str) -> list[str]:
    return [part.strip() for part in value.replace(",", ";").split(";") if part.strip()]


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    return [{key: value or "" for key, value in row.items()} for row in rows]


def index_by(rows: Iterable[dict[str, str]], field: str) -> dict[str, dict[str, str]]:
    return {row.get(field, "").strip(): row for row in rows if row.get(field, "").strip()}


def path_has_evidence(repo_root: pathlib.Path, value: str) -> bool:
    raw = value.strip()
    if not raw:
        return False
    path = repo_root / raw
    if any(char in raw for char in "*?["):
        return any(repo_root.glob(raw))
    return path.exists()


def text_blob(row: dict[str, str]) -> str:
    return " ".join(str(row.get(column, "")) for column in REQUIRED_COLUMNS)


def contains_successor_overclaim(text: str) -> bool:
    lowered = text.lower()
    for match in SUCCESSOR_RE.finditer(text):
        start = max(0, match.start() - 80)
        end = min(len(text), match.end() + 80)
        window = lowered[start:end]
        if not any(word in window for word in SAFE_SUCCESSOR_WORDS):
            return True
    return False


def contains_local_cluster_claim(text: str) -> bool:
    lowered = text.lower()
    claim_words = ("production", "ready", "live", "evidence", "claim", "authority")
    for match in LOCAL_CLUSTER_RE.finditer(lowered):
        start = max(0, match.start() - 80)
        end = min(len(lowered), match.end() + 80)
        window = lowered[start:end]
        suffix = lowered[match.start():end]
        if any(word in suffix for word in claim_words) and not any(word in suffix for word in SAFE_CLUSTER_WORDS):
            return True
        if any(word in window for word in claim_words) and not any(word in window for word in SAFE_CLUSTER_WORDS):
            return True
    return False


def validate_row(row: dict[str, str], repo_root: pathlib.Path) -> list[Diagnostic]:
    metric = row.get("metric_family", "").strip()
    diagnostics: list[Diagnostic] = []
    status = normalize_status(row.get("status", ""))
    text = text_blob(row)
    lowered = text.lower()

    if not metric:
        diagnostics.append(Diagnostic("missing_metric_family", metric, "metric_family is required"))
    if not row.get("subsystem", "").strip():
        diagnostics.append(Diagnostic("missing_subsystem", metric, "subsystem is required"))
    if not row.get("operation_path", "").strip():
        diagnostics.append(Diagnostic("missing_operation_path", metric, "operation_path is required"))
    if not row.get("support_bundle_path", "").strip():
        diagnostics.append(Diagnostic("missing_support_bundle_path", metric, "support_bundle_path is required"))
    if not row.get("validation_gate", "").strip():
        diagnostics.append(Diagnostic("missing_validation_gate", metric, "validation_gate is required"))

    if not status or any(token in status for token in PENDING_STATUS_TOKENS):
        diagnostics.append(
            Diagnostic("pending_status", metric, "CEIC-090 producer coverage status must be non-pending")
        )
    elif not status.startswith(COMPLETE_STATUS_PREFIXES):
        diagnostics.append(
            Diagnostic("unsupported_status", metric, f"unsupported producer coverage status: {row.get('status', '')}")
        )
    if SUCCESSOR_RE.search(status):
        diagnostics.append(Diagnostic("successor_overclaim", metric, "CEIC-091..095 must remain pending/unclaimed"))

    producer_paths = split_refs(row.get("required_producer_path", ""))
    if not producer_paths:
        diagnostics.append(Diagnostic("missing_producer", metric, "required_producer_path is empty"))
    for producer_path in producer_paths:
        if DESCRIPTOR_ONLY_RE.search(producer_path.lower()) or STATIC_ONLY_RE.search(producer_path.lower()):
            diagnostics.append(
                Diagnostic("missing_producer", metric, "descriptor/static path is not producer evidence")
            )
        elif not path_has_evidence(repo_root, producer_path):
            diagnostics.append(Diagnostic("missing_producer", metric, f"producer path is absent: {producer_path}"))

    if DESCRIPTOR_ONLY_RE.search(lowered):
        diagnostics.append(Diagnostic("descriptor_only", metric, "descriptor-only evidence is not producer coverage"))
    if STATIC_ONLY_RE.search(lowered):
        diagnostics.append(Diagnostic("static_only", metric, "static-only evidence is not producer coverage"))
    if STALE_RE.search(lowered):
        diagnostics.append(Diagnostic("stale_artifact", metric, "stale producer evidence is forbidden"))
    if PLACEHOLDER_RE.search(lowered):
        diagnostics.append(Diagnostic("placeholder_evidence", metric, "placeholder support-bundle evidence is forbidden"))
    if SYNTHETIC_RE.search(lowered):
        diagnostics.append(Diagnostic("synthetic_evidence", metric, "synthetic production evidence is forbidden"))
    if contains_local_cluster_claim(text):
        diagnostics.append(Diagnostic("local_cluster_claim", metric, "local cluster production evidence is forbidden"))
    if UNSAFE_AUTHORITY_RE.search(lowered):
        diagnostics.append(Diagnostic("unsafe_authority", metric, "unsafe authority claim is forbidden"))
    if contains_successor_overclaim(text):
        diagnostics.append(Diagnostic("successor_overclaim", metric, "CEIC-091..095 must remain pending/unclaimed"))

    return diagnostics


def validate_matrix(rows: list[dict[str, str]], repo_root: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if not rows:
        return [Diagnostic("empty_matrix", "matrix", "matrix must contain metric rows")]

    missing = [column for column in REQUIRED_COLUMNS if column not in rows[0]]
    if missing:
        return [Diagnostic("missing_columns", "matrix", "missing columns: " + ", ".join(missing))]

    seen: set[str] = set()
    for row in rows:
        metric = row.get("metric_family", "").strip()
        if metric in seen:
            diagnostics.append(Diagnostic("duplicate_metric", metric, "metric_family appears more than once"))
        seen.add(metric)
        diagnostics.extend(validate_row(row, repo_root))
    return diagnostics


def artifact_available(repo_root: pathlib.Path, row: dict[str, str]) -> bool:
    status = normalize_status(row.get("status", ""))
    return status in {"present", "complete", "completed", "generated"} and path_has_evidence(
        repo_root, row.get("path", "")
    )


def validate_execution_plan_control(repo_root: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    tracker = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_STATUS_MATRIX.csv"), "slice_id")
    dependencies = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_DEPENDENCY_MATRIX.csv"), "dependency_id")
    gates = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_ACCEPTANCE_MATRIX.csv"), "gate_id")
    artifacts = index_by(read_csv(repo_root / EXECUTION_PLAN / "ARTIFACT_INDEX.csv"), "artifact_id")

    if normalize_status(tracker.get("CEIC-090", {}).get("status", "")) != "complete":
        diagnostics.append(Diagnostic("ceic090_status", "CEIC-090", "CEIC_STATUS_MATRIX.csv must mark CEIC-090 complete"))
    for slice_id in (f"CEIC-{value:03d}" for value in range(91, 96)):
        if normalize_status(tracker.get(slice_id, {}).get("status", "")) not in {"pending", "complete"}:
            diagnostics.append(Diagnostic("successor_status", slice_id, f"{slice_id} must be pending or complete"))

    if normalize_status(dependencies.get("CEIC-DEP-050", {}).get("status", "")) != "available":
        diagnostics.append(
            Diagnostic("dependency_unavailable", "CEIC-DEP-050", "CEIC-090 dependency must be available")
        )

    if normalize_status(gates.get("CEIC-GATE-049", {}).get("status", "")) != "complete":
        diagnostics.append(Diagnostic("gate_status", "CEIC-GATE-049", "CEIC-090 gate must be complete"))
    if normalize_status(gates.get("CEIC-GATE-050", {}).get("status", "")) not in {"pending", "complete"}:
        diagnostics.append(Diagnostic("successor_status", "CEIC-GATE-050", "CEIC-091 gate must be pending or complete"))

    for artifact_id in REQUIRED_ARTIFACTS:
        row = artifacts.get(artifact_id)
        if row is None:
            diagnostics.append(Diagnostic("missing_artifact", artifact_id, "artifact is absent from ARTIFACT_INDEX.csv"))
        elif not artifact_available(repo_root, row):
            diagnostics.append(Diagnostic("stale_artifact", artifact_id, "artifact is absent or not present"))

    cmake_text = (repo_root / CMAKE_GATE).read_text(encoding="utf-8")
    for token in ("ceic_090_metrics_producer_coverage_gate_check", "ceic_090_metrics_producer_coverage_gate"):
        if token not in cmake_text:
            diagnostics.append(Diagnostic("cmake_registration", "CEIC-090", f"missing CMake registration: {token}"))

    return diagnostics


def run(repo_root: pathlib.Path, matrix_path: pathlib.Path) -> list[Diagnostic]:
    matrix = matrix_path if matrix_path.is_absolute() else repo_root / matrix_path
    diagnostics = validate_matrix(read_csv(matrix), repo_root)
    diagnostics.extend(validate_execution_plan_control(repo_root))
    return diagnostics


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--matrix", type=pathlib.Path, default=DEFAULT_MATRIX)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    diagnostics = run(repo_root, args.matrix)
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"ceic_090_metrics_producer_coverage_gate=fail:{diagnostic.render()}", file=sys.stderr)
        return 1
    print("ceic_090_metrics_producer_coverage_gate=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
