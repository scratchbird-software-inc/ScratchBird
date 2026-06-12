// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-121 online maintenance progress/cancel/resume closure gate.

#include "agents/nosql_family_maintenance_agent.hpp"
#include "agents/storage_version_cleanup_agent.hpp"
#include "heavy_immutable_generation_publication.hpp"
#include "index_backup_restore.hpp"
#include "index_maintenance.hpp"
#include "online_maintenance_progress.hpp"
#include "optimizer_statistics_lifecycle.hpp"
#include "sorted_bulk_index_build.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace impl_agents = scratchbird::core::agents::implemented_agents;
namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "OPTIMIZER_DEFICIENCY_ODF_121_ONLINE_MAINTENANCE_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

platform::u64 NextMillis() {
  static platform::u64 next = 1779550000000ull;
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "ODF-121 generated UUID creation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind) {
  return uuid::UuidToString(NewUuid(kind).value);
}

bool HasEvidence(const agents::OnlineMaintenanceProgressSnapshot& snapshot,
                 std::string_view key,
                 std::string_view value = {}) {
  for (const auto& field : snapshot.evidence) {
    if (field.key == key && (value.empty() || field.value == value)) {
      return true;
    }
  }
  return false;
}

bool HasNoForbiddenRuntimeDocDependency(std::string_view text) {
  return !Contains(text, "docs" "/execution-plans") &&
         !Contains(text, "docs" "/findings") &&
         !Contains(text, "public_audit_summary") &&
         !Contains(text, "public_release_evidence") &&
         !Contains(text, "docs/references");
}

agents::WorkloadResourcePoolConfig MaintenancePool() {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = "odf121_online_maintenance_pool";
  pool.workload_class = agents::WorkloadClass::maintenance;
  pool.limits.hard.memory_bytes = 1 << 20;
  pool.limits.hard.worker_slots = 4;
  pool.limits.hard.active_requests = 8;
  pool.limits.hard.transaction_slots = 8;
  pool.limits.hard.temp_bytes = 1 << 20;
  pool.limits.hard.filespace_bytes = 1 << 20;
  pool.limits.hard.buffer_bytes = 1 << 20;
  return pool;
}

agents::WorkloadAdmissionRequest MaintenanceReservation(std::string request_id,
                                                        platform::u64 memory = 4096) {
  agents::WorkloadAdmissionRequest request;
  request.request_uuid = std::move(request_id);
  request.pool_id = "odf121_online_maintenance_pool";
  request.workload_class = agents::WorkloadClass::maintenance;
  request.source = agents::WorkloadAdmissionSource::engine;
  request.requested.memory_bytes = memory;
  request.requested.worker_slots = 1;
  request.requested.active_requests = 1;
  request.requested.transaction_slots = 1;
  request.principal_tag = "odf121_online_maintenance_gate";
  return request;
}

agents::OnlineMaintenanceStartRequest StartRequest(
    agents::OnlineMaintenanceOperationKind kind,
    std::string operation_uuid,
    std::string database_uuid,
    std::string target_uuid,
    platform::u64 total_units,
    agents::WorkloadResourceQuotaController* quota_controller = nullptr,
    platform::u64 memory = 4096) {
  agents::OnlineMaintenanceStartRequest request;
  request.kind = kind;
  request.operation_uuid = std::move(operation_uuid);
  request.database_uuid = std::move(database_uuid);
  request.target_uuid = std::move(target_uuid);
  request.stage = "scan";
  request.work_unit_label = "rows";
  request.total_units = total_units;
  request.now_microseconds = NextMillis() * 1000;
  request.engine_mga_authoritative = true;
  request.durable_checkpoint_persisted = true;
  request.support_bundle_sink_available = true;
  request.observability_sink_available = true;
  request.cancelable = true;
  request.resumable = true;
  request.require_resource_admission = quota_controller != nullptr;
  request.quota_controller = quota_controller;
  if (quota_controller != nullptr) {
    request.resource_request =
        MaintenanceReservation(request.operation_uuid + ":start", memory);
  }
  return request;
}

