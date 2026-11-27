#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-057 crypto/hash and pgcrypto fixture gate."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path


SURFACE_REGISTRY = "public_input_snapshot/SBSQL_SURFACE_REGISTRY.csv"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_057_CRYPTO_HASH_FIXTURES.csv"

TARGETS = {
    "SBSQL-697D0080DA8E": "sb.crypto.argon2",
    "SBSQL-F2657259D869": "sb.crypto.armor",
    "SBSQL-A475C1402E1D": "sb.crypto.armor_binary",
    "SBSQL-DEAE160F496C": "sb.crypto.bcrypt",
    "SBSQL-333906CB50B9": "sb.crypto.blake2b",
    "SBSQL-EDA08840598E": "sb.crypto.blake3",
    "SBSQL-38360AA175AA": "sb.crypto.crypt",
    "SBSQL-46DCE02DBE41": "sb.crypto.crypt_password_salt",
    "SBSQL-0253C933634F": "sb.crypto.dearmor",
    "SBSQL-DFF3ADE173F5": "sb.crypto.dearmor_text",
    "SBSQL-98AC558CD662": "sb.crypto.gen_random_bytes",
    "SBSQL-7612CF167F37": "sb.crypto.gen_random_bytes_n",
    "SBSQL-F609742097AE": "sb.crypto.gen_random_uuid",
    "SBSQL-5F23337B660A": "sb.crypto.gen_salt",
    "SBSQL-058471F02F03": "sb.crypto.gen_salt_algo",
    "SBSQL-6144531FD80E": "sb.crypto.hmac",
    "SBSQL-D0FF02C4CDDE": "sb.crypto.hmac_value_key_algo",
    "SBSQL-AF4E7BFEFDD1": "sb.crypto.pgcrypto",
    "SBSQL-6358314B6883": "sb.crypto.pgp_pub_decrypt",
    "SBSQL-2854D8B0790B": "sb.crypto.pgp_pub_encrypt",
    "SBSQL-6DBE85C4B814": "sb.crypto.pgp_sym_decrypt",
    "SBSQL-C98EC981ACD7": "sb.crypto.pgp_sym_encrypt",
    "SBSQL-C8996122850A": "sb.crypto.scrypt",
    "SBSQL-BD3080D87EA5": "sb.crypto.sha3_256",
    "SBSQL-51BB6328126C": "sb.crypto.sha3_512",
    "SBSQL-4AD05EF7474D": "sb.crypto.xxhash64",
    "SBSQL-B75400EDF4FB": "sb.crypto.xxhash64_value_seed",
}

DEPENDENCY_SURFACES = {
    "SBSQL-697D0080DA8E",
    "SBSQL-DEAE160F496C",
    "SBSQL-EDA08840598E",
    "SBSQL-38360AA175AA",
    "SBSQL-46DCE02DBE41",
}

# OLD (to 2026-12-20): each invalid-input fixture had to declare the generic SB_DIAG_FUNCTION_INVALID_INPUT.
# NEW: the engine answers each cause with its family's specific code (owner Q193: one distinct code per cause).
REQUIRED_INVALID_FIXTURES = {
    "SBSFC057-dearmor-invalid": "CRYPTO.ARMOR.INVALID_INPUT",
    "SBSFC057-hmac-bad-algo": "CRYPTO.HMAC.UNSUPPORTED_ALGORITHM",
    "SBSFC057-gen-random-bytes-too-large": "CRYPTO.RNG.INVALID_LENGTH",
}

# OLD (to 2026-12-20): pgcrypto and the four pgp_* rows were "positive" fixtures (a compatibility-envelope marker and the retired SBPGP1 envelope).
# NEW (owner 2026-11-26, PGP 2 a / PGP 3 a): they are REFUSAL fixtures. The pgp_* functions are refused until real OpenPGP is wired; pgcrypto is the
# package capability spelling, not a callable value. A refusal fixture never declares a result.
REFUSAL_SURFACES = {
    "SBSQL-AF4E7BFEFDD1": "CRYPTO.PACKAGE.NOT_CALLABLE",  # pgcrypto
    "SBSQL-6358314B6883": "CRYPTO.PGP.PROFILE_UNAVAILABLE",  # pgp_pub_decrypt
    "SBSQL-2854D8B0790B": "CRYPTO.PGP.PROFILE_UNAVAILABLE",  # pgp_pub_encrypt
    "SBSQL-6DBE85C4B814": "CRYPTO.PGP.PROFILE_UNAVAILABLE",  # pgp_sym_decrypt
    "SBSQL-C98EC981ACD7": "CRYPTO.PGP.PROFILE_UNAVAILABLE",  # pgp_sym_encrypt
}

