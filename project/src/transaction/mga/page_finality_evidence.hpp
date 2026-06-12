// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "transaction_horizon.hpp"

#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class PageFinalityScope : u16 {
  page,
  extent,
  unknown
};

enum class PageFinalityConsumer : u16 {
  index_only_scan,
  dml_recheck,
  cleanup,
  summary_pruning,
  unknown
};

enum class PageFinalityMapStatus : u16 {
  current,
  missing,
  stale,
  uncertain,
  incompatible,
  corrupt
};

enum class PageFinalityProvenance : u16 {
  engine_mga_transaction_inventory,
  engine_mga_cleanup_horizon,
  parser_claim,
  reference_claim,
  timestamp_claim,
  uuid_order_claim,
  external_log_claim,
  unknown
};

struct PageFinalityMapEntry {
  PageFinalityScope scope = PageFinalityScope::unknown;
  PageFinalityMapStatus status = PageFinalityMapStatus::missing;
  PageFinalityProvenance provenance = PageFinalityProvenance::unknown;
  std::string relation_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 extent_id = 0;
  u64 extent_epoch = 0;
  u64 relation_epoch = 0;
  u64 catalog_epoch = 0;
  LocalTransactionId final_through_local_transaction_id;
  u64 map_generation = 0;
  bool persisted_record_present = false;
  bool checksum_valid = false;
  bool all_visible = false;
  bool all_final = false;
};

struct PageFinalityObservedFacts {
  PageFinalityScope requested_scope = PageFinalityScope::unknown;
  std::string relation_uuid;
  u64 page_number = 0;
  u64 page_generation = 0;
  u64 extent_id = 0;
  u64 extent_epoch = 0;
  u64 relation_epoch = 0;
  u64 catalog_epoch = 0;
  LocalTransactionId reader_visible_through_local_transaction_id;
  LocalTransactionId oldest_active_local_transaction_id;
  bool transaction_horizon_authoritative = false;
  bool transaction_inventory_authoritative = false;
  bool normal_mga_visibility_authority_available = false;
};

struct PageFinalityEvidenceField {
  std::string name;
  std::string value;
};

struct PageFinalityDecisionCounters {
  u64 evidence_examined = 0;
  u64 accepted = 0;
  u64 refused = 0;
  u64 stale_refusals = 0;
  u64 epoch_refusals = 0;
  u64 horizon_refusals = 0;
  u64 provenance_refusals = 0;
};

struct PageFinalityEvidenceDecision {
  bool accepted = false;
  bool all_visible = false;
  bool all_final = false;
  bool map_is_transaction_finality_authority = false;
  bool durable_mga_inventory_remains_authority = true;
  bool normal_mga_recheck_required = true;
  std::string evidence_name = "mga_page_finality.refused";
  std::string refusal_reason = "not_evaluated";
  std::vector<PageFinalityEvidenceField> evidence;
  PageFinalityDecisionCounters counters;
};

struct ExactIndexCleanupAuthorityDecision {
  bool accepted = false;
  bool cleanup_horizon_authoritative = false;
  bool transaction_inventory_authoritative = false;
  bool page_finality_authoritative = false;
  u64 cleanup_horizon_local_transaction_id = 0;
  std::string authority_source = "none";
  std::string refusal_reason = "not_evaluated";
  std::vector<PageFinalityEvidenceField> evidence;
  PageFinalityDecisionCounters counters;
};

const char* PageFinalityScopeName(PageFinalityScope scope);
const char* PageFinalityConsumerName(PageFinalityConsumer consumer);
const char* PageFinalityMapStatusName(PageFinalityMapStatus status);
const char* PageFinalityProvenanceName(PageFinalityProvenance provenance);

PageFinalityEvidenceDecision EvaluatePageFinalityEvidence(
    const PageFinalityMapEntry& entry,
    const PageFinalityObservedFacts& observed,
    PageFinalityConsumer consumer);

ExactIndexCleanupAuthorityDecision EvaluateExactIndexCleanupAuthority(
    const PageFinalityEvidenceDecision& page_finality,
    LocalTransactionId cleanup_horizon,
    bool cleanup_horizon_authoritative,
    bool transaction_inventory_authoritative);

}  // namespace scratchbird::transaction::mga
