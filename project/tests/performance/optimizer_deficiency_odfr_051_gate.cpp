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
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;

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

std::string CacheUuid(const std::uint64_t suffix) {
  std::ostringstream out;
  out << "019f0000-0000-7500-8000-" << std::hex << std::setfill('0')
      << std::setw(12) << suffix;
  return out.str();
}

exec::PhysicalMgaStatementContext CacheStatementContext(
    const std::uint64_t statement_suffix = 5101,
    const bool zero_high_water = false) {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = CacheUuid(statement_suffix);
  context.owning_transaction_uuid = CacheUuid(5102);
  context.statement_snapshot_uuid = CacheUuid(5103);
  context.statement_metadata_snapshot_uuid = CacheUuid(5104);
  context.owning_local_transaction_id = 7;
  context.visible_committed_high_watermark = zero_high_water ? 0 : 6;
  context.oldest_active_transaction_id = 7;
  context.oldest_interesting_transaction_id = 3;
  context.oldest_snapshot_transaction_id = 3;
  context.retention_horizon_transaction_id = 3;
  context.active_excluded_local_transaction_ids = {7, 9};
  context.in_doubt_excluded_local_transaction_ids = {8};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 10;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

exec::TypedPhysicalNodeDag CacheSelectedDag(
    const exec::PhysicalMgaStatementContext& context) {
  exec::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = CacheUuid(5120);
  dag.root_physical_node_id = 1;
  dag.local_transaction_id = context.owning_local_transaction_id;
  dag.statement_snapshot_id = context.visible_committed_high_watermark;
  dag.mga_statement_context = context;
  dag.bound_sblr_tree_uuid = CacheUuid(5121);
  dag.catalog_epoch_uuid = CacheUuid(5122);
  dag.security_context_uuid = CacheUuid(5123);
  dag.capability_snapshot_uuid = CacheUuid(5124);
  dag.resource_snapshot_uuid = CacheUuid(5125);
  dag.statistics_snapshot_uuid = CacheUuid(5126);
  dag.route_snapshot_uuid = CacheUuid(5127);
  dag.catalog_generation = 1;
  dag.security_epoch = 1;
  dag.policy_epoch = 1;
  dag.resource_epoch = 1;
  dag.statistics_generation = 1;
  dag.route_epoch = 1;
  dag.route_generation = 1;
  dag.memory_budget_bytes = 1024;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  const std::vector<std::string> evidence{
      dag.bound_sblr_tree_uuid, dag.catalog_epoch_uuid,
      dag.security_context_uuid, context.statement_snapshot_uuid,
      dag.capability_snapshot_uuid, dag.resource_snapshot_uuid,
      dag.statistics_snapshot_uuid, dag.route_snapshot_uuid};
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    dag.admission_evidence.push_back(
        {static_cast<exec::PhysicalAdmissionStage>(index + 1),
         evidence[index]});
  }
  exec::PhysicalNodeRecord node;
  node.physical_node_id = 1;
  node.relational_node_id = 1;
  node.node_kind = exec::PhysicalNodeKind::kValues;
  node.implementation_id = "values.materialize.v1";
  node.output_descriptor_ids = {1};
  node.causal_counter_id = 1;
  node.selected_alternative_uuid = CacheUuid(5130);
  node.executor_capability_uuid = CacheUuid(5131);
  node.executor_capability_abi_version = 1;
  node.cost_vector_uuid = CacheUuid(5132);
  node.memory_bytes_required = 1;
  node.engine_capability_validated = true;
  node.mga_statement_context = context;
  dag.nodes.push_back(std::move(node));
  return dag;
}

struct CacheCurrentState {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  exec::PhysicalMgaStatementContext statement_context;
};

struct CacheBinding {
  exec::CanonicalExecutionMgaAuthority authority;
  exec::TypedPhysicalNodeDag selected_dag;
  std::string catalog_epoch_uuid;
  std::shared_ptr<CacheCurrentState> current;
};

CacheBinding MakeCacheBinding(const std::uint64_t statement_suffix = 5101,
                              const bool zero_high_water = false) {
  CacheBinding binding;
  binding.current = std::make_shared<CacheCurrentState>();
  binding.current->statement_context =
      CacheStatementContext(statement_suffix, zero_high_water);
  binding.selected_dag =
      CacheSelectedDag(binding.current->statement_context);
  binding.catalog_epoch_uuid = binding.selected_dag.catalog_epoch_uuid;
  binding.authority.statement_context = binding.current->statement_context;
  binding.authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  const auto current = binding.current;
  binding.authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.diagnostic = current->diagnostic;
    resolution.statement_context = current->statement_context;
    return resolution;
  };
  return binding;
}

