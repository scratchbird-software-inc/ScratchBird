#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public disaster recovery drill proof anchors."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys
from typing import Any


# PUBLIC_DISASTER_RECOVERY_GATE

FORBIDDEN_REFERENCE_FRAGMENTS = (
    "docs" + "/" + "execution-plans",
    "docs" + "/" + "completed-execution-plans",
    "docs" + "/" + "findings",
    "." + "git",
    "/" + "home" + "/",
    "ScratchBird" + "-Private",
    "local" + "_work",
)

DRILLS: tuple[dict[str, Any], ...] = (
    {
        "drill": "restore_to_new_location_logical",
        "path": "project/tests/release/public_backup_forward_session_gate.cpp",
        "tokens": (
            "CreateDatabaseFixture(work_dir / \"target.sbdb\"",
            "EngineRestoreLogicalBackup",
            "PCR-085 base restore should succeed",
            "EngineApplyDeltaStream",
            "PCR-085 target should contain base plus write-after rows",
            "restore_inspection_open:true",
            "recovery_classification_verified:true",
        ),
    },
    {
        "drill": "write_after_replay_idempotent",
        "path": "project/tests/release/public_backup_forward_session_gate.cpp",
        "tokens": (
            "EngineStartBackupForwardSession",
            "EngineFinishBackupForwardSession",
            "write_after_segment_immutable",
            "EngineApplyDeltaStream",
            "PCR-085 delta apply should be idempotent",
            "PCR-085 second delta apply should skip already-applied row",
            "deterministic_mga_operation_envelopes",
        ),
    },
    {
        "drill": "interrupted_restore_inspection_required",
        "path": "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp",
        "tokens": (
            "restore_inspection_open",
            "recovery_classification_verified",
            "RESTORE_INSPECTION_OPEN_REQUIRED",
            "RESTORE_RECOVERY_CLASSIFICATION_REQUIRED",
            "shutdown_blocker_registered",
            "drop_blocker_registered",
        ),
    },
    {
        "drill": "corrupted_backup_refused",
        "path": "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp",
        "tokens": (
            "ReadAndVerifyManifest",
            "ReadAndVerifyPhysicalManifest",
            "ReadAndVerifyDeltaManifest",
            "RESTORE_MANIFEST_CHECKSUM_MISSING",
            "RESTORE_MANIFEST_CHECKSUM_INVALID",
            "RESTORE_MANIFEST_MAGIC_INVALID",
        ),
    },
    {
        "drill": "missing_segment_refused",
        "path": "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp",
        "tokens": (
            "BACKUP_DELTA_REQUEST_INVALID",
            "BACKUP_DELTA_COVERAGE_GAP:expected_start_",
            "BACKUP_UPDATE_COVERAGE_GAP",
            "BACKUP_UPDATE_COVERAGE_OVERLAP",
            "BACKUP_UPDATE_IDEMPOTENCY_KEY_MISMATCH",
        ),
    },
    {
        "drill": "pitr_selector_and_refusals",
        "path": "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp",
        "tokens": (
            "pitr_target_selector:",
            "latest-valid",
            "pitr_target_transaction_id:",
            "pitr_target_unix_micros:",
            "restore_point_name:",
            "PITR_TARGET_OUTSIDE_COVERAGE:timestamp",
            "PITR_TARGET_OUTSIDE_COVERAGE:restore_point_name",
            "PITR_TARGET_OUTSIDE_COVERAGE:transaction",
        ),
    },
    {
        "drill": "pitr_public_gate_coverage",
        "path": "project/tests/release/public_backup_update_coverage_gate.cpp",
        "tokens": (
            "PCR-087 PITR target inside coverage should pass",
            "PCR-087 PITR target outside coverage should fail",
            "PITR_TARGET_OUTSIDE_COVERAGE:transaction",
            "BACKUP_UPDATE_COVERAGE_GAP",
            "BACKUP_UPDATE_COVERAGE_OVERLAP",
        ),
    },
    {
        "drill": "post_restore_identity_security_validation",
        "path": "project/tests/release/public_cluster_catalog_backup_export_gate.cpp",
        "tokens": (
            "ClusterCatalogTransferOperation::restore",
            "security_binding_proven",
            "projection_integrity_proven",
            "resolver_comment_security_projection_proven",
            "CLUSTER_CATALOG_TARGET_UUID_REUSED",
            "identity policy performed mutation",
            "identity policy enabled local cluster execution",
        ),
    },
    {
        "drill": "physical_restore_manifest_and_image_verification",
        "path": "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp",
        "tokens": (
            "EngineRestorePhysicalBackup",
            "ReadAndVerifyPhysicalManifest",
            "physical_restore_manifest_validated",
            "physical_restore_image_checksum",
            "evidence_before_success",
            "physical_restore_installed",
        ),
    },
    {
        "drill": "authority_boundary",
        "path": "project/src/engine/internal_api/backup_archive/backup_archive_api.cpp",
        "tokens": (
            "local_mga_transaction_inventory",
            "authoritative_wal",
            "BACKUP_UPDATE_AUTHORITATIVE_WAL_FORBIDDEN",
            "BACKUP_FORWARD_AUTHORITATIVE_WAL_FORBIDDEN",
            "transaction_finality_authority",
            "write_after_recovery_authority",
            "cluster_recovery_authority",
        ),
    },
    {
        "drill": "release_gate_wiring",
        "path": "project/tests/release/CMakeLists.txt",
        "tokens": (
            "public_backup_forward_session_gate",
            "public_backup_update_coverage_gate",
            "public_cluster_catalog_backup_export_gate",
            "public_disaster_recovery_gate",
        ),
    },
)


