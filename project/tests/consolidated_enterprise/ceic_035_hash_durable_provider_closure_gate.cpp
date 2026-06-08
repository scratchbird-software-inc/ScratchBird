// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-035 focused validation for hash durable provider closure.
#include "hash_durable_provider_closure.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace index = scratchbird::core::index;
namespace page = scratchbird::storage::page;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;

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
        static_cast<byte>((seed * 41u + i * 23u + 0x59u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<byte>((value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<byte>((value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

std::vector<byte> Key(std::string_view value) {
  return {value.begin(), value.end()};
}

bool EvidenceHas(const index::HashDurableProviderClosureResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexProviderAccessMethodContract ProviderContract() {
  index::IndexProviderAccessMethodContract contract;
  contract.family = index::IndexFamily::hash;
  contract.route = index::IndexRouteKind::bulk_build;
  contract.provider.provider_id = "ceic035-hash-provider";
  contract.provider.provider_name = "CEIC-035 Hash Provider";
  contract.provider.provider_contract_version = "ceic035.provider.v1";
  contract.provider.persistent_access_method = true;
  contract.provider.provider_backed = true;
  contract.route_boundary.route_capability_present = true;
  contract.route_boundary.provider_route_supported = true;
  contract.route_boundary.static_registry_complete_capability_seen = true;
  contract.route_boundary.external_cluster_provider_only = true;
  contract.route_boundary.route_specific_boundary_declared = true;
  contract.mutation_batch.batch_uuid = TestUuid(UuidKind::object, 10);
  contract.mutation_batch.operation_count = 11;
  contract.mutation_batch.batch_admission_requested = true;
  contract.mutation_batch.provider_batch_admission_supported = true;
  contract.mutation_batch.deterministic_batch_order = true;
  contract.mutation_batch.idempotent_replay_safe = true;
  contract.generation.generation_uuid = TestUuid(UuidKind::object, 11);
  contract.generation.generation_number = 35;
  contract.generation.provider_generation_id = "hash-provider-generation-35";
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation = true;
  contract.recovery.recovery_context_id = "hash-recovery-context-35";
  contract.recovery.recovery_reopen_supported = true;
  contract.recovery.crash_classification_supported = true;
  contract.recovery.corruption_classification_supported = true;
  contract.recovery.mga_recovery_evidence_only = true;
  contract.cleanup.oldest_active_transaction_id = 19;
  contract.cleanup.cleanup_generation_floor = 35;
  contract.cleanup.engine_mga_horizon_bound = true;
  contract.cleanup.provider_cleanup_supported = true;
  contract.validation_repair.validate_supported = true;
  contract.validation_repair.repair_supported = true;
  contract.validation_repair.rebuild_supported = true;
  contract.validation_repair.deterministic_diagnostics = true;
  contract.provider_evidence = {
      "ceic031_admitted_provider=true",
      "hash_page_provider_contract=true",
      "ceic035_consumes_ceic031=true"};
  return contract;
}

index::IndexMGARecoveryContract MGAContract() {
  index::IndexMGARecoveryContract contract;
  contract.identity.family = index::IndexFamily::hash;
  contract.identity.route = index::IndexRouteKind::bulk_build;
  contract.identity.provider_id = "ceic035-hash-provider";
  contract.identity.provider_contract_version = "ceic035.provider.v1";
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
  contract.mga_authority.inventory_epoch = 35;
  contract.mga_authority.snapshot_epoch = 35;
  contract.mga_authority.cleanup_horizon_epoch = 35;
  contract.mga_authority.required_engine_evidence_epoch = 35;
  contract.mga_authority.inventory_evidence_id = "mga-inventory-35";
  contract.mga_authority.snapshot_evidence_id = "mga-snapshot-35";
  contract.mga_authority.cleanup_horizon_evidence_id = "mga-horizon-35";
  contract.generation.index_uuid = TestUuid(UuidKind::object, 21);
  contract.generation.generation_uuid = TestUuid(UuidKind::object, 22);
  contract.generation.generation_number = 35;
  contract.generation.cow_generation_number = 35;
  contract.generation.provider_generation_id = "hash-provider-generation-35";
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation_identity_bound = true;
  contract.generation.publish_state =
      index::IndexGenerationPublishState::published;
  contract.recovery.crash_classification =
      index::IndexCrashRecoveryClassification::crash_after_generation_publish;
  contract.recovery.corruption_classification =
      index::IndexCorruptionClassification::checksum_mismatch;
  contract.recovery.recovery_evidence_id = "hash-recovery-35";
  contract.recovery.durable_recovery_evidence = true;
  contract.recovery.replay_idempotent = true;
  contract.recovery.provider_evidence_only = true;
  contract.provider_evidence = {
      "ceic032_admitted_mga_contract=true",
      "hash_mga_recovery_contract=true",
      "ceic035_consumes_ceic032=true"};
  return contract;
}

page::IndexHashPhysicalReport BuildHashPhysicalEvidence() {
  auto initialized = page::InitializeIndexHashPhysicalIndex(
      TestUuid(UuidKind::object, 21),
      384,
      0,
      page::kIndexHashAlgorithmVersion3KeyedHash128Fingerprint,
      1);
  Require(initialized.ok(), "hash physical index initialization failed");
  auto index_image = initialized.index;

  for (unsigned i = 0; i < 6; ++i) {
    auto route = page::LocateIndexHashBucket(index_image, Key("hot-key"));
    Require(route.ok(), "hash bucket route failed");
    auto latch =
        page::AcquireIndexHashBucketExclusiveLatch(&index_image,
                                                   route.bucket_page_number);
    Require(latch.active(), "hash exclusive latch failed");
    page::IndexHashPhysicalInsertRequest insert;
    insert.encoded_key = Key("hot-key");
    insert.row_uuid = TestUuid(UuidKind::row, 100 + i);
    insert.version_uuid = TestUuid(UuidKind::row, 200 + i);
    insert.latch_evidence = latch.evidence();
    auto inserted = page::InsertIndexHashEntry(&index_image, insert);
    if (!inserted.ok()) {
      std::cerr << "hash insert " << i << " failed: "
                << inserted.diagnostic.diagnostic_code << " "
                << inserted.diagnostic.message_key << '\n';
      Fail("hash insert failed");
    }
  }

  auto route = page::LocateIndexHashBucket(index_image, Key("hot-key"));
  Require(route.ok(), "hash delete route failed");
  {
    auto latch =
        page::AcquireIndexHashBucketExclusiveLatch(&index_image,
                                                   route.bucket_page_number);
    page::IndexHashPhysicalDeleteRequest remove;
    remove.encoded_key = Key("hot-key");
    remove.row_uuid = TestUuid(UuidKind::row, 100);
    remove.version_uuid = TestUuid(UuidKind::row, 200);
    remove.latch_evidence = latch.evidence();
    auto deleted = page::DeleteIndexHashEntry(&index_image, remove);
    Require(deleted.ok(), "hash tombstone delete failed");
  }

  auto pre_maintenance = page::BuildIndexHashPhysicalReport(index_image);
  Require(pre_maintenance.report.valid, "hash pre-maintenance report invalid");
  Require(pre_maintenance.report.max_collision_chain_length > 1,
          "hash setup did not create collision-chain depth");
  Require(pre_maintenance.report.max_overflow_depth > 0,
          "hash setup did not create overflow depth");
  std::vector<page::IndexHashBucketLatchEvidence> structural_latches;
  auto structural_latch =
      page::AcquireIndexHashBucketExclusiveLatch(&index_image,
                                                 route.bucket_page_number);
  Require(structural_latch.active(), "hash structural latch failed");
  structural_latches.push_back(structural_latch.evidence());
  page::IndexHashPhysicalMaintenanceRequest maintenance;
  maintenance.policy.tombstone_ratio_per_mille = 1;
  maintenance.policy.overflow_depth_threshold = 0;
  maintenance.exclusive_bucket_latches = structural_latches;
  auto maintained =
      page::MaintainIndexHashPhysicalStructure(&index_image, maintenance);
  Require(maintained.ok(), "hash physical maintenance failed");

  auto exported = page::ExportIndexHashPhysicalIndexImage(index_image);
  Require(exported.ok(), "hash physical image export failed");
  auto imported = page::ImportIndexHashPhysicalIndexImage(exported.image);
  Require(imported.ok(), "hash physical image import failed");
  auto report = page::BuildIndexHashPhysicalReport(imported.index);
  Require(report.report.valid, "hash physical report invalid");
  Require(report.report.max_collision_chain_length > 1,
          "hash final report lost collision-chain depth");
  Require(report.report.max_overflow_depth > 0,
          "hash final report lost overflow depth");
  Require(report.report.hash_algorithm_version ==
              page::kIndexHashAlgorithmVersion3KeyedHash128Fingerprint,
          "hash report did not preserve v3 fingerprint algorithm");
  Require(report.report.hash_seed_engine_generated,
          "hash report did not record engine-generated seed");
  Require(report.report.hash_seed_protected,
          "hash report did not record protected seed");
  return report.report;
}

index::HashDurableProviderClosureRequest Request() {
  auto physical_report = BuildHashPhysicalEvidence();
  index::HashDurableProviderClosureRequest request;
  request.family = index::IndexFamily::hash;
  request.route = index::IndexRouteKind::bulk_build;
  request.provider_id = "ceic035-hash-provider";
  request.provider_admission =
      index::AdmitIndexProviderAccessMethod(ProviderContract());
  request.mga_recovery_contract =
      index::AdmitIndexMGARecoveryContract(MGAContract());
  request.generation_directory.index_uuid = TestUuid(UuidKind::object, 21);
  request.generation_directory.generation_uuid = TestUuid(UuidKind::object, 22);
  request.generation_directory.directory_page_number =
      physical_report.directory_page_number;
  request.generation_directory.generation_number = 35;
  request.generation_directory.cow_generation_number = 35;
  request.generation_directory.cleanup_generation_floor = 35;
  request.generation_directory.oldest_active_transaction_id = 19;
  request.generation_directory.provider_generation_id =
      "hash-provider-generation-35";
  request.generation_directory.cleanup_horizon_evidence_id = "mga-horizon-35";
  request.generation_directory.cow_generation_publish_proven = true;
  request.generation_directory.directory_root_identity_bound = true;
  request.generation_directory.directory_reopen_identity_stable = true;
  request.generation_directory.provider_generation_matches_ceic031 = true;
  request.generation_directory.generation_matches_mga_contract = true;
  request.generation_directory.cleanup_identity_matches_ceic031_ceic032 = true;
  request.generation_directory.publish_after_durable_mga_evidence = true;
  request.physical_structure.physical_report = physical_report;
  request.physical_structure.physical_report_valid = true;
  request.physical_structure.directory_page_proof_present = true;
  request.physical_structure.bucket_page_proof_present = true;
  request.physical_structure.overflow_page_proof_present = true;
  request.physical_structure.bucket_split_or_directory_growth_proof_present =
      true;
  request.physical_structure.bucket_merge_or_compaction_proof_present = true;
  request.physical_structure.collision_chain_metadata_proof_present = true;
  request.physical_structure.deterministic_page_format = true;
  request.physical_structure.route_hash_proof_present = true;
  request.physical_structure.structural_evidence_id =
      "hash-directory-bucket-overflow-35";
  request.seed_fingerprint.hash_algorithm_version =
      page::kIndexHashAlgorithmVersion3KeyedHash128Fingerprint;
  request.seed_fingerprint.hash_seed_engine_generated = true;
  request.seed_fingerprint.hash_seed_protected = true;
  request.seed_fingerprint.hash_seed_high64_protected = true;
  request.seed_fingerprint.hash_seed_key_material_128_bits = true;
  request.seed_fingerprint.hash_seed_redacted_from_diagnostics = true;
  request.seed_fingerprint.hash_seed_entropy_source_recorded = true;
  request.seed_fingerprint.keyed_v2_hash_supported = true;
  request.seed_fingerprint.keyed_v3_128bit_fingerprint_supported = true;
  request.seed_fingerprint.high_assurance_fingerprint_proof_present = true;
  request.seed_fingerprint.hash_seed_entropy_source =
      physical_report.hash_seed_entropy_source;
  request.seed_fingerprint.seed_evidence_id = "hash-seed-fingerprint-35";
  request.full_key_recheck.equality_probe.requested_hash = 7;
  request.full_key_recheck.equality_probe.stored_hash = 7;
  request.full_key_recheck.equality_probe.encoded_key = "left-key";
  request.full_key_recheck.equality_probe.stored_encoded_key = "right-key";
  request.full_key_recheck.equality_probe.stored_key_present = true;
  request.full_key_recheck.equality_probe.allow_collision_recheck = true;
  request.full_key_recheck.equality_decision_checked = true;
  request.full_key_recheck.mandatory_full_encoded_key_recheck = true;
  request.full_key_recheck.hash_match_is_candidate_only = true;
  request.full_key_recheck.collision_requires_full_key_compare = true;
  request.full_key_recheck.fingerprint_is_filter_not_authority = true;
  request.full_key_recheck.mga_recheck_required = true;
  request.full_key_recheck.security_recheck_required = true;
  request.full_key_recheck.candidate_set_only = true;
  request.full_key_recheck.recheck_evidence_id = "hash-full-key-recheck-35";
  request.collision_chain.directory_route_hash_consumed = true;
  request.collision_chain.bucket_index_matches_route_hash = true;
  request.collision_chain.collision_root_metadata_checked = true;
  request.collision_chain.collision_chain_traversal_proven = true;
  request.collision_chain.collision_chain_cycle_refusal_proven = true;
  request.collision_chain.tombstones_excluded_from_probe = true;
  request.collision_chain.overflow_chain_metadata_checked = true;
  request.collision_chain.max_collision_chain_length =
      physical_report.max_collision_chain_length;
  request.collision_chain.max_overflow_depth = physical_report.max_overflow_depth;
  request.collision_chain.collision_evidence_id =
      "hash-route-collision-chain-35";
  request.cleanup.oldest_active_transaction_id = 19;
  request.cleanup.cleanup_generation_floor = 35;
  request.cleanup.engine_mga_horizon_bound = true;
  request.cleanup.cleanup_uses_engine_horizon = true;
  request.cleanup.tombstone_cleanup_compaction_proven = true;
  request.cleanup.overflow_compaction_proven = true;
  request.cleanup.collision_chains_rebuilt_after_compaction = true;
  request.cleanup.provider_cleanup_evidence_only = true;
  request.cleanup.cleanup_evidence_id = "hash-cleanup-compaction-35";
  request.crash_corruption_repair.crash_classification =
      index::IndexCrashRecoveryClassification::crash_after_generation_publish;
  request.crash_corruption_repair.corruption_classification =
      index::IndexCorruptionClassification::checksum_mismatch;
  request.crash_corruption_repair.physical_corruption_class =
      page::IndexHashPhysicalCorruptionClass::none;
  request.crash_corruption_repair.crash_reopen_classification_present = true;
  request.crash_corruption_repair.corruption_classification_present = true;
  request.crash_corruption_repair.durable_provider_evidence_only = true;
  request.crash_corruption_repair.validate_before_repair = true;
  request.crash_corruption_repair.repair_supported = true;
  request.crash_corruption_repair.rebuild_supported = true;
  request.crash_corruption_repair.deterministic_recommendation_present = true;
  request.crash_corruption_repair.recommendation_matches_mga_recovery_contract =
      true;
  request.crash_corruption_repair.classification_evidence_id =
      "hash-crash-corruption-35";
  request.crash_corruption_repair.recommendation_evidence_id =
      "hash-repair-rebuild-35";
  request.adversarial_benchmark.adversarial_collision_benchmark_present = true;
  request.adversarial_benchmark.deterministic_collision_fixture_isolated = true;
  request.adversarial_benchmark.benchmark_evidence_only = true;
  request.adversarial_benchmark.benchmark_clean_capability_claimed = false;
  request.adversarial_benchmark.collision_thresholds_recorded = true;
  request.adversarial_benchmark.rebuild_or_reseed_recommendation_recorded =
      true;
  request.adversarial_benchmark.support_bundle_rows_deterministic = true;
  request.adversarial_benchmark.benchmark_evidence_id =
      "hash-adversarial-collision-benchmark-35";
  request.evidence = {
      "hash_durable_provider_evidence=true",
      "ceic040_runtime_metric_producer_pending=true",
      "ceic041_crash_matrix_pending=true",
      "ceic042_readiness_drift_gate_pending=true"};
  return request;
}

void RequireStatus(const index::HashDurableProviderClosureResult& result,
                   index::HashDurableProviderClosureStatus status,
                   std::string_view message) {
  Require(result.closure_status == status, message);
  Require(!result.ok(), "refused CEIC-035 result unexpectedly ok");
  Require(result.fail_closed, "refused CEIC-035 result did not fail closed");
  Require(!result.diagnostic.diagnostic_code.empty(),
          "refused CEIC-035 result lacks diagnostic code");
  Require(!result.enterprise_ready_claimed,
          "refused CEIC-035 result must not claim enterprise readiness");
  Require(!result.all_index_readiness_claimed,
          "refused CEIC-035 result must not claim all-index readiness");
  Require(!result.ceic_040_runtime_metric_producer_claimed,
          "refused CEIC-035 result must not claim CEIC-040");
  Require(!result.ceic_041_crash_matrix_claimed,
          "refused CEIC-035 result must not claim CEIC-041");
  Require(!result.ceic_042_readiness_drift_gate_claimed,
          "refused CEIC-035 result must not claim CEIC-042");
}

void ValidHashClosureAdmitsEvidenceOnly() {
  auto result = index::AdmitHashDurableProviderClosure(Request());
  Require(result.ok(), "valid hash CEIC-035 closure was refused");
  Require(result.closure_status ==
              index::HashDurableProviderClosureStatus::
                  admitted_durable_provider_evidence,
          "valid hash CEIC-035 closure returned wrong status");
  Require(result.durable_provider_evidence,
          "valid hash closure must admit durable provider evidence");
  Require(result.hash_provider_closure_claimed,
          "valid hash closure must claim hash provider closure");
  Require(!result.enterprise_ready_claimed,
          "CEIC-035 must not claim enterprise readiness");
  Require(!result.all_index_readiness_claimed,
          "CEIC-035 must not claim all-index readiness");
  Require(!result.ceic_040_runtime_metric_producer_claimed,
          "CEIC-035 must not claim CEIC-040 runtime metric closure");
  Require(!result.ceic_041_crash_matrix_claimed,
          "CEIC-035 must not claim CEIC-041 crash matrix");
  Require(!result.ceic_042_readiness_drift_gate_claimed,
          "CEIC-035 must not claim CEIC-042 drift gate");
  Require(result.recommendation.validate,
          "CEIC-035 should carry CEIC-032 validation recommendation");
  Require(result.recommendation.rebuild,
          "CEIC-035 should carry CEIC-032 rebuild recommendation");
  Require(EvidenceHas(result,
                      "ceic_search_key=CEIC_035_HASH_DURABLE_PROVIDER_CLOSURE"),
          "CEIC-035 evidence anchor missing");
  Require(EvidenceHas(result, "full_encoded_key_compare_mandatory=true"),
          "CEIC-035 full-key recheck evidence missing");
  Require(EvidenceHas(result, "hash_seed_key_material_128_bits=true"),
          "CEIC-035 protected hash seed evidence missing");
}

void MissingProviderAndMGAFailClosed() {
  auto missing_provider = Request();
  missing_provider.provider_admission = {};
  RequireStatus(index::AdmitHashDurableProviderClosure(missing_provider),
                index::HashDurableProviderClosureStatus::
                    provider_admission_not_admitted,
                "missing CEIC-031 provider admission did not fail closed");

  auto missing_mga = Request();
  missing_mga.mga_recovery_contract = {};
  RequireStatus(index::AdmitHashDurableProviderClosure(missing_mga),
                index::HashDurableProviderClosureStatus::
                    mga_recovery_contract_not_admitted,
                "missing CEIC-032 MGA contract did not fail closed");

  auto provider_mismatch = Request();
  provider_mismatch.provider_id = "different-provider";
  RequireStatus(index::AdmitHashDurableProviderClosure(provider_mismatch),
                index::HashDurableProviderClosureStatus::
                    provider_mga_identity_mismatch,
                "provider identity mismatch did not fail closed");

  auto generation_mismatch = Request();
  generation_mismatch.generation_directory.generation_number = 36;
  RequireStatus(index::AdmitHashDurableProviderClosure(generation_mismatch),
                index::HashDurableProviderClosureStatus::
                    provider_mga_identity_mismatch,
                "generation identity mismatch did not fail closed");

  auto cleanup_identity_mismatch = Request();
  cleanup_identity_mismatch.generation_directory.cleanup_horizon_evidence_id =
      "different-mga-horizon";
  RequireStatus(index::AdmitHashDurableProviderClosure(
                    cleanup_identity_mismatch),
                index::HashDurableProviderClosureStatus::
                    provider_mga_identity_mismatch,
                "cleanup horizon identity mismatch did not fail closed");
}

void RequiredProofFailuresFailClosed() {
  auto unsupported = Request();
  unsupported.family = index::IndexFamily::btree;
  RequireStatus(index::AdmitHashDurableProviderClosure(unsupported),
                index::HashDurableProviderClosureStatus::unsupported_family,
                "non-hash family did not fail closed");

  auto missing_directory = Request();
  missing_directory.generation_directory.directory_root_identity_bound = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(missing_directory),
                index::HashDurableProviderClosureStatus::
                    missing_generation_directory_identity,
                "missing directory root identity did not fail closed");

  auto missing_report = Request();
  missing_report.physical_structure.physical_report_valid = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(missing_report),
                index::HashDurableProviderClosureStatus::
                    missing_hash_physical_report,
                "missing physical report did not fail closed");

  auto missing_bucket = Request();
  missing_bucket.physical_structure.bucket_page_proof_present = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(missing_bucket),
                index::HashDurableProviderClosureStatus::
                    missing_directory_bucket_overflow_proof,
                "missing bucket proof did not fail closed");

  auto missing_seed = Request();
  missing_seed.seed_fingerprint.hash_seed_protected = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(missing_seed),
                index::HashDurableProviderClosureStatus::
                    missing_hash_seed_provenance,
                "missing hash seed provenance did not fail closed");

  auto report_mismatch = Request();
  report_mismatch.physical_structure.physical_report
      .high_assurance_fingerprint_present = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(report_mismatch),
                index::HashDurableProviderClosureStatus::
                    missing_hash_seed_provenance,
                "hash seed proof not cross-checked with physical report");

  auto missing_high_assurance_proof = Request();
  missing_high_assurance_proof.seed_fingerprint
      .high_assurance_fingerprint_proof_present = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(
                    missing_high_assurance_proof),
                index::HashDurableProviderClosureStatus::
                    missing_hash_seed_provenance,
                "missing high-assurance fingerprint proof did not fail closed");

  auto missing_v3 = Request();
  missing_v3.seed_fingerprint.keyed_v3_128bit_fingerprint_supported = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(missing_v3),
                index::HashDurableProviderClosureStatus::
                    missing_v2_v3_hash_proof,
                "missing v3 fingerprint proof did not fail closed");

  auto fixture = Request();
  fixture.seed_fingerprint.forced_route_collision_hook_used = true;
  fixture.seed_fingerprint.test_fixture_forced_collision_evidence = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(fixture),
                index::HashDurableProviderClosureStatus::
                    fixture_forced_collision_without_test_evidence,
                "forced collision hook without fixture evidence did not fail closed");

  auto no_recheck = Request();
  no_recheck.full_key_recheck.mandatory_full_encoded_key_recheck = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(no_recheck),
                index::HashDurableProviderClosureStatus::
                    missing_full_key_recheck,
                "missing full-key recheck did not fail closed");

  auto no_collision = Request();
  no_collision.collision_chain.collision_chain_traversal_proven = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(no_collision),
                index::HashDurableProviderClosureStatus::
                    missing_collision_chain_route_proof,
                "missing collision-chain proof did not fail closed");

  auto collision_report_mismatch = Request();
  collision_report_mismatch.collision_chain.max_overflow_depth += 1;
  RequireStatus(index::AdmitHashDurableProviderClosure(
                    collision_report_mismatch),
                index::HashDurableProviderClosureStatus::
                    missing_collision_chain_route_proof,
                "collision-chain proof not cross-checked with physical report");
}

