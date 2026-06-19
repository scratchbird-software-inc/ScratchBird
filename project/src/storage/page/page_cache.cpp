// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_cache.hpp"

#include "metric_contracts.hpp"
#include "page_registry.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::storage::page {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::UuidToString;
using scratchbird::core::memory::DefaultMemoryManager;
using scratchbird::core::memory::MemoryCategory;
using scratchbird::core::memory::MemoryLifetime;
using scratchbird::core::memory::MemoryManager;
using scratchbird::core::memory::MemoryTag;
using scratchbird::core::memory::PageBufferRequest;
using scratchbird::core::memory::ScopedPageBuffer;
using scratchbird::storage::disk::IsSupportedDatabasePageSize;
using scratchbird::storage::disk::PageTypeName;

Status CacheOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status CacheErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

bool IsTypedEngineIdentity(const TypedUuid& uuid, UuidKind kind) {
  return uuid.kind == kind && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

std::size_t ContextIndex(PageCacheIoContext context) {
  switch (context) {
    case PageCacheIoContext::normal: return 0;
    case PageCacheIoContext::index_read: return 1;
    case PageCacheIoContext::bulk_read: return 2;
    case PageCacheIoContext::bulk_write: return 3;
    case PageCacheIoContext::vacuum_cleanup: return 4;
    case PageCacheIoContext::index_build: return 5;
    case PageCacheIoContext::strict_bulk_load: return 6;
  }
  return 0;
}

PageCacheIoContext ContextFromIndex(std::size_t index) {
  switch (index) {
    case 0: return PageCacheIoContext::normal;
    case 1: return PageCacheIoContext::index_read;
    case 2: return PageCacheIoContext::bulk_read;
    case 3: return PageCacheIoContext::bulk_write;
    case 4: return PageCacheIoContext::vacuum_cleanup;
    case 5: return PageCacheIoContext::index_build;
    case 6: return PageCacheIoContext::strict_bulk_load;
  }
  return PageCacheIoContext::normal;
}

bool IsBulkContext(PageCacheIoContext context) {
  return context != PageCacheIoContext::normal &&
         context != PageCacheIoContext::index_read;
}

bool IsReadResidencyContext(PageCacheIoContext context) {
  return context == PageCacheIoContext::normal ||
         context == PageCacheIoContext::index_read;
}

bool IsIndexPageType(PageType page_type) {
  return page_type == PageType::index_btree ||
         page_type == PageType::index_btree_root ||
         page_type == PageType::index_btree_branch ||
         page_type == PageType::index_btree_leaf ||
         page_type == PageType::index_btree_posting;
}

PageCacheIoContext DefaultContextForEntry(const PageCacheEntry& entry) {
  return IsIndexPageType(entry.page_type) ? PageCacheIoContext::index_read
                                          : PageCacheIoContext::normal;
}

PageCacheResult Error(std::string diagnostic_code, std::string message_key, std::string detail = {}) {
  PageCacheResult result;
  result.status = CacheErrorStatus();
  result.diagnostic = MakePageCacheDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

// MMCH_PAGE_CACHE_FRAME_OWNERSHIP
std::string PageFrameKey(const TypedUuid& page_uuid) {
  return UuidToString(page_uuid.value);
}

u64 DeterministicFrameShardHash(std::string_view value) {
  u64 hash = 1469598103934665603ull;
  for (const unsigned char ch : value) {
    hash ^= static_cast<u64>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::size_t FrameShardIndexForKey(std::string_view key) {
  return static_cast<std::size_t>(
      DeterministicFrameShardHash(key) % kPageCacheFrameShardCount);
}

std::string PageFrameScopeId(const PageCacheEntry& entry,
                             PageCacheIoContext context,
                             std::size_t shard_index) {
  return "database=" + UuidToString(entry.database_uuid.value) +
         ";filespace=" + UuidToString(entry.filespace_uuid.value) +
         ";context=" + PageCacheIoContextName(context) +
         ";page=" + UuidToString(entry.page_uuid.value) +
         ";shard=" + std::to_string(shard_index);
}

void AppendFrameOwnershipEvidence(std::vector<std::string>* evidence,
                                  const PageCacheSnapshot& snapshot) {
  if (evidence == nullptr) {
    return;
  }
  evidence->push_back("MMCH_PAGE_CACHE_FRAME_OWNERSHIP");
  evidence->push_back("page_cache.memory_manager_frames_bound=" +
                      std::string(snapshot.memory_manager_frames_bound ? "true" : "false"));
  evidence->push_back("page_cache.memory_manager_frame_count=" +
                      std::to_string(snapshot.memory_manager_frame_count));
  evidence->push_back("page_cache.memory_manager_frame_bytes=" +
                      std::to_string(snapshot.memory_manager_frame_bytes));
  evidence->push_back("MMCH_PAGE_CACHE_SHARDED_FRAME_TABLE");
  evidence->push_back("page_cache.frame_shard_count=" +
                      std::to_string(snapshot.frame_shard_count));
  evidence->push_back("page_cache.active_frame_shard_count=" +
                      std::to_string(snapshot.active_frame_shard_count));
  evidence->push_back("page_cache.sharded_snapshot_order=shard_id_ascending");
  evidence->push_back(
      "page_cache.frame_authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority");
  evidence->push_back(
      "page_cache.enterprise_frame_authority_scope=evidence_only_not_transaction_finality_row_visibility_security_authorization_recovery_parser_reference_wal_benchmark_optimizer_plan_index_finality_or_agent_action_authority");
  evidence->push_back(
      "page_cache.cluster_scope=external_cluster_provider_only_no_local_cluster_page_cache_authority");
}

MemoryTag PageCacheFrameTag(const PageCacheEntry& entry,
                            PageCacheIoContext context) {
  MemoryTag tag;
  tag.subsystem = Subsystem::storage_page;
  tag.purpose = "page_cache_resident_frame";
  tag.category = MemoryCategory::page_buffer;
  tag.lifetime = MemoryLifetime::page_buffer;
  tag.owner = "page_cache:" + UuidToString(entry.database_uuid.value);
  tag.context_id = PageFrameKey(entry.page_uuid);
  tag.database_id = UuidToString(entry.database_uuid.value);
  tag.callsite = std::string("storage.page_cache.") + PageCacheIoContextName(context);
  return tag;
}

scratchbird::core::memory::ScopedPageBufferResult AllocateResidentFrame(
    PageCacheLedger* ledger,
    const PageCacheEntry& entry,
    PageCacheIoContext context) {
  MemoryManager* manager =
      ledger != nullptr && ledger->memory_manager != nullptr
          ? ledger->memory_manager
          : &DefaultMemoryManager();
  PageBufferRequest request;
  request.page_size = entry.page_size;
  request.page_count = 1;
  request.alignment = entry.page_size;
  request.tag = PageCacheFrameTag(entry, context);
  return manager->AllocateScopedPageBuffer(std::move(request));
}

PageCacheResidentFrame MakeResidentFrameRecord(PageCacheEntry entry,
                                               PageCacheIoContext context,
                                               std::size_t shard_index,
                                               ScopedPageBuffer&& buffer) {
  PageCacheResidentFrame record;
  record.buffer = std::move(buffer);
  record.database_uuid = entry.database_uuid;
  record.filespace_uuid = entry.filespace_uuid;
  record.page_uuid = entry.page_uuid;
  record.io_context = context;
  record.page_number = entry.page_number;
  record.page_generation = entry.page_generation;
  record.page_size = entry.page_size;
  record.scope_id = PageFrameScopeId(entry, context, shard_index);
  return record;
}

void RecordFrameAllocationFailureUnlocked(PageCacheLedger* ledger, const TypedUuid& page_uuid) {
  if (ledger == nullptr) {
    return;
  }
  const auto key = PageFrameKey(page_uuid);
  auto& shard = ledger->frame_shards[FrameShardIndexForKey(key)];
  std::lock_guard<std::mutex> shard_lock(shard.mutex);
  ++shard.allocation_failure_count;
  ++ledger->frame_allocation_failure_count;
}

std::size_t InsertResidentFrameUnlocked(PageCacheLedger* ledger,
                                        const PageCacheEntry& entry,
                                        PageCacheIoContext context,
                                        ScopedPageBuffer&& buffer) {
  const auto key = PageFrameKey(entry.page_uuid);
  const std::size_t shard_index = FrameShardIndexForKey(key);
  auto& shard = ledger->frame_shards[shard_index];
  std::lock_guard<std::mutex> shard_lock(shard.mutex);
  auto existing = shard.frames.find(key);
  if (existing != shard.frames.end()) {
    if (existing->second.buffer.valid()) {
      ++shard.release_count;
      ++ledger->frame_release_count;
    }
    shard.frames.erase(existing);
  }
  shard.frames.emplace(key, MakeResidentFrameRecord(entry, context, shard_index, std::move(buffer)));
  ++shard.allocation_count;
  ++ledger->frame_allocation_count;
  return shard_index;
}

bool ReleaseResidentFrameUnlocked(PageCacheLedger* ledger, const TypedUuid& page_uuid) {
  if (ledger == nullptr) {
    return false;
  }
  const auto key = PageFrameKey(page_uuid);
  auto& shard = ledger->frame_shards[FrameShardIndexForKey(key)];
  std::lock_guard<std::mutex> shard_lock(shard.mutex);
  const auto frame = shard.frames.find(key);
  if (frame == shard.frames.end()) {
    return false;
  }
  if (frame->second.buffer.valid()) {
    ++shard.release_count;
    ++ledger->frame_release_count;
  }
  shard.frames.erase(frame);
  return true;
}

PageCacheLifecycleResult LifecycleError(std::string diagnostic_code,
                                        std::string message_key,
                                        std::string detail = {}) {
  PageCacheLifecycleResult result;
  result.status = CacheErrorStatus();
  result.state = PageCacheLifecycleState::failed;
  result.diagnostic = MakePageCacheDiagnostic(result.status,
                                              std::move(diagnostic_code),
                                              std::move(message_key),
                                              std::move(detail));
  return result;
}

PageCacheLifecycleResult LifecycleOk(PageCacheLifecycleState state) {
  PageCacheLifecycleResult result;
  result.status = CacheOkStatus();
  result.state = state;
  return result;
}

PageCacheSnapshot SnapshotUnlocked(const PageCacheLedger& ledger) {
  PageCacheSnapshot snapshot;
  snapshot.contexts.reserve(kPageCacheIoContextCount);
  snapshot.frame_shard_count = kPageCacheFrameShardCount;
  snapshot.frame_shards.reserve(kPageCacheFrameShardCount);
  for (std::size_t index = 0; index < kPageCacheFrameShardCount; ++index) {
    const auto& shard = ledger.frame_shards[index];
    std::lock_guard<std::mutex> shard_lock(shard.mutex);
    PageCacheFrameShardSnapshot shard_snapshot;
    shard_snapshot.shard_id = static_cast<u64>(index);
    shard_snapshot.allocation_count = shard.allocation_count;
    shard_snapshot.release_count = shard.release_count;
    shard_snapshot.allocation_failure_count = shard.allocation_failure_count;
    for (const auto& frame : shard.frames) {
      if (!frame.second.buffer.valid()) {
        continue;
      }
      const auto context_index = ContextIndex(frame.second.io_context);
      ++shard_snapshot.resident_frames;
      shard_snapshot.resident_bytes += frame.second.buffer.size();
      ++shard_snapshot.context_frames[context_index];
      shard_snapshot.context_bytes[context_index] += frame.second.buffer.size();
    }
    if (shard_snapshot.resident_frames != 0) {
      ++snapshot.active_frame_shard_count;
    }
    snapshot.memory_manager_frame_count += shard_snapshot.resident_frames;
    snapshot.memory_manager_frame_bytes += shard_snapshot.resident_bytes;
    snapshot.memory_manager_frame_allocation_count += shard_snapshot.allocation_count;
    snapshot.memory_manager_frame_release_count += shard_snapshot.release_count;
    snapshot.memory_manager_frame_allocation_failure_count +=
        shard_snapshot.allocation_failure_count;
    snapshot.frame_shards.push_back(std::move(shard_snapshot));
  }
  snapshot.memory_manager_frames_bound =
      ledger.memory_manager != nullptr || snapshot.memory_manager_frame_count != 0;
  snapshot.sharded_frame_table_bound = true;
  for (std::size_t index = 0; index < kPageCacheIoContextCount; ++index) {
    const PageCacheIoContext context = ContextFromIndex(index);
    const auto& counters = ledger.context_counters[index];
    PageCacheContextSnapshot context_snapshot;
    context_snapshot.context = context;
    context_snapshot.context_name = PageCacheIoContextName(context);
    context_snapshot.admissions = counters.admissions;
    context_snapshot.reuses = counters.reuses;
    context_snapshot.evictions = counters.evictions;
    context_snapshot.protected_normal_hot_skips = counters.protected_normal_hot_skips;
    context_snapshot.refusals = counters.refusals;
    snapshot.contexts.push_back(std::move(context_snapshot));
  }
  for (const auto& entry : ledger.entries) {
    if (!entry.resident) {
      continue;
    }
    auto& context_snapshot = snapshot.contexts[ContextIndex(entry.io_context)];
    ++context_snapshot.resident_pages;
    context_snapshot.resident_bytes += entry.resident_bytes;
    ++snapshot.resident_pages;
    snapshot.resident_bytes += entry.resident_bytes;
    if (entry.pin_count != 0) {
      ++context_snapshot.pinned_pages;
      ++snapshot.pinned_pages;
    }
    if (entry.dirty) {
      ++context_snapshot.dirty_pages;
      ++snapshot.dirty_pages;
    }
    if (entry.writeback_in_progress) {
      ++snapshot.writeback_pages;
    }
  }
  return snapshot;
}

PageCacheEntry* FindMutable(PageCacheLedger* ledger, const TypedUuid& page_uuid) {
  for (auto& entry : ledger->entries) {
    if (entry.page_uuid.value == page_uuid.value) {
      return &entry;
    }
  }
  return nullptr;
}

bool Evictable(const PageCacheEntry& entry, const PageCachePolicy& policy) {
  return entry.resident &&
         entry.pin_count == 0 &&
         (!entry.dirty || policy.allow_dirty_eviction);
}

bool ProtectedHotReadPage(const PageCacheEntry& entry, PageCacheIoContext requesting_context) {
  return IsBulkContext(requesting_context) &&
         IsReadResidencyContext(entry.io_context) &&
         entry.cache_hot;
}

int EvictionPriority(const PageCacheEntry& entry, PageCacheIoContext requesting_context) {
  if (!IsBulkContext(requesting_context)) {
    return 0;
  }
  if (entry.io_context == requesting_context) {
    return 0;
  }
  if (entry.io_context != PageCacheIoContext::normal) {
    return 1;
  }
  return 2;
}

PageCacheEntry* SelectEvictionCandidate(PageCacheLedger* ledger,
                                        const PageCachePolicy& policy,
                                        PageCacheIoContext requesting_context,
                                        const PageCacheIoContext* required_context,
                                        u64* protected_normal_hot_skips) {
  PageCacheEntry* selected = nullptr;
  int selected_priority = 0;
  for (auto& entry : ledger->entries) {
    if (!entry.resident) {
      continue;
    }
    if (required_context != nullptr && entry.io_context != *required_context) {
      continue;
    }
    if (ProtectedHotReadPage(entry, requesting_context)) {
      if (protected_normal_hot_skips != nullptr) {
        ++(*protected_normal_hot_skips);
      }
      continue;
    }
    if (!Evictable(entry, policy)) {
      continue;
    }
    const int priority = EvictionPriority(entry, requesting_context);
    if (selected == nullptr ||
        priority < selected_priority ||
        (priority == selected_priority && entry.last_access_tick < selected->last_access_tick)) {
      selected = &entry;
      selected_priority = priority;
    }
  }
  return selected;
}

PageCacheEntry* SelectRingEvictionCandidate(PageCacheLedger* ledger,
                                            const PageCachePolicy& policy,
                                            PageCacheIoContext context) {
  if (ledger == nullptr) {
    return nullptr;
  }
  std::vector<PageCacheEntry*> lane_entries;
  for (auto& entry : ledger->entries) {
    if (entry.resident && entry.io_context == context) {
      lane_entries.push_back(&entry);
    }
  }
  if (lane_entries.empty()) {
    return nullptr;
  }

  auto& cursor = ledger->ring_reuse_cursors[ContextIndex(context)];
  const u64 start = cursor % lane_entries.size();
  for (u64 offset = 0; offset < lane_entries.size(); ++offset) {
    const u64 index = (start + offset) % lane_entries.size();
    PageCacheEntry* candidate = lane_entries[static_cast<std::size_t>(index)];
    if (!Evictable(*candidate, policy)) {
      continue;
    }
    cursor = (index + 1) % lane_entries.size();
    return candidate;
  }
  return nullptr;
}

u64 CountResidentByContext(const PageCacheLedger& ledger, PageCacheIoContext context) {
  u64 count = 0;
  for (const auto& entry : ledger.entries) {
    if (entry.resident && entry.io_context == context) {
      ++count;
    }
  }
  return count;
}

void EvictUnlocked(PageCacheLedger* ledger,
                   PageCacheEntry* entry,
                   std::array<u64, kPageCacheIoContextCount>* evictions_by_context) {
  if (ledger == nullptr || entry == nullptr) {
    return;
  }
  const std::size_t index = ContextIndex(entry->io_context);
  (void)ReleaseResidentFrameUnlocked(ledger, entry->page_uuid);
  entry->resident = false;
  entry->resident_bytes = 0;
  entry->cache_hot = false;
  entry->frame_shard_id = 0;
  entry->frame_scope_id.clear();
  ++ledger->context_counters[index].evictions;
  if (evictions_by_context != nullptr) {
    ++(*evictions_by_context)[index];
  }
}

void PublishSnapshot(const PageCacheSnapshot& snapshot) {
  (void)scratchbird::core::metrics::PublishPageCacheSnapshot(static_cast<double>(snapshot.resident_pages),
                                                            static_cast<double>(snapshot.resident_bytes),
                                                            static_cast<double>(snapshot.pinned_pages),
                                                            static_cast<double>(snapshot.dirty_pages),
                                                            "all",
                                                            "all",
                                                            "all");
  for (const auto& context : snapshot.contexts) {
    (void)scratchbird::core::metrics::PublishPageCacheContextSnapshot(
        static_cast<double>(context.resident_pages),
        static_cast<double>(context.resident_bytes),
        static_cast<double>(context.pinned_pages),
        static_cast<double>(context.dirty_pages),
        "all",
        "all",
        "all",
        context.context_name,
        "current",
        "snapshot");
  }
}

void RecordAdmissionMetric(PageCacheIoContext context, u64 count, const char* reason) {
  if (count == 0) {
    return;
  }
  (void)scratchbird::core::metrics::RecordPageCacheContextAdmission(
      static_cast<double>(count),
      "all",
      "all",
      "all",
      PageCacheIoContextName(context),
      "ok",
      reason);
}

void RecordReuseMetric(PageCacheIoContext context, u64 count, const char* reason) {
  if (count == 0) {
    return;
  }
  (void)scratchbird::core::metrics::RecordPageCacheContextReuse(
      static_cast<double>(count),
      "all",
      "all",
      "all",
      PageCacheIoContextName(context),
      "ok",
      reason);
}

void RecordProtectedSkipMetric(PageCacheIoContext context, u64 count, const char* reason) {
  if (count == 0) {
    return;
  }
  (void)scratchbird::core::metrics::RecordPageCacheContextProtectedNormalHotSkip(
      static_cast<double>(count),
      "all",
      "all",
      "all",
      PageCacheIoContextName(context),
      "protected",
      reason);
}

void RecordRefusalMetric(PageCacheIoContext context, u64 count, const char* reason) {
  if (count == 0) {
    return;
  }
  (void)scratchbird::core::metrics::RecordPageCacheContextRefusal(
      static_cast<double>(count),
      "all",
      "all",
      "all",
      PageCacheIoContextName(context),
      "refused",
      reason);
}

void RecordContextEvictionMetrics(const std::array<u64, kPageCacheIoContextCount>& evictions,
                                  const char* result,
                                  const char* reason) {
  for (std::size_t index = 0; index < evictions.size(); ++index) {
    if (evictions[index] == 0) {
      continue;
    }
    (void)scratchbird::core::metrics::RecordPageCacheContextEviction(
        static_cast<double>(evictions[index]),
        "all",
        "all",
        "all",
        PageCacheIoContextName(ContextFromIndex(index)),
        result,
        reason);
  }
}

std::string JsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(ch); break;
    }
  }
  return out;
}

void AddDiagnostic(PageCacheCheckpointPublication* publication, std::string diagnostic) {
  if (publication != nullptr) {
    publication->diagnostics.push_back(std::move(diagnostic));
  }
}

PageCacheCheckpointPublication BasePublication(const PageCacheLifecycleInput& input,
                                               const PageCacheSnapshot& snapshot,
                                               PageCacheLifecycleState state,
                                               PageCacheCheckpointMode mode) {
  PageCacheCheckpointPublication publication;
  publication.database_uuid = scratchbird::core::uuid::UuidToString(input.database_uuid.value);
  publication.filespace_uuid = scratchbird::core::uuid::UuidToString(input.filespace_uuid.value);
  publication.database_lifecycle_state = input.database_lifecycle_state;
  publication.lifecycle_state = state;
  publication.checkpoint_mode = mode;
  publication.policy_generation = input.policy_generation;
  publication.checkpoint_generation = input.checkpoint_generation == 0 ? 1 : input.checkpoint_generation;
  publication.dirty_epoch = input.dirty_epoch;
  publication.resident_pages = snapshot.resident_pages;
  publication.dirty_pages_before = snapshot.dirty_pages;
  publication.dirty_pages_after = snapshot.dirty_pages;
  publication.cluster_paths_failed_closed = true;
  publication.ordinary_admission_allowed = state == PageCacheLifecycleState::active ||
                                           state == PageCacheLifecycleState::clean;
  return publication;
}

PageCacheLifecycleResult ValidateLifecycleInput(const PageCacheLifecycleInput& input,
                                                bool require_shutdown_request = false) {
  if (!IsTypedEngineIdentity(input.database_uuid, UuidKind::database) ||
      !IsTypedEngineIdentity(input.filespace_uuid, UuidKind::filespace)) {
    return LifecycleError("CACHE.CHECKPOINT_INPUT_INVALID",
                          "storage.page_cache.lifecycle_identity_invalid");
  }
  if (input.policy_generation == 0 || input.checkpoint_generation == 0) {
    return LifecycleError("CACHE.CHECKPOINT_INPUT_INVALID",
                          "storage.page_cache.lifecycle_generation_invalid");
  }
  if (!input.tx2_activation_committed ||
      !input.cache_runtime_started ||
      !input.engine_agent_active) {
    return LifecycleError("CACHE.CHECKPOINT_INPUT_INVALID",
                          "storage.page_cache.lifecycle_activation_required");
  }
  if (!input.standalone_mode || input.cluster_authority_available) {
    return LifecycleError("CACHE.CHECKPOINT_CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
                          "storage.page_cache.cluster_external_provider_required");
  }
  if (require_shutdown_request && !input.shutdown_requested) {
    return LifecycleError("CACHE.CHECKPOINT_INPUT_INVALID",
                          "storage.page_cache.shutdown_request_required");
  }
  return ValidatePageCacheAuthorityBoundary(PageCacheAuthorityBoundary{});
}

PageCacheLifecycleResult WithSnapshot(PageCacheLifecycleResult result,
                                      const PageCacheLifecycleInput& input,
                                      PageCacheCheckpointMode mode,
                                      const PageCacheLedger& ledger) {
  result.snapshot = SnapshotPageCache(ledger);
  result.publication = BasePublication(input, result.snapshot, result.state, mode);
  AppendFrameOwnershipEvidence(&result.evidence, result.snapshot);
  return result;
}

u64 CountPinnedResidentPages(const PageCacheLedger& ledger) {
  u64 count = 0;
  for (const auto& entry : ledger.entries) {
    if (entry.resident && entry.pin_count != 0) {
      ++count;
    }
  }
  return count;
}

u64 FlushDirtyUnlocked(PageCacheLedger* ledger, u64 checkpoint_generation) {
  u64 flushed = 0;
  for (auto& entry : ledger->entries) {
    if (!entry.resident || !entry.dirty) {
      continue;
    }
    entry.writeback_in_progress = true;
    entry.dirty = false;
    entry.writeback_in_progress = false;
    entry.last_writeback_checkpoint_generation = checkpoint_generation;
    entry.dirty_epoch = 0;
    ++flushed;
  }
  ledger->writeback_count += flushed;
  return flushed;
}

PageCacheLifecycleResult MakeLifecycleInputError(const PageCacheLifecycleResult& validation,
                                                 const PageCacheLifecycleInput& input,
                                                 PageCacheCheckpointMode mode,
                                                 const PageCacheLedger* ledger) {
  PageCacheLifecycleResult result = validation;
  if (ledger != nullptr) {
    result.snapshot = SnapshotPageCache(*ledger);
    result.publication = BasePublication(input, result.snapshot, result.state, mode);
    result.publication.ordinary_admission_allowed = false;
    AddDiagnostic(&result.publication, validation.diagnostic.diagnostic_code);
  }
  return result;
}

}  // namespace

