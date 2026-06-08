// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "donor_emulated_index_mapping.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

struct DonorProfileRule {
  const char* provider;
  const char* profile;
  const char* donor_family;
  const char* native_family_id;
  const char* native_candidate_path;
  const char* semantic_profile;
};

constexpr std::array<DonorProfileRule, 9> kProfileRules = {{
    {"sqlite", "automatic_transient_index", "automatic_index",
     "temporary_work", "native.temporary_work.candidate_rows",
     "sqlite_automatic_index"},
    {"firebird", "temporary_sort_index", "temporary_sort",
     "temporary_work", "native.temporary_work.candidate_rows",
     "firebird_temporary_sort"},
    {"scratchbird", "in_memory_lookup", "memory_index", "in_memory",
     "native.in_memory.candidate_rows", "scratchbird_in_memory_lookup"},
    {"scratchbird", "graph_adjacency", "graph", "graph",
     "native.graph.adjacency_candidates", "scratchbird_graph_adjacency"},
    {"postgresql", "btree", "btree", "btree", "native.btree.candidates",
     "postgresql_btree"},
    {"postgresql", "gin", "gin", "gin", "native.gin.candidates",
     "postgresql_gin"},
    {"mysql", "fulltext", "full_text", "full_text",
     "native.full_text.candidates", "mysql_fulltext"},
    {"mysql", "hash", "hash", "hash", "native.hash.candidates",
     "mysql_hash"},
    {"postgresql", "rum", "rum", "rum", "native.rum.candidates",
     "postgresql_rum"},
}};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error,
          Subsystem::engine};
}

std::string NormalizeToken(std::string_view input) {
  std::string out;
  bool last_underscore = false;
  for (unsigned char ch : input) {
    if (std::isalnum(ch) != 0) {
      out.push_back(static_cast<char>(std::tolower(ch)));
      last_underscore = false;
      continue;
    }
    if (!last_underscore) {
      out.push_back('_');
      last_underscore = true;
    }
  }
  while (!out.empty() && out.front() == '_') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

std::string NormalizeProvider(std::string_view input) {
  auto provider = NormalizeToken(input);
  if (provider == "postgres" || provider == "pg") return "postgresql";
  if (provider == "sqlite3") return "sqlite";
  if (provider == "mariadb" || provider == "maria_db") return "mysql";
  if (provider == "fb") return "firebird";
  return provider;
}

std::string NormalizeProfile(std::string_view provider, std::string_view input) {
  auto profile = NormalizeToken(input);
  if (provider == "postgresql") {
    if (profile == "b_tree" || profile == "btree_ops") return "btree";
    if (profile == "gin_ops") return "gin";
  }
  if (provider == "sqlite" &&
      (profile == "automatic_index" || profile == "auto_index")) {
    return "automatic_transient_index";
  }
  if (provider == "mysql" && profile == "full_text") return "fulltext";
  return profile;
}

const DonorProfileRule* FindProfileRule(std::string_view provider,
                                        std::string_view profile) {
  for (const auto& rule : kProfileRules) {
    if (provider == rule.provider && profile == rule.profile) {
      return &rule;
    }
  }
  return nullptr;
}

const char* FirstAuthorityClaim(const DonorEmulatedAuthorityClaims& claims) {
  if (claims.parser_finality) return "parser_finality";
  if (claims.donor_finality) return "donor_finality";
  if (claims.provider_finality) return "provider_finality";
  if (claims.index_finality) return "index_finality";
  if (claims.security) return "security";
  if (claims.visibility) return "visibility";
  if (claims.transaction_finality) return "transaction_finality";
  if (claims.recovery) return "recovery";
  if (claims.log_finality) return "log_finality";
  if (claims.physical_truth) return "physical_truth";
  return "";
}

bool HasAuthorityClaim(const DonorEmulatedAuthorityClaims& claims) {
  return FirstAuthorityClaim(claims)[0] != '\0';
}

std::vector<std::string> BaseEvidence(std::string_view provider,
                                      std::string_view profile,
                                      std::string_view donor_family) {
  return {std::string(kDonorEmulatedIndexMappingKey),
          std::string("donor_emulated.provider=") + std::string(provider),
          std::string("donor_emulated.profile=") + std::string(profile),
          std::string("donor_emulated.family=") + std::string(donor_family),
          "donor_emulated.catalog_identity_only=true",
          "donor_emulated.candidate_only=true",
          "donor_emulated.final_rows_authorized=false",
          "donor_emulated.metadata_authority=false",
          "donor_emulated.physical_truth_authority=false",
          "donor_emulated.parser_authority=false",
          "donor_emulated.donor_authority=false",
          "donor_emulated.provider_authority=false",
          "donor_emulated.index_authority=false",
          "donor_emulated.security_authority=false",
          "donor_emulated.visibility_authority=false",
          "donor_emulated.transaction_finality_authority=false",
          "donor_emulated.recovery_authority=false",
          "donor_emulated.log_finality_authority=false",
          "donor_emulated.exact_source.required=true",
          "donor_emulated.mga_visibility_recheck.required=true",
          "donor_emulated.security_recheck.required=true",
          "donor_emulated.engine_rechecks.required=true"};
}

void AddNativeEvidence(std::vector<std::string>* evidence,
                       std::string_view native_family_id,
                       std::string_view native_candidate_path) {
  evidence->push_back(std::string("donor_emulated.native_family=") +
                      std::string(native_family_id));
  evidence->push_back(std::string("donor_emulated.native_candidate_path=") +
                      std::string(native_candidate_path));
}

bool AdmissionProofValid(const DonorEmulatedIndexAdmissionProof& proof) {
  return proof.exact_source_required && proof.exact_source_available &&
         proof.mga_visibility_recheck_required &&
         proof.mga_visibility_recheck_available &&
         proof.security_recheck_required && proof.security_recheck_available &&
         !proof.exact_source_token.empty() &&
         !proof.mga_visibility_token.empty() && !proof.security_token.empty();
}

DonorEmulatedIndexAdmissionResult RefuseAdmission(
    std::string_view provider,
    std::string_view profile,
    std::string_view donor_family,
    std::string_view native_family_id,
    std::string_view native_candidate_path,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason) {
  DonorEmulatedIndexAdmissionResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.normalized_provider = provider;
  result.normalized_profile = profile;
  result.normalized_family = donor_family;
  result.native_family_id = native_family_id;
  result.native_candidate_path = native_candidate_path;
  result.evidence = BaseEvidence(provider, profile, donor_family);
  if (!native_family_id.empty() || !native_candidate_path.empty()) {
    AddNativeEvidence(&result.evidence, native_family_id, native_candidate_path);
  }
  result.evidence.push_back("fail_closed=true");
  result.evidence.push_back("refusal_reason=" + reason);
  result.refusal_reasons.push_back(reason);
  result.diagnostic = MakeDonorEmulatedIndexMappingDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      reason);
  return result;
}

}  // namespace

