#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Offline proof gate for beta SBsql language support.

This gate ties the public language-resource pack, driver-facing resource
manifest, stable database messages, online verification corpus, and public
limitations document together before driver-specific work consumes the language
surface. It deliberately does not contact online translation providers.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


PACK_REL = "project/resources/seed-packs/initial-resource-pack/resources/i18n/sbsql-language-resource-pack"
DRIVER_MANIFEST_REL = "project/drivers/language/sbsql_language_surface_manifest.json"
LIMITATIONS_REL = "KNOWN_LIMITATIONS.md"

EXPECTED_PROFILES = ["en-US", "en-CA", "fr-FR", "fr-CA", "de-DE", "it-IT", "es-ES"]
RELEASE_PROFILES = {"en-US", "en-CA"}
BETA_PROFILES = {"fr-FR", "fr-CA", "de-DE", "it-IT", "es-ES"}
REQUIRED_MESSAGE_CODES = {
    "SECURITY.AUTHORIZATION.DENIED",
    "SBLR.DESCRIPTOR.INVALID",
    "SBLR.ENVELOPE.CHECKSUM_INVALID",
    "SBLR.ENVELOPE.INVALID",
    "SBLR.OPCODE.REFERENCE_META_FORBIDDEN",
    "SBLR.OPCODE.UNKNOWN",
    "SBLR.VERSION.UNSUPPORTED",
}
REQUIRED_LIMITATION_PHRASES = (
    "## SBsql Language Support",
    "`en-US` and `en-CA` are the canonical English profiles",
    "`fr-FR`, `fr-CA`, `de-DE`, `it-IT`, and `es-ES` are fully populated beta profiles",
    "Online translation checks are spot-verification evidence",
    "Client-generated SBLR, UUID descriptors, localized streams, and locally cached command bundles are untrusted",
    "must fall back to canonical English SBsql or fail closed according to policy",
    "stable public diagnostics and SBLR envelope diagnostics",
)

NETWORK_MODULE_RE = re.compile(
    r"^\s*(?:import|from)\s+(urllib|http|socket|ssl|requests|httpx|aiohttp)(?:[\.\s]|$)",
    re.MULTILINE,
)


def fail(message: str) -> None:
    print(f"sbsql_language_beta_release_proof_gate=failed:{message}", file=sys.stderr)
    raise SystemExit(1)


