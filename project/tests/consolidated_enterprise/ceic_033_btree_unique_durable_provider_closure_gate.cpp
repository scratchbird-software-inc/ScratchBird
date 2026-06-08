// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-033 focused validation for B-tree and unique durable provider closure.
#include "btree_unique_durable_provider_closure.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

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
            (seed * 37u + i * 19u + 0x67u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

bool EvidenceHas(const index::BtreeUniqueDurableProviderClosureResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexProviderAccessMethodContract ProviderContract(
    index::IndexFamily family) {
  index::IndexProviderAccessMethodContract contract;
  contract.family = family;
  contract.route = index::IndexRouteKind::bulk_build;
  contract.provider.provider_id = "ceic033-btree-provider";
  contract.provider.provider_name = "CEIC-033 B-tree Provider";
  contract.provider.provider_contract_version = "ceic033.provider.v1";
  contract.provider.persistent_access_method = true;
  contract.provider.provider_backed = true;
  contract.route_boundary.route_capability_present = true;
  contract.route_boundary.provider_route_supported = true;
  contract.route_boundary.static_registry_complete_capability_seen = true;
  contract.route_boundary.external_cluster_provider_only = true;
  contract.route_boundary.route_specific_boundary_declared = true;
  contract.mutation_batch.batch_uuid = TestUuid(UuidKind::object, 10);
  contract.mutation_batch.operation_count = 8;
  contract.mutation_batch.batch_admission_requested = true;
  contract.mutation_batch.provider_batch_admission_supported = true;
  contract.mutation_batch.deterministic_batch_order = true;
  contract.mutation_batch.idempotent_replay_safe = true;
  contract.generation.generation_uuid = TestUuid(UuidKind::object, 11);
  contract.generation.generation_number = 33;
  contract.generation.provider_generation_id = "btree-provider-generation-33";
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation = true;
  contract.recovery.recovery_context_id = "btree-recovery-context-33";
  contract.recovery.recovery_reopen_supported = true;
  contract.recovery.crash_classification_supported = true;
  contract.recovery.corruption_classification_supported = true;
  contract.recovery.mga_recovery_evidence_only = true;
  contract.cleanup.oldest_active_transaction_id = 17;
  contract.cleanup.cleanup_generation_floor = 31;
  contract.cleanup.engine_mga_horizon_bound = true;
  contract.cleanup.provider_cleanup_supported = true;
  contract.validation_repair.validate_supported = true;
  contract.validation_repair.repair_supported = true;
  contract.validation_repair.rebuild_supported = true;
  contract.validation_repair.deterministic_diagnostics = true;
  contract.provider_evidence = {
      "ceic031_admitted_provider=true",
      "btree_page_provider_contract=true",
      "ceic033_consumes_ceic031=true"};
  return contract;
}

index::IndexMGARecoveryContract MGAContract(index::IndexFamily family) {
  index::IndexMGARecoveryContract contract;
  contract.identity.family = family;
  contract.identity.route = index::IndexRouteKind::bulk_build;
  contract.identity.provider_id = "ceic033-btree-provider";
  contract.identity.provider_contract_version = "ceic033.provider.v1";
  contract.identity.persistent_provider = true;
  contract.identity.external_cluster_provider_only = true;
  contract.mga_authority.inventory_present = true;
  contract.mga_authority.inventory_authoritative = true;
  contract.mga_authority.inventory_durable = true;
  contract.mga_authority.snapshot_present = true;
  contract.mga_authority.snapshot_authoritative = true;
  contract.mga_authority.cleanup_horizon_present = true;
  contract.mga_authority.cleanup_horizon_authoritative = true;
  contract.mga_authority.cleanup_horizon_engine_bound = true;
  contract.mga_authority.inventory_epoch = 33;
  contract.mga_authority.snapshot_epoch = 33;
  contract.mga_authority.cleanup_horizon_epoch = 33;
  contract.mga_authority.required_engine_evidence_epoch = 33;
  contract.mga_authority.inventory_evidence_id = "mga-inventory-33";
  contract.mga_authority.snapshot_evidence_id = "mga-snapshot-33";
  contract.mga_authority.cleanup_horizon_evidence_id = "mga-horizon-33";
  contract.generation.index_uuid = TestUuid(UuidKind::object, 21);
  contract.generation.generation_uuid = TestUuid(UuidKind::object, 22);
  contract.generation.generation_number = 33;
  contract.generation.cow_generation_number = 33;
  contract.generation.provider_generation_id = "btree-provider-generation-33";
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation_identity_bound = true;
  contract.generation.publish_state =
      index::IndexGenerationPublishState::published;
  contract.recovery.crash_classification =
      index::IndexCrashRecoveryClassification::crash_after_generation_publish;
  contract.recovery.corruption_classification =
      index::IndexCorruptionClassification::checksum_mismatch;
  contract.recovery.recovery_evidence_id = "btree-recovery-33";
  contract.recovery.durable_recovery_evidence = true;
  contract.recovery.replay_idempotent = true;
  contract.recovery.provider_evidence_only = true;
  contract.provider_evidence = {
      "ceic032_admitted_mga_contract=true",
      "btree_mga_recovery_contract=true",
      "ceic033_consumes_ceic032=true"};
  return contract;
}

index::BtreeUniqueDurableProviderClosureRequest Request(
    index::IndexFamily family) {
  index::BtreeUniqueDurableProviderClosureRequest request;
  request.family = family;
  request.route = index::IndexRouteKind::bulk_build;
  request.provider_id = "ceic033-btree-provider";
  request.provider_admission =
      index::AdmitIndexProviderAccessMethod(ProviderContract(family));
  request.mga_recovery_contract =
      index::AdmitIndexMGARecoveryContract(MGAContract(family));
  request.generation_root.index_uuid = TestUuid(UuidKind::object, 21);
  request.generation_root.root_page_uuid = TestUuid(UuidKind::page, 23);
  request.generation_root.generation_uuid = TestUuid(UuidKind::object, 22);
  request.generation_root.root_page_number = 1001;
  request.generation_root.generation_number = 33;
  request.generation_root.cow_generation_number = 33;
  request.generation_root.provider_generation_id =
      "btree-provider-generation-33";
  request.generation_root.cow_generation_publish_proven = true;
  request.generation_root.root_identity_bound = true;
  request.generation_root.root_reopen_identity_stable = true;
  request.generation_root.provider_generation_matches_mga_contract = true;
  request.generation_root.publish_after_durable_mga_evidence = true;
  request.split_merge_scan.split_capability_present = true;
  request.split_merge_scan.split_proof_present = true;
  request.split_merge_scan.merge_capability_present = true;
  request.split_merge_scan.merge_proof_present = true;
  request.split_merge_scan.ordered_scan_capability_present = true;
  request.split_merge_scan.ordered_scan_proof_present = true;
  request.split_merge_scan.point_lookup_capability_present = true;
  request.split_merge_scan.deterministic_page_format = true;
  request.split_merge_scan.page_integrity_proof_present = true;
  request.split_merge_scan.structural_evidence_id =
      "btree-split-merge-scan-33";
  request.unique_duplicate.duplicate_preflight_proven = true;
  request.unique_duplicate.reservation_proof_present = true;
  request.unique_duplicate.reservation_engine_transaction_bound = true;
  request.unique_duplicate.duplicate_recheck_engine_bound = true;
  request.unique_duplicate.ceic_034_unique_finality_protocol_pending = true;
  request.unique_duplicate.reservation_evidence_id =
      "unique-duplicate-reservation-33";
  request.cleanup.oldest_active_transaction_id = 17;
  request.cleanup.cleanup_generation_floor = 31;
  request.cleanup.engine_mga_horizon_bound = true;
  request.cleanup.cleanup_uses_engine_horizon = true;
  request.cleanup.provider_cleanup_evidence_only = true;
  request.cleanup.cleanup_evidence_id = "btree-cleanup-horizon-33";
  request.crash_corruption.crash_classification =
      index::IndexCrashRecoveryClassification::crash_after_generation_publish;
  request.crash_corruption.corruption_classification =
      index::IndexCorruptionClassification::checksum_mismatch;
  request.crash_corruption.crash_reopen_classification_present = true;
  request.crash_corruption.corruption_classification_present = true;
  request.crash_corruption.durable_provider_evidence_only = true;
  request.crash_corruption.classification_evidence_id =
      "btree-crash-corruption-33";
  request.repair_rebuild.validate_before_repair = true;
  request.repair_rebuild.repair_supported = true;
  request.repair_rebuild.rebuild_supported = true;
  request.repair_rebuild.deterministic_recommendation_present = true;
  request.repair_rebuild.recommendation_matches_mga_recovery_contract = true;
  request.repair_rebuild.recommendation_evidence_id =
      "btree-repair-rebuild-33";
  request.concurrency.generation_publish_fence_hook = true;
  request.concurrency.root_publish_observer_hook = true;
  request.concurrency.split_merge_latch_boundary_hook = true;
  request.concurrency.concurrent_scan_mutation_boundary_hook = true;
  request.concurrency.evidence_only_not_transaction_finality = true;
  request.concurrency.concurrency_evidence_id = "btree-concurrency-33";
  request.evidence = {
      "btree_durable_provider_evidence=true",
      "ceic034_unique_protocol_pending=true",
      "ceic041_crash_matrix_pending=true"};
  return request;
}

void RequireStatus(
    const index::BtreeUniqueDurableProviderClosureResult& result,
    index::BtreeUniqueDurableProviderClosureStatus status,
    std::string_view message) {
  Require(result.closure_status == status, message);
  Require(!result.ok(), "refused CEIC-033 result unexpectedly ok");
  Require(result.fail_closed, "refused CEIC-033 result did not fail closed");
  Require(!result.diagnostic.diagnostic_code.empty(),
          "refused CEIC-033 result lacks diagnostic code");
  Require(!result.enterprise_ready_claimed,
          "refused CEIC-033 result must not claim enterprise readiness");
  Require(!result.all_index_readiness_claimed,
          "refused CEIC-033 result must not claim all-index readiness");
}

void ValidBtreeClosureAdmitsEvidenceOnly() {
  auto result =
      index::AdmitBtreeUniqueDurableProviderClosure(
          Request(index::IndexFamily::btree));
  Require(result.ok(), "valid B-tree CEIC-033 closure was refused");
  Require(result.closure_status ==
              index::BtreeUniqueDurableProviderClosureStatus::
                  admitted_durable_provider_evidence,
          "valid B-tree CEIC-033 closure returned wrong status");
  Require(result.durable_provider_evidence,
          "valid B-tree closure must admit durable provider evidence");
  Require(result.btree_unique_provider_closure_claimed,
          "valid B-tree closure must claim only B-tree/unique provider closure");
  Require(!result.enterprise_ready_claimed,
          "CEIC-033 must not claim enterprise readiness");
  Require(!result.all_index_readiness_claimed,
          "CEIC-033 must not claim all-index readiness");
  Require(result.ceic_034_unique_protocol_pending,
          "CEIC-033 must not claim CEIC-034 protocol closure");
  Require(result.ceic_041_crash_matrix_pending,
          "CEIC-041 must remain pending");
  Require(result.recommendation.validate,
          "CEIC-033 should carry CEIC-032 validation recommendation");
  Require(result.recommendation.rebuild,
          "CEIC-033 should carry CEIC-032 rebuild recommendation");
  Require(EvidenceHas(
              result,
              "ceic_search_key=CEIC_033_BTREE_UNIQUE_DURABLE_PROVIDER_CLOSURE"),
          "CEIC-033 evidence anchor missing");
  Require(EvidenceHas(result, "result_enterprise_ready_claimed=false"),
          "CEIC-033 result did not record no enterprise-ready claim");
}

void UniqueBtreeRequiresUniqueProof() {
  auto valid =
      index::AdmitBtreeUniqueDurableProviderClosure(
          Request(index::IndexFamily::unique_btree));
  Require(valid.ok(), "valid unique_btree CEIC-033 closure was refused");

  auto missing = Request(index::IndexFamily::unique_btree);
  missing.unique_duplicate.reservation_proof_present = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing),
      index::BtreeUniqueDurableProviderClosureStatus::
          unique_duplicate_proof_required,
      "unique_btree missing unique proof did not fail closed");

  auto plain_btree_without_unique = Request(index::IndexFamily::btree);
  plain_btree_without_unique.unique_duplicate.reservation_proof_present = false;
  Require(index::AdmitBtreeUniqueDurableProviderClosure(
              plain_btree_without_unique)
              .ok(),
          "plain B-tree must not require unique duplicate proof");
}