void ApplyBinding(SnapshotSafeCacheStoreRequest* request,
                  const CacheBinding& binding) {
  request->entry.key.catalog_epoch_uuid = binding.catalog_epoch_uuid;
  request->entry.producing_statement_context =
      binding.authority.statement_context;
  request->entry.catalog_epoch_uuid = binding.catalog_epoch_uuid;
  request->mga_authority = binding.authority;
  request->selected_physical_dag = binding.selected_dag;
  request->selected_catalog_epoch_uuid = binding.catalog_epoch_uuid;
}

void ApplyBinding(SnapshotSafeCacheLookupRequest* request,
                  const CacheBinding& binding) {
  request->key.catalog_epoch_uuid = binding.catalog_epoch_uuid;
  request->mga_authority = binding.authority;
  request->selected_physical_dag = binding.selected_dag;
  request->selected_catalog_epoch_uuid = binding.catalog_epoch_uuid;
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
  ApplyBinding(&request, MakeCacheBinding());
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
  ApplyBinding(&request, MakeCacheBinding());
  return request;
}

void RequireAuthorityEvidence(const SnapshotSafeCacheDecision& decision) {
  const auto& evidence = decision.evidence;
  Require(HasEvidence(evidence, "cache_authority=none"),
          "cache must retain data without authority booleans");
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
  Require(HasEvidence(evidence, "catalog_epoch_uuid=" + CacheUuid(5122)),
          "missing independent catalog UUID evidence");
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
  ApplyBinding(&store_request, MakeCacheBinding());
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
  ApplyBinding(&lookup, MakeCacheBinding());
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
  ApplyBinding(&small_store, MakeCacheBinding());
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
  ApplyBinding(&small_lookup, MakeCacheBinding());
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
  authority.mga_authority.origin = exec::CanonicalMgaAuthorityOrigin::kMissing;
  const auto authority_decision = cache.Lookup(authority);
  Require(!authority_decision.accepted && authority_decision.fail_closed,
          "cached finality authority must fail closed");
  Require(authority_decision.diagnostic_code ==
              "EXECUTOR.SNAPSHOT_RESULT_CACHE.MGA_AUTHORITY_REQUIRED",
          "authority diagnostic mismatch");
}

void ProveExactStatementBindingAndAtomicRefusal() {
  SnapshotSafeResultCache cache;
  auto zero_binding = MakeCacheBinding(5101, true);
  auto store = CandidateStoreRequest();
  ApplyBinding(&store, zero_binding);
  const auto stored = cache.Store(store);
  Require(stored.action == SnapshotSafeCacheAction::kStore &&
              cache.Size() == 1,
          "valid zero-high-water producing statement did not store");

  auto lookup = CandidateLookupRequest();
  ApplyBinding(&lookup, zero_binding);
  Require(cache.Lookup(lookup).action == SnapshotSafeCacheAction::kHit,
          "exact zero-high-water producing statement did not hit");

  auto different_statement = CandidateLookupRequest();
  ApplyBinding(&different_statement, MakeCacheBinding(5199, true));
  const auto miss = cache.Lookup(different_statement);
  Require(miss.action == SnapshotSafeCacheAction::kMissRecompute &&
              !miss.cache_hit && cache.Size() == 1,
          "different statement reused equal scalar/digest result data");

  auto missing_resolver = CandidateLookupRequest();
  missing_resolver.mga_authority.resolve_current = {};
  const auto refused = cache.Lookup(missing_resolver);
  Require(refused.action == SnapshotSafeCacheAction::kRefuse &&
              !refused.accepted && !refused.cache_hit && cache.Size() == 1,
          "missing resolver exposed or mutated a retained payload");

  auto changed_current_binding = MakeCacheBinding();
  auto changed_current = CandidateLookupRequest();
  ApplyBinding(&changed_current, changed_current_binding);
  changed_current_binding.current->statement_context.current = false;
  const auto stale = cache.Lookup(changed_current);
  Require(stale.action == SnapshotSafeCacheAction::kRefuse &&
              !stale.cache_hit && cache.Size() == 1,
          "changed current inventory state exposed retained payload");

  auto mismatched_entry = CandidateStoreRequest();
  mismatched_entry.entry.producing_statement_context.statement_uuid =
      CacheUuid(5188);
  const auto mismatched = cache.Store(mismatched_entry);
  Require(mismatched.action == SnapshotSafeCacheAction::kRefuse &&
              !mismatched.cache_hit && cache.Size() == 1,
          "mismatched producing context stored or exposed payload");
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
  ProveExactStatementBindingAndAtomicRefusal();
  ProveDisabledPathRecomputes();
  ProveEligibilityAndRecomputeProofRequired();
  return 0;
}
