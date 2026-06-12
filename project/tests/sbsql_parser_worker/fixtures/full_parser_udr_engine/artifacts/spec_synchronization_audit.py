#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""FSPE-013 contract, matrix, and generated-artifact synchronization audit."""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path
import re
import sys


ARTIFACTS = Path(__file__).resolve().parent
EXECUTION_PLAN = ARTIFACTS.parent
CANON = Path("public_input_snapshot")
DETERMINISTIC_MANIFEST = Path(
    "project/tests/sbsql_parser_worker/generated/repro/DETERMINISTIC_ARTIFACT_MANIFEST.csv"
)


class Audit:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def finish(self) -> int:
        if self.failures:
            print("spec_synchronization_audit: failed")
            for failure in self.failures:
                print(f" - {failure}")
            return 1
        print("spec_synchronization_audit: passed")
        return 0


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def status(path: Path) -> str:
    match = re.search(r"^Status:\s*(.*)$", read_text(path), flags=re.M)
    return match.group(1).strip() if match else ""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def parse_manifest_authority_files(manifest: Path) -> list[str]:
    files: list[str] = []
    in_authority_files = False
    for raw_line in read_text(manifest).splitlines():
        line = raw_line.rstrip()
        if line == "authority_files:":
            in_authority_files = True
            continue
        if not in_authority_files:
            continue
        if line and not line.startswith("  "):
            break
        stripped = line.strip()
        if stripped.startswith("- "):
            files.append(stripped[2:].strip())
    return files


def generated_artifacts(repo: Path) -> list[dict[str, str]]:
    roots = (
        repo / "project/src/parsers/sbsql_worker/registry/generated",
        repo / "project/tests/sbsql_parser_worker/generated",
    )
    rows: list[dict[str, str]] = []
    for root in roots:
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            path_text = path.as_posix()
            if "/generated/repro/" in path_text:
                continue
            if "__pycache__" in path.parts or path.suffix == ".pyc":
                continue
            parts = path.parts
            category = "generated"
            if "registry" in parts and "generated" in parts:
                category = "registry_generated"
            elif "generated" in parts:
                index = parts.index("generated")
                category = "test_generated_" + (parts[index + 1] if index + 1 < len(parts) else "root")
            source_inputs = (
                "SURFACE_IMPLEMENTATION_BACKLOG.csv;BATCH_ROW_MEMBERSHIP.csv;SEMANTIC_ORACLE_AUTHORITY_MAP.csv"
                if category == "registry_generated"
                else "repo_tracked_generated_fixture"
            )
            rows.append(
                {
                    "artifact_path": rel(path, repo),
                    "sha256": sha256(path),
                    "size_bytes": str(path.stat().st_size),
                    "category": category,
                    "source_inputs": source_inputs,
                }
            )
    return sorted(rows, key=lambda row: row["artifact_path"])


def audit_manifest(repo: Path, audit: Audit) -> None:
    specs = repo / "public_release_evidence"
    manifest = specs / "MANIFEST.yaml"
    authority = specs / "AUTHORITY.md"
    audit.require(manifest.exists(), "public_contract_snapshot is missing")
    audit.require(authority.exists(), "public_contract_snapshot is missing")
    if not manifest.exists():
        return

    text = read_text(manifest)
    for token in [
        "engine_executes_sblr_not_sql",
        "parser_dialects_compile_to_shared_sblr",
        "internal_identity_is_uuid_based",
        "scratchbird_mga_is_authoritative_transaction_recovery_model",
        "wal_is_not_authoritative_recovery",
    ]:
        audit.require(token in text, f"MANIFEST.yaml missing invariant {token}")

    authority_files = parse_manifest_authority_files(manifest)
    audit.require(authority_files, "MANIFEST.yaml authority_files list is empty")
    for item in authority_files:
        audit.require(not item.startswith("../") and not item.startswith("/"),
                      f"authority file escapes public_release_evidence {item}")

    parser_authority = [
        item for item in authority_files
        if item.startswith("chapters/parser-v3/") or
        item.startswith("implementation_inputs/sbsql-canonicalization/")
    ]
    audit.require(parser_authority, "MANIFEST.yaml has no parser-v3 or SBSQL canonicalization authority entries")
    for item in parser_authority:
        audit.require((specs / item).exists(), f"manifest authority file missing: {item}")


def audit_matrices(repo: Path, audit: Audit) -> None:
    canon = repo / CANON
    surfaces = read_csv(canon / "SBSQL_SURFACE_REGISTRY.csv")
    operations = read_csv(canon / "SBSQL_TO_SBLR_OPERATION_MATRIX.csv")
    engine_gaps = read_csv(canon / "SBSQL_ENGINE_GAP_MATRIX.csv")
    references = read_csv(canon / "REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv")
    membership = read_csv(EXECUTION_PLAN / "artifacts/BATCH_ROW_MEMBERSHIP.csv")
    oracles = read_csv(EXECUTION_PLAN / "artifacts/SEMANTIC_ORACLE_AUTHORITY_MAP.csv")

    surface_ids = [row["surface_id"] for row in surfaces]
    audit.require(len(surface_ids) == len(set(surface_ids)), "SBSQL surface IDs are not unique")
    audit.require(len(surface_ids) == 2617, "SBSQL surface registry count is not 2617")
    audit.require({row["surface_id"] for row in operations} == set(surface_ids),
                  "SBLR operation matrix does not exactly cover surface registry")
    audit.require({row["surface_id"] for row in membership} == set(surface_ids),
                  "batch membership does not exactly cover surface registry")
    audit.require({row["surface_id"] for row in oracles} == set(surface_ids),
                  "semantic oracle map does not exactly cover surface registry")
    audit.require(len(engine_gaps) == 932, "engine gap matrix count is not 932")
    audit.require(len(references) == 312, "reference alias matrix count is not 312")

    for row in surfaces:
        spec = row["canonical_spec"].split("#", 1)[0]
        audit.require(spec.startswith("public_release_evidence"),
                      f"{row['surface_id']} canonical spec is not under public_release_evidence")
        audit.require((repo / spec).exists(), f"{row['surface_id']} canonical spec path missing: {spec}")

    gap_open = [row["gap_id"] for row in engine_gaps if row.get("current_status", "").startswith("open")]
    audit.require(not gap_open, f"engine gap matrix has open rows: {gap_open[:5]}")


