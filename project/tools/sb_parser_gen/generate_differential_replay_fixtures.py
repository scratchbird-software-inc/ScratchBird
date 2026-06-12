#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SBSQL differential replay fixture index and payload JSONL.

The replay fixtures are derived from the canonical SBSQL surface registry,
the SBLR operation matrix, and the full parser/UDR/engine proof artifacts.
The output is intentionally deterministic and repo-local; it does not execute
the parser or engine and does not introduce parser-side storage/finality.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CANONICALIZATION_ROOT = (
    "public_input_snapshot"
)
FULL_SURFACE_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
)
REPLAY_ROOT = "project/tests/sbsql_parser_worker/generated/replay"

INDEX_COLUMNS = [
    "fixture_id",
    "surface_id",
    "batch_id",
    "canonical_name",
    "family",
    "surface_kind",
    "source_status",
    "cluster_scope",
    "operation_family",
    "primary_route",
    "route_set",
    "parser_profile",
    "session_context",
    "input_text",
    "expected_parse",
    "expected_bound_shape",
    "expected_sblr_digest_policy",
    "expected_server_result",
    "expected_engine_effect",
    "expected_message_vector",
    "expected_rendered_output",
    "oracle_type",
    "oracle_source",
    "source_search_key",
    "expected_result_summary",
    "expected_payload_json",
    "status",
]

