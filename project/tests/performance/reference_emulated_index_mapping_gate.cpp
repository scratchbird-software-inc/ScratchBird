// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "reference_emulated_index_mapping.hpp"

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
    std::cerr << "reference_emulated_index_mapping_gate: " << message << '\n';
    std::exit(1);
  }
}

idx::ReferenceEmulatedIndexAdmissionProof ValidProof() {
  idx::ReferenceEmulatedIndexAdmissionProof proof;
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

idx::ReferenceEmulatedIndexDeclaration Declaration(std::string provider,
                                               std::string profile,
                                               std::string family = {}) {
  idx::ReferenceEmulatedIndexDeclaration declaration;
  declaration.reference_provider = std::move(provider);
  declaration.reference_profile = std::move(profile);
  declaration.reference_family = std::move(family);
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
    idx::ReferenceEmulatedIndexDeclaration declaration;
    idx::IndexFamily family;
    std::string provider_evidence;
    std::string profile_evidence;
    std::string native_evidence;
  };
  std::vector<Case> cases = {
      {Declaration(" SQLite3 ", "Automatic-Index"), idx::IndexFamily::temporary_work,
       "reference_emulated.provider=sqlite",
       "reference_emulated.profile=automatic_transient_index",
       "reference_emulated.native_family=temporary_work"},
      {Declaration("scratchbird", "in-memory lookup"), idx::IndexFamily::in_memory,
       "reference_emulated.provider=scratchbird",
       "reference_emulated.profile=in_memory_lookup",
       "reference_emulated.native_family=in_memory"},
      {Declaration("scratchbird", "graph adjacency"), idx::IndexFamily::graph,
       "reference_emulated.provider=scratchbird",
       "reference_emulated.profile=graph_adjacency",
       "reference_emulated.native_family=graph"}};

  for (const auto& item : cases) {
    RequireNativeComplete(item.family);
    const auto result = idx::AdmitReferenceEmulatedIndexMapping(item.declaration,
                                                           proof);
    Require(result.ok(), "complete native reference mapping was refused");
    Require(result.native_family == item.family, "native family mismatch");
    Require(result.candidate_only, "result was not candidate-only");
    Require(!result.reference_metadata_authority,
            "reference metadata became authority");
    Require(!result.physical_truth_authority,
            "reference mapping became physical truth");
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
                        "reference_emulated.native_family_benchmark_clean=true"),
            "benchmark clean native evidence missing");
    Require(HasEvidence(result.evidence,
                        "reference_emulated.metadata_authority=false"),
            "metadata non-authority evidence missing");
    Require(HasEvidence(result.evidence,
                        "reference_emulated.mga_visibility_recheck.required=true"),
            "MGA recheck evidence missing");
    Require(HasEvidence(result.evidence,
                        "reference_emulated.security_recheck.required=true"),
            "security recheck evidence missing");
  }
}

void MetadataProjectionIsNonExecutable() {
  const auto projection = idx::ProjectReferenceEmulatedIndexMetadata(
      Declaration("Postgres", "B-Tree"));
  Require(projection.ok(), "metadata projection refused recognized profile");
  Require(projection.management_visible, "management visibility missing");
  Require(projection.catalog_identity_only, "catalog identity surface missing");
  Require(!projection.executable, "projection became executable");
  Require(projection.native_admission_required,
          "projection did not require native admission");
  Require(!projection.reference_metadata_authority,
          "projection made reference metadata authoritative");
  Require(projection.native_family == idx::IndexFamily::btree,
          "projection native family mismatch");
  Require(HasEvidence(projection.evidence,
                      "reference_emulated.executable=false"),
          "non-executable projection evidence missing");
}

