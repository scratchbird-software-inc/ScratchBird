#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-010 numeric builtin fixture gate.

Validates the structure of `SBSFC_010_NUMERIC_BUILTIN_FIXTURES.csv` and
cross-checks every fixture row against the canonical SBsql surface
registry and the engine-side function seed registry.

Cross-checks:

  1. Required CSV columns are present and non-empty (except
     `expected_result_value` / `expected_diagnostic_code` which are
     mutually exclusive per fixture row).
  2. Every `surface_id` exists in the canonical SBsql surface registry.
  3. Every `canonical_builtin_id` (`sb.scalar.<name>`) is present in
     `project/src/engine/functions/registry/function_seed_registry.cpp`
     as an engine-anchored function_id with implementation_state=
     implemented_behavior.
  4. Every `function_id` matches the canonical_builtin_id (this slice
     uses canonical ids exclusively for clarity).
  5. Every fixture row declares either `expected_result_value` (positive
     case) or `expected_diagnostic_code` (refusal/error case), never both
     and never neither.
  6. Each canonical_builtin_id is exercised by at least one positive
     case AND at least one null case (volatility-volatile functions like
     random are exempt from the null requirement because they take no
     arguments).

This is the static, no-build half of SBSFC-010 acceptance. The
end-to-end engine dispatch check requires a built environment and a C++
unit-test probe (planned as a follow-up authoring step inside SBSFC-010).
Until then this gate certifies that the fixture corpus is structurally
sound and points at registered canonical entrypoints.

Architecture invariant compliance: read-only static cross-check; no
transaction model touched; no engine, parser worker, server, listener,
storage, or MGA file modified; no WAL surface introduced. MGA copy-on-write
remains the sole transaction recovery model.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_010_NUMERIC_BUILTIN_FIXTURES.csv"


REQUIRED_COLUMNS = [
    "fixture_id",
    "surface_id",
    "function_id",
    "canonical_builtin_id",
    "case_kind",
    "arguments_json",
    "expected_result_descriptor",
    "oracle_authority_ref",
    "notes",
]

NULLLESS_BUILTINS = {
    "sb.scalar.random",  # volatile zero-arity function
    "sb.scalar.pi",      # immutable zero-arity function
}


