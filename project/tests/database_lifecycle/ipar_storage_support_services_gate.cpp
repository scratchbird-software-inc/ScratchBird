// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_storage_support_services.hpp"
#include "resource_governance_admission.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace page = scratchbird::storage::page;
using scratchbird::core::platform::byte;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ipar_storage_support_services_gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view item) {
  for (const auto& entry : evidence) {
    if (entry == item) {
      return true;
    }
  }
  return false;
}

page::IparSupportQueueItem QueueItem(std::string id,
                                     page::IparSupportQueueKind kind,
                                     std::uint64_t bytes,
                                     std::uint64_t priority) {
  page::IparSupportQueueItem item;
  item.item_id = std::move(id);
  item.queue_kind = kind;
  item.bytes = bytes;
  item.priority = priority;
  return item;
}

page::IparCacheEpoch Epoch(std::uint64_t value) {
  return {value, value, value, value};
}

agents::ResourceGovernanceQuotaVector Quota(std::int64_t value) {
  agents::ResourceGovernanceQuotaVector quota;
  quota.memory_bytes = value;
  quota.device_memory_bytes = value;
  quota.pinned_memory_bytes = value;
  quota.io_bytes = value;
  quota.io_ops = value;
  quota.worker_threads = value;
  quota.backlog_items = value;
  quota.candidate_rows = value;
  quota.cache_entries = value;
  quota.batch_rows = value;
  quota.fragments = value;
  quota.lanes = value;
  quota.time_budget_microseconds = value;
  return quota;
}

agents::ResourceGovernanceAdmissionRequest AsyncGovernance() {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = "ipar-storage-support-async";
  request.descriptor.descriptor_id = "ipar-storage-support-async-policy";
  request.descriptor.family = agents::ResourceGovernanceFamily::kAsyncPageIo;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime.ipar.storage_support";
  request.descriptor.descriptor_generation = 7;
  request.descriptor.expected_generation = 7;
  request.descriptor.limits = Quota(1024);
  request.descriptor.over_limit_action = agents::ResourceGovernanceAction::kFailClosed;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.requested = Quota(1);
  return request;
}

page::AsyncPageIoOperation WriteOperation(std::string id,
                                          std::uint64_t page_number,
                                          std::vector<byte> payload) {
  page::AsyncPageIoOperation operation;
  operation.kind = page::AsyncPageIoOperationKind::kWritePage;
  operation.operation_id = std::move(id);
  operation.page_number = page_number;
  operation.page_generation = 1;
  operation.descriptor_generation = 9;
  operation.payload = std::move(payload);
  operation.publication_marker_required = false;
  operation.filespace_class = "hot_row";
  operation.physical_plan_id = "ipar-storage-support-plan";
  return operation;
}