agents::OnlineMaintenanceProgressRequest ProgressRequest(
    std::string operation_uuid,
    platform::u64 completed,
    platform::u64 total,
    std::string stage) {
  agents::OnlineMaintenanceProgressRequest request;
  request.operation_uuid = std::move(operation_uuid);
  request.completed_units = completed;
  request.total_units = total;
  request.stage = std::move(stage);
  request.now_microseconds = NextMillis() * 1000;
  request.durable_checkpoint_persisted = true;
  request.checkpoint_payload =
      "odf121_checkpoint_units:" + std::to_string(completed);
  return request;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "ODF-121 transaction identity creation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry Txn(platform::u64 local_id,
                                   mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = NewIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = NextMillis();
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = NextMillis();
    entry.evidence_record_written = true;
  }
  return entry;
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest(
    std::vector<mga::TransactionInventoryEntry> entries,
    platform::u64 next_local_transaction_id) {
  mga::LocalTransactionInventory inventory;
  inventory.entries = std::move(entries);
  inventory.next_local_transaction_id = next_local_transaction_id;

  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = std::move(inventory);
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  return request;
}

mga::RowVersionMetadata RowVersion(
    const mga::RowIdentity& row,
    const mga::TransactionInventoryEntry& creator,
    mga::RowVersionState state,
    platform::u64 sequence,
    platform::u64 next_sequence = 0,
    platform::u64 successor_local_id = 0) {
  mga::RowVersionMetadata version;
  version.identity.row = row;
  version.identity.creator_transaction = creator.identity;
  version.identity.version_sequence = sequence;
  version.state = state;
  version.creator_transaction_state = creator.state;
  version.payload_present = state != mga::RowVersionState::rolled_back;
  if (next_sequence != 0) {
    version.chain.next_version_sequence = next_sequence;
  }
  if (successor_local_id != 0) {
    version.successor_transaction_local_id =
        mga::MakeLocalTransactionId(successor_local_id);
  }
  return version;
}

idx::SortedBulkIndexBuildRequest SortedBuildRequest() {
  idx::SortedBulkIndexBuildRequest request;
  request.metadata.index_uuid = NewUuid(platform::UuidKind::object);
  request.metadata.table_uuid = NewUuid(platform::UuidKind::object);
  request.metadata.family = idx::IndexFamily::btree;
  request.metadata.family_name = "btree";
  request.metadata.rebuild = true;
  request.metadata.page_budget = 16;
  request.metadata.byte_budget = 65536;
  request.metadata.time_budget_microseconds = 1000000;
  request.metadata.leaf_entry_capacity = 2;
  for (int i = 0; i != 6; ++i) {
    idx::SortedBulkIndexRowInput row;
    row.encoded_key = "key:" + std::to_string(6 - i);
    row.row_uuid = NewUuidText(platform::UuidKind::row);
    row.version_uuid = NewUuidText(platform::UuidKind::object);
    row.payload_value = "payload";
    row.source_ordinal = static_cast<platform::u64>(i);
    request.rows.push_back(std::move(row));
  }
  return request;
}

idx::IndexMaintenanceRequest IndexRebuildRequest(platform::TypedUuid index_uuid) {
  idx::IndexMaintenanceRequest request;
  request.index_uuid = index_uuid;
  request.family = idx::IndexFamily::btree;
  request.operation = idx::IndexMaintenanceOperation::rebuild;
  request.page_budget = 16;
  request.byte_budget = 65536;
  request.time_budget_microseconds = 1000000;
  request.online = true;
  request.policy_allows_mutation = true;
  return request;
}

