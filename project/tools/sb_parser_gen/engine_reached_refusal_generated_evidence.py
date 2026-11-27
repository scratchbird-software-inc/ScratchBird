#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Closed refusal kinds for the SBSFC-057 PGP/package rows (owner 2026-11-27).

Two closed kinds, both under the existing per-row final state ``exact_refusal_passed``
(no new final state: three release statuses stay three):

``engine_reached_refusal`` (the four ``pgp_*`` rows)
    The parser lowers the call to ``sb.crypto.<name>``, executable SBLR IS emitted,
    verified and admitted, the engine function IS reached through its SBLR dispatch,
    and it refuses ``CRYPTO.PGP.PROFILE_UNAVAILABLE`` with no result row and no
    mutation. This is NOT ``e2e_passed`` (no accepted positive route) and NOT the
    pre-SBLR kind (the literal token ``executable_sblr_emitted=false`` is false here
    and is FORBIDDEN on these rows). The proof scope is stated in tokens: the engine
    is reached by the in-process route (no database file); on a native database the
    call stops at the native dispatch gate (opcode 1024) before any engine function.

``parser_package_refusal`` (the ``pgcrypto`` row)
    ``pgcrypto(...)`` is a package/capability spelling, not a callable scalar. The
    parser answers ``CRYPTO.PACKAGE.NOT_CALLABLE`` through a verifier error; no
    executable SBLR reaches the engine. Same mechanism as the SBSQL.POLICY_BLOCKED
    diagnostic identity (pre-SBLR exact refusal).

Fail-closed: a row that claims either kind must carry EVERY token of its closed
vector (exact ``;``-separated tokens, never substrings), a refusal code that exists
in the engine diagnostic catalog, and proof references naming ctests that are really
registered in the CMake lists. Any contradictory single-valued token fails. A row
outside the closed surface-id sets that carries a kind marker fails.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Iterable


FIXTURE_CSV = (
    "project/tests/sbsql_parser_worker/generated/full_surface/"
    "SBSFC_057_CRYPTO_HASH_FIXTURES.csv"
)
RUNTIME_CTEST = "sbsql_sbsfc_057_crypto_hash_runtime_conformance"
RUNTIME_SOURCE = (
    "project/tests/sbsql_parser_worker/generated/full_surface/"
    "sbsql_sbsfc_057_crypto_hash_runtime_conformance.cpp"
)
ROUTE_CTEST = "sbsql_pgp_refusal_route_conformance"
ROUTE_SOURCE = "project/tests/sbsql_parser_worker/sbsql_pgp_refusal_route_conformance.cpp"
NATIVE_CTESTS = tuple(
    f"sbsql_sblr_alignment_native_pgp_refusal_{size}"
    for size in ("8k", "16k", "32k", "64k", "128k")
)
NATIVE_SOURCE = "project/tests/sbsql_sblr_alignment/native_route_b_test.cpp"
DIAGNOSTIC_CATALOG = "project/src/core/diagnostics/canonical_diagnostic_catalog_data.inc"
CMAKE_ROOTS = ("project/tests", "project/drivers")

PGP_CODE = "CRYPTO.PGP.PROFILE_UNAVAILABLE"
PACKAGE_CODE = "CRYPTO.PACKAGE.NOT_CALLABLE"

KIND_ENGINE_REACHED = "engine_reached_refusal"
KIND_PACKAGE = "parser_package_refusal"