void ProveStorageMaintenanceInfrastructure() {
  page::IparStorageMaintenanceSupportRequest request;
  request.queue_policy = {2, 8};
  request.maintenance_items.push_back(
      QueueItem("compact-a", page::IparSupportQueueKind::post_commit_maintenance, 4, 10));
  request.maintenance_items.push_back(
      QueueItem("verify-b", page::IparSupportQueueKind::post_commit_maintenance, 4, 9));
  request.maintenance_items.push_back(
      QueueItem("diagnostic-c", page::IparSupportQueueKind::post_commit_diagnostics, 4, 8));

  request.debt_policy.engine_mga_authoritative = true;
  request.debt_policy.max_scheduled_items = 1;
  request.debt_policy.max_scheduled_units = 8;
  request.debt_entries.push_back({"debt-index-shard",
                                  page::IparMaintenanceDebtFamily::index_shard,
                                  6,
                                  4096,
                                  2000,
                                  20,
                                  false,
                                  false,
                                  true});
  request.debt_entries.push_back({"debt-large-value-horizon",
                                  page::IparMaintenanceDebtFamily::large_value,
                                  3,
                                  8192,
                                  1000,
                                  0,
                                  true,
                                  false,
                                  true});

  request.warmup_request.target_workers = 3;
  request.warmup_request.min_workers = 1;
  request.warmup_request.max_workers = 3;
  request.warmup_request.queue_policy = {2, 2};
  request.warmup_request.affinity = {2, 4, 0, 1};

  const auto plan = page::PlanIparStorageMaintenanceSupport(request);
  Require(plan.ok(), "IPAR-P3-14 storage support infrastructure should plan");
  Require(plan.queue_plan.diagnostic.diagnostic_code == "SB_IPAR_SUPPORT_QUEUE_BOUNDED",
          "IPAR-P3-14 bounded queue diagnostic drifted");
  Require(plan.queue_plan.accepted_count == 2 && plan.queue_plan.refused_count == 1,
          "IPAR-P3-14 bounded maintenance queue counts drifted");
  Require(plan.debt_plan.scheduled_count == 1 && plan.debt_plan.retained_count == 1,
          "IPAR-P6-34 debt ledger schedule/retain counts drifted");
  Require(plan.warmup_plan.diagnostic.diagnostic_code ==
              "SB_IPAR_WORKER_WARMUP_BOUNDED",
          "IPAR-P6-33 worker warmup bounded diagnostic drifted");
  Require(plan.warmup_plan.warmed_workers == 2 &&
              plan.warmup_plan.refused_workers == 1,
          "IPAR-P6-33 worker warmup counts drifted");
  Require(HasEvidence(plan.evidence,
                      "ipar.storage_maintenance.finality_authority=false"),
          "IPAR-P3-14 support infrastructure authority evidence missing");
}

void ProveAppendLocalityAndScratch() {
  page::IparAppendLocalityRequest request;
  request.object_id = "table-1";
  request.encoded_row_bytes = 96;
  request.current_append_cursor_page_id = "page-hot-current";
  request.preallocation_quantum_pages = 4;
  request.candidates.push_back({"page-cold-far", "table-1", "fs-1", 40, 1, 4096,
                                30, page::IparPageTemperature::cold, true, true});
  request.candidates.push_back({"page-hot-current", "table-1", "fs-1", 41, 1, 512,
                                0, page::IparPageTemperature::hot, true, true});
  const auto plan = page::PlanIparAppendLocality(request);
  Require(plan.selected && plan.selected_page_id == "page-hot-current",
          "IPAR-P3-15 append cursor did not preserve current page locality");
  Require(plan.locality_group == "fs-1:table-1:append:2",
          "IPAR-P3-15 locality group drifted");

  page::IparScratchSpaceRequest scratch;
  scratch.queue_policy = {1, 128};
  scratch.scratch_items.push_back(
      QueueItem("scratch-hash", page::IparSupportQueueKind::worktable_scratch, 64, 10));
  scratch.scratch_items.push_back(
      QueueItem("scratch-sort", page::IparSupportQueueKind::worktable_scratch, 64, 9));
  const auto scratch_plan = page::PlanIparWorktableScratchAgent(scratch);
  Require(scratch_plan.diagnostic.diagnostic_code == "SB_IPAR_SUPPORT_QUEUE_BOUNDED",
          "IPAR-P3-16 scratch-space bounded diagnostic drifted");
  Require(scratch_plan.accepted_count == 1 && scratch_plan.refused_count == 1,
          "IPAR-P3-16 scratch-space bounded queue counts drifted");
}

