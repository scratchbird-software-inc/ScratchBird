// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-BTREE-PAGE-ANCHOR
#include "datatype_binary.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::datatypes::DatatypeBinaryValue;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr u32 kIndexBtreePageBodyHeaderBytes = 96;

enum class IndexBtreePageKind : u16 {
  root = 1,
  internal = 2,
  leaf = 3,
  unknown = 0xffffu
};

struct IndexBtreeCell {
  u16 key_ordinal = 0;
  DatatypeBinaryValue key_value;
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  u64 child_page_number = 0;
  bool high_key = false;
  bool deleted = false;
};

struct IndexBtreePageBody {
  TypedUuid index_uuid;
  u64 page_number = 0;
  u64 parent_page_number = 0;
  u64 left_sibling_page_number = 0;
  u64 right_sibling_page_number = 0;
  u32 free_space_bytes = 0;
  u16 tree_level = 0;
  IndexBtreePageKind page_kind = IndexBtreePageKind::unknown;
  std::vector<IndexBtreeCell> cells;
};

struct IndexBtreePageBodyResult {
  Status status;
  IndexBtreePageBody body;
  std::vector<byte> serialized;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct IndexBtreePhysicalPageImage {
  u64 page_number = 0;
  std::vector<byte> serialized;
};

struct IndexBtreePhysicalTree {
  u32 page_size = 0;
  TypedUuid index_uuid;
  u64 root_page_number = 0;
  u64 next_page_number = 1;
  std::vector<IndexBtreePhysicalPageImage> pages;
};

struct IndexBtreePhysicalTreeResult {
  Status status;
  IndexBtreePhysicalTree tree;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexBtreePhysicalTreeImage {
  u32 page_size = 0;
  TypedUuid index_uuid;
  u64 root_page_number = 0;
  u64 next_page_number = 1;
  std::vector<IndexBtreePhysicalPageImage> pages;
  std::vector<std::string> evidence;
};

struct IndexBtreePhysicalTreeImageResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexBtreePhysicalTreeImage image;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexBtreePhysicalTreeValidationResult {
  Status status;
  DiagnosticRecord diagnostic;
  u64 reachable_page_count = 0;
  u64 reachable_leaf_page_count = 0;
  u64 live_entry_count = 0;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

enum class IndexBtreePhysicalCorruptionClass : u16 {
  none = 0,
  checksum = 1,
  page = 2,
  parent = 3,
  fence = 4,
  sibling = 5,
  order = 6,
  duplicate = 7,
  orphan_stale_page_image = 8,
  tree = 9,
  unknown = 0xffffu
};

struct IndexBtreePhysicalTreeReport {
  Status status;
  DiagnosticRecord diagnostic;
  u64 root_page_number = 0;
  u64 page_count = 0;
  u64 reachable_page_count = 0;
  u64 reachable_leaf_count = 0;
  u64 tuple_live_entry_estimate = 0;
  u64 tombstone_deleted_entry_count = 0;
  u64 tree_height = 0;
  u32 page_size = 0;
  u64 next_page_number = 0;
  IndexBtreePhysicalCorruptionClass corruption_class =
      IndexBtreePhysicalCorruptionClass::none;
  std::string exact_diagnostic_code;
  std::string exact_diagnostic_message_key;
  bool valid = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  std::vector<std::string> evidence;
  std::vector<std::string> support_bundle_rows;

