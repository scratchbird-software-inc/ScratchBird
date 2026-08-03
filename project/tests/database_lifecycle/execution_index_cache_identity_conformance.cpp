// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_key_encoding.hpp"
#include "snapshot_safe_result_cache.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace exec = scratchbird::engine::executor;
namespace platform = scratchbird::core::platform;

exec::PhysicalMgaStatementContext CacheMgaContext(
    const std::string& statement_uuid =
        "019f0000-0000-7500-8000-000000006101") {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = statement_uuid;
  context.owning_transaction_uuid =
      "019f0000-0000-7500-8000-000000006102";
  context.statement_snapshot_uuid =
      "019f0000-0000-7500-8000-000000006103";
  context.statement_metadata_snapshot_uuid =
      "019f0000-0000-7500-8000-000000006104";
  context.owning_local_transaction_id = 7;
  context.visible_committed_high_watermark = 6;
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
  dag.selected_plan_uuid = "019f0000-0000-7500-8000-000000006120";
  dag.root_physical_node_id = 1;
  dag.local_transaction_id = context.owning_local_transaction_id;
  dag.statement_snapshot_id = context.visible_committed_high_watermark;
  dag.mga_statement_context = context;
  dag.bound_sblr_tree_uuid = "019f0000-0000-7500-8000-000000006121";
  dag.catalog_epoch_uuid = "019f0000-0000-7500-8000-000000006122";
  dag.security_context_uuid = "019f0000-0000-7500-8000-000000006123";
  dag.capability_snapshot_uuid = "019f0000-0000-7500-8000-000000006124";
  dag.resource_snapshot_uuid = "019f0000-0000-7500-8000-000000006125";
  dag.statistics_snapshot_uuid = "019f0000-0000-7500-8000-000000006126";
  dag.route_snapshot_uuid = "019f0000-0000-7500-8000-000000006127";
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
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest, dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute, dag.route_snapshot_uuid}};
  exec::PhysicalNodeRecord node;
  node.physical_node_id = 1;
  node.relational_node_id = 1;
  node.node_kind = exec::PhysicalNodeKind::kValues;
  node.implementation_id = "values.materialize.v1";
  node.output_descriptor_ids = {1};
  node.causal_counter_id = 1;
  node.selected_alternative_uuid = "019f0000-0000-7500-8000-000000006130";
  node.executor_capability_uuid = "019f0000-0000-7500-8000-000000006131";
  node.executor_capability_abi_version = 1;
  node.cost_vector_uuid = "019f0000-0000-7500-8000-000000006132";
  node.memory_bytes_required = 1;
  node.engine_capability_validated = true;
  node.mga_statement_context = context;
  dag.nodes.push_back(node);
  return dag;
}

template <typename Request>
void BindCacheRequest(Request* request,
                      const exec::PhysicalMgaStatementContext& context =
                          CacheMgaContext()) {
  const auto dag = CacheSelectedDag(context);
  request->mga_authority.statement_context = context;
  request->mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  request->mga_authority.resolve_current = [context] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = context;
    return resolution;
  };
  request->selected_physical_dag = dag;
  request->selected_catalog_epoch_uuid = dag.catalog_epoch_uuid;
  if constexpr (requires { request->entry; }) {
    request->entry.key.catalog_epoch_uuid = dag.catalog_epoch_uuid;
    request->entry.producing_statement_context = context;
    request->entry.catalog_epoch_uuid = dag.catalog_epoch_uuid;
  } else {
    request->key.catalog_epoch_uuid = dag.catalog_epoch_uuid;
  }
}


[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid TypedUuid(platform::byte seed) {
  platform::TypedUuid uuid;
  uuid.kind = platform::UuidKind::object;
  for (std::size_t index = 0; index < uuid.value.bytes.size(); ++index) {
    uuid.value.bytes[index] = static_cast<platform::byte>(seed + index);
  }
  uuid.value.bytes[6] =
      static_cast<platform::byte>((uuid.value.bytes[6] & 0x0f) | 0x70);
  uuid.value.bytes[8] =
      static_cast<platform::byte>((uuid.value.bytes[8] & 0x3f) | 0x80);
  return uuid;
}

idx::IndexKeySemanticProfile StableProfile() {
  idx::IndexKeySemanticProfile profile;
  profile.profile_id = "edr012-index-cache-identity";
  profile.bytewise_stable = true;
  return profile;
}

idx::IndexKeyEncodingComponent Component(platform::byte descriptor_seed,
                                         platform::u64 descriptor_epoch,
                                         std::string_view payload) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = TypedUuid(descriptor_seed);
  component.type_descriptor_epoch = descriptor_epoch;
  component.sort_direction = idx::IndexKeySortDirection::ascending;
  component.null_placement = idx::IndexKeyNullPlacement::nulls_last;
  component.payload.assign(payload.begin(), payload.end());
  return component;
}

