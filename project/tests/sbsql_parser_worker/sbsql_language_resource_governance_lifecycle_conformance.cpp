// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "resources/language_resource_contract.hpp"

#include <cstdlib>
#include <functional>
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

sbsql::LanguageDataProvenance ReleaseProvenance() {
  return sbsql::LanguageDataProvenance{
      "ScratchBird release language pack", "1",
      "MPL-2.0", "sbsql-langpack-release-v1",
      "sbom.langpack.release", "notice.langpack.release",
      true, false, true};
}

sbsql::LanguageResourceManifest ReleaseProfile() {
  sbsql::LanguageResourceManifest manifest;
  manifest.profile_uuid = "019f1880-0000-7000-8000-000000000085";
  manifest.exact_tag = "fr-CA";
  manifest.common_resource_hash = "common.resource.pack.hash.v1";
  manifest.canonical_surface_registry_hash = "surface.registry.hash.v1";
  manifest.sblr_registry_hash = "sblr.registry.hash.v1";
  manifest.predictive_grammar_hash = "predictive.grammar.hash.v1";
  manifest.renderer_registry_hash = "renderer.registry.hash.v1";
  manifest.diagnostic_pack_hash = "diagnostic.pack.hash.v1";
  manifest.signature_id = "signature.release.fr-ca.v1";
  manifest.signing_key_id = "signing.key.release.2026q2";
  manifest.governance_evidence_id = "governance.release.fr-ca.v1";
  manifest.author_id = "author.langpack.fr-ca";
  manifest.reviewer_id = "reviewer.langpack.fr-ca";
  manifest.native_review_evidence_id = "native.review.fr-ca.v1";
  manifest.native_technical_reviewer_id = "native.technical.reviewer.fr-ca";
  manifest.security_reviewer_id = "security.reviewer.langpack";
  manifest.support_owner_id = "support.owner.language";
  manifest.trace_oracle_id = "trace.oracle.fr-ca.v1";
  manifest.release_approval_id = "release.approval.fr-ca.v1";
  manifest.revocation_policy_id = "revocation.policy.fr-ca.v1";
  manifest.contribution_provenance_id = "contribution.provenance.fr-ca.v1";
  manifest.channel = sbsql::LanguageResourceChannel::kReleaseSupported;
  manifest.support_state = sbsql::LanguageResourceSupportState::kReleaseSupported;
  manifest.canonical_ids = {"SBSQL.SELECT", "SBSQL.FROM", "SBLR.QUERY.V1"};
  manifest.renderer_edges = {"renderer.fr-ca"};
  manifest.provenance.push_back(ReleaseProvenance());
  return manifest;
}

sbsql::LanguageResourceBundleManifest ReleasePack() {
  auto profile = ReleaseProfile();
  sbsql::LanguageResourceBundleManifest bundle;
  bundle.bundle_uuid = "019f1880-0000-7000-8000-000000000088";
  bundle.bundle_contract_id = "sbsql.langpack.fr-ca.release@1";
  bundle.exact_tag = profile.exact_tag;
  bundle.dialect_profile_uuid = "sbsql.v3";
  bundle.topology_profile_uuid = "topology.sbsql.canonical_svo.v1";
  bundle.common_resource_hash = profile.common_resource_hash;
  bundle.canonical_element_stream_schema_hash = "canonical.stream.schema.v1";
  bundle.predictive_resource_hash = profile.predictive_grammar_hash;
  bundle.renderer_resource_hash = profile.renderer_registry_hash;
  bundle.diagnostic_resource_hash = profile.diagnostic_pack_hash;
  bundle.compatibility_identity = "sbsql.resource.compat.v1";
  bundle.compatibility_version = 2;
  bundle.min_parser_compatibility_version = 1;
  bundle.max_parser_compatibility_version = 3;
  bundle.parser_compatibility_version = 2;
  bundle.lifecycle_state = "cached";
  bundle.generation_evidence_id = "lifecycle.generation.fr-ca.v1";
  bundle.signing_evidence_id = "lifecycle.signing.fr-ca.v1";
  bundle.publication_evidence_id = "lifecycle.publication.fr-ca.v1";
  bundle.admission_evidence_id = "lifecycle.admission.fr-ca.v1";
  bundle.download_evidence_id = "lifecycle.download.fr-ca.v1";
  bundle.cache_evidence_id = "lifecycle.cache.fr-ca.v1";
  bundle.delta_update_evidence_id = "lifecycle.delta_update.fr-ca.v1";
  bundle.rollback_evidence_id = "lifecycle.rollback.fr-ca.v1";
  bundle.revocation_evidence_id = "lifecycle.revocation.fr-ca.v1";
  bundle.expiry_evidence_id = "lifecycle.expiry.fr-ca.v1";
  bundle.key_rotation_evidence_id = "lifecycle.key_rotation.fr-ca.v1";
  bundle.support_bundle_identity = "support.bundle.langpack.fr-ca";
  bundle.support_bundle_version = "support.bundle.langpack.v1";
  bundle.language_profile = profile;
  bundle.provenance = profile.provenance;
  return bundle;
}

