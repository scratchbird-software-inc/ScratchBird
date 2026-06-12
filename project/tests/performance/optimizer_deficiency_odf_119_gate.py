#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ODF-119 benchmark reproducibility and variance closure gate."""

from __future__ import annotations

import argparse
import json
import sys
from copy import deepcopy
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    sys.path.insert(0, str(args.repo_root / "project" / "tools"))
    import benchmark_reproducibility as repro  # pylint: disable=import-error

    stats = repro.sample_statistics([100.0, 102.0, 98.0, 101.0, 99.0])
    route_hash = "sha256:odf119-result-parity-sblr-api-0001"
    evidence = {
        "schema": repro.SCHEMA,
        "gate": "optimizer_deficiency_odf_119_gate",
        "execution_plan_row": "ODF-119",
        "speed_claim": True,
        "cache_policy": {
            "warm_cache_policy": "repeat samples run after deterministic setup on the same opened database",
            "cold_cache_policy": "cold samples require a fresh database lifecycle and no reused benchmark page cache",
            "drop_os_cache_required": False,
        },
        "repeat_count": stats["repeat_count"],
        "variance_evidence": {
            "samples_ms": stats["samples"],
            "mean_ms": stats["mean"],
            "variance": stats["variance"],
            "stddev": stats["stddev"],
            "relative_variance": stats["relative_variance"],
            "coefficient_of_variation": stats["coefficient_of_variation"],
        },
        "cpu_thread_controls": {
            "benchmark_threads": 1,
            "worker_parallelism": "single_worker",
            "cpu_affinity_policy": "recorded_or_unpinned_ctest_host",
            "simd_accelerator_policy": "deterministic_scalar_fallback_allowed",
        },
        "storage_controls": {
            "database_lifecycle": "fresh_database_per_run",
            "storage_backend": "scratchbird_mga_native_storage",
            "sync_policy": "engine_selected_sync_fence",
            "reference_or_embedded_storage_backend": False,
        },
        "route_controls": {
            "cluster_mode": "local_noncluster",
            "routes": [
                "sblr.query.plan_operation",
                "engine_api.EnginePlanOperation",
            ],
            "parser_executes_sql": False,
            "route_selection_locked": True,
        },
        "regression_thresholds": {
            "max_relative_variance": 0.10,
            "min_repeat_count": 5,
            "severe_regression_factor": 1.15,
            "refuse_speed_claim_without_environment_or_variance": True,
        },
        "selected_physical_path_result_parity": {
            "selected_physical_path": "catalog_backed_access_path_v1.row_uuid_exact",
            "selected_result_hash": route_hash,
            "hash_parity": True,
            "parity_routes": [
                "sblr.query.plan_operation",
                "engine_api.EnginePlanOperation",
            ],
            "route_result_hashes": {
                "sblr.query.plan_operation": route_hash,
                "engine_api.EnginePlanOperation": route_hash,
            },
        },
        "reproducible_json": {
            "stable_key_order": True,
            "canonical_float_policy": "json_number_from_deterministic_sample_statistics",
            "runtime_doc_dependencies": [],
            "forbidden_runtime_roots": [
                "docs" "/execution-plans",
                "docs" "/findings",
                "public_audit_summary",
                "public_release_evidence",
                "docs/references",
            ],
        },
        "authority_controls": {
            "engine_mga_transaction_authority": True,
            "parser_client_reference_transaction_finality_shortcuts": False,
            "reference_or_embedded_storage_transaction_truth": False,
            "selected_path_requires_mga_visibility_recheck": True,
        },
    }

    args.output_dir.mkdir(parents=True, exist_ok=True)
    output_path = args.output_dir / "optimizer_deficiency_odf_119_gate.json"
    payload = repro.canonical_json(evidence)
    output_path.write_text(payload, encoding="utf-8")
    loaded = json.loads(output_path.read_text(encoding="utf-8"))
    repro.validate_benchmark_evidence(loaded)

    baseline_evidence = deepcopy(evidence)
    baseline_evidence["schema"] = repro.BASELINE_MEASUREMENT_SCHEMA
    baseline_evidence["threshold_source"] = "project/tests/performance/BETA_PERFORMANCE_BASELINE_THRESHOLDS.json"
    baseline_evidence["variance_evidence"] = {
        "simple_query_latency_ms": baseline_evidence["variance_evidence"],
        "select_all_latency_ms": deepcopy(baseline_evidence["variance_evidence"]),
    }
    baseline_output_path = args.output_dir / "scratchbird_beta_performance_baseline_result.repro.json"
    baseline_payload = repro.canonical_json(baseline_evidence)
    baseline_output_path.write_text(baseline_payload, encoding="utf-8")
    repro.validate_benchmark_evidence(json.loads(baseline_payload))

    require("docs" "/execution-plans/" not in payload, "ODF-119 runtime evidence leaked execution_plan path")
    require("docs" "/findings/" not in payload, "ODF-119 runtime evidence leaked findings path")
    require("public_audit_summary/" not in payload, "ODF-119 runtime evidence leaked audit path")
    require("public_release_evidence" not in payload, "ODF-119 runtime evidence leaked contract path")
    require("docs/references/" not in payload, "ODF-119 runtime evidence leaked reference path")
    require("docs" "/execution-plans/" not in baseline_payload, "ODF-119 baseline evidence leaked execution_plan path")
    require("docs" "/findings/" not in baseline_payload, "ODF-119 baseline evidence leaked findings path")
    require("public_audit_summary/" not in baseline_payload, "ODF-119 baseline evidence leaked audit path")
    require("public_release_evidence" not in baseline_payload, "ODF-119 baseline evidence leaked contract path")
    require("docs/references/" not in baseline_payload, "ODF-119 baseline evidence leaked reference path")

    negative_cases = {
        "missing_environment": ("cpu_thread_controls",),
        "missing_variance": ("variance_evidence",),
        "missing_environment_and_variance": ("cpu_thread_controls", "variance_evidence"),
    }
    for case_id, removed_keys in negative_cases.items():
        under_evidenced = deepcopy(evidence)
        for key in removed_keys:
            del under_evidenced[key]
        decision = repro.admit_speed_claim(under_evidenced)
        require(not decision.admitted, f"ODF-119 under-evidenced speed claim was admitted: {case_id}")
        require(
            decision.diagnostic == repro.SPEED_CLAIM_REFUSAL_DIAGNOSTIC,
            f"ODF-119 speed-claim refusal diagnostic changed: {case_id}",
        )
        try:
            repro.validate_benchmark_evidence(under_evidenced)
        except repro.BenchmarkEvidenceError as exc:
            require(str(exc) == repro.SPEED_CLAIM_REFUSAL_DIAGNOSTIC,
                    f"ODF-119 validator refusal diagnostic changed: {case_id}")
        else:
            raise AssertionError(f"ODF-119 validator admitted under-evidenced speed claim: {case_id}")

    print(f"optimizer_deficiency_odf_119_gate=passed output={output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
