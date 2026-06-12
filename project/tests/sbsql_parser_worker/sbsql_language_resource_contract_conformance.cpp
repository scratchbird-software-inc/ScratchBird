// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "rendering/rendering.hpp"
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

bool ContainsText(std::string_view text, std::string_view expected) {
  return text.find(expected) != std::string_view::npos;
}

void RequireNoSensitiveMaterial(std::string_view rendered, std::string_view label) {
  static constexpr std::string_view kSensitiveTerms[] = {
      "SELECT * FROM hidden_table",
      "hidden_table",
      "policy.secret",
      "provider.local_password",
      "/tmp/secret/fr-ca.json",
      "019f1000-0000-7000-8000-00000000feed",
      "sbsql.common_resource_pack.private",
      "profile.private.fr-ca",
      "dialect.private.firebird",
      "topology.private.rtl",
      "draft_sblr_local_secret",
  };
  for (const auto term : kSensitiveTerms) {
    Require(!ContainsText(rendered, term),
            std::string(label) + " disclosed sensitive term: " + std::string(term));
  }
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

sbsql::LanguageResourceBundleManifest ValidLanguageBundle() {
  auto profile = ValidReleaseProfile();
  sbsql::LanguageResourceBundleManifest bundle;
  bundle.bundle_uuid = "019f1000-0000-7000-8000-000000000201";
  bundle.bundle_contract_id = "sbsql.lang.fr-ca@1";
  bundle.exact_tag = profile.exact_tag;
  bundle.dialect_profile_uuid = "sbsql.v3";
  bundle.topology_profile_uuid = "topology.sbsql.canonical_svo.v1";
  bundle.common_resource_hash = profile.common_resource_hash;
  bundle.canonical_element_stream_schema_hash = "canonical.stream.schema.v1";
  bundle.predictive_resource_hash = profile.predictive_grammar_hash;
  bundle.renderer_resource_hash = profile.renderer_registry_hash;
  bundle.diagnostic_resource_hash = profile.diagnostic_pack_hash;
  bundle.language_profile = profile;
  bundle.provenance = profile.provenance;
  return bundle;
}

sbsql::LanguageElementManifest ValidLanguageElementManifest() {
  auto profile = ValidReleaseProfile();
  auto bundle = ValidLanguageBundle();
  sbsql::LanguageElementManifest manifest;
  manifest.manifest_uuid = "019f1000-0000-7000-8000-000000000301";
  manifest.profile_uuid = profile.profile_uuid;
  manifest.exact_tag = profile.exact_tag;
  manifest.dialect_profile_uuid = bundle.dialect_profile_uuid;
  manifest.topology_profile_uuid = bundle.topology_profile_uuid;
  manifest.common_resource_hash = profile.common_resource_hash;
  manifest.compatibility_identity = bundle.compatibility_identity;
  manifest.language_profile = profile;
  manifest.bundle_manifest = bundle;
  manifest.compatibility_ids = {"compat.sbsql.select.v1", "compat.sbsql.from.v1"};
  manifest.diagnostics = {
      {"SBSQL.LANG_ELEMENT.SELECT", "msg.sbsql.select", "info", "message.hash.select"},
      {"SBSQL.LANG_ELEMENT.FROM", "msg.sbsql.from", "info", "message.hash.from"},
  };
  manifest.renderers = {
      {"renderer.fr-ca.select", profile.profile_uuid, "sbsql.builtin.recovery.en",
       sbsql::SblrRenderLossiness::kCanonicalEquivalent, true},
      {"renderer.fr-ca.from", profile.profile_uuid, "sbsql.builtin.recovery.en",
       sbsql::SblrRenderLossiness::kCanonicalEquivalent, true},
  };
  manifest.topology_slots = {
      {"slot.query.projection", "phrase.query.select", "projection", "SBSQL.SELECT",
       "SBSQL.SELECT.SURFACE", 1, 1},
      {"slot.query.source", "phrase.query.from", "source", "SBSQL.FROM",
       "SBSQL.FROM.SURFACE", 1, 1},
  };
  manifest.predictive_states = {
      {"predictive.select", "SBSQL.SELECT.SURFACE", "predictive.hash.select", true, true},
      {"predictive.from", "SBSQL.FROM.SURFACE", "predictive.hash.from", true, true},
  };
  manifest.surfaces = {
      {"SBSQL.SELECT.SURFACE", "SELECT", "keyword", "query",
       "sblr.query.runtime.v3", "slot.query.projection", "predictive.select",
       "renderer.fr-ca.select", "compat.sbsql.select.v1", "SBSQL.LANG_ELEMENT.SELECT",
       "msg.sbsql.select", sbsql::LanguageResourceChannel::kReleaseSupported},
      {"SBSQL.FROM.SURFACE", "FROM", "keyword", "query",
       "sblr.query.runtime.v3", "slot.query.source", "predictive.from",
       "renderer.fr-ca.from", "compat.sbsql.from.v1", "SBSQL.LANG_ELEMENT.FROM",
       "msg.sbsql.from", sbsql::LanguageResourceChannel::kReleaseSupported},
  };
  manifest.keywords = {
      {"kw.select", "SELECT", "SBSQL.SELECT", "SBSQL.SELECT.SURFACE", true, false},
      {"kw.from", "FROM", "SBSQL.FROM", "SBSQL.FROM.SURFACE", true, false},
  };
  manifest.provenance = profile.provenance;
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

void VerifyLanguageElementManifestCompleteness() {
  auto manifest = ValidLanguageElementManifest();
  auto result = sbsql::ValidateLanguageElementManifest(manifest);
  Require(result.accepted, "SML-010 complete language element manifest was rejected");

  manifest = ValidLanguageElementManifest();
  manifest.keywords.clear();
  result = sbsql::ValidateLanguageElementManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_ELEMENT_MANIFEST.KEYWORDS_MISSING"),
          "SML-010 manifest without keyword declarations was accepted");

  manifest = ValidLanguageElementManifest();
  manifest.surfaces.front().topology_slot_id = "missing.slot";
  result = sbsql::ValidateLanguageElementManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_ELEMENT_MANIFEST.SURFACE_TOPOLOGY_SLOT_MISSING"),
          "SML-010 surface without topology slot coverage was accepted");

  manifest = ValidLanguageElementManifest();
  manifest.surfaces.front().predictive_state_id = "missing.predictive";
  result = sbsql::ValidateLanguageElementManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_ELEMENT_MANIFEST.SURFACE_PREDICTIVE_STATE_MISSING"),
          "SML-010 surface without predictive-state coverage was accepted");

  manifest = ValidLanguageElementManifest();
  manifest.renderers.front().server_revalidation_required = false;
  result = sbsql::ValidateLanguageElementManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_ELEMENT_MANIFEST.RENDERER_REVALIDATION_REQUIRED"),
          "SML-010 renderer without server revalidation was accepted");

  manifest = ValidLanguageElementManifest();
  manifest.compatibility_ids.push_back("compat.sbsql.select.v1");
  result = sbsql::ValidateLanguageElementManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_ELEMENT_MANIFEST.DUPLICATE_COMPATIBILITY_ID"),
          "SML-010 duplicate compatibility ID was accepted");

  manifest = ValidLanguageElementManifest();
  manifest.surfaces.front().release_channel = sbsql::LanguageResourceChannel::kRemoved;
  result = sbsql::ValidateLanguageElementManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_ELEMENT_MANIFEST.SURFACE_CHANNEL_REFUSED"),
          "SML-010 manifest published a removed surface");
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
  manifest.expired = true;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.EXPIRED"),
          "SML-093 expired profile did not fail closed");

  manifest = ValidReleaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kReleaseSupported;
  manifest.support_state = sbsql::LanguageResourceSupportState::kMachineBootstrap;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_RESOURCE.SUPPORT_STATE_MISMATCH"),
          "SML-088 machine-bootstrap profile was release-supported");
}

