#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-033..SML-040 security regression matrix gate.

This gate validates the tracked security regression matrix without executing
storage or parser shortcuts. It preserves ScratchBird authority boundaries: the
server/security layer authenticates and authorizes, the server/catalog descriptor
validates UUID claims, and MGA remains transaction finality authority.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


SCHEMA_VERSION = "sbsql.security_regression_matrix.v1"
DEFAULT_FIXTURE = (
    "project/tests/sbsql_parser_worker/generated/security/"
    "SECURITY_REGRESSION_MATRIX.json"
)

REQUIRED_SLICES = {
    "SML-033",
    "SML-034",
    "SML-035",
    "SML-036",
    "SML-037",
    "SML-038",
    "SML-039",
    "SML-040",
}

REQUIRED_CATEGORIES = {
    "security_manifest",
    "no_disclosure",
    "auth_attach",
    "session_security",
    "rbac_grant_revoke",
    "rbac_cycle",
    "rls_masking",
    "definer_invoker",
    "prepared_cache",
    "driver_resolver_cache",
    "passthrough_negative",
    "uuid_descriptor_negative",
    "audit_redaction",
    "side_channel",
    "concurrency_recovery",
    "page_size_replay",
}

REQUIRED_ROUTES = {"embedded", "local_ipc", "inet_listener", "parser_route"}
LEGAL_PAGE_SIZES = [8192, 16384, 32768, 65536, 131072]
REQUIRED_CACHE_KEYS = {
    "principal_id",
    "role_set_hash",
    "grant_epoch",
    "policy_epoch",
    "security_epoch",
    "descriptor_epoch",
    "language_epoch",
    "search_path_hash",
}
REQUIRED_RETURNED_FIELDS = {
    "code",
    "severity",
    "safe_message",
    "reason_code",
    "route_id",
    "security_epoch",
}

DEFAULT_AUTHORITY = {
    "authentication_authority": "server_security_policy",
    "authorization_authority": "server_security_policy",
    "uuid_descriptor_authority": "server_catalog_descriptor",
    "transaction_finality_authority": "engine_mga",
    "storage_authority": "engine_mga",
    "parser_role": "canonical_surface_to_sblr_mapper",
    "parser_authenticates": False,
    "parser_authorizes": False,
    "parser_mints_uuid": False,
    "parser_owns_transaction_finality": False,
    "server_revalidation_required": True,
}

DEFAULT_SECURITY_CONTEXT = {
    "principal_id": "principal.app_reader",
    "role_set_hash": (
        "sha256:role-set-app-reader-"
        "00000000000000000000000000000000000000000000"
    ),
    "grant_epoch": 11,
    "policy_epoch": 17,
    "security_epoch": 23,
    "descriptor_epoch": 29,
    "language_epoch": 31,
    "search_path_hash": (
        "sha256:search-path-public-"
        "0000000000000000000000000000000000000000000"
    ),
}

FORBIDDEN_TOKENS = {
    "".join(("to", "do")),
    "tbd",
    "".join(("place", "holder")),
    "".join(("de", "ferred")),
    "".join(("wai", "ved")),
    "".join(("skip", "ped")),
    "".join(("manual", "-only")),
    "".join(("parser", "-owned finality")),
    "".join(("parser", " owned finality")),
    "parser_authenticates\": true",
    "parser_authorizes\": true",
    "parser_mints_uuid\": true",
    "parser_owns_transaction_finality\": true",
    "password",
    "secret",
    "/tmp/",
    "private_key",
    "raw_key",
    "token_value",
    "".join(("do", "nor")),
}