# surface_id -> (engine entrypoint, SBSFC-057 fixture id)
ENGINE_REACHED_REFUSALS = {
    "SBSQL-C98EC981ACD7": ("pgp_sym_encrypt", "SBSFC057-pgp-sym-encrypt"),
    "SBSQL-6DBE85C4B814": ("pgp_sym_decrypt", "SBSFC057-pgp-sym-decrypt"),
    "SBSQL-2854D8B0790B": ("pgp_pub_encrypt", "SBSFC057-pgp-pub-encrypt"),
    "SBSQL-6358314B6883": ("pgp_pub_decrypt", "SBSFC057-pgp-pub-decrypt"),
}
ENGINE_REACHED_REFUSAL_SURFACE_IDS = frozenset(ENGINE_REACHED_REFUSALS)
PACKAGE_REFUSAL_SURFACE_ID = "SBSQL-AF4E7BFEFDD1"
PACKAGE_REFUSAL_SURFACE_IDS = frozenset({PACKAGE_REFUSAL_SURFACE_ID})
PACKAGE_FIXTURE_ID = "SBSFC057-pgcrypto-marker"
PACKAGE_ENTRYPOINT = "pgcrypto"

NOTE_AUTH = "engine_reached_refusal_evidence_promoted_by_sbsql_full_surface_fixture_promotion_gate"
NOTE_ROUND = "engine_reached_refusal_round_trip_scope_evidence_promoted_by_sbsql_full_surface_fixture_promotion_gate"
NOT_APPLICABLE_ROUND_TRIP = (
    "not_applicable_engine_reached_refusal_no_byte_identical_round_trip_claimed"
)

# single-valued keys: every occurrence must equal the required value
_ENGINE_SINGLE = {
    "refusal_kind": KIND_ENGINE_REACHED,
    "executable_sblr_emitted": "true",
    "engine_reached": "true",
    "engine_reached_route": "in_process_no_database_file",
    "result_rows": "0",
    "mutation": "none",
    "refusal_code": PGP_CODE,
    "native_database_route": "refused_at_native_dispatch_gate_engine_function_not_reached",
    "native_gate_code": "SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED",
    "native_gate_opcode": "1024",
    "exact_refusal": "true",
}
_PACKAGE_SINGLE = {
    "refusal_kind": KIND_PACKAGE,
    "executable_sblr_emitted": "false",
    "pre_sblr_refusal": "true",
    "verifier_admitted": "false",
    "engine_reached": "false",
    "result_rows": "0",
    "mutation": "none",
    "refusal_code": PACKAGE_CODE,
    "refusal_field_function": PACKAGE_ENTRYPOINT,
    "callable_function": "false",
    "package_identity": PACKAGE_ENTRYPOINT,
    "native_database_route": "parser_answers_engine_not_asked",
    "exact_refusal": "true",
    "engine_dispatch_not_reached": "true",
    "result_published": "false",
    "catalog_mutation": "false",
}
_FORBIDDEN_TOKENS_ENGINE = (
    "executable_sblr_emitted=false",
    "pre_sblr_refusal=true",
    "engine_dispatch_not_reached=true",
    "engine_reached=false",
    "server_dispatch=false",
    "server_admission_not_reached=true",
    "server_admission_reached=false",
    "engine_dispatch_reached=false",
    "result_published=true",
)
_FORBIDDEN_TOKENS_PACKAGE = (
    "executable_sblr_emitted=true",
    "engine_reached=true",
    "engine_dispatch_reached=true",
    "server_dispatch=true",
    "engine_mutation=true",
    "result_published=true",
)
_PROOF_KEYS = ("proof", "native_route_proof", "engine_function_proof")


def is_engine_reached_refusal(surface_id: str) -> bool:
    return surface_id in ENGINE_REACHED_REFUSAL_SURFACE_IDS


def is_package_refusal(surface_id: str) -> bool:
    return surface_id in PACKAGE_REFUSAL_SURFACE_IDS


def is_engine_reached_or_package_refusal(surface_id: str) -> bool:
    return is_engine_reached_refusal(surface_id) or is_package_refusal(surface_id)