ALLOWED_DESCRIPTORS = {
    "binary",
    "character",
    "uint64",
    "uuid",
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    surfaces = {row["surface_id"]: row for row in read_csv(root / SURFACE_REGISTRY)}
    fixtures = read_csv(root / FIXTURES)
    seed_text = (root / SEED_REGISTRY).read_text(encoding="utf-8")
    registered_ids = set(re.findall(r'"(sb\.[^"]+)"', seed_text))

    errors: list[str] = []
    covered_by_surface: dict[str, set[str]] = {surface_id: set() for surface_id in TARGETS}
    invalid_seen: set[str] = set()
    fixture_ids: set[str] = set()

    for row in fixtures:
        fixture_id = row.get("fixture_id", "")
        if not fixture_id:
            errors.append("fixture row missing fixture_id")
            continue
        if fixture_id in fixture_ids:
            errors.append(f"{fixture_id}: duplicate fixture_id")
        fixture_ids.add(fixture_id)

        surface_id = row.get("surface_id", "")
        function_id = row.get("function_id", "")
        canonical = row.get("canonical_builtin_id", "")
        case_kind = row.get("case_kind", "")
        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()
        descriptor = row.get("expected_result_descriptor", "")

        try:
            parsed_args = json.loads(row.get("arguments_json", ""))
            if not isinstance(parsed_args, list):
                errors.append(f"{fixture_id}: arguments_json must be a JSON list")
        except json.JSONDecodeError as exc:
            errors.append(f"{fixture_id}: invalid arguments_json: {exc}")

        expected_function_id = TARGETS.get(surface_id)
        if expected_function_id is None:
            errors.append(f"{fixture_id}: unexpected surface_id {surface_id}")
            continue
        if function_id != expected_function_id or canonical != expected_function_id:
            errors.append(f"{fixture_id}: function/canonical id mismatch for {surface_id}")
        if function_id not in registered_ids:
            errors.append(f"{fixture_id}: {function_id} not registered in function_seed_registry")

        surface = surfaces.get(surface_id)
        if surface is None:
            errors.append(f"{fixture_id}: surface_id missing from registry")
        else:
            if surface.get("status") != "native_now":
                errors.append(f"{fixture_id}: surface_id is not native_now")
            if surface.get("sblr_operation_family") != "sblr.expression.runtime.v3":
                errors.append(f"{fixture_id}: unexpected SBLR family {surface.get('sblr_operation_family')}")

        if case_kind == "invalid_input":
            invalid_seen.add(fixture_id)
            required_code = REQUIRED_INVALID_FIXTURES.get(fixture_id)
            if required_code is None:
                errors.append(f"{fixture_id}: unexpected invalid_input fixture")
            elif expected_diag != required_code:
                errors.append(f"{fixture_id}: invalid_input must declare {required_code}")
            continue

        if case_kind == "refusal":
            required_code = REFUSAL_SURFACES.get(surface_id)
            if required_code is None:
                errors.append(f"{fixture_id}: refusal is not expected for {surface_id}")
            elif expected_diag != required_code:
                errors.append(f"{fixture_id}: refusal must declare {required_code}")
            if row.get("expected_result_json", "") or descriptor:
                errors.append(f"{fixture_id}: a refusal fixture must not declare a result")
            covered_by_surface[surface_id].add(fixture_id)
            continue

        if case_kind == "dependency_unavailable":
            if surface_id not in DEPENDENCY_SURFACES:
                errors.append(f"{fixture_id}: dependency_unavailable is not expected for {surface_id}")
            if expected_diag != "SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE":
                errors.append(f"{fixture_id}: dependency_unavailable must declare SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE")
            covered_by_surface[surface_id].add(fixture_id)
            continue

        if case_kind != "positive":
            errors.append(f"{fixture_id}: unexpected case_kind {case_kind}")
            continue
        if surface_id in DEPENDENCY_SURFACES:
            errors.append(f"{fixture_id}: dependency-gated surface must use dependency_unavailable case_kind")
        if surface_id in REFUSAL_SURFACES:
            errors.append(f"{fixture_id}: pgcrypto and the pgp_* surfaces must use the refusal case_kind, never positive")
        if expected_diag:
            errors.append(f"{fixture_id}: positive fixture must not declare diagnostic")
        if descriptor not in ALLOWED_DESCRIPTORS:
            errors.append(f"{fixture_id}: unexpected positive descriptor {descriptor}")
        if not row.get("expected_result_json", ""):
            errors.append(f"{fixture_id}: positive fixture must declare expected_result_json")
        covered_by_surface[surface_id].add(fixture_id)

    for surface_id, fixtures_for_surface in covered_by_surface.items():
        if not fixtures_for_surface:
            errors.append(f"{surface_id}: missing SBSFC-057 fixture")
    for fixture_id in sorted(set(REQUIRED_INVALID_FIXTURES) - invalid_seen):
        errors.append(f"{fixture_id}: missing required invalid-input fixture")

    print(
        "sbsql_sbsfc_057_crypto_hash_fixture_gate "
        f"fixtures={len(fixtures)} surfaces={len(TARGETS)} errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_057_crypto_hash_fixture_gate=failed", file=sys.stderr)
        for error in errors[:100]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_057_crypto_hash_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
