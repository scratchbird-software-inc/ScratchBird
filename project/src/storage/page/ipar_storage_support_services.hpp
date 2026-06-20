// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-IPAR-STORAGE-SUPPORT-SERVICES-ANCHOR
// Database-local storage support services. These planners and bounded queues
// preallocate, schedule, write physical pages, compact, verify, warm caches, and
// publish diagnostics only. Durable MGA transaction inventory remains the
// authority for commit, rollback, visibility, and cleanup horizons.

#include "async_page_io.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IparSupportQueueKind : u32 {
  worktable_scratch = 1,
  async_io = 2,
  dirty_writeback = 3,
  post_commit_diagnostics = 4,
  post_commit_maintenance = 5,
  page_zeroing = 6,
  worker_warmup = 7
};

enum class IparPageTemperature : u32 {
  cold = 1,
  warm = 2,
  hot = 3
};

enum class IparLockMode : u32 {
  shared = 1,
  update = 2,
  exclusive = 3
};

enum class IparPostCommitWorkKind : u32 {
  diagnostics = 1,
  maintenance = 2,
  index_cleanup = 3,
  large_value_cleanup = 4
};

enum class IparMaintenanceDebtFamily : u32 {
  storage_compaction = 1,
  dirty_writeback = 2,
  large_value = 3,
  index_shard = 4,
  scratch_cleanup = 5
};

struct IparSupportEvidenceField {
  std::string key;
  std::string value;
};

struct IparStorageSupportAuthorityBoundary {
  bool durable_transaction_inventory_authority = true;
  bool support_service_finality_authority = false;
  bool support_service_visibility_authority = false;
  bool parser_finality_authority = false;
  bool client_finality_authority = false;
  bool provider_finality_authority = false;
  bool publication_marker_finality_authority = false;
};

struct IparBoundedQueuePolicy {
  u64 max_items = 0;
  u64 max_bytes = 0;
};

struct IparSupportQueueItem {
  std::string item_id;
  IparSupportQueueKind queue_kind = IparSupportQueueKind::worktable_scratch;
  u64 bytes = 0;
  u64 priority = 0;
  bool requires_committed_transaction_evidence = false;
  bool committed_transaction_evidence_present = false;
  IparStorageSupportAuthorityBoundary authority;
};

struct IparBoundedQueuePlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool accepted = false;
  bool fail_closed = false;
  u64 accepted_count = 0;
  u64 refused_count = 0;
  u64 accepted_bytes = 0;
  std::vector<IparSupportQueueItem> accepted_items;
  std::vector<IparSupportQueueItem> refused_items;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparAppendLocalityCandidate {
  std::string page_id;
  std::string object_id;
  std::string filespace_id;
  u64 page_number = 0;
  u64 page_generation = 1;
  u64 free_bytes = 0;
  u64 append_cursor_distance = 0;
  IparPageTemperature temperature = IparPageTemperature::warm;
  bool active = true;
  bool zero_initialized = true;
};

struct IparAppendLocalityRequest {
  std::string object_id;
  u64 encoded_row_bytes = 0;
  std::string current_append_cursor_page_id;
  u64 preallocation_quantum_pages = 0;
  std::vector<IparAppendLocalityCandidate> candidates;
  IparStorageSupportAuthorityBoundary authority;
};

struct IparAppendLocalityPlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool selected = false;
  std::string selected_page_id;
  u64 selected_page_number = 0;
  u64 scheduled_preallocation_pages = 0;
  std::string locality_group;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct IparScratchSpaceRequest {
  IparBoundedQueuePolicy queue_policy;
  std::vector<IparSupportQueueItem> scratch_items;
};

struct IparShardMapRequest {
  std::string object_id;
  std::string index_id;
  u64 first_page = 0;
  u64 last_page = 0;
  u64 shard_count = 0;
  u64 latch_partition_count = 0;
  u64 worker_count = 0;
  IparStorageSupportAuthorityBoundary authority;
};

struct IparIndexPageShard {
  u64 shard_id = 0;
  u64 page_low = 0;
  u64 page_high = 0;
  u64 owner_worker_id = 0;
  u64 latch_partition = 0;
  std::string shard_key;
};

struct IparShardOwnershipMap {
  Status status;
  DiagnosticRecord diagnostic;
  bool built = false;
  std::vector<IparIndexPageShard> shards;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && built; }
};

