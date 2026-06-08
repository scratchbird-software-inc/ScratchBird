#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Cross-cutting crash/fault coverage matrix for PCR-115."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_CRASH_FAULT_MATRIX

MATRIX_ROWS: tuple[dict[str, Any], ...] = (
    {
        "row_id": "create_sync",
        "fault_class": "create_sync_crash_reopen",
        "gate": "public_disk_file_device_gate",
        "mga_authority": "not_applicable_storage_sync_only",
        "evidence_files": {
            "tests/release/public_disk_file_device_gate.cpp": (
                "create_new should atomically create a missing file",
                "scratchbird-durable-sync",
                "sync should perform durable writable-file sync",
                "concurrent create_new must have exactly one winner",
            ),
        },
    },
    {
        "row_id": "transaction_publish",
        "fault_class": "copy_on_write_publish_recovery",
        "gate": "public_transaction_mga_cow_gate",
        "mga_authority": "durable_transaction_inventory",
        "evidence_files": {
            "tests/release/public_transaction_mga_cow_gate.cpp": (
                "publish_pending_transaction",
                "published COW without evidence must be refused",
                "evidence_record_written",
                "recovery_required",
                "prepared commit should publish committed state",
            ),
        },
    },
    {
        "row_id": "index_update",
        "fault_class": "index_metadata_reopen_and_update_tamper",
        "gate": "public_index_durable_metadata_validator_gate",
        "mga_authority": "durable_index_metadata_is_evidence_not_finality",
        "evidence_files": {
            "tests/release/public_index_readiness_matrix_gate.cpp": (
                "recovery_reopen",
                "durable_closure_admitted_from_state",
                "durable_closure_claimed",
            ),
            "tests/release/public_index_durable_metadata_validator_gate.cpp": (
                "durable_metadata_present",
                "metadata_checksum_low64",
                "accepted tampered durable metadata",
                "corrupted metapage metadata",
                "family validator claimed engine authority",
            ),
        },
    },
    {
        "row_id": "archive_before_reclaim",
        "fault_class": "archive_manifest_fail_closed",
        "gate": "public_archive_before_reclaim_gate",
        "mga_authority": "archive_requires_local_mga_authority",
        "evidence_files": {
            "tests/release/public_archive_before_reclaim_gate.cpp": (
                "SBARCHIVERECLAIM1",
                "movement_record_checksum",
                "transaction_finality_authority",
                "missing MGA authority should fail closed",
                "cluster archive route should fail closed",
            ),
        },
    },
    {
        "row_id": "backup_forward",
        "fault_class": "backup_forward_segment_replay_gap",
        "gate": "public_backup_forward_session_gate",
        "mga_authority": "backup_forward_is_replay_evidence_not_finality",
        "evidence_files": {
            "tests/release/public_backup_forward_session_gate.cpp": (
                "write_after_segment_immutable",
                "coverage_contiguous",
                "authoritative_wal",
                "cluster_authority_required",
                "coverage gap should fail closed",
            ),
            "tests/release/public_backup_update_coverage_gate.cpp": (
                "verified_segment_manifest_uris",
                "PITR_TARGET_OUTSIDE_COVERAGE",
                "reused_segment_count",
                "cluster_recovery_authority",
            ),
        },
    },
    {
        "row_id": "repair_ledger",
        "fault_class": "repair_ledger_crash_resume_tamper",
        "gate": "public_repair_tamper_retention_crash_resume_gate",
        "mga_authority": "repair_evidence_never_replaces_mga",
        "evidence_files": {
            "tests/release/public_repair_tamper_retention_crash_resume_gate.cpp": (
                "crash_resume_started",
                "crash_resume_replay_admitted",
                "crash_resume_completed",
                "repair_evidence_recovery_authority=false",
                "durable_mga_inventory_authority",
                "crash resume should reject forged in-memory ledger state",
            ),
        },
    },
    {
        "row_id": "security_state",
        "fault_class": "security_state_credential_replay",
        "gate": "public_security_durable_crypto_hardening_gate",
        "mga_authority": "security_catalog_authority_not_transaction_finality",
        "evidence_files": {
            "tests/release/public_security_durable_crypto_hardening_gate.cpp": (
                "failed to create durable principal credential state",
                "storage_authority=durable_security_catalog",
                "durable_principal_credential_missing",
                "durable verifier mismatch was accepted",
                "security_database_temporary_token_revoked",
                "security_state_authority",
            ),
        },
    },
    {
        "row_id": "agent_action_record",
        "fault_class": "agent_action_idempotent_replay_and_compensation",
        "gate": "public_agent_action_dispatch_idempotency_gate",
        "mga_authority": "agent_action_evidence_only",
        "evidence_files": {
            "tests/agents/agent_action_dispatch_idempotency_gate.cpp": (
                "fsync_or_checkpoint_evidence",
                "durable_record_written",
                "IDEMPOTENT_REPLAY",
                "OUTCOME_UNVERIFIED_COMPENSATION_REQUIRED",
                "durable action record claimed untrusted authority",
            ),
        },
    },
    {
        "row_id": "agent_fault_injection",
        "fault_class": "agent_faults_fail_closed_without_state_mutation",
        "gate": "agent_fault_injection_gate",
        "mga_authority": "agent_fault_evidence_only",
        "evidence_files": {
            "tests/agents/agent_fault_injection_gate.cpp": (
                "fsync_failure",
                "durable_state_changed=false",
                "evidence_before_success=true",
                "restart_mid_action",
                "queue_corruption",
                "partial_page_preallocation",
            ),
            "tests/agents/agent_security_crash_fixture_hardening_gate.cpp": (
                "crash_recovery_mode",
                "previous_catalog_root_digest",
                "durable replay history was not retained",
                "client spoofed action authority was accepted",
                "agent_fixture_separation.transaction_finality_authority=false",
            ),
        },
    },
)


