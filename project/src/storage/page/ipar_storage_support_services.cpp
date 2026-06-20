// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_storage_support_services.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::storage::page {
namespace platform = scratchbird::core::platform;

namespace {

Status SupportOkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::storage_page};
}

Status SupportErrorStatus() {
  return {platform::StatusCode::platform_required_feature_missing,
          platform::Severity::error, platform::Subsystem::storage_page};
}

bool AddWouldOverflow(u64 left, u64 right) {
  return left > std::numeric_limits<u64>::max() - right;
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(std::vector<std::string>* evidence,
                 std::string key,
                 std::string value) {
  evidence->push_back(std::move(key) + "=" + std::move(value));
}

u64 StableHash(std::string_view value) {
  u64 hash = 1469598103934665603ull;
  for (const char ch : value) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool EpochMatches(const IparCacheEpoch& left, const IparCacheEpoch& right) {
  return left.catalog_generation == right.catalog_generation &&
         left.page_generation == right.page_generation &&
         left.codec_generation == right.codec_generation &&
         left.filespace_generation == right.filespace_generation;
}

bool EpochStaleForAnyLane(const IparCacheEpoch& entry,
                          const IparCacheEpoch& current) {
  return entry.catalog_generation != current.catalog_generation ||
         entry.page_generation != current.page_generation ||
         entry.codec_generation != current.codec_generation ||
         entry.filespace_generation != current.filespace_generation;
}

u64 TemperatureWeight(IparPageTemperature temperature) {
  switch (temperature) {
    case IparPageTemperature::hot:
      return 300;
    case IparPageTemperature::warm:
      return 150;
    case IparPageTemperature::cold:
      return 20;
  }
  return 0;
}

IparPageTemperature TemperatureFromScore(u64 score) {
  if (score >= 100) {
    return IparPageTemperature::hot;
  }
  if (score >= 25) {
    return IparPageTemperature::warm;
  }
  return IparPageTemperature::cold;
}

u64 DirtyPriorityScore(const IparDirtyPageCandidate& page) {
  u64 score = page.age_microseconds / 1000;
  score += page.dirty_bytes / 4096;
  score += page.temperature_score;
  score += page.checkpoint_blocker ? 500000 : 0;
  score += page.shutdown_blocker ? 1000000 : 0;
  score += page.transaction_inventory_page ? 200000 : 0;
  return score;
}

std::string IntentConflictKey(const IparDmlLockIntent& intent) {
  return intent.object_id + "|" + intent.key_low + "|" + intent.key_high;
}

bool ModeCompatible(IparLockMode left, IparLockMode right) {
  return left == IparLockMode::shared && right == IparLockMode::shared;
}

std::string RangeHigh(const IparDmlLockIntent& intent) {
  return intent.key_high.empty() ? intent.key_low : intent.key_high;
}

bool RangesOverlap(const IparDmlLockIntent& left,
                   const IparDmlLockIntent& right) {
  if (left.object_id != right.object_id) {
    return false;
  }
  const std::string left_high = RangeHigh(left);
  const std::string right_high = RangeHigh(right);
  return left.key_low <= right_high && right.key_low <= left_high;
}

bool HasPathTo(const std::map<std::string, std::vector<std::string>>& graph,
               const std::string& from,
               const std::string& to,
               std::set<std::string>* seen) {
  if (from == to) {
    return true;
  }
  if (!seen->insert(from).second) {
    return false;
  }
  const auto found = graph.find(from);
  if (found == graph.end()) {
    return false;
  }
  for (const auto& next : found->second) {
    if (HasPathTo(graph, next, to, seen)) {
      return true;
    }
  }
  return false;
}

template <typename TEntry, typename TKeyMatch>
Status PutCacheEntry(std::vector<TEntry>* entries,
                     u64 max_entries,
                     TEntry entry,
                     TKeyMatch key_matches) {
  if (entries == nullptr || max_entries == 0) {
    return SupportErrorStatus();
  }
  for (auto& existing : *entries) {
    if (key_matches(existing, entry)) {
      existing = std::move(entry);
      return SupportOkStatus();
    }
  }
  if (entries->size() >= max_entries) {
    auto victim = entries->end();
    for (auto it = entries->begin(); it != entries->end(); ++it) {
      if (it->pin_count != 0) {
        continue;
      }
      if (victim == entries->end() || it->last_use_tick < victim->last_use_tick) {
        victim = it;
      }
    }
    if (victim == entries->end()) {
      return SupportErrorStatus();
    }
    entries->erase(victim);
  }
  entries->push_back(std::move(entry));
  return SupportOkStatus();
}

template <typename TResult>
TResult CacheMiss(std::string diagnostic_code,
                  std::string message_key,
                  std::string detail) {
  TResult result;
  result.status = SupportErrorStatus();
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  AddEvidence(&result.evidence, "ipar.cache.hit", "false");
  return result;
}

u64 QueueBytes(const std::vector<IparPostCommitWorkItem>& items) {
  u64 total = 0;
  for (const auto& item : items) {
    if (AddWouldOverflow(total, item.bytes)) {
      return std::numeric_limits<u64>::max();
    }
    total += item.bytes;
  }
  return total;
}

bool EnqueuePostCommitItem(const IparPostCommitQueuePolicy& policy,
                           const IparPostCommitWorkItem& item,
                           std::vector<IparPostCommitWorkItem>* queue) {
  if (queue->size() >= policy.max_items_per_queue) {
    return false;
  }
  const u64 current_bytes = QueueBytes(*queue);
  if (AddWouldOverflow(current_bytes, item.bytes) ||
      current_bytes + item.bytes > policy.max_bytes_per_queue) {
    return false;
  }
  queue->push_back(item);
  return true;
}

u64 MaintenanceDebtScore(const IparMaintenanceDebtEntry& entry) {
  return entry.priority_boost + entry.debt_units * 10 +
         entry.debt_bytes / 4096 + entry.age_microseconds / 1000;
}

}  // namespace

const char* IparSupportQueueKindName(IparSupportQueueKind kind) {
  switch (kind) {
    case IparSupportQueueKind::worktable_scratch:
      return "worktable_scratch";
    case IparSupportQueueKind::async_io:
      return "async_io";
    case IparSupportQueueKind::dirty_writeback:
      return "dirty_writeback";
    case IparSupportQueueKind::post_commit_diagnostics:
      return "post_commit_diagnostics";
    case IparSupportQueueKind::post_commit_maintenance:
      return "post_commit_maintenance";
    case IparSupportQueueKind::page_zeroing:
      return "page_zeroing";
    case IparSupportQueueKind::worker_warmup:
      return "worker_warmup";
  }
  return "unknown";
}

const char* IparPageTemperatureName(IparPageTemperature temperature) {
  switch (temperature) {
    case IparPageTemperature::cold:
      return "cold";
    case IparPageTemperature::warm:
      return "warm";
    case IparPageTemperature::hot:
      return "hot";
  }
  return "unknown";
}

const char* IparLockModeName(IparLockMode mode) {
  switch (mode) {
    case IparLockMode::shared:
      return "shared";
    case IparLockMode::update:
      return "update";
    case IparLockMode::exclusive:
      return "exclusive";
  }
  return "unknown";
}

const char* IparPostCommitWorkKindName(IparPostCommitWorkKind kind) {
  switch (kind) {
    case IparPostCommitWorkKind::diagnostics:
      return "diagnostics";
    case IparPostCommitWorkKind::maintenance:
      return "maintenance";
    case IparPostCommitWorkKind::index_cleanup:
      return "index_cleanup";
    case IparPostCommitWorkKind::large_value_cleanup:
      return "large_value_cleanup";
  }
  return "unknown";
}

const char* IparMaintenanceDebtFamilyName(IparMaintenanceDebtFamily family) {
  switch (family) {
    case IparMaintenanceDebtFamily::storage_compaction:
      return "storage_compaction";
    case IparMaintenanceDebtFamily::dirty_writeback:
      return "dirty_writeback";
    case IparMaintenanceDebtFamily::large_value:
      return "large_value";
    case IparMaintenanceDebtFamily::index_shard:
      return "index_shard";
    case IparMaintenanceDebtFamily::scratch_cleanup:
      return "scratch_cleanup";
  }
  return "unknown";
}

bool IparStorageSupportAuthorityBoundarySafe(
    const IparStorageSupportAuthorityBoundary& authority) {
  return authority.durable_transaction_inventory_authority &&
         !authority.support_service_finality_authority &&
         !authority.support_service_visibility_authority &&
         !authority.parser_finality_authority &&
         !authority.client_finality_authority &&
         !authority.provider_finality_authority &&
         !authority.publication_marker_finality_authority;
}

DiagnosticRecord MakeIparStorageSupportDiagnostic(Status status,
                                                  std::string diagnostic_code,
                                                  std::string message_key,
                                                  std::string detail) {
  std::vector<platform::DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return platform::MakeDiagnostic(
      status.code, status.severity, status.subsystem,
      std::move(diagnostic_code), std::move(message_key),
      std::move(arguments), {}, "storage.page.ipar_support_services",
      "preserve MGA transaction inventory authority and keep support work bounded");
}

IparBoundedQueuePlan PlanIparBoundedSupportQueue(
    const IparBoundedQueuePolicy& policy,
    const std::vector<IparSupportQueueItem>& items) {
  IparBoundedQueuePlan result;
  result.status = SupportOkStatus();
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, "SB_IPAR_STORAGE_SUPPORT_OK",
      "storage.ipar.support.queue.ok", "bounded support queue planned");
  AddEvidence(&result.evidence, "ipar.support.queue.bounded", "true");
  AddEvidence(&result.evidence, "ipar.support.queue.max_items",
              std::to_string(policy.max_items));
  AddEvidence(&result.evidence, "ipar.support.queue.max_bytes",
              std::to_string(policy.max_bytes));

  if (policy.max_items == 0 || policy.max_bytes == 0) {
    result.status = SupportErrorStatus();
    result.fail_closed = true;
    result.refused_count = items.size();
    result.refused_items = items;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_SUPPORT_QUEUE_POLICY_INVALID",
        "storage.ipar.support.queue.policy_invalid",
        "queue policy must have non-zero item and byte limits");
    return result;
  }

  std::vector<IparSupportQueueItem> ordered = items;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const IparSupportQueueItem& left,
                      const IparSupportQueueItem& right) {
                     if (left.priority != right.priority) {
                       return left.priority > right.priority;
                     }
                     return left.item_id < right.item_id;
                   });

  for (const auto& item : ordered) {
    if (!IparStorageSupportAuthorityBoundarySafe(item.authority)) {
      result.status = SupportErrorStatus();
      result.fail_closed = true;
      result.refused_items.push_back(item);
      ++result.refused_count;
      result.diagnostic = MakeIparStorageSupportDiagnostic(
          result.status, "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
          "storage.ipar.support.authority_drift", item.item_id);
      AddEvidence(&result.evidence, "ipar.support.authority_drift", item.item_id);
      return result;
    }
    if (item.requires_committed_transaction_evidence &&
        !item.committed_transaction_evidence_present) {
      result.refused_items.push_back(item);
      ++result.refused_count;
      continue;
    }
    if (result.accepted_count >= policy.max_items ||
        AddWouldOverflow(result.accepted_bytes, item.bytes) ||
        result.accepted_bytes + item.bytes > policy.max_bytes) {
      result.refused_items.push_back(item);
      ++result.refused_count;
      continue;
    }
    result.accepted_items.push_back(item);
    ++result.accepted_count;
    result.accepted_bytes += item.bytes;
  }

  result.accepted = result.accepted_count != 0;
  if (result.refused_count != 0) {
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_SUPPORT_QUEUE_BOUNDED",
        "storage.ipar.support.queue.bounded",
        "one or more support queue items were refused by capacity or commit evidence");
  }
  AddEvidence(&result.evidence, "ipar.support.queue.accepted_count",
              std::to_string(result.accepted_count));
  AddEvidence(&result.evidence, "ipar.support.queue.refused_count",
              std::to_string(result.refused_count));
  AddEvidence(&result.evidence, "ipar.support.queue.accepted_bytes",
              std::to_string(result.accepted_bytes));
  AddEvidence(&result.evidence, "ipar.support.finality_authority", "false");
  AddEvidence(&result.evidence, "ipar.support.visibility_authority", "false");
  return result;
}

