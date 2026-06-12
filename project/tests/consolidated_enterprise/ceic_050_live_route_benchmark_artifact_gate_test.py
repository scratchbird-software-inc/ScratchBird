#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate the CEIC-050 live-route benchmark artifact harness bridge.

SEARCH_KEY: CEIC_050_LIVE_ROUTE_BENCHMARK_ARTIFACT_GATE_TEST
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import pathlib
import re
import sys


EXECUTION_PLAN = pathlib.Path("docs" "/completed-execution-plans/consolidated-enterprise-proof-implementation-closure")
COMPLETED_OPTIMIZER = pathlib.Path("docs" "/completed-execution-plans/optimizer-enterprise-implementation-closure")
ARTIFACT = EXECUTION_PLAN / "artifacts/CEIC-050_LIVE_ROUTE_BENCHMARK_ARTIFACT_HARNESS_EVIDENCE.md"
ROUTE_GATE = pathlib.Path("project/tests/optimizer/optimizer_enterprise_route_validation_gate.cpp")
RUNTIME_BENCHMARK = pathlib.Path("project/src/engine/optimizer/runtime_consumption_benchmark_evidence.cpp")
OPTIMIZER_TEST_CMAKE = pathlib.Path("project/tests/optimizer/CMakeLists.txt")
OPTIMIZER_SOURCE_CMAKE = pathlib.Path("project/src/engine/optimizer/CMakeLists.txt")
CONSOLIDATED_CMAKE = pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt")

COMPLETE_STATUSES = {"complete", "completed", "done", "closed", "complete_move_ready"}
PENDING_STATUS = "pending"
REQUIRED_ROUTES = ("embedded", "ipc", "inet", "cli", "driver")
CEIC_OPTIMIZER_PENDING = tuple(f"CEIC-{value:03d}" for value in range(51, 63))
CEIC_INTEGRATED_PENDING = tuple(f"CEIC-{value:03d}" for value in range(90, 96))
FINGERPRINTED_PATHS = (
    ROUTE_GATE,
    RUNTIME_BENCHMARK,
    OPTIMIZER_TEST_CMAKE,
    OPTIMIZER_SOURCE_CMAKE,
    CONSOLIDATED_CMAKE,
)
FINGERPRINT_RE = re.compile(r"^- `([^`]+)`: `sha256:([0-9a-f]{64})`$", re.MULTILINE)


def rel(path: pathlib.Path) -> str:
    return path.as_posix()


def fail(message: str) -> None:
    print(f"ceic_050_live_route_benchmark_artifact_gate=fail:{message}", file=sys.stderr)


def normalize_status(value: str) -> str:
    return value.strip().lower().replace(" ", "_").replace("-", "_")


def read_text(repo_root: pathlib.Path, path: pathlib.Path, errors: list[str]) -> str:
    absolute = repo_root / path
    if not absolute.exists():
        errors.append(f"missing required file: {rel(path)}")
        return ""
    return absolute.read_text(encoding="utf-8")


def read_csv(repo_root: pathlib.Path, path: pathlib.Path, errors: list[str]) -> list[dict[str, str]]:
    absolute = repo_root / path
    if not absolute.exists():
        errors.append(f"missing required CSV: {rel(path)}")
        return []
    with absolute.open(newline="", encoding="utf-8") as handle:
        return [{key: value or "" for key, value in row.items()} for row in csv.DictReader(handle)]


def index_by(rows: list[dict[str, str]], key: str) -> dict[str, dict[str, str]]:
    return {row.get(key, "").strip(): row for row in rows if row.get(key, "").strip()}