def audit_generated_artifacts(repo: Path, audit: Audit) -> None:
    manifest_path = repo / DETERMINISTIC_MANIFEST
    expected = read_csv(manifest_path)
    actual = generated_artifacts(repo)
    audit.require(actual == expected, "deterministic artifact manifest does not match generated artifact tree")
    upgrade_paths = {row["artifact_path"] for row in expected if row["category"] == "test_generated_upgrade"}
    audit.require(
        {
            "project/tests/sbsql_parser_worker/generated/upgrade/UPGRADE_MIGRATION_COMPATIBILITY_FIXTURES.csv",
            "project/tests/sbsql_parser_worker/generated/upgrade/sbsql_upgrade_migration_compatibility_gate.cpp",
        }.issubset(upgrade_paths),
        "FSPE-012H upgrade artifacts are not tracked in deterministic manifest",
    )


def audit_execution_plan_evidence(repo: Path, audit: Audit) -> None:
    tracker = read_csv(EXECUTION_PLAN / "TRACKER.csv")
    commands = read_csv(EXECUTION_PLAN / "artifacts/VALIDATION_COMMAND_MATERIALIZATION.csv")
    acceptance = read_csv(EXECUTION_PLAN / "ACCEPTANCE_GATES.csv")
    audit_matrix = read_csv(EXECUTION_PLAN / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv")

    tracker_by_slice = {row["slice_id"]: row for row in tracker}
    audit.require(tracker_by_slice["FSPE-012H"]["status"] == "complete", "FSPE-012H is not complete")
    audit.require(tracker_by_slice["FSPE-013"]["status"] in {"ready_for_assignment", "complete"},
                  "FSPE-013 is not ready or complete")

    for row in tracker:
        if row["status"] != "complete":
            continue
        for item in row["outputs"].split(";"):
            if item.startswith("artifacts/"):
                audit.require((EXECUTION_PLAN / item).exists(),
                              f"{row['slice_id']} output artifact missing: {item}")

    for row in commands:
        if row["materialization_status"] != "complete":
            continue
        audit.require(row["runnable_now"] == "yes", f"{row['command_name']} complete but not runnable")
        audit.require("owning slice must provide" not in row["executable_or_contract"],
                      f"{row['command_name']} still has future-gate command text")
        for item in row["evidence_artifact"].split(";"):
            if item and item.endswith((".md", ".csv")):
                audit.require((EXECUTION_PLAN / "artifacts" / item).exists() or (EXECUTION_PLAN / item).exists(),
                              f"{row['command_name']} evidence artifact missing: {item}")

    gates = {row["gate_id"]: row["status"] for row in acceptance}
    for row in audit_matrix:
        statuses = [gates.get(gate) for gate in row["validation_gate"].split(";")]
        if any(item == "complete" for item in statuses):
            audit.require(row["status"] == "complete",
                          f"audit matrix row {row['source_search_key']} not complete for complete gate")

    report = EXECUTION_PLAN / "artifacts/SPEC_SYNCHRONIZATION_AUDIT.md"
    audit.require(report.exists(), "SPEC_SYNCHRONIZATION_AUDIT.md is missing")
    if report.exists():
        audit.require(status(report) == "complete", "SPEC_SYNCHRONIZATION_AUDIT.md is not complete")


def audit_boundary_invariants(repo: Path, audit: Audit) -> None:
    evidence_files = [
        repo / "project/src/server/sblr_admission.cpp",
        repo / "project/src/engine/internal_api/engine_internal_api.cpp",
        repo / "project/tests/sbsql_parser_worker/generated/hardening/sbsql_no_spin_no_wal_no_direct_db_gate.cpp",
        repo / "project/tests/sbsql_parser_worker/generated/upgrade/sbsql_upgrade_migration_compatibility_gate.cpp",
    ]
    combined = "\n".join(read_text(path) for path in evidence_files if path.exists())
    for token in [
        "SBLR.SQL_TEXT_FORBIDDEN",
        "parser_resolved_names_to_uuids",
        "SB-ENGINE-API-SQL-TEXT-NOT-ACCEPTED",
        "SB-ENGINE-API-PARSER-MUST-NOT-BE-TRUSTED",
        "no_wal_recovery=true",
        "wal_recovery_forbidden",
    ]:
        audit.require(token in combined, f"boundary invariant evidence missing token {token}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    audit = Audit()

    audit_manifest(repo, audit)
    audit_matrices(repo, audit)
    audit_generated_artifacts(repo, audit)
    audit_execution_plan_evidence(repo, audit)
    audit_boundary_invariants(repo, audit)
    return audit.finish()


if __name__ == "__main__":
    raise SystemExit(main())
