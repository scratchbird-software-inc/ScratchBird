// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "visibility_status_cache.hpp"

#include <utility>

namespace scratchbird::transaction::mga {
namespace {

bool IsInventoryCommittedEvidence(const TransactionInventoryEntry& entry) {
  return (entry.state == TransactionState::committed ||
          entry.state == TransactionState::archived) &&
         entry.evidence_record_written;
}

bool IsEngineMgaProvenance(PageFinalityProvenance provenance) {
  return provenance == PageFinalityProvenance::engine_mga_transaction_inventory ||
         provenance == PageFinalityProvenance::engine_mga_cleanup_horizon;
}

void AddEvidence(VisibilityStatusCacheDecision* decision,
                 std::string name,
                 std::string value) {
  decision->evidence.push_back({std::move(name), std::move(value)});
}

void MergeCounters(VisibilityStatusCacheCounters* target,
                   const VisibilityStatusCacheCounters& source) {
  target->probes += source.probes;
  target->accepted += source.accepted;
  target->refused += source.refused;
  target->stale_refusals += source.stale_refusals;
  target->epoch_refusals += source.epoch_refusals;
  target->horizon_refusals += source.horizon_refusals;
  target->authority_refusals += source.authority_refusals;
}

VisibilityStatusCacheDecision StartDecision(VisibilityStatusCacheProbe probe) {
  VisibilityStatusCacheDecision decision;
  decision.probe = probe;
  decision.counters.probes = 1;
  AddEvidence(&decision, "probe", VisibilityStatusCacheProbeName(probe));
  AddEvidence(&decision, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(&decision, "cache_transaction_finality_authority", "false");
  return decision;
}

VisibilityStatusCacheDecision Refuse(VisibilityStatusCacheDecision decision,
                                     std::string reason) {
  decision.accepted = false;
  decision.all_committed = false;
  decision.all_visible = false;
  decision.no_older_reader = false;
  decision.authoritative_path_required = true;
  decision.evidence_name = "mga_visibility_status_cache.refused";
  decision.refusal_reason = std::move(reason);
  ++decision.counters.refused;
  return decision;
}

VisibilityStatusCacheDecision Accept(VisibilityStatusCacheDecision decision,
                                     std::string evidence_name) {
  decision.accepted = true;
  decision.authoritative_path_required = false;
  decision.evidence_name = std::move(evidence_name);
  decision.refusal_reason = "none";
  ++decision.counters.accepted;
  return decision;
}

bool CommonFactsPresent(const VisibilityStatusCacheFacts& facts) {
  return facts.cache_generation != 0 &&
         facts.invalidation_generation != 0 &&
         facts.horizon_epoch != 0 &&
         facts.snapshot_epoch != 0 &&
         facts.reader_visible_through_local_transaction_id.valid() &&
         facts.oldest_active_local_transaction_id.valid() &&
         facts.oldest_snapshot_local_transaction_id.valid();
}

bool RelationFactsPresent(const VisibilityStatusCacheFacts& facts) {
  return facts.relation_epoch != 0 && facts.catalog_epoch != 0;
}

bool CommonAuthorityPresent(const VisibilityStatusCacheFacts& facts) {
  return facts.transaction_inventory_authoritative &&
         facts.transaction_horizon_authoritative &&
         facts.normal_mga_visibility_authority_available;
}

VisibilityStatusCacheDecision ValidateCommonFacts(VisibilityStatusCacheDecision decision,
                                                  const VisibilityStatusCacheFacts& facts) {
  if (!CommonFactsPresent(facts)) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "cache_generation_epoch_or_horizon_missing");
  }
  if (!CommonAuthorityPresent(facts)) {
    ++decision.counters.authority_refusals;
    return Refuse(std::move(decision), "mga_authority_inputs_uncertain");
  }
  return decision;
}

VisibilityStatusCacheDecision ValidateCacheContainerFreshness(
    VisibilityStatusCacheDecision decision,
    const MgaVisibilityStatusCache& cache,
    const VisibilityStatusCacheFacts& facts) {
  AddEvidence(&decision, "cache_container_generation",
              std::to_string(cache.cache_generation));
  AddEvidence(&decision, "cache_container_invalidation_generation",
              std::to_string(cache.invalidation_generation));
  if (cache.cache_generation == 0 || cache.invalidation_generation == 0 ||
      cache.cache_generation != facts.cache_generation ||
      cache.invalidation_generation != facts.invalidation_generation) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision),
                  "visibility_status_cache_generation_mismatch");
  }
  return decision;
}