IparAppendLocalityPlan PlanIparAppendLocality(
    const IparAppendLocalityRequest& request) {
  IparAppendLocalityPlan result;
  result.status = SupportOkStatus();
  if (!IparStorageSupportAuthorityBoundarySafe(request.authority)) {
    result.status = SupportErrorStatus();
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
        "storage.ipar.support.authority_drift", request.object_id);
    return result;
  }
  if (request.object_id.empty() || request.encoded_row_bytes == 0) {
    result.status = SupportErrorStatus();
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_APPEND_LOCALITY_INVALID",
        "storage.ipar.append_locality.invalid",
        "object id and encoded row bytes are required");
    return result;
  }

  const IparAppendLocalityCandidate* selected = nullptr;
  u64 best_score = 0;
  for (const auto& candidate : request.candidates) {
    if (!candidate.active || !candidate.zero_initialized ||
        candidate.object_id != request.object_id ||
        candidate.free_bytes < request.encoded_row_bytes) {
      continue;
    }
    u64 score = candidate.free_bytes + TemperatureWeight(candidate.temperature);
    if (candidate.page_id == request.current_append_cursor_page_id) {
      score += 1000000;
    }
    if (candidate.append_cursor_distance < 100000) {
      score += 100000 - candidate.append_cursor_distance;
    }
    if (selected == nullptr || score > best_score ||
        (score == best_score && candidate.page_number < selected->page_number)) {
      selected = &candidate;
      best_score = score;
    }
  }

  if (selected == nullptr) {
    result.scheduled_preallocation_pages = request.preallocation_quantum_pages;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_APPEND_PREALLOCATE",
        "storage.ipar.append_locality.preallocate",
        "no active zeroed candidate has enough free bytes");
    AddEvidence(&result.evidence, "ipar.append.preallocation_pages",
                std::to_string(result.scheduled_preallocation_pages));
    AddEvidence(&result.evidence, "ipar.append.finality_authority", "false");
    return result;
  }

  result.selected = true;
  result.selected_page_id = selected->page_id;
  result.selected_page_number = selected->page_number;
  result.locality_group = selected->filespace_id + ":" + selected->object_id +
                          ":append:" + std::to_string(selected->page_number / 16);
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, "SB_IPAR_APPEND_LOCALITY_SELECTED",
      "storage.ipar.append_locality.selected", result.selected_page_id);
  AddEvidence(&result.evidence, "ipar.append.selected_page", result.selected_page_id);
  AddEvidence(&result.evidence, "ipar.append.locality_group", result.locality_group);
  AddEvidence(&result.evidence, "ipar.append.cursor_preserved",
              BoolText(selected->page_id == request.current_append_cursor_page_id));
  AddEvidence(&result.evidence, "ipar.append.finality_authority", "false");
  return result;
}

