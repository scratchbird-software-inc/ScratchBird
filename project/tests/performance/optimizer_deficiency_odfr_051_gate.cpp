// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "snapshot_safe_result_cache.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using scratchbird::engine::executor::SnapshotSafeCacheAction;
using scratchbird::engine::executor::SnapshotSafeCacheDecision;
using scratchbird::engine::executor::SnapshotSafeCacheEntry;
using scratchbird::engine::executor::SnapshotSafeCacheKey;
using scratchbird::engine::executor::SnapshotSafeCacheLookupRequest;
using scratchbird::engine::executor::SnapshotSafeCachePayloadKind;
using scratchbird::engine::executor::SnapshotSafeCacheStoreRequest;
using scratchbird::engine::executor::SnapshotSafeResultCache;
using scratchbird::engine::executor::kSnapshotSafeCandidateResultCacheSearchKey;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ODFR-051 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

bool HasEvidencePrefix(const std::vector<std::string>& evidence,
                       const std::string& prefix) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const std::string& value) {
                       return value.rfind(prefix, 0) == 0;
                     });
}

SnapshotSafeCacheKey BaseKey() {
  SnapshotSafeCacheKey key;
  key.normalized_operation = "select document where tenant=? and status=?";
  key.safe_parameter_digest = "safe_params:tenant_hash,status_active";
  key.catalog_epoch = 11;
  key.statistics_epoch = 12;
  key.security_epoch = 13;
  key.redaction_epoch = 14;
  key.mga_visibility_snapshot_class = "repeatable_read_snapshot:42";
  key.provider_generation = 15;
  key.descriptor_identity_digest = "descriptor:candidate-rowset:v1";
  key.descriptor_epoch = 16;
  key.result_contract_identity = "candidate_rowset.v1";
  key.result_contract_hash = "sha256:candidate-rowset-contract";
  key.route_compatibility = "embedded_ipc_v1";
  key.dialect_compatibility = "sbsql_v1";
  return key;
}

SnapshotSafeCacheEntry CandidateEntry() {
  SnapshotSafeCacheEntry entry;
  entry.key = BaseKey();
  entry.payload_kind = SnapshotSafeCachePayloadKind::kCandidateSet;
  entry.row_count = 32;
  entry.cached_result_digest = "candidate_digest:32:ordered";
  entry.cached_mga_security_digest = "mga_security_digest:visible_authorized_32";
  return entry;
}

SnapshotSafeCacheStoreRequest CandidateStoreRequest() {
  SnapshotSafeCacheStoreRequest request;
  request.entry = CandidateEntry();
  request.read_only_operation = true;
  request.candidate_set_snapshot_safe = true;
  request.small_final_result = false;
  return request;
}

SnapshotSafeCacheLookupRequest CandidateLookupRequest() {
  SnapshotSafeCacheLookupRequest request;
  request.key = BaseKey();
  request.payload_kind = SnapshotSafeCachePayloadKind::kCandidateSet;
  request.read_only_operation = true;
  request.candidate_set_snapshot_safe = true;
  request.row_count = 32;
  request.recomputed_result_digest = "candidate_digest:32:ordered";
  request.recomputed_mga_security_digest =
      "mga_security_digest:visible_authorized_32";
  return request;
}

void RequireAuthorityEvidence(const SnapshotSafeCacheDecision& decision) {
  const auto& evidence = decision.evidence;
  Require(HasEvidence(evidence, "cache_storage_authority=false"),
          "cache must not own storage authority");
  Require(HasEvidence(evidence, "cache_authorization_authority=false"),
          "cache must not own authorization authority");
  Require(HasEvidence(evidence, "cache_visibility_authority=false"),
          "cache must not own visibility authority");
  Require(HasEvidence(evidence, "cache_transaction_finality_authority=false"),
          "cache must not own transaction finality authority");
  Require(HasEvidence(evidence, "cache_recovery_authority=false"),
          "cache must not own recovery authority");
  Require(HasEvidence(evidence, "cache_parser_execution_authority=false"),
          "cache must not own parser execution authority");
  Require(HasEvidence(evidence, "cache_reference_behavior_authority=false"),
          "cache must not own reference behavior authority");
  Require(HasEvidence(evidence, "cache_durability_log_authority=false"),
          "cache must not own durability-log authority");
}