struct IparDirtyPageCandidate {
  std::string page_id;
  std::string page_family;
  u64 page_number = 0;
  u64 dirty_epoch = 0;
  u64 dirty_bytes = 0;
  u64 age_microseconds = 0;
  u64 temperature_score = 0;
  u64 pin_count = 0;
  bool checkpoint_blocker = false;
  bool shutdown_blocker = false;
  bool transaction_inventory_page = false;
  IparStorageSupportAuthorityBoundary authority;
};

struct IparDirtyPagePriorityPolicy {
  u64 max_pages = 0;
  u64 max_bytes = 0;
};

struct IparDirtyPageAssignment {
  IparDirtyPageCandidate page;
  u64 priority_score = 0;
  u64 priority_rank = 0;
};

struct IparDirtyPagePriorityPlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  u64 selected_pages = 0;
  u64 selected_bytes = 0;
  u64 skipped_pinned_pages = 0;
  std::vector<IparDirtyPageAssignment> assignments;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparCacheEpoch {
  u64 catalog_generation = 0;
  u64 page_generation = 0;
  u64 codec_generation = 0;
  u64 filespace_generation = 0;
};

struct IparLargeValueStreamContext {
  std::string stream_id;
  std::string object_id;
  IparCacheEpoch epoch;
  u64 byte_count = 0;
  u64 open_count = 0;
  u64 last_use_tick = 0;
  u64 pin_count = 0;
};

struct IparLargeValueStreamContextCache {
  u64 max_entries = 0;
  u64 next_tick = 1;
  u64 hit_count = 0;
  u64 miss_count = 0;
  std::vector<IparLargeValueStreamContext> entries;
};

struct IparOrdinaryPageCodecContext {
  std::string codec_id;
  std::string page_family;
  IparCacheEpoch epoch;
  u64 page_size = 0;
  u64 last_use_tick = 0;
  u64 pin_count = 0;
};

struct IparOrdinaryPageCodecContextCache {
  u64 max_entries = 0;
  u64 next_tick = 1;
  u64 hit_count = 0;
  u64 miss_count = 0;
  std::vector<IparOrdinaryPageCodecContext> entries;
};

struct IparFilesystemFilespaceHandle {
  std::string filespace_id;
  std::string path;
  IparCacheEpoch epoch;
  std::string open_flags;
  u64 handle_ordinal = 0;
  u64 last_use_tick = 0;
  u64 pin_count = 0;
};

struct IparFilesystemFilespaceHandleCache {
  u64 max_entries = 0;
  u64 next_tick = 1;
  u64 next_handle_ordinal = 1;
  u64 hit_count = 0;
  u64 miss_count = 0;
  std::vector<IparFilesystemFilespaceHandle> entries;
};

template <typename TEntry>
struct IparCacheLookupResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool cache_hit = false;
  bool stale = false;
  TEntry entry;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && cache_hit && !stale; }
};

struct IparPageZeroingRequest {
  u64 first_page_number = 0;
  u64 page_count = 0;
  u64 page_size = 0;
  u64 max_queue_pages = 0;
  IparStorageSupportAuthorityBoundary authority;
};

struct IparZeroedPage {
  u64 page_number = 0;
  std::string ticket_id;
  std::vector<byte> bytes;
};

struct IparPageZeroingResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  u64 zeroed_count = 0;
  u64 refused_count = 0;
  std::vector<IparZeroedPage> zeroed_pages;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparPhysicalLocalitySample {
  std::string page_id;
  u64 page_number = 0;
  u64 recent_reads = 0;
  u64 recent_writes = 0;
  u64 age_microseconds = 0;
  bool dirty = false;
};

struct IparPhysicalLocalityDecision {
  std::string page_id;
  IparPageTemperature temperature = IparPageTemperature::cold;
  std::string target_region;
  u64 temperature_score = 0;
};

