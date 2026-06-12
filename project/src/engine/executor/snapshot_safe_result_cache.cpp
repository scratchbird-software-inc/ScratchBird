// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "snapshot_safe_result_cache.hpp"

#include <sstream>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

void AddBool(std::vector<std::string>* evidence,
             const std::string& key,
             bool value) {
  Add(evidence, key + "=" + (value ? "true" : "false"));
}

bool KeyComplete(const SnapshotSafeCacheKey& key) {
  return !key.normalized_operation.empty() &&
         !key.safe_parameter_digest.empty() &&
         key.catalog_epoch != 0 &&
         key.statistics_epoch != 0 &&
         key.security_epoch != 0 &&
         key.redaction_epoch != 0 &&
         !key.mga_visibility_snapshot_class.empty() &&
         key.provider_generation != 0 &&
         !key.descriptor_identity_digest.empty() &&
         key.descriptor_epoch != 0 &&
         !key.result_contract_identity.empty() &&
         !key.result_contract_hash.empty() &&
         !key.route_compatibility.empty() &&
         !key.dialect_compatibility.empty();
}

bool AnyUncertainty(const SnapshotSafeCacheStoreRequest& request) {
  return request.dml_uncertain || request.ddl_uncertain ||
         request.security_uncertain || request.redaction_uncertain ||
         request.statistics_uncertain ||
         request.provider_generation_uncertain || request.route_uncertain ||
         request.dialect_uncertain || request.visibility_uncertain;
}

bool AnyUncertainty(const SnapshotSafeCacheLookupRequest& request) {
  return request.dml_uncertain || request.ddl_uncertain ||
         request.security_uncertain || request.redaction_uncertain ||
         request.statistics_uncertain ||
         request.provider_generation_uncertain || request.route_uncertain ||
         request.dialect_uncertain || request.visibility_uncertain;
}

bool HardRefusal(const SnapshotSafeCacheStoreRequest& request,
                 std::string* code,
                 std::string* detail) {
  if (request.result_contract_uncertain) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.RESULT_CONTRACT_UNCERTAIN";
    *detail = "result contract identity and hash must be proven before caching";
    return true;
  }
  if (request.provider_generation_mutable) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.PROVIDER_GENERATION_MUTABLE";
    *detail = "mutable provider generations cannot back snapshot cache entries";
    return true;
  }
  if (request.route_mismatch) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.ROUTE_MISMATCH";
    *detail = "request route does not match cache key route compatibility";
    return true;
  }
  if (request.dialect_mismatch) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.DIALECT_MISMATCH";
    *detail = "request dialect does not match cache key dialect compatibility";
    return true;
  }
  if (request.volatile_function_dependency) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.VOLATILE_FUNCTION_REFUSED";
    *detail = "volatile function dependencies cannot be result-cached";
    return true;
  }
  if (request.uncommitted_own_transaction_visibility_dependency) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.OWN_TRANSACTION_VISIBILITY_REFUSED";
    *detail = "uncommitted own-transaction visibility dependency is not snapshot-safe";
    return true;
  }
  if (request.negative_cache_entry) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.NEGATIVE_CACHE_REFUSED";
    *detail = "negative caching is refused until all MGA and security dimensions are proven snapshot-safe";
    return true;
  }
  return false;
}

bool HardRefusal(const SnapshotSafeCacheLookupRequest& request,
                 std::string* code,
                 std::string* detail) {
  if (request.result_contract_uncertain) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.RESULT_CONTRACT_UNCERTAIN";
    *detail = "result contract identity and hash must be proven before lookup";
    return true;
  }
  if (request.provider_generation_mutable) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.PROVIDER_GENERATION_MUTABLE";
    *detail = "mutable provider generations cannot back snapshot cache entries";
    return true;
  }
  if (request.route_mismatch) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.ROUTE_MISMATCH";
    *detail = "request route does not match cache key route compatibility";
    return true;
  }
  if (request.dialect_mismatch) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.DIALECT_MISMATCH";
    *detail = "request dialect does not match cache key dialect compatibility";
    return true;
  }
  if (request.volatile_function_dependency) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.VOLATILE_FUNCTION_REFUSED";
    *detail = "volatile function dependencies cannot be result-cached";
    return true;
  }
  if (request.uncommitted_own_transaction_visibility_dependency) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.OWN_TRANSACTION_VISIBILITY_REFUSED";
    *detail = "uncommitted own-transaction visibility dependency is not snapshot-safe";
    return true;
  }
  if (request.negative_cache_entry) {
    *code = "EXECUTOR.SNAPSHOT_RESULT_CACHE.NEGATIVE_CACHE_REFUSED";
    *detail = "negative caching is refused until all MGA and security dimensions are proven snapshot-safe";
    return true;
  }
  return false;
}

