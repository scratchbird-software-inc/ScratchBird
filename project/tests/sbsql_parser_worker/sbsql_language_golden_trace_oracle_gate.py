#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SML-089 golden corpus trace oracle and stable hash gate."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import re
import sys
import uuid
from pathlib import Path
from typing import Any


GATE_ID = "SML-GATE-089"
TRACK_ID = "SML-089"
SCHEMA_VERSION = "sbsql.language_golden_trace_oracle.v1"
DEFAULT_FIXTURE_ROOT = (
    "project/tests/sbsql_parser_worker/fixtures/language_golden_trace"
)
CORPUS_NAME = "golden_trace_corpus.json"

REQUIRED_CLASSES = {
    "positive",
    "negative",
    "fallback",
    "ambiguity",
    "renderer_lossiness",
    "unicode",
    "revoked",
    "expired",
    "incompatible_resource",
    "localized",
    "standard",
    "canonical_element_stream",
    "sblr_uuid_reference",
    "preferred_render",
    "parse_back",
    "diagnostic_path",
}

REQUIRED_HASH_KEYS = [
    "source_sha256",
    "canonical_stream_sha256",
    "semantic_stream_sha256",
    "sblr_reference_sha256",
    "render_sha256",
    "parse_back_stream_sha256",
    "parse_back_semantic_stream_sha256",
    "diagnostics_sha256",
    "trace_sha256",
]

FAILURE_DIAGNOSTICS = {
    "negative": {"SBSQL.LANG_RESOURCE.MISSING"},
    "fallback": {"SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH"},
    "ambiguity": {"SBSQL.LANG_RESOURCE.AMBIGUOUS_FALLBACK"},
    "renderer_lossiness": {"SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED"},
    "revoked": {"SBSQL.LANG_RESOURCE.REVOKED"},
    "expired": {"SBSQL.LANG_RESOURCE.EXPIRED"},
    "incompatible_resource": {"SBSQL.LANG_RESOURCE.INCOMPATIBLE"},
}

RENDER_DIAGNOSTIC_BY_DECISION = {
    "preferred_language": "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED",
    "canonical_english_fallback": "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
    "refuse_missing_canonical_authority": (
        "SBSQL.LANG_RESOURCE.MISSING_CANONICAL_AUTHORITY"
    ),
    "refuse_revoked_resource": "SBSQL.LANG_RESOURCE.REVOKED",
    "refuse_incompatible_resource": "SBSQL.LANG_RESOURCE.INCOMPATIBLE",
    "refuse_source_reconstruction": (
        "SBSQL.LANG_RESOURCE.RENDERER_SOURCE_RECONSTRUCTION_FORBIDDEN"
    ),
    "refuse_renderer_unavailable": "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE",
}

ALLOWED_DIAGNOSTIC_FIELDS = {
    "diagnostic_contract",
    "failure_kind",
    "disclosure_state",
    "private_input_state",
    "resource_identity_state",
    "profile_identity_state",
    "input_text_state",
    "identifier_evidence_state",
    "source_location_state",
    "local_sblr_state",
    "telemetry_redaction",
    "support_bundle_redaction",
    "server_revalidation_required",
    "render_decision",
    "selected_language_profile",
    "fallback_language_profile",
    "lossiness",
    "parse_profile_order",
    "canonical_stream_state",
    "ambiguity_state",
    "resource_lifecycle_state",
}

FORBIDDEN_DIAGNOSTIC_TEXT = (
    "hidden_table",
    "local_password",
    "provider.local_password",
    "draft_sblr_local_secret",
    "/tmp/secret",
)

NETWORK_MODULE_NAMES = (
    "".join(("url", "lib")),
    "http",
    "".join(("so", "cket")),
    "ssl",
    "".join(("ft", "plib")),
    "".join(("sm", "tplib")),
    "".join(("telnet", "lib")),
    "".join(("imap", "lib")),
    "".join(("pop", "lib")),
    "".join(("nntp", "lib")),
    "".join(("xml", "rpc")),
    "".join(("web", "browser")),
    "".join(("re", "quests")),
    "httpx",
    "aiohttp",
    "".join(("url", "lib3")),
    "pycurl",
    "paramiko",
    "asyncssh",
    "botocore",
    "boto3",
    "google.cloud",
    "google.api_core",
)

