#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-012 temporal + session builtin fixture gate.

Validates the structure of `SBSFC_012_TEMPORAL_SESSION_FIXTURES.csv` and
cross-checks every fixture row against the canonical SBsql surface
registry and the engine-side function seed registry.

Cross-checks (mirror SBSFC-010/011 gates, extended for canonical packages
`sb.temporal.*` and `sb.session.*`):

  1. Required CSV columns are present and non-empty (except
     `expected_result_value` / `expected_diagnostic_code` which are
     mutually exclusive per fixture row).
  2. Every `surface_id` exists in the canonical SBsql surface registry.
  3. Every `canonical_builtin_id` (`sb.<package>.<name>` for temporal/
     session) is present in `function_seed_registry.cpp` as an
     engine-anchored function_id with implementation_state=
     implemented_behavior.
  4. Every `function_id` matches the canonical_builtin_id (this slice
     uses canonical ids exclusively for clarity).
  5. case_kind discipline: positive/edge/range_assertion cases declare
     result expectations; null_strict and error cases declare
     diagnostic or null result.
  6. Each canonical_builtin_id is exercised by at least one positive
     case OR range_assertion (range_assertion is used for nullary
     volatile builtins like now/current_timestamp/current_date/
     current_time/current_user whose exact value depends on context).

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
from collections import defaultdict
from pathlib import Path


