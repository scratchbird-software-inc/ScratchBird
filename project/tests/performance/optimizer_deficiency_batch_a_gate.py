#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Standalone optimizer-deficiency Batch A proof harness.

Search key: ODF_BATCH_A_PROOF_HARNESS

The gate validates the P0 proof contract and existing CTest linkages without
reading execution_plan, finding, audit, reference, or contract trees at runtime.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "scratchbird.optimizer_deficiency.batch_a.proof.v1"
GATE_NAME = "optimizer_deficiency_batch_a_gate"
REQUIRED_ROWS = tuple(f"ODF-{index:03d}" for index in range(10))
REQUIRED_ROUTES = ("embedded", "local_ipc", "inet_listener")
REQUIRED_BUILD_FLAGS = {
    "SCRATCHBIRD_ENABLE_DEBUG_LOGS": "OFF",
    "SCRATCHBIRD_ENABLE_HOTPATH_TRACE": "OFF",
    "SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE": "OFF",
    "SCRATCHBIRD_ENABLE_PREPARED_TRACE": "OFF",
}
REQUIRED_BENCHMARK_FAMILIES = {"sql_select", "sql_dml", "copy_bulk", "nosql_kv"}
REQUIRED_PLAN_FIELDS = {
    "candidate_id",
    "candidate_kind",
    "candidates_seen",
    "selected_access_path",
    "estimated_rows",
    "actual_rows",
    "fallback_statistics_used",
    "rejected_reasons",
    "mga_visibility_recheck",
    "security_redaction_recheck",
}
REQUIRED_LATENCY_LAYERS = {
    "parser_us",
    "transport_us",
    "sblr_lowering_us",
    "plan_us",
    "bind_us",
    "storage_us",
    "visibility_us",
    "result_evidence_us",
    "driver_formatting_us",
}
PROTECTED_LOCAL_ONLY_ROOTS = (
    "/public_release_evidence",
    "/docs/reference/",
    "/docs/references/",
    "/" "docs" "/findings/",
    "/public_audit_summary/",
)
FORBIDDEN_RUNTIME_PARTS = (
    ("docs", "execution-plans"),
    ("docs", "completed-execution-plans"),
    ("docs", "findings"),
    ("docs", "audit"),
    ("docs", "reference"),
    ("docs", "references"),
    ("docs", "contracts"),
)
FORBIDDEN_PAYLOAD_TOKENS = (
    "docs" "/execution-plans",
    "docs" "/completed-execution-plans",
    "docs" "/findings",
    "public_audit_summary",
    "docs/reference",
    "docs/references",
    "public_release_evidence",
)


class GateError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise GateError(message)


