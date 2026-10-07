// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "core/platform/runtime_platform.hpp"
#include <stdexcept>
#include <string>
#include <string_view>

namespace scratchbird::tests {
// Independent generation-1 UUID comparison-key oracle. No production key or
// datatype encoder is used: pin the canonical unsigned-byte profile identity,
// descriptor/generations, nulls-first policy and PRESENT state explicitly.
inline void AppendEscapedUuidIndexOracle(std::string& output,
    const core::platform::Uuid& descriptor, std::uint64_t generation,
    std::string_view value) {
  if (value.size() != 16 || !generation) throw std::invalid_argument("invalid UUID key oracle input");
  std::string payload{"\x01\xa1\x16\xe6\xa8\xfe\x7e\x77\x9e\x95\xd2\xd0\x8b\x4a\x43\x71", 16};
  const auto append_generation = [&](std::uint64_t epoch) {
    for (int shift = 56; shift >= 0; shift -= 8)
      payload.push_back(static_cast<char>(epoch >> shift));
  };
  append_generation(1);
  payload.append(reinterpret_cast<const char*>(descriptor.bytes.data()), 16);
  append_generation(generation);
  payload.append("\0\1", 2);
  payload.append(value);
  for (const unsigned char byte : payload) {
    output.push_back(static_cast<char>(byte));
    if (byte == 0) output.push_back(static_cast<char>(0xff));
  }
  output.append(2, '\0');
}
}  // namespace scratchbird::tests