const char* PageCacheIoContextName(PageCacheIoContext context) {
  switch (context) {
    case PageCacheIoContext::normal: return "normal";
    case PageCacheIoContext::index_read: return "index_read";
    case PageCacheIoContext::bulk_read: return "bulk_read";
    case PageCacheIoContext::bulk_write: return "bulk_write";
    case PageCacheIoContext::vacuum_cleanup: return "vacuum_cleanup";
    case PageCacheIoContext::index_build: return "index_build";
    case PageCacheIoContext::strict_bulk_load: return "strict_bulk_load";
  }
  return "normal";
}

bool PageCacheIoContextFromName(std::string_view name, PageCacheIoContext* context) {
  if (context == nullptr) {
    return false;
  }
  if (name == "normal") {
    *context = PageCacheIoContext::normal;
    return true;
  }
  if (name == "index_read") {
    *context = PageCacheIoContext::index_read;
    return true;
  }
  if (name == "bulk_read") {
    *context = PageCacheIoContext::bulk_read;
    return true;
  }
  if (name == "bulk_write") {
    *context = PageCacheIoContext::bulk_write;
    return true;
  }
  if (name == "vacuum_cleanup") {
    *context = PageCacheIoContext::vacuum_cleanup;
    return true;
  }
  if (name == "index_build") {
    *context = PageCacheIoContext::index_build;
    return true;
  }
  if (name == "strict_bulk_load") {
    *context = PageCacheIoContext::strict_bulk_load;
    return true;
  }
  return false;
}

