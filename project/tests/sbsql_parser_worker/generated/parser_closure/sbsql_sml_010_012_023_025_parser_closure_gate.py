#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-010/011/012/023/025 parser-closure proof gate."""

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


MANIFEST_NAME = "SML_010_012_023_025_PARSER_CLOSURE_MANIFEST.csv"
ORACLE_NAME = "SML_010_012_023_025_PARSER_CLOSURE_ORACLE.json"
GENERATOR = (
    "project/tools/sb_parser_gen/"
    "generate_sbsql_sml_010_012_023_025_parser_closure.py"
)
SCHEMA_VERSION = "sbsql.parser_closure.sml_010_012_023_025.v1"

MANIFEST_COLUMNS = [
    "sml_id",
    "gate_id",
    "row_id",
    "proof_domain",
    "coverage_class",
    "scenario",
    "route_class",
    "evidence_kind",
    "closure_status",
    "parser_role",
    "parser_executes_sql",
    "parser_has_security_authority",
    "parser_has_storage_authority",
    "parser_owns_finality",
    "server_evaluated",
    "resolver_filtering",
    "language_library_authority",
    "security_projection_authority",
    "expected_contract",
    "expected_contract_hash",
    "expected_diagnostic",
    "expected_diagnostic_hash",
    "oracle_id",
    "oracle_hash",
    "evidence_paths",
    "evidence_tokens",
    "artifact_paths",
]

EXPECTED_DOMAINS = {
    "SML-010": "localized_rendering_resolver_filtering",
    "SML-011": "parser_language_library_non_authority",
    "SML-012": "multilingual_edge_cases",
    "SML-023": "sbsql_full_parser_closure",
    "SML-025": "legacy_security_projection_server_evaluated",
}

REQUIRED_COVERAGE = {
    "SML-010": {
        "localized_renderer_server_revalidation",
        "sensitive_resolver_filtering",
        "release_manifest_completeness",
    },
    "SML-011": {
        "language_control_non_authority",
        "bundle_manifest_fail_closed",
        "cache_authority_refusal",
    },
    "SML-012": {
        "exact_locale_matrix",
        "fallback_cache_isolation",
        "edge_safety_scan",
    },
    "SML-023": {
        "route_fixture_closure",
        "strict_row_final_state",
        "scalar_projection_dispatch",
    },
    "SML-025": {
        "security_authorization_server_reclassify",
        "protected_material_redacted_projection",
        "security_epoch_prepared_refusal",
    },
}

ALLOWED_STATUSES = {"closed_executable", "closed_oracle", "closed_refusal"}
PARSER_ROLE = "translate_to_sblr_and_diagnostics_only"
HASH_RE = re.compile(r"^[0-9a-f]{64}$")

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

FORBIDDEN_VALUE_TOKENS = (
    "".join(("to", "do")),
    "tbd",
    "".join(("place", "holder")),
    "".join(("st", "ub")),
    "mock",
    "fake",
    "".join(("pend", "ing")),
    "".join(("manual", "_only")),
    "".join(("manual", "-only")),
    "".join(("generated", "_only")),
    "".join(("generated", "-only")),
    "parser_owned",
    "".join(("parser", "-owned")),
)

FORBIDDEN_PATH_PREFIXES = (
    "docs/documentation/draft/",
    "/home/" + "dcalford/" + "Cli" + "Work/ScratchBird-" + "Private/",
    "../ScratchBird-" + "Private/",
)

PUBLIC_TERMINOLOGY_RE = re.compile(r"\b" + "d" + "onor" + r"\b", re.IGNORECASE)


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


