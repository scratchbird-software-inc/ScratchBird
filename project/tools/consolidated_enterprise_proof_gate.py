#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate CEIC proof-control and traceability invariants.

SEARCH_KEY: CEIC_PROOF_CONTROL_GATE
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import re
import sys
from collections import Counter, defaultdict
from collections import deque
from collections.abc import Iterable


EXECUTION_PLAN = pathlib.Path("docs" "/completed-execution-plans/consolidated-enterprise-proof-implementation-closure")
FINDINGS = pathlib.Path("docs" "/findings/Consolidated Audit Direction.md")

FINDING_ID_RE = re.compile(r"^\|\s*((?:AGT|MEM|IDX|OPT|X)-\d{3})\s*\|")
GLOB_CHARS = set("*?[")
COMPLETE_STATUSES = {"complete", "completed", "done", "closed", "complete_move_ready"}
PRESENT_STATUSES = {"present", "complete", "completed", "generated"}
PLANNED_GENERATED_KINDS = {
    "memory-readiness-manifest",
    "index-readiness-manifests",
    "optimizer-readiness-manifest",
    "agent-readiness-manifest",
    "integrated-final-manifest",
}
AUTHORITY_SUBJECTS = [
    "wal",
    "parser",
    "reference",
    "benchmark",
    "support bundle",
    "support-bundle",
    "memory",
    "metric",
    "index",
    "optimizer",
    "agent",
]
AUTHORITY_PHRASES = [
    "transaction finality",
    "visibility authority",
    "row visibility",
    "commit authority",
    "finality authority",
]
NEGATION_MARKERS = [
    " not ",
    " no ",
    " never ",
    " cannot ",
    " can't ",
    " must not ",
    " do not ",
    " does not ",
    " non-authority ",
    " only ",
    " refuse ",
    " fail closed ",
    " fail-closed ",
    " forbidden ",
    " without ",
    " must fail ",
    " fail when ",
]


def fail(message: str) -> None:
    print(f"consolidated_enterprise_proof_gate=fail:{message}", file=sys.stderr)


def normalize_status(value: str) -> str:
    return value.strip().lower().replace(" ", "_").replace("-", "_")


def compact_text(value: str) -> str:
    return " ".join(value.lower().split())


def split_refs(value: str) -> list[str]:
    refs: list[str] = []
    for raw in value.replace(",", ";").split(";"):
        ref = raw.strip()
        if ref and ref.lower() not in {"none", "n/a", "na"}:
            refs.append(ref)
    return refs


def read_text(repo_root: pathlib.Path, rel: pathlib.Path) -> str:
    path = repo_root / rel
    if not path.exists():
        raise FileNotFoundError(str(rel))
    return path.read_text(encoding="utf-8")


def read_csv(repo_root: pathlib.Path, rel: pathlib.Path) -> list[dict[str, str]]:
    path = repo_root / rel
    if not path.exists():
        raise FileNotFoundError(str(rel))
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    return [{key: value or "" for key, value in row.items()} for row in rows]


def artifact_path_exists(repo_root: pathlib.Path, artifact_path: str) -> bool:
    if not artifact_path.strip():
        return False
    path = repo_root / artifact_path
    if any(char in artifact_path for char in GLOB_CHARS):
        return any(pathlib.Path(match).exists() for match in repo_root.glob(artifact_path))
    return path.exists()


def require_columns(
    errors: list[str],
    rel: pathlib.Path,
    rows: list[dict[str, str]],
    columns: Iterable[str],
) -> None:
    if not rows:
        errors.append(f"{rel} must not be empty")
        return
    missing = [column for column in columns if column not in rows[0]]
    if missing:
        errors.append(f"{rel} missing columns: {', '.join(missing)}")


def extract_finding_ids(findings_text: str) -> list[str]:
    ids: list[str] = []
    for line in findings_text.splitlines():
        match = FINDING_ID_RE.match(line)
        if match:
            ids.append(match.group(1))
    return ids


def index_by_id(errors: list[str], rows: list[dict[str, str]], field: str, label: str) -> dict[str, dict[str, str]]:
    ids = [row.get(field, "").strip() for row in rows]
    counts = Counter(ids)
    for value, count in sorted(counts.items()):
        if not value:
            errors.append(f"{label} has a blank {field}")
        elif count != 1:
            errors.append(f"{label} {field} {value} appears {count} times")
    return {row.get(field, "").strip(): row for row in rows if row.get(field, "").strip()}