bool PageCacheIoContextUsesBoundedRing(PageCacheIoContext context) {
  return context != PageCacheIoContext::normal;
}

u64 PageCacheIoContextRingLimit(const PageCachePolicy& policy, PageCacheIoContext context) {
  switch (context) {
    case PageCacheIoContext::normal:
      return policy.max_resident_pages == 0 ? 1 : policy.max_resident_pages;
    case PageCacheIoContext::index_read:
      return policy.index_read_ring_pages == 0 ? 1 : policy.index_read_ring_pages;
    case PageCacheIoContext::bulk_read:
      return policy.bulk_read_ring_pages == 0 ? 1 : policy.bulk_read_ring_pages;
    case PageCacheIoContext::bulk_write:
      return policy.bulk_write_ring_pages == 0 ? 1 : policy.bulk_write_ring_pages;
    case PageCacheIoContext::vacuum_cleanup:
      return policy.vacuum_cleanup_ring_pages == 0 ? 1 : policy.vacuum_cleanup_ring_pages;
    case PageCacheIoContext::index_build:
      return policy.index_build_ring_pages == 0 ? 1 : policy.index_build_ring_pages;
    case PageCacheIoContext::strict_bulk_load:
      return policy.strict_bulk_load_ring_pages == 0 ? 1 : policy.strict_bulk_load_ring_pages;
  }
  return 1;
}