void MetadataProjectionRefusesUnsafeDeclarations() {
  auto descriptor = Declaration("sqlite", "automatic index");
  descriptor.descriptor_scan_fallback_requested = true;
  auto projection = idx::ProjectReferenceEmulatedIndexMetadata(descriptor);
  Require(!projection.ok() &&
              projection.diagnostic.diagnostic_code ==
                  "INDEX.REFERENCE_EMULATED.DESCRIPTOR_SCAN_REFUSED",
          "metadata projection accepted descriptor-scan fallback");

  auto behavior = Declaration("sqlite", "automatic index");
  behavior.behavior_scan_fallback_requested = true;
  projection = idx::ProjectReferenceEmulatedIndexMetadata(behavior);
  Require(!projection.ok() &&
              projection.diagnostic.diagnostic_code ==
                  "INDEX.REFERENCE_EMULATED.BEHAVIOR_SCAN_REFUSED",
          "metadata projection accepted behavior-scan fallback");

  auto authority = Declaration("sqlite", "automatic index");
  authority.authority_claims.reference_finality = true;
  projection = idx::ProjectReferenceEmulatedIndexMetadata(authority);
  Require(!projection.ok() &&
              projection.diagnostic.diagnostic_code ==
                  "INDEX.REFERENCE_EMULATED.AUTHORITY_CLAIM_REFUSED" &&
              DetailContains(projection.diagnostic, "reference_finality"),
          "metadata projection accepted reference authority claim");
}

void AdmitsPostgresBtreeCompleteNativeTarget() {
  const auto result = idx::AdmitReferenceEmulatedIndexMapping(
      Declaration("postgresql", "btree"), ValidProof());
  Require(result.ok(), "complete PostgreSQL B-tree native target refused");
  Require(result.native_family == idx::IndexFamily::btree,
          "PostgreSQL B-tree native family mismatch");
  Require(result.candidate_only, "PostgreSQL B-tree mapping was not candidate-only");
  Require(!result.reference_metadata_authority,
          "PostgreSQL B-tree reference metadata became authority");
  Require(!result.physical_truth_authority,
          "PostgreSQL B-tree reference mapping became physical truth");
  Require(result.exact_source_required &&
              result.mga_visibility_recheck_required &&
              result.security_recheck_required,
          "PostgreSQL B-tree recheck requirements missing");
  Require(HasEvidence(result.evidence, "reference_emulated.provider=postgresql"),
          "PostgreSQL provider evidence missing");
  Require(HasEvidence(result.evidence, "reference_emulated.profile=btree"),
          "PostgreSQL B-tree profile evidence missing");
  Require(HasEvidence(result.evidence, "reference_emulated.native_family=btree"),
          "PostgreSQL B-tree native family evidence missing");
  Require(HasEvidence(result.evidence,
                      "reference_emulated.native_family_benchmark_clean=true"),
          "PostgreSQL B-tree benchmark clean evidence missing");
  Require(HasEvidence(result.evidence,
                      "reference_emulated.metadata_authority=false"),
          "PostgreSQL B-tree metadata non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "reference_emulated.mga_visibility_recheck.required=true"),
          "PostgreSQL B-tree MGA recheck evidence missing");
  Require(HasEvidence(result.evidence,
                      "reference_emulated.security_recheck.required=true"),
          "PostgreSQL B-tree security recheck evidence missing");
}

void RefusesMissingNativeTarget() {
  const auto result = idx::AdmitReferenceEmulatedIndexMapping(
      Declaration("postgresql", "rum"), ValidProof());
  Require(!result.ok() && result.fail_closed,
          "missing native target admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.REFERENCE_EMULATED.NATIVE_TARGET_MISSING",
          "wrong diagnostic for missing native target");
  Require(HasEvidence(result.evidence,
                      "reference_emulated.native_family=rum"),
          "missing target family evidence missing");
}

void RefusesUnsupportedProfile() {
  const auto result = idx::AdmitReferenceEmulatedIndexMapping(
      Declaration("postgresql", "bloom custom"), ValidProof());
  Require(!result.ok() && result.fail_closed,
          "unsupported reference profile admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.REFERENCE_EMULATED.UNSUPPORTED_PROFILE",
          "wrong diagnostic for unsupported profile");
}