def artifact_is_available(repo_root: pathlib.Path, artifact: dict[str, str]) -> bool:
    status = normalize_status(artifact.get("status", ""))
    return status in PRESENT_STATUSES and artifact_path_exists(repo_root, artifact.get("path", ""))


def validate_traceability(
    errors: list[str],
    finding_ids: list[str],
    trace_rows: list[dict[str, str]],
    tracker: dict[str, dict[str, str]],
    gates: dict[str, dict[str, str]],
    artifacts: dict[str, dict[str, str]],
) -> None:
    finding_counts = Counter(finding_ids)
    for finding_id, count in sorted(finding_counts.items()):
        if count != 1:
            errors.append(f"{FINDINGS} finding {finding_id} appears {count} times")

    trace_counts = Counter(row.get("finding_id", "").strip() for row in trace_rows)
    for finding_id in sorted(finding_counts):
        if trace_counts[finding_id] != 1:
            errors.append(
                f"{EXECUTION_PLAN / 'AUDIT_TRACEABILITY_MATRIX.csv'} must map {finding_id} exactly once; "
                f"found {trace_counts[finding_id]}"
            )
    for finding_id, count in sorted(trace_counts.items()):
        if not finding_id:
            errors.append("AUDIT_TRACEABILITY_MATRIX.csv has a blank finding_id")
        elif finding_id not in finding_counts:
            errors.append(f"AUDIT_TRACEABILITY_MATRIX.csv maps unknown finding_id {finding_id}")
        elif count != 1:
            errors.append(f"AUDIT_TRACEABILITY_MATRIX.csv maps {finding_id} {count} times")

    for row in trace_rows:
        finding_id = row.get("finding_id", "").strip()
        for slice_id in split_refs(row.get("tracker_slices", "")):
            if slice_id not in tracker:
                errors.append(f"{finding_id} references missing tracker slice {slice_id}")
        for gate_id in split_refs(row.get("acceptance_gates", "")):
            if gate_id not in gates:
                errors.append(f"{finding_id} references missing acceptance gate {gate_id}")
        for artifact_id in split_refs(row.get("evidence_artifacts", "")):
            if artifact_id not in artifacts:
                errors.append(f"{finding_id} references missing artifact {artifact_id}")


def validate_artifacts(
    errors: list[str],
    repo_root: pathlib.Path,
    artifact_rows: list[dict[str, str]],
) -> None:
    for row in artifact_rows:
        artifact_id = row.get("artifact_id", "").strip()
        status = normalize_status(row.get("status", ""))
        kind = row.get("artifact_kind", "").strip()
        exists = artifact_path_exists(repo_root, row.get("path", ""))

        if status in PRESENT_STATUSES and not exists:
            errors.append(f"{artifact_id} is marked {status} but path is absent: {row.get('path', '')}")

        if kind in PLANNED_GENERATED_KINDS:
            if exists and status == "planned":
                errors.append(f"{artifact_id} generated artifact exists but is still marked planned")
            if not exists and status != "planned":
                errors.append(f"{artifact_id} generated artifact is absent and must be marked planned, not {status}")


def required_artifact_maps(
    artifact_rows: list[dict[str, str]],
) -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    by_gate: dict[str, set[str]] = defaultdict(set)
    by_slice: dict[str, set[str]] = defaultdict(set)

    for row in artifact_rows:
        artifact_id = row.get("artifact_id", "").strip()
        if not artifact_id:
            continue
        for gate_id in split_refs(row.get("required_for_gate", "")):
            by_gate[gate_id].add(artifact_id)
        for slice_id in split_refs(row.get("slice_id", "")):
            by_slice[slice_id].add(artifact_id)

    return by_gate, by_slice