bool AnyAuthorityCached(const SnapshotSafeCacheStoreRequest& request) {
  return request.storage_authority_cached ||
         request.authorization_authority_cached ||
         request.visibility_authority_cached ||
         request.transaction_finality_authority_cached ||
         request.recovery_authority_cached ||
         request.parser_execution_authority_cached ||
         request.donor_behavior_authority_cached ||
         request.durability_log_authority_cached;
}

bool AnyAuthorityCached(const SnapshotSafeCacheLookupRequest& request) {
  return request.storage_authority_cached ||
         request.authorization_authority_cached ||
         request.visibility_authority_cached ||
         request.transaction_finality_authority_cached ||
         request.recovery_authority_cached ||
         request.parser_execution_authority_cached ||
         request.donor_behavior_authority_cached ||
         request.durability_log_authority_cached;
}

bool Eligible(SnapshotSafeCachePayloadKind kind,
              bool candidate_set_snapshot_safe,
              bool small_final_result,
              std::uint64_t row_count,
              std::uint64_t max_small_result_rows) {
  if (kind == SnapshotSafeCachePayloadKind::kCandidateSet) {
    return candidate_set_snapshot_safe;
  }
  return small_final_result && row_count <= max_small_result_rows;
}

SnapshotSafeCacheDecision BaseDecision(const SnapshotSafeCacheKey& key,
                                       SnapshotSafeCachePayloadKind kind) {
  SnapshotSafeCacheDecision decision;
  decision.cache_key_text = std::string("payload=") +
                            SnapshotSafeCachePayloadKindName(kind) + "|" +
                            SnapshotSafeCacheKeyText(key);
  Add(&decision.evidence, kSnapshotSafeCandidateResultCacheSearchKey);
  Add(&decision.evidence, "snapshot_cache_payload_kind=" +
                              std::string(SnapshotSafeCachePayloadKindName(kind)));
  Add(&decision.evidence, "snapshot_cache_key=" + decision.cache_key_text);
  Add(&decision.evidence,
      "normalized_operation=" + key.normalized_operation);
  Add(&decision.evidence,
      "safe_parameter_digest=" + key.safe_parameter_digest);
  Add(&decision.evidence,
      "catalog_epoch=" + std::to_string(key.catalog_epoch));
  Add(&decision.evidence,
      "statistics_epoch=" + std::to_string(key.statistics_epoch));
  Add(&decision.evidence,
      "security_epoch=" + std::to_string(key.security_epoch));
  Add(&decision.evidence,
      "redaction_epoch=" + std::to_string(key.redaction_epoch));
  Add(&decision.evidence,
      "mga_visibility_snapshot_class=" +
          key.mga_visibility_snapshot_class);
  Add(&decision.evidence,
      "provider_generation=" + std::to_string(key.provider_generation));
  Add(&decision.evidence,
      "descriptor_identity_digest=" + key.descriptor_identity_digest);
  Add(&decision.evidence,
      "descriptor_epoch=" + std::to_string(key.descriptor_epoch));
  Add(&decision.evidence,
      "result_contract_identity=" + key.result_contract_identity);
  Add(&decision.evidence,
      "result_contract_hash=" + key.result_contract_hash);
  Add(&decision.evidence,
      "route_compatibility=" + key.route_compatibility);
  Add(&decision.evidence,
      "dialect_compatibility=" + key.dialect_compatibility);
  Add(&decision.evidence, "cache_storage_authority=false");
  Add(&decision.evidence, "cache_authorization_authority=false");
  Add(&decision.evidence, "cache_visibility_authority=false");
  Add(&decision.evidence, "cache_transaction_finality_authority=false");
  Add(&decision.evidence, "cache_recovery_authority=false");
  Add(&decision.evidence, "cache_parser_execution_authority=false");
  Add(&decision.evidence, "cache_donor_behavior_authority=false");
  Add(&decision.evidence, "cache_durability_log_authority=false");
  Add(&decision.evidence, "support_bundle_ready=true");
  return decision;
}

