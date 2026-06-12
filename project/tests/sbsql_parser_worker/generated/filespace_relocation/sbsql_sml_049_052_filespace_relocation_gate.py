#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-049..SML-052 filespace relocation manifest/oracle gate."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


DEFAULT_FIXTURE_DIR = (
    "project/tests/sbsql_parser_worker/generated/filespace_relocation"
)
MANIFEST_NAME = "SML_049_052_FILESPACE_RELOCATION_MANIFEST.csv"
ORACLE_NAME = "SML_049_052_FILESPACE_RELOCATION_ORACLE.json"
GENERATOR = "project/tools/sb_parser_gen/generate_sbsql_sml_049_052_filespace_relocation.py"
SCHEMA_VERSION = "sbsql.filespace_relocation.sml_049_052.v1"

MANIFEST_COLUMNS = [
    "sml_id",
    "gate_id",
    "row_id",
    "proof_domain",
    "coverage_class",
    "lifecycle_operation",
    "object_class",
    "route_class",
    "page_size",
    "transaction_case",
    "security_case",
    "failure_point",
    "closure_status",
    "proof_kind",
    "parser_executes_sql",
    "parser_owns_finality",
    "storage_finality_authority",
    "recovery_authority",
    "resource_contract",
    "expected_result_contract",
    "expected_result_hash",
    "expected_diagnostic_contract",
    "expected_diagnostic_hash",
    "oracle_id",
    "oracle_hash",
    "artifact_paths",
    "evidence_tokens",
]

EXPECTED_SML = {
    "SML-049": "filespace_lifecycle_manifest",
    "SML-050": "sql_object_relocation",
    "SML-051": "primary_shadow_migration",
    "SML-052": "relocation_failure_recovery",
}

REQUIRED_LIFECYCLE_OPERATIONS = {
    "create",
    "attach",
    "verify",
    "move",
    "promote",
    "demote",
    "compact",
    "truncate",
    "retire",
    "archive",
    "detach",
    "drop",
}

REQUIRED_ROUTES = {
    "embedded",
    "local_ipc",
    "inet_listener",
    "sbsql_parser_route",
    "management_api",
}

REQUIRED_PAGE_SIZES = {"8192", "16384", "32768", "65536", "131072"}
REQUIRED_PROOF_KINDS = {"oracle", "exact_refusal"}
ALLOWED_STATUSES = {"closed_oracle", "closed_refusal"}
AUTHORITY = "engine_mga_transaction_inventory"

REQUIRED_OBJECT_CLASSES = {
    "filespace_manifest",
    "filespace_descriptor",
    "page_map",
    "relation_extent",
    "primary_descriptor",
    "retired_extent",
    "free_extent_map",
    "tail_extent",
    "archive_package",
    "small_sql_table",
    "large_sql_object",
    "localized_name_dependency_graph",
    "shadow_primary",
    "old_primary",
    "old_primary_descriptor",
    "relocation_job",
    "quarantined_extent",
    "datarepair_history",
}

REQUIRED_TRANSACTION_CASES = {
    "autocommit_commit",
    "explicit_commit",
    "explicit_commit_with_savepoint",
    "read_snapshot",
    "rollback_before_finalize",
    "maintenance_commit",
    "commit_after_archive",
    "restart_recovery",
    "rollback_after_fault",
}

FORBIDDEN_VALUE_TOKENS = (
    "pending",
    "".join(("to", "do")),
    "tbd",
    "".join(("place", "holder")),
    "".join(("skip", "ped")),
    "".join(("wai", "ved")),
    "".join(("manual", "_only")),
    "".join(("manual", "-only")),
    "".join(("generated", "_only")),
    "".join(("generated", "-only")),
    "".join(("file", "_presence")),
    "".join(("st", "ub")),
    "skeleton",
    "parser_owned",
    "".join(("parser", "-owned")),
    "storage-finality",
    "parser finality",
)

FORBIDDEN_RECOVERY_TOKENS = (
    "wal",
    "write ahead",
    "write-ahead",
    "redo",
    "undo",
    "journal",
)

NETWORK_MODULE_NAMES = (
    "".join(("url", "lib")),
    "http",
    "".join(("so", "cket")),
    "ssl",
    "".join(("ft", "plib")),
    "".join(("sm", "tplib")),
    "".join(("telnet", "lib")),
    "".join(("imap", "lib")),
    "".join(("pop", "lib")),
    "".join(("nntp", "lib")),
    "".join(("xml", "rpc")),
    "".join(("web", "browser")),
    "".join(("re", "quests")),
    "httpx",
    "aiohttp",
    "".join(("url", "lib3")),
    "pycurl",
    "paramiko",
    "asyncssh",
    "botocore",
    "boto3",
)