NETWORK_MODULE_RE = re.compile(
    r"^\s*(?:import|from)\s+("
    + "|".join(re.escape(name) for name in NETWORK_MODULE_NAMES)
    + r")(?:[\.\s]|$)",
    re.MULTILINE,
)


class GateError(AssertionError):
    pass


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def stable_hash(value: Any) -> str:
    return hashlib.sha256(canonical_json(value)).hexdigest()


def text_hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise GateError(f"golden trace corpus missing: {path}")
    with path.open(encoding="utf-8") as handle:
        loaded = json.load(handle)
    if not isinstance(loaded, dict):
        raise GateError("golden trace corpus root must be an object")
    return loaded


def scan_gate_no_network(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    findings: list[str] = []
    for match in NETWORK_MODULE_RE.finditer(text):
        line_num = text[: match.start()].count("\n") + 1
        findings.append(f"{path.name}:{line_num} forbidden_network_import={match.group(1)!r}")
    return findings


def scan_fixture_no_external_locator(value: Any, path: str, errors: list[str]) -> None:
    if isinstance(value, dict):
        for key, item in value.items():
            scan_fixture_no_external_locator(item, f"{path}.{key}", errors)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            scan_fixture_no_external_locator(item, f"{path}[{index}]", errors)
    elif isinstance(value, str):
        lowered = value.lower()
        for forbidden in ("http://", "https://", "ftp://", "s3://"):
            if forbidden in lowered:
                errors.append(f"{path} contains external locator {forbidden!r}")


def byte_slice(source: str, offset: int, length: int) -> str | None:
    source_bytes = source.encode("utf-8")
    if offset < 0 or length <= 0 or offset + length > len(source_bytes):
        return None
    try:
        return source_bytes[offset : offset + length].decode("utf-8")
    except UnicodeDecodeError:
        return None


def validate_uuid(value: Any, context: str, errors: list[str]) -> None:
    if not isinstance(value, str):
        errors.append(f"{context} must be a UUID string")
        return
    try:
        uuid.UUID(value)
    except ValueError:
        errors.append(f"{context} is not a valid UUID: {value!r}")


def semantic_stream(stream: dict[str, Any] | None) -> Any:
    if stream is None:
        return None
    return {
        "resource_identity": stream.get("resource_identity"),
        "dialect_profile_uuid": stream.get("dialect_profile_uuid"),
        "topology_profile_uuid": stream.get("topology_profile_uuid"),
        "common_resource_hash": stream.get("common_resource_hash"),
        "canonical_order_id": stream.get("canonical_order_id"),
        "elements": [
            {
                "kind": element.get("kind"),
                "canonical_text": element.get("canonical_text"),
                "canonical_id": element.get("canonical_id"),
                "surface_id": element.get("surface_id"),
                "slot_id": element.get("slot_id"),
                "alias_id": element.get("alias_id"),
                "topology_role": element.get("topology_role"),
            }
            for element in stream.get("elements", [])
        ],
    }


def validate_canonical_stream(
    stream: Any,
    source_text: str,
    context: str,
    errors: list[str],
) -> None:
    if not isinstance(stream, dict):
        errors.append(f"{context} canonical element stream must be an object")
        return

    for key in (
        "resource_identity",
        "language_profile_uuid",
        "exact_tag",
        "dialect_profile_uuid",
        "topology_profile_uuid",
        "common_resource_hash",
        "source_hash",
        "canonical_order_id",
    ):
        require(bool(stream.get(key)), f"{context}.{key} missing", errors)

    validate_uuid(stream.get("language_profile_uuid"), f"{context}.language_profile_uuid", errors)
    require(
        stream.get("canonical_order_id") == "sbsql.canonical_order.v1",
        f"{context}.canonical_order_id drifted",
        errors,
    )
    require(
        stream.get("normalized_before_uuid_resolution") is True,
        f"{context} must normalize before UUID resolution",
        errors,
    )
    require(
        stream.get("server_revalidation_required") is True,
        f"{context} must require server revalidation",
        errors,
    )
    require(
        stream.get("source_hash") == text_hash(source_text),
        f"{context}.source_hash does not match input text",
        errors,
    )

    elements = stream.get("elements")
    if not isinstance(elements, list) or not elements:
        errors.append(f"{context}.elements must be a non-empty list")
        return

    for index, element in enumerate(elements):
        if not isinstance(element, dict):
            errors.append(f"{context}.elements[{index}] must be an object")
            continue
        element_context = f"{context}.elements[{index}]"
        for key in (
            "kind",
            "canonical_text",
            "canonical_id",
            "slot_id",
            "alias_id",
            "topology_role",
            "localized_text_hash",
            "source_text",
        ):
            require(bool(element.get(key)), f"{element_context}.{key} missing", errors)

        source_span = element.get("source_span")
        if not isinstance(source_span, dict):
            errors.append(f"{element_context}.source_span missing")
            continue
        offset = source_span.get("offset")
        length = source_span.get("length")
        if not isinstance(offset, int) or not isinstance(length, int):
            errors.append(f"{element_context}.source_span offset/length must be integers")
            continue
        span_text = byte_slice(source_text, offset, length)
        require(
            span_text == element.get("source_text"),
            f"{element_context}.source_span does not match source_text",
            errors,
        )
        require(
            element.get("localized_text_hash") == text_hash(str(element.get("source_text", ""))),
            f"{element_context}.localized_text_hash drifted",
            errors,
        )


def validate_sblr_reference(
    sblr: Any,
    computed_canonical_hash: str,
    source_hash: str,
    context: str,
    errors: list[str],
) -> None:
    if not isinstance(sblr, dict):
        errors.append(f"{context}.sblr must be an object")
        return
    for key in ("operation_uuid", "statement_uuid", "resource_uuid"):
        validate_uuid(sblr.get(key), f"{context}.sblr.{key}", errors)

    refs = sblr.get("reference_fields")
    if not isinstance(refs, dict):
        errors.append(f"{context}.sblr.reference_fields must be an object")
        return
    for key in (
        "operation_family",
        "operation_name",
        "canonical_stream_hash_ref",
        "source_hash_ref",
        "resource_identity_ref",
    ):
        require(bool(refs.get(key)), f"{context}.sblr.reference_fields.{key} missing", errors)
    require(
        refs.get("canonical_stream_hash_ref") == computed_canonical_hash,
        f"{context}.sblr canonical stream hash reference drifted",
        errors,
    )
    require(
        refs.get("source_hash_ref") == source_hash,
        f"{context}.sblr source hash reference drifted",
        errors,
    )


def validate_render(render: Any, classes: set[str], context: str, errors: list[str]) -> None:
    if render is None:
        return
    if not isinstance(render, dict):
        errors.append(f"{context}.render must be an object when present")
        return

    decision = render.get("decision")
    diagnostic_code = render.get("diagnostic_code")
    require(decision in RENDER_DIAGNOSTIC_BY_DECISION, f"{context}.render decision unsupported", errors)
    require(
        diagnostic_code == RENDER_DIAGNOSTIC_BY_DECISION.get(str(decision)),
        f"{context}.render diagnostic code does not match decision",
        errors,
    )
    require(
        render.get("server_revalidation_required") is True,
        f"{context}.render must require server revalidation",
        errors,
    )

    text = render.get("text")
    require(isinstance(text, str), f"{context}.render.text must be a string", errors)
    if isinstance(text, str):
        require(
            render.get("text_sha256") == text_hash(text),
            f"{context}.render.text_sha256 drifted",
            errors,
        )

    if "preferred_render" in classes:
        require(decision == "preferred_language", f"{context} preferred render decision drifted", errors)
        require(
            render.get("used_canonical_english_fallback") is False,
            f"{context} preferred render unexpectedly used fallback",
            errors,
        )
    if "fallback" in classes:
        require(
            decision == "canonical_english_fallback",
            f"{context} fallback render decision drifted",
            errors,
        )
        require(
            render.get("used_canonical_english_fallback") is True,
            f"{context} fallback flag not retained",
            errors,
        )
        require(
            render.get("lossiness") == "canonical_english_fallback",
            f"{context} fallback lossiness drifted",
            errors,
        )
    if "renderer_lossiness" in classes:
        require(
            render.get("lossiness") in {
                "canonical_equivalent",
                "preferred_language_partial",
                "lossless_canonical",
            },
            f"{context} renderer lossiness class missing",
            errors,
        )
    if "revoked" in classes:
        require(decision == "refuse_revoked_resource", f"{context} revoked render did not refuse", errors)
    if "incompatible_resource" in classes:
        require(
            decision == "refuse_incompatible_resource",
            f"{context} incompatible render did not refuse",
            errors,
        )


def validate_parse_back(
    parse_back: Any,
    original_stream: dict[str, Any] | None,
    original_semantic_hash: str,
    context: str,
    errors: list[str],
) -> None:
    if not isinstance(parse_back, dict):
        errors.append(f"{context}.parse_back must be an object")
        return
    input_text = parse_back.get("input_text")
    if not isinstance(input_text, str):
        errors.append(f"{context}.parse_back.input_text must be a string")
        return
    require(
        parse_back.get("input_sha256") == text_hash(input_text),
        f"{context}.parse_back.input_sha256 drifted",
        errors,
    )
    stream = parse_back.get("canonical_element_stream")
    validate_canonical_stream(stream, input_text, f"{context}.parse_back", errors)
    parse_semantic_hash = stable_hash(semantic_stream(stream if isinstance(stream, dict) else None))
    require(
        parse_semantic_hash == original_semantic_hash,
        f"{context}.parse_back semantic canonical stream changed",
        errors,
    )
    require(
        original_stream is not None,
        f"{context}.parse_back cannot exist without an original stream",
        errors,
    )


def validate_diagnostics(
    diagnostics: Any,
    classes: set[str],
    context: str,
    errors: list[str],
) -> None:
    if not isinstance(diagnostics, list):
        errors.append(f"{context}.diagnostics must be a list")
        return
    if classes.intersection(FAILURE_DIAGNOSTICS) or "diagnostic_path" in classes:
        require(diagnostics, f"{context} missing diagnostic path evidence", errors)
    if "positive" in classes and "diagnostic_path" not in classes:
        require(not diagnostics, f"{context} positive case should not emit diagnostics", errors)

    codes = {
        item.get("code")
        for item in diagnostics
        if isinstance(item, dict) and isinstance(item.get("code"), str)
    }
    for class_name, expected_codes in FAILURE_DIAGNOSTICS.items():
        if class_name in classes:
            require(
                bool(codes.intersection(expected_codes)),
                f"{context} missing diagnostic code for {class_name}",
                errors,
            )

    rendered = json.dumps(diagnostics, ensure_ascii=False, sort_keys=True)
    for forbidden in FORBIDDEN_DIAGNOSTIC_TEXT:
        require(
            forbidden not in rendered,
            f"{context} diagnostic disclosed forbidden private text {forbidden!r}",
            errors,
        )

    for index, diagnostic in enumerate(diagnostics):
        if not isinstance(diagnostic, dict):
            errors.append(f"{context}.diagnostics[{index}] must be an object")
            continue
        diag_context = f"{context}.diagnostics[{index}]"
        require(str(diagnostic.get("code", "")).startswith("SBSQL."), f"{diag_context}.code drifted", errors)
        require(
            diagnostic.get("severity") in {"INFO", "WARNING", "ERROR"},
            f"{diag_context}.severity unsupported",
            errors,
        )
        require(
            diagnostic.get("component") == "sbp_sbsql.language_resource",
            f"{diag_context}.component drifted",
            errors,
        )
        path = diagnostic.get("path")
        require(
            isinstance(path, list) and path and path[0] == "sml089",
            f"{diag_context}.path must start at sml089",
            errors,
        )
        public_fields = diagnostic.get("public_fields")
        if not isinstance(public_fields, list):
            errors.append(f"{diag_context}.public_fields must be a list")
            continue
        for field_index, field in enumerate(public_fields):
            if not isinstance(field, dict):
                errors.append(f"{diag_context}.public_fields[{field_index}] must be an object")
                continue
            name = field.get("name")
            require(
                name in ALLOWED_DIAGNOSTIC_FIELDS,
                f"{diag_context}.public_fields[{field_index}] field {name!r} not public",
                errors,
            )


def compute_case_hashes(case: dict[str, Any]) -> dict[str, str]:
    case_without_expected = copy.deepcopy(case)
    case_without_expected.pop("expected_hashes", None)

    input_text = case.get("input", {}).get("text")
    canonical_stream = case.get("canonical_element_stream")
    parse_back = case.get("parse_back")
    parse_back_stream = (
        parse_back.get("canonical_element_stream")
        if isinstance(parse_back, dict)
        else None
    )

    return {
        "source_sha256": text_hash(input_text) if isinstance(input_text, str) else stable_hash(None),
        "canonical_stream_sha256": stable_hash(canonical_stream),
        "semantic_stream_sha256": stable_hash(semantic_stream(canonical_stream)),
        "sblr_reference_sha256": stable_hash(case.get("sblr")),
        "render_sha256": stable_hash(case.get("render")),
        "parse_back_stream_sha256": stable_hash(parse_back_stream),
        "parse_back_semantic_stream_sha256": stable_hash(semantic_stream(parse_back_stream)),
        "diagnostics_sha256": stable_hash(case.get("diagnostics")),
        "trace_sha256": stable_hash(case_without_expected),
    }


def corpus_with_computed_hashes(corpus: dict[str, Any]) -> dict[str, Any]:
    updated = copy.deepcopy(corpus)
    updated.pop("expected_manifest_hash", None)
    for case in updated.get("cases", []):
        if isinstance(case, dict):
            case["expected_hashes"] = compute_case_hashes(case)
    return updated


def validate_case(case: Any, errors: list[str]) -> dict[str, str] | None:
    if not isinstance(case, dict):
        errors.append("each golden trace case must be an object")
        return None
    case_id = str(case.get("case_id", ""))
    context = case_id or "<missing case_id>"
    require(case_id.startswith("SML-089-"), f"{context} case_id must be SML-089-prefixed", errors)

    classes_value = case.get("classes")
    if not isinstance(classes_value, list) or not all(isinstance(item, str) for item in classes_value):
        errors.append(f"{context}.classes must be a string list")
        return None
    classes = set(classes_value)

    input_obj = case.get("input")
    if not isinstance(input_obj, dict):
        errors.append(f"{context}.input must be an object")
        return None
    input_text = input_obj.get("text")
    require(isinstance(input_text, str), f"{context}.input.text must be a string", errors)
    input_profile = input_obj.get("profile")
    require(input_profile in {"localized", "standard"}, f"{context}.input.profile unsupported", errors)
    if input_profile in {"localized", "standard"}:
        require(input_profile in classes, f"{context}.classes missing input profile {input_profile}", errors)
    if isinstance(input_text, str):
        require(
            input_obj.get("source_sha256") == text_hash(input_text),
            f"{context}.input.source_sha256 drifted",
            errors,
        )
    if "unicode" in classes:
        require(
            isinstance(input_text, str) and any(ord(ch) > 0x7F for ch in input_text),
            f"{context} unicode class requires non-ASCII source",
            errors,
        )

    canonical_stream = case.get("canonical_element_stream")
    if "canonical_element_stream" in classes:
        validate_canonical_stream(canonical_stream, str(input_text), context, errors)
    elif canonical_stream is not None:
        errors.append(f"{context} has canonical stream without class coverage")

    computed_hashes = compute_case_hashes(case)
    source_hash = computed_hashes["source_sha256"]
    canonical_hash = computed_hashes["canonical_stream_sha256"]
    semantic_hash = computed_hashes["semantic_stream_sha256"]

    if "sblr_uuid_reference" in classes:
        validate_sblr_reference(case.get("sblr"), canonical_hash, source_hash, context, errors)
    elif case.get("sblr") is not None:
        errors.append(f"{context} has SBLR reference without class coverage")

    validate_render(case.get("render"), classes, context, errors)

    if "parse_back" in classes:
        validate_parse_back(
            case.get("parse_back"),
            canonical_stream if isinstance(canonical_stream, dict) else None,
            semantic_hash,
            context,
            errors,
        )
    elif case.get("parse_back") is not None:
        errors.append(f"{context} has parse_back without class coverage")

    validate_diagnostics(case.get("diagnostics"), classes, context, errors)

    expected = case.get("expected_hashes")
    if not isinstance(expected, dict):
        errors.append(f"{context}.expected_hashes missing")
        return computed_hashes
    require(
        list(expected.keys()) == REQUIRED_HASH_KEYS,
        f"{context}.expected_hashes keys/order drifted",
        errors,
    )
    for key in REQUIRED_HASH_KEYS:
        require(
            expected.get(key) == computed_hashes[key],
            f"{context}.{key} drifted expected={expected.get(key)} computed={computed_hashes[key]}",
            errors,
        )
    return computed_hashes


def validate_corpus(corpus: dict[str, Any], fixture_root: Path) -> list[str]:
    errors: list[str] = []
    errors.extend(scan_gate_no_network(Path(__file__).resolve()))
    scan_fixture_no_external_locator(corpus, "corpus", errors)

    require(corpus.get("schema_version") == SCHEMA_VERSION, "schema_version drifted", errors)
    require(corpus.get("gate_id") == GATE_ID, "gate_id drifted", errors)
    require(corpus.get("track_id") == TRACK_ID, "track_id drifted", errors)
    require(
        corpus.get("hash_algorithm") == "sha256",
        "hash_algorithm must remain sha256",
        errors,
    )
    require(
        corpus.get("fixture_root") == DEFAULT_FIXTURE_ROOT,
        "fixture_root must remain repo-local and stable",
        errors,
    )
    require(fixture_root.is_dir(), f"fixture root missing: {fixture_root}", errors)

    cases = corpus.get("cases")
    if not isinstance(cases, list) or not cases:
        errors.append("golden trace corpus must contain cases")
        return errors

    seen: set[str] = set()
    covered: set[str] = set()
    for case in cases:
        if isinstance(case, dict):
            case_id = str(case.get("case_id", ""))
            require(case_id not in seen, f"duplicate case_id {case_id}", errors)
            seen.add(case_id)
            classes = case.get("classes", [])
            if isinstance(classes, list):
                covered.update(item for item in classes if isinstance(item, str))
        validate_case(case, errors)

    missing = sorted(REQUIRED_CLASSES - covered)
    require(not missing, f"missing required SML-089 corpus classes: {missing}", errors)

    expected_class_list = corpus.get("required_classes")
    require(
        expected_class_list == sorted(REQUIRED_CLASSES),
        "required_classes list drifted",
        errors,
    )

    manifest_without_expected = copy.deepcopy(corpus)
    manifest_without_expected.pop("expected_manifest_hash", None)
    manifest_hash = stable_hash(manifest_without_expected)
    require(
        corpus.get("expected_manifest_hash") == manifest_hash,
        "expected_manifest_hash drifted "
        f"expected={corpus.get('expected_manifest_hash')} computed={manifest_hash}",
        errors,
    )
    return errors


def dump_computed_hashes(corpus: dict[str, Any]) -> None:
    updated = corpus_with_computed_hashes(corpus)
    manifest_hash = stable_hash(updated)
    payload = {
        "expected_manifest_hash": manifest_hash,
        "case_hashes": {
            case["case_id"]: case["expected_hashes"]
            for case in updated.get("cases", [])
            if isinstance(case, dict) and "case_id" in case
        },
    }
    print(json.dumps(payload, indent=2, sort_keys=True))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--fixture-root", type=Path, default=None)
    parser.add_argument(
        "--dump-computed-hashes",
        action="store_true",
        help="print current computed hashes for fixture maintenance",
    )
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    fixture_root = args.fixture_root
    if fixture_root is None:
        fixture_root = repo_root / DEFAULT_FIXTURE_ROOT
    elif not fixture_root.is_absolute():
        fixture_root = repo_root / fixture_root
    fixture_root = fixture_root.resolve()

    corpus = load_json(fixture_root / CORPUS_NAME)
    if args.dump_computed_hashes:
        dump_computed_hashes(corpus)
        return 0

    errors = validate_corpus(corpus, fixture_root)
    if errors:
        print("sbsql_language_golden_trace_oracle_gate=failed", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    print(
        "sbsql_language_golden_trace_oracle_gate=passed "
        f"cases={len(corpus.get('cases', []))} gate={GATE_ID}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
