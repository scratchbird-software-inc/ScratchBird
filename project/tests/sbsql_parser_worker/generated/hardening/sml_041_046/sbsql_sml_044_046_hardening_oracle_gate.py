#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-044 and SML-046 hardening oracle gate."""

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


SCHEMA_VERSION = "sbsql.sml_044_046.hardening_oracle.v1"
GENERATOR = "project/tools/sb_parser_gen/generate_sbsql_sml_041_046_product_qa_gates.py"
DEFAULT_ORACLE = "SML_044_046_HARDENING_ORACLE.json"
REQUIRED_EDGE_CLASSES = {
    "boundary",
    "null",
    "overflow",
    "hidden",
    "missing",
    "stale_cache",
    "invalid_input",
    "resource_refusal",
    "redaction",
    "no_disclosure",
    "no_mutation",
}
REQUIRED_PROOF_CLASSES = {"fuzz", "property", "metamorphic", "round_trip"}
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


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_text(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


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


def load_oracle(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise RuntimeError(f"oracle missing: {path}")
    with path.open(encoding="utf-8") as handle:
        payload = json.load(handle)
    if not isinstance(payload, dict):
        raise RuntimeError("oracle root must be a JSON object")
    return payload


def validate_determinism(repo: Path, oracle: Path, errors: list[str]) -> None:
    generator = repo / GENERATOR
    require(generator.is_file(), f"generator missing: {GENERATOR}", errors)
    if not generator.is_file():
        return
    errors.extend(scan_no_network(generator))
    errors.extend(scan_no_network(Path(__file__)))
    with tempfile.TemporaryDirectory(prefix="sml_044_046_hardening_") as tmp:
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
                "oracle regeneration failed "
                f"exit={result.returncode} stderr={result.stderr.strip()[:240]}"
            )
            return
        if hardening_out.read_bytes() != oracle.read_bytes():
            errors.append("tracked SML-044/046 oracle differs from deterministic regeneration")


def validate_shape(payload: dict[str, Any], errors: list[str]) -> None:
    require(payload.get("schema_version") == SCHEMA_VERSION, "schema_version mismatch", errors)
    require(payload.get("network_dependency") is False, "oracle must be no-network", errors)
    observed_hash = payload.get("manifest_sha256")
    require(isinstance(observed_hash, str) and HASH_RE.fullmatch(observed_hash), "manifest_sha256 missing", errors)
    material = dict(payload)
    material.pop("manifest_sha256", None)
    require(observed_hash == sha256_text(canonical_json(material)), "manifest_sha256 mismatch", errors)
    for text in walk_strings(payload):
        lowered = text.lower()
        for word in BLOCKED_WORDS:
            require(word not in lowered, f"blocked oracle state token {word!r} found in {text!r}", errors)
        require("".join(("http", "://")) not in lowered and
                "".join(("https", "://")) not in lowered and
                "".join(("ftp", "://")) not in lowered,
                f"external locator found in oracle text {text!r}", errors)


def validate_edges(payload: dict[str, Any], errors: list[str]) -> None:
    rows = payload.get("edge_case_negative_oracles", [])
    require(isinstance(rows, list) and rows, "edge case oracle rows required", errors)
    if not isinstance(rows, list):
        return
    by_class = {row.get("case_class"): row for row in rows if isinstance(row, dict)}
    require(set(by_class) == REQUIRED_EDGE_CLASSES, f"edge case classes mismatch: {sorted(by_class)}", errors)
    for klass, row in by_class.items():
        require(row.get("oracle_status") == "closed_refusal", f"{klass} oracle is not closed_refusal", errors)
        require(row.get("no_mutation_required") is True, f"{klass} must require no mutation", errors)
        require(row.get("no_disclosure_required") is True, f"{klass} must require no disclosure", errors)
        require(row.get("parser_storage_authority") == "none", f"{klass} gives parser storage authority", errors)
        require(row.get("parser_owns_finality") is False, f"{klass} gives parser finality", errors)
        require(isinstance(row.get("expected_diagnostic_code"), str) and row["expected_diagnostic_code"].startswith("SBSQL."),
                f"{klass} diagnostic code missing", errors)
        expected = (
            f"{klass}:{row.get('expected_diagnostic_code')}:{row.get('expected_contract')}:"
            "no_mutation:no_disclosure"
        )
        require(row.get("expected_contract_sha256") == sha256_text(expected),
                f"{klass} expected contract hash mismatch", errors)
        require(row_hash_ok(row), f"{klass} row hash mismatch", errors)


def validate_fuzz(payload: dict[str, Any], errors: list[str]) -> None:
    rows = payload.get("fuzz_property_metamorphic_round_trip_evidence", [])
    require(isinstance(rows, list) and rows, "fuzz/property evidence rows required", errors)
    if not isinstance(rows, list):
        return
    by_class = {row.get("proof_class"): row for row in rows if isinstance(row, dict)}
    require(set(by_class) == REQUIRED_PROOF_CLASSES, f"proof classes mismatch: {sorted(by_class)}", errors)
    seeds = set()
    for proof_class, row in by_class.items():
        seed = row.get("seed")
        require(isinstance(seed, int) and seed > 0, f"{proof_class} seed invalid", errors)
        require(seed not in seeds, f"{proof_class} seed is duplicated", errors)
        seeds.add(seed)
        require(row.get("reproducible_status") == "closed_reproducible",
                f"{proof_class} status is not closed_reproducible", errors)
        require(row.get("external_network_dependency") is False,
                f"{proof_class} declares network dependency", errors)
        require(row.get("parser_storage_authority") == "none",
                f"{proof_class} gives parser storage authority", errors)
        require(row.get("parser_owns_finality") is False,
                f"{proof_class} gives parser finality", errors)
        require(isinstance(row.get("max_iterations"), int) and row["max_iterations"] >= 1,
                f"{proof_class} max_iterations invalid", errors)
        require(row.get("diagnostic_sha256") == sha256_text(row.get("expected_diagnostic_code", "")),
                f"{proof_class} diagnostic hash mismatch", errors)
        contract = (
            f"{proof_class}:{seed}:{row.get('input_class')}:"
            f"{row.get('expected_diagnostic_code')}:{row.get('metamorphic_relation')}"
        )
        require(row.get("reproducible_evidence_sha256") == sha256_text(contract),
                f"{proof_class} reproducible evidence hash mismatch", errors)
        require(row_hash_ok(row), f"{proof_class} row hash mismatch", errors)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[6])
    parser.add_argument("--oracle", type=Path, default=Path(__file__).with_name(DEFAULT_ORACLE))
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    oracle = args.oracle.resolve()
    errors: list[str] = []

    try:
        payload = load_oracle(oracle)
        validate_determinism(repo, oracle, errors)
        validate_shape(payload, errors)
        validate_edges(payload, errors)
        validate_fuzz(payload, errors)
    except Exception as exc:  # noqa: BLE001 - gate should report a single closed failure.
        print(f"sbsql_sml_044_046_hardening_oracle_gate=failed: {exc}", file=sys.stderr)
        return 1

    print(
        "sbsql_sml_044_046_hardening_oracle_gate "
        f"edge_cases={len(payload.get('edge_case_negative_oracles', []))} "
        f"fuzz_cases={len(payload.get('fuzz_property_metamorphic_round_trip_evidence', []))}"
    )
    if errors:
        print("sbsql_sml_044_046_hardening_oracle_gate=failed", file=sys.stderr)
        for error in errors[:80]:
            print(f"  {error}", file=sys.stderr)
        if len(errors) > 80:
            print(f"  ... {len(errors) - 80} more errors", file=sys.stderr)
        return 1
    print("sbsql_sml_044_046_hardening_oracle_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
