#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Generate SBSQL differential replay fixture index and payload JSONL.

Canonical surface identities and operation families come only from the
single public surface snapshot.  Executable, exact-refusal, and cluster-provider
route classifications come only from the release declaration.  The older
implementation backlogs remain joined evidence and never override either
authority.  The output is deterministic and repo-local; it does not execute
the parser or engine and does not introduce parser-side storage/finality.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
CANONICAL_SURFACE_REGISTRY = "public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv"
FULL_SURFACE_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/full_parser_udr_engine/artifacts"
)
RELEASE_ARTIFACT_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
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


def surface_source_status(surface: dict[str, str]) -> str:
    return surface.get("source_status") or surface.get("status", "")


def read_surfaces(repo_root: Path) -> list[dict[str, str]]:
    path = repo_root / CANONICAL_SURFACE_REGISTRY
    if not path.is_file():
        raise FileNotFoundError(path)
    rows = read_csv(path)
    for row in rows:
        row["source_status"] = surface_source_status(row)
    return rows


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


def read_reference_native_names(artifact_root: Path) -> set[str]:
    path = artifact_root / "REFERENCE_ALIAS_COVERAGE_BACKLOG.csv"
    if not path.is_file():
        raise FileNotFoundError(path)
    rows = read_csv(path)
    names: set[str] = set()
    for row in rows:
        native_name = row.get("native_sbsql_surface", "")
        if not native_name:
            raise ValueError(
                "REFERENCE_ALIAS_COVERAGE_BACKLOG row missing native_sbsql_surface"
            )
        names.add(native_name)
    if not names:
        raise ValueError("REFERENCE_ALIAS_COVERAGE_BACKLOG has no native surface aliases")
    return names


def fixture_id(surface_id: str) -> str:
    digest = hashlib.sha256(surface_id.encode("utf-8")).hexdigest()[:12].upper()
    return f"SBSQL-SURFACE-{digest}"


VALID_RELEASE_STATUSES = {
    "e2e_passed",
    "exact_refusal_passed",
    "cluster_provider_route_passed",
}


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


def route_set(
    surface: dict[str, str],
    final_status: str,
    reference_native_names: set[str],
) -> list[str]:
    routes = list(BASE_ROUTES)
    if final_status == "e2e_passed":
        routes.extend(EXECUTION_ROUTES)
    if surface["canonical_name"] in reference_native_names:
        routes.append("reference_alias")
    return routes


def expected_engine_effect(final_status: str) -> str:
    if final_status == "e2e_passed":
        return "execute-sblr-internal-procedure-only-no-sql-text"
    if final_status == "cluster_provider_route_passed":
        return "no-engine-mutation;exact-refusal-or-profile-gate"
    if final_status == "exact_refusal_passed":
        return "no-engine-mutation-exact-refusal"
    raise ValueError(f"unsupported release final_status: {final_status}")


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
    parser.add_argument("--artifact-root", default=FULL_SURFACE_ARTIFACT_ROOT)
    parser.add_argument("--replay-root", default=REPLAY_ROOT)
    args = parser.parse_args()

    root = args.repo_root
    artifact_root = root / args.artifact_root
    release_root = root / RELEASE_ARTIFACT_ROOT
    replay_root = root / args.replay_root

    surfaces = read_surfaces(root)
    canonical_by_id = index_unique(
        surfaces, "surface_id", "SBSQL_SURFACE_REGISTRY"
    )
    reference_native_names = read_reference_native_names(artifact_root)
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
    release = index_unique(
        read_csv(release_root / "SBSQL_SURFACE_RELEASE_DECLARATION.csv"),
        "surface_id",
        "SBSQL_SURFACE_RELEASE_DECLARATION",
    )

    surface_ids = set(canonical_by_id)
    for label, rows_by_id in {
        "SURFACE_IMPLEMENTATION_BACKLOG": backlog,
        "BATCH_ROW_MEMBERSHIP": membership,
        "SEMANTIC_ORACLE_AUTHORITY_MAP": oracle,
        "SBSQL_SURFACE_RELEASE_DECLARATION": release,
    }.items():
        if set(rows_by_id) != surface_ids:
            missing = sorted(surface_ids - set(rows_by_id))
            extra = sorted(set(rows_by_id) - surface_ids)
            raise ValueError(
                f"{label} identity set mismatch: missing={missing[:3]} extra={extra[:3]}"
            )

    rows: list[dict[str, str]] = []
    for surface in surfaces:
        sid = surface["surface_id"]
        backlog_row = backlog[sid]
        member = membership[sid]
        oracle_row = oracle[sid]
        release_row = release[sid]
        for column in (
            "fixed_uuid_v7",
            "canonical_name",
            "family",
            "surface_kind",
            "source_status",
            "cluster_scope",
        ):
            if surface[column] != backlog_row[column] or surface[column] != member[column]:
                raise ValueError(f"{sid}: canonical {column} differs from evidence joins")
        for column in ("fixed_uuid_v7", "canonical_name", "family", "surface_kind"):
            if surface[column] != release_row[column]:
                raise ValueError(f"{sid}: canonical {column} differs from release evidence")
        if surface["validation_fixture_id"] != member["validation_fixture_id"]:
            raise ValueError(f"{sid}: canonical fixture differs from membership evidence")
        if surface["canonical_spec"] != backlog_row["canonical_spec"]:
            raise ValueError(f"{sid}: canonical specification differs from backlog evidence")
        if surface["canonical_spec"] != oracle_row["oracle_source"]:
            raise ValueError(f"{sid}: canonical specification differs from oracle evidence")
        if surface["oracle_key"] != oracle_row["oracle_type"]:
            raise ValueError(f"{sid}: canonical oracle identity differs from oracle evidence")
        final_status = release_row["final_status"]
        if final_status not in VALID_RELEASE_STATUSES:
            raise ValueError(f"{sid}: unsupported release final_status {final_status}")
        if release_row["release_status"] != "row_evidence_complete":
            raise ValueError(f"{sid}: release evidence is not complete")
        fid = member["validation_fixture_id"] or fixture_id(sid)
        routes = route_set(surface, final_status, reference_native_names)
        row = {
            "fixture_id": fid,
            "surface_id": sid,
            "batch_id": member["batch_id"],
            "canonical_name": surface["canonical_name"],
            "family": surface["family"],
            "surface_kind": surface["surface_kind"],
            "source_status": surface_source_status(surface),
            "cluster_scope": surface["cluster_scope"],
            "operation_family": surface["sblr_operation_family"],
            "primary_route": "parser_parse_only",
            "route_set": ";".join(routes),
            "parser_profile": "standalone-native-profile",
            "session_context": (
                "engine-issued-session-database-transaction-security-result-authority;"
                "release-evidence-bound"
            ),
            "input_text": input_text(surface),
            "expected_parse": "accepted-or-exact-canonical-refusal",
            "expected_bound_shape": (
                f"canonical-operation-family={surface['sblr_operation_family']};"
                "engine-issued-descriptor-and-result-authority;"
                "ExecutionResultEnvelope.v3 with message_vector_set"
            ),
            "expected_sblr_digest_policy": "stable-normalized-envelope-digest;not-derived-from-current-output",
            "expected_server_result": (
                f"release-evidence={final_status};"
                "admit-revalidate-or-exact-refuse-without-unauthorized-mutation"
            ),
            "expected_engine_effect": expected_engine_effect(final_status),
            "expected_message_vector": (
                "canonical_message_vector_and_parser_rendering;"
                f"{release_row['diagnostic_refs']};SBSQL.BINDING.*;SBLR.ENVELOPE.*;"
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