def read_json(path: Path) -> Any:
    try:
        with path.open(encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot_read_json:{path}:{exc}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        fail(f"cannot_hash_file:{path}:{exc}")
    return "sha256:" + digest.hexdigest()


def require(condition: bool, message: str, errors: list[str]) -> None:
    if not condition:
        errors.append(message)


def load_pack(pack_root: Path) -> dict[str, Any]:
    return {
        "manifest": read_json(pack_root / "manifest.sblrp.json"),
        "corpus_rows": [
            json.loads(line)
            for line in (pack_root / "resources/canonical/translation-source-corpus.jsonl").read_text(encoding="utf-8").splitlines()
            if line.strip()
        ],
        "topology": read_json(pack_root / "resources/topology/topology-profiles.json"),
        "predictive": read_json(pack_root / "resources/predictive/predictive-grammar.json"),
        "unicode": read_json(pack_root / "resources/unicode/unicode-policy.json"),
        "resolver": read_json(pack_root / "resources/resolver/resolver-policy.json"),
        "database_messages": read_json(pack_root / "resources/diagnostics/database-message-catalog.json"),
        "online_corpus": read_json(pack_root / "resources/conformance/online-translation-verification-corpus.json"),
    }


def validate_manifest_and_driver(repo_root: Path, pack_root: Path, pack: dict[str, Any], errors: list[str]) -> None:
    manifest = pack["manifest"]
    driver = read_json(repo_root / DRIVER_MANIFEST_REL)
    profiles = manifest.get("profiles", [])
    exact_tags = [row.get("exact_tag") for row in profiles]
    require(exact_tags == EXPECTED_PROFILES, f"profile_order_drift:{exact_tags}", errors)
    require(manifest.get("resource_identity") == "sbsql.common_resource_pack.v1", "resource_identity_drift", errors)
    require(manifest.get("dialect_profile_uuid") == "sbsql.v3", "dialect_profile_uuid_drift", errors)
    require(manifest.get("topology_profile_uuid") == "topology.sbsql.canonical.v1", "topology_profile_uuid_drift", errors)
    authority = manifest.get("authority", {})
    require(authority.get("local_sblr_uuid_streams_are_untrusted") is True, "local_sblr_trust_flag_missing", errors)
    require(authority.get("server_revalidates_sblr_uuid_descriptor_authorization_policy_and_mga") is True,
            "server_revalidation_flag_missing", errors)
    require(authority.get("normalization_before_uuid_resolution") is True,
            "normalization_before_uuid_resolution_flag_missing", errors)
    for row in profiles:
        tag = row.get("exact_tag")
        if tag in RELEASE_PROFILES:
            require(row.get("release_channel") == "release_supported", f"{tag}:release_channel_not_supported", errors)
            require(row.get("support_state") == "release_supported", f"{tag}:support_state_not_supported", errors)
            require(row.get("native_review_state") == "source_authority_reviewed", f"{tag}:source_authority_missing", errors)
        elif tag in BETA_PROFILES:
            require(row.get("release_channel") == "beta", f"{tag}:release_channel_not_beta", errors)
            require(row.get("support_state") == "fully_populated_native_review_required", f"{tag}:support_state_not_beta", errors)
            require(row.get("native_review_state") == "native_technical_review_required_before_release_support",
                    f"{tag}:native_review_state_missing", errors)
        else:
            errors.append(f"unexpected_profile:{tag}")
    driver_meta = driver.get("common_resource_pack_metadata", {})
    require(driver_meta.get("resource_pack_path") == PACK_REL, "driver_resource_pack_path_drift", errors)
    require(driver_meta.get("supported_exact_profiles") == EXPECTED_PROFILES, "driver_profile_list_drift", errors)
    require(driver_meta.get("resource_pack_common_resource_hash") == manifest.get("common_resource_hash"),
            "driver_common_resource_hash_drift", errors)
    require(driver_meta.get("resource_pack_manifest_sha256") == sha256_file(pack_root / "manifest.sblrp.json"),
            "driver_manifest_hash_drift", errors)
    policy = driver.get("common_resource_contract", {})
    require(policy.get("draft_sblr_is_untrusted_until_server_admission") is True,
            "driver_untrusted_sblr_policy_missing", errors)
    require(policy.get("server_revalidates_client_sblr") is True,
            "driver_server_revalidation_policy_missing", errors)
    require(policy.get("server_owns_uuid_descriptor_security_authority") is True,
            "driver_uuid_security_authority_policy_missing", errors)
    require(policy.get("server_owns_mga_transaction_finality") is True,
            "driver_mga_finality_policy_missing", errors)
    require(policy.get("standard_english_fallback_preserves_preferred_language") is True,
            "driver_canonical_english_fallback_policy_missing", errors)
    require(policy.get("predictive_text_must_not_infer_hidden_objects") is True,
            "driver_predictive_privacy_policy_missing", errors)


def validate_language_profiles(pack_root: Path, corpus_rows: list[dict[str, Any]], errors: list[str]) -> None:
    corpus_ids = {row.get("record_id") for row in corpus_rows}
    require(len(corpus_ids) == len(corpus_rows), "translation_source_corpus_duplicate_record_ids", errors)
    for tag in EXPECTED_PROFILES:
        profile = read_json(pack_root / f"resources/languages/{tag}/language-profile.json")
        require(profile.get("exact_tag") == tag, f"{tag}:profile_exact_tag_drift", errors)
        require(profile.get("translation_count") == len(corpus_rows), f"{tag}:translation_count_drift", errors)
        require(profile.get("fallback_translation_count") == 0, f"{tag}:fallback_translation_count_nonzero", errors)
        status_counts = profile.get("translation_status_counts", {})
        require(status_counts.get("english_fallback_machine_seed", 0) == 0,
                f"{tag}:english_fallback_status_present", errors)
        translations = profile.get("translations", [])
        ids = {row.get("record_id") for row in translations}
        require(ids == corpus_ids, f"{tag}:translation_corpus_coverage_drift", errors)
        for row in translations:
            require(bool(row.get("localized_text")), f"{tag}:empty_translation:{row.get('record_id')}", errors)
            require(row.get("translation_status") != "english_fallback_machine_seed",
                    f"{tag}:fallback_row:{row.get('record_id')}", errors)
        if tag in BETA_PROFILES:
            require(profile.get("release_channel") == "beta", f"{tag}:language_profile_not_beta", errors)
            require(profile.get("native_review_state") == "native_technical_review_required_before_release_support",
                    f"{tag}:native_review_state_drift", errors)


def validate_topology_policy(pack: dict[str, Any], errors: list[str]) -> None:
    topology = pack["topology"]
    require(topology.get("normalization_stage") == "stream_analysis_before_uuid_resolution",
            "topology_normalization_stage_drift", errors)
    framework = topology.get("framework", {})
    require(framework.get("name") == "Universal Dependencies", "topology_framework_drift", errors)
    require(framework.get("raw_treebank_material_included") is False, "topology_raw_treebank_included", errors)
    require([row.get("exact_tag") for row in topology.get("profiles", [])] == EXPECTED_PROFILES,
            "topology_profile_list_drift", errors)
    for profile in topology.get("profiles", []):
        source = profile.get("topology_source", {})
        require(source.get("raw_treebank_material_included") is False,
                f"{profile.get('exact_tag')}:raw_treebank_included", errors)
        require(source.get("derived_metrics_only") is True,
                f"{profile.get('exact_tag')}:topology_not_derived_metrics_only", errors)
        require(str(source.get("source_url", "")).startswith("https://github.com/UniversalDependencies/"),
                f"{profile.get('exact_tag')}:ud_source_url_missing", errors)
        require(profile.get("uuid_resolution_stage") == "after_canonical_stream_normalization",
                f"{profile.get('exact_tag')}:uuid_resolution_stage_drift", errors)


def validate_message_and_online_corpora(pack: dict[str, Any], errors: list[str]) -> None:
    messages = pack["database_messages"]
    require(messages.get("profiles") == EXPECTED_PROFILES, "database_message_profiles_drift", errors)
    require(messages.get("message_count") == len(messages.get("messages", [])), "database_message_count_drift", errors)
    policy = messages.get("translation_policy", {})
    require(policy.get("no_english_fallback_rows_allowed") is True, "message_no_fallback_policy_missing", errors)
    require(policy.get("sentence_topology_reordering_allowed") is False, "message_sentence_topology_policy_drift", errors)
    message_codes = {row.get("diagnostic_code") for row in messages.get("messages", [])}
    require(REQUIRED_MESSAGE_CODES <= message_codes,
            f"database_message_required_codes_missing:{sorted(REQUIRED_MESSAGE_CODES - message_codes)}", errors)
    for message in messages.get("messages", []):
        translations = message.get("translations", [])
        require([row.get("exact_tag") for row in translations] == EXPECTED_PROFILES,
                f"{message.get('diagnostic_code')}:translation_profile_drift", errors)
        for row in translations:
            require(row.get("localized_template"), f"{message.get('diagnostic_code')}:{row.get('exact_tag')}:empty_template", errors)
            require(row.get("translation_status") != "english_fallback_machine_seed",
                    f"{message.get('diagnostic_code')}:{row.get('exact_tag')}:fallback_status", errors)
            require("topology_transform" not in row,
                    f"{message.get('diagnostic_code')}:{row.get('exact_tag')}:sentence_topology_transform_present", errors)

    online = pack["online_corpus"]
    require(online.get("profiles") == EXPECTED_PROFILES, "online_corpus_profiles_drift", errors)
    require(online.get("case_count") == len(online.get("cases", [])), "online_corpus_case_count_drift", errors)
    require(online.get("case_count", 0) >= 15, "online_corpus_case_count_too_low", errors)
    provider_policy = online.get("provider_policy", {})
    require(provider_policy.get("release_pack_generation_uses_network") is False,
            "online_corpus_release_generation_network_enabled", errors)
    require(provider_policy.get("online_reference_checks_are_optional_external_verification") is True,
            "online_corpus_optional_external_policy_missing", errors)
    pairs = provider_policy.get("supported_language_pairs", {})
    require(set(pairs) == BETA_PROFILES, f"online_corpus_language_pair_drift:{sorted(pairs)}", errors)
    for case in online.get("cases", []):
        require(case.get("network_required") is True, f"{case.get('case_id')}:network_required_flag_missing", errors)
        require(case.get("external_reference_text_not_vendored") is True,
                f"{case.get('case_id')}:external_reference_vendored", errors)
        translations = case.get("translations", [])
        require([row.get("exact_tag") for row in translations] == EXPECTED_PROFILES,
                f"{case.get('case_id')}:translation_profile_drift", errors)
        source_text = case.get("source_text")
        for row in translations:
            tag = row.get("exact_tag")
            text = row.get("localized_template", "")
            require(text, f"{case.get('case_id')}:{tag}:empty_online_case_translation", errors)
            if tag in BETA_PROFILES:
                require(row.get("translation_status") == "localized_phrase_seed",
                        f"{case.get('case_id')}:{tag}:not_phrase_seeded", errors)
                require(text != source_text, f"{case.get('case_id')}:{tag}:beta_translation_same_as_source", errors)


def validate_privacy_resources(pack: dict[str, Any], errors: list[str]) -> None:
    predictive = pack["predictive"]
    require(predictive.get("no_database_object_names") is True, "predictive_database_object_names_allowed", errors)
    require(predictive.get("no_uuid_values") is True, "predictive_uuid_values_allowed", errors)
    require(predictive.get("server_revalidation_required") is True, "predictive_server_revalidation_missing", errors)
    unicode_policy = pack["unicode"]
    require(unicode_policy.get("hidden_object_disclosure_allowed") is False,
            "unicode_hidden_object_disclosure_allowed", errors)
    resolver = pack["resolver"]
    require(resolver.get("authorized_schema_path_resolution_required") is True,
            "resolver_schema_path_auth_filter_missing", errors)
    require(resolver.get("authorized_uuid_to_path_required") is True,
            "resolver_uuid_to_path_auth_filter_missing", errors)
    require(resolver.get("server_authority") == "server_filters_schema_object_path_and_uuid_resolution_by_auth_policy",
            "resolver_server_authority_drift", errors)


def validate_public_limitations(repo_root: Path, errors: list[str]) -> None:
    limitations_path = repo_root / LIMITATIONS_REL
    text = limitations_path.read_text(encoding="utf-8")
    for phrase in REQUIRED_LIMITATION_PHRASES:
        require(phrase in text, f"known_limitations_missing_phrase:{phrase}", errors)


def validate_gate_no_network() -> list[str]:
    text = Path(__file__).read_text(encoding="utf-8")
    return [match.group(1) for match in NETWORK_MODULE_RE.finditer(text)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args()

    repo_root = args.repo_root.resolve()
    pack_root = repo_root / PACK_REL
    if not pack_root.is_dir():
        fail(f"pack_root_missing:{pack_root}")
    network_imports = validate_gate_no_network()
    if network_imports:
        fail("network_imports_present:" + ",".join(network_imports))

    errors: list[str] = []
    pack = load_pack(pack_root)
    validate_manifest_and_driver(repo_root, pack_root, pack, errors)
    validate_language_profiles(pack_root, pack["corpus_rows"], errors)
    validate_topology_policy(pack, errors)
    validate_message_and_online_corpora(pack, errors)
    validate_privacy_resources(pack, errors)
    validate_public_limitations(repo_root, errors)

    if errors:
        print(f"sbsql_language_beta_release_proof_gate=failed errors={len(errors)}", file=sys.stderr)
        for error in errors[:80]:
            print(f"  {error}", file=sys.stderr)
        return 1

    print(
        "sbsql_language_beta_release_proof_gate=passed "
        f"profiles={len(EXPECTED_PROFILES)} beta_profiles={len(BETA_PROFILES)} "
        f"messages={pack['database_messages']['message_count']} "
        f"online_cases={pack['online_corpus']['case_count']} "
        f"common_resource_hash={pack['manifest']['common_resource_hash']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
