// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB_GIN_PHYSICAL_PROVIDER
#include "text_inverted_segment.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kGinPhysicalProviderSearchKey =
    "SB_GIN_PHYSICAL_PROVIDER";
inline constexpr const char* kGinPhysicalProviderArtifactKind =
    "gin_physical_provider";
inline constexpr u32 kGinPhysicalProviderCurrentMajor = 1;
inline constexpr u32 kGinPhysicalProviderCurrentMinor = 0;
inline constexpr u32 kGinPhysicalProviderDefaultPendingFlushThreshold = 4;
inline constexpr u32 kGinPhysicalProviderDefaultPostingListLimit = 3;

enum class GinPhysicalOpenClass : u32 {
  current = 1,
  stale_format = 2,
  stale_generation = 3,
  bad_checksum = 4,
  corrupt_payload = 5,
  identity_mismatch = 6,
  stale_epoch = 7,
  unsafe_opclass = 8,
  missing_recheck_proof = 9,
  authority_claim_refused = 10,
  refused = 11
};

enum class GinTriState : u32 {
  no = 1,
  maybe = 2,
  yes = 3
};

enum class GinQueryStrategy : u32 {
  contains_all = 1,
  contains_any = 2
};

enum class GinMutationKind : u32 {
  insert_row = 1,
  delete_row = 2,
  update_row = 3,
  flush_pending = 4
};

struct GinExactRecheckProof {
  bool proof_supplied = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::string evidence_ref;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
};

struct GinOpclassDescriptor {
  std::string opclass_name;
  u64 opclass_epoch = 0;
  u64 resource_epoch = 0;
  bool deterministic = false;
  bool immutable = false;
  bool safe = false;
  bool tri_consistent_supported = false;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
};

struct GinOpclassExtractorInput {
  std::string exact_source_value;
  TextInvertedRowLocator locator;
};

struct GinOpclassExtractorOutput {
  std::vector<std::string> keys;
  bool deterministic = false;
  bool exact_source_recheck_evidence_present = false;
  std::string evidence_ref;
};

using GinOpclassExtractor =
    std::function<GinOpclassExtractorOutput(const GinOpclassExtractorInput&)>;

struct GinSourceRow {
  TextInvertedRowLocator locator;
  std::string exact_source_value;
};

struct GinPostingStorageEntry {
  std::string key;
  std::vector<TextInvertedRowLocator> posting_list;
  std::vector<std::vector<TextInvertedRowLocator>> posting_tree_pages;
  bool posting_list_used = true;
  bool posting_tree_used = false;
};

struct GinPendingEntry {
  std::string key;
  TextInvertedRowLocator locator;
};

struct GinPhysicalProvider {
  std::string artifact_kind = kGinPhysicalProviderArtifactKind;
  PageExtentSummaryFormatVersion format_version{
      kGinPhysicalProviderCurrentMajor, kGinPhysicalProviderCurrentMinor};
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  GinOpclassDescriptor opclass;
  u32 pending_flush_threshold =
      kGinPhysicalProviderDefaultPendingFlushThreshold;
  u32 posting_list_limit = kGinPhysicalProviderDefaultPostingListLimit;
  bool pending_list_present = true;
  bool posting_lists_present = true;
  bool posting_trees_present = false;
  bool tri_consistent_executor_present = true;
  bool candidate_evidence_only = true;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool write_ahead_log_finality_authority_claimed = false;
  std::vector<GinPendingEntry> pending_list;
  std::vector<GinPostingStorageEntry> entries;
  std::vector<std::string> evidence;
};

struct GinPhysicalBuildRequest {
  std::string relation_uuid;
  std::string index_uuid;
  std::string provider_uuid;
  u64 base_generation = 0;
  u64 provider_generation = 0;
  GinOpclassDescriptor opclass;
  u32 pending_flush_threshold =
      kGinPhysicalProviderDefaultPendingFlushThreshold;
  u32 posting_list_limit = kGinPhysicalProviderDefaultPostingListLimit;
  GinExactRecheckProof recheck_proof;
  std::vector<GinSourceRow> rows;
  GinOpclassExtractor extractor;
};