PageCachePolicy MakeAdaptivePageCachePolicyFromMemoryPolicy(
    const scratchbird::core::memory::AllocationPolicy& memory_policy,
    u32 page_size,
    u64 minimum_resident_pages) {
  PageCachePolicy policy;
  const u64 safe_page_size = page_size == 0 ? 8192ull : static_cast<u64>(page_size);
  const u64 default_pool_bytes = 512ull * 1024ull * 1024ull;
  u64 pool_bytes = memory_policy.page_buffer_pool_limit_bytes;
  if (pool_bytes == 0 && memory_policy.hard_limit_bytes != 0) {
    pool_bytes = std::max<u64>(default_pool_bytes, memory_policy.hard_limit_bytes / 2ull);
  }
  if (pool_bytes == 0) {
    pool_bytes = default_pool_bytes;
  }
  const u64 resident_pages_from_bytes =
      std::max<u64>(1, pool_bytes / safe_page_size);
  policy.max_resident_pages =
      std::max(minimum_resident_pages == 0 ? 1 : minimum_resident_pages,
               resident_pages_from_bytes);
  policy.max_resident_bytes = policy.max_resident_pages * safe_page_size;
  const u64 ring_pages = std::max<u64>(4, policy.max_resident_pages / 32ull);
  const u64 index_read_ring_pages = std::max<u64>(32, policy.max_resident_pages / 2ull);
  const u64 index_ring_pages = std::max<u64>(8, policy.max_resident_pages / 16ull);
  policy.index_read_ring_pages = std::min(policy.max_resident_pages, index_read_ring_pages);
  policy.bulk_read_ring_pages = ring_pages;
  policy.bulk_write_ring_pages = ring_pages;
  policy.vacuum_cleanup_ring_pages = ring_pages;
  policy.index_build_ring_pages = index_ring_pages;
  policy.strict_bulk_load_ring_pages = index_ring_pages;
  policy.allow_dirty_eviction = false;
  policy.require_memory_manager_frames = true;
  return policy;
}

const char* PageCacheLifecycleStateName(PageCacheLifecycleState state) {
  switch (state) {
    case PageCacheLifecycleState::not_started: return "not_started";
    case PageCacheLifecycleState::starting: return "starting";
    case PageCacheLifecycleState::active: return "active";
    case PageCacheLifecycleState::memory_pressure: return "memory_pressure";
    case PageCacheLifecycleState::checkpointing: return "checkpointing";
    case PageCacheLifecycleState::clean: return "clean";
    case PageCacheLifecycleState::draining: return "draining";
    case PageCacheLifecycleState::stopped: return "stopped";
    case PageCacheLifecycleState::failed: return "failed";
    case PageCacheLifecycleState::quarantined: return "quarantined";
  }
  return "failed";
}

const char* PageCacheCheckpointModeName(PageCacheCheckpointMode mode) {
  switch (mode) {
    case PageCacheCheckpointMode::try_checkpoint: return "try_checkpoint";
    case PageCacheCheckpointMode::wait_checkpoint: return "wait_checkpoint";
    case PageCacheCheckpointMode::force_checkpoint: return "force_checkpoint";
    case PageCacheCheckpointMode::clean_close: return "clean_close";
    case PageCacheCheckpointMode::shutdown_flush: return "shutdown_flush";
  }
  return "try_checkpoint";
}

bool PageCacheAuthorityBoundaryValid(const PageCacheAuthorityBoundary& boundary) {
  return !boundary.transaction_finality_authority &&
         !boundary.transaction_visibility_authority &&
         !boundary.recovery_finality_authority &&
         !boundary.catalog_truth_authority &&
         !boundary.filespace_identity_authority &&
         !boundary.parser_authority &&
         !boundary.wal_or_redo_authority &&
         boundary.checkpoint_is_clean_close_evidence_only &&
         boundary.cache_flush_requires_mga_finality_evidence;
}

PageCacheLifecycleResult ValidatePageCacheAuthorityBoundary(
    const PageCacheAuthorityBoundary& boundary) {
  if (PageCacheAuthorityBoundaryValid(boundary)) {
    return LifecycleOk(PageCacheLifecycleState::active);
  }
  return LifecycleError("CACHE.CHECKPOINT_AUTHORITY_DENIED",
                        "storage.page_cache.authority_boundary_invalid");
}

