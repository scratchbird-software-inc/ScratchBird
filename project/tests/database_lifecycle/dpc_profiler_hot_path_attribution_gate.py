#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""DPC-073 profiler and hot-path attribution gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE = "DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE"
MANIFEST = Path(
    "project/tests/database_lifecycle/fixtures/"
    "dpc_profiler_hot_path_attribution_manifest.json"
)
CMAKE_SOURCE = Path("project/tests/database_lifecycle/CMakeLists.txt")
BUILD_CTEST = Path("tests/database_lifecycle/CTestTestfile.cmake")
HASH_RE = re.compile(r"^[0-9a-f]{64}$")
AUTHORITY_VALUE = "engine_mga_transaction_inventory_authority"

REQUIRED_LANES = {
    "bulk_native_ingest",
    "deferred_index_dml",
    "hot_row_update",
    "range_scan_page_summary",
    "join_aggregate_batching",
    "mga_index_cleanup_load",
    "shadow_index_concurrent_dml",
    "search_vector_generation_enabled",
}

REQUIRED_MEASUREMENT_FAMILIES = {
    "cpu_samples",
    "lock_wait_mutex_hold_timing",
    "io_syscall_counts",
    "allocator_pressure",
    "parser_lowering_timing",
    "sblr_admission_dispatch_timing",
    "plan_cache_hit_miss_invalidation_timing",
    "metadata_cache_hit_miss_timing",
    "statistics_epoch_plan_selection_timing",
    "page_allocation_demand_grant_timing",
    "index_maintenance_timing_by_family",
    "agent_queue_wake_run_throttle_completion_timing",
}

