#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate public reference-replay policy matrices.

These matrices are public test contracts, not private workplans. They describe
the checks every original-tool replay lane must satisfy before release while
keeping acquired upstream payloads, private reports, and private specifications
out of the public repository.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import platform
import resource
import shutil
import socket
import sys
from pathlib import Path
from typing import Any


REFERENCE_ROOT = Path("project/tests/reference_regression")

MATRICES: dict[str, dict[str, Any]] = {
    "database-contamination": {
        "path": REFERENCE_ROOT / "policy/reference_database_contamination_rules.csv",
        "required_columns": {
            "rule_id",
            "status",
            "rule",
            "required_check",
            "blocker_if_failed",
            "evidence_key",
        },
        "min_rows": 10,
        "labels": ["reference_replay_database_contamination_gate"],
    },
    "tool-adapter-contract": {
        "path": REFERENCE_ROOT / "policy/reference_original_tool_adapter_contract.csv",
        "required_columns": {
            "contract_id",
            "status",
            "family_scope",
            "endpoint_input",
            "auth_input",
            "tls_input",
            "database_input",
            "output_capture",
            "result_normalizer",
            "authority_rule",
        },
        "min_rows": 8,
        "labels": ["reference_original_tool_adapter_contract_gate"],
    },
    "security-authorization": {
        "path": REFERENCE_ROOT / "security/reference_security_authorization_replay_matrix.csv",
        "required_columns": {
            "security_id",
            "status",
            "case",
            "actor",
            "expected_result",
            "proof_gate",
            "authority_rule",
        },
        "min_rows": 12,
        "labels": ["reference_security_authorization_replay_gate"],
    },
    "result-canonicalization": {
        "path": REFERENCE_ROOT / "result_normalizer/reference_result_canonicalization_policy.csv",
        "required_columns": {
            "canon_id",
            "status",
            "output_area",
            "allowed_normalization",
            "forbidden_normalization",
            "proof",
        },
        "min_rows": 7,
        "labels": ["reference_result_canonicalization_gate"],
    },
    "retry-quarantine": {
        "path": REFERENCE_ROOT / "result_normalizer/reference_retry_quarantine_policy.csv",
        "required_columns": {
            "policy_id",
            "status",
            "case_state",
            "retry_limit",
            "classification_after_limit",
            "required_evidence",
            "release_effect",
        },
        "min_rows": 7,
        "labels": ["reference_retry_quarantine_gate"],
    },
    "resource-timeout": {
        "path": REFERENCE_ROOT / "native_tool_runner/reference_resource_timeout_policy.csv",
        "required_columns": {
            "policy_id",
            "status",
            "scope",
            "default_value",
            "required_evidence",
            "notes",
        },
        "min_rows": 10,
        "labels": ["reference_resource_timeout_policy_gate"],
    },
    "failure-ledger-schema": {
        "path": REFERENCE_ROOT / "failure_ledger/reference_failure_ledger_schema.csv",
        "required_columns": {
            "field_name",
            "required",
            "type",
            "meaning",
        },
        "min_rows": 18,
        "labels": ["reference_failure_ledger_schema_gate"],
    },
    "example-source-ledger": {
        "path": REFERENCE_ROOT / "example_database/reference_example_database_source_ledger.csv",
        "required_columns": {
            "source_id",
            "status",
            "reference_family",
            "source_kind",
            "source_identifier",
            "license_status",
            "redistribution_status",
            "transformation_policy",
            "target_namespace",
            "object_manifest",
            "checksum_policy",
            "notes",
        },
        "min_rows": 2,
        "labels": ["reference_example_database_source_ledger_gate"],
    },
    "example-database-matrix": {
        "path": REFERENCE_ROOT / "example_database/reference_example_database_matrix.csv",
        "required_columns": {
            "example_id",
            "status",
            "reference_family",
            "source_policy",
            "target_namespace",
            "redistribution_policy",
            "verification_gate",
            "notes",
        },
        "min_rows": 25,
        "labels": ["reference_example_source_map_gate"],
    },
    "reproduction-transcript": {
        "path": REFERENCE_ROOT / "reproduction/reference_reproduction_command_transcript_targets.csv",
        "required_columns": {
            "target_id",
            "status",
            "target",
            "command_shape",
            "required_outputs",
            "notes",
        },
        "min_rows": 7,
        "labels": ["reference_reproduction_command_transcript_gate"],
    },
    "secret-redaction": {
        "path": REFERENCE_ROOT / "policy/reference_secret_redaction_artifact_policy.csv",
        "required_columns": {
            "redaction_id",
            "status",
            "artifact_area",
            "forbidden_content",
            "required_check",
            "notes",
        },
        "min_rows": 5,
        "labels": ["reference_secret_redaction_artifact_gate"],
    },
    "platform-variance": {
        "path": REFERENCE_ROOT / "platform/reference_platform_toolchain_variance_matrix.csv",
        "required_columns": {
            "variance_id",
            "status",
            "dimension",
            "required_capture",
            "impact",
        },
        "min_rows": 8,
        "labels": ["reference_platform_toolchain_variance_gate"],
    },
}

