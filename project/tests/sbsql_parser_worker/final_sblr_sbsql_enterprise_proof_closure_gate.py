#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Retain and verify the current SBsql/SBLR enterprise closure evidence.

The gate proves authority, inventory, obligation, retention, and CTest-profile
integrity.  It intentionally preserves the active workplan's open findings and
in-progress status rather than converting structural closure into an
implementation or release-acceptance claim.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path
import re
import sys

import sbsql_final_language_expansion_closure_gate as language_gate


FIXTURE_RELATIVE = Path(
    "project/tests/sbsql_parser_worker/fixtures/final_sblr_sbsql_closure/"
    "FINAL_CLOSURE_EVIDENCE_HASHES.csv"
)

EVIDENCE_ARTIFACTS = {
    "core_authority_manifest": "Specifications/Core/MANIFEST.yaml",
    "sbsql_surface_registry": (
        "Specifications/Core/registries/sbsql-consolidated-surface-registry.csv"
    ),
    "sbsql_surface_to_sblr": (
        "Specifications/Core/registries/sbsql-consolidated-surface-to-sblr.csv"
    ),
    "sbsql_command_zero_grey": (
        "Specifications/Core/registries/sbsql-command-sblr-zero-grey-closure.csv"
    ),
    "sblr_opcode_zero_grey": (
        "Specifications/Core/registries/sblr-opcode-executor-zero-grey-closure.csv"
    ),
    "sblr_operation_envelopes": "Specifications/Core/registries/sblr-operation-matrix.yaml",
    "result_shape_registry": "Specifications/Core/registries/result-shape-registry.yaml",
    "implementation_obligation_inventory": (
        "Workplans/sbsql-sblr-implementation-alignment/IMPLEMENTATION_LEDGER.csv"
    ),
    "layered_test_obligation_inventory": (
        "Workplans/sbsql-sblr-implementation-alignment/TEST_LEDGER.csv"
    ),
    "alignment_generated_provenance": (
        "Workplans/sbsql-sblr-implementation-alignment/GENERATED_PROVENANCE.csv"
    ),
}

HASH_COLUMNS = {
    "evidence",
    "artifact_path",
    "generator",
    "ctest_label",
    "retention_policy",
    "hash",
}

ALWAYS_REQUIRED_TESTS = {
    "sblr_surface_fse_p7_execution_proof_gate",
    "sbsql_surface_to_sblr_function_coverage_gate",
    "sbsql_no_stub_source_integrity_gate",
    "sbsql_sblr_binary_round_trip_fixture_gate",
    "sbsql_final_language_expansion_closure_gate",
    "public_cluster_provider_handshake_gate",
    "ctest_no_execution_plan_runtime_dependency_gate",
    "database_lifecycle_full_route_conformance",
    "sb_listener_sbp_sbsql_sbwp_tls_engine_auth_route_smoke",
    "final_sblr_sbsql_enterprise_proof_closure_gate",
    "final_sblr_sbsql_master_closure_gate",
}

COMPATIBILITY_PROFILE_TESTS = {
    "parser_dialect_isolation_audit_gate",
    "compatibility_sql_first_tranche_original_tool_replay_gate",
}

FORBIDDEN_RUNTIME_TOKENS = (
    "public_" + "execution_plan",
    "--execution_" + "plan-root",
    "--closed-execution_" + "plan-root",
    "--closure-execution_" + "plan-root",
)

OWNED_CLOSURE_ARTIFACTS = (
    "project/tests/sbsql_parser_worker/sbsql_final_language_expansion_closure_gate.py",
    "project/tests/sbsql_parser_worker/final_sblr_sbsql_enterprise_proof_closure_gate.py",
    "project/tests/sbsql_parser_worker/final_sblr_sbsql_master_closure_gate.py",
    str(FIXTURE_RELATIVE),
)


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(path: Path, required_columns: set[str], label: str) -> list[dict[str, str]]:
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            fieldnames = set(reader.fieldnames or ())
            rows = list(reader)
    except FileNotFoundError:
        fail(f"missing {label}: {path}")
    missing = sorted(required_columns - fieldnames)
    require(not missing, f"{label} missing columns: {missing}")
    require(rows, f"{label} has no rows: {path}")
    return rows