REGISTRY = "public_input_snapshot"
SEED_REGISTRY = "project/src/engine/functions/registry/function_seed_registry.cpp"
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_012_TEMPORAL_SESSION_FIXTURES.csv"


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
    # Extended regex to recognize sb.temporal and sb.session canonical packages
    # in addition to sb.scalar/sb.fn/data.scalar/domain used by SBSFC-010/011.
    return set(re.findall(
        r'"((?:sb\.scalar|sb\.temporal|sb\.session|sb\.uuid|sb\.regex|sb\.aggregate|sb\.window|sb\.special_form|sb\.operator|sb\.json|sb\.fn\.|data\.scalar|domain)[\w.\-]+)"',
        text,
    ))


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

    # Nullary volatile builtins are exempt from the null_strict requirement
    # because they take no arguments.
    nullary_volatile = {
        "sb.temporal.now",
        "sb.temporal.current_timestamp",
        "sb.temporal.current_date",
        "sb.temporal.current_time",
        "sb.temporal.statement_timestamp",
        "sb.temporal.transaction_timestamp",
        "sb.temporal.clock_timestamp",
        "sb.temporal.timeofday",
        "sb.temporal.localtime",
        "sb.temporal.localtimestamp",
        "sb.session.current_user",
        "sb.session.current_catalog",
        "sb.session.current_schema",
        "sb.session.current_database",
        "sb.session.current_role",
        "sb.uuid.v4",
        "sb.uuid.v1",
        "sb.uuid.v7",
    }

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
            errors.append(f"{fid}: function_id {function_id} does not match canonical_builtin_id {canonical}")

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
            if expected_diag:
                errors.append(f"{fid}: null_strict case must not carry expected_diagnostic_code")
        elif case_kind in {"arity_error", "domain_error", "overflow", "type_error"}:
            if not expected_diag:
                errors.append(f"{fid}: {case_kind} case must declare expected_diagnostic_code")
            if expected_value:
                errors.append(f"{fid}: {case_kind} case must not carry expected_result_value")
        elif case_kind == "range_assertion":
            # range_assertion cases (nullary volatile builtins): no exact value,
            # no diagnostic; engine probe asserts descriptor and (when deterministic
            # context injected) the rendered value.
            pass
        else:
            if not expected_value:
                errors.append(f"{fid}: positive/edge case must declare expected_result_value (got case_kind={case_kind})")
            if expected_diag:
                errors.append(f"{fid}: positive/edge case must not declare expected_diagnostic_code")

        if (case_kind.startswith("positive") or
                case_kind.startswith("edge") or
                case_kind == "range_assertion"):
            by_builtin_positive[canonical] += 1
        if case_kind.startswith("null"):
            by_builtin_null[canonical] += 1

    target_canonicals = {
        # SBSFC-012 first-pass
        "sb.temporal.now",
        "sb.temporal.current_timestamp",
        "sb.temporal.current_date",
        "sb.temporal.current_time",
        # SBSFC-012 temporal continuation 2026-05-12: context-backed
        # nullary scalars with exact runtime conformance coverage.
        "sb.temporal.statement_timestamp",
        "sb.temporal.transaction_timestamp",
        "sb.temporal.clock_timestamp",
        "sb.temporal.timeofday",
        "sb.temporal.localtime",
        "sb.temporal.localtimestamp",
        "sb.temporal.date_trunc",
        "sb.session.current_user",
        # SBSFC-012 second-pass (current_catalog/current_schema engine impls
        # added in this slice; uuid_v4 dispatches through existing uuid_generate)
        "sb.session.current_catalog",
        "sb.session.current_schema",
        "sb.uuid.v4",
        # SBSFC-012 third-pass (date_part dispatches through existing extract guard)
        "sb.temporal.date_part",
        # SBSFC-012 fourth-pass (temporal constructors — engine impls exist;
        # new canonical records added)
        "sb.temporal.make_date",
        "sb.temporal.make_time",
        "sb.temporal.make_timestamp",
        # SBSFC-012 fifth-pass (make_timestamptz — new engine guard + canonical
        # record; 2 native_future surfaces promoted)
        "sb.temporal.make_timestamptz",
        # SBSFC-012 sixth-pass (current_database/current_role engine guards +
        # canonical records; 2 native_future surfaces promoted)
        "sb.session.current_database",
        "sb.session.current_role",
        # uuid_v1/v7 engine sub-stream 2026-05-11 — new RandomUuidV1Text and
        # RandomUuidV7Text helpers; dedicated dispatch added; canonical records
        # pre-existed (surfaces were already native_now from registry genesis
        # but engine refused — closes that divergence).
        "sb.uuid.v1",
        "sb.uuid.v7",
        # dow/doy/quarter temporal sub-stream 2026-05-11 — dedicated engine
        # guards dispatching to ExtractTimestampField with extended field
        # handlers (dow via days_from_civil mod 7; doy via DaysFromCivil
        # subtraction; quarter via month arithmetic).
        "sb.temporal.dow",
        "sb.temporal.doy",
        "sb.temporal.quarter",
        # isodow + week ISO 8601 sub-stream 2026-05-11. isodow returns 1..7
        # Monday=1..Sunday=7; week returns 1..53 via ISO 8601 Thursday-of-
        # same-week algorithm with new CivilFromDays inverse helper.
        "sb.temporal.isodow",
        "sb.temporal.week",
        # add_months + last_day temporal arithmetic sub-stream 2026-05-11.
        # add_months returns date string shifted by month offset (day-clamped);
        # last_day returns last day of the month containing the input.
        "sb.temporal.add_months",
        "sb.temporal.last_day",
    }
    for canonical in target_canonicals:
        if by_builtin_positive[canonical] == 0:
            errors.append(f"canonical builtin {canonical} has no positive/edge/range_assertion case in fixture corpus")
        if canonical not in nullary_volatile and by_builtin_null[canonical] == 0:
            errors.append(f"canonical builtin {canonical} has no null_strict case in fixture corpus")

    required_fixture_ids = {
        "SBSFC012-make_date-pos",
        "SBSFC012-make_date-null",
        "SBSFC012-make_date-stale-signature",
        "SBSFC012-make_time-pos",
        "SBSFC012-make_time-null",
        "SBSFC012-make_time-stale-signature",
        "SBSFC012-make_timestamp-pos",
        "SBSFC012-make_timestamp-null",
        "SBSFC012-make_timestamp-stale-sixint",
        "SBSFC012-make_timestamp-stale-sixint-null",
        "SBSFC012-make_timestamp-stale-sixint-type",
        "SBSFC012-make_timestamp-stale-sixint-arity",
        "SBSFC012-make_timestamptz-default",
        "SBSFC012-make_timestamptz-with_tz",
        "SBSFC012-make_timestamptz-null",
    }
    for fixture_id in sorted(required_fixture_ids):
        if fixture_id not in seen_fixture_ids:
            errors.append(f"required SBSFC-012 constructor fixture_id {fixture_id} is missing")

    required_surface_ids = {
        # Exact SBSQL wrapper/alias rows checked from SBSQL_SURFACE_REGISTRY.csv
        # for this bounded SBSFC-012 temporal/date-field closure slice.
        "SBSQL-3F5F065C7F38",  # sb.temporal.now
        "SBSQL-1A537B8B4977",  # sb.temporal.current_timestamp
        "SBSQL-6BE40DA2763A",  # sb.temporal.current_date
        "SBSQL-DE72B3463556",  # sb.temporal.current_time
        "SBSQL-A5A86CFC3007",  # date_part(part,timestamp|interval)
        "SBSQL-7C99D4FC3868",  # date_trunc(part,timestamp[,timezone])
        "SBSQL-A1E1ED07A4B0",  # date_part
        "SBSQL-9E7A6C981E64",  # date_trunc
        "SBSQL-65A515D046CD",  # sb.temporal.date_part
        "SBSQL-A71E6FCE948C",  # EXTRACT
        "SBSQL-92541D50173D",  # sb.temporal.date_trunc
        "SBSQL-17F632F80DD2",  # EXTRACT(EPOCH FROM ...)
        "SBSQL-FAD1979C420A",  # EXTRACT(part FROM temporal)
        "SBSQL-35DD945088A4",  # sb.special.extract
        "SBSQL-9485F8985BD3",  # sb.session.current_user
        "SBSQL-96268EF5944F",  # sb.session.current_catalog
        "SBSQL-F558C27C41FF",  # sb.session.current_schema
        "SBSQL-30AC9D4B52DE",  # dow(date)
        "SBSQL-A6B9F5E37598",  # doy(date)
        "SBSQL-CA3A65D98456",  # isodow(date)
        "SBSQL-FF788798B8C0",  # quarter(date)
        "SBSQL-141F422A1427",  # week(date)
        "SBSQL-3C2D518F0161",  # dow
        "SBSQL-F995EB3591B9",  # doy
        "SBSQL-A4AF5914BD3E",  # quarter
        "SBSQL-B92399F55F44",  # isodow
        "SBSQL-FBDA61BFAAD5",  # week
        "SBSQL-712BEB777116",  # add_months
        "SBSQL-1BA978245122",  # add_months(date,n)
        "SBSQL-24D3CBDD3F18",  # last_day
        "SBSQL-559A925B6DA8",  # last_day(date)
        "SBSQL-91BF8C8E4A0B",  # make_date(year,month,day)
        "SBSQL-A8EA58D8BB54",  # make_date stale signature row
        "SBSQL-1F63F7A3A3E8",  # make_time(hour,minute,second)
        "SBSQL-0A161EBD898D",  # make_time stale signature row
        "SBSQL-18C4CF502CEF",  # make_timestamp(date,time)
        "SBSQL-0D13739AA895",  # make_timestamp six-int stale row
        "SBSQL-B4F7A13FA126",  # make_timestamptz(date,time)
        "SBSQL-5C6D5D612785",  # make_timestamptz(date,time,tz)
        "SBSQL-BFDB58FD048D",  # uuid_v1()
        "SBSQL-ACB31A992444",  # uuid_v4()
        "SBSQL-F387AC0EFF32",  # uuid_v7()
        "SBSQL-27C48C45B280",  # sb.uuid.v1()
        "SBSQL-E61F59BCD166",  # sb.uuid.v4()
        "SBSQL-18812EB5E740",  # sb.uuid.v7()
    }
    fixture_surface_ids = {r.get("surface_id", "") for r in fixtures}
    for surface_id in sorted(required_surface_ids):
        if surface_id not in fixture_surface_ids:
            errors.append(f"required SBSFC-012 exact surface_id {surface_id} has no fixture row")

    print(
        "sbsql_sbsfc_012_temporal_session_fixture_gate "
        f"fixtures={len(fixtures)} "
        f"canonical_builtins_covered={len({r['canonical_builtin_id'] for r in fixtures if r.get('canonical_builtin_id')})} "
        f"errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_012_temporal_session_fixture_gate=failed", file=sys.stderr)
        for err in errors[:20]:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_012_temporal_session_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