IparBoundedQueuePlan PlanIparWorktableScratchAgent(
    const IparScratchSpaceRequest& request) {
  auto plan = PlanIparBoundedSupportQueue(request.queue_policy,
                                          request.scratch_items);
  plan.evidence.push_back("ipar.worktable_scratch.agent=bounded");
  plan.evidence.push_back("ipar.worktable_scratch.finality_authority=false");
  return plan;
}

IparShardOwnershipMap BuildIparObjectIndexPageShardOwnershipMap(
    const IparShardMapRequest& request) {
  IparShardOwnershipMap result;
  result.status = SupportOkStatus();
  if (!IparStorageSupportAuthorityBoundarySafe(request.authority)) {
    result.status = SupportErrorStatus();
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
        "storage.ipar.support.authority_drift", request.index_id);
    return result;
  }
  if (request.object_id.empty() || request.index_id.empty() ||
      request.last_page < request.first_page || request.shard_count == 0 ||
      request.latch_partition_count == 0 || request.worker_count == 0) {
    result.status = SupportErrorStatus();
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_SHARD_MAP_INVALID",
        "storage.ipar.shard_map.invalid",
        "object, index, page range, shard, latch, and worker counts are required");
    return result;
  }

  const u64 page_count = request.last_page - request.first_page + 1;
  const u64 pages_per_shard =
      (page_count + request.shard_count - 1) / request.shard_count;
  u64 page = request.first_page;
  for (u64 shard_id = 0; shard_id < request.shard_count && page <= request.last_page;
       ++shard_id) {
    IparIndexPageShard shard;
    shard.shard_id = shard_id;
    shard.page_low = page;
    shard.page_high = std::min(request.last_page, page + pages_per_shard - 1);
    shard.owner_worker_id = shard_id % request.worker_count;
    shard.latch_partition = shard_id % request.latch_partition_count;
    shard.shard_key = request.object_id + ":" + request.index_id + ":shard:" +
                      std::to_string(shard_id);
    result.shards.push_back(std::move(shard));
    page = result.shards.back().page_high + 1;
  }
  result.built = true;
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, "SB_IPAR_SHARD_MAP_BUILT",
      "storage.ipar.shard_map.built", request.index_id);
  AddEvidence(&result.evidence, "ipar.shard_map.shard_count",
              std::to_string(result.shards.size()));
  AddEvidence(&result.evidence, "ipar.shard_map.owner_authority", "page_agent");
  AddEvidence(&result.evidence, "ipar.shard_map.finality_authority", "false");
  return result;
}

const IparIndexPageShard* FindIparShardForPage(
    const IparShardOwnershipMap& map,
    u64 page_number) {
  for (const auto& shard : map.shards) {
    if (page_number >= shard.page_low && page_number <= shard.page_high) {
      return &shard;
    }
  }
  return nullptr;
}

