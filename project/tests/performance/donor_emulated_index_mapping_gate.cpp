// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "donor_emulated_index_mapping.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace idx = scratchbird::core::index;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "donor_emulated_index_mapping_gate: " << message << '\n';
    std::exit(1);
  }
}

idx::DonorEmulatedIndexAdmissionProof ValidProof() {
  idx::DonorEmulatedIndexAdmissionProof proof;
  proof.exact_source_required = true;
  proof.exact_source_available = true;
  proof.mga_visibility_recheck_required = true;
  proof.mga_visibility_recheck_available = true;
  proof.security_recheck_required = true;
  proof.security_recheck_available = true;
  proof.exact_source_token = "exact-source-token";
  proof.mga_visibility_token = "mga-visibility-token";
  proof.security_token = "security-token";
  return proof;
}

idx::DonorEmulatedIndexDeclaration Declaration(std::string provider,
                                               std::string profile,
                                               std::string family = {}) {
  idx::DonorEmulatedIndexDeclaration declaration;
  declaration.donor_provider = std::move(provider);
  declaration.donor_profile = std::move(profile);
  declaration.donor_family = std::move(family);
  declaration.catalog_identity = "catalog:index:orders_lookup";
  declaration.declared_name = "orders_lookup";
  return declaration;
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

bool DetailContains(const scratchbird::core::platform::DiagnosticRecord& record,
                    const std::string& value) {
  for (const auto& argument : record.arguments) {
    if (argument.key == "detail" &&
        argument.value.find(value) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

void RequireNoRuntimeDocLeak(const std::vector<std::string>& values) {
  for (const auto& value : values) {
    const auto lower = Lower(value);
    for (const auto marker : {"execution_plan", "spec", "reference"}) {
      Require(lower.find(marker) == std::string::npos,
              std::string("runtime value leaked marker ") + marker + ": " +
                  value);
    }
  }
}

void AppendDiagnosticValues(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    std::vector<std::string>* values) {
  values->push_back(diagnostic.diagnostic_code);
  values->push_back(diagnostic.message_key);
  values->push_back(diagnostic.source_component);
  values->push_back(diagnostic.remediation_hint);
  for (const auto& argument : diagnostic.arguments) {
    values->push_back(argument.key);
    values->push_back(argument.value);
  }
}

void RequireNativeComplete(idx::IndexFamily family) {
  const auto* state = idx::FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  Require(state != nullptr, "native family capability state missing");
  Require(state->runtime_available, "native family not runtime available");
  Require(state->benchmark_clean, "native family not benchmark clean");
  Require(state->physically_complete(), "native family not physically complete");
}

void AllowedMappingToCompleteNativeFamilies() {
  const auto proof = ValidProof();
  struct Case {
    idx::DonorEmulatedIndexDeclaration declaration;
    idx::IndexFamily family;
    std::string provider_evidence;
    std::string profile_evidence;
    std::string native_evidence;
  };
  std::vector<Case> cases = {
      {Declaration(" SQLite3 ", "Automatic-Index"), idx::IndexFamily::temporary_work,
       "donor_emulated.provider=sqlite",
       "donor_emulated.profile=automatic_transient_index",
       "donor_emulated.native_family=temporary_work"},
      {Declaration("scratchbird", "in-memory lookup"), idx::IndexFamily::in_memory,
       "donor_emulated.provider=scratchbird",
       "donor_emulated.profile=in_memory_lookup",
       "donor_emulated.native_family=in_memory"},
      {Declaration("scratchbird", "graph adjacency"), idx::IndexFamily::graph,
       "donor_emulated.provider=scratchbird",
       "donor_emulated.profile=graph_adjacency",
       "donor_emulated.native_family=graph"}};

  for (const auto& item : cases) {
    RequireNativeComplete(item.family);
    const auto result = idx::AdmitDonorEmulatedIndexMapping(item.declaration,
                                                           proof);
    Require(result.ok(), "complete native donor mapping was refused");
    Require(result.native_family == item.family, "native family mismatch");
    Require(result.candidate_only, "result was not candidate-only");
    Require(!result.donor_metadata_authority,
            "donor metadata became authority");
    Require(!result.physical_truth_authority,
            "donor mapping became physical truth");
    Require(result.exact_source_required &&
                result.mga_visibility_recheck_required &&
                result.security_recheck_required,
            "engine recheck requirements missing");
    Require(HasEvidence(result.evidence, item.provider_evidence),
            "provider evidence missing");
    Require(HasEvidence(result.evidence, item.profile_evidence),
            "profile evidence missing");
    Require(HasEvidence(result.evidence, item.native_evidence),
            "native family evidence missing");
    Require(HasEvidence(result.evidence,
                        "donor_emulated.native_family_benchmark_clean=true"),
            "benchmark clean native evidence missing");
    Require(HasEvidence(result.evidence,
                        "donor_emulated.metadata_authority=false"),
            "metadata non-authority evidence missing");
    Require(HasEvidence(result.evidence,
                        "donor_emulated.mga_visibility_recheck.required=true"),
            "MGA recheck evidence missing");
    Require(HasEvidence(result.evidence,
                        "donor_emulated.security_recheck.required=true"),
            "security recheck evidence missing");
  }
}

void MetadataProjectionIsNonExecutable() {
  const auto projection = idx::ProjectDonorEmulatedIndexMetadata(
      Declaration("Postgres", "B-Tree"));
  Require(projection.ok(), "metadata projection refused recognized profile");
  Require(projection.management_visible, "management visibility missing");
  Require(projection.catalog_identity_only, "catalog identity surface missing");
  Require(!projection.executable, "projection became executable");
  Require(projection.native_admission_required,
          "projection did not require native admission");
  Require(!projection.donor_metadata_authority,
          "projection made donor metadata authoritative");
  Require(projection.native_family == idx::IndexFamily::btree,
          "projection native family mismatch");
  Require(HasEvidence(projection.evidence,
                      "donor_emulated.executable=false"),
          "non-executable projection evidence missing");
}

void MetadataProjectionRefusesUnsafeDeclarations() {
  auto descriptor = Declaration("sqlite", "automatic index");
  descriptor.descriptor_scan_fallback_requested = true;
  auto projection = idx::ProjectDonorEmulatedIndexMetadata(descriptor);
  Require(!projection.ok() &&
              projection.diagnostic.diagnostic_code ==
                  "INDEX.DONOR_EMULATED.DESCRIPTOR_SCAN_REFUSED",
          "metadata projection accepted descriptor-scan fallback");

  auto behavior = Declaration("sqlite", "automatic index");
  behavior.behavior_scan_fallback_requested = true;
  projection = idx::ProjectDonorEmulatedIndexMetadata(behavior);
  Require(!projection.ok() &&
              projection.diagnostic.diagnostic_code ==
                  "INDEX.DONOR_EMULATED.BEHAVIOR_SCAN_REFUSED",
          "metadata projection accepted behavior-scan fallback");

  auto authority = Declaration("sqlite", "automatic index");
  authority.authority_claims.donor_finality = true;
  projection = idx::ProjectDonorEmulatedIndexMetadata(authority);
  Require(!projection.ok() &&
              projection.diagnostic.diagnostic_code ==
                  "INDEX.DONOR_EMULATED.AUTHORITY_CLAIM_REFUSED" &&
              DetailContains(projection.diagnostic, "donor_finality"),
          "metadata projection accepted donor authority claim");
}

void AdmitsPostgresBtreeCompleteNativeTarget() {
  const auto result = idx::AdmitDonorEmulatedIndexMapping(
      Declaration("postgresql", "btree"), ValidProof());
  Require(result.ok(), "complete PostgreSQL B-tree native target refused");
  Require(result.native_family == idx::IndexFamily::btree,
          "PostgreSQL B-tree native family mismatch");
  Require(result.candidate_only, "PostgreSQL B-tree mapping was not candidate-only");
  Require(!result.donor_metadata_authority,
          "PostgreSQL B-tree donor metadata became authority");
  Require(!result.physical_truth_authority,
          "PostgreSQL B-tree donor mapping became physical truth");
  Require(result.exact_source_required &&
              result.mga_visibility_recheck_required &&
              result.security_recheck_required,
          "PostgreSQL B-tree recheck requirements missing");
  Require(HasEvidence(result.evidence, "donor_emulated.provider=postgresql"),
          "PostgreSQL provider evidence missing");
  Require(HasEvidence(result.evidence, "donor_emulated.profile=btree"),
          "PostgreSQL B-tree profile evidence missing");
  Require(HasEvidence(result.evidence, "donor_emulated.native_family=btree"),
          "PostgreSQL B-tree native family evidence missing");
  Require(HasEvidence(result.evidence,
                      "donor_emulated.native_family_benchmark_clean=true"),
          "PostgreSQL B-tree benchmark clean evidence missing");
  Require(HasEvidence(result.evidence,
                      "donor_emulated.metadata_authority=false"),
          "PostgreSQL B-tree metadata non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "donor_emulated.mga_visibility_recheck.required=true"),
          "PostgreSQL B-tree MGA recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "donor_emulated.security_recheck.required=true"),
          "PostgreSQL B-tree security recheck evidence missing");
}

void RefusesMissingNativeTarget() {
  const auto result = idx::AdmitDonorEmulatedIndexMapping(
      Declaration("postgresql", "rum"), ValidProof());
  Require(!result.ok() && result.fail_closed,
          "missing native target admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.DONOR_EMULATED.NATIVE_TARGET_MISSING",
          "wrong diagnostic for missing native target");
  Require(HasEvidence(result.evidence,
                      "donor_emulated.native_family=rum"),
          "missing target family evidence missing");
}

void RefusesUnsupportedProfile() {
  const auto result = idx::AdmitDonorEmulatedIndexMapping(
      Declaration("postgresql", "bloom custom"), ValidProof());
  Require(!result.ok() && result.fail_closed,
          "unsupported donor profile admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.DONOR_EMULATED.UNSUPPORTED_PROFILE",
          "wrong diagnostic for unsupported profile");
}

void RefusesFallbackScans() {
  auto descriptor = Declaration("sqlite", "automatic index");
  descriptor.descriptor_scan_fallback_requested = true;
  auto result = idx::AdmitDonorEmulatedIndexMapping(descriptor, ValidProof());
  Require(!result.ok() && result.fail_closed,
          "descriptor scan fallback admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.DONOR_EMULATED.DESCRIPTOR_SCAN_REFUSED",
          "wrong descriptor scan diagnostic");

  auto behavior = Declaration("sqlite", "automatic index");
  behavior.behavior_scan_fallback_requested = true;
  result = idx::AdmitDonorEmulatedIndexMapping(behavior, ValidProof());
  Require(!result.ok() && result.fail_closed,
          "behavior scan fallback admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.DONOR_EMULATED.BEHAVIOR_SCAN_REFUSED",
          "wrong behavior scan diagnostic");
}

void RefusesAuthorityClaims() {
  std::vector<std::pair<std::string, idx::DonorEmulatedAuthorityClaims>> cases;
  idx::DonorEmulatedAuthorityClaims claim;
  claim.parser_finality = true;
  cases.push_back({"parser_finality", claim});
  claim = {};
  claim.donor_finality = true;
  cases.push_back({"donor_finality", claim});
  claim = {};
  claim.provider_finality = true;
  cases.push_back({"provider_finality", claim});
  claim = {};
  claim.index_finality = true;
  cases.push_back({"index_finality", claim});
  claim = {};
  claim.security = true;
  cases.push_back({"security", claim});
  claim = {};
  claim.visibility = true;
  cases.push_back({"visibility", claim});
  claim = {};
  claim.transaction_finality = true;
  cases.push_back({"transaction_finality", claim});
  claim = {};
  claim.recovery = true;
  cases.push_back({"recovery", claim});
  claim = {};
  claim.log_finality = true;
  cases.push_back({"log_finality", claim});
  claim = {};
  claim.physical_truth = true;
  cases.push_back({"physical_truth", claim});

  for (const auto& item : cases) {
    auto declaration = Declaration("sqlite", "automatic index");
    declaration.authority_claims = item.second;
    const auto result =
        idx::AdmitDonorEmulatedIndexMapping(declaration, ValidProof());
    Require(!result.ok() && result.fail_closed,
            "authority claim admitted: " + item.first);
    Require(result.diagnostic.diagnostic_code ==
                "INDEX.DONOR_EMULATED.AUTHORITY_CLAIM_REFUSED",
            "wrong authority claim diagnostic: " + item.first);
    Require(DetailContains(result.diagnostic, item.first),
            "authority claim detail missing: " + item.first);
  }
}

void RefusesMissingProof() {
  const auto result = idx::AdmitDonorEmulatedIndexMapping(
      Declaration("sqlite", "automatic index"),
      idx::DonorEmulatedIndexAdmissionProof{});
  Require(!result.ok() && result.fail_closed, "missing proof admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.DONOR_EMULATED.MISSING_RECHECK_PROOF",
          "wrong missing proof diagnostic");
}

void NoDocPathRuntimeLeakage() {
  std::vector<std::string> values;
  const auto admitted = idx::AdmitDonorEmulatedIndexMapping(
      Declaration("sqlite", "automatic index"), ValidProof());
  values.insert(values.end(), admitted.evidence.begin(), admitted.evidence.end());
  AppendDiagnosticValues(admitted.diagnostic, &values);

  const auto refused = idx::AdmitDonorEmulatedIndexMapping(
      Declaration("postgresql", "btree"), ValidProof());
  values.insert(values.end(), refused.evidence.begin(), refused.evidence.end());
  AppendDiagnosticValues(refused.diagnostic, &values);

  const auto projected = idx::ProjectDonorEmulatedIndexMetadata(
      Declaration("postgresql", "btree"));
  values.insert(values.end(), projected.evidence.begin(),
                projected.evidence.end());
  AppendDiagnosticValues(projected.diagnostic, &values);

  RequireNoRuntimeDocLeak(values);
}

void DonorEmulatedFamilyRemainsNonPhysicalSurface() {
  const auto* state = idx::FindBuiltinIndexFamilyPhysicalCapabilityState(
      idx::IndexFamily::donor_emulated);
  Require(state != nullptr, "donor_emulated state missing");
  Require(!state->runtime_available,
          "donor_emulated advertised runtime availability");
  Require(!state->benchmark_clean,
          "donor_emulated advertised benchmark clean physical capability");
  Require(!state->physically_complete(),
          "donor_emulated became physically complete");
  Require(state->blocker ==
              idx::IndexFamilyPhysicalCapabilityBlocker::contract_only,
          "donor_emulated did not keep contract-only blocker");
  Require(state->blocker_diagnostic_code ==
              "INDEX.CAPABILITY.DONOR_EMULATED.CONTRACT_ONLY_NON_AUTHORITY_MAPPING",
          "donor_emulated blocker diagnostic drifted");
}

}  // namespace

int main() {
  AllowedMappingToCompleteNativeFamilies();
  MetadataProjectionIsNonExecutable();
  MetadataProjectionRefusesUnsafeDeclarations();
  AdmitsPostgresBtreeCompleteNativeTarget();
  RefusesMissingNativeTarget();
  RefusesUnsupportedProfile();
  RefusesFallbackScans();
  RefusesAuthorityClaims();
  RefusesMissingProof();
  NoDocPathRuntimeLeakage();
  DonorEmulatedFamilyRemainsNonPhysicalSurface();
  std::cout << "donor_emulated_index_mapping_gate=passed\n";
  return 0;
}