bool IsRefused(const VisibilityStatusCacheDecision& decision) {
  return !decision.refusal_reason.empty() && decision.refusal_reason != "not_evaluated";
}

template <typename Entry>
VisibilityStatusCacheDecision ValidateEntryFreshness(VisibilityStatusCacheDecision decision,
                                                     const Entry& entry,
                                                     const VisibilityStatusCacheFacts& facts) {
  if (entry.status == VisibilityStatusCacheEntryStatus::missing) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "visibility_status_cache_missing");
  }
  if (entry.status == VisibilityStatusCacheEntryStatus::stale ||
      entry.status == VisibilityStatusCacheEntryStatus::uncertain ||
      entry.status == VisibilityStatusCacheEntryStatus::incompatible) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "visibility_status_cache_not_current");
  }
  if (!IsEngineMgaProvenance(entry.provenance)) {
    ++decision.counters.authority_refusals;
    return Refuse(std::move(decision), "visibility_status_cache_external_provenance_refused");
  }
  if (entry.cache_generation != facts.cache_generation ||
      entry.invalidation_generation != facts.invalidation_generation) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "visibility_status_cache_generation_mismatch");
  }
  if (entry.horizon_epoch != facts.horizon_epoch ||
      entry.snapshot_epoch != facts.snapshot_epoch) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "visibility_status_cache_epoch_mismatch");
  }
  return decision;
}

bool ReaderAndActiveHorizonsCover(const VisibilityStatusCacheFacts& facts,
                                  LocalTransactionId through) {
  return through.valid() &&
         facts.reader_visible_through_local_transaction_id.value >= through.value &&
         facts.oldest_active_local_transaction_id.value > through.value;
}

}  // namespace

const char* VisibilityStatusCacheEntryStatusName(VisibilityStatusCacheEntryStatus status) {
  switch (status) {
    case VisibilityStatusCacheEntryStatus::current: return "current";
    case VisibilityStatusCacheEntryStatus::missing: return "missing";
    case VisibilityStatusCacheEntryStatus::stale: return "stale";
    case VisibilityStatusCacheEntryStatus::uncertain: return "uncertain";
    case VisibilityStatusCacheEntryStatus::incompatible: return "incompatible";
  }
  return "unknown";
}

const char* VisibilityStatusCacheProbeName(VisibilityStatusCacheProbe probe) {
  switch (probe) {
    case VisibilityStatusCacheProbe::txid_range_all_committed:
      return "txid_range_all_committed";
    case VisibilityStatusCacheProbe::page_all_visible: return "page_all_visible";
    case VisibilityStatusCacheProbe::page_all_committed: return "page_all_committed";
    case VisibilityStatusCacheProbe::relation_no_older_reader:
      return "relation_no_older_reader";
    case VisibilityStatusCacheProbe::unknown: return "unknown";
  }
  return "unknown";
}

MgaVisibilityStatusCache MakeMgaVisibilityStatusCache(u64 cache_generation,
                                                      u64 invalidation_generation) {
  MgaVisibilityStatusCache cache;
  cache.cache_generation = cache_generation;
  cache.invalidation_generation = invalidation_generation;
  return cache;
}