void RequireKeyEvidence(const SnapshotSafeCacheDecision& decision) {
  const auto& evidence = decision.evidence;
  Require(HasEvidence(evidence, kSnapshotSafeCandidateResultCacheSearchKey),
          "missing ODFR-051 search key");
  Require(HasEvidencePrefix(evidence, "snapshot_cache_key="),
          "missing strict cache key text");
  Require(HasEvidencePrefix(evidence, "normalized_operation="),
          "missing normalized operation key evidence");
  Require(HasEvidencePrefix(evidence, "safe_parameter_digest="),
          "missing safe parameter digest evidence");
  Require(HasEvidence(evidence, "catalog_epoch=11"),
          "missing catalog epoch evidence");
  Require(HasEvidence(evidence, "statistics_epoch=12"),
          "missing statistics epoch evidence");
  Require(HasEvidence(evidence, "security_epoch=13"),
          "missing security epoch evidence");
  Require(HasEvidence(evidence, "redaction_epoch=14"),
          "missing redaction epoch evidence");
  Require(HasEvidence(evidence,
                      "mga_visibility_snapshot_class=repeatable_read_snapshot:42"),
          "missing MGA visibility snapshot class evidence");
  Require(HasEvidence(evidence, "provider_generation=15"),
          "missing provider generation evidence");
  Require(HasEvidence(evidence, "result_contract_identity=candidate_rowset.v1"),
          "missing result contract identity evidence");
  Require(HasEvidence(evidence,
                      "result_contract_hash=sha256:candidate-rowset-contract"),
          "missing result contract hash evidence");
  Require(HasEvidence(evidence, "route_compatibility=embedded_ipc_v1"),
          "missing route compatibility evidence");
  Require(HasEvidence(evidence, "dialect_compatibility=sbsql_v1"),
          "missing dialect compatibility evidence");
  Require(HasEvidence(evidence, "support_bundle_ready=true"),
          "missing support bundle evidence");
  RequireAuthorityEvidence(decision);
}

void ProveCandidateSetHitRequiresIdenticalRecompute() {
  SnapshotSafeResultCache cache;
  const auto store = cache.Store(CandidateStoreRequest());
  Require(store.accepted && !store.fail_closed,
          "candidate-set store should be accepted");
  Require(store.action == SnapshotSafeCacheAction::kStore,
          "candidate-set store action mismatch");
  RequireKeyEvidence(store);

  const auto hit = cache.Lookup(CandidateLookupRequest());
  Require(hit.accepted && !hit.fail_closed,
          "candidate-set lookup should be accepted");
  Require(hit.cache_hit, "candidate-set lookup should be a hit");
  Require(hit.action == SnapshotSafeCacheAction::kHit,
          "candidate-set hit action mismatch");
  Require(HasEvidence(hit.evidence,
                      "snapshot_cache_identical_to_recompute=true"),
          "candidate hit missing recompute identity proof");
  Require(HasEvidence(hit.evidence,
                      "snapshot_cache_recompute_mga_security_match=true"),
          "candidate hit missing MGA/security recompute proof");
  Require(HasEvidence(hit.evidence,
                      "snapshot_cache_payload_kind_match=true"),
          "candidate hit missing payload kind match evidence");
  Require(HasEvidence(hit.evidence, "snapshot_cache_row_count_match=true"),
          "candidate hit missing row count match evidence");
  RequireKeyEvidence(hit);
}