# --------------------------------------------------------------------------
# closed token vectors (what the generators write and the gates demand)
# --------------------------------------------------------------------------
def engine_reached_vector(surface_id: str) -> tuple[str, ...]:
    entrypoint, _ = ENGINE_REACHED_REFUSALS[surface_id]
    tokens = [f"{key}={value}" for key, value in _ENGINE_SINGLE.items()]
    tokens.append(f"refusal_operation_field={entrypoint}")
    tokens.append(f"proof=ctest:{ROUTE_CTEST}")
    tokens.append(f"engine_function_proof=ctest:{RUNTIME_CTEST}")
    tokens.extend(f"native_route_proof=ctest:{name}" for name in NATIVE_CTESTS)
    return tuple(tokens)


def package_vector() -> tuple[str, ...]:
    tokens = [f"{key}={value}" for key, value in _PACKAGE_SINGLE.items()]
    tokens.append(f"proof=ctest:{ROUTE_CTEST}")
    tokens.extend(f"native_route_proof=ctest:{name}" for name in NATIVE_CTESTS)
    return tuple(tokens)


def vector_for(surface_id: str) -> tuple[str, ...]:
    if is_engine_reached_refusal(surface_id):
        return engine_reached_vector(surface_id)
    if is_package_refusal(surface_id):
        return package_vector()
    raise ValueError(f"{surface_id} is not an engine-reached/package refusal row")


def refusal_code_for(surface_id: str) -> str:
    return PGP_CODE if is_engine_reached_refusal(surface_id) else PACKAGE_CODE


def entrypoint_for(surface_id: str) -> str:
    if is_engine_reached_refusal(surface_id):
        return ENGINE_REACHED_REFUSALS[surface_id][0]
    return PACKAGE_ENTRYPOINT


def fixture_id_for(surface_id: str) -> str:
    if is_engine_reached_refusal(surface_id):
        return ENGINE_REACHED_REFUSALS[surface_id][1]
    return PACKAGE_FIXTURE_ID


# --------------------------------------------------------------------------
# fail-closed validation shared by every gate
# --------------------------------------------------------------------------
def _tokens(values: Iterable[str]) -> list[str]:
    out: list[str] = []
    for value in values:
        for token in str(value).split(";"):
            token = token.strip()
            if token:
                out.append(token)
    return out


def registered_ctest_names(root: Path) -> tuple[set[str], list[re.Pattern[str]]]:
    """Names registered by ``add_test(NAME ...)`` in the CMake lists: exact names and
    templates (``name_${var}``) as regular expressions."""
    exact: set[str] = set()
    templates: list[re.Pattern[str]] = []
    pattern = re.compile(r"add_test\(\s*NAME\s+\"?([^\s\")]+)\"?", re.IGNORECASE)
    for sub in CMAKE_ROOTS:
        base = root / sub
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("CMakeLists.txt")):
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in pattern.finditer(text):
                name = match.group(1)
                if "${" in name:
                    parts = re.split(r"\$\{[^}]*\}", name)
                    regex = "[A-Za-z0-9]+".join(re.escape(part) for part in parts)
                    templates.append(re.compile(f"^{regex}$"))
                else:
                    exact.add(name)
    return exact, templates


def ctest_is_registered(root: Path, name: str) -> bool:
    exact, templates = registered_ctest_names(root)
    return name in exact or any(template.match(name) for template in templates)


def diagnostic_code_in_catalog(root: Path, code: str) -> bool:
    path = root / DIAGNOSTIC_CATALOG
    if not path.is_file():
        return False
    return f'{{"{code}",' in path.read_text(encoding="utf-8", errors="replace")