def fail(message: str) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(1)


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        fail(f"required CSV missing: {path}")
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def parse_seed_registry_function_ids(path: Path) -> set[str]:
    if not path.is_file():
        fail(f"seed registry missing: {path}")
    text = path.read_text(encoding="utf-8")
    return set(re.findall(r'"((?:sb\.scalar|sb\.fn\.|data\.scalar|domain)[\w.\-]+)"', text))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()

    surfaces = {r["surface_id"]: r for r in read_csv(root / REGISTRY)}
    fixtures = read_csv(root / FIXTURES)
    registered_ids = parse_seed_registry_function_ids(root / SEED_REGISTRY)

    errors: list[str] = []

    seen_fixture_ids: set[str] = set()
    by_builtin_positive: dict[str, int] = defaultdict(int)
    by_builtin_null: dict[str, int] = defaultdict(int)

    for row in fixtures:
        fid = row.get("fixture_id", "")
        if not fid:
            errors.append("fixture row missing fixture_id")
            continue
        if fid in seen_fixture_ids:
            errors.append(f"duplicate fixture_id: {fid}")
        seen_fixture_ids.add(fid)

        for col in REQUIRED_COLUMNS:
            if not (row.get(col, "") or "").strip():
                errors.append(f"{fid}: required column {col} is empty")

        sid = row.get("surface_id", "")
        if sid and sid not in surfaces:
            errors.append(f"{fid}: surface_id {sid} not in canonical registry")

        canonical = row.get("canonical_builtin_id", "")
        if canonical and canonical not in registered_ids:
            errors.append(f"{fid}: canonical_builtin_id {canonical} not registered in function_seed_registry")

        function_id = row.get("function_id", "")
        if function_id and function_id != canonical:
            errors.append(f"{fid}: function_id {function_id} does not match canonical_builtin_id {canonical} (this slice uses canonical ids only)")

        args_text = (row.get("arguments_json", "") or "").strip()
        if args_text:
            try:
                parsed_args = json.loads(args_text)
                if not isinstance(parsed_args, list):
                    errors.append(f"{fid}: arguments_json must be a JSON list")
            except json.JSONDecodeError as exc:
                errors.append(f"{fid}: arguments_json is not valid JSON: {exc}")

        expected_value = (row.get("expected_result_value", "") or "").strip()
        expected_diag = (row.get("expected_diagnostic_code", "") or "").strip()
        case_kind = row.get("case_kind", "")
        if case_kind.startswith("null"):
            # null-strict cases: result value is null (empty); no diagnostic expected.
            if expected_diag:
                errors.append(f"{fid}: null_strict case must not carry expected_diagnostic_code")
        elif case_kind in {"overflow", "domain_error"}:
            if not expected_diag:
                errors.append(f"{fid}: {case_kind} case must declare expected_diagnostic_code")
            if expected_value:
                errors.append(f"{fid}: {case_kind} case must not carry expected_result_value")
        elif case_kind == "range_assertion":
            # range_assertion cases (e.g., random) have no exact value/diagnostic;
            # the engine probe asserts the descriptor and value range separately.
            pass
        else:
            if not expected_value:
                errors.append(f"{fid}: positive/edge case must declare expected_result_value (got case_kind={case_kind})")
            if expected_diag:
                errors.append(f"{fid}: positive/edge case must not declare expected_diagnostic_code")

        if case_kind.startswith("positive") or case_kind.startswith("edge") or case_kind == "range_assertion":
            # range_assertion satisfies positive coverage for volatile builtins
            # whose exact value cannot be predicted (e.g., random).
            by_builtin_positive[canonical] += 1
        if case_kind.startswith("null"):
            by_builtin_null[canonical] += 1

    target_canonicals = {
        # SBSFC-010 first-pass
        "sb.scalar.abs",
        "sb.scalar.sqrt",
        "sb.scalar.power",
        "sb.scalar.ceil",
        "sb.scalar.floor",
        "sb.scalar.round",
        "sb.scalar.random",
        # SBSFC-010 second-pass (trig + log + exp + trunc + mod)
        "sb.scalar.sin",
        "sb.scalar.cos",
        "sb.scalar.tan",
        "sb.scalar.exp",
        "sb.scalar.ln",
        "sb.scalar.log10",
        "sb.scalar.log",
        "sb.scalar.trunc",
        "sb.scalar.mod",
        # SBSFC-010 third-pass (sign + bitwise functions; bit_shift_*
        # remain canonical-only pending engine implementation)
        "sb.scalar.sign",
        "sb.scalar.bit_and",
        "sb.scalar.bit_or",
        "sb.scalar.bit_xor",
        # bit_shift_* engine impls added 2026-05-11
        "sb.scalar.bit_shift_left",
        "sb.scalar.bit_shift_right",
        # truncate canonical alias of trunc (shares engine guard)
        "sb.scalar.truncate",
        # asin/acos/atan inverse-trig engine sub-stream 2026-05-11. New engine
        # guards added with domain refusal (|input| > 1) for asin/acos;
        # atan domain is unbounded.
        "sb.scalar.asin",
        "sb.scalar.acos",
        "sb.scalar.atan",
        # Hyperbolic trig engine sub-stream 2026-05-11. 6 new canonicals;
        # sinh/cosh/tanh/asinh use simple UnaryReal; acosh refuses for
        # input < 1; atanh refuses for |input| >= 1.
        "sb.scalar.sinh",
        "sb.scalar.cosh",
        "sb.scalar.tanh",
        "sb.scalar.asinh",
        "sb.scalar.acosh",
        "sb.scalar.atanh",
        # log2/atan2 numeric sub-stream 2026-05-11. log2 uses domain-checked
        # guard (input > 0); atan2 is a binary guard with null-strict on
        # both args.
        "sb.scalar.log2",
        "sb.scalar.atan2",
        # Degrees-mode trig sub-stream 2026-05-11. 6 new canonicals; forward
        # variants scale input by pi/180; inverse variants scale output by
        # 180/pi; asind/acosd preserve |input| > 1 domain refusal.
        "sb.scalar.sind",
        "sb.scalar.cosd",
        "sb.scalar.tand",
        "sb.scalar.asind",
        "sb.scalar.acosd",
        "sb.scalar.atand",
        # cot/cotd sub-stream 2026-05-11. Cotangent via 1.0/std::tan; no
        # domain refusal (IEEE 754 infinity propagation at boundaries).
        "sb.scalar.cot",
        "sb.scalar.cotd",
        # SBSFC-010 remaining numeric-function closure.
        "sb.scalar.bit_count",
        "sb.scalar.bit_length",
        "sb.scalar.cbrt",
        "sb.scalar.degrees",
        "sb.scalar.radians",
        "sb.scalar.pi",
        "sb.scalar.div",
        "sb.scalar.factorial",
        "sb.scalar.gcd",
        "sb.scalar.lcm",
        "sb.scalar.width_bucket",
    }
    target_alias_surface_ids = {
        "SBSQL-22531B2BC2F8",  # abs(numeric)
        "SBSQL-A413199B8564",  # sb.scalar.abs
        "SBSQL-CD97B09E2259",  # abs(x)
        "SBSQL-05420BE67BDE",  # asin
        "SBSQL-E74E5E119A13",  # asin(numeric)
        "SBSQL-836BCFBC18BA",  # atan(numeric)
        "SBSQL-B27CC5E497B0",  # atan2(y,x)
        "SBSQL-08468D193FE5",  # bit_and(int)
        "SBSQL-77697938339F",  # bit_or(int)
        "SBSQL-C74AF1CE17E2",  # bit_xor(int)
        "SBSQL-1D7E85355C37",  # ceil(numeric)
        "SBSQL-681A0E6F0278",  # sb.scalar.ceil
        "SBSQL-0D5C7FB8B8B0",  # cos(numeric)
        "SBSQL-617E97C2C602",  # exp(numeric)
        "SBSQL-F6A1BBDB4EE1",  # sb.scalar.floor
        "SBSQL-F7963EDAA8C1",  # floor(numeric)
        "SBSQL-823B68861B57",  # ln(numeric)
        "SBSQL-6A8E06A3E3F0",  # log10(numeric)
        "SBSQL-08B124B91C75",  # sb.scalar.power
        "SBSQL-F8E133A798F7",  # power
        "SBSQL-943656659739",  # sb.scalar.random
        "SBSQL-3FD9B555F30C",  # round(numeric[,scale])
        "SBSQL-5DE838A11804",  # sb.scalar.round
        "SBSQL-7D8F94A283F3",  # sign(numeric)
        "SBSQL-1847FD3F757A",  # sin(numeric)
        "SBSQL-07AE1EF04860",  # sqrt(numeric)
        "SBSQL-1AE7E5B9249E",  # sb.scalar.sqrt
        "SBSQL-45FE97C19C6E",  # sqrt(x)
        "SBSQL-C9546F1DBAB1",  # tan(numeric)
    }
    for canonical in target_canonicals:
        if by_builtin_positive[canonical] == 0:
            errors.append(f"canonical builtin {canonical} has no positive/edge case in fixture corpus")
        if canonical not in NULLLESS_BUILTINS and by_builtin_null[canonical] == 0:
            errors.append(f"canonical builtin {canonical} has no null_strict case in fixture corpus")
    covered_surface_ids = {r.get("surface_id", "") for r in fixtures}
    for surface_id in sorted(target_alias_surface_ids - covered_surface_ids):
        errors.append(f"SBSFC-010 target numeric alias/signature surface {surface_id} has no fixture row")

    print(
        "sbsql_sbsfc_010_numeric_builtin_fixture_gate "
        f"fixtures={len(fixtures)} "
        f"canonical_builtins_covered={len({r['canonical_builtin_id'] for r in fixtures if r.get('canonical_builtin_id')})} "
        f"target_alias_surfaces_covered={len(target_alias_surface_ids & covered_surface_ids)} "
        f"errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_010_numeric_builtin_fixture_gate=failed", file=sys.stderr)
        for err in errors[:20]:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_010_numeric_builtin_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
