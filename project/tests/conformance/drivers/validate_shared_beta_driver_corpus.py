#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Validate shared beta driver fixture corpora without live drivers."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


MANIFEST_REL = Path(
    "project/tests/conformance/drivers/fixtures/shared_beta_driver_corpus/manifest.json"
)
GOLDEN_REL = Path(
    "project/tests/conformance/drivers/goldens/shared_beta_driver_corpus/expected.json"
)
RESOURCE_REL = Path("project/tests/conformance/resources/driver_resource_soak_requirements.json")
SECURITY_REL = Path("project/tests/security/driver_auth_replay_refusal_corpus.json")
PERFORMANCE_REL = Path("project/tests/performance/driver_resource_soak_requirements.json")
DISCOVERY_REL = Path("project/drivers/fixtures/beta_shared_conformance/manifest.json")

REQUIRED_LANGUAGES = {
    "en-US",
    "en-CA",
    "fr-CA",
    "fr-FR",
    "de-DE",
    "it-IT",
    "es-ES",
}

REQUIRED_CORPORA = {
    "shared_sbsql_commands",
    "auth_injection",
    "schema_resolution",
    "language_surfaces",
    "cache_lifecycle",
    "wire_transcript_oracles",
}

REQUIRED_COVERAGE = {
    "driver_sbsql_command_surface",
    "auth_context_injection",
    "sblr_uuid_server_revalidation",
    "cross_user_replay_refusal",
    "role_group_replay_refusal",
    "authorization_filtered_uuid_path_resolution",
    "language_surface_en-US",
    "language_surface_en-CA",
    "language_surface_fr-CA",
    "language_surface_fr-FR",
    "language_surface_de-DE",
    "language_surface_it-IT",
    "language_surface_es-ES",
    "language_fallback_standard_en",
    "cache_lifecycle",
    "wire_transcript_oracle",
    "resource_soak_requirement",
    "dbeaver_exclusion",
}

REQUIRED_CACHE_FAMILIES = {
    "sblr_preparation_cache",
    "uuid_path_resolution_cache",
    "language_resource_cache",
}

DBEAVER_PATH = "project/drivers/adaptor/scratchbird-dbeaver-driver"


def load_json(path: Path, errors: list[str]) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        errors.append(f"missing file: {path}")
    except json.JSONDecodeError as exc:
        errors.append(f"invalid json: {path}: {exc}")
    return {}


def as_list(value: Any) -> list[Any]:
    return value if isinstance(value, list) else []


def coverage_from(items: list[dict[str, Any]]) -> set[str]:
    found: set[str] = set()
    for item in items:
        found.update(str(value) for value in as_list(item.get("coverage")))
    return found


def item_id(item: dict[str, Any]) -> str:
    return str(item.get("case_id") or item.get("budget_id") or "")


def collect_cases(doc: dict[str, Any]) -> list[dict[str, Any]]:
    cases = []
    for key in ("cases", "oracles", "budgets"):
        cases.extend(item for item in as_list(doc.get(key)) if isinstance(item, dict))
    return cases


def has_true_driver_finality(value: Any) -> bool:
    if isinstance(value, dict):
        for key, nested in value.items():
            if key == "driver_finality_authority" and nested is True:
                return True
            if has_true_driver_finality(nested):
                return True
    elif isinstance(value, list):
        return any(has_true_driver_finality(item) for item in value)
    return False


