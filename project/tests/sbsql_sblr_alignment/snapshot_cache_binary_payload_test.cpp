// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual cache and descriptor/MGA validators; controlled resolver inputs are
// component coverage, not SQL/IPC or live transaction-inventory evidence.
#include "snapshot_safe_result_cache.hpp"
#include "uuid.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

namespace fault { thread_local long remaining = -1; thread_local bool hit = false; }
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace ex = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace {
unsigned checks = 0, faults = 0;
void Check(bool ok, const char* detail) {
  ++checks;
  if (!ok) throw std::runtime_error(detail);
}
api::EngineUuid Id(unsigned n) {
  api::EngineUuid id;
  id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80;
  id.bytes[14] = n >> 8; id.bytes[15] = n;
  return id;
}
ex::SnapshotSafeCacheStoreRequest StoreRequest(bool candidates = false) {
  ex::SnapshotSafeCacheStoreRequest r;
  auto& dag = r.selected_physical_dag;
  auto& ctx = r.mga_authority.statement_context;
  ctx = {Id(1), Id(2), Id(3), Id(4), 7, 6, 7, 3, 3, 3, {7,9}, {8},
         "statement_stable", 10, true, true, true, "2026-09-12T00:00:00Z"};
  r.mga_authority.origin = ex::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  r.mga_authority.resolve_current = [ctx] {
    ex::CanonicalMgaCurrentResolution result; result.statement_context = ctx; return result;
  };
  dag.abi_version = 2; dag.selected_plan_uuid = Id(20);
  dag.root_physical_node_id = 1; dag.local_transaction_id = 7;
  dag.statement_snapshot_id = 6; dag.mga_statement_context = ctx;
  dag.bound_sblr_tree_uuid = Id(21); dag.catalog_epoch_uuid = Id(22);
  dag.security_context_uuid = Id(23); dag.capability_snapshot_uuid = Id(24);
  dag.resource_snapshot_uuid = Id(25); dag.statistics_snapshot_uuid = Id(26);
  dag.route_snapshot_uuid = Id(27);
  unsigned stage = 0;
  for (auto id : {Id(21), Id(22), Id(23), Id(3), Id(24), Id(25), Id(26), Id(27)})
    dag.admission_evidence.push_back({static_cast<ex::PhysicalAdmissionStage>(++stage), id});
  ex::PhysicalNodeRecord node;
  node.physical_node_id = 1; node.relational_node_id = 1;
  node.node_kind = ex::PhysicalNodeKind::kScan; node.implementation_id = "scan.heap.v1";
  node.output_descriptor_ids = {1}; node.causal_counter_id = 1;
  node.selected_alternative_uuid = Id(30); node.executor_capability_uuid = Id(31);
  node.executor_capability_abi_version = 1; node.cost_vector_uuid = Id(32);
  node.engine_capability_validated = true; node.mga_statement_context = ctx;
  dag.nodes = {node};
  dag.catalog_generation = dag.security_epoch = dag.policy_epoch = dag.resource_epoch = 1;
  dag.statistics_generation = dag.route_epoch = dag.route_generation = 1;
  dag.memory_budget_bytes = 1 << 20; dag.optimizer_published = true;
  dag.immutable_node_identity_validated = dag.capability_validated_before_access = true;
  auto& key = r.entry.key;
  key.bound_sblr_tree_uuid = Id(21); key.catalog_epoch_uuid = Id(22);
  key.security_context_uuid = Id(23); key.safe_parameter_digest = "parameters-content";
  key.catalog_epoch = key.statistics_epoch = key.security_epoch = key.redaction_epoch = 1;
  key.mga_visibility_snapshot_class = "statement_stable"; key.provider_generation = 1;
  key.descriptor_identity_digest = "descriptor-content"; key.descriptor_epoch = 1;
  key.result_contract_identity = "canonical-result-v2";
  key.result_contract_hash = "result-contract-content"; key.route_compatibility = "local-node";
  r.selected_catalog_epoch_uuid = r.entry.catalog_epoch_uuid = Id(22);
  r.entry.producing_statement_context = ctx;
  r.entry.payload_kind = candidates ? ex::SnapshotSafeCachePayloadKind::kCandidateSet :
                                    ex::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  r.small_final_result = !candidates; r.candidate_set_snapshot_safe = candidates;
  r.entry.row_count = 1; r.entry.payload.emplace();
  if (candidates) {
    ex::CanonicalScanCandidateEvidence candidate;
    candidate.candidate_uuid = Id(40); candidate.record_uuid = Id(41);
    candidate.relation_uuid = Id(42); candidate.visibility_decision_uuid = Id(43);
    candidate.row_version_id = 1; candidate.candidate_generation = 1;
    candidate.observed_generation = 1; candidate.creator_local_transaction_id = 5;
    candidate.source = ex::CanonicalScanCandidateSource::kRelationPage;
    candidate.visibility = ex::CanonicalMgaVisibilityDecision::kVisible;
    candidate.security_decision = ex::CanonicalMgaSecurityDecision::kAllowed;
    candidate.residual_truth = api::EngineSqlTruthValue::true_value;
    candidate.locator_identity_matches = true;
    r.entry.payload->candidates = {candidate};
  } else {
    api::EngineDescriptor descriptor;
    descriptor.descriptor_uuid = Id(40); descriptor.type_uuid = Id(41);
    descriptor.descriptor_kind = "scalar"; descriptor.canonical_type_name = "int64";
    descriptor.encoded_descriptor = "nullability=non_null";
    r.entry.payload->final_result.columns = {{"n", descriptor, false, 1}};
    r.entry.payload->final_result.rows = {{{api::EngineTypedValue(descriptor, "42")}}};
  }
  return r;
}
ex::SnapshotSafeCacheLookupRequest LookupRequest(const ex::SnapshotSafeCacheStoreRequest& r) {
  ex::SnapshotSafeCacheLookupRequest q;
  q.key = r.entry.key; q.mga_authority = r.mga_authority;
  q.selected_physical_dag = r.selected_physical_dag;
  q.selected_catalog_epoch_uuid = r.selected_catalog_epoch_uuid;
  q.payload_kind = r.entry.payload_kind; q.candidate_set_snapshot_safe = r.candidate_set_snapshot_safe;
  q.small_final_result = r.small_final_result; q.row_count = r.entry.row_count;
  q.recomputed_payload = r.entry.payload;
  return q;
}
void PayloadOwnership() {
  for (bool candidates : {false, true}) {
    ex::SnapshotSafeResultCache cache;
    auto input = StoreRequest(candidates);
    const auto query = LookupRequest(input);
    auto stored = cache.Store(input);
    Check(stored.accepted && cache.Size() == 1, "actual payload store refused");
    input.entry.payload.reset();
    auto hit = cache.Lookup(query);
    Check(hit.cache_hit && hit.retained_entry && hit.retained_entry->payload,
          "hit did not return real retained payload");
    Check(scratchbird::core::uuid::IsEngineIdentityUuid(hit.retained_entry->entry_uuid),
          "cache object lacks issued UUIDv7");
    auto second = cache.Lookup(query);
    Check(second.retained_entry == hit.retained_entry, "lookup manufactured a substitute object");
    Check(hit.retained_entry->row_count == 1 &&
          (candidates ? hit.retained_entry->payload->candidates[0].record_uuid == Id(41) :
           hit.retained_entry->payload->final_result.rows[0].values[0].encoded_value == "42"),
          "cache did not retain actual input bytes");
    cache.Clear();
    Check(cache.Size() == 0 && !cache.Lookup(query).cache_hit && hit.retained_entry->payload,
          "cache eviction destroyed borrowed payload or reported a hit");
  }
}
void RefusalsAndInvalidation() {
  ex::SnapshotSafeResultCache cache;
  auto input = StoreRequest(); auto query = LookupRequest(input);
  auto missing = input; missing.entry.payload.reset();
  Check(!cache.Store(missing).accepted && cache.Size() == 0, "digest-only store succeeded");
  input.entry.cached_result_digest = "forged";
  Check(!cache.Store(input).accepted && cache.Size() == 0, "forged supplied digest accepted");
  input.entry.cached_result_digest.clear();
  Check(cache.Store(input).accepted, "valid store failed");
  query.recomputed_payload.reset();
  Check(!cache.Lookup(query).accepted, "digest-only recompute hit");
  query = LookupRequest(input);
  query.recomputed_payload->final_result.rows[0].values[0].encoded_value = "43";
  auto mismatch = cache.Lookup(query);
  Check(!mismatch.cache_hit && mismatch.action == ex::SnapshotSafeCacheAction::kInvalidateRecompute &&
        !mismatch.retained_entry && cache.Size() == 0, "mismatch did not actually evict entry");
  Check(cache.Store(input).accepted, "store after invalidation failed");
  auto conflict = input;
  conflict.entry.payload->final_result.rows[0].values[0].encoded_value = "43";
  const auto refused = cache.Store(conflict);
  Check(!refused.accepted && cache.Size() == 1, "conflicting same-key payload replaced entry");
  for (const auto& evidence : refused.evidence)
    Check(evidence != "snapshot_cache_stored=true", "refusal retained staged success evidence");
  query = LookupRequest(input);
  Check(cache.Lookup(query).cache_hit, "conflicting store damaged old payload");
  auto ctx = query.mga_authority.statement_context;
  query.mga_authority.resolve_current = [ctx, calls = 0]() mutable {
    ex::CanonicalMgaCurrentResolution result; result.statement_context = ctx;
    if (++calls > 1) result.statement_context.current = false;
    return result;
  };
  Check(!cache.Lookup(query).cache_hit, "final MGA drift produced a hit");
}
void BinaryAndFraming() {
  Check(ex::SnapshotSafeCachePayloadDigest({}, ex::SnapshotSafeCachePayloadKind::kSmallFinalResult) ==
        "sha256:53a3e31237c15b093e23772d70ce0fef96265232088770519f6190d74a88a2ca",
        "SBCS domain4 independent literal digest differs");
  auto request = StoreRequest();
  const auto baseline = ex::SnapshotSafeCacheKeyText(request.entry.key);
  for (auto member : {&ex::SnapshotSafeCacheKey::bound_sblr_tree_uuid,
                      &ex::SnapshotSafeCacheKey::catalog_epoch_uuid,
                      &ex::SnapshotSafeCacheKey::security_context_uuid}) {
    for (unsigned bit = 0; bit != 128; ++bit) {
      auto changed = request;
      (changed.entry.key.*member).bytes[bit / 8] ^= 1u << (bit % 8);
      Check(ex::SnapshotSafeCacheKeyText(changed.entry.key) != baseline, "key digest lost UUID bit");
      ex::SnapshotSafeResultCache cache;
      Check(!cache.Store(changed).accepted && cache.Size() == 0, "swapped binding entered cache");
    }
  }
  auto left = request.entry.key, right = left;
  left.result_contract_identity = "a|result_contract_hash=b";
  left.result_contract_hash = "c";
  right.result_contract_identity = "a";
  right.result_contract_hash = "b|result_contract_hash=c";
  Check(ex::SnapshotSafeCacheKeyText(left) != ex::SnapshotSafeCacheKeyText(right),
        "delimiter fields aliased");
  right = left; right.safe_parameter_digest.push_back('\0');
  Check(ex::SnapshotSafeCacheKeyText(left) != ex::SnapshotSafeCacheKeyText(right),
        "embedded NUL omitted");
  auto payload = *request.entry.payload;
  const auto digest = ex::SnapshotSafeCachePayloadDigest(payload, request.entry.payload_kind);
  payload.final_result.rows[0].values[0].binary_value = {0, 1, 0};
  Check(ex::SnapshotSafeCachePayloadDigest(payload, request.entry.payload_kind) != digest,
        "typed value binary payload omitted");
  auto candidate_request = StoreRequest(true);
  for (auto member : {&ex::CanonicalScanCandidateEvidence::candidate_uuid,
                      &ex::CanonicalScanCandidateEvidence::record_uuid,
                      &ex::CanonicalScanCandidateEvidence::relation_uuid,
                      &ex::CanonicalScanCandidateEvidence::visibility_decision_uuid}) {
    const auto base = ex::SnapshotSafeCachePayloadDigest(*candidate_request.entry.payload,
                                                        candidate_request.entry.payload_kind);
    for (unsigned bit = 0; bit != 128; ++bit) {
      auto changed = *candidate_request.entry.payload;
      (changed.candidates[0].*member).bytes[bit / 8] ^= 1u << (bit % 8);
      Check(ex::SnapshotSafeCachePayloadDigest(changed, candidate_request.entry.payload_kind) != base,
            "candidate content digest lost a UUID bit");
    }
  }
  for (unsigned version = 0; version != 16; ++version) {
    auto changed = candidate_request;
    changed.entry.payload->candidates[0].record_uuid.bytes[6] = version << 4;
    ex::SnapshotSafeResultCache cache;
    Check(cache.Store(changed).accepted == (version == 7), "candidate system UUID version policy");
  }
  auto invalid = candidate_request;
  invalid.entry.payload->candidates[0].visibility =
      static_cast<ex::CanonicalMgaVisibilityDecision>(255);
  ex::SnapshotSafeResultCache cache;
  Check(!cache.Store(invalid).accepted, "unknown candidate enum entered cache");
  invalid = candidate_request;
  invalid.entry.payload->candidates.push_back(invalid.entry.payload->candidates.front());
  invalid.entry.row_count = 2;
  Check(!cache.Store(invalid).accepted, "duplicate candidate entered cache");
}
void FailureAtomicity() {
  for (bool lookup : {false, true}) {
    bool reached = false;
    for (long point = 0; point != 2048; ++point) {
      ex::SnapshotSafeResultCache cache;
      const auto input = StoreRequest(); const auto query = LookupRequest(input);
      if (lookup) Check(cache.Store(input).accepted, "fault baseline store refused");
      fault::hit = false; fault::remaining = point;
      bool success = false;
      try {
        const auto result = lookup ? cache.Lookup(query) : cache.Store(input);
        success = result.accepted;
      } catch (const std::exception&) {}
      fault::remaining = -1;
      if (!fault::hit) { Check(success, "fault sweep ended without success"); reached = true; break; }
      ++faults;
      Check(!success && cache.Size() == (lookup ? 1 : 0), "allocation failure changed cache state");
      if (lookup) Check(cache.Lookup(query).cache_hit, "lookup failure damaged retained entry");
    }
    Check(reached, "allocation sweep incomplete");
  }
}
void ConcurrentStore() {
  ex::SnapshotSafeResultCache cache;
  const auto input = StoreRequest(); const auto query = LookupRequest(input);
  std::atomic<unsigned> successes{0};
  std::vector<std::thread> workers;
  for (unsigned i = 0; i != 4; ++i) workers.emplace_back([&] {
    for (unsigned j = 0; j != 16; ++j)
      if (cache.Store(input).accepted && cache.Lookup(query).cache_hit) ++successes;
  });
  for (auto& worker : workers) worker.join();
  Check(successes == 64 && cache.Size() == 1, "concurrent store/reuse lost ownership");
}
}
int main() {
  try {
    PayloadOwnership(); RefusalsAndInvalidation(); BinaryAndFraming();
    FailureAtomicity(); ConcurrentStore();
    std::cout << "snapshot cache binary payload PASS checks=" << checks << " faults=" << faults << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
