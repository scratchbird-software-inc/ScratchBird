// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "core/platform/runtime_platform.hpp"
#include "core/hash/hash_digest.hpp"
#include <array>
#include <initializer_list>
#include <string_view>
#include <variant>
#include <vector>
namespace scratchbird::engine {
using AuthorityHashField = std::variant<std::string_view, core::platform::Uuid>;
// Digest preimage only: not an identity generator or transaction authority.
// Type tags, counts, and lengths keep UUID bytes distinct from user text.
inline std::array<std::uint8_t, 32> HashAuthorityMaterial(
    std::string_view domain, std::initializer_list<AuthorityHashField> fields,
    std::initializer_list<std::uint64_t> numbers,
    std::initializer_list<std::array<std::uint8_t, 32>> hashes = {}) {
  std::vector<std::uint8_t> material{'S','B','A','U','T','H','0','2'};
  const auto number = [&](std::uint64_t value) {
    for (unsigned n=0; n<8; ++n) material.push_back(static_cast<std::uint8_t>(value >> (8*n)));
  };
  number(domain.size()); material.insert(material.end(), domain.begin(), domain.end());
  number(fields.size());
  for (const auto& field : fields) {
    if (const auto* identity = std::get_if<core::platform::Uuid>(&field)) {
      material.push_back(2);
      material.insert(material.end(), identity->bytes.begin(), identity->bytes.end());
    } else {
      const auto text = std::get<std::string_view>(field);
      material.push_back(1); number(text.size());
      material.insert(material.end(), text.begin(), text.end());
    }
  }
  number(numbers.size()); for (const auto value : numbers) number(value);
  number(hashes.size());
  for (const auto& hash : hashes) material.insert(material.end(), hash.begin(), hash.end());
  return core::hash::ComputeSha256Digest(material).digest;
}
} // namespace scratchbird::engine