void ProveShardAndDirtyManagers() {
  page::IparShardMapRequest map_request;
  map_request.object_id = "table-1";
  map_request.index_id = "idx-1";
  map_request.first_page = 100;
  map_request.last_page = 115;
  map_request.shard_count = 4;
  map_request.latch_partition_count = 2;
  map_request.worker_count = 2;
  const auto map = page::BuildIparObjectIndexPageShardOwnershipMap(map_request);
  Require(map.ok() && map.shards.size() == 4,
          "IPAR-P3-17 shard ownership map did not build four shards");
  const auto* shard = page::FindIparShardForPage(map, 110);
  Require(shard != nullptr && shard->shard_id == 2 &&
              shard->owner_worker_id == 0 && shard->latch_partition == 0,
          "IPAR-P3-17 shard ownership for page 110 drifted");

  std::vector<page::IparDirtyPageCandidate> dirty;
  dirty.push_back({"data-hot", "data", 7, 4, 4096, 1000, 20, 0,
                   false, true, false});
  dirty.push_back({"txn-inventory", "transaction", 2, 4, 4096, 500, 1, 0,
                   false, false, true});
  dirty.push_back({"pinned", "data", 9, 4, 4096, 1000000, 200, 1,
                   true, false, false});
  const auto priority = page::PlanIparDirtyPagePriority({2, 8192}, dirty);
  Require(priority.ok() && priority.assignments.size() == 2,
          "IPAR-P3-18 dirty-page priority should select two unpinned pages");
  Require(priority.assignments.front().page.page_id == "data-hot",
          "IPAR-P3-18 shutdown dirty page should have first priority");
  Require(priority.skipped_pinned_pages == 1,
          "IPAR-P3-18 pinned dirty page skip count drifted");
  Require(priority.diagnostic.diagnostic_code == "SB_IPAR_DIRTY_PRIORITY_PLANNED",
          "IPAR-P3-18 dirty priority diagnostic drifted");
}

void ProveCachesAndZeroing() {
  page::IparLargeValueStreamContextCache large_cache;
  large_cache.max_entries = 2;
  Require(page::PutIparLargeValueStreamContext(
              &large_cache, {"lv-1", "table-1", Epoch(1), 8192, 1})
              .ok(),
          "IPAR-P3-19 large-value stream context put failed");
  const auto large_hit =
      page::LookupIparLargeValueStreamContext(&large_cache, "lv-1", Epoch(1));
  Require(large_hit.ok(), "IPAR-P3-19 large-value stream cache did not hit");
  const auto large_stale =
      page::LookupIparLargeValueStreamContext(&large_cache, "lv-1", Epoch(2));
  Require(!large_stale.ok() && large_stale.stale &&
              large_stale.diagnostic.diagnostic_code == "SB_IPAR_CACHE_STALE",
          "IPAR-P3-19 large-value stream stale diagnostic drifted");
  Require(page::InvalidateStaleIparLargeValueStreamContexts(&large_cache,
                                                            Epoch(2)) == 1,
          "IPAR-P3-19 large-value stale invalidation count drifted");

  page::IparOrdinaryPageCodecContextCache codec_cache;
  codec_cache.max_entries = 2;
  Require(page::PutIparOrdinaryPageCodecContext(
              &codec_cache, {"codec-row-v1", "data", Epoch(3), 16384})
              .ok(),
          "IPAR-P3-21 ordinary page codec put failed");
  Require(page::LookupIparOrdinaryPageCodecContext(
              &codec_cache, "codec-row-v1", "data", Epoch(3))
              .ok(),
          "IPAR-P3-21 ordinary page codec cache did not hit");

  page::IparFilesystemFilespaceHandleCache handle_cache;
  handle_cache.max_entries = 2;
  Require(page::PutIparFilesystemFilespaceHandle(
              &handle_cache, {"fs-1", "/tmp/sb-ipar-fs-1", Epoch(4), "rw"})
              .ok(),
          "IPAR-P3-22 filespace handle put failed");
  const auto handle_hit = page::LookupIparFilesystemFilespaceHandle(
      &handle_cache, "fs-1", "/tmp/sb-ipar-fs-1", Epoch(4));
  Require(handle_hit.ok() && handle_hit.entry.handle_ordinal == 1,
          "IPAR-P3-22 filespace handle cache did not hit first handle");

  const auto zeroed = page::PlanIparPageZeroing({200, 3, 32, 2});
  Require(zeroed.ok() && zeroed.zeroed_count == 2 && zeroed.refused_count == 1,
          "IPAR-P3-23 page zeroing bounded queue counts drifted");
  Require(zeroed.diagnostic.diagnostic_code == "SB_IPAR_ZEROING_QUEUE_BOUNDED",
          "IPAR-P3-23 page zeroing bounded diagnostic drifted");
  for (const auto& zeroed_page : zeroed.zeroed_pages) {
    for (const auto value : zeroed_page.bytes) {
      Require(value == byte{0}, "IPAR-P3-23 zeroed page contains non-zero byte");
    }
  }
}

