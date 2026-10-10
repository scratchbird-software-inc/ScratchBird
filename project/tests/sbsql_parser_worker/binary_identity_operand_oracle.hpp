// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "lowering/lowering.hpp"
#include "sblr_engine_envelope.hpp"
#include <algorithm>

namespace scratchbird::test::sbsql {
// Inspect the raw carrier independently of the production identity decoder.
// A duplicate, display-text arm, wrong value-kind or changed byte is failure.
inline bool HasExactBinaryIdentityOperand(
    const parser::sbsql::SblrEnvelope& envelope, std::string_view role,
    const core::platform::Uuid& expected) {
  const parser::sbsql::SblrOperand* match = nullptr;
  for (const auto& operand : envelope.operands) {
    if (operand.name != role) continue;
    if (match) return false;
    match = &operand;
  }
  return match && match->type == "uuid" && match->value.empty() &&
      match->canonical_value_kind ==
          static_cast<std::uint16_t>(engine::sblr::SblrValueKind::uuid_ref) &&
      match->canonical_value_body.size() == expected.bytes.size() &&
      std::equal(expected.bytes.begin(), expected.bytes.end(),
                 match->canonical_value_body.begin()) &&
      std::find(envelope.resolved_object_uuids.begin(),
                envelope.resolved_object_uuids.end(), expected) !=
          envelope.resolved_object_uuids.end();
}
} // namespace scratchbird::test::sbsql