def oracle_payload(row: dict[str, str]) -> dict[str, str]:
    return {
        "sml_id": row["sml_id"],
        "gate_id": row["gate_id"],
        "row_id": row["row_id"],
        "proof_domain": row["proof_domain"],
        "coverage_class": row["coverage_class"],
        "scenario": row["scenario"],
        "route_class": row["route_class"],
        "evidence_kind": row["evidence_kind"],
        "closure_status": row["closure_status"],
        "parser_role": row["parser_role"],
        "parser_executes_sql": row["parser_executes_sql"],
        "parser_has_security_authority": row["parser_has_security_authority"],
        "parser_has_storage_authority": row["parser_has_storage_authority"],
        "parser_owns_finality": row["parser_owns_finality"],
        "server_evaluated": row["server_evaluated"],
        "resolver_filtering": row["resolver_filtering"],
        "language_library_authority": row["language_library_authority"],
        "security_projection_authority": row["security_projection_authority"],
        "expected_contract": row["expected_contract"],
        "expected_diagnostic": row["expected_diagnostic"],
        "evidence_paths": row["evidence_paths"],
        "evidence_tokens": row["evidence_tokens"],
    }


def scan_no_network(path: Path) -> list[str]:
    text = read_text(path)
    return [
        f"{path}:{text[:match.start()].count(chr(10)) + 1}: forbidden network import {match.group(1)!r}"
        for match in NETWORK_MODULE_RE.finditer(text)
    ]


def scan_public_terminology(path: Path) -> list[str]:
    text = read_text(path)
    return [
        f"{path}:{text[:match.start()].count(chr(10)) + 1}: forbidden public terminology"
        for match in PUBLIC_TERMINOLOGY_RE.finditer(text)
    ]


