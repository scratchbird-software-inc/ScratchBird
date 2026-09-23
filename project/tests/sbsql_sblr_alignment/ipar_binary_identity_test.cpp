// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog/ipar_fast_path_support.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace api = scratchbird::engine::internal_api;
using scratchbird::tests::FixtureUuid;

static void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(EXIT_FAILURE);
  }
}

int main() {
  static_assert(sizeof(api::EngineUuid) == 16);
  static_assert(std::is_same_v<decltype(api::IparParameterBinding::column_uuid),
                               api::EngineUuid>);
  const auto table = FixtureUuid(6017, 1);
  const auto statement = FixtureUuid(6017, 2);
  const auto epoch = api::CompressIparFastPathEpochVector({1, 2, 3, 4, 5, 6, 7});
  api::IparRowLayoutColumn column;
  column.column_uuid = FixtureUuid(6017, 3);
  column.descriptor.descriptor_uuid = FixtureUuid(6017, 4);
  column.descriptor.canonical_type_name = "int64";
  column.fixed_width_bytes = 8;
  const auto layout = api::BuildIparRowLayoutDescriptor(table, statement, epoch, {column});
  Check(layout.ok, "binary row layout rejected");
  Check(layout.layout.parameter_bind_map.size() == 1 &&
            layout.layout.parameter_bind_map[0].column_uuid == column.column_uuid,
        "parameter binding lost binary identity");
  api::IparParameterEncoderCache cache;
  Check(cache.Put(layout.layout).ok, "binary layout insertion failed");
  Check(cache.Lookup(table, statement, epoch, layout.layout.encoder_digest).ok,
        "binary lookup failed");
  // The key must retain all 128 bits independently of UUID shape policy.
  // This is a cache representation test, not system-identity admission.
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto different = table;
    different.bytes[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
    Check(!cache.Lookup(different, statement, epoch, layout.layout.encoder_digest).ok,
          "cache aliased a table UUID bit");
    different = statement;
    different.bytes[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
    Check(!cache.Lookup(table, different, epoch, layout.layout.encoder_digest).ok,
          "cache aliased a statement UUID bit");
    auto other_column = column;
    other_column.column_uuid.bytes[bit / 8] ^= static_cast<unsigned char>(1u << (bit % 8));
    auto other = api::BuildIparRowLayoutDescriptor(table, statement, epoch, {other_column});
    Check(other.ok && other.layout.encoder_digest != layout.layout.encoder_digest,
          "encoder digest dropped a column UUID bit");
  }
  auto a = epoch, b = epoch;
  a.digest = "epoch|suffix";
  b.digest = "epoch";
  Check(api::IparRowLayoutCacheKey(table, statement, a, "digest") !=
            api::IparRowLayoutCacheKey(table, statement, b, "suffix|digest"),
        "cache key has ambiguous field boundaries");
  Check(!api::BuildIparRowLayoutDescriptor({}, statement, epoch, {column}).ok,
        "nil table accepted");
  Check(!cache.Lookup(table, {}, epoch, layout.layout.encoder_digest).ok,
        "nil statement accepted");
  std::puts("IPAR binary identity: PASS; layout/cache representation only");
}
