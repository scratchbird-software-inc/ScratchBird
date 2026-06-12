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
#include <set>
#include <string>
#include <string_view>
#include <utility>
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

std::string DescribeIssues(const sbsql::ResourceValidationResult& result) {
  std::string description;
  for (const auto& issue : result.issues) {
    if (!description.empty()) description += ",";
    description += issue.code;
  }
  return description.empty() ? "none" : description;
}

struct MatrixIssue {
  std::string code;
  std::string detail;
};

struct MatrixResult {
  bool accepted{true};
  std::vector<MatrixIssue> issues;

  void AddError(std::string code, std::string detail) {
    accepted = false;
    issues.push_back(MatrixIssue{std::move(code), std::move(detail)});
  }

  bool HasIssue(std::string_view code) const {
    return std::any_of(issues.begin(), issues.end(), [&](const auto& issue) {
      return issue.code == code;
    });
  }
};

const std::vector<std::string>& RequiredExactLocaleTags() {
  static const std::vector<std::string> tags = {
      "en-CA", "en-US", "en-GB", "fr-CA", "fr-FR", "es-419"};
  return tags;
}

bool Contains(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

std::string NormalizeTag(std::string_view tag) {
  std::string normalized;
  normalized.reserve(tag.size());
  for (const char ch : tag) {
    normalized.push_back(ch == '-' ? '_' : ch);
  }
  return normalized;
}

bool IsWildcardTag(std::string_view tag) {
  return tag.find('*') != std::string_view::npos ||
         tag.find('?') != std::string_view::npos;
}

bool IsBaseLanguageTag(std::string_view tag) {
  return tag == "en" || tag == "fr" || tag == "es";
}

sbsql::LanguageDataProvenance LocaleProvenance(std::string_view exact_tag) {
  const auto normalized = NormalizeTag(exact_tag);
  return sbsql::LanguageDataProvenance{
      "ScratchBird SBsql beta exact locale profile " + std::string(exact_tag),
      "SML-014.beta.1",
      "MPL-2.0",
      "sbsql-sml014-exact-locale-normalize-v1",
      "sbom.sbsql.beta.locale." + normalized,
      "notice.sbsql.beta.locale." + normalized,
      true,
      false,
      true};
}

sbsql::LanguageResourceManifest ExactBetaProfile(std::string_view exact_tag,
                                                 std::string profile_uuid) {
  const auto normalized = NormalizeTag(exact_tag);
  sbsql::LanguageResourceManifest manifest;
  manifest.profile_uuid = std::move(profile_uuid);
  manifest.exact_tag = std::string(exact_tag);
  manifest.common_resource_hash = "sha256:sbsql.beta.locale." + normalized + ".common";
  manifest.canonical_surface_registry_hash =
      "sha256:sbsql.beta.locale." + normalized + ".surface";
  manifest.sblr_registry_hash = "sha256:sbsql.sblr.registry.v3";
  manifest.predictive_grammar_hash =
      "sha256:sbsql.beta.locale." + normalized + ".predictive";
  manifest.renderer_registry_hash =
      "sha256:sbsql.beta.locale." + normalized + ".renderer";
  manifest.diagnostic_pack_hash =
      "sha256:sbsql.beta.locale." + normalized + ".diagnostic";
  manifest.signature_id = "sig.sbsql.beta.locale." + normalized;
  manifest.signing_key_id = "scratchbird.sbsql.beta.locale.signing.key";
  manifest.governance_evidence_id =
      "SML-014.beta.locale." + normalized + ".governance";
  manifest.native_review_evidence_id =
      "SML-GATE-013.beta.locale." + normalized + ".native_review";
  manifest.support_owner_id = "scratchbird.sbsql.localization." + normalized;
  manifest.trace_oracle_id =
      "SML-GATE-014.beta.locale." + normalized + ".trace_oracle";
  manifest.release_channel_evidence_id =
      "SML-GATE-013.beta.locale." + normalized + ".channel_evidence";
  manifest.fallback_parent_uuid =
      std::string(sbsql::BuiltInCanonicalEnglishRecoveryProfile().profile_uuid);
  manifest.channel = sbsql::LanguageResourceChannel::kBeta;
  manifest.support_state = sbsql::LanguageResourceSupportState::kNativeReviewed;
  manifest.canonical_ids = {
      "SBSQL.CANONICAL." + normalized,
      "SBSQL.LOCALE_PROFILE." + normalized,
      "SBLR.REGISTRY.V1"};
  manifest.renderer_edges = {"renderer.sbsql.beta.locale." + normalized};
  manifest.provenance.push_back(LocaleProvenance(exact_tag));
  return manifest;
}

std::vector<sbsql::LanguageResourceManifest> ExactBetaLocaleMatrix() {
  return {
      ExactBetaProfile("en-CA", "019f1014-0000-7000-8000-000000000101"),
      ExactBetaProfile("en-US", "019f1014-0000-7000-8000-000000000102"),
      ExactBetaProfile("en-GB", "019f1014-0000-7000-8000-000000000103"),
      ExactBetaProfile("fr-CA", "019f1014-0000-7000-8000-000000000201"),
      ExactBetaProfile("fr-FR", "019f1014-0000-7000-8000-000000000202"),
      ExactBetaProfile("es-419", "019f1014-0000-7000-8000-000000000301"),
  };
}

sbsql::LanguageResourceBundleManifest BundleForProfile(
    const sbsql::LanguageResourceManifest& profile) {
  const auto normalized = NormalizeTag(profile.exact_tag);
  sbsql::LanguageResourceBundleManifest bundle;
  bundle.bundle_uuid = profile.profile_uuid + ".bundle";
  bundle.bundle_contract_id = "sbsql.lang." + normalized + "@beta.1";
  bundle.exact_tag = profile.exact_tag;
  bundle.dialect_profile_uuid = "sbsql.v3";
  bundle.topology_profile_uuid = "topology.sbsql.canonical_svo.v1";
  bundle.common_resource_hash = profile.common_resource_hash;
  bundle.canonical_element_stream_schema_hash =
      "sha256:sbsql.canonical_element_stream.schema.v1";
  bundle.predictive_resource_hash = profile.predictive_grammar_hash;
  bundle.renderer_resource_hash = profile.renderer_registry_hash;
  bundle.diagnostic_resource_hash = profile.diagnostic_pack_hash;
  bundle.compatibility_identity = "sbsql.resource.compat.v1";
  bundle.lifecycle_state = "staged";
  bundle.language_profile = profile;
  bundle.provenance = profile.provenance;
  return bundle;
}

const sbsql::LanguageResourceManifest* FindExactProfile(
    const std::vector<sbsql::LanguageResourceManifest>& profiles,
    std::string_view exact_tag) {
  const auto found = std::find_if(profiles.begin(), profiles.end(), [&](const auto& profile) {
    return profile.exact_tag == exact_tag;
  });
  return found == profiles.end() ? nullptr : &*found;
}

void RequireEvidenceFields(const sbsql::LanguageResourceManifest& profile,
                           MatrixResult* result) {
  if (profile.signature_id.empty() || profile.signing_key_id.empty() ||
      profile.governance_evidence_id.empty() ||
      profile.native_review_evidence_id.empty() ||
      profile.support_owner_id.empty() || profile.trace_oracle_id.empty() ||
      profile.release_channel_evidence_id.empty() || profile.provenance.empty()) {
    result->AddError("SML014.EVIDENCE_MISSING",
                     "exact locale profile " + profile.exact_tag +
                         " is missing signature, provenance, or governance evidence");
  }
}

void RequireUniqueHash(std::string_view field_name,
                       const std::vector<sbsql::LanguageResourceManifest>& profiles,
                       const std::vector<std::string>& values,
                       MatrixResult* result) {
  std::set<std::string> seen;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (values[i].empty()) continue;
    if (!seen.insert(values[i]).second) {
      result->AddError("SML014.COLLAPSED_RESOURCE_HASH",
                       std::string(field_name) + " collapsed at exact locale " +
                           profiles[i].exact_tag);
    }
  }
}