exec::SnapshotSafeCacheKey CacheKey(std::string descriptor_digest,
                                    std::uint64_t descriptor_epoch) {
  exec::SnapshotSafeCacheKey key;
  key.normalized_operation = "edr012.select.by.descriptor";
  key.safe_parameter_digest = "tenant:stable";
  key.catalog_epoch = 12;
  key.statistics_epoch = 13;
  key.security_epoch = 14;
  key.redaction_epoch = 15;
  key.mga_visibility_snapshot_class = "repeatable_read:edr012";
  key.provider_generation = 16;
  key.descriptor_identity_digest = std::move(descriptor_digest);
  key.descriptor_epoch = descriptor_epoch;
  key.result_contract_identity = "edr012.rowset.v1";
  key.result_contract_hash = "sha256:edr012-rowset";
  key.route_compatibility = "embedded";
  key.dialect_compatibility = "sbsql_v3";
  return key;
}

exec::SnapshotSafeCacheEntry CacheEntry() {
  exec::SnapshotSafeCacheEntry entry;
  entry.key = CacheKey("descriptor:edr012:v1", 21);
  entry.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  entry.row_count = 1;
  entry.cached_result_digest = "sha256:result";
  entry.cached_mga_security_digest = "sha256:mga-security";
  return entry;
}

exec::SnapshotSafeCacheStoreRequest StoreRequest() {
  exec::SnapshotSafeCacheStoreRequest request;
  request.entry = CacheEntry();
  request.read_only_operation = true;
  request.small_final_result = true;
  request.max_small_result_rows = 16;
  BindCacheRequest(&request);
  return request;
}

exec::SnapshotSafeCacheLookupRequest LookupRequest(exec::SnapshotSafeCacheKey key) {
  exec::SnapshotSafeCacheLookupRequest request;
  request.key = std::move(key);
  request.payload_kind = exec::SnapshotSafeCachePayloadKind::kSmallFinalResult;
  request.read_only_operation = true;
  request.small_final_result = true;
  request.row_count = 1;
  request.max_small_result_rows = 16;
  request.recomputed_result_digest = "sha256:result";
  request.recomputed_mga_security_digest = "sha256:mga-security";
  BindCacheRequest(&request);
  return request;
}

bool EvidenceContains(const exec::SnapshotSafeCacheDecision& decision,
                      std::string_view needle) {
  return std::any_of(decision.evidence.begin(),
                     decision.evidence.end(),
                     [&](const std::string& evidence) {
                       return evidence.find(needle) != std::string::npos;
                     });
}

void TestIndexKeyDescriptorIdentity() {
  const auto payload_v1 = Component(0x20, 1, "same-raw-payload");
  const auto payload_v2 = Component(0x20, 2, "same-raw-payload");
  const auto payload_other_descriptor = Component(0x30, 1, "same-raw-payload");

  Require(payload_v1.payload == payload_v2.payload &&
              payload_v1.payload == payload_other_descriptor.payload,
          "EDR-012 fixture raw payloads differ");

  const auto encoded_v1 = idx::EncodeIndexKey({payload_v1}, StableProfile());
  const auto encoded_v2 = idx::EncodeIndexKey({payload_v2}, StableProfile());
  const auto encoded_other =
      idx::EncodeIndexKey({payload_other_descriptor}, StableProfile());
  Require(encoded_v1.ok() && encoded_v2.ok() && encoded_other.ok(),
          "EDR-012 descriptor-bound index encoding refused valid keys");
  Require(encoded_v1.encoded != encoded_v2.encoded,
          "EDR-012 descriptor epoch was not part of index key identity");
  Require(encoded_v1.encoded != encoded_other.encoded,
          "EDR-012 descriptor UUID was not part of index key identity");
  Require(!encoded_v1.evidence.empty(),
          "EDR-012 index encoding did not emit descriptor identity evidence");

  const auto prefix_v1 =
      idx::BuildEncodedPrefixMatcher({payload_v1}, StableProfile());
  const auto prefix_v2 =
      idx::BuildEncodedPrefixMatcher({payload_v2}, StableProfile());
  Require(prefix_v1.ok() && prefix_v2.ok(),
          "EDR-012 descriptor-bound prefix builder refused valid keys");
  Require(prefix_v1.matcher_prefix != prefix_v2.matcher_prefix,
          "EDR-012 prefix identity ignored descriptor epoch");
}

