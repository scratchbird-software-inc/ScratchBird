// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_validation_repair_tooling.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

namespace {

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "index_family_validation_repair_framework_gate: " << message
            << '\n';
  std::exit(1);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

struct UuidFactory {
  platform::TypedUuid Typed(platform::UuidKind kind, platform::u64 salt) const {
    const auto generated = uuid::GenerateEngineIdentityV7(kind, 1780000000000ULL + salt);
    Require(generated.ok(), "generated UUID creation failed");
    return generated.value;
  }
};

idx::IndexValidationRepairTarget Target(const UuidFactory& uuids,
                                        idx::IndexFamily family,
                                        platform::u64 salt) {
  idx::IndexValidationRepairTarget target;
  target.database_uuid = uuids.Typed(platform::UuidKind::database, salt + 1);
  target.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  target.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  target.generation_uuid = uuids.Typed(platform::UuidKind::object, salt + 4);
  target.physical_family = family;
  return target;
}

idx::IndexFamilyValidationRepairProof CompleteProof(
    const idx::IndexFamilyDescriptor& descriptor) {
  idx::IndexFamilyValidationRepairProof proof;
  proof.catalog_uuid_binding_proven = true;
  proof.exact_base_table_source_present = true;
  proof.physical_generation_present = true;
  proof.physical_generation_checksum_valid = true;
  proof.runtime_provider_attached = true;
  proof.runtime_epoch_current = true;
  proof.cold_start_source_present =
      descriptor.persistence ==
      idx::IndexPersistenceClass::memory_primary_persisted_cold_start;
  proof.cold_start_checksum_valid = proof.cold_start_source_present;
  proof.repair_output_validated = true;
  proof.rebuild_output_validated = true;
  proof.proof_token = "irc180-proof-token";
  return proof;
}

idx::IndexFamilyValidationRepairRequest Request(
    const UuidFactory& uuids,
    const idx::IndexFamilyDescriptor& descriptor,
    idx::IndexValidationRepairOperation operation,
    platform::u64 salt) {
  idx::IndexFamilyValidationRepairRequest request;
  request.operation = operation;
  request.family = descriptor.family;
  request.target = Target(uuids, descriptor.family, salt);
  request.proof = CompleteProof(descriptor);
  request.policy_allows_mutation =
      idx::IndexValidationRepairOperationMutates(operation);
  return request;
}

bool HasEvidence(const idx::IndexFamilyValidationRepairResult& result,
                 std::string_view key,
                 std::string_view value = {}) {
  for (const auto& evidence : result.support_evidence) {
    if (evidence.key == key && (value.empty() || evidence.value == value)) {
      return true;
    }
  }
  return false;
}

std::string EvidenceValue(const idx::IndexFamilyValidationRepairResult& result,
                          std::string_view key) {
  for (const auto& evidence : result.support_evidence) {
    if (evidence.key == key) {
      return evidence.value;
    }
  }
  Fail("missing evidence key " + std::string(key));
}

void RequireNoAuthority(const idx::IndexFamilyValidationRepairResult& result) {
  Require(result.observational_only, "result must remain observational");
  Require(!result.catalog_authority, "catalog authority was claimed");
  Require(!result.parser_authority, "parser authority was claimed");
  Require(!result.provider_authority, "provider authority was claimed");
  Require(!result.donor_authority, "donor authority was claimed");
  Require(!result.transaction_finality_authority,
          "transaction authority was claimed");
  Require(!result.visibility_authority, "visibility authority was claimed");
  Require(!result.security_authority, "security authority was claimed");
  Require(!result.recovery_authority, "recovery authority was claimed");
  Require(!result.storage_authority, "storage authority was claimed");
  Require(HasEvidence(result, "observational_only", "true"),
          "observational evidence missing");
}

void RequireNoRuntimeDependencyMarkers(
    const idx::IndexFamilyValidationRepairResult& result) {
  static const std::vector<std::string> markers = {
      "docs" "/execution-plans", "public_release_evidence", "docs" "/findings",
      "docs/reference", "implementation_packet"};
  for (const auto& evidence : result.support_evidence) {
    for (const auto& marker : markers) {
      Require(evidence.value.find(marker) == std::string::npos,
              "runtime evidence leaked path marker " + marker);
    }
  }
}

void ProveEveryBuiltinFamilyHasAPath() {
  const UuidFactory uuids;
  std::set<std::string> seen;
  platform::u64 salt = 1000;
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    const auto* state =
        idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    Require(state != nullptr,
            "missing physical capability state for " + descriptor.id);
    auto request = Request(uuids, descriptor,
                           idx::IndexValidationRepairOperation::validate,
                           salt += 100);
    const auto result = idx::ExecuteIndexFamilyValidationRepairOperation(request);
    Require(seen.insert(result.family_id).second,
            "duplicate family classification " + result.family_id);
    Require(result.family_id == descriptor.id,
            "family id mismatch for " + descriptor.id);
    Require(!result.diagnostic.diagnostic_code.empty(),
            "missing diagnostic for " + descriptor.id);
    Require(HasEvidence(result, "family_id", descriptor.id),
            "missing family evidence for " + descriptor.id);
    Require(HasEvidence(result, "path"), "missing path evidence");
    Require(HasEvidence(result, "open_state"), "missing open state evidence");
    Require(HasEvidence(result, "validation_state"),
            "missing validation state evidence");
    Require(HasEvidence(result, "blocker"), "missing blocker evidence");
    RequireNoAuthority(result);
    RequireNoRuntimeDependencyMarkers(result);
  }
  Require(seen.size() == idx::BuiltinIndexFamilyDescriptors().size(),
          "not every built-in family was classified");
}