struct GinPhysicalBuildResult {
  Status status;
  DiagnosticRecord diagnostic;
  GinPhysicalProvider provider;
  bool built = false;
  bool fail_closed = true;

  bool ok() const { return status.ok() && built && !fail_closed; }
};

struct GinPhysicalSerializeResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<byte> bytes;
  u64 checksum = 0;

  bool ok() const { return status.ok() && !bytes.empty(); }
};

struct GinPhysicalOpenRequest {
  std::vector<byte> bytes;
  bool expected_relation_uuid_present = false;
  std::string expected_relation_uuid;
  bool expected_index_uuid_present = false;
  std::string expected_index_uuid;
  bool expected_provider_uuid_present = false;
  std::string expected_provider_uuid;
  bool expected_base_generation_present = false;
  u64 expected_base_generation = 0;
  bool expected_provider_generation_present = false;
  u64 expected_provider_generation = 0;
  bool expected_opclass_epoch_present = false;
  u64 expected_opclass_epoch = 0;
  bool expected_resource_epoch_present = false;
  u64 expected_resource_epoch = 0;
  GinExactRecheckProof recheck_proof;
};

struct GinPhysicalOpenResult {
  Status status;
  DiagnosticRecord diagnostic;
  GinPhysicalOpenClass open_class = GinPhysicalOpenClass::refused;
  GinPhysicalProvider provider;
  bool fail_closed = true;
  bool restricted_repair_required = false;
  std::vector<std::string> actions;

  bool ok() const {
    return status.ok() && open_class == GinPhysicalOpenClass::current &&
           !fail_closed;
  }
};

struct GinTriConsistentRequest {
  GinPhysicalProvider provider;
  GinQueryStrategy strategy = GinQueryStrategy::contains_all;
  std::vector<std::string> query_keys;
  GinExactRecheckProof recheck_proof;
  bool opclass_epoch_current = true;
  bool resource_epoch_current = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool provider_only_fallback = false;
};

struct GinCandidate {
  TextInvertedRowLocator locator;
  GinTriState tri_state = GinTriState::no;
  u32 matched_key_count = 0;
  bool from_pending_list = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool final_row_admitted = false;
  std::string source_recheck_evidence_ref;
};

struct GinTriConsistentResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;
  std::vector<GinCandidate> candidates;
  u64 posting_list_probe_count = 0;
  u64 posting_tree_probe_count = 0;
  u64 pending_list_probe_count = 0;
  bool tri_consistent_executor_used = false;
  bool candidate_rows_only = true;
  bool final_rows_authorized = false;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool exact_source_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct GinPhysicalMutation {
  GinMutationKind kind = GinMutationKind::insert_row;
  bool before_row_present = false;
  GinSourceRow before_row;
  bool after_row_present = false;
  GinSourceRow after_row;
  GinExactRecheckProof recheck_proof;
  GinOpclassExtractor extractor;
};

struct GinPhysicalMutationResult {
  Status status;
  DiagnosticRecord diagnostic;
  GinPhysicalProvider provider;
  bool applied = false;
  bool pending_flushed = false;
  bool fail_closed = true;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && applied && !fail_closed; }
};

GinPhysicalBuildResult BuildGinPhysicalProvider(
    const GinPhysicalBuildRequest& request);
GinPhysicalSerializeResult SerializeGinPhysicalProvider(
    const GinPhysicalProvider& provider);
GinPhysicalOpenResult OpenGinPhysicalProvider(
    const GinPhysicalOpenRequest& request);
GinTriConsistentResult ExecuteGinTriConsistent(
    const GinTriConsistentRequest& request);
GinPhysicalMutationResult ApplyGinPhysicalMutation(
    const GinPhysicalProvider& provider,
    const GinPhysicalMutation& mutation);

const char* GinPhysicalOpenClassName(GinPhysicalOpenClass open_class);
const char* GinTriStateName(GinTriState state);
DiagnosticRecord MakeGinPhysicalProviderDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::core::index