def unique_index(
    rows: list[dict[str, str]], column: str, label: str
) -> dict[str, dict[str, str]]:
    index: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row[column].strip()
        require(value, f"{label} row missing {column}")
        require(value not in index, f"{label} duplicate {column}: {value}")
        index[value] = row
    return index


def sha256(path: Path) -> str:
    require(path.is_file(), f"retained evidence artifact missing: {path}")
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def validate_retained_hashes(root: Path, workspace: Path) -> int:
    rows = read_csv(root / FIXTURE_RELATIVE, HASH_COLUMNS, "retained closure hash manifest")
    by_evidence = unique_index(rows, "evidence", "retained closure hash manifest")
    require(
        set(by_evidence) == set(EVIDENCE_ARTIFACTS),
        "retained closure hash manifest does not exactly cover current Core/workplan evidence",
    )

    workspace_resolved = workspace.resolve()
    for evidence, expected_relative in EVIDENCE_ARTIFACTS.items():
        row = by_evidence[evidence]
        require(
            row["artifact_path"] == expected_relative,
            f"retained evidence path drift for {evidence}",
        )
        for column in ("generator", "ctest_label", "retention_policy"):
            require(row[column].strip(), f"retained evidence {evidence} has empty {column}")
        artifact = (workspace / row["artifact_path"]).resolve()
        require(
            workspace_resolved in artifact.parents,
            f"retained evidence path escapes the controlled workspace: {evidence}",
        )
        require(
            row["hash"] == sha256(artifact),
            f"retained evidence hash drift for {evidence}: {row['artifact_path']}",
        )
    return len(rows)


def validate_active_controls(root: Path, workplan_root: Path) -> dict[str, int]:
    findings = read_csv(
        workplan_root / "FINDINGS.csv",
        {
            "finding_id",
            "severity",
            "summary",
            "specification_authority",
            "code_evidence",
            "required_action",
            "status",
            "resolution",
        },
        "alignment findings",
    )
    finding_by_id = unique_index(findings, "finding_id", "alignment findings")
    for finding_id, row in finding_by_id.items():
        for column in (
            "severity",
            "summary",
            "specification_authority",
            "code_evidence",
            "required_action",
            "status",
        ):
            require(row[column].strip(), f"finding {finding_id} has empty {column}")
        require(
            row["status"] in {"open", "resolved"},
            f"finding {finding_id} has unknown status {row['status']}",
        )
        if row["status"] == "resolved":
            require(row["resolution"].strip(), f"resolved finding has no resolution: {finding_id}")

    decisions = read_csv(
        workplan_root / "OWNER_DECISIONS.csv",
        {
            "decision_id",
            "affected_items",
            "question",
            "owner_decision",
            "status",
            "resolution_authority",
        },
        "alignment owner decisions",
    )
    decision_by_id = unique_index(decisions, "decision_id", "alignment owner decisions")
    for decision_id, row in decision_by_id.items():
        for column in (
            "affected_items",
            "question",
            "owner_decision",
            "status",
            "resolution_authority",
        ):
            require(row[column].strip(), f"owner decision {decision_id} has empty {column}")
        require(
            row["status"].startswith("accepted_"),
            f"owner decision is not accepted: {decision_id} {row['status']}",
        )
    compatibility_decision = decision_by_id.get("IA-DECISION-0005")
    require(compatibility_decision is not None, "compatibility-parser build decision is missing")
    require(
        compatibility_decision["status"] == "accepted_build_scheduling_gate",
        "compatibility-parser build decision is not accepted",
    )

    provenance = read_csv(
        workplan_root / "GENERATED_PROVENANCE.csv",
        {
            "artifact_id",
            "artifact_paths",
            "authoritative_inputs",
            "generator_path",
            "generator_version",
            "deterministic_invocation",
            "output_sha256_set",
            "validation_result",
            "status",
        },
        "alignment generated provenance",
    )
    provenance_by_id = unique_index(provenance, "artifact_id", "alignment generated provenance")
    for artifact_id, row in provenance_by_id.items():
        for column in (
            "artifact_paths",
            "authoritative_inputs",
            "generator_path",
            "generator_version",
            "deterministic_invocation",
            "output_sha256_set",
            "validation_result",
            "status",
        ):
            require(row[column].strip(), f"provenance {artifact_id} has empty {column}")
        require(
            row["validation_result"] == "PASS",
            f"generated provenance is not retained as PASS: {artifact_id}",
        )
        generator = Path(row["generator_path"])
        generator_candidates = (workplan_root / generator, root / generator)
        require(
            any(path.is_file() for path in generator_candidates),
            f"retained generator path is missing: {artifact_id} {row['generator_path']}",
        )

    return {
        "open_findings": sum(row["status"] == "open" for row in findings),
        "open_release_blockers": sum(
            row["status"] == "open" and row["severity"] == "release_blocking"
            for row in findings
        ),
        "owner_decisions": len(decisions),
        "provenance_records": len(provenance),
    }


