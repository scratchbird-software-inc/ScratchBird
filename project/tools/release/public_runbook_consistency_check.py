#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public admin runbook coverage and evidence anchors."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_RUNBOOK_CONSISTENCY_CHECK
# PUBLIC_RUNBOOK_GATE

DOC_PATH = Path("project") / "docs" / "admin" / "PUBLIC_ADMIN_RUNBOOKS.md"
CMAKE_PATH = Path("project") / "tests" / "release" / "CMakeLists.txt"

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

POLICY_TOKENS = (
    "PUBLIC_ADMIN_RUNBOOKS",
    "public_release_evidence_only",
    "SQL text is not runtime authority",
    "MGA transaction inventory remains finality authority",
    "Cluster-positive production behavior is outside the public runbook scope",
)

RUNBOOKS: tuple[dict[str, Any], ...] = (
    {
        "runbook_id": "RUNBOOK_CREATE_DATABASE",
        "topic": "create",
        "doc_tokens": ("CreateDatabaseFile", "public_example_database_seed_gate"),
        "evidence": (
            ("project/tools/release/public_example_database_seed.cpp", ("CreateDatabaseFile",)),
            ("project/tests/release/CMakeLists.txt", ("public_example_database_seed_gate",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_OPEN_DATABASE",
        "topic": "open",
        "doc_tokens": ("OpenDatabaseFile", "public_example_database_seed_gate"),
        "evidence": (
            ("project/tools/release/public_example_database_seed.cpp", ("OpenDatabaseFile",)),
            ("project/tests/release/CMakeLists.txt", ("public_example_database_seed_gate",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_CONFIG_DEFAULTS",
        "topic": "config",
        "doc_tokens": ("PUBLIC_DEFAULT_CONFIG_CHECK", "PUBLIC_SECURE_DEFAULTS_GATE"),
        "evidence": (
            ("project/tools/release/public_default_config_check.py", ("PUBLIC_DEFAULT_CONFIG_CHECK",)),
            ("project/tests/release/public_secure_defaults_gate.cpp", ("PUBLIC_SECURE_DEFAULTS_GATE",)),
            ("project/tests/release/CMakeLists.txt", ("public_secure_defaults_gate",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_SECURITY_POLICY",
        "topic": "security",
        "doc_tokens": ("public_policy_coverage_matrix_gate", "public_enterprise_threat_gate"),
        "evidence": (
            ("project/tests/release/CMakeLists.txt", ("public_policy_coverage_matrix_gate", "public_custom_policy_pack_gate", "public_enterprise_threat_gate")),
            ("project/tools/release/public_unsupported_feature_matrix.py", ("SECURITY.AUTH_PROVIDER_UNSUPPORTED",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_BACKUP",
        "topic": "backup",
        "doc_tokens": ("public_backup_forward_session_gate", "public_backup_update_coverage_gate"),
        "evidence": (
            ("project/tests/release/CMakeLists.txt", ("public_backup_forward_session_gate", "public_backup_update_coverage_gate")),
        ),
    },
    {
        "runbook_id": "RUNBOOK_RESTORE",
        "topic": "restore",
        "doc_tokens": ("public_backup_update_coverage_gate", "public_cluster_catalog_backup_export_gate"),
        "evidence": (
            ("project/tests/release/CMakeLists.txt", ("public_backup_update_coverage_gate", "public_cluster_catalog_backup_export_gate", "restore")),
        ),
    },
    {
        "runbook_id": "RUNBOOK_VERIFY",
        "topic": "verify",
        "doc_tokens": ("public_example_smoke_gate", "public_disk_resource_bundle_gate"),
        "evidence": (
            ("project/tests/release/CMakeLists.txt", ("public_example_smoke_gate", "public_disk_resource_bundle_gate", "--verify")),
        ),
    },
    {
        "runbook_id": "RUNBOOK_REPAIR",
        "topic": "repair",
        "doc_tokens": ("public_repair_event_ledger_quarantine_gate", "public_repair_tamper_retention_crash_resume_gate"),
        "evidence": (
            ("project/tests/release/CMakeLists.txt", ("public_repair_event_ledger_quarantine_gate", "public_repair_history_inspection_api_gate", "public_repair_identity_rules_gate", "public_repair_tamper_retention_crash_resume_gate")),
        ),
    },
    {
        "runbook_id": "RUNBOOK_MEMORY_PRESSURE",
        "topic": "memory_pressure",
        "doc_tokens": ("public_memory_pressure_executor_gate", "memory_pressure"),
        "evidence": (
            ("project/tests/release/CMakeLists.txt", ("public_memory_pressure_executor_gate", "public_query_memory_reservation_gate", "public_performance_baseline_gate")),
            ("project/tests/performance/public_performance_baselines.json", ("memory_pressure",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_SWEEP",
        "topic": "sweep",
        "doc_tokens": ("public_mga_physical_sweep_gate", "SB-ROW-DATA-PHYSICAL-SWEEP-MGA-AUTHORITY-REQUIRED"),
        "evidence": (
            ("project/tests/release/CMakeLists.txt", ("public_mga_physical_sweep_gate",)),
            ("project/src/storage/page/row_data_physical_sweep.cpp", ("SB-ROW-DATA-PHYSICAL-SWEEP-MGA-AUTHORITY-REQUIRED",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_ARCHIVE",
        "topic": "archive",
        "doc_tokens": ("public_archive_before_reclaim_gate", "archive_slice"),
        "evidence": (
            ("project/tests/release/CMakeLists.txt", ("public_archive_before_reclaim_gate", "public_backup_forward_session_gate", "public_performance_baseline_gate")),
            ("project/tests/performance/public_performance_baselines.json", ("archive_slice",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_DIAGNOSTICS",
        "topic": "diagnostics",
        "doc_tokens": ("PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR", "public_diagnostic_stability_gate"),
        "evidence": (
            ("project/tools/release/public_diagnostic_matrix_generator.py", ("PUBLIC_DIAGNOSTIC_MATRIX_GENERATOR", "stable_public", "fail_closed_stable")),
            ("project/tests/release/CMakeLists.txt", ("public_diagnostic_stability_gate",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_UNSUPPORTED_FEATURES",
        "topic": "unsupported_features",
        "doc_tokens": ("PUBLIC_UNSUPPORTED_FEATURE_MATRIX", "public_unsupported_feature_gate"),
        "evidence": (
            ("project/tools/release/public_unsupported_feature_matrix.py", ("PUBLIC_UNSUPPORTED_FEATURE_MATRIX", "external_provider_required", "compile_time_disabled", "policy_blocked")),
            ("project/tests/release/CMakeLists.txt", ("public_unsupported_feature_gate",)),
        ),
    },
    {
        "runbook_id": "RUNBOOK_UPGRADE",
        "topic": "upgrade",
        "doc_tokens": ("PUBLIC_UPGRADE_MIGRATION_GATE", "downgrade_refusal", "rollback"),
        "evidence": (
            ("project/tests/release/public_upgrade_migration_gate.cpp", ("PUBLIC_UPGRADE_MIGRATION_GATE", "downgrade_requested", "rollback_before_commit")),
            ("project/tests/release/CMakeLists.txt", ("public_upgrade_migration_gate", "downgrade_refusal")),
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_runbook_consistency_check=fail:{message}", file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def reject_private_reference(value: str, context: str) -> None:
    if Path(value).is_absolute():
        fail(f"absolute_path_recorded:{context}:{value}")
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in value:
            fail(f"private_reference_recorded:{context}:{value}")


def read_text(repo_root: Path, relative_path: str | Path) -> str:
    path_text = Path(relative_path).as_posix()
    reject_private_reference(path_text, "source_path")
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{path_text}")
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{path_text}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{path_text}:{exc}")


def require_token(text: str, token: str, context: str) -> None:
    require(isinstance(token, str) and token, f"token_invalid:{context}")
    if token not in text:
        fail(f"token_missing:{context}:{token}")


def validate_docs(repo_root: Path) -> tuple[str, str]:
    doc_text = read_text(repo_root, DOC_PATH)
    for fragment in FORBIDDEN_REFERENCE_FRAGMENTS:
        if fragment in doc_text:
            fail(f"private_reference_recorded:runbook_doc:{fragment}")
    for token in POLICY_TOKENS:
        require_token(doc_text, token, "runbook_policy")
    return doc_text, sha256_text(doc_text)


def validate_runbook(repo_root: Path, doc_text: str, row: dict[str, Any]) -> dict[str, Any]:
    runbook_id = row["runbook_id"]
    topic = row["topic"]
    require_token(doc_text, runbook_id, f"runbook_doc:{runbook_id}")
    for token in row["doc_tokens"]:
        require_token(doc_text, token, f"runbook_doc:{runbook_id}")

    source_count = 0
    token_count = 0
    source_hashes: list[str] = []
    for path_text, tokens in row["evidence"]:
        source = read_text(repo_root, path_text)
        source_count += 1
        source_hashes.append(sha256_text(source))
        for token in tokens:
            require_token(source, token, f"evidence:{runbook_id}:{path_text}")
            token_count += 1

    return {
        "runbook_id": runbook_id,
        "topic": topic,
        "doc_token_count": 1 + len(row["doc_tokens"]),
        "evidence_source_count": source_count,
        "evidence_token_count": token_count,
        "source_digest_sha256": sha256_text("\n".join(sorted(source_hashes)) + "\n"),
        "status": "pass",
    }


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "runbook_id",
                "topic",
                "doc_token_count",
                "evidence_source_count",
                "evidence_token_count",
                "source_digest_sha256",
                "status",
            ],
        )
        writer.writeheader()
        writer.writerows(records)


def write_evidence(path: Path, evidence: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    doc_text, doc_sha256 = validate_docs(repo_root)
    records = [validate_runbook(repo_root, doc_text, row) for row in RUNBOOKS]
    topics = {record["topic"] for record in records}
    require(len(topics) == len(records), "topic_duplicate")

    write_csv(args.csv_output, records)
    csv_text = args.csv_output.read_text(encoding="utf-8")
    evidence = {
        "schema": "scratchbird.public.admin_runbook_gate.v1",
        "marker": "PUBLIC_RUNBOOK_CONSISTENCY_CHECK",
        "gate_marker": "PUBLIC_RUNBOOK_GATE",
        "gate": "PCR-GATE-140",
        "status": "pass",
        "doc_path": DOC_PATH.as_posix(),
        "doc_sha256": doc_sha256,
        "csv_sha256": sha256_text(csv_text),
        "runbook_count": len(records),
        "topics": sorted(topics),
        "authority": "public_release_evidence_only",
        "policy": {
            "private_execution_plan_references_allowed": False,
            "absolute_local_paths_allowed": False,
            "release_proof_is_evidence_only": True,
            "mga_finality_authority_preserved": True,
            "parser_sql_text_authority": False,
            "cluster_public_production_claims": False,
        },
        "records": records,
    }
    write_evidence(args.evidence_output, evidence)
    print(
        "public_runbook_consistency_gate=passed "
        f"runbooks={len(records)} "
        f"csv_sha256={evidence['csv_sha256']}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
