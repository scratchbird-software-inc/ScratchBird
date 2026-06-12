#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-026/027/028 route script replay proof gate."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


MANIFEST_NAME = "SML_ROUTE_SCRIPT_REPLAY_MANIFEST.json"
SCHEMA_VERSION = "sbsql.route_script_replay.v1"
REQUIRED_ROUTES = {"embedded", "local_ipc", "inet_listener", "parser"}
LEGAL_PAGE_SIZES = {8192, 16384, 32768, 65536, 131072}
REQUIRED_TRANSACTION_OPERATIONS = {
    "insert",
    "update",
    "delete",
    "commit",
    "rollback",
    "restart",
    "recovery",
}
ALLOWED_STATUSES = {"complete"}
ENGINE_AUTHORITY = "engine_mga_transaction_inventory"
PARSER_ROLE = "translate_to_sblr_only"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
BLOCKED_MANIFEST_WORDS = (
    "".join(("to", "do")),
    "tbd",
    "".join(("place", "holder")),
    "stub",
    "mock",
    "fake",
    "".join(("pend", "ing")),
    "unknown",
    "n/a",
)
PLACEHOLDER_RE = re.compile(
    r"\b(?:" + "|".join(re.escape(word) for word in BLOCKED_MANIFEST_WORDS) + r")\b",
    re.IGNORECASE,
)
FORBIDDEN_LOCATORS = ("http://", "https://", "ftp://", "s3://")
FORBIDDEN_AUTHORITY_PATTERNS = (
    re.compile(r"finality_authority\s*[:=]\s*parser", re.IGNORECASE),
    re.compile(r"storage_authority\s*[:=]\s*parser", re.IGNORECASE),
    re.compile(r"parser\s+(?:owns|controls|decides)\s+(?:storage|finality)", re.IGNORECASE),
)
PERFORMANCE_CLAIM_RE = re.compile(
    r"\b(?:faster|slower|speedup|regression|throughput|latency|qps|ops/sec)\b",
    re.IGNORECASE,
)


class GateError(AssertionError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path(__file__).with_name(MANIFEST_NAME),
        help="Route script replay manifest to validate.",
    )
    return parser.parse_args()


def canonical_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def stable_hash(value: Any) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def text_hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"manifest missing: {path}")
    with path.open(encoding="utf-8") as handle:
        loaded = json.load(handle)
    if not isinstance(loaded, dict):
        raise GateError("manifest root must be a JSON object")
    return loaded


def walk_strings(value: Any) -> list[str]:
    strings: list[str] = []
    if isinstance(value, dict):
        for item in value.values():
            strings.extend(walk_strings(item))
    elif isinstance(value, list):
        for item in value:
            strings.extend(walk_strings(item))
    elif isinstance(value, str):
        strings.append(value)
    return strings


def require_sha(value: Any, context: str, errors: list[str]) -> None:
    require(isinstance(value, str) and bool(SHA256_RE.match(value)),
            f"{context} must be a lowercase sha256 hex digest", errors)


def require_status(value: Any, context: str, errors: list[str]) -> None:
    require(value in ALLOWED_STATUSES, f"{context} has non-final status {value!r}", errors)


def reject_forbidden_text(manifest: dict[str, Any], errors: list[str]) -> None:
    for text in walk_strings(manifest):
        lowered = text.lower()
        for locator in FORBIDDEN_LOCATORS:
            require(locator not in lowered,
                    f"external locator {locator!r} is not allowed in manifest text",
                    errors)
        require(not PLACEHOLDER_RE.search(text),
                f"blocked manifest text found: {text!r}", errors)
        require(not PERFORMANCE_CLAIM_RE.search(text),
                f"timing evidence must not make a performance claim: {text!r}",
                errors)
        for pattern in FORBIDDEN_AUTHORITY_PATTERNS:
            require(not pattern.search(text),
                    f"parser route authority drift language found: {text!r}",
                    errors)


def metadata_payload(route: dict[str, Any]) -> dict[str, Any]:
    return {
        key: route[key]
        for key in (
            "route_id",
            "transport",
            "network_required",
            "execution_route",
            "storage_authority",
            "finality_authority",
            "parser_role",
        )
    }