def validate_deterministic_regeneration(repo_root: Path, fixture_dir: Path) -> list[str]:
    generator = repo_root / GENERATOR
    if not generator.is_file():
        return [f"generator missing: {generator}"]

    with tempfile.TemporaryDirectory(prefix="sbsql_parser_closure_") as temp:
        temp_dir = Path(temp)
        result = subprocess.run(
            [sys.executable, "-B", str(generator), "--output-dir", str(temp_dir)],
            cwd=repo_root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0:
            return [
                "generator failed during deterministic regeneration: "
                f"exit={result.returncode} stderr={result.stderr.strip()[:400]}"
            ]
        errors: list[str] = []
        for name in (MANIFEST_NAME, ORACLE_NAME):
            expected = fixture_dir / name
            observed = temp_dir / name
            if not observed.is_file():
                errors.append(f"generator did not produce {name}")
                continue
            if expected.read_bytes() != observed.read_bytes():
                errors.append(f"{name} differs from deterministic regeneration")
        return errors


def resolve_public_file(repo_root: Path, rel_path: str) -> Path:
    if rel_path.startswith("/") or rel_path.startswith("../"):
        raise GateError(f"path is not repo-relative public scope: {rel_path}")
    for prefix in FORBIDDEN_PATH_PREFIXES:
        if rel_path.startswith(prefix):
            raise GateError(f"forbidden proof path: {rel_path}")
    path = repo_root / rel_path
    if not path.is_file():
        raise GateError(f"proof path missing: {rel_path}")
    return path


def validate_text_values(row: dict[str, str], errors: list[str]) -> None:
    searchable_fields = [
        "row_id",
        "proof_domain",
        "coverage_class",
        "scenario",
        "route_class",
        "evidence_kind",
        "closure_status",
        "parser_role",
        "resolver_filtering",
        "language_library_authority",
        "security_projection_authority",
        "expected_contract",
        "expected_diagnostic",
        "evidence_paths",
        "evidence_tokens",
        "artifact_paths",
    ]
    for field in searchable_fields:
        value = row.get(field, "")
        lowered = value.lower()
        for token in FORBIDDEN_VALUE_TOKENS:
            if token in lowered:
                errors.append(f"{row['row_id']} contains blocked token {token!r} in {field}")
        if PUBLIC_TERMINOLOGY_RE.search(value):
            errors.append(f"{row['row_id']} contains forbidden public terminology in {field}")


def validate_evidence_tokens(
    row: dict[str, str],
    repo_root: Path,
    errors: list[str],
) -> None:
    paths = split_semicolon(row["evidence_paths"])
    tokens = split_semicolon(row["evidence_tokens"])
    if not paths:
        errors.append(f"{row['row_id']} has no evidence paths")
        return
    if not tokens:
        errors.append(f"{row['row_id']} has no evidence tokens")
        return

    texts: list[tuple[str, str]] = []
    for rel_path in paths:
        try:
            path = resolve_public_file(repo_root, rel_path)
            texts.append((rel_path, read_text(path)))
        except GateError as exc:
            errors.append(str(exc))
    if not texts:
        return

    for token in tokens:
        if not any(token in text for _, text in texts):
            errors.append(f"{row['row_id']} missing evidence token {token!r}")


def validate_artifacts(row: dict[str, str], repo_root: Path, errors: list[str]) -> None:
    artifacts = split_semicolon(row["artifact_paths"])
    required = {
        GENERATOR,
        f"project/tests/sbsql_parser_worker/generated/parser_closure/{MANIFEST_NAME}",
        f"project/tests/sbsql_parser_worker/generated/parser_closure/{ORACLE_NAME}",
        "project/tests/sbsql_parser_worker/generated/parser_closure/"
        "sbsql_sml_010_012_023_025_parser_closure_gate.py",
    }
    if set(artifacts) != required:
        errors.append(f"{row['row_id']} artifact path set drifted")
    for rel_path in artifacts:
        try:
            resolve_public_file(repo_root, rel_path)
        except GateError as exc:
            errors.append(str(exc))


def validate_row(row: dict[str, str], repo_root: Path, errors: list[str]) -> None:
    sml_id = row["sml_id"]
    row_id = row["row_id"]
    if sml_id not in EXPECTED_DOMAINS:
        errors.append(f"{row_id} unexpected sml_id {sml_id}")
        return
    if row["gate_id"] != sml_id.replace("SML-", "SML-GATE-"):
        errors.append(f"{row_id} gate_id mismatch")
    if row["proof_domain"] != EXPECTED_DOMAINS[sml_id]:
        errors.append(f"{row_id} proof_domain mismatch")
    if row["coverage_class"] not in REQUIRED_COVERAGE[sml_id]:
        errors.append(f"{row_id} unexpected coverage_class {row['coverage_class']}")
    if row["closure_status"] not in ALLOWED_STATUSES:
        errors.append(f"{row_id} invalid closure_status {row['closure_status']}")
    if row["parser_role"] != PARSER_ROLE:
        errors.append(f"{row_id} parser_role drifted")
    for field in (
        "parser_executes_sql",
        "parser_has_security_authority",
        "parser_has_storage_authority",
        "parser_owns_finality",
    ):
        if row[field] != "false":
            errors.append(f"{row_id} {field} must be false")
    if row["server_evaluated"] != "true":
        errors.append(f"{row_id} must remain server_evaluated=true")
    if row["expected_contract_hash"] != sha256_text(row["expected_contract"]):
        errors.append(f"{row_id} expected_contract_hash mismatch")
    if row["expected_diagnostic_hash"] != sha256_text(row["expected_diagnostic"]):
        errors.append(f"{row_id} expected_diagnostic_hash mismatch")
    if not HASH_RE.match(row["oracle_hash"]):
        errors.append(f"{row_id} oracle_hash is not sha256")
    expected_oracle_hash = sha256_text(canonical_json(oracle_payload(row)))
    if row["oracle_hash"] != expected_oracle_hash:
        errors.append(f"{row_id} oracle_hash mismatch")
    if sml_id == "SML-010" and row["resolver_filtering"] in {"", "none", "not_applicable"}:
        errors.append(f"{row_id} SML-010 resolver filtering proof missing")
    if sml_id == "SML-011" and row["language_library_authority"] in {"", "none"}:
        errors.append(f"{row_id} SML-011 language authority boundary missing")
    if sml_id == "SML-025" and row["security_projection_authority"] != (
        "server_sblr_admission_and_engine_security_api"
    ):
        errors.append(f"{row_id} SML-025 security projection authority drifted")
    validate_text_values(row, errors)
    validate_evidence_tokens(row, repo_root, errors)
    validate_artifacts(row, repo_root, errors)


def validate_oracle(rows: list[dict[str, str]], oracle: dict[str, Any], errors: list[str]) -> None:
    if oracle.get("schema_version") != SCHEMA_VERSION:
        errors.append("oracle schema_version mismatch")
    if oracle.get("manifest_name") != MANIFEST_NAME:
        errors.append("oracle manifest_name mismatch")
    if oracle.get("row_count") != len(rows):
        errors.append("oracle row_count mismatch")
    counts: dict[str, int] = {}
    for row in rows:
        counts[row["sml_id"]] = counts.get(row["sml_id"], 0) + 1
    if oracle.get("sml_counts") != dict(sorted(counts.items())):
        errors.append("oracle sml_counts mismatch")
    oracle_rows = oracle.get("rows")
    if not isinstance(oracle_rows, list):
        errors.append("oracle rows must be a list")
        return
    by_id = {row["row_id"]: row for row in rows}
    if {item.get("row_id") for item in oracle_rows if isinstance(item, dict)} != set(by_id):
        errors.append("oracle row ids mismatch")
        return
    for item in oracle_rows:
        if not isinstance(item, dict):
            errors.append("oracle row entries must be objects")
            continue
        row = by_id[item["row_id"]]
        for key in ("sml_id", "proof_domain", "coverage_class", "oracle_id", "oracle_hash"):
            if item.get(key) != row[key]:
                errors.append(f"oracle {item['row_id']} {key} mismatch")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[5],
    )
    parser.add_argument(
        "--fixture-dir",
        type=Path,
        default=Path(__file__).resolve().parent,
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    fixture_dir = args.fixture_dir
    if not fixture_dir.is_absolute():
        fixture_dir = repo_root / fixture_dir
    fixture_dir = fixture_dir.resolve()

    errors: list[str] = []
    rows = read_manifest(fixture_dir / MANIFEST_NAME)
    oracle = read_oracle(fixture_dir / ORACLE_NAME)

    errors.extend(validate_deterministic_regeneration(repo_root, fixture_dir))
    for rel_path in (
        GENERATOR,
        f"project/tests/sbsql_parser_worker/generated/parser_closure/{MANIFEST_NAME}",
        f"project/tests/sbsql_parser_worker/generated/parser_closure/{ORACLE_NAME}",
        "project/tests/sbsql_parser_worker/generated/parser_closure/"
        "sbsql_sml_010_012_023_025_parser_closure_gate.py",
    ):
        path = repo_root / rel_path
        errors.extend(scan_no_network(path))
        errors.extend(scan_public_terminology(path))

    seen: set[str] = set()
    coverage_by_sml: dict[str, set[str]] = {key: set() for key in EXPECTED_DOMAINS}
    for row in rows:
        row_id = row["row_id"]
        if row_id in seen:
            errors.append(f"duplicate row_id {row_id}")
            continue
        seen.add(row_id)
        if row["sml_id"] in coverage_by_sml:
            coverage_by_sml[row["sml_id"]].add(row["coverage_class"])
        validate_row(row, repo_root, errors)

    if set(coverage_by_sml) != set(EXPECTED_DOMAINS):
        errors.append("coverage key drift")
    for sml_id, required in REQUIRED_COVERAGE.items():
        missing = sorted(required - coverage_by_sml.get(sml_id, set()))
        if missing:
            errors.append(f"{sml_id} missing coverage classes: {missing}")
    validate_oracle(rows, oracle, errors)

    print(
        "sbsql_sml_010_012_023_025_parser_closure_gate "
        f"rows={len(rows)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sml_010_012_023_025_parser_closure_gate=failed", file=sys.stderr)
        for error in errors[:40]:
            print(f"  {error}", file=sys.stderr)
        if len(errors) > 40:
            print(f"  ... {len(errors) - 40} additional errors", file=sys.stderr)
        return 1
    print("sbsql_sml_010_012_023_025_parser_closure_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