def refusal_kind_errors(surface_id: str, values: Iterable[str], root: Path | None) -> list[str]:
    """Errors for one row (any artifact kind: ledger, manifest, release, fixture).

    ``values`` are all text cells of the row; tokens are the exact ``;``-separated
    pieces. Returns [] only for a row that carries its closed vector in full."""
    vals = list(values)
    tokens = _tokens(vals)
    claims_engine = f"refusal_kind={KIND_ENGINE_REACHED}" in tokens
    claims_package = f"refusal_kind={KIND_PACKAGE}" in tokens
    errors: list[str] = []
    if claims_engine and not is_engine_reached_refusal(surface_id):
        errors.append(f"{surface_id} claims {KIND_ENGINE_REACHED} but is not an admitted row")
    if claims_package and not is_package_refusal(surface_id):
        errors.append(f"{surface_id} claims {KIND_PACKAGE} but is not an admitted row")
    if not is_engine_reached_or_package_refusal(surface_id):
        return errors
    engine = is_engine_reached_refusal(surface_id)
    kind = KIND_ENGINE_REACHED if engine else KIND_PACKAGE
    single = _ENGINE_SINGLE if engine else _PACKAGE_SINGLE
    token_set = set(tokens)
    for required in vector_for(surface_id):
        if required not in token_set:
            errors.append(f"{surface_id} {kind} row is missing token {required}")
    for token in _FORBIDDEN_TOKENS_ENGINE if engine else _FORBIDDEN_TOKENS_PACKAGE:
        if token in token_set:
            errors.append(f"{surface_id} {kind} row carries forbidden token {token}")
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        if key in single and value != single[key]:
            errors.append(f"{surface_id} {kind} row has contradictory {key}={value}")
        if key == "refusal_operation_field" and engine and value != entrypoint_for(surface_id):
            errors.append(f"{surface_id} {kind} row has contradictory {key}={value}")
    code = refusal_code_for(surface_id)
    if code not in token_set:
        errors.append(f"{surface_id} {kind} row lacks the concrete diagnostic token {code}")
    if "canonical_message_vector_set" not in token_set:
        errors.append(f"{surface_id} {kind} row lacks canonical_message_vector_set")
    if root is not None:
        if not diagnostic_code_in_catalog(root, code):
            errors.append(f"{surface_id} {kind} refusal code {code} is not in the engine diagnostic catalog")
        exact, templates = registered_ctest_names(root)
        proofs = 0
        for token in tokens:
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            if key in _PROOF_KEYS and value.startswith("ctest:"):
                proofs += 1
                name = value[len("ctest:"):]
                if name not in exact and not any(t.match(name) for t in templates):
                    errors.append(f"{surface_id} {kind} proof names an unregistered ctest {name}")
        if proofs == 0:
            errors.append(f"{surface_id} {kind} row names no proof ctest")
    return errors


def validate_authoritative_runtime_inputs(root: Path) -> None:
    """Generator preflight: the proof sources exist and say what the tokens claim."""
    for source in (ROUTE_SOURCE, NATIVE_SOURCE, RUNTIME_SOURCE, FIXTURE_CSV):
        if not (root / source).is_file():
            raise ValueError(f"engine-reached refusal proof source is missing: {source}")
    for code in (PGP_CODE, PACKAGE_CODE):
        if not diagnostic_code_in_catalog(root, code):
            raise ValueError(f"refusal code is not in the engine diagnostic catalog: {code}")
    for name in (ROUTE_CTEST, RUNTIME_CTEST, *NATIVE_CTESTS):
        if not ctest_is_registered(root, name):
            raise ValueError(f"refusal proof ctest is not registered: {name}")
    import csv  # local: only the preflight reads the fixture CSV

    with (root / FIXTURE_CSV).open(newline="", encoding="utf-8") as handle:
        fixtures = {row["surface_id"]: row for row in csv.DictReader(handle)}
    for surface_id in sorted(ENGINE_REACHED_REFUSAL_SURFACE_IDS | PACKAGE_REFUSAL_SURFACE_IDS):
        row = fixtures.get(surface_id)
        if (
            row is None
            or row.get("fixture_id") != fixture_id_for(surface_id)
            or row.get("case_kind") != "refusal"
            or row.get("expected_diagnostic_code") != refusal_code_for(surface_id)
        ):
            raise ValueError(f"{surface_id} SBSFC-057 fixture row is not the expected refusal case")
    route_text = (root / ROUTE_SOURCE).read_text(encoding="utf-8")
    for token in (PGP_CODE, PACKAGE_CODE, "DispatchSblrOperation", "AdmitServerSblrEnvelope"):
        if token not in route_text:
            raise ValueError(f"route proof source drift: {token}")
    native_text = (root / NATIVE_SOURCE).read_text(encoding="utf-8")
    for token in ("pgp_refusal", "NATIVE_NOT_IMPLEMENTED", "1024"):
        if token not in native_text:
            raise ValueError(f"native route proof source drift: {token}")