BASE_ROUTES = [
    "parser_parse_only",
    "parser_bind_lower",
    "diagnostic",
    "server_admission",
]
EXECUTION_ROUTES = ["udr_sql_to_sblr", "engine_behavior", "full_route"]


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def index_unique(rows: list[dict[str, str]], key: str, label: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        value = row.get(key, "")
        if not value:
            raise ValueError(f"{label} row missing {key}")
        if value in out:
            raise ValueError(f"{label} duplicate {key}: {value}")
        out[value] = row
    return out


def read_reference_native_names(canonicalization_root: Path) -> set[str]:
    rows = read_csv(canonicalization_root / "REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX.csv")
    names: set[str] = set()
    for row in rows:
        native_name = row.get("native_sbsql_surface", "")
        if not native_name:
            raise ValueError("REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX row missing native_sbsql_surface")
        names.add(native_name)
    if not names:
        raise ValueError("REFERENCE_ALIAS_TO_SBSQL_SURFACE_MATRIX has no native surface aliases")
    return names


def fixture_id(surface_id: str) -> str:
    digest = hashlib.sha256(surface_id.encode("utf-8")).hexdigest()[:12].upper()
    return f"SBSQL-SURFACE-{digest}"


def active_native(surface: dict[str, str]) -> bool:
    return (
        surface["status"] == "native_now"
        and surface["cluster_scope"] != "cluster_private"
    )


def input_text(surface: dict[str, str]) -> str:
    name = surface["canonical_name"]
    if name == "begin_transaction":
        return "BEGIN TRANSACTION"
    if name == "begin_stmt":
        return "BEGIN"
    if name == "commit":
        return "COMMIT"
    if name == "commit_stmt":
        return "COMMIT"
    if name == "rollback":
        return "ROLLBACK"
    if name == "rollback_stmt":
        return "ROLLBACK"
    if name == "savepoint":
        return "SAVEPOINT replay_savepoint"
    if name == "set_transaction_stmt":
        return "SET TRANSACTION READ WRITE"
    if name == "migrate_from_reference":
        return "MIGRATE FROM REFERENCE postgres WITH PACKAGE pg_compat_pack"
    if name == "alter_migration":
        return "ALTER MIGRATION 019f0000-0000-7000-8000-00000000a001 START"
    if name == "show_migration":
        return "SHOW MIGRATION 019f0000-0000-7000-8000-00000000a001"
    if name == "show_migrations":
        return "SHOW MIGRATIONS"
    if name == "lock_table":
        return "LOCK TABLE accounts IN SHARE MODE"
    if name == "lock_table_stmt":
        return "LOCK TABLE accounts IN SHARE MODE"
    if name == "get_lock(name,timeout)":
        return "SELECT get_lock('gate', 1)"
    if name == "release_lock(name)":
        return "SELECT release_lock('gate')"
    return f"SBSQL_SURFACE_REPLAY {surface['surface_id']}"


def route_set(surface: dict[str, str], reference_native_names: set[str]) -> list[str]:
    routes = list(BASE_ROUTES)
    if active_native(surface):
        routes.extend(EXECUTION_ROUTES)
    if surface["canonical_name"] in reference_native_names:
        routes.append("reference_alias")
    return routes


def expected_engine_effect(surface: dict[str, str]) -> str:
    if active_native(surface):
        return "execute-sblr-internal-procedure-only-no-sql-text"
    if surface["cluster_scope"] == "cluster_private":
        return "no-engine-mutation;exact-refusal-or-profile-gate"
    return "no-engine-mutation-exact-refusal"


def payload_for(row: dict[str, str]) -> dict[str, object]:
    routes = row["route_set"].split(";")
    return {
        "binding": {
            "expected_bound_shape": row["expected_bound_shape"],
            "expected_sblr_digest_policy": row["expected_sblr_digest_policy"],
            "session_context": row["session_context"],
        },
        "cleanup_policy": "retain-failure-packet-and-redacted-log;delete-temporary-db-and-sockets",
        "diagnostics": {
            "expected_message_vector": row["expected_message_vector"],
            "expected_rendered_output": row["expected_rendered_output"],
        },
        "engine": {
            "expected_effect": row["expected_engine_effect"],
            "operation_family": row["operation_family"],
        },
        "fixture_id": row["fixture_id"],
        "input": {
            "kind": "client_sbsql_or_reference_profile_text",
            "surface_authority": "SBSQL_SURFACE_REGISTRY.csv",
            "text": row["input_text"],
        },
        "oracle": {
            "source": row["oracle_source"],
            "summary": row["expected_result_summary"],
            "type": row["oracle_type"],
        },
        "parser": {
            "expected_cst_ast": "lossless-cst-ast-or-exact-diagnostic",
            "expected_parse": row["expected_parse"],
            "profile": row["parser_profile"],
        },
        "routes": routes,
        "server": {"expected_result": row["expected_server_result"]},
        "surface_id": row["surface_id"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--canonicalization-root", default=CANONICALIZATION_ROOT)
    parser.add_argument("--artifact-root", default=FULL_SURFACE_ARTIFACT_ROOT)
    parser.add_argument("--replay-root", default=REPLAY_ROOT)
    args = parser.parse_args()

    root = args.repo_root
    canonicalization_root = root / args.canonicalization_root
    artifact_root = root / args.artifact_root
    replay_root = root / args.replay_root

    surfaces = read_csv(canonicalization_root / "SBSQL_SURFACE_REGISTRY.csv")
    reference_native_names = read_reference_native_names(canonicalization_root)
    operations = index_unique(
        read_csv(canonicalization_root / "SBSQL_TO_SBLR_OPERATION_MATRIX.csv"),
        "surface_id",
        "SBSQL_TO_SBLR_OPERATION_MATRIX",
    )
    backlog = index_unique(
        read_csv(artifact_root / "SURFACE_IMPLEMENTATION_BACKLOG.csv"),
        "surface_id",
        "SURFACE_IMPLEMENTATION_BACKLOG",
    )
    membership = index_unique(
        read_csv(artifact_root / "BATCH_ROW_MEMBERSHIP.csv"),
        "surface_id",
        "BATCH_ROW_MEMBERSHIP",
    )
    oracle = index_unique(
        read_csv(artifact_root / "SEMANTIC_ORACLE_AUTHORITY_MAP.csv"),
        "surface_id",
        "SEMANTIC_ORACLE_AUTHORITY_MAP",
    )

    rows: list[dict[str, str]] = []
    for surface in surfaces:
        sid = surface["surface_id"]
        if sid not in operations or sid not in backlog or sid not in membership or sid not in oracle:
            raise ValueError(f"{sid}: missing replay authority row")
        operation = operations[sid]
        member = membership[sid]
        oracle_row = oracle[sid]
        fid = member["validation_fixture_id"] or fixture_id(sid)
        routes = route_set(surface, reference_native_names)
        row = {
            "fixture_id": fid,
            "surface_id": sid,
            "batch_id": member["batch_id"],
            "canonical_name": surface["canonical_name"],
            "family": surface["family"],
            "surface_kind": surface["surface_kind"],
            "source_status": surface["status"],
            "cluster_scope": surface["cluster_scope"],
            "operation_family": operation["sblr_operation_family"],
            "primary_route": "parser_parse_only",
            "route_set": ";".join(routes),
            "parser_profile": "standalone-native-profile",
            "session_context": operation["required_context"],
            "input_text": input_text(surface),
            "expected_parse": "accepted-or-exact-canonical-refusal",
            "expected_bound_shape": (
                f"{operation['sblr_operation_family']};{operation['result_shape']};"
                "ExecutionResultEnvelope.v3 with message_vector_set"
            ),
            "expected_sblr_digest_policy": "stable-normalized-envelope-digest;not-derived-from-current-output",
            "expected_server_result": (
                "accepted-or-exact-canonical-refusal;"
                "admit_revalidate_route_stream_cancel_and_return_message_vector"
            ),
            "expected_engine_effect": expected_engine_effect(surface),
            "expected_message_vector": (
                "canonical_message_vector_and_parser_rendering;"
                f"{operation['diagnostics']};SBSQL.BINDING.*;SBLR.ENVELOPE.*;"
                "SBLR.OPCODE.*;SECURITY.*;CATALOG.NAME.*"
            ),
            "expected_rendered_output": "ExecutionResultEnvelope.v3 with message_vector_set",
            "oracle_type": oracle_row["oracle_type"],
            "oracle_source": oracle_row["oracle_source"],
            "source_search_key": sid,
            "expected_result_summary": oracle_row["expected_result_summary"],
            "expected_payload_json": (
                "project/tests/sbsql_parser_worker/generated/replay/"
                f"DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl#{fid}"
            ),
            "status": "replay_ready",
        }
        rows.append(row)

    replay_root.mkdir(parents=True, exist_ok=True)
    with (replay_root / "DIFFERENTIAL_REPLAY_FIXTURE_INDEX.csv").open(
        "w", newline="", encoding="utf-8"
    ) as handle:
        writer = csv.DictWriter(handle, fieldnames=INDEX_COLUMNS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    with (replay_root / "DIFFERENTIAL_REPLAY_EXPECTED_PAYLOADS.jsonl").open(
        "w", encoding="utf-8"
    ) as handle:
        for row in rows:
            handle.write(
                json.dumps(payload_for(row), sort_keys=True, separators=(",", ":"))
            )
            handle.write("\n")

    print(
        "differential_replay_fixtures=generated "
        f"rows={len(rows)} replay_root={replay_root}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
