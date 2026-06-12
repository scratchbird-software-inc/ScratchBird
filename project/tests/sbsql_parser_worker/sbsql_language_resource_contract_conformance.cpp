// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resources/language_resource_contract.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool Contains(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
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
  Require(profile.exact_tag == "en", "SML-084 recovery profile must be standard English");
  Require(profile.fallback_parent_uuid.empty(), "SML-084 standard English fallback must not require another fallback");
  Require(!profile.common_resource_hash.empty() &&
              !profile.canonical_surface_registry_hash.empty() &&
              !profile.sblr_registry_hash.empty(),
          "SML-084 standard English fallback must declare canonical resource authority");
  Require(!profile.renderer_registry_hash.empty() &&
              !profile.diagnostic_pack_hash.empty(),
          "SML-086 standard English fallback must declare renderer and diagnostics resources");
  Require(Contains(profile.canonical_ids, "SBSQL.CANONICAL.EN"),
          "SML-084 standard English fallback canonical ID missing");
  Require(profile.channel == sbsql::LanguageResourceChannel::kReleaseSupported,
          "SML-094 recovery profile must be release supported");
  const auto result = sbsql::ValidateLanguageResourceManifest(profile);
  Require(result.accepted, "SML-094 built-in recovery profile did not validate");
}

void VerifyPreferredLanguageResourceDeclarations() {
  const auto& fallback = sbsql::BuiltInCanonicalEnglishRecoveryProfile();
  auto manifest = ValidReleaseProfile();
  manifest.fallback_parent_uuid = fallback.profile_uuid;

  Require(manifest.exact_tag == "fr-CA", "SML-086 preferred language exact tag drifted");
  Require(!manifest.renderer_registry_hash.empty() &&
              !manifest.diagnostic_pack_hash.empty() &&
              !manifest.predictive_grammar_hash.empty(),
          "SML-086 preferred language renderer/resource declarations missing");
  Require(Contains(manifest.renderer_edges, "renderer.fr-ca"),
          "SML-086 preferred language renderer edge missing");
  Require(Contains(manifest.canonical_ids, "SBSQL.SELECT") &&
              Contains(manifest.canonical_ids, "SBLR.QUERY.V1"),
          "SML-086 preferred language canonical resource declarations missing");

  const auto result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(result.accepted, "SML-084/SML-086 preferred language profile with English fallback was rejected");
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

void VerifyCanonicalElementStreamContract() {
  sbsql::CanonicalElementStream stream;
  stream.resource_identity = "sbsql.common_resource_pack.v1";
  stream.language_profile_uuid = "019f1000-0000-7000-8000-000000000101";
  stream.exact_tag = "fr-CA";
  stream.dialect_profile_uuid = "sbsql.v3";
  stream.topology_profile_uuid = "topology.svo.v1";
  stream.common_resource_hash = "common.hash.v1";
  stream.source_hash = "source.hash.v1";
  stream.elements.push_back(sbsql::CanonicalElement{
      sbsql::CanonicalElementKind::kCommand,
      "SBSQL.SELECT",
      "SBSQL.SELECT.SURFACE",
      "slot.projection",
      "alias.selectionner",
      "projection",
      "localized.hash.select",
      sbsql::CanonicalElementSourceSpan{12, 12}});
  stream.elements.push_back(sbsql::CanonicalElement{
      sbsql::CanonicalElementKind::kClause,
      "SBSQL.FROM",
      "SBSQL.FROM.SURFACE",
      "slot.source",
      "alias.depuis",
      "source",
      "localized.hash.from",
      sbsql::CanonicalElementSourceSpan{0, 6}});

  auto result = sbsql::ValidateCanonicalElementStream(stream);
  Require(result.accepted, "SML-005/SML-082 valid canonical element stream was rejected");

  stream.normalized_before_uuid_resolution = false;
  result = sbsql::ValidateCanonicalElementStream(stream);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.CANONICAL_STREAM.POST_UUID_NORMALIZATION"),
          "SML-082 stream normalized after UUID resolution was not refused");

  stream.normalized_before_uuid_resolution = true;
  stream.server_revalidation_required = false;
  result = sbsql::ValidateCanonicalElementStream(stream);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.CANONICAL_STREAM.SERVER_REVALIDATION_REQUIRED"),
          "SML-083 client stream without server revalidation was not refused");

  stream.server_revalidation_required = true;
  stream.elements.front().localized_text_hash.clear();
  result = sbsql::ValidateCanonicalElementStream(stream);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.CANONICAL_STREAM.ELEMENT_SOURCE_HASH_MISSING"),
          "SML-089 localized element source evidence was not required");
}