void MissingProviderAndMGAFailClosed() {
  auto missing_provider = Request(index::IndexFamily::btree);
  missing_provider.provider_admission = {};
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_provider),
      index::BtreeUniqueDurableProviderClosureStatus::
          provider_admission_not_admitted,
      "missing CEIC-031 provider admission did not fail closed");

  auto missing_mga = Request(index::IndexFamily::btree);
  missing_mga.mga_recovery_contract = {};
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_mga),
      index::BtreeUniqueDurableProviderClosureStatus::
          mga_recovery_contract_not_admitted,
      "missing CEIC-032 MGA contract did not fail closed");

  auto provider_mismatch = Request(index::IndexFamily::btree);
  provider_mismatch.provider_id = "different-provider";
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(provider_mismatch),
      index::BtreeUniqueDurableProviderClosureStatus::
          provider_mga_identity_mismatch,
      "CEIC-031 provider identity mismatch did not fail closed");

  auto generation_mismatch = Request(index::IndexFamily::btree);
  generation_mismatch.generation_root.generation_number = 34;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(generation_mismatch),
      index::BtreeUniqueDurableProviderClosureStatus::
          provider_mga_identity_mismatch,
      "CEIC-032 generation identity mismatch did not fail closed");

  auto no_durable_evidence = Request(index::IndexFamily::btree);
  no_durable_evidence.durable_provider_evidence_claimed = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(no_durable_evidence),
      index::BtreeUniqueDurableProviderClosureStatus::
          durable_provider_evidence_missing,
      "missing durable provider evidence claim did not fail closed");
}

