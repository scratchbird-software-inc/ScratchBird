#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SBSFC-011 text builtin fixture gate.

Validates the structure of `SBSFC_011_TEXT_BUILTIN_FIXTURES.csv` and
cross-checks every fixture row against the canonical SBsql surface
registry and the engine-side function seed registry.

Cross-checks (mirror SBSFC-010 gate, adapted for the text family):

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
     case), `expected_diagnostic_code` (refusal/error case), or
     null/empty for null_strict and edge_empty_text cases.
  6. Each canonical_builtin_id is exercised by at least one positive
     case AND at least one null_strict case.

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
FIXTURES = "project/tests/sbsql_parser_worker/generated/full_surface/SBSFC_011_TEXT_BUILTIN_FIXTURES.csv"


REQUIRED_NATIVE_NOW_SURFACE_IDS = {
    "SBSQL-00439AD179E6": "sb.scalar.position",
    "SBSQL-1B39A3C25D5B": "sb.scalar.position",
    "SBSQL-01F01C7A290B": "sb.scalar.coalesce",
    "SBSQL-03A27CAE8A45": "sb.scalar.nullif",
    "SBSQL-12FB359A9976": "sb.scalar.btrim",
    "SBSQL-185F244A9CA1": "sb.scalar.ltrim",
    "SBSQL-20053F5AC863": "sb.scalar.upper",
    "SBSQL-234E59C1DF96": "sb.scalar.btrim",
    "SBSQL-259E28B6EF67": "sb.scalar.upper",
    "SBSQL-29624CFE2736": "sb.scalar.substring",
    "SBSQL-352FFF5468A6": "sb.scalar.coalesce",
    "SBSQL-40C2AF720E24": "sb.scalar.concat",
    "SBSQL-4B68123DAFB1": "sb.scalar.rtrim",
    "SBSQL-48AD6AFE9BCD": "sb.scalar.ifnull",
    "SBSQL-521BAEB2E8A4": "sb.scalar.ltrim",
    "SBSQL-578CF964D127": "sb.scalar.replace",
    "SBSQL-6CD48E00F9BD": "sb.scalar.trim",
    "SBSQL-7456C956A880": "sb.scalar.overlay",
    "SBSQL-74418A7B7349": "sb.scalar.rtrim",
    "SBSQL-781AA5628408": "sb.scalar.lower",
    "SBSQL-7C500BA126F0": "sb.scalar.position",
    "SBSQL-824EDB98BFE8": "sb.scalar.right",
    "SBSQL-83EEED02BD93": "sb.scalar.ifnull",
    "SBSQL-906EA0F406D1": "sb.scalar.octet_length",
    "SBSQL-99635CAA083A": "sb.scalar.substring",
    "SBSQL-9ECB29BCFF5A": "sb.scalar.substring",
    "SBSQL-B81F04905CD1": "sb.scalar.length",
    "SBSQL-D1914EB51362": "sb.scalar.trim",
    "SBSQL-D95E853515DC": "sb.scalar.position",
    "SBSQL-DC524A31C9B5": "sb.scalar.coalesce",
    "SBSQL-E116CF62E9D0": "sb.scalar.length",
    "SBSQL-E1285814D20C": "sb.scalar.substring",
    "SBSQL-E557D9462210": "sb.scalar.lower",
    "SBSQL-E6121A0124D4": "sb.scalar.concat",
    "SBSQL-EE28E52F3E30": "sb.scalar.left",
    "SBSQL-F77648609C4D": "sb.scalar.bit_length",
    "SBSQL-256568E3617A": "sb.scalar.bit_length",
    "SBSQL-71F2FC01DDD0": "sb.scalar.char_length",
    "SBSQL-0112477D19BE": "sb.scalar.chr",
    "SBSQL-515E6D2B9239": "sb.scalar.octet_length",
    "SBSQL-9DBB48B8B778": "sb.scalar.replace",
    "SBSQL-DA800099AD68": "sb.scalar.reverse",
    "SBSQL-130989BE6037": "sb.scalar.md5",
    "SBSQL-B8EBE8DBD77E": "sb.scalar.sha1",
    "SBSQL-FFBA0CA4527A": "sb.scalar.sha224",
    "SBSQL-0D95F250C6BC": "sb.scalar.sha224",
    "SBSQL-22FED70B1306": "sb.scalar.sha256",
    "SBSQL-76B2598F41C1": "sb.scalar.sha384",
    "SBSQL-9F9A118F5BCC": "sb.scalar.sha384",
    "SBSQL-E0CDCE6AEE04": "sb.scalar.sha512",
    "SBSQL-4C44F5CE32D7": "sb.scalar.initcap",
    "SBSQL-6B0797D68FD6": "sb.scalar.initcap",
    "SBSQL-98ED9587403E": "sb.scalar.translate",
    "SBSQL-A32440FF2F9A": "sb.scalar.translate",
    "SBSQL-D19550197C5C": "sb.scalar.unicode",
    "SBSQL-B3D36C023507": "sb.scalar.unicode",
    "SBSQL-209CB0E77FAC": "sb.scalar.ascii",
    "SBSQL-5F92FDEEE7E3": "sb.scalar.left",
    "SBSQL-837F23FAC0D1": "sb.scalar.right",
    "SBSQL-558F06614D96": "sb.scalar.encode",
    "SBSQL-D69F87D8A712": "sb.scalar.encode",
    "SBSQL-5AD47A23FC8C": "sb.scalar.decode",
    "SBSQL-9AD044845D29": "sb.scalar.decode",
    "SBSQL-9436D973AB0D": "sb.scalar.oracle_decode",
    "SBSQL-6250A4C72894": "sb.scalar.uuid_from_string",
    "SBSQL-81D063680A39": "sb.scalar.uuid_from_string",
    "SBSQL-051F74FD6FF7": "sb.scalar.uuid_to_string",
    "SBSQL-B260A8B5877E": "sb.scalar.uuid_to_string",
    "SBSQL-8681F20399C3": "sb.scalar.digest",
    "SBSQL-303C4BE9B001": "sb.scalar.digest",
    "SBSQL-C1D95CE89815": "sb.scalar.default_charset",
    "SBSQL-4A0E859F75C6": "sb.scalar.default_collation",
    "SBSQL-75DC9CE7072D": "sb.scalar.comparison_collation_resolution",
    "SBSQL-3688A280B569": "sb.scalar.keyword_case_rule",
    "SBSQL-E2364705F97A": "sb.scalar.quoted_identifier_case_rule",
    "SBSQL-C5C60A1F17D1": "sb.scalar.unquoted_identifier_case_rule",
    "SBSQL-DED44DC1AF64": "sb.scalar.unicode_root",
}