# --------------------------------------------------------------------------
# generator overrides (strict ledger / per-row manifest / matrices)
# --------------------------------------------------------------------------
def strict_ledger_classification(
    surface: dict[str, str],
    function_id: str,
    sblr_binding: str,
    engine_entrypoint: str,
) -> dict[str, str]:
    surface_id = surface["surface_id"]
    vector = ";".join(vector_for(surface_id))
    operation_id = (
        f"{function_id};sblr_binding={sblr_binding};engine_entrypoint={engine_entrypoint}"
    )
    fixture = (
        f"{FIXTURE_CSV};fixture_id={fixture_id_for(surface_id)};surface_id={surface_id};"
        "row_labeled_crypto_hash_fixture_evidence"
    )
    if is_engine_reached_refusal(surface_id):
        return {
            "current_state": "exact_refusal_passed",
            "parser_evidence": fixture,
            "binder_evidence": (
                f"{ROUTE_SOURCE};surface_label_and_canonical_crypto_function_asserted;"
                f"function_id={function_id}"
            ),
            "lowering_evidence": (
                f"{ROUTE_SOURCE};SBLR_QUERY_EVALUATE_PROJECTION;sblr_binding={sblr_binding};"
                f"lowers_to={function_id};no_source_sql_text"
            ),
            "server_admission_evidence": (
                f"ctest:{ROUTE_CTEST};server_admission_admitted;"
                "operation_id=query.evaluate_projection;engine_dispatch_required"
            ),
            "engine_runtime_evidence": vector,
            "function_or_api_operation_id": operation_id,
            "diagnostic_evidence": (
                f"canonical_message_vector_set;{PGP_CODE};refusal_kind={KIND_ENGINE_REACHED};"
                f"refusal_code={PGP_CODE};exact_refusal=true"
            ),
            "fixture_evidence": (
                f"{FIXTURE_CSV};ctest:{ROUTE_CTEST};ctest:{RUNTIME_CTEST};"
                f"source={ROUTE_SOURCE};native_gate_source={NATIVE_SOURCE}"
            ),
            "evidence_complete": "yes",
            "notes": (
                "Engine-reached refusal (closed kind engine_reached_refusal under exact_refusal_passed): the parser lowers the call, "
                "executable SBLR is emitted, verified and admitted, and the engine function is reached through its SBLR dispatch in process "
                f"and refuses {PGP_CODE} with no result row and no mutation. The engine is reached only by the in-process route (no database "
                "file); on a native database the call stops at the native dispatch gate (opcode 1024) before any engine function. This is not "
                "a positive route: real OpenPGP is not wired, and no accepted result is claimed."
            ),
        }
    return {
        "current_state": "exact_refusal_passed",
        "parser_evidence": fixture,
        "binder_evidence": (
            f"{ROUTE_SOURCE};package_capability_spelling_not_a_scalar_function;"
            f"function_id={function_id}"
        ),
        "lowering_evidence": (
            f"{ROUTE_SOURCE};SB_PGCRYPTO_PACKAGE_PROBE;parser_verifier_error_{PACKAGE_CODE};"
            "no_lowered_function_call_reaches_an_engine_envelope"
        ),
        "server_admission_evidence": (
            f"not_reached_no_executable_sblr_emitted;ctest:{ROUTE_CTEST}"
        ),
        "engine_runtime_evidence": vector,
        "function_or_api_operation_id": operation_id,
        "diagnostic_evidence": (
            f"{PACKAGE_CODE};canonical_message_vector_set;refusal_kind={KIND_PACKAGE};"
            f"refusal_code={PACKAGE_CODE};exact_refusal=true;no_mutation"
        ),
        "fixture_evidence": (
            f"{FIXTURE_CSV};ctest:{ROUTE_CTEST};ctest:{RUNTIME_CTEST};"
            f"source={ROUTE_SOURCE};native_gate_source={NATIVE_SOURCE}"
        ),
        "evidence_complete": "yes",
        "notes": (
            "pgcrypto is a package/capability spelling, not a callable scalar. The parser answers pgcrypto(...) with "
            f"{PACKAGE_CODE} (field function=pgcrypto) through a verifier error (lowering SB_PGCRYPTO_PACKAGE_PROBE); no executable SBLR "
            "reaches the engine. Same mechanism as the SBSQL.POLICY_BLOCKED pre-SBLR exact refusal."
        ),
    }