void BindPageCacheMemoryManager(PageCacheLedger* ledger, MemoryManager* manager) {
  if (ledger == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(ledger->mutex);
  ledger->memory_manager = manager;
}

PageCacheSnapshot SnapshotPageCache(const PageCacheLedger& ledger) {
  std::lock_guard<std::mutex> lock(ledger.mutex);
  return SnapshotUnlocked(ledger);
}

PageCacheContextSnapshot SnapshotPageCacheContext(const PageCacheLedger& ledger,
                                                  PageCacheIoContext context) {
  const auto snapshot = SnapshotPageCache(ledger);
  const auto index = ContextIndex(context);
  if (index < snapshot.contexts.size()) {
    return snapshot.contexts[index];
  }
  PageCacheContextSnapshot empty;
  empty.context = PageCacheIoContext::normal;
  empty.context_name = PageCacheIoContextName(PageCacheIoContext::normal);
  return empty;
}

PageCacheResult AdmitPageCacheEntry(PageCacheLedger* ledger,
                                    const PageCachePolicy& policy,
                                    const PageCacheEntry& entry) {
  return AdmitPageCacheEntryForContext(ledger, policy, entry, DefaultContextForEntry(entry));
}

PageCacheResult AdmitPageCacheEntryForContext(PageCacheLedger* ledger,
                                              const PageCachePolicy& policy,
                                              const PageCacheEntry& entry,
                                              PageCacheIoContext context) {
  if (ledger == nullptr) {
    return Error("page_cache_ledger_required", "storage.page_cache.ledger_required");
  }
  if (!IsTypedEngineIdentity(entry.database_uuid, UuidKind::database) ||
      !IsTypedEngineIdentity(entry.filespace_uuid, UuidKind::filespace) ||
      !IsTypedEngineIdentity(entry.page_uuid, UuidKind::page)) {
    return Error("page_cache_identity_invalid", "storage.page_cache.identity_invalid");
  }
  if (entry.page_type == PageType::unknown || !IsKnownPageFamilyName(PageTypeName(entry.page_type))) {
    return Error("page_cache_page_type_invalid", "storage.page_cache.page_type_invalid", PageTypeName(entry.page_type));
  }
  if (!IsSupportedDatabasePageSize(entry.page_size)) {
    return Error("page_cache_page_size_invalid", "storage.page_cache.page_size_invalid", std::to_string(entry.page_size));
  }

  ScopedPageBuffer resident_frame;
  if (policy.require_memory_manager_frames) {
    auto frame = AllocateResidentFrame(ledger, entry, context);
    if (!frame.ok()) {
      PageCacheResult result;
      result.status = frame.status;
      result.diagnostic = frame.diagnostic.diagnostic_code.empty()
                              ? MakePageCacheDiagnostic(frame.status,
                                                        "page_cache_frame_allocation_failed",
                                                        "storage.page_cache.frame_allocation_failed")
                              : frame.diagnostic;
      {
        std::lock_guard<std::mutex> lock(ledger->mutex);
        RecordFrameAllocationFailureUnlocked(ledger, entry.page_uuid);
        result.snapshot = SnapshotUnlocked(*ledger);
      }
      AppendFrameOwnershipEvidence(&result.evidence, result.snapshot);
      return result;
    }
    resident_frame = std::move(frame.buffer);
  }

  PageCacheEntry admitted = entry;
  admitted.io_context = context;
  if (context == PageCacheIoContext::index_read) {
    admitted.cache_hot = true;
  }
  PageCacheResult result;
  u64 context_admissions = 0;
  u64 context_reuses = 0;
  u64 protected_normal_hot_skips = 0;
  u64 context_refusals = 0;
  const char* refusal_reason = nullptr;
  const char* eviction_metric_reason = "budget_or_ring";
  std::array<u64, kPageCacheIoContextCount> evictions_by_context{};
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    PageCacheEntry* existing = FindMutable(ledger, entry.page_uuid);
    if (existing != nullptr && existing->resident) {
      return Error("page_cache_duplicate_page", "storage.page_cache.duplicate_page");
    }
    const bool reused_nonresident_slot = existing != nullptr;

    const u64 max_pages = policy.max_resident_pages == 0 ? 1 : policy.max_resident_pages;
    const u64 max_bytes = policy.max_resident_bytes == 0 ? entry.page_size : policy.max_resident_bytes;
    if (PageCacheIoContextUsesBoundedRing(context)) {
      const u64 ring_limit = PageCacheIoContextRingLimit(policy, context);
      while (CountResidentByContext(*ledger, context) >= ring_limit) {
        PageCacheEntry* selected = SelectRingEvictionCandidate(ledger, policy, context);
        if (selected == nullptr) {
          ++context_refusals;
          refusal_reason = "ring_pinned_or_dirty";
          eviction_metric_reason = "ring";
          ++ledger->context_counters[ContextIndex(context)].refusals;
          result = Error("page_cache_context_ring_exhausted_pinned_or_dirty",
                         "storage.page_cache.context_ring_exhausted_pinned_or_dirty",
                         PageCacheIoContextName(context));
          result.snapshot = SnapshotUnlocked(*ledger);
          break;
        }
        EvictUnlocked(ledger, selected, &evictions_by_context);
        ++context_reuses;
        ++ledger->context_counters[ContextIndex(context)].reuses;
      }
    }
    if (refusal_reason == nullptr) {
      PageCacheSnapshot snapshot = SnapshotUnlocked(*ledger);
      while ((snapshot.resident_pages + 1 > max_pages) || (snapshot.resident_bytes + entry.page_size > max_bytes)) {
        PageCacheEntry* selected = SelectEvictionCandidate(ledger,
                                                           policy,
                                                           context,
                                                           nullptr,
                                                           &protected_normal_hot_skips);
        if (selected == nullptr) {
          if (protected_normal_hot_skips != 0) {
            ledger->context_counters[ContextIndex(context)].protected_normal_hot_skips +=
                protected_normal_hot_skips;
          }
          ++context_refusals;
          refusal_reason = protected_normal_hot_skips != 0
              ? "normal_hot_protected"
              : "budget_pinned_or_dirty";
          ++ledger->context_counters[ContextIndex(context)].refusals;
          result = Error("page_cache_budget_exhausted_pinned_or_dirty",
                         "storage.page_cache.budget_exhausted_pinned_or_dirty");
          result.snapshot = SnapshotUnlocked(*ledger);
          break;
        }
        EvictUnlocked(ledger, selected, &evictions_by_context);
        snapshot = SnapshotUnlocked(*ledger);
      }
    }
    if (refusal_reason == nullptr) {
      if (protected_normal_hot_skips != 0) {
        ledger->context_counters[ContextIndex(context)].protected_normal_hot_skips +=
            protected_normal_hot_skips;
      }

      admitted.resident = true;
      admitted.resident_bytes = entry.page_size;
      admitted.last_access_tick = ledger->next_access_tick++;
      if (admitted.dirty && admitted.dirty_epoch == 0) {
        admitted.dirty_epoch = ++ledger->dirty_epoch;
      }
      if (policy.require_memory_manager_frames) {
        const auto key = PageFrameKey(admitted.page_uuid);
        const std::size_t shard_index = FrameShardIndexForKey(key);
        admitted.frame_shard_id = static_cast<u64>(shard_index);
        admitted.frame_scope_id = PageFrameScopeId(admitted, context, shard_index);
      }
      if (existing != nullptr) {
        *existing = admitted;
      } else {
        ledger->entries.push_back(admitted);
      }
      if (policy.require_memory_manager_frames) {
        const std::size_t shard_index =
            InsertResidentFrameUnlocked(ledger, admitted, context, std::move(resident_frame));
        if (existing != nullptr) {
          existing->frame_shard_id = static_cast<u64>(shard_index);
          existing->frame_scope_id = PageFrameScopeId(*existing, context, shard_index);
        } else {
          ledger->entries.back().frame_shard_id = static_cast<u64>(shard_index);
          ledger->entries.back().frame_scope_id =
              PageFrameScopeId(ledger->entries.back(), context, shard_index);
        }
      }
      ++context_admissions;
      ++ledger->context_counters[ContextIndex(context)].admissions;
      if (reused_nonresident_slot) {
        ++context_reuses;
        ++ledger->context_counters[ContextIndex(context)].reuses;
      }

      result.status = CacheOkStatus();
      result.changed = true;
      result.entry = admitted;
      result.snapshot = SnapshotUnlocked(*ledger);
    }
  }
  if (refusal_reason != nullptr) {
    RecordProtectedSkipMetric(context, protected_normal_hot_skips, "bulk_context_budget");
    RecordRefusalMetric(context, context_refusals, refusal_reason);
    RecordContextEvictionMetrics(evictions_by_context, "evicted", eviction_metric_reason);
    PublishSnapshot(result.snapshot);
    AppendFrameOwnershipEvidence(&result.evidence, result.snapshot);
    return result;
  }
  u64 budget_evictions = 0;
  for (const auto count : evictions_by_context) {
    budget_evictions += count;
  }
  for (u64 i = 0; i < budget_evictions; ++i) {
    (void)scratchbird::core::metrics::RecordPageCacheEviction("all", "all", "all", "budget");
  }
  RecordAdmissionMetric(context, context_admissions, "admit");
  RecordReuseMetric(context, context_reuses, "ring_or_slot_reuse");
  RecordProtectedSkipMetric(context, protected_normal_hot_skips, "bulk_context_budget");
  RecordContextEvictionMetrics(evictions_by_context, "evicted", "budget_or_ring");
  PublishSnapshot(result.snapshot);
  AppendFrameOwnershipEvidence(&result.evidence, result.snapshot);
  return result;
}