void VerifyReleaseChannelLifecycle() {
  auto manifest = ValidReleaseProfile();
  auto lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(lifecycle.load_allowed && lifecycle.use_allowed &&
              lifecycle.support_claim_allowed &&
              !lifecycle.emits_deprecation_warning,
          "SML-092 release-supported lifecycle classification drifted");

  manifest.channel = sbsql::LanguageResourceChannel::kExperimental;
  manifest.support_state = sbsql::LanguageResourceSupportState::kMachineBootstrap;
  auto result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.EXPERIMENTAL_UNSUPPORTED") &&
              lifecycle.load_allowed && !lifecycle.support_claim_allowed,
          "SML-092 experimental resource did not admit without support commitment");

  manifest.support_state = sbsql::LanguageResourceSupportState::kNativeReviewed;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.EXPERIMENTAL_SUPPORT_STATE_MISMATCH"),
          "SML-092 experimental resource claimed reviewed support state");

  manifest = ValidReleaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kPreview;
  manifest.support_state = sbsql::LanguageResourceSupportState::kNativeReviewed;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.PREVIEW_LIMITED_SUPPORT") &&
              lifecycle.load_allowed && !lifecycle.support_claim_allowed,
          "SML-092 preview resource did not admit as limited support");

  manifest.support_state = sbsql::LanguageResourceSupportState::kMachineBootstrap;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.PREVIEW_REVIEW_REQUIRED"),
          "SML-092 preview resource did not require native review");

  manifest = ValidReleaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kBeta;
  manifest.support_state = sbsql::LanguageResourceSupportState::kNativeReviewed;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT") &&
              lifecycle.load_allowed && !lifecycle.support_claim_allowed,
          "SML-092 beta resource did not admit as limited support");

  manifest = ValidReleaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kDeprecated;
  manifest.deprecation_notice_id = "deprecation.fr-ca.v1";
  manifest.deprecation_replacement_profile_uuid = "019f1000-0000-7000-8000-000000000102";
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.DEPRECATED") &&
              lifecycle.load_allowed && lifecycle.support_claim_allowed &&
              lifecycle.emits_deprecation_warning,
          "SML-092 deprecated resource did not admit with deprecation warning");

  manifest.deprecation_notice_id.clear();
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.DEPRECATION_NOTICE_MISSING"),
          "SML-092 deprecated resource did not require notice evidence");

  manifest = ValidReleaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kRevoked;
  manifest.revocation_notice_id = "revocation.fr-ca.v1";
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.REVOKED") &&
              !lifecycle.load_allowed && !lifecycle.use_allowed &&
              !lifecycle.support_claim_allowed,
          "SML-092 revoked resource was not refused");

  manifest = ValidReleaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kRemoved;
  manifest.removal_notice_id = "removal.fr-ca.v1";
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.REMOVED") &&
              !lifecycle.load_allowed && !lifecycle.use_allowed,
          "SML-092 removed resource was not refused");

  auto bundle = ValidLanguageBundle();
  bundle.language_profile.channel = sbsql::LanguageResourceChannel::kDeprecated;
  bundle.language_profile.deprecation_notice_id = "deprecation.fr-ca.v1";
  auto admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kLoad, bundle});
  Require(admission.accepted &&
              admission.HasIssue("SBSQL.LANG_RESOURCE.DEPRECATED"),
          "SML-092 deprecated bundle load did not admit with warning");

  bundle = ValidLanguageBundle();
  bundle.language_profile.channel = sbsql::LanguageResourceChannel::kRevoked;
  bundle.language_profile.revocation_notice_id = "revocation.fr-ca.v1";
  admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kLoad, bundle});
  Require(!admission.accepted &&
              admission.HasIssue("SBSQL.LANG_RESOURCE.REVOKED"),
          "SML-092 revoked bundle load was not refused");
}