void RequireProfileIssue(
    std::string_view expected_code,
    const std::function<void(sbsql::LanguageResourceManifest*)>& mutate,
    std::string_view message) {
  auto profile = ReleaseProfile();
  mutate(&profile);
  const auto result = sbsql::ValidateLanguageResourceManifest(profile);
  Require(!result.accepted && result.HasIssue(expected_code), message);
}

void RequireBundleIssue(
    std::string_view expected_code,
    const std::function<void(sbsql::LanguageResourceBundleManifest*)>& mutate,
    std::string_view message) {
  auto bundle = ReleasePack();
  mutate(&bundle);
  const auto result = sbsql::ValidateLanguageResourceBundleManifest(bundle);
  Require(!result.accepted && result.HasIssue(expected_code), message);
}

void VerifyReleaseGovernancePolicy() {
  const auto result = sbsql::ValidateLanguageResourceManifest(ReleaseProfile());
  Require(result.accepted, "SML-088 complete release-supported profile was rejected");

  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.AUTHOR_MISSING",
      [](auto* profile) { profile->author_id.clear(); },
      "SML-088 release profile without author identity was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.REVIEWER_MISSING",
      [](auto* profile) { profile->reviewer_id.clear(); },
      "SML-088 release profile without reviewer identity was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.NATIVE_TECHNICAL_REVIEWER_MISSING",
      [](auto* profile) { profile->native_technical_reviewer_id.clear(); },
      "SML-088 release profile without native technical reviewer was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.SECURITY_REVIEWER_MISSING",
      [](auto* profile) { profile->security_reviewer_id.clear(); },
      "SML-088 release profile without security reviewer was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.SUPPORT_OWNER_MISSING",
      [](auto* profile) { profile->support_owner_id.clear(); },
      "SML-088 release profile without support owner was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.UNSIGNED",
      [](auto* profile) { profile->signature_id.clear(); },
      "SML-088 release profile without signing identity was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.SIGNING_KEY_MISSING",
      [](auto* profile) { profile->signing_key_id.clear(); },
      "SML-088 release profile without signing key was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.REVOCATION_POLICY_MISSING",
      [](auto* profile) { profile->revocation_policy_id.clear(); },
      "SML-088 release profile without revocation policy was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.RELEASE_APPROVAL_MISSING",
      [](auto* profile) { profile->release_approval_id.clear(); },
      "SML-088 release profile without release approval was accepted");
  RequireProfileIssue(
      "SBSQL.LANG_RESOURCE.CONTRIBUTION_PROVENANCE_MISSING",
      [](auto* profile) { profile->contribution_provenance_id.clear(); },
      "SML-088 release profile without contribution provenance was accepted");
}

