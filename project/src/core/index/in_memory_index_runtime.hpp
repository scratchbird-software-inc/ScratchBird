// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "candidate_set.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kInMemoryIndexRuntimeSearchKey =
    "SB_IN_MEMORY_INDEX_RUNTIME";
inline constexpr const char* kInMemoryIndexRuntimeArtifactKind =
    "in_memory_index_runtime";

enum class InMemoryIndexOpenClass : u32 {
  current = 1,
  stale_runtime_epoch = 2,
  missing_recheck_proof = 3,
  memory_quota_denied = 4,
  corrupt_cold_source = 5,
  descriptor_scan_refused = 6,
  behavior_scan_refused = 7,
  unsafe_fallback_refused = 8,
  authority_claim_refused = 9,
  dropped = 10,
  refused = 11
};

enum class InMemoryIndexLookupMode : u32 {
  point = 1,
  range = 2,
  prefix = 3
};

enum class InMemoryIndexMutationKind : u32 {
  insert_entry = 1,
  update_entry = 2,
  delete_entry = 3
};

struct InMemoryIndexRuntimeOptions {
  u64 runtime_epoch = 1;
  u64 memory_quota_bytes = 0;
  std::string relation_uuid;
  std::string index_uuid;
};

struct InMemoryIndexAuthorityProof {
  bool proof_supplied = false;
  bool exact_source_recheck_required = true;
  bool exact_source_available = false;
  bool mga_visibility_recheck_required = true;
  bool mga_visibility_recheck_available = false;
  bool security_recheck_required = true;
  bool security_context_bound = false;
  bool snapshot_proof_supplied = false;
  bool snapshot_still_valid = false;
  u64 runtime_epoch = 0;
  u64 mga_snapshot_epoch = 0;
  u64 catalog_snapshot_epoch = 0;
  u64 security_snapshot_epoch = 0;
  std::string evidence_token;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
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
  bool unsafe_mutable_shared_read = false;
};

struct InMemoryIndexEntry {
  std::string key;
  std::string payload;
  u64 row_ordinal = 0;
  std::string exact_source_token;
};

struct InMemoryIndexColdSourceDescriptor {
  std::string relation_uuid;
  std::string index_uuid;
  u64 descriptor_epoch = 0;
  u64 persisted_generation = 0;
  bool cold_source_supplied = false;
  bool deterministic_order = false;
  bool candidate_entries_only = true;
  bool exact_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool descriptor_store_scan = false;
  bool behavior_store_scan = false;
  bool contract_only_fallback = false;
  bool lifecycle_only_fallback = false;
  bool provider_only_fallback = false;
  bool parser_finality_authority_claimed = false;
  bool donor_finality_authority_claimed = false;
  bool provider_finality_authority_claimed = false;
  bool index_finality_authority_claimed = false;
  bool transaction_finality_authority_claimed = false;
  bool recovery_finality_authority_claimed = false;
  bool visibility_authority_claimed = false;
  bool security_authority_claimed = false;
  bool log_finality_authority_claimed = false;
  std::vector<InMemoryIndexEntry> entries;
};

struct InMemoryIndexLookupRequest {
  InMemoryIndexLookupMode mode = InMemoryIndexLookupMode::point;
  std::string key;
  std::string lower_key;
  std::string upper_key;
  std::string prefix;
  bool lower_inclusive = true;
  bool upper_inclusive = true;
  u64 runtime_epoch = 0;
  InMemoryIndexAuthorityProof proof;
};

struct InMemoryIndexMutation {
  InMemoryIndexMutationKind kind = InMemoryIndexMutationKind::insert_entry;
  InMemoryIndexEntry entry;
  std::string replacement_payload;
  std::string replacement_exact_source_token;
  u64 runtime_epoch = 0;
  InMemoryIndexAuthorityProof proof;
};

