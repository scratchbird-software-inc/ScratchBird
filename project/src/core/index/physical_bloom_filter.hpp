// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// IRC-091 physical Bloom filter bitset storage.
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

inline constexpr const char* kPhysicalBloomFilterArtifactKind =
    "physical_bloom_filter_bitset";
inline constexpr const char* kPhysicalBloomFilterFormatSearchKey =
    "IRC_091_PHYSICAL_BLOOM_FILTER_FORMAT";
inline constexpr u32 kPhysicalBloomFilterCurrentMajor = 1;
inline constexpr u32 kPhysicalBloomFilterCurrentMinor = 0;
inline constexpr u32 kPhysicalBloomFilterMinHashCount = 1;
inline constexpr u32 kPhysicalBloomFilterMaxHashCount = 30;
inline constexpr u64 kPhysicalBloomFilterMinBitCount = 64;
inline constexpr u64 kPhysicalBloomFilterMaxBitCount = 1ull << 28;
inline constexpr u32 kPhysicalBloomFilterDefaultBlockBits = 512;

enum class PhysicalBloomFilterOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  malformed_bitset_length = 6,
  invalid_hash_count = 7,
  identity_mismatch = 8,
  refused = 9
};

enum class PhysicalBloomProbeDecision : u32 {
  definitely_absent = 1,
  maybe_present = 2,
  scan_or_rebuild_required = 3
};

enum class PhysicalBloomMutationKind : u32 {
  append_key = 1,
  delete_key = 2,
  update_key = 3
};

struct PhysicalBloomBlockedLayoutMetadata {
  u32 block_bit_count = kPhysicalBloomFilterDefaultBlockBits;
  u32 bits_per_word = 64;
  u64 block_count = 0;
  u64 bitset_byte_count = 0;
  bool packed_bitset = true;
  bool blocked_layout = true;
};

struct PhysicalBloomEncodedKeyEvidence {
  std::string row_uuid;
  std::string version_uuid;
  std::string encoded_key;
  bool authoritative_encoded_key_evidence = true;
  bool engine_mga_visible = true;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct PhysicalBloomAbsentProbeEvidence {
  std::string encoded_key;
  bool authoritative_absent_probe_evidence = true;
};

struct PhysicalBloomFilterPage {
  std::string relation_uuid;
  std::string index_uuid;
  std::string segment_uuid;
  PageExtentSummaryFormatVersion format_version{
      kPhysicalBloomFilterCurrentMajor, kPhysicalBloomFilterCurrentMinor};
  u64 base_generation = 0;
  u64 filter_generation = 0;
  u64 seed = 0;
  u32 seed_version = 0;
  u32 hash_count = 0;
  u64 bit_count = 0;
  u64 inserted_key_count = 0;
  double fpr_target = 0.0;
  double estimated_fpr = 0.0;
  double observed_fpr = 0.0;
  u64 observed_absent_probe_count = 0;
  u64 observed_false_positive_count = 0;
  PhysicalBloomBlockedLayoutMetadata layout;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool exact_recheck_required_for_maybe_present = true;
  bool visibility_finality_authority = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  std::vector<byte> bitset;
  std::vector<std::string> evidence;
};

struct PhysicalBloomFilterBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string segment_uuid;
  u64 base_generation = 0;
  u64 filter_generation = 0;
  u64 seed = 0;
  u32 seed_version = 0;
  u32 hash_count = 0;
  u64 bit_count = 0;
  double fpr_target = 0.0;
  bool absent_probe_sample_required_for_benchmark_clean = false;
  std::vector<PhysicalBloomEncodedKeyEvidence> authoritative_keys;
  std::vector<PhysicalBloomAbsentProbeEvidence> absent_probe_sample;
};

struct PhysicalBloomFilterBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalBloomFilterPage page;
  bool built = false;
  bool benchmark_clean_admissible = false;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && built; }
};

struct PhysicalBloomFilterSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct PhysicalBloomFilterOpenRequest {
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

struct PhysicalBloomFilterOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalBloomFilterOpenClass open_class = PhysicalBloomFilterOpenClass::refused;
  PhysicalBloomFilterPage page;
  bool scan_required = true;
  bool rebuild_required = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == PhysicalBloomFilterOpenClass::current;
  }
};

struct PhysicalBloomProbeRequest {
  PhysicalBloomFilterPage page;
  std::string encoded_key;
};

struct PhysicalBloomProbeResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalBloomProbeDecision decision =
      PhysicalBloomProbeDecision::scan_or_rebuild_required;
  bool can_prune = false;
  bool scan_required = true;
  bool exact_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool false_positive_possible = true;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok(); }
};

struct PhysicalBloomFprMeasurementRequest {
  PhysicalBloomFilterPage page;
  std::vector<PhysicalBloomAbsentProbeEvidence> absent_probe_sample;
  bool sample_required_for_benchmark_clean = true;
};

struct PhysicalBloomFprMeasurementResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool measured = false;
  bool benchmark_clean_admissible = false;
  double estimated_fpr = 0.0;
  double observed_fpr = 0.0;
  u64 absent_probe_count = 0;
  u64 false_positive_count = 0;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && measured; }
};

struct PhysicalBloomFilterMutation {
  PhysicalBloomMutationKind kind = PhysicalBloomMutationKind::append_key;
  bool before_key_present = false;
  PhysicalBloomEncodedKeyEvidence before_key;
  bool after_key_present = false;
  PhysicalBloomEncodedKeyEvidence after_key;
  bool rebuild_admitted = false;
  std::vector<PhysicalBloomEncodedKeyEvidence> authoritative_source_keys;
  std::vector<PhysicalBloomAbsentProbeEvidence> absent_probe_sample;
};

struct PhysicalBloomFilterMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  PhysicalBloomFilterPage page;
  bool applied = false;
  bool filter_invalidated = false;
  bool rebuild_performed = false;
  bool scan_required = true;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied; }
};

PhysicalBloomFilterBuildResult BuildPhysicalBloomFilterFromEncodedKeyEvidence(
    const PhysicalBloomFilterBuildRequest& request);

PhysicalBloomFilterSerializeResult SerializePhysicalBloomFilterPage(
    const PhysicalBloomFilterPage& page);

PhysicalBloomFilterOpenResult OpenPhysicalBloomFilterPage(
    const PhysicalBloomFilterOpenRequest& request);

PhysicalBloomProbeResult ProbePhysicalBloomFilter(
    const PhysicalBloomProbeRequest& request);

PhysicalBloomFprMeasurementResult MeasurePhysicalBloomFilterFpr(
    const PhysicalBloomFprMeasurementRequest& request);

PhysicalBloomFilterMutationResult ApplyPhysicalBloomFilterMutation(
    const PhysicalBloomFilterPage& page,
    const PhysicalBloomFilterMutation& mutation);

PhysicalBloomFilterMutationResult RepairPhysicalBloomFilterFromEncodedKeyEvidence(
    const PhysicalBloomFilterPage& stale_or_corrupt_page,
    const std::vector<PhysicalBloomEncodedKeyEvidence>& authoritative_source_keys,
    bool repair_admitted,
    const std::vector<PhysicalBloomAbsentProbeEvidence>& absent_probe_sample = {});

const char* PhysicalBloomFilterOpenClassName(
    PhysicalBloomFilterOpenClass open_class);
const char* PhysicalBloomProbeDecisionName(PhysicalBloomProbeDecision decision);

DiagnosticRecord MakePhysicalBloomFilterDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::core::index