VisibilityStatusCacheDecision CacheTransactionStatusRangeFromInventory(
    MgaVisibilityStatusCache* cache,
    const LocalTransactionInventory& inventory,
    const TransactionStatusRangeCacheRequest& request) {
  auto decision = ValidateCommonFacts(
      StartDecision(VisibilityStatusCacheProbe::txid_range_all_committed),
      request.facts);
  if (IsRefused(decision)) { return decision; }
  if (cache == nullptr ||
      !request.first_local_transaction_id.valid() ||
      !request.last_local_transaction_id.valid() ||
      request.first_local_transaction_id.value > request.last_local_transaction_id.value) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "txid_range_invalid");
  }
  decision = ValidateCacheContainerFreshness(
      std::move(decision), *cache, request.facts);
  if (IsRefused(decision)) {
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }
  if (!ReaderAndActiveHorizonsCover(request.facts, request.last_local_transaction_id)) {
    ++decision.counters.horizon_refusals;
    return Refuse(std::move(decision), "txid_range_horizon_not_closed");
  }

  for (u64 id = request.first_local_transaction_id.value;
       id <= request.last_local_transaction_id.value;
       ++id) {
    const auto lookup = LookupLocalTransaction(inventory, MakeLocalTransactionId(id));
    if (!lookup.ok() || !IsInventoryCommittedEvidence(lookup.entry)) {
      ++decision.counters.authority_refusals;
      return Refuse(std::move(decision), "txid_range_not_all_committed_by_inventory");
    }
    if (id == request.last_local_transaction_id.value) { break; }
  }

  TransactionStatusRangeCacheEntry entry;
  entry.status = VisibilityStatusCacheEntryStatus::current;
  entry.provenance = PageFinalityProvenance::engine_mga_transaction_inventory;
  entry.first_local_transaction_id = request.first_local_transaction_id;
  entry.last_local_transaction_id = request.last_local_transaction_id;
  entry.cache_generation = request.facts.cache_generation;
  entry.invalidation_generation = request.facts.invalidation_generation;
  entry.horizon_epoch = request.facts.horizon_epoch;
  entry.snapshot_epoch = request.facts.snapshot_epoch;
  entry.all_committed = true;
  cache->transaction_status_ranges.push_back(entry);

  decision.all_committed = true;
  AddEvidence(&decision, "first_local_transaction_id",
              std::to_string(request.first_local_transaction_id.value));
  AddEvidence(&decision, "last_local_transaction_id",
              std::to_string(request.last_local_transaction_id.value));
  decision = Accept(std::move(decision),
                    "mga_visibility_status_cache.txid_range_all_committed.cached");
  MergeCounters(&cache->counters, decision.counters);
  return decision;
}

VisibilityStatusCacheDecision EvaluateCachedTransactionStatusRange(
    MgaVisibilityStatusCache* cache,
    const TransactionStatusRangeCacheRequest& request) {
  auto decision = ValidateCommonFacts(
      StartDecision(VisibilityStatusCacheProbe::txid_range_all_committed),
      request.facts);
  if (IsRefused(decision)) { return decision; }
  if (cache == nullptr ||
      !request.first_local_transaction_id.valid() ||
      !request.last_local_transaction_id.valid()) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "visibility_status_cache_missing");
  }
  decision = ValidateCacheContainerFreshness(
      std::move(decision), *cache, request.facts);
  if (IsRefused(decision)) {
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }

  for (const auto& entry : cache->transaction_status_ranges) {
    if (!entry.first_local_transaction_id.valid() ||
        !entry.last_local_transaction_id.valid() ||
        entry.first_local_transaction_id.value > request.first_local_transaction_id.value ||
        entry.last_local_transaction_id.value < request.last_local_transaction_id.value) {
      continue;
    }
    decision = ValidateEntryFreshness(std::move(decision), entry, request.facts);
    if (IsRefused(decision)) {
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    if (!entry.all_committed ||
        !ReaderAndActiveHorizonsCover(request.facts, request.last_local_transaction_id)) {
      ++decision.counters.horizon_refusals;
      decision = Refuse(std::move(decision), "txid_range_horizon_not_closed");
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    decision.all_committed = true;
    AddEvidence(&decision, "cache_generation", std::to_string(entry.cache_generation));
    decision = Accept(std::move(decision),
                      "mga_visibility_status_cache.txid_range_all_committed.accepted");
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }

  ++decision.counters.stale_refusals;
  decision = Refuse(std::move(decision), "visibility_status_cache_missing");
  MergeCounters(&cache->counters, decision.counters);
  return decision;
}

VisibilityStatusCacheDecision CachePageVisibilityStatus(
    MgaVisibilityStatusCache* cache,
    const PageVisibilityStatusCacheRequest& request,
    const PageFinalityEvidenceDecision& page_finality,
    bool all_committed) {
  auto decision = ValidateCommonFacts(
      StartDecision(VisibilityStatusCacheProbe::page_all_visible),
      request.facts);
  if (IsRefused(decision)) { return decision; }
  if (cache == nullptr || request.relation_uuid.empty() ||
      request.page_generation == 0 || request.extent_epoch == 0 ||
      !RelationFactsPresent(request.facts)) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "page_visibility_cache_identity_or_epoch_missing");
  }
  decision = ValidateCacheContainerFreshness(
      std::move(decision), *cache, request.facts);
  if (IsRefused(decision)) {
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }
  if (!page_finality.accepted || !page_finality.all_visible ||
      page_finality.map_is_transaction_finality_authority ||
      !page_finality.durable_mga_inventory_remains_authority) {
    ++decision.counters.authority_refusals;
    return Refuse(std::move(decision), "page_finality_evidence_not_acceptable_for_cache");
  }
  if (!ReaderAndActiveHorizonsCover(request.facts,
                                    request.final_through_local_transaction_id)) {
    ++decision.counters.horizon_refusals;
    return Refuse(std::move(decision), "page_visibility_horizon_not_closed");
  }

  PageVisibilityStatusCacheEntry entry;
  entry.status = VisibilityStatusCacheEntryStatus::current;
  entry.provenance = PageFinalityProvenance::engine_mga_transaction_inventory;
  entry.relation_uuid = request.relation_uuid;
  entry.page_number = request.page_number;
  entry.page_generation = request.page_generation;
  entry.extent_id = request.extent_id;
  entry.extent_epoch = request.extent_epoch;
  entry.relation_epoch = request.facts.relation_epoch;
  entry.catalog_epoch = request.facts.catalog_epoch;
  entry.final_through_local_transaction_id =
      request.final_through_local_transaction_id;
  entry.cache_generation = request.facts.cache_generation;
  entry.invalidation_generation = request.facts.invalidation_generation;
  entry.horizon_epoch = request.facts.horizon_epoch;
  entry.snapshot_epoch = request.facts.snapshot_epoch;
  entry.all_visible = true;
  entry.all_committed = all_committed;
  cache->page_visibility_entries.push_back(entry);

  decision.all_visible = true;
  decision.all_committed = all_committed;
  decision = Accept(std::move(decision),
                    "mga_visibility_status_cache.page_visibility.cached");
  MergeCounters(&cache->counters, decision.counters);
  return decision;
}