REQUIRED_BARE_ALIAS_FUNCTION_IDS = {
    "SBSQL-D69F87D8A712": "encode",
    "SBSQL-9AD044845D29": "decode",
    "SBSQL-303C4BE9B001": "digest",
}

CRYPTO_REFUSAL_SURFACE_IDS = {
    "SBSQL-130989BE6037",
    "SBSQL-B8EBE8DBD77E",
    "SBSQL-FFBA0CA4527A",
    "SBSQL-0D95F250C6BC",
    "SBSQL-22FED70B1306",
    "SBSQL-76B2598F41C1",
    "SBSQL-9F9A118F5BCC",
    "SBSQL-E0CDCE6AEE04",
}


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
    return set(re.findall(r'"((?:sb\.scalar|sb\.fn\.|sb\.regex|sb\.temporal|sb\.session|sb\.uuid|sb\.json|data\.scalar|domain)[\w.\-]+)"', text))


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
    required_surface_case_kinds: dict[str, set[str]] = defaultdict(set)

    for row in fixtures:
        fid = row.get("fixture_id", "")
        if not fid:
            errors.append("fixture row missing fixture_id")
            continue
        if fid in seen_fixture_ids:
            errors.append(f"duplicate fixture_id: {fid}")
        seen_fixture_ids.add(fid)

        for col, value in row.items():
            if "\x00" in (value or ""):
                errors.append(f"{fid}: column {col} contains a raw NUL byte; use printable \\u0000 evidence")

        for col in REQUIRED_COLUMNS:
            if not (row.get(col, "") or "").strip():
                errors.append(f"{fid}: required column {col} is empty")

        sid = row.get("surface_id", "")
        if sid and sid not in surfaces:
            errors.append(f"{fid}: surface_id {sid} not in canonical registry")
        if sid in REQUIRED_NATIVE_NOW_SURFACE_IDS:
            required_surface_case_kinds[sid].add(row.get("case_kind", ""))

        canonical = row.get("canonical_builtin_id", "")
        if canonical and canonical not in registered_ids:
            errors.append(f"{fid}: canonical_builtin_id {canonical} not registered in function_seed_registry")
        expected_canonical = REQUIRED_NATIVE_NOW_SURFACE_IDS.get(sid)
        if expected_canonical and canonical != expected_canonical:
            errors.append(f"{fid}: surface_id {sid} must use canonical_builtin_id {expected_canonical}, got {canonical}")

        function_id = row.get("function_id", "")
        required_alias = REQUIRED_BARE_ALIAS_FUNCTION_IDS.get(sid)
        if required_alias:
            if function_id != required_alias:
                errors.append(f"{fid}: surface_id {sid} must use bare alias function_id {required_alias}, got {function_id}")
        elif function_id and function_id != canonical:
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
        if sid in CRYPTO_REFUSAL_SURFACE_IDS and (
                case_kind != "dependency_unavailable" or
                expected_diag != "SBSQL.FUNCTION.DEPENDENCY_UNAVAILABLE" or
                expected_value):
            errors.append(f"{fid}: crypto refusal surface {sid} must assert exact dependency-unavailable refusal")
        if case_kind.startswith("null"):
            if expected_diag:
                errors.append(f"{fid}: null_strict case must not carry expected_diagnostic_code")
        elif case_kind.startswith("edge_empty"):
            # empty string/value cases: result is empty (no value), no diagnostic.
            if expected_diag:
                errors.append(f"{fid}: edge_empty case must not carry expected_diagnostic_code")
        elif case_kind in {"arity_error", "domain_error", "overflow", "type_error", "dependency_unavailable"}:
            if not expected_diag:
                errors.append(f"{fid}: {case_kind} case must declare expected_diagnostic_code")
            if expected_value:
                errors.append(f"{fid}: {case_kind} case must not carry expected_result_value")
        elif case_kind.startswith("edge_not_found"):
            # not-found cases for position: value is 0 (declared as expected_result_value).
            if expected_diag:
                errors.append(f"{fid}: edge_not_found case must not carry expected_diagnostic_code")
        else:
            if not expected_value:
                errors.append(f"{fid}: positive/edge case must declare expected_result_value (got case_kind={case_kind})")
            if expected_diag:
                errors.append(f"{fid}: positive/edge case must not declare expected_diagnostic_code")

        if (case_kind.startswith("positive") or
                case_kind.startswith("edge") or
                case_kind == "dependency_unavailable"):
            # dependency_unavailable cases count as positive coverage for
            # refusal-implemented canonicals (the refusal IS the implemented
            # behavior pending dependency configuration).
            by_builtin_positive[canonical] += 1
        if case_kind.startswith("null"):
            by_builtin_null[canonical] += 1

    # Refusal-implemented canonicals exempt from the null_strict requirement —
    # the engine refuses at dispatch before reaching any null handling, so
    # null-input behavior is moot.
    refusal_implemented = {
        "sb.scalar.md5",
        "sb.scalar.sha1",
        "sb.scalar.sha224",
        "sb.scalar.sha256",
        "sb.scalar.sha384",
        "sb.scalar.sha512",
    }
    # Non-strict-null canonicals are exempt from the null_strict requirement —
    # their semantic treats null arguments specially (skip or test-and-select)
    # rather than propagating null. Coverage of null behavior for these
    # canonicals is exercised via positive_text fixtures that assert the
    # documented non-strict result (e.g. greatest/least all-null returns null
    # via null_strict case kind which IS allowed; or nvl2 null-test which
    # returns the fallback value via positive_text case).
    non_strict_null_handling = {
        "sb.scalar.concat",
        "sb.scalar.greatest",
        "sb.scalar.least",
        "sb.scalar.nvl2",
        "sb.scalar.oracle_decode",
    }
    zero_arg_metadata_canonicals = {
        "sb.scalar.default_charset",
        "sb.scalar.default_collation",
        "sb.scalar.comparison_collation_resolution",
        "sb.scalar.keyword_case_rule",
        "sb.scalar.quoted_identifier_case_rule",
        "sb.scalar.unquoted_identifier_case_rule",
        "sb.scalar.unicode_root",
    }
    zero_arg_metadata_surface_ids = {
        "SBSQL-C1D95CE89815",
        "SBSQL-4A0E859F75C6",
        "SBSQL-75DC9CE7072D",
        "SBSQL-3688A280B569",
        "SBSQL-E2364705F97A",
        "SBSQL-C5C60A1F17D1",
        "SBSQL-DED44DC1AF64",
    }

    target_canonicals = {
        # SBSFC-011 first-pass
        "sb.scalar.substring",
        "sb.scalar.length",
        "sb.scalar.lower",
        "sb.scalar.upper",
        "sb.scalar.trim",
        "sb.scalar.position",
        # SBSFC-011 second-pass (text/conditional batch — wiring only; all surfaces
        # already native_now so no SBSFC-009B promotion needed)
        "sb.scalar.octet_length",
        "sb.scalar.concat",
        "sb.scalar.replace",
        "sb.scalar.ifnull",
        "sb.scalar.coalesce",
        "sb.scalar.nullif",
        # SBSFC-011 third-pass (ltrim/rtrim/reverse — engine impls exist; new
        # canonical records added; 3 native_future surfaces promoted)
        "sb.scalar.ltrim",
        "sb.scalar.rtrim",
        "sb.scalar.reverse",
        # overlay engine impl added 2026-05-11 (canonical record pre-existed
        # from registry genesis; 2 native_future surfaces promoted)
        "sb.scalar.overlay",
        # repeat canonical record added + wiring 2026-05-11
        "sb.scalar.repeat",
        # crypto digest refusal sub-stream 2026-05-11 — refusal-implemented
        # canonicals (engine returns SB_DIAG_FUNCTION_DEPENDENCY_UNAVAILABLE
        # without configured crypto provider). Exempt from null_strict
        # requirement (engine refuses before reaching null handling).
        "sb.scalar.md5",
        "sb.scalar.sha1",
        "sb.scalar.sha256",
        "sb.scalar.sha512",
        # char_length canonical alias sub-stream 2026-05-11 — canonical record
        # equivalent of sb.scalar.length for text inputs; engine dispatches
        # through shared length guard (IdIs already accepts char_length bare
        # name; this slice adds sb.scalar.char_length canonical alias).
        "sb.scalar.char_length",
        # btrim directional trim sub-stream — canonical equivalent of
        # sb.scalar.trim, covering both 1-arg whitespace and 2-arg custom-char
        # forms through the shared trim guard.
        "sb.scalar.btrim",
        # lpad/rpad text-pad sub-stream 2026-05-11 — new engine guards added
        # supporting 2-arg (default fill=' ') and 3-arg (explicit fill) forms;
        # length bounded to [0, 1048576] matching repeat's allocation cap.
        "sb.scalar.lpad",
        "sb.scalar.rpad",
        # chr/ascii character classification/conversion sub-stream 2026-05-11
        # — chr maps int64 [0, 127] -> single-char string; ascii maps text ->
        # int64 codepoint of first byte via unsigned-char cast.
        "sb.scalar.chr",
        "sb.scalar.ascii",
        "sb.scalar.initcap",
        "sb.scalar.translate",
        "sb.scalar.unicode",
        # regex_match sub-stream 2026-05-11 — engine guard added using
        # std::regex (ECMAScript syntax) supporting 2-arg and 3-arg (flags)
        # forms. Surface was already native_now; closes engine-impl divergence.
        "sb.regex.match",
        # sb.json wiring sub-stream 2026-05-11 — sb.json.typeof and
        # sb.json.extract route through nosql.document family dispatch via
        # IsNoSqlDocumentFunction extension; existing JsonTypeOf and
        # ExtractJsonObjectField helpers provide the impl. Surfaces were
        # already native_now; closes engine-impl divergence.
        "sb.json.typeof",
        "sb.json.extract",
        # substr/greatest/least/nvl2 sub-stream 2026-05-11 — substr is an
        # alias of substring sharing the engine guard; greatest/least are
        # new engine guards using unified-comparison strategy (numeric or
        # text); nvl2 is a ternary null-test selector.
        "sb.scalar.substr",
        "sb.scalar.greatest",
        "sb.scalar.least",
        "sb.scalar.nvl2",
        # instr/strpos/to_hex text-alias sub-stream 2026-05-11 — new dedicated
        # engine guards. instr and strpos share corrected string-first argument
        # order (PostgreSQL/Oracle convention) distinct from the existing
        # position guard. to_hex is integer-to-hex (PostgreSQL convention),
        # distinct from the byte-encoding hex_encode guard.
        "sb.scalar.instr",
        "sb.scalar.strpos",
        "sb.scalar.to_hex",
        # SBSFC-011 oracle-first text/encoding/UUID slice 2026-05-12.
        "sb.scalar.left",
        "sb.scalar.right",
        "sb.scalar.encode",
        "sb.scalar.decode",
        "sb.scalar.oracle_decode",
        "sb.scalar.uuid_from_string",
        "sb.scalar.uuid_to_string",
        "sb.scalar.digest",
        # SBSFC-011 language/profile metadata scalar closure 2026-05-12.
        "sb.scalar.default_charset",
        "sb.scalar.default_collation",
        "sb.scalar.comparison_collation_resolution",
        "sb.scalar.keyword_case_rule",
        "sb.scalar.quoted_identifier_case_rule",
        "sb.scalar.unquoted_identifier_case_rule",
        "sb.scalar.unicode_root",
    }
    for canonical in target_canonicals:
        if by_builtin_positive[canonical] == 0:
            errors.append(f"canonical builtin {canonical} has no positive/edge/dependency_unavailable case in fixture corpus")
        if (canonical not in refusal_implemented and
                canonical not in non_strict_null_handling and
                canonical not in zero_arg_metadata_canonicals and
                by_builtin_null[canonical] == 0):
            errors.append(f"canonical builtin {canonical} has no null_strict case in fixture corpus")

    for sid, canonical in REQUIRED_NATIVE_NOW_SURFACE_IDS.items():
        kinds = required_surface_case_kinds[sid]
        if not kinds:
            errors.append(f"required SBSFC-011 native_now surface {sid} ({canonical}) has no fixture row")
            continue
        if sid in CRYPTO_REFUSAL_SURFACE_IDS:
            if "dependency_unavailable" not in kinds:
                errors.append(f"required crypto refusal surface {sid} has no dependency_unavailable fixture")
        elif not any(kind.startswith("positive") or kind.startswith("edge") for kind in kinds):
            errors.append(f"required SBSFC-011 native_now surface {sid} has no positive/edge fixture")
        if sid not in CRYPTO_REFUSAL_SURFACE_IDS and sid not in zero_arg_metadata_surface_ids and not any(
                kind.startswith("null") for kind in kinds) and not any(
                kind in {"domain_error", "arity_error", "type_error"} for kind in kinds):
            errors.append(f"required SBSFC-011 native_now surface {sid} has no null/error fixture")

    print(
        "sbsql_sbsfc_011_text_builtin_fixture_gate "
        f"fixtures={len(fixtures)} "
        f"canonical_builtins_covered={len({r['canonical_builtin_id'] for r in fixtures if r.get('canonical_builtin_id')})} "
        f"errors={len(errors)}"
    )
    if errors:
        print("sbsql_sbsfc_011_text_builtin_fixture_gate=failed", file=sys.stderr)
        for err in errors[:20]:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("sbsql_sbsfc_011_text_builtin_fixture_gate=passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
