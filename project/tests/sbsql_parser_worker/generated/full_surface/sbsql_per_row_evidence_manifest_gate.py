#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Per-row evidence manifest gate for the SBsql Surface-to-SBLR regression fixtures.

The manifest is the release handoff surface for row-level evidence. This gate
checks that it remains a deterministic, one-row-per-surface projection of the
canonical registry and strict ledger, and that rows claiming a final state carry
specific evidence instead of family-only or blocked placeholders.

Architecture invariant compliance: read-only consistency check; no parser,
engine, server, storage, donor, cluster, or MGA behavior is modified. The gate
does not introduce WAL, donor-backed storage, or parser-side execution authority.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from pathlib import Path


REGISTRY = "public_input_snapshot"
DEFAULT_ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
STRICT_LEDGER_NAME = "STRICT_ROW_COVERAGE_LEDGER.csv"
MANIFEST_NAME = "PER_ROW_EVIDENCE_MANIFEST.csv"

REQUIRED_COLUMNS = [
    "surface_id",
    "canonical_name",
    "status",
    "cluster_scope",
    "surface_kind",
    "sblr_operation_family",
    "final_state",
    "ctest_label",
    "fixture_path",
    "implementation_refs",
    "diagnostic_proof",
    "result_proof",
    "evidence_collected_utc",
    "promoter_slice",
    "notes",
]

REGISTRY_FIELDS = [
    "canonical_name",
    "status",
    "cluster_scope",
    "surface_kind",
    "sblr_operation_family",
]