def cmake_cache_bool(build_root: Path, key: str) -> bool:
    cache = build_root / "CMakeCache.txt"
    require(cache.is_file(), f"CMake cache missing: {cache}")
    match = re.search(
        rf"^{re.escape(key)}:BOOL=(ON|OFF|TRUE|FALSE|1|0)$",
        cache.read_text(encoding="utf-8", errors="replace"),
        flags=re.MULTILINE | re.IGNORECASE,
    )
    require(match is not None, f"CMake cache has no boolean {key}")
    return match.group(1).upper() in {"ON", "TRUE", "1"}


def validate_owned_artifact_inputs(root: Path) -> None:
    for relative in OWNED_CLOSURE_ARTIFACTS:
        path = root / relative
        require(path.is_file(), f"closure artifact missing: {relative}")
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in FORBIDDEN_RUNTIME_TOKENS:
            require(token not in text, f"closure artifact retains obsolete runtime input {token}: {relative}")


def validate_ctest_profile(root: Path, build_root: Path | None) -> str:
    validate_owned_artifact_inputs(root)
    if build_root is None:
        source = "\n".join(
            path.read_text(encoding="utf-8", errors="replace")
            for path in (root / "project/tests").rglob("CMakeLists.txt")
        )
        missing = sorted(name for name in ALWAYS_REQUIRED_TESTS if name not in source)
        require(not missing, f"CTest source inventory missing enterprise closure tests: {missing}")
        return "source_only"

    require(build_root.is_dir(), f"build root missing: {build_root}")
    names = language_gate.generated_ctest_names(build_root)
    require(names, f"build root has no generated CTest inventory: {build_root}")
    missing = sorted(ALWAYS_REQUIRED_TESTS - names)
    require(not missing, f"generated CTest inventory missing enterprise closure tests: {missing}")

    compatibility_enabled = cmake_cache_bool(build_root, "SB_BUILD_COMPATIBILITY_PARSERS")
    if compatibility_enabled:
        missing_compatibility = sorted(COMPATIBILITY_PROFILE_TESTS - names)
        require(
            not missing_compatibility,
            f"compatibility-enabled build omits compatibility proof tests: {missing_compatibility}",
        )
        profile = "compatibility_enabled"
    else:
        unexpected = sorted(COMPATIBILITY_PROFILE_TESTS & names)
        require(
            not unexpected,
            f"SBsql-only build registers compatibility-parser proof tests: {unexpected}",
        )
        profile = "sbsql_only"

    for ctest_file in build_root.rglob("CTestTestfile.cmake"):
        text = ctest_file.read_text(encoding="utf-8", errors="replace")
        for token in FORBIDDEN_RUNTIME_TOKENS:
            require(token not in text, f"generated CTest command retains obsolete runtime input {token}")
    return profile


def validate_enterprise(
    root: Path, build_root: Path | None
) -> dict[str, int | str]:
    language_summary = language_gate.validate_all(root, build_root)
    workspace, _, workplan_root = language_gate.resolve_control_roots(root)
    retained_hashes = validate_retained_hashes(root, workspace)
    control_summary = validate_active_controls(root, workplan_root)
    profile = validate_ctest_profile(root, build_root)
    return {
        **language_summary,
        **control_summary,
        "retained_hashes": retained_hashes,
        "ctest_profile": profile,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root")
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    build_root = Path(args.build_root).resolve() if args.build_root else None

    summary = validate_enterprise(root, build_root)
    print(
        "final_sblr_sbsql_enterprise_proof_closure_gate=passed "
        f"ctest_profile={summary['ctest_profile']} "
        f"retained_hashes={summary['retained_hashes']} "
        f"implementation_items={summary['implementation_items']} "
        f"test_obligations={summary['test_obligations']} "
        f"open_findings={summary['open_findings']} "
        f"open_release_blockers={summary['open_release_blockers']} "
        f"workplan_status={summary['workplan_status']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