PageCacheResult PinPageCacheEntry(PageCacheLedger* ledger, const TypedUuid& page_uuid) {
  if (ledger == nullptr) {
    return Error("page_cache_ledger_required", "storage.page_cache.ledger_required");
  }
  PageCacheResult result;
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    auto* entry = FindMutable(ledger, page_uuid);
    if (entry == nullptr || !entry->resident) {
      return Error("page_cache_entry_not_found", "storage.page_cache.entry_not_found");
    }
    ++entry->pin_count;
    entry->last_access_tick = ledger->next_access_tick++;
    if (IsReadResidencyContext(entry->io_context)) {
      entry->cache_hot = true;
    }
    result.status = CacheOkStatus();
    result.changed = true;
    result.entry = *entry;
    result.snapshot = SnapshotUnlocked(*ledger);
  }
  PublishSnapshot(result.snapshot);
  return result;
}

PageCacheResult UnpinPageCacheEntry(PageCacheLedger* ledger, const TypedUuid& page_uuid) {
  if (ledger == nullptr) {
    return Error("page_cache_ledger_required", "storage.page_cache.ledger_required");
  }
  PageCacheResult result;
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    auto* entry = FindMutable(ledger, page_uuid);
    if (entry == nullptr || !entry->resident) {
      return Error("page_cache_entry_not_found", "storage.page_cache.entry_not_found");
    }
    if (entry->pin_count == 0) {
      return Error("page_cache_entry_not_pinned", "storage.page_cache.entry_not_pinned");
    }
    --entry->pin_count;
    entry->last_access_tick = ledger->next_access_tick++;
    if (IsReadResidencyContext(entry->io_context)) {
      entry->cache_hot = true;
    }
    result.status = CacheOkStatus();
    result.changed = true;
    result.entry = *entry;
    result.snapshot = SnapshotUnlocked(*ledger);
  }
  PublishSnapshot(result.snapshot);
  return result;
}

PageCacheResult MarkPageCacheEntryDirty(PageCacheLedger* ledger, const TypedUuid& page_uuid, bool dirty) {
  if (ledger == nullptr) {
    return Error("page_cache_ledger_required", "storage.page_cache.ledger_required");
  }
  PageCacheResult result;
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    auto* entry = FindMutable(ledger, page_uuid);
    if (entry == nullptr || !entry->resident) {
      return Error("page_cache_entry_not_found", "storage.page_cache.entry_not_found");
    }
    entry->dirty = dirty;
    if (dirty && entry->dirty_epoch == 0) {
      entry->dirty_epoch = ++ledger->dirty_epoch;
    }
    entry->last_access_tick = ledger->next_access_tick++;
    if (IsReadResidencyContext(entry->io_context)) {
      entry->cache_hot = true;
    }
    result.status = CacheOkStatus();
    result.changed = true;
    result.entry = *entry;
    result.snapshot = SnapshotUnlocked(*ledger);
  }
  PublishSnapshot(result.snapshot);
  return result;
}

PageCacheResult EvictOnePageCacheEntry(PageCacheLedger* ledger, const PageCachePolicy& policy) {
  return EvictOnePageCacheEntryForContext(ledger, policy, PageCacheIoContext::normal);
}

PageCacheResult EvictOnePageCacheEntryForContext(PageCacheLedger* ledger,
                                                 const PageCachePolicy& policy,
                                                 PageCacheIoContext context) {
  if (ledger == nullptr) {
    return Error("page_cache_ledger_required", "storage.page_cache.ledger_required");
  }
  PageCacheResult result;
  u64 protected_normal_hot_skips = 0;
  std::array<u64, kPageCacheIoContextCount> evictions_by_context{};
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    PageCacheEntry* selected = SelectEvictionCandidate(ledger,
                                                       policy,
                                                       context,
                                                       nullptr,
                                                       &protected_normal_hot_skips);
    if (selected == nullptr) {
      if (protected_normal_hot_skips != 0) {
        ledger->context_counters[ContextIndex(context)].protected_normal_hot_skips +=
            protected_normal_hot_skips;
      }
      auto result = Error("page_cache_no_evictable_page", "storage.page_cache.no_evictable_page");
      result.snapshot = SnapshotUnlocked(*ledger);
      AppendFrameOwnershipEvidence(&result.evidence, result.snapshot);
      return result;
    }
    result.status = CacheOkStatus();
    result.changed = true;
    result.evicted = true;
    result.entry = *selected;
    EvictUnlocked(ledger, selected, &evictions_by_context);
    if (protected_normal_hot_skips != 0) {
      ledger->context_counters[ContextIndex(context)].protected_normal_hot_skips +=
          protected_normal_hot_skips;
    }
    result.snapshot = SnapshotUnlocked(*ledger);
  }
  (void)scratchbird::core::metrics::RecordPageCacheEviction("all", "all", "all", "explicit");
  RecordProtectedSkipMetric(context, protected_normal_hot_skips, "explicit");
  RecordContextEvictionMetrics(evictions_by_context, "evicted", "explicit");
  PublishSnapshot(result.snapshot);
  AppendFrameOwnershipEvidence(&result.evidence, result.snapshot);
  return result;
}

PageCacheLifecycleResult StartPageCacheLifecycle(
    PageCacheLedger* ledger,
    const PageCachePolicy& policy,
    const PageCacheLifecycleInput& input,
    const std::vector<PageCacheEntry>& preload_entries) {
  if (ledger == nullptr) {
    return LifecycleError("CACHE.CHECKPOINT_LEDGER_REQUIRED",
                          "storage.page_cache.ledger_required");
  }
  const auto validation = ValidateLifecycleInput(input);
  if (!validation.ok()) {
    return MakeLifecycleInputError(validation, input, PageCacheCheckpointMode::try_checkpoint, ledger);
  }

  PageCacheLifecycleResult result = LifecycleOk(PageCacheLifecycleState::starting);
  u64 admitted = 0;
  for (auto entry : preload_entries) {
    entry.database_uuid = input.database_uuid;
    entry.filespace_uuid = input.filespace_uuid;
    const auto admit = AdmitPageCacheEntry(ledger, policy, entry);
    if (!admit.ok()) {
      result = LifecycleError("CACHE.CHECKPOINT_PRELOAD_FAILED",
                              "storage.page_cache.preload_failed",
                              admit.diagnostic.diagnostic_code);
      result.snapshot = SnapshotPageCache(*ledger);
      result.publication = BasePublication(input,
                                           result.snapshot,
                                           PageCacheLifecycleState::failed,
                                           PageCacheCheckpointMode::try_checkpoint);
      AddDiagnostic(&result.publication, result.diagnostic.diagnostic_code);
      return result;
    }
    ++admitted;
  }

  result = WithSnapshot(LifecycleOk(PageCacheLifecycleState::active),
                        input,
                        PageCacheCheckpointMode::try_checkpoint,
                        *ledger);
  result.publication.preload_complete = true;
  result.publication.flushed_pages = 0;
  AddDiagnostic(&result.publication, "CACHE.CHECKPOINT_PRELOAD_COMPLETE:" + std::to_string(admitted));
  PublishSnapshot(result.snapshot);
  return result;
}

PageCacheLifecycleResult WritebackDirtyPageCacheEntries(PageCacheLedger* ledger,
                                                        const PageCacheLifecycleInput& input) {
  if (ledger == nullptr) {
    return LifecycleError("CACHE.CHECKPOINT_LEDGER_REQUIRED",
                          "storage.page_cache.ledger_required");
  }
  const auto validation = ValidateLifecycleInput(input);
  if (!validation.ok()) {
    return MakeLifecycleInputError(validation, input, PageCacheCheckpointMode::wait_checkpoint, ledger);
  }
  if (!input.writeback_allowed) {
    return MakeLifecycleInputError(
        LifecycleError("CACHE.CHECKPOINT_WRITEBACK_REFUSED",
                       "storage.page_cache.writeback_refused"),
        input,
        PageCacheCheckpointMode::wait_checkpoint,
        ledger);
  }

  PageCacheSnapshot before;
  PageCacheSnapshot after;
  u64 flushed = 0;
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    before = SnapshotUnlocked(*ledger);
    flushed = FlushDirtyUnlocked(ledger, input.checkpoint_generation);
    after = SnapshotUnlocked(*ledger);
  }

  PageCacheLifecycleResult result = LifecycleOk(after.dirty_pages == 0
      ? PageCacheLifecycleState::clean
      : PageCacheLifecycleState::checkpointing);
  result.flushed_pages = flushed;
  result.snapshot = after;
  result.publication = BasePublication(input, before, result.state, PageCacheCheckpointMode::wait_checkpoint);
  result.publication.flushed_pages = flushed;
  result.publication.dirty_pages_after = after.dirty_pages;
  result.publication.writeback_complete = after.dirty_pages == 0;
  result.publication.ordinary_admission_allowed = after.dirty_pages == 0;
  AddDiagnostic(&result.publication, result.publication.writeback_complete
      ? "CACHE.CHECKPOINT_WRITEBACK_COMPLETE"
      : "CACHE.CHECKPOINT_DIRTY_PAGES_REMAIN");
  PublishSnapshot(after);
  return result;
}