void ProveCompleteFamiliesValidateAndRepairWithProof() {
  const UuidFactory uuids;
  platform::u64 salt = 10000;
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    const auto* state =
        idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    Require(state != nullptr,
            "missing state for complete-family proof " + descriptor.id);
    if (!state->physically_complete() || !state->runtime_available ||
        descriptor.persistence == idx::IndexPersistenceClass::donor_emulated ||
        descriptor.persistence == idx::IndexPersistenceClass::policy_blocked) {
      continue;
    }

    const auto validated = idx::ExecuteIndexFamilyValidationRepairOperation(
        Request(uuids, descriptor, idx::IndexValidationRepairOperation::validate,
                salt += 100));
    Require(validated.ok(), "complete family did not validate " + descriptor.id);
    Require(validated.planner_visible,
            "complete family validation did not clear planner visibility " +
                descriptor.id);
    Require(HasEvidence(validated, "mutation_applied", "false"),
            "validate mutated " + descriptor.id);

    const auto repaired = idx::ExecuteIndexFamilyValidationRepairOperation(
        Request(uuids, descriptor, idx::IndexValidationRepairOperation::repair,
                salt += 100));
    Require(repaired.ok(), "complete family did not repair " + descriptor.id);
    Require(repaired.mutation_applied,
            "repair did not record mutation " + descriptor.id);
    Require(repaired.planner_visible,
            "repair did not restore planner visibility " + descriptor.id);
    Require(HasEvidence(repaired, "repair_state", "repaired_and_validated"),
            "repair state missing " + descriptor.id);
    RequireNoAuthority(repaired);
  }
}

void ProveAcceptedNativeFamiliesAreComplete() {
  bool saw_complete = false;
  for (const auto& descriptor : idx::BuiltinIndexFamilyDescriptors()) {
    const auto* state =
        idx::FindBuiltinIndexFamilyPhysicalCapabilityState(descriptor.family);
    Require(state != nullptr,
            "missing state for accepted-family completion proof " +
                descriptor.id);
    if (descriptor.persistence == idx::IndexPersistenceClass::donor_emulated ||
        descriptor.persistence == idx::IndexPersistenceClass::policy_blocked) {
      continue;
    }
    saw_complete = true;
    Require(state->physically_complete(),
            "accepted native family is not physically complete " +
                descriptor.id);
    Require(state->runtime_available,
            "accepted native family is not runtime available " +
                descriptor.id);
    Require(state->benchmark_clean,
            "accepted native family is not benchmark clean " + descriptor.id);
    Require(state->blocker == idx::IndexFamilyPhysicalCapabilityBlocker::none,
            "accepted native family retained blocker " + descriptor.id);
    Require(state->blocker_diagnostic_code.empty(),
            "accepted native family retained blocker diagnostic " +
                descriptor.id);
  }
  Require(saw_complete, "accepted native completion coverage missing");
}

void ProveDonorAndPolicyStayNonPhysical() {
  const UuidFactory uuids;
  const auto* donor = idx::FindBuiltinIndexFamily(idx::IndexFamily::donor_emulated);
  const auto* policy = idx::FindBuiltinIndexFamily(idx::IndexFamily::policy_blocked);
  Require(donor != nullptr, "donor-emulated descriptor missing");
  Require(policy != nullptr, "policy-blocked descriptor missing");

  const auto donor_result = idx::ExecuteIndexFamilyValidationRepairOperation(
      Request(uuids, *donor, idx::IndexValidationRepairOperation::repair, 30000));
  Require(!donor_result.ok() && donor_result.fail_closed,
          "donor-emulated mapping became physical");
  Require(donor_result.open_state ==
              idx::IndexFamilyValidationRepairOpenState::non_physical_refused,
          "donor-emulated open state changed");
  Require(donor_result.diagnostic.diagnostic_code ==
              "IRC.INDEX_REPAIR.DONOR_EMULATED.NON_AUTHORITY_MAPPING",
          "donor-emulated diagnostic changed");
  RequireNoAuthority(donor_result);

  const auto policy_result = idx::ExecuteIndexFamilyValidationRepairOperation(
      Request(uuids, *policy, idx::IndexValidationRepairOperation::validate, 31000));
  Require(!policy_result.ok() && policy_result.fail_closed,
          "policy-blocked family admitted");
  Require(policy_result.open_state ==
              idx::IndexFamilyValidationRepairOpenState::non_physical_refused,
          "policy-blocked open state changed");
  Require(policy_result.diagnostic.diagnostic_code ==
              "INDEX.CAPABILITY.POLICY_BLOCKED.NOT_ACCEPTED_ALPHA",
          "policy-blocked diagnostic changed");
  RequireNoAuthority(policy_result);
}