MatrixResult ValidateExactBetaLocaleMatrix(
    const std::vector<sbsql::LanguageResourceManifest>& profiles) {
  MatrixResult result;
  std::set<std::string> seen_tags;
  std::set<std::string> seen_profile_uuids;
  bool has_exact_es419 = false;
  bool has_base_es = false;
  bool has_wildcard_es = false;

  std::vector<std::string> common_hashes;
  std::vector<std::string> predictive_hashes;
  std::vector<std::string> renderer_hashes;
  std::vector<std::string> diagnostic_hashes;
  common_hashes.reserve(profiles.size());
  predictive_hashes.reserve(profiles.size());
  renderer_hashes.reserve(profiles.size());
  diagnostic_hashes.reserve(profiles.size());

  for (const auto& profile : profiles) {
    const auto public_validation = sbsql::ValidateLanguageResourceManifest(profile);
    if (!public_validation.accepted) {
      result.AddError("SML014.PROFILE_VALIDATION_FAILED",
                      "public language-resource validation rejected " +
                          profile.exact_tag);
    }
    if (!public_validation.HasIssue("SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT")) {
      result.AddError("SML014.BETA_LIMITED_SUPPORT_WARNING_MISSING",
                      "beta exact locale profile did not emit limited-support evidence");
    }
    if (profile.channel != sbsql::LanguageResourceChannel::kBeta ||
        profile.support_state != sbsql::LanguageResourceSupportState::kNativeReviewed) {
      result.AddError("SML014.BETA_NATIVE_REVIEW_REQUIRED",
                      "exact locale profile must be beta and native reviewed");
    }
    if (profile.exact_tag == "es-419") has_exact_es419 = true;
    if (profile.exact_tag == "es") has_base_es = true;
    if (profile.exact_tag.rfind("es-", 0) == 0 && IsWildcardTag(profile.exact_tag)) {
      has_wildcard_es = true;
    }
    if (IsWildcardTag(profile.exact_tag)) {
      result.AddError("SML014.WILDCARD_LOCALE_PROFILE",
                      "wildcard locale profile is not an exact profile: " +
                          profile.exact_tag);
    }
    if (IsBaseLanguageTag(profile.exact_tag)) {
      result.AddError("SML014.BASE_LANGUAGE_PROFILE",
                      "base language profile cannot satisfy exact locale: " +
                          profile.exact_tag);
    }
    if (!Contains(RequiredExactLocaleTags(), profile.exact_tag)) {
      result.AddError("SML014.UNEXPECTED_LOCALE_PROFILE",
                      "beta locale matrix contains a profile outside the exact matrix: " +
                          profile.exact_tag);
    }
    if (!seen_tags.insert(profile.exact_tag).second) {
      result.AddError("SML014.DUPLICATE_EXACT_TAG",
                      "duplicate exact locale profile: " + profile.exact_tag);
    }
    if (!seen_profile_uuids.insert(profile.profile_uuid).second) {
      result.AddError("SML014.DUPLICATE_PROFILE_UUID",
                      "profile UUID was reused across exact locale profiles");
    }
    RequireEvidenceFields(profile, &result);
    common_hashes.push_back(profile.common_resource_hash);
    predictive_hashes.push_back(profile.predictive_grammar_hash);
    renderer_hashes.push_back(profile.renderer_registry_hash);
    diagnostic_hashes.push_back(profile.diagnostic_pack_hash);
  }

  for (const auto& required_tag : RequiredExactLocaleTags()) {
    if (!seen_tags.count(required_tag)) {
      result.AddError("SML014.MISSING_EXACT_LOCALE_PROFILE",
                      "beta locale matrix is missing exact profile " + required_tag);
    }
  }
  if (!has_exact_es419 && (has_base_es || has_wildcard_es)) {
    result.AddError("SML014.SPANISH_REGIONAL_EXACT_PROFILE_REQUIRED",
                    "es-419 must be represented by its own exact profile, not es or es-*");
  }

  RequireUniqueHash("common_resource_hash", profiles, common_hashes, &result);
  RequireUniqueHash("predictive_grammar_hash", profiles, predictive_hashes, &result);
  RequireUniqueHash("renderer_registry_hash", profiles, renderer_hashes, &result);
  RequireUniqueHash("diagnostic_pack_hash", profiles, diagnostic_hashes, &result);
  return result;
}

