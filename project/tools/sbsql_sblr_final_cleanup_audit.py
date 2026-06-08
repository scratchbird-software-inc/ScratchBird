#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Source-based SBsql/SBLR final cleanup audit.

The audit derives its rows from canonical contract registries and current
project source/evidence. Execution_Plans are intentionally not runtime inputs.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError as exc:  # pragma: no cover - environment gate
    print("PyYAML is required for canonical registry parsing", file=sys.stderr)
    raise SystemExit(2) from exc


AUTHORITY_FILES = (
    "public_contract_snapshot",
    "public_contract_snapshot",
)
SBLR_OPERATION_MATRIX = "public_contract_snapshot"
SBLR_OPCODE_REGISTRY = "public_contract_snapshot"
SHOW_COMMAND_MATRIX = "public_contract_snapshot"
MANAGEMENT_CLUSTER_MATRIX = (
    "public_contract_snapshot"
)
SURFACE_REGISTRY = (
    "public_input_snapshot"
    "SBSQL_SURFACE_REGISTRY.csv"
)
PER_ROW_EVIDENCE = (
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts/"
    "PER_ROW_EVIDENCE_MANIFEST.csv"
)
API_OPERATION_MATRIX = "project/src/engine/internal_api/SBLR_API_OPERATION_MATRIX.yaml"
SERVER_ADMISSION_SOURCE = "project/src/server/sblr_admission.cpp"
SBLR_OPCODE_SOURCE = "project/src/engine/sblr/sblr_opcode_registry.cpp"
SBSQL_LOWERING_SOURCE = "project/src/parsers/sbsql_worker/lowering/lowering.cpp"
NO_CLUSTER_PROVIDER_SOURCE = "project/src/cluster_provider/no_cluster_provider.cpp"
STUB_CLUSTER_PROVIDER_SOURCE = "project/src/cluster_provider_stub/stub_cluster_provider.cpp"
CLUSTER_PROVIDER_CONFORMANCE = (
    "project/tests/sbsql_parser_worker/sbsql_cluster_provider_conformance.cpp"
)
CLUSTER_PROVIDER_ROUTE_CONFORMANCE = (
    "project/tests/sbsql_parser_worker/"
    "sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance.cpp"
)
BASELINE_FIXTURE = "public_audit_summary"

SOURCE_EVIDENCE_ROOTS = (
    "project/src/parsers/sbsql_worker",
    "project/src/server",
    "project/src/engine/internal_api",
    "project/src/engine/sblr",
    "project/src/cluster_provider",
    "project/src/cluster_provider_stub",
    "project/tests/sbsql_parser_worker",
)
IMPLEMENTATION_EVIDENCE_ROOTS = (
    "project/src/parsers/sbsql_worker",
    "project/src/server",
    "project/src/engine/internal_api",
    "project/src/engine/sblr",
    "project/src/cluster_provider",
    "project/src/cluster_provider_stub",
)
TEST_EVIDENCE_ROOTS = ("project/tests/sbsql_parser_worker",)
SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hpp",
    ".inc",
    ".py",
    ".csv",
    ".yaml",
    ".yml",
}
EXCLUDED_SOURCE_PARTS = {
    "__pycache__",
    "full_parser_udr_engine",
}


@dataclass(frozen=True)
class AuditRow:
    audit_id: str
    layer: str
    required_item: str
    required_kind: str
    authority_source: str
    current_state: str
    evidence_checked: str
    gap_class: str
    cluster_scope: str
    required_next_action: str
    notes: str


@dataclass(frozen=True)
class ClusterEvidenceState:
    noncluster_provider_exact_unsupported: bool
    cluster_stub_provider_metadata: bool
    cluster_provider_ctest_contract: bool
    exact_cluster_provider_route_evidence: bool
    cluster_surface_provider_route_rows: int
    cluster_surface_refusal_rows: int
    cluster_surface_provider_route_evidence_complete: bool

    @property
    def complete(self) -> bool:
        return (
            self.noncluster_provider_exact_unsupported
            and self.cluster_stub_provider_metadata
            and self.cluster_provider_ctest_contract
            and self.exact_cluster_provider_route_evidence
            and self.cluster_surface_provider_route_rows > 0
            and self.cluster_surface_refusal_rows == 0
            and self.cluster_surface_provider_route_evidence_complete
        )


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(2)


