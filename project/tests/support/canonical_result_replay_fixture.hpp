// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/core/uuid/uuid.hpp"
#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

namespace scratchbird::tests {

// Test-only comparison of independently executed canonical results. Only the
// top-level, framed execution-attempt identity may differ; no descriptor,
// user value, NULL state or other envelope octet is normalized away.
inline std::string NormalizeCanonicalReplayAttempt(
    std::string bytes, core::platform::Uuid* attempt) {
  if (!attempt) return {};
  std::size_t cursor = 0;
  unsigned attempts = 0;
  while (cursor < bytes.size()) {
    const auto equals = bytes.find('=', cursor);
    const auto colon = equals == std::string::npos ? equals : bytes.find(':', equals + 1);
    if (colon == std::string::npos) return {};
    std::size_t size = 0;
    const auto parsed = std::from_chars(bytes.data() + equals + 1, bytes.data() + colon, size);
    if (parsed.ec != std::errc{} || parsed.ptr != bytes.data() + colon ||
        size >= bytes.size() - colon) return {};
    const auto end = colon + 1 + size;
    if (end >= bytes.size() || bytes[end] != '\n') return {};
    if (std::string_view(bytes).substr(cursor, equals - cursor) == "execution_attempt_uuid") {
      if (++attempts != 1 || size != 16) return {};
      std::copy_n(bytes.begin() + colon + 1, 16, attempt->bytes.begin());
      if (!core::uuid::IsEngineIdentityUuid(*attempt)) return {};
      std::fill_n(bytes.begin() + colon + 1, 16, '\0');
    }
    cursor = end + 1;
  }
  return attempts == 1 ? bytes : std::string{};
}

template<class Result>
bool SameCanonicalResultReplay(const Result& first, const Result& second) {
  core::platform::Uuid first_attempt, second_attempt;
  const auto first_bytes = NormalizeCanonicalReplayAttempt(first.canonical_result_bytes, &first_attempt);
  const auto second_bytes = NormalizeCanonicalReplayAttempt(second.canonical_result_bytes, &second_attempt);
  if (!first.api_result.ok || !second.api_result.ok || first_bytes.empty() ||
      first_bytes != second_bytes || first_attempt == second_attempt) return false;
  const auto& a = first.api_result.result_shape;
  const auto& b = second.api_result.result_shape;
  if (a.result_kind != b.result_kind || a.columns != b.columns ||
      a.null_extended_columns != b.null_extended_columns || a.rows.size() != b.rows.size()) return false;
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    if (a.rows[i].requested_row_uuid != b.rows[i].requested_row_uuid ||
        a.rows[i].fields.size() != b.rows[i].fields.size()) return false;
    for (std::size_t j = 0; j < a.rows[i].fields.size(); ++j) {
      const auto& [an, av] = a.rows[i].fields[j];
      const auto& [bn, bv] = b.rows[i].fields[j];
      if (an != bn || av.descriptor != bv.descriptor || av.state != bv.state ||
          av.is_null != bv.is_null || av.encoded_value != bv.encoded_value ||
          av.binary_value != bv.binary_value) return false;
    }
  }
  return true;
}
}  // namespace scratchbird::tests
