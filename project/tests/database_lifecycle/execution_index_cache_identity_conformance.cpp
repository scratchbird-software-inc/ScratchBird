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