void TestIndexKeyOrderingWithinDescriptor() {
  const auto left = idx::EncodeIndexKey({Component(0x40, 5, "alpha")},
                                       StableProfile());
  const auto right = idx::EncodeIndexKey({Component(0x40, 5, "beta")},
                                        StableProfile());
  Require(left.ok() && right.ok(),
          "EDR-012 descriptor-bound ordered keys were refused");
  const auto compared = idx::CompareEncodedIndexKeys(left.encoded, right.encoded);
  Require(compared.ok(), "EDR-012 encoded key compare failed");
  Require(compared.comparison < 0,
          "EDR-012 descriptor-bound index key lost payload ordering");
}

void TestIndexKeyDescriptorFailures() {
  auto missing_epoch = Component(0x50, 0, "same-raw-payload");
  const auto encoded = idx::EncodeIndexKey({missing_epoch}, StableProfile());
  Require(!encoded.ok(), "EDR-012 accepted index key without descriptor epoch");
  Require(encoded.diagnostic.diagnostic_code ==
              "SB-INDEX-KEY-ENCODING-TYPE-EPOCH-MISSING",
          "EDR-012 descriptor epoch refusal diagnostic mismatch");

  const auto prefix =
      idx::BuildEncodedPrefixMatcher({missing_epoch}, StableProfile());
  Require(!prefix.ok(),
          "EDR-012 accepted index prefix without descriptor epoch");
  Require(prefix.diagnostic.diagnostic_code ==
              "SB-INDEX-KEY-PREFIX-TYPE-EPOCH-MISSING",
          "EDR-012 prefix descriptor epoch diagnostic mismatch");
}

void TestSnapshotCacheDescriptorIdentity() {
  exec::SnapshotSafeResultCache cache;
  const auto stored = cache.Store(StoreRequest());
  Require(stored.accepted && stored.action == exec::SnapshotSafeCacheAction::kStore,
          "EDR-012 descriptor-bound cache store was refused");
  Require(EvidenceContains(stored,
                           "descriptor_identity_digest=descriptor:edr012:v1"),
          "EDR-012 cache store evidence did not include descriptor identity");

  auto hit = cache.Lookup(LookupRequest(CacheKey("descriptor:edr012:v1", 21)));
  Require(hit.cache_hit && hit.action == exec::SnapshotSafeCacheAction::kHit,
          "EDR-012 descriptor-identical cache lookup did not hit");

  auto descriptor_miss =
      cache.Lookup(LookupRequest(CacheKey("descriptor:edr012:v2", 21)));
  Require(!descriptor_miss.cache_hit &&
              descriptor_miss.action ==
                  exec::SnapshotSafeCacheAction::kMissRecompute,
          "EDR-012 descriptor digest change did not miss cache");

  auto epoch_miss =
      cache.Lookup(LookupRequest(CacheKey("descriptor:edr012:v1", 22)));
  Require(!epoch_miss.cache_hit &&
              epoch_miss.action ==
                  exec::SnapshotSafeCacheAction::kMissRecompute,
          "EDR-012 descriptor epoch change did not miss cache");

  auto cross_statement =
      LookupRequest(CacheKey("descriptor:edr012:v1", 21));
  BindCacheRequest(
      &cross_statement,
      CacheMgaContext("019f0000-0000-7500-8000-000000006199"));
  const auto statement_miss = cache.Lookup(cross_statement);
  Require(!statement_miss.cache_hit &&
              statement_miss.action ==
                  exec::SnapshotSafeCacheAction::kMissRecompute,
          "EDR-012 equal digest/scalar cross-statement result reused cache");

  auto incomplete = StoreRequest();
  incomplete.entry.key.descriptor_identity_digest.clear();
  const auto refused = cache.Store(incomplete);
  Require(!refused.accepted &&
              refused.diagnostic_code ==
                  "EXECUTOR.SNAPSHOT_RESULT_CACHE.KEY_INCOMPLETE",
          "EDR-012 accepted cache key without descriptor identity");
}

}  // namespace

int main() {
  TestIndexKeyDescriptorIdentity();
  TestIndexKeyOrderingWithinDescriptor();
  TestIndexKeyDescriptorFailures();
  TestSnapshotCacheDescriptorIdentity();
  return EXIT_SUCCESS;
}