PageCacheLifecycleResult CheckpointPageCacheLifecycle(PageCacheLedger* ledger,
                                                      const PageCacheLifecycleInput& input,
                                                      PageCacheCheckpointMode mode) {
  if (ledger == nullptr) {
    return LifecycleError("CACHE.CHECKPOINT_LEDGER_REQUIRED",
                          "storage.page_cache.ledger_required");
  }
  const auto validation = ValidateLifecycleInput(input);
  if (!validation.ok()) {
    return MakeLifecycleInputError(validation, input, mode, ledger);
  }
  if (!input.checkpoint_allowed) {
    return MakeLifecycleInputError(
        LifecycleError("CACHE.CHECKPOINT_REFUSED",
                       "storage.page_cache.checkpoint_refused"),
        input,
        mode,
        ledger);
  }

  PageCacheSnapshot before;
  PageCacheSnapshot after;
  u64 flushed = 0;
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    before = SnapshotUnlocked(*ledger);
    if (before.dirty_pages != 0 && mode == PageCacheCheckpointMode::try_checkpoint) {
      PageCacheLifecycleResult result =
          LifecycleError("CACHE.CHECKPOINT_DIRTY_PAGES_REMAIN",
                         "storage.page_cache.try_checkpoint_dirty_pages");
      result.snapshot = before;
      result.publication = BasePublication(input, before, PageCacheLifecycleState::checkpointing, mode);
      result.publication.checkpoint_complete = false;
      result.publication.ordinary_admission_allowed = false;
      AddDiagnostic(&result.publication, result.diagnostic.diagnostic_code);
      return result;
    }
    if (before.dirty_pages != 0 && !input.writeback_allowed) {
      PageCacheLifecycleResult result =
          LifecycleError("CACHE.CHECKPOINT_WRITEBACK_REFUSED",
                         "storage.page_cache.writeback_refused");
      result.snapshot = before;
      result.publication = BasePublication(input, before, PageCacheLifecycleState::failed, mode);
      result.publication.ordinary_admission_allowed = false;
      AddDiagnostic(&result.publication, result.diagnostic.diagnostic_code);
      return result;
    }
    flushed = FlushDirtyUnlocked(ledger, input.checkpoint_generation);
    ledger->last_checkpoint_generation = input.checkpoint_generation;
    ++ledger->checkpoint_count;
    after = SnapshotUnlocked(*ledger);
  }

  PageCacheLifecycleResult result = LifecycleOk(PageCacheLifecycleState::clean);
  result.flushed_pages = flushed;
  result.snapshot = after;
  result.publication = BasePublication(input, before, result.state, mode);
  result.publication.flushed_pages = flushed;
  result.publication.dirty_pages_after = after.dirty_pages;
  result.publication.writeback_complete = after.dirty_pages == 0;
  result.publication.checkpoint_complete = after.dirty_pages == 0;
  result.publication.clean_close_evidence =
      mode == PageCacheCheckpointMode::clean_close ||
      mode == PageCacheCheckpointMode::shutdown_flush;
  result.publication.shutdown_flush_complete =
      mode == PageCacheCheckpointMode::shutdown_flush && after.dirty_pages == 0;
  result.publication.ordinary_admission_allowed =
      mode != PageCacheCheckpointMode::shutdown_flush && after.dirty_pages == 0;
  AddDiagnostic(&result.publication, result.publication.checkpoint_complete
      ? "CACHE.CHECKPOINT_COMPLETE"
      : "CACHE.CHECKPOINT_DIRTY_PAGES_REMAIN");
  PublishSnapshot(after);
  return result;
}

PageCacheLifecycleResult ApplyPageCacheMemoryPressure(PageCacheLedger* ledger,
                                                      const PageCachePolicy& policy,
                                                      const PageCacheLifecycleInput& input) {
  if (ledger == nullptr) {
    return LifecycleError("CACHE.CHECKPOINT_LEDGER_REQUIRED",
                          "storage.page_cache.ledger_required");
  }
  const auto validation = ValidateLifecycleInput(input);
  if (!validation.ok()) {
    return MakeLifecycleInputError(validation, input, PageCacheCheckpointMode::wait_checkpoint, ledger);
  }
  if (!input.writeback_allowed) {
    return MakeLifecycleInputError(
        LifecycleError("CACHE.CHECKPOINT_WRITEBACK_REFUSED",
                       "storage.page_cache.writeback_refused"),
        input,
        PageCacheCheckpointMode::wait_checkpoint,
        ledger);
  }

  PageCacheSnapshot before;
  PageCacheSnapshot after;
  u64 flushed = 0;
  u64 evicted = 0;
  bool pinned_blocked = false;
  std::array<u64, kPageCacheIoContextCount> evictions_by_context{};
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    before = SnapshotUnlocked(*ledger);
    flushed = FlushDirtyUnlocked(ledger, input.checkpoint_generation);
    const u64 target_pages = input.target_resident_pages == 0
        ? policy.max_resident_pages
        : input.target_resident_pages;
    PageCacheSnapshot current = SnapshotUnlocked(*ledger);
    while ((target_pages != 0 && current.resident_pages > target_pages) ||
           (policy.max_resident_bytes != 0 && current.resident_bytes > policy.max_resident_bytes)) {
      PageCachePolicy eviction_policy = policy;
      eviction_policy.allow_dirty_eviction = false;
      PageCacheEntry* selected = SelectEvictionCandidate(ledger,
                                                         eviction_policy,
                                                         PageCacheIoContext::normal,
                                                         nullptr,
                                                         nullptr);
      if (selected == nullptr) {
        pinned_blocked = CountPinnedResidentPages(*ledger) != 0;
        break;
      }
      EvictUnlocked(ledger, selected, &evictions_by_context);
      ++evicted;
      current = SnapshotUnlocked(*ledger);
    }
    after = SnapshotUnlocked(*ledger);
  }
  for (u64 i = 0; i < evicted; ++i) {
    (void)scratchbird::core::metrics::RecordPageCacheEviction("all", "all", "all", "memory_pressure");
  }
  RecordContextEvictionMetrics(evictions_by_context, "evicted", "memory_pressure");
  PublishSnapshot(after);

  if (pinned_blocked) {
    PageCacheLifecycleResult result =
        LifecycleError("CACHE.CHECKPOINT_MEMORY_PRESSURE_PINNED",
                       "storage.page_cache.memory_pressure_pinned");
    result.flushed_pages = flushed;
    result.evicted_pages = evicted;
    result.snapshot = after;
    result.publication = BasePublication(input, before, PageCacheLifecycleState::memory_pressure,
                                         PageCacheCheckpointMode::wait_checkpoint);
    result.publication.flushed_pages = flushed;
    result.publication.evicted_pages = evicted;
    result.publication.dirty_pages_after = after.dirty_pages;
    result.publication.memory_pressure_handled = false;
    result.publication.ordinary_admission_allowed = false;
    AddDiagnostic(&result.publication, result.diagnostic.diagnostic_code);
    AppendFrameOwnershipEvidence(&result.evidence, result.snapshot);
    return result;
  }

  PageCacheLifecycleResult result = LifecycleOk(PageCacheLifecycleState::active);
  result.flushed_pages = flushed;
  result.evicted_pages = evicted;
  result.snapshot = after;
  result.publication = BasePublication(input, before, result.state, PageCacheCheckpointMode::wait_checkpoint);
  result.publication.flushed_pages = flushed;
  result.publication.evicted_pages = evicted;
  result.publication.dirty_pages_after = after.dirty_pages;
  result.publication.writeback_complete = after.dirty_pages == 0;
  result.publication.memory_pressure_handled = true;
  result.publication.ordinary_admission_allowed = true;
  AddDiagnostic(&result.publication, "CACHE.CHECKPOINT_MEMORY_PRESSURE_HANDLED");
  AppendFrameOwnershipEvidence(&result.evidence, result.snapshot);
  return result;
}