def per_row_manifest_classification(
    surface: dict[str, str],
    function_id: str,
    sblr_binding: str,
    engine_entrypoint: str,
    ctest_label: str,
) -> dict[str, str]:
    surface_id = surface["surface_id"]
    vector = ";".join(vector_for(surface_id))
    if is_engine_reached_refusal(surface_id):
        return {
            "final_state": "exact_refusal_passed",
            "ctest_label": ctest_label,
            "fixture_path": f"{FIXTURE_CSV};{ROUTE_SOURCE};{NATIVE_SOURCE}",
            "implementation_refs": (
                f"function_id={function_id};surface_id={surface_id};"
                f"sblr_binding={sblr_binding};engine_entrypoint={engine_entrypoint};"
                f"refusal_kind={KIND_ENGINE_REACHED};executable_sblr_emitted=true;engine_reached=true;"
                "engine_reached_route=in_process_no_database_file"
            ),
            "diagnostic_proof": (
                f"canonical_message_vector_set;{PGP_CODE};refusal_code={PGP_CODE};"
                f"refusal_kind={KIND_ENGINE_REACHED}"
            ),
            "result_proof": f"surface_id={surface_id};{vector}",
            "evidence_collected_utc": "static_existing_ctest_evidence",
            "promoter_slice": "SBSFC-057-engine-reached-refusal-v1",
            "notes": (
                f"Engine-reached refusal: {engine_entrypoint} is lowered, admitted and dispatched to the engine function, which refuses "
                f"{PGP_CODE} (real OpenPGP is not wired). No result row and no mutation. In-process route only: a native database stops "
                "the call at the native dispatch gate before any engine function. Not a positive route and not e2e_passed."
            ),
        }
    return {
        "final_state": "exact_refusal_passed",
        "ctest_label": ctest_label,
        "fixture_path": f"{FIXTURE_CSV};{ROUTE_SOURCE};{NATIVE_SOURCE}",
        "implementation_refs": (
            f"function_id={function_id};surface_id={surface_id};"
            f"sblr_binding={sblr_binding};engine_entrypoint={engine_entrypoint};"
            f"package_identity={PACKAGE_ENTRYPOINT};callable_function=false;pre_sblr_refusal=true;"
            f"refusal_kind={KIND_PACKAGE};executable_sblr_emitted=false"
        ),
        "diagnostic_proof": (
            f"{PACKAGE_CODE};canonical_message_vector_set;refusal_code={PACKAGE_CODE};"
            f"refusal_kind={KIND_PACKAGE}"
        ),
        "result_proof": f"surface_id={surface_id};{vector}",
        "evidence_collected_utc": "static_existing_ctest_evidence",
        "promoter_slice": "SBSFC-057-package-parser-refusal-v1",
        "notes": (
            f"pgcrypto(...) is answered at the parser with {PACKAGE_CODE} (SB_PGCRYPTO_PACKAGE_PROBE verifier error); no executable SBLR "
            "reaches the engine, no result row, no mutation. Same mechanism as the SBSQL.POLICY_BLOCKED diagnostic identity."
        ),
    }


