// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_INVERTED_SEARCH_SEGMENT_PUBLICATION
#include "index_optimizer_integration.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kInvertedSearchSegmentPublicationSearchKey =
    "DPC_INVERTED_SEARCH_SEGMENT_PUBLICATION";
inline constexpr const char* kInvertedSearchSegmentAuthoritySource =
    "engine_mga_base_table_search";

enum class InvertedSearchSegmentState : u32 {
  building = 1,
  validated = 2,
  publish_ready = 3,
  visible = 4,
  retired = 5,
  refused = 6
};

enum class InvertedSearchSegmentFallbackReason : u32 {
  none = 1,
  disabled_segment_exact_fallback = 2,
  missing_segment_exact_fallback = 3,
  stale_segment_exact_fallback = 4,
  corrupt_segment_exact_fallback = 5,
  incomplete_segment_exact_fallback = 6,
  non_authoritative_segment_exact_fallback = 7,
  invalid_identity_exact_fallback = 8
};

enum class InvertedSearchSegmentRecoveryClass : u32 {
  clean_visible_segments = 1,
  old_visible_segments_or_exact_fallback = 2,
  unsafe_segment_refused = 3
};

enum class InvertedSearchSegmentRecoveryAction : u32 {
  keep_visible_segments = 1,
  use_exact_base_table_fallback = 2,
  refuse_unsafe_segment_use = 3
};

struct InvertedSearchSegmentDescriptor {
  TypedUuid segment_uuid;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  u64 generation = 0;
  InvertedSearchSegmentState state = InvertedSearchSegmentState::building;
  bool visible = false;
  bool persisted_record_present = false;
  bool checksum_valid = true;
  bool complete = false;
  bool stale = false;
  bool validation_evidence_present = false;
  bool publish_barrier_evidence_present = false;
  bool publish_barrier_engine_owned_mga = false;
  std::string validation_evidence_ref;
  std::string publish_barrier_evidence_ref;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
  std::string authority_source = kInvertedSearchSegmentAuthoritySource;
  bool parser_finality_authority_claimed = false;
  bool client_finality_authority_claimed = false;
  bool timestamp_finality_authority_claimed = false;
  bool uuid_ordering_finality_authority_claimed = false;
  bool event_stream_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct InvertedSearchSegmentEvidenceRow {
  u64 sequence = 0;
  TypedUuid evidence_id;
  TypedUuid segment_uuid;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid merge_id;
  InvertedSearchSegmentState state = InvertedSearchSegmentState::building;
  u64 generation = 0;
  bool visible = false;
  bool validation_evidence_present = false;
  bool publish_barrier_evidence_present = false;
  bool publish_barrier_engine_owned_mga = false;
  bool old_visible_segments_retained = true;
  u64 old_visible_segment_count = 0;
  u64 new_visible_segment_count = 0;
  std::string authority_source = kInvertedSearchSegmentAuthoritySource;
  bool parser_finality_authority = false;
  bool client_state_authority = false;
  bool timestamp_ordering_authority = false;
  bool uuid_ordering_authority = false;
  bool event_stream_authority = false;
  bool reference_authority = false;
  bool write_ahead_authority = false;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct InvertedSearchSegmentLedger {
  std::vector<InvertedSearchSegmentDescriptor> segments;
  std::vector<InvertedSearchSegmentEvidenceRow> evidence;
  u64 next_evidence_sequence = 1;
  u64 ledger_generation = 1;
};

struct InvertedSearchSegmentBuildRequest {
  TypedUuid segment_uuid;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  u64 generation = 1;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
  bool parser_finality_authority = false;
  bool client_state_authority = false;
  bool timestamp_ordering_authority = false;
  bool uuid_ordering_authority = false;
  bool event_stream_authority = false;
  bool reference_authority = false;
  bool write_ahead_authority = false;
};

struct InvertedSearchSegmentValidationRequest {
  std::string validation_evidence_ref;
  bool validation_succeeded = false;
  bool engine_mga_inventory_evidence_present = false;
  bool complete = false;
};

struct InvertedSearchSegmentPublishRequest {
  std::string publish_barrier_evidence_ref;
  bool engine_owned_mga_publish_barrier = false;
  std::string authority_source = kInvertedSearchSegmentAuthoritySource;
};

struct InvertedSearchSegmentLifecycleResult {
  Status status;
  bool accepted = false;
  bool fail_closed = true;
  InvertedSearchSegmentDescriptor segment;
  InvertedSearchSegmentEvidenceRow evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && accepted && !fail_closed; }
};

struct InvertedSearchSegmentMergeRequest {
  TypedUuid merge_id;
  TypedUuid merged_segment_uuid;
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  std::vector<TypedUuid> input_segment_uuids;
  u64 merged_generation = 0;
  std::string validation_evidence_ref;
  std::string publish_barrier_evidence_ref;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
  bool validation_succeeded = false;
  bool engine_mga_inventory_evidence_present = false;
  bool engine_owned_mga_publish_barrier = false;
  bool merge_commit_marker_complete = false;
  std::string authority_source = kInvertedSearchSegmentAuthoritySource;
};

struct InvertedSearchSegmentMergeResult {
  Status status;
  bool merge_committed = false;
  bool old_visible_segments_retained = true;
  bool fail_closed = true;
  InvertedSearchSegmentDescriptor merged_segment;
  u64 retired_input_segment_count = 0;
  InvertedSearchSegmentEvidenceRow evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && merge_committed && !fail_closed; }
};