void ProveSmallFinalResultHit() {
  SnapshotSafeResultCache cache;
  auto entry = CandidateEntry();
  entry.payload_kind = SnapshotSafeCachePayloadKind::kSmallFinalResult;
  entry.row_count = 4;
  entry.cached_result_digest = "small_result_digest:4";
  entry.cached_mga_security_digest = "mga_security_digest:small_visible_4";

  SnapshotSafeCacheStoreRequest store_request;
  store_request.entry = entry;
  store_request.read_only_operation = true;
  store_request.small_final_result = true;
  const auto store = cache.Store(store_request);
  Require(store.accepted && !store.fail_closed,
          "small final result store should be accepted");

  SnapshotSafeCacheLookupRequest lookup;
  lookup.key = entry.key;
  lookup.payload_kind = SnapshotSafeCachePayloadKind::kSmallFinalResult;
  lookup.read_only_operation = true;
  lookup.small_final_result = true;
  lookup.row_count = 4;
  lookup.recomputed_result_digest = "small_result_digest:4";
  lookup.recomputed_mga_security_digest =
      "mga_security_digest:small_visible_4";
  const auto hit = cache.Lookup(lookup);
  Require(hit.accepted && hit.cache_hit && !hit.fail_closed,
          "small final result lookup should hit");
  Require(HasEvidence(hit.evidence,
                      "snapshot_cache_payload_kind=small_final_result"),
          "small result payload evidence missing");
  Require(HasEvidence(hit.evidence,
                      "snapshot_cache_payload_kind_match=true"),
          "small result missing payload kind match evidence");
  Require(HasEvidence(hit.evidence, "snapshot_cache_row_count_match=true"),
          "small result missing row count match evidence");
  RequireKeyEvidence(hit);
}

void ProvePayloadKindsDoNotCollideForSameBaseKey() {
  SnapshotSafeResultCache cache;
  const auto candidate_store = cache.Store(CandidateStoreRequest());
  Require(candidate_store.accepted && !candidate_store.fail_closed,
          "candidate store should be accepted");

  auto small_entry = CandidateEntry();
  small_entry.payload_kind = SnapshotSafeCachePayloadKind::kSmallFinalResult;
  small_entry.row_count = 4;
  small_entry.cached_result_digest = "small_result_digest:4";
  small_entry.cached_mga_security_digest =
      "mga_security_digest:small_visible_4";
  SnapshotSafeCacheStoreRequest small_store;
  small_store.entry = small_entry;
  small_store.read_only_operation = true;
  small_store.small_final_result = true;
  const auto small_store_decision = cache.Store(small_store);
  Require(small_store_decision.accepted && !small_store_decision.fail_closed,
          "small-result store with same base key should be accepted");
  Require(cache.Size() == 2,
          "payload kind must be part of the internal cache key");

  const auto candidate_hit = cache.Lookup(CandidateLookupRequest());
  Require(candidate_hit.accepted && candidate_hit.cache_hit,
          "candidate entry should still hit after small-result store");
  Require(HasEvidence(candidate_hit.evidence,
                      "snapshot_cache_payload_kind=candidate_set"),
          "candidate payload evidence missing after same-key store");

  SnapshotSafeCacheLookupRequest small_lookup;
  small_lookup.key = small_entry.key;
  small_lookup.payload_kind = SnapshotSafeCachePayloadKind::kSmallFinalResult;
  small_lookup.read_only_operation = true;
  small_lookup.small_final_result = true;
  small_lookup.row_count = 4;
  small_lookup.recomputed_result_digest = "small_result_digest:4";
  small_lookup.recomputed_mga_security_digest =
      "mga_security_digest:small_visible_4";
  const auto small_hit = cache.Lookup(small_lookup);
  Require(small_hit.accepted && small_hit.cache_hit,
          "small-result entry should hit independently");
  Require(HasEvidence(small_hit.evidence,
                      "snapshot_cache_payload_kind=small_final_result"),
          "small-result payload evidence missing after same-key store");
}

void ProveRowCountMismatchInvalidatesBeforeHit() {
  SnapshotSafeResultCache cache;
  cache.Store(CandidateStoreRequest());
  auto lookup = CandidateLookupRequest();
  lookup.row_count = 31;
  const auto decision = cache.Lookup(lookup);
  Require(decision.accepted && !decision.fail_closed,
          "row-count mismatch should recompute, not fail closed");
  Require(!decision.cache_hit, "row-count mismatch must not hit");
  Require(decision.action == SnapshotSafeCacheAction::kInvalidateRecompute,
          "row-count mismatch did not invalidate");
  Require(HasEvidence(decision.evidence,
                      "snapshot_cache_payload_kind_match=true"),
          "row-count mismatch should still show payload kind match");
  Require(HasEvidence(decision.evidence,
                      "snapshot_cache_row_count_match=false"),
          "row-count mismatch evidence missing");
}