DonorEmulatedIndexMetadataProjection ProjectDonorEmulatedIndexMetadata(
    const DonorEmulatedIndexDeclaration& declaration) {
  const auto provider = NormalizeProvider(declaration.donor_provider);
  const auto profile = NormalizeProfile(
      provider, declaration.donor_profile);
  const auto* rule = FindProfileRule(provider, profile);
  const auto donor_family =
      !declaration.donor_family.empty()
          ? NormalizeToken(declaration.donor_family)
          : std::string(rule != nullptr ? rule->donor_family : "");

  DonorEmulatedIndexMetadataProjection result;
  result.normalized_provider = provider;
  result.normalized_profile = profile;
  result.normalized_family = donor_family;
  result.catalog_identity = declaration.catalog_identity;
  result.declared_name = declaration.declared_name;
  result.management_visible = true;
  result.catalog_identity_only = true;
  result.executable = false;
  result.native_admission_required = true;
  result.donor_metadata_authority = false;

  if (declaration.descriptor_scan_fallback_requested) {
    result.status = ErrorStatus();
    result.evidence = BaseEvidence(provider, profile, donor_family);
    result.evidence.push_back("donor_emulated.metadata_projection=false");
    result.refusal_reasons.push_back("descriptor_scan_fallback_refused");
    result.diagnostic = MakeDonorEmulatedIndexMappingDiagnostic(
        result.status, "INDEX.DONOR_EMULATED.DESCRIPTOR_SCAN_REFUSED",
        "index.donor_emulated.descriptor_scan_refused",
        "descriptor_scan_fallback_refused");
    return result;
  }
  if (declaration.behavior_scan_fallback_requested) {
    result.status = ErrorStatus();
    result.evidence = BaseEvidence(provider, profile, donor_family);
    result.evidence.push_back("donor_emulated.metadata_projection=false");
    result.refusal_reasons.push_back("behavior_scan_fallback_refused");
    result.diagnostic = MakeDonorEmulatedIndexMappingDiagnostic(
        result.status, "INDEX.DONOR_EMULATED.BEHAVIOR_SCAN_REFUSED",
        "index.donor_emulated.behavior_scan_refused",
        "behavior_scan_fallback_refused");
    return result;
  }
  if (HasAuthorityClaim(declaration.authority_claims)) {
    const std::string reason =
        std::string("authority_claim_refused:") +
        FirstAuthorityClaim(declaration.authority_claims);
    result.status = ErrorStatus();
    result.evidence = BaseEvidence(provider, profile, donor_family);
    result.evidence.push_back("donor_emulated.metadata_projection=false");
    result.evidence.push_back(reason);
    result.refusal_reasons.push_back(reason);
    result.diagnostic = MakeDonorEmulatedIndexMappingDiagnostic(
        result.status, "INDEX.DONOR_EMULATED.AUTHORITY_CLAIM_REFUSED",
        "index.donor_emulated.authority_claim_refused", reason);
    return result;
  }

  if (provider.empty() || profile.empty() || rule == nullptr ||
      donor_family.empty() || donor_family != rule->donor_family) {
    result.status = ErrorStatus();
    result.evidence = BaseEvidence(provider, profile, donor_family);
    result.evidence.push_back("donor_emulated.metadata_projection=false");
    result.refusal_reasons.push_back("unsupported_donor_profile");
    result.diagnostic = MakeDonorEmulatedIndexMappingDiagnostic(
        result.status,
        "INDEX.DONOR_EMULATED.UNSUPPORTED_PROFILE",
        "index.donor_emulated.unsupported_profile",
        provider + ":" + profile);
    return result;
  }

  result.status = OkStatus();
  result.semantic_profile = rule->semantic_profile;
  result.native_family_id = rule->native_family_id;
  result.native_candidate_path = rule->native_candidate_path;
  if (const auto lookup = FindBuiltinIndexFamilyById(rule->native_family_id);
      lookup.ok()) {
    result.native_family = lookup.descriptor->family;
  }
  result.evidence = BaseEvidence(provider, profile, donor_family);
  AddNativeEvidence(&result.evidence, result.native_family_id,
                    result.native_candidate_path);
  result.evidence.push_back(std::string("donor_emulated.semantic_profile=") +
                            result.semantic_profile);
  result.evidence.push_back("donor_emulated.metadata_projection=true");
  result.evidence.push_back("donor_emulated.executable=false");
  result.evidence.push_back("donor_emulated.native_admission_required=true");
  result.diagnostic = MakeDonorEmulatedIndexMappingDiagnostic(
      result.status,
      "INDEX.DONOR_EMULATED.METADATA_PROJECTED",
      "index.donor_emulated.metadata_projected",
      provider + ":" + profile);
  return result;
}