  bool ok() const {
    return status.ok();
  }
};

struct IndexBtreePhysicalTreeReportResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexBtreePhysicalTreeReport report;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexBtreePhysicalTreeRebuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexBtreePhysicalTree tree;
  IndexBtreePhysicalTreeImage image;
  IndexBtreePhysicalTreeReport report;
  bool rebuilt = false;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexBtreePhysicalBulkBuildRequest {
  TypedUuid index_uuid;
  u32 page_size = 0;
  u32 leaf_entry_capacity = 0;
  u32 internal_entry_capacity = 0;
  std::vector<IndexBtreeCell> sorted_cells;
  bool sorted_order_proof_valid = false;
};

struct IndexBtreePhysicalBulkBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexBtreePhysicalTree tree;
  IndexBtreePhysicalTreeReport report;
  bool physical_leaf_pack = false;
  bool branch_levels_built = false;
  bool fence_keys_stored = false;
  bool candidate_root_generation_created = false;
  u64 leaf_page_count = 0;
  u64 branch_level_count = 0;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && candidate_root_generation_created;
  }
};

struct IndexBtreePhysicalTreeRepairResult {
  Status status;
  DiagnosticRecord diagnostic;
  IndexBtreePhysicalTree tree;
  IndexBtreePhysicalTreeImage image;
  IndexBtreePhysicalTreeReport before_report;
  IndexBtreePhysicalTreeReport after_report;
  bool repaired = false;
  bool refused = false;
  IndexBtreePhysicalCorruptionClass corruption_class =
      IndexBtreePhysicalCorruptionClass::none;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexBtreePhysicalInsertRequest {
  IndexBtreeCell cell;
};

struct IndexBtreePhysicalInsertResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool inserted = false;
  bool split_performed = false;
  bool root_split_performed = false;
  u64 root_page_number = 0;
  u64 leaf_page_number = 0;
  u64 left_page_number = 0;
  u64 right_page_number = 0;
  IndexBtreeCell separator_cell;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

enum class IndexBtreePhysicalUniqueNullPolicy : u16 {
  nulls_distinct = 1,
  nulls_not_distinct = 2
};

enum class IndexBtreePhysicalUniqueActiveDuplicatePolicy : u16 {
  wait_for_mga = 1,
  refuse_candidate = 2
};

enum class IndexBtreePhysicalUniqueConflictState : u16 {
  none = 0,
  wait_for_mga = 1,
  refuse_candidate = 2
};

struct IndexBtreePhysicalUniqueInsertRequest {
  IndexBtreeCell cell;
  IndexBtreePhysicalUniqueNullPolicy null_policy =
      IndexBtreePhysicalUniqueNullPolicy::nulls_distinct;
  IndexBtreePhysicalUniqueActiveDuplicatePolicy active_duplicate_policy =
      IndexBtreePhysicalUniqueActiveDuplicatePolicy::wait_for_mga;
  bool incoming_key_has_null = false;
  bool partial_predicate_participates = true;
  bool allow_same_row_update = false;
  TypedUuid same_row_proof_uuid;
};

struct IndexBtreePhysicalUniqueConflictCandidate {
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  u64 leaf_page_number = 0;
  u32 cell_ordinal = 0;
  bool same_key_identity = true;
  bool same_row = false;
  bool exact_live_entry = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
};

struct IndexBtreePhysicalUniqueInsertResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool inserted = false;
  bool bypassed_partial_predicate = false;
  bool null_exempt_from_conflict = false;
  bool conflict = false;
  bool same_row_update_allowed = false;
  IndexBtreePhysicalUniqueConflictState conflict_state =
      IndexBtreePhysicalUniqueConflictState::none;
  IndexBtreePhysicalInsertResult insert_result;
  std::vector<IndexBtreePhysicalUniqueConflictCandidate> conflict_candidates;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

struct IndexBtreePhysicalDeleteRequest {
  IndexBtreeCell cell;
};

struct IndexBtreePhysicalDeleteResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool deleted = false;
  bool tombstone_marked = false;
  bool cleanup_performed = false;
  bool rebalance_performed = false;
  bool merge_performed = false;
  bool structural_rebuild_performed = false;
  bool root_collapsed = false;
  u64 root_page_number = 0;
  u64 leaf_page_number = 0;
  u64 kept_page_number = 0;
  u64 removed_page_number = 0;
  u32 tombstones_removed = 0;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok();
  }
};

enum class IndexBtreePhysicalScanMode : u16 {
  point = 1,
  range = 2,
  prefix = 3,
  ordered = 4
};

enum class IndexBtreePhysicalScanOrdering : u16 {
  forward = 1,
  reverse = 2
};

struct IndexBtreePhysicalScanBound {
  std::vector<byte> encoded_key;
  bool inclusive = true;
  bool unbounded = true;
};

struct IndexBtreePhysicalScanRequest {
  IndexBtreePhysicalScanMode mode = IndexBtreePhysicalScanMode::ordered;
  IndexBtreePhysicalScanOrdering ordering = IndexBtreePhysicalScanOrdering::forward;
  std::vector<byte> point_key;
  IndexBtreePhysicalScanBound lower_bound;
  IndexBtreePhysicalScanBound upper_bound;
  std::vector<byte> prefix;
  u64 limit = 0;
};