def read_text(repo: Path, rel: str) -> str:
    path = repo / rel
    if not path.is_file():
        fail(f"required file missing: {rel}")
    return path.read_text(encoding="utf-8", errors="replace")


def read_yaml(repo: Path, rel: str) -> Any:
    text = read_text(repo, rel)
    try:
        return yaml.safe_load(text)
    except yaml.YAMLError as exc:
        fail(f"failed to parse YAML {rel}: {exc}")


def read_csv(repo: Path, rel: str) -> list[dict[str, str]]:
    path = repo / rel
    if not path.is_file():
        fail(f"required CSV missing: {rel}")
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        return list(csv.DictReader(handle))


def require_authority_files(repo: Path) -> None:
    for rel in AUTHORITY_FILES:
        if not (repo / rel).is_file():
            fail(f"canonical authority file missing: {rel}")

    manifest = read_yaml(repo, AUTHORITY_FILES[0])
    authority_files = set(manifest.get("authority_files", []))
    required_manifest_entries = {
        "MANIFEST.yaml",
        "AUTHORITY.md",
        "registries/sblr-operation-matrix.yaml",
        "registries/sblr-opcodes.yaml",
        "registries/sbsql-native-surface-registry.yaml",
        "registries/sbsql-show-command-surface-matrix.yaml",
        "registries/sbsql-management-metrics-cluster-surface-matrix.yaml",
    }
    missing_entries = sorted(required_manifest_entries - authority_files)
    if missing_entries:
        fail(
            "canonical manifest is missing required audit authority entries: "
            + ", ".join(missing_entries)
        )


def iter_source_files(repo: Path, roots: tuple[str, ...] = SOURCE_EVIDENCE_ROOTS) -> list[Path]:
    files: list[Path] = []
    for rel_root in roots:
        root = repo / rel_root
        if not root.exists():
            continue
        for child in root.rglob("*"):
            if not child.is_file():
                continue
            if child.suffix not in SOURCE_SUFFIXES:
                continue
            if any(part in EXCLUDED_SOURCE_PARTS for part in child.relative_to(repo).parts):
                continue
            files.append(child)
    return sorted(files)


def source_evidence_text(repo: Path, roots: tuple[str, ...] = SOURCE_EVIDENCE_ROOTS) -> str:
    chunks: list[str] = []
    for path in iter_source_files(repo, roots):
        chunks.append(path.read_text(encoding="utf-8", errors="replace"))
    return "\n".join(chunks)


def normalize_syntax(syntax: str) -> str:
    value = syntax.upper()
    value = re.sub(r"\[[^\]]*\]", "", value)
    value = re.sub(r"<[^>]*>", "", value)
    value = re.sub(r"\s+", " ", value)
    return value.strip()


def exact_surface_rows(repo: Path) -> list[tuple[str, str, dict[str, Any]]]:
    rows: list[tuple[str, str, dict[str, Any]]] = []
    for rel in (SHOW_COMMAND_MATRIX, MANAGEMENT_CLUSTER_MATRIX):
        data = read_yaml(repo, rel)
        for group_name, value in data.items():
            if not (group_name.startswith("exact_") and group_name.endswith("_rows")):
                continue
            if not isinstance(value, list):
                continue
            for row in value:
                if isinstance(row, dict) and row.get("surface_key"):
                    rows.append((rel, group_name, row))
    return rows


def cluster_scope_from_surface(row: dict[str, Any]) -> str:
    edition_scope = str(row.get("edition_scope", ""))
    key = str(row.get("surface_key", ""))
    if edition_scope == "private_cluster" or key.startswith("show.cluster.") or key.startswith("alter.cluster."):
        return "cluster_private"
    return "noncluster"