void ProveAsyncIoTicketing() {
  page::AsyncPageIoRequest request;
  request.route_generation = 9;
  request.capabilities.async_write_supported = true;
  request.capabilities.async_fsync_supported = true;
  request.capabilities.write_combining_supported = true;
  request.capabilities.publication_marker_supported = true;
  request.capabilities.durable_sync_fence_supported = true;
  request.capabilities.max_batch_operations = 4;
  request.capabilities.max_batch_bytes = 128;
  request.capabilities.max_combined_writes = 4;
  request.policy.estimated_sync_micros = 100;
  request.policy.estimated_async_micros = 10;
  request.resource_governance = AsyncGovernance();
  request.operations.push_back(WriteOperation("write-a", 300, {1, 2, 3, 4}));
  request.operations.push_back(WriteOperation("write-b", 300, {5, 6, 7, 8}));
  page::AsyncPageIoOperation fsync;
  fsync.kind = page::AsyncPageIoOperationKind::kFsync;
  fsync.operation_id = "sync-fence";
  fsync.page_generation = 1;
  fsync.descriptor_generation = 9;
  fsync.publication_marker_required = false;
  request.operations.push_back(fsync);
  page::AsyncPageIoOperation marker;
  marker.kind = page::AsyncPageIoOperationKind::kPublicationMarker;
  marker.operation_id = "publish-marker";
  marker.page_generation = 1;
  marker.descriptor_generation = 9;
  marker.publication_marker = "commit-evidence:42";
  marker.expected_publication_marker = "commit-evidence:42";
  request.operations.push_back(marker);

  std::uint64_t write_calls = 0;
  page::AsyncPageIoRouteBackend backend;
  backend.write_page = [&](const page::AsyncPageIoOperation&) {
    ++write_calls;
    page::AsyncPageIoBackendResult result;
    result.status = {scratchbird::core::platform::StatusCode::ok,
                     scratchbird::core::platform::Severity::info,
                     scratchbird::core::platform::Subsystem::storage_page};
    return result;
  };
  backend.fsync = [&]() {
    page::AsyncPageIoBackendResult result;
    result.status = {scratchbird::core::platform::StatusCode::ok,
                     scratchbird::core::platform::Severity::info,
                     scratchbird::core::platform::Subsystem::storage_page};
    return result;
  };

  const auto executed = page::ExecuteAsyncPageIoBatch(request, backend);
  Require(executed.ok(), "IPAR-P3-20 async I/O batch did not execute");
  Require(write_calls == 1, "IPAR-P3-20 write combining did not collapse duplicate page");
  Require(executed.counters.write_tickets_issued == 1 &&
              executed.counters.write_tickets_completed == 1 &&
              executed.counters.write_ticket_waits == 1,
          "IPAR-P3-20 async write ticket counters drifted");
  Require(executed.publication_marker_published,
          "IPAR-P3-20 async publication marker was not published after sync");
  Require(HasEvidence(executed.evidence,
                      "async_page_io.write_ticket.commit_wait_satisfied=true"),
          "IPAR-P3-20 async ticket commit wait evidence missing");
}