void RefusesFallbackScans() {
  auto descriptor = Declaration("sqlite", "automatic index");
  descriptor.descriptor_scan_fallback_requested = true;
  auto result = idx::AdmitReferenceEmulatedIndexMapping(descriptor, ValidProof());
  Require(!result.ok() && result.fail_closed,
          "descriptor scan fallback admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.REFERENCE_EMULATED.DESCRIPTOR_SCAN_REFUSED",
          "wrong descriptor scan diagnostic");

  auto behavior = Declaration("sqlite", "automatic index");
  behavior.behavior_scan_fallback_requested = true;
  result = idx::AdmitReferenceEmulatedIndexMapping(behavior, ValidProof());
  Require(!result.ok() && result.fail_closed,
          "behavior scan fallback admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.REFERENCE_EMULATED.BEHAVIOR_SCAN_REFUSED",
          "wrong behavior scan diagnostic");
}

void RefusesAuthorityClaims() {
  std::vector<std::pair<std::string, idx::ReferenceEmulatedAuthorityClaims>> cases;
  idx::ReferenceEmulatedAuthorityClaims claim;
  claim.parser_finality = true;
  cases.push_back({"parser_finality", claim});
  claim = {};
  claim.reference_finality = true;
  cases.push_back({"reference_finality", claim});
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
        idx::AdmitReferenceEmulatedIndexMapping(declaration, ValidProof());
    Require(!result.ok() && result.fail_closed,
            "authority claim admitted: " + item.first);
    Require(result.diagnostic.diagnostic_code ==
                "INDEX.REFERENCE_EMULATED.AUTHORITY_CLAIM_REFUSED",
            "wrong authority claim diagnostic: " + item.first);
    Require(DetailContains(result.diagnostic, item.first),
            "authority claim detail missing: " + item.first);
  }
}

void RefusesMissingProof() {
  const auto result = idx::AdmitReferenceEmulatedIndexMapping(
      Declaration("sqlite", "automatic index"),
      idx::ReferenceEmulatedIndexAdmissionProof{});
  Require(!result.ok() && result.fail_closed, "missing proof admitted");
  Require(result.diagnostic.diagnostic_code ==
              "INDEX.REFERENCE_EMULATED.MISSING_RECHECK_PROOF",
          "wrong missing proof diagnostic");
}

void NoDocPathRuntimeLeakage() {
  std::vector<std::string> values;
  const auto admitted = idx::AdmitReferenceEmulatedIndexMapping(
      Declaration("sqlite", "automatic index"), ValidProof());
  values.insert(values.end(), admitted.evidence.begin(), admitted.evidence.end());
  AppendDiagnosticValues(admitted.diagnostic, &values);

  const auto refused = idx::AdmitReferenceEmulatedIndexMapping(
      Declaration("postgresql", "btree"), ValidProof());
  values.insert(values.end(), refused.evidence.begin(), refused.evidence.end());
  AppendDiagnosticValues(refused.diagnostic, &values);

  const auto projected = idx::ProjectReferenceEmulatedIndexMetadata(
      Declaration("postgresql", "btree"));
  values.insert(values.end(), projected.evidence.begin(),
                projected.evidence.end());
  AppendDiagnosticValues(projected.diagnostic, &values);

  RequireNoRuntimeDocLeak(values);
}

void ReferenceEmulatedFamilyRemainsNonPhysicalSurface() {
  const auto* state = idx::FindBuiltinIndexFamilyPhysicalCapabilityState(
      idx::IndexFamily::reference_emulated);
  Require(state != nullptr, "reference_emulated state missing");
  Require(!state->runtime_available,
          "reference_emulated advertised runtime availability");
  Require(!state->benchmark_clean,
          "reference_emulated advertised benchmark clean physical capability");
  Require(!state->physically_complete(),
          "reference_emulated became physically complete");
  Require(state->blocker ==
              idx::IndexFamilyPhysicalCapabilityBlocker::contract_only,
          "reference_emulated did not keep contract-only blocker");
  Require(state->blocker_diagnostic_code ==
              "INDEX.CAPABILITY.REFERENCE_EMULATED.CONTRACT_ONLY_NON_AUTHORITY_MAPPING",
          "reference_emulated blocker diagnostic drifted");
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
  ReferenceEmulatedFamilyRemainsNonPhysicalSurface();
  std::cout << "reference_emulated_index_mapping_gate=passed\n";
  return 0;
}