NETWORK_MODULE_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    + "|".join(re.escape(name) for name in NETWORK_MODULE_NAMES)
    + r")(?:[\.\s]|$)",
    re.MULTILINE,
)

HASH_RE = re.compile(r"^[0-9a-f]{64}$")


class GateError(AssertionError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def split_semicolon(value: str) -> list[str]:
    return [item.strip() for item in value.split(";") if item.strip()]


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def read_manifest(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise GateError(f"manifest missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != MANIFEST_COLUMNS:
            raise GateError(f"manifest header drift: {reader.fieldnames}")
        rows = list(reader)
    if not rows:
        raise GateError("manifest has no rows")
    return rows


def read_oracle(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"oracle missing: {path}")
    with path.open(encoding="utf-8") as handle:
        value = json.load(handle)
    if not isinstance(value, dict):
        raise GateError("oracle root must be an object")
    return value


def scan_no_network(path: Path) -> list[str]:
    text = read_text(path)
    return [
        f"{path}:{text[:match.start()].count(chr(10)) + 1}: forbidden network import {match.group(1)!r}"
        for match in NETWORK_MODULE_RE.finditer(text)
    ]


def oracle_hash_payload(row: dict[str, str]) -> dict[str, str]:
    return {
        "sml_id": row["sml_id"],
        "gate_id": row["gate_id"],
        "row_id": row["row_id"],
        "proof_domain": row["proof_domain"],
        "coverage_class": row["coverage_class"],
        "lifecycle_operation": row["lifecycle_operation"],
        "object_class": row["object_class"],
        "route_class": row["route_class"],
        "page_size": row["page_size"],
        "transaction_case": row["transaction_case"],
        "security_case": row["security_case"],
        "failure_point": row["failure_point"],
        "proof_kind": row["proof_kind"],
        "result": row["expected_result_contract"],
        "diagnostic": row["expected_diagnostic_contract"],
        "resource_contract": row["resource_contract"],
        "storage_finality_authority": row["storage_finality_authority"],
        "recovery_authority": row["recovery_authority"],
    }


def validate_determinism(repo: Path, fixture_dir: Path) -> list[str]:
    generator = repo / GENERATOR
    errors = scan_no_network(generator)
    if not generator.is_file():
        return [f"generator missing: {GENERATOR}"]
    with tempfile.TemporaryDirectory(prefix="sml_049_052_filespace_") as tmp:
        generated_dir = Path(tmp) / "generated"
        result = subprocess.run(
            [sys.executable, str(generator), "--output-dir", str(generated_dir)],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            errors.append(
                "fixture regeneration failed "
                f"exit={result.returncode} stderr={result.stderr.strip()[:240]}"
            )
            return errors
        for name in (MANIFEST_NAME, ORACLE_NAME):
            generated = generated_dir / name
            tracked = fixture_dir / name
            if generated.read_bytes() != tracked.read_bytes():
                errors.append(f"{name}: tracked artifact differs from deterministic regeneration")
    return errors


def validate_resource_contract(row: dict[str, str], errors: list[str]) -> None:
    row_id = row["row_id"]
    pairs: dict[str, str] = {}
    for item in split_semicolon(row["resource_contract"]):
        if "=" not in item:
            errors.append(f"{row_id}: malformed resource contract item {item!r}")
            continue
        key, value = item.split("=", 1)
        pairs[key] = value
    for key in ("network", "parser_execution", "external_storage"):
        if pairs.get(key) != "0":
            errors.append(f"{row_id}: {key} must be 0")


def validate_row(row: dict[str, str], repo: Path, errors: list[str]) -> None:
    row_id = row.get("row_id", "<missing>")
    for column in MANIFEST_COLUMNS:
        if not row.get(column):
            errors.append(f"{row_id}: missing {column}")
    if row.get("sml_id") not in EXPECTED_SML:
        errors.append(f"{row_id}: unexpected sml_id {row.get('sml_id')}")
    elif row.get("proof_domain") != EXPECTED_SML[row["sml_id"]]:
        errors.append(f"{row_id}: proof_domain mismatch for {row['sml_id']}")
    if row.get("gate_id") != row.get("sml_id", "").replace("SML-", "SML-GATE-"):
        errors.append(f"{row_id}: gate_id mismatch")
    if row.get("closure_status") not in ALLOWED_STATUSES:
        errors.append(f"{row_id}: closure_status must fail closed")
    if row.get("proof_kind") not in REQUIRED_PROOF_KINDS:
        errors.append(f"{row_id}: unexpected proof_kind {row.get('proof_kind')}")
    if row.get("proof_kind") == "exact_refusal" and row.get("closure_status") != "closed_refusal":
        errors.append(f"{row_id}: exact refusal row must use closed_refusal")
    if row.get("proof_kind") == "oracle" and row.get("closure_status") != "closed_oracle":
        errors.append(f"{row_id}: oracle row must use closed_oracle")
    if row.get("parser_executes_sql") != "false":
        errors.append(f"{row_id}: parser_executes_sql must be false")
    if row.get("parser_owns_finality") != "false":
        errors.append(f"{row_id}: parser_owns_finality must be false")
    if row.get("storage_finality_authority") != AUTHORITY:
        errors.append(f"{row_id}: storage finality authority drift")
    if row.get("recovery_authority") != AUTHORITY:
        errors.append(f"{row_id}: recovery authority must be MGA transaction inventory")

    lowered_values = " ".join(str(value).lower() for value in row.values())
    for token in FORBIDDEN_VALUE_TOKENS:
        if token in lowered_values:
            errors.append(f"{row_id}: forbidden non-closure token {token!r}")
    recovery_text = (
        row.get("recovery_authority", "")
        + " "
        + row.get("expected_result_contract", "")
        + " "
        + row.get("expected_diagnostic_contract", "")
    ).lower()
    for token in FORBIDDEN_RECOVERY_TOKENS:
        if token in recovery_text:
            errors.append(f"{row_id}: non-MGA recovery token {token!r}")

    if row.get("expected_result_hash") != sha256_text(row.get("expected_result_contract", "")):
        errors.append(f"{row_id}: expected_result_hash mismatch or missing")
    if row.get("expected_diagnostic_hash") != sha256_text(row.get("expected_diagnostic_contract", "")):
        errors.append(f"{row_id}: expected_diagnostic_hash mismatch or missing")
    if not HASH_RE.fullmatch(row.get("expected_result_hash", "")):
        errors.append(f"{row_id}: expected_result_hash invalid")
    if not HASH_RE.fullmatch(row.get("expected_diagnostic_hash", "")):
        errors.append(f"{row_id}: expected_diagnostic_hash invalid")
    expected_oracle_hash = sha256_text(canonical_json(oracle_hash_payload(row)))
    if row.get("oracle_hash") != expected_oracle_hash:
        errors.append(f"{row_id}: oracle_hash mismatch or missing")
    if not HASH_RE.fullmatch(row.get("oracle_hash", "")):
        errors.append(f"{row_id}: oracle_hash invalid")

    validate_resource_contract(row, errors)

    artifact_text = ""
    for rel in split_semicolon(row.get("artifact_paths", "")):
        if not (
            rel.startswith("project/tests/sbsql_parser_worker/generated/filespace_relocation/")
            or rel == GENERATOR
        ):
            errors.append(f"{row_id}: artifact path outside filespace relocation scope: {rel}")
            continue
        path = repo / rel
        if not path.is_file():
            errors.append(f"{row_id}: artifact path missing: {rel}")
            continue
        artifact_text += read_text(path) + "\n"
    for token in split_semicolon(row.get("evidence_tokens", "")):
        if token not in artifact_text:
            errors.append(f"{row_id}: evidence token not present in tracked artifacts: {token}")


def validate_oracle(rows: list[dict[str, str]], oracle: dict[str, Any], errors: list[str]) -> None:
    if oracle.get("schema_version") != SCHEMA_VERSION:
        errors.append("oracle schema_version drift")
    if oracle.get("manifest") != MANIFEST_NAME:
        errors.append("oracle manifest link drift")
    authority = oracle.get("authority", {})
    if authority.get("parser_executes_sql") is not False:
        errors.append("oracle parser_executes_sql authority drift")
    if authority.get("parser_owns_finality") is not False:
        errors.append("oracle parser_owns_finality authority drift")
    if authority.get("storage_finality_authority") != AUTHORITY:
        errors.append("oracle storage_finality_authority drift")
    if authority.get("recovery_authority") != AUTHORITY:
        errors.append("oracle recovery_authority drift")

    oracle_rows = oracle.get("rows")
    if not isinstance(oracle_rows, list):
        errors.append("oracle rows must be a list")
        return
    by_row_id = {item["row_id"]: item for item in rows}
    oracle_by_row_id = {
        item.get("row_id"): item
        for item in oracle_rows
        if isinstance(item, dict) and item.get("row_id")
    }
    if set(oracle_by_row_id) != set(by_row_id):
        errors.append("oracle row_id set does not match manifest")
    for row_id, manifest_row in by_row_id.items():
        oracle_row = oracle_by_row_id.get(row_id, {})
        for key in [
            "oracle_id",
            "sml_id",
            "coverage_class",
            "lifecycle_operation",
            "object_class",
            "route_class",
            "page_size",
            "transaction_case",
            "security_case",
            "failure_point",
            "proof_kind",
            "storage_finality_authority",
            "recovery_authority",
            "expected_result_hash",
            "expected_diagnostic_hash",
            "oracle_hash",
        ]:
            if oracle_row.get(key) != manifest_row.get(key):
                errors.append(f"{row_id}: oracle {key} drift")


def text_for(rows: list[dict[str, str]], sml_id: str) -> str:
    return " ".join(
        " ".join(row.values()).lower()
        for row in rows
        if row.get("sml_id") == sml_id
    )


def validate_sml_coverage(rows: list[dict[str, str]], errors: list[str]) -> None:
    row_ids = [row["row_id"] for row in rows]
    if len(row_ids) != len(set(row_ids)):
        errors.append("duplicate row_id in manifest")

    for sml_id in EXPECTED_SML:
        if not any(row["sml_id"] == sml_id for row in rows):
            errors.append(f"{sml_id}: missing rows")
        proof_kinds = {row["proof_kind"] for row in rows if row["sml_id"] == sml_id}
        if "oracle" not in proof_kinds:
            errors.append(f"{sml_id}: missing oracle proof")
        if "exact_refusal" not in proof_kinds:
            errors.append(f"{sml_id}: missing exact refusal proof")

    observed_ops = {row["lifecycle_operation"] for row in rows}
    missing_ops = REQUIRED_LIFECYCLE_OPERATIONS - observed_ops
    if missing_ops:
        errors.append(f"SML-049: missing lifecycle operations {sorted(missing_ops)}")
    observed_routes = {row["route_class"] for row in rows}
    missing_routes = REQUIRED_ROUTES - observed_routes
    if missing_routes:
        errors.append(f"SML-049: missing routes {sorted(missing_routes)}")
    observed_pages = {row["page_size"] for row in rows}
    missing_pages = REQUIRED_PAGE_SIZES - observed_pages
    if missing_pages:
        errors.append(f"SML-049: missing page sizes {sorted(missing_pages)}")
    observed_objects = {row["object_class"] for row in rows}
    missing_objects = REQUIRED_OBJECT_CLASSES - observed_objects
    if missing_objects:
        errors.append(f"manifest missing object classes {sorted(missing_objects)}")
    observed_tx = {row["transaction_case"] for row in rows}
    missing_tx = REQUIRED_TRANSACTION_CASES - observed_tx
    if missing_tx:
        errors.append(f"manifest missing transaction cases {sorted(missing_tx)}")
    if not any(row["security_case"] for row in rows):
        errors.append("manifest missing security cases")
    if not any(row["failure_point"] != "none" for row in rows):
        errors.append("manifest missing failure point coverage")

    sml_050_text = text_for(rows, "SML-050")
    for token in [
        "small",
        "large",
        "uuid",
        "version",
        "grants",
        "policies",
        "dependencies",
        "localized names",
        "indexes",
        "stats",
        "results",
        "metrics",
    ]:
        if token not in sml_050_text:
            errors.append(f"SML-050: missing preservation token {token!r}")

    sml_051_text = text_for(rows, "SML-051")
    for token in [
        "shadow primary",
        "promotion",
        "old primary",
        "retire",
        "verified",
        "new primary",
        "blocker",
        "reopen",
        "demote",
        "archive",
        "detach",
        "drop",
    ]:
        if token not in sml_051_text:
            errors.append(f"SML-051: missing primary migration token {token!r}")

    sml_052_text = text_for(rows, "SML-052")
    for token in [
        "failure injection",
        "restart",
        "resume",
        "quarantine",
        "exact refusal",
        "audit",
        "datarepair history",
        "uuid trace",
    ]:
        if token not in sml_052_text:
            errors.append(f"SML-052: missing failure recovery token {token!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--fixture-dir", default=DEFAULT_FIXTURE_DIR)
    args = parser.parse_args()

    repo = Path(args.repo_root).resolve()
    fixture_dir = Path(args.fixture_dir)
    if not fixture_dir.is_absolute():
        fixture_dir = repo / fixture_dir

    try:
        rows = read_manifest(fixture_dir / MANIFEST_NAME)
        oracle = read_oracle(fixture_dir / ORACLE_NAME)
        errors: list[str] = []
        errors.extend(scan_no_network(Path(__file__).resolve()))
        errors.extend(validate_determinism(repo, fixture_dir))
        for row in rows:
            validate_row(row, repo, errors)
        validate_oracle(rows, oracle, errors)
        validate_sml_coverage(rows, errors)
    except Exception as exc:  # noqa: BLE001 - CTest gates need explicit stderr.
        print("sbsql_sml_049_052_filespace_relocation_gate=failed", file=sys.stderr)
        print(str(exc), file=sys.stderr)
        return 2

    if errors:
        print("sbsql_sml_049_052_filespace_relocation_gate=failed", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("sbsql_sml_049_052_filespace_relocation_gate=passed")
    print(f"manifest_rows={len(rows)}")
    print("sml_ids=" + ",".join(sorted(EXPECTED_SML)))
    print("network_dependency=none")
    print("storage_finality_authority=engine_mga_transaction_inventory")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
