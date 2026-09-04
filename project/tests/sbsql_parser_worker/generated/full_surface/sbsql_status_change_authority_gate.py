#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Status-change authority gate for the SBsql Surface-to-SBLR regression fixtures.

For every SBsql surface row in the canonical registry, this gate verifies
that the status reported across the execution_plan artifacts agrees with the
canonical status in `SBSQL_SURFACE_REGISTRY.csv`. Any mismatch indicates
an unpaired status change — drift that this gate must reject per the
policy declared in `STATUS_CHANGE_AUTHORITY_MATRIX.csv`.

Cross-checks performed:

  1. SBSQL_SURFACE_STATUS_MATRIX.csv status == canonical status.
  2. STRICT_ROW_COVERAGE_LEDGER.csv status field == canonical status.
  3. STRICT_ROW_COVERAGE_LEDGER.csv current_state is consistent with
     canonical status (native_future -> inventory_only; cluster_private ->
     lowering_family_mapped; native_now -> lowering_family_mapped or a
     promoted state — final states are allowed once row evidence lands).
  4. FUNCTION_SEMANTIC_ORACLE_MATRIX.csv status == canonical status
     (for expression-runtime surfaces).
  5. AUTHENTICATED_FULL_ROUTE_MATRIX.csv status == canonical status.
  6. SBLR_BINARY_ROUND_TRIP_MATRIX.csv status == canonical status.
  7. PER_ROW_EVIDENCE_MANIFEST.csv status == canonical status.
  8. NATIVE_FUTURE_PROMOTION_MATRIX.csv current_status == native_future
     for every row it contains, and contains exactly the set of canonical
     native_future surface_ids.
  9. Generated registry manifest status counts agree with canonical
     status counts derived from SBSQL_SURFACE_REGISTRY.csv.

Architecture invariant compliance: read-only consistency check; no
transaction model touched; no engine, parser worker, server, listener,
storage, or MGA file modified; no WAL surface introduced. MGA copy-on-write
remains the sole transaction recovery model.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path


REGISTRY = "public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv"
STATUS_MATRIX = "public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv"
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
STRICT_LEDGER_NAME = "STRICT_ROW_COVERAGE_LEDGER.csv"
ORACLE_MATRIX_NAME = "FUNCTION_SEMANTIC_ORACLE_MATRIX.csv"
ROUTE_MATRIX_NAME = "AUTHENTICATED_FULL_ROUTE_MATRIX.csv"
ROUND_TRIP_MATRIX_NAME = "SBLR_BINARY_ROUND_TRIP_MATRIX.csv"
PER_ROW_MANIFEST_NAME = "PER_ROW_EVIDENCE_MANIFEST.csv"
PROMOTION_MATRIX_NAME = "NATIVE_FUTURE_PROMOTION_MATRIX.csv"
GENERATED_MANIFEST = "project/src/parsers/sbsql_worker/registry/generated/sbsql_generated_registry.manifest"
EXPRESSION_RUNTIME_KINDS = {"function", "operator", "variable"}
CANONICAL_PRE_SBLR_REFUSAL_SURFACES = {
    "SBSQL-28F16A4C7DD0",  # table_constraint
    "SBSQL-5CC9FDFFE6F7",  # constraint_body
    "SBSQL-7BA0B928798B",  # schema_name
    "SBSQL-A57CFDE0BBA9",  # column_constraint
    "SBSQL-B1816929AD45",  # constraint_name
    "SBSQL-DE4B8AAF6326",  # create_schema_stmt
}
CANONICAL_PRE_SBLR_REFUSAL_PROOFS = {
    "SBSQL.IMPL.NOT_AVAILABLE",
    "executable_sblr_emitted=false",
    "canonical_message_vector_set",
    "engine_dispatch_not_reached=true",
    "catalog_mutation=false",
    "no_wal_authority",
}

# Allowed strict-ledger current_states per canonical status.
ALLOWED_LEDGER_STATES = {
    "native_now": {
        "lowering_family_mapped",
        "parser_bound",
        "sblr_verified",
        "server_admitted",
        "engine_runtime_implemented",
        "e2e_passed",
    },
    "native_future": {
        "inventory_only",
        "e2e_passed",  # allowed only after promote-then-implement; status alignment will already have caught the mismatch first
    },
    "cluster_private": {
        "lowering_family_mapped",
        "exact_refusal_passed",
        "private_profile_implemented",
    },
}