def authenticated_route_override(
    surface: dict[str, str], classification: dict[str, str]
) -> dict[str, str]:
    surface_id = surface.get("surface_id", "")
    if is_engine_reached_refusal(surface_id):
        out = dict(classification)
        name = entrypoint_for(surface_id)
        out.update(
            {
                "credential_profile_accepted": "not_applicable_function_refused_until_openpgp_profile_is_wired",
                "credential_profile_refused": "authenticated_sbsql_session_any_valid_credential_refusal_is_function_profile_driven",
                "expected_authorization_accepted_outcome": (
                    f"not_applicable_{name}_is_refused_{PGP_CODE.replace('.', '_')}_no_accepted_route"
                ),
                "expected_authorization_refused_outcome": (
                    f"engine_function_refuses_{PGP_CODE.replace('.', '_')}_with_no_result_row_and_no_mutation_"
                    "in_process_route_native_database_route_stops_at_dispatch_gate_NATIVE_NOT_IMPLEMENTED_opcode_1024"
                ),
                "expected_diagnostic_codes": (
                    f"{PGP_CODE};SBLR.EXECUTOR.NATIVE_NOT_IMPLEMENTED;SBLR.ENVELOPE.*;SBLR.OPCODE.*"
                ),
                "fixture_status": "pending_authoring",
                "notes": (
                    f"Engine-reached refusal row: {name} is refused by the engine with {PGP_CODE} after the parser lowered, "
                    "verified and admitted the call. No accepted route exists until real OpenPGP is wired."
                ),
            }
        )
        return out
    if is_package_refusal(surface_id):
        out = dict(classification)
        out.update(
            {
                "credential_profile_accepted": "not_applicable_package_spelling_is_not_callable",
                "credential_profile_refused": "authenticated_sbsql_session_pgcrypto_call_spelling_refused",
                "transaction_profile": "not_applicable_refusal_before_executable_sblr",
                "transport_route": (
                    f"{classification['transport_route']};parser_refusal_before_sbps_submission"
                ),
                "ipc_admission_path": "not_reached_no_executable_sblr_emitted",
                "engine_admission_authority": "not_reached_pgcrypto_is_package_spelling_not_function",
                "mga_execution_authority": "not_reached_no_mutation_no_wal_authority",
                "expected_authorization_accepted_outcome": "not_applicable_pgcrypto_is_a_capability_spelling_not_a_value",
                "expected_authorization_refused_outcome": (
                    f"pgcrypto_call_refused_{PACKAGE_CODE.replace('.', '_')}_before_sblr_dispatch"
                ),
                "expected_diagnostic_codes": f"{PACKAGE_CODE}",
                "fixture_status": "pending_authoring",
                "notes": (
                    f"pgcrypto is a package/capability spelling. The parser answers pgcrypto(...) with {PACKAGE_CODE} "
                    "(function=pgcrypto) through a verifier error before executable SBLR and mutation."
                ),
            }
        )
        return out
    return classification