void VerifyCommonPackLifecycleEvidence() {
  const auto result = sbsql::ValidateLanguageResourceBundleManifest(ReleasePack());
  Require(result.accepted, "SML-085 complete common resource pack was rejected");

  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.GENERATION_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->generation_evidence_id.clear(); },
      "SML-085 pack without generation evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.SIGNING_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->signing_evidence_id.clear(); },
      "SML-085 pack without signing evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.PUBLICATION_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->publication_evidence_id.clear(); },
      "SML-085 pack without publication evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.ADMISSION_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->admission_evidence_id.clear(); },
      "SML-085 pack without admission evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.DOWNLOAD_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->download_evidence_id.clear(); },
      "SML-085 pack without download evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.CACHE_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->cache_evidence_id.clear(); },
      "SML-085 pack without cache evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.DELTA_UPDATE_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->delta_update_evidence_id.clear(); },
      "SML-085 pack without delta-update evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.ROLLBACK_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->rollback_evidence_id.clear(); },
      "SML-085 pack without rollback evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.REVOCATION_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->revocation_evidence_id.clear(); },
      "SML-085 pack without revocation evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.EXPIRY_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->expiry_evidence_id.clear(); },
      "SML-085 pack without expiry evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.KEY_ROTATION_EVIDENCE_MISSING",
      [](auto* bundle) { bundle->key_rotation_evidence_id.clear(); },
      "SML-085 pack without key-rotation evidence was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.SUPPORT_BUNDLE_IDENTITY_MISSING",
      [](auto* bundle) { bundle->support_bundle_identity.clear(); },
      "SML-085 pack without support-bundle identity was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.SUPPORT_BUNDLE_VERSION_MISSING",
      [](auto* bundle) { bundle->support_bundle_version.clear(); },
      "SML-085 pack without support-bundle version was accepted");
}

void VerifyCompatibilityVersioning() {
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.COMPATIBILITY_IDENTITY_UNSUPPORTED",
      [](auto* bundle) { bundle->compatibility_identity = "sbsql.resource.compat.v2"; },
      "SML-085 unsupported compatibility identity was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.COMPATIBILITY_VERSION_MISSING",
      [](auto* bundle) { bundle->compatibility_version = 0; },
      "SML-085 missing compatibility version was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.COMPATIBILITY_VERSION_RANGE_INVALID",
      [](auto* bundle) {
        bundle->min_parser_compatibility_version = 4;
        bundle->max_parser_compatibility_version = 3;
      },
      "SML-085 invalid compatibility version range was accepted");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.PARSER_VERSION_INCOMPATIBLE",
      [](auto* bundle) { bundle->parser_compatibility_version = 4; },
      "SML-085 parser compatibility version outside range was accepted");

  auto bundle = ReleasePack();
  bundle.compatible_with_parser = false;
  const auto admission = sbsql::AdmitLanguageResourceBundleOperation(
      sbsql::LanguageBundleAdmissionRequest{sbsql::LanguageBundleOperation::kLoad, bundle});
  Require(!admission.accepted &&
              admission.HasIssue("SBSQL.LANG_BUNDLE.INCOMPATIBLE"),
          "SML-085 incompatible pack admission did not fail closed");
}