IparDirtyPagePriorityPlan PlanIparDirtyPagePriority(
    const IparDirtyPagePriorityPolicy& policy,
    const std::vector<IparDirtyPageCandidate>& candidates) {
  IparDirtyPagePriorityPlan result;
  result.status = SupportOkStatus();
  if (policy.max_pages == 0 || policy.max_bytes == 0) {
    result.status = SupportErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_DIRTY_PRIORITY_POLICY_INVALID",
        "storage.ipar.dirty_priority.policy_invalid",
        "dirty page priority requires non-zero page and byte bounds");
    return result;
  }

  std::vector<IparDirtyPageAssignment> assignments;
  for (const auto& page : candidates) {
    if (!IparStorageSupportAuthorityBoundarySafe(page.authority)) {
      result.status = SupportErrorStatus();
      result.fail_closed = true;
      result.diagnostic = MakeIparStorageSupportDiagnostic(
          result.status, "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
          "storage.ipar.support.authority_drift", page.page_id);
      return result;
    }
    if (page.pin_count != 0) {
      ++result.skipped_pinned_pages;
      continue;
    }
    assignments.push_back({page, DirtyPriorityScore(page), 0});
  }
  std::stable_sort(assignments.begin(), assignments.end(),
                   [](const IparDirtyPageAssignment& left,
                      const IparDirtyPageAssignment& right) {
                     if (left.priority_score != right.priority_score) {
                       return left.priority_score > right.priority_score;
                     }
                     return left.page.page_number < right.page.page_number;
                   });

  u64 rank = 1;
  for (auto& assignment : assignments) {
    if (result.selected_pages >= policy.max_pages ||
        AddWouldOverflow(result.selected_bytes, assignment.page.dirty_bytes) ||
        result.selected_bytes + assignment.page.dirty_bytes > policy.max_bytes) {
      continue;
    }
    assignment.priority_rank = rank++;
    result.selected_bytes += assignment.page.dirty_bytes;
    ++result.selected_pages;
    result.assignments.push_back(assignment);
  }

  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, "SB_IPAR_DIRTY_PRIORITY_PLANNED",
      "storage.ipar.dirty_priority.planned",
      "dirty page physical writeback priority planned");
  AddEvidence(&result.evidence, "ipar.dirty_priority.selected_pages",
              std::to_string(result.selected_pages));
  AddEvidence(&result.evidence, "ipar.dirty_priority.skipped_pinned_pages",
              std::to_string(result.skipped_pinned_pages));
  AddEvidence(&result.evidence, "ipar.dirty_priority.finality_authority", "false");
  return result;
}

Status PutIparLargeValueStreamContext(
    IparLargeValueStreamContextCache* cache,
    IparLargeValueStreamContext entry) {
  if (cache == nullptr) {
    return SupportErrorStatus();
  }
  entry.last_use_tick = cache->next_tick++;
  return PutCacheEntry(&cache->entries, cache->max_entries, std::move(entry),
                       [](const IparLargeValueStreamContext& left,
                          const IparLargeValueStreamContext& right) {
                         return left.stream_id == right.stream_id;
                       });
}

IparCacheLookupResult<IparLargeValueStreamContext>
LookupIparLargeValueStreamContext(IparLargeValueStreamContextCache* cache,
                                  const std::string& stream_id,
                                  const IparCacheEpoch& epoch) {
  using Result = IparCacheLookupResult<IparLargeValueStreamContext>;
  if (cache == nullptr) {
    return CacheMiss<Result>("SB_IPAR_CACHE_MISS",
                             "storage.ipar.large_value_cache.miss",
                             "large-value stream cache is absent");
  }
  for (auto& entry : cache->entries) {
    if (entry.stream_id != stream_id) {
      continue;
    }
    if (!EpochMatches(entry.epoch, epoch)) {
      ++cache->miss_count;
      auto result = CacheMiss<Result>("SB_IPAR_CACHE_STALE",
                                      "storage.ipar.large_value_cache.stale",
                                      stream_id);
      result.stale = true;
      result.entry = entry;
      return result;
    }
    ++cache->hit_count;
    entry.last_use_tick = cache->next_tick++;
    Result result;
    result.status = SupportOkStatus();
    result.cache_hit = true;
    result.entry = entry;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_CACHE_HIT",
        "storage.ipar.large_value_cache.hit", stream_id);
    AddEvidence(&result.evidence, "ipar.large_value_cache.hit", "true");
    return result;
  }
  ++cache->miss_count;
  return CacheMiss<Result>("SB_IPAR_CACHE_MISS",
                           "storage.ipar.large_value_cache.miss", stream_id);
}

u64 InvalidateStaleIparLargeValueStreamContexts(
    IparLargeValueStreamContextCache* cache,
    const IparCacheEpoch& epoch) {
  if (cache == nullptr) {
    return 0;
  }
  const auto before = cache->entries.size();
  cache->entries.erase(
      std::remove_if(cache->entries.begin(), cache->entries.end(),
                     [&](const IparLargeValueStreamContext& entry) {
                       return EpochStaleForAnyLane(entry.epoch, epoch);
                     }),
      cache->entries.end());
  return static_cast<u64>(before - cache->entries.size());
}

Status PutIparOrdinaryPageCodecContext(
    IparOrdinaryPageCodecContextCache* cache,
    IparOrdinaryPageCodecContext entry) {
  if (cache == nullptr) {
    return SupportErrorStatus();
  }
  entry.last_use_tick = cache->next_tick++;
  return PutCacheEntry(&cache->entries, cache->max_entries, std::move(entry),
                       [](const IparOrdinaryPageCodecContext& left,
                          const IparOrdinaryPageCodecContext& right) {
                         return left.codec_id == right.codec_id &&
                                left.page_family == right.page_family;
                       });
}

IparCacheLookupResult<IparOrdinaryPageCodecContext>
LookupIparOrdinaryPageCodecContext(IparOrdinaryPageCodecContextCache* cache,
                                   const std::string& codec_id,
                                   const std::string& page_family,
                                   const IparCacheEpoch& epoch) {
  using Result = IparCacheLookupResult<IparOrdinaryPageCodecContext>;
  if (cache == nullptr) {
    return CacheMiss<Result>("SB_IPAR_CACHE_MISS",
                             "storage.ipar.codec_cache.miss",
                             "ordinary page codec cache is absent");
  }
  for (auto& entry : cache->entries) {
    if (entry.codec_id != codec_id || entry.page_family != page_family) {
      continue;
    }
    if (!EpochMatches(entry.epoch, epoch)) {
      ++cache->miss_count;
      auto result = CacheMiss<Result>("SB_IPAR_CACHE_STALE",
                                      "storage.ipar.codec_cache.stale",
                                      codec_id);
      result.stale = true;
      result.entry = entry;
      return result;
    }
    ++cache->hit_count;
    entry.last_use_tick = cache->next_tick++;
    Result result;
    result.status = SupportOkStatus();
    result.cache_hit = true;
    result.entry = entry;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_CACHE_HIT",
        "storage.ipar.codec_cache.hit", codec_id);
    AddEvidence(&result.evidence, "ipar.codec_cache.hit", "true");
    return result;
  }
  ++cache->miss_count;
  return CacheMiss<Result>("SB_IPAR_CACHE_MISS",
                           "storage.ipar.codec_cache.miss", codec_id);
}