def fail(message: str) -> None:
    print(f"public_disaster_recovery_gate=fail:{message}", file=sys.stderr)
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


def read_text(repo_root: Path, relative_path: str) -> str:
    reject_private_reference(relative_path, "source_path")
    path = repo_root / relative_path
    require(path.is_file(), f"source_missing:{relative_path}")
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        fail(f"utf8_decode_failed:{relative_path}:{exc}")
    except OSError as exc:
        fail(f"read_failed:{relative_path}:{exc}")


def validate_drill(repo_root: Path, drill: dict[str, Any]) -> dict[str, Any]:
    drill_id = drill["drill"]
    path_text = drill["path"]
    tokens = drill["tokens"]
    require(isinstance(drill_id, str) and drill_id, "drill_invalid")
    require(isinstance(path_text, str) and path_text, f"path_invalid:{drill_id}")
    require(isinstance(tokens, tuple) and tokens, f"tokens_invalid:{drill_id}")
    text = read_text(repo_root, path_text)
    token_digests: list[str] = []
    for token in tokens:
        require(isinstance(token, str) and token, f"token_invalid:{drill_id}")
        if token not in text:
            fail(f"token_missing:{drill_id}:{path_text}:{token}")
        token_digests.append(sha256_text(token))
    return {
        "drill": drill_id,
        "path": path_text,
        "token_count": len(tokens),
        "source_sha256": sha256_text(text),
        "token_digest_sha256": sha256_text("\n".join(token_digests) + "\n"),
        "status": "pass",
    }


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "drill",
                "path",
                "token_count",
                "source_sha256",
                "token_digest_sha256",
                "status",
            ],
        )
        writer.writeheader()
        writer.writerows(records)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--csv-output", required=True, type=Path)
    parser.add_argument("--evidence-output", required=True, type=Path)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    records = [validate_drill(repo_root, drill) for drill in DRILLS]
    matrix_text = "\n".join(
        f"{record['drill']},{record['path']},{record['status']}" for record in records
    ) + "\n"
    payload = {
        "gate": "PUBLIC_DISASTER_RECOVERY_GATE",
        "status": "pass",
        "drills": records,
        "drill_count": len(records),
        "matrix_sha256": sha256_text(matrix_text),
        "authority": {
            "transaction_finality": "local_mga_transaction_inventory",
            "authoritative_wal": False,
            "cluster_recovery_authority": False,
        },
    }
    write_csv(args.csv_output, records)
    write_json(args.evidence_output, payload)
    print(
        "public_disaster_recovery_gate=passed "
        f"drills={len(records)} matrix_sha256={payload['matrix_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
