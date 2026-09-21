// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/server/session_registry.hpp"
#include "../../src/server/statement_coordination_uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <type_traits>

namespace server = scratchbird::server;
using Uuid = scratchbird::core::platform::Uuid;
using CoordinationMap = decltype(server::ServerSessionRegistry::parameter_coordinations_by_uuid);
static_assert(sizeof(Uuid) == 16);
static_assert(std::is_same_v<CoordinationMap::key_type, Uuid>);
static_assert(std::is_same_v<decltype(server::ServerParameterExecutionCoordinationRecord::session_uuid), Uuid>);
static_assert(std::is_same_v<decltype(server::ServerParameterExecutionCoordinationRecord::operation_uuid), Uuid>);
static_assert(!std::is_constructible_v<Uuid, std::string>);

int main() {
  std::size_t checks = 0;
  const auto check = [&](bool condition) {
    ++checks;
    if (!condition) {
      std::fprintf(stderr, "server coordination UUID check %zu failed\n", checks);
      std::abort();
    }
  };
  Uuid base{{0x01,0x99,0x65,0xab,0xcd,0xef,0x70,0x00,
             0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}};
  std::array<std::uint8_t, 16> sentinel{};
  sentinel.fill(0xa5);
  for (std::size_t position = 0; position != 16; ++position) {
    for (unsigned octet = 0; octet != 256; ++octet) {
      auto identity = base;
      identity.bytes[position] = static_cast<std::uint8_t>(octet);
      // Independent wire-shape oracle, not the production UUID validator.
      const bool valid = (identity.bytes[6] >> 4) == 7 &&
                         (identity.bytes[8] & 0xc0) == 0x80;
      for (const bool optional : {false, true}) {
        auto output = sentinel;
        check(server::CopyCoordinationSystemUuid(identity, &output, optional) == valid);
        check(output == (valid ? identity.bytes : sentinel));
        check(!server::CopyCoordinationSystemUuid(identity, nullptr, optional));
      }
    }
  }
  auto output = sentinel;
  check(!server::CopyCoordinationSystemUuid({}, &output));
  check(output == sentinel);
  check(server::CopyCoordinationSystemUuid({}, &output, true));
  check(output == std::array<std::uint8_t, 16>{});

  // All 128 identity bits participate in lookup; embedded NUL and high bytes
  // must not truncate or collide. This exercises the actual registry key type.
  CoordinationMap map;
  server::ServerParameterExecutionCoordinationRecord record;
  record.session_uuid = base;
  record.operation_uuid = base;
  record.private_handle = 71;
  record.generation = 9;
  check(map.emplace(base, record).second);
  for (std::size_t bit = 0; bit != 128; ++bit) {
    auto other = base;
    other.bytes[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
    check(map.find(other) == map.end());
    auto distinct = record;
    distinct.private_handle = 100 + bit;
    check(map.emplace(other, distinct).second);
    check(map.at(other).private_handle == 100 + bit);
    check(map.at(base).private_handle == 71);
    check(map.at(other).session_uuid.bytes == base.bytes);
  }
  check(map.size() == 129);
  check(!map.emplace(base, record).second);
  for (auto previous = map.begin(), current = std::next(previous);
       current != map.end(); ++previous, ++current) {
    check(std::lexicographical_compare(previous->first.bytes.begin(), previous->first.bytes.end(),
                                       current->first.bytes.begin(), current->first.bytes.end()));
  }
  std::printf("server coordination binary UUID component: PASS %zu checks; not live IPC acceptance\n", checks);
}