void VerifyLanguageBundleSchemaAdmission() {
  auto bundle = ValidLanguageBundle();
  auto result = sbsql::ValidateLanguageResourceBundleManifest(bundle);
  Require(result.accepted, "SML-003 valid language resource bundle schema was rejected");

  bundle.bundle_uuid.clear();
  result = sbsql::ValidateLanguageResourceBundleManifest(bundle);
  Require(!result.accepted && result.HasIssue("SBSQL.LANG_BUNDLE.BUNDLE_UUID_MISSING"),
          "SML-003 bundle UUID was not required");

  bundle = ValidLanguageBundle();
  bundle.renderer_resource_hash = "renderer.hash.mismatch";
  result = sbsql::ValidateLanguageResourceBundleManifest(bundle);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_BUNDLE.RENDERER_HASH_PROFILE_MISMATCH"),
          "SML-003 bundle renderer hash mismatch was not refused");

  bundle = ValidLanguageBundle();
  auto admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kLoad, bundle});
  Require(admission.accepted, "SML-009 signed compatible admitted bundle did not load-admit");

  bundle.signed_bundle = false;
  admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kLoad, bundle});
  Require(!admission.accepted && admission.HasIssue("SBSQL.LANG_BUNDLE.UNSIGNED"),
          "SML-009 unsigned bundle was not refused for load");

  bundle = ValidLanguageBundle();
  bundle.compatible_with_parser = false;
  admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kLoad, bundle});
  Require(!admission.accepted && admission.HasIssue("SBSQL.LANG_BUNDLE.INCOMPATIBLE"),
          "SML-009 incompatible bundle was not refused for load");

  bundle = ValidLanguageBundle();
  bundle.admitted_by_security_policy = false;
  admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kLoad, bundle});
  Require(!admission.accepted &&
              admission.HasIssue("SBSQL.LANG_BUNDLE.SECURITY_ADMISSION_REQUIRED"),
          "SML-009 unadmitted bundle was not refused for load");

  bundle = ValidLanguageBundle();
  bundle.active_profile = true;
  admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kUnload, bundle});
  Require(!admission.accepted &&
              admission.HasIssue("SBSQL.LANG_BUNDLE.ACTIVE_PROFILE_IN_USE"),
          "SML-009 active profile was not refused for unload");

  bundle = ValidLanguageBundle();
  bundle.required_profile = true;
  admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kUnload, bundle});
  Require(!admission.accepted &&
              admission.HasIssue("SBSQL.LANG_BUNDLE.REQUIRED_PROFILE"),
          "SML-009 required profile was not refused for unload");
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
  sbsql::CanonicalElement select;
  select.kind = sbsql::CanonicalElementKind::kCommand;
  select.canonical_text = "SELECT";
  select.canonical_id = "SBSQL.SELECT";
  select.surface_id = "SBSQL.SELECT.SURFACE";
  select.slot_id = "slot.projection";
  select.alias_id = "alias.selectionner";
  select.topology_role = "projection";
  select.localized_text_hash = "localized.hash.select";
  select.source_span = sbsql::CanonicalElementSourceSpan{12, 12};
  stream.elements.push_back(select);

  sbsql::CanonicalElement from;
  from.kind = sbsql::CanonicalElementKind::kClause;
  from.canonical_text = "FROM";
  from.canonical_id = "SBSQL.FROM";
  from.surface_id = "SBSQL.FROM.SURFACE";
  from.slot_id = "slot.source";
  from.alias_id = "alias.depuis";
  from.topology_role = "source";
  from.localized_text_hash = "localized.hash.from";
  from.source_span = sbsql::CanonicalElementSourceSpan{0, 6};
  stream.elements.push_back(from);

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

  sbsql::SblrRenderRequest preferred;
  preferred.sblr_uuid_authority_valid = true;
  preferred.preferred_renderer_available = true;
  preferred.canonical_english_renderer_available = true;
  preferred.preferred_language_profile = "fr-CA";
  auto selection = sbsql::ClassifySblrRenderSelection(preferred);
  Require(selection.decision == sbsql::SblrRenderDecision::kPreferredLanguage,
          "SML-084 preferred renderer selection changed");
  Require(selection.lossiness == sbsql::SblrRenderLossiness::kCanonicalEquivalent,
          "SML-086 preferred renderer did not classify canonical-equivalent output");
  Require(selection.selected_language_profile == "fr-CA",
          "SML-084 preferred renderer did not retain selected language profile");
  Require(selection.diagnostic_code == "SBSQL.LANG_RESOURCE.RENDERER_LOSSINESS_CLASSIFIED",
          "SML-086 preferred renderer classification diagnostic missing");

  preferred.preferred_renderer_partial = true;
  selection = sbsql::ClassifySblrRenderSelection(preferred);
  Require(selection.lossiness == sbsql::SblrRenderLossiness::kPreferredLanguagePartial,
          "SML-086 preferred partial renderer did not classify partial lossiness");

  preferred.preferred_renderer_partial = false;
  preferred.preferred_language_is_canonical_english = true;
  preferred.preferred_language_profile = "sbsql.builtin.recovery.en";
  selection = sbsql::ClassifySblrRenderSelection(preferred);
  Require(selection.lossiness == sbsql::SblrRenderLossiness::kLosslessCanonical,
          "SML-086 canonical English renderer did not classify lossless canonical output");

  sbsql::SblrRenderRequest fallback;
  fallback.sblr_uuid_authority_valid = true;
  fallback.canonical_english_renderer_available = true;
  fallback.preferred_language_profile = "fr-CA";
  fallback.canonical_english_profile = "sbsql.builtin.recovery.en";
  selection = sbsql::ClassifySblrRenderSelection(fallback);
  Require(selection.decision == sbsql::SblrRenderDecision::kCanonicalEnglishFallback,
          "SML-084 canonical English fallback renderer was not selected");
  Require(selection.lossiness == sbsql::SblrRenderLossiness::kCanonicalEnglishFallback,
          "SML-086 canonical English fallback lossiness was not classified");
  Require(selection.used_canonical_english_fallback,
          "SML-084 fallback selection did not record canonical English fallback");
  Require(selection.diagnostic_code == "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
          "SML-084 canonical English fallback diagnostic missing");
  Require(selection.server_revalidation_required,
          "SML-084 renderer selection did not preserve server revalidation authority");

  sbsql::SblrRenderRequest not_renderable;
  not_renderable.sblr_uuid_authority_valid = true;
  selection = sbsql::ClassifySblrRenderSelection(not_renderable);
  Require(selection.decision == sbsql::SblrRenderDecision::kRefuseRendererUnavailable,
          "SML-086 missing renderers did not refuse");
  Require(selection.lossiness == sbsql::SblrRenderLossiness::kNotRenderable,
          "SML-086 missing renderers did not classify not-renderable lossiness");
  Require(selection.diagnostic_code == "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE",
          "SML-086 not-renderable diagnostic missing");

  sbsql::SblrEnvelope envelope;
  envelope.operation_family = "sblr.query.multimodel_or_ddl.v3";
  envelope.statement_hash = 42;
  envelope.payload = "operation_id=ddl.create_table";
  const auto rendered_fallback =
      sbsql::RenderSblrEnvelopeWithProfileSelection(envelope, fallback);
  Require(rendered_fallback.ok, "SML-084 fallback render result was not accepted");
  Require(rendered_fallback.messages.diagnostics.size() == 1,
          "SML-084 fallback render result did not retain diagnostic evidence");
  Require(rendered_fallback.messages.diagnostics.front().code ==
              "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
          "SML-084 fallback render result did not emit fallback diagnostic");
  Require(rendered_fallback.text.find("SBLR sblr.query.multimodel_or_ddl.v3 42") !=
              std::string::npos,
          "SML-084 fallback render result did not preserve SBLR rendering output");

  const auto refused_render =
      sbsql::RenderSblrEnvelopeWithProfileSelection(envelope, not_renderable);
  Require(!refused_render.ok && refused_render.messages.has_errors(),
          "SML-086 not-renderable result did not fail closed");
  Require(refused_render.selection.lossiness == sbsql::SblrRenderLossiness::kNotRenderable,
          "SML-086 refused render result did not retain not-renderable classification");
  Require(refused_render.text.find("SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE") !=
              std::string::npos,
          "SML-086 refused render result did not render public diagnostic");
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

  protocol.hidden_object_no_disclosure = true;
  protocol.renderer_lossiness_classes.pop_back();
  result = sbsql::ValidateEditorToolProtocol(protocol);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.EDITOR_PROTOCOL.RENDERER_LOSSINESS_CLASSES_MISSING"),
          "SML-086 missing renderer lossiness class metadata was not refused");

  protocol.renderer_lossiness_classes.push_back("not_renderable");
  protocol.fallback_diagnostic_codes.clear();
  result = sbsql::ValidateEditorToolProtocol(protocol);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.EDITOR_PROTOCOL.FALLBACK_DIAGNOSTICS_MISSING"),
          "SML-084 missing fallback diagnostics metadata was not refused");

  protocol.fallback_diagnostic_codes = {
      "SBSQL.LANG_RESOURCE.FALLBACK_TO_CANONICAL_ENGLISH",
      "SBSQL.LANG_RESOURCE.FAIL_CLOSED_ON_PROFILE_MISMATCH"};
  protocol.server_revalidation_authority = false;
  result = sbsql::ValidateEditorToolProtocol(protocol);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.EDITOR_PROTOCOL.AUTHORITY_METADATA_MISSING"),
          "SML-084 missing server revalidation metadata was not refused");
}

