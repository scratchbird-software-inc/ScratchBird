#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-041..046 product QA manifest gate."""

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


SCHEMA_VERSION = "sbsql.sml_041_046.product_qa_manifest.v1"
GENERATOR = "project/tools/sb_parser_gen/generate_sbsql_sml_041_046_product_qa_gates.py"
DEFAULT_MANIFEST = "SML_041_046_PRODUCT_QA_MANIFEST.json"
ARTIFACT_ROOT = "project/tests/sbsql_parser_worker/fixtures/surface_to_sblr/artifacts"
SURFACE_ARTIFACTS = [
    "SBSQL_LANGUAGE_ELEMENT_MANIFEST.csv",
    "SBSQL_SURFACE_RELEASE_DECLARATION.csv",
    "PER_ROW_EVIDENCE_MANIFEST.csv",
    "AUTHENTICATED_FULL_ROUTE_MATRIX.csv",
    "SBLR_BINARY_ROUND_TRIP_MATRIX.csv",
]

REQUIRED_VARIATIONS = {
    "grammar",
    "semantic",
    "route",
    "language",
    "page_size",
    "security",
    "transaction",
    "cache",
    "diagnostic",
    "recovery",
    "refusal",
}
REQUIRED_SUITES = {
    "bootstrap",
    "catalog",
    "lifecycle",
    "constraints",
    "query",
    "dml",
    "datatypes",
    "udr",
    "security",
    "language",
    "wire",
    "driver",
    "storage",
    "fuzz",
    "soak",
    "release",
}
REQUIRED_ROUTES = {"embedded", "local_ipc", "inet_listener", "parser_lowering"}
REQUIRED_PAGE_SIZES = {8192, 16384, 32768, 65536, 131072}
REQUIRED_LANGUAGES = {"sbsql_v3_en", "sbsql_v3_unicode", "sbsql_v3_locale_fr", "sbsql_v3_locale_ja"}
REQUIRED_SECURITY = {"required_grant", "revoked_grant_refusal", "redacted_diagnostic"}
HASH_RE = re.compile(r"^[0-9a-f]{64}$")
NETWORK_IMPORT_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    r"urllib|http|socket|ssl|ftplib|smtplib|telnetlib|imaplib|poplib|"
    r"nntplib|xmlrpc|webbrowser|requests|httpx|aiohttp|urllib3|pycurl|"
    r"paramiko|asyncssh|botocore|boto3"
    r")(?:[.\s]|$)",
    re.MULTILINE,
)
BLOCKED_WORDS = (
    "".join(("to", "do")),
    "tbd",
    "".join(("place", "holder")),
    "".join(("skip", "ped")),
    "".join(("wai", "ved")),
    "".join(("generated", "_only")),
    "".join(("manual", "_only")),
    "".join(("file", "_presence")),
    "parser_owned_finality",
    "parser_owned_storage",
)


