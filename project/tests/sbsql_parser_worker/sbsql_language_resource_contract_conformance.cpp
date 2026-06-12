// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resources/language_resource_contract.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

sbsql::LanguageResourceManifest ValidReleaseProfile() {
  sbsql::LanguageResourceManifest manifest;
  manifest.profile_uuid = "019f1000-0000-7000-8000-000000000101";
  manifest.exact_tag = "fr-CA";
  manifest.common_resource_hash = "common.hash.v1";
  manifest.canonical_surface_registry_hash = "surface.hash.v1";
  manifest.sblr_registry_hash = "sblr.hash.v1";
  manifest.predictive_grammar_hash = "predictive.hash.v1";
  manifest.renderer_registry_hash = "renderer.hash.v1";
  manifest.diagnostic_pack_hash = "diagnostic.hash.v1";
  manifest.signature_id = "sig.fr-ca";
  manifest.signing_key_id = "signing.key.1";
  manifest.governance_evidence_id = "gov.fr-ca";
  manifest.native_review_evidence_id = "native.review.fr-ca";
  manifest.support_owner_id = "support.language.fr-ca";
  manifest.trace_oracle_id = "trace.fr-ca";
  manifest.channel = sbsql::LanguageResourceChannel::kReleaseSupported;
  manifest.support_state = sbsql::LanguageResourceSupportState::kReleaseSupported;
  manifest.canonical_ids = {"SBSQL.SELECT", "SBSQL.FROM", "SBLR.QUERY.V1"};
  manifest.renderer_edges = {"renderer.fr-ca"};
  manifest.provenance.push_back(sbsql::LanguageDataProvenance{
      "ScratchBird community French Canadian profile", "1",
      "MPL-2.0", "sbsql-langpack-normalize-v1", "sbom.lang.fr-ca",
      "notice.lang.fr-ca", true, false, true});
  return manifest;
}

void VerifyBuiltInRecoveryProfile() {
  const auto& profile = sbsql::BuiltInCanonicalEnglishRecoveryProfile();
  Require(profile.built_in_recovery_profile, "SML-094 recovery profile flag missing");
  Require(!profile.externally_replaceable, "SML-094 recovery profile is externally replaceable");
  Require(profile.exact_tag == "en", "SML-094 recovery profile must be canonical English");
  Require(profile.channel == sbsql::LanguageResourceChannel::kReleaseSupported,
          "SML-094 recovery profile must be release supported");
  const auto result = sbsql::ValidateLanguageResourceManifest(profile);
  Require(result.accepted, "SML-094 built-in recovery profile did not validate");
}

void VerifyReleaseProfileValidation() {
  auto manifest = ValidReleaseProfile();
  auto result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(result.accepted, "SML-088/SML-099 valid profile was rejected");

  manifest.signature_id.clear();
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.UNSIGNED"),
          "SML-093 unsigned profile did not fail closed");

  manifest = ValidReleaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kReleaseSupported;
  manifest.support_state = sbsql::LanguageResourceSupportState::kMachineBootstrap;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.SUPPORT_STATE_MISMATCH"),
          "SML-088 machine-bootstrap profile was release-supported");
}

void VerifyMalformedPackRefusals() {
  auto manifest = ValidReleaseProfile();
  manifest.revoked = true;
  auto result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.REVOKED"),
          "SML-098 revoked resource was not refused");

  manifest = ValidReleaseProfile();
  manifest.fallback_parent_uuid = manifest.profile_uuid;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.CYCLIC_FALLBACK_PARENT"),
          "SML-098 cyclic fallback parent was not refused");

  manifest = ValidReleaseProfile();
  manifest.canonical_ids.push_back("SBSQL.SELECT");
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.DUPLICATE_CANONICAL_ID"),
          "SML-098 duplicate canonical ID was not refused");

  manifest = ValidReleaseProfile();
  manifest.renderer_edges.push_back(manifest.profile_uuid);
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.RENDERER_RECURSION"),
          "SML-098 renderer recursion was not refused");

  manifest = ValidReleaseProfile();
  manifest.limits.max_transition_fanout = 4096;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.PREDICTIVE_LIMIT_EXCEEDED"),
          "SML-087/SML-098 predictive limit was not refused");
}