SnapshotSafeCacheDecision Finish(SnapshotSafeCacheDecision decision,
                                 SnapshotSafeCacheAction action,
                                 std::string diagnostic_code,
                                 std::string diagnostic_detail,
                                 bool accepted,
                                 bool fail_closed,
                                 bool cache_hit) {
  decision.action = action;
  decision.diagnostic_code = std::move(diagnostic_code);
  decision.diagnostic_detail = std::move(diagnostic_detail);
  decision.accepted = accepted;
  decision.fail_closed = fail_closed;
  decision.cache_hit = cache_hit;
  Add(&decision.evidence,
      "snapshot_cache_action=" +
          std::string(SnapshotSafeCacheActionName(action)));
  Add(&decision.evidence,
      "snapshot_cache_diagnostic=" + decision.diagnostic_code);
  if (!decision.diagnostic_detail.empty()) {
    Add(&decision.evidence,
        "snapshot_cache_detail=" + decision.diagnostic_detail);
  }
  AddBool(&decision.evidence, "snapshot_cache_hit", cache_hit);
  AddBool(&decision.evidence, "fail_closed", fail_closed);
  return decision;
}

SnapshotSafeCacheDecision Refuse(SnapshotSafeCacheDecision decision,
                                 std::string diagnostic_code,
                                 std::string detail) {
  return Finish(std::move(decision),
                SnapshotSafeCacheAction::kRefuse,
                std::move(diagnostic_code),
                std::move(detail),
                false,
                true,
                false);
}

void AddStoreBooleans(SnapshotSafeCacheDecision* decision,
                      const SnapshotSafeCacheStoreRequest& request) {
  AddBool(&decision->evidence, "executor_snapshot_result_cache_enabled",
          request.cache_enabled);
  AddBool(&decision->evidence, "read_only_operation",
          request.read_only_operation);
  AddBool(&decision->evidence, "candidate_set_snapshot_safe",
          request.candidate_set_snapshot_safe);
  AddBool(&decision->evidence, "small_final_result",
          request.small_final_result);
  AddBool(&decision->evidence, "negative_cache_entry",
          request.negative_cache_entry);
  AddBool(&decision->evidence, "negative_cache_snapshot_safe_proven",
          request.negative_cache_snapshot_safe_proven);
}

void AddLookupBooleans(SnapshotSafeCacheDecision* decision,
                       const SnapshotSafeCacheLookupRequest& request) {
  AddBool(&decision->evidence, "executor_snapshot_result_cache_enabled",
          request.cache_enabled);
  AddBool(&decision->evidence, "read_only_operation",
          request.read_only_operation);
  AddBool(&decision->evidence, "candidate_set_snapshot_safe",
          request.candidate_set_snapshot_safe);
  AddBool(&decision->evidence, "small_final_result",
          request.small_final_result);
  AddBool(&decision->evidence, "ordinary_recompute_available",
          request.ordinary_recompute_available);
  AddBool(&decision->evidence, "negative_cache_entry",
          request.negative_cache_entry);
  AddBool(&decision->evidence, "negative_cache_snapshot_safe_proven",
          request.negative_cache_snapshot_safe_proven);
}

}  // namespace