FORBIDDEN_TEXT = ("future", "defer", "deferred", "stub", "todo", "tbd", "fixme", "pending")
VALID_STATUS = {
    "required",
    "active",
    "enforced",
    "ready_for_replay",
    "schema_enforced",
    "public_safe",
    "local_only",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


def read_csv(path: Path, required_columns: set[str]) -> list[dict[str, str]]:
    require(path.is_file(), f"missing matrix: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        require(reader.fieldnames is not None, f"{path}: missing CSV header")
        missing = sorted(required_columns - set(reader.fieldnames))
        require(not missing, f"{path}: missing columns {missing}")
        rows = [dict(row) for row in reader]
    require(rows, f"{path}: no rows")
    return rows


def split_semicolon(value: str) -> list[str]:
    return [item.strip() for item in value.split(";") if item.strip()]


def validate_no_forbidden(path: Path, rows: list[dict[str, str]]) -> None:
    for row_number, row in enumerate(rows, start=2):
        lowered = " ".join(row.values()).lower()
        for token in FORBIDDEN_TEXT:
            require(token not in lowered, f"{path}:{row_number}: forbidden release token {token!r}")


def validate_status(path: Path, rows: list[dict[str, str]]) -> None:
    if "status" not in rows[0]:
        return
    for row_number, row in enumerate(rows, start=2):
        status = row.get("status", "")
        require(status in VALID_STATUS, f"{path}:{row_number}: invalid public status {status!r}")


def validate_matrix(repo_root: Path, name: str) -> dict[str, Any]:
    spec = MATRICES[name]
    path = repo_root / spec["path"]
    rows = read_csv(path, set(spec["required_columns"]))
    require(len(rows) >= int(spec["min_rows"]), f"{path}: expected at least {spec['min_rows']} rows")
    validate_no_forbidden(path, rows)
    validate_status(path, rows)

    if name == "security-authorization":
        expected = {row["expected_result"] for row in rows}
        require("success" in expected, "security matrix must include success cases")
        require(any("refused" in item for item in expected), "security matrix must include refusal cases")
        require(any("injection" in " ".join(row.values()).lower() for row in rows),
                "security matrix must include SBLR/UUID injection refusal")
    if name == "tool-adapter-contract":
        scopes = {row["family_scope"] for row in rows}
        require("all" in scopes, "tool adapter contract must include all-family base row")
        require("firebird" in scopes, "tool adapter contract must include Firebird")
    if name == "failure-ledger-schema":
        fields = {row["field_name"] for row in rows}
        required = {
            "case_id",
            "family_id",
            "tool_name",
            "input_digest",
            "actual_digest",
            "classification",
            "owner",
            "rerun_status",
            "evidence_path",
        }
        require(required <= fields, f"failure ledger schema missing {sorted(required - fields)}")
    if name == "example-database-matrix":
        families = {row["reference_family"] for row in rows}
        require({"all", "firebird", "postgresql", "mysql"} <= families,
                "example database matrix missing required core families")
    if name == "platform-variance":
        dimensions = {row["dimension"] for row in rows}
        require({"host_os", "tls", "locale", "toolchain"} <= dimensions,
                "platform variance matrix missing required dimensions")
    if name == "resource-timeout":
        scopes = {row["scope"] for row in rows}
        require({"standard_suite_timeout", "process_cleanup"} <= scopes,
                "resource timeout policy missing core scopes")
    if name == "result-canonicalization":
        areas = {row["output_area"] for row in rows}
        require({"whitespace", "row_order", "diagnostics"} <= areas,
                "canonicalization policy missing core output areas")

    digest = hashlib.sha256(
        json.dumps(rows, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return {
        "matrix": name,
        "path": spec["path"].as_posix(),
        "row_count": len(rows),
        "digest": digest,
        "labels": spec["labels"],
    }


def capture_platform_envelope() -> dict[str, Any]:
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    return {
        "host_os": platform.platform(),
        "machine": platform.machine(),
        "python": sys.version.split()[0],
        "shell": os.environ.get("SHELL", "unknown"),
        "lang": os.environ.get("LANG", ""),
        "lc_all": os.environ.get("LC_ALL", ""),
        "timezone": os.environ.get("TZ", ""),
        "filesystem_case_probe": "case_sensitive_expected_on_linux",
        "tls_probe": "captured_by_per-family replay tool wrapper",
        "loopback_probe": socket.gethostbyname("localhost"),
        "disk_free_bytes": shutil.disk_usage(Path.cwd()).free,
        "rlimit_nofile": {"soft": soft, "hard": hard},
    }


def write_evidence(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--matrix", choices=sorted(MATRICES), required=True)
    parser.add_argument("--evidence-file", type=Path)
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    result = validate_matrix(repo_root, args.matrix)
    payload = {
        "schema_version": "scratchbird_reference_replay_policy_matrix_gate_v1",
        "gate": "reference_replay_policy_matrix_gate",
        "status": "passed",
        "validated": result,
        "platform_envelope": capture_platform_envelope()
        if args.matrix == "platform-variance"
        else None,
        "authority_policy": "policy_matrices_define_replay_checks_only_engine_mga_security_storage_authority_remains_authoritative",
    }
    if args.evidence_file:
        write_evidence(args.evidence_file, payload)
    print(
        "reference_replay_policy_matrix_gate=passed "
        f"matrix={args.matrix} rows={result['row_count']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