void RequiredProofFailuresFailClosed() {
  auto missing_root = Request(index::IndexFamily::btree);
  missing_root.generation_root.root_identity_bound = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_root),
      index::BtreeUniqueDurableProviderClosureStatus::
          missing_generation_root_identity,
      "missing root identity did not fail closed");

  auto missing_generation = Request(index::IndexFamily::btree);
  missing_generation.generation_root.generation_number = 0;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_generation),
      index::BtreeUniqueDurableProviderClosureStatus::
          missing_generation_root_identity,
      "missing generation number did not fail closed");

  auto missing_split = Request(index::IndexFamily::btree);
  missing_split.split_merge_scan.split_proof_present = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_split),
      index::BtreeUniqueDurableProviderClosureStatus::
          missing_split_merge_scan_proof,
      "missing split proof did not fail closed");

  auto missing_merge = Request(index::IndexFamily::btree);
  missing_merge.split_merge_scan.merge_capability_present = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_merge),
      index::BtreeUniqueDurableProviderClosureStatus::
          missing_split_merge_scan_proof,
      "missing merge proof did not fail closed");

  auto missing_scan = Request(index::IndexFamily::btree);
  missing_scan.split_merge_scan.ordered_scan_proof_present = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_scan),
      index::BtreeUniqueDurableProviderClosureStatus::
          missing_split_merge_scan_proof,
      "missing scan proof did not fail closed");

  auto missing_cleanup = Request(index::IndexFamily::btree);
  missing_cleanup.cleanup.engine_mga_horizon_bound = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_cleanup),
      index::BtreeUniqueDurableProviderClosureStatus::
          cleanup_horizon_not_engine_bound,
      "missing cleanup engine binding did not fail closed");

  auto missing_crash = Request(index::IndexFamily::btree);
  missing_crash.crash_corruption.crash_classification =
      index::IndexCrashRecoveryClassification::unknown;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_crash),
      index::BtreeUniqueDurableProviderClosureStatus::
          crash_corruption_classification_absent,
      "missing crash classification did not fail closed");

  auto missing_corruption = Request(index::IndexFamily::btree);
  missing_corruption.crash_corruption.corruption_classification =
      index::IndexCorruptionClassification::unknown;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(missing_corruption),
      index::BtreeUniqueDurableProviderClosureStatus::
          crash_corruption_classification_absent,
      "missing corruption classification did not fail closed");
}