opt::OptimizerStatisticsLifecycleRequest StatsRefreshRequest() {
  opt::OptimizerStatisticsLifecycleRequest request;
  request.trigger = opt::OptimizerStatisticsLifecycleTrigger::kPostBulkRefresh;
  request.relation_uuid = NewUuidText(platform::UuidKind::object);
  request.column_uuids.push_back(NewUuidText(platform::UuidKind::object));
  request.current_stats_epoch = 4;
  request.request_stats_epoch = 4;
  request.catalog_epoch = 7;
  request.security_epoch = 7;
  request.policy_epoch = 7;
  request.stats_visibility_epoch = 9;
  request.current_freshness = opt::OptimizerStatsFreshnessState::kStale;
  request.sampled_rows = 64;
  request.total_rows_estimate = 128;
  request.page_count = 8;
  request.average_row_bytes = 64;
  request.bulk_rows_written = 32;
  request.histogram_bucket_target = 8;
  request.mcv_entry_target = 4;
  return request;
}

idx::OptimizedStructureLifecycleRequest BackfillRebuildRequest() {
  idx::OptimizedStructureLifecycleRequest request;
  request.structure = idx::OptimizedPersistedStructureKind::page_extent_summaries;
  request.operation = idx::OptimizedStructureLifecycleOperation::rebuild;
  request.movement_validation_required = false;
  request.manifest_coverage_verified = true;
  request.transaction_finality_proven_by_mga_inventory = true;
  request.authoritative_base_available = true;
  request.support_bundle_evidence_sink_available = true;
  request.repair_mutation_allowed = true;
  request.target_identity_resolved_to_generated_uuids = true;
  request.checksum_valid = true;
  request.structure_supported = true;
  return request;
}

idx::HeavyImmutableGenerationValidationRequest GenerationValidationRequest() {
  idx::HeavyImmutableGenerationValidationRequest request;
  request.identity.generation_uuid = NewUuid(platform::UuidKind::object);
  request.identity.table_or_collection_uuid = NewUuid(platform::UuidKind::object);
  request.identity.transaction_uuid = NewUuid(platform::UuidKind::transaction);
  request.identity.family = "vector";
  request.identity.profile = "odf121_generation_publish";
  request.source_row_count = 10;
  request.source_payload_count = 10;
  request.source_row_count_present = true;
  request.source_payload_count_present = true;
  request.validation_succeeded = true;
  request.immutable_payload_complete = true;
  request.source_counts_verified = true;
  request.checksum_verified = true;
  request.validation_proof_ref = "engine:validation:odf121";
  request.engine_mga_authority_evidence_ref = "engine:mga:authority:odf121";
  request.engine_mga_inventory_evidence_ref = "engine:mga:inventory:odf121";
  request.operation_id = "odf121.generation_publish";
  return request;
}

void RequireOnlineCommon(const agents::OnlineMaintenanceResult& result,
                         std::string_view diagnostic_code) {
  Require(result.snapshot.diagnostic_code == diagnostic_code,
          "ODF-121 online maintenance diagnostic mismatch");
  Require(HasEvidence(result.snapshot, "parser_finality_authority", "false"),
          "ODF-121 parser authority evidence missing");
  Require(HasEvidence(result.snapshot, "reference_finality_authority", "false"),
          "ODF-121 reference authority evidence missing");
  Require(HasEvidence(result.snapshot, "write_ahead_recovery_authority", "false"),
          "ODF-121 write-ahead authority evidence missing");
  Require(HasEvidence(result.snapshot,
                      "observability_metric_family",
                      "sys.metrics.online_maintenance"),
          "ODF-121 observability evidence missing");
}

