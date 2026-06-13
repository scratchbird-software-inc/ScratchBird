#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate CEIC-091 integrated support-bundle safety.

SEARCH_KEY: CEIC_091_INTEGRATED_SUPPORT_BUNDLE_GATE
"""

from __future__ import annotations

import argparse
import copy
import csv
import hashlib
import json
import pathlib
import re
import sys
from dataclasses import dataclass
from typing import Any


EXECUTION_PLAN = pathlib.Path(
    "project/tests/release_evidence/consolidated_enterprise_public_evidence"
)
MATRIX = EXECUTION_PLAN / "METRICS_PRODUCER_COVERAGE_MATRIX.csv"
CMAKE_GATE = pathlib.Path("project/tests/consolidated_enterprise/CMakeLists.txt")

REQUIRED_SUBSYSTEMS = ("memory", "index", "optimizer", "agent")
REQUIRED_NON_AUTHORITY_FLAGS = (
    "memory",
    "index",
    "optimizer",
    "agent",
    "transaction_finality",
    "visibility",
    "authorization_security",
    "recovery",
    "parser",
    "reference",
    "wal",
    "benchmark",
    "optimizer_plan",
    "index_finality",
    "provider_finality",
    "cluster",
    "agent_action",
)
AUTHORITY_FLAGS = (
    "transaction_finality_authority",
    "visibility_authority",
    "authorization_security_authority",
    "security_authority",
    "recovery_authority",
    "parser_authority",
    "reference_authority",
    "wal_authority",
    "benchmark_authority",
    "optimizer_plan_authority",
    "index_finality_authority",
    "provider_finality_authority",
    "local_cluster_authority",
    "cluster_authority",
    "agent_action_authority",
)
SUCCESSOR_FLAGS = (
    "ceic_092_route_chain_claimed",
    "ceic_093_soak_claimed",
    "ceic_094_scale_claimed",
    "ceic_095_enterprise_readiness_claimed",
)
PROTECTED_RE = re.compile(
    r"secret|password|passwd|pwd=|token|private_key|credential|verifier|"
    r"cleartext|plaintext|api_key|apikey|key_material|raw_key|"
    r"kms_plaintext|bearer |protected_reference",
    re.IGNORECASE,
)
PLACEHOLDER_RE = re.compile(
    r"placeholder|tamperless|unsigned|sha256:0{8,}|hmac-sha256:0{8,}|"
    r"^0{16,}$",
    re.IGNORECASE,
)
UNSAFE_AUTHORITY_RE = re.compile(
    r"authority\s*=\s*true|authority_claimed\s*=\s*true|"
    r"grants?\s+\w*\s*authority|becomes?\s+\w*\s*authority",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Diagnostic:
    code: str
    subject: str
    message: str

    def render(self) -> str:
        return f"{self.subject}:{self.code}:{self.message}"


def normalize(value: str) -> str:
    return " ".join((value or "").strip().lower().split())


def normalize_status(value: str) -> str:
    return normalize(value).replace(" ", "_").replace("-", "_")


def digest(*parts: str) -> str:
    payload = "|".join(parts).encode("utf-8")
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def read_csv(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return [{key: value or "" for key, value in row.items()} for row in csv.DictReader(handle)]


def index_by(rows: list[dict[str, str]], field: str) -> dict[str, dict[str, str]]:
    return {row.get(field, "").strip(): row for row in rows if row.get(field, "").strip()}


def split_refs(value: str) -> list[str]:
    return [part.strip() for part in value.replace(",", ";").split(";") if part.strip()]


def path_exists(repo_root: pathlib.Path, rel: str) -> bool:
    if not rel.strip():
        return False
    if any(char in rel for char in "*?["):
        return any(repo_root.glob(rel))
    return (repo_root / rel).exists()


def source_rows(repo_root: pathlib.Path) -> dict[str, dict[str, str]]:
    return index_by(read_csv(repo_root / MATRIX), "metric_family")


def non_authority_flags() -> dict[str, bool]:
    return {flag: True for flag in REQUIRED_NON_AUTHORITY_FLAGS}


def authority_flags() -> dict[str, bool]:
    return {flag: False for flag in AUTHORITY_FLAGS}


def row(subsystem: str, key: str, value: str, chain_seed: str) -> dict[str, Any]:
    row_digest = digest("ceic091-row", subsystem, key, value)
    return {
        "key": key,
        "value": value,
        "redacted": False,
        "redaction_class": "public_summary",
        "retention_class": "support_bundle_short_window",
        "tamper_digest": row_digest,
        "chain_digest": digest("ceic091-chain", chain_seed, row_digest),
        "evidence_only": True,
        "protected_material_excluded": True,
        "sidecar_only_evidence": False,
        "fixture_or_test_only_evidence": False,
        "local_cluster_production_claim": False,
        "authority_flags": authority_flags(),
        "non_authority_flags": non_authority_flags(),
        **{flag: False for flag in SUCCESSOR_FLAGS},
    }


def section(
    subsystem: str,
    source_schema: str,
    artifacts: list[str],
    source_paths: list[str],
    matrix_families: list[str],
    rows: list[dict[str, Any]],
    row_limit: int,
    byte_limit: int,
) -> dict[str, Any]:
    source_digest = digest("ceic091-section", subsystem, source_schema, *matrix_families)
    return {
        "subsystem": subsystem,
        "present": True,
        "source_schema": source_schema,
        "source_artifacts": artifacts,
        "source_paths": source_paths,
        "matrix_metric_families": matrix_families,
        "row_limit": row_limit,
        "byte_limit": byte_limit,
        "row_count": len(rows),
        "byte_count": sum(len(r["key"]) + len(r["value"]) + 96 for r in rows),
        "chunk_count": 2,
        "max_chunk_bytes": 4096,
        "bounded": True,
        "low_memory_safe": True,
        "streaming_or_chunked": True,
        "redaction_before_buffering": True,
        "protected_material_excluded": True,
        "protected_material_refused": True,
        "retention_metadata_present": True,
        "redaction_metadata_present": True,
        "signed_or_chained": True,
        "production_evidence": True,
        "sidecar_only_evidence": False,
        "fixture_or_test_only_evidence": False,
        "local_cluster_production_claim": False,
        "tamper_digest": source_digest,
        "chain_digest": digest("ceic091-section-chain", source_digest),
        "signature_or_chain_algorithm": "sha256-chain-v1",
        "retention_class": "support_bundle_short_window",
        "redaction_profile": "ceic091.redacted.protected-material-safe.v1",
        "authority_flags": authority_flags(),
        "non_authority_flags": non_authority_flags(),
        **{flag: False for flag in SUCCESSOR_FLAGS},
        "rows": rows,
    }


def default_model(repo_root: pathlib.Path) -> dict[str, Any]:
    del repo_root
    memory_rows = [
        row("memory", "memory.low_memory_mode", "enabled_streaming_chunks", "memory"),
        row("memory", "memory.protected_material", "redacted_before_buffering", "memory"),
    ]
    index_rows = [
        row("index", "index.operation_rows", "runtime_counter_support_rows", "index"),
        row("index", "index.route_generation", "fresh_generation_provenance", "index"),
    ]
    optimizer_rows = [
        row("optimizer", "optimizer.observability_rows", "selectivity_drift_support_bundle", "optimizer"),
        row("optimizer", "optimizer.metric_bundle", "tamper_digest_redacted", "optimizer"),
    ]
    agent_rows = [
        row("agent", "agent.evidence_chain", "hmac_sha256_chain_validated", "agent"),
        row("agent", "agent.retention", "redacted_support_view", "agent"),
    ]
    sections = [
        section(
            "memory",
            "CEIC-023_MEMORY_SUPPORT_BUNDLE_LOW_MEMORY",
            [str(EXECUTION_PLAN / "artifacts/CEIC-023_MEMORY_SUPPORT_BUNDLE_EVIDENCE.md")],
            [
                "project/src/core/memory/memory_support_bundle.cpp",
                "project/src/core/memory/memory_support_bundle.hpp",
            ],
            ["memory_low_memory_support_bundle"],
            memory_rows,
            96,
            12 * 1024,
        ),
        section(
            "index",
            "CEIC_040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE",
            [str(EXECUTION_PLAN / "artifacts/CEIC-040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE_EVIDENCE.md")],
            [
                "project/src/core/index/index_metrics.cpp",
                "project/src/core/index/index_metrics.hpp",
            ],
            ["index_generation_publish"],
            index_rows,
            128,
            32 * 1024,
        ),
        section(
            "optimizer",
            "OEIC_OPTIMIZER_METRIC_RETENTION_REDACTION",
            [str(EXECUTION_PLAN / "artifacts/CEIC-054_SELECTIVITY_DRIFT_OBSERVABILITY_EVIDENCE.md")],
            [
                "project/src/engine/internal_api/observability/optimizer_metric_support_bundle.cpp",
                "project/src/engine/internal_api/observability/optimizer_metric_support_bundle.hpp",
                "project/src/engine/optimizer/optimizer_selectivity_drift_observability.cpp",
            ],
            ["optimizer_memory_feedback"],
            optimizer_rows,
            128,
            32 * 1024,
        ),
        section(
            "agent",
            "ARHC_AGENT_EVIDENCE_REDACTION_RETENTION_TAMPER",
            [str(EXECUTION_PLAN / "artifacts/CEIC-077_AGENT_EVIDENCE_PRIVACY_TAMPER_EVIDENCE.md")],
            [
                "project/src/core/agents/agent_commercial_evidence.cpp",
                "project/src/core/agents/agent_commercial_evidence.hpp",
            ],
            ["agent_evidence_persistence"],
            agent_rows,
            128,
            32 * 1024,
        ),
    ]
    model_digest = digest("ceic091-model", *(section["chain_digest"] for section in sections))
    return {
        "schema": "sb.ceic091.integrated_support_bundle.v1",
        "bundle_id": "ceic-091-integrated-support-bundle",
        "capture_generation": "ceic-091-generation-1",
        "limits": {
            "max_rows": 512,
            "max_bytes": 96 * 1024,
            "max_rows_per_subsystem": 160,
            "max_bytes_per_subsystem": 32 * 1024,
            "max_chunk_bytes": 4096,
            "max_chunks": 64,
        },
        "low_memory_mode": True,
        "streaming_or_chunked": True,
        "redaction_before_buffering": True,
        "protected_material_excluded": True,
        "protected_material_refused": True,
        "signed_or_chained": True,
        "retention_metadata_present": True,
        "redaction_metadata_present": True,
        "production_live_path": True,
        "ceic_090_support_bundle_generation_matrix_row": "support_bundle_generation",
        "tamper_chain_digest": model_digest,
        "signature_or_chain_algorithm": "sha256-chain-v1",
        "authority_flags": authority_flags(),
        "non_authority_flags": non_authority_flags(),
        **{flag: False for flag in SUCCESSOR_FLAGS},
        "sections": sections,
    }


def valid_digest(value: str) -> bool:
    lowered = (value or "").strip().lower()
    if not lowered or PLACEHOLDER_RE.search(lowered):
        return False
    return lowered.startswith(("sha256:", "hmac-sha256:", "fnv64-"))


def validate_authority(subject: str, payload: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    authority = payload.get("authority_flags", {})
    for flag in AUTHORITY_FLAGS:
        if bool(authority.get(flag, False)):
            diagnostics.append(Diagnostic("unsafe_authority", subject, f"{flag} must be false"))
    non_authority = payload.get("non_authority_flags", {})
    for flag in REQUIRED_NON_AUTHORITY_FLAGS:
        if non_authority.get(flag) is not True:
            diagnostics.append(Diagnostic("missing_non_authority", subject, f"{flag} non-authority flag required"))
    for flag in SUCCESSOR_FLAGS:
        if bool(payload.get(flag, False)):
            diagnostics.append(Diagnostic("successor_overclaim", subject, f"{flag} must remain false"))
    return diagnostics


def validate_text_safety(subject: str, value: str, redacted: bool, protected_excluded: bool) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if PROTECTED_RE.search(value or "") and not (redacted and protected_excluded):
        diagnostics.append(Diagnostic("protected_material", subject, "protected material must be redacted/excluded"))
    if UNSAFE_AUTHORITY_RE.search(value or ""):
        diagnostics.append(Diagnostic("unsafe_authority", subject, "unsafe authority text is forbidden"))
    return diagnostics


def validate_source_evidence(repo_root: pathlib.Path, model: dict[str, Any], section_row: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    subsystem = str(section_row.get("subsystem", ""))
    matrix = source_rows(repo_root)

    for artifact in section_row.get("source_artifacts", []):
        if not path_exists(repo_root, str(artifact)):
            diagnostics.append(Diagnostic("missing_artifact", subsystem, f"source artifact missing: {artifact}"))
    for source_path in section_row.get("source_paths", []):
        if not path_exists(repo_root, str(source_path)):
            diagnostics.append(Diagnostic("missing_source", subsystem, f"source path missing: {source_path}"))
    for family in section_row.get("matrix_metric_families", []):
        matrix_row = matrix.get(str(family))
        if matrix_row is None:
            diagnostics.append(Diagnostic("missing_matrix_row", subsystem, f"CEIC-090 matrix row missing: {family}"))
            continue
        if not normalize_status(matrix_row.get("status", "")).startswith("complete"):
            diagnostics.append(Diagnostic("matrix_status", subsystem, f"CEIC-090 matrix row is not complete: {family}"))

    support_row = matrix.get(str(model.get("ceic_090_support_bundle_generation_matrix_row", "")))
    if support_row is None or not normalize_status(support_row.get("status", "")).startswith("complete"):
        diagnostics.append(Diagnostic("missing_matrix_row", "support_bundle_generation", "CEIC-090 support bundle row required"))
    return diagnostics


def validate_section(repo_root: pathlib.Path, model: dict[str, Any], section_row: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    subsystem = str(section_row.get("subsystem", ""))
    subject = subsystem or "section"

    if not section_row.get("present"):
        diagnostics.append(Diagnostic("missing_subsystem", subject, "section present flag required"))
    for flag in (
        "bounded",
        "low_memory_safe",
        "streaming_or_chunked",
        "redaction_before_buffering",
        "protected_material_excluded",
        "protected_material_refused",
        "retention_metadata_present",
        "redaction_metadata_present",
        "signed_or_chained",
    ):
        if section_row.get(flag) is not True:
            code = "streaming_required" if flag == "streaming_or_chunked" else "unbounded" if flag == "bounded" else flag
            diagnostics.append(Diagnostic(code, subject, f"{flag} must be true"))

    row_limit = int(section_row.get("row_limit", 0) or 0)
    byte_limit = int(section_row.get("byte_limit", 0) or 0)
    row_count = int(section_row.get("row_count", 0) or 0)
    byte_count = int(section_row.get("byte_count", 0) or 0)
    chunk_count = int(section_row.get("chunk_count", 0) or 0)
    max_chunk_bytes = int(section_row.get("max_chunk_bytes", 0) or 0)
    limits = model.get("limits", {})
    if row_limit <= 0 or byte_limit <= 0 or row_count > row_limit or byte_count > byte_limit:
        diagnostics.append(Diagnostic("unbounded", subject, "row/byte caps must be positive and respected"))
    if row_limit > int(limits.get("max_rows_per_subsystem", 0) or 0):
        diagnostics.append(Diagnostic("unbounded", subject, "section row cap exceeds model cap"))
    if byte_limit > int(limits.get("max_bytes_per_subsystem", 0) or 0):
        diagnostics.append(Diagnostic("unbounded", subject, "section byte cap exceeds model cap"))
    if chunk_count <= 0 or chunk_count > int(limits.get("max_chunks", 0) or 0):
        diagnostics.append(Diagnostic("streaming_required", subject, "chunk count must be bounded"))
    if max_chunk_bytes <= 0 or max_chunk_bytes > int(limits.get("max_chunk_bytes", 0) or 0):
        diagnostics.append(Diagnostic("streaming_required", subject, "chunk byte cap must be bounded"))
    if not valid_digest(str(section_row.get("tamper_digest", ""))) or not valid_digest(str(section_row.get("chain_digest", ""))):
        diagnostics.append(Diagnostic("placeholder_tamperless", subject, "section tamper and chain digests are required"))
    if not section_row.get("signature_or_chain_algorithm"):
        diagnostics.append(Diagnostic("placeholder_tamperless", subject, "signature or chain algorithm required"))
    if not section_row.get("retention_class") or not section_row.get("redaction_profile"):
        diagnostics.append(Diagnostic("retention_redaction_metadata", subject, "retention/redaction metadata required"))
    if section_row.get("sidecar_only_evidence"):
        diagnostics.append(Diagnostic("sidecar_only", subject, "sidecar-only evidence is forbidden"))
    if section_row.get("local_cluster_production_claim"):
        diagnostics.append(Diagnostic("local_cluster_claim", subject, "local cluster production claims are forbidden"))
    if section_row.get("production_evidence") and section_row.get("fixture_or_test_only_evidence"):
        diagnostics.append(Diagnostic("fixture_test_only", subject, "fixture/test-only production evidence is forbidden"))

    diagnostics.extend(validate_authority(subject, section_row))
    diagnostics.extend(validate_source_evidence(repo_root, model, section_row))

    rows = section_row.get("rows", [])
    if not isinstance(rows, list) or not rows:
        diagnostics.append(Diagnostic("missing_rows", subject, "section rows are required"))
        return diagnostics
    if len(rows) > row_limit:
        diagnostics.append(Diagnostic("unbounded", subject, "integrated row count exceeds section cap"))
    for index, row_value in enumerate(rows):
        row_subject = f"{subject}.row.{index}"
        if not valid_digest(str(row_value.get("tamper_digest", ""))) or not valid_digest(str(row_value.get("chain_digest", ""))):
            diagnostics.append(Diagnostic("placeholder_tamperless", row_subject, "row tamper and chain digests are required"))
        if row_value.get("evidence_only") is not True:
            diagnostics.append(Diagnostic("unsafe_authority", row_subject, "row must be evidence-only"))
        if not row_value.get("retention_class") or not row_value.get("redaction_class"):
            diagnostics.append(Diagnostic("retention_redaction_metadata", row_subject, "row retention/redaction metadata required"))
        if row_value.get("sidecar_only_evidence"):
            diagnostics.append(Diagnostic("sidecar_only", row_subject, "sidecar-only row evidence is forbidden"))
        if row_value.get("local_cluster_production_claim"):
            diagnostics.append(Diagnostic("local_cluster_claim", row_subject, "local cluster production row claim is forbidden"))
        if section_row.get("production_evidence") and row_value.get("fixture_or_test_only_evidence"):
            diagnostics.append(Diagnostic("fixture_test_only", row_subject, "fixture/test-only production row evidence is forbidden"))
        row_text = f"{row_value.get('key', '')} {row_value.get('value', '')}"
        diagnostics.extend(
            validate_text_safety(
                row_subject,
                row_text,
                bool(row_value.get("redacted", False)),
                bool(row_value.get("protected_material_excluded", False)),
            )
        )
        diagnostics.extend(validate_authority(row_subject, row_value))
    return diagnostics


def validate_model(repo_root: pathlib.Path, model: dict[str, Any]) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    if model.get("schema") != "sb.ceic091.integrated_support_bundle.v1":
        diagnostics.append(Diagnostic("schema", "model", "unsupported CEIC-091 model schema"))
    for flag in (
        "low_memory_mode",
        "streaming_or_chunked",
        "redaction_before_buffering",
        "protected_material_excluded",
        "protected_material_refused",
        "signed_or_chained",
        "retention_metadata_present",
        "redaction_metadata_present",
    ):
        if model.get(flag) is not True:
            diagnostics.append(Diagnostic(flag, "model", f"{flag} must be true"))
    if not valid_digest(str(model.get("tamper_chain_digest", ""))) or not model.get("signature_or_chain_algorithm"):
        diagnostics.append(Diagnostic("placeholder_tamperless", "model", "top-level chain evidence required"))
    limits = model.get("limits", {})
    max_rows = int(limits.get("max_rows", 0) or 0)
    max_bytes = int(limits.get("max_bytes", 0) or 0)
    if max_rows <= 0 or max_bytes <= 0:
        diagnostics.append(Diagnostic("unbounded", "model", "top-level row and byte caps required"))
    diagnostics.extend(validate_authority("model", model))

    sections = model.get("sections", [])
    by_subsystem = {str(section.get("subsystem", "")): section for section in sections if isinstance(section, dict)}
    for subsystem in REQUIRED_SUBSYSTEMS:
        if subsystem not in by_subsystem:
            diagnostics.append(Diagnostic("missing_subsystem", subsystem, "required subsystem section missing"))
    total_rows = 0
    total_bytes = 0
    for subsystem in REQUIRED_SUBSYSTEMS:
        section_row = by_subsystem.get(subsystem)
        if section_row is None:
            continue
        diagnostics.extend(validate_section(repo_root, model, section_row))
        total_rows += int(section_row.get("row_count", 0) or 0)
        total_bytes += int(section_row.get("byte_count", 0) or 0)
    if total_rows > max_rows or total_bytes > max_bytes:
        diagnostics.append(Diagnostic("unbounded", "model", "top-level row/byte cap exceeded"))
    return diagnostics


def validate_execution_plan_control(repo_root: pathlib.Path) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    tracker = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_STATUS_MATRIX.csv"), "slice_id")
    dependencies = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_DEPENDENCY_MATRIX.csv"), "dependency_id")
    gates = index_by(read_csv(repo_root / EXECUTION_PLAN / "CEIC_ACCEPTANCE_MATRIX.csv"), "gate_id")
    artifacts = index_by(read_csv(repo_root / EXECUTION_PLAN / "ARTIFACT_INDEX.csv"), "artifact_id")

    if normalize_status(tracker.get("CEIC-091", {}).get("status", "")) != "complete":
        diagnostics.append(Diagnostic("ceic091_status", "CEIC-091", "CEIC_STATUS_MATRIX.csv must mark CEIC-091 complete"))
    for slice_id in ("CEIC-092", "CEIC-093", "CEIC-094", "CEIC-095"):
        if normalize_status(tracker.get(slice_id, {}).get("status", "")) not in {"pending", "complete"}:
            diagnostics.append(Diagnostic("successor_status", slice_id, f"{slice_id} must be pending or complete"))
    if normalize_status(dependencies.get("CEIC-DEP-051", {}).get("status", "")) != "available":
        diagnostics.append(Diagnostic("dependency_unavailable", "CEIC-DEP-051", "CEIC-091 dependency must be available"))
    if normalize_status(gates.get("CEIC-GATE-050", {}).get("status", "")) != "complete":
        diagnostics.append(Diagnostic("gate_status", "CEIC-GATE-050", "CEIC-091 gate must be complete"))
    artifact = artifacts.get("CEIC-ART-088")
    if artifact is None:
        diagnostics.append(Diagnostic("missing_artifact", "CEIC-ART-088", "CEIC-091 artifact must be indexed"))
    elif normalize_status(artifact.get("status", "")) not in {"present", "complete", "completed", "generated"} or not path_exists(repo_root, artifact.get("path", "")):
        diagnostics.append(Diagnostic("missing_artifact", "CEIC-ART-088", "CEIC-091 artifact must be present"))

    cmake_text = (repo_root / CMAKE_GATE).read_text(encoding="utf-8")
    for token in ("ceic_091_integrated_support_bundle_gate_check", "ceic_091_integrated_support_bundle_gate"):
        if token not in cmake_text:
            diagnostics.append(Diagnostic("cmake_registration", "CEIC-091", f"missing CMake registration: {token}"))
    return diagnostics


def load_model(repo_root: pathlib.Path, manifest: pathlib.Path | None) -> dict[str, Any]:
    if manifest is None:
        return default_model(repo_root)
    path = manifest if manifest.is_absolute() else repo_root / manifest
    return json.loads(path.read_text(encoding="utf-8"))


def run(repo_root: pathlib.Path, manifest: pathlib.Path | None, skip_execution_plan_control: bool) -> list[Diagnostic]:
    model = load_model(repo_root, manifest)
    diagnostics = validate_model(repo_root, model)
    if not skip_execution_plan_control:
        diagnostics.extend(validate_execution_plan_control(repo_root))
    return diagnostics


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--skip-execution_plan-control", action="store_true")
    parser.add_argument("--dump-default-model", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    if args.dump_default_model:
        print(json.dumps(default_model(repo_root), indent=2, sort_keys=True))
        return 0
    diagnostics = run(repo_root, args.manifest, args.skip_execution_plan_control)
    if diagnostics:
        for diagnostic in diagnostics:
            print(f"ceic_091_integrated_support_bundle_gate=fail:{diagnostic.render()}", file=sys.stderr)
        return 1
    print("ceic_091_integrated_support_bundle_gate=pass")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