void RequireMatrixIssue(const MatrixResult& result,
                        std::string_view code,
                        std::string_view message) {
  Require(!result.accepted && result.HasIssue(code), message);
}

void VerifyPositiveExactBetaLocaleMatrix() {
  const auto matrix = ExactBetaLocaleMatrix();
  const auto result = ValidateExactBetaLocaleMatrix(matrix);
  Require(result.accepted, "SML-014 exact beta locale profile matrix was rejected");
  Require(matrix.size() == RequiredExactLocaleTags().size(),
          "SML-014 beta locale profile matrix size drifted");
  Require(FindExactProfile(matrix, "en-CA") != nullptr, "SML-014 en-CA profile missing");
  Require(FindExactProfile(matrix, "en-US") != nullptr, "SML-014 en-US profile missing");
  Require(FindExactProfile(matrix, "en-GB") != nullptr, "SML-014 en-GB profile missing");
  Require(FindExactProfile(matrix, "fr-CA") != nullptr, "SML-014 fr-CA profile missing");
  Require(FindExactProfile(matrix, "fr-FR") != nullptr, "SML-014 fr-FR profile missing");
  Require(FindExactProfile(matrix, "es-419") != nullptr, "SML-014 es-419 profile missing");
  Require(FindExactProfile(matrix, "es") == nullptr,
          "SML-014 base es profile leaked into exact locale matrix");
  Require(FindExactProfile(matrix, "es-*") == nullptr,
          "SML-014 wildcard es-* profile leaked into exact locale matrix");

  for (const auto& profile : matrix) {
    const auto validation = sbsql::ValidateLanguageResourceManifest(profile);
    if (!validation.accepted) {
      Fail("SML-014 exact beta profile failed public resource validation: " +
           DescribeIssues(validation));
    }
    Require(validation.HasIssue("SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT"),
            "SML-GATE-013 beta lifecycle evidence warning missing");

    const auto bundle = BundleForProfile(profile);
    const auto bundle_validation = sbsql::ValidateLanguageResourceBundleManifest(bundle);
    if (!bundle_validation.accepted) {
      Fail("SML-GATE-014 exact beta bundle manifest failed validation: " +
           DescribeIssues(bundle_validation));
    }
    Require(bundle_validation.HasIssue("SBSQL.LANG_RESOURCE.BETA_LIMITED_SUPPORT"),
            "SML-GATE-014 exact beta bundle did not retain beta lifecycle evidence");
    Require(bundle.exact_tag == profile.exact_tag &&
                bundle.common_resource_hash == profile.common_resource_hash &&
                bundle.language_profile.profile_uuid == profile.profile_uuid,
            "SML-GATE-014 bundle collapsed exact profile identity");
  }
}