struct InMemoryIndexGeneration {
  u64 runtime_epoch = 0;
  u64 generation_id = 0;
  u64 source_descriptor_epoch = 0;
  u64 persisted_generation = 0;
  u64 estimated_bytes = 0;
  std::map<std::string, std::vector<InMemoryIndexEntry>> entries_by_key;
  std::vector<std::string> evidence;
};

struct InMemoryIndexRuntimeState {
  InMemoryIndexRuntimeState() = default;
  InMemoryIndexRuntimeState(const InMemoryIndexRuntimeState&) = delete;
  InMemoryIndexRuntimeState& operator=(const InMemoryIndexRuntimeState&) =
      delete;
  InMemoryIndexRuntimeState(InMemoryIndexRuntimeState&& other) noexcept
      : options(std::move(other.options)),
        current_memory_bytes(other.current_memory_bytes),
        peak_memory_bytes(other.peak_memory_bytes),
        total_denied_bytes(other.total_denied_bytes),
        total_rebuilds(other.total_rebuilds),
        total_mutations(other.total_mutations),
        dropped(other.dropped),
        generation(std::move(other.generation)),
        evidence(std::move(other.evidence)) {}
  InMemoryIndexRuntimeState& operator=(
      InMemoryIndexRuntimeState&& other) noexcept {
    if (this != &other) {
      options = std::move(other.options);
      current_memory_bytes = other.current_memory_bytes;
      peak_memory_bytes = other.peak_memory_bytes;
      total_denied_bytes = other.total_denied_bytes;
      total_rebuilds = other.total_rebuilds;
      total_mutations = other.total_mutations;
      dropped = other.dropped;
      generation = std::move(other.generation);
      evidence = std::move(other.evidence);
    }
    return *this;
  }

  InMemoryIndexRuntimeOptions options;
  u64 current_memory_bytes = 0;
  u64 peak_memory_bytes = 0;
  u64 total_denied_bytes = 0;
  u64 total_rebuilds = 0;
  u64 total_mutations = 0;
  bool dropped = false;
  std::shared_ptr<const InMemoryIndexGeneration> generation;
  mutable std::mutex publish_mutex;
  std::vector<std::string> evidence;
};

struct InMemoryIndexResult {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  InMemoryIndexOpenClass open_class = InMemoryIndexOpenClass::refused;
  std::shared_ptr<const InMemoryIndexGeneration> generation;
  std::vector<InMemoryIndexEntry> candidates;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct InMemoryIndexSupportBundle {
  Status status;
  DiagnosticRecord diagnostic;
  bool fail_closed = false;
  bool dropped = false;
  u64 runtime_epoch = 0;
  u64 generation_id = 0;
  u64 current_memory_bytes = 0;
  u64 peak_memory_bytes = 0;
  u64 total_denied_bytes = 0;
  u64 entry_count = 0;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

InMemoryIndexRuntimeState CreateInMemoryIndexRuntime(
    InMemoryIndexRuntimeOptions options);

const char* InMemoryIndexOpenClassName(InMemoryIndexOpenClass open_class);
const char* InMemoryIndexLookupModeName(InMemoryIndexLookupMode mode);
const char* InMemoryIndexMutationKindName(InMemoryIndexMutationKind kind);

InMemoryIndexResult RebuildInMemoryIndexFromColdSource(
    InMemoryIndexRuntimeState* runtime,
    InMemoryIndexColdSourceDescriptor descriptor,
    const InMemoryIndexAuthorityProof& proof);

InMemoryIndexResult LookupInMemoryIndex(
    const InMemoryIndexRuntimeState* runtime,
    const InMemoryIndexLookupRequest& request);

InMemoryIndexResult ApplyInMemoryIndexMutation(
    InMemoryIndexRuntimeState* runtime,
    const InMemoryIndexMutation& mutation);

InMemoryIndexSupportBundle BuildInMemoryIndexSupportBundle(
    const InMemoryIndexRuntimeState* runtime);

InMemoryIndexSupportBundle DropInMemoryIndexRuntime(
    InMemoryIndexRuntimeState* runtime);

DiagnosticRecord MakeInMemoryIndexRuntimeDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::index
