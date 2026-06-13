// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-031 focused validation for index access-method provider contracts.
#include "index_access_method.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace index = scratchbird::core::index;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

TypedUuid TestUuid(UuidKind kind, unsigned seed) {
  TypedUuid value;
  value.kind = kind;
  for (std::size_t i = 0; i < value.value.bytes.size(); ++i) {
    value.value.bytes[i] =
        static_cast<scratchbird::core::platform::byte>(
            (seed * 31u + i * 13u + 0x41u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

bool EvidenceHas(const index::IndexProviderAdmissionResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexProviderAccessMethodContract Contract(
    index::IndexFamily family,
    index::IndexRouteKind route) {
  index::IndexProviderAccessMethodContract contract;
  contract.family = family;
  contract.route = route;
  contract.provider.provider_id = "ceic031-provider";
  contract.provider.provider_name = "CEIC-031 Provider";
  contract.provider.provider_contract_version = "ceic031.v1";
  contract.provider.persistent_access_method = true;
  contract.provider.provider_backed = true;
  contract.route_boundary.route_capability_present = true;
  contract.route_boundary.provider_route_supported = true;
  contract.route_boundary.static_registry_complete_capability_seen = true;
  contract.route_boundary.external_cluster_provider_only = true;
  contract.route_boundary.route_specific_boundary_declared = true;
  contract.mutation_batch.batch_uuid = TestUuid(UuidKind::object, 10);
  contract.mutation_batch.operation_count = 4;
  contract.mutation_batch.batch_admission_requested = true;
  contract.mutation_batch.provider_batch_admission_supported = true;
  contract.mutation_batch.deterministic_batch_order = true;
  contract.mutation_batch.idempotent_replay_safe = true;
  contract.generation.generation_uuid = TestUuid(UuidKind::object, 11);
  contract.generation.generation_number = 7;
  contract.generation.provider_generation_id = "provider-generation-7";
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation = true;
  contract.recovery.recovery_context_id = "recovery-context-7";
  contract.recovery.recovery_reopen_supported = true;
  contract.recovery.crash_classification_supported = true;
  contract.recovery.corruption_classification_supported = true;
  contract.recovery.mga_recovery_evidence_only = true;
  contract.cleanup.oldest_active_transaction_id = 3;
  contract.cleanup.cleanup_generation_floor = 5;
  contract.cleanup.engine_mga_horizon_bound = true;
  contract.cleanup.provider_cleanup_supported = true;
  contract.validation_repair.validate_supported = true;
  contract.validation_repair.repair_supported = true;
  contract.validation_repair.rebuild_supported = true;
  contract.validation_repair.deterministic_diagnostics = true;
  contract.provider_evidence = {
      "provider_backed_access_method=true",
      "route_specific_boundary=true",
      "ceic031_contract_evidence_only=true"};
  return contract;
}

void RequireStatus(const index::IndexProviderAdmissionResult& result,
                   index::IndexProviderAdmissionStatus status,
                   std::string_view message) {
  Require(result.admission_status == status, message);
  Require(!result.ok(), "refused CEIC-031 provider result unexpectedly ok");
  Require(result.fail_closed, "refused CEIC-031 provider result did not fail closed");
  Require(!result.diagnostic.diagnostic_code.empty(),
          "refused CEIC-031 provider result lacks diagnostic code");
}

void PersistentFamiliesCanBeRepresented() {
  for (const auto& descriptor : index::BuiltinIndexFamilyDescriptors()) {
    if (descriptor.persistence != index::IndexPersistenceClass::persistent) {
      continue;
    }
    auto contract =
        Contract(descriptor.family, index::IndexRouteKind::sql_select);
    auto result = index::AdmitIndexProviderAccessMethod(contract);
    Require(result.ok(),
            "persistent provider contract was not representable");
    Require(result.admission_status ==
                index::IndexProviderAdmissionStatus::admitted,
            "persistent provider contract returned wrong status");
    Require(result.provider_contract_only,
            "CEIC-031 must remain provider-contract-only");
    Require(!result.durable_family_closure_claimed,
            "CEIC-031 must not claim durable family closure");
    Require(!result.enterprise_ready_claimed,
            "CEIC-031 must not claim enterprise readiness");
    Require(index::IndexProviderAuthorityBoundaryClear(
                contract.authority_boundary),
            "provider authority boundary must remain clear");
    Require(EvidenceHas(result,
                        "ceic_search_key=CEIC_031_INDEX_ACCESS_METHOD_PROVIDER_INTERFACE"),
            "CEIC-031 provider evidence anchor missing");
  }
}

void PersistentMutationRequiresEvidenceFields() {
  auto admitted =
      index::AdmitIndexProviderAccessMethod(Contract(
          index::IndexFamily::btree, index::IndexRouteKind::dml_insert));
  Require(admitted.ok(), "complete B-tree mutation provider contract refused");
  Require(EvidenceHas(admitted,
                      "route_requires_mutation_batch_admission=true"),
          "mutation route did not require mutation-batch admission evidence");
  Require(EvidenceHas(admitted, "route_requires_generation=true"),
          "mutation route did not require generation evidence");
  Require(EvidenceHas(admitted, "route_requires_recovery_context=true"),
          "mutation route did not require recovery evidence");
  Require(EvidenceHas(admitted, "route_requires_cleanup_horizon=true"),
      "mutation route did not require cleanup evidence");

  auto missing_batch =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::dml_insert);
  missing_batch.mutation_batch.batch_admission_requested = false;
  RequireStatus(
      index::AdmitIndexProviderAccessMethod(missing_batch),
      index::IndexProviderAdmissionStatus::mutation_batch_admission_required,
      "missing mutation-batch admission did not fail closed");

  auto missing_generation =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::dml_insert);
  missing_generation.generation.provider_generation_id.clear();
  RequireStatus(index::AdmitIndexProviderAccessMethod(missing_generation),
                index::IndexProviderAdmissionStatus::generation_handle_required,
                "missing generation handle did not fail closed");

  auto missing_recovery =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::dml_insert);
  missing_recovery.recovery.recovery_context_id.clear();
  RequireStatus(index::AdmitIndexProviderAccessMethod(missing_recovery),
                index::IndexProviderAdmissionStatus::recovery_context_required,
                "missing recovery context did not fail closed");

  auto missing_cleanup =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::dml_insert);
  missing_cleanup.cleanup.engine_mga_horizon_bound = false;
  RequireStatus(index::AdmitIndexProviderAccessMethod(missing_cleanup),
                index::IndexProviderAdmissionStatus::cleanup_horizon_required,
                "missing cleanup horizon did not fail closed");

  auto missing_validation =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::maintenance);
  missing_validation.validation_repair.deterministic_diagnostics = false;
  RequireStatus(index::AdmitIndexProviderAccessMethod(missing_validation),
                index::IndexProviderAdmissionStatus::validation_repair_required,
                "missing validation/repair support did not fail closed");
}