void AuthorityParticipationAndSuccessorScopeFailClosed() {
  auto donor = Request(index::IndexFamily::btree);
  donor.donor_local_participation = true;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(donor),
      index::BtreeUniqueDurableProviderClosureStatus::
          donor_policy_cluster_participation,
      "donor local participation did not fail closed");

  auto policy = Request(index::IndexFamily::btree);
  policy.policy_local_participation = true;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(policy),
      index::BtreeUniqueDurableProviderClosureStatus::
          donor_policy_cluster_participation,
      "policy local participation did not fail closed");

  auto cluster = Request(index::IndexFamily::btree);
  cluster.cluster_local_participation = true;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(cluster),
      index::BtreeUniqueDurableProviderClosureStatus::
          donor_policy_cluster_participation,
      "cluster local participation did not fail closed");

  auto authority = Request(index::IndexFamily::btree);
  authority.authority_boundary.wal_authority = true;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(authority),
      index::BtreeUniqueDurableProviderClosureStatus::
          forbidden_authority_claim,
      "forbidden WAL authority claim did not fail closed");

  auto finality = Request(index::IndexFamily::btree);
  finality.authority_boundary.transaction_finality_authority = true;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(finality),
      index::BtreeUniqueDurableProviderClosureStatus::
          forbidden_authority_claim,
      "forbidden transaction finality claim did not fail closed");

  auto enterprise = Request(index::IndexFamily::btree);
  enterprise.enterprise_ready_claimed = true;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(enterprise),
      index::BtreeUniqueDurableProviderClosureStatus::
          enterprise_readiness_overclaim,
      "enterprise readiness overclaim did not fail closed");

  auto ceic034 = Request(index::IndexFamily::unique_btree);
  ceic034.ceic_034_unique_protocol_pending = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(ceic034),
      index::BtreeUniqueDurableProviderClosureStatus::
          successor_scope_overclaim,
      "CEIC-034 successor scope overclaim did not fail closed");

  auto ceic041 = Request(index::IndexFamily::btree);
  ceic041.ceic_041_crash_matrix_pending = false;
  RequireStatus(
      index::AdmitBtreeUniqueDurableProviderClosure(ceic041),
      index::BtreeUniqueDurableProviderClosureStatus::
          successor_scope_overclaim,
      "CEIC-041 successor scope overclaim did not fail closed");
}

}  // namespace

int main() {
  ValidBtreeClosureAdmitsEvidenceOnly();
  UniqueBtreeRequiresUniqueProof();
  MissingProviderAndMGAFailClosed();
  RequiredProofFailuresFailClosed();
  AuthorityParticipationAndSuccessorScopeFailClosed();
  std::cout << "ceic_033_btree_unique_durable_provider_closure_gate=pass\n";
  return EXIT_SUCCESS;
}