void ProveMismatchInvalidatesBeforeHit() {
  SnapshotSafeResultCache cache;
  cache.Store(CandidateStoreRequest());
  auto lookup = CandidateLookupRequest();
  lookup.recomputed_mga_security_digest = "mga_security_digest:changed";
  const auto decision = cache.Lookup(lookup);
  Require(decision.accepted && !decision.fail_closed,
          "digest mismatch should recompute, not fail closed");
  Require(!decision.cache_hit, "digest mismatch must not be a hit");
  Require(decision.action == SnapshotSafeCacheAction::kInvalidateRecompute,
          "digest mismatch did not invalidate");
  Require(HasEvidence(decision.evidence,
                      "snapshot_cache_recompute_mga_security_match=false"),
          "missing mismatch evidence");
  Require(HasEvidence(decision.evidence, "snapshot_cache_invalidated=true"),
          "missing invalidation evidence");
}

void ProveStrictUncertaintyAndAuthorityRefusals() {
  SnapshotSafeResultCache cache;
  auto uncertain = CandidateLookupRequest();
  uncertain.security_uncertain = true;
  const auto uncertainty = cache.Lookup(uncertain);
  Require(!uncertainty.accepted && uncertainty.fail_closed,
          "security uncertainty must fail closed");
  Require(uncertainty.action == SnapshotSafeCacheAction::kRefuse,
          "security uncertainty did not refuse");
  Require(uncertainty.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.UNCERTAINTY_REFUSED",
          "uncertainty diagnostic mismatch");

  auto authority = CandidateLookupRequest();
  authority.transaction_finality_authority_cached = true;
  const auto authority_decision = cache.Lookup(authority);
  Require(!authority_decision.accepted && authority_decision.fail_closed,
          "cached finality authority must fail closed");
  Require(authority_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.AUTHORITY_REFUSED",
          "authority diagnostic mismatch");
}

void ProveDisabledPathRecomputes() {
  SnapshotSafeResultCache cache;
  auto lookup = CandidateLookupRequest();
  lookup.cache_enabled = false;
  const auto decision = cache.Lookup(lookup);
  Require(decision.accepted && !decision.fail_closed,
          "disabled cache should recompute through ordinary engine path");
  Require(decision.action == SnapshotSafeCacheAction::kDisabledRecompute,
          "disabled cache action mismatch");
  Require(HasEvidence(decision.evidence,
                      "snapshot_cache_disabled_recompute=true"),
          "disabled recompute evidence missing");
  Require(decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.DISABLED_RECOMPUTE",
          "disabled recompute diagnostic mismatch");
}

void ProveEligibilityAndRecomputeProofRequired() {
  SnapshotSafeResultCache cache;
  auto dml = CandidateLookupRequest();
  dml.read_only_operation = false;
  const auto dml_decision = cache.Lookup(dml);
  Require(!dml_decision.accepted && dml_decision.fail_closed,
          "DML/non-read-only lookup must refuse");
  Require(dml_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.ELIGIBILITY_REFUSED",
          "DML eligibility diagnostic mismatch");

  auto large = CandidateLookupRequest();
  large.payload_kind = SnapshotSafeCachePayloadKind::kSmallFinalResult;
  large.candidate_set_snapshot_safe = false;
  large.small_final_result = true;
  large.row_count = 4096;
  large.max_small_result_rows = 16;
  const auto large_decision = cache.Lookup(large);
  Require(!large_decision.accepted && large_decision.fail_closed,
          "large final result must not be cache-eligible");

  cache.Store(CandidateStoreRequest());
  auto missing_recompute = CandidateLookupRequest();
  missing_recompute.recomputed_result_digest.clear();
  const auto missing = cache.Lookup(missing_recompute);
  Require(!missing.accepted && missing.fail_closed,
          "missing recompute proof must refuse");
  Require(missing.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.RECOMPUTE_PROOF_REQUIRED",
          "missing recompute proof diagnostic mismatch");
}

}  // namespace

int main() {
  ProveCandidateSetHitRequiresIdenticalRecompute();
  ProveSmallFinalResultHit();
  ProvePayloadKindsDoNotCollideForSameBaseKey();
  ProveRowCountMismatchInvalidatesBeforeHit();
  ProveMismatchInvalidatesBeforeHit();
  ProveStrictUncertaintyAndAuthorityRefusals();
  ProveDisabledPathRecomputes();
  ProveEligibilityAndRecomputeProofRequired();
  return 0;
}
