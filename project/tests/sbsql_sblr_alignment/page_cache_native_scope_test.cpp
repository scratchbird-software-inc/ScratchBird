// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/storage/page/page_cache.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
namespace page = scratchbird::storage::page;
using scratchbird::core::platform::UuidKind;
using scratchbird::tests::FixtureUuid;
void Check(bool ok) { if (!ok) std::abort(); }
int main() {
  auto database_a = FixtureUuid(1277, 1);
  auto database_b = FixtureUuid(1277, 2);
  auto filespace = FixtureUuid(1277, 3);
  database_a.bytes[9] = 0; database_a.bytes[10] = '\n'; database_a.bytes[11] = 255;
  database_b.bytes[9] = 0; database_b.bytes[10] = '\t'; database_b.bytes[11] = 255;
  page::PageCacheCheckpointPublication publication;
  publication.database_uuid = database_a; publication.filespace_uuid = filespace;
  const auto encoded = page::SerializePageCacheCheckpoint(publication, false);
  page::PageCacheCheckpointEnvelope decoded;
  Check(page::DecodePageCacheCheckpoint(encoded, &decoded));
  Check(decoded.database_uuid == database_a && decoded.filespace_uuid == filespace);
  Check(decoded.details_json.find("database_uuid") == std::string::npos);
  Check(decoded.details_json.find("authority_boundary_valid") != std::string::npos);
  for (std::size_t length = 0; length < encoded.size(); ++length)
    Check(!page::DecodePageCacheCheckpoint(std::string_view(encoded).substr(0, length), &decoded));
  Check(!page::DecodePageCacheCheckpoint(encoded + "x", &decoded));
  auto invalid = encoded; invalid[0] = 'X';
  Check(!page::DecodePageCacheCheckpoint(invalid, &decoded));
  invalid = encoded; invalid[47] = static_cast<char>(255);
  Check(!page::DecodePageCacheCheckpoint(invalid, &decoded));
  page::PageCacheLedger ledger;
  const auto entry = [&](auto database, auto context, std::uint64_t ordinal,
                         std::uint64_t bytes, bool pinned, bool dirty) {
    page::PageCacheEntry value;
    value.database_uuid = {UuidKind::database, database};
    value.filespace_uuid = {UuidKind::filespace, filespace};
    value.page_uuid = {UuidKind::page, FixtureUuid(1277, ordinal)};
    value.io_context = context; value.resident = true; value.resident_bytes = bytes;
    value.pin_count = pinned ? 1 : 0; value.dirty = dirty;
    return value;
  };
  ledger.entries = {entry(database_a, page::PageCacheIoContext::normal, 10, 16384, true, false),
      entry(database_a, page::PageCacheIoContext::bulk_read, 11, 32768, false, true),
      entry(database_b, page::PageCacheIoContext::normal, 12, 65536, false, false)};
  using scratchbird::core::memory::MemoryBinaryScopeKind;
  for (const auto& value : ledger.entries) {
    const auto key = page::PageFrameKey(value.page_uuid);
    Check(key == value.page_uuid.value);
    Check(page::FrameShardIndexForKey(key) < page::kPageCacheFrameShardCount);
    const auto tag = page::PageCacheFrameTag(value, value.io_context);
    Check(scratchbird::core::memory::MemoryBinaryOwnershipValid(tag));
    Check(tag.binary_ownership[MemoryBinaryScopeKind::owner] == value.database_uuid.value.bytes);
    Check(tag.binary_ownership[MemoryBinaryScopeKind::database] == value.database_uuid.value.bytes);
    Check(tag.binary_ownership[MemoryBinaryScopeKind::context] == value.page_uuid.value.bytes);
    Check(tag.owner.empty() && tag.context_id.empty() && tag.database_id.empty());
  }
  const auto snapshot = page::SnapshotPageCache(ledger);
  Check(snapshot.resident_pages == 3 && snapshot.resident_bytes == 114688);
  Check(snapshot.metric_scopes.size() == 2);
  const auto& a = snapshot.metric_scopes[0]; const auto& b = snapshot.metric_scopes[1];
  Check(a.database_uuid == database_a && b.database_uuid == database_b);
  Check(a.filespace_uuid == filespace && b.filespace_uuid == filespace);
  Check(a.resident_pages == 2 && a.resident_bytes == 49152 && a.pinned_pages == 1 && a.dirty_pages == 1);
  Check(b.resident_pages == 1 && b.resident_bytes == 65536 && b.pinned_pages == 0 && b.dirty_pages == 0);
  Check(a.contexts[page::ContextIndex(page::PageCacheIoContext::normal)].resident_bytes == 16384);
  Check(a.contexts[page::ContextIndex(page::PageCacheIoContext::bulk_read)].resident_bytes == 32768);
  // Exercise the real eviction bookkeeping after modeling successful unpin/writeback.
  ledger.entries[0].pin_count = 0; ledger.entries[1].dirty = false;
  page::PageCacheScopedEvictions evictions;
  for (auto& value : ledger.entries) page::EvictUnlocked(&ledger, &value, &evictions);
  Check(evictions.size() == 3);
  Check(evictions.at({database_a, filespace, page::PageCacheIoContext::normal}) == 1);
  Check(evictions.at({database_a, filespace, page::PageCacheIoContext::bulk_read}) == 1);
  Check(evictions.at({database_b, filespace, page::PageCacheIoContext::normal}) == 1);
  const auto empty = page::SnapshotPageCache(ledger);
  Check(empty.resident_pages == 0 && empty.metric_scopes.size() == 2);
  for (const auto& scope : empty.metric_scopes) {
    Check(scope.resident_pages == 0 && scope.resident_bytes == 0 && scope.pinned_pages == 0 && scope.dirty_pages == 0);
    for (const auto& context : scope.contexts)
      Check(context.resident_pages == 0 && context.resident_bytes == 0);
  }
  std::cout << "PASS page cache native scope snapshots and eviction accounting\n";
}