FINAL_STATES = {
    "e2e_passed",
    "exact_refusal_passed",
    "cluster_provider_route_passed",
    "private_profile_implemented",
}
ALLOWED_FINAL_STATES_BY_STATUS = {
    "native_now": {"pending", "e2e_passed"},
    "native_future": {"pending"},
    "cluster_private": {
        "pending",
        "exact_refusal_passed",
        "cluster_provider_route_passed",
        "private_profile_implemented",
    },
}
FINAL_STATE_TO_LEDGER_STATE = {
    "e2e_passed": "e2e_passed",
    "exact_refusal_passed": "exact_refusal_passed",
    "cluster_provider_route_passed": "exact_refusal_passed",
    "private_profile_implemented": "private_profile_implemented",
}
BLOCKED_TOKENS_FOR_FINAL_ROWS = {
    "blocked_until_canonical_builtin_expression_registry_records_oracle_or_row_routed_to_remove_by_spec_change_via_SBSFC-009B",
    "pending_canonical_authority_entry",
    "pending_row_level",
    "family_only",
    "covered_by_parser_packet_family_only",
    "covered_by_sblr_operation_matrix_family_only",
    "covered_by_sblr_admission_family_only",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            fail(f"required CSV has no header: {path}")
        return list(reader)


def index_unique(label: str, rows: list[dict[str, str]], errors: list[str]) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    seen: Counter[str] = Counter()
    for row in rows:
        sid = row.get("surface_id", "")
        if not sid:
            errors.append(f"{label} row missing surface_id")
            continue
        seen[sid] += 1
        out.setdefault(sid, row)
    for sid, count in seen.items():
        if count != 1:
            errors.append(f"{label} duplicate surface_id {sid} count={count}")
    return out


def contains_blocked_token(text: str) -> str | None:
    for token in sorted(BLOCKED_TOKENS_FOR_FINAL_ROWS):
        if token in text:
            return token
    return None


def allowed_final_states(status: str, cluster_scope: str) -> set[str] | None:
    if status == "native_now" and cluster_scope == "cluster_private":
        return {
            "pending",
            "exact_refusal_passed",
            "cluster_provider_route_passed",
            "private_profile_implemented",
        }
    return ALLOWED_FINAL_STATES_BY_STATUS.get(status)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--artifact-root", default=DEFAULT_ARTIFACT_ROOT)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    artifact_root = Path(args.artifact_root)
    if not artifact_root.is_absolute():
        artifact_root = root / artifact_root

    registry_rows = read_csv(root / REGISTRY)
    ledger_rows = read_csv(artifact_root / STRICT_LEDGER_NAME)
    manifest_rows = read_csv(artifact_root / MANIFEST_NAME)

    errors: list[str] = []

    with (artifact_root / MANIFEST_NAME).open(newline="", encoding="utf-8") as handle:
        fieldnames = csv.DictReader(handle).fieldnames or []
    if fieldnames != REQUIRED_COLUMNS:
        errors.append(f"PER_ROW_EVIDENCE_MANIFEST header mismatch: expected={REQUIRED_COLUMNS} observed={fieldnames}")

    registry = index_unique("SBSQL_SURFACE_REGISTRY", registry_rows, errors)
    ledger = index_unique("STRICT_ROW_COVERAGE_LEDGER", ledger_rows, errors)
    manifest = index_unique("PER_ROW_EVIDENCE_MANIFEST", manifest_rows, errors)

    registry_ids = set(registry)
    ledger_ids = set(ledger)
    manifest_ids = set(manifest)

    if manifest_ids != registry_ids:
        missing = sorted(registry_ids - manifest_ids)
        extra = sorted(manifest_ids - registry_ids)
        if missing:
            errors.append(f"PER_ROW_EVIDENCE_MANIFEST missing {len(missing)} registry surface_ids first={missing[:5]}")
        if extra:
            errors.append(f"PER_ROW_EVIDENCE_MANIFEST has {len(extra)} unknown surface_ids first={extra[:5]}")
    if ledger_ids != registry_ids:
        missing = sorted(registry_ids - ledger_ids)
        extra = sorted(ledger_ids - registry_ids)
        if missing:
            errors.append(f"STRICT_ROW_COVERAGE_LEDGER missing {len(missing)} registry surface_ids first={missing[:5]}")
        if extra:
            errors.append(f"STRICT_ROW_COVERAGE_LEDGER has {len(extra)} unknown surface_ids first={extra[:5]}")

    final_counts: Counter[str] = Counter()
    status_counts: Counter[str] = Counter()

    for sid in sorted(manifest_ids & registry_ids):
        row = manifest[sid]
        reg = registry[sid]
        led = ledger.get(sid, {})
        final_state = row.get("final_state", "")
        status = row.get("status", "")
        final_counts[final_state] += 1
        status_counts[status] += 1

        for field in REQUIRED_COLUMNS:
            if field not in row:
                errors.append(f"{sid} missing manifest field {field}")
            elif field not in {"evidence_collected_utc"} and row[field] == "":
                errors.append(f"{sid} manifest field {field} is blank")

        for field in REGISTRY_FIELDS:
            if row.get(field, "") != reg.get(field, ""):
                errors.append(f"{sid} manifest {field} drift: registry={reg.get(field, '')} manifest={row.get(field, '')}")
            if led and led.get(field, "") != reg.get(field, ""):
                errors.append(f"{sid} ledger {field} drift: registry={reg.get(field, '')} ledger={led.get(field, '')}")

        cluster_scope = row.get("cluster_scope", "")
        allowed_states = allowed_final_states(status, cluster_scope)
        if allowed_states is None:
            errors.append(f"{sid} unknown status {status}")
        elif final_state not in allowed_states:
            errors.append(
                f"{sid} final_state={final_state} not allowed for status={status} cluster_scope={cluster_scope}"
            )

        if led:
            ledger_state = led.get("current_state", "")
            expected_ledger_state = FINAL_STATE_TO_LEDGER_STATE.get(final_state)
            if expected_ledger_state and ledger_state != expected_ledger_state:
                errors.append(f"{sid} final_state={final_state} but strict ledger current_state={ledger_state}")
            if final_state == "pending" and ledger_state in FINAL_STATES:
                errors.append(f"{sid} manifest pending but strict ledger current_state={ledger_state}")

        if final_state in FINAL_STATES:
            final_text = ";".join(row.get(field, "") for field in REQUIRED_COLUMNS)
            blocked_token = contains_blocked_token(final_text)
            if blocked_token:
                errors.append(f"{sid} final row still contains blocked/family-only token {blocked_token}")
            for field in [
                "ctest_label",
                "fixture_path",
                "implementation_refs",
                "diagnostic_proof",
                "result_proof",
                "evidence_collected_utc",
                "promoter_slice",
            ]:
                if not row.get(field, ""):
                    errors.append(f"{sid} final row missing {field}")
            if final_state != "cluster_provider_route_passed" and (
                "sbsql_surface_to_sblr_full_implementation_closure" not in row.get("ctest_label", "")
            ):
                errors.append(f"{sid} final row ctest_label missing execution_plan label")
            if final_state == "cluster_provider_route_passed" and (
                "sbsql_cluster_provider_split" not in row.get("ctest_label", "")
            ):
                errors.append(f"{sid} cluster_provider_route_passed ctest_label missing cluster provider label")
            if "sbsql_parser_worker" not in row.get("ctest_label", ""):
                errors.append(f"{sid} final row ctest_label missing parser worker label")
            if final_state == "e2e_passed" and (
                row.get("status") != "native_now" or row.get("cluster_scope") == "cluster_private"
            ):
                errors.append(f"{sid} e2e_passed requires noncluster native_now status")
            if final_state == "exact_refusal_passed" and row.get("cluster_scope") != "cluster_private":
                errors.append(
                    f"{sid} exact_refusal_passed is only accepted for cluster_scope=cluster_private public fail-closed rows"
                )
            if final_state == "cluster_provider_route_passed":
                final_text = ";".join(row.get(field, "") for field in REQUIRED_COLUMNS)
                required_tokens = [
                    "provider_boundary_route_evidence",
                    "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
                    "SBLR.CLUSTER.HANDSHAKE.STUB_COMPILE_LINK_ONLY",
                    "request_lifecycle_routed_through_cluster_provider_boundary",
                    "private_cluster_execution=false",
                ]
                if row.get("cluster_scope") != "cluster_private":
                    errors.append(
                        f"{sid} cluster_provider_route_passed requires cluster_scope=cluster_private"
                    )
                for token in required_tokens:
                    if token not in final_text:
                        errors.append(f"{sid} cluster_provider_route_passed missing proof token {token}")
        else:
            if final_state != "pending":
                errors.append(f"{sid} unknown final_state {final_state}")
            if row.get("evidence_collected_utc", ""):
                errors.append(f"{sid} pending row unexpectedly has evidence_collected_utc={row['evidence_collected_utc']}")

    counts_text = " ".join(f"{key}={value}" for key, value in sorted(final_counts.items()))
    print(f"sbsql_per_row_evidence_manifest_gate rows={len(manifest_rows)} {counts_text} errors={len(errors)}")
    print("sbsql_per_row_evidence_manifest_gate_status " + " ".join(f"{key}={value}" for key, value in sorted(status_counts.items())))
    if errors:
        print("sbsql_per_row_evidence_manifest_gate=failed", file=sys.stderr)
        for err in errors[:40]:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("sbsql_per_row_evidence_manifest_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