struct InvertedSearchSegmentAccessRequest {
  std::vector<InvertedSearchSegmentDescriptor> segments;
  bool search_segments_enabled = true;
  bool exact_base_table_fallback_available = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
};

struct InvertedSearchSegmentAccessPlan {
  Status status;
  DiagnosticRecord diagnostic;
  IndexPlanCategory selected_category = IndexPlanCategory::fallback_full_scan;
  std::string selected_access = "exact_base_table_scan";
  std::string fallback_reason = "none";
  std::string authority_source = kInvertedSearchSegmentAuthoritySource;
  bool segment_search_selected = false;
  bool exact_base_table_fallback_selected = true;
  bool exact_base_table_fallback_available = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool segment_metadata_visibility_authority = false;
  bool segment_metadata_finality_authority = false;
  std::vector<TypedUuid> visible_segment_uuids;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && segment_search_selected; }
};

struct InvertedSearchSegmentRecoveryResult {
  Status status;
  bool fail_closed = true;
  InvertedSearchSegmentRecoveryClass recovery_class =
      InvertedSearchSegmentRecoveryClass::unsafe_segment_refused;
  InvertedSearchSegmentRecoveryAction action =
      InvertedSearchSegmentRecoveryAction::refuse_unsafe_segment_use;
  std::string stable_reason;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* InvertedSearchSegmentStateName(InvertedSearchSegmentState state);
const char* InvertedSearchSegmentFallbackReasonName(
    InvertedSearchSegmentFallbackReason reason);
const char* InvertedSearchSegmentRecoveryClassName(
    InvertedSearchSegmentRecoveryClass recovery_class);
const char* InvertedSearchSegmentRecoveryActionName(
    InvertedSearchSegmentRecoveryAction action);

bool InvertedSearchSegmentDescriptorIdentityValid(
    const InvertedSearchSegmentDescriptor& segment);
bool InvertedSearchSegmentDescriptorAuthorityClean(
    const InvertedSearchSegmentDescriptor& segment);
bool InvertedSearchSegmentDescriptorUsable(
    const InvertedSearchSegmentDescriptor& segment);

InvertedSearchSegmentLifecycleResult StartInvertedSearchSegmentBuild(
    InvertedSearchSegmentLedger* ledger,
    const InvertedSearchSegmentBuildRequest& request);
InvertedSearchSegmentLifecycleResult ValidateInvertedSearchSegmentBuild(
    InvertedSearchSegmentLedger* ledger,
    InvertedSearchSegmentDescriptor* segment,
    const InvertedSearchSegmentValidationRequest& request);
InvertedSearchSegmentLifecycleResult MarkInvertedSearchSegmentPublishReady(
    InvertedSearchSegmentLedger* ledger,
    InvertedSearchSegmentDescriptor* segment,
    const InvertedSearchSegmentPublishRequest& request);
InvertedSearchSegmentLifecycleResult PublishInvertedSearchSegment(
    InvertedSearchSegmentLedger* ledger,
    InvertedSearchSegmentDescriptor* segment);
InvertedSearchSegmentMergeResult ApplyInvertedSearchSegmentMerge(
    InvertedSearchSegmentLedger* ledger,
    const InvertedSearchSegmentMergeRequest& request);
InvertedSearchSegmentAccessPlan PlanInvertedSearchSegmentAccess(
    const InvertedSearchSegmentAccessRequest& request);
InvertedSearchSegmentRecoveryResult ClassifyInvertedSearchSegmentReopen(
    const InvertedSearchSegmentLedger& ledger,
    bool exact_base_table_fallback_available);

DiagnosticRecord MakeInvertedSearchSegmentDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