void TestSortedBuildRebuildCancelResumeAndUnsafePublish() {
  agents::OnlineMaintenanceStateStore store;
  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(MaintenancePool()).ok,
          "ODF-121 quota pool registration failed");

  auto sorted_request = SortedBuildRequest();
  const auto sorted = idx::BuildSortedExactBulkIndex(sorted_request);
  Require(sorted.ok(), "ODF-121 sorted exact bulk index build failed");
  Require(sorted.root_publish_fence ==
              "mga_index_append_path_after_bottom_up_root_generation",
          "ODF-121 sorted build root publish fence changed");

  const auto rebuild_plan =
      idx::PlanIndexMaintenance(
          IndexRebuildRequest(sorted_request.metadata.index_uuid));
  Require(rebuild_plan.ok(), "ODF-121 index rebuild planning failed");

  const std::string operation_uuid = NewUuidText(platform::UuidKind::object);
  const std::string database_uuid = NewUuidText(platform::UuidKind::database);
  const std::string target_uuid = NewUuidText(platform::UuidKind::object);
  auto started = agents::StartOnlineMaintenanceOperation(
      &store,
      StartRequest(agents::OnlineMaintenanceOperationKind::sorted_index_build,
                   operation_uuid,
                   database_uuid,
                   target_uuid,
                   100,
                   &quota));
  Require(started.ok(), "ODF-121 sorted build maintenance start failed");
  Require(started.snapshot.percent_basis_points == 0,
          "ODF-121 initial progress was not stable zero");
  Require(quota.ActiveReservationCount() == 1,
          "ODF-121 resource reservation missing");

  auto progressed = agents::RecordOnlineMaintenanceProgress(
      &store, ProgressRequest(operation_uuid, 35, 100, "sort_runs"));
  Require(progressed.ok(), "ODF-121 sorted build progress failed");
  Require(progressed.snapshot.percent_basis_points == 3500,
          "ODF-121 progress percent mismatch");
  Require(progressed.snapshot.checkpoint_generation == 1,
          "ODF-121 checkpoint generation did not advance");

  agents::OnlineMaintenanceCancelRequest cancel;
  cancel.operation_uuid = operation_uuid;
  cancel.now_microseconds = NextMillis() * 1000;
  cancel.durable_checkpoint_persisted = true;
  cancel.checkpoint_payload = "cancelled_after_sorted_runs";
  cancel.quota_controller = &quota;
  auto cancelled = agents::CancelOnlineMaintenanceOperation(&store, cancel);
  Require(cancelled.ok(), "ODF-121 cancel checkpoint failed");
  Require(cancelled.snapshot.phase == agents::OnlineMaintenancePhase::cancelled,
          "ODF-121 cancel phase mismatch");
  Require(cancelled.snapshot.resumable,
          "ODF-121 cancelled operation was not resumable");
  Require(!cancelled.snapshot.published_visible,
          "ODF-121 cancelled maintenance became visible");
  Require(quota.ActiveReservationCount() == 0,
          "ODF-121 cancel did not release reservation");

  const auto checkpoint_text =
      agents::SerializeOnlineMaintenanceCheckpoint(cancelled.record);
  Require(HasNoForbiddenRuntimeDocDependency(checkpoint_text),
          "ODF-121 checkpoint contains forbidden runtime doc dependency");
  const auto checkpoint_path =
      std::filesystem::temp_directory_path() /
      ("scratchbird_odf121_" + operation_uuid + ".checkpoint");
  Require(agents::PersistOnlineMaintenanceCheckpointFile(
              checkpoint_path.string(),
              cancelled.record)
              .ok,
          "ODF-121 checkpoint file persistence failed");
  const auto loaded_checkpoint =
      agents::LoadOnlineMaintenanceCheckpointFile(checkpoint_path.string());
  Require(loaded_checkpoint.ok(),
          "ODF-121 checkpoint file load failed");
  std::error_code ignored_remove_error;
  std::filesystem::remove(checkpoint_path, ignored_remove_error);
  const auto parsed = agents::ParseOnlineMaintenanceCheckpoint(checkpoint_text);
  Require(parsed.has_value(), "ODF-121 checkpoint parse failed");

  agents::OnlineMaintenanceStateStore unsafe_resume_store;
  Require(unsafe_resume_store.Upsert(*parsed).ok,
          "ODF-121 unsafe resume fixture store failed");
  agents::OnlineMaintenanceResumeRequest unsafe_resume;
  unsafe_resume.operation_uuid = operation_uuid;
  unsafe_resume.now_microseconds = NextMillis() * 1000;
  unsafe_resume.engine_mga_authoritative = false;
  unsafe_resume.durable_checkpoint_persisted = true;
  unsafe_resume.support_bundle_sink_available = true;
  unsafe_resume.observability_sink_available = true;
  auto resume_refused =
      agents::ResumeOnlineMaintenanceOperation(&unsafe_resume_store,
                                               unsafe_resume);
  Require(!resume_refused.ok(), "ODF-121 unsafe resume was accepted");
  Require(resume_refused.snapshot.diagnostic_code ==
              agents::kOnlineMaintenanceUnsafeResumeRefused,
          "ODF-121 unsafe resume diagnostic mismatch");

  agents::OnlineMaintenanceStateStore restart_store;
  auto recovered = agents::RecoverOnlineMaintenanceOperation(
      &restart_store,
      {loaded_checkpoint.record, NextMillis() * 1000, true, true, true});
  Require(recovered.ok(), "ODF-121 crash recovery did not produce resumable state");
  Require(recovered.snapshot.phase == agents::OnlineMaintenancePhase::resumable,
          "ODF-121 crash recovery phase mismatch");
  RequireOnlineCommon(recovered, agents::kOnlineMaintenanceRecoveredResumable);

  agents::OnlineMaintenanceResumeRequest resume;
  resume.operation_uuid = operation_uuid;
  resume.now_microseconds = NextMillis() * 1000;
  resume.engine_mga_authoritative = true;
  resume.durable_checkpoint_persisted = true;
  resume.support_bundle_sink_available = true;
  resume.observability_sink_available = true;
  resume.require_resource_admission = true;
  resume.quota_controller = &quota;
  resume.resource_request = MaintenanceReservation(operation_uuid + ":resume");
  auto resumed = agents::ResumeOnlineMaintenanceOperation(&restart_store, resume);
  Require(resumed.ok(), "ODF-121 resume failed");
  Require(resumed.snapshot.phase == agents::OnlineMaintenancePhase::running,
          "ODF-121 resume phase mismatch");

  auto publish_ready = agents::RecordOnlineMaintenanceProgress(
      &restart_store, ProgressRequest(operation_uuid, 100, 100, "publish_ready"));
  Require(publish_ready.ok(), "ODF-121 publish-ready progress failed");
  Require(publish_ready.snapshot.publish_ready,
          "ODF-121 sorted build did not become publish-ready");

  agents::OnlineMaintenancePublishRequest unsafe_publish;
  unsafe_publish.operation_uuid = operation_uuid;
  unsafe_publish.now_microseconds = NextMillis() * 1000;
  unsafe_publish.engine_mga_authoritative = true;
  unsafe_publish.durable_publication_fence_persisted = false;
  unsafe_publish.authoritative_generation_validated = true;
  unsafe_publish.no_partial_visibility = true;
  unsafe_publish.support_bundle_sink_available = true;
  unsafe_publish.observability_sink_available = true;
  unsafe_publish.quota_controller = &quota;
  auto refused =
      agents::PublishOnlineMaintenanceOperation(&restart_store, unsafe_publish);
  Require(!refused.ok(), "ODF-121 unsafe publish was accepted");
  Require(refused.snapshot.diagnostic_code ==
              agents::kOnlineMaintenanceUnsafePublishRefused,
          "ODF-121 unsafe publish diagnostic mismatch");
  Require(!refused.snapshot.published_visible,
          "ODF-121 unsafe publish exposed visibility");
  Require(HasEvidence(refused.snapshot,
                      "support_bundle_event",
                      "online_maintenance_lifecycle"),
          "ODF-121 unsafe publish support evidence missing");

  auto denied = agents::StartOnlineMaintenanceOperation(
      &store,
      StartRequest(agents::OnlineMaintenanceOperationKind::index_rebuild,
                   NewUuidText(platform::UuidKind::object),
                   database_uuid,
                   target_uuid,
                   10,
                   &quota,
                   2 << 20));
  Require(!denied.ok(), "ODF-121 over-quota maintenance was admitted");
  Require(denied.snapshot.diagnostic_code ==
              agents::kOnlineMaintenanceResourceDenied,
          "ODF-121 resource denial diagnostic mismatch");
}

