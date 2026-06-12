// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lifecycle/sequence_generator_lifecycle.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace seq_api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
std::string DiagnosticCode(const TResult& result) {
  if (result.diagnostics.empty()) { return {}; }
  return result.diagnostics.front().code;
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    std::cerr << DiagnosticCode(result) << '\n';
  }
  Require(result.ok, message);
}

template <typename TResult>
void RequireDiagnostic(const TResult& result,
                       std::string_view expected,
                       std::string_view message) {
  Require(!result.ok, message);
  if (DiagnosticCode(result) != expected) {
    std::cerr << "expected=" << expected << " actual=" << DiagnosticCode(result) << '\n';
  }
  Require(DiagnosticCode(result) == expected, message);
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestPath(std::string_view label) {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_013ah_" + std::string(label) + "_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

void Cleanup(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.sequence_generator_events", ignored);
}

seq_api::EngineRequestContext Context(const std::filesystem::path& path,
                                      std::uint64_t tx,
                                      bool cluster_authority = false) {
  seq_api::EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical = "database-013ah";
  context.principal_uuid.canonical = "principal-013ah";
  context.transaction_uuid.canonical = "txn-" + std::to_string(tx);
  context.local_transaction_id = tx;
  context.request_id = "request-" + std::to_string(tx);
  context.security_context_present = true;
  context.cluster_authority_available = cluster_authority;
  context.snapshot_visible_through_local_transaction_id = tx == 0 ? 0 : tx;
  return context;
}

seq_api::EngineSequenceGeneratorDefinition Definition(std::string uuid,
                                                      std::int64_t start,
                                                      std::int64_t min,
                                                      std::int64_t max,
                                                      std::uint64_t cache_size) {
  seq_api::EngineSequenceGeneratorDefinition definition;
  definition.generator_uuid = std::move(uuid);
  definition.database_uuid = "database-013ah";
  definition.schema_uuid = "schema-public";
  definition.value_type_uuid = "int64";
  definition.allocation_mode = "local_node_generator";
  definition.start_value = start;
  definition.min_value = min;
  definition.max_value = max;
  definition.increment_by = 1;
  definition.cache_size = cache_size;
  definition.policy_uuid = "policy-sequence-generator-cache";
  definition.policy_version_uuid = "policy-sequence-generator-cache-v1";
  return definition;
}

seq_api::EngineSequenceCreateGeneratorResult CreateGenerator(
    const std::filesystem::path& path,
    std::uint64_t tx,
    const seq_api::EngineSequenceGeneratorDefinition& definition) {
  seq_api::EngineSequenceCreateGeneratorRequest request;
  request.context = Context(path, tx);
  request.target_object.uuid.canonical = definition.generator_uuid;
  request.definition = definition;
  return seq_api::EngineSequenceCreateGenerator(request);
}

seq_api::EngineSequenceApplyMgaTransactionOutcomeResult ApplyOutcome(
    const std::filesystem::path& path,
    std::uint64_t tx,
    std::string outcome,
    bool committed_row_effects = true,
    bool folded_no_effect = false,
    bool external_exposure_observed = true) {
  seq_api::EngineSequenceApplyMgaTransactionOutcomeRequest request;
  request.context = Context(path, tx);
  request.outcome_local_transaction_id = tx;
  request.outcome_transaction_uuid = "txn-" + std::to_string(tx);
  request.mga_outcome = std::move(outcome);
  request.committed_row_effects = committed_row_effects;
  request.folded_to_no_effect = folded_no_effect;
  request.external_exposure_observed = external_exposure_observed;
  return seq_api::EngineSequenceApplyMgaTransactionOutcome(request);
}

seq_api::EngineSequenceAllocateValueResult Allocate(const std::filesystem::path& path,
                                                   std::uint64_t tx,
                                                   std::string generator_uuid,
                                                   bool external_exposure = true) {
  seq_api::EngineSequenceAllocateValueRequest request;
  request.context = Context(path, tx);
  request.generator_uuid = std::move(generator_uuid);
  request.statement_uuid = "statement-" + std::to_string(tx);
  request.record_uuid = "record-" + std::to_string(tx);
  request.external_exposure_allowed = external_exposure;
  return seq_api::EngineSequenceAllocateValue(request);
}

const seq_api::EngineSequenceGeneratorRecord& RequireGenerator(
    const seq_api::EngineSequenceGeneratorLifecycleState& state,
    const std::string& generator_uuid) {
  for (const auto& generator : state.generators) {
    if (generator.definition.generator_uuid == generator_uuid) { return generator; }
  }
  Fail("generator missing from lifecycle state");
}

const seq_api::EngineSequenceAllocationRecord& RequireAllocationForTx(
    const seq_api::EngineSequenceGeneratorLifecycleState& state,
    std::uint64_t tx) {
  for (const auto& allocation : state.allocations) {
    if (allocation.local_transaction_id == tx) { return allocation; }
  }
  Fail("allocation missing from lifecycle state");
}

bool HasEvidence(const seq_api::EngineApiResult& result, std::string_view kind, std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

void TestCacheWindowPersistenceAndRecovery() {
  const auto path = TestPath("cache_recovery");
  Cleanup(path);
  const std::string sequence_uuid = "019e0fda-aaaa-7aaa-8aaa-cache000001";
  RequireOk(CreateGenerator(path, 1, Definition(sequence_uuid, 10, 10, 100, 3)),
            "DBLC-013AH sequence create failed");
  RequireOk(ApplyOutcome(path, 1, "committed"), "DBLC-013AH create commit evidence failed");

  const auto first = Allocate(path, 2, sequence_uuid);
  RequireOk(first, "DBLC-013AH first allocation failed");
  Require(first.allocated_value == 10, "DBLC-013AH first cache value mismatch");
  Require(first.generator.durable_next_value == 13,
          "DBLC-013AH cache window was not durably advanced before return");

  const auto second = Allocate(path, 3, sequence_uuid);
  RequireOk(second, "DBLC-013AH second allocation failed");
  Require(second.allocated_value == 11, "DBLC-013AH second cache value mismatch");

  const auto loaded_before = seq_api::LoadSequenceGeneratorLifecycleState(Context(path, 4));
  Require(loaded_before.ok, "DBLC-013AH state load before recovery failed");
  const auto& before = RequireGenerator(loaded_before.state, sequence_uuid);
  Require(before.cache_window_active, "DBLC-013AH cache window unexpectedly inactive before recovery");
  Require(before.cache_next_value == 12, "DBLC-013AH cache next value mismatch before recovery");

  seq_api::EngineSequenceRecoverGeneratorStateRequest recover;
  recover.context = Context(path, 4);
  const auto recovered = seq_api::EngineSequenceRecoverGeneratorState(recover);
  RequireOk(recovered, "DBLC-013AH recovery snapshot failed");
  const auto& recovered_generator = RequireGenerator(recovered.state, sequence_uuid);
  Require(recovered_generator.recovered_from_persisted_state,
          "DBLC-013AH recovery snapshot evidence missing");
  Require(recovered_generator.cache_unused_on_recovery == 1,
          "DBLC-013AH unused cache gap count mismatch");
  Require(!recovered_generator.cache_window_active,
          "DBLC-013AH recovery did not discard volatile cache window");

  const auto after_recovery = Allocate(path, 5, sequence_uuid);
  RequireOk(after_recovery, "DBLC-013AH allocation after recovery failed");
  Require(after_recovery.allocated_value == 13,
          "DBLC-013AH recovery did not resume from persisted high water");
  Cleanup(path);
}

void TestRollbackAndReusableTransactionSemantics() {
  const auto path = TestPath("rollback");
  Cleanup(path);
  const std::string non_tx_uuid = "019e0fda-bbbb-7bbb-8bbb-rollback001";
  RequireOk(CreateGenerator(path, 1, Definition(non_tx_uuid, 1, 1, 20, 1)),
            "DBLC-013AH nontransactional create failed");
  RequireOk(ApplyOutcome(path, 1, "committed"), "DBLC-013AH nontransactional create commit failed");
  RequireOk(Allocate(path, 10, non_tx_uuid), "DBLC-013AH nontransactional allocation failed");
  RequireOk(ApplyOutcome(path, 10, "rolled_back", false, true),
            "DBLC-013AH rollback outcome evidence failed");
  const auto rolled_back = seq_api::LoadSequenceGeneratorLifecycleState(Context(path, 11));
  Require(rolled_back.ok, "DBLC-013AH rollback state load failed");
  const auto& consumed = RequireAllocationForTx(rolled_back.state, 10);
  Require(consumed.lifecycle_state == "rolled_back_consumed",
          "DBLC-013AH nontransactional rollback did not preserve consumption");
  Require(Allocate(path, 11, non_tx_uuid).allocated_value == 2,
          "DBLC-013AH nontransactional rollback reused a consumed value");

  auto reusable_definition = Definition("019e0fda-bbbb-7bbb-8bbb-rollback002", 100, 100, 120, 1);
  reusable_definition.transactional_allocation = true;
  reusable_definition.consumed_on_rollback = false;
  reusable_definition.reusable_if_no_effect = true;
  RequireOk(CreateGenerator(path, 20, reusable_definition), "DBLC-013AH reusable create failed");
  RequireOk(ApplyOutcome(path, 20, "committed"), "DBLC-013AH reusable create commit failed");
  const auto reserved = Allocate(path, 21, reusable_definition.generator_uuid, false);
  RequireOk(reserved, "DBLC-013AH reusable allocation failed");
  Require(reserved.allocated_value == 100, "DBLC-013AH reusable allocation value mismatch");
  RequireOk(ApplyOutcome(path, 21, "rolled_back", false, true, false),
            "DBLC-013AH reusable rollback outcome failed");
  const auto reusable_loaded = seq_api::LoadSequenceGeneratorLifecycleState(Context(path, 22));
  Require(reusable_loaded.ok, "DBLC-013AH reusable state load failed");
  const auto& reusable_generator = RequireGenerator(reusable_loaded.state, reusable_definition.generator_uuid);
  Require(!reusable_generator.reusable_released_values.empty(),
          "DBLC-013AH reusable rollback did not release value");
  const auto reused = Allocate(path, 22, reusable_definition.generator_uuid, false);
  RequireOk(reused, "DBLC-013AH reusable allocation reuse failed");
  Require(reused.allocated_value == 100, "DBLC-013AH reusable allocation did not reuse released value");
  Require(HasEvidence(reused, "transactional_released_value_reused", "100"),
          "DBLC-013AH reusable allocation evidence missing");
  Cleanup(path);
}

void TestRestartAlterDropAndExhaustion() {
  const auto path = TestPath("restart_alter_drop");
  Cleanup(path);
  const std::string sequence_uuid = "019e0fda-cccc-7ccc-8ccc-restart0001";
  RequireOk(CreateGenerator(path, 1, Definition(sequence_uuid, 1, 1, 5, 1)),
            "DBLC-013AH restart sequence create failed");
  RequireOk(ApplyOutcome(path, 1, "committed"), "DBLC-013AH restart create commit failed");
  Require(Allocate(path, 2, sequence_uuid).allocated_value == 1,
          "DBLC-013AH initial restart allocation mismatch");

  auto altered_definition = Definition(sequence_uuid, 1, 1, 51, 1);
  seq_api::EngineSequenceAlterGeneratorRequest alter;
  alter.context = Context(path, 3);
  alter.target_object.uuid.canonical = sequence_uuid;
  alter.definition = altered_definition;
  alter.restart_with_value = true;
  alter.restart_value = 50;
  RequireOk(seq_api::EngineSequenceAlterGenerator(alter), "DBLC-013AH alter restart failed");
  RequireOk(ApplyOutcome(path, 3, "committed"), "DBLC-013AH alter restart commit failed");
  Require(Allocate(path, 4, sequence_uuid).allocated_value == 50,
          "DBLC-013AH restart value was not used");
  Require(Allocate(path, 5, sequence_uuid).allocated_value == 51,
          "DBLC-013AH altered max second value mismatch");
  RequireDiagnostic(Allocate(path, 6, sequence_uuid),
                    seq_api::kSequenceDiagnosticExhausted,
                    "DBLC-013AH exhausted sequence did not refuse allocation");

  seq_api::EngineSequenceDropGeneratorRequest drop;
  drop.context = Context(path, 7);
  drop.generator_uuid = sequence_uuid;
  RequireOk(seq_api::EngineSequenceDropGenerator(drop), "DBLC-013AH drop failed");
  RequireOk(ApplyOutcome(path, 7, "committed"), "DBLC-013AH drop commit failed");
  RequireDiagnostic(Allocate(path, 8, sequence_uuid),
                    seq_api::kSequenceDiagnosticDropped,
                    "DBLC-013AH dropped generator accepted allocation");
  Cleanup(path);
}

void TestIdentityBindingMetadata() {
  const auto path = TestPath("identity_binding");
  Cleanup(path);
  seq_api::EngineSequenceBindIdentityValueRequest bind;
  bind.context = Context(path, 1);
  bind.table_uuid = "table-customers";
  bind.record_uuid = "record-row-uuid-001";
  bind.identity_column_uuid = "column-id";
  bind.identity_value_kind = "row_uuid_identity";
  const auto bound = seq_api::EngineSequenceBindIdentityValue(bind);
  RequireOk(bound, "DBLC-013AH row UUID identity bind failed");
  Require(bound.binding.identity_value == bind.record_uuid,
          "DBLC-013AH row UUID identity did not use row UUID");
  Require(HasEvidence(bound, "row_uuid_identity", bind.record_uuid),
          "DBLC-013AH row UUID identity evidence missing");

  bind.context = Context(path, 2);
  bind.identity_value = "second-generated-uuid";
  bind.attempted_second_uuid_for_row_identity = true;
  RequireDiagnostic(seq_api::EngineSequenceBindIdentityValue(bind),
                    seq_api::kSequenceDiagnosticIdentityDoubleUuidForbidden,
                    "DBLC-013AH double UUID identity was accepted");
  Cleanup(path);
}

void TestReferenceMappingDiagnosticsAndLabels() {
  const auto path = TestPath("reference_mapping");
  Cleanup(path);
  auto incomplete = Definition("019e0fda-dddd-7ddd-8ddd-referencebad001", 1, 1, 10, 2);
  incomplete.reference_profile_uuid = "reference-mariadb";
  RequireDiagnostic(CreateGenerator(path, 1, incomplete),
                    seq_api::kSequenceDiagnosticReferenceMappingIncomplete,
                    "DBLC-013AH incomplete reference mapping was accepted");

  auto mapped = Definition("019e0fda-dddd-7ddd-8ddd-referencegood01", 1, 1, 10, 2);
  mapped.reference_profile_uuid = "reference-mariadb";
  mapped.reference_family = "mariadb";
  mapped.reference_mapping_label = "reference:mariadb:sequence:nontransactional";
  mapped.reference_allocation_timing = "before_row_insert";
  mapped.reference_rollback_behavior = "consumed_on_rollback";
  mapped.reference_finality_behavior = "allocated_uncommitted_until_mga";
  mapped.reference_cache_behavior = "cache_window_persisted_high_water";
  mapped.reference_mapping_complete = true;
  RequireOk(CreateGenerator(path, 2, mapped), "DBLC-013AH reference mapped create failed");
  RequireOk(ApplyOutcome(path, 2, "committed"), "DBLC-013AH reference mapped create commit failed");
  const auto allocated = Allocate(path, 3, mapped.generator_uuid);
  RequireOk(allocated, "DBLC-013AH reference mapped allocation failed");
  Require(allocated.allocation.reference_mapping_label == mapped.reference_mapping_label,
          "DBLC-013AH reference mapping label missing from allocation");
  Require(HasEvidence(allocated, "reference_mapping_label", mapped.reference_mapping_label),
          "DBLC-013AH reference mapping evidence missing");
  Cleanup(path);
}

void TestClusterFailClosedBoundary() {
  const auto path = TestPath("cluster_fail_closed");
  Cleanup(path);
  auto clustered = Definition("019e0fda-eeee-7eee-8eee-cluster0001", 1, 1, 10, 1);
  clustered.allocation_mode = "strict_online_generator";
  clustered.requires_cluster_authority = true;
  RequireOk(CreateGenerator(path, 1, clustered), "DBLC-013AH cluster-bound generator create failed");
  RequireOk(ApplyOutcome(path, 1, "committed"), "DBLC-013AH cluster-bound create commit failed");
  const auto refused = Allocate(path, 2, clustered.generator_uuid);
  RequireDiagnostic(refused,
                    seq_api::kSequenceDiagnosticClusterPathAbsent,
                    "DBLC-013AH standalone cluster generator path was accepted");
  Require(refused.cluster_authority_required,
          "DBLC-013AH cluster fail-closed result did not require cluster authority");
  const auto loaded = seq_api::LoadSequenceGeneratorLifecycleState(Context(path, 3));
  Require(loaded.ok, "DBLC-013AH cluster fail-closed state load failed");
  Require(loaded.state.metrics.cluster_metric_paths.empty(),
          "DBLC-013AH standalone state exposed cluster generator metrics");
  Cleanup(path);
}

void TestMgaRetentionInteraction() {
  const auto path = TestPath("retention");
  Cleanup(path);
  const std::string sequence_uuid = "019e0fda-ffff-7fff-8fff-retention01";
  RequireOk(CreateGenerator(path, 1, Definition(sequence_uuid, 1, 1, 20, 1)),
            "DBLC-013AH retention create failed");
  RequireOk(ApplyOutcome(path, 1, "committed"), "DBLC-013AH retention create commit failed");
  RequireOk(Allocate(path, 10, sequence_uuid), "DBLC-013AH retention allocation failed");
  RequireOk(ApplyOutcome(path, 10, "committed"), "DBLC-013AH retention allocation commit failed");
  seq_api::EngineSequenceDropGeneratorRequest drop;
  drop.context = Context(path, 11);
  drop.generator_uuid = sequence_uuid;
  RequireOk(seq_api::EngineSequenceDropGenerator(drop), "DBLC-013AH retention drop failed");
  RequireOk(ApplyOutcome(path, 11, "committed"), "DBLC-013AH retention drop commit failed");

  seq_api::EngineSequenceEvaluateMgaRetentionRequest blocked;
  blocked.context = Context(path, 12);
  blocked.retention_visible_through_local_transaction_id = 5;
  const auto blocked_result = seq_api::EngineSequenceEvaluateMgaRetention(blocked);
  RequireDiagnostic(blocked_result,
                    seq_api::kSequenceDiagnosticMgaRetentionBlocked,
                    "DBLC-013AH MGA retention horizon did not block cleanup");
  Require(blocked_result.blocked_allocation_count == 1,
          "DBLC-013AH MGA retention blocked allocation count mismatch");

  blocked.context = Context(path, 13);
  blocked.retention_visible_through_local_transaction_id = 20;
  RequireOk(seq_api::EngineSequenceEvaluateMgaRetention(blocked),
            "DBLC-013AH MGA retention allowed horizon failed");
  Cleanup(path);
}

}  // namespace

int main() {
  TestCacheWindowPersistenceAndRecovery();
  TestRollbackAndReusableTransactionSemantics();
  TestRestartAlterDropAndExhaustion();
  TestIdentityBindingMetadata();
  TestReferenceMappingDiagnosticsAndLabels();
  TestClusterFailClosedBoundary();
  TestMgaRetentionInteraction();
  return EXIT_SUCCESS;
}