DonorEmulatedIndexAdmissionResult AdmitDonorEmulatedIndexMapping(
    const DonorEmulatedIndexDeclaration& declaration,
    const DonorEmulatedIndexAdmissionProof& proof) {
  const auto provider = NormalizeProvider(declaration.donor_provider);
  const auto profile = NormalizeProfile(provider, declaration.donor_profile);
  const auto* rule = FindProfileRule(provider, profile);
  const auto donor_family =
      !declaration.donor_family.empty()
          ? NormalizeToken(declaration.donor_family)
          : std::string(rule != nullptr ? rule->donor_family : "");

  if (declaration.descriptor_scan_fallback_requested) {
    return RefuseAdmission(provider, profile, donor_family, "", "",
                           "INDEX.DONOR_EMULATED.DESCRIPTOR_SCAN_REFUSED",
                           "index.donor_emulated.descriptor_scan_refused",
                           "descriptor_scan_fallback_refused");
  }
  if (declaration.behavior_scan_fallback_requested) {
    return RefuseAdmission(provider, profile, donor_family, "", "",
                           "INDEX.DONOR_EMULATED.BEHAVIOR_SCAN_REFUSED",
                           "index.donor_emulated.behavior_scan_refused",
                           "behavior_scan_fallback_refused");
  }
  if (HasAuthorityClaim(declaration.authority_claims)) {
    const std::string reason =
        std::string("authority_claim_refused:") +
        FirstAuthorityClaim(declaration.authority_claims);
    auto refused = RefuseAdmission(
        provider, profile, donor_family, "", "",
        "INDEX.DONOR_EMULATED.AUTHORITY_CLAIM_REFUSED",
        "index.donor_emulated.authority_claim_refused", reason);
    refused.evidence.push_back(reason);
    return refused;
  }
  if (provider.empty() || profile.empty() || rule == nullptr ||
      donor_family.empty() || donor_family != rule->donor_family) {
    return RefuseAdmission(provider, profile, donor_family, "", "",
                           "INDEX.DONOR_EMULATED.UNSUPPORTED_PROFILE",
                           "index.donor_emulated.unsupported_profile",
                           "unsupported_donor_profile");
  }

  const auto lookup = FindBuiltinIndexFamilyById(rule->native_family_id);
  if (!lookup.ok()) {
    return RefuseAdmission(provider, profile, donor_family,
                           rule->native_family_id, rule->native_candidate_path,
                           "INDEX.DONOR_EMULATED.NATIVE_TARGET_MISSING",
                           "index.donor_emulated.native_target_missing",
                           "native_target_missing");
  }
  const auto* state =
      FindBuiltinIndexFamilyPhysicalCapabilityState(lookup.descriptor->family);
  if (state == nullptr || !state->runtime_available ||
      !state->benchmark_clean || !state->physically_complete()) {
    auto refused = RefuseAdmission(
        provider, profile, donor_family, rule->native_family_id,
        rule->native_candidate_path,
        "INDEX.DONOR_EMULATED.NATIVE_TARGET_INCOMPLETE",
        "index.donor_emulated.native_target_incomplete",
        state != nullptr && !state->blocker_diagnostic_code.empty()
            ? state->blocker_diagnostic_code
            : "native_target_incomplete");
    refused.native_family = lookup.descriptor->family;
    if (state != nullptr) {
      refused.evidence.push_back(
          std::string("donor_emulated.native_blocker=") +
          IndexFamilyPhysicalCapabilityBlockerName(state->blocker));
      refused.evidence.push_back(
          std::string("donor_emulated.native_blocker_diagnostic=") +
          state->blocker_diagnostic_code);
    }
    return refused;
  }
  if (!AdmissionProofValid(proof)) {
    auto refused = RefuseAdmission(
        provider, profile, donor_family, rule->native_family_id,
        rule->native_candidate_path,
        "INDEX.DONOR_EMULATED.MISSING_RECHECK_PROOF",
        "index.donor_emulated.missing_recheck_proof",
        "missing_exact_source_mga_or_security_recheck_proof");
    refused.native_family = lookup.descriptor->family;
    return refused;
  }

  DonorEmulatedIndexAdmissionResult result;
  result.status = OkStatus();
  result.admitted = true;
  result.normalized_provider = provider;
  result.normalized_profile = profile;
  result.normalized_family = donor_family;
  result.semantic_profile = rule->semantic_profile;
  result.native_family = lookup.descriptor->family;
  result.native_family_id = rule->native_family_id;
  result.native_candidate_path = rule->native_candidate_path;
  result.candidate_only = true;
  result.exact_source_required = true;
  result.mga_visibility_recheck_required = true;
  result.security_recheck_required = true;
  result.donor_metadata_authority = false;
  result.physical_truth_authority = false;
  result.evidence = BaseEvidence(provider, profile, donor_family);
  AddNativeEvidence(&result.evidence, result.native_family_id,
                    result.native_candidate_path);
  result.evidence.push_back(std::string("donor_emulated.semantic_profile=") +
                            result.semantic_profile);
  result.evidence.push_back("donor_emulated.native_family_complete=true");
  result.evidence.push_back("donor_emulated.native_family_benchmark_clean=true");
  result.evidence.push_back("donor_emulated.admitted_via_native_family=true");
  result.diagnostic = MakeDonorEmulatedIndexMappingDiagnostic(
      result.status, "INDEX.DONOR_EMULATED.ADMITTED_NATIVE_CANDIDATE",
      "index.donor_emulated.admitted_native_candidate",
      provider + ":" + profile + "=>" + result.native_family_id);
  return result;
}

DiagnosticRecord MakeDonorEmulatedIndexMappingDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {},
                        "core.index.donor_emulated_mapping");
}

}  // namespace scratchbird::core::index