u64 InvalidateStaleIparOrdinaryPageCodecContexts(
    IparOrdinaryPageCodecContextCache* cache,
    const IparCacheEpoch& epoch) {
  if (cache == nullptr) {
    return 0;
  }
  const auto before = cache->entries.size();
  cache->entries.erase(
      std::remove_if(cache->entries.begin(), cache->entries.end(),
                     [&](const IparOrdinaryPageCodecContext& entry) {
                       return EpochStaleForAnyLane(entry.epoch, epoch);
                     }),
      cache->entries.end());
  return static_cast<u64>(before - cache->entries.size());
}

Status PutIparFilesystemFilespaceHandle(
    IparFilesystemFilespaceHandleCache* cache,
    IparFilesystemFilespaceHandle entry) {
  if (cache == nullptr) {
    return SupportErrorStatus();
  }
  entry.last_use_tick = cache->next_tick++;
  if (entry.handle_ordinal == 0) {
    entry.handle_ordinal = cache->next_handle_ordinal++;
  }
  return PutCacheEntry(&cache->entries, cache->max_entries, std::move(entry),
                       [](const IparFilesystemFilespaceHandle& left,
                          const IparFilesystemFilespaceHandle& right) {
                         return left.filespace_id == right.filespace_id &&
                                left.path == right.path;
                       });
}

IparCacheLookupResult<IparFilesystemFilespaceHandle>
LookupIparFilesystemFilespaceHandle(IparFilesystemFilespaceHandleCache* cache,
                                    const std::string& filespace_id,
                                    const std::string& path,
                                    const IparCacheEpoch& epoch) {
  using Result = IparCacheLookupResult<IparFilesystemFilespaceHandle>;
  if (cache == nullptr) {
    return CacheMiss<Result>("SB_IPAR_CACHE_MISS",
                             "storage.ipar.filespace_handle_cache.miss",
                             "filespace handle cache is absent");
  }
  for (auto& entry : cache->entries) {
    if (entry.filespace_id != filespace_id || entry.path != path) {
      continue;
    }
    if (!EpochMatches(entry.epoch, epoch)) {
      ++cache->miss_count;
      auto result = CacheMiss<Result>(
          "SB_IPAR_CACHE_STALE",
          "storage.ipar.filespace_handle_cache.stale", filespace_id);
      result.stale = true;
      result.entry = entry;
      return result;
    }
    ++cache->hit_count;
    entry.last_use_tick = cache->next_tick++;
    Result result;
    result.status = SupportOkStatus();
    result.cache_hit = true;
    result.entry = entry;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_CACHE_HIT",
        "storage.ipar.filespace_handle_cache.hit", filespace_id);
    AddEvidence(&result.evidence, "ipar.filespace_handle_cache.hit", "true");
    return result;
  }
  ++cache->miss_count;
  return CacheMiss<Result>("SB_IPAR_CACHE_MISS",
                           "storage.ipar.filespace_handle_cache.miss",
                           filespace_id);
}

u64 InvalidateStaleIparFilesystemFilespaceHandles(
    IparFilesystemFilespaceHandleCache* cache,
    const IparCacheEpoch& epoch) {
  if (cache == nullptr) {
    return 0;
  }
  const auto before = cache->entries.size();
  cache->entries.erase(
      std::remove_if(cache->entries.begin(), cache->entries.end(),
                     [&](const IparFilesystemFilespaceHandle& entry) {
                       return EpochStaleForAnyLane(entry.epoch, epoch);
                     }),
      cache->entries.end());
  return static_cast<u64>(before - cache->entries.size());
}

IparPageZeroingResult PlanIparPageZeroing(
    const IparPageZeroingRequest& request) {
  IparPageZeroingResult result;
  result.status = SupportOkStatus();
  if (!IparStorageSupportAuthorityBoundarySafe(request.authority)) {
    result.status = SupportErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
        "storage.ipar.support.authority_drift", "page zeroing");
    return result;
  }
  if (request.page_size == 0 || request.max_queue_pages == 0) {
    result.status = SupportErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_ZEROING_POLICY_INVALID",
        "storage.ipar.zeroing.policy_invalid",
        "page size and queue bound are required");
    return result;
  }

  const u64 admitted_pages =
      std::min(request.page_count, request.max_queue_pages);
  result.refused_count = request.page_count - admitted_pages;
  for (u64 offset = 0; offset < admitted_pages; ++offset) {
    IparZeroedPage page;
    page.page_number = request.first_page_number + offset;
    page.ticket_id = "zero-ticket:" + std::to_string(page.page_number);
    page.bytes.assign(static_cast<std::size_t>(request.page_size), byte{0});
    result.zeroed_pages.push_back(std::move(page));
  }
  result.zeroed_count = admitted_pages;
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status,
      result.refused_count == 0 ? "SB_IPAR_PAGE_ZEROING_COMPLETE"
                                : "SB_IPAR_ZEROING_QUEUE_BOUNDED",
      result.refused_count == 0 ? "storage.ipar.zeroing.complete"
                                : "storage.ipar.zeroing.queue_bounded",
      "page initialization zeroing planned");
  AddEvidence(&result.evidence, "ipar.zeroing.zeroed_count",
              std::to_string(result.zeroed_count));
  AddEvidence(&result.evidence, "ipar.zeroing.refused_count",
              std::to_string(result.refused_count));
  AddEvidence(&result.evidence, "ipar.zeroing.finality_authority", "false");
  return result;
}

IparPhysicalLocalityPlan PlanIparPhysicalLocalityTemperature(
    const std::vector<IparPhysicalLocalitySample>& samples) {
  IparPhysicalLocalityPlan result;
  result.status = SupportOkStatus();
  for (const auto& sample : samples) {
    IparPhysicalLocalityDecision decision;
    decision.page_id = sample.page_id;
    decision.temperature_score =
        sample.recent_writes * 4 + sample.recent_reads * 2 +
        (sample.dirty ? 10 : 0) +
        (sample.age_microseconds < 1000000 ? 20 : 0);
    decision.temperature = TemperatureFromScore(decision.temperature_score);
    switch (decision.temperature) {
      case IparPageTemperature::hot:
        decision.target_region = "hot_append_region";
        break;
      case IparPageTemperature::warm:
        decision.target_region = "warm_mixed_region";
        break;
      case IparPageTemperature::cold:
        decision.target_region = "cold_compaction_region";
        break;
    }
    result.decisions.push_back(std::move(decision));
  }
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, "SB_IPAR_PHYSICAL_LOCALITY_PLANNED",
      "storage.ipar.physical_locality.planned",
      "physical locality and temperature planned");
  AddEvidence(&result.evidence, "ipar.physical_locality.decision_count",
              std::to_string(result.decisions.size()));
  AddEvidence(&result.evidence, "ipar.physical_locality.finality_authority",
              "false");
  return result;
}