const char* SnapshotSafeCachePayloadKindName(
    SnapshotSafeCachePayloadKind kind) {
  switch (kind) {
    case SnapshotSafeCachePayloadKind::kCandidateSet:
      return "candidate_set";
    case SnapshotSafeCachePayloadKind::kSmallFinalResult:
      return "small_final_result";
  }
  return "candidate_set";
}

const char* SnapshotSafeCacheActionName(SnapshotSafeCacheAction action) {
  switch (action) {
    case SnapshotSafeCacheAction::kStore:
      return "store";
    case SnapshotSafeCacheAction::kHit:
      return "hit";
    case SnapshotSafeCacheAction::kMissRecompute:
      return "miss_recompute";
    case SnapshotSafeCacheAction::kInvalidateRecompute:
      return "invalidate_recompute";
    case SnapshotSafeCacheAction::kDisabledRecompute:
      return "disabled_recompute";
    case SnapshotSafeCacheAction::kRefuse:
      return "refuse";
  }
  return "refuse";
}

std::string SnapshotSafeCacheKeyText(const SnapshotSafeCacheKey& key) {
  std::ostringstream out;
  out << "op=" << key.normalized_operation
      << "|params=" << key.safe_parameter_digest
      << "|catalog=" << key.catalog_epoch
      << "|stats=" << key.statistics_epoch
      << "|security=" << key.security_epoch
      << "|redaction=" << key.redaction_epoch
      << "|mga_snapshot=" << key.mga_visibility_snapshot_class
      << "|provider_generation=" << key.provider_generation
      << "|descriptor_identity=" << key.descriptor_identity_digest
      << "|descriptor_epoch=" << key.descriptor_epoch
      << "|result_contract=" << key.result_contract_identity
      << "|result_contract_hash=" << key.result_contract_hash
      << "|route=" << key.route_compatibility
      << "|dialect=" << key.dialect_compatibility;
  return out.str();
}

SnapshotSafeCacheDecision SnapshotSafeResultCache::Store(
    const SnapshotSafeCacheStoreRequest& request) {
  auto decision = BaseDecision(request.entry.key, request.entry.payload_kind);
  AddStoreBooleans(&decision, request);
  if (!request.cache_enabled) {
    return Finish(std::move(decision),
                  SnapshotSafeCacheAction::kDisabledRecompute,
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.DISABLED_RECOMPUTE",
                  "executor.snapshot_result_cache=off",
                  true,
                  false,
                  false);
  }
  if (!KeyComplete(request.entry.key)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.KEY_INCOMPLETE",
                  "strict cache key dimensions are required");
  }
  if (!request.read_only_operation ||
      !Eligible(request.entry.payload_kind,
                request.candidate_set_snapshot_safe,
                request.small_final_result,
                request.entry.row_count,
                request.max_small_result_rows)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.ELIGIBILITY_REFUSED",
                  "only read-only snapshot-safe candidates or small final results are cacheable");
  }
  std::string hard_code;
  std::string hard_detail;
  if (HardRefusal(request, &hard_code, &hard_detail)) {
    return Refuse(std::move(decision), std::move(hard_code),
                  std::move(hard_detail));
  }
  if (AnyUncertainty(request)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.UNCERTAINTY_REFUSED",
                  "DML DDL security redaction statistics provider route dialect or visibility uncertainty");
  }
  if (AnyAuthorityCached(request)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.AUTHORITY_REFUSED",
                  "cache cannot own storage authorization visibility finality recovery parser donor or durability-log authority");
  }
  if (request.entry.cached_result_digest.empty() ||
      request.entry.cached_mga_security_digest.empty()) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.DIGEST_REQUIRED",
                  "cached result and MGA/security digests are required");
  }
  entries_[decision.cache_key_text] = request.entry;
  Add(&decision.evidence, "snapshot_cache_stored=true");
  return Finish(std::move(decision),
                SnapshotSafeCacheAction::kStore,
                "EXECUTOR.SNAPSHOT_RESULT_CACHE.STORED",
                "snapshot_safe_entry_stored",
                true,
                false,
                false);
}