void VerifyLifecycleStatesAndChannels() {
  const std::vector<std::string> admitted_states = {
      "generated", "signed", "published", "admitted",
      "downloaded", "cached", "delta_updated"};
  for (const auto& state : admitted_states) {
    auto bundle = ReleasePack();
    bundle.lifecycle_state = state;
    const auto result = sbsql::ValidateLanguageResourceBundleManifest(bundle);
    Require(result.accepted, "SML-085 admitted lifecycle state was rejected");
  }

  auto bundle = ReleasePack();
  bundle.lifecycle_state = "rolled_back";
  auto bundle_result = sbsql::ValidateLanguageResourceBundleManifest(bundle);
  Require(bundle_result.accepted &&
              bundle_result.HasIssue("SBSQL.LANG_BUNDLE.ROLLED_BACK"),
          "SML-085 rollback lifecycle state did not admit with warning");

  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.REVOKED",
      [](auto* pack) { pack->lifecycle_state = "revoked"; },
      "SML-085 revoked lifecycle pack did not fail closed");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.EXPIRED",
      [](auto* pack) { pack->lifecycle_state = "expired"; },
      "SML-085 expired lifecycle pack did not fail closed");
  RequireBundleIssue(
      "SBSQL.LANG_BUNDLE.REMOVED",
      [](auto* pack) { pack->lifecycle_state = "removed"; },
      "SML-085 removed lifecycle pack did not fail closed");

  auto profile = ReleaseProfile();
  profile.channel = sbsql::LanguageResourceChannel::kExperimental;
  profile.support_state = sbsql::LanguageResourceSupportState::kMachineBootstrap;
  auto profile_result = sbsql::ValidateLanguageResourceManifest(profile);
  auto lifecycle = sbsql::ClassifyLanguageResourceLifecycle(profile);
  Require(profile_result.accepted &&
              profile_result.HasIssue("SBSQL.LANG_RESOURCE.EXPERIMENTAL_UNSUPPORTED") &&
              lifecycle.load_allowed && !lifecycle.support_claim_allowed,
          "SML-088 experimental profile did not admit with unsupported warning");

  profile = ReleaseProfile();
  profile.channel = sbsql::LanguageResourceChannel::kPreview;
  profile.support_state = sbsql::LanguageResourceSupportState::kNativeReviewed;
  profile_result = sbsql::ValidateLanguageResourceManifest(profile);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(profile);
  Require(profile_result.accepted &&
              profile_result.HasIssue("SBSQL.LANG_RESOURCE.PREVIEW_LIMITED_SUPPORT") &&
              lifecycle.load_allowed && !lifecycle.support_claim_allowed,
          "SML-088 preview profile did not admit with warning");

  profile.support_state = sbsql::LanguageResourceSupportState::kMachineBootstrap;
  profile_result = sbsql::ValidateLanguageResourceManifest(profile);
  Require(!profile_result.accepted &&
              profile_result.HasIssue("SBSQL.LANG_RESOURCE.PREVIEW_REVIEW_REQUIRED"),
          "SML-088 preview profile without review did not fail closed");

  profile = ReleaseProfile();
  profile.channel = sbsql::LanguageResourceChannel::kBeta;
  profile.support_state = sbsql::LanguageResourceSupportState::kNativeReviewed;
  profile_result = sbsql::ValidateLanguageResourceManifest(profile);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(profile);
  Require(profile_result.accepted &&
              profile_result.HasIssue("SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT") &&
              lifecycle.load_allowed && !lifecycle.support_claim_allowed,
          "SML-088 beta profile did not admit with warning");

  profile = ReleaseProfile();
  profile.channel = sbsql::LanguageResourceChannel::kDeprecated;
  profile.deprecation_notice_id = "deprecation.fr-ca.v1";
  profile.deprecation_replacement_profile_uuid = "019f1880-0000-7000-8000-000000000089";
  profile_result = sbsql::ValidateLanguageResourceManifest(profile);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(profile);
  Require(profile_result.accepted &&
              profile_result.HasIssue("SBSQL.LANG_RESOURCE.DEPRECATED") &&
              lifecycle.load_allowed && lifecycle.support_claim_allowed &&
              lifecycle.emits_deprecation_warning,
          "SML-088 deprecated profile did not admit with warning");

  profile = ReleaseProfile();
  profile.channel = sbsql::LanguageResourceChannel::kRevoked;
  profile.revocation_notice_id = "revocation.fr-ca.v1";
  profile_result = sbsql::ValidateLanguageResourceManifest(profile);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(profile);
  Require(!profile_result.accepted &&
              profile_result.HasIssue("SBSQL.LANG_RESOURCE.REVOKED") &&
              !lifecycle.load_allowed && !lifecycle.use_allowed,
          "SML-088 revoked profile did not fail closed");

  profile = ReleaseProfile();
  profile.channel = sbsql::LanguageResourceChannel::kRemoved;
  profile.removal_notice_id = "removal.fr-ca.v1";
  profile_result = sbsql::ValidateLanguageResourceManifest(profile);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(profile);
  Require(!profile_result.accepted &&
              profile_result.HasIssue("SBSQL.LANG_RESOURCE.REMOVED") &&
              !lifecycle.load_allowed && !lifecycle.use_allowed,
          "SML-088 removed profile did not fail closed");

  profile = ReleaseProfile();
  profile.expired = true;
  profile_result = sbsql::ValidateLanguageResourceManifest(profile);
  Require(!profile_result.accepted &&
              profile_result.HasIssue("SBSQL.LANG_RESOURCE.EXPIRED"),
          "SML-088 expired profile did not fail closed");
}

} // namespace

int main() {
  VerifyReleaseGovernancePolicy();
  VerifyCommonPackLifecycleEvidence();
  VerifyCompatibilityVersioning();
  VerifyLifecycleStatesAndChannels();
  std::cout << "sbsql_language_resource_governance_lifecycle_conformance=passed\n";
  return EXIT_SUCCESS;
}