IparDmlLockPartitionPlan PlanIparDmlLockLatchPartitioning(
    u64 partition_count,
    u64 latch_count,
    const std::vector<IparDmlLockIntent>& intents) {
  IparDmlLockPartitionPlan result;
  result.status = SupportOkStatus();
  if (partition_count == 0 || latch_count == 0) {
    result.status = SupportErrorStatus();
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_LOCK_PARTITION_POLICY_INVALID",
        "storage.ipar.lock_partition.policy_invalid",
        "partition and latch counts must be non-zero");
    return result;
  }
  for (const auto& intent : intents) {
    IparDmlLockPartitionAssignment assignment;
    assignment.intent = intent;
    assignment.partition_id = StableHash(IntentConflictKey(intent)) % partition_count;
    assignment.latch_id = assignment.partition_id % latch_count;
    result.assignments.push_back(std::move(assignment));
  }
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, "SB_IPAR_LOCK_PARTITION_PLANNED",
      "storage.ipar.lock_partition.planned",
      "DML lock and latch partitions planned");
  AddEvidence(&result.evidence, "ipar.lock_partition.partition_count",
              std::to_string(partition_count));
  AddEvidence(&result.evidence, "ipar.lock_partition.assignment_count",
              std::to_string(result.assignments.size()));
  return result;
}

IparConflictPredictionPlan PredictIparDeadlockAndConflicts(
    const IparConflictPredictionRequest& request) {
  IparConflictPredictionPlan result;
  result.status = SupportOkStatus();
  std::map<std::string, std::vector<std::string>> graph;
  for (const auto& edge : request.existing_wait_edges) {
    graph[edge.waiter_transaction_id].push_back(edge.holder_transaction_id);
    result.serialization_edges.push_back(edge);
  }

  for (std::size_t i = 0; i < request.intents.size(); ++i) {
    for (std::size_t j = i + 1; j < request.intents.size(); ++j) {
      const auto& left = request.intents[i];
      const auto& right = request.intents[j];
      if (!RangesOverlap(left, right) || ModeCompatible(left.mode, right.mode)) {
        continue;
      }
      const bool right_waits =
          left.local_transaction_id == 0 ||
          right.local_transaction_id == 0 ||
          left.local_transaction_id <= right.local_transaction_id;
      IparWaitEdge edge;
      edge.waiter_transaction_id =
          right_waits ? right.transaction_id : left.transaction_id;
      edge.holder_transaction_id =
          right_waits ? left.transaction_id : right.transaction_id;
      graph[edge.waiter_transaction_id].push_back(edge.holder_transaction_id);
      result.serialization_edges.push_back(edge);
      result.conflicts.push_back(
          {edge.waiter_transaction_id, edge.holder_transaction_id, left.object_id,
           "incompatible lock intent overlap"});
    }
  }

  for (const auto& edge : result.serialization_edges) {
    std::set<std::string> seen;
    if (HasPathTo(graph, edge.holder_transaction_id,
                  edge.waiter_transaction_id, &seen)) {
      result.deadlock_predicted = true;
      break;
    }
  }

  result.conflict_predicted = !result.conflicts.empty();
  result.serialization_required = !result.serialization_edges.empty();
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status,
      result.deadlock_predicted ? "SB_IPAR_CONFLICT_DEADLOCK_PREDICTED"
                                : (result.conflict_predicted
                                       ? "SB_IPAR_CONFLICT_SERIALIZED"
                                       : "SB_IPAR_CONFLICT_NONE"),
      result.deadlock_predicted ? "storage.ipar.conflict.deadlock_predicted"
                                : (result.conflict_predicted
                                       ? "storage.ipar.conflict.serialized"
                                       : "storage.ipar.conflict.none"),
      "conflict prediction is advisory and does not decide transaction outcome");
  AddEvidence(&result.evidence, "ipar.conflict.predicted",
              BoolText(result.conflict_predicted));
  AddEvidence(&result.evidence, "ipar.conflict.deadlock_predicted",
              BoolText(result.deadlock_predicted));
  AddEvidence(&result.evidence, "ipar.conflict.finality_authority", "false");
  return result;
}

IparLockPreacquisitionPlan PlanIparLockIntentPreacquisition(
    const IparLockPreacquisitionRequest& request) {
  IparLockPreacquisitionPlan result;
  result.status = SupportOkStatus();
  const u64 max_intents = request.max_intents;
  if (max_intents == 0 || request.partition_count == 0) {
    result.status = SupportErrorStatus();
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_LOCK_PREACQUIRE_POLICY_INVALID",
        "storage.ipar.lock_preacquire.policy_invalid",
        "lock pre-acquisition requires non-zero bounds");
    return result;
  }
  auto partitioned = PlanIparDmlLockLatchPartitioning(
      request.partition_count, request.partition_count, request.intents);
  std::vector<IparDmlLockPartitionAssignment> ordered = partitioned.assignments;
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const IparDmlLockPartitionAssignment& left,
                      const IparDmlLockPartitionAssignment& right) {
                     if (left.partition_id != right.partition_id) {
                       return left.partition_id < right.partition_id;
                     }
                     if (left.intent.object_id != right.intent.object_id) {
                       return left.intent.object_id < right.intent.object_id;
                     }
                     if (left.intent.key_low != right.intent.key_low) {
                       return left.intent.key_low < right.intent.key_low;
                     }
                     return left.intent.transaction_id < right.intent.transaction_id;
                   });
  for (const auto& assignment : ordered) {
    if (result.selected_count < max_intents) {
      result.selected.push_back(assignment);
      ++result.selected_count;
    } else {
      result.overflow.push_back(assignment.intent);
      ++result.overflow_count;
    }
  }
  result.bounded = result.overflow_count != 0;
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status,
      result.bounded ? "SB_IPAR_LOCK_PREACQUIRE_BOUNDED"
                     : "SB_IPAR_LOCK_PREACQUIRE_PLANNED",
      result.bounded ? "storage.ipar.lock_preacquire.bounded"
                     : "storage.ipar.lock_preacquire.planned",
      "lock intent pre-acquisition planned in stable partition order");
  AddEvidence(&result.evidence, "ipar.lock_preacquire.selected_count",
              std::to_string(result.selected_count));
  AddEvidence(&result.evidence, "ipar.lock_preacquire.overflow_count",
              std::to_string(result.overflow_count));
  return result;
}