void TestNoSqlCompactionAndGenerationPublish() {
  const auto horizon = HorizonRequest(
      {Txn(1, mga::TransactionState::committed),
       Txn(2, mga::TransactionState::committed),
       Txn(3, mga::TransactionState::committed)},
      10);

  impl_agents::NoSqlFamilyMaintenanceAgentRequest nosql;
  nosql.horizon_request = horizon;
  nosql.engine_mga_authoritative = true;
  nosql.execute_plan = true;
  nosql.now_microseconds = NextMillis() * 1000;
  nosql.scheduler_policy.enabled = true;
  nosql.scheduler_policy.max_total_work_units = 32;
  nosql.scheduler_policy.max_scheduled_items = 32;
  impl_agents::NoSqlFamilyMaintenanceCandidate candidate;
  candidate.family = impl_agents::NoSqlFamilyMaintenanceFamily::document;
  candidate.generation_id = NewUuidText(platform::UuidKind::object);
  candidate.generation_kind = "document_shape_fragment";
  candidate.sealed_local_transaction_id = 1;
  candidate.superseded_local_transaction_id = 2;
  candidate.estimated_bytes = 4096;
  candidate.generation_evidence_authoritative = true;
  nosql.candidates.push_back(candidate);
  const auto compaction = impl_agents::RunNoSqlFamilyMaintenanceAgent(nosql);
  Require(compaction.ok(), "ODF-121 NoSQL compaction did not execute");
  Require(!compaction.actions.empty(),
          "ODF-121 NoSQL compaction action missing");

  agents::OnlineMaintenanceStateStore store;
  const std::string database_uuid = NewUuidText(platform::UuidKind::database);
  const std::string compaction_op = NewUuidText(platform::UuidKind::object);
  auto started = agents::StartOnlineMaintenanceOperation(
      &store,
      StartRequest(agents::OnlineMaintenanceOperationKind::nosql_compaction,
                   compaction_op,
                   database_uuid,
                   candidate.generation_id,
                   2));
  Require(started.ok(), "ODF-121 NoSQL maintenance start failed");
  auto done = agents::RecordOnlineMaintenanceProgress(
      &store, ProgressRequest(compaction_op, 2, 2, "compacted"));
  Require(done.ok(), "ODF-121 NoSQL compaction progress failed");
  agents::OnlineMaintenanceCompleteRequest complete;
  complete.operation_uuid = compaction_op;
  complete.now_microseconds = NextMillis() * 1000;
  complete.durable_checkpoint_persisted = true;
  complete.support_bundle_sink_available = true;
  complete.observability_sink_available = true;
  auto completed = agents::CompleteOnlineMaintenanceOperation(&store, complete);
  Require(completed.ok(), "ODF-121 NoSQL compaction completion failed");
  Require(completed.snapshot.phase == agents::OnlineMaintenancePhase::completed,
          "ODF-121 NoSQL completion phase mismatch");

  idx::HeavyImmutableGenerationLedger generation_ledger;
  auto validated = idx::ValidateHeavyImmutableGeneration(
      &generation_ledger, GenerationValidationRequest());
  Require(validated.ok(), "ODF-121 heavy immutable generation validation failed");

  const std::string publish_op = NewUuidText(platform::UuidKind::object);
  auto publish_started = agents::StartOnlineMaintenanceOperation(
      &store,
      StartRequest(agents::OnlineMaintenanceOperationKind::generation_publish,
                   publish_op,
                   database_uuid,
                   uuid::UuidToString(
                       validated.generation.identity.generation_uuid.value),
                   1));
  Require(publish_started.ok(),
          "ODF-121 generation publish maintenance start failed");
  auto publish_progress = agents::RecordOnlineMaintenanceProgress(
      &store, ProgressRequest(publish_op, 1, 1, "validated"));
  Require(publish_progress.ok(),
          "ODF-121 generation publish progress failed");

  idx::HeavyImmutableGenerationPublicationRequest publish_request;
  publish_request.publication_fence_ref = "engine:mga:fence:odf121";
  publish_request.engine_owned_mga_publication_fence = true;
  auto generation = validated.generation;
  const auto published_generation = idx::PublishHeavyImmutableGeneration(
      &generation_ledger, &generation, publish_request);
  Require(published_generation.ok(),
          "ODF-121 heavy immutable generation publish failed");
  Require(idx::HeavyImmutableGenerationPublished(
              published_generation.generation),
          "ODF-121 heavy immutable generation was not visible after publish");

  agents::OnlineMaintenancePublishRequest publish;
  publish.operation_uuid = publish_op;
  publish.now_microseconds = NextMillis() * 1000;
  publish.engine_mga_authoritative = true;
  publish.durable_publication_fence_persisted = true;
  publish.authoritative_generation_validated = true;
  publish.no_partial_visibility = true;
  publish.support_bundle_sink_available = true;
  publish.observability_sink_available = true;
  auto published = agents::PublishOnlineMaintenanceOperation(&store, publish);
  Require(published.ok(), "ODF-121 online generation publish failed");
  Require(published.snapshot.published_visible,
          "ODF-121 online generation publish not visible");

  const auto serialized =
      agents::SerializeOnlineMaintenanceCheckpoint(published.record);
  const auto parsed = agents::ParseOnlineMaintenanceCheckpoint(serialized);
  Require(parsed.has_value(), "ODF-121 published checkpoint parse failed");
  agents::OnlineMaintenanceStateStore restart_store;
  auto recovered = agents::RecoverOnlineMaintenanceOperation(
      &restart_store,
      {*parsed, NextMillis() * 1000, true, true, true});
  Require(recovered.ok() && recovered.snapshot.published_visible,
          "ODF-121 published generation recovery did not remain visible");
}