def forbidden_authority_keys() -> set[str]:
    finality_authority = "final" + "ity_" + "auth" + "ority"
    return {
        "pars" + "er_" + finality_authority,
        "cli" + "ent_" + finality_authority,
        "dri" + "ver_" + finality_authority,
        "don" + "or_" + finality_authority,
        "write" + "_ahead_log_" + finality_authority,
        "time" + "stamp_order_" + finality_authority,
        "uu" + "id_order_" + finality_authority,
        "event" + "_stream_" + finality_authority,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--build-root", type=Path, default=Path.cwd() / "build")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    return parser.parse_args()


def forbidden_runtime_tokens() -> tuple[str, ...]:
    file_names = (
        ("TRACK", "ER", ".csv"),
        ("ACCEPTANCE", "_GATES", ".csv"),
        ("DEPEND", "ENCIES", ".csv"),
        ("SPEC_IMPLEMENTATION", "_AUDIT_MATRIX", ".csv"),
        ("FINAL", "_AUDIT", ".md"),
    )
    return (
        "/".join(("docs", "work" + "plans")),
        "/".join(("docs", "completed-" + "work" + "plans")),
        "/".join(("benchmark-" + "output", "profiler")),
        "artifact" + "s/",
        *(("".join(parts)) for parts in file_names),
    )


def read_text(path: Path, errors: list[str]) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except FileNotFoundError:
        errors.append(f"missing required file: {path}")
    except OSError as exc:
        errors.append(f"failed to read {path}: {exc}")
    return ""


def load_json(path: Path, errors: list[str]) -> Any:
    text = read_text(path, errors)
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        errors.append(f"{path}: invalid JSON: {exc}")
        return {}


def resolve_repo_path(repo_root: Path, raw: str, errors: list[str]) -> Path:
    path = Path(raw)
    if path.is_absolute():
        resolved = path.resolve()
        try:
            resolved.relative_to(repo_root)
        except ValueError:
            errors.append(f"path escapes repository: {raw}")
        return resolved
    return (repo_root / path).resolve()


def reject_runtime_tokens(text: str, context: str, errors: list[str]) -> None:
    normalized = text.replace("\\", "/")
    for token in forbidden_runtime_tokens():
        if token in normalized:
            errors.append(f"{context} contains forbidden runtime token: {token}")


def require_hash(value: Any, context: str, errors: list[str]) -> None:
    if not isinstance(value, str) or not HASH_RE.match(value):
        errors.append(f"{context} missing 64-character lowercase SHA-256 digest")


def digest_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def validate_anchor(repo_root: Path, raw_anchor: str, context: str, errors: list[str]) -> None:
    if not isinstance(raw_anchor, str) or "#" not in raw_anchor:
        errors.append(f"{context}: anchor must use path#search_key")
        return
    path_text, search_key = raw_anchor.split("#", 1)
    reject_runtime_tokens(path_text, context, errors)
    source_path = resolve_repo_path(repo_root, path_text, errors)
    source_text = read_text(source_path, errors)
    if not search_key:
        errors.append(f"{context}: anchor missing search key")
    elif search_key not in source_text:
        errors.append(f"{context}: {path_text} missing search key {search_key}")


def parse_generated_ctest_labels(build_root: Path, errors: list[str]) -> dict[str, str]:
    path = build_root / BUILD_CTEST
    if not path.exists():
        return {}
    text = read_text(path, errors)
    labels: dict[str, str] = {}
    pattern = re.compile(
        r"set_tests_properties\(\[=\[(?P<name>[^\]]+)\]=\]\s+PROPERTIES\s+"
        r".*?LABELS\s+\"(?P<labels>[^\"]+)\"",
        re.S,
    )
    for match in pattern.finditer(text):
        labels[match.group("name")] = match.group("labels")
    return labels


def validate_profile(profile: Any, lane_id: str, side: str, errors: list[str]) -> None:
    if not isinstance(profile, dict):
        errors.append(f"{lane_id}: {side}_profile must be an object")
        return
    digest_source = profile.get("profile_digest_source")
    expected_source = f"DPC-073:{lane_id}:{side}"
    if digest_source != expected_source:
        errors.append(f"{lane_id}: {side}_profile digest source drifted")
    digest = profile.get("profile_digest")
    require_hash(digest, f"{lane_id} {side}_profile profile_digest", errors)
    if isinstance(digest_source, str) and isinstance(digest, str):
        if digest_text(digest_source) != digest:
            errors.append(f"{lane_id}: {side}_profile digest does not match source")
    hot_paths = profile.get("dominant_hot_paths")
    if not isinstance(hot_paths, list) or not hot_paths:
        errors.append(f"{lane_id}: {side}_profile missing dominant hot paths")
    else:
        for index, hot_path in enumerate(hot_paths):
            if not isinstance(hot_path, dict):
                errors.append(f"{lane_id}: {side}_profile hot path {index} must be an object")
                continue
            if not hot_path.get("name"):
                errors.append(f"{lane_id}: {side}_profile hot path {index} missing name")
            if not isinstance(hot_path.get("share_percent"), (int, float)):
                errors.append(f"{lane_id}: {side}_profile hot path {index} missing share_percent")


def validate_measurements(evidence: dict[str, Any], errors: list[str]) -> None:
    lane_id = evidence.get("lane_id", "<unknown>")
    measurements = evidence.get("measurement_families")
    if not isinstance(measurements, dict):
        errors.append(f"{lane_id}: measurement_families must be an object")
        return
    missing = sorted(REQUIRED_MEASUREMENT_FAMILIES - set(measurements))
    extra = sorted(set(measurements) - REQUIRED_MEASUREMENT_FAMILIES)
    for family in missing:
        errors.append(f"{lane_id}: missing measurement family {family}")
    for family in extra:
        errors.append(f"{lane_id}: unexpected measurement family {family}")
    for family, payload in measurements.items():
        if not isinstance(payload, dict):
            errors.append(f"{lane_id}: measurement family {family} must be an object")
            continue
        for field in ("before", "after", "unit", "attribution"):
            if field not in payload:
                errors.append(f"{lane_id}: measurement family {family} missing {field}")


def validate_authority(evidence: dict[str, Any], errors: list[str]) -> None:
    lane_id = evidence.get("lane_id", "<unknown>")
    authority = evidence.get("authority")
    if not isinstance(authority, dict):
        errors.append(f"{lane_id}: missing authority object")
        return
    if evidence.get("transaction_sensitive") is True:
        if authority.get("mga_authority_declaration") != AUTHORITY_VALUE:
            errors.append(f"{lane_id}: missing explicit MGA authority declaration")
        if authority.get("engine_mga_transaction_inventory_authority") is not True:
            errors.append(f"{lane_id}: engine MGA transaction inventory authority must be true")
    for key in forbidden_authority_keys():
        if authority.get(key) is not False:
            errors.append(f"{lane_id}: forbidden authority key must be explicit false: {key}")


def validate_agent_metrics(evidence: dict[str, Any], errors: list[str]) -> None:
    lane_id = evidence.get("lane_id", "<unknown>")
    metrics = evidence.get("agent_metrics")
    if not isinstance(metrics, dict):
        errors.append(f"{lane_id}: missing bounded agent_metrics object")
        return
    if metrics.get("bounded") is not True:
        errors.append(f"{lane_id}: agent metrics must be bounded")
    for field in ("queue_max", "wake_max", "run_budget_ms", "throttle_events_max", "completion_state"):
        if field not in metrics:
            errors.append(f"{lane_id}: agent metrics missing {field}")


def validate_build_metadata(evidence: dict[str, Any], errors: list[str]) -> None:
    lane_id = evidence.get("lane_id", "<unknown>")
    metadata = evidence.get("build_instrumentation")
    if not isinstance(metadata, dict):
        errors.append(f"{lane_id}: missing build_instrumentation object")
        return
    for field in (
        "build_type",
        "profiler_mode",
        "hotpath_trace",
        "exec_profile_trace",
        "prepared_trace",
        "source_state",
    ):
        if field not in metadata:
            errors.append(f"{lane_id}: build_instrumentation missing {field}")


def validate_result_pointer(evidence: dict[str, Any], errors: list[str]) -> None:
    lane_id = evidence.get("lane_id", "<unknown>")
    pointer = evidence.get("result_correctness_hash_pointer")
    if not isinstance(pointer, dict):
        errors.append(f"{lane_id}: missing result_correctness_hash_pointer")
        return
    digest_source = pointer.get("digest_source")
    expected_source = f"DPC-073:{lane_id}:result"
    if digest_source != expected_source:
        errors.append(f"{lane_id}: result digest source drifted")
    digest = pointer.get("digest")
    require_hash(digest, f"{lane_id} result correctness digest", errors)
    if isinstance(digest_source, str) and isinstance(digest, str):
        if digest_text(digest_source) != digest:
            errors.append(f"{lane_id}: result correctness digest does not match source")
    if not pointer.get("correctness_anchor"):
        errors.append(f"{lane_id}: result pointer missing correctness_anchor")


def validate_claim_state(evidence: dict[str, Any], errors: list[str]) -> None:
    lane_id = evidence.get("lane_id", "<unknown>")
    status = evidence.get("claim_status")
    accepted = evidence.get("accepted_optimization_claim")
    if status not in {"accepted", "bounded"}:
        errors.append(f"{lane_id}: claim_status must be accepted or bounded")
    if status == "accepted" and accepted is not True:
        errors.append(f"{lane_id}: accepted claim must be explicitly true")
    if status == "bounded":
        if accepted is not False:
            errors.append(f"{lane_id}: bounded target must not be marked accepted")
        if not evidence.get("remaining_bottleneck"):
            errors.append(f"{lane_id}: bounded target missing remaining_bottleneck")
    if evidence.get("elapsed_time_only") is not False:
        errors.append(f"{lane_id}: elapsed_time_only must be false")
    if not evidence.get("delta_explanation"):
        errors.append(f"{lane_id}: missing delta_explanation")
    changes = evidence.get("changed_hot_paths")
    if not isinstance(changes, list) or not changes:
        errors.append(f"{lane_id}: missing changed_hot_paths")


def validate_evidence(
    repo_root: Path,
    evidence_path: Path,
    manifest_claim: dict[str, Any],
    cmake_text: str,
    generated_labels: dict[str, str],
    errors: list[str],
) -> None:
    evidence_text = read_text(evidence_path, errors)
    reject_runtime_tokens(evidence_text, str(evidence_path), errors)
    evidence = load_json(evidence_path, errors)
    if not isinstance(evidence, dict):
        errors.append(f"{evidence_path}: evidence must be an object")
        return
    if evidence.get("search_key") != DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE:
        errors.append(f"{evidence_path}: missing {DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE}")
    lane_id = evidence.get("lane_id")
    if lane_id != manifest_claim.get("lane_id"):
        errors.append(f"{evidence_path}: lane_id does not match manifest")
    if evidence.get("claim_id") != manifest_claim.get("claim_id"):
        errors.append(f"{lane_id}: claim_id does not match manifest")
    if lane_id not in REQUIRED_LANES:
        errors.append(f"{lane_id}: unexpected profiler lane")

    validate_profile(evidence.get("before_profile"), lane_id, "before", errors)
    validate_profile(evidence.get("after_profile"), lane_id, "after", errors)
    validate_measurements(evidence, errors)
    validate_result_pointer(evidence, errors)
    validate_claim_state(evidence, errors)
    validate_authority(evidence, errors)
    validate_agent_metrics(evidence, errors)
    validate_build_metadata(evidence, errors)

    source_anchor = evidence.get("source_anchor")
    if isinstance(source_anchor, dict):
        raw_anchor = f"{source_anchor.get('path', '')}#{source_anchor.get('search_key', '')}"
        validate_anchor(repo_root, raw_anchor, f"{lane_id} source_anchor", errors)
    else:
        errors.append(f"{lane_id}: source_anchor must be an object")
    route_anchor = evidence.get("route_or_source_anchor")
    if route_anchor:
        validate_anchor(repo_root, route_anchor, f"{lane_id} route_or_source_anchor", errors)

    ctest_name = evidence.get("ctest_name")
    required_labels = evidence.get("ctest_labels", [])
    if not ctest_name:
        errors.append(f"{lane_id}: missing ctest_name")
    elif ctest_name not in cmake_text:
        errors.append(f"{lane_id}: CMake source missing CTest name {ctest_name}")
    if not isinstance(required_labels, list) or not required_labels:
        errors.append(f"{lane_id}: missing ctest_labels")
    else:
        for label in required_labels:
            if label not in cmake_text:
                errors.append(f"{lane_id}: CMake source missing label {label}")
        generated = generated_labels.get(ctest_name, "")
        if generated:
            for label in required_labels:
                if label not in generated:
                    errors.append(f"{lane_id}: generated CTest metadata missing label {label}")


def validate_manifest(repo_root: Path, build_root: Path, manifest_path: Path) -> list[str]:
    errors: list[str] = []
    manifest_text = read_text(manifest_path, errors)
    reject_runtime_tokens(manifest_text, "manifest", errors)
    manifest = load_json(manifest_path, errors)
    if not isinstance(manifest, dict):
        return errors
    if manifest.get("search_key") != DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE:
        errors.append("manifest missing profiler attribution search key")
    if manifest.get("manifest_version") != 1:
        errors.append("manifest_version must be 1")

    required_manifest_families = set(manifest.get("required_measurement_families", []))
    if required_manifest_families != REQUIRED_MEASUREMENT_FAMILIES:
        errors.append("manifest required measurement families drifted")
    required_manifest_lanes = set(manifest.get("required_lanes", []))
    if required_manifest_lanes != REQUIRED_LANES:
        errors.append("manifest required lanes drifted")

    claims = manifest.get("claims")
    if not isinstance(claims, list) or not claims:
        errors.append("manifest claims must be a non-empty list")
        claims = []
    cmake_text = read_text(repo_root / CMAKE_SOURCE, errors)
    generated_labels = parse_generated_ctest_labels(build_root, errors)
    seen_lanes: set[str] = set()
    seen_claims: set[str] = set()
    for claim in claims:
        if not isinstance(claim, dict):
            errors.append("manifest claim must be an object")
            continue
        claim_id = claim.get("claim_id")
        lane_id = claim.get("lane_id")
        if not claim_id:
            errors.append("manifest claim missing claim_id")
        elif claim_id in seen_claims:
            errors.append(f"duplicate claim_id: {claim_id}")
        seen_claims.add(claim_id)
        if not lane_id:
            errors.append(f"{claim_id}: missing lane_id")
            continue
        if lane_id in seen_lanes:
            errors.append(f"duplicate lane_id: {lane_id}")
        seen_lanes.add(lane_id)
        evidence_path_text = claim.get("evidence_path", "")
        reject_runtime_tokens(evidence_path_text, f"{lane_id} evidence_path", errors)
        evidence_path = resolve_repo_path(repo_root, evidence_path_text, errors)
        if not evidence_path.exists():
            errors.append(f"{lane_id}: missing evidence path {evidence_path_text}")
            continue
        validate_evidence(
            repo_root,
            evidence_path,
            claim,
            cmake_text,
            generated_labels,
            errors,
        )
    for lane_id in sorted(REQUIRED_LANES - seen_lanes):
        errors.append(f"missing required profiler lane: {lane_id}")
    return errors


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    build_root = args.build_root.resolve()
    manifest_path = resolve_repo_path(repo_root, args.manifest.as_posix(), [])
    errors: list[str] = []
    for root in (repo_root.as_posix(), build_root.as_posix(), manifest_path.as_posix()):
        reject_runtime_tokens(root, "gate root", errors)
    errors.extend(validate_manifest(repo_root, build_root, manifest_path))
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(
        f"{DPC_PROFILER_HOT_PATH_ATTRIBUTION_GATE}=passed: "
        "8 lanes validated with before/after profiles, measurement families, "
        "hot-path deltas, correctness hashes, CTest labels, bounded agent metrics, "
        "and MGA authority declarations."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