def fail(message: str) -> None:
    print(f"public_crash_fault_matrix=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def require_file(project_root: Path, relative: str) -> str:
    path = project_root / relative
    if not path.is_file():
        fail(f"missing_file:{relative}")
    return path.read_text(encoding="utf-8")


def require_tokens(text: str, relative: str, tokens: tuple[str, ...]) -> list[str]:
    missing = [token for token in tokens if token not in text]
    if missing:
        fail(f"missing_tokens:{relative}:{','.join(missing)}")
    return list(tokens)


def build_evidence(project_root: Path) -> dict[str, Any]:
    if project_root.name != "project" or not project_root.is_dir():
        fail("project_root_must_be_project_directory")

    rows: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    for row in MATRIX_ROWS:
        row_id = row["row_id"]
        if row_id in seen_ids:
            fail(f"duplicate_row:{row_id}")
        seen_ids.add(row_id)
        file_records: list[dict[str, Any]] = []
        for relative, tokens in row["evidence_files"].items():
            text = require_file(project_root, relative)
            file_records.append(
                {
                    "path": relative,
                    "sha256": sha256_text(text),
                    "required_tokens": require_tokens(text, relative, tokens),
                }
            )
        rows.append(
            {
                "row_id": row_id,
                "fault_class": row["fault_class"],
                "gate": row["gate"],
                "mga_authority": row["mga_authority"],
                "file_count": len(file_records),
                "files": file_records,
            }
        )

    return {
        "schema_version": 1,
        "gate": "PCR-GATE-115",
        "marker": "PUBLIC_CRASH_FAULT_MATRIX",
        "policy": {
            "deterministic": True,
            "private_docs_required": False,
            "faults_fail_closed": True,
            "mga_authority_drift_refused": True,
            "crash_reopen_rows_required": True,
        },
        "row_count": len(rows),
        "rows": rows,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    evidence = build_evidence(Path(args.project_root).resolve())
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n",
                      encoding="utf-8")
    print(
        "public_crash_fault_matrix=passed "
        f"rows={evidence['row_count']} output={output.name}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