def validate_complete_rows(
    errors: list[str],
    repo_root: pathlib.Path,
    tracker_rows: list[dict[str, str]],
    gate_rows: list[dict[str, str]],
    artifact_rows: list[dict[str, str]],
) -> None:
    artifacts = index_by_id([], artifact_rows, "artifact_id", "ARTIFACT_INDEX.csv")
    by_gate, by_slice = required_artifact_maps(artifact_rows)

    for gate in gate_rows:
        gate_id = gate.get("gate_id", "").strip()
        if normalize_status(gate.get("status", "")) not in COMPLETE_STATUSES:
            continue
        for artifact_id in sorted(by_gate.get(gate_id, set())):
            artifact = artifacts.get(artifact_id)
            if artifact is None or not artifact_is_available(repo_root, artifact):
                errors.append(f"{gate_id} is complete but required artifact {artifact_id} is absent or not present")

    for row in tracker_rows:
        slice_id = row.get("slice_id", "").strip()
        if normalize_status(row.get("status", "")) not in COMPLETE_STATUSES:
            continue
        for artifact_id in sorted(by_slice.get(slice_id, set())):
            artifact = artifacts.get(artifact_id)
            if artifact is None or not artifact_is_available(repo_root, artifact):
                errors.append(f"{slice_id} is complete but required artifact {artifact_id} is absent or not present")


def contains_fail_closed(text: str) -> bool:
    lowered = text.lower()
    return "fail closed" in lowered or "fail-closed" in lowered


def validate_cluster_boundary(
    errors: list[str],
    repo_root: pathlib.Path,
    claim_rows: list[dict[str, str]],
) -> None:
    readme = compact_text(read_text(repo_root, EXECUTION_PLAN / "README.md"))
    contracts = compact_text(read_text(repo_root, EXECUTION_PLAN / "INTERFACE_CONTRACTS.md"))

    if "external cluster provider" not in readme or not contains_fail_closed(readme):
        errors.append("README.md must keep cluster work external-provider and fail-closed")
    if "external cluster provider" not in contracts or not contains_fail_closed(contracts):
        errors.append("INTERFACE_CONTRACTS.md must keep cluster work external-provider and fail-closed")
    if "refuse cluster benchmark-clean and production-live claims" not in contracts:
        errors.append("INTERFACE_CONTRACTS.md must refuse cluster benchmark-clean and production-live claims")

    cluster_rows = [row for row in claim_rows if row.get("claim_surface", "").strip() == "cluster"]
    if len(cluster_rows) != 1:
        errors.append("CLAIM_BOUNDARY_MATRIX.csv must contain exactly one cluster row")
        return
    cluster_text = " ".join(cluster_rows[0].values()).lower()
    if not contains_fail_closed(cluster_text):
        errors.append("CLAIM_BOUNDARY_MATRIX.csv cluster row must be fail-closed")
    if "external" not in cluster_text or "provider" not in cluster_text:
        errors.append("CLAIM_BOUNDARY_MATRIX.csv cluster row must require an external provider")
    if "production cluster" not in cluster_text or "public stubs" not in cluster_text:
        errors.append("CLAIM_BOUNDARY_MATRIX.csv cluster row must forbid public-stub production cluster claims")


def validate_authority_boundaries(
    errors: list[str],
    repo_root: pathlib.Path,
    claim_rows: list[dict[str, str]],
) -> None:
    readme = compact_text(read_text(repo_root, EXECUTION_PLAN / "README.md"))
    contracts = compact_text(read_text(repo_root, EXECUTION_PLAN / "INTERFACE_CONTRACTS.md"))

    required_readme = [
        "mga transaction inventory remains transaction finality and visibility authority",
        "must not become transaction finality, visibility",
        "reference engines may provide comparison artifacts only",
        "wal must not be introduced",
    ]
    for phrase in required_readme:
        if phrase not in readme:
            errors.append(f"README.md missing authority boundary phrase: {phrase}")

    required_contracts = [
        "must not use memory feedback as transaction or visibility authority",
        "provider booleans are not accepted as final authority",
        "may not become optimizer plan authority, index generation finality authority, or transaction finality authority",
        "authoritative transaction finality claims",
    ]
    for phrase in required_contracts:
        if phrase not in contracts:
            errors.append(f"INTERFACE_CONTRACTS.md missing authority boundary phrase: {phrase}")

    required_claim_surfaces = {
        "memory",
        "indexes",
        "optimizer",
        "agents",
        "support_bundles",
        "metrics",
        "reference_comparisons",
    }
    claim_by_surface = {row.get("claim_surface", ""): row for row in claim_rows}
    missing = sorted(required_claim_surfaces - set(claim_by_surface))
    if missing:
        errors.append(f"CLAIM_BOUNDARY_MATRIX.csv missing claim surfaces: {', '.join(missing)}")

    for surface in sorted(required_claim_surfaces & set(claim_by_surface)):
        text = " ".join(claim_by_surface[surface].values()).lower()
        if "authority" in text and not any(marker.strip() in text for marker in ["not", "cannot", "never", "only"]):
            errors.append(f"CLAIM_BOUNDARY_MATRIX.csv {surface} row lacks a non-authority qualifier")

    for path in sorted((repo_root / EXECUTION_PLAN).glob("*")):
        if path.suffix not in {".md", ".csv"}:
            continue
        rel = path.relative_to(repo_root)
        prior: deque[str] = deque(maxlen=8)
        for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            lowered = f" {line.lower()} "
            if not any(phrase in lowered for phrase in AUTHORITY_PHRASES):
                if line.strip():
                    prior.append(line.lower())
                continue
            if not any(subject in lowered for subject in AUTHORITY_SUBJECTS):
                if line.strip():
                    prior.append(line.lower())
                continue
            window = f" {' '.join(prior)} {line.lower()} "
            if any(marker in window for marker in NEGATION_MARKERS):
                if line.strip():
                    prior.append(line.lower())
                continue
            errors.append(f"{rel}:{line_no} appears to grant forbidden transaction/visibility authority")
            if line.strip():
                prior.append(line.lower())


