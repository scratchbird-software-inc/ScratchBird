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


EXECUTION_PLAN = pathlib.Path("project/tests/release_evidence/consolidated_enterprise_public_evidence")
ARTIFACT = EXECUTION_PLAN / "artifacts/CEIC-050_LIVE_ROUTE_BENCHMARK_ARTIFACT_EVIDENCE.md"
ROUTE_GATE = pathlib.Path("project/tests/optimizer/optimizer_enterprise_route_validation_gate.cpp")
RUNTIME_BENCHMARK = pathlib.Path("project/src/engine/optimizer/runtime_consumption_benchmark_evidence.cpp")
OPTIMIZER_TEST_CMAKE = pathlib.Path("project/tests/optimizer/CMakeLists.txt")
OPTIMIZER_SOURCE_CMAKE = pathlib.Path("project/src/engine/optimizer/CMakeLists.txt")
CONSOLIDATED_CMAKE = pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt")

COMPLETE_STATUSES = {"complete", "completed", "done", "closed", "complete_move_ready"}
REQUIRED_ROUTES = ("embedded", "ipc", "inet", "cli", "driver")
CEIC_OPTIMIZER_COMPLETED = tuple(f"CEIC-{value:03d}" for value in range(51, 63))
CEIC_INTEGRATED_COMPLETE = tuple(f"CEIC-{value:03d}" for value in range(90, 96))
CEIC_INTEGRATED_ARTIFACTS = {
    "CEIC-090": ("CEIC-ART-018", "CEIC-ART-087"),
    "CEIC-091": ("CEIC-ART-088",),
    "CEIC-092": ("CEIC-ART-089",),
    "CEIC-093": ("CEIC-ART-090",),
    "CEIC-094": ("CEIC-ART-091",),
    "CEIC-095": ("CEIC-ART-015",),
}
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


def validate_consolidated_execution_plan(errors: list[str], repo_root: pathlib.Path, artifact_text: str) -> None:
    tracker = index_by(read_csv(repo_root, EXECUTION_PLAN / "CEIC_STATUS_MATRIX.csv", errors), "slice_id")
    gates = index_by(read_csv(repo_root, EXECUTION_PLAN / "CEIC_ACCEPTANCE_MATRIX.csv", errors), "gate_id")
    artifacts = index_by(read_csv(repo_root, EXECUTION_PLAN / "ARTIFACT_INDEX.csv", errors), "artifact_id")

    row = tracker.get("CEIC-050")
    require(errors, row is not None, "CEIC_STATUS_MATRIX.csv missing CEIC-050")
    if row is not None:
        require(errors, normalize_status(row.get("status", "")) in COMPLETE_STATUSES, "CEIC-050 must be complete")
    for slice_id in CEIC_OPTIMIZER_COMPLETED:
        completed = tracker.get(slice_id)
        require(errors, completed is not None, f"CEIC_STATUS_MATRIX.csv missing {slice_id}")
        if completed is not None:
            require(
                errors,
                normalize_status(completed.get("status", "")) in COMPLETE_STATUSES,
                f"{slice_id} must be complete with CEIC-062 closed",
            )
    for slice_id in CEIC_INTEGRATED_COMPLETE:
        integrated = tracker.get(slice_id)
        require(errors, integrated is not None, f"CEIC_STATUS_MATRIX.csv missing {slice_id}")
        if integrated is not None:
            require(
                errors,
                normalize_status(integrated.get("status", "")) in COMPLETE_STATUSES,
                f"{slice_id} integrated release proof must be complete",
            )
        for artifact_id in CEIC_INTEGRATED_ARTIFACTS[slice_id]:
            integrated_artifact = artifacts.get(artifact_id)
            require(errors, integrated_artifact is not None, f"ARTIFACT_INDEX.csv missing {artifact_id}")
            if integrated_artifact is not None:
                require(errors, integrated_artifact.get("slice_id", "") == slice_id, f"{artifact_id} must belong to {slice_id}")
                require(
                    errors,
                    normalize_status(integrated_artifact.get("status", "")) in COMPLETE_STATUSES | {"present", "generated"},
                    f"{artifact_id} must be present for {slice_id} integrated release proof",
                )
                path = integrated_artifact.get("path", "").strip()
                if path and not (repo_root / path).exists():
                    require(errors, False, f"{artifact_id} path is missing: {path}")

    gate_030 = gates.get("CEIC-GATE-030")
    require(errors, gate_030 is not None, "CEIC_ACCEPTANCE_MATRIX.csv missing CEIC-GATE-030")
    if gate_030 is not None:
        require(errors, normalize_status(gate_030.get("status", "")) in COMPLETE_STATUSES, "CEIC-GATE-030 must be complete")

    artifact = artifacts.get("CEIC-ART-058")
    require(errors, artifact is not None, "ARTIFACT_INDEX.csv missing CEIC-ART-058")
    if artifact is not None:
        require(errors, artifact.get("slice_id", "") == "CEIC-050", "CEIC-ART-058 must belong only to CEIC-050")
        require(errors, artifact.get("path", "") == rel(ARTIFACT), "CEIC-ART-058 path mismatch")
        require(errors, normalize_status(artifact.get("status", "")) == "present", "CEIC-ART-058 must be present")
    readiness_manifest = artifacts.get("CEIC-ART-013")
    if readiness_manifest is not None:
        require(
            errors,
            normalize_status(readiness_manifest.get("status", "")) in COMPLETE_STATUSES | {"generated", "present"},
            "CEIC-ART-013 optimizer readiness manifest must be generated/present",
        )

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