struct IparPhysicalLocalityPlan {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<IparPhysicalLocalityDecision> decisions;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct IparDmlLockIntent {
  std::string transaction_id;
  std::string object_id;
  std::string key_low;
  std::string key_high;
  IparLockMode mode = IparLockMode::shared;
  u64 local_transaction_id = 0;
};

struct IparDmlLockPartitionAssignment {
  IparDmlLockIntent intent;
  u64 partition_id = 0;
  u64 latch_id = 0;
};

struct IparDmlLockPartitionPlan {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<IparDmlLockPartitionAssignment> assignments;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct IparWaitEdge {
  std::string waiter_transaction_id;
  std::string holder_transaction_id;
};

struct IparConflictPredictionRequest {
  std::vector<IparDmlLockIntent> intents;
  std::vector<IparWaitEdge> existing_wait_edges;
};

struct IparConflictPrediction {
  std::string waiter_transaction_id;
  std::string holder_transaction_id;
  std::string object_id;
  std::string reason;
};

struct IparConflictPredictionPlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool conflict_predicted = false;
  bool deadlock_predicted = false;
  bool serialization_required = false;
  std::vector<IparConflictPrediction> conflicts;
  std::vector<IparWaitEdge> serialization_edges;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct IparLockPreacquisitionRequest {
  u64 max_intents = 0;
  u64 partition_count = 0;
  std::vector<IparDmlLockIntent> intents;
};

struct IparLockPreacquisitionPlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool bounded = false;
  u64 selected_count = 0;
  u64 overflow_count = 0;
  std::vector<IparDmlLockPartitionAssignment> selected;
  std::vector<IparDmlLockIntent> overflow;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct IparPostCommitWorkItem {
  std::string item_id;
  IparPostCommitWorkKind kind = IparPostCommitWorkKind::diagnostics;
  u64 bytes = 0;
  bool requires_commit_evidence = true;
  bool commit_evidence_present = false;
  IparStorageSupportAuthorityBoundary authority;
};

struct IparPostCommitQueuePolicy {
  u64 max_items_per_queue = 0;
  u64 max_bytes_per_queue = 0;
};

struct IparPostCommitQueuePlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  std::vector<IparPostCommitWorkItem> diagnostics_queue;
  std::vector<IparPostCommitWorkItem> maintenance_queue;
  std::vector<IparPostCommitWorkItem> index_queue;
  std::vector<IparPostCommitWorkItem> large_value_queue;
  std::vector<IparPostCommitWorkItem> refused_items;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparNumaCpuAffinityRequest {
  u64 numa_node_count = 0;
  u64 cpu_count = 0;
  u64 worker_count = 0;
  u64 preferred_start_cpu = 0;
};

struct IparNumaCpuAssignment {
  u64 worker_id = 0;
  u64 numa_node = 0;
  u64 cpu_id = 0;
};

struct IparNumaCpuAffinityPlan {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<IparNumaCpuAssignment> assignments;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct IparWorkerPoolWarmupRequest {
  u64 target_workers = 0;
  u64 min_workers = 0;
  u64 max_workers = 0;
  IparBoundedQueuePolicy queue_policy;
  IparNumaCpuAffinityRequest affinity;
  IparStorageSupportAuthorityBoundary authority;
};

struct IparWorkerPoolWarmupPlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  u64 warmed_workers = 0;
  u64 refused_workers = 0;
  std::vector<IparSupportQueueItem> warmup_tickets;
  IparNumaCpuAffinityPlan affinity_plan;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparMaintenanceDebtEntry {
  std::string debt_id;
  IparMaintenanceDebtFamily family = IparMaintenanceDebtFamily::storage_compaction;
  u64 debt_units = 0;
  u64 debt_bytes = 0;
  u64 age_microseconds = 0;
  u64 priority_boost = 0;
  bool destructive_cleanup = false;
  bool cleanup_horizon_authoritative = false;
  bool source_authoritative = true;
  IparStorageSupportAuthorityBoundary authority;
};

struct IparMaintenanceDebtPolicy {
  u64 max_scheduled_items = 0;
  u64 max_scheduled_units = 0;
  bool engine_mga_authoritative = false;
};

struct IparMaintenanceDebtAssignment {
  IparMaintenanceDebtEntry entry;
  u64 score = 0;
  u64 scheduled_units = 0;
  bool scheduled = false;
  std::string diagnostic_code;
};

struct IparMaintenanceDebtPlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  u64 scheduled_count = 0;
  u64 retained_count = 0;
  u64 scheduled_units = 0;
  std::vector<IparMaintenanceDebtAssignment> assignments;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct IparStorageMaintenanceSupportRequest {
  IparBoundedQueuePolicy queue_policy;
  std::vector<IparSupportQueueItem> maintenance_items;
  IparMaintenanceDebtPolicy debt_policy;
  std::vector<IparMaintenanceDebtEntry> debt_entries;
  IparWorkerPoolWarmupRequest warmup_request;
};

struct IparStorageMaintenanceSupportPlan {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  IparBoundedQueuePlan queue_plan;
  IparMaintenanceDebtPlan debt_plan;
  IparWorkerPoolWarmupPlan warmup_plan;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* IparSupportQueueKindName(IparSupportQueueKind kind);
const char* IparPageTemperatureName(IparPageTemperature temperature);
const char* IparLockModeName(IparLockMode mode);
const char* IparPostCommitWorkKindName(IparPostCommitWorkKind kind);
const char* IparMaintenanceDebtFamilyName(IparMaintenanceDebtFamily family);

bool IparStorageSupportAuthorityBoundarySafe(
    const IparStorageSupportAuthorityBoundary& authority);
DiagnosticRecord MakeIparStorageSupportDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail = {});

IparBoundedQueuePlan PlanIparBoundedSupportQueue(
    const IparBoundedQueuePolicy& policy,
    const std::vector<IparSupportQueueItem>& items);
IparAppendLocalityPlan PlanIparAppendLocality(
    const IparAppendLocalityRequest& request);
IparBoundedQueuePlan PlanIparWorktableScratchAgent(
    const IparScratchSpaceRequest& request);
IparShardOwnershipMap BuildIparObjectIndexPageShardOwnershipMap(
    const IparShardMapRequest& request);
const IparIndexPageShard* FindIparShardForPage(
    const IparShardOwnershipMap& map,
    u64 page_number);
IparDirtyPagePriorityPlan PlanIparDirtyPagePriority(
    const IparDirtyPagePriorityPolicy& policy,
    const std::vector<IparDirtyPageCandidate>& candidates);

Status PutIparLargeValueStreamContext(
    IparLargeValueStreamContextCache* cache,
    IparLargeValueStreamContext entry);
IparCacheLookupResult<IparLargeValueStreamContext>
LookupIparLargeValueStreamContext(IparLargeValueStreamContextCache* cache,
                                  const std::string& stream_id,
                                  const IparCacheEpoch& epoch);
u64 InvalidateStaleIparLargeValueStreamContexts(
    IparLargeValueStreamContextCache* cache,
    const IparCacheEpoch& epoch);

Status PutIparOrdinaryPageCodecContext(
    IparOrdinaryPageCodecContextCache* cache,
    IparOrdinaryPageCodecContext entry);
IparCacheLookupResult<IparOrdinaryPageCodecContext>
LookupIparOrdinaryPageCodecContext(IparOrdinaryPageCodecContextCache* cache,
                                   const std::string& codec_id,
                                   const std::string& page_family,
                                   const IparCacheEpoch& epoch);
u64 InvalidateStaleIparOrdinaryPageCodecContexts(
    IparOrdinaryPageCodecContextCache* cache,
    const IparCacheEpoch& epoch);

Status PutIparFilesystemFilespaceHandle(
    IparFilesystemFilespaceHandleCache* cache,
    IparFilesystemFilespaceHandle entry);
IparCacheLookupResult<IparFilesystemFilespaceHandle>
LookupIparFilesystemFilespaceHandle(IparFilesystemFilespaceHandleCache* cache,
                                    const std::string& filespace_id,
                                    const std::string& path,
                                    const IparCacheEpoch& epoch);
u64 InvalidateStaleIparFilesystemFilespaceHandles(
    IparFilesystemFilespaceHandleCache* cache,
    const IparCacheEpoch& epoch);

IparPageZeroingResult PlanIparPageZeroing(
    const IparPageZeroingRequest& request);
IparPhysicalLocalityPlan PlanIparPhysicalLocalityTemperature(
    const std::vector<IparPhysicalLocalitySample>& samples);
IparDmlLockPartitionPlan PlanIparDmlLockLatchPartitioning(
    u64 partition_count,
    u64 latch_count,
    const std::vector<IparDmlLockIntent>& intents);
IparConflictPredictionPlan PredictIparDeadlockAndConflicts(
    const IparConflictPredictionRequest& request);
IparLockPreacquisitionPlan PlanIparLockIntentPreacquisition(
    const IparLockPreacquisitionRequest& request);
IparPostCommitQueuePlan PlanIparPostCommitQueueSeparation(
    const IparPostCommitQueuePolicy& policy,
    const std::vector<IparPostCommitWorkItem>& items);
IparNumaCpuAffinityPlan PlanIparNumaCpuAffinity(
    const IparNumaCpuAffinityRequest& request);
IparWorkerPoolWarmupPlan PlanIparParallelWorkerPoolWarmup(
    const IparWorkerPoolWarmupRequest& request);
IparMaintenanceDebtPlan PlanIparMaintenanceDebt(
    const IparMaintenanceDebtPolicy& policy,
    const std::vector<IparMaintenanceDebtEntry>& entries);
IparStorageMaintenanceSupportPlan PlanIparStorageMaintenanceSupport(
    const IparStorageMaintenanceSupportRequest& request);

}  // namespace scratchbird::storage::page
