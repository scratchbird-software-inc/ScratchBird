// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// ODFR_SNAPSHOT_SAFE_CANDIDATE_RESULT_CACHE
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

constexpr const char* kSnapshotSafeCandidateResultCacheSearchKey =
    "ODFR_SNAPSHOT_SAFE_CANDIDATE_RESULT_CACHE";

enum class SnapshotSafeCachePayloadKind {
  kCandidateSet,
  kSmallFinalResult
};

enum class SnapshotSafeCacheAction {
  kStore,
  kHit,
  kMissRecompute,
  kInvalidateRecompute,
  kDisabledRecompute,
  kRefuse
};

struct SnapshotSafeCacheKey {
  std::string normalized_operation;
  std::string safe_parameter_digest;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t statistics_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::string mga_visibility_snapshot_class;
  std::uint64_t provider_generation = 0;
  std::string descriptor_identity_digest;
  std::uint64_t descriptor_epoch = 0;
  std::string result_contract_identity;
  std::string result_contract_hash;
  std::string route_compatibility;
  std::string dialect_compatibility;
};

struct SnapshotSafeCacheEntry {
  SnapshotSafeCacheKey key;
  SnapshotSafeCachePayloadKind payload_kind =
      SnapshotSafeCachePayloadKind::kCandidateSet;
  std::uint64_t row_count = 0;
  std::string cached_result_digest;
  std::string cached_mga_security_digest;
};

struct SnapshotSafeCacheStoreRequest {
  bool cache_enabled = true;
  SnapshotSafeCacheEntry entry;
  bool read_only_operation = true;
  bool candidate_set_snapshot_safe = false;
  bool small_final_result = false;
  std::uint64_t max_small_result_rows = 1024;
  bool dml_uncertain = false;
  bool ddl_uncertain = false;
  bool security_uncertain = false;
  bool redaction_uncertain = false;
  bool statistics_uncertain = false;
  bool provider_generation_uncertain = false;
  bool result_contract_uncertain = false;
  bool provider_generation_mutable = false;
  bool route_uncertain = false;
  bool route_mismatch = false;
  bool dialect_uncertain = false;
  bool dialect_mismatch = false;
  bool visibility_uncertain = false;
  bool volatile_function_dependency = false;
  bool uncommitted_own_transaction_visibility_dependency = false;
  bool negative_cache_entry = false;
  bool negative_cache_snapshot_safe_proven = false;
  bool storage_authority_cached = false;
  bool authorization_authority_cached = false;
  bool visibility_authority_cached = false;
  bool transaction_finality_authority_cached = false;
  bool recovery_authority_cached = false;
  bool parser_execution_authority_cached = false;
  bool donor_behavior_authority_cached = false;
  bool durability_log_authority_cached = false;
};

struct SnapshotSafeCacheLookupRequest {
  bool cache_enabled = true;
  SnapshotSafeCacheKey key;
  SnapshotSafeCachePayloadKind payload_kind =
      SnapshotSafeCachePayloadKind::kCandidateSet;
  bool read_only_operation = true;
  bool candidate_set_snapshot_safe = false;
  bool small_final_result = false;
  std::uint64_t row_count = 0;
  std::uint64_t max_small_result_rows = 1024;
  std::string recomputed_result_digest;
  std::string recomputed_mga_security_digest;
  bool ordinary_recompute_available = true;
  bool dml_uncertain = false;
  bool ddl_uncertain = false;
  bool security_uncertain = false;
  bool redaction_uncertain = false;
  bool statistics_uncertain = false;
  bool provider_generation_uncertain = false;
  bool result_contract_uncertain = false;
  bool provider_generation_mutable = false;
  bool route_uncertain = false;
  bool route_mismatch = false;
  bool dialect_uncertain = false;
  bool dialect_mismatch = false;
  bool visibility_uncertain = false;
  bool volatile_function_dependency = false;
  bool uncommitted_own_transaction_visibility_dependency = false;
  bool negative_cache_entry = false;
  bool negative_cache_snapshot_safe_proven = false;
  bool storage_authority_cached = false;
  bool authorization_authority_cached = false;
  bool visibility_authority_cached = false;
  bool transaction_finality_authority_cached = false;
  bool recovery_authority_cached = false;
  bool parser_execution_authority_cached = false;
  bool donor_behavior_authority_cached = false;
  bool durability_log_authority_cached = false;
};

struct SnapshotSafeCacheDecision {
  bool accepted = false;
  bool fail_closed = true;
  bool cache_hit = false;
  SnapshotSafeCacheAction action = SnapshotSafeCacheAction::kRefuse;
  std::string cache_key_text;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::vector<std::string> evidence;
};

class SnapshotSafeResultCache {
 public:
  SnapshotSafeCacheDecision Store(
      const SnapshotSafeCacheStoreRequest& request);
  SnapshotSafeCacheDecision Lookup(
      const SnapshotSafeCacheLookupRequest& request) const;
  void Clear();
  std::size_t Size() const;

 private:
  std::map<std::string, SnapshotSafeCacheEntry> entries_;
};

const char* SnapshotSafeCachePayloadKindName(
    SnapshotSafeCachePayloadKind kind);
const char* SnapshotSafeCacheActionName(SnapshotSafeCacheAction action);
std::string SnapshotSafeCacheKeyText(const SnapshotSafeCacheKey& key);

}  // namespace scratchbird::engine::executor
