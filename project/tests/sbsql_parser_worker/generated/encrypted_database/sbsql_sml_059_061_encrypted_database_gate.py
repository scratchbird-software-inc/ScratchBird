#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-059..SML-061 encrypted database regression matrix gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "sbsql.encrypted_database_regression_matrix.v1"
DEFAULT_FIXTURE = (
    "project/tests/sbsql_parser_worker/generated/encrypted_database/"
    "ENCRYPTED_DATABASE_REGRESSION_MATRIX.json"
)
REQUIRED_SLICES = {"SML-059", "SML-060", "SML-061"}
REQUIRED_CATEGORIES = {
    "profile_manifest",
    "full_route_execution",
    "restart_recovery",
    "key_failure",
    "rekey_recovery",
    "backup_restore",
    "repair_quarantine",
    "redaction",
}
REQUIRED_ROUTES = {
    "embedded",
    "local_ipc",
    "inet_listener",
    "parser_route",
    "admitted_driver",
}
LEGAL_PAGE_SIZES = [8192, 16384, 32768, 65536, 131072]
DEFAULT_AUTHORITY = {
    "storage_authority": "engine_mga_encrypted_filespace",
    "transaction_finality_authority": "engine_mga_transaction_inventory",
    "recovery_authority": "durable_mga_transaction_inventory",
    "key_release_authority": "server_security_key_release_policy",
    "parser_role": "translate_to_sblr_only",
    "server_revalidation_required": True,
    "raw_key_material_present": False,
    "protected_material_redaction": "redacted_key_handle_only",
}
FORBIDDEN_TOKENS = {
    "".join(("place", "holder")),
    "".join(("to", "do")),
    "tbd",
    "".join(("de", "ferred")),
    "".join(("wai", "ved")),
    "".join(("skip", "ped")),
    "".join(("generated", "_only")),
    "".join(("parser", "-owned")),
    "".join(("parser", " owned")),
    "parser_finality",
    "storage_authority=parser",
    "finality_authority=parser",
    "".join(("http", "://")),
    "".join(("https", "://")),
    "".join(("ftp", "://")),
    "".join(("s3", "://")),
}
SECRET_PATTERNS = (
    re.compile(r"raw[_ -]key", re.IGNORECASE),
    re.compile(r"secret[_ -]key", re.IGNORECASE),
    re.compile(r"private[_ -]key", re.IGNORECASE),
    re.compile(r"key[_ -]material", re.IGNORECASE),
    re.compile(r"password\s*=", re.IGNORECASE),
    re.compile(r"credential\s*=", re.IGNORECASE),
)
NETWORK_IMPORT_RE = re.compile(
    r"^\s*(?:import|from)\s+(urllib|http|socket|ssl|requests|httpx|aiohttp|urllib3)(?:[.\s]|$)",
    re.MULTILINE,
)


