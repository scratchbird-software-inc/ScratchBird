#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Static authority gate for SBsql surface-to-SBLR/function coverage."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import Counter
from pathlib import Path

DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def require_columns(path: Path, rows: list[dict[str, str]], columns: list[str]) -> None:
    if not rows:
        fail(f"{path} is empty")
    missing = [column for column in columns if column not in rows[0]]
    if missing:
        fail(f"{path} missing required columns: {', '.join(missing)}")


def unique_index(rows: list[dict[str, str]], key: str, label: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row.get(key, "")
        if not value:
            fail(f"{label} row missing {key}")
        if value in out:
            fail(f"{label} duplicate {key}: {value}")
        out[value] = row
    return out


def parse_generated_manifest(path: Path) -> dict[str, int]:
    if not path.is_file():
        fail(f"generated registry manifest missing: {path}")
    out: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if value.isdigit():
            out[key.strip()] = int(value)
    return out


def parse_generated_header_count(path: Path) -> int:
    if not path.is_file():
        fail(f"generated registry header missing: {path}")
    match = re.search(
        r"kGeneratedSurfaceRegistryRowCount\s*=\s*([0-9]+)",
        path.read_text(encoding="utf-8"),
    )
    if not match:
        fail(f"generated registry header missing row-count constant: {path}")
    return int(match.group(1))


def parse_registry_counts(path: Path) -> dict[str, object]:
    if not path.is_file():
        fail(f"coverage registry missing: {path}")
    lines = path.read_text(encoding="utf-8").splitlines()
    counts: dict[str, object] = {}
    section: str | None = None
    in_baseline = False
    for line in lines:
        if line == "baseline_counts:":
            in_baseline = True
            continue
        if in_baseline and line and not line.startswith(" "):
            break
        if not in_baseline:
            continue

        total = re.match(r"  total_surfaces:\s*([0-9]+)\s*$", line)
        if total:
            counts["total_surfaces"] = int(total.group(1))
            continue
        nested = re.match(r"  ([A-Za-z0-9_]+):\s*$", line)
        if nested:
            section = nested.group(1)
            counts[section] = {}
            continue
        item = re.match(r"    ([A-Za-z0-9_.]+):\s*([0-9]+)\s*$", line)
        if item and section:
            assert isinstance(counts[section], dict)
            counts[section][item.group(1)] = int(item.group(2))
    return counts


def compare_counter(
    observed: Counter[str],
    expected: dict[str, int],
    label: str,
) -> None:
    observed_dict = dict(observed)
    expected = {key: value for key, value in expected.items() if value != 0}
    if observed_dict != expected:
        fail(f"{label} count drift: expected {expected} observed {observed_dict}")


def source_status(row: dict[str, str]) -> str:
    return row.get("source_status") or row.get("status", "")


def status_policy(status: str) -> tuple[str, str]:
    if status == "native_now":
        return "yes", ""
    if status == "native_future":
        return "no_until_status_changes", "SBSQL.SURFACE.NOT_ADMITTED"
    if status == "cluster_private":
        return "no_public_cluster_provider", "SBSQL.CLUSTER.AUTHORITY_REQUIRED"
    return "no_unknown_status", "SBSQL.SURFACE.NOT_ADMITTED"


def project_status_matrix(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    projected: list[dict[str, str]] = []
    for row in rows:
        status = source_status(row)
        allowed, diagnostic = status_policy(status)
        projected.append(
            {
                "surface_id": row["surface_id"],
                "canonical_name": row["canonical_name"],
                "status": status,
                "allowed_lowering": allowed,
                "diagnostic_if_not_allowed": diagnostic,
            }
        )
    return projected


def project_operation_matrix(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    projected: list[dict[str, str]] = []
    for row in rows:
        status = source_status(row)
        _, diagnostic = status_policy(status)
        projected.append(
            {
                "surface_id": row["surface_id"],
                "canonical_name": row["canonical_name"],
                "sblr_operation_family": row["sblr_operation_family"],
                "ingress_envelope": "SBLRExecutionEnvelope.v3",
                "required_context": "parser_or_driver_lowered_sblr_uuid_context",
                "binding_steps": "surface_registry_lookup;uuid_resolution;sblr_lowering",
                "result_shape": "SBLRExecutionResultEnvelope.v3",
                "diagnostics": diagnostic or "canonical_message_vector_set",
            }
        )
    return projected


def derive_counts(rows: list[dict[str, str]]) -> dict[str, object]:
    status_counts = Counter(source_status(row) for row in rows)
    counts: dict[str, object] = {
        "total_surfaces": len(rows),
        "status": dict(status_counts),
        "cluster_scope": dict(Counter(row["cluster_scope"] for row in rows)),
        "surface_kind": dict(Counter(row["surface_kind"] for row in rows)),
        "family": dict(Counter(row["family"] for row in rows)),
        "sblr_operation_family": dict(Counter(row["sblr_operation_family"] for row in rows)),
    }
    expression_rows = [row for row in rows if row["family"] == "expression_runtime"]
    expression_status = Counter(source_status(row) for row in expression_rows)
    expression_counts: dict[str, int] = {"total_rows": len(expression_rows)}
    expression_counts.update(dict(expression_status))
    counts["expression_runtime"] = expression_counts
    return counts


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root)
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    spec = root / "public_contract_snapshot"
    registry = root / "public_contract_snapshot"
    manifest = root / "public_contract_snapshot"
    surface_csv = root / "public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv"
    status_csv = root / "public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv"
    operation_csv = root / "public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv"
    backlog_csv = artifact_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv"
    batch_csv = artifact_root / "BATCH_ROW_MEMBERSHIP.csv"
    oracle_csv = artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"
    generated_manifest = root / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest"
    generated_header = root / "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.hpp"

    spec_text = spec.read_text(encoding="utf-8") if spec.is_file() else ""
    if "SBSQL_MISSING_FUNCTIONALITY_PUBLIC_REGISTRY_SNAPSHOT" not in spec_text:
        for token in (
            "SBSQL-SURFACE-SBLR-FUNCTION-COVERAGE",
            "SBSQL-SURFACE-SBLR-COVERAGE-GATE-001",
            "SBSQL-SURFACE-SBLR-NO-FAMILY-ONLY-RELEASE",
        ):
            if token not in spec_text:
                fail(f"coverage spec missing search key/token {token}")

    manifest_text = manifest.read_text(encoding="utf-8") if manifest.is_file() else ""
    if "SBSQL_MISSING_FUNCTIONALITY_PUBLIC_REGISTRY_SNAPSHOT" not in manifest_text:
        for token in (
            "chapters/parser-v3/sblr-lowering/appendix-sbsql-surface-to-sblr-function-implementation-coverage.md",
            "registries/sbsql-surface-to-sblr-function-coverage.yaml",
        ):
            if token not in manifest_text:
                fail(f"MANIFEST missing coverage authority token {token}")

    surfaces = read_csv(surface_csv)
    counts = parse_registry_counts(registry)
    if "total_surfaces" not in counts:
        counts = derive_counts(surfaces)
    total = counts.get("total_surfaces")
    if not isinstance(total, int):
        fail("coverage registry missing baseline_counts.total_surfaces")

    statuses = read_csv(status_csv)
    operations = read_csv(operation_csv)
    if "allowed_lowering" not in statuses[0]:
        statuses = project_status_matrix(surfaces)
    if "ingress_envelope" not in operations[0]:
        operations = project_operation_matrix(surfaces)
    backlog = read_csv(backlog_csv)
    batches = read_csv(batch_csv)
    oracles = read_csv(oracle_csv)

    require_columns(
        surface_csv,
        surfaces,
        [
            "surface_id",
            "fixed_uuid_v7",
            "canonical_name",
            "surface_kind",
            "family",
            "status",
            "cluster_scope",
            "canonical_spec",
            "sblr_operation_family",
            "parser_packet",
            "engine_packet",
        ],
    )
    require_columns(
        status_csv,
        statuses,
        ["surface_id", "canonical_name", "status", "allowed_lowering", "diagnostic_if_not_allowed"],
    )
    require_columns(
        operation_csv,
        operations,
        [
            "surface_id",
            "canonical_name",
            "sblr_operation_family",
            "ingress_envelope",
            "required_context",
            "binding_steps",
            "result_shape",
            "diagnostics",
        ],
    )

    for label, rows in (
        ("SBSQL_SURFACE_REGISTRY", surfaces),
        ("SBSQL_SURFACE_STATUS_MATRIX", statuses),
        ("SBSQL_TO_SBLR_OPERATION_MATRIX", operations),
        ("SURFACE_IMPLEMENTATION_BACKLOG", backlog),
        ("BATCH_ROW_MEMBERSHIP", batches),
        ("SEMANTIC_ORACLE_AUTHORITY_MAP", oracles),
    ):
        if len(rows) != total:
            fail(f"{label} count drift: expected {total} observed {len(rows)}")

    generated_counts = parse_generated_manifest(generated_manifest)
    if generated_counts.get("surface_count") != total:
        fail(f"generated manifest surface_count drift: {generated_counts}")
    if parse_generated_header_count(generated_header) != total:
        fail("generated header row-count constant drift")

    surface_by_id = unique_index(surfaces, "surface_id", "SBSQL_SURFACE_REGISTRY")
    status_by_id = unique_index(statuses, "surface_id", "SBSQL_SURFACE_STATUS_MATRIX")
    operation_by_id = unique_index(operations, "surface_id", "SBSQL_TO_SBLR_OPERATION_MATRIX")
    backlog_by_id = unique_index(backlog, "surface_id", "SURFACE_IMPLEMENTATION_BACKLOG")
    batch_by_id = unique_index(batches, "surface_id", "BATCH_ROW_MEMBERSHIP")
    oracle_by_id = unique_index(oracles, "surface_id", "SEMANTIC_ORACLE_AUTHORITY_MAP")

    surface_ids = set(surface_by_id)
    for label, index in (
        ("status", status_by_id),
        ("operation", operation_by_id),
        ("backlog", backlog_by_id),
        ("batch", batch_by_id),
        ("oracle", oracle_by_id),
    ):
        if set(index) != surface_ids:
            missing = sorted(surface_ids - set(index))[:10]
            extra = sorted(set(index) - surface_ids)[:10]
            fail(f"{label} surface id drift: missing={missing} extra={extra}")

    compare_counter(Counter(source_status(row) for row in surfaces), counts["status"], "status")
    compare_counter(
        Counter(row["cluster_scope"] for row in surfaces),
        counts["cluster_scope"],
        "cluster_scope",
    )
    compare_counter(
        Counter(row["surface_kind"] for row in surfaces),
        counts["surface_kind"],
        "surface_kind",
    )
    compare_counter(Counter(row["family"] for row in surfaces), counts["family"], "family")
    compare_counter(
        Counter(row["sblr_operation_family"] for row in surfaces),
        counts["sblr_operation_family"],
        "sblr_operation_family",
    )

    expression = counts.get("expression_runtime")
    if not isinstance(expression, dict):
        fail("coverage registry missing expression_runtime counts")
    expression_rows = [row for row in surfaces if row["family"] == "expression_runtime"]
    if len(expression_rows) != expression.get("total_rows"):
        fail("expression_runtime total row drift")
    expected_expression_status = {
        "native_now": expression.get("native_now", 0),
        "native_future": expression.get("native_future", 0),
    }
    if "cluster_private" in expression:
        expected_expression_status["cluster_private"] = expression["cluster_private"]
    compare_counter(Counter(source_status(row) for row in expression_rows), expected_expression_status, "expression_runtime status")

    for surface_id, surface in surface_by_id.items():
        status = status_by_id[surface_id]
        operation = operation_by_id[surface_id]
        if surface["canonical_name"] != status["canonical_name"]:
            fail(f"{surface_id} canonical_name mismatch in status matrix")
        if surface["canonical_name"] != operation["canonical_name"]:
            fail(f"{surface_id} canonical_name mismatch in operation matrix")
        canonical_status = source_status(surface)
        if canonical_status != status["status"]:
            fail(f"{surface_id} status mismatch")
        if surface["sblr_operation_family"] != operation["sblr_operation_family"]:
            fail(f"{surface_id} SBLR operation family mismatch")
        if operation["ingress_envelope"] != "SBLRExecutionEnvelope.v3":
            fail(f"{surface_id} unexpected ingress envelope {operation['ingress_envelope']}")
        for column in ("required_context", "binding_steps", "result_shape", "diagnostics"):
            if not operation[column]:
                fail(f"{surface_id} operation row missing {column}")

        if canonical_status == "native_now" and status["allowed_lowering"] != "yes":
            fail(f"{surface_id} native_now row is not lowering-admitted")
        if canonical_status == "native_future":
            if status["allowed_lowering"] != "no_until_status_changes":
                fail(f"{surface_id} native_future row has invalid lowering policy")
            if status["diagnostic_if_not_allowed"] != "SBSQL.SURFACE.NOT_ADMITTED":
                fail(f"{surface_id} native_future row has invalid diagnostic")
        if canonical_status == "cluster_private":
            if status["diagnostic_if_not_allowed"] not in {
                "SBSQL.SURFACE.NOT_ADMITTED",
                "SBSQL.CLUSTER.AUTHORITY_REQUIRED",
                "SBSQL.SURFACE.PARSER_PRIVATE_REFUSED",
            }:
                fail(f"{surface_id} cluster_private row has invalid public diagnostic")

    print(
        "sbsql_surface_to_sblr_function_coverage_gate=passed "
        f"surfaces={total} native_now={counts['status'].get('native_now', 0)} "
        f"native_future={counts['status'].get('native_future', 0)} "
        f"cluster_private={counts['status'].get('cluster_private', 0)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