void VerifyParseFallbackAndRenderingContract() {
  auto order = sbsql::DefaultParseProfileOrder();
  auto result = sbsql::ValidateParseProfileOrder(order);
  Require(result.accepted, "SML-084 default parse profile order was rejected");
  Require(sbsql::ParseProfileStepName(order[2]) ==
              "canonical_english_fallback_when_preferred_fails",
          "SML-084 parse profile order does not include standard English fallback");

  order[1] = sbsql::ParseProfileStep::kCanonicalEnglishFallback;
  result = sbsql::ValidateParseProfileOrder(order);
  Require(!result.accepted && result.HasIssue("SBSQL.PARSE_PROFILE.ORDER_UNSUPPORTED"),
          "SML-084 reordered fallback profile was not refused");

  Require(sbsql::SelectParseProfile(sbsql::ParseProfileDecisionInput{true, false, false}) ==
              sbsql::ParseProfileDecision::kUseExplicitSyntaxProfile,
          "SML-086 explicit syntax profile was not first");
  Require(sbsql::SelectParseProfile(sbsql::ParseProfileDecisionInput{false, true, true}) ==
              sbsql::ParseProfileDecision::kUsePreferredLanguageAndDialect,
          "SML-084 preferred language parse was not selected before fallback");
  Require(sbsql::SelectParseProfile(sbsql::ParseProfileDecisionInput{false, false, true}) ==
              sbsql::ParseProfileDecision::kUseCanonicalEnglishFallback,
          "SML-084 standard English fallback was not selected");
  Require(sbsql::SelectParseProfile(sbsql::ParseProfileDecisionInput{false, false, false}) ==
              sbsql::ParseProfileDecision::kFailClosed,
          "SML-093 parse failure did not fail closed");

  Require(sbsql::ClassifySblrRenderRequest(
              sbsql::SblrRenderRequest{true, true, true, false, false, false}) ==
              sbsql::SblrRenderDecision::kPreferredLanguage,
          "SML-084 preferred-language renderer was not selected");
  Require(sbsql::ClassifySblrRenderRequest(
              sbsql::SblrRenderRequest{true, false, true, false, false, false}) ==
              sbsql::SblrRenderDecision::kCanonicalEnglishFallback,
          "SML-084 standard English renderer fallback was not selected");
  Require(sbsql::ClassifySblrRenderRequest(
              sbsql::SblrRenderRequest{false, true, true, false, false, false}) ==
              sbsql::SblrRenderDecision::kRefuseMissingCanonicalAuthority,
          "SML-084 rendering without SBLR UUID authority was not refused");
  Require(sbsql::ClassifySblrRenderRequest(
              sbsql::SblrRenderRequest{true, true, true, false, false, true}) ==
              sbsql::SblrRenderDecision::kRefuseSourceReconstruction,
          "SML-095 rendering by source reconstruction was not refused");
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
              sbsql::LanguageResourceRestoreRequest{true, false, true, false, false}) ==
              State::kCanonicalAuthorityValidRendererFallback,
          "SML-084 missing preferred resource did not fall back to standard English authority");
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
  VerifyPreferredLanguageResourceDeclarations();
  VerifyReleaseProfileValidation();
  VerifyCanonicalElementStreamContract();
  VerifyParseFallbackAndRenderingContract();
  VerifyMalformedPackRefusals();
  VerifyLocaleLiteralPolicy();
  VerifyConfusablePolicy();
  VerifyRestoreCompatibility();
  VerifyEditorToolProtocol();
  std::cout << "sbsql_language_resource_contract_conformance=passed\n";
  return EXIT_SUCCESS;
}