def require(errors: list[str], condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def require_tokens(errors: list[str], text: str, path: pathlib.Path, label: str, tokens: tuple[str, ...]) -> None:
    for token in tokens:
        if token not in text:
            errors.append(f"{rel(path)} missing {label} anchor: {token}")


def sha256_file(repo_root: pathlib.Path, path: pathlib.Path) -> str:
    return hashlib.sha256((repo_root / path).read_bytes()).hexdigest()


def validate_fingerprints(errors: list[str], repo_root: pathlib.Path, artifact_text: str) -> None:
    fingerprints = dict(FINGERPRINT_RE.findall(artifact_text))
    for path in FINGERPRINTED_PATHS:
        key = rel(path)
        expected = fingerprints.get(key)
        if expected is None:
            errors.append(f"{rel(ARTIFACT)} missing source fingerprint for {key}")
            continue
        actual = sha256_file(repo_root, path)
        if actual != expected:
            errors.append(f"{rel(ARTIFACT)} stale fingerprint for {key}: expected {expected}, actual {actual}")


def validate_optimizer_source_anchors(errors: list[str], route_text: str, runtime_text: str) -> None:
    require_tokens(
        errors,
        route_text,
        ROUTE_GATE,
        "production-build route refusal",
        (
            "OEIC_OPTIMIZER_PRODUCTION_BUILD_GATE",
            "placeholder_runtime_evidence_enabled",
            "reference_produced_evidence_enabled",
            "parser_execution_shortcuts_enabled",
            "cluster_stub_live_claims_enabled",
            "SB_OPT_PRODUCTION_GATE_PLACEHOLDER_RUNTIME_EVIDENCE_FORBIDDEN",
            "SB_OPT_PRODUCTION_GATE_CLUSTER_STUB_LIVE_CLAIMS_FORBIDDEN",
        ),
    )
    require_tokens(
        errors,
        route_text,
        ROUTE_GATE,
        "benchmark-clean route proof",
        (
            "OEIC_BENCHMARK_CLEAN_OPTIMIZER_ROUTE",
            "ValidateOptimizerBenchmarkRouteEvidence",
            'BenchmarkLane("cold", "cold", "postgresql")',
            'BenchmarkLane("warm", "warm", "postgresql")',
            "benchmark_clean_claim = true",
            "trusted = true",
            "fresh = true",
            "evidence_generation",
        ),
    )
    require_tokens(
        errors,
        route_text,
        ROUTE_GATE,
        "placeholder evidence rejection",
        (
            "EvaluateRouteCompletionClaim",
            "placeholder_result.can_mark_complete",
            "real_result.can_mark_complete",
            "placeholder runtime evidence closed benchmark-clean route",
            "result-contract-v1",
        ),
    )
    require_tokens(
        errors,
        route_text,
        ROUTE_GATE,
        "live parser/SBLR/optimizer/executor/result-frame route evidence",
        (
            "OEIC_LIVE_PARSER_SBLR_OPTIMIZER_EXECUTOR_ROUTE",
            "ValidateCrossRouteResultEquivalence",
            "engine.executor.enterprise_route",
            "sblr_digest",
            "executor_capability_set_id",
            "normalized_optimizer_controls_digest",
            "result_contract_hash",
            "binary_frame_equivalent",
            "rows =",
        ),
    )
    require_tokens(
        errors,
        route_text,
        ROUTE_GATE,
        "reference method evidence-only proof",
        (
            "OEIC_SCALE_REFERENCE_COMPARISON_SUITE",
            "ReferenceMethodEvidence",
            "ReferenceEngines",
            "ValidateBestMethodBenchmarkEquivalence",
            "reference_reference_only = engine != \"scratchbird\"",
            "uses_reference_storage_or_finality_for_scratchbird = false",
            "engine_mga_or_reference_native_reference_only",
        ),
    )
    require_tokens(
        errors,
        route_text,
        ROUTE_GATE,
        "driver-visible equivalence and authority negative",
        (
            "ValidateDriverVisibleExplainRouteEquivalence",
            "driver_visible_route = true",
            "redaction_applied = true",
            "driver/security authority drift was accepted",
        ),
    )
    for route in REQUIRED_ROUTES:
        require(errors, f'"{route}"' in route_text, f"{rel(ROUTE_GATE)} missing represented route {route}")
    require(
        errors,
        'Route("cluster")' not in route_text and '"cluster"' not in re.sub(r"cluster_stub_live_claims_enabled|cluster-stub", "", route_text),
        f"{rel(ROUTE_GATE)} must not treat cluster as a local live-route lane",
    )

    require_tokens(
        errors,
        runtime_text,
        RUNTIME_BENCHMARK,
        "route constants and driver-visible route set",
        (
            "kEmbeddedRoute",
            "kIpcRoute",
            "kInetRoute",
            "kCliRoute",
            "kDriverRoute",
            "IsDriverVisibleRoute",
            "ValidateDriverVisibleExplainRouteEquivalence",
            "MISSING_DRIVER_VISIBLE_ROUTE",
        ),
    )
    for route in REQUIRED_ROUTES:
        require(errors, f'"{route}"' in runtime_text, f"{rel(RUNTIME_BENCHMARK)} missing runtime route {route}")
    require(
        errors,
        "kClusterRoute" not in runtime_text and 'route_kind == "cluster"' not in runtime_text,
        f"{rel(RUNTIME_BENCHMARK)} must not admit cluster as a local driver-visible route",
    )
    require_tokens(
        errors,
        runtime_text,
        RUNTIME_BENCHMARK,
        "benchmark-clean runtime evidence",
        (
            "ValidateBenchmarkMethodologyEvidence",
            "ValidateOptimizerBenchmarkRouteEvidence",
            "p50_us",
            "p95_us",
            "p99_us",
            "COLD_WARM_PHASE_INCOMPLETE",
            "STALE_OR_UNTRUSTED",
            "PERCENTILES_MISSING",
        ),
    )
    require_tokens(
        errors,
        runtime_text,
        RUNTIME_BENCHMARK,
        "placeholder runtime rejection",
        (
            "HasPlaceholderProductionEvidenceValues",
            "result-contract-v1",
            "RuntimeEvidenceIsCleanlyConsumed",
            "RUNTIME_CONSUMPTION_MISSING",
        ),
    )
    require_tokens(
        errors,
        runtime_text,
        RUNTIME_BENCHMARK,
        "reference evidence-only authority rejection",
        (
            "ValidateBestMethodBenchmarkEquivalence",
            "IsReferenceEngine",
            "reference_reference_only",
            "uses_reference_storage_or_finality_for_scratchbird",
            "HasAuthorityDrift",
            "REFERENCE_AUTHORITY_DRIFT",
            "MGA_AUTHORITY_DRIFT",
        ),
    )
    require_tokens(
        errors,
        runtime_text,
        RUNTIME_BENCHMARK,
        "cross-route equivalence and result-frame proof",
        (
            "ValidateCrossRouteResultEquivalence",
            "MISSING_ROUTE_EVIDENCE",
            "RESULT_CONTRACT_MISSING",
            "RESULT_CONTRACT_MISMATCH",
            "ROWS_MISMATCH",
            "lowered_sblr_reused",
            "result_shape_reused",
            "frame_kind",
            "frame_version",
            "equivalent_result_materialization",
            "parser/cache cannot own transaction finality",
        ),
    )


def validate_cmake_registration(
    errors: list[str],
    optimizer_test_cmake: str,
    optimizer_source_cmake: str,
    consolidated_cmake: str,
) -> None:
    require_tokens(
        errors,
        optimizer_test_cmake,
        OPTIMIZER_TEST_CMAKE,
        "optimizer route validation CTest registration",
        (
            "add_executable(\n  optimizer_enterprise_route_validation_gate",
            "add_test(\n  NAME optimizer_enterprise_route_validation_gate",
            "OEIC-080;OEIC-081;OEIC-082;OEIC-083;OEIC-084;OEIC-085",
            "benchmark_clean;live_route;scale;reference;crash_restart;security",
        ),
    )
    require_tokens(
        errors,
        optimizer_source_cmake,
        OPTIMIZER_SOURCE_CMAKE,
        "runtime benchmark evidence registration",
        (
            "add_library(sb_engine_optimizer_contract_only",
            "runtime_consumption_benchmark_evidence.cpp",
            "runtime_consumption_evidence.cpp",
        ),
    )
    require_tokens(
        errors,
        consolidated_cmake,
        CONSOLIDATED_CMAKE,
        "CEIC-050 consolidated CTest registration",
        (
            "CEIC_050_LIVE_ROUTE_BENCHMARK_ARTIFACT_GATE_TEST",
            "ceic_050_live_route_benchmark_artifact_gate_check",
            "NAME ceic_050_live_route_benchmark_artifact_gate",
            "CEIC-050;CEIC-GATE-030",
        ),
    )


def validate_completed_optimizer_bridge(errors: list[str], repo_root: pathlib.Path) -> None:
    tracker = index_by(read_csv(repo_root, COMPLETED_OPTIMIZER / "TRACKER.csv", errors), "OEIC-080")
    if not tracker:
        # The completed tracker has no header quotes in older packages; read it directly by DictReader field names.
        rows = read_csv(repo_root, COMPLETED_OPTIMIZER / "TRACKER.csv", errors)
        tracker = {row.get("OEIC-080", "").strip(): row for row in rows if row.get("OEIC-080", "").strip()}
    rows = read_csv(repo_root, COMPLETED_OPTIMIZER / "TRACKER.csv", errors)
    if rows and "OEIC-080" in rows[0]:
        # CSV without a header was parsed with the first data row as header. Re-read with explicit headers.
        absolute = repo_root / COMPLETED_OPTIMIZER / "TRACKER.csv"
        with absolute.open(newline="", encoding="utf-8") as handle:
            reader = csv.reader(handle)
            rows = [
                {
                    "slice_id": raw[0],
                    "phase": raw[1],
                    "title": raw[2],
                    "status": raw[3],
                    "owner": raw[4],
                    "primary_modules": raw[5],
                    "depends_on": raw[6],
                    "acceptance": raw[7],
                }
                for raw in reader
                if raw
            ]
    completed_tracker = index_by(rows, "slice_id")
    for slice_id in ("OEIC-080", "OEIC-081", "OEIC-082", "OEIC-083", "OEIC-085", "OEIC-090"):
        row = completed_tracker.get(slice_id)
        require(errors, row is not None, f"{rel(COMPLETED_OPTIMIZER / 'TRACKER.csv')} missing {slice_id}")
        if row is not None:
            require(
                errors,
                normalize_status(row.get("status", "")) in COMPLETE_STATUSES,
                f"{slice_id} must remain completed in the optimizer package",
            )

    gates = read_csv(repo_root, COMPLETED_OPTIMIZER / "ACCEPTANCE_GATES.csv", errors)
    if gates and "OEIC-GATE-070" in gates[0]:
        absolute = repo_root / COMPLETED_OPTIMIZER / "ACCEPTANCE_GATES.csv"
        with absolute.open(newline="", encoding="utf-8") as handle:
            reader = csv.reader(handle)
            gates = [
                {
                    "gate_id": raw[0],
                    "title": raw[1],
                    "status": raw[2],
                    "requirement": raw[3],
                }
                for raw in reader
                if raw
            ]
    completed_gates = index_by(gates, "gate_id")
    for gate_id in ("OEIC-GATE-081", "OEIC-GATE-082", "OEIC-GATE-083"):
        row = completed_gates.get(gate_id)
        require(errors, row is not None, f"{rel(COMPLETED_OPTIMIZER / 'ACCEPTANCE_GATES.csv')} missing {gate_id}")
        if row is not None:
            require(
                errors,
                normalize_status(row.get("status", "")) in COMPLETE_STATUSES,
                f"{gate_id} must remain completed in the optimizer package",
            )


def validate_consolidated_execution_plan(errors: list[str], repo_root: pathlib.Path, artifact_text: str) -> None:
    tracker = index_by(read_csv(repo_root, EXECUTION_PLAN / "TRACKER.csv", errors), "slice_id")
    gates = index_by(read_csv(repo_root, EXECUTION_PLAN / "ACCEPTANCE_GATES.csv", errors), "gate_id")
    artifacts = index_by(read_csv(repo_root, EXECUTION_PLAN / "ARTIFACT_INDEX.csv", errors), "artifact_id")
    trace = index_by(read_csv(repo_root, EXECUTION_PLAN / "AUDIT_TRACEABILITY_MATRIX.csv", errors), "finding_id")
    spec_audit = index_by(read_csv(repo_root, EXECUTION_PLAN / "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv", errors), "audit_id")
    dependencies = index_by(read_csv(repo_root, EXECUTION_PLAN / "DEPENDENCIES.csv", errors), "dependency_id")

    row = tracker.get("CEIC-050")
    require(errors, row is not None, "TRACKER.csv missing CEIC-050")
    if row is not None:
        require(errors, normalize_status(row.get("status", "")) in COMPLETE_STATUSES, "CEIC-050 must be complete")
        require(
            errors,
            "project/tests/consolidated_enterprise" in row.get("primary_modules", ""),
            "CEIC-050 tracker row must include the consolidated gate module",
        )
        require(
            errors,
            "CEIC-051" in row.get("acceptance", "") and "pending" in row.get("acceptance", "").lower(),
            "CEIC-050 tracker row must keep successor optimizer slices pending",
        )
    for slice_id in CEIC_OPTIMIZER_PENDING + CEIC_INTEGRATED_PENDING:
        pending = tracker.get(slice_id)
        require(errors, pending is not None, f"TRACKER.csv missing {slice_id}")
        if pending is not None:
            require(
                errors,
                normalize_status(pending.get("status", "")) == PENDING_STATUS,
                f"{slice_id} must remain pending; CEIC-050 cannot close successor or integrated proof",
            )

    gate_030 = gates.get("CEIC-GATE-030")
    require(errors, gate_030 is not None, "ACCEPTANCE_GATES.csv missing CEIC-GATE-030")
    if gate_030 is not None:
        require(errors, normalize_status(gate_030.get("status", "")) == PENDING_STATUS, "CEIC-GATE-030 must remain pending")
        require(
            errors,
            "CEIC-050" in gate_030.get("evidence", "") and "CEIC-051" in gate_030.get("evidence", ""),
            "CEIC-GATE-030 evidence must distinguish CEIC-050 bridge closure from CEIC-051+ optimizer proof",
        )

    artifact = artifacts.get("CEIC-ART-058")
    require(errors, artifact is not None, "ARTIFACT_INDEX.csv missing CEIC-ART-058")
    if artifact is not None:
        require(errors, artifact.get("slice_id", "") == "CEIC-050", "CEIC-ART-058 must belong only to CEIC-050")
        require(errors, artifact.get("path", "") == rel(ARTIFACT), "CEIC-ART-058 path mismatch")
        require(errors, normalize_status(artifact.get("status", "")) == "present", "CEIC-ART-058 must be present")
        require(errors, "CEIC-GATE-030" in artifact.get("required_for_gate", ""), "CEIC-ART-058 must feed CEIC-GATE-030")
    readiness_manifest = artifacts.get("CEIC-ART-013")
    if readiness_manifest is not None:
        require(
            errors,
            normalize_status(readiness_manifest.get("status", "")) == "planned",
            "CEIC-ART-013 optimizer readiness manifest must remain planned",
        )

    opt_001 = trace.get("OPT-001")
    require(errors, opt_001 is not None, "AUDIT_TRACEABILITY_MATRIX.csv missing OPT-001")
    if opt_001 is not None:
        require(errors, "CEIC-ART-058" in opt_001.get("evidence_artifacts", ""), "OPT-001 must cite CEIC-ART-058")
        require(errors, normalize_status(opt_001.get("status", "")) in COMPLETE_STATUSES, "OPT-001 trace row must be complete")
    opt_011 = trace.get("OPT-011")
    require(errors, opt_011 is not None, "AUDIT_TRACEABILITY_MATRIX.csv missing OPT-011")
    if opt_011 is not None:
        require(errors, normalize_status(opt_011.get("status", "")) == PENDING_STATUS, "OPT-011 must remain pending for CEIC-093")
        require(errors, "CEIC-ART-058" in opt_011.get("evidence_artifacts", ""), "OPT-011 must include CEIC-050 evidence")
        require(errors, "CEIC-ART-015" in opt_011.get("evidence_artifacts", ""), "OPT-011 must keep integrated final proof pending")

    aud_030 = spec_audit.get("CEIC-AUD-030")
    require(errors, aud_030 is not None, "SPEC_IMPLEMENTATION_AUDIT_MATRIX.csv missing CEIC-AUD-030")
    if aud_030 is not None:
        refs = aud_030.get("implementation_refs", "")
        require(errors, "ceic_050_live_route_benchmark_artifact_gate_test.py#CEIC_050_LIVE_ROUTE_BENCHMARK_ARTIFACT_GATE_TEST" in refs, "CEIC-AUD-030 must cite the CEIC-050 gate")
        require(errors, rel(ARTIFACT) in refs, "CEIC-AUD-030 must cite the CEIC-050 artifact")
        require(errors, normalize_status(aud_030.get("status", "")) in COMPLETE_STATUSES, "CEIC-AUD-030 must be complete")
    for audit_id in ("CEIC-AUD-031", "CEIC-AUD-032", "CEIC-AUD-033", "CEIC-AUD-034", "CEIC-AUD-035", "CEIC-AUD-036", "CEIC-AUD-037"):
        row = spec_audit.get(audit_id)
        if row is not None:
            require(errors, normalize_status(row.get("status", "")) == PENDING_STATUS, f"{audit_id} must remain pending")

    dep_030 = dependencies.get("CEIC-DEP-030")
    require(errors, dep_030 is not None, "DEPENDENCIES.csv missing CEIC-DEP-030")
    if dep_030 is not None:
        require(errors, normalize_status(dep_030.get("status", "")) == "available", "CEIC-DEP-030 must be available after CEIC-050")

    lowered_artifact = artifact_text.lower()
    for forbidden in (
        "reference dominance complete",
        "all product enterprise readiness",
        "cluster production readiness",
        "ceic-051 complete",
        "ceic-062 complete",
        "ceic-090 complete",
        "ceic-095 complete",
    ):
        require(errors, forbidden not in lowered_artifact, f"{rel(ARTIFACT)} contains overclaim phrase: {forbidden}")
    for phrase in (
        "evidence only",
        "not transaction finality authority",
        "not visibility authority",
        "not security authority",
        "not recovery authority",
        "not parser authority",
        "not reference authority",
        "not wal authority",
        "not benchmark authority",
        "cluster routes remain external-provider-only",
    ):
        require(errors, phrase in lowered_artifact, f"{rel(ARTIFACT)} missing boundary phrase: {phrase}")


def run(repo_root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    artifact_text = read_text(repo_root, ARTIFACT, errors)
    route_text = read_text(repo_root, ROUTE_GATE, errors)
    runtime_text = read_text(repo_root, RUNTIME_BENCHMARK, errors)
    optimizer_test_cmake = read_text(repo_root, OPTIMIZER_TEST_CMAKE, errors)
    optimizer_source_cmake = read_text(repo_root, OPTIMIZER_SOURCE_CMAKE, errors)
    consolidated_cmake = read_text(repo_root, CONSOLIDATED_CMAKE, errors)
    if errors:
        return errors

    validate_fingerprints(errors, repo_root, artifact_text)
    validate_optimizer_source_anchors(errors, route_text, runtime_text)
    validate_cmake_registration(errors, optimizer_test_cmake, optimizer_source_cmake, consolidated_cmake)
    validate_completed_optimizer_bridge(errors, repo_root)
    validate_consolidated_execution_plan(errors, repo_root, artifact_text)
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args(argv)
    repo_root = pathlib.Path(args.repo_root).resolve()
    errors = run(repo_root)
    if errors:
        for message in errors:
            fail(message)
        return 1
    print("ceic_050_live_route_benchmark_artifact_gate=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
