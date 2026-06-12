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

sbsql::LanguageDataProvenance Provenance() {
  return sbsql::LanguageDataProvenance{
      "ScratchBird public SBsql language resource", "1",
      "MPL-2.0", "public-release-channel-gate",
      "sbom.sbsql.public.language_resource",
      "notice.scratchbird.builtin", false, false, true};
}

sbsql::LanguageResourceManifest BaseProfile() {
  sbsql::LanguageResourceManifest manifest;
  manifest.profile_uuid = "019f1000-0000-7000-8000-000000000401";
  manifest.exact_tag = "en";
  manifest.common_resource_hash = "public.common.sbsql.v1";
  manifest.canonical_surface_registry_hash = "public.surface.sbsql.v1";
  manifest.sblr_registry_hash = "public.sblr.registry.v1";
  manifest.predictive_grammar_hash = "public.predictive.sbsql.v1";
  manifest.renderer_registry_hash = "public.renderer.sbsql.v1";
  manifest.diagnostic_pack_hash = "public.diagnostics.sbsql.v1";
  manifest.signature_id = "public.release.signature";
  manifest.signing_key_id = "public.release.key";
  manifest.governance_evidence_id = "SML-092.public.release.governance";
  manifest.native_review_evidence_id = "SML-092.public.native.review";
  manifest.support_owner_id = "scratchbird.release.sbsql";
  manifest.trace_oracle_id = "SML-092.public.trace.oracle";
  manifest.channel = sbsql::LanguageResourceChannel::kReleaseSupported;
  manifest.support_state = sbsql::LanguageResourceSupportState::kReleaseSupported;
  manifest.canonical_ids = {"SBSQL.CANONICAL.EN", "SBLR.REGISTRY.V1"};
  manifest.provenance.push_back(Provenance());
  return manifest;
}

void VerifyAdmittedLimitedChannels() {
  auto manifest = BaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kExperimental;
  manifest.support_state = sbsql::LanguageResourceSupportState::kMachineBootstrap;
  auto result = sbsql::ValidateLanguageResourceManifest(manifest);
  auto lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              lifecycle.load_allowed &&
              !lifecycle.support_claim_allowed &&
              result.HasIssue("SBSQL.LANG_RESOURCE.EXPERIMENTAL_UNSUPPORTED"),
          "SML-092 experimental resource did not admit without support commitment");

  manifest = BaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kPreview;
  manifest.support_state = sbsql::LanguageResourceSupportState::kNativeReviewed;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              lifecycle.load_allowed &&
              !lifecycle.support_claim_allowed &&
              result.HasIssue("SBSQL.LANG_RESOURCE.PREVIEW_LIMITED_SUPPORT"),
          "SML-092 preview resource did not admit as limited support");

  manifest = BaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kBeta;
  manifest.support_state = sbsql::LanguageResourceSupportState::kNativeReviewed;
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              lifecycle.load_allowed &&
              !lifecycle.support_claim_allowed &&
              result.HasIssue("SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT"),
          "SML-092 beta resource did not admit as limited support");
}

void VerifyReleaseAndDeprecatedChannels() {
  auto manifest = BaseProfile();
  auto result = sbsql::ValidateLanguageResourceManifest(manifest);
  auto lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              lifecycle.load_allowed &&
              lifecycle.use_allowed &&
              lifecycle.support_claim_allowed &&
              !lifecycle.emits_deprecation_warning,
          "SML-092 release-supported resource did not carry support commitment");

  manifest.channel = sbsql::LanguageResourceChannel::kDeprecated;
  manifest.deprecation_notice_id = "SML-092.public.deprecation.notice";
  manifest.deprecation_replacement_profile_uuid =
      "019f1000-0000-7000-8000-000000000402";
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(result.accepted &&
              lifecycle.load_allowed &&
              lifecycle.use_allowed &&
              lifecycle.support_claim_allowed &&
              lifecycle.emits_deprecation_warning &&
              result.HasIssue("SBSQL.LANG_RESOURCE.DEPRECATED"),
          "SML-092 deprecated resource did not admit with warning");

  manifest.deprecation_notice_id.clear();
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  Require(!result.accepted &&
              result.HasIssue("SBSQL.LANG_RESOURCE.DEPRECATION_NOTICE_MISSING"),
          "SML-092 deprecated resource without notice was accepted");
}

void VerifyRefusedChannels() {
  auto manifest = BaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kRevoked;
  manifest.revocation_notice_id = "SML-092.public.revocation.notice";
  auto result = sbsql::ValidateLanguageResourceManifest(manifest);
  auto lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(!result.accepted &&
              !lifecycle.load_allowed &&
              !lifecycle.use_allowed &&
              !lifecycle.support_claim_allowed &&
              result.HasIssue("SBSQL.LANG_RESOURCE.REVOKED"),
          "SML-092 revoked resource was not refused");

  manifest = BaseProfile();
  manifest.channel = sbsql::LanguageResourceChannel::kRemoved;
  manifest.removal_notice_id = "SML-092.public.removal.notice";
  result = sbsql::ValidateLanguageResourceManifest(manifest);
  lifecycle = sbsql::ClassifyLanguageResourceLifecycle(manifest);
  Require(!result.accepted &&
              !lifecycle.load_allowed &&
              !lifecycle.use_allowed &&
              !lifecycle.support_claim_allowed &&
              result.HasIssue("SBSQL.LANG_RESOURCE.REMOVED"),
          "SML-092 removed resource was not refused");
}

} // namespace

int main() {
  VerifyAdmittedLimitedChannels();
  VerifyReleaseAndDeprecatedChannels();
  VerifyRefusedChannels();
  std::cout << "public_sbsql_language_release_channel_gate=passed\n";
  return EXIT_SUCCESS;
}