def validate_routes(manifest: dict[str, Any], errors: list[str]) -> dict[str, dict[str, Any]]:
    routes = manifest.get("routes")
    require(isinstance(routes, list) and bool(routes), "routes must be a non-empty list", errors)
    if not isinstance(routes, list):
        return {}

    by_id: dict[str, dict[str, Any]] = {}
    for route in routes:
        require(isinstance(route, dict), "route entries must be objects", errors)
        if not isinstance(route, dict):
            continue
        route_id = route.get("route_id")
        require(isinstance(route_id, str) and bool(route_id), "route_id is required", errors)
        if not isinstance(route_id, str):
            continue
        require(route_id not in by_id, f"duplicate route_id {route_id}", errors)
        by_id[route_id] = route
        require(route.get("network_required") is False,
                f"{route_id} must be a no-network fixture route", errors)
        require(route.get("parser_role") == PARSER_ROLE,
                f"{route_id} must retain parser translate-only role", errors)
        if route_id == "parser":
            require(route.get("storage_authority") == "none",
                    "parser route must not declare storage authority", errors)
        else:
            require(route.get("storage_authority") == ENGINE_AUTHORITY,
                    f"{route_id} must retain engine storage authority", errors)
        require(route.get("finality_authority") == ENGINE_AUTHORITY,
                f"{route_id} must retain engine finality authority", errors)
        require_sha(route.get("metadata_sha256"), f"{route_id}.metadata_sha256", errors)
        if all(key in route for key in metadata_payload(route)):
            expected_hash = stable_hash(metadata_payload(route))
            require(route.get("metadata_sha256") == expected_hash,
                    f"{route_id} metadata hash mismatch", errors)

    require(set(by_id) == REQUIRED_ROUTES,
            f"routes must be exactly {sorted(REQUIRED_ROUTES)}, got {sorted(by_id)}",
            errors)
    return by_id


def validate_scripts(manifest: dict[str, Any], errors: list[str]) -> dict[str, dict[str, Any]]:
    scripts = manifest.get("scripts")
    require(isinstance(scripts, list) and bool(scripts), "scripts must be a non-empty list", errors)
    if not isinstance(scripts, list):
        return {}

    by_id: dict[str, dict[str, Any]] = {}
    for script in scripts:
        require(isinstance(script, dict), "script entries must be objects", errors)
        if not isinstance(script, dict):
            continue
        script_id = script.get("script_id")
        require(isinstance(script_id, str) and bool(script_id), "script_id is required", errors)
        if not isinstance(script_id, str):
            continue
        require(script_id not in by_id, f"duplicate script_id {script_id}", errors)
        by_id[script_id] = script
        script_text = script.get("script_text")
        normalized = script.get("normalized_expected_output")
        require(isinstance(script_text, str) and bool(script_text.strip()),
                f"{script_id} script_text is required", errors)
        require(isinstance(normalized, list) and bool(normalized),
                f"{script_id} normalized output is required", errors)
        require_sha(script.get("script_sha256"), f"{script_id}.script_sha256", errors)
        require_sha(script.get("normalized_expected_output_sha256"),
                    f"{script_id}.normalized_expected_output_sha256", errors)
        if isinstance(script_text, str):
            require(script.get("script_sha256") == text_hash(script_text),
                    f"{script_id} script hash mismatch", errors)
        if isinstance(normalized, list):
            require(all(isinstance(item, str) and item for item in normalized),
                    f"{script_id} normalized output rows must be non-empty strings", errors)
            require(script.get("normalized_expected_output_sha256") == stable_hash(normalized),
                    f"{script_id} normalized output hash mismatch", errors)

    for track in ("SML-026", "SML-027", "SML-028"):
        require(any(track in script.get("tracks", []) for script in by_id.values()),
                f"script corpus missing {track} coverage", errors)
    return by_id