def run(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []

    try:
        findings_text = read_text(repo_root, FINDINGS)
        tracker_rows = read_csv(repo_root, EXECUTION_PLAN / "TRACKER.csv")
        gate_rows = read_csv(repo_root, EXECUTION_PLAN / "ACCEPTANCE_GATES.csv")
        artifact_rows = read_csv(repo_root, EXECUTION_PLAN / "ARTIFACT_INDEX.csv")
        trace_rows = read_csv(repo_root, EXECUTION_PLAN / "AUDIT_TRACEABILITY_MATRIX.csv")
        claim_rows = read_csv(repo_root, EXECUTION_PLAN / "CLAIM_BOUNDARY_MATRIX.csv")
    except FileNotFoundError as exc:
        return [f"missing required file: {exc}"]

    require_columns(errors, EXECUTION_PLAN / "TRACKER.csv", tracker_rows, ["slice_id", "status"])
    require_columns(errors, EXECUTION_PLAN / "ACCEPTANCE_GATES.csv", gate_rows, ["gate_id", "status"])
    require_columns(
        errors,
        EXECUTION_PLAN / "ARTIFACT_INDEX.csv",
        artifact_rows,
        ["artifact_id", "slice_id", "artifact_kind", "path", "required_for_gate", "status"],
    )
    require_columns(
        errors,
        EXECUTION_PLAN / "AUDIT_TRACEABILITY_MATRIX.csv",
        trace_rows,
        ["finding_id", "tracker_slices", "acceptance_gates", "evidence_artifacts"],
    )
    require_columns(
        errors,
        EXECUTION_PLAN / "CLAIM_BOUNDARY_MATRIX.csv",
        claim_rows,
        ["claim_surface", "allowed_after_completion", "forbidden_claim_without_external_or_future_proof", "authority_notes"],
    )
    if errors:
        return errors

    tracker = index_by_id(errors, tracker_rows, "slice_id", "TRACKER.csv")
    gates = index_by_id(errors, gate_rows, "gate_id", "ACCEPTANCE_GATES.csv")
    artifacts = index_by_id(errors, artifact_rows, "artifact_id", "ARTIFACT_INDEX.csv")

    finding_ids = extract_finding_ids(findings_text)
    if not finding_ids:
        errors.append(f"{FINDINGS} contains no finding table IDs")

    validate_traceability(errors, finding_ids, trace_rows, tracker, gates, artifacts)
    validate_artifacts(errors, repo_root, artifact_rows)
    validate_complete_rows(errors, repo_root, tracker_rows, gate_rows, artifact_rows)
    validate_cluster_boundary(errors, repo_root, claim_rows)
    validate_authority_boundaries(errors, repo_root, claim_rows)
    return errors


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path.cwd(),
        help="ScratchBird" "-Private repository root",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    errors = run(repo_root)
    if errors:
        for message in errors:
            fail(message)
        return 1
    print("consolidated_enterprise_proof_gate=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
