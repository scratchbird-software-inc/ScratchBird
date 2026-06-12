// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// IRC-092 physical columnar-zone summary and candidate-stream surface.
#include "page_extent_summary.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr const char* kPhysicalColumnarZoneArtifactKind =
    "physical_columnar_zone_segment";
inline constexpr const char* kPhysicalColumnarZoneFormatSearchKey =
    "IRC_092_PHYSICAL_COLUMNAR_ZONE_FORMAT";
inline constexpr u32 kPhysicalColumnarZoneCurrentMajor = 1;
inline constexpr u32 kPhysicalColumnarZoneCurrentMinor = 0;
inline constexpr u32 kPhysicalColumnarZoneDefaultDictionaryLimit = 8;

enum class PhysicalColumnarZoneOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  identity_mismatch = 5,
  malformed_column_payload = 6,
  malformed_row_group_payload = 7,
  unsafe_encoding = 8,
  truncated_payload = 9,
  corrupt_payload = 10,
  refused = 11
};

enum class PhysicalColumnarZonePruneDecision : u32 {
  scan_with_recheck = 1,
  pruned_by_min_max = 2,
  pruned_by_exact_dictionary = 3,
  exact_dictionary_overflow_scan = 4,
  full_scan_fallback = 5
};

enum class PhysicalColumnarZoneMutationKind : u32 {
  append_row = 1,
  update_row = 2,
  delete_row = 3
};

struct PhysicalColumnarZoneCompressionPolicy {
  u32 column_ordinal = 0;
  std::string scalar_type_key;
  std::string codec_id;
  u64 uncompressed_bytes = 0;
  u64 compressed_bytes = 0;
  double estimated_cpu_cost = 0.0;
  double estimated_read_cost = 0.0;
  double estimated_write_cost = 0.0;
  bool exact_fallback_equivalence = true;
  bool storage_runtime_evidence_only = true;
  bool compression_value_authority_claimed = false;
};

struct PhysicalColumnarZoneColumnValueEvidence {
  u32 column_ordinal = 0;
  std::string scalar_type_key;
  std::string encoded_scalar;
  bool value_is_null = false;
};

struct PhysicalColumnarZoneRowEvidence {
  u64 page_id = 0;
  u64 row_group_id = 0;
  u64 row_ordinal = 0;
  u64 base_generation = 0;
  bool authoritative_columnar_page_evidence = true;
  bool physically_deleted = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool summary_visibility_authority_claimed = false;
  bool summary_finality_authority_claimed = false;
  std::vector<PhysicalColumnarZoneColumnValueEvidence> columns;
};

struct PhysicalColumnarZoneColumnSummary {
  u32 column_ordinal = 0;
  std::string scalar_type_key;
  u64 row_count = 0;
  u64 null_count = 0;
  bool min_present = false;
  bool max_present = false;
  std::string encoded_min;
  std::string encoded_max;
  bool dictionary_exact = true;
  bool dictionary_overflow = false;
  std::vector<std::string> dictionary_values;
};

struct PhysicalColumnarZoneRowGroupBoundary {
  u64 row_group_id = 0;
  u64 first_page_id = 0;
  u32 page_count = 0;
  u64 first_row_ordinal = 0;
  u64 row_count = 0;
};

struct PhysicalColumnarZoneRowGroupSummary {
  PhysicalColumnarZoneRowGroupBoundary boundary;
  u64 row_count = 0;
  u64 deleted_row_count = 0;
  u64 base_generation = 0;
  u64 summary_generation = 0;
  PageExtentSummaryStatus status = PageExtentSummaryStatus::missing;
  bool persisted_record_present = false;
  bool checksum_valid = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool exact_recheck_required = true;
  std::vector<u64> candidate_row_ordinals;
  std::vector<PhysicalColumnarZoneColumnSummary> columns;
};

struct PhysicalColumnarZoneSegment {
  std::string relation_uuid;
  std::string index_uuid;
  std::string segment_uuid;
  PageExtentSummaryFormatVersion format_version{
      kPhysicalColumnarZoneCurrentMajor, kPhysicalColumnarZoneCurrentMinor};
  u64 base_generation = 0;
  u64 summary_generation = 0;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool exact_recheck_required = true;
  bool visibility_finality_authority = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  std::vector<PhysicalColumnarZoneRowGroupSummary> row_groups;
  std::vector<PhysicalColumnarZoneCompressionPolicy> compression_policies;
  std::vector<std::string> evidence;
};

struct PhysicalColumnarZoneBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string segment_uuid;
  u64 base_generation = 0;
  u64 summary_generation = 0;
  u32 dictionary_limit = kPhysicalColumnarZoneDefaultDictionaryLimit;
  std::vector<PhysicalColumnarZoneRowEvidence> rows;
  std::vector<PhysicalColumnarZoneCompressionPolicy> compression_policies;
};

struct PhysicalColumnarZoneBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalColumnarZoneSegment segment;
  bool built = false;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && built; }
};

struct PhysicalColumnarZoneSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct PhysicalColumnarZoneOpenRequest {
  std::vector<byte> bytes;
  bool expected_relation_uuid_present = false;
  std::string expected_relation_uuid;
  bool expected_index_uuid_present = false;
  std::string expected_index_uuid;
  bool expected_segment_uuid_present = false;
  std::string expected_segment_uuid;
  bool expected_base_generation_present = false;
  u64 expected_base_generation = 0;
};

struct PhysicalColumnarZoneOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalColumnarZoneOpenClass open_class =
      PhysicalColumnarZoneOpenClass::refused;
  PhysicalColumnarZoneSegment segment;
  bool scan_required = true;
  bool rebuild_required = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == PhysicalColumnarZoneOpenClass::current;
  }
};

struct PhysicalColumnarZonePredicate {
  u32 column_ordinal = 0;
  std::string scalar_type_key;
  bool lower_present = false;
  bool upper_present = false;
  std::string encoded_lower;
  std::string encoded_upper;
};

struct PhysicalColumnarZoneCandidateGroup {
  std::string segment_uuid;
  PhysicalColumnarZoneRowGroupBoundary boundary;
  PhysicalColumnarZonePruneDecision decision =
      PhysicalColumnarZonePruneDecision::full_scan_fallback;
  bool scan_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool exact_recheck_required = true;
  std::vector<u64> candidate_page_ids;
  std::vector<u64> candidate_row_ordinals;
  std::string reason;
};

struct PhysicalColumnarZonePruneRequest {
  PhysicalColumnarZoneSegment segment;
  std::vector<PhysicalColumnarZonePredicate> predicates;
};

struct PhysicalColumnarZonePruneResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool any_pruned = false;
  bool full_scan_required = false;
  std::vector<PhysicalColumnarZoneCandidateGroup> groups;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !full_scan_required; }
};

struct PhysicalColumnarZoneLateMaterializationRequest {
  PhysicalColumnarZoneSegment segment;
  PhysicalColumnarZonePruneResult prune_result;
  std::vector<u32> projection_column_ordinals;
};

struct PhysicalColumnarZoneCandidateStream {
  Status status;
  DiagnosticRecord diagnostic;
  bool stream_ready = false;
  bool fetches_non_candidate_rows = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool exact_recheck_required = true;
  bool visibility_finality_authority = false;
  std::vector<u32> projection_column_ordinals;
  std::vector<u64> candidate_row_ordinals;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && stream_ready; }
};

struct PhysicalColumnarZoneMutation {
  PhysicalColumnarZoneMutationKind kind =
      PhysicalColumnarZoneMutationKind::append_row;
  bool before_row_present = false;
  PhysicalColumnarZoneRowEvidence before_row;
  bool after_row_present = false;
  PhysicalColumnarZoneRowEvidence after_row;
  bool rebuild_admitted = false;
  std::vector<PhysicalColumnarZoneRowEvidence> authoritative_base_rows;
  std::vector<PhysicalColumnarZoneCompressionPolicy> compression_policies;
};

struct PhysicalColumnarZoneMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalColumnarZoneSegment segment;
  bool applied = false;
  bool segment_invalidated = false;
  bool rebuild_performed = false;
  bool scan_required = true;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied; }
};

PhysicalColumnarZoneBuildResult BuildPhysicalColumnarZoneFromPageEvidence(
    const PhysicalColumnarZoneBuildRequest& request);

PhysicalColumnarZoneSerializeResult SerializePhysicalColumnarZoneSegment(
    const PhysicalColumnarZoneSegment& segment);

PhysicalColumnarZoneOpenResult OpenPhysicalColumnarZoneSegment(
    const PhysicalColumnarZoneOpenRequest& request);

PhysicalColumnarZonePruneResult PrunePhysicalColumnarZone(
    const PhysicalColumnarZonePruneRequest& request);

PhysicalColumnarZoneCandidateStream OpenPhysicalColumnarZoneCandidateStream(
    const PhysicalColumnarZoneLateMaterializationRequest& request);

PhysicalColumnarZoneMutationResult ApplyPhysicalColumnarZoneMutation(
    const PhysicalColumnarZoneSegment& segment,
    const PhysicalColumnarZoneMutation& mutation,
    u32 dictionary_limit = kPhysicalColumnarZoneDefaultDictionaryLimit);

PhysicalColumnarZoneMutationResult RepairPhysicalColumnarZoneFromPageEvidence(
    const PhysicalColumnarZoneSegment& stale_or_corrupt_segment,
    const std::vector<PhysicalColumnarZoneRowEvidence>& authoritative_base_rows,
    const std::vector<PhysicalColumnarZoneCompressionPolicy>& compression_policies,
    bool repair_admitted,
    u32 dictionary_limit = kPhysicalColumnarZoneDefaultDictionaryLimit);

const char* PhysicalColumnarZoneOpenClassName(
    PhysicalColumnarZoneOpenClass open_class);
const char* PhysicalColumnarZonePruneDecisionName(
    PhysicalColumnarZonePruneDecision decision);
DiagnosticRecord MakePhysicalColumnarZoneDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail = {});

}  // namespace scratchbird::core::index
