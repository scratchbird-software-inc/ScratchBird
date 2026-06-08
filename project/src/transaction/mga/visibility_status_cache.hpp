// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "page_finality_evidence.hpp"
#include "transaction_inventory.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class VisibilityStatusCacheEntryStatus : u16 {
  current,
  missing,
  stale,
  uncertain,
  incompatible
};

enum class VisibilityStatusCacheProbe : u16 {
  txid_range_all_committed,
  page_all_visible,
  page_all_committed,
  relation_no_older_reader,
  unknown
};

struct VisibilityStatusCacheFacts {
  u64 cache_generation = 0;
  u64 invalidation_generation = 0;
  u64 horizon_epoch = 0;
  u64 snapshot_epoch = 0;
  u64 relation_epoch = 0;
  u64 catalog_epoch = 0;
  LocalTransactionId reader_visible_through_local_transaction_id;
  LocalTransactionId oldest_active_local_transaction_id;
  LocalTransactionId oldest_snapshot_local_transaction_id;
  bool transaction_inventory_authoritative = false;
  bool transaction_horizon_authoritative = false;
  bool normal_mga_visibility_authority_available = false;
};

struct VisibilityStatusCacheEvidenceField {
  std::string name;
  std::string value;
};

struct VisibilityStatusCacheCounters {
  u64 probes = 0;
  u64 accepted = 0;
  u64 refused = 0;
  u64 stale_refusals = 0;
  u64 epoch_refusals = 0;
  u64 horizon_refusals = 0;
  u64 authority_refusals = 0;
};

struct VisibilityStatusCacheDecision {
  bool accepted = false;
  bool all_committed = false;
  bool all_visible = false;
  bool no_older_reader = false;
  bool cache_is_transaction_finality_authority = false;
  bool durable_mga_inventory_remains_authority = true;
  bool authoritative_path_required = true;
  VisibilityStatusCacheProbe probe = VisibilityStatusCacheProbe::unknown;
  std::string evidence_name = "mga_visibility_status_cache.refused";
  std::string refusal_reason = "not_evaluated";
  VisibilityStatusCacheCounters counters;
  std::vector<VisibilityStatusCacheEvidenceField> evidence;
};

struct TransactionStatusRangeCacheRequest {
  LocalTransactionId first_local_transaction_id;
  LocalTransactionId last_local_transaction_id;
  VisibilityStatusCacheFacts facts;
};

struct TransactionStatusRangeCacheEntry {
  VisibilityStatusCacheEntryStatus status = VisibilityStatusCacheEntryStatus::missing;
  PageFinalityProvenance provenance = PageFinalityProvenance::unknown;
  LocalTransactionId first_local_transaction_id;
  LocalTransactionId last_local_transaction_id;
  u64 cache_generation = 0;
  u64 invalidation_generation = 0;
  u64 horizon_epoch = 0;
  u64 snapshot_epoch = 0;
  bool all_committed = false;
};

struct PageVisibilityStatusCacheRequest {
  std::string relation_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 extent_id = 0;
  u64 extent_epoch = 0;
  LocalTransactionId final_through_local_transaction_id;
  VisibilityStatusCacheFacts facts;
};

struct PageVisibilityStatusCacheEntry {
  VisibilityStatusCacheEntryStatus status = VisibilityStatusCacheEntryStatus::missing;
  PageFinalityProvenance provenance = PageFinalityProvenance::unknown;
  std::string relation_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 extent_id = 0;
  u64 extent_epoch = 0;
  u64 relation_epoch = 0;
  u64 catalog_epoch = 0;
  LocalTransactionId final_through_local_transaction_id;
  u64 cache_generation = 0;
  u64 invalidation_generation = 0;
  u64 horizon_epoch = 0;
  u64 snapshot_epoch = 0;
  bool all_visible = false;
  bool all_committed = false;
};

struct RelationNoOlderReaderCacheRequest {
  std::string relation_uuid;
  LocalTransactionId no_reader_older_than_local_transaction_id;
  VisibilityStatusCacheFacts facts;
};

struct RelationNoOlderReaderCacheEntry {
  VisibilityStatusCacheEntryStatus status = VisibilityStatusCacheEntryStatus::missing;
  PageFinalityProvenance provenance = PageFinalityProvenance::unknown;
  std::string relation_uuid;
  u64 relation_epoch = 0;
  u64 catalog_epoch = 0;
  LocalTransactionId no_reader_older_than_local_transaction_id;
  u64 cache_generation = 0;
  u64 invalidation_generation = 0;
  u64 horizon_epoch = 0;
  u64 snapshot_epoch = 0;
};

struct MgaVisibilityStatusCache {
  u64 cache_generation = 1;
  u64 invalidation_generation = 1;
  std::vector<TransactionStatusRangeCacheEntry> transaction_status_ranges;
  std::vector<PageVisibilityStatusCacheEntry> page_visibility_entries;
  std::vector<RelationNoOlderReaderCacheEntry> relation_reader_entries;
  VisibilityStatusCacheCounters counters;
};

const char* VisibilityStatusCacheEntryStatusName(VisibilityStatusCacheEntryStatus status);
const char* VisibilityStatusCacheProbeName(VisibilityStatusCacheProbe probe);

MgaVisibilityStatusCache MakeMgaVisibilityStatusCache(u64 cache_generation,
                                                      u64 invalidation_generation);

VisibilityStatusCacheDecision CacheTransactionStatusRangeFromInventory(
    MgaVisibilityStatusCache* cache,
    const LocalTransactionInventory& inventory,
    const TransactionStatusRangeCacheRequest& request);

VisibilityStatusCacheDecision EvaluateCachedTransactionStatusRange(
    MgaVisibilityStatusCache* cache,
    const TransactionStatusRangeCacheRequest& request);

VisibilityStatusCacheDecision CachePageVisibilityStatus(
    MgaVisibilityStatusCache* cache,
    const PageVisibilityStatusCacheRequest& request,
    const PageFinalityEvidenceDecision& page_finality,
    bool all_committed);

VisibilityStatusCacheDecision EvaluateCachedPageVisibilityStatus(
    MgaVisibilityStatusCache* cache,
    const PageVisibilityStatusCacheRequest& request,
    VisibilityStatusCacheProbe probe);

VisibilityStatusCacheDecision CacheRelationNoOlderReaderStatus(
    MgaVisibilityStatusCache* cache,
    const RelationNoOlderReaderCacheRequest& request);

VisibilityStatusCacheDecision EvaluateCachedRelationNoOlderReaderStatus(
    MgaVisibilityStatusCache* cache,
    const RelationNoOlderReaderCacheRequest& request);

}  // namespace scratchbird::transaction::mga
