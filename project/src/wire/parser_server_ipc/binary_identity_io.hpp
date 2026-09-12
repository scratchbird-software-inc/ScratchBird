// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../core/uuid/uuid.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace scratchbird::wire::parser_server_ipc {

// Fixed-width system identity decoding. A rejection changes neither the
// destination nor the cursor. User UUID data uses the datatype value codec,
// not this UUIDv7-only identity boundary.
inline bool ReadEngineIdentityUuid(
    std::span<const std::uint8_t> data, std::size_t* offset,
    scratchbird::core::platform::Uuid* out, bool allow_nil = false) {
  if (offset == nullptr || out == nullptr || *offset > data.size() ||
      data.size() - *offset < 16) return false;
  scratchbird::core::platform::Uuid candidate;
  std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(*offset), 16,
              candidate.bytes.begin());
  if (!(allow_nil && candidate.is_nil()) &&
      !scratchbird::core::uuid::IsEngineIdentityUuid(candidate)) return false;
  *out = candidate;
  *offset += 16;
  return true;
}

} // namespace scratchbird::wire::parser_server_ipc