IparPostCommitQueuePlan PlanIparPostCommitQueueSeparation(
    const IparPostCommitQueuePolicy& policy,
    const std::vector<IparPostCommitWorkItem>& items) {
  IparPostCommitQueuePlan result;
  result.status = SupportOkStatus();
  if (policy.max_items_per_queue == 0 || policy.max_bytes_per_queue == 0) {
    result.status = SupportErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_POST_COMMIT_POLICY_INVALID",
        "storage.ipar.post_commit.policy_invalid",
        "post-commit queues require non-zero item and byte bounds");
    return result;
  }

  for (const auto& item : items) {
    if (!IparStorageSupportAuthorityBoundarySafe(item.authority)) {
      result.status = SupportErrorStatus();
      result.fail_closed = true;
      result.refused_items.push_back(item);
      result.diagnostic = MakeIparStorageSupportDiagnostic(
          result.status, "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
          "storage.ipar.support.authority_drift", item.item_id);
      return result;
    }
    if (item.requires_commit_evidence && !item.commit_evidence_present) {
      result.refused_items.push_back(item);
      continue;
    }
    bool enqueued = false;
    switch (item.kind) {
      case IparPostCommitWorkKind::diagnostics:
        enqueued = EnqueuePostCommitItem(policy, item, &result.diagnostics_queue);
        break;
      case IparPostCommitWorkKind::maintenance:
        enqueued = EnqueuePostCommitItem(policy, item, &result.maintenance_queue);
        break;
      case IparPostCommitWorkKind::index_cleanup:
        enqueued = EnqueuePostCommitItem(policy, item, &result.index_queue);
        break;
      case IparPostCommitWorkKind::large_value_cleanup:
        enqueued = EnqueuePostCommitItem(policy, item, &result.large_value_queue);
        break;
    }
    if (!enqueued) {
      result.refused_items.push_back(item);
    }
  }

  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status,
      result.refused_items.empty() ? "SB_IPAR_POST_COMMIT_QUEUES_SEPARATED"
                                   : "SB_IPAR_POST_COMMIT_QUEUE_BOUNDED",
      result.refused_items.empty() ? "storage.ipar.post_commit.separated"
                                   : "storage.ipar.post_commit.bounded",
      "post-commit diagnostic and maintenance queues separated");
  AddEvidence(&result.evidence, "ipar.post_commit.diagnostics_queue",
              std::to_string(result.diagnostics_queue.size()));
  AddEvidence(&result.evidence, "ipar.post_commit.maintenance_queue",
              std::to_string(result.maintenance_queue.size()));
  AddEvidence(&result.evidence, "ipar.post_commit.index_queue",
              std::to_string(result.index_queue.size()));
  AddEvidence(&result.evidence, "ipar.post_commit.large_value_queue",
              std::to_string(result.large_value_queue.size()));
  AddEvidence(&result.evidence, "ipar.post_commit.finality_authority", "false");
  return result;
}

IparNumaCpuAffinityPlan PlanIparNumaCpuAffinity(
    const IparNumaCpuAffinityRequest& request) {
  IparNumaCpuAffinityPlan result;
  result.status = SupportOkStatus();
  if (request.numa_node_count == 0 || request.cpu_count == 0 ||
      request.worker_count == 0) {
    result.status = SupportErrorStatus();
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_AFFINITY_POLICY_INVALID",
        "storage.ipar.affinity.policy_invalid",
        "NUMA node, CPU, and worker counts are required");
    return result;
  }
  for (u64 worker = 0; worker < request.worker_count; ++worker) {
    const u64 cpu = (request.preferred_start_cpu + worker) % request.cpu_count;
    result.assignments.push_back(
        {worker + 1, cpu % request.numa_node_count, cpu});
  }
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, "SB_IPAR_AFFINITY_PLANNED",
      "storage.ipar.affinity.planned", "NUMA and CPU affinity planned");
  AddEvidence(&result.evidence, "ipar.affinity.worker_count",
              std::to_string(result.assignments.size()));
  return result;
}

IparWorkerPoolWarmupPlan PlanIparParallelWorkerPoolWarmup(
    const IparWorkerPoolWarmupRequest& request) {
  IparWorkerPoolWarmupPlan result;
  result.status = SupportOkStatus();
  if (!IparStorageSupportAuthorityBoundarySafe(request.authority)) {
    result.status = SupportErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
        "storage.ipar.support.authority_drift", "worker warmup");
    return result;
  }
  if (request.max_workers == 0 || request.min_workers > request.max_workers) {
    result.status = SupportErrorStatus();
    result.fail_closed = true;
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_WORKER_WARMUP_POLICY_INVALID",
        "storage.ipar.worker_warmup.policy_invalid",
        "worker warmup requires valid min and max bounds");
    return result;
  }
  const u64 wanted =
      std::min(request.max_workers,
               std::max(request.min_workers, request.target_workers));
  std::vector<IparSupportQueueItem> tickets;
  for (u64 worker = 1; worker <= wanted; ++worker) {
    IparSupportQueueItem item;
    item.item_id = "worker-warmup:" + std::to_string(worker);
    item.queue_kind = IparSupportQueueKind::worker_warmup;
    item.bytes = 1;
    item.priority = wanted - worker + 1;
    tickets.push_back(std::move(item));
  }
  auto queue = PlanIparBoundedSupportQueue(request.queue_policy, tickets);
  result.warmup_tickets = queue.accepted_items;
  result.warmed_workers = queue.accepted_count;
  result.refused_workers = queue.refused_count;
  auto affinity_request = request.affinity;
  affinity_request.worker_count = result.warmed_workers;
  result.affinity_plan = PlanIparNumaCpuAffinity(affinity_request);
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status,
      result.refused_workers == 0 ? "SB_IPAR_WORKER_WARMUP_COMPLETE"
                                  : "SB_IPAR_WORKER_WARMUP_BOUNDED",
      result.refused_workers == 0 ? "storage.ipar.worker_warmup.complete"
                                  : "storage.ipar.worker_warmup.bounded",
      "parallel worker pool warmup planned");
  AddEvidence(&result.evidence, "ipar.worker_warmup.warmed_workers",
              std::to_string(result.warmed_workers));
  AddEvidence(&result.evidence, "ipar.worker_warmup.refused_workers",
              std::to_string(result.refused_workers));
  AddEvidence(&result.evidence, "ipar.worker_warmup.finality_authority", "false");
  return result;
}

