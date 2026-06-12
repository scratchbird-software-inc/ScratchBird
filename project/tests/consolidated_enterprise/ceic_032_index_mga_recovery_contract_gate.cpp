// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-032 focused validation for common index MGA/COW recovery contracts.
#include "index_mga_recovery_contract.hpp"

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
            (seed * 29u + i * 17u + 0x53u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

bool EvidenceHas(const index::IndexMGARecoveryContractResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexMGARecoveryContract ValidContract() {
  index::IndexMGARecoveryContract contract;
  contract.identity.family = index::IndexFamily::btree;
  contract.identity.route = index::IndexRouteKind::bulk_build;
  contract.identity.provider_id = "ceic032-provider";
  contract.identity.provider_contract_version = "ceic032.v1";
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
  contract.mga_authority.inventory_epoch = 9;
  contract.mga_authority.snapshot_epoch = 9;
  contract.mga_authority.cleanup_horizon_epoch = 9;
  contract.mga_authority.required_engine_evidence_epoch = 9;
  contract.mga_authority.inventory_evidence_id = "mga-inventory-9";
  contract.mga_authority.snapshot_evidence_id = "mga-snapshot-9";
  contract.mga_authority.cleanup_horizon_evidence_id = "mga-horizon-9";
  contract.generation.index_uuid = TestUuid(UuidKind::object, 31);
  contract.generation.generation_uuid = TestUuid(UuidKind::object, 32);
  contract.generation.generation_number = 12;
  contract.generation.cow_generation_number = 12;
  contract.generation.provider_generation_id = "provider-generation-12";
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation_identity_bound = true;
  contract.generation.publish_state =
      index::IndexGenerationPublishState::published;
  contract.recovery.crash_classification =
      index::IndexCrashRecoveryClassification::crash_after_generation_publish;
  contract.recovery.corruption_classification =
      index::IndexCorruptionClassification::provider_payload_corrupt;
  contract.recovery.recovery_evidence_id = "index-recovery-12";
  contract.recovery.durable_recovery_evidence = true;
  contract.recovery.replay_idempotent = true;
  contract.recovery.provider_evidence_only = true;
  contract.provider_evidence = {
      "provider_generation_state=published",
      "provider_recovery_state=classified",
      "ceic032_contract_evidence_only=true"};
  return contract;
}

void RequireStatus(const index::IndexMGARecoveryContractResult& result,
                   index::IndexMGARecoveryContractStatus status,
                   std::string_view message) {
  Require(result.contract_status == status, message);
  Require(!result.ok(), "refused CEIC-032 result unexpectedly ok");
  Require(result.fail_closed, "refused CEIC-032 result did not fail closed");
  Require(!result.diagnostic.diagnostic_code.empty(),
          "refused CEIC-032 result lacks diagnostic code");
  Require(result.contract_evidence_only,
          "refused CEIC-032 result must remain contract evidence only");
}

void ValidPersistentContractAdmitsAsEvidenceOnly() {
  auto result = index::AdmitIndexMGARecoveryContract(ValidContract());
  Require(result.ok(), "valid persistent CEIC-032 contract was refused");
  Require(result.contract_status ==
              index::IndexMGARecoveryContractStatus::admitted_contract_evidence,
          "valid CEIC-032 contract returned wrong status");
  Require(result.contract_evidence_only,
          "CEIC-032 must admit contract evidence only");
  Require(!result.durable_family_closure_claimed,
          "CEIC-032 must not claim durable family closure");
  Require(!result.enterprise_ready_claimed,
          "CEIC-032 must not claim enterprise readiness");
  Require(result.recommendation.validate,
          "CEIC-032 should deterministically recommend validation");
  Require(result.recommendation.rebuild,
          "CEIC-032 corruption classification should recommend rebuild");
  Require(!result.recommendation.replay,
          "crash-after-publish should not recommend replay");
  Require(EvidenceHas(result,
                      "ceic_search_key=CEIC_032_INDEX_MGA_RECOVERY_CONTRACT"),
          "CEIC-032 evidence anchor missing");
  Require(EvidenceHas(result,
                      "crash_classification=CRASH_AFTER_GENERATION_PUBLISH"),
          "crash classification was not recorded");
  Require(EvidenceHas(result,
                      "corruption_classification=PROVIDER_PAYLOAD_CORRUPT"),
          "corruption classification was not recorded");
  Require(EvidenceHas(result, "recommendation_action=validate"),
          "validation recommendation was not recorded");
  Require(EvidenceHas(result, "recommendation_action=rebuild"),
          "rebuild recommendation was not recorded");
}

void MissingMGAAuthorityFailsClosed() {
  auto missing_inventory = ValidContract();
  missing_inventory.mga_authority.inventory_present = false;
  RequireStatus(index::AdmitIndexMGARecoveryContract(missing_inventory),
                index::IndexMGARecoveryContractStatus::missing_mga_inventory,
                "missing MGA inventory did not fail closed");

  auto missing_snapshot = ValidContract();
  missing_snapshot.mga_authority.snapshot_authoritative = false;
  RequireStatus(index::AdmitIndexMGARecoveryContract(missing_snapshot),
                index::IndexMGARecoveryContractStatus::missing_mga_snapshot,
                "missing MGA snapshot did not fail closed");

  auto missing_horizon = ValidContract();
  missing_horizon.mga_authority.cleanup_horizon_present = false;
  RequireStatus(index::AdmitIndexMGARecoveryContract(missing_horizon),
                index::IndexMGARecoveryContractStatus::missing_cleanup_horizon,
                "missing cleanup horizon did not fail closed");

  auto stale = ValidContract();
  stale.mga_authority.snapshot_epoch = 8;
  RequireStatus(index::AdmitIndexMGARecoveryContract(stale),
                index::IndexMGARecoveryContractStatus::stale_mga_evidence,
                "stale MGA evidence did not fail closed");
}

void AuthorityClaimsFailClosed() {
  auto transaction_finality = ValidContract();
  transaction_finality.authority_boundary.transaction_finality_authority = true;
  RequireStatus(index::AdmitIndexMGARecoveryContract(transaction_finality),
                index::IndexMGARecoveryContractStatus::forbidden_authority_claim,
                "transaction finality claim did not fail closed");

  auto parser = ValidContract();
  parser.authority_boundary.parser_authority = true;
  RequireStatus(index::AdmitIndexMGARecoveryContract(parser),
                index::IndexMGARecoveryContractStatus::forbidden_authority_claim,
                "parser authority claim did not fail closed");

  auto reference = ValidContract();
  reference.authority_boundary.reference_authority = true;
  RequireStatus(index::AdmitIndexMGARecoveryContract(reference),
                index::IndexMGARecoveryContractStatus::forbidden_authority_claim,
                "reference authority claim did not fail closed");

  auto wal = ValidContract();
  wal.authority_boundary.wal_authority = true;
  RequireStatus(index::AdmitIndexMGARecoveryContract(wal),
                index::IndexMGARecoveryContractStatus::forbidden_authority_claim,
                "WAL authority claim did not fail closed");

  auto provider_finality = ValidContract();
  provider_finality.authority_boundary.provider_finality_authority = true;
  RequireStatus(index::AdmitIndexMGARecoveryContract(provider_finality),
                index::IndexMGARecoveryContractStatus::forbidden_authority_claim,
                "provider finality claim did not fail closed");
}

void IdentityCleanupRecoveryAndRouteFailures() {
  auto missing_generation = ValidContract();
  missing_generation.generation.provider_generation_id.clear();
  RequireStatus(index::AdmitIndexMGARecoveryContract(missing_generation),
                index::IndexMGARecoveryContractStatus::missing_generation_identity,
                "missing generation identity did not fail closed");

  auto unbound_cleanup = ValidContract();
  unbound_cleanup.mga_authority.cleanup_horizon_engine_bound = false;
  RequireStatus(
      index::AdmitIndexMGARecoveryContract(unbound_cleanup),
      index::IndexMGARecoveryContractStatus::cleanup_horizon_not_engine_bound,
      "cleanup horizon without engine binding did not fail closed");

  auto weak_recovery = ValidContract();
  weak_recovery.recovery.durable_recovery_evidence = false;
  RequireStatus(index::AdmitIndexMGARecoveryContract(weak_recovery),
                index::IndexMGARecoveryContractStatus::recovery_evidence_not_durable,
                "weak recovery evidence did not fail closed");

  auto reference_family = ValidContract();
  reference_family.identity.family = index::IndexFamily::reference_emulated;
  RequireStatus(
      index::AdmitIndexMGARecoveryContract(reference_family),
      index::IndexMGARecoveryContractStatus::reference_policy_local_route_blocked,
      "reference family did not fail closed");

  auto policy_route = ValidContract();
  policy_route.identity.policy_route_requested = true;
  RequireStatus(
      index::AdmitIndexMGARecoveryContract(policy_route),
      index::IndexMGARecoveryContractStatus::reference_policy_local_route_blocked,
      "policy route did not fail closed");

  auto cluster = ValidContract();
  cluster.identity.cluster_path_requested = true;
  RequireStatus(
      index::AdmitIndexMGARecoveryContract(cluster),
      index::IndexMGARecoveryContractStatus::cluster_external_provider_only,
      "cluster path did not fail external-provider-only");

  auto overclaim = ValidContract();
  overclaim.enterprise_ready_claimed = true;
  RequireStatus(
      index::AdmitIndexMGARecoveryContract(overclaim),
      index::IndexMGARecoveryContractStatus::enterprise_readiness_overclaim,
      "enterprise readiness overclaim did not fail closed");
}

void RecommendationsAreDeterministic() {
  auto replay = index::RecommendIndexRecoveryAction(
      index::IndexCrashRecoveryClassification::provider_replay_required,
      index::IndexCorruptionClassification::none);
  Require(replay.validate, "replay case should validate");
  Require(replay.replay, "replay case should recommend replay");
  Require(!replay.rebuild, "replay case should not rebuild without corruption");
  Require(replay.stable_actions.size() == 2,
          "replay case should have deterministic action count");
  Require(replay.stable_actions[0] == "validate",
          "first replay action should be validate");
  Require(replay.stable_actions[1] == "replay",
          "second replay action should be replay");

  auto rebuild = index::RecommendIndexRecoveryAction(
      index::IndexCrashRecoveryClassification::clean_reopen,
      index::IndexCorruptionClassification::checksum_mismatch);
  Require(rebuild.validate, "corruption case should validate");
  Require(rebuild.rebuild, "corruption case should recommend rebuild");
  Require(!rebuild.replay, "clean corruption case should not replay");
  Require(rebuild.stable_actions.size() == 2,
          "rebuild case should have deterministic action count");
  Require(rebuild.stable_actions[0] == "validate",
          "first rebuild action should be validate");
  Require(rebuild.stable_actions[1] == "rebuild",
          "second rebuild action should be rebuild");
}

}  // namespace

int main() {
  ValidPersistentContractAdmitsAsEvidenceOnly();
  MissingMGAAuthorityFailsClosed();
  AuthorityClaimsFailClosed();
  IdentityCleanupRecoveryAndRouteFailures();
  RecommendationsAreDeterministic();
  std::cout << "ceic_032_index_mga_recovery_contract_gate=pass\n";
  return EXIT_SUCCESS;
}