def validate_manifest(
    repo_root: Path,
    manifest: dict[str, Any],
    golden: dict[str, Any],
    discovery: dict[str, Any],
    errors: list[str],
) -> dict[str, Path]:
    if manifest.get("fixture_id") != "BDRV-FIX-SHARED-001":
        errors.append("manifest fixture_id must be BDRV-FIX-SHARED-001")
    if golden.get("fixture_id") != manifest.get("fixture_id"):
        errors.append("golden fixture_id does not match manifest fixture_id")
    if manifest.get("requires_live_driver") is not False:
        errors.append("manifest must declare requires_live_driver=false")
    if manifest.get("requires_live_server") is not False:
        errors.append("manifest must declare requires_live_server=false")
    if manifest.get("transaction_authority") != "engine_mga_only":
        errors.append("manifest transaction_authority must be engine_mga_only")
    if set(manifest.get("language_profiles", [])) != REQUIRED_LANGUAGES:
        errors.append("manifest language_profiles do not cover the required beta language set")
    if set(golden.get("expected_language_profiles", [])) != REQUIRED_LANGUAGES:
        errors.append("golden expected_language_profiles do not cover the required beta language set")
    if set(manifest.get("covered_requirements", [])) != REQUIRED_COVERAGE:
        errors.append("manifest covered_requirements drifted from required coverage")
    if set(golden.get("required_coverage", [])) != REQUIRED_COVERAGE:
        errors.append("golden required_coverage drifted from required coverage")

    excluded_adapters = as_list(manifest.get("excluded_adapters"))
    if not any(item.get("adapter") == "scratchbird-dbeaver-driver" for item in excluded_adapters if isinstance(item, dict)):
        errors.append("manifest must explicitly exclude scratchbird-dbeaver-driver")
    if any(str(ref.get("path", "")).startswith(DBEAVER_PATH) for ref in as_list(manifest.get("corpora")) if isinstance(ref, dict)):
        errors.append("manifest corpus paths must not point at the DBeaver adapter tree")

    if discovery.get("fixture_manifest") != str(MANIFEST_REL):
        errors.append("driver fixture discovery manifest points at the wrong fixture manifest")
    if discovery.get("requires_live_driver") is not False:
        errors.append("driver fixture discovery manifest must be static only")

    corpus_refs: dict[str, Path] = {}
    for ref in as_list(manifest.get("corpora")):
        if not isinstance(ref, dict):
            errors.append("manifest corpora entries must be objects")
            continue
        corpus_id = str(ref.get("corpus_id", ""))
        path_text = str(ref.get("path", ""))
        if not corpus_id:
            errors.append("manifest corpus entry missing corpus_id")
        if not path_text:
            errors.append(f"manifest corpus {corpus_id} missing path")
        if path_text.startswith(DBEAVER_PATH):
            errors.append(f"manifest corpus {corpus_id} is under excluded DBeaver path")
        corpus_refs[corpus_id] = repo_root / path_text

    if set(corpus_refs) != REQUIRED_CORPORA:
        errors.append(f"manifest corpora must be {sorted(REQUIRED_CORPORA)}, got {sorted(corpus_refs)}")
    return corpus_refs


def validate_case_ids(corpus_cases: dict[str, list[dict[str, Any]]], errors: list[str]) -> None:
    seen: set[str] = set()
    for corpus_id, cases in corpus_cases.items():
        for case in cases:
            cid = item_id(case)
            if not cid:
                errors.append(f"{corpus_id}: case missing case_id or budget_id")
                continue
            if not cid.startswith("BDRV-"):
                errors.append(f"{corpus_id}: case id {cid} must start with BDRV-")
            if cid in seen:
                errors.append(f"duplicate case id: {cid}")
            seen.add(cid)


def validate_server_revalidation(corpus_cases: dict[str, list[dict[str, Any]]], errors: list[str]) -> None:
    for corpus_id, cases in corpus_cases.items():
        for case in cases:
            category = case.get("category")
            if category == "scope_exclusion":
                if case.get("server_revalidation") != "not_applicable":
                    errors.append(f"{item_id(case)}: scope exclusions must mark server_revalidation not_applicable")
                continue
            if corpus_id in {"shared_sbsql_commands", "auth_injection", "schema_resolution", "language_surfaces", "cache_lifecycle", "wire_transcript_oracles"}:
                if case.get("server_revalidation") != "required":
                    errors.append(f"{item_id(case)}: server_revalidation must be required")


def validate_counts(
    corpus_cases: dict[str, list[dict[str, Any]]],
    golden: dict[str, Any],
    errors: list[str],
) -> None:
    expected_counts = golden.get("expected_case_counts", {})
    if not isinstance(expected_counts, dict):
        errors.append("golden expected_case_counts must be an object")
        return
    for corpus_id in REQUIRED_CORPORA:
        expected = expected_counts.get(corpus_id)
        actual = len(corpus_cases.get(corpus_id, []))
        if expected != actual:
            errors.append(f"{corpus_id}: expected {expected} cases, found {actual}")