IparMaintenanceDebtPlan PlanIparMaintenanceDebt(
    const IparMaintenanceDebtPolicy& policy,
    const std::vector<IparMaintenanceDebtEntry>& entries) {
  IparMaintenanceDebtPlan result;
  result.status = SupportOkStatus();
  if (!policy.engine_mga_authoritative || policy.max_scheduled_items == 0 ||
      policy.max_scheduled_units == 0) {
    result.status = SupportErrorStatus();
    result.fail_closed = true;
    result.retained_count = entries.size();
    result.diagnostic = MakeIparStorageSupportDiagnostic(
        result.status, "SB_IPAR_MAINTENANCE_DEBT_POLICY_INVALID",
        "storage.ipar.maintenance_debt.policy_invalid",
        "MGA authority and non-zero scheduling bounds are required");
    return result;
  }

  std::vector<IparMaintenanceDebtAssignment> assignments;
  for (const auto& entry : entries) {
    if (!IparStorageSupportAuthorityBoundarySafe(entry.authority)) {
      result.status = SupportErrorStatus();
      result.fail_closed = true;
      result.diagnostic = MakeIparStorageSupportDiagnostic(
          result.status, "SB_IPAR_STORAGE_SUPPORT_AUTHORITY_DRIFT",
          "storage.ipar.support.authority_drift", entry.debt_id);
      return result;
    }
    IparMaintenanceDebtAssignment assignment;
    assignment.entry = entry;
    assignment.score = MaintenanceDebtScore(entry);
    if (!entry.source_authoritative ||
        (entry.destructive_cleanup && !entry.cleanup_horizon_authoritative)) {
      assignment.diagnostic_code = "SB_IPAR_MAINTENANCE_DEBT_RETAINED";
      assignments.push_back(std::move(assignment));
      continue;
    }
    assignment.diagnostic_code = "SB_IPAR_MAINTENANCE_DEBT_CANDIDATE";
    assignments.push_back(std::move(assignment));
  }

  std::stable_sort(assignments.begin(), assignments.end(),
                   [](const IparMaintenanceDebtAssignment& left,
                      const IparMaintenanceDebtAssignment& right) {
                     if (left.score != right.score) {
                       return left.score > right.score;
                     }
                     return left.entry.debt_id < right.entry.debt_id;
                   });

  for (auto& assignment : assignments) {
    const bool retained =
        assignment.diagnostic_code == "SB_IPAR_MAINTENANCE_DEBT_RETAINED";
    if (retained || result.scheduled_count >= policy.max_scheduled_items ||
        result.scheduled_units >= policy.max_scheduled_units) {
      assignment.scheduled = false;
      if (!retained) {
        assignment.diagnostic_code = "SB_IPAR_MAINTENANCE_DEBT_BOUNDED";
      }
      ++result.retained_count;
      result.assignments.push_back(assignment);
      continue;
    }
    const u64 remaining = policy.max_scheduled_units - result.scheduled_units;
    assignment.scheduled_units = std::min(assignment.entry.debt_units, remaining);
    assignment.scheduled = assignment.scheduled_units != 0;
    assignment.diagnostic_code = assignment.scheduled
                                     ? "SB_IPAR_MAINTENANCE_DEBT_SCHEDULED"
                                     : "SB_IPAR_MAINTENANCE_DEBT_BOUNDED";
    if (assignment.scheduled) {
      ++result.scheduled_count;
      result.scheduled_units += assignment.scheduled_units;
    } else {
      ++result.retained_count;
    }
    result.assignments.push_back(assignment);
  }

  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status, "SB_IPAR_MAINTENANCE_DEBT_PLANNED",
      "storage.ipar.maintenance_debt.planned",
      "maintenance debt ledger planned with authoritative cleanup gates");
  AddEvidence(&result.evidence, "ipar.maintenance_debt.scheduled_count",
              std::to_string(result.scheduled_count));
  AddEvidence(&result.evidence, "ipar.maintenance_debt.retained_count",
              std::to_string(result.retained_count));
  AddEvidence(&result.evidence, "ipar.maintenance_debt.finality_authority",
              "false");
  return result;
}

IparStorageMaintenanceSupportPlan PlanIparStorageMaintenanceSupport(
    const IparStorageMaintenanceSupportRequest& request) {
  IparStorageMaintenanceSupportPlan result;
  result.status = SupportOkStatus();
  result.queue_plan =
      PlanIparBoundedSupportQueue(request.queue_policy,
                                  request.maintenance_items);
  result.debt_plan = PlanIparMaintenanceDebt(request.debt_policy,
                                             request.debt_entries);
  result.warmup_plan = PlanIparParallelWorkerPoolWarmup(request.warmup_request);
  result.fail_closed = !result.queue_plan.ok() || !result.debt_plan.ok() ||
                       !result.warmup_plan.ok();
  result.status = result.fail_closed ? SupportErrorStatus() : SupportOkStatus();
  result.diagnostic = MakeIparStorageSupportDiagnostic(
      result.status,
      result.fail_closed ? "SB_IPAR_STORAGE_MAINTENANCE_SUPPORT_BLOCKED"
                         : "SB_IPAR_STORAGE_MAINTENANCE_SUPPORT_PLANNED",
      result.fail_closed ? "storage.ipar.maintenance_support.blocked"
                         : "storage.ipar.maintenance_support.planned",
      "storage maintenance support infrastructure planned");
  AddEvidence(&result.evidence, "ipar.storage_maintenance.queue_ok",
              BoolText(result.queue_plan.ok()));
  AddEvidence(&result.evidence, "ipar.storage_maintenance.debt_ok",
              BoolText(result.debt_plan.ok()));
  AddEvidence(&result.evidence, "ipar.storage_maintenance.warmup_ok",
              BoolText(result.warmup_plan.ok()));
  AddEvidence(&result.evidence, "ipar.storage_maintenance.finality_authority",
              "false");
  return result;
}

}  // namespace scratchbird::storage::page
