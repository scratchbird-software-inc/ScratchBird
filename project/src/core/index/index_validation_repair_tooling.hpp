// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_INDEX_VALIDATION_REPAIR_TOOLING
#include "index_family_registry.hpp"
#include "index_verification.hpp"
#include "inverted_search_segment_publication.hpp"
#include "page_extent_summary.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "shadow_index_build_lifecycle.hpp"
#include "time_range_summary_pruning.hpp"
#include "vector_index_generation_publication.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kIndexValidationRepairToolingSearchKey =
    "DPC_INDEX_VALIDATION_REPAIR_TOOLING";

enum class IndexValidationRepairFamily : u32 {
  ordered_table_candidate_set = 1,
  secondary_delta_ledger = 2,
  page_extent_summary = 3,
  time_range_summary = 4,
  shadow_index_build_state = 5,
  inverted_search_segment_state = 6,
  vector_generation_state = 7
};

enum class IndexValidationRepairOperation : u32 {
  validate = 1,
  repair = 2,
  rebuild = 3,
  discard_unpublished = 4
};

enum class IndexValidationRepairClass : u32 {
  clean = 1,
  safe_fallback = 2,
  repair_required = 3,
  rebuild_required = 4,
  discard_required = 5,
  refused = 6,
  fail_closed = 7
};

struct IndexValidationRepairTarget {
  TypedUuid database_uuid;
  TypedUuid table_uuid;
  TypedUuid index_uuid;
  TypedUuid generation_uuid;
  IndexFamily physical_family = IndexFamily::unknown;
  bool names_resolved_to_uuids = true;
  bool catalog_resolution_proven = true;
  bool contains_sql_text = false;
};

struct IndexValidationSupportEvidence {
  std::string key;
  std::string value;
  bool sensitive = false;
};

struct IndexValidationRepairState {
  IndexVerificationRequest ordered_candidate_set;
  PersistentSecondaryIndexDeltaLedger delta_ledger;
  PageExtentSummaryMetadata page_extent_summary;
  PageExtentSummaryFormatCompatibility page_extent_summary_format;
  PageExtentSummaryMaintenanceEvent page_extent_rebuild_event;
  TimeRangeSummaryPruneRequest time_range_summary;
  ShadowIndexBuildRecord shadow_build;
  InvertedSearchSegmentLedger inverted_segments;
  VectorGenerationLedger vector_generations;
  bool exact_base_table_fallback_available = true;
  bool exact_vector_scan_fallback_available = true;
};

struct IndexValidationRepairRequest {
  IndexValidationRepairOperation operation =
      IndexValidationRepairOperation::validate;
  IndexValidationRepairFamily validation_family =
      IndexValidationRepairFamily::ordered_table_candidate_set;
  IndexValidationRepairTarget target;
  IndexValidationRepairState state;
  bool read_only_database = false;
  bool policy_allows_mutation = false;
  bool allow_sensitive_support_data = false;
  bool expected_generation_present = false;
  u64 expected_generation = 0;
};

struct IndexValidationRepairResult {
  Status status;
  bool admitted = false;
  bool mutating = false;
  bool validation_read_only = true;
  bool mutation_applied = false;
  bool planner_visible = false;
  bool fail_closed = false;
  bool redaction_required = true;
  IndexValidationRepairClass classification =
      IndexValidationRepairClass::refused;
  IndexValidationRepairState repaired_state;
  std::vector<std::string> actions;
  std::vector<IndexValidationSupportEvidence> support_evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

enum class IndexFamilyValidationRepairPath : u32 {
  persistent_physical = 1,
  memory_only_runtime = 2,
  memory_primary_cold_start = 3,
  reference_semantic_mapping = 4,
  policy_refusal = 5,
  unavailable = 6
};

enum class IndexFamilyValidationRepairOpenState : u32 {
  open_allowed = 1,
  open_refused = 2,
  repair_required = 3,
  rebuild_required = 4,
  non_physical_refused = 5
};

struct IndexFamilyValidationRepairProof {
  bool catalog_uuid_binding_proven = false;
  bool exact_base_table_source_present = false;
  bool physical_generation_present = false;
  bool physical_generation_checksum_valid = false;
  bool runtime_provider_attached = false;
  bool runtime_epoch_current = false;
  bool cold_start_source_present = false;
  bool cold_start_checksum_valid = false;
  bool repair_output_validated = false;
  bool rebuild_output_validated = false;
  std::string proof_token;
  std::string sensitive_detail;
};

struct IndexFamilyValidationRepairRequest {
  IndexValidationRepairOperation operation =
      IndexValidationRepairOperation::validate;
  IndexFamily family = IndexFamily::unknown;
  IndexValidationRepairTarget target;
  IndexFamilyValidationRepairProof proof;
  bool read_only_database = false;
  bool policy_allows_mutation = false;
  bool allow_sensitive_support_data = false;
};

struct IndexFamilyValidationRepairResult {
  Status status;
  bool admitted = false;
  bool mutating = false;
  bool validation_read_only = true;
  bool mutation_applied = false;
  bool planner_visible = false;
  bool open_allowed = false;
  bool open_refused = true;
  bool fail_closed = true;
  bool redaction_required = true;
  bool observational_only = true;
  bool catalog_authority = false;
  bool parser_authority = false;
  bool provider_authority = false;
  bool reference_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool storage_authority = false;
  IndexFamily family = IndexFamily::unknown;
  std::string family_id;
  std::string blocker;
  std::string validation_state;
  std::string repair_state;
  IndexFamilyValidationRepairPath path =
      IndexFamilyValidationRepairPath::unavailable;
  IndexFamilyValidationRepairOpenState open_state =
      IndexFamilyValidationRepairOpenState::open_refused;
  IndexValidationRepairClass classification =
      IndexValidationRepairClass::fail_closed;
  std::vector<std::string> actions;
  std::vector<IndexValidationSupportEvidence> support_evidence;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && admitted && !fail_closed && open_allowed;
  }
};

const char* IndexValidationRepairFamilyName(
    IndexValidationRepairFamily family);
const char* IndexValidationRepairOperationName(
    IndexValidationRepairOperation operation);
const char* IndexValidationRepairClassName(
    IndexValidationRepairClass classification);
const char* IndexFamilyValidationRepairPathName(
    IndexFamilyValidationRepairPath path);
const char* IndexFamilyValidationRepairOpenStateName(
    IndexFamilyValidationRepairOpenState state);

bool IndexValidationRepairOperationMutates(
    IndexValidationRepairOperation operation);
IndexValidationRepairResult ExecuteIndexValidationRepairOperation(
    const IndexValidationRepairRequest& request);
IndexFamilyValidationRepairResult ExecuteIndexFamilyValidationRepairOperation(
    const IndexFamilyValidationRepairRequest& request);
DiagnosticRecord MakeIndexValidationRepairDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