struct FailureDiagnosticCase {
  sbsql::LanguageResourceFailureKind kind;
  std::string_view code;
  std::string_view name;
};

sbsql::LanguageResourceFailureDiagnosticInput SensitiveFailureInput(
    sbsql::LanguageResourceFailureKind kind) {
  sbsql::LanguageResourceFailureDiagnosticInput input;
  input.failure_kind = kind;
  input.telemetry_export = true;
  input.support_bundle_export = true;
  input.server_revalidation_required = true;
  input.resource_identity = "sbsql.common_resource_pack.private";
  input.language_profile_uuid = "019f1000-0000-7000-8000-00000000feed";
  input.exact_tag = "fr-CA-x-private";
  input.dialect_profile_uuid = "dialect.private.firebird";
  input.topology_profile_uuid = "topology.private.rtl";
  input.query_text = "SELECT * FROM hidden_table WHERE policy.secret = provider.local_password";
  input.hidden_identifier = "hidden_table";
  input.local_path = "/tmp/secret/fr-ca.json";
  input.local_sblr_payload = "draft_sblr_local_secret operation";
  return input;
}

void VerifyStableFailureDiagnosticsNoDisclosure() {
  const std::vector<FailureDiagnosticCase> cases = {
      {sbsql::LanguageResourceFailureKind::kMissingResource,
       "SBSQL.LANG_RESOURCE.MISSING",
       "missing_resource"},
      {sbsql::LanguageResourceFailureKind::kUnsignedResource,
       "SBSQL.LANG_RESOURCE.UNSIGNED",
       "unsigned_resource"},
      {sbsql::LanguageResourceFailureKind::kRevokedResource,
       "SBSQL.LANG_RESOURCE.REVOKED",
       "revoked_resource"},
      {sbsql::LanguageResourceFailureKind::kExpiredResource,
       "SBSQL.LANG_RESOURCE.EXPIRED",
       "expired_resource"},
      {sbsql::LanguageResourceFailureKind::kIncompatibleResource,
       "SBSQL.LANG_RESOURCE.INCOMPATIBLE",
       "incompatible_resource"},
      {sbsql::LanguageResourceFailureKind::kUnsupportedChannel,
       "SBSQL.LANG_RESOURCE.UNSUPPORTED_CHANNEL",
       "unsupported_channel"},
      {sbsql::LanguageResourceFailureKind::kAmbiguousFallback,
       "SBSQL.LANG_RESOURCE.AMBIGUOUS_FALLBACK",
       "ambiguous_fallback"},
      {sbsql::LanguageResourceFailureKind::kUnsupportedRenderer,
       "SBSQL.LANG_RESOURCE.RENDERER_NOT_RENDERABLE",
       "unsupported_renderer"},
      {sbsql::LanguageResourceFailureKind::kTopologyDialectUnicodeUnsupported,
       "SBSQL.LANG_RESOURCE.TOPOLOGY_DIALECT_UNICODE_UNSUPPORTED",
       "topology_dialect_unicode_unsupported"},
      {sbsql::LanguageResourceFailureKind::kPredictiveResourceRefused,
       "SBSQL.LANG_RESOURCE.PREDICTIVE_RESOURCE_REFUSED",
       "predictive_resource_refused"},
      {sbsql::LanguageResourceFailureKind::kLocalDraftSblrRefused,
       "SBSQL.LANG_RESOURCE.LOCAL_DRAFT_SBLR_REFUSED",
       "local_draft_sblr_refused"},
  };
  const std::vector<std::string_view> expected_field_order = {
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
  };

  for (const auto& diagnostic_case : cases) {
    Require(sbsql::LanguageResourceFailureDiagnosticCode(diagnostic_case.kind) ==
                diagnostic_case.code,
            "SML-093 language resource diagnostic code mapping drifted");
    Require(sbsql::LanguageResourceFailureKindName(diagnostic_case.kind) ==
                diagnostic_case.name,
            "SML-093 language resource failure name mapping drifted");

    const auto input = SensitiveFailureInput(diagnostic_case.kind);
    const auto diagnostic = sbsql::MakeLanguageResourceFailureDiagnostic(input);
    Require(diagnostic.code == diagnostic_case.code,
            "SML-093 failure diagnostic emitted unstable code");
    Require(diagnostic.severity == "ERROR",
            "SML-093 failure diagnostic did not fail closed as ERROR");
    Require(diagnostic.component == "sbp_sbsql.language_resource",
            "SML-093 failure diagnostic component drifted");
    Require(diagnostic.fields.size() == expected_field_order.size(),
            "SML-091 redacted diagnostic public field count drifted");
    for (std::size_t i = 0; i < expected_field_order.size(); ++i) {
      Require(diagnostic.fields[i].name == expected_field_order[i],
              "SML-091 redacted diagnostic public field order drifted");
      Require(sbsql::IsPublicDiagnosticFieldAllowed(diagnostic.fields[i].name,
                                                    diagnostic.fields[i].value),
              "SML-091 diagnostic builder emitted a non-public field");
    }

    const auto messages = sbsql::MakeLanguageResourceFailureMessageVector(input);
    Require(messages.has_errors(), "SML-093 language resource message vector did not fail closed");
    const auto rendered = sbsql::RenderMessageVectorSet(messages);
    const auto json = sbsql::MessageVectorToJson(messages);
    Require(ContainsText(rendered, diagnostic_case.code),
            "SML-093 rendered diagnostic omitted stable failure code");
    Require(ContainsText(rendered, "diagnostic_contract=sbsql.language_resource.failure.v1"),
            "SML-093 rendered diagnostic omitted stable contract field");
    Require(ContainsText(rendered, "private_input_state=redacted"),
            "SML-091 rendered diagnostic did not mark private input redacted");
    Require(ContainsText(rendered, "telemetry_redaction=applied"),
            "SML-091 rendered diagnostic did not mark telemetry redaction");
    Require(ContainsText(rendered, "support_bundle_redaction=applied"),
            "SML-091 rendered diagnostic did not mark support bundle redaction");
    Require(ContainsText(json, std::string("\"code\":\"") +
                                   std::string(diagnostic_case.code) + "\""),
            "SML-093 JSON diagnostic omitted stable failure code");
    Require(ContainsText(json, "\"telemetry_redaction\":\"applied\""),
            "SML-091 JSON diagnostic did not mark telemetry redaction");
    Require(ContainsText(json, "\"support_bundle_redaction\":\"applied\""),
            "SML-091 JSON diagnostic did not mark support bundle redaction");
    RequireNoSensitiveMaterial(rendered, "SML-091 rendered language-resource diagnostic");
    RequireNoSensitiveMaterial(json, "SML-091 JSON language-resource diagnostic");
  }

  auto no_export = SensitiveFailureInput(sbsql::LanguageResourceFailureKind::kLocalDraftSblrRefused);
  no_export.telemetry_export = false;
  no_export.support_bundle_export = false;
  const auto rendered = sbsql::RenderMessageVectorSet(
      sbsql::MakeLanguageResourceFailureMessageVector(no_export));
  Require(ContainsText(rendered, "telemetry_redaction=not_requested"),
          "SML-091 telemetry redaction state was not deterministic when export disabled");
  Require(ContainsText(rendered, "support_bundle_redaction=not_requested"),
          "SML-091 support-bundle redaction state was not deterministic when export disabled");
  RequireNoSensitiveMaterial(rendered, "SML-091 non-export language-resource diagnostic");
}

} // namespace

int main() {
  VerifyBuiltInRecoveryProfile();
  VerifyPreferredLanguageResourceDeclarations();
  VerifyLanguageElementManifestCompleteness();
  VerifyReleaseProfileValidation();
  VerifyReleaseChannelLifecycle();
  VerifyLanguageBundleSchemaAdmission();
  VerifyCanonicalElementStreamContract();
  VerifyParseFallbackAndRenderingContract();
  VerifyMalformedPackRefusals();
  VerifyLocaleLiteralPolicy();
  VerifyConfusablePolicy();
  VerifyRestoreCompatibility();
  VerifyEditorToolProtocol();
  VerifyStableFailureDiagnosticsNoDisclosure();
  std::cout << "sbsql_language_resource_contract_conformance=passed\n";
  return EXIT_SUCCESS;
}