void FailClosedCases() {
  auto missing_provider =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::sql_select);
  missing_provider.provider.provider_backed = false;
  missing_provider.provider_evidence.clear();
  missing_provider.route_boundary.static_registry_complete_capability_seen =
      false;
  RequireStatus(index::AdmitIndexProviderAccessMethod(missing_provider),
                index::IndexProviderAdmissionStatus::missing_provider_evidence,
                "missing provider evidence did not fail closed");

  auto static_only =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::sql_select);
  static_only.provider.provider_backed = false;
  static_only.provider.provider_id.clear();
  static_only.provider_evidence.clear();
  static_only.route_boundary.static_registry_complete_capability_seen = true;
  RequireStatus(index::AdmitIndexProviderAccessMethod(static_only),
                index::IndexProviderAdmissionStatus::static_capability_only,
                "static CompleteCapability alone was not refused");

  RequireStatus(index::AdmitIndexProviderAccessMethod(
                    Contract(index::IndexFamily::reference_emulated,
                             index::IndexRouteKind::sql_select)),
                index::IndexProviderAdmissionStatus::reference_emulated_non_runtime,
                "reference-emulated family was not blocked");

  RequireStatus(index::AdmitIndexProviderAccessMethod(
                    Contract(index::IndexFamily::policy_blocked,
                             index::IndexRouteKind::sql_select)),
                index::IndexProviderAdmissionStatus::policy_blocked_non_runtime,
                "policy-blocked family was not blocked");

  auto missing_route =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::sql_select);
  missing_route.route_boundary.route_capability_present = false;
  RequireStatus(index::AdmitIndexProviderAccessMethod(missing_route),
                index::IndexProviderAdmissionStatus::route_capability_required,
                "missing route capability did not fail closed");

  const auto supported_hash_insert =
      index::AdmitIndexProviderAccessMethod(Contract(
          index::IndexFamily::hash, index::IndexRouteKind::dml_insert));
  Require(supported_hash_insert.ok(),
          "hash DML insert provider contract should be admitted for complete index support");
  Require(EvidenceHas(supported_hash_insert,
                      "route_requires_mutation_batch_admission=true"),
          "hash DML insert must require mutation-batch admission evidence");

  auto unsupported_route =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::nosql_vector);
  RequireStatus(index::AdmitIndexProviderAccessMethod(unsupported_route),
                index::IndexProviderAdmissionStatus::route_not_supported,
                "unsupported route/family pair did not fail closed");

  auto authority_claim =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::sql_select);
  authority_claim.authority_boundary.transaction_finality_authority = true;
  RequireStatus(index::AdmitIndexProviderAccessMethod(authority_claim),
                index::IndexProviderAdmissionStatus::authority_boundary_refused,
                "provider authority claim did not fail closed");

  auto cluster =
      Contract(index::IndexFamily::btree, index::IndexRouteKind::sql_select);
  cluster.route_boundary.cluster_path_requested = true;
  RequireStatus(index::AdmitIndexProviderAccessMethod(cluster),
                index::IndexProviderAdmissionStatus::cluster_external_provider_only,
                "cluster route did not fail external-provider-only");
}

}  // namespace

int main() {
  PersistentFamiliesCanBeRepresented();
  PersistentMutationRequiresEvidenceFields();
  FailClosedCases();
  std::cout << "ceic_031_index_access_method_provider_gate=pass\n";
  return EXIT_SUCCESS;
}