void ProveLocksConflictsAndQueues() {
  std::vector<page::IparDmlLockIntent> intents = {
      {"tx1", "table-1", "10", "20", page::IparLockMode::exclusive, 1},
      {"tx2", "table-1", "15", "16", page::IparLockMode::shared, 2},
      {"tx3", "table-2", "1", "1", page::IparLockMode::shared, 3}};
  const auto partitions =
      page::PlanIparDmlLockLatchPartitioning(4, 2, intents);
  Require(partitions.ok() && partitions.assignments.size() == 3,
          "IPAR-P6-12 DML lock/latch partition assignment count drifted");
  Require(partitions.assignments.front().latch_id < 2,
          "IPAR-P6-12 latch assignment outside policy bounds");

  const auto serialized =
      page::PredictIparDeadlockAndConflicts({intents, {}});
  Require(serialized.conflict_predicted && serialized.serialization_required &&
              !serialized.deadlock_predicted,
          "IPAR-P6-20 conflict serialization prediction drifted");
  Require(serialized.diagnostic.diagnostic_code == "SB_IPAR_CONFLICT_SERIALIZED",
          "IPAR-P6-20 conflict serialization diagnostic drifted");

  const auto deadlock =
      page::PredictIparDeadlockAndConflicts({intents, {{"tx1", "tx2"}}});
  Require(deadlock.deadlock_predicted &&
              deadlock.diagnostic.diagnostic_code ==
                  "SB_IPAR_CONFLICT_DEADLOCK_PREDICTED",
          "IPAR-P6-20 deadlock prediction diagnostic drifted");

  const auto preacquire =
      page::PlanIparLockIntentPreacquisition({1, 4, intents});
  Require(preacquire.bounded && preacquire.selected_count == 1 &&
              preacquire.overflow_count == 2,
          "IPAR-P6-32 lock intent pre-acquisition bounds drifted");

  page::IparPostCommitQueuePolicy policy{1, 64};
  std::vector<page::IparPostCommitWorkItem> post_commit;
  post_commit.push_back({"diag-1", page::IparPostCommitWorkKind::diagnostics,
                         16, true, true});
  post_commit.push_back({"diag-2", page::IparPostCommitWorkKind::diagnostics,
                         16, true, true});
  post_commit.push_back({"maint-1", page::IparPostCommitWorkKind::maintenance,
                         16, true, true});
  post_commit.push_back({"index-no-commit",
                         page::IparPostCommitWorkKind::index_cleanup,
                         16, true, false});
  const auto queues = page::PlanIparPostCommitQueueSeparation(policy, post_commit);
  Require(queues.ok() &&
              queues.diagnostics_queue.size() == 1 &&
              queues.maintenance_queue.size() == 1 &&
              queues.refused_items.size() == 2,
          "IPAR-P6-21 post-commit queue separation counts drifted");
  Require(queues.diagnostic.diagnostic_code == "SB_IPAR_POST_COMMIT_QUEUE_BOUNDED",
          "IPAR-P6-21 post-commit bounded diagnostic drifted");
}

void ProveLocalityAffinityAndAuthority() {
  const auto locality = page::PlanIparPhysicalLocalityTemperature(
      {{"hot-page", 1, 4, 30, 100, true},
       {"warm-page", 2, 10, 1, 2000000, false},
       {"cold-page", 3, 1, 0, 9000000, false}});
  Require(locality.ok() && locality.decisions.size() == 3,
          "IPAR-P6-28 physical locality decision count drifted");
  Require(locality.decisions[0].temperature == page::IparPageTemperature::hot &&
              locality.decisions[0].target_region == "hot_append_region",
          "IPAR-P6-28 hot page temperature classification drifted");

  const auto affinity = page::PlanIparNumaCpuAffinity({2, 4, 4, 1});
  Require(affinity.ok() && affinity.assignments.size() == 4,
          "IPAR-P6-29 NUMA affinity assignment count drifted");
  Require(affinity.assignments[0].cpu_id == 1 &&
              affinity.assignments[0].numa_node == 1,
          "IPAR-P6-29 first NUMA/CPU assignment drifted");

  page::IparSupportQueueItem unsafe =
      QueueItem("unsafe-finality", page::IparSupportQueueKind::dirty_writeback, 1, 1);
  unsafe.authority.support_service_finality_authority = true;
  const auto refused =
      page::PlanIparBoundedSupportQueue({4, 16}, {unsafe});
  Require(refused.fail_closed &&
              refused.diagnostic.diagnostic_code ==
                  "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
          "MGA authority drift refusal diagnostic changed");
}

}  // namespace

int main() {
  ProveStorageMaintenanceInfrastructure();
  ProveAppendLocalityAndScratch();
  ProveShardAndDirtyManagers();
  ProveCachesAndZeroing();
  ProveAsyncIoTicketing();
  ProveLocksConflictsAndQueues();
  ProveLocalityAffinityAndAuthority();
  std::cout << "ipar_storage_support_services_gate=passed\n";
  return 0;
}