void VerifyLocaleLiteralPolicy() {
  sbsql::LocaleLiteralPolicy strict;
  Require(sbsql::ClassifyLocaleLiteral("2026-06-12", strict) ==
              sbsql::LocaleLiteralClassification::kCanonical,
          "SML-096 canonical ISO literal was not accepted");
  Require(sbsql::ClassifyLocaleLiteral("1,25", strict) ==
              sbsql::LocaleLiteralClassification::kRequiresExplicitProfile,
          "SML-096 decimal comma did not require explicit profile");

  strict.admits_decimal_comma = true;
  Require(sbsql::ClassifyLocaleLiteral("1,25", strict) ==
              sbsql::LocaleLiteralClassification::kCanonical,
          "SML-096 admitted decimal comma did not classify as canonical");

  const std::string arabic_digits = "\xD9\xA1\xD9\xA2";
  sbsql::LocaleLiteralPolicy no_arabic_digits;
  Require(sbsql::ClassifyLocaleLiteral(arabic_digits, no_arabic_digits) ==
              sbsql::LocaleLiteralClassification::kRequiresExplicitProfile,
          "SML-096 localized digits did not require explicit profile");
}

void VerifyConfusablePolicy() {
  sbsql::ConfusablePolicy strict;
  Require(!sbsql::HasMixedScriptOrConfusableRisk("customer_name", strict),
          "SML-097 ASCII identifier was flagged");
  const std::string cyrillic_o = "cust\xD0\xBEmer";
  const std::string greek_beta = "alpha\xCE\xB2";
  Require(sbsql::HasMixedScriptOrConfusableRisk(cyrillic_o, strict),
          "SML-097 Cyrillic homoglyph was not flagged");
  Require(sbsql::HasMixedScriptOrConfusableRisk(greek_beta, strict),
          "SML-097 mixed Greek/Latin was not flagged");

  strict.allow_mixed_script_identifiers = true;
  strict.allow_transliteration_aliases = true;
  Require(!sbsql::HasMixedScriptOrConfusableRisk(greek_beta, strict),
          "SML-097 explicit mixed-script policy did not admit identifier");
}

void VerifyRestoreCompatibility() {
  using State = sbsql::RestoreLanguageResourceState;
  Require(sbsql::ClassifyRestoreLanguageResourceState(
              sbsql::LanguageResourceRestoreRequest{true, true, true, false, false}) ==
              State::kExactResourceAvailable,
          "SML-100 exact restore resource was not accepted");
  Require(sbsql::ClassifyRestoreLanguageResourceState(
              sbsql::LanguageResourceRestoreRequest{true, false, false, false, false}) ==
              State::kCanonicalAuthorityValidRendererFallback,
          "SML-100 missing renderer did not fall back to canonical authority");
  Require(sbsql::ClassifyRestoreLanguageResourceState(
              sbsql::LanguageResourceRestoreRequest{true, true, true, true, false}) ==
              State::kRefuseRevokedResource,
          "SML-100 revoked restore resource was not refused");
  Require(sbsql::ClassifyRestoreLanguageResourceState(
              sbsql::LanguageResourceRestoreRequest{false, true, true, false, false}) ==
              State::kRefuseMissingCanonicalAuthority,
          "SML-100 missing canonical authority was not refused");
}

void VerifyEditorToolProtocol() {
  sbsql::EditorToolProtocol protocol;
  protocol.resource_identity = "common.hash.v1";
  auto result = sbsql::ValidateEditorToolProtocol(protocol);
  Require(result.accepted, "SML-101 valid editor protocol was rejected");

  protocol.hidden_object_no_disclosure = false;
  result = sbsql::ValidateEditorToolProtocol(protocol);
  Require(!result.accepted && result.HasIssue("SBSQL.EDITOR_PROTOCOL.REQUIRED_FEATURE_MISSING"),
          "SML-101 missing no-disclosure feature was not refused");
}

} // namespace

int main() {
  VerifyBuiltInRecoveryProfile();
  VerifyReleaseProfileValidation();
  VerifyMalformedPackRefusals();
  VerifyLocaleLiteralPolicy();
  VerifyConfusablePolicy();
  VerifyRestoreCompatibility();
  VerifyEditorToolProtocol();
  std::cout << "sbsql_language_resource_contract_conformance=passed\n";
  return EXIT_SUCCESS;
}
