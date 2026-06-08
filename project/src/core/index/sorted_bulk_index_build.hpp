// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "index_family_registry.hpp"
#include "index_ordered_access.hpp"
#include "index_btree_page.hpp"
#include "unique_index_reservation_ledger.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::index {

struct SortedBulkIndexMetadata {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  IndexFamily family = IndexFamily::btree;
  std::string family_name;
  std::string semantic_profile;
  bool unique = false;
  bool unique_nulls_distinct = true;
  bool rebuild = false;
  bool input_presorted = false;
  bool order_proof_valid = false;
  bool policy_allows_mutation = true;
  bool read_only_database = false;
  bool online = true;
  u64 page_budget = 0;
  u64 byte_budget = 0;
  u64 time_budget_microseconds = 0;
  u32 physical_page_size = 4096;
  u32 leaf_entry_capacity = 128;
  u32 internal_entry_capacity = 128;
};

struct SortedBulkIndexRowInput {
  std::string encoded_key;
  std::string row_uuid;
  std::string version_uuid;
  std::string payload_value;
  u64 source_ordinal = 0;
  bool null_key = false;
};

struct SortedBulkIndexEntry {
  std::string encoded_key;
  std::string row_uuid;
  std::string version_uuid;
  std::string payload_value;
  u64 source_ordinal = 0;
  u64 sorted_ordinal = 0;
  u64 key_run_ordinal = 0;
  u64 leaf_ordinal = 0;
  bool null_key = false;
};

struct SortedBulkIndexBuildEvidence {
  std::string evidence_kind;
  std::string evidence_id;
};

struct SortedBulkIndexCandidateRootGeneration {
  bool created = false;
  bool physical_leaf_pack = false;
  bool branch_levels_built = false;
  bool fence_keys_stored = false;
  bool validated_tree = false;
  bool candidate_root_page_present = false;
  bool root_publish_authorized = false;
  bool physical_append_authorized = false;
  u64 candidate_generation = 0;
  u64 root_page_number = 0;
  u64 page_count = 0;
  u64 leaf_page_count = 0;
  u64 branch_level_count = 0;
  u64 live_entry_count = 0;
  scratchbird::storage::page::IndexBtreePhysicalTree tree;
  scratchbird::storage::page::IndexBtreePhysicalTreeReport report;
};

struct SortedBulkIndexBuildRequest {
  SortedBulkIndexMetadata metadata;
  std::vector<SortedBulkIndexRowInput> rows;
  std::vector<SortedBulkIndexRowInput> visible_unique_keys;
  TypedUuid unique_constraint_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  UniqueIndexReservationLedger* unique_reservation_ledger = nullptr;
  bool validate_unique_reservation_batch = false;
  std::string unique_reservation_validation_evidence_token;
  std::vector<UniqueIndexReservationTransactionProof>
      unique_transaction_state_proofs;
};

struct SortedBulkIndexBuildResult {
  Status status;
  bool accepted = false;
  bool bottom_up_build_selected = false;
  bool uniqueness_proven = false;
  bool uniqueness_refused = false;
  bool unique_visible_conflict_refused = false;
  bool unsafe_key_refused = false;
  bool invalid_descriptor_refused = false;
  bool missing_mga_proof_refused = false;
  bool unique_reservation_ledger_used = false;
  bool unique_reservation_validation_passed = false;
  bool physical_append_authorized_by_proof = false;
  bool root_publish_authorized_by_proof = false;
  u64 sorted_key_run_count = 0;
  u64 unique_effective_key_count = 0;
  u64 unique_visible_key_count = 0;
  u64 unique_duplicate_run_count = 0;
  u64 leaf_generation_count = 0;
  u64 root_generation_count = 0;
  std::string root_publish_fence;
  OrderedBuildPlan ordered_plan;
  SortedBulkIndexCandidateRootGeneration candidate_root_generation;
  std::vector<SortedBulkIndexEntry> entries;
  std::vector<SortedBulkIndexBuildEvidence> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && accepted; }
};

SortedBulkIndexBuildResult BuildSortedExactBulkIndex(
    const SortedBulkIndexBuildRequest& request);

}  // namespace scratchbird::core::index