def cluster_scope_from_family(family: str, metadata: Any) -> str:
    if isinstance(metadata, dict) and metadata.get("edition_scope") == "private_cluster":
        return "cluster_private"
    if ".cluster." in family or family.startswith("sblr.replication.consumer."):
        return "cluster_private"
    return "noncluster"


def cluster_scope_from_opcode(entry: dict[str, Any]) -> str:
    family = str(entry.get("family", ""))
    security_class = str(entry.get("security_class", ""))
    transaction_effect = str(entry.get("transaction_effect", ""))
    if (
        family in {"cluster-management", "replication-consumer"}
        or security_class == "cluster_authorized"
        or transaction_effect == "cluster_write"
    ):
        return "cluster_private"
    return "noncluster"


def has_exact_command_evidence(source_text_upper: str, syntax: str) -> bool:
    normalized = normalize_syntax(syntax)
    if not normalized:
        return False
    return normalized in source_text_upper


def make_row(
    rows: list[AuditRow],
    *,
    layer: str,
    required_item: str,
    required_kind: str,
    authority_source: str,
    current_state: str,
    evidence_checked: str,
    gap_class: str,
    cluster_scope: str,
    required_next_action: str,
    notes: str = "",
) -> None:
    rows.append(
        AuditRow(
            audit_id=f"SSFC-AUDIT-{len(rows) + 1:04d}",
            layer=layer,
            required_item=required_item,
            required_kind=required_kind,
            authority_source=authority_source,
            current_state=current_state,
            evidence_checked=evidence_checked,
            gap_class=gap_class,
            cluster_scope=cluster_scope,
            required_next_action=required_next_action,
            notes=notes,
        )
    )


def build_family_rows(repo: Path, rows: list[AuditRow]) -> None:
    operation_matrix = read_yaml(repo, SBLR_OPERATION_MATRIX)
    families = operation_matrix.get("envelope_families", {})
    if not isinstance(families, dict):
        fail(f"{SBLR_OPERATION_MATRIX} envelope_families is not a mapping")
    admission_source = read_text(repo, SERVER_ADMISSION_SOURCE)

    for family, metadata in sorted(families.items()):
        if family in admission_source:
            continue
        make_row(
            rows,
            layer="SBLR family admission",
            required_item=family,
            required_kind="envelope_family",
            authority_source=SBLR_OPERATION_MATRIX,
            current_state="specified first-class family is not admitted by exact family name",
            evidence_checked=f"{SERVER_ADMISSION_SOURCE}: exact family string absent",
            gap_class="sblr_family_not_first_class_admitted",
            cluster_scope=cluster_scope_from_family(family, metadata),
            required_next_action=(
                "Add exact family admission/verification/dispatch or reconcile the "
                "canonical family name."
            ),
            notes=f"metadata={metadata}",
        )


def build_opcode_rows(repo: Path, rows: list[AuditRow]) -> None:
    opcode_registry = read_yaml(repo, SBLR_OPCODE_REGISTRY)
    entries = opcode_registry.get("entries", [])
    if not isinstance(entries, list):
        fail(f"{SBLR_OPCODE_REGISTRY} entries is not a list")
    opcode_source = read_text(repo, SBLR_OPCODE_SOURCE)

    for entry in entries:
        if not isinstance(entry, dict):
            continue
        if str(entry.get("status", "required")) != "required":
            continue
        name = str(entry.get("name", ""))
        if not name or name in opcode_source:
            continue
        make_row(
            rows,
            layer="SBLR opcode registry",
            required_item=name,
            required_kind="opcode",
            authority_source=SBLR_OPCODE_REGISTRY,
            current_state="required canonical opcode name is absent from the engine registry",
            evidence_checked=f"{SBLR_OPCODE_SOURCE}: exact opcode string absent",
            gap_class="required_opcode_missing_or_renamed",
            cluster_scope=cluster_scope_from_opcode(entry),
            required_next_action=(
                "Register the canonical opcode name or update the authoritative "
                "registry/alias model intentionally."
            ),
            notes=(
                f"family={entry.get('family', '')}; code={entry.get('code', '')}; "
                f"search_key={entry.get('search_key', '')}"
            ),
        )