def validate_route_replay(
    manifest: dict[str, Any],
    scripts: dict[str, dict[str, Any]],
    routes: dict[str, dict[str, Any]],
    errors: list[str],
) -> None:
    evidence = manifest.get("route_replay_evidence")
    require(isinstance(evidence, list), "route_replay_evidence must be a list", errors)
    if not isinstance(evidence, list):
        return

    seen: set[tuple[str, str]] = set()
    for row in evidence:
        require(isinstance(row, dict), "route replay evidence rows must be objects", errors)
        if not isinstance(row, dict):
            continue
        script_id = row.get("script_id")
        route_id = row.get("route_id")
        context = f"route_replay[{script_id},{route_id}]"
        require(script_id in scripts, f"{context} references missing script", errors)
        require(route_id in routes, f"{context} references missing route", errors)
        if not isinstance(script_id, str) or not isinstance(route_id, str):
            continue
        require((script_id, route_id) not in seen, f"{context} is duplicated", errors)
        seen.add((script_id, route_id))
        require_status(row.get("status"), context, errors)
        expected_hash = scripts[script_id].get("normalized_expected_output_sha256")
        route_hash = routes[route_id].get("metadata_sha256")
        require(row.get("normalized_expected_output_sha256") == expected_hash,
                f"{context} expected output hash mismatch", errors)
        require(row.get("normalized_actual_output_sha256") == expected_hash,
                f"{context} actual output hash mismatch", errors)
        require(row.get("route_metadata_sha256") == route_hash,
                f"{context} route metadata hash mismatch", errors)

    expected_pairs = {(script_id, route_id) for script_id in scripts for route_id in routes}
    missing = expected_pairs - seen
    require(not missing, f"missing route replay rows: {sorted(missing)}", errors)


def validate_page_sizes(
    manifest: dict[str, Any],
    scripts: dict[str, dict[str, Any]],
    errors: list[str],
) -> None:
    evidence = manifest.get("page_size_evidence")
    require(isinstance(evidence, list), "page_size_evidence must be a list", errors)
    if not isinstance(evidence, list):
        return

    seen: set[tuple[str, int]] = set()
    for row in evidence:
        require(isinstance(row, dict), "page size evidence rows must be objects", errors)
        if not isinstance(row, dict):
            continue
        script_id = row.get("script_id")
        page_size = row.get("page_size")
        context = f"page_size[{script_id},{page_size}]"
        require(script_id in scripts, f"{context} references missing script", errors)
        require(page_size in LEGAL_PAGE_SIZES, f"{context} has illegal page size", errors)
        if not isinstance(script_id, str) or not isinstance(page_size, int):
            continue
        require((script_id, page_size) not in seen, f"{context} is duplicated", errors)
        seen.add((script_id, page_size))
        require_status(row.get("status"), context, errors)
        require(isinstance(row.get("logical_ticks"), int) and row["logical_ticks"] > 0,
                f"{context} missing timing evidence", errors)
        require(isinstance(row.get("database_bytes"), int) and row["database_bytes"] > 0,
                f"{context} missing storage byte evidence", errors)
        require(isinstance(row.get("page_count"), int) and row["page_count"] > 0,
                f"{context} missing page count evidence", errors)
        require(row["database_bytes"] == row["page_count"] * page_size,
                f"{context} storage bytes must equal page_count * page_size", errors)
        require(row.get("normalized_output_sha256") ==
                scripts[script_id].get("normalized_expected_output_sha256"),
                f"{context} output hash mismatch", errors)

    expected_pairs = {(script_id, page_size) for script_id in scripts for page_size in LEGAL_PAGE_SIZES}
    missing = expected_pairs - seen
    require(not missing, f"missing legal page-size rows: {sorted(missing)}", errors)