def validate_language_corpus(doc: dict[str, Any], cases: list[dict[str, Any]], errors: list[str]) -> None:
    profiles = as_list(doc.get("profiles"))
    profile_tags = {str(profile.get("language_tag")) for profile in profiles if isinstance(profile, dict)}
    if profile_tags != REQUIRED_LANGUAGES:
        errors.append("language corpus profiles do not cover all required languages")
    for profile in profiles:
        if not isinstance(profile, dict):
            continue
        tag = str(profile.get("language_tag"))
        if profile.get("standard_english_fallback") is not True:
            errors.append(f"language profile {tag} must declare standard_english_fallback=true")
        chain = set(str(value) for value in as_list(profile.get("fallback_chain")))
        if not ({"en", "en-US"} & chain):
            errors.append(f"language profile {tag} must fall back to standard English")

    rendered_tags = {str(case.get("language_tag")) for case in cases if case.get("category") == "language_rendering"}
    if rendered_tags != REQUIRED_LANGUAGES:
        errors.append("language rendering cases do not cover all required language tags")
    if not any("language_fallback_standard_en" in case.get("coverage", []) for case in cases):
        errors.append("language corpus must include a standard English fallback case")
    for case in cases:
        if case.get("localized_text_is_not_authority") is not True:
            errors.append(f"{item_id(case)}: localized_text_is_not_authority must be true")


def validate_schema_corpus(cases: list[dict[str, Any]], errors: list[str]) -> None:
    directions = {str(case.get("resolution_direction")) for case in cases}
    if not {"path_to_uuid", "uuid_to_path"}.issubset(directions):
        errors.append("schema corpus must include both path_to_uuid and uuid_to_path resolution")
    for case in cases:
        if case.get("authorization_filter") != "required":
            errors.append(f"{item_id(case)}: authorization_filter must be required")


def validate_cache_corpus(doc: dict[str, Any], cases: list[dict[str, Any]], errors: list[str]) -> None:
    families = set(str(value) for value in as_list(doc.get("cache_families")))
    if families != REQUIRED_CACHE_FAMILIES:
        errors.append("cache corpus cache_families must cover SBLR, UUID/path, and language resources")
    for case in cases:
        if str(case.get("cache_family", "")) not in REQUIRED_CACHE_FAMILIES:
            errors.append(f"{item_id(case)}: cache_family is missing or unsupported")
        if not case.get("expected_action"):
            errors.append(f"{item_id(case)}: cache case missing expected_action")


def validate_wire_corpus(doc: dict[str, Any], cases: list[dict[str, Any]], errors: list[str]) -> None:
    if len(cases) < 3:
        errors.append("wire transcript corpus must include at least three oracles")
    if not any(case.get("category") == "replay_refusal" for case in cases):
        errors.append("wire transcript corpus must include a replay_refusal oracle")
    for oracle in cases:
        forbidden = set(str(value) for value in as_list(oracle.get("forbidden_frames")))
        for step in as_list(oracle.get("steps")):
            if not isinstance(step, dict):
                errors.append(f"{item_id(oracle)}: wire steps must be objects")
                continue
            frame = str(step.get("frame", ""))
            if frame in forbidden:
                errors.append(f"{item_id(oracle)}: forbidden frame appears in steps: {frame}")
        if has_true_driver_finality(oracle):
            errors.append(f"{item_id(oracle)}: driver_finality_authority must never be true")


def validate_security_static(doc: dict[str, Any], errors: list[str]) -> set[str]:
    if doc.get("requires_live_driver") is not False:
        errors.append("security corpus must declare requires_live_driver=false")
    cases = collect_cases(doc)
    coverage = coverage_from(cases)
    if "cross_user_replay_refusal" not in coverage:
        errors.append("security corpus must cover cross-user replay refusal")
    if "role_group_replay_refusal" not in coverage:
        errors.append("security corpus must cover role/group replay refusal")
    for case in cases:
        if case.get("requires_server_revalidation") is not True:
            errors.append(f"{item_id(case)}: security case must require server revalidation")
    return coverage


def validate_resource_static(doc: dict[str, Any], errors: list[str]) -> set[str]:
    if doc.get("requires_live_driver") is not False:
        errors.append("resource soak requirements must declare requires_live_driver=false")
    cases = collect_cases(doc)
    coverage = coverage_from(cases)
    if "resource_soak_requirement" not in coverage:
        errors.append("resource requirements must cover resource_soak_requirement")
    language_sets = [
        set(str(value) for value in as_list(case.get("required_language_profiles")))
        for case in cases
        if case.get("required_language_profiles")
    ]
    if not any(values == REQUIRED_LANGUAGES for values in language_sets):
        errors.append("resource requirements must include the full beta language profile set")
    return coverage