def build_exact_command_rows(
    repo: Path,
    rows: list[AuditRow],
    implementation_text_upper: str,
    test_text_upper: str,
) -> None:
    for rel, group, row in exact_surface_rows(repo):
        surface_key = str(row.get("surface_key", ""))
        syntax = str(row.get("syntax", ""))
        if has_exact_command_evidence(implementation_text_upper, syntax):
            continue
        test_fixture_mentions_syntax = has_exact_command_evidence(test_text_upper, syntax)
        scope = cluster_scope_from_surface(row)
        if scope == "cluster_private":
            gap_class = "cluster_private_positive_execution_missing_or_private_provider_only"
            current_state = "private cluster exact command lacks per-surface public stub/provider route evidence"
            action = (
                "Add noncluster exact refusal plus cluster-enabled stub/provider "
                "route evidence for this exact surface."
            )
        else:
            gap_class = "required_exact_sbsql_command_missing"
            current_state = "exact SBsql command syntax was not found in source/evidence"
            action = "Add parser, lowering, API route, result shape, message vector, and regression evidence."
        make_row(
            rows,
            layer="SBsql exact command",
            required_item=surface_key,
            required_kind="exact_surface",
            authority_source=rel,
            current_state=current_state,
            evidence_checked=(
                "project/src implementation source lacks exact syntax evidence; "
                f"normalized_syntax={normalize_syntax(syntax)!r}; "
                f"test_fixture_mentions_syntax={str(test_fixture_mentions_syntax).lower()}"
            ),
            gap_class=gap_class,
            cluster_scope=scope,
            required_next_action=action,
            notes=f"group={group}; sblr_operation={row.get('sblr_operation', '')}",
        )


def build_api_route_rows(repo: Path, rows: list[AuditRow]) -> None:
    api_matrix = read_yaml(repo, API_OPERATION_MATRIX)
    entries = api_matrix.get("entries", [])
    if not isinstance(entries, list):
        fail(f"{API_OPERATION_MATRIX} entries is not a list")
    lowering_source = read_text(repo, SBSQL_LOWERING_SOURCE)

    for entry in entries:
        if not isinstance(entry, dict):
            continue
        op_id = str(entry.get("api_operation_id", ""))
        if not op_id:
            continue
        scope_status = str(entry.get("scope_status", ""))
        if op_id in lowering_source:
            continue
        if scope_status == "noncluster_required":
            make_row(
                rows,
                layer="SBsql-to-SBLR parser route",
                required_item=op_id,
                required_kind="engine_api_operation",
                authority_source=API_OPERATION_MATRIX,
                current_state="engine API operation is not reachable from SBsql lowering source",
                evidence_checked=f"{SBSQL_LOWERING_SOURCE}: api_operation_id absent",
                gap_class="engine_api_not_reachable_from_sbsql_lowering",
                cluster_scope="noncluster",
                required_next_action=(
                    "Add a SBsql parse/bind/lower route or reconcile that no SBsql "
                    "surface is required."
                ),
                notes=f"sblr_operation={entry.get('sblr_operation', '')}; scope_status={scope_status}",
            )
        elif scope_status.startswith("cluster"):
            make_row(
                rows,
                layer="SBsql-to-SBLR parser route",
                required_item=op_id,
                required_kind="cluster_engine_api_operation",
                authority_source=API_OPERATION_MATRIX,
                current_state="cluster provider API operation is not public-SBsql lowered",
                evidence_checked=f"{SBSQL_LOWERING_SOURCE}: api_operation_id absent",
                gap_class="cluster_private_provider_route_not_public_sbsql_lowered",
                cluster_scope="cluster_private",
                required_next_action=(
                    "Add public fail-closed lowering and cluster-enabled provider-route "
                    "evidence, or reconcile the provider ABI contract."
                ),
                notes=f"sblr_operation={entry.get('sblr_operation', '')}; scope_status={scope_status}",
            )