SnapshotSafeCacheDecision SnapshotSafeResultCache::Lookup(
    const SnapshotSafeCacheLookupRequest& request) const {
  auto decision = BaseDecision(request.key, request.payload_kind);
  AddLookupBooleans(&decision, request);
  if (!request.cache_enabled) {
    Add(&decision.evidence, "snapshot_cache_disabled_recompute=true");
    return Finish(std::move(decision),
                  SnapshotSafeCacheAction::kDisabledRecompute,
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.DISABLED_RECOMPUTE",
                  "executor.snapshot_result_cache=off",
                  true,
                  false,
                  false);
  }
  if (!KeyComplete(request.key)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.KEY_INCOMPLETE",
                  "strict cache key dimensions are required");
  }
  if (!request.read_only_operation ||
      !Eligible(request.payload_kind,
                request.candidate_set_snapshot_safe,
                request.small_final_result,
                request.row_count,
                request.max_small_result_rows)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.ELIGIBILITY_REFUSED",
                  "only read-only snapshot-safe candidates or small final results are cacheable");
  }
  std::string hard_code;
  std::string hard_detail;
  if (HardRefusal(request, &hard_code, &hard_detail)) {
    return Refuse(std::move(decision), std::move(hard_code),
                  std::move(hard_detail));
  }
  if (AnyUncertainty(request)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.UNCERTAINTY_REFUSED",
                  "DML DDL security redaction statistics provider route dialect or visibility uncertainty");
  }
  if (AnyAuthorityCached(request)) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.AUTHORITY_REFUSED",
                  "cache cannot own storage authorization visibility finality recovery parser donor or durability-log authority");
  }
  if (!request.ordinary_recompute_available ||
      request.recomputed_result_digest.empty() ||
      request.recomputed_mga_security_digest.empty()) {
    return Refuse(std::move(decision),
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.RECOMPUTE_PROOF_REQUIRED",
                  "ordinary engine recomputation digest is required before a hit");
  }

  const auto found = entries_.find(decision.cache_key_text);
  if (found == entries_.end()) {
    Add(&decision.evidence, "snapshot_cache_miss_recompute=true");
    return Finish(std::move(decision),
                  SnapshotSafeCacheAction::kMissRecompute,
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.MISS_RECOMPUTE",
                  "cache_entry_missing_recompute_ordinary_engine_path",
                  true,
                  false,
                  false);
  }

  const bool result_match =
      found->second.cached_result_digest == request.recomputed_result_digest;
  const bool mga_security_match =
      found->second.cached_mga_security_digest ==
      request.recomputed_mga_security_digest;
  const bool payload_kind_match =
      found->second.payload_kind == request.payload_kind;
  const bool row_count_match = found->second.row_count == request.row_count;
  AddBool(&decision.evidence, "snapshot_cache_payload_kind_match",
          payload_kind_match);
  AddBool(&decision.evidence, "snapshot_cache_row_count_match",
          row_count_match);
  AddBool(&decision.evidence, "snapshot_cache_recompute_result_match",
          result_match);
  AddBool(&decision.evidence, "snapshot_cache_recompute_mga_security_match",
          mga_security_match);
  if (!payload_kind_match || !row_count_match ||
      !result_match || !mga_security_match) {
    Add(&decision.evidence, "snapshot_cache_invalidated=true");
    return Finish(std::move(decision),
                  SnapshotSafeCacheAction::kInvalidateRecompute,
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.STORED_ENTRY_MISMATCH",
                  "stored_payload_kind_row_count_or_digest_differs_from_request_recompute",
                  true,
                  false,
                  false);
  }
  Add(&decision.evidence, "snapshot_cache_identical_to_recompute=true");
  return Finish(std::move(decision),
                SnapshotSafeCacheAction::kHit,
                "EXECUTOR.SNAPSHOT_RESULT_CACHE.HIT_ACCEPTED",
                "cached_result_identical_to_ordinary_mga_security_recompute",
                true,
                false,
                true);
}

void SnapshotSafeResultCache::Clear() {
  entries_.clear();
}

std::size_t SnapshotSafeResultCache::Size() const {
  return entries_.size();
}

}  // namespace scratchbird::engine::executor