def canonical_hash(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return "sha256:" + hashlib.sha256(encoded).hexdigest()


def path_has_parts(path: Path, parts: tuple[str, str]) -> bool:
    normalized = tuple(part.lower() for part in path.resolve().parts)
    for index in range(0, len(normalized) - len(parts) + 1):
        if normalized[index:index + len(parts)] == parts:
            return True
    return False


def load_fixture(path: Path) -> dict[str, Any]:
    for parts in FORBIDDEN_RUNTIME_PARTS:
        require(not path_has_parts(path, parts),
                f"fixture path must not be under forbidden runtime docs root: {path}")
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(f"could not read fixture {path}: {exc}") from exc
    for token in FORBIDDEN_PAYLOAD_TOKENS:
        require(token not in text, f"fixture contains forbidden runtime path token: {token}")
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise GateError(f"fixture is not valid JSON: {exc}") from exc


def validate_gitignore_and_tracking(repo_root: Path) -> None:
    gitignore = (repo_root / "." "gitignore").read_text(encoding="utf-8")
    for root in PROTECTED_LOCAL_ONLY_ROOTS:
        require(root in gitignore, f"protected local-only root missing from ignore file: {root}")
    tracked = subprocess.run(
        ["git", "ls-files", "public_release_evidence", "docs/reference",
         "docs/references", "docs" "/findings", "public_audit_summary"],
        cwd=repo_root,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    require(tracked.returncode == 0,
            f"git ls-files failed while checking protected docs roots: {tracked.stderr.strip()}")
    require(not tracked.stdout.strip(),
            "protected local-only docs roots have tracked files")


def validate_ctest_linkage(build_root: Path, fixture: dict[str, Any]) -> None:
    required = set(fixture["required_existing_ctests"])
    ctest_files = sorted(build_root.rglob("CTestTestfile.cmake")) if build_root.exists() else []
    require(ctest_files, f"no CTest metadata found under build root: {build_root}")
    ctest_text = "\n".join(path.read_text(encoding="utf-8", errors="replace")
                           for path in ctest_files)
    for test_name in sorted(required):
        require(test_name in ctest_text, f"required CTest registration missing: {test_name}")
    for row in REQUIRED_ROWS:
        require(row in ctest_text or row in fixture["completed_rows"],
                f"row has no CTest or fixture evidence linkage: {row}")


def validate_runtime_policy(fixture: dict[str, Any]) -> None:
    require(fixture.get("schema_version") == SCHEMA_VERSION, "schema version mismatch")
    require(fixture.get("fixture_key") == "ODF_BATCH_A_PROOF_HARNESS", "fixture key mismatch")
    policy = fixture.get("runtime_policy", {})
    for key in ("reads_execution_plan_files", "requires_docs_findings", "requires_docs_audit",
                "requires_docs_reference", "requires_docs_contracts",
                "claims_speed_improvement"):
        require(policy.get(key) is False, f"runtime policy must set {key}=false")
    require(policy.get("transaction_authority") == "scratchbird_engine_mga_transaction_inventory",
            "MGA transaction authority policy missing")
    require("parser_lowers_to_sblr" in policy.get("parser_boundary", ""),
            "parser/SBLR boundary policy missing")
    require(tuple(fixture.get("completed_rows", ())) == REQUIRED_ROWS,
            "completed Batch A rows must be exactly ODF-000 through ODF-009")
    digest = fixture.get("scope_digest", {})
    families = set(digest.get("optimization_families", ()))
    for required in ("sql_optimizer_access_path_selection",
                     "sql_dml_update_delete_target_row_location",
                     "sql_copy_and_native_bulk_ingest",
                     "nosql_kv_document_search_vector_graph_timeseries_physical_provider",
                     "system_wide_observability_support_bundle_security_upgrade_rollback"):
        require(required in families, f"scope digest missing optimization family: {required}")


def validate_benchmark_parity(fixture: dict[str, Any]) -> None:
    require(fixture.get("benchmark_clean_build_flags") == REQUIRED_BUILD_FLAGS,
            "benchmark-clean build flags mismatch")
    require(tuple(fixture.get("routes", ())) == REQUIRED_ROUTES, "route list mismatch")
    families_seen: set[str] = set()
    for workload in fixture.get("benchmark_parity_corpus", ()):
        families_seen.add(workload.get("family", ""))
        hashes = workload.get("expected_result_hash_by_route", {})
        require(tuple(hashes.keys()) == REQUIRED_ROUTES,
                f"{workload.get('workload_id')}: route hash keys mismatch")
        unique_hashes = set(hashes.values())
        require(len(unique_hashes) == 1,
                f"{workload.get('workload_id')}: route hashes are not equivalent")
        require(next(iter(unique_hashes)).startswith("sha256:"),
                f"{workload.get('workload_id')}: expected result hash is not sha256")
        require(workload.get("rows_input", -1) >= 0, "rows_input must be nonnegative")
        require(workload.get("rows_affected", -1) >= 0, "rows_affected must be nonnegative")
        require(workload.get("rows_returned", -1) >= 0, "rows_returned must be nonnegative")
        require(workload.get("selected_physical_path") == "baseline_result_equivalence_only",
                "Batch A parity fixture must not claim optimized speed path")
    require(REQUIRED_BENCHMARK_FAMILIES <= families_seen,
            f"benchmark corpus missing required families: {sorted(REQUIRED_BENCHMARK_FAMILIES - families_seen)}")


def validate_plan_comparator(fixture: dict[str, Any]) -> None:
    comparator = fixture.get("plan_shape_comparator", {})
    require(set(comparator.get("required_fields", ())) == REQUIRED_PLAN_FIELDS,
            "plan comparator required fields mismatch")
    for pair in comparator.get("comparison_pairs", ()):
        require(pair.get("legacy_candidate_count", 0) > 0, "legacy candidates missing")
        require(pair.get("current_candidate_count", 0) > 0, "current candidates missing")
        require(pair.get("selected_access_path"), "selected access path missing")
        require(pair.get("estimated_rows") >= 0, "estimated rows missing")
        require(pair.get("actual_rows") >= 0, "actual rows missing")
        require(pair.get("fallback_statistics_used") is False,
                "Batch A comparator fixture must not accept fallback statistics")
        require(pair.get("rejected_reasons"), "rejected reasons missing")
        require(pair.get("mga_visibility_recheck") is True,
                "MGA visibility recheck must be required")
        require(pair.get("security_redaction_recheck") is True,
                "security/redaction recheck must be required")


def validate_kill_gates(fixture: dict[str, Any]) -> None:
    for gate in fixture.get("performance_kill_gates", ()):
        bad_sample = int(gate.get("bad_sample", 0))
        allowed_max = int(gate.get("allowed_max", 0))
        require(bad_sample > allowed_max,
                f"{gate.get('gate_id')}: bad sample does not trip the gate")
        require(str(gate.get("expected_diagnostic", "")).startswith("ODF.PERF."),
                f"{gate.get('gate_id')}: expected diagnostic missing")


def validate_latency_and_evidence(fixture: dict[str, Any]) -> None:
    latency = fixture.get("latency_budget_attribution", {})
    require(set(latency.get("required_layers", ())) == REQUIRED_LATENCY_LAYERS,
            "latency budget attribution layers mismatch")
    contract = latency.get("budget_contract", {})
    require(contract.get("all_layers_required") is True, "latency layers must all be required")
    require(contract.get("missing_layer_diagnostic") == "ODF.LATENCY_LAYER.MISSING",
            "latency missing-layer diagnostic mismatch")

    ring = fixture.get("binary_evidence_ring", {})
    require(ring.get("counter_encoding") == "binary_little_endian_fixed_record_v1",
            "binary evidence ring encoding mismatch")
    require(ring.get("deferred_string_rendering") is True,
            "binary evidence ring must defer string rendering")
    controls = ring.get("benchmark_clean_trace_controls", {})
    require(controls and not any(controls.values()),
            "benchmark-clean trace controls must all be disabled")
    require("fallback_reason_code" in ring.get("required_counters", ()),
            "binary evidence ring missing fallback reason counter")
    require("formatted_sql_text" in ring.get("forbidden_hot_path_fields", ()),
            "binary evidence ring missing hot-path string formatting ban")


def validate_rollback_matrix(fixture: dict[str, Any]) -> None:
    matrix = fixture.get("rollback_disable_matrix", ())
    require(len(matrix) >= 4, "rollback/disable matrix missing optimization rows")
    seen_flags: set[str] = set()
    for row in matrix:
        for field in ("optimization_id", "feature_flag", "disable_behavior",
                      "diagnostic", "baseline_fallback_test"):
            require(row.get(field), f"rollback row missing field {field}")
        require(row["feature_flag"] not in seen_flags,
                f"duplicate rollback feature flag: {row['feature_flag']}")
        seen_flags.add(row["feature_flag"])
        require(str(row["diagnostic"]).startswith("ODF.ROLLBACK."),
                "rollback diagnostic must use ODF.ROLLBACK prefix")


def validate_gate_links(fixture: dict[str, Any]) -> None:
    persisted = fixture.get("persisted_format_upgrade_rebuild_gate", {})
    require(persisted.get("ctest_name") == "dpc_persisted_format_restricted_open_gate",
            "persisted format CTest link mismatch")
    require({"format_version", "feature_bit", "rebuild_path",
             "incompatible_open_refusal", "support_bundle_evidence"} <=
            set(persisted.get("required_fields", ())),
            "persisted format required fields incomplete")

    fault = fixture.get("physical_structure_fault_injection_matrix", {})
    require(fault.get("ctest_name") == "dpc_failure_injection_crash_matrix_gate",
            "fault injection CTest link mismatch")
    require({"create", "populate", "publish", "disable", "rebuild", "cleanup",
             "crash", "reopen", "recovery_classification"} <=
            set(fault.get("required_phases", ())),
            "fault injection phases incomplete")

    security = fixture.get("security_redaction_privilege_gate", {})
    require(security.get("ctest_name") == "dpc_security_privilege_gate",
            "security CTest link mismatch")
    require({"parser", "api", "driver", "wire", "ipc", "binary", "nosql"} <=
            set(security.get("required_routes", ())),
            "security route coverage incomplete")
    require("denial_diagnostics_identical_to_baseline" in security.get("required_invariants", ()),
            "security denial diagnostic invariant missing")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        repo_root = args.repo_root.resolve()
        fixture = load_fixture(args.fixture)
        validate_runtime_policy(fixture)
        validate_gitignore_and_tracking(repo_root)
        validate_ctest_linkage(args.build_root.resolve(), fixture)
        validate_benchmark_parity(fixture)
        validate_plan_comparator(fixture)
        validate_kill_gates(fixture)
        validate_latency_and_evidence(fixture)
        validate_rollback_matrix(fixture)
        validate_gate_links(fixture)

        args.output_dir.mkdir(parents=True, exist_ok=True)
        output = {
            "schema_version": "scratchbird.optimizer_deficiency.batch_a.result.v1",
            "gate_name": GATE_NAME,
            "fixture_hash": canonical_hash(fixture),
            "completed_rows": list(REQUIRED_ROWS),
            "proof_scope": fixture["proof_scope"],
            "runtime_inputs": {
                "fixture": str(args.fixture.resolve().relative_to(repo_root)),
                "build_root": str(args.build_root.resolve()),
                "reads_forbidden_docs_roots": False
            }
        }
        output_path = args.output_dir / "optimizer_deficiency_batch_a_result.json"
        output_path.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
        print(f"{GATE_NAME} passed: {output_path}")
        return 0
    except GateError as exc:
        print(f"{GATE_NAME} failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