VisibilityStatusCacheDecision EvaluateCachedPageVisibilityStatus(
    MgaVisibilityStatusCache* cache,
    const PageVisibilityStatusCacheRequest& request,
    VisibilityStatusCacheProbe probe) {
  auto decision = ValidateCommonFacts(StartDecision(probe), request.facts);
  if (IsRefused(decision)) { return decision; }
  if (probe != VisibilityStatusCacheProbe::page_all_visible &&
      probe != VisibilityStatusCacheProbe::page_all_committed) {
    return Refuse(std::move(decision), "page_visibility_probe_incompatible");
  }
  if (cache == nullptr || request.relation_uuid.empty()) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "visibility_status_cache_missing");
  }
  decision = ValidateCacheContainerFreshness(
      std::move(decision), *cache, request.facts);
  if (IsRefused(decision)) {
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }

  for (const auto& entry : cache->page_visibility_entries) {
    if (entry.relation_uuid != request.relation_uuid ||
        entry.page_number != request.page_number ||
        entry.page_generation != request.page_generation ||
        entry.extent_id != request.extent_id) {
      continue;
    }
    decision = ValidateEntryFreshness(std::move(decision), entry, request.facts);
    if (IsRefused(decision)) {
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    if (entry.extent_epoch != request.extent_epoch ||
        entry.relation_epoch != request.facts.relation_epoch ||
        entry.catalog_epoch != request.facts.catalog_epoch) {
      ++decision.counters.epoch_refusals;
      decision = Refuse(std::move(decision), "page_visibility_epoch_mismatch");
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    if (!ReaderAndActiveHorizonsCover(request.facts,
                                      entry.final_through_local_transaction_id)) {
      ++decision.counters.horizon_refusals;
      decision = Refuse(std::move(decision), "page_visibility_horizon_not_closed");
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    if (probe == VisibilityStatusCacheProbe::page_all_visible && !entry.all_visible) {
      decision = Refuse(std::move(decision), "page_all_visible_cache_evidence_missing");
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    if (probe == VisibilityStatusCacheProbe::page_all_committed && !entry.all_committed) {
      decision = Refuse(std::move(decision), "page_all_committed_cache_evidence_missing");
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    decision.all_visible = entry.all_visible;
    decision.all_committed = entry.all_committed;
    decision = Accept(std::move(decision),
                      probe == VisibilityStatusCacheProbe::page_all_visible
                          ? "mga_visibility_status_cache.page_all_visible.accepted"
                          : "mga_visibility_status_cache.page_all_committed.accepted");
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }

  ++decision.counters.stale_refusals;
  decision = Refuse(std::move(decision), "visibility_status_cache_missing");
  MergeCounters(&cache->counters, decision.counters);
  return decision;
}

VisibilityStatusCacheDecision CacheRelationNoOlderReaderStatus(
    MgaVisibilityStatusCache* cache,
    const RelationNoOlderReaderCacheRequest& request) {
  auto decision = ValidateCommonFacts(
      StartDecision(VisibilityStatusCacheProbe::relation_no_older_reader),
      request.facts);
  if (IsRefused(decision)) { return decision; }
  if (cache == nullptr || request.relation_uuid.empty() ||
      !request.no_reader_older_than_local_transaction_id.valid() ||
      !RelationFactsPresent(request.facts)) {
    ++decision.counters.epoch_refusals;
    return Refuse(std::move(decision), "relation_reader_cache_identity_or_epoch_missing");
  }
  decision = ValidateCacheContainerFreshness(
      std::move(decision), *cache, request.facts);
  if (IsRefused(decision)) {
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }
  if (request.facts.oldest_snapshot_local_transaction_id.value <=
      request.no_reader_older_than_local_transaction_id.value) {
    ++decision.counters.horizon_refusals;
    return Refuse(std::move(decision), "relation_reader_horizon_not_closed");
  }

  RelationNoOlderReaderCacheEntry entry;
  entry.status = VisibilityStatusCacheEntryStatus::current;
  entry.provenance = PageFinalityProvenance::engine_mga_transaction_inventory;
  entry.relation_uuid = request.relation_uuid;
  entry.relation_epoch = request.facts.relation_epoch;
  entry.catalog_epoch = request.facts.catalog_epoch;
  entry.no_reader_older_than_local_transaction_id =
      request.no_reader_older_than_local_transaction_id;
  entry.cache_generation = request.facts.cache_generation;
  entry.invalidation_generation = request.facts.invalidation_generation;
  entry.horizon_epoch = request.facts.horizon_epoch;
  entry.snapshot_epoch = request.facts.snapshot_epoch;
  cache->relation_reader_entries.push_back(entry);

  decision.no_older_reader = true;
  decision = Accept(std::move(decision),
                    "mga_visibility_status_cache.relation_no_older_reader.cached");
  MergeCounters(&cache->counters, decision.counters);
  return decision;
}

VisibilityStatusCacheDecision EvaluateCachedRelationNoOlderReaderStatus(
    MgaVisibilityStatusCache* cache,
    const RelationNoOlderReaderCacheRequest& request) {
  auto decision = ValidateCommonFacts(
      StartDecision(VisibilityStatusCacheProbe::relation_no_older_reader),
      request.facts);
  if (IsRefused(decision)) { return decision; }
  if (cache == nullptr || request.relation_uuid.empty() ||
      !request.no_reader_older_than_local_transaction_id.valid()) {
    ++decision.counters.stale_refusals;
    return Refuse(std::move(decision), "visibility_status_cache_missing");
  }
  decision = ValidateCacheContainerFreshness(
      std::move(decision), *cache, request.facts);
  if (IsRefused(decision)) {
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }

  for (const auto& entry : cache->relation_reader_entries) {
    if (entry.relation_uuid != request.relation_uuid ||
        entry.no_reader_older_than_local_transaction_id.value <
            request.no_reader_older_than_local_transaction_id.value) {
      continue;
    }
    decision = ValidateEntryFreshness(std::move(decision), entry, request.facts);
    if (IsRefused(decision)) {
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    if (entry.relation_epoch != request.facts.relation_epoch ||
        entry.catalog_epoch != request.facts.catalog_epoch) {
      ++decision.counters.epoch_refusals;
      decision = Refuse(std::move(decision), "relation_reader_epoch_mismatch");
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    if (request.facts.oldest_snapshot_local_transaction_id.value <=
        request.no_reader_older_than_local_transaction_id.value) {
      ++decision.counters.horizon_refusals;
      decision = Refuse(std::move(decision), "relation_reader_horizon_not_closed");
      MergeCounters(&cache->counters, decision.counters);
      return decision;
    }
    decision.no_older_reader = true;
    decision = Accept(std::move(decision),
                      "mga_visibility_status_cache.relation_no_older_reader.accepted");
    MergeCounters(&cache->counters, decision.counters);
    return decision;
  }

  ++decision.counters.stale_refusals;
  decision = Refuse(std::move(decision), "visibility_status_cache_missing");
  MergeCounters(&cache->counters, decision.counters);
  return decision;
}

}  // namespace scratchbird::transaction::mga
