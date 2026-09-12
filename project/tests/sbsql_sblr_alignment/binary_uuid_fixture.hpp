// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../../src/engine/internal_api/api_types.hpp"
#include <cstdlib>
#include <string_view>

namespace scratchbird::tests {
// Literal fixture decoding, independent of the production UUID validator.
// Nil and non-v7 bit patterns are permitted for admission negative tests.
inline engine::internal_api::EngineUuid BinaryUuid(std::string_view text) {
  if (text.size() != 36) std::abort();
  engine::internal_api::EngineUuid value;
  unsigned nibble = 0;
  for (unsigned i = 0; i != text.size(); ++i) {
    const char ch = text[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (ch != '-') std::abort();
      continue;
    }
    const unsigned digit = ch >= '0' && ch <= '9' ? ch - '0' :
                           ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : 16;
    if (digit == 16 || nibble >= 32) std::abort();
    value.bytes[nibble / 2] |= digit << (nibble % 2 ? 0 : 4);
    ++nibble;
  }
  return value;
}
}  // namespace scratchbird::tests
