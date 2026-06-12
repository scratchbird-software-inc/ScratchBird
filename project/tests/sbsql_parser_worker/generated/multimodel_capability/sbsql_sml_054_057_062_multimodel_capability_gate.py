#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-054/055/056/057/062 multimodel capability proof gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "sbsql.multimodel_capability.v1"
GATE_ID = "SML-GATE-054-057-062"
MANIFEST_NAME = "SML_054_057_062_MULTIMODEL_CAPABILITY_MANIFEST.json"
ORACLE_NAME = "SML_054_057_062_MULTIMODEL_CAPABILITY_ORACLE.jsonl"
REQUIRED_SML_IDS = {"SML-054", "SML-055", "SML-056", "SML-057", "SML-062"}
REQUIRED_ROUTES = {"embedded", "inet_listener", "local_ipc", "parser_route"}
LEGAL_PAGE_SIZES = {8192, 16384, 32768, 65536, 131072}
REQUIRED_CAPABILITY_FAMILIES = {
    "document",
    "key_value",
    "wide_column",
    "graph",
    "vector",
    "text_search",
    "time_series",
    "hybrid",
    "sql_nosql_join",
}
REQUIRED_SML054_CLASSES = {
    "manifest_script",
    "route_page_language",
    "transaction_security_diagnostics_recovery",
    "refusal",
}
REQUIRED_SML055_CLASSES = {
    "lifecycle",
    "json_path",
    "xml_path",
    "mutation",
    "keyspace",
    "conditional_mutation",
    "range_scan",
    "batch",
    "ttl_policy",
}
REQUIRED_SML056_CLASSES = {
    "graph_search",
    "graph_traversal",
    "graph_mutation",
    "vector_exact_search",
    "vector_approximate_search",
    "hybrid_search",
    "text_search",
    "time_series",
    "relational_multimodel_uuid",
}
REQUIRED_SML057_CLASSES = {
    "reference_profile_storage_map",
    "reference_profile_security_map",
    "reference_profile_recovery_map",
    "reference_profile_fail_closed_storage",
    "reference_profile_fail_closed_finality",
}
ALLOWED_STATUSES = {"complete", "closed_manifest_oracle", "closed_refusal"}
ALLOWED_PROOF_KINDS = {"manifest_oracle", "refusal_oracle"}
ENGINE_STORAGE_AUTHORITY = "scratchbird_engine_mga_pages"
ENGINE_FINALITY_AUTHORITY = "scratchbird_mga_transaction_inventory"
ENGINE_RECOVERY_AUTHORITY = "scratchbird_mga_recovery"
FAIL_CLOSED_AUTHORITY = "not_reached_fail_closed"
ALLOWED_REFERENCE_PROFILE_BEHAVIOR = {
    "not_reference_profile",
    "maps_to_scratchbird_behavior",
    "fail_closed_no_reference_owned_storage",
    "fail_closed_no_reference_owned_finality",
}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
BLOCKED_WORDS = (
    "".join(("to", "do")),
    "tbd",
    "".join(("place", "holder")),
    "".join(("st", "ub")),
    "mock",
    "".join(("fa", "ke")),
    "".join(("pend", "ing")),
    "".join(("skip", "ped")),
    "".join(("wai", "ved")),
    "".join(("generated", "-only")),
    "".join(("generated", "_only")),
    "unknown",
    "n/a",
)
BLOCKED_RE = re.compile(
    r"\b(?:" + "|".join(re.escape(word) for word in BLOCKED_WORDS) + r")\b",
    re.IGNORECASE,
)
FORBIDDEN_LOCATORS = (
    "".join(("http", "://")),
    "".join(("https", "://")),
    "".join(("ftp", "://")),
    "".join(("s3", "://")),
    "".join(("gs", "://")),
)
NETWORK_MODULE_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    r"urllib|http|socket|ssl|ftplib|smtplib|telnetlib|imaplib|poplib|"
    r"nntplib|xmlrpc|webbrowser|requests|httpx|aiohttp|urllib3|pycurl|"
    r"paramiko|asyncssh|botocore|boto3|google.cloud|google.api_core"
    r")(?:[.\s]|$)",
    re.MULTILINE,
)