void VerifyWildcardSpanishCannotSatisfyRegionalProfile() {
  auto matrix = ExactBetaLocaleMatrix();
  matrix.back().exact_tag = "es-*";
  auto result = ValidateExactBetaLocaleMatrix(matrix);
  RequireMatrixIssue(result, "SML014.WILDCARD_LOCALE_PROFILE",
                     "SML-014 wildcard es-* profile was accepted");
  RequireMatrixIssue(result, "SML014.SPANISH_REGIONAL_EXACT_PROFILE_REQUIRED",
                     "SML-014 wildcard es-* satisfied es-419");
}

void VerifyBaseSpanishCannotSatisfyRegionalProfile() {
  auto matrix = ExactBetaLocaleMatrix();
  matrix.back().exact_tag = "es";
  auto result = ValidateExactBetaLocaleMatrix(matrix);
  RequireMatrixIssue(result, "SML014.BASE_LANGUAGE_PROFILE",
                     "SML-014 base es profile was accepted");
  RequireMatrixIssue(result, "SML014.SPANISH_REGIONAL_EXACT_PROFILE_REQUIRED",
                     "SML-014 base es satisfied es-419");
}

void VerifyMissingExactManifestFailsClosed() {
  auto matrix = ExactBetaLocaleMatrix();
  matrix.erase(std::remove_if(matrix.begin(), matrix.end(), [](const auto& profile) {
                 return profile.exact_tag == "es-419";
               }),
               matrix.end());
  const auto result = ValidateExactBetaLocaleMatrix(matrix);
  RequireMatrixIssue(result, "SML014.MISSING_EXACT_LOCALE_PROFILE",
                     "SML-014 missing es-419 profile was accepted");
}

void VerifyCollapsedProfileIdentitiesFailClosed() {
  auto matrix = ExactBetaLocaleMatrix();
  matrix[1].profile_uuid = matrix[0].profile_uuid;
  auto result = ValidateExactBetaLocaleMatrix(matrix);
  RequireMatrixIssue(result, "SML014.DUPLICATE_PROFILE_UUID",
                     "SML-014 duplicate exact profile UUID was accepted");

  matrix = ExactBetaLocaleMatrix();
  matrix[4].common_resource_hash = matrix[3].common_resource_hash;
  result = ValidateExactBetaLocaleMatrix(matrix);
  RequireMatrixIssue(result, "SML014.COLLAPSED_RESOURCE_HASH",
                     "SML-014 collapsed exact locale resource hash was accepted");
}

void VerifyMissingEvidenceFailsClosed() {
  auto matrix = ExactBetaLocaleMatrix();
  matrix[0].governance_evidence_id.clear();
  auto result = ValidateExactBetaLocaleMatrix(matrix);
  RequireMatrixIssue(result, "SML014.EVIDENCE_MISSING",
                     "SML-014 profile without governance evidence was accepted");

  matrix = ExactBetaLocaleMatrix();
  matrix[1].provenance.clear();
  result = ValidateExactBetaLocaleMatrix(matrix);
  RequireMatrixIssue(result, "SML014.EVIDENCE_MISSING",
                     "SML-014 profile without provenance was accepted");

  matrix = ExactBetaLocaleMatrix();
  matrix[2].signature_id.clear();
  result = ValidateExactBetaLocaleMatrix(matrix);
  RequireMatrixIssue(result, "SML014.EVIDENCE_MISSING",
                     "SML-014 profile without signature was accepted");
}

} // namespace

int main() {
  VerifyPositiveExactBetaLocaleMatrix();
  VerifyWildcardSpanishCannotSatisfyRegionalProfile();
  VerifyBaseSpanishCannotSatisfyRegionalProfile();
  VerifyMissingExactManifestFailsClosed();
  VerifyCollapsedProfileIdentitiesFailClosed();
  VerifyMissingEvidenceFailsClosed();
  std::cout << "sbsql_language_locale_profile_matrix_conformance=passed\n";
  return EXIT_SUCCESS;
}