struct IndexBtreePhysicalRowLocator {
  std::vector<byte> encoded_key;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  u64 leaf_page_number = 0;
  u32 cell_ordinal = 0;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool tombstone_excluded = true;
};

struct IndexBtreePhysicalScanResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<IndexBtreePhysicalRowLocator> locators;
  std::vector<std::string> evidence;
  u64 reachable_leaf_pages = 0;
  u64 visited_leaf_pages = 0;
  u64 pruned_leaf_pages = 0;
  u64 pruned_subtrees = 0;

  bool ok() const {
    return status.ok();
  }
};

const char* IndexBtreePageKindName(IndexBtreePageKind kind);
const char* IndexBtreePhysicalScanModeName(IndexBtreePhysicalScanMode mode);
const char* IndexBtreePhysicalScanOrderingName(IndexBtreePhysicalScanOrdering ordering);
const char* IndexBtreePhysicalCorruptionClassName(
    IndexBtreePhysicalCorruptionClass corruption_class);
u64 ComputeIndexBtreePageChecksum(const std::vector<byte>& body);
IndexBtreePageBodyResult BuildIndexBtreePageBody(const IndexBtreePageBody& body, u32 page_size);
IndexBtreePageBodyResult ParseIndexBtreePageBody(const std::vector<byte>& serialized, u64 page_number);
IndexBtreePhysicalTreeResult InitializeIndexBtreePhysicalTree(TypedUuid index_uuid, u32 page_size);
IndexBtreePhysicalTreeValidationResult ValidateIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree);
IndexBtreePhysicalTreeReportResult BuildIndexBtreePhysicalTreeReport(
    const IndexBtreePhysicalTree& tree);
IndexBtreePhysicalTreeRebuildResult RebuildIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree);
IndexBtreePhysicalBulkBuildResult BuildIndexBtreePhysicalBulkLoadedTree(
    const IndexBtreePhysicalBulkBuildRequest& request);
IndexBtreePhysicalTreeRepairResult RepairIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree);
IndexBtreePhysicalTreeImageResult ExportIndexBtreePhysicalTreeImage(
    const IndexBtreePhysicalTree& tree);
IndexBtreePhysicalTreeResult ImportIndexBtreePhysicalTreeImage(
    const IndexBtreePhysicalTreeImage& image);
IndexBtreePageBodyResult FetchIndexBtreePhysicalPage(const IndexBtreePhysicalTree& tree,
                                                     u64 page_number);
IndexBtreePhysicalInsertResult InsertIndexBtreeCell(IndexBtreePhysicalTree* tree,
                                                   const IndexBtreePhysicalInsertRequest& request);
IndexBtreePhysicalUniqueInsertResult InsertUniqueIndexBtreeCell(
    IndexBtreePhysicalTree* tree,
    const IndexBtreePhysicalUniqueInsertRequest& request);
IndexBtreePhysicalDeleteResult DeleteIndexBtreeCell(IndexBtreePhysicalTree* tree,
                                                   const IndexBtreePhysicalDeleteRequest& request);
IndexBtreePhysicalScanResult ScanIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    const IndexBtreePhysicalScanRequest& request);
IndexBtreePhysicalScanResult PointLookupIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    const std::vector<byte>& encoded_key,
    u64 limit = 0);
IndexBtreePhysicalScanResult RangeScanIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    const IndexBtreePhysicalScanBound& lower_bound,
    const IndexBtreePhysicalScanBound& upper_bound,
    IndexBtreePhysicalScanOrdering ordering = IndexBtreePhysicalScanOrdering::forward,
    u64 limit = 0);
IndexBtreePhysicalScanResult PrefixScanIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    const std::vector<byte>& encoded_prefix,
    IndexBtreePhysicalScanOrdering ordering = IndexBtreePhysicalScanOrdering::forward,
    u64 limit = 0);
IndexBtreePhysicalScanResult OrderedScanIndexBtreePhysicalTree(
    const IndexBtreePhysicalTree& tree,
    IndexBtreePhysicalScanOrdering ordering = IndexBtreePhysicalScanOrdering::forward,
    u64 limit = 0);
DiagnosticRecord MakeIndexBtreePageDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

}  // namespace scratchbird::storage::page