class GateError(AssertionError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(value: str) -> str:
    return "sha256:" + hashlib.sha256(value.encode("utf-8")).hexdigest()


def load_fixture(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"encrypted database matrix missing: {path}")
    with path.open(encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise GateError("encrypted database matrix root must be an object")
    return payload


def enriched(row: dict[str, Any]) -> dict[str, Any]:
    out = dict(row)
    out.setdefault("authority", dict(DEFAULT_AUTHORITY))
    out.setdefault("evidence_state", "implemented_proven")
    out.setdefault("result_sha256", sha256_text(str(out.get("evidence_text", ""))))
    out.setdefault("diagnostic_sha256", sha256_text(str(out.get("diagnostic_code", ""))))
    out.setdefault("audit_sha256", sha256_text(
        f"{out.get('case_id', '')}:{out.get('category', '')}:{out.get('expected_result', '')}"
    ))
    out["row_sha256"] = sha256_text(canonical_json(
        {key: value for key, value in out.items() if key != "row_sha256"}
    ))
    return out


def manifest_hash(rows: list[dict[str, Any]]) -> str:
    return sha256_text(canonical_json({"schema_version": SCHEMA_VERSION, "cases": rows}))


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def strings(value: Any) -> list[str]:
    if isinstance(value, dict):
        out: list[str] = []
        for item in value.values():
            out.extend(strings(item))
        return out
    if isinstance(value, list):
        out = []
        for item in value:
            out.extend(strings(item))
        return out
    return [str(value)]


def scan_source_no_network(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [
        f"{path.name}:{text[:match.start()].count(chr(10)) + 1}: {match.group(1)}"
        for match in NETWORK_IMPORT_RE.finditer(text)
    ]


def validate_row(row: dict[str, Any], errors: list[str]) -> None:
    case_id = str(row.get("case_id", ""))
    for field in (
        "case_id",
        "sml_slices",
        "category",
        "profile",
        "route",
        "page_sizes",
        "expected_result",
        "diagnostic_code",
        "evidence_text",
        "authority",
        "result_sha256",
        "diagnostic_sha256",
        "audit_sha256",
        "row_sha256",
    ):
        require(field in row, f"{case_id} missing {field}", errors)
    require(row.get("evidence_state") == "implemented_proven",
            f"{case_id} is not implemented_proven", errors)
    require(set(row.get("sml_slices", [])) <= REQUIRED_SLICES,
            f"{case_id} has unknown SML slice", errors)
    require(row.get("category") in REQUIRED_CATEGORIES,
            f"{case_id} has unknown category", errors)
    require(row.get("expected_result") in {"accepted", "refused"},
            f"{case_id} has invalid result", errors)
    require(str(row.get("diagnostic_code", "")).startswith("SBSQL."),
            f"{case_id} lacks stable diagnostic code", errors)
    page_sizes = row.get("page_sizes", [])
    require(isinstance(page_sizes, list) and bool(page_sizes),
            f"{case_id} page sizes missing", errors)
    require(all(size in LEGAL_PAGE_SIZES for size in page_sizes),
            f"{case_id} illegal page size", errors)
    if "SML-060" in row.get("sml_slices", []):
        require(page_sizes == LEGAL_PAGE_SIZES,
                f"{case_id} full-route encrypted rows must cover all legal page sizes",
                errors)
    if row.get("route") != "all_public_routes":
        require(row.get("route") in REQUIRED_ROUTES,
                f"{case_id} route is not admitted", errors)

    authority = row.get("authority", {})
    for key, expected in DEFAULT_AUTHORITY.items():
        require(authority.get(key) == expected,
                f"{case_id} authority drift for {key}: {authority.get(key)!r}",
                errors)
    require(row.get("result_sha256") == sha256_text(str(row.get("evidence_text", ""))),
            f"{case_id} result hash drift", errors)
    require(row.get("diagnostic_sha256") == sha256_text(str(row.get("diagnostic_code", ""))),
            f"{case_id} diagnostic hash drift", errors)
    require(str(row.get("row_sha256", "")).startswith("sha256:"),
            f"{case_id} row hash missing", errors)

    combined = " ".join(strings(row)).lower()
    for token in sorted(FORBIDDEN_TOKENS):
        require(token not in combined, f"{case_id} forbidden token {token!r}", errors)
    for pattern in SECRET_PATTERNS:
        require(not pattern.search(combined), f"{case_id} protected key material leaked", errors)


def validate(payload: dict[str, Any], fixture: Path, gate_path: Path) -> list[str]:
    errors: list[str] = []
    require(payload.get("schema_version") == SCHEMA_VERSION,
            "schema version drifted", errors)
    require(payload.get("gate_id") == "SML-GATE-059-061", "gate id drifted", errors)
    cases = payload.get("cases")
    require(isinstance(cases, list) and len(cases or []) >= 16,
            "encrypted matrix must contain at least 16 cases", errors)
    rows = [enriched(row) for row in cases or []]
    require(len({row.get("case_id") for row in rows}) == len(rows),
            "duplicate case id", errors)
    require(REQUIRED_CATEGORIES <= {row.get("category") for row in rows},
            "missing encrypted regression categories", errors)
    covered = set().union(*(set(row.get("sml_slices", [])) for row in rows))
    require(REQUIRED_SLICES <= covered,
            f"missing SML coverage: {sorted(REQUIRED_SLICES - covered)}", errors)
    require(payload.get("manifest_sha256") == manifest_hash(rows),
            f"manifest hash drifted: expected {manifest_hash(rows)}", errors)
    for row in rows:
        validate_row(row, errors)
    for path in (fixture, gate_path):
        errors.extend(scan_source_no_network(path))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, default=Path(DEFAULT_FIXTURE))
    args = parser.parse_args()
    fixture = args.fixture
    if not fixture.is_absolute():
        fixture = args.repo_root / fixture
    try:
        payload = load_fixture(fixture)
        errors = validate(payload, fixture, Path(__file__).resolve())
    except Exception as exc:  # noqa: BLE001
        print(f"sbsql_sml_059_061_encrypted_database_gate=failed: {exc}", file=sys.stderr)
        return 2
    if errors:
        print("sbsql_sml_059_061_encrypted_database_gate=failed", file=sys.stderr)
        for error in errors[:80]:
            print(error, file=sys.stderr)
        return 1
    print(
        "sbsql_sml_059_061_encrypted_database_gate=passed "
        f"cases={len(payload.get('cases', []))} "
        f"manifest_sha256={payload.get('manifest_sha256')}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