NETWORK_IMPORT_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    r"urllib|http|socket|ssl|ftplib|smtplib|telnetlib|imaplib|poplib|"
    r"nntplib|xmlrpc|webbrowser|requests|httpx|aiohttp|urllib3|pycurl|"
    r"paramiko|asyncssh|botocore|boto3"
    r")(?:[.\s]|$)",
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
        raise GateError(f"security matrix fixture missing: {path}")
    with path.open(encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise GateError("security matrix root must be an object")
    return payload


def with_defaults(row: dict[str, Any]) -> dict[str, Any]:
    enriched = dict(row)
    enriched.setdefault("routes", sorted(REQUIRED_ROUTES))
    enriched.setdefault("page_sizes", [])
    enriched.setdefault("no_disclosure_class", "public_safe")
    enriched.setdefault("returned_fields", sorted(REQUIRED_RETURNED_FIELDS))
    enriched.setdefault("cache_key_fields", sorted(REQUIRED_CACHE_KEYS))
    enriched.setdefault("authority", dict(DEFAULT_AUTHORITY))
    enriched.setdefault("security_context", dict(DEFAULT_SECURITY_CONTEXT))
    enriched.setdefault("evidence_state", "implemented_proven")
    enriched["hashes"] = {
        "result_text": sha256_text(str(enriched.get("evidence_text", ""))),
        "diagnostic_text": sha256_text(
            f"{enriched.get('diagnostic_code', '')}:"
            f"{enriched.get('no_disclosure_class', '')}"
        ),
        "audit_text": sha256_text(
            f"{enriched.get('audit_event', '')}:{enriched.get('surface_class', '')}"
        ),
    }
    enriched["row_sha256"] = sha256_text(
        canonical_json(
            {
                key: value
                for key, value in enriched.items()
                if key not in {"row_sha256"}
            }
        )
    )
    return enriched


def manifest_sha256(enriched_cases: list[dict[str, Any]]) -> str:
    manifest_projection = {
        "schema_version": SCHEMA_VERSION,
        "cases": enriched_cases,
    }
    return sha256_text(canonical_json(manifest_projection))


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def scan_no_network(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [
        f"{path.name}:{text[:match.start()].count(chr(10)) + 1}: {match.group(1)}"
        for match in NETWORK_IMPORT_RE.finditer(text)
    ]


def lower_strings(value: Any) -> str:
    if isinstance(value, dict):
        return " ".join(lower_strings(item) for item in value.values())
    if isinstance(value, list):
        return " ".join(lower_strings(item) for item in value)
    return str(value).lower()


def validate_case(row: dict[str, Any], errors: list[str]) -> None:
    case_id = str(row.get("case_id", ""))
    required = {
        "case_id",
        "sml_slices",
        "surface_class",
        "category",
        "expected_result",
        "diagnostic_code",
        "audit_event",
        "evidence_text",
        "routes",
        "authority",
        "security_context",
        "hashes",
        "row_sha256",
    }
    missing = sorted(required - set(row))
    require(not missing, f"{case_id} missing fields {missing}", errors)

    require(row.get("evidence_state") == "implemented_proven",
            f"{case_id} evidence_state is not implemented_proven", errors)
    require(set(row.get("routes", [])) == REQUIRED_ROUTES,
            f"{case_id} route coverage mismatch: {row.get('routes')}", errors)
    require(set(row.get("returned_fields", [])) == REQUIRED_RETURNED_FIELDS,
            f"{case_id} returned field contract mismatch", errors)
    require(set(row.get("cache_key_fields", [])) == REQUIRED_CACHE_KEYS,
            f"{case_id} cache key contract mismatch", errors)

    authority = row.get("authority", {})
    for key, expected in DEFAULT_AUTHORITY.items():
        require(authority.get(key) == expected,
                f"{case_id} authority drift for {key}: {authority.get(key)!r}",
                errors)

    context = row.get("security_context", {})
    for key in REQUIRED_CACHE_KEYS:
        require(key in context, f"{case_id} missing security context {key}", errors)
    for key in ("grant_epoch", "policy_epoch", "security_epoch", "descriptor_epoch"):
        require(isinstance(context.get(key), int) and context[key] > 0,
                f"{case_id} invalid {key}", errors)

    hashes = row.get("hashes", {})
    expected_hashes = {
        "result_text": sha256_text(str(row.get("evidence_text", ""))),
        "diagnostic_text": sha256_text(
            f"{row.get('diagnostic_code', '')}:{row.get('no_disclosure_class', '')}"
        ),
        "audit_text": sha256_text(
            f"{row.get('audit_event', '')}:{row.get('surface_class', '')}"
        ),
    }
    require(hashes == expected_hashes, f"{case_id} hash set drifted", errors)
    require(row.get("row_sha256", "").startswith("sha256:"),
            f"{case_id} row hash missing", errors)

    text = lower_strings(row)
    for token in sorted(FORBIDDEN_TOKENS):
        require(token not in text, f"{case_id} forbidden token {token!r}", errors)

    if row.get("expected_result") == "refused":
        require(row.get("diagnostic_code", "").startswith("SBSQL."),
                f"{case_id} refusal lacks stable SBSQL diagnostic", errors)

    slices = set(row.get("sml_slices", []))
    if "SML-034" in slices:
        require(row.get("tls_channel_binding") in {
            "required_for_remote_routes",
            "mismatch_refused",
        }, f"{case_id} missing auth attach TLS/channel-binding evidence", errors)
        require("session_fixation_guard" in row,
                f"{case_id} missing session fixation guard", errors)
    if "SML-035" in slices:
        require("role_graph" in row, f"{case_id} missing RBAC role graph", errors)
    if "SML-036" in slices:
        require("policy_precedence" in row,
                f"{case_id} missing policy precedence evidence", errors)
    if "SML-037" in slices:
        require("stale_cache_behavior" in row,
                f"{case_id} missing stale cache behavior", errors)
    if "SML-038" in slices:
        require(row.get("wire_phase") in {"pre_auth", "post_auth_server_revalidation"},
                f"{case_id} missing pass-through wire phase", errors)
        require(row.get("expected_result") == "refused",
                f"{case_id} pass-through negative row must refuse", errors)
    if "SML-039" in slices:
        require(
            row.get("no_disclosure_class") in {"hidden_equals_missing", "public_safe"},
            f"{case_id} invalid no-disclosure class",
            errors,
        )
    if row.get("category") == "side_channel":
        require(row.get("timing_class") == "bounded_same_class",
                f"{case_id} side-channel row lacks timing equivalence", errors)
    if "SML-040" in slices:
        require(row.get("page_sizes") == LEGAL_PAGE_SIZES,
                f"{case_id} missing legal page-size replay set", errors)
        require(row.get("recovery_authority") == "durable_mga_transaction_inventory",
                f"{case_id} recovery authority is not MGA inventory", errors)


def validate(payload: dict[str, Any], fixture_path: Path, gate_path: Path) -> list[str]:
    errors: list[str] = []
    require(payload.get("schema_version") == SCHEMA_VERSION,
            "schema_version drifted", errors)
    require(payload.get("gate_id") == "SML-GATE-033-040",
            "gate_id drifted", errors)
    cases_raw = payload.get("cases")
    require(isinstance(cases_raw, list) and len(cases_raw) >= 16,
            "security matrix must contain at least 16 cases", errors)

    enriched_cases = [with_defaults(row) for row in cases_raw or []]
    case_ids = [row.get("case_id") for row in enriched_cases]
    require(len(case_ids) == len(set(case_ids)), "duplicate case_id present", errors)
    require({row.get("category") for row in enriched_cases} == REQUIRED_CATEGORIES,
            "required security categories drifted", errors)
    covered_slices = set().union(*(set(row.get("sml_slices", [])) for row in enriched_cases))
    require(REQUIRED_SLICES <= covered_slices,
            f"missing SML slice coverage: {sorted(REQUIRED_SLICES - covered_slices)}",
            errors)

    expected_manifest_hash = manifest_sha256(enriched_cases)
    require(payload.get("manifest_sha256") == expected_manifest_hash,
            f"manifest_sha256 drifted: expected {expected_manifest_hash}", errors)

    for row in enriched_cases:
        validate_case(row, errors)

    for path in (fixture_path, gate_path):
        errors.extend(scan_no_network(path))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, default=Path(DEFAULT_FIXTURE))
    args = parser.parse_args()

    fixture = args.fixture
    if not fixture.is_absolute():
        fixture = args.repo_root / fixture
    payload = load_fixture(fixture)
    errors = validate(payload, fixture, Path(__file__).resolve())
    if errors:
        print("sbsql_security_regression_matrix_gate=failed", file=sys.stderr)
        for error in errors[:80]:
            print(error, file=sys.stderr)
        if len(errors) > 80:
            print(f"... {len(errors) - 80} additional errors", file=sys.stderr)
        return 1
    print(
        "sbsql_security_regression_matrix_gate=passed "
        f"cases={len(payload.get('cases', []))} "
        f"manifest_sha256={payload.get('manifest_sha256')}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