void ProveReadOnlyAndMissingCatalogProofRefusals() {
  const UuidFactory uuids;
  const auto* graph = idx::FindBuiltinIndexFamily(idx::IndexFamily::graph);
  Require(graph != nullptr, "graph descriptor missing");

  auto read_only =
      Request(uuids, *graph, idx::IndexValidationRepairOperation::repair, 40000);
  read_only.read_only_database = true;
  const auto read_only_result =
      idx::ExecuteIndexFamilyValidationRepairOperation(read_only);
  Require(!read_only_result.ok(), "read-only repair was admitted");
  Require(read_only_result.diagnostic.diagnostic_code ==
              "IRC.INDEX_REPAIR.READ_ONLY_REFUSED",
          "read-only diagnostic changed");

  auto missing_catalog =
      Request(uuids, *graph, idx::IndexValidationRepairOperation::validate, 41000);
  missing_catalog.proof.catalog_uuid_binding_proven = false;
  const auto missing_catalog_result =
      idx::ExecuteIndexFamilyValidationRepairOperation(missing_catalog);
  Require(!missing_catalog_result.ok(), "missing catalog proof admitted");
  Require(missing_catalog_result.diagnostic.diagnostic_code ==
              "IRC.INDEX_REPAIR.CATALOG_UUID_PROOF_REQUIRED",
          "missing catalog proof diagnostic changed");

  auto mismatched_family =
      Request(uuids, *graph, idx::IndexValidationRepairOperation::validate, 42000);
  mismatched_family.target.physical_family = idx::IndexFamily::btree;
  const auto mismatch_result =
      idx::ExecuteIndexFamilyValidationRepairOperation(mismatched_family);
  Require(!mismatch_result.ok(), "family mismatch admitted");
  Require(mismatch_result.diagnostic.diagnostic_code ==
              "IRC.INDEX_REPAIR.IDENTITY_REFUSED",
          "family mismatch diagnostic changed");

  auto missing_uuid =
      Request(uuids, *graph, idx::IndexValidationRepairOperation::validate, 43000);
  missing_uuid.target.index_uuid = {};
  const auto missing_uuid_result =
      idx::ExecuteIndexFamilyValidationRepairOperation(missing_uuid);
  Require(!missing_uuid_result.ok(), "missing index UUID admitted");
  Require(missing_uuid_result.diagnostic.diagnostic_code ==
              "IRC.INDEX_REPAIR.IDENTITY_REFUSED",
          "missing UUID diagnostic changed");
}

void ProveRedactionKeepsStateAndHidesSensitiveDetail() {
  const UuidFactory uuids;
  const auto* in_memory = idx::FindBuiltinIndexFamily(idx::IndexFamily::in_memory);
  Require(in_memory != nullptr, "in-memory descriptor missing");
  auto request = Request(uuids, *in_memory,
                         idx::IndexValidationRepairOperation::validate, 50000);
  request.allow_sensitive_support_data = false;
  request.proof.proof_token = "/var/private/proof-token";
  request.proof.sensitive_detail = "/var/private/token=value";
  const auto result = idx::ExecuteIndexFamilyValidationRepairOperation(request);
  Require(result.ok(), "redaction validation did not admit in-memory family");
  Require(EvidenceValue(result, "proof_token_present") == "true",
          "proof token presence evidence missing");
  Require(EvidenceValue(result, "sensitive_detail") ==
              "<redacted-detail-present>",
          "sensitive evidence was not redacted");
  for (const auto& evidence : result.support_evidence) {
    Require(evidence.value.find("/var/private/proof-token") == std::string::npos,
            "raw proof token leaked into support evidence");
  }
  Require(HasEvidence(result, "family_id", "in_memory"),
          "redaction removed family state");
  Require(HasEvidence(result, "validation_state", "validated"),
          "redaction removed validation state");
  Require(HasEvidence(result, "open_state", "open_allowed"),
          "redaction removed open state");
}

}  // namespace

int main() {
  ProveEveryBuiltinFamilyHasAPath();
  ProveCompleteFamiliesValidateAndRepairWithProof();
  ProveAcceptedNativeFamiliesAreComplete();
  ProveDonorAndPolicyStayNonPhysical();
  ProveReadOnlyAndMissingCatalogProofRefusals();
  ProveRedactionKeepsStateAndHidesSensitiveDetail();
  return EXIT_SUCCESS;
}