class GateError(AssertionError):
    pass


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def id_set_hash(rows: list[dict[str, str]]) -> str:
    return sha256_text("\n".join(sorted(row["surface_id"] for row in rows)))


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def row_hash_ok(row: dict[str, Any]) -> bool:
    observed = row.get("row_sha256")
    if not isinstance(observed, str) or not HASH_RE.fullmatch(observed):
        return False
    material = dict(row)
    del material["row_sha256"]
    return sha256_text(canonical_json(material)) == observed


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"manifest missing: {path}")
    with path.open(encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise GateError("manifest root must be a JSON object")
    return payload


def walk_strings(value: Any) -> list[str]:
    if isinstance(value, dict):
        out: list[str] = []
        for item in value.values():
            out.extend(walk_strings(item))
        return out
    if isinstance(value, list):
        out = []
        for item in value:
            out.extend(walk_strings(item))
        return out
    if isinstance(value, str):
        return [value]
    return []


def scan_no_network(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    findings = []
    for match in NETWORK_IMPORT_RE.finditer(text):
        line = text[: match.start()].count("\n") + 1
        findings.append(f"{path}:{line}: forbidden network import {match.group(1)!r}")
    return findings


def validate_determinism(repo: Path, manifest: Path, errors: list[str]) -> None:
    generator = repo / GENERATOR
    require(generator.is_file(), f"generator missing: {GENERATOR}", errors)
    if not generator.is_file():
        return
    errors.extend(scan_no_network(generator))
    errors.extend(scan_no_network(Path(__file__)))
    with tempfile.TemporaryDirectory(prefix="sml_041_046_product_") as tmp:
        product_out = Path(tmp) / "product.json"
        hardening_out = Path(tmp) / "hardening.json"
        result = subprocess.run(
            [
                sys.executable,
                str(generator),
                "--repo-root",
                str(repo),
                "--product-output",
                str(product_out),
                "--hardening-output",
                str(hardening_out),
            ],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            errors.append(
                "manifest regeneration failed "
                f"exit={result.returncode} stderr={result.stderr.strip()[:240]}"
            )
            return
        if product_out.read_bytes() != manifest.read_bytes():
            errors.append("tracked SML-041..046 product manifest differs from deterministic regeneration")


def validate_source_artifacts(repo: Path, payload: dict[str, Any], errors: list[str]) -> dict[str, list[dict[str, str]]]:
    tables: dict[str, list[dict[str, str]]] = {}
    artifact_records = payload.get("source_artifacts", [])
    require(isinstance(artifact_records, list), "source_artifacts must be a list", errors)
    by_name = {Path(row.get("path", "")).name: row for row in artifact_records if isinstance(row, dict)}
    for name in SURFACE_ARTIFACTS:
        path = repo / ARTIFACT_ROOT / name
        rows = read_csv(path)
        tables[name] = rows
        record = by_name.get(name)
        require(isinstance(record, dict), f"source artifact record missing for {name}", errors)
        if not isinstance(record, dict):
            continue
        require(record.get("sha256") == sha256_file(path), f"{name} sha256 mismatch", errors)
        require(record.get("row_count") == len(rows), f"{name} row_count mismatch", errors)
        require(record.get("surface_id_set_sha256") == id_set_hash(rows), f"{name} surface id hash mismatch", errors)
        require(row_hash_ok(record), f"{name} artifact row hash mismatch", errors)
    id_hashes = {id_set_hash(rows) for rows in tables.values()}
    require(len(id_hashes) == 1, "admitted surface id sets differ across source artifacts", errors)
    return tables


def validate_manifest_shape(payload: dict[str, Any], errors: list[str]) -> None:
    require(payload.get("schema_version") == SCHEMA_VERSION, "schema_version mismatch", errors)
    require(payload.get("network_dependency") is False, "manifest must be no-network", errors)
    observed_hash = payload.get("manifest_sha256")
    require(isinstance(observed_hash, str) and HASH_RE.fullmatch(observed_hash), "manifest_sha256 missing", errors)
    material = dict(payload)
    material.pop("manifest_sha256", None)
    require(observed_hash == sha256_text(canonical_json(material)), "manifest_sha256 mismatch", errors)
    for text in walk_strings(payload):
        lowered = text.lower()
        for word in BLOCKED_WORDS:
            require(word not in lowered, f"blocked manifest state token {word!r} found in {text!r}", errors)
        require("".join(("http", "://")) not in lowered and
                "".join(("https", "://")) not in lowered and
                "".join(("ftp", "://")) not in lowered,
                f"external locator found in manifest text {text!r}", errors)


def validate_variations(payload: dict[str, Any], tables: dict[str, list[dict[str, str]]], errors: list[str]) -> None:
    rows = payload.get("product_surface_variation_manifest", [])
    require(isinstance(rows, list) and rows, "product surface variation manifest must be non-empty", errors)
    if not isinstance(rows, list):
        return
    by_class = {row.get("variation_class"): row for row in rows if isinstance(row, dict)}
    require(set(by_class) == REQUIRED_VARIATIONS,
            f"variation classes mismatch: {sorted(by_class)}", errors)
    admitted = len(tables["SBSQL_LANGUAGE_ELEMENT_MANIFEST.csv"])
    for klass, row in by_class.items():
        require(row.get("variation_status") == "closed", f"{klass} variation is not closed", errors)
        require(row_hash_ok(row), f"{klass} variation row hash mismatch", errors)
        if klass != "refusal":
            require(row.get("surface_count", 0) > 0, f"{klass} variation has no coverage", errors)
        if klass in {
            "grammar",
            "semantic",
            "route",
            "language",
            "page_size",
            "security",
            "transaction",
            "cache",
            "diagnostic",
            "recovery",
        }:
            require(row.get("surface_count") == admitted, f"{klass} variation must cover all admitted surfaces", errors)


def validate_suite_corpus(repo: Path, payload: dict[str, Any], errors: list[str]) -> None:
    rows = payload.get("product_qa_script_corpus", [])
    require(isinstance(rows, list) and rows, "script corpus must be non-empty", errors)
    if not isinstance(rows, list):
        return
    by_suite = {row.get("suite_class"): row for row in rows if isinstance(row, dict)}
    require(set(by_suite) == REQUIRED_SUITES, f"suite classes mismatch: {sorted(by_suite)}", errors)
    for suite, row in by_suite.items():
        require(row.get("suite_status") == "closed", f"{suite} suite is not closed", errors)
        require(row.get("network_dependency") is False, f"{suite} suite declares network dependency", errors)
        require(row_hash_ok(row), f"{suite} suite row hash mismatch", errors)
        material = {
            "suite_class": row.get("suite_class"),
            "ctest_label": row.get("ctest_label"),
            "evidence_paths": row.get("evidence_paths"),
            "network_dependency": row.get("network_dependency"),
            "suite_status": row.get("suite_status"),
        }
        require(row.get("script_hash") == sha256_text(canonical_json(material)),
                f"{suite} script hash mismatch", errors)
        for evidence_path in row.get("evidence_paths", []):
            require((repo / evidence_path).is_file(), f"{suite} evidence path missing: {evidence_path}", errors)


def validate_route_result(payload: dict[str, Any], tables: dict[str, list[dict[str, str]]], errors: list[str]) -> None:
    rows = payload.get("route_result_evidence", [])
    require(isinstance(rows, list) and len(rows) == 2, "route_result_evidence must have two classifications", errors)
    if not isinstance(rows, list):
        return
    admitted = len(tables["SBSQL_SURFACE_RELEASE_DECLARATION.csv"])
    total = 0
    lower_level_count = 0
    for row in rows:
        require(row_hash_ok(row), f"{row.get('classification')} row hash mismatch", errors)
        require(isinstance(row.get("result_evidence_sha256"), str) and HASH_RE.fullmatch(row["result_evidence_sha256"]),
                f"{row.get('classification')} missing result evidence hash", errors)
        total += int(row.get("surface_count", 0))
        if row.get("lower_level_only") is True:
            lower_level_count += int(row.get("surface_count", 0))
            require(row.get("classification_status") == "closed_lower_level_only",
                    "lower-level-only classification must be explicit and closed", errors)
        else:
            require(row.get("classification_status") == "closed_executable",
                    "executable route result classification must be closed", errors)
    release_rows = tables["SBSQL_SURFACE_RELEASE_DECLARATION.csv"]
    expected_lower = sum(1 for row in release_rows if row["final_status"] == "cluster_provider_route_passed")
    require(total == admitted, "route/result classifications do not cover all admitted surfaces", errors)
    require(lower_level_count == expected_lower, "lower-level-only count does not match release declaration", errors)


def validate_route_matrix(payload: dict[str, Any], errors: list[str]) -> None:
    matrix = payload.get("route_page_language_security_matrix", {})
    require(isinstance(matrix, dict), "route/page/language/security matrix must be an object", errors)
    if not isinstance(matrix, dict):
        return
    rows = matrix.get("rows", [])
    require(isinstance(rows, list), "matrix rows must be a list", errors)
    if not isinstance(rows, list):
        return
    expected_count = len(REQUIRED_ROUTES) * len(REQUIRED_PAGE_SIZES) * len(REQUIRED_LANGUAGES) * len(REQUIRED_SECURITY)
    require(matrix.get("combination_count") == expected_count, "matrix combination_count mismatch", errors)
    require(len(rows) == expected_count, "matrix row count mismatch", errors)
    observed = set()
    for row in rows:
        route = row.get("route")
        page_size = row.get("page_size")
        language = row.get("language_profile")
        security = row.get("security_profile")
        observed.add((route, page_size, language, security))
        require(route in REQUIRED_ROUTES, f"unexpected route {route}", errors)
        require(page_size in REQUIRED_PAGE_SIZES, f"unexpected page size {page_size}", errors)
        require(language in REQUIRED_LANGUAGES, f"unexpected language profile {language}", errors)
        require(security in REQUIRED_SECURITY, f"unexpected security profile {security}", errors)
        require(row.get("external_network_dependency") is False, f"{row.get('matrix_id')} declares network dependency", errors)
        require(row.get("parser_storage_authority") == "none", f"{row.get('matrix_id')} gives parser storage authority", errors)
        require(row.get("parser_owns_finality") is False, f"{row.get('matrix_id')} gives parser finality", errors)
        require(row.get("matrix_status") == "closed", f"{row.get('matrix_id')} is not closed", errors)
        require(row.get("expected_contract_sha256") == sha256_text(row.get("expected_contract", "")),
                f"{row.get('matrix_id')} contract hash mismatch", errors)
        require(row_hash_ok(row), f"{row.get('matrix_id')} row hash mismatch", errors)
    expected = {
        (route, page, language, security)
        for route in REQUIRED_ROUTES
        for page in REQUIRED_PAGE_SIZES
        for language in REQUIRED_LANGUAGES
        for security in REQUIRED_SECURITY
    }
    require(observed == expected, "matrix does not equal required Cartesian product", errors)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[6])
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name(DEFAULT_MANIFEST))
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    manifest = args.manifest.resolve()
    errors: list[str] = []

    try:
        payload = load_manifest(manifest)
        validate_determinism(repo, manifest, errors)
        validate_manifest_shape(payload, errors)
        tables = validate_source_artifacts(repo, payload, errors)
        validate_variations(payload, tables, errors)
        validate_suite_corpus(repo, payload, errors)
        validate_route_result(payload, tables, errors)
        validate_route_matrix(payload, errors)
    except Exception as exc:  # noqa: BLE001 - gate should report a single closed failure.
        print(f"sbsql_sml_041_046_product_qa_manifest_gate=failed: {exc}", file=sys.stderr)
        return 1

    print(
        "sbsql_sml_041_046_product_qa_manifest_gate "
        f"surface_count={payload.get('admitted_surface_set', {}).get('surface_count')} "
        f"suites={len(payload.get('product_qa_script_corpus', []))} "
        f"matrix_rows={payload.get('route_page_language_security_matrix', {}).get('combination_count')}"
    )
    if errors:
        print("sbsql_sml_041_046_product_qa_manifest_gate=failed", file=sys.stderr)
        for error in errors[:80]:
            print(f"  {error}", file=sys.stderr)
        if len(errors) > 80:
            print(f"  ... {len(errors) - 80} more errors", file=sys.stderr)
        return 1
    print("sbsql_sml_041_046_product_qa_manifest_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