void CleanupCrashBenchmarkAndAuthorityFailClosed() {
  auto cleanup = Request();
  cleanup.cleanup.engine_mga_horizon_bound = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(cleanup),
                index::HashDurableProviderClosureStatus::
                    cleanup_horizon_not_engine_bound,
                "missing cleanup engine binding did not fail closed");

  auto crash = Request();
  crash.crash_corruption_repair.crash_classification =
      index::IndexCrashRecoveryClassification::unknown;
  RequireStatus(index::AdmitHashDurableProviderClosure(crash),
                index::HashDurableProviderClosureStatus::
                    crash_corruption_classification_absent,
                "missing crash classification did not fail closed");

  auto repair = Request();
  repair.crash_corruption_repair.recommendation_matches_mga_recovery_contract =
      false;
  RequireStatus(index::AdmitHashDurableProviderClosure(repair),
                index::HashDurableProviderClosureStatus::
                    repair_rebuild_recommendation_missing,
                "missing deterministic repair recommendation did not fail closed");

  auto benchmark = Request();
  benchmark.adversarial_benchmark.adversarial_collision_benchmark_present =
      false;
  RequireStatus(index::AdmitHashDurableProviderClosure(benchmark),
                index::HashDurableProviderClosureStatus::
                    adversarial_collision_benchmark_missing,
                "missing adversarial collision benchmark did not fail closed");

  auto deterministic = Request();
  deterministic.adversarial_benchmark.support_bundle_rows_deterministic = false;
  RequireStatus(index::AdmitHashDurableProviderClosure(deterministic),
                index::HashDurableProviderClosureStatus::
                    deterministic_diagnostics_missing,
                "missing deterministic diagnostics did not fail closed");

  auto donor = Request();
  donor.donor_local_participation = true;
  RequireStatus(index::AdmitHashDurableProviderClosure(donor),
                index::HashDurableProviderClosureStatus::
                    donor_policy_cluster_participation,
                "donor local participation did not fail closed");

  auto authority = Request();
  authority.authority_boundary.provider_finality_authority = true;
  RequireStatus(index::AdmitHashDurableProviderClosure(authority),
                index::HashDurableProviderClosureStatus::
                    forbidden_authority_claim,
                "forbidden provider-finality authority did not fail closed");

  auto ceic040 = Request();
  ceic040.ceic_040_runtime_metric_producer_claimed = true;
  RequireStatus(index::AdmitHashDurableProviderClosure(ceic040),
                index::HashDurableProviderClosureStatus::
                    successor_scope_overclaim,
                "CEIC-040 successor scope overclaim did not fail closed");

  auto ceic041 = Request();
  ceic041.ceic_041_crash_matrix_claimed = true;
  RequireStatus(index::AdmitHashDurableProviderClosure(ceic041),
                index::HashDurableProviderClosureStatus::
                    successor_scope_overclaim,
                "CEIC-041 successor scope overclaim did not fail closed");

  auto enterprise = Request();
  enterprise.enterprise_ready_claimed = true;
  RequireStatus(index::AdmitHashDurableProviderClosure(enterprise),
                index::HashDurableProviderClosureStatus::
                    enterprise_readiness_overclaim,
                "enterprise readiness overclaim did not fail closed");
}

}  // namespace

int main() {
  ValidHashClosureAdmitsEvidenceOnly();
  MissingProviderAndMGAFailClosed();
  RequiredProofFailuresFailClosed();
  CleanupCrashBenchmarkAndAuthorityFailClosed();
  std::cout << "ceic_035_hash_durable_provider_closure_gate=pass\n";
  return EXIT_SUCCESS;
}