PageCacheLifecycleResult ShutdownFlushPageCacheLifecycle(PageCacheLedger* ledger,
                                                         const PageCacheLifecycleInput& input) {
  if (ledger == nullptr) {
    return LifecycleError("CACHE.CHECKPOINT_LEDGER_REQUIRED",
                          "storage.page_cache.ledger_required");
  }
  const auto validation = ValidateLifecycleInput(input, true);
  if (!validation.ok()) {
    return MakeLifecycleInputError(validation, input, PageCacheCheckpointMode::shutdown_flush, ledger);
  }
  if (!input.writeback_allowed || !input.checkpoint_allowed) {
    return MakeLifecycleInputError(
        LifecycleError("CACHE.CHECKPOINT_SHUTDOWN_FLUSH_REFUSED",
                       "storage.page_cache.shutdown_flush_refused"),
        input,
        PageCacheCheckpointMode::shutdown_flush,
        ledger);
  }

  PageCacheSnapshot before;
  PageCacheSnapshot after;
  u64 flushed = 0;
  u64 evicted = 0;
  std::array<u64, kPageCacheIoContextCount> evictions_by_context{};
  {
    std::lock_guard<std::mutex> lock(ledger->mutex);
    before = SnapshotUnlocked(*ledger);
    if (CountPinnedResidentPages(*ledger) != 0) {
      PageCacheLifecycleResult result =
          LifecycleError("CACHE.CHECKPOINT_SHUTDOWN_PINNED_PAGES",
                         "storage.page_cache.shutdown_pinned_pages");
      result.snapshot = before;
      result.publication = BasePublication(input, before, PageCacheLifecycleState::draining,
                                           PageCacheCheckpointMode::shutdown_flush);
      result.publication.ordinary_admission_allowed = false;
      AddDiagnostic(&result.publication, result.diagnostic.diagnostic_code);
      return result;
    }
    flushed = FlushDirtyUnlocked(ledger, input.checkpoint_generation);
    for (auto& entry : ledger->entries) {
      if (!entry.resident) {
        continue;
      }
      EvictUnlocked(ledger, &entry, &evictions_by_context);
      ++evicted;
    }
    ledger->last_checkpoint_generation = input.checkpoint_generation;
    ++ledger->checkpoint_count;
    ++ledger->shutdown_flush_count;
    after = SnapshotUnlocked(*ledger);
  }
  for (u64 i = 0; i < evicted; ++i) {
    (void)scratchbird::core::metrics::RecordPageCacheEviction("all", "all", "all", "shutdown_flush");
  }
  RecordContextEvictionMetrics(evictions_by_context, "evicted", "shutdown_flush");
  PublishSnapshot(after);

  PageCacheLifecycleResult result = LifecycleOk(PageCacheLifecycleState::stopped);
  result.flushed_pages = flushed;
  result.evicted_pages = evicted;
  result.snapshot = after;
  result.publication = BasePublication(input, before, result.state, PageCacheCheckpointMode::shutdown_flush);
  result.publication.flushed_pages = flushed;
  result.publication.evicted_pages = evicted;
  result.publication.dirty_pages_after = after.dirty_pages;
  result.publication.writeback_complete = after.dirty_pages == 0;
  result.publication.checkpoint_complete = after.dirty_pages == 0;
  result.publication.clean_close_evidence = true;
  result.publication.shutdown_flush_complete = after.dirty_pages == 0;
  result.publication.ordinary_admission_allowed = false;
  AddDiagnostic(&result.publication, "CACHE.CHECKPOINT_SHUTDOWN_FLUSH_COMPLETE");
  return result;
}

std::string SerializePageCacheCheckpointJson(const PageCacheCheckpointPublication& publication,
                                             bool diagnostic_role) {
  std::ostringstream out;
  out << "{\"page_cache_checkpoint\":{"
      << "\"database_uuid\":\"" << JsonEscape(publication.database_uuid) << "\","
      << "\"filespace_uuid\":\"" << JsonEscape(publication.filespace_uuid) << "\","
      << "\"database_lifecycle_state\":\"" << JsonEscape(publication.database_lifecycle_state) << "\","
      << "\"lifecycle_state\":\"" << PageCacheLifecycleStateName(publication.lifecycle_state) << "\","
      << "\"checkpoint_mode\":\"" << PageCacheCheckpointModeName(publication.checkpoint_mode) << "\","
      << "\"policy_generation\":" << publication.policy_generation << ","
      << "\"checkpoint_generation\":" << publication.checkpoint_generation << ","
      << "\"dirty_epoch\":" << publication.dirty_epoch << ","
      << "\"resident_pages\":" << publication.resident_pages << ","
      << "\"dirty_pages_before\":" << publication.dirty_pages_before << ","
      << "\"dirty_pages_after\":" << publication.dirty_pages_after << ","
      << "\"flushed_pages\":" << publication.flushed_pages << ","
      << "\"evicted_pages\":" << publication.evicted_pages << ","
      << "\"preload_complete\":" << (publication.preload_complete ? "true" : "false") << ","
      << "\"writeback_complete\":" << (publication.writeback_complete ? "true" : "false") << ","
      << "\"checkpoint_complete\":" << (publication.checkpoint_complete ? "true" : "false") << ","
      << "\"clean_close_evidence\":" << (publication.clean_close_evidence ? "true" : "false") << ","
      << "\"shutdown_flush_complete\":"
      << (publication.shutdown_flush_complete ? "true" : "false") << ","
      << "\"memory_pressure_handled\":"
      << (publication.memory_pressure_handled ? "true" : "false") << ","
      << "\"cluster_paths_failed_closed\":"
      << (publication.cluster_paths_failed_closed ? "true" : "false") << ","
      << "\"ordinary_admission_allowed\":"
      << (publication.ordinary_admission_allowed ? "true" : "false") << ","
      << "\"authority_boundary_valid\":"
      << (PageCacheAuthorityBoundaryValid(publication.authority_boundary) ? "true" : "false") << ","
      << "\"diagnostics\":[";
  for (std::size_t i = 0; i < publication.diagnostics.size(); ++i) {
    if (i != 0) {
      out << ',';
    }
    const std::string diagnostic = diagnostic_role
        ? publication.diagnostics[i]
        : publication.diagnostics[i].substr(0, publication.diagnostics[i].find(':'));
    out << "\"" << JsonEscape(diagnostic) << "\"";
  }
  out << "]}}";
  return out.str();
}

std::vector<std::string> PageCacheDiagnosticCodes() {
  return {"CACHE.CHECKPOINT_AUTHORITY_DENIED",
          "CACHE.CHECKPOINT_INPUT_INVALID",
          "CACHE.CHECKPOINT_CLUSTER_EXTERNAL_PROVIDER_REQUIRED",
          "CACHE.CHECKPOINT_LEDGER_REQUIRED",
          "CACHE.CHECKPOINT_PRELOAD_COMPLETE",
          "CACHE.CHECKPOINT_PRELOAD_FAILED",
          "CACHE.CHECKPOINT_WRITEBACK_COMPLETE",
          "CACHE.CHECKPOINT_WRITEBACK_REFUSED",
          "CACHE.CHECKPOINT_DIRTY_PAGES_REMAIN",
          "CACHE.CHECKPOINT_COMPLETE",
          "CACHE.CHECKPOINT_REFUSED",
          "CACHE.CHECKPOINT_NOT_STARTED",
          "page_cache_context_ring_exhausted_pinned_or_dirty",
          "page_cache_budget_exhausted_pinned_or_dirty",
          "CACHE.CHECKPOINT_MEMORY_PRESSURE_HANDLED",
          "CACHE.CHECKPOINT_MEMORY_PRESSURE_PINNED",
          "CACHE.CHECKPOINT_SHUTDOWN_FLUSH_COMPLETE",
          "CACHE.CHECKPOINT_SHUTDOWN_FLUSH_REFUSED",
          "CACHE.CHECKPOINT_SHUTDOWN_PINNED_PAGES"};
}

DiagnosticRecord MakePageCacheDiagnostic(Status status,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.cache");
}

}  // namespace scratchbird::storage::page