void TestStatsBackfillCleanupAndCrashSafety() {
  const auto stats = opt::EvaluateOptimizerStatisticsLifecycle(
      StatsRefreshRequest());
  Require(stats.accepted, "ODF-121 stats refresh lifecycle was refused");
  Require(!stats.transaction_finality_semantics_changed,
          "ODF-121 stats refresh changed finality semantics");

  const auto backfill =
      idx::EvaluateOptimizedStructureLifecycle(BackfillRebuildRequest());
  Require(backfill.ok(), "ODF-121 optimized backfill rebuild was refused");
  Require(backfill.engine_mga_authority_preserved,
          "ODF-121 backfill did not preserve MGA authority");

  const auto old = Txn(1, mga::TransactionState::committed);
  const auto successor = Txn(2, mga::TransactionState::committed);
  const auto rolled_back = Txn(3, mga::TransactionState::rolled_back);
  mga::RowIdentity row;
  row.row_uuid = NewUuid(platform::UuidKind::row);
  impl_agents::StorageVersionCleanupAgentRequest cleanup;
  cleanup.horizon_request = HorizonRequest({old, successor, rolled_back}, 4);
  cleanup.row_versions = {
      RowVersion(row, old, mga::RowVersionState::committed, 10, 20, 2),
      RowVersion(row, successor, mga::RowVersionState::committed, 20),
      RowVersion(mga::RowIdentity{NewUuid(platform::UuidKind::row)},
                 rolled_back,
                 mga::RowVersionState::rolled_back,
                 30)};
  cleanup.max_candidate_row_versions = 16;
  cleanup.engine_mga_authoritative = true;
  const auto cleaned =
      impl_agents::RunStorageVersionCleanupAgentBatch(cleanup);
  Require(cleaned.ok(), "ODF-121 storage cleanup batch failed");
  Require(cleaned.sweep.cleanup.reclaimed_row_version_count == 2,
          "ODF-121 storage cleanup reclaimed count mismatch");

  agents::OnlineMaintenanceStateStore store;
  const std::string database_uuid = NewUuidText(platform::UuidKind::database);
  const std::string stats_op = NewUuidText(platform::UuidKind::object);
  auto stats_started = agents::StartOnlineMaintenanceOperation(
      &store,
      StartRequest(
          agents::OnlineMaintenanceOperationKind::optimizer_stats_refresh,
          stats_op,
          database_uuid,
          StatsRefreshRequest().relation_uuid,
          4));
  Require(stats_started.ok(), "ODF-121 stats refresh start failed");
  auto stats_progress = agents::RecordOnlineMaintenanceProgress(
      &store, ProgressRequest(stats_op, 2, 4, "sampled"));
  Require(stats_progress.ok(), "ODF-121 stats refresh progress failed");

  const auto checkpoint =
      agents::SerializeOnlineMaintenanceCheckpoint(stats_progress.record);
  const auto parsed = agents::ParseOnlineMaintenanceCheckpoint(checkpoint);
  Require(parsed.has_value(), "ODF-121 stats checkpoint parse failed");
  agents::OnlineMaintenanceStateStore restart_store;
  auto recovered = agents::RecoverOnlineMaintenanceOperation(
      &restart_store,
      {*parsed, NextMillis() * 1000, true, true, true});
  Require(recovered.ok() &&
              recovered.snapshot.phase ==
                  agents::OnlineMaintenancePhase::resumable,
          "ODF-121 stats refresh restart was not resumable");

  agents::OnlineMaintenanceResumeRequest resume;
  resume.operation_uuid = stats_op;
  resume.now_microseconds = NextMillis() * 1000;
  resume.engine_mga_authoritative = true;
  resume.durable_checkpoint_persisted = true;
  resume.support_bundle_sink_available = true;
  resume.observability_sink_available = true;
  auto resumed = agents::ResumeOnlineMaintenanceOperation(&restart_store, resume);
  Require(resumed.ok(), "ODF-121 stats refresh resume failed");
  auto stats_done = agents::RecordOnlineMaintenanceProgress(
      &restart_store, ProgressRequest(stats_op, 4, 4, "catalog_persisted"));
  Require(stats_done.ok(), "ODF-121 stats refresh completion progress failed");

  agents::OnlineMaintenanceCompleteRequest complete;
  complete.operation_uuid = stats_op;
  complete.now_microseconds = NextMillis() * 1000;
  complete.durable_checkpoint_persisted = true;
  complete.support_bundle_sink_available = true;
  complete.observability_sink_available = true;
  auto completed =
      agents::CompleteOnlineMaintenanceOperation(&restart_store, complete);
  Require(completed.ok(), "ODF-121 stats refresh completion failed");
  Require(completed.snapshot.phase == agents::OnlineMaintenancePhase::completed,
          "ODF-121 stats refresh completion phase mismatch");

  auto unsafe = completed.record;
  unsafe.snapshot.phase = agents::OnlineMaintenancePhase::publish_ready;
  unsafe.snapshot.partial_publish_visible = true;
  unsafe.publication_fence_persisted = false;
  unsafe.authoritative_generation_validated = false;
  agents::OnlineMaintenanceStateStore unsafe_store;
  auto refused = agents::RecoverOnlineMaintenanceOperation(
      &unsafe_store,
      {unsafe, NextMillis() * 1000, true, true, true});
  Require(!refused.ok(), "ODF-121 unsafe restart state was accepted");
  Require(refused.snapshot.diagnostic_code ==
              agents::kOnlineMaintenanceUnsafeRestartRefused,
          "ODF-121 unsafe restart diagnostic mismatch");
  Require(!refused.snapshot.published_visible,
          "ODF-121 unsafe restart became visible");
}

}  // namespace

int main() {
  (void)kGateSearchKey;
  TestSortedBuildRebuildCancelResumeAndUnsafePublish();
  TestNoSqlCompactionAndGenerationPublish();
  TestStatsBackfillCleanupAndCrashSafety();
  std::cout << "optimizer_deficiency_odf_121_gate=passed\n";
  return EXIT_SUCCESS;
}