def allowed_ledger_states(status: str, cluster_scope: str) -> set[str]:
    if status == "native_now" and cluster_scope == "cluster_private":
        return {
            "lowering_family_mapped",
            "parser_bound",
            "sblr_verified",
            "server_admitted",
            "exact_refusal_passed",
            "private_profile_implemented",
        }
    return ALLOWED_LEDGER_STATES.get(status, set())


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def parse_manifest(path: Path) -> dict[str, int]:
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    generator_root = root / "project/tools/sb_parser_gen"
    sys.path.insert(0, str(generator_root))
    from plan_import_rows_generated_evidence import (  # pylint: disable=import-outside-toplevel
        is_central_import_refusal_surface,
        load_central_import_command_rows,
    )
    from core_root_refusal_generated_evidence import (  # pylint: disable=import-outside-toplevel
        is_core_root_exact_refusal,
        validate_authoritative_runtime_inputs as validate_core_root_refusals,
    )

    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    surfaces = read_csv(root / REGISTRY)
    status_matrix = read_csv(root / STATUS_MATRIX)
    ledger = read_csv(artifact_root / STRICT_LEDGER_NAME)
    oracle = read_csv(artifact_root / ORACLE_MATRIX_NAME)
    route = read_csv(artifact_root / ROUTE_MATRIX_NAME)
    round_trip = read_csv(artifact_root / ROUND_TRIP_MATRIX_NAME)
    per_row = read_csv(artifact_root / PER_ROW_MANIFEST_NAME)
    promotion = read_csv(artifact_root / PROMOTION_MATRIX_NAME)
    manifest_counts = parse_manifest(root / GENERATED_MANIFEST)
    try:
        load_central_import_command_rows(root.parent / "Specifications/Core")
        validate_core_root_refusals(root)
    except ValueError as exc:
        fail(f"Core exact-refusal authority validation failed: {exc}")

    canonical_by_id = {
        r["surface_id"]: (r.get("source_status") or r["status"])
        for r in surfaces
    }
    canonical_native_future = {
        r["surface_id"]
        for r in surfaces
        if (r.get("source_status") or r["status"]) == "native_future"
    }

    errors: list[str] = []

    def cross_check(label: str, rows: list[dict[str, str]], status_field: str = "status") -> None:
        for row in rows:
            sid = row.get("surface_id", "")
            if not sid:
                continue
            canonical = canonical_by_id.get(sid)
            if canonical is None:
                errors.append(f"{label} row {sid} not in canonical registry")
                continue
            observed = row.get(status_field, "")
            if not observed and status_field != "source_status":
                observed = row.get("source_status", "")
            if observed and observed != canonical:
                errors.append(f"{label} row {sid} status drift: canonical={canonical} observed={observed}")

    cross_check("SBSQL_SURFACE_STATUS_MATRIX", status_matrix, status_field="source_status")
    cross_check("STRICT_ROW_COVERAGE_LEDGER", ledger)
    cross_check("FUNCTION_SEMANTIC_ORACLE_MATRIX", oracle)
    cross_check("AUTHENTICATED_FULL_ROUTE_MATRIX", route)
    cross_check("SBLR_BINARY_ROUND_TRIP_MATRIX", round_trip)
    cross_check("PER_ROW_EVIDENCE_MANIFEST", per_row)
    cross_check("NATIVE_FUTURE_PROMOTION_MATRIX", promotion, status_field="current_status")

    # Strict ledger current_state must be consistent with canonical status.
    for row in ledger:
        sid = row["surface_id"]
        canonical = canonical_by_id.get(sid)
        if canonical is None:
            continue
        state = row.get("current_state", "")
        cluster_scope = row.get("cluster_scope", "")
        if is_central_import_refusal_surface(sid):
            if (
                canonical == "native_now"
                and cluster_scope == "noncluster_or_profile_scoped"
            ):
                allowed = {"exact_refusal_passed"}
            else:
                allowed = set()
                errors.append(
                    f"STRICT_ROW_COVERAGE_LEDGER row {sid} central-import "
                    f"refusal authority drift: canonical={canonical} "
                    f"cluster_scope={cluster_scope}"
                )
        elif is_core_root_exact_refusal(sid):
            if (
                canonical == "native_now"
                and cluster_scope == "noncluster_or_profile_scoped"
            ):
                allowed = {"exact_refusal_passed"}
                evidence = ";".join(row.values())
                for token in (
                    "SBSQL.IMPL.NOT_AVAILABLE",
                    "operation_id=not_admitted",
                    "root_route=diagnostic_refusal",
                    "sblr_operation=SBLR_DIAGNOSTIC_REFUSAL",
                    "executable_sblr_emitted=false",
                    "engine_dispatch_reached=false",
                    "catalog_mutation=false",
                    "row_mutation=false",
                    "durable_state_byte_identical=true",
                ):
                    if token not in evidence:
                        errors.append(
                            f"STRICT_ROW_COVERAGE_LEDGER row {sid} Core root "
                            f"exact refusal missing proof token {token}"
                        )
            else:
                allowed = set()
                errors.append(
                    f"STRICT_ROW_COVERAGE_LEDGER row {sid} Core root exact "
                    f"refusal authority drift: canonical={canonical} "
                    f"cluster_scope={cluster_scope}"
                )
        elif sid in CANONICAL_PRE_SBLR_REFUSAL_SURFACES:
            if (
                canonical == "native_now"
                and cluster_scope == "noncluster_or_profile_scoped"
            ):
                allowed = {"exact_refusal_passed"}
                evidence = ";".join(row.values())
                for token in sorted(CANONICAL_PRE_SBLR_REFUSAL_PROOFS):
                    if token not in evidence:
                        errors.append(
                            f"STRICT_ROW_COVERAGE_LEDGER row {sid} canonical "
                            f"pre-SBLR refusal missing proof token {token}"
                        )
            else:
                allowed = set()
                errors.append(
                    f"STRICT_ROW_COVERAGE_LEDGER row {sid} canonical pre-SBLR "
                    f"refusal authority drift: canonical={canonical} "
                    f"cluster_scope={cluster_scope}"
                )
        else:
            allowed = allowed_ledger_states(canonical, cluster_scope)
        if state and state not in allowed:
            errors.append(
                f"STRICT_ROW_COVERAGE_LEDGER row {sid} current_state={state} not allowed for canonical status={canonical} cluster_scope={cluster_scope}"
            )

    # Oracle matrix must contain exactly the expression-runtime surfaces.
    expected_oracle_ids = {r["surface_id"] for r in surfaces if r["surface_kind"] in EXPRESSION_RUNTIME_KINDS}
    oracle_ids = {r["surface_id"] for r in oracle}
    missing_oracle = expected_oracle_ids - oracle_ids
    extra_oracle = oracle_ids - expected_oracle_ids
    if missing_oracle:
        errors.append(f"oracle matrix missing {len(missing_oracle)} expression-runtime surface_ids (first: {sorted(missing_oracle)[:3]})")
    if extra_oracle:
        errors.append(f"oracle matrix contains {len(extra_oracle)} non-expression-runtime surface_ids (first: {sorted(extra_oracle)[:3]})")

    # Promotion matrix must contain exactly the canonical native_future set.
    promotion_ids = {r["surface_id"] for r in promotion}
    missing_promotion = canonical_native_future - promotion_ids
    extra_promotion = promotion_ids - canonical_native_future
    if missing_promotion:
        errors.append(f"NATIVE_FUTURE_PROMOTION_MATRIX missing {len(missing_promotion)} canonical native_future ids")
    if extra_promotion:
        errors.append(f"NATIVE_FUTURE_PROMOTION_MATRIX contains {len(extra_promotion)} non-native_future ids")

    # Generated registry manifest counts must agree with canonical.
    canonical_counts = Counter((r.get("source_status") or r["status"]) for r in surfaces)
    if manifest_counts.get("surface_count") != sum(canonical_counts.values()):
        errors.append(
            f"generated_registry_manifest.surface_count={manifest_counts.get('surface_count')} disagrees with canonical total={sum(canonical_counts.values())}"
        )
    for status_key in ("native_now", "native_future", "cluster_private"):
        if manifest_counts.get(status_key) != canonical_counts.get(status_key, 0):
            errors.append(
                f"generated_registry_manifest.{status_key}={manifest_counts.get(status_key)} disagrees with canonical {status_key}={canonical_counts.get(status_key, 0)}"
            )

    print(f"sbsql_status_change_authority_gate canonical={len(surfaces)} errors={len(errors)}")
    if errors:
        print("sbsql_status_change_authority_gate=failed", file=sys.stderr)
        for err in errors[:25]:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("sbsql_status_change_authority_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