def build_cluster_refusal_rows(repo: Path, rows: list[AuditRow]) -> None:
    evidence_rows = read_csv(repo, PER_ROW_EVIDENCE)
    for row in evidence_rows:
        if row.get("cluster_scope") != "cluster_private":
            continue
        if row.get("final_state") != "exact_refusal_passed":
            continue
        make_row(
            rows,
            layer="SBsql cluster-private surface",
            required_item=row.get("canonical_name", row.get("surface_id", "")),
            required_kind="cluster_private_surface",
            authority_source=f"{SURFACE_REGISTRY} + {PER_ROW_EVIDENCE}",
            current_state="public build proves exact refusal only; positive cluster provider route still required",
            evidence_checked=(
                f"{PER_ROW_EVIDENCE}: final_state=exact_refusal_passed, "
                "private_cluster_execution not proven"
            ),
            gap_class="cluster_private_exact_refusal_only",
            cluster_scope="cluster_private",
            required_next_action=(
                "Keep public exact refusal and add cluster-enabled stub/provider route, "
                "metadata, support-mode, and message-vector evidence."
            ),
            notes=f"surface_id={row.get('surface_id', '')}; ctest_label={row.get('ctest_label', '')}",
        )


def cluster_evidence_state(repo: Path, rows: list[AuditRow]) -> ClusterEvidenceState:
    no_cluster = read_text(repo, NO_CLUSTER_PROVIDER_SOURCE)
    no_cluster += "\n" + read_text(repo, "project/src/cluster_provider/cluster_provider.hpp")
    stub = read_text(repo, STUB_CLUSTER_PROVIDER_SOURCE)
    conformance = read_text(repo, CLUSTER_PROVIDER_CONFORMANCE)
    conformance += "\n" + read_text(repo, "project/tests/sbsql_parser_worker/CMakeLists.txt")
    route_conformance = read_text(repo, CLUSTER_PROVIDER_ROUTE_CONFORMANCE)
    route_conformance += "\n" + read_text(repo, "project/tests/sbsql_parser_worker/CMakeLists.txt")
    evidence_rows = read_csv(repo, PER_ROW_EVIDENCE)
    cluster_surface_rows = [
        row for row in evidence_rows if row.get("cluster_scope") == "cluster_private"
    ]

    noncluster_tokens = (
        "cluster support is not enabled in this build",
        "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "unsupported_features",
        "cluster_provider_support",
        "no_cluster",
    )
    stub_tokens = (
        "scratchbird.cluster.dummy_provider",
        "provider_name",
        "provider_type",
        "provider_version",
        "support_status",
        "supports_execution",
        "SBLR.CLUSTER.STUB_RESPONSE",
        "cluster.provider.stub.v1",
    )
    conformance_tokens = (
        "VerifyProviderInfoResult",
        "ClusterProviderSupportsExecution",
        "cluster_provider_stub_conformance",
        "cluster_provider_no_cluster_error_vector_conformance",
        "cluster provider info returned the wrong provider name",
        "cluster stub provider diagnostic is missing",
    )
    route_conformance_tokens = (
        "SHOW CLUSTER STATE",
        "ENGINE CLUSTER INSPECT STATE",
        "RequireRegistryAndDispatch",
        "RequireSurfaceProviderEvidence",
        "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "SBLR.CLUSTER.STUB_RESPONSE",
        "cluster.provider.stub.v1",
        "private_cluster_execution",
        "sbsql_sblr_final_cleanup_b015_cluster_provider_route_conformance",
        "sbsql_sblr_final_cleanup_b016_cluster_provider_evidence_conformance",
    )

    provider_boundary_tokens = (
        "cluster_provider_route_passed",
        "provider_boundary_route_evidence",
        "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "SBLR.CLUSTER.STUB_RESPONSE",
        "cluster.provider.stub.v1",
        "private_cluster_execution=false",
    )
    cluster_surface_provider_rows = [
        row
        for row in cluster_surface_rows
        if row.get("final_state") == "cluster_provider_route_passed"
        and all(
            token
            in "\n".join(
                (
                    row.get("final_state", ""),
                    row.get("implementation_refs", ""),
                    row.get("diagnostic_proof", ""),
                    row.get("result_proof", ""),
                    row.get("notes", ""),
                    row.get("ctest_label", ""),
                )
            )
            for token in provider_boundary_tokens
        )
    ]
    cluster_refusal_rows = [
        row for row in cluster_surface_rows if row.get("final_state") == "exact_refusal_passed"
    ]

    return ClusterEvidenceState(
        noncluster_provider_exact_unsupported=all(token in no_cluster for token in noncluster_tokens),
        cluster_stub_provider_metadata=all(token in stub for token in stub_tokens),
        cluster_provider_ctest_contract=all(token in conformance for token in conformance_tokens),
        exact_cluster_provider_route_evidence=all(
            token in route_conformance for token in route_conformance_tokens
        ),
        cluster_surface_provider_route_rows=len(cluster_surface_provider_rows),
        cluster_surface_refusal_rows=len(cluster_refusal_rows),
        cluster_surface_provider_route_evidence_complete=(
            bool(cluster_surface_rows)
            and len(cluster_surface_provider_rows) == len(cluster_surface_rows)
        ),
    )