def binary_round_trip_override(row: dict[str, str]) -> dict[str, str]:
    surface_id = row.get("surface_id", "")
    if is_engine_reached_refusal(surface_id):
        out = dict(row)
        name = entrypoint_for(surface_id)
        out.update(
            {
                "oracle_authority_status": "per_row_engine_reached_refusal_manifest",
                "parse_phase_expectation": "parse_sbsql_text_to_cst_pass",
                "bind_phase_expectation": "bind_to_bound_ast_with_uuid_and_descriptor_pass_no_names_as_authority",
                "lower_phase_expectation": (
                    "lower_bound_ast_to_sblrexecutionenvelope_v3_pass_sb_crypto_function_call_no_text_branch_or_reference_command_as_authority"
                ),
                "binary_serialize_phase_expectation": "not_applicable_no_byte_identical_round_trip_claimed_in_process_route",
                "verify_phase_expectation": "verifier_and_server_admission_admit_the_lowered_envelope_pass",
                "binary_deserialize_phase_expectation": "not_applicable_no_byte_identical_round_trip_claimed_in_process_route",
                "dispatch_phase_expectation": "engine_sblr_dispatch_query_evaluate_projection_reaches_the_crypto_function_in_process_pass",
                "execute_phase_expectation": (
                    f"engine_function_refuses_{PGP_CODE.replace('.', '_')}_no_result_row_no_mutation"
                ),
                "render_phase_expectation": (
                    f"renderer_emit_{PGP_CODE.replace('.', '_')}_message_vector_with_operation_field_only"
                ),
                "canonical_container_magic": NOT_APPLICABLE_ROUND_TRIP,
                "canonical_container_header_size_bytes": NOT_APPLICABLE_ROUND_TRIP,
                "byte_identical_round_trip_required": NOT_APPLICABLE_ROUND_TRIP,
                "crc32c_check_required": NOT_APPLICABLE_ROUND_TRIP,
                "engine_anchored_uuids_required": "yes",
                "execution_authority_model": (
                    "mga_copy_on_write;no_wal_authority;sblr_envelope_with_uuid_and_descriptor_authority_only;"
                    "engine_reached_refusal;no_mutation"
                ),
                "fixture_status": "pending_authoring",
                "notes": (
                    f"Engine-reached refusal: {name} lowers to an executable SBLR envelope that the verifier and server admission accept; "
                    f"the engine function is reached through its SBLR dispatch in process and refuses {PGP_CODE}. No byte-identical "
                    "container round trip is claimed. On a native database the call stops at the native dispatch gate (opcode 1024)."
                ),
            }
        )
        return out
    if is_package_refusal(surface_id):
        out = dict(row)
        out.update(
            {
                "oracle_authority_status": "per_row_package_parser_refusal_manifest",
                "expected_canonical_function_or_api_operation_id": (
                    f"not_admitted_package_pgcrypto_{PACKAGE_CODE}"
                ),
                "parse_phase_expectation": "parse_presented_function_syntax_then_classify_pgcrypto_as_package_spelling",
                "bind_phase_expectation": "bind_refuses_non_callable_package_spelling_with_CRYPTO_PACKAGE_NOT_CALLABLE",
                "lower_phase_expectation": "not_reached_no_executable_sblr_emitted",
                "binary_serialize_phase_expectation": "not_applicable_no_sblr_envelope_to_serialize",
                "verify_phase_expectation": "not_applicable_no_container_to_verify",
                "binary_deserialize_phase_expectation": "not_applicable_no_container_to_deserialize",
                "dispatch_phase_expectation": "not_applicable_no_server_or_engine_dispatch",
                "execute_phase_expectation": "not_applicable_no_engine_execution_or_mutation",
                "render_phase_expectation": "renderer_emit_CRYPTO_PACKAGE_NOT_CALLABLE_message_vector_with_function_field",
                "canonical_container_magic": "not_applicable_pre_sblr_exact_refusal",
                "canonical_container_header_size_bytes": "not_applicable_pre_sblr_exact_refusal",
                "byte_identical_round_trip_required": "not_applicable_pre_sblr_exact_refusal",
                "crc32c_check_required": "not_applicable_pre_sblr_exact_refusal",
                "engine_anchored_uuids_required": "not_applicable_pre_sblr_exact_refusal",
                "execution_authority_model": (
                    "parser_package_probe_only;no_executable_sblr;no_engine_execution;no_mutation;no_wal_authority"
                ),
                "fixture_status": "pending_authoring",
                "notes": (
                    f"pgcrypto is a package/capability spelling. pgcrypto(...) refuses at the parser with {PACKAGE_CODE} before "
                    "executable SBLR through a verifier error; nothing reaches the engine."
                ),
            }
        )
        return out
    return row