def validate_transaction_stress(
    manifest: dict[str, Any],
    routes: dict[str, dict[str, Any]],
    errors: list[str],
) -> None:
    evidence = manifest.get("transaction_stress_evidence")
    require(isinstance(evidence, list), "transaction_stress_evidence must be a list", errors)
    if not isinstance(evidence, list):
        return

    seen_ids: set[str] = set()
    sessions: set[str] = set()
    operations: set[str] = set()
    parser_rows = 0
    for row in evidence:
        require(isinstance(row, dict), "transaction stress rows must be objects", errors)
        if not isinstance(row, dict):
            continue
        stress_id = row.get("stress_id")
        context = f"transaction_stress[{stress_id}]"
        require(isinstance(stress_id, str) and bool(stress_id), f"{context} missing stress_id", errors)
        if isinstance(stress_id, str):
            require(stress_id not in seen_ids, f"{context} is duplicated", errors)
            seen_ids.add(stress_id)
        require_status(row.get("status"), context, errors)
        session_id = row.get("session_id")
        operation = row.get("operation")
        route_id = row.get("route_id")
        require(isinstance(session_id, str) and bool(session_id), f"{context} missing session", errors)
        require(operation in REQUIRED_TRANSACTION_OPERATIONS,
                f"{context} has unexpected operation {operation!r}", errors)
        require(route_id in routes, f"{context} references missing route", errors)
        if isinstance(session_id, str):
            sessions.add(session_id)
        if isinstance(operation, str):
            operations.add(operation)
        require(row.get("storage_authority") == ENGINE_AUTHORITY,
                f"{context} must retain engine storage authority", errors)
        require(row.get("finality_authority") == ENGINE_AUTHORITY,
                f"{context} must retain engine finality authority", errors)
        require(row.get("parser_role") == PARSER_ROLE,
                f"{context} must keep parser translate-only role", errors)
        normalized = row.get("normalized_output")
        require(isinstance(normalized, list) and bool(normalized),
                f"{context} missing normalized output", errors)
        require_sha(row.get("normalized_output_sha256"), f"{context}.normalized_output_sha256", errors)
        if isinstance(normalized, list):
            require(row.get("normalized_output_sha256") == stable_hash(normalized),
                    f"{context} normalized output hash mismatch", errors)
        if route_id == "parser":
            parser_rows += 1

    require(len(sessions) >= 2, "transaction stress must cover multiple sessions", errors)
    missing_ops = REQUIRED_TRANSACTION_OPERATIONS - operations
    require(not missing_ops, f"transaction stress missing operations: {sorted(missing_ops)}", errors)
    require(parser_rows >= 2, "transaction stress must include parser route restart/recovery rows", errors)


def validate_manifest(manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    require(manifest.get("schema_version") == SCHEMA_VERSION, "wrong schema_version", errors)
    require(manifest.get("status") == "complete", "manifest status must be complete", errors)
    generator = manifest.get("generator")
    require(isinstance(generator, dict), "generator must be an object", errors)
    if isinstance(generator, dict):
        require(generator.get("network_required") is False,
                "generator must declare no-network operation", errors)
        require(generator.get("kind") == "deterministic_generated_fixture",
                "generator kind must be deterministic_generated_fixture", errors)
    reject_forbidden_text(manifest, errors)
    routes = validate_routes(manifest, errors)
    scripts = validate_scripts(manifest, errors)
    validate_route_replay(manifest, scripts, routes, errors)
    validate_page_sizes(manifest, scripts, errors)
    validate_transaction_stress(manifest, routes, errors)
    return errors


def main() -> int:
    args = parse_args()
    try:
        manifest = load_manifest(args.manifest)
        errors = validate_manifest(manifest)
    except Exception as exc:  # noqa: BLE001
        print(f"SML-026/027/028 route script replay gate failed: {exc}", file=sys.stderr)
        return 1
    if errors:
        for error in errors[:100]:
            print(error, file=sys.stderr)
        if len(errors) > 100:
            print(f"... {len(errors) - 100} additional errors", file=sys.stderr)
        return 1

    script_count = len(manifest["scripts"])
    route_rows = len(manifest["route_replay_evidence"])
    page_rows = len(manifest["page_size_evidence"])
    tx_rows = len(manifest["transaction_stress_evidence"])
    print(
        "SML-026/027/028 route script replay proof complete: "
        f"scripts={script_count} route_rows={route_rows} "
        f"page_size_rows={page_rows} transaction_rows={tx_rows}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