def validate_performance_static(doc: dict[str, Any], errors: list[str]) -> set[str]:
    if doc.get("requires_live_driver") is not False:
        errors.append("performance soak requirements must declare requires_live_driver=false")
    cases = collect_cases(doc)
    coverage = coverage_from(cases)
    if "resource_soak_requirement" not in coverage:
        errors.append("performance requirements must cover resource_soak_requirement")
    for budget in cases:
        if budget.get("requires_live_driver") is not False:
            errors.append(f"{item_id(budget)}: budget must declare requires_live_driver=false")
        if budget.get("maximum_stale_cache_acceptances", 0) != 0:
            errors.append(f"{item_id(budget)}: stale cache acceptances must be zero")
        if budget.get("maximum_driver_finality_frames", 0) != 0:
            errors.append(f"{item_id(budget)}: driver finality frames must be zero")
    return coverage


def validate(repo_root: Path) -> list[str]:
    errors: list[str] = []
    manifest = load_json(repo_root / MANIFEST_REL, errors)
    golden = load_json(repo_root / GOLDEN_REL, errors)
    resource = load_json(repo_root / RESOURCE_REL, errors)
    security = load_json(repo_root / SECURITY_REL, errors)
    performance = load_json(repo_root / PERFORMANCE_REL, errors)
    discovery = load_json(repo_root / DISCOVERY_REL, errors)

    corpus_refs = validate_manifest(repo_root, manifest, golden, discovery, errors)

    corpus_docs: dict[str, dict[str, Any]] = {}
    corpus_cases: dict[str, list[dict[str, Any]]] = {}
    for corpus_id, path in corpus_refs.items():
        doc = load_json(path, errors)
        corpus_docs[corpus_id] = doc
        if doc.get("corpus_id") != corpus_id:
            errors.append(f"{path}: corpus_id does not match manifest entry {corpus_id}")
        corpus_cases[corpus_id] = collect_cases(doc)

    validate_case_ids(corpus_cases, errors)
    validate_server_revalidation(corpus_cases, errors)
    validate_counts(corpus_cases, golden, errors)

    if "language_surfaces" in corpus_docs:
        validate_language_corpus(corpus_docs["language_surfaces"], corpus_cases.get("language_surfaces", []), errors)
    if "schema_resolution" in corpus_docs:
        validate_schema_corpus(corpus_cases.get("schema_resolution", []), errors)
    if "cache_lifecycle" in corpus_docs:
        validate_cache_corpus(corpus_docs["cache_lifecycle"], corpus_cases.get("cache_lifecycle", []), errors)
    if "wire_transcript_oracles" in corpus_docs:
        validate_wire_corpus(corpus_docs["wire_transcript_oracles"], corpus_cases.get("wire_transcript_oracles", []), errors)

    coverage = set()
    for cases in corpus_cases.values():
        coverage.update(coverage_from(cases))
    coverage.update(validate_resource_static(resource, errors))
    coverage.update(validate_security_static(security, errors))
    coverage.update(validate_performance_static(performance, errors))

    missing_coverage = REQUIRED_COVERAGE - coverage
    if missing_coverage:
        errors.append(f"missing required coverage keys: {sorted(missing_coverage)}")

    expected_static_inputs = set(str(value) for value in as_list(golden.get("expected_static_inputs")))
    related_static_inputs = set(str(value) for value in as_list(manifest.get("related_static_inputs")))
    if expected_static_inputs != related_static_inputs:
        errors.append("manifest related_static_inputs must match golden expected_static_inputs")

    for rel in expected_static_inputs:
        if rel.startswith(DBEAVER_PATH):
            errors.append(f"static input under excluded DBeaver path: {rel}")
        if not (repo_root / rel).exists():
            errors.append(f"golden expected static input does not exist: {rel}")

    return errors


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parents[4],
        help="ScratchBird repository root. Defaults to this script's repository.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    repo_root = args.repo_root.resolve()
    errors = validate(repo_root)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("shared beta driver fixture corpus: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