class GateError(AssertionError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":"))


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def stable_hash(value: Any) -> str:
    return sha256_text(canonical_json(value))


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"manifest missing: {path}")
    with path.open(encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise GateError("manifest root must be a JSON object")
    return payload


def load_oracle(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        raise GateError(f"oracle missing: {path}")
    records: list[dict[str, Any]] = []
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            stripped = line.strip()
            if not stripped:
                raise GateError(f"blank oracle line {line_number}")
            record = json.loads(stripped)
            if not isinstance(record, dict):
                raise GateError(f"oracle line {line_number} must be a JSON object")
            records.append(record)
    return records


def walk_strings(value: Any) -> list[str]:
    if isinstance(value, dict):
        result: list[str] = []
        for item in value.values():
            result.extend(walk_strings(item))
        return result
    if isinstance(value, list):
        result = []
        for item in value:
            result.extend(walk_strings(item))
        return result
    if isinstance(value, str):
        return [value]
    return []


def scan_payload_text(value: Any, label: str, errors: list[str]) -> None:
    for text in walk_strings(value):
        lowered = text.lower()
        for locator in FORBIDDEN_LOCATORS:
            require(locator not in lowered, f"{label} contains external locator {locator}", errors)
        require(not BLOCKED_RE.search(text), f"{label} contains blocked text {text!r}", errors)


def scan_no_network(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [
        f"{path}:{text[:match.start()].count(chr(10)) + 1}: forbidden network import {match.group(1)}"
        for match in NETWORK_MODULE_RE.finditer(text)
    ]


def require_sha(value: Any, context: str, errors: list[str]) -> None:
    require(isinstance(value, str) and bool(SHA256_RE.match(value)),
            f"{context} must be a lowercase sha256 hex digest", errors)


def validate_hashes(row: dict[str, Any], oracle: dict[str, Any], errors: list[str]) -> None:
    row_id = str(row.get("row_id", ""))
    for field in (
        "script_sha256",
        "expected_result_sha256",
        "expected_diagnostic_sha256",
        "oracle_sha256",
        "row_sha256",
    ):
        require_sha(row.get(field), f"{row_id}.{field}", errors)
    require_sha(oracle.get("oracle_sha256"), f"{row_id}.oracle.oracle_sha256", errors)

    script_text = row.get("script_text", "")
    require(isinstance(script_text, str) and bool(script_text.strip()),
            f"{row_id} missing script_text", errors)
    if isinstance(script_text, str):
        require(row.get("script_sha256") == sha256_text(script_text),
                f"{row_id} script hash mismatch", errors)

    result_contract = oracle.get("expected_result_contract")
    diagnostic_contract = oracle.get("expected_diagnostic_contract")
    require(isinstance(result_contract, dict), f"{row_id} oracle result contract missing", errors)
    require(isinstance(diagnostic_contract, dict), f"{row_id} oracle diagnostic contract missing", errors)
    if isinstance(result_contract, dict):
        require(row.get("expected_result_sha256") == stable_hash(result_contract),
                f"{row_id} result contract hash mismatch", errors)
    if isinstance(diagnostic_contract, dict):
        require(row.get("expected_diagnostic_sha256") == stable_hash(diagnostic_contract),
                f"{row_id} diagnostic contract hash mismatch", errors)

    oracle_projection = {key: value for key, value in oracle.items() if key != "oracle_sha256"}
    require(oracle.get("oracle_sha256") == stable_hash(oracle_projection),
            f"{row_id} oracle record hash mismatch", errors)
    require(row.get("oracle_sha256") == oracle.get("oracle_sha256"),
            f"{row_id} row/oracle hash mismatch", errors)

    row_projection = {key: value for key, value in row.items() if key != "row_sha256"}
    require(row.get("row_sha256") == stable_hash(row_projection),
            f"{row_id} row hash mismatch", errors)


def validate_authority(row: dict[str, Any], oracle: dict[str, Any], errors: list[str]) -> None:
    row_id = str(row.get("row_id", ""))
    expected_result = "unknown"
    result_contract = oracle.get("expected_result_contract", {})
    if isinstance(result_contract, dict):
        expected_result = str(result_contract.get("expected_result", ""))

    require(row.get("network_required") is False, f"{row_id} must be no-network", errors)
    require(row.get("external_storage_allowed") is False,
            f"{row_id} must forbid external storage", errors)
    require(row.get("external_finality_allowed") is False,
            f"{row_id} must forbid external finality", errors)
    require(row.get("parser_executes_sql") is False,
            f"{row_id} parser must not execute SQL", errors)

    if expected_result == "refused":
        require(row.get("status") == "closed_refusal", f"{row_id} refusal status mismatch", errors)
        require(row.get("proof_kind") == "refusal_oracle", f"{row_id} refusal proof kind mismatch", errors)
        require(row.get("storage_authority") == FAIL_CLOSED_AUTHORITY,
                f"{row_id} refusal must not reach storage", errors)
        require(row.get("finality_authority") == FAIL_CLOSED_AUTHORITY,
                f"{row_id} refusal must not reach finality", errors)
        require(row.get("recovery_authority") == FAIL_CLOSED_AUTHORITY,
                f"{row_id} refusal must not reach recovery", errors)
    else:
        require(row.get("status") == "closed_manifest_oracle",
                f"{row_id} admitted row status mismatch", errors)
        require(row.get("proof_kind") == "manifest_oracle",
                f"{row_id} admitted row proof kind mismatch", errors)
        require(row.get("storage_authority") == ENGINE_STORAGE_AUTHORITY,
                f"{row_id} storage authority drift", errors)
        require(row.get("finality_authority") == ENGINE_FINALITY_AUTHORITY,
                f"{row_id} finality authority drift", errors)
        require(row.get("recovery_authority") == ENGINE_RECOVERY_AUTHORITY,
                f"{row_id} recovery authority drift", errors)

    require(row.get("reference_profile_behavior") in ALLOWED_REFERENCE_PROFILE_BEHAVIOR,
            f"{row_id} invalid reference profile behavior", errors)
    if "SML-057" in set(row.get("sml_ids", [])):
        require(row.get("reference_profile_behavior") != "not_reference_profile",
                f"{row_id} SML-057 row must declare reference profile behavior", errors)


def validate_contract_flags(row: dict[str, Any], oracle: dict[str, Any], errors: list[str]) -> None:
    row_id = str(row.get("row_id", ""))
    for field in (
        "uuid_identity_preserved",
        "security_context_preserved",
        "result_contract_preserved",
        "diagnostic_contract_preserved",
    ):
        require(row.get(field) is True, f"{row_id} missing {field}", errors)
    result_contract = oracle.get("expected_result_contract", {})
    if isinstance(result_contract, dict):
        for field in (
            "uuid_identity_preserved",
            "security_context_preserved",
            "result_contract_preserved",
            "diagnostic_contract_preserved",
        ):
            require(result_contract.get(field) is True,
                    f"{row_id} oracle missing {field}", errors)


def validate_rows(manifest: dict[str, Any], oracle_records: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    rows = manifest.get("rows")
    require(isinstance(rows, list) and bool(rows), "manifest rows must be non-empty", errors)
    if not isinstance(rows, list):
        return errors

    oracle_by_id: dict[str, dict[str, Any]] = {}
    for record in oracle_records:
        oracle_id = record.get("oracle_id")
        require(isinstance(oracle_id, str) and bool(oracle_id), "oracle_id missing", errors)
        if isinstance(oracle_id, str):
            require(oracle_id not in oracle_by_id, f"duplicate oracle_id {oracle_id}", errors)
            oracle_by_id[oracle_id] = record

    row_ids: set[str] = set()
    covered_smls: set[str] = set()
    covered_families: set[str] = set()
    classes_by_sml: dict[str, set[str]] = {sml_id: set() for sml_id in REQUIRED_SML_IDS}
    family_manifest_script: set[str] = set()

    for row in rows:
        require(isinstance(row, dict), "manifest rows must be objects", errors)
        if not isinstance(row, dict):
            continue
        row_id = row.get("row_id")
        require(isinstance(row_id, str) and bool(row_id), "row_id missing", errors)
        if not isinstance(row_id, str):
            continue
        require(row_id not in row_ids, f"duplicate row_id {row_id}", errors)
        row_ids.add(row_id)
        oracle = oracle_by_id.get(row_id)
        require(oracle is not None, f"{row_id} missing oracle record", errors)
        if oracle is None:
            continue

        require(row.get("status") in ALLOWED_STATUSES, f"{row_id} invalid status", errors)
        require(row.get("proof_kind") in ALLOWED_PROOF_KINDS, f"{row_id} invalid proof kind", errors)
        require(set(row.get("routes", [])) == REQUIRED_ROUTES,
                f"{row_id} route coverage mismatch", errors)
        require(set(row.get("page_sizes", [])) == LEGAL_PAGE_SIZES,
                f"{row_id} legal page size coverage mismatch", errors)
        require(row.get("language_profile") == "sbsql_multimodel_v1",
                f"{row_id} language profile drift", errors)
        require(row.get("transaction_profile") == ENGINE_FINALITY_AUTHORITY,
                f"{row_id} transaction profile drift", errors)
        require(row.get("security_profile") == "server_security_policy_revalidated",
                f"{row_id} security profile drift", errors)
        require(row.get("diagnostic_profile") == "canonical_message_vector_set",
                f"{row_id} diagnostic profile drift", errors)

        sml_ids = set(row.get("sml_ids", []))
        require(bool(sml_ids), f"{row_id} missing SML ids", errors)
        require(sml_ids <= REQUIRED_SML_IDS, f"{row_id} unknown SML ids {sorted(sml_ids - REQUIRED_SML_IDS)}", errors)
        covered_smls.update(sml_ids)
        coverage_class = row.get("coverage_class")
        for sml_id in sml_ids:
            if isinstance(coverage_class, str):
                classes_by_sml[sml_id].add(coverage_class)
        capability_family = row.get("capability_family")
        if isinstance(capability_family, str):
            covered_families.add(capability_family)
            if coverage_class == "manifest_script":
                family_manifest_script.add(capability_family)

        validate_hashes(row, oracle, errors)
        validate_authority(row, oracle, errors)
        validate_contract_flags(row, oracle, errors)

    require(set(oracle_by_id) == row_ids,
            f"oracle/row id mismatch rows={len(row_ids)} oracle={len(oracle_by_id)}", errors)
    require(REQUIRED_SML_IDS <= covered_smls,
            f"missing SML coverage {sorted(REQUIRED_SML_IDS - covered_smls)}", errors)
    require(REQUIRED_CAPABILITY_FAMILIES <= covered_families,
            f"missing capability families {sorted(REQUIRED_CAPABILITY_FAMILIES - covered_families)}",
            errors)
    require(REQUIRED_CAPABILITY_FAMILIES <= family_manifest_script,
            f"missing manifest/script rows for {sorted(REQUIRED_CAPABILITY_FAMILIES - family_manifest_script)}",
            errors)
    require(REQUIRED_SML054_CLASSES <= classes_by_sml["SML-054"],
            f"SML-054 missing classes {sorted(REQUIRED_SML054_CLASSES - classes_by_sml['SML-054'])}",
            errors)
    require(REQUIRED_SML054_CLASSES <= classes_by_sml["SML-062"],
            f"SML-062 missing classes {sorted(REQUIRED_SML054_CLASSES - classes_by_sml['SML-062'])}",
            errors)
    require(REQUIRED_SML055_CLASSES <= classes_by_sml["SML-055"],
            f"SML-055 missing classes {sorted(REQUIRED_SML055_CLASSES - classes_by_sml['SML-055'])}",
            errors)
    require(REQUIRED_SML056_CLASSES <= classes_by_sml["SML-056"],
            f"SML-056 missing classes {sorted(REQUIRED_SML056_CLASSES - classes_by_sml['SML-056'])}",
            errors)
    require(REQUIRED_SML057_CLASSES <= classes_by_sml["SML-057"],
            f"SML-057 missing classes {sorted(REQUIRED_SML057_CLASSES - classes_by_sml['SML-057'])}",
            errors)
    return errors


def validate_manifest_and_oracle(
    manifest: dict[str, Any],
    oracle_records: list[dict[str, Any]],
    manifest_path: Path,
    oracle_path: Path,
    generator_path: Path,
    gate_path: Path,
) -> list[str]:
    errors: list[str] = []
    require(manifest.get("schema_version") == SCHEMA_VERSION, "schema_version drift", errors)
    require(manifest.get("gate_id") == GATE_ID, "gate_id drift", errors)
    require(manifest.get("status") == "complete", "manifest status must be complete", errors)
    require(set(manifest.get("required_sml_ids", [])) == REQUIRED_SML_IDS,
            "manifest required_sml_ids drift", errors)
    require(set(manifest.get("required_routes", [])) == REQUIRED_ROUTES,
            "manifest required_routes drift", errors)
    require(set(manifest.get("required_page_sizes", [])) == LEGAL_PAGE_SIZES,
            "manifest required_page_sizes drift", errors)

    generator = manifest.get("generator", {})
    require(isinstance(generator, dict), "generator metadata missing", errors)
    if isinstance(generator, dict):
        require(generator.get("network_required") is False,
                "generator must declare no-network operation", errors)
        require(generator.get("kind") == "deterministic_generated_fixture",
                "generator kind drift", errors)
        require(generator.get("output_files") == [MANIFEST_NAME, ORACLE_NAME],
                "generator output file list drift", errors)

    authority = manifest.get("authority_contract", {})
    require(isinstance(authority, dict), "authority contract missing", errors)
    if isinstance(authority, dict):
        require(authority.get("storage_authority") == ENGINE_STORAGE_AUTHORITY,
                "manifest storage authority drift", errors)
        require(authority.get("finality_authority") == ENGINE_FINALITY_AUTHORITY,
                "manifest finality authority drift", errors)
        require(authority.get("recovery_authority") == ENGINE_RECOVERY_AUTHORITY,
                "manifest recovery authority drift", errors)
        require(authority.get("parser_executes_sql") is False,
                "manifest parser execution authority drift", errors)
        require(authority.get("external_storage_allowed") is False,
                "manifest external storage flag drift", errors)
        require(authority.get("external_finality_allowed") is False,
                "manifest external finality flag drift", errors)

    scan_payload_text(manifest, "manifest", errors)
    for record in oracle_records:
        scan_payload_text(record, "oracle", errors)

    require(manifest.get("oracle_file") == ORACLE_NAME, "oracle file name drift", errors)
    require_sha(manifest.get("oracle_file_sha256"), "oracle_file_sha256", errors)
    require(manifest.get("oracle_file_sha256") == sha256_bytes(oracle_path.read_bytes()),
            "oracle file hash mismatch", errors)
    require_sha(manifest.get("manifest_sha256"), "manifest_sha256", errors)
    manifest_projection = {key: value for key, value in manifest.items() if key != "manifest_sha256"}
    require(manifest.get("manifest_sha256") == stable_hash(manifest_projection),
            "manifest self hash mismatch", errors)

    errors.extend(validate_rows(manifest, oracle_records))
    errors.extend(scan_no_network(generator_path))
    errors.extend(scan_no_network(gate_path))

    if manifest_path.name != MANIFEST_NAME:
        errors.append(f"unexpected manifest file name {manifest_path.name}")
    return errors


def check_deterministic_regeneration(
    repo_root: Path,
    generator_path: Path,
    manifest_path: Path,
    oracle_path: Path,
) -> list[str]:
    errors: list[str] = []
    with tempfile.TemporaryDirectory(prefix="sbsql_multimodel_capability_") as temp:
        output_dir = Path(temp)
        result = subprocess.run(
            [
                sys.executable,
                str(generator_path),
                "--repo-root",
                str(repo_root),
                "--output-dir",
                str(output_dir),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            return [
                "deterministic regeneration failed "
                f"exit={result.returncode} stderr={result.stderr.strip()[:300]}"
            ]
        generated_manifest = output_dir / MANIFEST_NAME
        generated_oracle = output_dir / ORACLE_NAME
        if not generated_manifest.is_file():
            errors.append("regenerated manifest missing")
        elif generated_manifest.read_bytes() != manifest_path.read_bytes():
            errors.append("regenerated manifest differs from checked-in manifest")
        if not generated_oracle.is_file():
            errors.append("regenerated oracle missing")
        elif generated_oracle.read_bytes() != oracle_path.read_bytes():
            errors.append("regenerated oracle differs from checked-in oracle")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name(MANIFEST_NAME),
    )
    parser.add_argument(
        "--oracle",
        type=Path,
        default=Path(__file__).with_name(ORACLE_NAME),
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    manifest_path = args.manifest if args.manifest.is_absolute() else repo_root / args.manifest
    oracle_path = args.oracle if args.oracle.is_absolute() else repo_root / args.oracle
    if not manifest_path.is_file():
        manifest_path = Path(__file__).with_name(MANIFEST_NAME)
    if not oracle_path.is_file():
        oracle_path = Path(__file__).with_name(ORACLE_NAME)

    try:
        manifest = load_manifest(manifest_path)
        oracle_records = load_oracle(oracle_path)
        generator_rel = manifest.get("generator", {}).get("path", "")
        if not isinstance(generator_rel, str) or not generator_rel:
            raise GateError("generator path missing")
        generator_path = repo_root / generator_rel
        if not generator_path.is_file():
            raise GateError(f"generator missing: {generator_path}")
        errors = validate_manifest_and_oracle(
            manifest,
            oracle_records,
            manifest_path,
            oracle_path,
            generator_path,
            Path(__file__).resolve(),
        )
        errors.extend(
            check_deterministic_regeneration(
                repo_root,
                generator_path,
                manifest_path,
                oracle_path,
            )
        )
    except Exception as exc:  # noqa: BLE001
        print(f"sbsql_sml_054_057_062_multimodel_capability_gate=failed: {exc}", file=sys.stderr)
        return 1

    if errors:
        print("sbsql_sml_054_057_062_multimodel_capability_gate=failed", file=sys.stderr)
        for error in errors[:120]:
            print(error, file=sys.stderr)
        if len(errors) > 120:
            print(f"... {len(errors) - 120} additional errors", file=sys.stderr)
        return 1

    print(
        "sbsql_sml_054_057_062_multimodel_capability_gate=passed "
        f"rows={len(manifest.get('rows', []))} "
        f"oracle_records={len(oracle_records)} "
        f"manifest_sha256={manifest.get('manifest_sha256')}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
