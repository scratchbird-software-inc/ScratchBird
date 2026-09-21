// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/statement_identity_binding.hpp"
#include "datatype_catalog_manifest.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <type_traits>

namespace engine = scratchbird::engine;
namespace dt = scratchbird::core::datatypes;
using Uuid = scratchbird::core::platform::Uuid;
static_assert(sizeof(Uuid) == 16);
static_assert(std::is_same_v<decltype(engine::LiteralDemandDescriptorIdentityV1({})),
                             std::optional<Uuid>>);
static_assert(noexcept(engine::MatchesStatementIdentity(Uuid{}, {})));
unsigned checks = 0, failures = 0;
void Check(bool result, const char* detail) {
  ++checks;
  if (!result && ++failures <= 10) std::cerr << detail << '\n';
}

int main() {
  constexpr Uuid base{{1, 0x9d, 0, 0, 0, 0, 0x70, 0,
                       0x80, 0, 0, 0, 0, 0, 0xd7, 1}};
  for (unsigned byte = 0; byte < 16; ++byte) {
    for (unsigned value = 0; value < 256; ++value) {
      Uuid candidate = base;
      candidate.bytes[byte] = static_cast<std::uint8_t>(value);
      const bool system = (candidate.bytes[6] >> 4) == 7 &&
                          (candidate.bytes[8] >> 6) == 2;
      for (bool optional : {false, true}) {
        Check(engine::MatchesStatementIdentity(candidate, candidate.bytes, optional) == system,
              "invalid system UUID shape admitted");
        Check(engine::MatchesStatementIdentity(base, candidate.bytes, optional) ==
                  (candidate.bytes == base.bytes), "binary mismatch admitted");
      }
    }
  }
  std::array<std::uint8_t, 32> storage{};
  std::copy(base.bytes.begin(), base.bytes.end(), storage.begin());
  for (unsigned length = 0; length <= storage.size(); ++length)
    Check(engine::MatchesStatementIdentity(base, {storage.data(), length}) ==
              (length == 16), "wrong identity span size admitted");
  const Uuid nil{};
  Check(!engine::MatchesStatementIdentity(nil, nil.bytes), "required nil admitted");
  Check(engine::MatchesStatementIdentity(nil, nil.bytes, true), "optional nil refused");
  Check(!engine::MatchesStatementIdentity(nil, base.bytes, true), "nil matches nonnil");
  Check(!engine::MatchesStatementIdentity(base, nil.bytes, true), "nonnil matches nil");

  // Independent bytes from manifest-listed datatype-type-codec-identity-registry.
  constexpr Uuid bigint{{1,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x11}};
  constexpr Uuid decimal{{0xa0,0,0,0,0x64,0x65,0x73,0x69,0xad,0x61,0x6c,0,0,0,0,0}};
  for (unsigned lexical = 0; lexical <= std::numeric_limits<std::uint16_t>::max(); ++lexical) {
    engine::sblr::SblrLiteralDemandV1 demand;
    demand.lexical_class = static_cast<std::uint16_t>(lexical);
    demand.context_class = 1;
    const auto selected = engine::LiteralDemandDescriptorIdentityV1(demand);
    const bool expected = lexical == 1 || lexical == 2;
    Check(selected.has_value() == expected, "incorrect literal demand selection");
    if (selected) {
      Check(*selected == (lexical == 1 ? bigint : decimal), "descriptor identity drift");
      const auto row = dt::LookupDatatypeTypeCodecIdentityV1(base, 1, 1, *selected, 1);
      Check(row.ok && row.row.descriptor_uuid == *selected &&
                row.row.type_uuid.bytes[15] == (lexical == 1 ? 0x12 : 0x13) &&
                row.row.codec_id == (lexical == 1 ? "datatype.int64.le.v1" :
                                                   "datatype.decimal.base1e9.le.v1"),
            "selected descriptor has no exact registry binding");
      Check(!dt::LookupDatatypeTypeCodecIdentityV1(nil, 1, 1, *selected, 1).ok,
            "descriptor selection fabricated a catalog snapshot");
      Check(!dt::LookupDatatypeTypeCodecIdentityV1(base, 2, 1, *selected, 1).ok &&
                !dt::LookupDatatypeTypeCodecIdentityV1(base, 1, 2, *selected, 1).ok &&
                !dt::LookupDatatypeTypeCodecIdentityV1(base, 1, 1, *selected, 2).ok,
            "descriptor selection fabricated a generation");
    }
    demand.nullable = true;
    Check(!engine::LiteralDemandDescriptorIdentityV1(demand),
          "nullable demand gained an unregistered SBLP v1 rule");
  }
  for (unsigned context = 0; context <= std::numeric_limits<std::uint16_t>::max(); ++context)
    for (std::uint16_t lexical : {1, 2}) {
      engine::sblr::SblrLiteralDemandV1 demand;
      demand.context_class = static_cast<std::uint16_t>(context);
      demand.lexical_class = lexical;
      Check(engine::LiteralDemandDescriptorIdentityV1(demand).has_value() == (context == 1),
            "context demand mismatch");
    }
  std::cout << (failures ? "FAIL" : "PASS") << " checks=" << checks << '\n';
  return failures ? 1 : 0;
}
