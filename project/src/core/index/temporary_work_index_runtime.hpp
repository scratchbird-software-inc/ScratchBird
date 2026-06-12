// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "candidate_set.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kTemporaryWorkIndexRuntimeSearchKey =
    "SB_TEMPORARY_WORK_INDEX_RUNTIME";
inline constexpr const char* kTemporaryWorkIndexRuntimeArtifactKind =
    "temporary_work_index_runtime";
inline constexpr u32 kTemporaryWorkIndexRuntimeFormatVersion = 1;

enum class TemporaryWorkFamily : u32 {
  sort_run = 1,
  hash_join_build_table = 2,
  temporary_bitmap_candidate_set = 3,
  bulk_sort_buffer = 4
};

enum class TemporaryWorkOpenClass : u32 {
  current = 1,
  stale_runtime_generation = 2,
  corrupt_spill_payload = 3,
  missing_recheck_proof = 4,
  unsafe_fallback_refused = 5,
  authority_claim_refused = 6,
  memory_grant_denied = 7,
  cleaned_artifact = 8,
  cancelled = 9,
  refused = 10
};

struct TemporaryWorkRuntimeOptions {
  std::filesystem::path spill_directory;
  u64 runtime_generation = 1;
  u64 memory_quota_bytes = 0;
  std::string artifact_prefix = "scratchbird_temporary_work";
};

struct TemporaryWorkAuthorityProof {
  bool proof_supplied = false;
  bool exact_recheck_required = true;
  bool exact_recheck_available = false;
  bool mga_visibility_recheck_required = true;
  bool mga_visibility_recheck_available = false;
  bool security_recheck_required = true;
  bool security_context_bound = false;
  bool parser_finality_authority_claimed = false;
  bool reference_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
  bool recovery_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool log_finality_authority_claimed = false;
  bool contract_only_fallback = false;
  bool lifecycle_only_fallback = false;
  bool provider_only_fallback = false;
  bool unsafe_materialized_final_rows = false;
  std::string evidence_ref;
};

struct TemporaryWorkRecord {
  std::string key;
  std::string payload;
  u64 row_ordinal = 0;
};

struct TemporaryHashBuildRow {
  std::string key;
  std::string payload;
  u64 row_ordinal = 0;
};

struct TemporaryBulkSortBufferEntry {
  std::string key;
  std::string payload;
  u64 sequence = 0;
};

struct TemporaryWorkArtifactDescriptor {
  std::string artifact_id;
  TemporaryWorkFamily family = TemporaryWorkFamily::sort_run;
  u64 runtime_generation = 0;
  u64 artifact_ordinal = 0;
  u64 row_count = 0;
  u64 memory_grant_bytes = 0;
  bool spilled = false;
  bool candidate_rows_only = true;
  bool exact_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  std::filesystem::path path;
  std::vector<byte> artifact;
  std::vector<std::string> evidence;
};

struct TemporaryWorkRuntimeState {
  TemporaryWorkRuntimeOptions options;
  u64 live_granted_bytes = 0;
  u64 peak_granted_bytes = 0;
  u64 total_granted_bytes = 0;
  u64 total_denied_bytes = 0;
  u64 next_artifact_ordinal = 1;
  bool cancelled = false;
  std::vector<TemporaryWorkArtifactDescriptor> active_artifacts;
  std::vector<std::string> cleaned_artifact_ids;
  std::vector<std::string> evidence;
};

struct TemporaryWorkResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  TemporaryWorkOpenClass open_class = TemporaryWorkOpenClass::refused;
  TemporaryWorkArtifactDescriptor descriptor;
  std::vector<TemporaryWorkRecord> sorted_rows;
  std::vector<TemporaryHashBuildRow> hash_build_rows;
  CandidateSet bitmap_candidate_set;
  std::vector<TemporaryBulkSortBufferEntry> bulk_sort_buffer;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct TemporaryWorkCleanupResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  bool cleaned = false;
  u64 released_grant_bytes = 0;
  std::vector<std::string> cleaned_artifact_ids;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

TemporaryWorkRuntimeState CreateTemporaryWorkRuntime(
    TemporaryWorkRuntimeOptions options);

const char* TemporaryWorkFamilyName(TemporaryWorkFamily family);
const char* TemporaryWorkOpenClassName(TemporaryWorkOpenClass open_class);

TemporaryWorkResult BuildTemporarySortRun(
    TemporaryWorkRuntimeState* runtime,
    std::vector<TemporaryWorkRecord> rows,
    const TemporaryWorkAuthorityProof& proof,
    bool spill_allowed = true);

TemporaryWorkResult BuildTemporaryHashJoinTable(
    TemporaryWorkRuntimeState* runtime,
    std::vector<TemporaryHashBuildRow> rows,
    const TemporaryWorkAuthorityProof& proof,
    bool spill_allowed = true);

TemporaryWorkResult BuildTemporaryBitmapCandidateSet(
    TemporaryWorkRuntimeState* runtime,
    CandidateSet candidate_set,
    const TemporaryWorkAuthorityProof& proof,
    bool spill_allowed = true);

TemporaryWorkResult BuildTemporaryBulkSortBuffer(
    TemporaryWorkRuntimeState* runtime,
    std::vector<TemporaryBulkSortBufferEntry> entries,
    const TemporaryWorkAuthorityProof& proof,
    bool spill_allowed = true);

TemporaryWorkResult OpenTemporaryWorkArtifact(
    TemporaryWorkRuntimeState* runtime,
    const TemporaryWorkArtifactDescriptor& descriptor,
    TemporaryWorkFamily expected_family,
    const TemporaryWorkAuthorityProof& proof);

TemporaryWorkCleanupResult CleanupTemporaryWorkArtifact(
    TemporaryWorkRuntimeState* runtime,
    const std::string& artifact_id);

TemporaryWorkCleanupResult CancelTemporaryWorkRuntime(
    TemporaryWorkRuntimeState* runtime);

DiagnosticRecord MakeTemporaryWorkIndexRuntimeDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