def build_audit_rows(repo: Path) -> tuple[list[AuditRow], ClusterEvidenceState]:
    require_authority_files(repo)
    implementation_text_upper = source_evidence_text(repo, IMPLEMENTATION_EVIDENCE_ROOTS).upper()
    test_text_upper = source_evidence_text(repo, TEST_EVIDENCE_ROOTS).upper()
    rows: list[AuditRow] = []

    build_family_rows(repo, rows)
    build_opcode_rows(repo, rows)
    build_exact_command_rows(repo, rows, implementation_text_upper, test_text_upper)
    build_api_route_rows(repo, rows)
    build_cluster_refusal_rows(repo, rows)

    return rows, cluster_evidence_state(repo, rows)


def baseline_fixture_counts(repo: Path) -> dict[str, int] | None:
    path = repo / BASELINE_FIXTURE
    if not path.is_file():
        return None
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        rows = list(csv.DictReader(handle))
    counts = Counter(row.get("gap_class", "") for row in rows)
    counts["__total__"] = len(rows)
    return dict(sorted(counts.items()))


def write_matrix_csv(path: Path, rows: list[AuditRow]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = list(asdict(rows[0]).keys()) if rows else [field for field in AuditRow.__dataclass_fields__]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def summarize(rows: list[AuditRow], cluster_evidence: ClusterEvidenceState) -> dict[str, Any]:
    by_gap = Counter(row.gap_class for row in rows)
    by_layer = Counter(row.layer for row in rows)
    by_scope = Counter(row.cluster_scope for row in rows)
    noncluster_open = sum(1 for row in rows if row.cluster_scope != "cluster_private")
    cluster_open = sum(1 for row in rows if row.cluster_scope == "cluster_private")
    return {
        "matrix_rows": len(rows),
        "noncluster_open_rows": noncluster_open,
        "cluster_open_rows": cluster_open,
        "by_gap_class": dict(sorted(by_gap.items())),
        "by_layer": dict(sorted(by_layer.items())),
        "by_cluster_scope": dict(sorted(by_scope.items())),
        "cluster_split_evidence": asdict(cluster_evidence) | {"complete": cluster_evidence.complete},
    }


def print_text_summary(summary: dict[str, Any], mode: str, baseline_counts: dict[str, int] | None) -> None:
    print(f"sbsql_sblr_final_cleanup_audit mode={mode}")
    print("runtime_authority=canonical_specs_and_project_source")
    print("execution_plan_runtime_dependency=none")
    print(f"matrix_rows={summary['matrix_rows']}")
    print(f"noncluster_open_rows={summary['noncluster_open_rows']}")
    print(f"cluster_open_rows={summary['cluster_open_rows']}")
    print("by_gap_class:")
    for key, value in summary["by_gap_class"].items():
        print(f"  {key}={value}")
    print("by_layer:")
    for key, value in summary["by_layer"].items():
        print(f"  {key}={value}")
    print("cluster_split_evidence:")
    for key, value in summary["cluster_split_evidence"].items():
        print(f"  {key}={value}")
    if baseline_counts is not None:
        print("baseline_fixture_comparison_not_authority:")
        for key, value in baseline_counts.items():
            print(f"  {key}={value}")


def final_mode_failures(rows: list[AuditRow], cluster_evidence: ClusterEvidenceState) -> list[str]:
    failures: list[str] = []
    noncluster_open = [row for row in rows if row.cluster_scope != "cluster_private"]
    cluster_open = [row for row in rows if row.cluster_scope == "cluster_private"]
    if noncluster_open:
        counts = Counter(row.gap_class for row in noncluster_open)
        failures.append(
            "non-cluster implementation rows remain open: "
            + ", ".join(f"{key}={value}" for key, value in sorted(counts.items()))
        )
    if cluster_open:
        counts = Counter(row.gap_class for row in cluster_open)
        failures.append(
            "cluster split rows remain open: "
            + ", ".join(f"{key}={value}" for key, value in sorted(counts.items()))
        )
    if not cluster_evidence.complete:
        missing: list[str] = []
        if not cluster_evidence.noncluster_provider_exact_unsupported:
            missing.append("noncluster_provider_exact_unsupported")
        if not cluster_evidence.cluster_stub_provider_metadata:
            missing.append("cluster_stub_provider_metadata")
        if not cluster_evidence.cluster_provider_ctest_contract:
            missing.append("cluster_provider_ctest_contract")
        if not cluster_evidence.exact_cluster_provider_route_evidence:
            missing.append("exact_cluster_provider_route_evidence")
        if cluster_evidence.cluster_surface_provider_route_rows <= 0:
            missing.append("cluster_surface_provider_route_rows")
        if cluster_evidence.cluster_surface_refusal_rows != 0:
            missing.append(
                f"cluster_surface_refusal_rows={cluster_evidence.cluster_surface_refusal_rows}"
            )
        if not cluster_evidence.cluster_surface_provider_route_evidence_complete:
            missing.append("cluster_surface_provider_route_evidence_complete")
        failures.append("cluster split source evidence is incomplete: " + ", ".join(missing))
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument(
        "--mode",
        choices=("baseline", "current", "final"),
        default="baseline",
        help="baseline/current reports open rows without failing; final enforces zero-open criteria.",
    )
    parser.add_argument(
        "--compare-baseline-fixture",
        action="store_true",
        help="Also read public_audit_summary baseline CSV as a labelled comparison fixture, not authority.",
    )
    parser.add_argument("--emit-csv", type=Path, help="Write the generated audit matrix to this path.")
    parser.add_argument("--format", choices=("text", "json"), default="text")
    args = parser.parse_args()

    repo = args.repo_root.resolve()
    rows, cluster_evidence = build_audit_rows(repo)
    summary = summarize(rows, cluster_evidence)

    baseline_counts = baseline_fixture_counts(repo) if args.compare_baseline_fixture else None
    if args.emit_csv:
        write_matrix_csv(args.emit_csv if args.emit_csv.is_absolute() else repo / args.emit_csv, rows)

    if args.format == "json":
        payload = {
            "mode": args.mode,
            "runtime_authority": "canonical_specs_and_project_source",
            "execution_plan_runtime_dependency": "none",
            "summary": summary,
            "baseline_fixture_comparison_not_authority": baseline_counts,
            "rows": [asdict(row) for row in rows],
        }
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        print_text_summary(summary, args.mode, baseline_counts)

    if args.mode == "final":
        failures = final_mode_failures(rows, cluster_evidence)
        if failures:
            print("sbsql_sblr_final_cleanup_audit=failed", file=sys.stderr)
            for failure in failures:
                print(f" - {failure}", file=sys.stderr)
            return 1

    print("sbsql_sblr_final_cleanup_audit=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
